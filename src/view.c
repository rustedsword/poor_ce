/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "view.h"

void view_init(struct view *view, const struct text *text)
{
	view->text = text;
	view->top = view->left = 0;
	view->anchor = view->caret = 0;
	view->width = view->height = 0;
	view->cell_width = view->cell_height = 1;
	view->padding = 0;
}

static size_t full_rows(const struct view *view)
{
	int32_t rows = (view->height - view->padding) / view->cell_height;

	return rows > 1 ? rows : 1;
}

static size_t full_columns(const struct view *view)
{
	int32_t columns = (view->width - view->padding) / view->cell_width;

	return columns > 1 ? columns : 1;
}

static size_t max_top(const struct view *view)
{
	return view->text->line_count > full_rows(view) ? view->text->line_count - full_rows(view) : 0;
}

/* The caret after the longest line shows too */
static size_t max_left(const struct view *view)
{
	return view->text->columns + 1 > full_columns(view) ? view->text->columns + 1 - full_columns(view) : 0;
}

static size_t step(size_t value, ptrdiff_t delta, size_t max)
{
	size_t distance = delta < 0 ? -(size_t)delta : (size_t)delta;

	if (value > max)
		value = max;
	if (delta < 0)
		return distance < value ? value - distance : 0;
	return distance < max - value ? value + distance : max;
}

void view_resize(struct view *view, int32_t width, int32_t height, int32_t cell_width, int32_t cell_height, int32_t padding)
{
	view->width = width;
	view->height = height;
	view->cell_width = cell_width;
	view->cell_height = cell_height;
	view->padding = padding;
	view_scroll(view, 0, 0);
}

size_t view_rows(const struct view *view)
{
	int32_t rows = (view->height - view->padding + view->cell_height - 1) / view->cell_height;

	return rows > 0 ? rows : 0;
}

size_t view_columns(const struct view *view)
{
	int32_t columns = (view->width - view->padding + view->cell_width - 1) / view->cell_width;

	return columns > 0 ? columns : 0;
}

size_t view_page(const struct view *view)
{
	return full_rows(view) > 1 ? full_rows(view) - 1 : 1;
}

bool view_scroll(struct view *view, ptrdiff_t lines, ptrdiff_t columns)
{
	size_t top = step(view->top, lines, max_top(view)), left = step(view->left, columns, max_left(view));
	bool changed = top != view->top || left != view->left;

	view->top = top;
	view->left = left;
	return changed;
}

bool view_reveal(struct view *view, size_t offset)
{
	size_t line = text_line_of(view->text, offset), column = text_column_of(view->text, line, offset);
	size_t rows = full_rows(view), columns = full_columns(view);
	bool changed = view_scroll(view, 0, 0);
	ptrdiff_t lines = 0, shift = 0;

	if (line < view->top)
		lines = -(ptrdiff_t)(view->top - line);
	else if (line >= view->top + rows)
		lines = line - (view->top + rows - 1);
	if (column < view->left)
		shift = -(ptrdiff_t)(view->left - column);
	else if (column >= view->left + columns)
		shift = column - (view->left + columns - 1);
	return view_scroll(view, lines, shift) || changed;
}

size_t view_hit(const struct view *view, double x, double y)
{
	double row = floor((y - view->padding) / view->cell_height), column = round((x - view->padding) / view->cell_width);
	double line = view->top + row;

	if (line < 0)
		line = 0;
	if (line > view->text->line_count - 1)
		line = view->text->line_count - 1;
	column += view->left;
	return text_offset_at(view->text, (size_t)line, column > 0 ? (size_t)column : 0);
}

bool view_select(struct view *view, size_t anchor, size_t caret)
{
	bool changed = anchor != view->anchor || caret != view->caret;

	view->anchor = anchor;
	view->caret = caret;
	return changed;
}

bool view_drag(struct view *view, double x, double y)
{
	bool changed = view_scroll(view, y < view->padding ? -1 : y >= view->height ? 1 : 0,
				   x < view->padding ? -1 : x >= view->width ? 1 : 0);

	x = x < view->padding ? view->padding : x > view->width ? view->width : x;
	y = y < view->padding ? view->padding : y >= view->height ? view->height - 1 : y;
	return view_select(view, view->anchor, view_hit(view, x, y)) || changed;
}

bool view_selection(const struct view *view, size_t *start, size_t *end)
{
	*start = view->anchor < view->caret ? view->anchor : view->caret;
	*end = view->anchor < view->caret ? view->caret : view->anchor;
	return *start != *end;
}
