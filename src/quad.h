/* SPDX-License-Identifier: MIT */
#ifndef QUAD_H
#define QUAD_H

#include <stdint.h>

/* The 'u' of a quad of one color */
constexpr uint16_t QUAD_SOLID = UINT16_MAX;

/* A rectangle of the image in 'color', which is RGBA: solid, or as much as an atlas cell of the same size covers it */
struct quad {
	int16_t x, y;
	uint16_t width, height;
	uint16_t u, v;
	uint32_t color;
};

static_assert(sizeof(struct quad) == 16);

/* RGBA of 0xRRGGBB */
#define QUAD_RGB(c) (0xff000000u | ((c) & 0xffu) << 16 | ((c) & 0xff00u) | ((c) >> 16 & 0xffu))

#endif
