/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <poor_loop_uring_array.h>
#include <poor_mman.h>
#include <string.h>

#include "logger.h"
#include "seat.h"

/* In the order that pastes prefer them */
static const char *const MIME_TYPES[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING" };

poor_loop_group_attach(seat_transfer_file, struct seat_transfer, file, write_op, transfer_on_write);
poor_loop_group_attach(seat_paste_file, struct seat_paste, file, read_op, paste_on_read);

static void transfer_write(struct seat_transfer *transfer)
{
	struct seat_source *source = transfer->source;
	auto unsent = arrview(transfer->offset, transfer->end - transfer->offset, (*source->bytes)[transfer->copy]);

	io_uring_prep_write_array(need_sqe(poor_loop_group_get_sqe_or_submit(source->seat->loop, &transfer->file,
									     &transfer->write_op)),
				  transfer->file.fd, unsent, -1);
}

/* A pipe takes as much as it has room for, the rest goes after it */
POOR_LOOP_GROUP_OP(transfer_on_write, loop, file, op, cqe)
{
	struct seat_transfer *transfer = container_of(file, struct seat_transfer, file);

	if (cqe->res > 0 && (transfer->offset += cqe->res) < transfer->end && !transfer->source->seat->closing)
		transfer_write(transfer);
}

POOR_LOOP_FD_DONE(seat_transfer_file, loop, file)
{
	struct seat_transfer *transfer = container_of(file, struct seat_transfer, file);

	transfer->source->transfers[transfer->copy]--;
	transfer->used = false;
}

static void transfer_start(struct seat_source *source, int32_t fd)
{
	struct seat *seat = source->seat;

	foreach_array_ref(seat->transfers, transfer) {
		if (transfer->used)
			continue;
		transfer->used = true;
		transfer->source = source;
		transfer->copy = source->current;
		transfer->offset = 0;
		transfer->end = source->size[source->current];
		source->transfers[source->current]++;
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
	transfer_start(data, fd);
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
	transfer_start(data, fd);
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

/* The copy that no paste writes takes the bytes, the one before stays for the pastes that it has */
bool seat_copy_impl(struct seat *seat, bool primary, size_t size, const char (*bytes)[size])
{
	struct seat_source *source = primary ? &seat->selection : &seat->clipboard;
	unsigned copy = source->transfers[source->current ^ 1] ? source->current : source->current ^ 1;

	if (!size || !(primary ? seat->have_primary_device : seat->have_data_device))
		return false;
	if (size > SEAT_BYTES || source->transfers[copy]) {
		log_printf("poor_text_viewer: no room to copy %zu bytes\n", size);
		return false;
	}
	copy_array(arrview_first(size, (*source->bytes)[copy]), bytes);
	source->size[copy] = size;
	source->current = copy;
	if (source->live)
		source_destroy(seat, source);
	source->live = true;
	if (primary) {
		poor_zwp_primary_selection_device_manager_v1_create_source(&seat->primary_manager, &source->primary);
		poor_zwp_primary_selection_source_v1_add_listener(&source->primary, &primary_source_listener, source);
		foreach_array_const_ref(MIME_TYPES, type)
			poor_zwp_primary_selection_source_v1_offer(&source->primary, *type);
		poor_zwp_primary_selection_device_v1_set_selection(&seat->primary_device, &source->primary, seat->serial);
	} else {
		poor_wl_data_device_manager_create_data_source(&seat->data_manager, &source->data);
		poor_wl_data_source_add_listener(&source->data, &data_source_listener, source);
		foreach_array_const_ref(MIME_TYPES, type)
			poor_wl_data_source_offer(&source->data, *type);
		poor_wl_data_device_set_selection(&seat->data_device, &source->data, seat->serial);
	}
	return true;
}

static void offer_type(struct seat_offer *offer, const char *mime)
{
	foreach_array_index(MIME_TYPES, i)
		if (!strcmp(mime, MIME_TYPES[i]))
			offer->types |= 1u << i;
}

static void offer_destroy(struct seat *seat, struct seat_offer *offer)
{
	if (offer->primary)
		poor_zwp_primary_selection_offer_v1_destroy(&offer->primary_offer);
	else
		poor_wl_data_offer_destroy(&offer->data);
	offer->used = false;
	if (seat->paste.offer == offer)
		seat->paste.offer = nullptr;
}

/* The selection of another client replaces the one before */
static void offer_select(struct seat *seat, struct seat_offer **selection, struct seat_offer *offer)
{
	if (*selection)
		offer_destroy(seat, *selection);
	*selection = offer;
}

static void data_offer_offer(void *data, struct poor_wl_data_offer *, const char *mime)
{
	offer_type(data, mime);
}

static void data_offer_source_actions(void *, struct poor_wl_data_offer *, uint32_t)
{
}

static void data_offer_action(void *, struct poor_wl_data_offer *, uint32_t)
{
}

static const struct poor_wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer,
	.source_actions = data_offer_source_actions,
	.action = data_offer_action,
};

static void data_device_data_offer(void *data, struct poor_wl_data_device *, struct poor_wl_data_offer *proxy)
{
	struct seat_offer *offer = container_of(proxy, struct seat_offer, data);

	offer->seat = data;
	offer->types = 0;
	poor_wl_data_offer_add_listener(proxy, &data_offer_listener, offer);
}

/* Nothing is dropped */
static void data_device_enter(void *data, struct poor_wl_data_device *, uint32_t, struct poor_wl_surface *, poor_wl_fixed_t,
			      poor_wl_fixed_t, struct poor_wl_data_offer *offer)
{
	if (offer)
		offer_destroy(data, container_of(offer, struct seat_offer, data));
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

static void data_device_selection(void *data, struct poor_wl_data_device *, struct poor_wl_data_offer *offer)
{
	struct seat *seat = data;

	offer_select(seat, &seat->clipboard_offer, offer ? container_of(offer, struct seat_offer, data) : nullptr);
}

static const struct poor_wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.enter = data_device_enter,
	.leave = data_device_leave,
	.motion = data_device_motion,
	.drop = data_device_drop,
	.selection = data_device_selection,
};

static void primary_offer_offer(void *data, struct poor_zwp_primary_selection_offer_v1 *, const char *mime)
{
	offer_type(data, mime);
}

static const struct poor_zwp_primary_selection_offer_v1_listener primary_offer_listener = {
	.offer = primary_offer_offer,
};

static void primary_device_data_offer(void *data, struct poor_zwp_primary_selection_device_v1 *,
				      struct poor_zwp_primary_selection_offer_v1 *proxy)
{
	struct seat_offer *offer = container_of(proxy, struct seat_offer, primary_offer);

	offer->seat = data;
	offer->types = 0;
	poor_zwp_primary_selection_offer_v1_add_listener(proxy, &primary_offer_listener, offer);
}

static void primary_device_selection(void *data, struct poor_zwp_primary_selection_device_v1 *,
				     struct poor_zwp_primary_selection_offer_v1 *offer)
{
	struct seat *seat = data;

	offer_select(seat, &seat->primary_offer, offer ? container_of(offer, struct seat_offer, primary_offer) : nullptr);
}

static const struct poor_zwp_primary_selection_device_v1_listener primary_device_listener = {
	.data_offer = primary_device_data_offer,
	.selection = primary_device_selection,
};

static void paste_read(struct seat_paste *paste)
{
	struct seat *seat = container_of(paste, struct seat, paste);

	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(seat->loop, &paste->file, &paste->read_op)),
				 paste->file.fd, arrview_cfront(paste->size, paste->bytes), -1);
}

