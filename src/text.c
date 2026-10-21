/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>

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

void text_mark(struct text *text, size_t mark)
{
	assert(mark < TEXT_MARKS);
	text->changes[mark] = (struct text_changes){ .first = SIZE_MAX, .last = 0, .lines = 0 };
}

static void mark_all(struct text *text)
{
	foreach_array_index(text->changes, mark)
		text_mark(text, mark);
}

void text_init_impl(struct text *text, size_t capacity, char (*bytes)[capacity], size_t line_capacity,
		    uint32_t (*lines)[line_capacity], size_t state_capacity, uint8_t (*states)[state_capacity])
{
	assert(line_capacity && line_capacity == state_capacity && capacity <= UINT32_MAX);
	text->bytes = *bytes;
	text->lines = *lines;
	text->states = *states;
	text->capacity = capacity;
	text->line_capacity = line_capacity;
	text->size = 0;
	text->line_count = 1;
	text->lines[0] = 0;
	text->states[0] = 0;
	text->columns = 0;
	text->widest = 1;
	mark_all(text);
}

static void changed(struct text *text, size_t first, size_t last, size_t count)
{
	ptrdiff_t added = (ptrdiff_t)count - (ptrdiff_t)(last - first + 1);
	size_t end = first + count - 1;

	foreach_array_ref(text->changes, changes) {
		size_t from = changes->first, to = changes->last;

		if (from <= to) {
			from = from < first ? from : from > last ? from + added : first;
			to = to < first ? to : to > last ? to + added : end;
		}
		changes->first = from < first ? from : first;
		changes->last = from <= to && to > end ? to : end;
		changes->lines += added;
	}
	if (!text->widest && first <= text->remeasured) {
		text->remeasured = last < text->remeasured ? (size_t)((ptrdiff_t)text->remeasured + added) : first;
		text->measured = text->lines[text->remeasured];
		text->measured_columns = 0;
	}
}

size_t text_marked_line(const struct text *text, size_t mark, size_t line)
{
	const struct text_changes *changes;

	assert(mark < TEXT_MARKS);
	changes = &text->changes[mark];
	if (line < changes->first)
		return line;
	if (line > changes->last)
		return (size_t)((ptrdiff_t)line - changes->lines);
	return SIZE_MAX;
}

static size_t width(const struct text *text, size_t line)
{
	struct text_char c;

	text_chars_init(text, line, &c);
	while (text_chars_next(text, &c))
		;
	return c.column + c.width;
}

static void grow(size_t *columns, size_t *widest, size_t width)
{
	if (width > *columns) {
		*columns = width;
		*widest = 1;
	} else if (width == *columns) {
		++*widest;
	}
}

static void recount(struct text *text)
{
	text->remeasured = text->measured = text->measured_columns = 0;
	text->remeasured_columns = text->remeasured_widest = 0;
}

/* Lines that come or go, the widest ones are counted again once none of them is left */
static void measure(struct text *text, size_t first, size_t count, bool gone)
{
	foreach_array_index(arrview(first, count, array_ptr(text->lines, text->line_count)), i) {
		size_t columns = width(text, first + i);
		bool counted = !text->widest && first + i < text->remeasured;

		if (!gone) {
			if (counted)
				grow(&text->remeasured_columns, &text->remeasured_widest, columns);
			grow(&text->columns, &text->widest, columns);
			continue;
		}
		if (counted && columns == text->remeasured_columns && !--text->remeasured_widest)
			recount(text);
		if (columns == text->columns && !--text->widest)
			recount(text);
	}
}

void text_begin(struct text *text, size_t size)
{
	assert(size <= text->capacity);
	text->size = size;
	text->indexed = text->measured = text->measured_columns = 0;
	text->line_count = 1;
	text->columns = text->widest = 0;
	mark_all(text);
}

static void measure_to(struct text *text, size_t stop, size_t end)
{
	struct text_char c;

	c.next = text->measured;
	c.end = end;
	c.column = text->measured_columns;
	c.width = 0;
	while (c.next < stop && text_chars_next(text, &c))
		;
	text->measured = c.next;
	text->measured_columns = c.column + c.width;
}

