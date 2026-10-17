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

/* Shift and Control are the first and the third modifier of the keymap of the server */
constexpr uint32_t SHIFT = 1 << 0;
constexpr uint32_t CONTROL = 1 << 2;

/* What the seat told: the last key, button and paste, and how far it scrolled */
struct client {
	struct poor_wl_display display;
	struct poor_loop *loop;
	struct poor_wl_registry registry;
	struct poor_wl_callback callback;
	struct poor_wl_compositor compositor;
	struct poor_wl_surface surface;
	struct seat seat;
	xkb_keysym_t sym;
	uint32_t modifiers, button, time;
	unsigned keys, buttons, motions, leaves, pastes;
	ptrdiff_t rows, columns;
	double x, y;
	size_t text_size, paste_size;
	bool pressed, synced, done, error_called;
	char text[SEAT_TEXT];
	char paste[256];
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

static struct seat_memory seat_memory;

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

static struct client *client_of(struct seat *seat)
{
	return container_of(seat, struct client, seat);
}

/* Up and down repeat */
static bool seat_key(struct seat *seat, xkb_keysym_t sym, uint32_t modifiers, size_t size, const char (*text)[size])
{
	struct client *c = client_of(seat);

	c->keys++;
	c->sym = sym;
	c->modifiers = modifiers;
	c->text_size = size;
	copy_array(c->text, text);
	return sym == XKB_KEY_Up || sym == XKB_KEY_Down;
}

static void seat_button(struct seat *seat, double x, double y, uint32_t button, bool pressed, uint32_t time, uint32_t modifiers)
{
	struct client *c = client_of(seat);

	c->buttons++;
	c->x = x;
	c->y = y;
	c->button = button;
	c->pressed = pressed;
	c->time = time;
	c->modifiers = modifiers;
}

static void seat_motion(struct seat *seat, double x, double y)
{
	struct client *c = client_of(seat);

	c->motions++;
	c->x = x;
	c->y = y;
}

static void seat_leave(struct seat *seat)
{
	client_of(seat)->leaves++;
}

static void seat_scroll(struct seat *seat, double, double, ptrdiff_t rows, ptrdiff_t columns)
{
	struct client *c = client_of(seat);

	c->rows += rows;
	c->columns += columns;
}

static void seat_pasted(struct seat *seat, size_t size, const char (*bytes)[size])
{
	struct client *c = client_of(seat);

	assert(size <= ARRAY_SIZE(c->paste));
	c->pastes++;
	c->paste_size = size;
	copy_array(c->paste, bytes);
}

static const struct seat_listener seat_listener = {
	.key = seat_key,
	.button = seat_button,
	.motion = seat_motion,
	.leave = seat_leave,
	.scroll = seat_scroll,
	.paste = seat_pasted,
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

/* Cells are 10 x 20 pixels, at scale 1 */
static void client_start(struct client *c, struct poor_loop *loop)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);

	c->loop = loop;
	c->keys = c->buttons = c->motions = c->pastes = 0;
	c->rows = c->columns = 0;
	c->synced = c->done = c->error_called = false;
	assert(seat_init(&c->seat, loop, &seat_listener, &seat_memory));
	seat_resize(&c->seat, 120, 10, 20);
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
	       !input_server.has_primary && input_server.offers == input_server.offers_destroyed);
	poor_wl_display_disconnect(&c->display);
	run_until(c->loop, c->done && !seat_busy(&c->seat));
}