/* What doesn't fit is left out, a paste that fails has nothing */
POOR_LOOP_GROUP_OP(paste_on_read, loop, file, op, cqe)
{
	struct seat_paste *paste = container_of(file, struct seat_paste, file);
	struct seat *seat = container_of(paste, struct seat, paste);

	if (seat->closing)
		return;
	if (cqe->res < 0)
		paste->size = 0;
	if (cqe->res > 0 && (paste->size += cqe->res) < ARRAY_SIZE(paste->bytes)) {
		paste_read(paste);
		return;
	}
	if (paste->size == ARRAY_SIZE(paste->bytes))
		log_printf("poor_text_viewer: the paste is cut off after %zu bytes\n", paste->size);
	seat->listener->paste(seat, paste->size, arrview_first(paste->size, paste->bytes));
}

POOR_LOOP_FD_DONE(seat_paste_file, loop, file)
{
	container_of(file, struct seat_paste, file)->busy = false;
}

/* The client of the offer writes its selection into the pipe, the connection takes the end that it writes */
static void paste_on_pipe(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct seat_paste *paste = container_of(op, struct seat_paste, pipe_op);
	struct seat *seat = container_of(paste, struct seat, paste);
	struct seat_offer *offer = paste->offer;
	const char *mime = nullptr;

	if (cqe->res < 0 || !offer || seat->closing) {
		if (cqe->res >= 0)
			foreach_array_const_ref(paste->fds, fd)
				poor_loop_close_fd_or_submit(seat->loop, *fd, 0);
		paste->busy = false;
		paste->size = 0;
		if (!seat->closing)
			seat->listener->paste(seat, paste->size, arrview_first(paste->size, paste->bytes));
		return;
	}
	foreach_array_index_bw(MIME_TYPES, i)
		if (offer->types & 1u << i)
			mime = MIME_TYPES[i];
	if (offer->primary)
		poor_zwp_primary_selection_offer_v1_receive(&offer->primary_offer, mime, paste->fds[1]);
	else
		poor_wl_data_offer_receive(&offer->data, mime, paste->fds[1]);
	poor_loop_fd_init(&paste->file, paste->fds[0]);
	paste->size = 0;
	paste_read(paste);
}

