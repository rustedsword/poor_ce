/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <linux/input-event-codes.h>
#include <poor_loop_uring_array.h>
#include <poor_mman.h>
#include <string.h>

#include "logger.h"
#include "seat.h"

static const char *const MIME_TYPES[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" };

poor_loop_group_attach(seat_transfer_file, struct seat_transfer, file, write_op, transfer_on_write);

static void changed(struct seat *seat)
{
	seat->listener->changed(seat);
}

static void transfer_write(struct seat_transfer *transfer)
{
	struct seat *seat = transfer->seat;

	io_uring_prep_write_array(need_sqe(poor_loop_group_get_sqe_or_submit(seat->loop, &transfer->file, &transfer->write_op)),
				  transfer->file.fd,
				  array_ptr(seat->view->text->bytes + transfer->offset, transfer->end - transfer->offset), -1);
}

/* A pipe takes as much as it has room for, the rest goes after it */
POOR_LOOP_GROUP_OP(transfer_on_write, loop, file, op, cqe)
{
	struct seat_transfer *transfer = container_of(file, struct seat_transfer, file);

	if (cqe->res > 0 && (transfer->offset += cqe->res) < transfer->end && !transfer->seat->closing)
		transfer_write(transfer);
}

POOR_LOOP_FD_DONE(seat_transfer_file, loop, file)
{
	container_of(file, struct seat_transfer, file)->used = false;
}

static void transfer_start(struct seat *seat, const struct seat_source *source, int32_t fd)
{
	foreach_array_ref(seat->transfers, transfer) {
		if (transfer->used)
			continue;
		transfer->used = true;
		transfer->seat = seat;
		transfer->offset = source->start;
		transfer->end = source->end;
		poor_loop_fd_init(&transfer->file, fd);
		poor_loop_op_init(&transfer->write_op, transfer_on_write);
		transfer_write(transfer);
		return;
	}
	log_printf("poor_text_viewer: too many pastes at once\n");
	poor_loop_close_fd_or_submit(seat->loop, fd, 0);
}

static void source_destroy(struct seat *seat, struct seat_source *source)
{
	if (source == &seat->selection)
		poor_zwp_primary_selection_source_v1_destroy(&source->primary);
	else
		poor_wl_data_source_destroy(&source->data);
	source->live = false;
}

static void data_source_target(void *, struct poor_wl_data_source *, const char *)
{
}

static void data_source_send(void *data, struct poor_wl_data_source *, const char *, int32_t fd)
{
	struct seat_source *source = data;

	transfer_start(source->seat, source, fd);
}

static void data_source_cancelled(void *data, struct poor_wl_data_source *)
{
	struct seat_source *source = data;

	source_destroy(source->seat, source);
}

static void data_source_dnd(void *, struct poor_wl_data_source *)
{
}

static void data_source_action(void *, struct poor_wl_data_source *, uint32_t)
{
}

static const struct poor_wl_data_source_listener data_source_listener = {
	.target = data_source_target,
	.send = data_source_send,
	.cancelled = data_source_cancelled,
	.dnd_drop_performed = data_source_dnd,
	.dnd_finished = data_source_dnd,
	.action = data_source_action,
};

static void primary_source_send(void *data, struct poor_zwp_primary_selection_source_v1 *, const char *, int32_t fd)
{
	struct seat_source *source = data;

	transfer_start(source->seat, source, fd);
}

static void primary_source_cancelled(void *data, struct poor_zwp_primary_selection_source_v1 *)
{
	struct seat_source *source = data;

	source_destroy(source->seat, source);
}

static const struct poor_zwp_primary_selection_source_v1_listener primary_source_listener = {
	.send = primary_source_send,
	.cancelled = primary_source_cancelled,
};

/* The selection of the view goes to the clipboard or becomes the primary selection, until another client takes over */
static void source_set(struct seat *seat, struct seat_source *source, uint32_t serial)
{
	bool primary = source == &seat->selection;
	size_t start, end;

	if (!view_selection(seat->view, &start, &end) || !(primary ? seat->have_primary_device : seat->have_data_device))
		return;
	if (source->live)
		source_destroy(seat, source);
	source->start = start;
	source->end = end;
	source->live = true;
	if (primary) {
		poor_zwp_primary_selection_device_manager_v1_create_source(&seat->primary_manager, &source->primary);
		poor_zwp_primary_selection_source_v1_add_listener(&source->primary, &primary_source_listener, source);
		foreach_array_const_ref(MIME_TYPES, type)
			poor_zwp_primary_selection_source_v1_offer(&source->primary, *type);
		poor_zwp_primary_selection_device_v1_set_selection(&seat->primary_device, &source->primary, serial);
	} else {
		poor_wl_data_device_manager_create_data_source(&seat->data_manager, &source->data);
		poor_wl_data_source_add_listener(&source->data, &data_source_listener, source);
		foreach_array_const_ref(MIME_TYPES, type)
			poor_wl_data_source_offer(&source->data, *type);
		poor_wl_data_device_set_selection(&seat->data_device, &source->data, serial);
	}
}

static void data_offer_destroy(struct poor_wl_data_offer *offer)
{
	poor_wl_data_offer_destroy(offer);
	container_of(offer, struct seat_offer, data)->used = false;
}

static void data_device_data_offer(void *, struct poor_wl_data_device *, struct poor_wl_data_offer *)
{
}

static void data_device_enter(void *, struct poor_wl_data_device *, uint32_t, struct poor_wl_surface *, poor_wl_fixed_t, poor_wl_fixed_t,
			      struct poor_wl_data_offer *offer)
{
	if (offer)
		data_offer_destroy(offer);
}

static void data_device_leave(void *, struct poor_wl_data_device *)
{
}

static void data_device_motion(void *, struct poor_wl_data_device *, uint32_t, poor_wl_fixed_t, poor_wl_fixed_t)
{
}

static void data_device_drop(void *, struct poor_wl_data_device *)
{
}

static void data_device_selection(void *, struct poor_wl_data_device *, struct poor_wl_data_offer *offer)
{
	if (offer)
		data_offer_destroy(offer);
}

static const struct poor_wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.enter = data_device_enter,
	.leave = data_device_leave,
	.motion = data_device_motion,
	.drop = data_device_drop,
	.selection = data_device_selection,
};

