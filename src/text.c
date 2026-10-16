/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "text.h"

uint32_t utf8_decode_impl(size_t size, const char (*bytes)[size], size_t *length)
{
	auto s = (const unsigned char (*)[size])bytes;
	unsigned char lead = (*s)[0];
	size_t n = lead < 0x80 ? 1 : lead < 0xc2 ? 0 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : lead < 0xf5 ? 4 : 0;
	uint32_t c;

	*length = 1;
	if (n == 1)
		return lead;
	if (!n || n > size)
		return TEXT_REPLACEMENT;
	c = lead & (0x7f >> n);
	foreach_array_const_ref(arrview(1, n - 1, s), byte) {
		if ((*byte & 0xc0) != 0x80)
			return TEXT_REPLACEMENT;
		c = c << 6 | (*byte & 0x3f);
	}
	if ((n == 3 && (c < 0x800 || (c >= 0xd800 && c < 0xe000))) || (n == 4 && (c < 0x10000 || c > 0x10ffff)))
		return TEXT_REPLACEMENT;
	*length = n;
	return c;
}

void text_init_impl(struct text *text, size_t size, const char (*bytes)[size], size_t capacity, uint32_t (*lines)[capacity])
{
	struct text_char c;

	assert(capacity && size <= UINT32_MAX);
	text->bytes = *bytes;
	text->lines = *lines;
	text->size = size;
	text->line_count = 1;
	(*lines)[0] = 0;
	foreach_array_index(bytes, i) {
		if ((*bytes)[i] != '\n' || i + 1 == size)
			continue;
		if (text->line_count == capacity) {
			text->size = i;
			break;
		}
		(*lines)[text->line_count++] = i + 1;
	}
	text->columns = 0;
	foreach_array_index(arrview_first(text->line_count, lines), line) {
		text_chars_init(text, line, &c);
		while (text_chars_next(text, &c))
			;
		if (c.column + c.width > text->columns)
			text->columns = c.column + c.width;
	}
}

size_t text_line_start(const struct text *text, size_t line)
{
	return text->lines[line];
}

size_t text_line_end(const struct text *text, size_t line)
{
	size_t start = text->lines[line], end = line + 1 < text->line_count ? text->lines[line + 1] - 1 : text->size;

	if (end > start && end == text->size && text->bytes[end - 1] == '\n')
		end--;
	if (end > start && text->bytes[end - 1] == '\r')
		end--;
	return end;
}

size_t text_line_of(const struct text *text, size_t offset)
{
	size_t low = 0, high = text->line_count;

	while (high - low > 1) {
		size_t middle = low + (high - low) / 2;

		if (text->lines[middle] <= offset)
			low = middle;
		else
			high = middle;
	}
	return low;
}

void text_chars_init(const struct text *text, size_t line, struct text_char *c)
{
	c->next = text->lines[line];
	c->end = text_line_end(text, line);
	c->column = c->width = 0;
}

/* Combining marks go over the character before them */
static bool combining(uint32_t c)
{
	static const uint32_t ranges[][2] = {
		{ 0x300, 0x36f }, { 0x483, 0x489 }, { 0x1ab0, 0x1aff }, { 0x1dc0, 0x1dff }, { 0x20d0, 0x20ff }, { 0xfe20, 0xfe2f },
	};

	foreach_array_const_ref(ranges, range)
		if (c >= (*range)[0] && c <= (*range)[1])
			return true;
	return false;
}

/* Tabs go to the next tab stop and show nothing, other control characters show U+FFFD */
bool text_chars_next(const struct text *text, struct text_char *c)
{
	size_t length;
	uint32_t codepoint;

	if (c->next >= c->end)
		return false;
	c->offset = c->next;
	c->column += c->width;
	codepoint = utf8_decode(array_ptr(text->bytes + c->offset, c->end - c->offset), &length);
	c->next = c->offset + length;
	c->width = codepoint == '\t' ? TEXT_TAB - c->column % TEXT_TAB : combining(codepoint) ? 0 : 1;
	c->codepoint = codepoint == '\t' ? ' ' : codepoint < 0x20 || codepoint == 0x7f ? TEXT_REPLACEMENT : codepoint;
	return true;
}

size_t text_column_of(const struct text *text, size_t line, size_t offset)
{
	struct text_char c;

	text_chars_init(text, line, &c);
	while (text_chars_next(text, &c))
		if (c.offset >= offset)
			return c.column;
	return c.column + c.width;
}

size_t text_offset_at(const struct text *text, size_t line, size_t column)
{
	struct text_char c;

	text_chars_init(text, line, &c);
	while (text_chars_next(text, &c)) {
		if (column <= c.column && c.width)
			return c.offset;
		if (column < c.column + c.width)
			return column - c.column <= c.column + c.width - column ? c.offset : c.next;
	}
	return c.end;
}