/* The selection of the app comes right from its copy */
bool seat_paste(struct seat *seat, bool primary)
{
	struct seat_source *source = primary ? &seat->selection : &seat->clipboard;
	struct seat_offer *offer = primary ? seat->primary_offer : seat->clipboard_offer;
	struct seat_paste *paste = &seat->paste;

	if (source->live) {
		seat->listener->paste(seat, source->size[source->current],
				      arrview_first(source->size[source->current], (*source->bytes)[source->current]));
		return true;
	}
	if (!offer || !offer->types || paste->busy || seat->closing)
		return false;
	paste->busy = true;
	paste->offer = offer;
	io_uring_prep_pipe(need_sqe(poor_loop_get_sqe_or_submit(seat->loop, &paste->pipe_op)), paste->fds, O_CLOEXEC);
	return true;
}

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

/* The shape goes with the serial of the enter, a pointer that left gets it when it comes back */
void seat_cursor(struct seat *seat, uint32_t shape)
{
	if (seat->shape_set && shape == seat->shape)
		return;
	seat->shape = shape;
	seat->shape_set = seat->enter_serial && seat->have_shape_device;
	if (seat->shape_set)
		poor_wp_cursor_shape_device_v1_set_shape(&seat->shape_device, seat->enter_serial, shape);
}

