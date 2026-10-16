/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <poor_array.h>
#include <string.h>

#include "atlas.h"
#include "test-runner.h"
#include "text.h"
#include "view.h"

static uint32_t decode(const char *s, size_t size, size_t *length)
{
	return utf8_decode(array_ptr(s, size), length);
}

TEST(utf8_decoding)
{
	size_t length;

	assert(decode("a", 1, &length) == 'a' && length == 1);
	assert(decode("\xc3\xa9", 2, &length) == 0xe9 && length == 2);
	assert(decode("\xe2\x82\xac", 3, &length) == 0x20ac && length == 3);
	assert(decode("\xf0\x9f\x98\x80", 4, &length) == 0x1f600 && length == 4);
	assert(decode("\x80", 1, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xc0\xaf", 2, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xe0\x80\xaf", 3, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xed\xa0\x80", 3, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xf4\x90\x80\x80", 4, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xe2\x82", 2, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xe2\x28\xac", 3, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xf8\x88\x80\x80", 4, &length) == TEXT_REPLACEMENT && length == 1);
}

static void text_of(struct text *text, const char *s, size_t capacity, uint32_t (*lines)[capacity])
{
	text_init(text, array_ptr(s, strlen(s)), arrview_first(capacity, lines));
}

TEST(text_lines)
{
	uint32_t lines[8];
	struct text text;

	text_of(&text, "a\nbc\n\nd", ARRAY_SIZE(lines), &lines);
	assert(text.line_count == 4 && lines[1] == 2 && lines[2] == 5 && lines[3] == 6);
	assert(text_line_end(&text, 1) == 4 && text_line_end(&text, 2) == 5 && text_line_end(&text, 3) == 7);
	assert(text_line_of(&text, 0) == 0 && text_line_of(&text, 4) == 1 && text_line_of(&text, 5) == 2 && text_line_of(&text, 7) == 3);
	assert(text.columns == 2);
	text_of(&text, "a\n", ARRAY_SIZE(lines), &lines);
	assert(text.line_count == 1 && text_line_end(&text, 0) == 1);
	text_of(&text, "", ARRAY_SIZE(lines), &lines);
	assert(text.line_count == 1 && text_line_end(&text, 0) == 0 && text.columns == 0);
	text_of(&text, "ab\r\ncd\r\n", ARRAY_SIZE(lines), &lines);
	assert(text.line_count == 2 && text_line_end(&text, 0) == 2 && text_line_end(&text, 1) == 6);
	text_of(&text, "a\nb\nc", 2, &lines);
	assert(text.line_count == 2 && text.size == 3 && text_line_end(&text, 1) == 3);
}

TEST(text_columns)
{
	uint32_t lines[8];
	struct text text;

	text_of(&text, "\tx\nab\tc\nh\xc3\xa9llo\na\tb\ne\xcc\x81x", ARRAY_SIZE(lines), &lines);
	assert(text_column_of(&text, 0, 1) == 8 && text_column_of(&text, 1, 6) == 8);
	assert(text.columns == 9);
	assert(text_offset_at(&text, 2, 2) == 11 && text_column_of(&text, 2, 11) == 2);
	assert(text_offset_at(&text, 2, 100) == text_line_end(&text, 2));
	assert(text_offset_at(&text, 3, 3) == 16 && text_offset_at(&text, 3, 6) == 17 && text_offset_at(&text, 3, 8) == 17);
	assert(text_offset_at(&text, 4, 1) == 22 && text_column_of(&text, 4, 22) == 1);
}

static const char numbered[] = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n"
			       "a long line of fourty characters at most\n21";

TEST(view_scrolling)
{
	uint32_t lines[32];
	struct text text;
	struct view view;

	text_of(&text, numbered, ARRAY_SIZE(lines), &lines);
	view_init(&view, &text);
	view_resize(&view, 104, 104, 10, 20, 4);
	assert(view_rows(&view) == 5 && view_columns(&view) == 10 && view_page(&view) == 4);
	assert(view_scroll(&view, 3, 0) && view.top == 3);
	assert(view_scroll(&view, PTRDIFF_MAX, 0) && view.top == 22 - 5);
	assert(!view_scroll(&view, 1, 0));
	assert(view_scroll(&view, PTRDIFF_MIN, 0) && view.top == 0);
	assert(view_scroll(&view, 0, PTRDIFF_MAX) && view.left == 40 - 10);
	assert(view_scroll(&view, 0, -5) && view.left == 25);
	view_resize(&view, 1000, 1000, 10, 20, 4);
	assert(view.top == 0 && view.left == 0);
}

TEST(view_selecting)
{
	uint32_t lines[32];
	struct text text;
	struct view view;
	size_t start, end;

	text_of(&text, numbered, ARRAY_SIZE(lines), &lines);
	view_init(&view, &text);
	view_resize(&view, 104, 104, 10, 20, 4);
	assert(view_hit(&view, 4, 4) == 0 && view_hit(&view, 9, 4) == 1 && view_hit(&view, -50, -50) == 0);
	assert(view_hit(&view, 4, 24) == 2 && view_hit(&view, 1000, 24) == 3);
	view.top = 20;
	assert(view_hit(&view, 26, 4) == text.lines[20] + 2 && view_hit(&view, 4, 1000) == text.lines[21]);
	view.top = 0;
	assert(view_select(&view, 2, 2) && !view_selection(&view, &start, &end));
	assert(view_drag(&view, 1000, 44));
	assert(view_selection(&view, &start, &end) && start == 2 && end == 5);
	assert(view_drag(&view, 4, 1000) && view.top == 1 && view.left == 1);
	assert(view_selection(&view, &start, &end) && start == 2 && end == text_line_end(&text, 5));
	assert(view_select(&view, 9, 3) && view_selection(&view, &start, &end) && start == 3 && end == 9);
}

TEST(atlas_cells)
{
	static struct atlas atlas;
	struct font font;
	uint16_t x, y, x2, y2;
	uint32_t codepoint = 0x4e00;

	assert(font_init(&font, 15) && font.cell_width > 0 && font.cell_height > font.cell_width);
	atlas_init(&atlas, &font);
	assert(atlas.dirty_top >= atlas.dirty_bottom);
	assert(atlas_find(&atlas, 'a', &x, &y) && x == 0 && y == 0);
	assert(atlas_find(&atlas, 'b', &x2, &y2) && x2 == font.cell_width && y2 == 0);
	assert(atlas_find(&atlas, 'a', &x, &y) && x == 0 && y == 0 && atlas.count == 2);
	assert(atlas.dirty_top == 0 && atlas.dirty_bottom == (size_t)font.cell_height);
	atlas_uploaded(&atlas);
	while (atlas_find(&atlas, codepoint, &x, &y))
		codepoint++;
	assert(atlas.count == atlas.capacity && atlas.capacity <= ATLAS_GLYPHS / 2);
	atlas_clear(&atlas);
	assert(atlas.count == 0 && atlas_find(&atlas, 'b', &x, &y) && x == 0 && y == 0);
	assert(font_resize(&font, 60) && font.cell_width > 30);
	atlas_clear(&atlas);
	assert(atlas.capacity == (ATLAS_SIZE / font.cell_width) * (ATLAS_SIZE / font.cell_height));
	font_fini(&font);
}
