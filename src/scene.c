/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_stdio.h>
#include <string.h>

#include "path.h"
#include "scene.h"
#include "syntax.h"

static const uint32_t PANEL = QUAD_RGB(THEME_PANEL);
static const uint32_t BORDER = QUAD_RGB(THEME_BORDER);
static const uint32_t TEXT = QUAD_RGB(THEME_TEXT);
static const uint32_t DIM = QUAD_RGB(THEME_DIM);
static const uint32_t LINE_NUMBER = QUAD_RGB(THEME_LINE_NUMBER);
static const uint32_t CURRENT_LINE = QUAD_RGB(THEME_CURRENT_LINE);
static const uint32_t SELECTION = QUAD_RGB(THEME_SELECTION);
static const uint32_t SELECTION_INACTIVE = QUAD_RGB(THEME_SELECTION_INACTIVE);
static const uint32_t CARET = QUAD_RGB(THEME_CARET);
static const uint32_t ACCENT = QUAD_RGB(THEME_ACCENT);
static const uint32_t TREE_ACTIVE = QUAD_RGB(THEME_TREE_ACTIVE);
static const uint32_t CLASSES[SYNTAX_CLASSES] = {
	[SYNTAX_PLAIN] = QUAD_RGB(THEME_TEXT),
	[SYNTAX_KEYWORD] = QUAD_RGB(THEME_KEYWORD),
	[SYNTAX_CONTROL] = QUAD_RGB(THEME_CONTROL),
	[SYNTAX_TYPE] = QUAD_RGB(THEME_TYPE),
	[SYNTAX_FUNCTION] = QUAD_RGB(THEME_FUNCTION),
	[SYNTAX_CONSTANT] = QUAD_RGB(THEME_CONSTANT),
	[SYNTAX_NUMBER] = QUAD_RGB(THEME_NUMBER),
	[SYNTAX_STRING] = QUAD_RGB(THEME_STRING),
	[SYNTAX_COMMENT] = QUAD_RGB(THEME_COMMENT),
	[SYNTAX_PREPROCESSOR] = QUAD_RGB(THEME_PREPROCESSOR),
	[SYNTAX_ATTRIBUTE] = QUAD_RGB(THEME_ATTRIBUTE),
};

constexpr uint32_t ARROW_RIGHT = 0x25b8;
constexpr uint32_t ARROW_DOWN = 0x25be;
constexpr uint32_t DOT = 0x25cf;
constexpr uint32_t CROSS = 0xd7;
constexpr uint32_t ELLIPSIS = 0x2026;

/* Quads go into 'quads', 'full' tells that the atlas had no room for a glyph */
struct scene {
	const struct editor *editor;
	struct atlas *atlas;
	struct quad *quads;
	size_t count, capacity;
	bool full;
};

static void solid(struct scene *scene, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color)
{
	if (width <= 0 || height <= 0 || scene->count == scene->capacity)
		return;
	scene->quads[scene->count++] = (struct quad){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
		.u = QUAD_SOLID,
		.v = 0,
		.color = color,
	};
}

static void glyph(struct scene *scene, int32_t x, int32_t y, uint32_t codepoint, uint32_t color)
{
	uint16_t u, v;

	if (codepoint == ' ' || scene->count == scene->capacity)
		return;
	if (!atlas_find(scene->atlas, codepoint, &u, &v)) {
		scene->full = true;
		return;
	}
	scene->quads[scene->count++] = (struct quad){
		.x = x,
		.y = y,
		.width = scene->editor->cell_width,
		.height = scene->editor->cell_height,
		.u = u,
		.v = v,
		.color = color,
	};
}

static size_t characters(const char *string)
{
	size_t count = 0;

	foreach_array_const_ref(array_ptr(string, strlen(string)), byte)
		count += ((unsigned char)*byte & 0xc0) != 0x80;
	return count;
}

/* A string in the cells from 'x' to 'end', an ellipsis ends one that doesn't fit */
static void label(struct scene *scene, int32_t x, int32_t y, int32_t end, const char *string, uint32_t color)
{
	int32_t cell = scene->editor->cell_width;
	size_t cells = end > x ? (end - x) / cell : 0, count = characters(string), length;
	auto bytes = array_ptr(string, strlen(string));

	for (size_t at = 0, i = 0; at < ARRAY_SIZE(bytes) && i < cells; at += length, i++) {
		uint32_t codepoint = utf8_decode(arrview_cfront(at, bytes), &length);

		glyph(scene, x + i * cell, y, count > cells && i + 1 == cells ? ELLIPSIS : codepoint < 0x20 ? TEXT_REPLACEMENT : codepoint,
		      color);
	}
}

