/* SPDX-License-Identifier: MIT */
#ifndef GO_H
#define GO_H

#include "document.h"

constexpr size_t GO_BYTES = 16;
constexpr size_t GO_EDITS = 64;
constexpr size_t GO_HISTORY = 1 << 10;

/* The memory of the query, which is one line, and of what can be undone of it */
struct go_memory {
	char bytes[GO_BYTES];
	uint32_t lines[1];
	uint8_t states[1];
	struct history_edit edits[GO_EDITS];
	char history[GO_HISTORY];
};

/*
 * The 'query', the number of a line of 'target' in digits that are edited like a document in memory of the caller.
 * The caret of the target goes to the start of that line, or back to where it was without one: the selection from
 * 'anchor' to 'caret' that keeps column 'goal' going up and down, of a view from line 'top' and column 'left'. The
 * target follows the query of 'version'.
 */
struct go {
	struct document query;
	struct document *target;
	size_t anchor, caret, goal, top, left;
	uint64_t version;
};

void go_init(struct go *go, struct go_memory *memory);
/* Go to lines of 'target' from where it is, the query starts empty */
void go_start(struct go *go, struct document *target);
/* If the query changed, the caret goes to its line, the last one past the end, or back without one */
void go_update(struct go *go);
/* The caret and the view go back to where they were */
void go_back(struct go *go);
/* The first line of 'bytes', if it is digits, replaces the selection of the query. Returns whether it changed. */
bool go_insert_impl(struct go *go, size_t size, const char (*bytes)[size]);
#define go_insert(go, bytes) go_insert_impl(go, ARRAY_SIZE(bytes), &auto_arr(bytes))

#endif
