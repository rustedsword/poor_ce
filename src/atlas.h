/* SPDX-License-Identifier: MIT */
#ifndef ATLAS_H
#define ATLAS_H

#include "font.h"

constexpr size_t ATLAS_SIZE = 2048;
constexpr size_t ATLAS_GLYPHS = 8192;

struct atlas_glyph {
	uint32_t codepoint;
	uint16_t x, y;
	bool used;
};

/*
 * Glyphs of a font in cells of an image. The rows from 'dirty_top' to 'dirty_bottom' changed since the last upload.
 * At most half of 'glyphs' is used, so that a lookup finds its glyph or a free entry soon.
 */
struct atlas {
	const struct font *font;
	struct atlas_glyph glyphs[ATLAS_GLYPHS];
	size_t count, capacity, columns;
	size_t dirty_top, dirty_bottom;
	uint8_t pixels[ATLAS_SIZE][ATLAS_SIZE];
};

void atlas_init(struct atlas *atlas, const struct font *font);
/* Forget all glyphs, when the font changed or the atlas is full */
void atlas_clear(struct atlas *atlas);
/* The cell of 'codepoint', which is drawn if it is not there yet. Returns false if the atlas is full. */
[[nodiscard]] bool atlas_find(struct atlas *atlas, uint32_t codepoint, uint16_t *x, uint16_t *y);
void atlas_uploaded(struct atlas *atlas);

#endif
