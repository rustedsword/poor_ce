/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <poor_damage_buffers.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "damage.h"
#include "editor.h"
#include "fractional-scale-v1-client-protocol.h"
#include "logger.h"
#include "path.h"
#include "poor_wayland_client_display.h"
#include "render.h"
#include "scene.h"
#include "seat.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/*
 * A window that edits the files of a directory, drawn with Vulkan on the GPU that the compositor names in its dmabuf
 * feedback and presented through the swapchain with explicit synchronization. It runs until its close button, Ctrl+Q,
 * SIGINT or SIGTERM. WAYLAND_DEBUG=client shows the protocol.
 */
constexpr int32_t DEFAULT_WIDTH = 1100;
constexpr int32_t DEFAULT_HEIGHT = 700;
constexpr uint32_t FONT_SIZE = 15;
constexpr int32_t PADDING = 4;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

static const char *const SERVER[] = { "clangd", "--log=error", nullptr };

poor_loop_fd_declare(signal_file);
poor_damage_buffers_declare(buffer_damage, POOR_WL_SWAPCHAIN_BUFFERS, DAMAGE_RECTS);

struct app {
	struct poor_loop loop;
	struct poor_wl_display display;
	struct poor_wl_registry registry;
	struct poor_wl_callback sync;
	struct poor_wl_compositor compositor;
	struct poor_zwp_linux_dmabuf_v1 dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 syncobj;
	struct poor_xdg_wm_base wm_base;
	struct poor_zxdg_decoration_manager_v1 decoration_manager;
	struct poor_wp_fractional_scale_manager_v1 fractional_manager;
	struct poor_wp_viewporter viewporter;
	struct poor_wl_surface surface;
	struct poor_xdg_surface shell_surface;
	struct poor_xdg_toplevel toplevel;
	struct poor_zxdg_toplevel_decoration_v1 decoration;
	struct poor_wp_fractional_scale_v1 fractional;
	struct poor_wp_viewport viewport;
	struct poor_wl_swapchain swapchain;
	struct renderer renderer;
	struct frame *shown;
	struct buffer_damage damage;
	struct font font;
	struct seat seat;
	signal_file signals;
	struct poor_loop_op signal_op, root_op, node_op, idle_op;
	struct signalfd_siginfo siginfo;
	struct statx root_statx;
	sigset_t sigset;
	char **paths;
	size_t path_count;
	int status, drm_fd, idle_fd;
	int32_t width, height, pending_width, pending_height;
	uint32_t scale, font_pixels;
	bool have_compositor, have_dmabuf, have_syncobj, have_wm_base;
	bool have_decoration_manager, have_fractional_manager, have_viewporter, have_window;
	bool have_renderer, have_font, swapchain_started, swapchain_closed, signals_closed;
	bool pending_activated, viewport_changed, closing;
	char node_path[32];
	char cwd[PATH_MAX];
	char path[PATH_MAX];
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[128];
	union poor_wl_map_entry server_objects[16];
};

/* Large memory of the app, which isn't in struct app to keep its debug builds' stack frames small */
static struct atlas atlas;
static struct editor editor;
static struct editor_memory editor_memory;
static struct seat_memory seat_memory;
static struct frame frames[2];
static struct app app;

poor_loop_group_attach(signal_file, struct app, signals, signal_op, on_signal);

static struct app *app_of_swapchain(struct poor_wl_swapchain *swapchain)
{
	return container_of(swapchain, struct app, swapchain);
}

static void swapchain_done(struct poor_wl_swapchain *swapchain)
{
	app_of_swapchain(swapchain)->swapchain_closed = true;
}

/* The buffers go before the device that they are on, the seat and the window before the connection */
static void app_teardown(struct app *app)
{
	seat_fini(&app->seat);
	if (app->swapchain_started)
		poor_wl_swapchain_destroy(&app->swapchain);
	if (app->have_renderer)
		renderer_fini(&app->renderer);
	if (app->drm_fd >= 0)
		poor_loop_close_fd_or_submit(&app->loop, app->drm_fd, 0);
	poor_wl_display_disconnect(&app->display);
	if (app->signal_op.pending)
		io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(&app->loop)), &app->signal_op, 0);
}