static void start(struct poor_loop *loop)
{
	loop_init(loop);
	log_init(loop, "input-test");
	input_server_start(loop);
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

static bool got(const struct client *c, const char *expected)
{
	return c->paste_size == strlen(expected) && !memcmp(c->paste, expected, c->paste_size);
}

/*
 * Copies become the clipboard and the primary selection with the serial of the last input, other clients paste them
 * through pipes and the client from its own copies. A new copy leaves the old one to pastes that write it.
 */
TEST(copy_and_paste_own)
{
	static struct client c;
	struct poor_loop loop;
	uint32_t serial;

	start(&loop);
	client_start(&c, &loop);
	serial = input_server_pointer_enter(15, 5);
	client_roundtrip(&c);
	assert(input_server.shape == POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT && input_server.shape_serial == serial);
	assert(c.motions == 1 && c.x == 15 && c.y == 5);
	serial = input_server_pointer_button(BTN_LEFT, false);
	client_roundtrip(&c);
	assert(seat_copy(&c.seat, true, arrview_str("rst line\nsecond")));
	client_roundtrip(&c);
	assert(input_server.has_primary && input_server.primary_serial == serial && input_server.primary_utf8 &&
	       input_server.primary_mimes == 5 && !input_server.has_selection);
	assert(pasted(&loop, true, "text/plain;charset=utf-8", "rst line\nsecond"));
	input_server_keyboard_enter();
	serial = input_server_key(KEY_C, true);
	client_roundtrip(&c);
	assert(seat_copy(&c.seat, false, arrview_str("copied")));
	client_roundtrip(&c);
	assert(input_server.has_selection && input_server.selection_serial == serial && input_server.selection_mimes == 5);
	assert(pasted(&loop, false, "UTF8_STRING", "copied") && pasted(&loop, false, "text/plain", "copied"));
	assert(seat_paste(&c.seat, false) && c.pastes == 1 && got(&c, "copied"));
	assert(seat_paste(&c.seat, true) && c.pastes == 2 && got(&c, "rst line\nsecond"));
	assert(seat_copy(&c.seat, false, arrview_str("again")) && seat_copy(&c.seat, false, arrview_str("and again")));
	client_roundtrip(&c);
	assert(pasted(&loop, false, "TEXT", "and again"));
	assert(!seat_copy_impl(&c.seat, false, 0, &""));
	client_stop(&c);
	stop(&loop);
}

/* The pointer leaves when it leaves the window, and when the seat loses it */
TEST(pointer_leaves)
{
	static struct client c;
	struct poor_loop loop;

	start(&loop);
	client_start(&c, &loop);
	input_server_pointer_enter(15, 5);
	input_server_pointer_leave();
	client_roundtrip(&c);
	assert(c.leaves == 1);
	input_server_capabilities(POOR_WL_SEAT_CAPABILITY_KEYBOARD);
	client_roundtrip(&c);
	assert(c.leaves == 2 && !input_server.has_pointer);
	client_stop(&c);
	stop(&loop);
}

/* The selections of other clients stay until they are replaced, and come through pipes */
TEST(paste_from_others)
{
	static struct client c;
	struct poor_loop loop;

	start(&loop);
	client_start(&c, &loop);
	assert(!seat_paste(&c.seat, false));
	client_roundtrip(&c);
	assert(!c.pastes);
	input_server_offer(false);
	client_roundtrip(&c);
	assert(seat_paste(&c.seat, false) && !seat_paste(&c.seat, false));
	run_until(&loop, c.pastes == 1);
	assert(got(&c, "from another client") && input_server.offers_received == 1 && !input_server.offers_destroyed);
	input_server.offer_text = "the primary selection";
	input_server_offer(true);
	client_roundtrip(&c);
	run_until(&loop, !seat_busy(&c.seat));
	assert(seat_paste(&c.seat, true));
	run_until(&loop, c.pastes == 2);
	assert(got(&c, "the primary selection"));
	for (unsigned i = 0; i < 3 * SEAT_OFFERS; i++) {
		input_server_offer(i % 2);
		client_roundtrip(&c);
		assert(input_server.offers_destroyed == i + 1);
	}
	run_until(&loop, !seat_busy(&c.seat));
	client_stop(&c);
	stop(&loop);
}

/* After a rate of 0 the compositor repeats keys, but only those whose commands repeat */
TEST(compositor_repeats)
{
	static struct client c;
	struct poor_loop loop;

	start(&loop);
	input_server.repeat_rate = 0;
	client_start(&c, &loop);
	input_server_keyboard_enter();
	input_server_key(KEY_A, true);
	input_server_key_repeated(KEY_A);
	client_roundtrip(&c);
	assert(c.keys == 1 && c.sym == XKB_KEY_a);
	input_server_key(KEY_A, false);
	input_server_key(KEY_DOWN, true);
	input_server_key_repeated(KEY_DOWN);
	input_server_key_repeated(KEY_DOWN);
	client_roundtrip(&c);
	assert(c.keys == 4 && c.sym == XKB_KEY_Down);
	input_server_key(KEY_DOWN, false);
	client_stop(&c);
	stop(&loop);
}

/* Down repeats after the delay at the rate of the compositor, a wheel notch scrolls three lines */
TEST(keys_and_wheel)
{
	static struct client c;
	struct poor_loop loop;
	unsigned keys;

	start(&loop);
	input_server.repeat_rate = 100;
	input_server.repeat_delay = 50;
	client_start(&c, &loop);
	input_server_keyboard_enter();
	input_server_key(KEY_DOWN, true);
	client_roundtrip(&c);
	assert(c.keys == 1 && c.sym == XKB_KEY_Down && !c.modifiers && !c.text_size);
	run_for(&loop, 200'000'000);
	assert(c.keys >= 10);
	input_server_key(KEY_DOWN, false);
	client_roundtrip(&c);
	keys = c.keys;
	run_for(&loop, 50'000'000);
	assert(c.keys == keys);
	input_server_key(KEY_A, true);
	input_server_key(KEY_A, false);
	client_roundtrip(&c);
	assert(c.sym == XKB_KEY_a && c.text_size == 1 && c.text[0] == 'a');
	input_server_keyboard_modifiers(SHIFT, 0);
	input_server_key(KEY_A, true);
	input_server_key(KEY_A, false);
	client_roundtrip(&c);
	assert(c.sym == XKB_KEY_a && c.modifiers == SEAT_SHIFT && c.text_size == 1 && c.text[0] == 'A');
	run_for(&loop, 100'000'000);
	assert(c.keys == keys + 2);
	input_server_pointer_enter(5, 5);
	input_server_pointer_wheel(240);
	client_roundtrip(&c);
	assert(c.rows == 6);
	input_server_pointer_wheel(-60);
	input_server_pointer_wheel(-60);
	input_server_pointer_wheel(-120);
	client_roundtrip(&c);
	assert(c.rows == 0);
	input_server_pointer_button(BTN_MIDDLE, true);
	client_roundtrip(&c);
	assert(c.buttons == 1 && c.button == BTN_MIDDLE && c.pressed && c.modifiers == SEAT_SHIFT);
	client_stop(&c);
	stop(&loop);
}

/* The second layout of the server is Russian, the third has Dvorak's j on the key of c */
TEST(shortcuts_in_other_layouts)
{
	static struct client c;
	struct poor_loop loop;

	start(&loop);
	client_start(&c, &loop);
	input_server_keyboard_enter();
	input_server_keyboard_modifiers(CONTROL, 1);
	input_server_key(KEY_A, true);
	input_server_key(KEY_A, false);
	client_roundtrip(&c);
	assert(c.sym == XKB_KEY_a && c.modifiers == SEAT_CONTROL);
	input_server_keyboard_modifiers(CONTROL, 2);
	input_server_key(KEY_C, true);
	input_server_key(KEY_C, false);
	client_roundtrip(&c);
	assert(c.sym == XKB_KEY_j);
	input_server_keyboard_modifiers(0, 1);
	input_server_key(KEY_C, true);
	input_server_key(KEY_C, false);
	client_roundtrip(&c);
	assert(c.sym == XKB_KEY_c && !c.modifiers && c.text_size == 2 && !memcmp(c.text, "\xd1\x81", 2));
	client_stop(&c);
	stop(&loop);
}