static void primary_device_data_offer(void *, struct poor_zwp_primary_selection_device_v1 *, struct poor_zwp_primary_selection_offer_v1 *)
{
}

static void primary_device_selection(void *, struct poor_zwp_primary_selection_device_v1 *, struct poor_zwp_primary_selection_offer_v1 *offer)
{
	if (!offer)
		return;
	poor_zwp_primary_selection_offer_v1_destroy(offer);
	container_of(offer, struct seat_offer, primary)->used = false;
}

static const struct poor_zwp_primary_selection_device_v1_listener primary_device_listener = {
	.data_offer = primary_device_data_offer,
	.selection = primary_device_selection,
};

static void devices_init(struct seat *seat)
{
	if (seat->have_seat && seat->have_data_manager && !seat->have_data_device) {
		poor_wl_data_device_manager_get_data_device(&seat->data_manager, &seat->data_device, &seat->wl_seat);
		poor_wl_data_device_add_listener(&seat->data_device, &data_device_listener, seat);
		seat->have_data_device = true;
	}
	if (seat->have_seat && seat->have_primary_manager && !seat->have_primary_device) {
		poor_zwp_primary_selection_device_manager_v1_get_device(&seat->primary_manager, &seat->primary_device, &seat->wl_seat);
		poor_zwp_primary_selection_device_v1_add_listener(&seat->primary_device, &primary_device_listener, seat);
		seat->have_primary_device = true;
	}
}

static void shape_device_init(struct seat *seat)
{
	if (!seat->have_pointer || !seat->have_shape_manager || seat->have_shape_device)
		return;
	poor_wp_cursor_shape_manager_v1_get_pointer(&seat->shape_manager, &seat->shape_device, &seat->pointer);
	seat->have_shape_device = true;
}

