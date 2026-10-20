/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <poor_array.h>
#include <string.h>

#include "atlas.h"
#include "document.h"
#include "json.h"
#include "path.h"
#include "syntax.h"
#include "test-runner.h"
#include "text.h"
#include "view.h"

/* A small document in memory of the test */
struct sample {
	struct text text;
	char bytes[256];
	uint32_t lines[32];
	uint8_t states[32];
};

static uint32_t decode(const char *s, size_t size, size_t *length)
{
	return utf8_decode(array_ptr(s, size), length);
}

static bool text_of(struct sample *sample, size_t line_capacity, const char *contents)
{
	text_init(&sample->text, sample->bytes, arrview_first(line_capacity, sample->lines),
		  arrview_first(line_capacity, sample->states));
	copy_array(sample->bytes, array_ptr(contents, strlen(contents)));
	return text_load(&sample->text, strlen(contents));
}

static bool text_is(const struct text *text, const char *contents)
{
	return text->size == strlen(contents) && !memcmp(text->bytes, contents, text->size);
}

TEST(utf8_decoding)
{
	size_t length;

	assert(decode("a", 1, &length) == 'a' && length == 1);
	assert(decode("\xc3\xa9", 2, &length) == 0xe9 && length == 2);
	assert(decode("\xe2\x82\xac", 3, &length) == 0x20ac && length == 3);
	assert(decode("\xf0\x9f\x98\x80", 4, &length) == 0x1f600 && length == 4);
	assert(decode("\x80", 1, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xc0\xaf", 2, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xe0\x80\xaf", 3, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xed\xa0\x80", 3, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xf4\x90\x80\x80", 4, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xe2\x82", 2, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xe2\x28\xac", 3, &length) == TEXT_REPLACEMENT && length == 1);
	assert(decode("\xf8\x88\x80\x80", 4, &length) == TEXT_REPLACEMENT && length == 1);
}

TEST(text_lines)
{
	static struct sample sample;
	struct text *text = &sample.text;

	assert(text_of(&sample, 8, "a\nbc\n\nd"));
	assert(text->line_count == 4 && text->lines[1] == 2 && text->lines[2] == 5 && text->lines[3] == 6);
	assert(text_line_end(text, 1) == 4 && text_line_end(text, 2) == 5 && text_line_end(text, 3) == 7);
	assert(text_line_of(text, 0) == 0 && text_line_of(text, 4) == 1 && text_line_of(text, 5) == 2 && text_line_of(text, 7) == 3);
	assert(text->columns == 2);
	assert(text_of(&sample, 8, "a\n") && text->line_count == 2 && text_line_end(text, 0) == 1 && text_line_end(text, 1) == 2);
	assert(text_of(&sample, 8, "") && text->line_count == 1 && text_line_end(text, 0) == 0 && text->columns == 0);
	assert(text_of(&sample, 8, "ab\r\ncd\r\n") && text->line_count == 3);
	assert(text_line_end(text, 0) == 2 && text_line_end(text, 1) == 6 && text_line_end(text, 2) == 8);
	assert(!text_of(&sample, 2, "a\nb\nc") && text->line_count == 2 && text->size == 3 && text_line_end(text, 1) == 3);
}

