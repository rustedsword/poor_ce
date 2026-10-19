/* SPDX-License-Identifier: MIT */
#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>

#include "quad.h"

constexpr size_t FRAME_QUADS = 1 << 18;
constexpr size_t FRAME_GROUPS = 1 << 12;

/*
 * The quads of a frame of 'width' x 'height' pixels with glyphs of the atlas of 'generation'. Its groups start at the
 * quads in 'groups' and keep their place from frame to frame, like the rows of the text.
 */
struct frame {
	size_t count, group_count;
	int32_t width, height;
	uint64_t generation;
	uint32_t groups[FRAME_GROUPS];
	struct quad quads[FRAME_QUADS];
};

#endif