/* Pointer coordinates are in surface units, the view is in buffer pixels */
static void pointer_position(struct seat *seat, poor_wl_fixed_t x, poor_wl_fixed_t y)
{
	seat->pointer_x = poor_wl_fixed_to_double(x) * seat->view->scale / 120;
	seat->pointer_y = poor_wl_fixed_to_double(y) * seat->view->scale / 120;
}

static void pointer_enter(void *data, struct poor_wl_pointer *, uint32_t serial, struct poor_wl_surface *, poor_wl_fixed_t x,
			  poor_wl_fixed_t y)
{
	struct seat *seat = data;

	pointer_position(seat, x, y);
	if (seat->have_shape_device)
		poor_wp_cursor_shape_device_v1_set_shape(&seat->shape_device, serial, POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
}

static void pointer_leave(void *, struct poor_wl_pointer *, uint32_t, struct poor_wl_surface *)
{
}

/* While the button is down the pointer stays with the surface, beyond its edges it scrolls */
static void pointer_motion(void *data, struct poor_wl_pointer *, uint32_t, poor_wl_fixed_t x, poor_wl_fixed_t y)
{
	struct seat *seat = data;

	pointer_position(seat, x, y);
	if (seat->view->selecting && view_drag(seat->view, seat->pointer_x, seat->pointer_y))
		changed(seat);
}

static void pointer_button(void *data, struct poor_wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
	struct seat *seat = data;
	struct view *view = seat->view;
	size_t offset;

	if (button != BTN_LEFT)
		return;
	if (state == POOR_WL_POINTER_BUTTON_STATE_PRESSED) {
		offset = view_hit(view, seat->pointer_x, seat->pointer_y);
		view->selecting = true;
		if (view_select(view, offset, offset))
			changed(seat);
	} else if (view->selecting) {
		view->selecting = false;
		source_set(seat, &seat->selection, serial);
	}
}

static void pointer_axis(void *data, struct poor_wl_pointer *, uint32_t, uint32_t axis, poor_wl_fixed_t value)
{
	struct seat *seat = data;

	if (axis < ARRAY_SIZE(seat->scroll))
		seat->scroll[axis] += poor_wl_fixed_to_double(value) * seat->view->scale / 120;
}

/* A wheel scrolls by lines or columns per notch, a touchpad by pixels */
static void pointer_frame(void *data, struct poor_wl_pointer *)
{
	struct seat *seat = data;
	struct view *view = seat->view;
	double cells[] = { view->cell_height, view->cell_width };
	ptrdiff_t steps[ARRAY_SIZE(cells)];

	static_assert(ARRAY_SIZE(steps) == ARRAY_SIZE(seat->scroll));
	foreach_array_index(steps, axis) {
		if (seat->wheel_moved[axis]) {
			steps[axis] = seat->wheel[axis] / 120 * SEAT_WHEEL_LINES;
			seat->wheel[axis] %= 120;
			seat->scroll[axis] = 0;
		} else {
			steps[axis] = seat->scroll[axis] / cells[axis];
			seat->scroll[axis] -= steps[axis] * cells[axis];
		}
		seat->wheel_moved[axis] = false;
	}
	if (view_scroll(view, steps[0], steps[1]))
		changed(seat);
}

static void pointer_axis_source(void *, struct poor_wl_pointer *, uint32_t)
{
}

static void pointer_axis_stop(void *data, struct poor_wl_pointer *, uint32_t, uint32_t axis)
{
	struct seat *seat = data;

	if (axis < ARRAY_SIZE(seat->scroll))
		seat->scroll[axis] = 0;
}

static void pointer_axis_value120(void *data, struct poor_wl_pointer *, uint32_t axis, int32_t value120)
{
	struct seat *seat = data;

	if (axis >= ARRAY_SIZE(seat->wheel))
		return;
	seat->wheel[axis] += value120;
	seat->wheel_moved[axis] = true;
}

static void pointer_axis_discrete(void *data, struct poor_wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
	pointer_axis_value120(data, pointer, axis, discrete * 120);
}

static void pointer_axis_relative_direction(void *, struct poor_wl_pointer *, uint32_t, uint32_t)
{
}

static void pointer_warp(void *, struct poor_wl_pointer *, poor_wl_fixed_t, poor_wl_fixed_t)
{
}

static const struct poor_wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
	.warp = pointer_warp,
};