TEST(text_editing)
{
	static struct sample sample;
	struct text *text = &sample.text;

	assert(text_of(&sample, 8, "ab\ncd"));
	text->states[1] = 7;
	assert(text_replace(text, 1, 4, arrview_str("X\nY\nZ")) && text_is(text, "aX\nY\nZd"));
	assert(text->line_count == 3 && text->lines[1] == 3 && text->lines[2] == 5 && text->states[1] == 0);
	assert(text_replace(text, 2, 3, arrview_str(" ")) && text_is(text, "aX Y\nZd") && text->line_count == 2 && text->lines[1] == 5);
	assert(text_replace(text, 7, 7, arrview_str("\n")) && text->line_count == 3 && text_line_end(text, 2) == 8);
	assert(text->columns == 4 && text->widest == 1);
	assert(!text_fits(text, 0, 0, arrview_str("1\n2\n3\n4\n5\n6\n7")) && text_fits(text, 0, 8, arrview_str("1\n2\n3\n4\n5\n6\n7")));
	assert(text_replace(text, 0, text->size, arrview_str("1\n2\n3\n4\n5\n6\n7")) && text->line_count == 7);
	assert(text->columns == 1 && text->widest == 7);
	assert(!text_replace(text, 0, 0, arrview_str("\n\n")) && text_is(text, "1\n2\n3\n4\n5\n6\n7"));
	assert(text_replace(text, 0, 1, arrview_str("long")) && text->columns == 4 && text->widest == 1);
	assert(text_replace_impl(text, 0, text->size, 0, &"") && text_is(text, "") && text->line_count == 1 && !text->columns);
	assert(text_of(&sample, 8, "e\xcc\x81x\r\ny"));
	assert(text_next(text, 0) == 3 && text_next(text, 3) == 4 && text_next(text, 4) == 6 && text_next(text, 7) == 7);
	assert(text_prev(text, 6) == 4 && text_prev(text, 4) == 3 && text_prev(text, 3) == 0 && text_prev(text, 0) == 0);
}

static size_t cut_two(const void *, size_t start, size_t end)
{
	return end - start < 2 ? end - start : 2;
}

/* Bytes go at the starts of the lines that aren't empty and come off them again, the lines and the widths follow */
TEST(text_prefixes)
{
	static struct sample sample;
	static char wide[100];
	struct text *text = &sample.text;

	assert(text_of(&sample, 8, "ab\r\n\ncd\ne"));
	assert(text_prefix(text, 0, 3, arrview_str("--")) && text_is(text, "--ab\r\n\n--cd\n--e"));
	assert(text->line_count == 4 && text->lines[1] == 6 && text->lines[2] == 7 && text->lines[3] == 12);
	assert(text->columns == 4 && text_line_end(text, 0) == 4 && text_line_end(text, 1) == 6);
	text_unprefix(text, 1, 3, cut_two, nullptr);
	assert(text_is(text, "--ab\r\n\ncd\ne") && text->lines[2] == 7 && text->lines[3] == 10 && text->columns == 4);
	text_unprefix(text, 0, 0, cut_two, nullptr);
	assert(text_is(text, "ab\r\n\ncd\ne") && text->lines[1] == 4 && text->columns == 2);
	memset(wide, 'x', sizeof(wide));
	assert(!text_prefix(text, 0, 3, wide) && text_is(text, "ab\r\n\ncd\ne"));
}

static bool marked(const struct text *text, size_t count, const size_t (*lines)[count])
{
	if (text->line_count != count)
		return false;
	foreach_array_index(*lines, line)
		if (text_marked_line(text, line) != (*lines)[line])
			return false;
	return true;
}

/* Lines before and after all those that edits changed since the mark know where they were */
TEST(text_marks)
{
	static struct sample sample;
	struct text *text = &sample.text;
	constexpr size_t gone = SIZE_MAX;
	size_t none = 0;

	assert(text_of(&sample, 16, "a\nb\nc\nd\ne\n"));
	assert(marked(text, 6, &(const size_t[]){ 0, 1, 2, 3, 4, 5 }));
	assert(text_replace(text, 2, 3, arrview_str("B\nB")) && text_is(text, "a\nB\nB\nc\nd\ne\n"));
	assert(marked(text, 7, &(const size_t[]){ 0, gone, gone, 2, 3, 4, 5 }));
	assert(text_replace(text, 8, 10, arrview_first(none, "")) && text_is(text, "a\nB\nB\nc\ne\n"));
	assert(marked(text, 6, &(const size_t[]){ 0, gone, gone, gone, gone, 5 }));
	text_mark(text);
	assert(text_prefix(text, 4, 5, arrview_str("-")) && text_is(text, "a\nB\nB\nc\n-e\n"));
	assert(marked(text, 6, &(const size_t[]){ 0, 1, 2, 3, gone, gone }));
	assert(text_replace(text, 0, 2, arrview_first(none, "")) && text_is(text, "B\nB\nc\n-e\n"));
	assert(marked(text, 5, &(const size_t[]){ gone, gone, gone, gone, gone }));
}

