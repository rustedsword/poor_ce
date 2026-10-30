/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <string.h>

#include "search.h"

/* Bytes of code and prose, from the most common on. The other ASCII bytes are rarer. */
constexpr char COMMON[] = " e\tt_aoinsrlcdhu(),;.mpf=-gy>b*/wvk\"x0'1:2{}[]<#&q+!j|z\\";

static bool capital(char c)
{
	return c >= 'A' && c <= 'Z';
}

static char fold(char c)
{
	return capital(c) ? c - 'A' + 'a' : c;
}

/* A byte that starts a character of another script is as common as the script, one that follows it less so */
static size_t rarity(char c)
{
	const char *common;

	if ((unsigned char)c >= 0xc0)
		return 0;
	if ((unsigned char)c >= 0x80)
		return ARRAY_SIZE(COMMON) / 2;
	common = memchr(COMMON, c, ARRAY_SIZE(COMMON) - 1);
	return common ? (size_t)(common - COMMON) : ARRAY_SIZE(COMMON);
}

static void update(struct search *search)
{
	auto query = arrview_first(search->size, search->query);

	search->exact = false;
	search->anchor = 0;
	foreach_array_index(query, i) {
		search->exact |= capital((*query)[i]);
		if (rarity((*query)[i]) > rarity((*query)[search->anchor]))
			search->anchor = i;
	}
}

void search_init(struct search *search)
{
	search->size = search->anchor = 0;
	search->exact = false;
}

size_t search_fit_impl(size_t size, const char (*bytes)[size], size_t room)
{
	size_t at = 0, length;

	while (at < size && (*bytes)[at] != '\n' && (*bytes)[at] != '\r') {
		utf8_decode(arrview_cfront(at, bytes), &length);
		if (length > room - at)
			break;
		at += length;
	}
	return at;
}

void search_add_impl(struct search *search, size_t size, const char (*bytes)[size])
{
	size_t fit = search_fit_impl(size, bytes, ARRAY_SIZE(search->query) - search->size);

	copy_array(arrview(search->size, fit, search->query), arrview_first(fit, bytes));
	search->size += fit;
	update(search);
}

static bool folded(size_t size, const char (*bytes)[size], const char (*query)[size])
{
	foreach_array_index(query, i)
		if (fold((*bytes)[i]) != (*query)[i])
			return false;
	return true;
}

bool search_at(const struct search *search, const struct text *text, size_t offset)
{
	auto query = arrview_first(search->size, search->query);

	assert(offset <= text->size);
	if (!search->size || text->size - offset < search->size)
		return false;
	auto bytes = arrview(offset, search->size, array_ptr(text->bytes, text->size));
	return (search->exact ? !memcmp(*bytes, *query, search->size) : folded(search->size, bytes, query)) &&
	       text_is_boundary(text, offset) && text_is_boundary(text, offset + search->size);
}

static char other_case(const struct search *search)
{
	char c = search->query[search->anchor];

	return !search->exact && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* The anchors of the matches that start from 'from' to before 'to' are from 'begin' to before 'end' */
static void anchors(const struct search *search, const struct text *text, size_t from, size_t to, size_t *begin,
		    size_t *end)
{
	*end = text->size - to > search->anchor ? to + search->anchor : text->size;
	*begin = from + search->anchor < *end ? from + search->anchor : *end;
}

/* 'to' if 'c' isn't there */
static size_t first(const struct text *text, char c, size_t from, size_t to)
{
	auto bytes = arrview(from, to - from, array_ptr(text->bytes, text->size));
	const char *at = memchr(*bytes, c, ARRAY_SIZE(bytes));

	return at ? (size_t)(at - text->bytes) : to;
}

/* SIZE_MAX if 'c' isn't there */
static size_t last(const struct text *text, char c, size_t from, size_t to)
{
	auto bytes = arrview(from, to - from, array_ptr(text->bytes, text->size));
	const char *at = memrchr(*bytes, c, ARRAY_SIZE(bytes));

	return at ? (size_t)(at - text->bytes) : SIZE_MAX;
}

/* Both cases of the anchor, each looked for again only after where it was found */
struct starts {
	size_t lower, upper, end, anchor, to;
	char c, other;
};

static void starts_init(struct starts *starts, const struct search *search, const struct text *text, size_t from, size_t to)
{
	size_t begin;

	anchors(search, text, from, to, &begin, &starts->end);
	starts->anchor = search->anchor;
	starts->c = search->query[search->anchor];
	starts->other = other_case(search);
	starts->to = to;
	starts->lower = first(text, starts->c, begin, starts->end);
	starts->upper = starts->other == starts->c ? starts->end : first(text, starts->other, begin, starts->end);
}

/* Where the next match may start, 'to' once there are no more */
static size_t starts_next(struct starts *starts, const struct text *text)
{
	size_t at = starts->lower < starts->upper ? starts->lower : starts->upper;

	if (at == starts->end)
		return starts->to;
	if (at == starts->lower)
		starts->lower = first(text, starts->c, at + 1, starts->end);
	else
		starts->upper = first(text, starts->other, at + 1, starts->end);
	return at - starts->anchor;
}

size_t search_forward(const struct search *search, const struct text *text, size_t from, size_t to)
{
	struct starts starts;
	size_t at;

	if (!search->size || from >= to)
		return SIZE_MAX;
	starts_init(&starts, search, text, from, to);
	while ((at = starts_next(&starts, text)) < to)
		if (search_at(search, text, at))
			return at;
	return SIZE_MAX;
}

size_t search_count(const struct search *search, const struct text *text, size_t from, size_t to, size_t mark, size_t *upto)
{
	struct starts starts;
	size_t at, count = 0;

	if (!search->size || from >= to)
		return 0;
	starts_init(&starts, search, text, from, to);
	while ((at = starts_next(&starts, text)) < to) {
		if (!search_at(search, text, at))
			continue;
		count++;
		*upto += at <= mark;
	}
	return count;
}

size_t search_backward(const struct search *search, const struct text *text, size_t from, size_t to)
{
	size_t lower, upper, begin, end;
	char c, other;

	if (!search->size || from >= to)
		return SIZE_MAX;
	anchors(search, text, from, to, &begin, &end);
	c = search->query[search->anchor];
	other = other_case(search);
	lower = last(text, c, begin, end);
	upper = other == c ? SIZE_MAX : last(text, other, begin, end);
	while (lower != SIZE_MAX || upper != SIZE_MAX) {
		size_t at = upper == SIZE_MAX || (lower != SIZE_MAX && lower > upper) ? lower : upper;

		if (search_at(search, text, at - search->anchor))
			return at - search->anchor;
		if (at == lower)
			lower = last(text, c, begin, at);
		else
			upper = last(text, other, begin, at);
	}
	return SIZE_MAX;
}