static void number(struct scene *scene, int32_t right, int32_t y, size_t value, uint32_t color)
{
	do {
		right -= scene->editor->cell_width;
		glyph(scene, right, y, '0' + value % 10, color);
		value /= 10;
	} while (value);
}

static void tree(struct scene *scene)
{
	const struct editor *editor = scene->editor;
	const struct tree *tree = &editor->tree;
	const struct editor_rect *rect = &editor->tree_rect;
	const struct editor_tab *tab = editor_tab(editor);
	uint32_t active = tab && editor->started ? tree_find(tree, tab->path) : TREE_NONE;
	int32_t y = rect->y + editor->padding, end = rect->x + rect->width - editor->padding;

	if (!rect->width)
		return;
	solid(scene, rect->x, rect->y, rect->width, rect->height, PANEL);
	solid(scene, rect->x + rect->width, rect->y, editor->line, rect->height, BORDER);
	if (!editor->started)
		return;
	for (uint32_t node = tree_row(tree, tree->top); node != TREE_NONE && y < rect->y + rect->height;
	     node = tree_next(tree, node), y += editor->cell_height) {
		const struct tree_node *n = &tree->nodes[node];
		int32_t x = rect->x + editor->padding + (n->depth - 1) * editor->cell_width;

		if (node == active)
			solid(scene, rect->x, y, rect->width, editor->cell_height, TREE_ACTIVE);
		if (n->directory && x + editor->cell_width <= end)
			glyph(scene, x, y, n->expanded ? ARROW_DOWN : ARROW_RIGHT, DIM);
		label(scene, x + 2 * editor->cell_width, y, end, tree_name(tree, node), TEXT);
	}
}

/* A tab shows a mark of changes, or one that closes it if it is active */
static void tabs(struct scene *scene)
{
	const struct editor *editor = scene->editor;
	const struct editor_rect *rect = &editor->tabs_rect;
	int32_t start, end, y = rect->y + editor->padding, right = rect->x + rect->width;

	solid(scene, rect->x, rect->y, rect->width, rect->height, PANEL);
	foreach_array_index(arrview_first(editor->tab_count, editor->order), i) {
		const struct editor_tab *tab = editor->order[i];
		bool active = i == editor->active, modified = history_modified(&tab->document.history);
		int32_t mark;

		if (!editor_tab_extent(editor, i, &start, &end))
			continue;
		mark = end - 2 * editor->cell_width;
		if (active) {
			solid(scene, start, rect->y, end - start, rect->height, SCENE_BACKGROUND);
			solid(scene, start, rect->y, end - start, 2 * editor->line, ACCENT);
		}
		label(scene, start + editor->cell_width, y, mark < right ? mark : right, path_name(tab->path), active ? TEXT : DIM);
		if ((modified || active) && mark + editor->cell_width <= right)
			glyph(scene, mark, y, modified ? DOT : CROSS, active ? TEXT : DIM);
		solid(scene, end - editor->line, rect->y, editor->line, rect->height, BORDER);
	}
	solid(scene, rect->x, rect->y + rect->height - editor->line, rect->width, editor->line, BORDER);
}

/* The cells of a selection in a line cover its line break too, if it goes on */
static void selection(struct scene *scene, const struct document *document, size_t line, int32_t y, size_t start, size_t end)
{
	const struct editor *editor = scene->editor;
	const struct text *text = &document->text;
	const struct view *view = &document->view;
	size_t first = text_line_start(text, line), last = text_line_end(text, line), from, to;
	int32_t x = editor->text_rect.x + view->padding, x0, x1;

	if (end <= first || start > last)
		return;
	from = start <= first ? 0 : text_column_of(text, line, start);
	to = end > last ? text_column_of(text, line, last) + 1 : text_column_of(text, line, end);
	if (to <= view->left)
		return;
	x0 = x + (from > view->left ? from - view->left : 0) * editor->cell_width;
	x1 = x + (to - view->left) * editor->cell_width;
	if (x1 > editor->text_rect.x + editor->text_rect.width)
		x1 = editor->text_rect.x + editor->text_rect.width;
	solid(scene, x0, y, x1 - x0, editor->cell_height, editor->focused ? SELECTION : SELECTION_INACTIVE);
}