TEST(text_columns)
{
	static struct sample sample;
	struct text *text = &sample.text;

	assert(text_of(&sample, 8, "\tx\nab\tc\nh\xc3\xa9llo\na\tb\ne\xcc\x81x"));
	assert(text_column_of(text, 0, 1) == 8 && text_column_of(text, 1, 6) == 8);
	assert(text->columns == 9);
	assert(text_offset_at(text, 2, 2) == 11 && text_column_of(text, 2, 11) == 2);
	assert(text_offset_at(text, 2, 100) == text_line_end(text, 2));
	assert(text_offset_at(text, 3, 3) == 16 && text_offset_at(text, 3, 6) == 17 && text_offset_at(text, 3, 8) == 17);
	assert(text_offset_at(text, 4, 1) == 22 && text_column_of(text, 4, 22) == 1);
	assert(text_boundary(text, 0) == 0 && text_boundary(text, 10) == 9 && text_boundary(text, 14) == 14);
	assert(text_boundary(text, 21) == 19 && text_boundary(text, 22) == 22 && text_boundary(text, 23) == 23);
	assert(text_of(&sample, 8, "abc\r\n") && text_boundary(text, 4) == 3 && text_boundary(text, 3) == 3);
}

static const char numbered[] = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n"
			       "a long line of fourty characters at most\n21";

TEST(view_scrolling)
{
	static struct sample sample;
	struct view view;

	assert(text_of(&sample, ARRAY_SIZE(sample.lines), numbered));
	view_init(&view, &sample.text);
	view_resize(&view, 104, 104, 10, 20, 4);
	assert(view_rows(&view) == 5 && view_columns(&view) == 10 && view_page(&view) == 4);
	assert(view_scroll(&view, 3, 0) && view.top == 3);
	assert(view_scroll(&view, PTRDIFF_MAX, 0) && view.top == 22 - 5);
	assert(!view_scroll(&view, 1, 0));
	assert(view_scroll(&view, PTRDIFF_MIN, 0) && view.top == 0);
	assert(view_scroll(&view, 0, PTRDIFF_MAX) && view.left == 41 - 10);
	assert(view_scroll(&view, 0, -5) && view.left == 26);
	assert(view_reveal(&view, sample.text.lines[20]) && view.top == 16 && view.left == 0);
	assert(view_reveal(&view, sample.text.lines[3]) && view.top == 3 && !view_reveal(&view, sample.text.lines[5]));
	view_resize(&view, 1000, 1000, 10, 20, 4);
	assert(view.top == 0 && view.left == 0);
}

TEST(view_selecting)
{
	static struct sample sample;
	struct text *text = &sample.text;
	struct view view;
	size_t start, end;

	assert(text_of(&sample, ARRAY_SIZE(sample.lines), numbered));
	view_init(&view, text);
	view_resize(&view, 104, 104, 10, 20, 4);
	assert(view_hit(&view, 4, 4) == 0 && view_hit(&view, 9, 4) == 1 && view_hit(&view, -50, -50) == 0);
	assert(view_hit(&view, 4, 24) == 2 && view_hit(&view, 1000, 24) == 3);
	view.top = 20;
	assert(view_hit(&view, 26, 4) == text->lines[20] + 2 && view_hit(&view, 4, 1000) == text->lines[21]);
	view.top = 0;
	assert(view_select(&view, 2, 2) && !view_selection(&view, &start, &end));
	assert(view_drag(&view, 1000, 44));
	assert(view_selection(&view, &start, &end) && start == 2 && end == 5);
	assert(view_drag(&view, 4, 1000) && view.top == 1 && view.left == 1);
	assert(view_selection(&view, &start, &end) && start == 2 && end == text_line_end(text, 5));
	assert(view_select(&view, 9, 3) && view_selection(&view, &start, &end) && start == 3 && end == 9);
}

