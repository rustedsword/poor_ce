/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <linux/input-event-codes.h>
#include <poor_poll.h>
#include <poor_stdio.h>
#include <poor_unistd.h>
#include <string.h>

#include "input-server.h"
#include "logger.h"
#include "poor_wayland_client_display.h"
#include "seat.h"
#include "test-loop.h"
#include "test-runner.h"

/* Control is the third modifier of the keymap of the server */
constexpr uint32_t CONTROL = 1 << 2;

struct client {
	struct poor_wl_display display;
	struct poor_loop *loop;
	struct poor_wl_registry registry;
	struct poor_wl_callback callback;
	struct poor_wl_compositor compositor;
	struct poor_wl_surface surface;
	struct seat seat;
	struct text text;
	struct view view;
	unsigned changes;
	bool quit, synced, done, error_called;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
	uint32_t lines[256];
};

struct expiry {
	struct poor_loop_timer timer;
	bool expired;
};

static void expire(struct poor_loop *, struct poor_loop_timer *timer)
{
	container_of(timer, struct expiry, timer)->expired = true;
}

static void run_for(struct poor_loop *loop, uint64_t ns)
{
	struct expiry expiry;

	poor_loop_timer_init(&expiry.timer, expire);
	expiry.expired = false;
	poor_loop_timer_arm(loop, &expiry.timer, poor_loop_now() + ns);
	run_until(loop, expiry.expired);
}

static void seat_changed(struct seat *seat)
{
	container_of(seat, struct client, seat)->changes++;
}

static void seat_quit(struct seat *seat)
{
	container_of(seat, struct client, seat)->quit = true;
}

static const struct seat_listener seat_listener = {
	.changed = seat_changed,
	.quit = seat_quit,
};

static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct client *c = data;

	if (!seat_global(&c->seat, registry, name, interface, version) && !strcmp(interface, poor_wl_compositor_interface.name))
		poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, version, &c->compositor);
}

static void registry_global_remove(void *data, struct poor_wl_registry *, uint32_t name)
{
	seat_global_remove(&((struct client *)data)->seat, name);
}

static const struct poor_wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void sync_done(void *data, struct poor_wl_callback *callback, uint32_t)
{
	((struct client *)data)->synced = true;
	poor_wl_callback_destroy(callback);
}

static const struct poor_wl_callback_listener sync_listener = {
	.done = sync_done,
};

static void client_roundtrip(struct client *c)
{
	c->synced = false;
	poor_wl_display_sync(&c->display, &c->callback);
	poor_wl_callback_add_listener(&c->callback, &sync_listener, c);
	run_until(c->loop, c->synced || c->error_called);
	assert(!c->error_called);
}

static void client_error(struct poor_wl_display *display)
{
	container_of(display, struct client, display)->error_called = true;
}

static void client_done(struct poor_wl_display *display)
{
	container_of(display, struct client, display)->done = true;
}

static struct poor_wl_proxy *new_proxy(struct poor_wl_display *display, struct poor_wl_proxy *, const struct poor_wl_interface *interface)
{
	return seat_new_proxy(&container_of(display, struct client, display)->seat, interface);
}

/* The view has rows of 20 and columns of 10 pixels, at scale 1 */
static void client_start(struct client *c, struct poor_loop *loop, const char *text)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);

	c->loop = loop;
	c->changes = 0;
	c->quit = c->synced = c->done = c->error_called = false;
	text_init(&c->text, array_ptr(text, strlen(text)), c->lines);
	view_init(&c->view, &c->text);
	view_resize(&c->view, 800, 100, 10, 20, 0);
	assert(seat_init(&c->seat, loop, &c->view, &seat_listener));
	poor_wl_display_connect_to_fd(&c->display, loop, &memory, input_server_connect(), client_error, client_done);
	poor_wl_display_set_new_proxy(&c->display, new_proxy);
	poor_wl_display_get_registry(&c->display, &c->registry);
	poor_wl_registry_add_listener(&c->registry, &registry_listener, c);
	client_roundtrip(c);
	poor_wl_compositor_create_surface(&c->compositor, &c->surface);
	client_roundtrip(c);
	client_roundtrip(c);
	assert(input_server.has_surface && input_server.has_pointer && input_server.has_keyboard && input_server.has_data_device &&
	       input_server.has_primary_device && input_server.has_shape_device && c->seat.state);
}

static void client_stop(struct client *c)
{
	seat_fini(&c->seat);
	client_roundtrip(c);
	assert(!input_server.has_pointer && !input_server.has_keyboard && !input_server.has_data_device &&
	       !input_server.has_primary_device && !input_server.has_shape_device && !input_server.has_selection &&
	       !input_server.has_primary);
	poor_wl_display_disconnect(&c->display);
	run_until(c->loop, c->done && !seat_busy(&c->seat));
}

static void stop(struct poor_loop *loop)
{
	input_server_stop();
	run_until(loop, input_server.display_done && !log_pending());
	loop_fini(loop);
}

static bool readable(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	return poll_array(array_ptr(&pfd), 0) == 1;
}