/* Vulkan learns from the fence of the empty batch that all drawing is done */
static void on_idle(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *)
{
	struct app *app = container_of(op, struct app, idle_op);

	poor_loop_close_fd_or_submit(&app->loop, app->idle_fd, 0);
	vkGetFenceStatus(app->renderer.device, app->renderer.drained);
	app_teardown(app);
}

/* The drawing has to end before the buffers go, which is waited for through the loop. Saves go on. */
static void app_quit(struct app *app, int status)
{
	if (app->closing)
		return;
	app->closing = true;
	app->status = status;
	editor_stop(&editor);
	app->idle_fd = app->have_renderer ? renderer_idle_fence(&app->renderer) : -1;
	if (app->idle_fd < 0) {
		if (app->have_renderer && app->renderer.device)
			vkDeviceWaitIdle(app->renderer.device);
		app_teardown(app);
		return;
	}
	io_uring_prep_poll_add(need_sqe(poor_loop_get_sqe_or_submit(&app->loop, &app->idle_op)), app->idle_fd, POLLIN);
}

static void app_changed(struct app *app)
{
	if (app->swapchain_started && !app->closing)
		poor_wl_swapchain_request_frame(&app->swapchain);
}

static int32_t scaled(int32_t size, uint32_t scale)
{
	return (size * scale + 60) / 120;
}

/* The buffers have the size of the window times the scale, the viewport scales them back */
static void app_layout(struct app *app)
{
	uint32_t pixels = scaled(FONT_SIZE, app->scale);
	int32_t width = scaled(app->width, app->scale), height = scaled(app->height, app->scale);

	if (pixels != app->font_pixels) {
		if (!font_resize(&app->font, pixels)) {
			log_printf("ce: no font of %u pixels\n", pixels);
			app_quit(app, 1);
			return;
		}
		atlas_clear(&atlas);
		app->font_pixels = pixels;
	}
	editor_resize(&editor, width, height, app->font.cell_width, app->font.cell_height, scaled(PADDING, app->scale), app->scale);
	seat_resize(&app->seat, app->scale, app->font.cell_width, app->font.cell_height);
	app->viewport_changed = true;
	poor_damage_buffers_init(&app->damage, width, height);
	poor_wl_swapchain_resize(&app->swapchain, width, height);
	app_changed(app);
}

static struct app *app_of_seat(struct seat *seat)
{
	return container_of(seat, struct app, seat);
}

static bool seat_key(struct seat *seat, xkb_keysym_t sym, uint32_t modifiers, size_t size, const char (*text)[size])
{
	return !app_of_seat(seat)->closing && editor_key(&editor, sym, modifiers, size, text);
}

static void seat_button(struct seat *seat, double x, double y, uint32_t button, bool pressed, uint32_t time, uint32_t modifiers)
{
	if (!app_of_seat(seat)->closing)
		editor_button(&editor, x, y, button, pressed, time, modifiers);
}

static void seat_motion(struct seat *seat, double x, double y)
{
	if (!app_of_seat(seat)->closing)
		editor_motion(&editor, x, y);
}

static void seat_leave(struct seat *seat)
{
	if (!app_of_seat(seat)->closing)
		editor_leave(&editor);
}

static void seat_scroll(struct seat *seat, double x, double y, ptrdiff_t rows, ptrdiff_t columns)
{
	if (!app_of_seat(seat)->closing)
		editor_scroll(&editor, x, y, rows, columns);
}

static void seat_pasted(struct seat *seat, size_t size, const char (*bytes)[size])
{
	if (!app_of_seat(seat)->closing)
		editor_paste(&editor, bytes);
}

static const struct seat_listener seat_listener = {
	.key = seat_key,
	.button = seat_button,
	.motion = seat_motion,
	.leave = seat_leave,
	.scroll = seat_scroll,
	.paste = seat_pasted,
};

static void editor_changed(struct editor *)
{
	app_changed(&app);
}

static void editor_title(struct editor *, const char *title)
{
	if (app.have_window)
		poor_xdg_toplevel_set_title(&app.toplevel, title);
}

static bool editor_copy(struct editor *, bool primary, size_t size, const char (*bytes)[size])
{
	return seat_copy(&app.seat, primary, bytes);
}