bool text_index(struct text *text, size_t budget, bool *fits)
{
	auto bytes = arrview_first(text->size, array_ptr(text->bytes, text->capacity));
	size_t first = text->line_count - 1, end = budget < text->size - text->indexed ? text->indexed + budget : text->size;

	assert(budget);
	*fits = true;
	for (const char *p = &(*bytes)[text->indexed]; (p = memchr(p, '\n', &(*bytes)[end] - p)); p++) {
		if (text->line_count == text->line_capacity) {
			end = text->size = p - *bytes;
			*fits = false;
			break;
		}
		text->states[text->line_count] = 0;
		text->lines[text->line_count++] = p + 1 - *bytes;
	}
	text->indexed = end;
	for (size_t line = first; line + 1 < text->line_count; line++) {
		size_t line_end = text_line_end(text, line);

		measure_to(text, line_end, line_end);
		grow(&text->columns, &text->widest, text->measured_columns);
		text->measured = text->lines[line + 1];
		text->measured_columns = 0;
	}
	if (end < text->size) {
		/* A character in the last three bytes may go on after them, and a \r there may start the line break */
		measure_to(text, end > 3 ? end - 3 : 0, end);
		return false;
	}
	measure_to(text, text->size, text->size);
	grow(&text->columns, &text->widest, text->measured_columns);
	return true;
}

bool text_remeasuring(const struct text *text)
{
	return !text->widest;
}

bool text_remeasure(struct text *text, size_t budget)
{
	size_t end;

	assert(budget);
	if (text->widest)
		return true;
	end = budget < SIZE_MAX - text->measured ? text->measured + budget : SIZE_MAX;
	while (text->measured < end) {
		size_t line_end = text_line_end(text, text->remeasured);

		measure_to(text, end < line_end ? end : line_end, line_end);
		if (text->measured < line_end)
			return false;
		grow(&text->remeasured_columns, &text->remeasured_widest, text->measured_columns);
		if (++text->remeasured == text->line_count) {
			text->columns = text->remeasured_columns;
			text->widest = text->remeasured_widest;
			return true;
		}
		text->measured = text->lines[text->remeasured];
		text->measured_columns = 0;
	}
	return false;
}

static size_t breaks(size_t size, const char (*bytes)[size])
{
	size_t count = 0;

	foreach_array_const_ref(bytes, byte)
		count += *byte == '\n';
	return count;
}

bool text_fits_impl(const struct text *text, size_t start, size_t end, size_t size, const char (*bytes)[size])
{
	size_t lines = text_line_of(text, end) - text_line_of(text, start);

	return text->size - (end - start) + size <= text->capacity &&
	       text->line_count - lines + breaks(size, bytes) <= text->line_capacity;
}

/* The line starts from 'start' to 'end' go, the line breaks of 'bytes' start lines after the line of 'start' */
bool text_replace_impl(struct text *text, size_t start, size_t end, size_t size, const char (*bytes)[size])
{
	auto all = array_ptr(text->bytes, text->capacity);
	auto lines = array_ptr(text->lines, text->line_capacity);
	auto states = array_ptr(text->states, text->line_capacity);
	size_t first = text_line_of(text, start), last = text_line_of(text, end), added = breaks(size, bytes);
	size_t line = first + 1, line_count = text->line_count - (last - first) + added;

	assert(start <= end && end <= text->size);
	if (!text_fits_impl(text, start, end, size, bytes))
		return false;
	measure(text, first, last - first + 1, true);
	auto tail = arrview(end, text->size - end, all);
	auto moved_lines = arrview(last + 1, text->line_count - last - 1, lines);
	auto moved_states = arrview(last + 1, ARRAY_SIZE(moved_lines), states);
	memmove(arrview(start + size, ARRAY_SIZE(tail), all), tail, ARRAY_SIZE_BYTES(tail));
	copy_array(arrview(start, size, all), bytes);
	memmove(arrview(first + 1 + added, ARRAY_SIZE(moved_lines), lines), moved_lines, ARRAY_SIZE_BYTES(moved_lines));
	memmove(arrview(first + 1 + added, ARRAY_SIZE(moved_states), states), moved_states, ARRAY_SIZE_BYTES(moved_states));
	foreach_array_index(bytes, i) {
		if ((*bytes)[i] != '\n')
			continue;
		(*lines)[line] = start + i + 1;
		(*states)[line++] = 0;
	}
	foreach_array_ref(arrview(line, line_count - line, lines), next)
		*next = *next - (end - start) + size;
	text->size = text->size - (end - start) + size;
	text->line_count = line_count;
	changed(text, first, last, added + 1);
	measure(text, first, added + 1, false);
	return true;
}

/* Where the line break before 'next' starts, of a line from 'start' */
static size_t line_break(const struct text *text, size_t start, size_t next)
{
	return next - 1 > start && text->bytes[next - 2] == '\r' ? next - 2 : next - 1;
}

