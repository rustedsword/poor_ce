/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_stdio.h>
#include <stdarg.h>
#include <string.h>

#include "json.h"
#include "text.h"

constexpr unsigned JSON_DEPTH = 64;

static size_t space(size_t size, const char (*text)[size], size_t at)
{
	while (at < size && ((*text)[at] == ' ' || (*text)[at] == '\t' || (*text)[at] == '\n' || (*text)[at] == '\r'))
		at++;
	return at;
}

static size_t string_end(size_t size, const char (*text)[size], size_t at)
{
	if (at >= size || (*text)[at] != '"')
		return SIZE_MAX;
	for (at++; at < size; at++) {
		if ((*text)[at] == '\\')
			at++;
		else if ((*text)[at] == '"')
			return at + 1;
	}
	return SIZE_MAX;
}

static bool number_char(char c)
{
	return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static size_t value_end(size_t size, const char (*text)[size], size_t at, unsigned depth);

static size_t container_end(size_t size, const char (*text)[size], size_t at, unsigned depth, char close)
{
	at = space(size, text, at);
	if (at < size && (*text)[at] == close)
		return at + 1;
	for (;;) {
		if (close == '}') {
			at = space(size, text, string_end(size, text, at));
			if (at >= size || (*text)[at] != ':')
				return SIZE_MAX;
			at++;
		}
		at = space(size, text, value_end(size, text, at, depth + 1));
		if (at >= size)
			return SIZE_MAX;
		if ((*text)[at] == close)
			return at + 1;
		if ((*text)[at] != ',')
			return SIZE_MAX;
		at = space(size, text, at + 1);
	}
}

/* Where the value after 'at' ends, or SIZE_MAX if there is none */
static size_t value_end(size_t size, const char (*text)[size], size_t at, unsigned depth)
{
	static const char *const literals[] = { "true", "false", "null" };
	size_t start;

	at = space(size, text, at);
	if (at >= size || depth > JSON_DEPTH)
		return SIZE_MAX;
	switch ((*text)[at]) {
	case '{':
		return container_end(size, text, at + 1, depth, '}');
	case '[':
		return container_end(size, text, at + 1, depth, ']');
	case '"':
		return string_end(size, text, at);
	}
	foreach_array_const_ref(literals, literal)
		if (size - at >= strlen(*literal) && !memcmp(&(*text)[at], *literal, strlen(*literal)))
			return at + strlen(*literal);
	for (start = at; at < size && number_char((*text)[at]); at++)
		;
	return at > start ? at : SIZE_MAX;
}

bool json_parse_impl(size_t size, const char (*text)[size], struct json *value)
{
	size_t start = space(size, text, 0), end = value_end(size, text, start, 0);

	if (end == SIZE_MAX || space(size, text, end) != size)
		return false;
	*value = (struct json){ .start = start, .end = end };
	return true;
}

/* The next value of a parsed object or array after 'at', and where the one after it starts */
static struct json next_value(size_t size, const char (*text)[size], size_t *at)
{
	struct json value;

	value.start = space(size, text, *at);
	value.end = value_end(size, text, value.start, 0);
	*at = space(size, text, value.end);
	if (*at < size && (*text)[*at] == ',')
		*at = space(size, text, *at + 1);
	return value;
}

bool json_member_impl(size_t size, const char (*text)[size], struct json object, const char *key, struct json *value)
{
	size_t at = space(size, text, object.start + 1), length = strlen(key);

	if ((*text)[object.start] != '{')
		return false;
	while (at < object.end && (*text)[at] == '"') {
		size_t name = at + 1, name_end = string_end(size, text, at) - 1;

		at = space(size, text, name_end + 1) + 1;
		*value = next_value(size, text, &at);
		if (name_end - name == length && !memcmp(&(*text)[name], key, length))
			return true;
	}
	return false;
}

bool json_element_impl(size_t size, const char (*text)[size], struct json array, size_t index, struct json *value)
{
	size_t at = space(size, text, array.start + 1);

	if ((*text)[array.start] != '[')
		return false;
	for (size_t i = 0; at < array.end && (*text)[at] != ']'; i++) {
		*value = next_value(size, text, &at);
		if (i == index)
			return true;
	}
	return false;
}

bool json_next_impl(size_t size, const char (*text)[size], struct json array, size_t *at, struct json *value)
{
	if ((*text)[array.start] != '[')
		return false;
	if (!*at)
		*at = space(size, text, array.start + 1);
	if (*at >= array.end || (*text)[*at] == ']')
		return false;
	*value = next_value(size, text, at);
	return value->end != SIZE_MAX;
}

bool json_integer_impl(size_t size, const char (*text)[size], struct json value, int64_t *integer)
{
	bool negative = (*text)[value.start] == '-';
	auto digits = arrview(value.start + negative, value.end - value.start - negative, text);
	uint64_t magnitude = 0;

	if (!ARRAY_SIZE(digits))
		return false;
	foreach_array_const_ref(digits, c) {
		if (*c < '0' || *c > '9' || magnitude > INT64_MAX / 10)
			return false;
		magnitude = magnitude * 10 + (*c - '0');
	}
	if (magnitude > INT64_MAX)
		return false;
	*integer = negative ? -(int64_t)magnitude : (int64_t)magnitude;
	return true;
}

static uint32_t hex(size_t size, const char (*text)[size], size_t at)
{
	uint32_t value = 0;

	foreach_array_const_ref(arrview(at, at + 4 <= size ? 4 : size - at, text), c) {
		value <<= 4;
		if (*c >= '0' && *c <= '9')
			value |= *c - '0';
		else if ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'f')
			value |= (*c | 0x20) - 'a' + 10;
		else
			return TEXT_REPLACEMENT;
	}
	return value;
}