/* A document in memory of the test, with a view of 10 x 5 cells */
struct editing {
	struct document document;
	char bytes[256];
	uint32_t lines[32];
	uint8_t states[32];
	struct history_edit edits[64];
	char history[1024];
};

static void document_of(struct editing *editing, const char *contents)
{
	document_init(&editing->document, editing->bytes, editing->lines, editing->states, editing->edits, editing->history);
	editing->document.code = true;
	view_resize(&editing->document.view, 100, 100, 10, 20, 0);
	copy_array(editing->bytes, array_ptr(contents, strlen(contents)));
	assert(document_load(&editing->document, strlen(contents)));
}

static bool type(struct document *document, const char *bytes)
{
	return document_insert(document, array_ptr(bytes, strlen(bytes)), true);
}

static bool caret_at(const struct document *document, size_t anchor, size_t caret)
{
	return document->view.anchor == anchor && document->view.caret == caret;
}

TEST(document_typing)
{
	static struct editing editing;
	struct document *document = &editing.document;
	uint64_t version;

	document_of(&editing, "int x;\n");
	version = document->version;
	assert(document_move(document, DOCUMENT_END, false) && caret_at(document, 6, 6));
	assert(type(document, "a") && type(document, "b") && type(document, " ") && type(document, "c"));
	assert(text_is(&document->text, "int x;ab c\n") && document->version == version + 4);
	assert(document_undo(document) && text_is(&document->text, "int x;ab\n") && caret_at(document, 8, 8));
	assert(document_undo(document) && text_is(&document->text, "int x;\n") && caret_at(document, 6, 6));
	assert(!document_undo(document) && !history_modified(&document->history));
	assert(document_redo(document) && text_is(&document->text, "int x;ab\n") && caret_at(document, 8, 8));
	assert(document_redo(document) && !document_redo(document) && text_is(&document->text, "int x;ab c\n"));
	assert(document_select_word(document, 7) && caret_at(document, 6, 8));
	assert(document_insert(document, arrview_str("yz"), false) && text_is(&document->text, "int x;yz c\n"));
	assert(document_erase(document, false, false) && document_erase(document, false, false));
	assert(text_is(&document->text, "int x; c\n") && document_undo(document) && text_is(&document->text, "int x;yz c\n"));
	assert(document_move(document, DOCUMENT_HOME, false) && caret_at(document, 0, 0));
	assert(document_erase(document, true, true) && text_is(&document->text, " x;yz c\n"));
	assert(document_select_all(document) && caret_at(document, 0, 8));
	assert(document_erase(document, false, false) && text_is(&document->text, "") && document->text.line_count == 1);
	assert(document_undo(document) && text_is(&document->text, " x;yz c\n") && caret_at(document, 0, 8));
}

/* A save that goes on counts until it ends, even after undoing and editing leave its copy out of reach */
TEST(document_saving)
{
	static struct editing editing;
	struct document *document = &editing.document;
	struct history *history = &document->history;

	document_of(&editing, "a");
	assert(type(document, "b"));
	history_save(history);
	assert(!history_unsaved(history) && history_modified(history));
	assert(document_undo(document) && history_unsaved(history) && !history_modified(history));
	assert(type(document, "c") && document_undo(document) && text_is(&document->text, "a"));
	assert(history_unsaved(history) && !history_modified(history));
	history_saved(history, true);
	assert(history_unsaved(history) && history_modified(history));
	document_of(&editing, "");
	assert(type(document, "a"));
	history_save(history);
	assert(type(document, "b") && document_undo(document) && text_is(&document->text, "a") && !history_unsaved(history));
	assert(document_undo(document) && text_is(&document->text, ""));
	assert(document_redo(document) && text_is(&document->text, "a") && document_redo(document) && text_is(&document->text, "ab"));
}