/* Returns whether the key repeats */
static bool key_action(struct seat *seat, uint32_t key, uint32_t serial)
{
	struct view *view = seat->view;
	xkb_keysym_t sym = xkb_keysym_to_lower(xkb_state_key_get_one_sym(seat->state, key + 8));
	bool control = xkb_state_mod_name_is_active(seat->state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	ptrdiff_t lines = 0, columns = 0;

	if (control && sym == XKB_KEY_c) {
		source_set(seat, &seat->clipboard, serial);
		return false;
	}
	if (control && sym == XKB_KEY_a) {
		if (view_select(view, 0, view->text->size))
			changed(seat);
		source_set(seat, &seat->selection, serial);
		return false;
	}
	if (control && sym == XKB_KEY_q) {
		seat->listener->quit(seat);
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		if (view_select(view, view->caret, view->caret))
			changed(seat);
		return false;
	case XKB_KEY_Up:
		lines = -1;
		break;
	case XKB_KEY_Down:
		lines = 1;
		break;
	case XKB_KEY_Page_Up:
		lines = -(ptrdiff_t)view_page(view);
		break;
	case XKB_KEY_Page_Down:
	case XKB_KEY_space:
		lines = view_page(view);
		break;
	case XKB_KEY_Home:
		lines = PTRDIFF_MIN;
		break;
	case XKB_KEY_End:
		lines = PTRDIFF_MAX;
		break;
	case XKB_KEY_Left:
		columns = -1;
		break;
	case XKB_KEY_Right:
		columns = 1;
		break;
	default:
		return false;
	}
	if (view_scroll(view, lines, columns))
		changed(seat);
	return true;
}

static void seat_on_repeat(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct seat *seat = container_of(timer, struct seat, repeat);

	if (key_action(seat, seat->repeat_key, seat->key_serial))
		poor_loop_timer_arm(loop, timer, poor_loop_now() + 1'000'000'000ull / seat->repeat_rate);
}

/* A keymap of version 7 is mapped privately */
static void keyboard_keymap(void *data, struct poor_wl_keyboard *, uint32_t format, int32_t fd, uint32_t size)
{
	struct seat *seat = data;
	struct xkb_keymap *keymap = nullptr;
	void *memory = MAP_FAILED;

	if (format == POOR_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		memory = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
	poor_loop_close_fd_or_submit(seat->loop, fd, 0);
	if (memory != MAP_FAILED) {
		auto map = array_ptr((char *)memory, size);

		keymap = xkb_keymap_new_from_buffer(seat->xkb, *map, strnlen(*map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
						    XKB_KEYMAP_COMPILE_NO_FLAGS);
		munmap_array(map);
	}
	if (!keymap) {
		log_printf("poor_text_viewer: the keymap of the compositor is not usable\n");
		return;
	}
	xkb_state_unref(seat->state);
	xkb_keymap_unref(seat->keymap);
	seat->keymap = keymap;
	seat->state = xkb_state_new(keymap);
}

static void keyboard_enter(void *, struct poor_wl_keyboard *, uint32_t, struct poor_wl_surface *, struct poor_wl_array *)
{
}

static void keyboard_leave(void *data, struct poor_wl_keyboard *, uint32_t, struct poor_wl_surface *)
{
	poor_loop_timer_disarm(&((struct seat *)data)->repeat);
}

/* A new key stops the repetition of the last one. Version 10 can repeat keys itself, after a rate of 0. */
static void keyboard_key(void *data, struct poor_wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
{
	struct seat *seat = data;

	if (state == POOR_WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (key == seat->repeat_key)
			poor_loop_timer_disarm(&seat->repeat);
		return;
	}
	if (!seat->state)
		return;
	seat->key_serial = serial;
	if (state == POOR_WL_KEYBOARD_KEY_STATE_PRESSED)
		poor_loop_timer_disarm(&seat->repeat);
	if (key_action(seat, key, serial) && state == POOR_WL_KEYBOARD_KEY_STATE_PRESSED && seat->repeat_rate > 0 &&
	    xkb_keymap_key_repeats(seat->keymap, key + 8)) {
		seat->repeat_key = key;
		poor_loop_timer_arm(seat->loop, &seat->repeat, poor_loop_now() + seat->repeat_delay * 1'000'000ull);
	}
}

static void keyboard_modifiers(void *data, struct poor_wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched,
			       uint32_t locked, uint32_t group)
{
	struct seat *seat = data;

	if (seat->state)
		xkb_state_update_mask(seat->state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct poor_wl_keyboard *, int32_t rate, int32_t delay)
{
	struct seat *seat = data;

	seat->repeat_rate = rate;
	seat->repeat_delay = delay;
}

static const struct poor_wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void pointer_release(struct seat *seat)
{
	if (seat->have_shape_device)
		poor_wp_cursor_shape_device_v1_destroy(&seat->shape_device);
	poor_wl_pointer_release(&seat->pointer);
	seat->have_shape_device = seat->have_pointer = false;
	seat->view->selecting = false;
}

static void keyboard_release(struct seat *seat)
{
	poor_loop_timer_disarm(&seat->repeat);
	poor_wl_keyboard_release(&seat->keyboard);
	xkb_state_unref(seat->state);
	xkb_keymap_unref(seat->keymap);
	seat->state = nullptr;
	seat->keymap = nullptr;
	seat->have_keyboard = false;
}

static void seat_capabilities(void *data, struct poor_wl_seat *, uint32_t capabilities)
{
	struct seat *seat = data;
	bool pointer = capabilities & POOR_WL_SEAT_CAPABILITY_POINTER, keyboard = capabilities & POOR_WL_SEAT_CAPABILITY_KEYBOARD;

	if (pointer && !seat->have_pointer) {
		poor_wl_seat_get_pointer(&seat->wl_seat, &seat->pointer);
		poor_wl_pointer_add_listener(&seat->pointer, &pointer_listener, seat);
		seat->have_pointer = true;
		shape_device_init(seat);
	} else if (!pointer && seat->have_pointer) {
		pointer_release(seat);
	}
	if (keyboard && !seat->have_keyboard) {
		poor_wl_seat_get_keyboard(&seat->wl_seat, &seat->keyboard);
		poor_wl_keyboard_add_listener(&seat->keyboard, &keyboard_listener, seat);
		seat->have_keyboard = true;
	} else if (!keyboard && seat->have_keyboard) {
		keyboard_release(seat);
	}
}

static void seat_name(void *, struct poor_wl_seat *, const char *)
{
}

static const struct poor_wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void seat_release(struct seat *seat)
{
	if (seat->have_pointer)
		pointer_release(seat);
	if (seat->have_keyboard)
		keyboard_release(seat);
	if (seat->have_data_device && poor_wl_data_device_get_version(&seat->data_device) >= POOR_WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
		poor_wl_data_device_release(&seat->data_device);
	else if (seat->have_data_device)
		poor_wl_data_device_destroy(&seat->data_device);
	if (seat->have_primary_device)
		poor_zwp_primary_selection_device_v1_destroy(&seat->primary_device);
	poor_wl_seat_release(&seat->wl_seat);
	seat->have_data_device = seat->have_primary_device = seat->have_seat = false;
}

bool seat_init(struct seat *seat, struct poor_loop *loop, struct view *view, const struct seat_listener *listener)
{
	seat->loop = loop;
	seat->view = view;
	seat->listener = listener;
	seat->clipboard.seat = seat->selection.seat = seat;
	seat->clipboard.live = seat->selection.live = false;
	foreach_array_ref(seat->offers, offer)
		offer->used = false;
	foreach_array_ref(seat->transfers, transfer)
		transfer->used = false;
	poor_loop_timer_init(&seat->repeat, seat_on_repeat);
	seat->keymap = nullptr;
	seat->state = nullptr;
	seat->repeat_key = UINT32_MAX;
	seat->repeat_rate = 25;
	seat->repeat_delay = 600;
	fill_array(seat->wheel, 0);
	fill_array(seat->scroll, 0);
	fill_array(seat->wheel_moved, false);
	seat->have_seat = seat->have_pointer = seat->have_keyboard = false;
	seat->have_shape_manager = seat->have_shape_device = false;
	seat->have_data_manager = seat->have_data_device = seat->have_primary_manager = seat->have_primary_device = false;
	seat->closing = false;
	seat->xkb = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	return seat->xkb;
}

/* Frames and the release of the seat need version 5 */
bool seat_global(struct seat *seat, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	if (!strcmp(interface, poor_wl_seat_interface.name)) {
		if (seat->have_seat || version < 5)
			return true;
		seat->have_seat = poor_wl_registry_bind(registry, name, &poor_wl_seat_interface, version < 10 ? version : 10, &seat->wl_seat);
		seat->seat_name = name;
		poor_wl_seat_add_listener(&seat->wl_seat, &seat_listener, seat);
	} else if (!strcmp(interface, poor_wl_data_device_manager_interface.name)) {
		if (seat->have_data_manager)
			return true;
		seat->have_data_manager = poor_wl_registry_bind(registry, name, &poor_wl_data_device_manager_interface,
								version < 3 ? version : 3, &seat->data_manager);
	} else if (!strcmp(interface, poor_zwp_primary_selection_device_manager_v1_interface.name)) {
		if (seat->have_primary_manager)
			return true;
		seat->have_primary_manager = poor_wl_registry_bind(registry, name, &poor_zwp_primary_selection_device_manager_v1_interface,
								   1, &seat->primary_manager);
	} else if (!strcmp(interface, poor_wp_cursor_shape_manager_v1_interface.name)) {
		if (seat->have_shape_manager)
			return true;
		seat->have_shape_manager = poor_wl_registry_bind(registry, name, &poor_wp_cursor_shape_manager_v1_interface, 1,
								 &seat->shape_manager);
		shape_device_init(seat);
		return true;
	} else {
		return false;
	}
	devices_init(seat);
	return true;
}

void seat_global_remove(struct seat *seat, uint32_t name)
{
	if (seat->have_seat && name == seat->seat_name)
		seat_release(seat);
}

struct poor_wl_proxy *seat_new_proxy(struct seat *seat, const struct poor_wl_interface *interface)
{
	bool data = interface == &poor_wl_data_offer_interface;

	if (!data && interface != &poor_zwp_primary_selection_offer_v1_interface)
		return nullptr;
	foreach_array_ref(seat->offers, offer) {
		if (offer->used)
			continue;
		offer->used = true;
		return data ? &offer->data.proxy : &offer->primary.proxy;
	}
	log_printf("poor_text_viewer: too many offers\n");
	return nullptr;
}

/* Pastes in progress end early, the other clients see the end of their pipes */
void seat_fini(struct seat *seat)
{
	seat->closing = true;
	foreach_array_ref(seat->transfers, transfer)
		if (transfer->used && transfer->write_op.pending)
			io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(seat->loop)), &transfer->write_op, 0);
	if (seat->have_seat)
		seat_release(seat);
	if (seat->clipboard.live)
		source_destroy(seat, &seat->clipboard);
	if (seat->selection.live)
		source_destroy(seat, &seat->selection);
	if (seat->have_shape_manager)
		poor_wp_cursor_shape_manager_v1_destroy(&seat->shape_manager);
	if (seat->have_data_manager)
		poor_wl_data_device_manager_destroy(&seat->data_manager);
	if (seat->have_primary_manager)
		poor_zwp_primary_selection_device_manager_v1_destroy(&seat->primary_manager);
	seat->have_shape_manager = seat->have_data_manager = seat->have_primary_manager = false;
	poor_loop_timer_disarm(&seat->repeat);
	xkb_context_unref(seat->xkb);
}

bool seat_busy(const struct seat *seat)
{
	foreach_array_const_ref(seat->transfers, transfer)
		if (transfer->used)
			return true;
	return false;
}
