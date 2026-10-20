/* SPDX-License-Identifier: MIT */
#ifndef TEXT_H
#define TEXT_H

#include <poor_array.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t TEXT_TAB = 8;
constexpr uint32_t TEXT_REPLACEMENT = 0xfffd;
constexpr size_t TEXT_MARKS = 2;

struct text_changes {
	size_t first, last;
	ptrdiff_t lines;
};

struct text {
	char *bytes;
	uint32_t *lines;
	uint8_t *states;
	size_t size, capacity;
	size_t line_count, line_capacity;
	size_t columns, widest;
	struct text_changes changes[TEXT_MARKS];
};

/* A character of a line: its bytes, the columns that it covers and what shows */
struct text_char {
	size_t offset, next, end;
	size_t column, width;
	uint32_t codepoint;
};

/* The code point at the start of 'bytes' and its length, invalid UTF-8 is U+FFFD of one byte */
uint32_t utf8_decode_impl(size_t size, const char (*bytes)[size], size_t *length);
#define utf8_decode(bytes, length) utf8_decode_impl(ARRAY_SIZE(bytes), &auto_arr(bytes), length)

/* An empty document in 'bytes', with as many lines as 'lines' and 'states' hold */
void text_init_impl(struct text *text, size_t capacity, char (*bytes)[capacity], size_t line_capacity,
		    uint32_t (*lines)[line_capacity], size_t state_capacity, uint8_t (*states)[state_capacity]);
#define text_init(text, bytes, lines, states) \
	text_init_impl(text, ARRAY_SIZE(bytes), &auto_arr(bytes), ARRAY_SIZE(lines), &auto_arr(lines), ARRAY_SIZE(states), \
		       &auto_arr(states))

/* Index the first 'size' bytes, which the caller put there. If their lines don't fit, the rest is cut off. */
[[nodiscard]] bool text_load(struct text *text, size_t size);
/* Whether the document has room to replace the bytes from 'start' to 'end' with 'bytes' */
bool text_fits_impl(const struct text *text, size_t start, size_t end, size_t size, const char (*bytes)[size]);
#define text_fits(text, start, end, bytes) text_fits_impl(text, start, end, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* Replace the bytes from 'start' to 'end' with 'bytes', unless the document has no room for them */
[[nodiscard]] bool text_replace_impl(struct text *text, size_t start, size_t end, size_t size, const char (*bytes)[size]);
#define text_replace(text, start, end, bytes) text_replace_impl(text, start, end, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* The lines from 'first' to 'last' that aren't empty get 'bytes' at their starts, unless the document has no room */
[[nodiscard]] bool text_prefix_impl(struct text *text, size_t first, size_t last, size_t size, const char (*bytes)[size]);
#define text_prefix(text, first, last, bytes) text_prefix_impl(text, first, last, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* The lines from 'first' to 'last' lose as many bytes at their starts as 'cut' counts between their start and end */
void text_unprefix(struct text *text, size_t first, size_t last, size_t (*cut)(const void *data, size_t start, size_t end),
		   const void *data);

void text_mark(struct text *text, size_t mark);
size_t text_marked_line(const struct text *text, size_t mark, size_t line);

size_t text_line_start(const struct text *text, size_t line);
/* Where the line break of the line starts, or the end of the document */
size_t text_line_end(const struct text *text, size_t line);
size_t text_line_of(const struct text *text, size_t offset);

/* The character boundaries after and before 'offset'. A line break is one character, a combining mark goes with the one before it. */
size_t text_next(const struct text *text, size_t offset);
size_t text_prev(const struct text *text, size_t offset);
/* The character boundary at 'offset', or the one before it */
size_t text_boundary(const struct text *text, size_t offset);

void text_chars_init(const struct text *text, size_t line, struct text_char *c);
bool text_chars_next(const struct text *text, struct text_char *c);

/* The column where the character at 'offset' of 'line' starts */
size_t text_column_of(const struct text *text, size_t line, size_t offset);
/* The character boundary of 'line' that is the nearest to the boundary before 'column', not before a combining mark */
size_t text_offset_at(const struct text *text, size_t line, size_t column);

#endif