/* Indenting many lines at once is one step of the history, which holds few edits */
TEST(document_indents_many)
{
	static struct {
		struct document document;
		char bytes[1 << 16];
		uint32_t lines[1 << 15];
		uint8_t states[1 << 15];
		struct history_edit edits[8];
		char history[1 << 17];
	} big;
	static char original[1 << 16];
	struct document *document = &big.document;
	size_t size = 0, full = 0;

	document_init(document, big.bytes, big.lines, big.states, big.edits, big.history);
	view_resize(&document->view, 100, 100, 10, 20, 0);
	for (size_t i = 0; i < 20000; i++) {
		if (i % 3) {
			big.bytes[size++] = 'x';
			full++;
		}
		big.bytes[size++] = '\n';
	}
	copy_array(arrview_first(size, original), arrview_first(size, big.bytes));
	assert(document_load(document, size) && document_select_all(document) && document_indent(document, false));
	assert(document->text.size == size + full && !memcmp(big.bytes, "\n\tx\n\tx\n\n\tx\n", 11));
	assert(document_undo(document) && document->text.size == size && !memcmp(big.bytes, original, size));
	assert(!document_undo(document) && document_redo(document) && document->text.size == size + full);
	assert(caret_at(document, 0, size + full) && document_indent(document, true));
	assert(document->text.size == size && !memcmp(big.bytes, original, size));
}

TEST(document_lines)
{
	static struct editing editing;
	struct document *document = &editing.document;

	document_of(&editing, "\tif (x) {");
	assert(document_move(document, DOCUMENT_BOTTOM, false) && document_newline(document));
	assert(text_is(&document->text, "\tif (x) {\n\t\t") && caret_at(document, 12, 12));
	assert(type(document, "}") && text_is(&document->text, "\tif (x) {\n\t}"));
	assert(document_undo(document) && text_is(&document->text, "\tif (x) {\n\t\t"));
	document_of(&editing, "{}");
	assert(document_place(document, 1, false) && document_newline(document));
	assert(text_is(&document->text, "{\n\t\n}") && caret_at(document, 3, 3));
	assert(document_undo(document) && document_redo(document) && caret_at(document, 3, 3));
	document_of(&editing, "\t{}");
	assert(document_place(document, 2, false) && document_newline(document) && text_is(&document->text, "\t{\n\t\t\n\t}"));
	assert(caret_at(document, 5, 5) && document->view.left <= 16 && document->view.left + view_columns(&document->view) > 16);
	document_of(&editing, "a\nb\n\nc");
	assert(document_place(document, 1, false) && document_place(document, 5, true) && document_indent(document, false));
	assert(text_is(&document->text, "\ta\n\tb\n\nc") && caret_at(document, 0, 7));
	assert(document_indent(document, true) && text_is(&document->text, "a\nb\n\nc"));
	assert(document_undo(document) && text_is(&document->text, "\ta\n\tb\n\nc"));
	assert(document_undo(document) && text_is(&document->text, "a\nb\n\nc") && caret_at(document, 1, 5));
	assert(document_place(document, 4, false) && document_indent(document, false) && text_is(&document->text, "a\nb\n\t\nc"));
	document_of(&editing, "\tabcdefghijklmnopqrstuvwxyz\n");
	assert(document_move(document, DOCUMENT_END, false) && document->view.left);
	assert(document_indent(document, true) && text_is(&document->text, "abcdefghijklmnopqrstuvwxyz\n"));
	assert(caret_at(document, 26, 26) && document->view.left <= 26 && document->view.left + view_columns(&document->view) > 26);
	document_of(&editing, "a\nb\n");
	assert(document_select_line(document, 0) && document_indent(document, false));
	assert(text_is(&document->text, "\ta\nb\n") && caret_at(document, 0, 3));
	assert(document_undo(document) && document_redo(document) && caret_at(document, 0, 3));
	assert(document_indent(document, false) && text_is(&document->text, "\t\ta\nb\n"));
	assert(document_indent(document, true) && text_is(&document->text, "\ta\nb\n") && caret_at(document, 0, 3));
	document_of(&editing, "  a\n");
	assert(document->indent == 2 && document_place(document, 3, false) && document_indent(document, true));
	assert(text_is(&document->text, "a\n") && caret_at(document, 1, 1));
	assert(document_indent(document, false) && text_is(&document->text, "a \n"));
}

