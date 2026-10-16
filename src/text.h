/* SPDX-License-Identifier: MIT */
#ifndef TEXT_H
#define TEXT_H

#include <poor_array.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t TEXT_TAB = 8;
constexpr uint32_t TEXT_REPLACEMENT = 0xfffd;

/* A document in memory of the caller and where its lines start. A line break that ends the document starts no line. */
struct text {
	const char *bytes;
	const uint32_t *lines;
	size_t size;
	size_t line_count;
	size_t columns;
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

/* Index the lines of 'bytes', as many as 'lines' holds. The rest of the document is cut off. */
void text_init_impl(struct text *text, size_t size, const char (*bytes)[size], size_t capacity, uint32_t (*lines)[capacity]);
#define text_init(text, bytes, lines) \
	text_init_impl(text, ARRAY_SIZE(bytes), &auto_arr(bytes), ARRAY_SIZE(lines), &auto_arr(lines))

size_t text_line_start(const struct text *text, size_t line);
/* Where the line break of the line starts, or the end of the document */
size_t text_line_end(const struct text *text, size_t line);
size_t text_line_of(const struct text *text, size_t offset);

void text_chars_init(const struct text *text, size_t line, struct text_char *c);
bool text_chars_next(const struct text *text, struct text_char *c);

/* The column where the character at 'offset' of 'line' starts */
size_t text_column_of(const struct text *text, size_t line, size_t offset);
/* The character boundary of 'line' that is the nearest to the boundary before 'column', not before a combining mark */
size_t text_offset_at(const struct text *text, size_t line, size_t column);

#endif
