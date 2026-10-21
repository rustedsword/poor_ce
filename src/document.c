/* SPDX-License-Identifier: MIT */
#include <assert.h>

#include "document.h"
#include "syntax.h"

constexpr size_t INDENT_LINES = 1000;
constexpr size_t NEWLINE_BYTES = 1024;

#define apply(document, start, end, bytes) apply_impl(document, start, end, ARRAY_SIZE(bytes), &auto_arr(bytes))
#define edit(document, start, end, bytes, joined) edit_impl(document, start, end, ARRAY_SIZE(bytes), &auto_arr(bytes), joined)

static const char nothing[1];
static const char units[] = "\t        ";

#define level_of(document) arrview((document)->indent ? 1 : 0, (document)->indent ? (document)->indent : 1, units)

static bool blank(char c)
{
	return c == ' ' || c == '\t';
}

/* Blanks, word characters and the rest */
static int kind(char c)
{
	unsigned char u = c;

	if (blank(c))
		return 0;
	return u == '_' || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u >= 0x80 ? 2 : 1;
}

/* A level of indentation is a tab, or the spaces of the first line that starts with a few and no comment */
static size_t indentation(const struct text *text)
{
	auto all = array_ptr(text->bytes, text->size);

	foreach_array_index(arrview_first(text->line_count < INDENT_LINES ? text->line_count : INDENT_LINES,
					  array_ptr(text->lines, text->line_count)), line) {
		size_t start = text_line_start(text, line), end = text_line_end(text, line), spaces = 0;

		if (start < end && (*all)[start] == '\t')
			return 0;
		while (start + spaces < end && (*all)[start + spaces] == ' ')
			spaces++;
		if (spaces >= 2 && spaces <= TEXT_TAB && start + spaces < end && (*all)[start + spaces] != '*')
			return spaces;
	}
	return 0;
}

void document_init_impl(struct document *document, size_t capacity, char (*bytes)[capacity], size_t line_capacity,
			uint32_t (*lines)[line_capacity], size_t state_capacity, uint8_t (*states)[state_capacity],
			size_t edit_capacity, struct history_edit (*edits)[edit_capacity], size_t history_capacity,
			char (*history)[history_capacity])
{
	text_init_impl(&document->text, capacity, bytes, line_capacity, lines, state_capacity, states);
	view_init(&document->view, &document->text);
	history_init_impl(&document->history, edit_capacity, edits, history_capacity, history);
	document->goal = SIZE_MAX;
	document->indent = 0;
	document->version = 0;
	document->streak = DOCUMENT_NONE;
	document->crlf = document->code = document->read_only = false;
}

void document_begin(struct document *document, size_t size)
{
	text_begin(&document->text, size);
	history_clear(&document->history);
	view_select(&document->view, 0, 0);
	document->view.top = document->view.left = 0;
	document->goal = SIZE_MAX;
	document->version++;
	document->streak = DOCUMENT_NONE;
}

bool document_index(struct document *document, size_t budget)
{
	struct text *text = &document->text;
	size_t first = text->line_count - 1;
	bool fits, done = text_index(text, budget, &fits);

	if (document->code)
		syntax_update(text, first, text->line_count - 1);
	if (!done)
		return false;
	document->read_only = !fits;
	document->crlf = text->line_count > 1 && text_line_end(text, 0) + 2 == text_line_start(text, 1);
	document->indent = indentation(text);
	return true;
}

static bool place(struct document *document, size_t caret, bool extend)
{
	struct view *view = &document->view;
	bool changed = view_select(view, extend ? view->anchor : caret, caret);

	document->streak = DOCUMENT_NONE;
	return view_reveal(view, caret) || changed;
}

static size_t word_right(const struct text *text, size_t offset)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t end = text_line_end(text, text_line_of(text, offset));
	int run;

	if (offset == end)
		return text_next(text, offset);
	while (offset < end && blank((*all)[offset]))
		offset++;
	run = offset < end ? kind((*all)[offset]) : 0;
	while (offset < end && kind((*all)[offset]) == run)
		offset++;
	return offset;
}

static size_t word_left(const struct text *text, size_t offset)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t start = text_line_start(text, text_line_of(text, offset));
	int run;

	if (offset == start)
		return text_prev(text, offset);
	while (offset > start && blank((*all)[offset - 1]))
		offset--;
	run = offset > start ? kind((*all)[offset - 1]) : 0;
	while (offset > start && kind((*all)[offset - 1]) == run)
		offset--;
	return offset;
}