TEST(document_motions)
{
	static struct editing editing;
	struct document *document = &editing.document;

	document_of(&editing, "foo bar(baz)\n\tx\nlonger line");
	assert(document_move(document, DOCUMENT_WORD_RIGHT, false) && document->view.caret == 3);
	assert(document_move(document, DOCUMENT_WORD_RIGHT, false) && document->view.caret == 7);
	assert(document_move(document, DOCUMENT_WORD_RIGHT, false) && document->view.caret == 8);
	assert(document_move(document, DOCUMENT_WORD_RIGHT, true) && caret_at(document, 8, 11));
	assert(document_move(document, DOCUMENT_LEFT, false) && caret_at(document, 8, 8));
	assert(document_move(document, DOCUMENT_WORD_LEFT, false) && document->view.caret == 7);
	assert(document_move(document, DOCUMENT_WORD_LEFT, false) && document->view.caret == 4);
	assert(document_move(document, DOCUMENT_DOWN, false) && document->view.caret == 13);
	assert(document_move(document, DOCUMENT_DOWN, false) && document->view.caret == 20);
	assert(document_move(document, DOCUMENT_UP, false) && document_move(document, DOCUMENT_UP, false));
	assert(document->view.caret == 4 && document_move(document, DOCUMENT_UP, false) && document->view.caret == 0);
	assert(document_place(document, 15, false) && document_move(document, DOCUMENT_HOME, false) && document->view.caret == 14);
	assert(document_move(document, DOCUMENT_HOME, false) && document->view.caret == 13);
	assert(document_move(document, DOCUMENT_END, false) && document->view.caret == 15);
	assert(document_move(document, DOCUMENT_RIGHT, false) && document->view.caret == 16);
	assert(document_move(document, DOCUMENT_LEFT, false) && document->view.caret == 15);
	assert(document_select_line(document, 14) && caret_at(document, 13, 16));
}

/* The classes of the tokens of a line after 'state', in order, and the state of the next line */
static uint8_t tokens(uint8_t state, const char *line, size_t count, const enum syntax_class (*classes)[count],
		      const char *const (*words)[count])
{
	struct syntax_token token;
	struct syntax syntax;
	size_t i = 0;

	syntax_init(&syntax, array_ptr(line, strlen(line)), state);
	while (syntax_next(&syntax, &token)) {
		assert(i < count && token.class == (*classes)[i]);
		assert(token.end - token.start == strlen((*words)[i]) && !memcmp(line + token.start, (*words)[i], token.end - token.start));
		i++;
	}
	assert(i == count);
	return syntax_end(&syntax);
}

/* .. after a name of the path stays, since the name may be a link */
TEST(path_joining)
{
	char path[32];

	assert(path_join(path, "/home/u/proj", "src/./x.c") && !strcmp(path, "/home/u/proj/src/x.c"));
	assert(path_join(path, "/home/u/proj", "..//other/x.c") && !strcmp(path, "/home/u/other/x.c"));
	assert(path_join(path, "/home/u/proj", "link/../x.c") && !strcmp(path, "/home/u/proj/link/../x.c"));
	assert(path_join(path, "/home/u/proj", "../../../..") && !strcmp(path, "/"));
	assert(path_join(path, "/home/u/proj", "/a/../../b") && !strcmp(path, "/a/../../b"));
	assert(path_join(path, "/", "../b/.") && !strcmp(path, "/b"));
	assert(!path_join(path, "/home/u/proj", "a/path/that/is/too/long"));
}