/* Combining marks go over the cell before them */
static void characters_of(struct scene *scene, const struct document *document, size_t line, int32_t y)
{
	const struct editor *editor = scene->editor;
	const struct text *text = &document->text;
	const struct view *view = &document->view;
	size_t columns = view_columns(view), start = text_line_start(text, line);
	int32_t x = editor->text_rect.x + view->padding;
	struct syntax_token token;
	struct syntax syntax;
	struct text_char c;
	bool tokens = document->code;

	if (tokens) {
		syntax_init(&syntax, arrview(start, text_line_end(text, line) - start, array_ptr(text->bytes, text->size)),
			    text->states[line]);
		tokens = syntax_next(&syntax, &token);
	}
	text_chars_init(text, line, &c);
	while (text_chars_next(text, &c)) {
		size_t column = c.width || !c.column ? c.column : c.column - 1;

		if (c.column >= view->left + columns)
			break;
		if (column < view->left || c.codepoint == ' ')
			continue;
		while (tokens && c.offset - start >= token.end)
			tokens = syntax_next(&syntax, &token);
		glyph(scene, x + (column - view->left) * editor->cell_width, y, c.codepoint,
		      CLASSES[tokens && c.offset - start >= token.start ? token.class : SYNTAX_PLAIN]);
	}
}

/* Two keys in a row, the most important ones first */
static const struct {
	const char *keys, *action;
} KEYS[][2] = {
	{ { "Ctrl+S", "Save" }, { "Ctrl+Q", "Quit" } },
	{ { "F12", "Definition" }, { "Shift+F12", "Declaration" } },
	{ { "Alt+Left", "Back" }, { "Alt+Right", "Forward" } },
	{ { "Ctrl+Z", "Undo" }, { "Ctrl+Shift+Z", "Redo" } },
	{ { "Ctrl+C", "Copy" }, { "Ctrl+V", "Paste" } },
	{ { "Ctrl+X", "Cut" }, { "Ctrl+A", "Select all" } },
	{ { "Ctrl+W", "Close tab" }, { "Ctrl+B", "File tree" } },
	{ { "Ctrl+Tab", "Next tab" }, { "Ctrl+Shift+Tab", "Previous tab" } },
	{ { "Tab", "Indent" }, { "Shift+Tab", "Outdent" } },
	{ { "Ctrl+Left/Right", "By words" }, { "Ctrl+Home/End", "File start/end" } },
	{ { "Shift+arrows", "Select" }, { "Esc", "Deselect" } },
	{ { "Ctrl+Up/Down", "Scroll" }, { "Ctrl+Bksp/Del", "Delete word" } },
	{ { "Ctrl+click", "Definition" }, { "Middle click", "Paste selection" } },
	{ { "Double click", "Select word" }, { "Triple click", "Select line" } },
};

/* Without a file the text area shows the keys, as a table in the middle */
static void keys(struct scene *scene)
{
	const struct editor *editor = scene->editor;
	const struct editor_rect *rect = &editor->text_rect;
	int32_t cell = editor->cell_width, row = editor->cell_height * 3 / 2, x, y;
	size_t widths[2][2], total = 0;

	foreach_array_ref(widths, column)
		fill_array(*column, 0);
	foreach_array_const_ref(KEYS, line) {
		foreach_array_index(*line, side) {
			if (characters((*line)[side].keys) > widths[side][0])
				widths[side][0] = characters((*line)[side].keys);
			if (characters((*line)[side].action) > widths[side][1])
				widths[side][1] = characters((*line)[side].action);
		}
	}
	foreach_array_const_ref(widths, column)
		total += (*column)[0] + 2 + (*column)[1] + 4;
	x = rect->x + (rect->width - (int32_t)(total - 4) * cell) / 2;
	y = rect->y + (rect->height - (int32_t)ARRAY_SIZE(KEYS) * row) / 3;
	if (x < rect->x + editor->padding)
		x = rect->x + editor->padding;
	if (y < rect->y + editor->padding)
		y = rect->y + editor->padding;
	foreach_array_const_ref(KEYS, line) {
		int32_t start = x;

		foreach_array_index(*line, side) {
			label(scene, start, y, rect->x + rect->width, (*line)[side].keys, TEXT);
			label(scene, start + (widths[side][0] + 2) * cell, y, rect->x + rect->width, (*line)[side].action, DIM);
			start += (widths[side][0] + 2 + widths[side][1] + 4) * cell;
		}
		y += row;
	}
}

