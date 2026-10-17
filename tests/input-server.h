/* SPDX-License-Identifier: MIT */
#ifndef INPUT_SERVER_H
#define INPUT_SERVER_H

#include <poor_loop.h>
#include <stdint.h>

/*
 * A compositor for the input tests with one client: a surface, a seat with a pointer and a keyboard, a data device,
 * primary selections and cursor shapes. It records what the client asks for, and the tests drive the input.
 */
struct input_server_state {
	const char *offer_text;
	uint32_t shape, shape_serial;
	uint32_t selection_serial, primary_serial;
	unsigned selection_mimes, primary_mimes;
	unsigned offers, offers_destroyed, offers_received;
	int32_t repeat_rate, repeat_delay;
	bool selection_utf8, primary_utf8;
	bool has_surface, has_pointer, has_keyboard, has_data_device, has_primary_device, has_shape_device;
	bool has_selection, has_primary, client_done, display_done;
};

extern struct input_server_state input_server;

void input_server_start(struct poor_loop *loop);
/* Returns the fd of the client end of the connection */
int input_server_connect(void);
void input_server_stop(void);
void input_server_capabilities(uint32_t capabilities);

uint32_t input_server_pointer_enter(double x, double y);
void input_server_pointer_leave(void);
void input_server_pointer_motion(double x, double y);
uint32_t input_server_pointer_button(uint32_t button, bool pressed);
void input_server_pointer_wheel(int32_t value120);
uint32_t input_server_keyboard_enter(void);
void input_server_keyboard_modifiers(uint32_t depressed, uint32_t group);
uint32_t input_server_key(uint32_t key, bool pressed);
/* A repetition of a key that is down, after a repeat rate of 0 */
void input_server_key_repeated(uint32_t key);

/* The selection or the primary selection writes into the returned pipe */
int input_server_receive(bool primary, const char *mime);
/* A selection of another client, which the client gets an offer for. It pastes 'offer_text'. */
void input_server_offer(bool primary);

#endif
