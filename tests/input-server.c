/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <poor_loop_fd.h>
#include <poor_unistd.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cursor-shape-v1-server-protocol.h"
#include "input-server.h"
#include "poor_wayland_server_display.h"
#include "poor_wayland_server.h"
#include "primary-selection-unstable-v1-server-protocol.h"

/* Enough of a keymap for the keys of the tests, without the include path of xkeyboard-config */
static const char keymap[] =
	"xkb_keymap {\n"
	"xkb_keycodes \"test\" {\n"
	"	minimum = 8; maximum = 255;\n"
	"	<ESC> = 9; <AD01> = 24; <LCTL> = 37; <AC01> = 38; <AB03> = 54; <UP> = 111; <DOWN> = 116;\n"
	"};\n"
	"xkb_types \"test\" {\n"
	"	type \"ONE_LEVEL\" { modifiers = none; level_name[Level1] = \"Any\"; };\n"
	"	type \"ALPHABETIC\" {\n"
	"		modifiers = Shift + Lock; map[Shift] = Level2; map[Lock] = Level2;\n"
	"		level_name[Level1] = \"Base\"; level_name[Level2] = \"Caps\";\n"
	"	};\n"
	"};\n"
	"xkb_compatibility \"test\" { };\n"
	"xkb_symbols \"test\" {\n"
	"	key <ESC> { [ Escape ] }; key <LCTL> { [ Control_L ] }; key <UP> { [ Up ] }; key <DOWN> { [ Down ] };\n"
	"	key <AD01> { type = \"ALPHABETIC\", [ q, Q ], [ Cyrillic_shorti, Cyrillic_SHORTI ] };\n"
	"	key <AC01> { type = \"ALPHABETIC\", [ a, A ], [ Cyrillic_ef, Cyrillic_EF ] };\n"
	"	key <AB03> { type = \"ALPHABETIC\", [ c, C ], [ Cyrillic_es, Cyrillic_ES ], [ j, J ] };\n"
	"	modifier_map Control { <LCTL> };\n"
	"};\n"
	"};\n";

struct input_server_state input_server;

/* A source of the client and the mime types that it offers */
struct source {
	union {
		struct poor_wl_data_source_resource data;
		struct poor_zwp_primary_selection_source_v1_resource primary;
	};
	unsigned mimes;
	bool utf8, used;
};

static struct {
	struct poor_loop *loop;
	struct poor_wl_display display;
	struct poor_wl_global compositor_global, seat_global, data_global, primary_global, shape_global;
	struct poor_wl_client client;
	struct poor_wl_compositor_resource compositor;
	struct poor_wl_surface_resource surface;
	struct poor_wl_seat_resource seat;
	struct poor_wl_pointer_resource pointer;
	struct poor_wl_keyboard_resource keyboard;
	struct poor_wl_data_device_manager_resource data_manager;
	struct poor_wl_data_device_resource data_device;
	struct poor_wl_data_offer_resource offers[2];
	struct poor_zwp_primary_selection_device_manager_v1_resource primary_manager;
	struct poor_zwp_primary_selection_device_v1_resource primary_device;
	struct poor_zwp_primary_selection_offer_v1_resource primary_offers[2];
	struct poor_wp_cursor_shape_manager_v1_resource shape_manager;
	struct poor_wp_cursor_shape_device_v1_resource shape_device;
	struct source sources[8];
	struct source *selection, *primary;
	unsigned offer_count[2];
	struct {
		alignas(max_align_t) char bytes[1024];
		bool used;
	} blocks[4];
	uint32_t serial;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
} server;

static void *server_alloc(struct poor_wl_display *, struct poor_wl_client *, size_t size)
{
	assert(size <= ARRAY_SIZE_BYTES(server.blocks[0].bytes));
	foreach_array_ref(server.blocks, block) {
		if (!block->used) {
			block->used = true;
			return block->bytes;
		}
	}
	return nullptr;
}

static void server_free(struct poor_wl_display *, void *ptr)
{
	foreach_array_ref(server.blocks, block)
		if (block->bytes == ptr)
			block->used = false;
}

static void ignore_destroy(struct poor_wl_resource *)
{
}