static bool editor_paste_request(struct editor *, bool primary)
{
	return seat_paste(&app.seat, primary);
}

static void editor_cursor(struct editor *, uint32_t shape)
{
	seat_cursor(&app.seat, shape);
}

static void editor_quit_request(struct editor *)
{
	app_quit(&app, 0);
}

static const struct editor_listener editor_listener = {
	.changed = editor_changed,
	.title = editor_title,
	.copy = editor_copy,
	.paste = editor_paste_request,
	.cursor = editor_cursor,
	.quit = editor_quit_request,
};

/* A directory as the first argument is the root, otherwise the current one is. The other arguments are files. */
static void start(struct app *app, bool directory)
{
	char root[PATH_MAX];

	if (!path_join(root, app->cwd, directory ? app->paths[0] : ".")) {
		log_printf("ce: %s is too long\n", app->paths[0]);
		app_quit(app, 1);
		return;
	}
	editor_start(&editor, root, SERVER);
	foreach_array_const_ref(arrview(directory, app->path_count - directory, array_ptr(app->paths, app->path_count)), path) {
		if (path_join(app->path, app->cwd, *path))
			editor_open(&editor, app->path);
		else
			log_printf("ce: %s is too long\n", *path);
	}
}

static void on_root(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct app *app = container_of(op, struct app, root_op);

	if (!app->closing)
		start(app, !cqe->res && S_ISDIR(app->root_statx.stx_mode));
}

/* The render node only makes the timelines of explicit synchronization */
static void on_node(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct app *app = container_of(op, struct app, node_op);
	int ret;

	if (app->closing) {
		if (cqe->res >= 0)
			poor_loop_close_fd_or_submit(&app->loop, cqe->res, 0);
		return;
	}
	if (cqe->res < 0) {
		log_printf("ce: %s: %s\n", app->node_path, strerror(-cqe->res));
		app_quit(app, 1);
		return;
	}
	app->drm_fd = cqe->res;
	ret = poor_wl_swapchain_set_syncobj(&app->swapchain, &app->syncobj, app->drm_fd);
	if (ret < 0) {
		log_printf("ce: no explicit synchronization: %s\n", strerror(-ret));
		app_quit(app, 1);
		return;
	}
	log_printf("ce: %s\n", app->renderer.device_name);
	poor_wl_swapchain_set_allocator(&app->swapchain, &renderer_allocator, &app->renderer);
}

static void swapchain_device(struct poor_wl_swapchain *swapchain, dev_t device)
{
	struct app *app = app_of_swapchain(swapchain);
	uint32_t render_minor;

	if (app->have_renderer) {
		log_printf("ce: the compositor moved to another GPU\n");
		app_quit(app, 1);
		return;
	}
	app->have_renderer = true;
	if (!renderer_init(&app->renderer, &app->loop, &atlas, device, &render_minor)) {
		app_quit(app, 1);
		return;
	}
	snprintf_array(app->node_path, "/dev/dri/renderD%u", render_minor);
	io_uring_prep_openat(need_sqe(poor_loop_get_sqe_or_submit(&app->loop, &app->node_op)), AT_FDCWD, app->node_path,
			     O_RDWR | O_CLOEXEC, 0);
}

static void present(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, int fence,
		    const struct damage_region *changes)
{
	struct poor_wl_swapchain_rect rects[DAMAGE_RECTS];

	foreach_array_index(arrview_first(changes->count, changes->rects), i)
		rects[i] = (struct poor_wl_swapchain_rect){ changes->rects[i].x, changes->rects[i].y, changes->rects[i].width,
							   changes->rects[i].height };
	poor_wl_swapchain_present_damage(swapchain, buffer, fence, arrview_first(changes->count, rects));
}