bool text_prefix_impl(struct text *text, size_t first, size_t last, size_t size, const char (*bytes)[size])
{
	auto all = array_ptr(text->bytes, text->capacity);
	auto lines = array_ptr(text->lines, text->line_capacity);
	size_t end = text_line_end(text, last), next = end, count = 0, grow;

	assert(first <= last && last < text->line_count);
	for (size_t line = first; line <= last; line++)
		count += text_line_start(text, line) < text_line_end(text, line);
	grow = count * size;
	if (text->size + grow > text->capacity)
		return false;
	measure(text, first, last - first + 1, true);
	auto tail = arrview(end, text->size - end, all);
	memmove(arrview(end + grow, ARRAY_SIZE(tail), all), tail, ARRAY_SIZE_BYTES(tail));
	for (size_t line = last + 1, shift = grow; line-- > first;) {
		size_t start = (*lines)[line];
		bool full = start < (line == last ? end : line_break(text, start, next));
		auto moved = arrview(start, next - start, all);

		memmove(arrview(start + shift, ARRAY_SIZE(moved), all), moved, ARRAY_SIZE_BYTES(moved));
		if (full) {
			shift -= size;
			copy_array(arrview(start + shift, size, all), bytes);
		}
		(*lines)[line] = start + shift;
		next = start;
	}
	foreach_array_ref(arrview(last + 1, text->line_count - last - 1, lines), line)
		*line += grow;
	text->size += grow;
	changed(text, first, last, last - first + 1);
	measure(text, first, last - first + 1, false);
	return true;
}

void text_unprefix(struct text *text, size_t first, size_t last, size_t (*cut)(const void *data, size_t start, size_t end),
		   const void *data)
{
	auto all = array_ptr(text->bytes, text->capacity);
	auto lines = array_ptr(text->lines, text->line_capacity);
	size_t end = text_line_end(text, last), removed = 0;

	assert(first <= last && last < text->line_count);
	measure(text, first, last - first + 1, true);
	for (size_t line = first; line <= last; line++) {
		size_t start = (*lines)[line], next = line == last ? end : (*lines)[line + 1];
		size_t count = cut(data, start, line == last ? end : line_break(text, start, next));
		auto moved = arrview(start + count, next - start - count, all);

		memmove(arrview(start - removed, ARRAY_SIZE(moved), all), moved, ARRAY_SIZE_BYTES(moved));
		(*lines)[line] = start - removed;
		removed += count;
	}
	auto tail = arrview(end, text->size - end, all);
	memmove(arrview(end - removed, ARRAY_SIZE(tail), all), tail, ARRAY_SIZE_BYTES(tail));
	foreach_array_ref(arrview(last + 1, text->line_count - last - 1, lines), line)
		*line -= removed;
	text->size -= removed;
	changed(text, first, last, last - first + 1);
	measure(text, first, last - first + 1, false);
}

size_t text_line_start(const struct text *text, size_t line)
{
	return text->lines[line];
}

size_t text_line_end(const struct text *text, size_t line)
{
	if (line + 1 == text->line_count)
		return text->size;
	return line_break(text, text->lines[line], text->lines[line + 1]);
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

size_t text_next(const struct text *text, size_t offset)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t line = text_line_of(text, offset), end = text_line_end(text, line), length;

	if (offset >= end)
		return line + 1 < text->line_count ? text->lines[line + 1] : text->size;
	do {
		utf8_decode(arrview(offset, end - offset, all), &length);
		offset += length;
	} while (offset < end && combining(utf8_decode(arrview(offset, end - offset, all), &length)));
	return offset;
}

size_t text_prev(const struct text *text, size_t offset)
{
	size_t line = text_line_of(text, offset), boundary = text->lines[line];
	struct text_char c;

	if (offset == boundary)
		return line ? text_line_end(text, line - 1) : 0;
	text_chars_init(text, line, &c);
	while (text_chars_next(text, &c) && c.offset < offset)
		if (c.width)
			boundary = c.offset;
	return boundary;
}

size_t text_boundary(const struct text *text, size_t offset)
{
	size_t line = text_line_of(text, offset), boundary = text->lines[line];
	struct text_char c;

	text_chars_init(text, line, &c);
	if (offset >= c.end)
		return c.end;
	while (text_chars_next(text, &c) && c.offset <= offset)
		if (c.width)
			boundary = c.offset;
	return boundary;
}

void text_chars_init(const struct text *text, size_t line, struct text_char *c)
{
	c->next = text->lines[line];
	c->end = text_line_end(text, line);
	c->column = c->width = 0;
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
	codepoint = utf8_decode(arrview(c->offset, c->end - c->offset, array_ptr(text->bytes, text->size)), &length);
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
