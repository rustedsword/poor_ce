/* SPDX-License-Identifier: MIT */
#ifndef SEARCH_H
#define SEARCH_H

#include "text.h"

constexpr size_t SEARCH_BYTES = 256;

/*
 * ASCII letters match in either case unless the query has an ASCII capital. Matches start and end between characters,
 * and may overlap.
 */
struct search {
	size_t size;
	bool exact;
	char query[SEARCH_BYTES];
};

void search_init(struct search *search);
/* How many bytes of 'bytes' the whole characters of their first line that fit 'room' take */
size_t search_fit_impl(size_t size, const char (*bytes)[size], size_t room);
#define search_fit(bytes, room) search_fit_impl(ARRAY_SIZE(bytes), &auto_arr(bytes), room)
/* Adds the whole characters of the first line of 'bytes' that fit */
void search_add_impl(struct search *search, size_t size, const char (*bytes)[size]);
#define search_add(search, bytes) search_add_impl(search, ARRAY_SIZE(bytes), &auto_arr(bytes))
bool search_at(const struct search *search, const struct text *text, size_t offset);
/* The first or the last match that starts from 'from' to before 'to', SIZE_MAX if there is none */
size_t search_forward(const struct search *search, const struct text *text, size_t from, size_t to);
size_t search_backward(const struct search *search, const struct text *text, size_t from, size_t to);
/* The matches that start from 'from' to before 'to', of which those that start up to 'mark' are added to 'upto' */
size_t search_count(const struct search *search, const struct text *text, size_t from, size_t to, size_t mark, size_t *upto);

#endif