/* UTF-8 of 'codepoint' at 'at' of 'out', which keeps a byte for a terminating 0. Returns its length, or 0 if it doesn't fit. */
static size_t encode(size_t capacity, char (*out)[capacity], size_t at, uint32_t codepoint)
{
	static const unsigned char leads[] = { 0, 0, 0xc0, 0xe0, 0xf0 };
	size_t length = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
	auto bytes = (unsigned char (*)[capacity])out;

	if (capacity - at <= length)
		return 0;
	foreach_array_ref_bw(arrview(at + 1, length - 1, bytes), byte) {
		*byte = 0x80 | (codepoint & 0x3f);
		codepoint >>= 6;
	}
	(*bytes)[at] = leads[length] | codepoint;
	return length;
}

/* Escaped surrogate pairs make one code point, lone surrogates are U+FFFD */
size_t json_string_impl(size_t size, const char (*text)[size], struct json value, size_t capacity, char (*out)[capacity])
{
	size_t at = value.start + 1, length = 0, n;
	uint32_t codepoint;

	if ((*text)[value.start] != '"')
		return SIZE_MAX;
	while (at < value.end - 1) {
		char c = (*text)[at++];

		if (c != '\\') {
			if (capacity - length < 2)
				return SIZE_MAX;
			(*out)[length++] = c;
			continue;
		}
		switch (c = (*text)[at++]) {
		case 'b':
			codepoint = '\b';
			break;
		case 'f':
			codepoint = '\f';
			break;
		case 'n':
			codepoint = '\n';
			break;
		case 'r':
			codepoint = '\r';
			break;
		case 't':
			codepoint = '\t';
			break;
		case 'u':
			codepoint = hex(size, text, at);
			at += 4;
			if (codepoint >= 0xd800 && codepoint < 0xdc00 && at + 6 <= value.end && (*text)[at] == '\\' &&
			    (*text)[at + 1] == 'u' && hex(size, text, at + 2) >= 0xdc00 && hex(size, text, at + 2) < 0xe000) {
				codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (hex(size, text, at + 2) - 0xdc00);
				at += 6;
			} else if (codepoint >= 0xd800 && codepoint < 0xe000) {
				codepoint = TEXT_REPLACEMENT;
			}
			break;
		default:
			codepoint = (unsigned char)c;
		}
		if (!(n = encode(capacity, out, length, codepoint)))
			return SIZE_MAX;
		length += n;
	}
	(*out)[length] = 0;
	return length;
}

bool json_is_impl(size_t size, const char (*text)[size], struct json value, const char *literal)
{
	return value.end - value.start == strlen(literal) && !memcmp(&(*text)[value.start], literal, strlen(literal));
}

void json_writer_init_impl(struct json_writer *writer, size_t capacity, char (*bytes)[capacity])
{
	writer->bytes = *bytes;
	writer->size = 0;
	writer->capacity = capacity;
	writer->overflow = false;
}

void json_printf(struct json_writer *writer, const char *fmt, ...)
{
	auto room = arrview_cfront(writer->size, array_ptr(writer->bytes, writer->capacity));
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf_array(room, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= ARRAY_SIZE(room))
		writer->overflow = true;
	else
		writer->size += n;
}

static void put(struct json_writer *writer, size_t size, const char (*bytes)[size])
{
	if (writer->capacity - writer->size < size) {
		writer->overflow = true;
		return;
	}
	copy_array(arrview(writer->size, size, array_ptr(writer->bytes, writer->capacity)), bytes);
	writer->size += size;
}

static bool plain(unsigned char c)
{
	return c >= 0x20 && c < 0x80 && c != '"' && c != '\\';
}

/* The subtractions borrow from bytes below 0x20, quotes and backslashes, which may flag bytes after them too */
static bool plain_word(const char (*bytes)[8])
{
	constexpr uint64_t ones = 0x0101010101010101;
	uint64_t word, borrows;

	memcpy(&word, *bytes, sizeof(word));
	borrows = (word - 0x20 * ones) | ((word ^ '"' * ones) - ones) | ((word ^ '\\' * ones) - ones);
	return !((borrows | word) & 0x80 * ones);
}

void json_quote_impl(struct json_writer *writer, size_t size, const char (*bytes)[size])
{
	static const char digits[] = "0123456789abcdef";
	size_t at = 0, run = 0, length;

	put(writer, 1, &"\"");
	while (at < size) {
		while (size - at >= 8 && plain_word(arrview(at, 8, bytes)))
			at += 8;
		while (at < size && plain((*bytes)[at]))
			at++;
		if (at == size)
			break;
		unsigned char c = (*bytes)[at];

		if (c >= 0x80 && (utf8_decode(arrview_cfront(at, bytes), &length) != TEXT_REPLACEMENT || length > 1)) {
			at += length;
			continue;
		}
		char escape[] = { '\\', 'u', '0', '0', digits[c >> 4], digits[c & 15] };
		size_t n = sizeof(escape);

		if (c >= 0x80) {
			copy_array(arrview(2, 4, escape), arrview_str("fffd"));
		} else if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
			escape[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : c;
			n = 2;
		}
		put(writer, at - run, arrview(run, at - run, bytes));
		put(writer, n, arrview_first(n, escape));
		run = ++at;
	}
	put(writer, at - run, arrview(run, at - run, bytes));
	put(writer, 1, &"\"");
}
