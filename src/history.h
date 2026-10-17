/* SPDX-License-Identifier: MIT */
#ifndef HISTORY_H
#define HISTORY_H

#include <poor_array.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t HISTORY_EDITS = 1 << 14;
constexpr size_t HISTORY_BYTES = 4 << 20;

/*
 * 'removed' bytes at 'offset' became 'inserted' ones, which the history keeps one after the other at 'bytes'. The
 * selection was from 'anchor' to 'caret' before, and from 'after_anchor' to 'after_caret' after. A joined edit is
 * undone and redone together with the one before it.
 */
struct history_edit {
	uint32_t offset, removed, inserted, bytes;
	uint32_t anchor, caret, after_anchor, after_caret;
	bool joined;
};

/*
 * Edits of a document in memory of the caller, the oldest ones go when there is no room for new ones. Those before
 * 'current' are done, the others were undone. The document is as it was saved while 'current' is 'saved', and as it
 * is being saved, if a save is 'pending', while it is 'saving'. They are SIZE_MAX when no edit leads there.
 */
struct history {
	struct history_edit *edits;
	char *bytes;
	size_t edit_capacity, byte_capacity;
	size_t count, current, saved, saving;
	bool pending;
};

void history_init_impl(struct history *history, size_t edit_capacity, struct history_edit (*edits)[edit_capacity],
		       size_t byte_capacity, char (*bytes)[byte_capacity]);
#define history_init(history, edits, bytes) \
	history_init_impl(history, ARRAY_SIZE(edits), &auto_arr(edits), ARRAY_SIZE(bytes), &auto_arr(bytes))
/* Nothing is left to undo or redo */
void history_clear(struct history *history);
/*
 * Drops the edits that were undone. An edit doesn't join those before a save, and one that is too big for the history
 * leaves nothing to undo.
 */
void history_record_impl(struct history *history, size_t offset, size_t removed_size, const char (*removed)[removed_size],
			 size_t inserted_size, const char (*inserted)[inserted_size], size_t anchor, size_t caret, bool joined);
#define history_record(history, offset, removed, inserted, anchor, caret, joined) \
	history_record_impl(history, offset, ARRAY_SIZE(removed), &auto_arr(removed), ARRAY_SIZE(inserted), &auto_arr(inserted), \
			    anchor, caret, joined)
/* Room for an edit whose bytes the caller copies into its views, nullptr if it is too big and nothing is left to undo */
struct history_edit *history_reserve(struct history *history, size_t offset, size_t removed_size, size_t inserted_size,
				     size_t anchor, size_t caret, bool joined);
/* The selection after the last edit, if it isn't a caret after what the edit inserted */
void history_select(struct history *history, size_t anchor, size_t caret);
/* Forget the last edit, which didn't happen */
void history_drop(struct history *history);
/* The edit to undo, or to redo, which is done or undone then. A step of joined edits takes several calls. */
const struct history_edit *history_undo(struct history *history);
const struct history_edit *history_redo(struct history *history);
/* Whether the edit that would be redone next is joined to the one before it */
bool history_redo_joined(const struct history *history);
/* Views of the bytes that an edit removed and inserted */
#define history_removed(history, edit) \
	arrview((edit)->bytes, (edit)->removed, array_ptr((history)->bytes, (history)->byte_capacity))
#define history_inserted(history, edit) \
	arrview((edit)->bytes + (edit)->removed, (edit)->inserted, array_ptr((history)->bytes, (history)->byte_capacity))
/* A copy of the document is being saved, and was saved or not */
void history_save(struct history *history);
void history_saved(struct history *history, bool done);
/* Whether the document differs from its file, or from what is being saved over it */
bool history_modified(const struct history *history);
bool history_unsaved(const struct history *history);

#endif