/* The first line starts the document and the last one ends it */
static size_t vertical(struct document *document, size_t caret, ptrdiff_t lines)
{
	const struct text *text = &document->text;
	size_t line = text_line_of(text, caret), distance = lines < 0 ? -(size_t)lines : (size_t)lines;

	if (document->goal == SIZE_MAX)
		document->goal = text_column_of(text, line, caret);
	if (lines < 0 && distance > line)
		return 0;
	if (lines > 0 && distance >= text->line_count - line)
		return text->size;
	return text_offset_at(text, lines < 0 ? line - distance : line + distance, document->goal);
}

/* Home goes to the indentation first, and from there to the start of the line */
static size_t home(const struct text *text, size_t caret)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t line = text_line_of(text, caret), start = text_line_start(text, line), end = text_line_end(text, line);
	size_t indent = start;

	while (indent < end && blank((*all)[indent]))
		indent++;
	return caret == indent ? start : indent;
}

/* Without extending, left and right go to the edges of a selection */
bool document_move(struct document *document, enum document_motion motion, bool extend)
{
	struct view *view = &document->view;
	const struct text *text = &document->text;
	size_t caret = view->caret, start, end;
	bool selected = view_selection(view, &start, &end), scrolled = false;
	ptrdiff_t page = view_page(view);

	if (motion != DOCUMENT_UP && motion != DOCUMENT_DOWN && motion != DOCUMENT_PAGE_UP && motion != DOCUMENT_PAGE_DOWN)
		document->goal = SIZE_MAX;
	switch (motion) {
	case DOCUMENT_LEFT:
		caret = selected && !extend ? start : text_prev(text, caret);
		break;
	case DOCUMENT_RIGHT:
		caret = selected && !extend ? end : text_next(text, caret);
		break;
	case DOCUMENT_WORD_LEFT:
		caret = word_left(text, caret);
		break;
	case DOCUMENT_WORD_RIGHT:
		caret = word_right(text, caret);
		break;
	case DOCUMENT_UP:
		caret = vertical(document, caret, -1);
		break;
	case DOCUMENT_DOWN:
		caret = vertical(document, caret, 1);
		break;
	case DOCUMENT_PAGE_UP:
		scrolled = view_scroll(view, -page, 0);
		caret = vertical(document, caret, -page);
		break;
	case DOCUMENT_PAGE_DOWN:
		scrolled = view_scroll(view, page, 0);
		caret = vertical(document, caret, page);
		break;
	case DOCUMENT_HOME:
		caret = home(text, caret);
		break;
	case DOCUMENT_END:
		caret = text_line_end(text, text_line_of(text, caret));
		break;
	case DOCUMENT_TOP:
		caret = 0;
		break;
	case DOCUMENT_BOTTOM:
		caret = text->size;
		break;
	}
	return place(document, caret, extend) || scrolled;
}

bool document_place(struct document *document, size_t offset, bool extend)
{
	document->goal = SIZE_MAX;
	return place(document, offset, extend);
}

static bool select_range(struct document *document, size_t anchor, size_t caret)
{
	struct view *view = &document->view;
	bool changed = view_select(view, anchor, caret);

	document->goal = SIZE_MAX;
	document->streak = DOCUMENT_NONE;
	return view_reveal(view, caret) || changed;
}

bool document_select_word(struct document *document, size_t offset)
{
	const struct text *text = &document->text;
	auto all = array_ptr(text->bytes, text->size);
	size_t line = text_line_of(text, offset), first = text_line_start(text, line), last = text_line_end(text, line);
	size_t start = offset, end = offset;
	int run;

	if (first == last)
		return select_range(document, offset, offset);
	run = kind((*all)[offset < last ? offset : offset - 1]);
	while (start > first && kind((*all)[start - 1]) == run)
		start--;
	while (end < last && kind((*all)[end]) == run)
		end++;
	return select_range(document, start, end);
}

bool document_select_line(struct document *document, size_t offset)
{
	const struct text *text = &document->text;
	size_t line = text_line_of(text, offset);

	return select_range(document, text_line_start(text, line),
			    line + 1 < text->line_count ? text_line_start(text, line + 1) : text->size);
}

bool document_select_all(struct document *document)
{
	return select_range(document, 0, document->text.size);
}

static void apply_impl(struct document *document, size_t start, size_t end, size_t size, const char (*bytes)[size])
{
	struct text *text = &document->text;
	size_t first = text_line_of(text, start);
	[[maybe_unused]] bool replaced = text_replace(text, start, end, bytes);

	assert(replaced);
	if (document->code)
		syntax_update(text, first, text_line_of(text, start + size));
	document->version++;
}

