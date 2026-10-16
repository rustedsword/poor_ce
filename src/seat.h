/* SPDX-License-Identifier: MIT */
#ifndef SEAT_H
#define SEAT_H

#include <poor_loop_fd.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "poor_wayland_client.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "view.h"

constexpr size_t SEAT_OFFERS = 8;
constexpr size_t SEAT_TRANSFERS = 4;
constexpr ptrdiff_t SEAT_WHEEL_LINES = 3;

struct seat;

struct seat_listener {
	/* The view changed, by scrolling or selecting */
	void (*changed)(struct seat *seat);
	/* Ctrl+Q */
	void (*quit)(struct seat *seat);
};

poor_loop_fd_declare(seat_transfer_file);

/* Bytes of the text from 'offset' to 'end' that go into the pipe of a client that pastes them */
struct seat_transfer {
	seat_transfer_file file;
	struct poor_loop_op write_op;
	struct seat *seat;
	size_t offset, end;
	bool used;
};

/* A clipboard or a primary selection source, which offers the bytes of the text from 'start' to 'end' */
struct seat_source {
	union {
		struct poor_wl_data_source data;
		struct poor_zwp_primary_selection_source_v1 primary;
	};
	struct seat *seat;
	size_t start, end;
	bool live;
};

/* The memory of an offer that the compositor makes, the offers are destroyed as they come since nothing is pasted */
struct seat_offer {
	union {
		struct poor_wl_data_offer data;
		struct poor_zwp_primary_selection_offer_v1 primary;
	};
	bool used;
};

/*
 * Input of the first seat for a view: the pointer selects and scrolls, the keyboard scrolls, Ctrl+C copies the
 * selection to the clipboard, Ctrl+A selects all and Ctrl+Q quits. Selections become primary selections.
 */
struct seat {
	struct poor_loop *loop;
	struct view *view;
	const struct seat_listener *listener;
	struct poor_wl_seat wl_seat;
	struct poor_wl_pointer pointer;
	struct poor_wl_keyboard keyboard;
	struct poor_wp_cursor_shape_manager_v1 shape_manager;
	struct poor_wp_cursor_shape_device_v1 shape_device;
	struct poor_wl_data_device_manager data_manager;
	struct poor_wl_data_device data_device;
	struct poor_zwp_primary_selection_device_manager_v1 primary_manager;
	struct poor_zwp_primary_selection_device_v1 primary_device;
	struct seat_source clipboard, selection;
	struct seat_offer offers[SEAT_OFFERS];
	struct seat_transfer transfers[SEAT_TRANSFERS];
	struct poor_loop_timer repeat;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	uint32_t seat_name, repeat_key, key_serial;
	int32_t repeat_rate, repeat_delay;
	int32_t wheel[2];
	double scroll[2];
	double pointer_x, pointer_y;
	bool have_seat, have_pointer, have_keyboard, have_shape_manager, have_shape_device;
	bool have_data_manager, have_data_device, have_primary_manager, have_primary_device;
	bool wheel_moved[2];
	bool closing;
};

[[nodiscard]] bool seat_init(struct seat *seat, struct poor_loop *loop, struct view *view, const struct seat_listener *listener);
/* Bind the globals of input and of selections, false for other globals */
bool seat_global(struct seat *seat, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
void seat_global_remove(struct seat *seat, uint32_t name);
/* The memory of the objects that the compositor creates, for poor_wl_display_set_new_proxy() */
struct poor_wl_proxy *seat_new_proxy(struct seat *seat, const struct poor_wl_interface *interface);
/* Release the objects and stop copying, wait until seat_busy() is false before the memory goes */
void seat_fini(struct seat *seat);
bool seat_busy(const struct seat *seat);

#endif
