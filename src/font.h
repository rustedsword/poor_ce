/* SPDX-License-Identifier: MIT */
#ifndef FONT_H
#define FONT_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <poor_array.h>
#include <stdint.h>

/* The embedded monospace font at a pixel size: characters go in cells as wide as the advance and as high as a line */
struct font {
	FT_Library library;
	FT_Face face;
	int32_t cell_width, cell_height, ascent;
};

[[nodiscard]] bool font_init(struct font *font, uint32_t pixels);
[[nodiscard]] bool font_resize(struct font *font, uint32_t pixels);
void font_fini(struct font *font);

/* Draw 'codepoint' into the cell at 'x', 'y' of 'pixels', clipped to the cell */
void font_draw_impl(const struct font *font, uint32_t codepoint, size_t rows, size_t columns, uint8_t (*pixels)[rows][columns],
		    size_t x, size_t y);
#define font_draw(font, codepoint, pixels, x, y) \
	font_draw_impl(font, codepoint, ARRAY_SIZE(pixels), ARRAY_SIZE(auto_arr(pixels)[0]), &auto_arr(pixels), x, y)

#endif