static bool edit_impl(struct document *document, size_t start, size_t end, size_t size, const char (*bytes)[size], bool joined)
{
	struct view *view = &document->view;

	if (document->read_only || !text_fits(&document->text, start, end, bytes))
		return false;
	history_record(&document->history, start, arrview(start, end - start, array_ptr(document->text.bytes, document->text.size)),
		       bytes, view->anchor, view->caret, joined);
	apply_impl(document, start, end, size, bytes);
	document->goal = SIZE_MAX;
	view_select(view, start + size, start + size);
	view_reveal(view, view->caret);
	return true;
}

/* The blanks before 'offset' that the line starts with, if nothing else comes before it */
static size_t blank_prefix(const struct text *text, size_t offset)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t start = text_line_start(text, text_line_of(text, offset)), at = offset;

	while (at > start && blank((*all)[at - 1]))
		at--;
	return at == start ? offset - start : 0;
}

static size_t level_before(const struct document *document, size_t offset)
{
	auto all = array_ptr(document->text.bytes, document->text.size);
	size_t width = document->indent ? document->indent : TEXT_TAB, count = 0;

	if (offset && (*all)[offset - 1] == '\t')
		return 1;
	while (count < width && count < offset && (*all)[offset - 1 - count] == ' ')
		count++;
	return count;
}

bool document_insert_impl(struct document *document, size_t size, const char (*bytes)[size], bool typed)
{
	const struct text *text = &document->text;
	auto all = array_ptr(text->bytes, text->size);
	size_t start, end;
	bool selected = view_selection(&document->view, &start, &end);
	bool joined = typed && !selected && document->streak == DOCUMENT_TYPING &&
		      !(size && blank((*bytes)[0]) && start && !blank((*all)[start - 1]));

	if (typed && !selected && size == 1 && (*bytes)[0] == '}' && blank_prefix(text, start)) {
		start -= level_before(document, start);
		joined = false;
	}
	if (!edit_impl(document, start, end, size, bytes, joined))
		return false;
	document->streak = typed ? DOCUMENT_TYPING : DOCUMENT_NONE;
	return true;
}

/* Between { and } the caret goes on a line of its own, between them */
bool document_newline(struct document *document)
{
	const struct text *text = &document->text;
	auto all = array_ptr(text->bytes, text->size);
	size_t start, end, first, indent, before, size = 0, caret;
	char bytes[NEWLINE_BYTES];
	bool open, close;

	view_selection(&document->view, &start, &end);
	first = text_line_start(text, text_line_of(text, start));
	indent = first;
	while (indent < start && blank((*all)[indent]))
		indent++;
	before = start;
	while (before > first && blank((*all)[before - 1]))
		before--;
	open = before > first && (*all)[before - 1] == '{';
	close = open && end < text_line_end(text, text_line_of(text, end)) && (*all)[end] == '}';
	auto line_break = arrview_cfront(document->crlf ? 0 : 1, arrview_str("\r\n"));
	auto spaces = arrview(first, indent - first < NEWLINE_BYTES / 4 ? indent - first : NEWLINE_BYTES / 4, all);
	auto level = level_of(document);

	copy_array(arrview(size, ARRAY_SIZE(line_break), bytes), line_break);
	size += ARRAY_SIZE(line_break);
	copy_array(arrview(size, ARRAY_SIZE(spaces), bytes), spaces);
	size += ARRAY_SIZE(spaces);
	if (open) {
		copy_array(arrview(size, ARRAY_SIZE(level), bytes), level);
		size += ARRAY_SIZE(level);
	}
	caret = size;
	if (close) {
		copy_array(arrview(size, ARRAY_SIZE(line_break), bytes), line_break);
		size += ARRAY_SIZE(line_break);
		copy_array(arrview(size, ARRAY_SIZE(spaces), bytes), spaces);
		size += ARRAY_SIZE(spaces);
	}
	if (!edit(document, start, end, arrview_first(size, bytes), false))
		return false;
	view_select(&document->view, start + caret, start + caret);
	history_select(&document->history, start + caret, start + caret);
	view_reveal(&document->view, start + caret);
	document->streak = DOCUMENT_NONE;
	return true;
}

bool document_erase(struct document *document, bool forward, bool word)
{
	const struct text *text = &document->text;
	size_t start, end;
	bool selected = view_selection(&document->view, &start, &end);

	if (!selected && forward)
		end = word ? word_right(text, start) : text_next(text, start);
	else if (!selected)
		start = word ? word_left(text, end) : text_prev(text, end);
	if (start == end || !edit_impl(document, start, end, 0, &nothing, !selected && !word && document->streak == DOCUMENT_ERASING))
		return false;
	document->streak = selected || word ? DOCUMENT_NONE : DOCUMENT_ERASING;
	return true;
}