/* A frame shows the view as it is now, the next one comes when something changes */
static void swapchain_frame(struct poor_wl_swapchain *swapchain, uint32_t)
{
	struct app *app = app_of_swapchain(swapchain);
	struct frame *frame = app->shown == &frames[0] ? &frames[1] : &frames[0];
	struct poor_wl_swapchain_buffer *buffer;
	struct damage_region changes, redraws;
	size_t index;
	int fence;

	if (app->closing)
		return;
	editor_layout(&editor);
	scene_build(&editor, &atlas, frame);
	damage_diff(app->viewport_changed ? nullptr : app->shown, frame, &changes);
	if (!changes.count)
		return;
	buffer = poor_wl_swapchain_acquire(swapchain);
	assert(buffer);
	index = array_ref_index(swapchain->buffers, buffer);
	if (!buffer->age)
		poor_damage_buffers_forget(&app->damage, index);
	poor_damage_buffers_get(&app->damage, index, &redraws);
	poor_damage_region_add_region(&redraws, &changes);
	fence = renderer_draw(&app->renderer, buffer, SCENE_BACKGROUND, arrview_first(frame->count, frame->quads),
			      arrview_first(redraws.count, redraws.rects));
	if (fence < 0) {
		app_quit(app, 1);
		return;
	}
	if (app->viewport_changed && app->have_viewporter)
		poor_wp_viewport_set_destination(&app->viewport, app->width, app->height);
	app->viewport_changed = false;
	present(swapchain, buffer, fence, &changes);
	poor_damage_buffers_drawn(&app->damage, index, &changes);
	app->shown = frame;
}

static void swapchain_error(struct poor_wl_swapchain *swapchain, int error)
{
	log_printf("ce: swapchain: %s\n", strerror(error));
	app_quit(app_of_swapchain(swapchain), 1);
}

static const struct poor_wl_swapchain_listener swapchain_listener = {
	.device = swapchain_device,
	.frame = swapchain_frame,
	.error = swapchain_error,
	.done = swapchain_done,
};

static void shell_surface_configure(void *data, struct poor_xdg_surface *shell_surface, uint32_t serial)
{
	struct app *app = data;

	poor_xdg_surface_ack_configure(shell_surface, serial);
	editor_focus(&editor, app->pending_activated);
	if (app->pending_width != app->width || app->pending_height != app->height || !app->have_window) {
		app->width = app->pending_width;
		app->height = app->pending_height;
		app->have_window = true;
		app_layout(app);
	}
}

static const struct poor_xdg_surface_listener shell_surface_listener = {
	.configure = shell_surface_configure,
};

/* A size of 0 leaves the size to the client */
static void toplevel_configure(void *data, struct poor_xdg_toplevel *, int32_t width, int32_t height, struct poor_wl_array *states)
{
	struct app *app = data;
	const uint32_t *state;

	app->pending_width = width > 0 ? width : app->width;
	app->pending_height = height > 0 ? height : app->height;
	app->pending_activated = false;
	poor_wl_array_for_each(state, states)
		if (*state == POOR_XDG_TOPLEVEL_STATE_ACTIVATED)
			app->pending_activated = true;
}

static void toplevel_close(void *, struct poor_xdg_toplevel *)
{
	editor_quit(&editor);
}

static void toplevel_configure_bounds(void *, struct poor_xdg_toplevel *, int32_t, int32_t)
{
}

static void toplevel_wm_capabilities(void *, struct poor_xdg_toplevel *, struct poor_wl_array *)
{
}

static const struct poor_xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static void decoration_configure(void *, struct poor_zxdg_toplevel_decoration_v1 *, uint32_t)
{
}

static const struct poor_zxdg_toplevel_decoration_v1_listener decoration_listener = {
	.configure = decoration_configure,
};

static void fractional_preferred_scale(void *data, struct poor_wp_fractional_scale_v1 *, uint32_t scale)
{
	struct app *app = data;

	if (scale == app->scale)
		return;
	app->scale = scale;
	if (app->have_window)
		app_layout(app);
}

static const struct poor_wp_fractional_scale_v1_listener fractional_listener = {
	.preferred_scale = fractional_preferred_scale,
};

static void surface_enter(void *, struct poor_wl_surface *, struct poor_wl_output *)
{
}

static void surface_leave(void *, struct poor_wl_surface *, struct poor_wl_output *)
{
}

/* Without fractional scaling the integer scale of the outputs goes through the viewport too */
static void surface_preferred_buffer_scale(void *data, struct poor_wl_surface *, int32_t factor)
{
	struct app *app = data;

	if (app->have_viewporter && !app->have_fractional_manager && factor > 0)
		fractional_preferred_scale(app, &app->fractional, factor * 120);
}

