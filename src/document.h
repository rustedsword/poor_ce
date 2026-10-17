/* SPDX-License-Identifier: MIT */
#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "history.h"
#include "text.h"
#include "view.h"

enum document_motion {
	DOCUMENT_LEFT,
	DOCUMENT_RIGHT,
	DOCUMENT_WORD_LEFT,
	DOCUMENT_WORD_RIGHT,
	DOCUMENT_UP,
	DOCUMENT_DOWN,
	DOCUMENT_PAGE_UP,
	DOCUMENT_PAGE_DOWN,
	DOCUMENT_HOME,
	DOCUMENT_END,
	DOCUMENT_TOP,
	DOCUMENT_BOTTOM,
};

/* Typed characters join into one step of the history, and so do erased ones */
enum document_streak : uint8_t {
	DOCUMENT_NONE,
	DOCUMENT_TYPING,
	DOCUMENT_ERASING,
};

/*
 * A text that is edited: the view holds the selection and what shows of the text, the history what can be undone.
 * 'goal' is the column that the caret keeps while it goes up and down, 'version' changes with every edit and
 * 'indent' is the number of spaces of a level of indentation, 0 for a tab. Only code is highlighted.
 */
struct document {
	struct text text;
	struct view view;
	struct history history;
	size_t goal, indent;
	uint64_t version;
	enum document_streak streak;
	bool crlf, code, read_only;
};

/* An empty document in memory of the caller: its bytes and lines, and what can be undone */
void document_init_impl(struct document *document, size_t capacity, char (*bytes)[capacity], size_t line_capacity,
			uint32_t (*lines)[line_capacity], size_t state_capacity, uint8_t (*states)[state_capacity],
			size_t edit_capacity, struct history_edit (*edits)[edit_capacity], size_t history_capacity,
			char (*history)[history_capacity]);
#define document_init(document, bytes, lines, states, edits, history) \
	document_init_impl(document, ARRAY_SIZE(bytes), &auto_arr(bytes), ARRAY_SIZE(lines), &auto_arr(lines), \
			   ARRAY_SIZE(states), &auto_arr(states), ARRAY_SIZE(edits), &auto_arr(edits), ARRAY_SIZE(history), \
			   &auto_arr(history))
/* The text got 'size' bytes. If not all of its lines fit, the rest is cut off and the document is read only. */
[[nodiscard]] bool document_load(struct document *document, size_t size);

bool document_move(struct document *document, enum document_motion motion, bool extend);
/* The caret goes to the character boundary 'offset', and so does the anchor unless the selection extends */
bool document_place(struct document *document, size_t offset, bool extend);
bool document_select_word(struct document *document, size_t offset);
bool document_select_line(struct document *document, size_t offset);
bool document_select_all(struct document *document);

/* Replace the selection with 'bytes'. Typed ones join the typing before them, a typed } outdents a blank line. */
bool document_insert_impl(struct document *document, size_t size, const char (*bytes)[size], bool typed);
#define document_insert(document, bytes, typed) document_insert_impl(document, ARRAY_SIZE(bytes), &auto_arr(bytes), typed)
/* A line break with the indentation of the line, one level deeper after a { */
bool document_newline(struct document *document);
/* Erase the selection, or the character or the word before or after the caret */
bool document_erase(struct document *document, bool forward, bool word);
/* Indent or outdent the lines of a selection with a line break, a level of indentation goes in otherwise */
bool document_indent(struct document *document, bool outdent);
bool document_undo(struct document *document);
bool document_redo(struct document *document);

#endif