TEST(syntax_tokens)
{
	static const enum syntax_class include[] = { SYNTAX_PREPROCESSOR, SYNTAX_STRING };
	static const char *const include_words[] = { "#include", "<stdio.h>" };
	static const enum syntax_class code[] = {
		SYNTAX_KEYWORD, SYNTAX_TYPE, SYNTAX_FUNCTION, SYNTAX_TYPE, SYNTAX_CONTROL, SYNTAX_JUMP, SYNTAX_CONSTANT,
		SYNTAX_NUMBER, SYNTAX_STRING, SYNTAX_STRING, SYNTAX_COMMENT,
	};
	static const char *const code_words[] = {
		"static", "int", "foo", "void", "if", "return", "BAR", "0x1'0p+3", "'c'", "u8\"s\\\"\"", "// done",
	};
	static const enum syntax_class tag[] = { SYNTAX_KEYWORD, SYNTAX_TYPE };
	static const char *const tag_words[] = { "struct", "seat" };
	static const enum syntax_class attribute[] = { SYNTAX_ATTRIBUTE, SYNTAX_TYPE, SYNTAX_CONSTANT, SYNTAX_COMMENT };
	static const char *const attribute_words[] = { "nodiscard", "size_t", "nullptr", "/* a" };
	static const enum syntax_class continued[] = { SYNTAX_COMMENT, SYNTAX_PREPROCESSOR, SYNTAX_STRING };
	static const char *const continued_words[] = { "b */", "defined", "\"x\\" };
	static const enum syntax_class directive[] = { SYNTAX_COMMENT, SYNTAX_PREPROCESSOR, SYNTAX_CONSTANT };
	static const char *const directive_words[] = { "/**/", "#  define", "X" };
	static const enum syntax_class none[1];
	static const char *const no_words[1];
	uint8_t state;

	assert(!tokens(0, "#include <stdio.h>", 2, &include, &include_words));
	assert(!tokens(0, "static int foo(void) { if (x) return BAR + 0x1'0p+3 + 'c' + u8\"s\\\"\"; } // done", 11, &code, &code_words));
	assert(!tokens(0, "struct seat *seat;", 2, &tag, &tag_words));
	state = tokens(0, "[[nodiscard]] size_t x = nullptr; /* a", 4, &attribute, &attribute_words);
	assert(state);
	state = tokens(state | 8, "b */ defined \"x\\", 3, &continued, &continued_words);
	assert(state && tokens(state, "", 0, &none, &no_words) == 0);
	assert(!tokens(0, "/**/ #  define X", 3, &directive, &directive_words));
}

TEST(syntax_states)
{
	static struct sample sample;
	struct text *text = &sample.text;

	assert(text_of(&sample, 8, "/*\nint x;\n*/\nint y;\n#define A \\\n\t1\nB"));
	syntax_update(text, 0, text->line_count - 1);
	assert(text->states[1] && text->states[2] && !text->states[3] && !text->states[4] && text->states[5] && !text->states[6]);
	assert(text_replace(text, 0, 2, arrview_str("//")));
	syntax_update(text, 0, 0);
	assert(!text->states[1] && !text->states[2] && !text->states[3] && text->states[5]);
}