static void surface_preferred_buffer_transform(void *, struct poor_wl_surface *, uint32_t)
{
}

static const struct poor_wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
	.preferred_buffer_scale = surface_preferred_buffer_scale,
	.preferred_buffer_transform = surface_preferred_buffer_transform,
};

static void wm_base_ping(void *, struct poor_xdg_wm_base *wm_base, uint32_t serial)
{
	poor_xdg_wm_base_pong(wm_base, serial);
}

static const struct poor_xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* The surface feedback needs version 4 of linux-dmabuf, damage_buffer version 4 of wl_compositor */
static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct app *app = data;

	if (seat_global(&app->seat, registry, name, interface, version))
		return;
	if (!strcmp(interface, poor_wl_compositor_interface.name) && !app->have_compositor && version >= 4) {
		app->have_compositor = poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, version < 6 ? version : 6,
							     &app->compositor);
	} else if (!strcmp(interface, poor_zwp_linux_dmabuf_v1_interface.name) && !app->have_dmabuf && version >= 4) {
		app->have_dmabuf = poor_wl_registry_bind(registry, name, &poor_zwp_linux_dmabuf_v1_interface,
							 version < 6 ? version : 6, &app->dmabuf);
	} else if (!strcmp(interface, poor_wp_linux_drm_syncobj_manager_v1_interface.name) && !app->have_syncobj) {
		app->have_syncobj = poor_wl_registry_bind(registry, name, &poor_wp_linux_drm_syncobj_manager_v1_interface, 1,
							  &app->syncobj);
	} else if (!strcmp(interface, poor_xdg_wm_base_interface.name) && !app->have_wm_base) {
		app->have_wm_base = poor_wl_registry_bind(registry, name, &poor_xdg_wm_base_interface, version < 6 ? version : 6,
							  &app->wm_base);
		poor_xdg_wm_base_add_listener(&app->wm_base, &wm_base_listener, app);
	} else if (!strcmp(interface, poor_zxdg_decoration_manager_v1_interface.name) && !app->have_decoration_manager) {
		app->have_decoration_manager = poor_wl_registry_bind(registry, name, &poor_zxdg_decoration_manager_v1_interface, 1,
								     &app->decoration_manager);
	} else if (!strcmp(interface, poor_wp_fractional_scale_manager_v1_interface.name) && !app->have_fractional_manager) {
		app->have_fractional_manager = poor_wl_registry_bind(registry, name, &poor_wp_fractional_scale_manager_v1_interface, 1,
								     &app->fractional_manager);
	} else if (!strcmp(interface, poor_wp_viewporter_interface.name) && !app->have_viewporter) {
		app->have_viewporter = poor_wl_registry_bind(registry, name, &poor_wp_viewporter_interface, 1, &app->viewporter);
	}
}

static void registry_global_remove(void *data, struct poor_wl_registry *, uint32_t name)
{
	seat_global_remove(&((struct app *)data)->seat, name);
}

