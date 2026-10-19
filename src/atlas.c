/* SPDX-License-Identifier: MIT */
#include "atlas.h"

void atlas_init(struct atlas *atlas, const struct font *font)
{
	atlas->font = font;
	atlas->generation = 0;
	atlas_clear(atlas);
}

void atlas_clear(struct atlas *atlas)
{
	size_t columns = ATLAS_SIZE / atlas->font->cell_width, slots = columns * (ATLAS_SIZE / atlas->font->cell_height);

	foreach_array_ref(atlas->glyphs, glyph)
		glyph->used = false;
	atlas->count = 0;
	atlas->columns = columns;
	atlas->capacity = slots < ATLAS_GLYPHS / 2 ? slots : ATLAS_GLYPHS / 2;
	atlas->generation++;
	atlas_uploaded(atlas);
}

bool atlas_find(struct atlas *atlas, uint32_t codepoint, uint16_t *x, uint16_t *y)
{
	size_t index = codepoint * 2654435761u % ATLAS_GLYPHS;
	struct atlas_glyph *glyph;

	while ((glyph = &atlas->glyphs[index])->used && glyph->codepoint != codepoint)
		index = (index + 1) % ATLAS_GLYPHS;
	if (!glyph->used) {
		if (atlas->count == atlas->capacity)
			return false;
		glyph->used = true;
		glyph->codepoint = codepoint;
		glyph->x = atlas->count % atlas->columns * atlas->font->cell_width;
		glyph->y = atlas->count / atlas->columns * atlas->font->cell_height;
		atlas->count++;
		font_draw(atlas->font, codepoint, atlas->pixels, glyph->x, glyph->y);
		if (glyph->y < atlas->dirty_top)
			atlas->dirty_top = glyph->y;
		if (glyph->y + (size_t)atlas->font->cell_height > atlas->dirty_bottom)
			atlas->dirty_bottom = glyph->y + atlas->font->cell_height;
	}
	*x = glyph->x;
	*y = glyph->y;
	return true;
}

void atlas_uploaded(struct atlas *atlas)
{
	atlas->dirty_top = ATLAS_SIZE;
	atlas->dirty_bottom = 0;
}
