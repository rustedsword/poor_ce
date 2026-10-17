/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>

#include "history.h"

void history_init_impl(struct history *history, size_t edit_capacity, struct history_edit (*edits)[edit_capacity],
		       size_t byte_capacity, char (*bytes)[byte_capacity])
{
	assert(byte_capacity <= UINT32_MAX);
	history->edits = *edits;
	history->bytes = *bytes;
	history->edit_capacity = edit_capacity;
	history->byte_capacity = byte_capacity;
	history_clear(history);
}

void history_clear(struct history *history)
{
	history->count = history->current = history->saved = 0;
	history->saving = SIZE_MAX;
	history->pending = false;
}

/* A position that the edits from 'from' on led to can't be reached anymore */
static size_t lost(size_t position, size_t from)
{
	return position != SIZE_MAX && position > from ? SIZE_MAX : position;
}

/* The oldest 'count' edits went */
static size_t shifted(size_t position, size_t count)
{
	return position != SIZE_MAX && position >= count ? position - count : SIZE_MAX;
}

static size_t used(const struct history *history)
{
	const struct history_edit *last;

	if (!history->count)
		return 0;
	last = &history->edits[history->count - 1];
	return last->bytes + last->removed + last->inserted;
}

static void forget(struct history *history)
{
	auto all_bytes = array_ptr(history->bytes, history->byte_capacity);
	auto all_edits = array_ptr(history->edits, history->edit_capacity);
	size_t n = 1, total = used(history), shift;

	while (n < history->count && history->edits[n].joined)
		n++;
	shift = n < history->count ? history->edits[n].bytes : total;
	auto bytes = arrview(shift, total - shift, all_bytes);
	auto edits = arrview(n, history->count - n, all_edits);
	memmove(arrview_first(ARRAY_SIZE(bytes), all_bytes), bytes, ARRAY_SIZE_BYTES(bytes));
	memmove(arrview_first(ARRAY_SIZE(edits), all_edits), edits, ARRAY_SIZE_BYTES(edits));
	history->count -= n;
	history->current -= n;
	history->saved = shifted(history->saved, n);
	history->saving = shifted(history->saving, n);
	foreach_array_ref(arrview_first(history->count, all_edits), edit)
		edit->bytes -= shift;
}

struct history_edit *history_reserve(struct history *history, size_t offset, size_t removed_size, size_t inserted_size,
				     size_t anchor, size_t caret, bool joined)
{
	struct history_edit *edit;
	size_t start;

	joined &= history->current != history->saved && (!history->pending || history->current != history->saving);
	history->saved = lost(history->saved, history->current);
	history->saving = lost(history->saving, history->current);
	history->count = history->current;
	if (removed_size + inserted_size > history->byte_capacity) {
		history->count = history->current = 0;
		history->saved = history->saving = SIZE_MAX;
		return nullptr;
	}
	while (history->count == history->edit_capacity || used(history) + removed_size + inserted_size > history->byte_capacity)
		forget(history);
	start = used(history);
	edit = &history->edits[history->count++];
	*edit = (struct history_edit){
		.offset = offset,
		.removed = removed_size,
		.inserted = inserted_size,
		.bytes = start,
		.anchor = anchor,
		.caret = caret,
		.after_anchor = offset + inserted_size,
		.after_caret = offset + inserted_size,
		.joined = joined && history->count > 1,
	};
	history->current = history->count;
	return edit;
}

void history_record_impl(struct history *history, size_t offset, size_t removed_size, const char (*removed)[removed_size],
			 size_t inserted_size, const char (*inserted)[inserted_size], size_t anchor, size_t caret, bool joined)
{
	struct history_edit *edit = history_reserve(history, offset, removed_size, inserted_size, anchor, caret, joined);

	if (!edit)
		return;
	copy_array(history_removed(history, edit), removed);
	copy_array(history_inserted(history, edit), inserted);
}

void history_select(struct history *history, size_t anchor, size_t caret)
{
	if (!history->current)
		return;
	history->edits[history->current - 1].after_anchor = anchor;
	history->edits[history->current - 1].after_caret = caret;
}

void history_drop(struct history *history)
{
	assert(history->count && history->current == history->count);
	history->current = --history->count;
}

const struct history_edit *history_undo(struct history *history)
{
	return history->current ? &history->edits[--history->current] : nullptr;
}

const struct history_edit *history_redo(struct history *history)
{
	return history->current < history->count ? &history->edits[history->current++] : nullptr;
}

bool history_redo_joined(const struct history *history)
{
	return history->current < history->count && history->edits[history->current].joined;
}

void history_save(struct history *history)
{
	history->saving = history->current;
	history->pending = true;
}

/* A file that wasn't saved may have been cut short */
void history_saved(struct history *history, bool done)
{
	history->saved = done ? history->saving : SIZE_MAX;
	history->pending = false;
}

bool history_modified(const struct history *history)
{
	return history->current != history->saved;
}

bool history_unsaved(const struct history *history)
{
	return history->current != (history->pending ? history->saving : history->saved);
}
