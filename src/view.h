/* SPDX-License-Identifier: MIT */
#ifndef VIEW_H
#define VIEW_H

#include <stddef.h>
#include <stdint.h>

#include "text.h"

/*
 * The part of a text that shows in 'width' x 'height' buffer pixels, from line 'top' and column 'left', and the
 * selection between 'anchor' and 'caret'. 'scale' is in 120ths of a buffer pixel per surface unit.
 */
struct view {
	const struct text *text;
	size_t top, left;
	size_t anchor, caret;
	int32_t width, height;
	int32_t cell_width, cell_height;
	int32_t padding;
	uint32_t scale;
	bool selecting, focused;
};

void view_init(struct view *view, const struct text *text);
/* Keeps the scroll position within the text, call it after the text changes too */
void view_resize(struct view *view, int32_t width, int32_t height, int32_t cell_width, int32_t cell_height, int32_t padding);
/* Rows and columns that show, the last ones partly */
size_t view_rows(const struct view *view);
size_t view_columns(const struct view *view);
size_t view_page(const struct view *view);
bool view_scroll(struct view *view, ptrdiff_t lines, ptrdiff_t columns);
/* The character boundary next to buffer pixel 'x', 'y' */
size_t view_hit(const struct view *view, double x, double y);
bool view_select(struct view *view, size_t anchor, size_t caret);
/* Moves the caret to the pointer, or to the edge that the pointer is beyond while that scrolls */
bool view_drag(struct view *view, double x, double y);
bool view_selection(const struct view *view, size_t *start, size_t *end);

#endif