static uint32_t modifiers(const struct seat *seat)
{
	if (!seat->state)
		return 0;
	return (xkb_state_mod_name_is_active(seat->state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0 ? SEAT_SHIFT : 0) |
	       (xkb_state_mod_name_is_active(seat->state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ? SEAT_CONTROL : 0) |
	       (xkb_state_mod_name_is_active(seat->state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ? SEAT_ALT : 0);
}

/* Pointer coordinates are in surface units, the listener gets buffer pixels */
static void pointer_position(struct seat *seat, poor_wl_fixed_t x, poor_wl_fixed_t y)
{
	seat->pointer_x = poor_wl_fixed_to_double(x) * seat->scale / 120;
	seat->pointer_y = poor_wl_fixed_to_double(y) * seat->scale / 120;
}

static void pointer_enter(void *data, struct poor_wl_pointer *, uint32_t serial, struct poor_wl_surface *, poor_wl_fixed_t x,
			  poor_wl_fixed_t y)
{
	struct seat *seat = data;

	pointer_position(seat, x, y);
	seat->enter_serial = serial;
	seat->shape_set = false;
	seat->listener->motion(seat, seat->pointer_x, seat->pointer_y);
	seat_cursor(seat, seat->shape);
}

static void pointer_leave(void *data, struct poor_wl_pointer *, uint32_t, struct poor_wl_surface *)
{
	struct seat *seat = data;

	seat->enter_serial = 0;
	seat->shape_set = false;
	seat->listener->leave(seat);
}

/* While a button is down the pointer stays with the surface, beyond its edges too */
static void pointer_motion(void *data, struct poor_wl_pointer *, uint32_t, poor_wl_fixed_t x, poor_wl_fixed_t y)
{
	struct seat *seat = data;

	pointer_position(seat, x, y);
	seat->listener->motion(seat, seat->pointer_x, seat->pointer_y);
}

static void pointer_button(void *data, struct poor_wl_pointer *, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	struct seat *seat = data;

	seat->serial = serial;
	seat->listener->button(seat, seat->pointer_x, seat->pointer_y, button, state == POOR_WL_POINTER_BUTTON_STATE_PRESSED, time,
			       modifiers(seat));
}

static void pointer_axis(void *data, struct poor_wl_pointer *, uint32_t, uint32_t axis, poor_wl_fixed_t value)
{
	struct seat *seat = data;

	if (axis < ARRAY_SIZE(seat->scroll))
		seat->scroll[axis] += poor_wl_fixed_to_double(value) * seat->scale / 120;
}

/* A wheel scrolls by lines or columns per notch, a touchpad by pixels */
static void pointer_frame(void *data, struct poor_wl_pointer *)
{
	struct seat *seat = data;
	double cells[] = { seat->cell_height, seat->cell_width };
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
	if (steps[0] || steps[1])
		seat->listener->scroll(seat, seat->pointer_x, seat->pointer_y, steps[0], steps[1]);
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

static bool latin(xkb_keysym_t sym)
{
	return sym >= XKB_KEY_space && sym <= XKB_KEY_ydiaeresis;
}

/* In a layout without Latin letters, like Russian, shortcuts take the key's Latin keysym from the first layout that has one */
static xkb_keysym_t shortcut_sym(struct xkb_state *state, xkb_keycode_t key)
{
	struct xkb_keymap *keymap = xkb_state_get_keymap(state);
	xkb_layout_index_t active = xkb_state_key_get_layout(state, key);
	xkb_keysym_t sym = xkb_state_key_get_one_sym(state, key);
	const xkb_keysym_t *syms;

	if (latin(sym))
		return sym;
	for (xkb_layout_index_t layout = 0; layout < xkb_keymap_num_layouts_for_key(keymap, key); layout++) {
		xkb_level_index_t level = xkb_state_key_get_level(state, key, layout);

		if (layout != active && xkb_keymap_key_get_syms_by_level(keymap, key, layout, level, &syms) == 1 &&
		    latin(syms[0]))
			return syms[0];
	}
	return sym;
}

/* Returns whether the key repeats */
static bool key_action(struct seat *seat, uint32_t key)
{
	char text[SEAT_TEXT];
	int size = xkb_state_key_get_utf8(seat->state, key + 8, text, sizeof(text));

	if (size < 0 || (size_t)size >= sizeof(text))
		size = 0;
	return seat->listener->key(seat, xkb_keysym_to_lower(shortcut_sym(seat->state, key + 8)), modifiers(seat), size,
				   arrview_first(size, text));
}

static void seat_on_repeat(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct seat *seat = container_of(timer, struct seat, repeat);

	if (key_action(seat, seat->repeat_key))
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

static void stop_repeat(struct seat *seat)
{
	poor_loop_timer_disarm(&seat->repeat);
	seat->repeat_key = UINT32_MAX;
}

static void keyboard_leave(void *data, struct poor_wl_keyboard *, uint32_t, struct poor_wl_surface *)
{
	stop_repeat(data);
}

/*
 * A new key stops the repetition of the last one. A key repeats if its command does, by the timer or by the
 * compositor, which version 10 can do after a rate of 0.
 */
static void keyboard_key(void *data, struct poor_wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
{
	struct seat *seat = data;
	bool repeated = state == POOR_WL_KEYBOARD_KEY_STATE_REPEATED;

	if (state == POOR_WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (key == seat->repeat_key)
			stop_repeat(seat);
		return;
	}
	if (!seat->state || (repeated && key != seat->repeat_key))
		return;
	seat->serial = serial;
	if (!repeated)
		stop_repeat(seat);
	if (!key_action(seat, key) || !xkb_keymap_key_repeats(seat->keymap, key + 8)) {
		seat->repeat_key = UINT32_MAX;
		return;
	}
	if (repeated)
		return;
	seat->repeat_key = key;
	if (seat->repeat_rate > 0)
		poor_loop_timer_arm(seat->loop, &seat->repeat, poor_loop_now() + seat->repeat_delay * 1'000'000ull);
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
	seat->enter_serial = 0;
	seat->listener->leave(seat);
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
	foreach_array_ref(((struct seat_offer **[]){ &seat->clipboard_offer, &seat->primary_offer }), selection)
		offer_select(seat, *selection, nullptr);
	if (seat->have_data_device && poor_wl_data_device_get_version(&seat->data_device) >= POOR_WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
		poor_wl_data_device_release(&seat->data_device);
	else if (seat->have_data_device)
		poor_wl_data_device_destroy(&seat->data_device);
	if (seat->have_primary_device)
		poor_zwp_primary_selection_device_v1_destroy(&seat->primary_device);
	poor_wl_seat_release(&seat->wl_seat);
	seat->have_data_device = seat->have_primary_device = seat->have_seat = false;
}

bool seat_init(struct seat *seat, struct poor_loop *loop, const struct seat_listener *listener, struct seat_memory *memory)
{
	seat->loop = loop;
	seat->listener = listener;
	seat->clipboard.bytes = &memory->clipboard;
	seat->selection.bytes = &memory->primary;
	seat->paste.bytes = &memory->paste;
	foreach_array_ref(((struct seat_source *[]){ &seat->clipboard, &seat->selection }), source) {
		(*source)->seat = seat;
		(*source)->live = false;
		(*source)->current = 0;
		fill_array((*source)->transfers, 0);
	}
	foreach_array_ref(seat->offers, offer)
		offer->used = false;
	seat->clipboard_offer = seat->primary_offer = nullptr;
	foreach_array_ref(seat->transfers, transfer)
		transfer->used = false;
	poor_loop_op_init(&seat->paste.pipe_op, paste_on_pipe);
	poor_loop_op_init(&seat->paste.read_op, paste_on_read);
	seat->paste.offer = nullptr;
	seat->paste.busy = false;
	poor_loop_timer_init(&seat->repeat, seat_on_repeat);
	seat->keymap = nullptr;
	seat->state = nullptr;
	seat->repeat_key = UINT32_MAX;
	seat->serial = seat->enter_serial = 0;
	seat->shape = POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
	seat->repeat_rate = 25;
	seat->repeat_delay = 600;
	seat->cell_width = seat->cell_height = 1;
	seat->scale = 120;
	fill_array(seat->wheel, 0);
	fill_array(seat->scroll, 0);
	fill_array(seat->wheel_moved, false);
	seat->have_seat = seat->have_pointer = seat->have_keyboard = false;
	seat->have_shape_manager = seat->have_shape_device = false;
	seat->have_data_manager = seat->have_data_device = seat->have_primary_manager = seat->have_primary_device = false;
	seat->shape_set = seat->closing = false;
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
		offer->primary = !data;
		return data ? &offer->data.proxy : &offer->primary_offer.proxy;
	}
	log_printf("poor_text_viewer: too many offers\n");
	return nullptr;
}

void seat_resize(struct seat *seat, uint32_t scale, int32_t cell_width, int32_t cell_height)
{
	seat->scale = scale;
	seat->cell_width = cell_width;
	seat->cell_height = cell_height;
}

/* Pastes in progress end early, the other clients see the end of their pipes */
void seat_fini(struct seat *seat)
{
	seat->closing = true;
	foreach_array_ref(seat->transfers, transfer)
		if (transfer->used && transfer->write_op.pending)
			io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(seat->loop)), &transfer->write_op, 0);
	foreach_array_ref(((struct poor_loop_op *[]){ &seat->paste.pipe_op, &seat->paste.read_op }), op)
		if (seat->paste.busy && (*op)->pending)
			io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(seat->loop)), *op, 0);
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
	return seat->paste.busy;
}
