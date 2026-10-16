/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "font.h"

static const unsigned char font_data[] = {
#embed "../data/NotoSansMono-Regular.ttf"
};

bool font_init(struct font *font, uint32_t pixels)
{
	if (FT_Init_FreeType(&font->library))
		return false;
	if (FT_New_Memory_Face(font->library, font_data, ARRAY_SIZE(font_data), 0, &font->face)) {
		FT_Done_FreeType(font->library);
		return false;
	}
	if (!font_resize(font, pixels)) {
		font_fini(font);
		return false;
	}
	return true;
}

/* The advance of a monospace font is the same for all characters */
bool font_resize(struct font *font, uint32_t pixels)
{
	const FT_Size_Metrics *metrics;

	if (FT_Set_Pixel_Sizes(font->face, 0, pixels) || FT_Load_Char(font->face, 'M', FT_LOAD_TARGET_LIGHT))
		return false;
	metrics = &font->face->size->metrics;
	font->ascent = (metrics->ascender + 63) >> 6;
	font->cell_height = font->ascent + ((-metrics->descender + 63) >> 6);
	font->cell_width = (font->face->glyph->advance.x + 32) >> 6;
	return font->cell_width > 0 && font->cell_height > 0;
}

void font_fini(struct font *font)
{
	FT_Done_Face(font->face);
	FT_Done_FreeType(font->library);
}

void font_draw_impl(const struct font *font, uint32_t codepoint, size_t rows, size_t columns, uint8_t (*pixels)[rows][columns],
		    size_t x, size_t y)
{
	FT_GlyphSlot glyph = font->face->glyph;
	int32_t top, left, first_row, end_row, first_column, end_column;

	assert(x + font->cell_width <= columns && y + font->cell_height <= rows);
	foreach_array_ref(arrview(y, font->cell_height, pixels), row)
		memset_array(arrview(x, font->cell_width, row), 0);
	if (FT_Load_Char(font->face, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) || glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
		return;
	assert(glyph->bitmap.pitch >= (int)glyph->bitmap.width);
	top = font->ascent - glyph->bitmap_top;
	left = glyph->advance.x ? glyph->bitmap_left : (font->cell_width - (int32_t)glyph->bitmap.width) / 2;
	first_row = top < 0 ? -top : 0;
	end_row = font->cell_height - top < (int32_t)glyph->bitmap.rows ? font->cell_height - top : (int32_t)glyph->bitmap.rows;
	first_column = left < 0 ? -left : 0;
	end_column = font->cell_width - left < (int32_t)glyph->bitmap.width ? font->cell_width - left : (int32_t)glyph->bitmap.width;
	if (first_row >= end_row || first_column >= end_column)
		return;
	auto bitmap = array_ptr((const uint8_t (*)[glyph->bitmap.pitch])glyph->bitmap.buffer, glyph->bitmap.rows);
	foreach_array_index(arrview(first_row, end_row - first_row, bitmap), row)
		copy_array(arrview(x + left + first_column, end_column - first_column, (*pixels)[y + top + first_row + row]),
			   arrview(first_column, end_column - first_column, (*bitmap)[first_row + row]));
}
