/* SPDX-License-Identifier: MIT */
#ifndef SEAT_H
#define SEAT_H

#include <poor_loop_fd.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "poor_wayland_client.h"
#include "primary-selection-unstable-v1-client-protocol.h"

constexpr size_t SEAT_OFFERS = 8;
constexpr size_t SEAT_TRANSFERS = 4;
constexpr size_t SEAT_BYTES = 16 << 20;
constexpr size_t SEAT_TEXT = 64;
constexpr ptrdiff_t SEAT_WHEEL_LINES = 3;

enum : uint32_t {
	SEAT_SHIFT = 1 << 0,
	SEAT_CONTROL = 1 << 1,
	SEAT_ALT = 1 << 2,
};

struct seat;

/* Positions are in buffer pixels of the surface */
struct seat_listener {
	/*
	 * A key was pressed or repeats: 'sym' is lower case, and Latin if a layout has a Latin one for the key. 'text' is
	 * what the key types. Returns whether the key repeats.
	 */
	bool (*key)(struct seat *seat, xkb_keysym_t sym, uint32_t modifiers, size_t size, const char (*text)[size]);
	/* 'time' is in milliseconds */
	void (*button)(struct seat *seat, double x, double y, uint32_t button, bool pressed, uint32_t time, uint32_t modifiers);
	void (*motion)(struct seat *seat, double x, double y);
	/* The pointer left the window or went away, a button that is down comes up elsewhere if at all */
	void (*leave)(struct seat *seat);
	/* Rows and columns that a wheel or a touchpad scrolled by */
	void (*scroll)(struct seat *seat, double x, double y, ptrdiff_t rows, ptrdiff_t columns);
	/* What seat_paste() got, nothing if it failed */
	void (*paste)(struct seat *seat, size_t size, const char (*bytes)[size]);
};

poor_loop_fd_declare(seat_transfer_file);
poor_loop_fd_declare(seat_paste_file);

struct seat_source;

/* Bytes of a copy of a source that go into the pipe of a client that pastes them */
struct seat_transfer {
	seat_transfer_file file;
	struct poor_loop_op write_op;
	struct seat_source *source;
	size_t offset, end;
	unsigned copy;
	bool used;
};

/*
 * The clipboard or the primary selection, while the compositor has it. Of two copies, 'current' is offered, and the
 * other one may still be pasted by transfers that started before.
 */
struct seat_source {
	union {
		struct poor_wl_data_source data;
		struct poor_zwp_primary_selection_source_v1 primary;
	};
	struct seat *seat;
	char (*bytes)[2][SEAT_BYTES];
	size_t size[2];
	unsigned current, transfers[2];
	bool live;
};

/* The memory of an offer that the compositor makes, of the clipboard or of the primary selection, and its text types */
struct seat_offer {
	union {
		struct poor_wl_data_offer data;
		struct poor_zwp_primary_selection_offer_v1 primary_offer;
	};
	struct seat *seat;
	unsigned types;
	bool primary, used;
};

/* What another client pastes: it writes the pipe, until it closes it */
struct seat_paste {
	seat_paste_file file;
	struct poor_loop_op pipe_op, read_op;
	struct seat_offer *offer;
	char (*bytes)[SEAT_BYTES];
	int fds[2];
	size_t size;
	bool busy;
};

/* The copies of the clipboard and of the primary selection, and what is pasted */
struct seat_memory {
	char clipboard[2][SEAT_BYTES];
	char primary[2][SEAT_BYTES];
	char paste[SEAT_BYTES];
};

/*
 * Input of the first seat, and its clipboard and primary selection. The selections that other clients set are kept,
 * as 'clipboard_offer' and 'primary_offer', until they are pasted or replaced.
 */
struct seat {
	struct poor_loop *loop;
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
	struct seat_offer *clipboard_offer, *primary_offer;
	struct seat_transfer transfers[SEAT_TRANSFERS];
	struct seat_paste paste;
	struct poor_loop_timer repeat;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	uint32_t seat_name, repeat_key, serial, enter_serial, shape;
	int32_t repeat_rate, repeat_delay;
	int32_t cell_width, cell_height;
	uint32_t scale;
	int32_t wheel[2];
	double scroll[2];
	double pointer_x, pointer_y;
	bool have_seat, have_pointer, have_keyboard, have_shape_manager, have_shape_device;
	bool have_data_manager, have_data_device, have_primary_manager, have_primary_device;
	bool wheel_moved[2];
	bool shape_set, closing;
};

[[nodiscard]] bool seat_init(struct seat *seat, struct poor_loop *loop, const struct seat_listener *listener, struct seat_memory *memory);
/* Bind the globals of input and of selections, false for other globals */
bool seat_global(struct seat *seat, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
void seat_global_remove(struct seat *seat, uint32_t name);
/* The memory of the objects that the compositor creates, for poor_wl_display_set_new_proxy() */
struct poor_wl_proxy *seat_new_proxy(struct seat *seat, const struct poor_wl_interface *interface);
/* 'scale' is in 120ths of a buffer pixel per surface unit, a wheel scrolls by cells */
void seat_resize(struct seat *seat, uint32_t scale, int32_t cell_width, int32_t cell_height);
void seat_cursor(struct seat *seat, uint32_t shape);
/* Offer a copy of 'bytes' as the clipboard or the primary selection. False if there is no room for it. */
bool seat_copy_impl(struct seat *seat, bool primary, size_t size, const char (*bytes)[size]);
#define seat_copy(seat, primary, bytes) seat_copy_impl(seat, primary, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* Get the clipboard or the primary selection for the paste callback, which is called once unless this returns false */
bool seat_paste(struct seat *seat, bool primary);
/* Release the objects and stop copying and pasting, wait until seat_busy() is false before the memory goes */
void seat_fini(struct seat *seat);
bool seat_busy(const struct seat *seat);

#endif