static const struct poor_wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* Scaling needs the viewport, the server draws the decorations if it can */
static void window_init(struct app *app)
{
	poor_wl_compositor_create_surface(&app->compositor, &app->surface);
	poor_wl_surface_add_listener(&app->surface, &surface_listener, app);
	poor_xdg_wm_base_get_xdg_surface(&app->wm_base, &app->shell_surface, &app->surface);
	poor_xdg_surface_add_listener(&app->shell_surface, &shell_surface_listener, app);
	poor_xdg_surface_get_toplevel(&app->shell_surface, &app->toplevel);
	poor_xdg_toplevel_add_listener(&app->toplevel, &toplevel_listener, app);
	poor_xdg_toplevel_set_title(&app->toplevel, editor.title);
	poor_xdg_toplevel_set_app_id(&app->toplevel, "ce");
	if (app->have_decoration_manager) {
		poor_zxdg_decoration_manager_v1_get_toplevel_decoration(&app->decoration_manager, &app->decoration, &app->toplevel);
		poor_zxdg_toplevel_decoration_v1_add_listener(&app->decoration, &decoration_listener, app);
		poor_zxdg_toplevel_decoration_v1_set_mode(&app->decoration, POOR_ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	if (app->have_viewporter)
		poor_wp_viewporter_get_viewport(&app->viewporter, &app->viewport, &app->surface);
	if (app->have_viewporter && app->have_fractional_manager) {
		poor_wp_fractional_scale_manager_v1_get_fractional_scale(&app->fractional_manager, &app->fractional, &app->surface);
		poor_wp_fractional_scale_v1_add_listener(&app->fractional, &fractional_listener, app);
	}
	poor_wl_swapchain_init(&app->swapchain, &app->loop, &app->dmabuf, &app->surface, ((const uint32_t[]){ RENDER_DRM_FORMAT }),
			       &swapchain_listener);
	app->swapchain_started = true;
	poor_wl_surface_commit(&app->surface);
}

/* The sync callback comes after all globals were announced */
static void synced(void *data, struct poor_wl_callback *callback, uint32_t)
{
	struct app *app = data;

	poor_wl_callback_destroy(callback);
	if (!app->have_compositor || !app->have_dmabuf || !app->have_syncobj || !app->have_wm_base) {
		log_printf("ce: the compositor has no wl_compositor 4, zwp_linux_dmabuf_v1 4, "
			   "wp_linux_drm_syncobj_manager_v1 or xdg_wm_base\n");
		app_quit(app, 1);
		return;
	}
	window_init(app);
}

static const struct poor_wl_callback_listener sync_listener = {
	.done = synced,
};

static struct poor_wl_proxy *new_proxy(struct poor_wl_display *display, struct poor_wl_proxy *, const struct poor_wl_interface *interface)
{
	return seat_new_proxy(&container_of(display, struct app, display)->seat, interface);
}

static void display_error(struct poor_wl_display *display)
{
	struct app *app = container_of(display, struct app, display);
	int error = poor_wl_display_get_error(display);
	const struct poor_wl_interface *interface;
	uint32_t code, id;

	if (error == EPROTO) {
		code = poor_wl_display_get_protocol_error(display, &interface, &id);
		log_printf("ce: protocol error %u on %s#%u\n", code, interface ? interface->name : "[unknown]", id);
	} else {
		log_printf("ce: connection: %s\n", strerror(error));
	}
	app_quit(app, 1);
}

static void display_done(struct poor_wl_display *display)
{
	poor_loop_stop(&container_of(display, struct app, display)->loop);
}

/* Signals are unblocked afterwards, so a second one kills a client that hangs */
POOR_LOOP_GROUP_OP(on_signal, loop, file, op, cqe)
{
	struct app *app = container_of(file, struct app, signals);

	if (cqe->res == sizeof(app->siginfo))
		log_printf("ce: %s\n", strsignal(app->siginfo.ssi_signo));
	sigprocmask(SIG_UNBLOCK, &app->sigset, nullptr);
	app_quit(app, 0);
}

POOR_LOOP_FD_DONE(signal_file, loop, file)
{
	container_of(file, struct app, signals)->signals_closed = true;
}

static void app_prepare(struct app *app)
{
	app->status = 0;
	app->drm_fd = app->idle_fd = -1;
	app->width = app->pending_width = DEFAULT_WIDTH;
	app->height = app->pending_height = DEFAULT_HEIGHT;
	app->scale = 120;
	app->have_compositor = app->have_dmabuf = app->have_syncobj = app->have_wm_base = false;
	app->have_decoration_manager = app->have_fractional_manager = app->have_viewporter = app->have_window = false;
	app->have_renderer = app->swapchain_started = app->swapchain_closed = false;
	app->pending_activated = app->viewport_changed = app->closing = false;
	app->have_font = false;
	app->signals_closed = true;
	poor_loop_op_init(&app->root_op, on_root);
	poor_loop_op_init(&app->node_op, on_node);
	poor_loop_op_init(&app->idle_op, on_idle);
	foreach_array_ref(app->seat.transfers, transfer)
		transfer->used = false;
	app->seat.paste.busy = false;
	editor_init(&editor, &app->loop, &editor_listener, &editor_memory);
	app->shown = nullptr;
}

/*
 * getcwd() is the one way to learn the current directory, io_uring can't. A client that pastes may close its pipe
 * early, which is no reason to die.
 */
[[nodiscard]] static int app_init(struct app *app, size_t count, char *(*paths)[count])
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(app->in, app->out, app->objects, app->server_objects);
	struct sigaction ignore = { .sa_handler = SIG_IGN };
	int fd;

	app->paths = *paths;
	app->path_count = count;
	if (!getcwd(app->cwd, sizeof(app->cwd))) {
		log_printf("ce: getcwd: %s\n", strerror(errno));
		return -1;
	}
	app->have_font = font_init(&app->font, FONT_SIZE);
	if (!app->have_font) {
		log_printf("ce: the font failed\n");
		return -1;
	}
	app->font_pixels = FONT_SIZE;
	atlas_init(&atlas, &app->font);
	if (!seat_init(&app->seat, &app->loop, &seat_listener, &seat_memory)) {
		log_printf("ce: no xkbcommon context\n");
		return -1;
	}
	if (poor_wl_display_connect(&app->display, &app->loop, &memory, nullptr, display_error, display_done) < 0) {
		log_printf("ce: can't connect: %s\n", strerror(errno));
		seat_fini(&app->seat);
		return -1;
	}
	poor_wl_display_set_new_proxy(&app->display, new_proxy);
	poor_wl_display_get_registry(&app->display, &app->registry);
	poor_wl_registry_add_listener(&app->registry, &registry_listener, app);
	poor_wl_display_sync(&app->display, &app->sync);
	poor_wl_callback_add_listener(&app->sync, &sync_listener, app);
	if (count)
		io_uring_prep_statx(need_sqe(poor_loop_get_sqe_or_submit(&app->loop, &app->root_op)), AT_FDCWD, (*paths)[0], 0,
				    STATX_TYPE, &app->root_statx);
	else
		start(app, false);

	sigaction(SIGPIPE, &ignore, nullptr);
	sigemptyset(&app->sigset);
	sigaddset(&app->sigset, SIGINT);
	sigaddset(&app->sigset, SIGTERM);
	sigprocmask(SIG_BLOCK, &app->sigset, nullptr);
	fd = signalfd(-1, &app->sigset, SFD_CLOEXEC);
	if (fd < 0) {
		log_printf("ce: signalfd: %s\n", strerror(errno));
		app_quit(app, 1);
		return 0;
	}
	poor_loop_fd_init(&app->signals, fd);
	poor_loop_op_init(&app->signal_op, on_signal);
	app->signals_closed = false;
	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(&app->loop, &app->signals, &app->signal_op)), fd,
				 array_ptr(&app->siginfo), -1);
	return 0;
}