/* The line of the caret stands out while nothing is selected */
static void text(struct scene *scene)
{
	const struct editor *editor = scene->editor;
	const struct editor_tab *tab = editor_tab(editor);
	const struct editor_rect *gutter = &editor->gutter_rect, *rect = &editor->text_rect;
	const struct document *document;
	const struct text *text;
	const struct view *view;
	size_t start, end, caret_line, column, rows;
	bool selected;

	if (!tab) {
		keys(scene);
		return;
	}
	if (tab->loading)
		return;
	document = &tab->document;
	text = &document->text;
	view = &document->view;
	selected = view_selection(view, &start, &end);
	caret_line = text_line_of(text, view->caret);
	rows = view_rows(view);
	for (size_t row = 0; row < rows && view->top + row < text->line_count; row++) {
		size_t line = view->top + row;
		int32_t y = rect->y + view->padding + row * editor->cell_height;

		if (!selected && line == caret_line)
			solid(scene, gutter->x, y, gutter->width + rect->width, editor->cell_height, CURRENT_LINE);
		number(scene, gutter->x + gutter->width - editor->cell_width, y, line + 1, line == caret_line ? TEXT : LINE_NUMBER);
		if (selected)
			selection(scene, document, line, y, start, end);
		characters_of(scene, document, line, y);
	}
	column = text_column_of(text, caret_line, view->caret);
	if (caret_line >= view->top && caret_line < view->top + rows && column >= view->left &&
	    column - view->left < view_columns(view))
		solid(scene, rect->x + view->padding + (column - view->left) * editor->cell_width,
		      rect->y + view->padding + (caret_line - view->top) * editor->cell_height, 2 * editor->line, editor->cell_height,
		      editor->focused ? CARET : DIM);
}

static const char *server_state(const struct lsp *lsp)
{
	switch (lsp->state) {
	case LSP_OFF:
		return " is off";
	case LSP_STARTING:
		return " is starting";
	case LSP_READY:
		return "";
	case LSP_STOPPING:
		return " is stopping";
	case LSP_EXITED:
		return " exited";
	}
	return "";
}

/* What happened or the file of the tab on the left, where the caret is and the server on the right */
static void status_bar(struct scene *scene)
{
	const struct editor *editor = scene->editor;
	const struct editor_rect *rect = &editor->status_rect;
	const struct editor_tab *tab = editor_tab(editor);
	int32_t y = rect->y + editor->padding, start = rect->x + editor->padding, end = rect->x + rect->width - editor->padding;
	char left[PATH_MAX + 64], right[256];
	size_t length = 0;

	solid(scene, rect->x, rect->y, rect->width, rect->height, PANEL);
	solid(scene, rect->x, rect->y, rect->width, editor->line, BORDER);
	right[0] = 0;
	if (tab && !tab->loading) {
		const struct text *text = &tab->document.text;
		size_t line = text_line_of(text, tab->document.view.caret);

		length = snprintf_array(right, "Ln %zu, Col %zu", line + 1, text_column_of(text, line, tab->document.view.caret) + 1);
	}
	if (editor->lsp.argv && length < ARRAY_SIZE(right))
		snprintf_array(arrview_cfront(length, right), "%s%s%s", length ? "   " : "", editor->lsp.argv[0],
			       server_state(&editor->lsp));
	if (editor->status[0])
		snprintf_array(left, "%s", editor->status);
	else if (tab)
		snprintf_array(left, "%s%s%s", path_relative(editor->root, tab->path),
			       history_modified(&tab->document.history) ? " [+]" : "", tab->document.read_only ? " [read only]" : "");
	else
		snprintf_array(left, "%s", editor->root);
	end -= characters(right) * editor->cell_width;
	label(scene, end, y, rect->x + rect->width, right, DIM);
	label(scene, start, y, end - editor->cell_width, left, TEXT);
}

static void draw(struct scene *scene)
{
	text(scene);
	tree(scene);
	tabs(scene);
	status_bar(scene);
}

size_t scene_build_impl(const struct editor *editor, struct atlas *atlas, size_t capacity, struct quad (*quads)[capacity])
{
	struct scene scene = {
		.editor = editor,
		.atlas = atlas,
		.quads = *quads,
		.count = 0,
		.capacity = capacity,
		.full = false,
	};

	draw(&scene);
	if (scene.full) {
		atlas_clear(atlas);
		scene.count = 0;
		scene.full = false;
		draw(&scene);
	}
	return scene.count;
}
