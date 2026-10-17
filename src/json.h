/* SPDX-License-Identifier: MIT */
#ifndef JSON_H
#define JSON_H

#include <poor_array.h>
#include <stddef.h>
#include <stdint.h>

/* A value of a JSON text: its bytes from 'start' to 'end' */
struct json {
	size_t start, end;
};

/* The value that the whole text is */
[[nodiscard]] bool json_parse_impl(size_t size, const char (*text)[size], struct json *value);
#define json_parse(text, value) json_parse_impl(ARRAY_SIZE(text), &auto_arr(text), value)
/* The member 'key' of an object, or the element 'index' of an array */
[[nodiscard]] bool json_member_impl(size_t size, const char (*text)[size], struct json object, const char *key, struct json *value);
#define json_member(text, object, key, value) json_member_impl(ARRAY_SIZE(text), &auto_arr(text), object, key, value)
[[nodiscard]] bool json_element_impl(size_t size, const char (*text)[size], struct json array, size_t index, struct json *value);
#define json_element(text, array, index, value) json_element_impl(ARRAY_SIZE(text), &auto_arr(text), array, index, value)
[[nodiscard]] bool json_integer_impl(size_t size, const char (*text)[size], struct json value, int64_t *integer);
#define json_integer(text, value, integer) json_integer_impl(ARRAY_SIZE(text), &auto_arr(text), value, integer)
/* Unescape a string into 'out' and end it with a 0. Returns its length, or SIZE_MAX if it is no string or doesn't fit. */
size_t json_string_impl(size_t size, const char (*text)[size], struct json value, size_t capacity, char (*out)[capacity]);
#define json_string(text, value, out) json_string_impl(ARRAY_SIZE(text), &auto_arr(text), value, ARRAY_SIZE(out), &auto_arr(out))
bool json_is_impl(size_t size, const char (*text)[size], struct json value, const char *literal);
#define json_is(text, value, literal) json_is_impl(ARRAY_SIZE(text), &auto_arr(text), value, literal)

/* JSON that goes into memory of the caller, as much as fits. 'overflow' tells that some didn't. */
struct json_writer {
	char *bytes;
	size_t size, capacity;
	bool overflow;
};

void json_writer_init_impl(struct json_writer *writer, size_t capacity, char (*bytes)[capacity]);
#define json_writer_init(writer, bytes) json_writer_init_impl(writer, ARRAY_SIZE(bytes), &auto_arr(bytes))
[[gnu::format(printf, 2, 3)]] void json_printf(struct json_writer *writer, const char *fmt, ...);
/* A string of 'bytes': invalid UTF-8 becomes U+FFFD */
void json_quote_impl(struct json_writer *writer, size_t size, const char (*bytes)[size]);
#define json_quote(writer, bytes) json_quote_impl(writer, ARRAY_SIZE(bytes), &auto_arr(bytes))

#endif