TEST(json_reading)
{
	static const char message[] = "{\"id\": 3, \"result\": [{\"uri\": \"file:///a%20b.c\", \"range\": {\"start\": "
				      "{\"line\": 4, \"character\": 2}}}], \"s\": \"a\\n\\u00e9\\ud83d\\ude00\\\"\", \"n\": null}";
	auto text = arrview_str(message);
	struct json root, value, location, range;
	int64_t integer;
	char out[32];

	assert(json_parse(text, &root) && root.start == 0 && root.end == ARRAY_SIZE(text));
	assert(json_member(text, root, "id", &value) && json_integer(text, value, &integer) && integer == 3);
	assert(json_member(text, root, "result", &value) && json_element(text, value, 0, &location));
	assert(!json_element(text, value, 1, &range));
	assert(json_member(text, location, "uri", &value) && json_string(text, value, out) == 15 && !strcmp(out, "file:///a%20b.c"));
	assert(json_member(text, location, "range", &range) && json_member(text, range, "start", &range));
	assert(json_member(text, range, "character", &value) && json_integer(text, value, &integer) && integer == 2);
	assert(json_member(text, root, "s", &value) && json_string(text, value, out) == 9 && !strcmp(out, "a\n\xc3\xa9\xf0\x9f\x98\x80\""));
	assert(json_string(text, value, arrview_first(5, out)) == SIZE_MAX);
	assert(json_member(text, root, "n", &value) && json_is(text, value, "null") && !json_member(text, root, "x", &value));
	assert(!json_parse(arrview_str("{\"a\": 1,}"), &root) && !json_parse(arrview_str("[1 2]"), &root));
	assert(!json_parse(arrview_str("{\"a\" 1}"), &root) && !json_parse(arrview_str("\"a"), &root));
	assert(json_parse(arrview_str(" [] "), &root) && !json_element(arrview_str(" [] "), root, 0, &value));
}

TEST(json_writing)
{
	struct json_writer writer;
	char bytes[64];

	json_writer_init(&writer, bytes);
	json_printf(&writer, "{\"n\": %d, \"s\": ", 12);
	json_quote(&writer, arrview_str("a\"b\\c\n\t\x01\xff\xc3\xa9"));
	json_printf(&writer, "}");
	assert(!writer.overflow);
	assert(writer.size == strlen("{\"n\": 12, \"s\": \"a\\\"b\\\\c\\n\\t\\u0001\\ufffd\xc3\xa9\"}"));
	assert(!memcmp(bytes, "{\"n\": 12, \"s\": \"a\\\"b\\\\c\\n\\t\\u0001\\ufffd\xc3\xa9\"}", writer.size));
	json_quote(&writer, arrview_first(40, bytes));
	assert(writer.overflow);
}

TEST(atlas_cells)
{
	static struct atlas atlas;
	struct font font;
	uint16_t x, y, x2, y2;
	uint32_t codepoint = 0x4e00;

	assert(font_init(&font, 15) && font.cell_width > 0 && font.cell_height > font.cell_width);
	atlas_init(&atlas, &font);
	assert(atlas.dirty_top >= atlas.dirty_bottom);
	assert(atlas_find(&atlas, 'a', &x, &y) && x == 0 && y == 0);
	assert(atlas_find(&atlas, 'b', &x2, &y2) && x2 == font.cell_width && y2 == 0);
	assert(atlas_find(&atlas, 'a', &x, &y) && x == 0 && y == 0 && atlas.count == 2);
	assert(atlas.dirty_top == 0 && atlas.dirty_bottom == (size_t)font.cell_height);
	atlas_uploaded(&atlas);
	while (atlas_find(&atlas, codepoint, &x, &y))
		codepoint++;
	assert(atlas.count == atlas.capacity && atlas.capacity <= ATLAS_GLYPHS / 2);
	atlas_clear(&atlas);
	assert(atlas.count == 0 && atlas_find(&atlas, 'b', &x, &y) && x == 0 && y == 0);
	assert(font_resize(&font, 60) && font.cell_width > 30);
	atlas_clear(&atlas);
	assert(atlas.capacity == (ATLAS_SIZE / font.cell_width) * (ATLAS_SIZE / font.cell_height));
	font_fini(&font);
}