/* The pipe ends once the client closes it. Closes wake nobody up, so the loop runs in short slices. */
static size_t read_pipe(struct poor_loop *loop, int fd, size_t size, char (*buffer)[size])
{
	size_t length = 0;
	ssize_t n;

	do {
		while (!readable(fd))
			run_for(loop, 1'000'000);
		n = read_array(fd, arrview_cfront(length, buffer));
		assert(n >= 0 && length + n < size);
		length += n;
	} while (n);
	poor_loop_close_fd_or_submit(loop, fd, 0);
	return length;
}

static bool pasted(struct poor_loop *loop, bool primary, const char *mime, const char *expected)
{
	char text[256];
	size_t length = read_pipe(loop, input_server_receive(primary, mime), ARRAY_SIZE(text), &text);

	return length == strlen(expected) && !memcmp(text, expected, length);
}

/*
 * A drag from the third character of the first line to the seventh of the second one selects their text, which
 * becomes the primary selection. Ctrl+C copies it to the clipboard with the serial of the key.
 */
TEST(select_and_copy)
{
	static struct client c;
	struct poor_loop loop;
	uint32_t serial;

	loop_init(&loop);
	log_init(&loop, "input-test");
	input_server_start(&loop);
	client_start(&c, &loop, "first line\nsecond line\nthird line\n");
	serial = input_server_pointer_enter(15, 5);
	client_roundtrip(&c);
	assert(input_server.shape == POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT && input_server.shape_serial == serial);
	input_server_pointer_button(BTN_LEFT, true);
	input_server_pointer_motion(55, 25);
	serial = input_server_pointer_button(BTN_LEFT, false);
	client_roundtrip(&c);
	assert(c.view.anchor == 2 && c.view.caret == 17 && c.changes == 2 && !c.view.selecting);
	assert(input_server.has_primary && input_server.primary_serial == serial && input_server.primary_utf8 &&
	       input_server.primary_mimes == 5 && !input_server.has_selection);
	assert(pasted(&loop, true, "text/plain;charset=utf-8", "rst line\nsecond"));
	input_server_keyboard_enter();
	input_server_keyboard_modifiers(CONTROL);
	serial = input_server_key(KEY_C, true);
	input_server_key(KEY_C, false);
	client_roundtrip(&c);
	assert(input_server.has_selection && input_server.selection_serial == serial && input_server.selection_utf8 &&
	       input_server.selection_mimes == 5);
	assert(pasted(&loop, false, "UTF8_STRING", "rst line\nsecond") && pasted(&loop, false, "text/plain", "rst line\nsecond"));
	serial = input_server_key(KEY_A, true);
	client_roundtrip(&c);
	assert(c.view.anchor == 0 && c.view.caret == c.text.size && input_server.primary_serial == serial);
	assert(pasted(&loop, true, "STRING", "first line\nsecond line\nthird line\n"));
	assert(pasted(&loop, false, "TEXT", "rst line\nsecond"));
	client_stop(&c);
	stop(&loop);
}

/* Nothing is pasted, so the offers of other clients go right away and their memory comes back */
TEST(offers_are_destroyed)
{
	static struct client c;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "input-test");
	input_server_start(&loop);
	client_start(&c, &loop, "text");
	for (unsigned i = 0; i < 3 * SEAT_OFFERS; i++) {
		input_server_offer(i % 2);
		client_roundtrip(&c);
		assert(input_server.offers_destroyed == i + 1);
	}
	client_stop(&c);
	stop(&loop);
}

/* Down repeats after the delay at the rate of the compositor, a wheel notch scrolls three lines */
TEST(keys_and_wheel_scroll)
{
	static struct client c;
	static char text[2048];
	struct poor_loop loop;
	size_t length = 0, top;

	for (unsigned i = 0; i < 100; i++)
		length += snprintf_array(arrview_cfront(length, text), "line %u\n", i);
	loop_init(&loop);
	log_init(&loop, "input-test");
	input_server_start(&loop);
	input_server.repeat_rate = 100;
	input_server.repeat_delay = 50;
	client_start(&c, &loop, text);
	input_server_keyboard_enter();
	input_server_key(KEY_DOWN, true);
	client_roundtrip(&c);
	assert(c.view.top == 1);
	run_for(&loop, 200'000'000);
	assert(c.view.top >= 10);
	input_server_key(KEY_DOWN, false);
	client_roundtrip(&c);
	top = c.view.top;
	run_for(&loop, 50'000'000);
	assert(c.view.top == top);
	input_server_pointer_enter(5, 5);
	input_server_pointer_wheel(240);
	client_roundtrip(&c);
	assert(c.view.top == top + 6);
	input_server_pointer_wheel(-60);
	input_server_pointer_wheel(-60);
	input_server_pointer_wheel(-120);
	client_roundtrip(&c);
	assert(c.view.top == top);
	input_server_key(KEY_UP, true);
	input_server_key(KEY_UP, false);
	input_server_key(KEY_ESC, true);
	input_server_key(KEY_ESC, false);
	client_roundtrip(&c);
	assert(c.view.top == top - 1 && !c.quit);
	input_server_keyboard_modifiers(CONTROL);
	input_server_key(KEY_Q, true);
	client_roundtrip(&c);
	assert(c.quit);
	client_stop(&c);
	stop(&loop);
}