/* Write out the log and the files, then wait for the closes, which nobody tracks */
static int app_finish(struct app *app)
{
	int ret = 0;

	while (!ret && (!app->signals_closed || (app->swapchain_started && !app->swapchain_closed) || app->root_op.pending ||
			app->node_op.pending || app->idle_op.pending || seat_busy(&app->seat) || editor_busy(&editor) ||
			log_pending()))
		ret = poor_loop_run_once(&app->loop);
	if (!ret)
		ret = poor_loop_drain(&app->loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (app->have_font)
		font_fini(&app->font);
	poor_loop_exit(&app->loop);
	if (ret) {
		dprintf(STDERR_FILENO, "ce: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app->status;
}

int main(int argc, char **argv)
{
	int ret;

	ret = poor_loop_init(&app.loop, 64,
			     &(struct io_uring_params){ .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN });
	if (ret) {
		dprintf(STDERR_FILENO, "ce: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	log_init(&app.loop, "ce");
	poor_wl_log_set_handler_client(log_vprintf);
	app_prepare(&app);
	if (argc > 1 && argv[1][0] == '-') {
		log_printf("usage: ce [DIRECTORY] [FILE]...\n");
		app.status = 2;
		return app_finish(&app);
	}
	if (app_init(&app, argc - 1, arrview_cfront(1, array_ptr(argv, argc))) < 0) {
		app.status = 1;
		return app_finish(&app);
	}
	ret = poor_loop_run(&app.loop);
	if (ret) {
		dprintf(STDERR_FILENO, "ce: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app_finish(&app);
}