static void surface_destroy_request(struct poor_wl_client *, struct poor_wl_surface_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_surface_interface surface_impl = {
	.destroy = surface_destroy_request,
};

static void surface_destroy(struct poor_wl_resource *)
{
	input_server.has_surface = false;
}

static void compositor_create_surface(struct poor_wl_client *client, struct poor_wl_compositor_resource *resource, uint32_t id)
{
	assert(!input_server.has_surface &&
	       !poor_wl_surface_resource_init(&server.surface, client, poor_wl_resource_get_version(&resource->resource), id));
	poor_wl_surface_resource_set_implementation(&server.surface, &surface_impl, nullptr, surface_destroy);
	input_server.has_surface = true;
}

static const struct poor_wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
};

static void bind_compositor(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	assert(!poor_wl_compositor_resource_init(&server.compositor, client, version, id));
	poor_wl_compositor_resource_set_implementation(&server.compositor, &compositor_impl, nullptr, ignore_destroy);
}

static void pointer_set_cursor(struct poor_wl_client *, struct poor_wl_pointer_resource *, uint32_t, struct poor_wl_surface_resource *,
			       int32_t, int32_t)
{
}

static void pointer_release(struct poor_wl_client *, struct poor_wl_pointer_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor,
	.release = pointer_release,
};

static void pointer_destroy(struct poor_wl_resource *)
{
	input_server.has_pointer = false;
}