static size_t leading_level(const struct document *document, size_t start, size_t end)
{
	auto all = array_ptr(document->text.bytes, document->text.size);
	size_t count = 0, width = document->indent ? document->indent : TEXT_TAB;

	if (start < end && (*all)[start] == '\t')
		return 1;
	while (count < width && start + count < end && (*all)[start + count] == ' ')
		count++;
	return count;
}

static size_t cut_level(const void *document, size_t start, size_t end)
{
	return leading_level(document, start, end);
}

/*
 * A selection that ends at the start of a line leaves that line alone. The lines stay selected, but outdenting one
 * line keeps the selection where it was in the line. The lines change at once, in one step of the history.
 */
bool document_indent(struct document *document, bool outdent)
{
	struct text *text = &document->text;
	struct view *view = &document->view;
	auto all = array_ptr(text->bytes, text->capacity);
	auto level = level_of(document);
	size_t start, end, first, last, anchor = view->anchor, caret = view->caret, removed = 0, block, old, size;
	struct history_edit *edit;
	bool whole;

	view_selection(view, &start, &end);
	first = text_line_of(text, start);
	last = text_line_of(text, end);
	if (first == last && !outdent && !document->indent)
		return document_insert(document, level, false);
	if (first == last && !outdent)
		return document_insert(document, arrview(1, document->indent - text_column_of(text, first, start) % document->indent, units),
				       false);
	if ((whole = last > first && end == text_line_start(text, last)))
		last--;
	block = text_line_start(text, first);
	size = old = text_line_end(text, last) - block;
	for (size_t line = first; line <= last; line++) {
		size_t line_start = text_line_start(text, line), line_end = text_line_end(text, line);

		if (outdent)
			removed += leading_level(document, line_start, line_end);
		else if (line_start < line_end)
			size += ARRAY_SIZE(level);
	}
	size -= removed;
	if (document->read_only || size == old || text->size - old + size > text->capacity)
		return false;
	if ((edit = history_reserve(&document->history, block, old, size, anchor, caret, false)))
		copy_array(history_removed(&document->history, edit), arrview(block, old, all));
	if (outdent) {
		text_unprefix(text, first, last, cut_level, document);
	} else {
		[[maybe_unused]] bool prefixed = text_prefix(text, first, last, level);

		assert(prefixed);
	}
	if (edit)
		copy_array(history_inserted(&document->history, edit), arrview(block, size, all));
	if (document->code)
		syntax_update(text, first, last);
	document->version++;
	document->goal = SIZE_MAX;
	if (first == last && !whole) {
		anchor -= anchor - block < removed ? anchor - block : removed;
		caret -= caret - block < removed ? caret - block : removed;
		view_select(view, anchor, caret);
	} else {
		view_select(view, block, whole ? text_line_start(text, last + 1) : text_line_end(text, last));
	}
	if (edit)
		history_select(&document->history, view->anchor, view->caret);
	view_reveal(view, view->caret);
	document->streak = DOCUMENT_NONE;
	return true;
}

bool document_undo(struct document *document)
{
	struct view *view = &document->view;
	const struct history_edit *edit;
	size_t anchor = view->anchor, caret = view->caret;
	bool undone = false;

	while (!document->read_only && (edit = history_undo(&document->history))) {
		apply(document, edit->offset, edit->offset + edit->inserted, history_removed(&document->history, edit));
		anchor = edit->anchor;
		caret = edit->caret;
		undone = true;
		if (!edit->joined)
			break;
	}
	if (!undone)
		return false;
	document->goal = SIZE_MAX;
	document->streak = DOCUMENT_NONE;
	view_select(view, anchor, caret);
	view_reveal(view, caret);
	return true;
}

bool document_redo(struct document *document)
{
	struct view *view = &document->view;
	const struct history_edit *edit;

	if (document->read_only || !(edit = history_redo(&document->history)))
		return false;
	for (;;) {
		apply(document, edit->offset, edit->offset + edit->removed, history_inserted(&document->history, edit));
		if (!history_redo_joined(&document->history))
			break;
		edit = history_redo(&document->history);
	}
	document->goal = SIZE_MAX;
	document->streak = DOCUMENT_NONE;
	view_select(view, edit->after_anchor, edit->after_caret);
	view_reveal(view, edit->after_caret);
	return true;
}
