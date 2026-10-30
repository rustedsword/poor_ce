/* SPDX-License-Identifier: MIT */
#ifndef FIND_H
#define FIND_H

#include "document.h"
#include "search.h"

constexpr size_t FIND_EDITS = 256;
constexpr size_t FIND_HISTORY = 16 << 10;

/* The memory of the query, which is one line, and of what can be undone of it */
struct find_memory {
	char bytes[SEARCH_BYTES];
	uint32_t lines[1];
	uint8_t states[1];
	struct history_edit edits[FIND_EDITS];
	char history[FIND_HISTORY];
};

/*
 * The 'query', a line that is edited like a document in memory of the caller, and its matches in 'target', where the
 * one that is found is selected. The search has the query of 'version', and finds from 'origin' on. Matches are
 * counted up to 'counted', 'index' of them up to 'match', which is SIZE_MAX if there is none.
 */
struct find {
	struct document query;
	struct search search;
	struct document *target;
	size_t origin, match, counted, count, index;
	uint64_t version;
};

void find_init(struct find *find, struct find_memory *memory);
/* Find from the selection of 'target' on, a selection within a line becomes the query. All of the query is selected. */
void find_start(struct find *find, struct document *target);
/* Find the query from the origin on again, if it changed. Returns whether it did. */
bool find_update(struct find *find);
/* Select the next or the previous match, around the ends. Returns whether there is one. */
bool find_step(struct find *find, bool back);
/* Select the next or the previous match of the search in 'document'. Returns where it starts, SIZE_MAX if there is none. */
size_t find_again(const struct find *find, struct document *document, bool back);
/* The whole characters of the first line of 'bytes' that fit replace the selection of the query. Returns whether it changed. */
bool find_insert_impl(struct find *find, size_t size, const char (*bytes)[size]);
#define find_insert(find, bytes) find_insert_impl(find, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* Count the matches in 'budget' more bytes of the target. Returns whether all of them are counted. */
bool find_count(struct find *find, size_t budget);
bool find_counted(const struct find *find);

#endif