static void keyboard_release(struct poor_wl_client *, struct poor_wl_keyboard_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_keyboard_interface keyboard_impl = {
	.release = keyboard_release,
};

static void keyboard_destroy(struct poor_wl_resource *)
{
	input_server.has_keyboard = false;
}

static void seat_get_pointer(struct poor_wl_client *client, struct poor_wl_seat_resource *resource, uint32_t id)
{
	assert(!input_server.has_pointer &&
	       !poor_wl_pointer_resource_init(&server.pointer, client, poor_wl_resource_get_version(&resource->resource), id));
	poor_wl_pointer_resource_set_implementation(&server.pointer, &pointer_impl, nullptr, pointer_destroy);
	input_server.has_pointer = true;
}

/* The keymap goes in a memfd, which the connection takes */
static void seat_get_keyboard(struct poor_wl_client *client, struct poor_wl_seat_resource *resource, uint32_t id)
{
	int fd = memfd_create("input-server", MFD_CLOEXEC);

	assert(fd >= 0 && write_array(fd, keymap) == (ssize_t)ARRAY_SIZE_BYTES(keymap));
	assert(!input_server.has_keyboard &&
	       !poor_wl_keyboard_resource_init(&server.keyboard, client, poor_wl_resource_get_version(&resource->resource), id));
	poor_wl_keyboard_resource_set_implementation(&server.keyboard, &keyboard_impl, nullptr, keyboard_destroy);
	input_server.has_keyboard = true;
	poor_wl_keyboard_send_keymap(&server.keyboard, POOR_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, ARRAY_SIZE_BYTES(keymap));
	poor_wl_keyboard_send_repeat_info(&server.keyboard, input_server.repeat_rate, input_server.repeat_delay);
}

static void seat_release(struct poor_wl_client *, struct poor_wl_seat_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.release = seat_release,
};

static void bind_seat(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	assert(!poor_wl_seat_resource_init(&server.seat, client, version, id));
	poor_wl_seat_resource_set_implementation(&server.seat, &seat_impl, nullptr, ignore_destroy);
	poor_wl_seat_send_capabilities(&server.seat, POOR_WL_SEAT_CAPABILITY_POINTER | POOR_WL_SEAT_CAPABILITY_KEYBOARD);
	poor_wl_seat_send_name(&server.seat, "seat0");
}

static struct source *source_new(void)
{
	foreach_array_ref(server.sources, source) {
		if (source->used)
			continue;
		source->used = true;
		source->mimes = 0;
		source->utf8 = false;
		return source;
	}
	assert(!"too many sources");
}

static void source_offer(struct source *source, const char *mime)
{
	source->mimes++;
	source->utf8 |= !strcmp(mime, "text/plain;charset=utf-8");
}

static void source_gone(struct source *source)
{
	source->used = false;
	if (server.selection == source) {
		server.selection = nullptr;
		input_server.has_selection = false;
	}
	if (server.primary == source) {
		server.primary = nullptr;
		input_server.has_primary = false;
	}
}

static void data_source_offer(struct poor_wl_client *, struct poor_wl_data_source_resource *resource, const char *mime)
{
	source_offer(container_of(resource, struct source, data), mime);
}

static void data_source_destroy_request(struct poor_wl_client *, struct poor_wl_data_source_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_data_source_interface data_source_impl = {
	.offer = data_source_offer,
	.destroy = data_source_destroy_request,
};

static void data_source_destroy(struct poor_wl_resource *resource)
{
	source_gone(container_of(resource, struct source, data.resource));
}

static void data_manager_create_data_source(struct poor_wl_client *client, struct poor_wl_data_device_manager_resource *resource,
					    uint32_t id)
{
	struct source *source = source_new();

	assert(!poor_wl_data_source_resource_init(&source->data, client, poor_wl_resource_get_version(&resource->resource), id));
	poor_wl_data_source_resource_set_implementation(&source->data, &data_source_impl, nullptr, data_source_destroy);
}

static void data_device_set_selection(struct poor_wl_client *, struct poor_wl_data_device_resource *,
				      struct poor_wl_data_source_resource *resource, uint32_t serial)
{
	server.selection = resource ? container_of(resource, struct source, data) : nullptr;
	input_server.has_selection = resource;
	input_server.selection_serial = serial;
	input_server.selection_mimes = resource ? server.selection->mimes : 0;
	input_server.selection_utf8 = resource && server.selection->utf8;
}

static void data_device_release(struct poor_wl_client *, struct poor_wl_data_device_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_data_device_interface data_device_impl = {
	.set_selection = data_device_set_selection,
	.release = data_device_release,
};

static void data_device_destroy(struct poor_wl_resource *)
{
	input_server.has_data_device = false;
}

static void data_manager_get_data_device(struct poor_wl_client *client, struct poor_wl_data_device_manager_resource *resource,
					 uint32_t id, struct poor_wl_seat_resource *)
{
	assert(!input_server.has_data_device &&
	       !poor_wl_data_device_resource_init(&server.data_device, client, poor_wl_resource_get_version(&resource->resource), id));
	poor_wl_data_device_resource_set_implementation(&server.data_device, &data_device_impl, nullptr, data_device_destroy);
	input_server.has_data_device = true;
}

static const struct poor_wl_data_device_manager_interface data_manager_impl = {
	.create_data_source = data_manager_create_data_source,
	.get_data_device = data_manager_get_data_device,
};

static void bind_data_manager(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	assert(!poor_wl_data_device_manager_resource_init(&server.data_manager, client, version, id));
	poor_wl_data_device_manager_resource_set_implementation(&server.data_manager, &data_manager_impl, nullptr, ignore_destroy);
}

static void primary_source_offer(struct poor_wl_client *, struct poor_zwp_primary_selection_source_v1_resource *resource, const char *mime)
{
	source_offer(container_of(resource, struct source, primary), mime);
}

static void primary_source_destroy_request(struct poor_wl_client *, struct poor_zwp_primary_selection_source_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_zwp_primary_selection_source_v1_interface primary_source_impl = {
	.offer = primary_source_offer,
	.destroy = primary_source_destroy_request,
};

static void primary_source_destroy(struct poor_wl_resource *resource)
{
	source_gone(container_of(resource, struct source, primary.resource));
}

static void primary_manager_create_source(struct poor_wl_client *client,
					  struct poor_zwp_primary_selection_device_manager_v1_resource *, uint32_t id)
{
	struct source *source = source_new();

	assert(!poor_zwp_primary_selection_source_v1_resource_init(&source->primary, client, 1, id));
	poor_zwp_primary_selection_source_v1_resource_set_implementation(&source->primary, &primary_source_impl, nullptr,
									  primary_source_destroy);
}

static void primary_device_set_selection(struct poor_wl_client *, struct poor_zwp_primary_selection_device_v1_resource *,
					 struct poor_zwp_primary_selection_source_v1_resource *resource, uint32_t serial)
{
	server.primary = resource ? container_of(resource, struct source, primary) : nullptr;
	input_server.has_primary = resource;
	input_server.primary_serial = serial;
	input_server.primary_mimes = resource ? server.primary->mimes : 0;
	input_server.primary_utf8 = resource && server.primary->utf8;
}

static void primary_device_destroy_request(struct poor_wl_client *, struct poor_zwp_primary_selection_device_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_zwp_primary_selection_device_v1_interface primary_device_impl = {
	.set_selection = primary_device_set_selection,
	.destroy = primary_device_destroy_request,
};

static void primary_device_destroy(struct poor_wl_resource *)
{
	input_server.has_primary_device = false;
}

static void primary_manager_get_device(struct poor_wl_client *client, struct poor_zwp_primary_selection_device_manager_v1_resource *,
				       uint32_t id, struct poor_wl_seat_resource *)
{
	assert(!input_server.has_primary_device &&
	       !poor_zwp_primary_selection_device_v1_resource_init(&server.primary_device, client, 1, id));
	poor_zwp_primary_selection_device_v1_resource_set_implementation(&server.primary_device, &primary_device_impl, nullptr,
									  primary_device_destroy);
	input_server.has_primary_device = true;
}

static void primary_manager_destroy(struct poor_wl_client *, struct poor_zwp_primary_selection_device_manager_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_zwp_primary_selection_device_manager_v1_interface primary_manager_impl = {
	.create_source = primary_manager_create_source,
	.get_device = primary_manager_get_device,
	.destroy = primary_manager_destroy,
};

static void bind_primary_manager(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	assert(!poor_zwp_primary_selection_device_manager_v1_resource_init(&server.primary_manager, client, version, id));
	poor_zwp_primary_selection_device_manager_v1_resource_set_implementation(&server.primary_manager, &primary_manager_impl,
										  nullptr, ignore_destroy);
}

static void shape_device_destroy_request(struct poor_wl_client *, struct poor_wp_cursor_shape_device_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void shape_device_set_shape(struct poor_wl_client *, struct poor_wp_cursor_shape_device_v1_resource *, uint32_t serial,
				   uint32_t shape)
{
	input_server.shape = shape;
	input_server.shape_serial = serial;
}

static const struct poor_wp_cursor_shape_device_v1_interface shape_device_impl = {
	.destroy = shape_device_destroy_request,
	.set_shape = shape_device_set_shape,
};

static void shape_device_destroy(struct poor_wl_resource *)
{
	input_server.has_shape_device = false;
}

static void shape_manager_destroy(struct poor_wl_client *, struct poor_wp_cursor_shape_manager_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void shape_manager_get_pointer(struct poor_wl_client *client, struct poor_wp_cursor_shape_manager_v1_resource *, uint32_t id,
				      struct poor_wl_pointer_resource *)
{
	assert(!input_server.has_shape_device && !poor_wp_cursor_shape_device_v1_resource_init(&server.shape_device, client, 1, id));
	poor_wp_cursor_shape_device_v1_resource_set_implementation(&server.shape_device, &shape_device_impl, nullptr,
								    shape_device_destroy);
	input_server.has_shape_device = true;
}

static const struct poor_wp_cursor_shape_manager_v1_interface shape_manager_impl = {
	.destroy = shape_manager_destroy,
	.get_pointer = shape_manager_get_pointer,
};

static void bind_shape_manager(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	assert(!poor_wp_cursor_shape_manager_v1_resource_init(&server.shape_manager, client, version, id));
	poor_wp_cursor_shape_manager_v1_resource_set_implementation(&server.shape_manager, &shape_manager_impl, nullptr,
								     ignore_destroy);
}

/* The text is small enough for an empty pipe */
static void offer_send(int32_t fd)
{
	input_server.offers_received++;
	assert(write_array(fd, array_ptr(input_server.offer_text, strlen(input_server.offer_text))) ==
	       (ssize_t)strlen(input_server.offer_text));
	poor_loop_close_fd_or_submit(server.loop, fd, 0);
}

static void offer_receive(struct poor_wl_client *, struct poor_wl_data_offer_resource *, const char *, int32_t fd)
{
	offer_send(fd);
}

static void offer_destroy_request(struct poor_wl_client *, struct poor_wl_data_offer_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_data_offer_interface offer_impl = {
	.receive = offer_receive,
	.destroy = offer_destroy_request,
};

static void primary_offer_receive(struct poor_wl_client *, struct poor_zwp_primary_selection_offer_v1_resource *, const char *,
				  int32_t fd)
{
	offer_send(fd);
}

static void primary_offer_destroy_request(struct poor_wl_client *, struct poor_zwp_primary_selection_offer_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_zwp_primary_selection_offer_v1_interface primary_offer_impl = {
	.receive = primary_offer_receive,
	.destroy = primary_offer_destroy_request,
};

static void offer_destroy(struct poor_wl_resource *)
{
	input_server.offers_destroyed++;
}

/* The client keeps the offer of a selection until the next one comes, so two offers take turns */
void input_server_offer(bool primary)
{
	unsigned slot = server.offer_count[primary]++ % 2;

	assert(++input_server.offers - input_server.offers_destroyed <= 3);
	if (primary) {
		struct poor_zwp_primary_selection_offer_v1_resource *offer = &server.primary_offers[slot];

		assert(!poor_zwp_primary_selection_offer_v1_resource_init(offer, &server.client, 1, 0));
		poor_zwp_primary_selection_offer_v1_resource_set_implementation(offer, &primary_offer_impl, nullptr, offer_destroy);
		poor_zwp_primary_selection_device_v1_send_data_offer(&server.primary_device, offer);
		poor_zwp_primary_selection_offer_v1_send_offer(offer, "image/png");
		poor_zwp_primary_selection_offer_v1_send_offer(offer, "text/plain");
		poor_zwp_primary_selection_device_v1_send_selection(&server.primary_device, offer);
		return;
	}
	struct poor_wl_data_offer_resource *offer = &server.offers[slot];

	assert(!poor_wl_data_offer_resource_init(offer, &server.client, poor_wl_resource_get_version(&server.data_device.resource), 0));
	poor_wl_data_offer_resource_set_implementation(offer, &offer_impl, nullptr, offer_destroy);
	poor_wl_data_device_send_data_offer(&server.data_device, offer);
	poor_wl_data_offer_send_offer(offer, "text/plain");
	poor_wl_data_offer_send_offer(offer, "text/plain;charset=utf-8");
	poor_wl_data_device_send_selection(&server.data_device, offer);
}

int input_server_receive(bool primary, const char *mime)
{
	int fds[2];

	assert(!pipe2(fds, O_CLOEXEC));
	if (primary)
		poor_zwp_primary_selection_source_v1_send_send(&server.primary->primary, mime, fds[1]);
	else
		poor_wl_data_source_send_send(&server.selection->data, mime, fds[1]);
	return fds[0];
}

uint32_t input_server_pointer_enter(double x, double y)
{
	uint32_t serial = ++server.serial;

	poor_wl_pointer_send_enter(&server.pointer, serial, &server.surface, poor_wl_fixed_from_double(x), poor_wl_fixed_from_double(y));
	poor_wl_pointer_send_frame(&server.pointer);
	return serial;
}

void input_server_pointer_leave(void)
{
	poor_wl_pointer_send_leave(&server.pointer, ++server.serial, &server.surface);
	poor_wl_pointer_send_frame(&server.pointer);
}

void input_server_pointer_motion(double x, double y)
{
	poor_wl_pointer_send_motion(&server.pointer, 0, poor_wl_fixed_from_double(x), poor_wl_fixed_from_double(y));
	poor_wl_pointer_send_frame(&server.pointer);
}

uint32_t input_server_pointer_button(uint32_t button, bool pressed)
{
	uint32_t serial = ++server.serial;

	poor_wl_pointer_send_button(&server.pointer, serial, 0, button,
				    pressed ? POOR_WL_POINTER_BUTTON_STATE_PRESSED : POOR_WL_POINTER_BUTTON_STATE_RELEASED);
	poor_wl_pointer_send_frame(&server.pointer);
	return serial;
}

/* A wheel sends the old axis events too, which the client leaves out */
void input_server_pointer_wheel(int32_t value120)
{
	poor_wl_pointer_send_axis_source(&server.pointer, POOR_WL_POINTER_AXIS_SOURCE_WHEEL);
	poor_wl_pointer_send_axis_value120(&server.pointer, POOR_WL_POINTER_AXIS_VERTICAL_SCROLL, value120);
	poor_wl_pointer_send_axis(&server.pointer, 0, POOR_WL_POINTER_AXIS_VERTICAL_SCROLL, poor_wl_fixed_from_int(value120 / 8));
	poor_wl_pointer_send_frame(&server.pointer);
}

void input_server_capabilities(uint32_t capabilities)
{
	poor_wl_seat_send_capabilities(&server.seat, capabilities);
}

uint32_t input_server_keyboard_enter(void)
{
	uint32_t serial = ++server.serial, storage[1];
	struct poor_wl_array keys = POOR_WL_ARRAY_INIT(storage);

	poor_wl_keyboard_send_enter(&server.keyboard, serial, &server.surface, &keys);
	return serial;
}

void input_server_keyboard_modifiers(uint32_t depressed, uint32_t group)
{
	poor_wl_keyboard_send_modifiers(&server.keyboard, ++server.serial, depressed, 0, 0, group);
}

uint32_t input_server_key(uint32_t key, bool pressed)
{
	uint32_t serial = ++server.serial;

	poor_wl_keyboard_send_key(&server.keyboard, serial, 0, key,
				  pressed ? POOR_WL_KEYBOARD_KEY_STATE_PRESSED : POOR_WL_KEYBOARD_KEY_STATE_RELEASED);
	return serial;
}

void input_server_key_repeated(uint32_t key)
{
	poor_wl_keyboard_send_key(&server.keyboard, ++server.serial, 0, key, POOR_WL_KEYBOARD_KEY_STATE_REPEATED);
}

static void client_done(struct poor_wl_client *)
{
	input_server.client_done = true;
}

int input_server_connect(void)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(server.in, server.out, server.objects, server.server_objects);
	int fds[2];

	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	poor_wl_client_init(&server.client, &server.display, fds[0], &memory, client_done);
	return fds[1];
}

static void display_done(struct poor_wl_display *)
{
	input_server.display_done = true;
}

void input_server_start(struct poor_loop *loop)
{
	server.loop = loop;
	server.selection = server.primary = nullptr;
	fill_array(server.offer_count, 0);
	server.serial = 0;
	foreach_array_ref(server.sources, source)
		source->used = false;
	foreach_array_ref(server.blocks, block)
		block->used = false;
	input_server.shape = input_server.shape_serial = 0;
	input_server.offers = input_server.offers_destroyed = input_server.offers_received = 0;
	input_server.offer_text = "from another client";
	input_server.repeat_rate = 25;
	input_server.repeat_delay = 600;
	input_server.has_surface = input_server.has_pointer = input_server.has_keyboard = false;
	input_server.has_data_device = input_server.has_primary_device = input_server.has_shape_device = false;
	input_server.has_selection = input_server.has_primary = false;
	input_server.client_done = input_server.display_done = false;
	poor_wl_display_init(&server.display, loop, display_done);
	poor_wl_display_set_allocator(&server.display, server_alloc, server_free);
	poor_wl_global_init(&server.compositor_global, &server.display, &poor_wl_compositor_interface, 6, nullptr, bind_compositor);
	poor_wl_global_init(&server.seat_global, &server.display, &poor_wl_seat_interface, 10, nullptr, bind_seat);
	poor_wl_global_init(&server.data_global, &server.display, &poor_wl_data_device_manager_interface, 3, nullptr, bind_data_manager);
	poor_wl_global_init(&server.primary_global, &server.display, &poor_zwp_primary_selection_device_manager_v1_interface, 1,
			    nullptr, bind_primary_manager);
	poor_wl_global_init(&server.shape_global, &server.display, &poor_wp_cursor_shape_manager_v1_interface, 1, nullptr,
			    bind_shape_manager);
}

void input_server_stop(void)
{
	poor_wl_display_destroy(&server.display);
}
