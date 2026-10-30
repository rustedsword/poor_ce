/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poor_stdio.h>
#include <stdarg.h>
#include <string.h>

#include "cursor-shape-v1-client-protocol.h"
#include "editor.h"
#include "path.h"
#include "seat.h"

/* Pixels of the line between the tree and the tabs that drag it, on each side, at scale 1 */
constexpr int32_t GRIP = 4;
constexpr size_t GUTTER_DIGITS = 3;

static void changed(struct editor *editor)
{
	editor->listener->changed(editor);
}

[[gnu::format(printf, 2, 3)]] static void status(struct editor *editor, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf_array(editor->status, fmt, ap);
	va_end(ap);
	if (editor->searching)
		poor_loop_timer_arm(editor->loop, &editor->prompting, poor_loop_now() + EDITOR_PROMPT_NS);
	changed(editor);
}

static void on_prompt(struct poor_loop *, struct poor_loop_timer *timer)
{
	struct editor *editor = container_of(timer, struct editor, prompting);

	if (editor->confirm != EDITOR_NOTHING)
		return;
	editor->status[0] = 0;
	changed(editor);
}

struct editor_tab *editor_tab(const struct editor *editor)
{
	return editor->tab_count ? editor->order[editor->active] : nullptr;
}

/* The document that keys edit: the query while searching */
static struct document *focus(struct editor *editor)
{
	struct editor_tab *tab = editor_tab(editor);

	if (editor->searching)
		return &editor->find.query;
	return tab && !tab->loading ? &tab->document : nullptr;
}

bool editor_browsing(const struct editor *editor)
{
	return editor->started && editor->show_tree && (editor->browsing || !editor->tab_count);
}

static size_t index_of(const struct editor *editor, const struct editor_tab *tab)
{
	foreach_array_index(arrview_first(editor->tab_count, editor->order), i)
		if (editor->order[i] == tab)
			return i;
	return SIZE_MAX;
}

static struct editor_tab *tab_find(const struct editor *editor, const char *path)
{
	foreach_array_const_ref(arrview_first(editor->tab_count, editor->order), tab)
		if (!strcmp((*tab)->path, path))
			return *tab;
	return nullptr;
}

static void retitle(struct editor *editor)
{
	struct editor_tab *tab = editor_tab(editor);

	snprintf_array(editor->title, "%s%sce", tab ? path_name(tab->path) : "", tab ? " - " : "");
	editor->listener->title(editor, editor->title);
}

/* Returns whether the server has the document as it is, a document that isn't code or is being read it doesn't need */
static bool lsp_sync(struct editor *editor, struct editor_tab *tab)
{
	const char *lang = lsp_language(tab->path);
	bool sent;

	if (!lang || tab->loading)
		return true;
	if (editor->lsp.state != LSP_READY)
		return false;
	if (tab->lsp_version == tab->document.version)
		return true;
	if (tab->lsp_version)
		sent = lsp_change(&editor->lsp, tab->path, tab->document.version, &tab->document.text, EDITOR_MARK_SERVER);
	else
		sent = lsp_open(&editor->lsp, tab->path, lang, tab->document.version, &tab->document.text);
	if (sent) {
		tab->lsp_version = tab->document.version;
		text_mark(&tab->document.text, EDITOR_MARK_SERVER);
	}
	return sent;
}

/* The server gets the documents that changed, it may answer from any of them. Returns one that it can't take now. */
static struct editor_tab *lsp_sync_all(struct editor *editor)
{
	struct editor_tab *stale = nullptr;

	foreach_array_ref(arrview_first(editor->tab_count, editor->order), tab)
		if (!lsp_sync(editor, *tab) && !stale)
			stale = *tab;
	return stale;
}

static bool serving(const struct editor *editor)
{
	return editor->lsp.state == LSP_READY && !editor->lsp.stopping;
}

/* Whether the shown document needs tokens that haven't been asked for yet */
static bool stale(const struct editor *editor, const struct editor_tab *tab)
{
	return tab && !tab->loading && tab->document.code && editor->lsp.highlights &&
	       tab->tokens_version != tab->document.version &&
	       (editor->highlight_tab != tab || editor->highlight_version != tab->document.version);
}

/* Documents go to the server, and the shown one gets its tokens, once they stop changing for a moment */
static void highlight(struct editor *editor)
{
	bool unsent = false;

	if (!serving(editor))
		return;
	foreach_array_const_ref(arrview_first(editor->tab_count, editor->order), tab)
		unsent |= lsp_language((*tab)->path) && !(*tab)->loading && (*tab)->lsp_version != (*tab)->document.version;
	if (unsent || stale(editor, editor_tab(editor)))
		poor_loop_timer_arm(editor->loop, &editor->highlight, poor_loop_now() + EDITOR_HIGHLIGHT_NS);
}

/* Every changed document goes to the server first, tokens depend on headers. After a full buffer, writes retry this. */
static void on_highlight(struct poor_loop *, struct poor_loop_timer *timer)
{
	struct editor *editor = container_of(timer, struct editor, highlight);
	struct editor_tab *tab = editor_tab(editor);

	if (!serving(editor) || lsp_sync_all(editor) || !stale(editor, tab) || !lsp_highlight(&editor->lsp, tab->path))
		return;
	editor->highlight_tab = tab;
	editor->highlight_version = tab->document.version;
}

static size_t cells_of(const char *string)
{
	size_t cells = 0;

	foreach_array_const_ref(array_ptr(string, strlen(string)), byte)
		cells += ((unsigned char)*byte & 0xc0) != 0x80;
	return cells;
}

/* Cells of a tab: its name between a blank and a mark that closes it or shows changes */
static size_t tab_cells(const struct editor_tab *tab)
{
	return cells_of(path_name(tab->path)) + 4;
}

bool editor_tab_extent(const struct editor *editor, size_t index, int32_t *start, int32_t *end)
{
	int32_t x = editor->tabs_rect.x;

	if (index < editor->tab_first || index >= editor->tab_count)
		return false;
	foreach_array_const_ref(arrview(editor->tab_first, index - editor->tab_first, editor->order), tab)
		x += tab_cells(*tab) * editor->cell_width;
	*start = x;
	*end = x + tab_cells(editor->order[index]) * editor->cell_width;
	return *start < editor->tabs_rect.x + editor->tabs_rect.width;
}

static size_t digits(size_t number)
{
	size_t count = 1;

	while (number >= 10) {
		number /= 10;
		count++;
	}
	return count < GUTTER_DIGITS ? GUTTER_DIGITS : count;
}

static int32_t gutter_width(const struct editor *editor, const struct editor_tab *tab)
{
	return (digits(tab ? tab->document.text.line_count : 1) + 2) * editor->cell_width + editor->padding;
}

/* The tree keeps at least a few columns, and leaves half of the window */
static size_t tree_columns(const struct editor *editor, size_t columns)
{
	size_t half = editor->width / 2 / editor->cell_width;

	if (columns < EDITOR_TREE_MIN)
		columns = EDITOR_TREE_MIN;
	return columns < half ? columns : half;
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

/* The matches show once they are counted */
size_t editor_details_impl(const struct editor *editor, size_t size, char (*out)[size])
{
	const struct editor_tab *tab = editor_tab(editor);
	const struct find *find = &editor->find;
	size_t length = 0;

	(*out)[0] = 0;
	if (editor->searching && find->search.size && find_counted(find)) {
		if (!find->count)
			length = snprintf_array(out, "No matches");
		else if (find->match == SIZE_MAX)
			length = snprintf_array(out, "%zu %s", find->count, find->count == 1 ? "match" : "matches");
		else
			length = snprintf_array(out, "%zu of %zu", find->index, find->count);
	}
	if (tab && !tab->loading) {
		const struct text *text = &tab->document.text;
		size_t line = text_line_of(text, tab->document.view.caret);

		length += snprintf_array(arrview_cfront(length, out), "%sLn %zu, Col %zu", length ? "   " : "", line + 1,
					 text_column_of(text, line, tab->document.view.caret) + 1);
	}
	if (editor->lsp.argv && length < size)
		snprintf_array(arrview_cfront(length, out), "%s%s%s", length ? "   " : "", editor->lsp.argv[0],
			       server_state(&editor->lsp));
	return cells_of(*out);
}

/*
 * All views get the size of the text area, which depends on the lines of their documents. A view that narrows for
 * more lines keeps its caret in sight, where an edit left it. The tab bar shows from the first tab, unless the active
 * one wouldn't fit. The query gets the whole cells between the prompt and the details.
 */
void editor_layout(struct editor *editor)
{
	struct editor_tab *active = editor_tab(editor);
	int32_t bar = editor->cell_height + 2 * editor->padding, body = editor->height - 2 * bar;
	int32_t tree = editor->show_tree ? tree_columns(editor, editor->tree_columns) * editor->cell_width + 2 * editor->padding : 0;
	int32_t x = tree ? tree + editor->line : 0;
	int32_t gutter = gutter_width(editor, active);
	int32_t query = editor->padding + (ARRAY_SIZE(EDITOR_PROMPT) - 1) * editor->cell_width, end;
	char details[EDITOR_DETAILS];

	if (body < 0)
		body = 0;
	end = editor->width - editor->padding - (editor_details(editor, details) + 1) * editor->cell_width;
	editor->tree_rect = (struct editor_rect){ .x = 0, .y = 0, .width = tree, .height = editor->height - bar };
	editor->tabs_rect = (struct editor_rect){ .x = x, .y = 0, .width = editor->width - x, .height = bar };
	editor->status_rect = (struct editor_rect){ .x = 0, .y = editor->height - bar, .width = editor->width, .height = bar };
	editor->gutter_rect = (struct editor_rect){ .x = x, .y = bar, .width = gutter, .height = body };
	editor->text_rect = (struct editor_rect){ .x = x + gutter, .y = bar, .width = editor->width - x - gutter, .height = body };
	editor->query_rect = (struct editor_rect){
		.x = query,
		.y = editor->height - bar,
		.width = end > query ? (end - query) / editor->cell_width * editor->cell_width : 0,
		.height = bar,
	};
	text_remeasure(&editor->find.query.text, SIZE_MAX);
	view_resize(&editor->find.query.view, editor->query_rect.width, editor->cell_height, editor->cell_width,
		    editor->cell_height, 0);
	view_reveal(&editor->find.query.view, editor->find.query.view.caret);
	foreach_array_ref(editor->tabs, tab) {
		if (!tab->used || tab->closing)
			continue;
		view_resize(&tab->document.view, editor->width - x - gutter_width(editor, tab), body, editor->cell_width,
			    editor->cell_height, editor->padding);
		if (digits(tab->document.text.line_count) > tab->digits)
			view_reveal(&tab->document.view, tab->document.view.caret);
		tab->digits = digits(tab->document.text.line_count);
	}
	for (editor->tab_first = 0; active && editor->tab_first < editor->active; editor->tab_first++) {
		int32_t start, end;

		if (editor_tab_extent(editor, editor->active, &start, &end) && end <= editor->tabs_rect.x + editor->tabs_rect.width)
			break;
	}
}

static bool searching_in(const struct editor *editor, const struct editor_tab *tab)
{
	return editor->searching && tab && &tab->document == editor->find.target;
}

static void stop_searching(struct editor *editor)
{
	if (!editor->searching)
		return;
	editor->searching = false;
	if (editor->drag == EDITOR_SELECTING)
		editor->drag = EDITOR_IDLE;
	if (editor->paste_query)
		editor->paste_tab = nullptr;
	poor_loop_timer_disarm(&editor->counting);
	poor_loop_timer_disarm(&editor->prompting);
	changed(editor);
}

static void activate(struct editor *editor, size_t index)
{
	editor->active = index;
	editor->drag = EDITOR_IDLE;
	if (!searching_in(editor, editor_tab(editor)))
		stop_searching(editor);
	editor_layout(editor);
	retitle(editor);
	highlight(editor);
	changed(editor);
}

/* The memory of a closed tab is free once its file is */
static void tab_release(struct editor_tab *tab)
{
	if (tab->closing && !tab->file.busy)
		tab->used = false;
}

static int file_identified(struct file *file);
static void file_done(struct file *file, int error);

static const struct file_listener file_listener = {
	.identified = file_identified,
	.done = file_done,
};

static struct editor_tab *tab_new(struct editor *editor, const char *path)
{
	struct editor_tab *tab = nullptr;
	size_t index = editor->tab_count ? editor->active + 1 : 0;

	foreach_array_ref(editor->tabs, slot) {
		if (!slot->used) {
			tab = slot;
			break;
		}
	}
	if (!tab || editor->tab_count == EDITOR_TABS) {
		status(editor, "No room for another tab");
		return nullptr;
	}
	auto memory = &editor->memory->documents[array_ref_index(editor->tabs, tab)];

	tab->used = true;
	tab->editor = editor;
	document_init(&tab->document, memory->bytes, memory->lines, memory->states, memory->edits, memory->history);
	tab->document.code = lsp_language(path) != nullptr;
	file_init(&tab->file, editor->loop, &file_listener, tab->path);
	tab->tokens = &memory->tokens;
	tab->token_count = 0;
	tab->jump_offset = SIZE_MAX;
	tab->digits = GUTTER_DIGITS;
	tab->lsp_version = tab->tokens_version = 0;
	tab->loading = tab->queued = tab->closing = tab->jump = tab->close_saved = tab->indexing = false;
	snprintf_array(tab->path, "%s", path);
	memmove(arrview(index + 1, editor->tab_count - index, editor->order), arrview(index, editor->tab_count - index, editor->order),
		ARRAY_SIZE_BYTES(arrview(index, editor->tab_count - index, editor->order)));
	editor->order[index] = tab;
	editor->tab_count++;
	activate(editor, index);
	return tab;
}

static void tab_close(struct editor_tab *tab)
{
	struct editor *editor = tab->editor;
	size_t index = index_of(editor, tab);

	auto after = arrview(index + 1, editor->tab_count - index - 1, editor->order);
	memmove(arrview(index, ARRAY_SIZE(after), editor->order), after, ARRAY_SIZE_BYTES(after));
	editor->tab_count--;
	if (editor->active > index || (editor->active == editor->tab_count && editor->active))
		editor->active--;
	if (tab->lsp_version)
		lsp_close(&editor->lsp, tab->path);
	tab->closing = true;
	tab->queued = false;
	if (editor->paste_tab == tab)
		editor->paste_tab = nullptr;
	if (editor->request && editor->request_tab == tab)
		editor->request = 0;
	if (editor->highlight_tab == tab)
		editor->highlight_tab = nullptr;
	tab->indexing = false;
	file_cancel(&tab->file);
	tab_release(tab);
	activate(editor, editor->active);
}

/* The line of the caret shows in the upper third. An offset from before edits may be inside a character. */
static void reveal(struct editor_tab *tab, size_t offset)
{
	const struct text *text = &tab->document.text;
	struct view *view = &tab->document.view;
	size_t at = text_boundary(text, offset), line = text_line_of(text, at), rows = view_rows(view);

	document_place(&tab->document, at, false);
	view->top = line > rows / 3 ? line - rows / 3 : 0;
	view_reveal(view, at);
}

static void loaded(struct editor_tab *tab)
{
	struct editor *editor = tab->editor;
	struct document *document = &tab->document;

	tab->loading = false;
	if (document->read_only || tab->file.done == document->text.capacity) {
		document->read_only = true;
		status(editor, "%s is too big to edit", path_name(tab->path));
	}
	editor_layout(editor);
	if (tab->jump)
		reveal(tab, lsp_offset(&editor->lsp, &document->text, tab->jump_line, tab->jump_character));
	else if (tab->jump_offset != SIZE_MAX)
		reveal(tab, tab->jump_offset < document->text.size ? tab->jump_offset : document->text.size);
	tab->jump = false;
	tab->jump_offset = SIZE_MAX;
	changed(editor);
	poor_loop_timer_arm(editor->loop, &editor->highlight, 0);
}

static bool indexing(const struct editor_tab *tab)
{
	return tab->indexing || (!tab->loading && text_remeasuring(&tab->document.text));
}

static struct editor_tab *indexing_tab(struct editor *editor)
{
	struct editor_tab *shown = editor_tab(editor);

	if (shown && indexing(shown))
		return shown;
	foreach_array_ref(editor->tabs, tab)
		if (tab->used && indexing(tab))
			return tab;
	return nullptr;
}

static void on_index(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct editor *editor = container_of(timer, struct editor, indexing);
	struct editor_tab *tab = indexing_tab(editor);

	if (!tab)
		return;
	if (!tab->indexing) {
		if (text_remeasure(&tab->document.text, EDITOR_INDEX_BYTES) && view_scroll(&tab->document.view, 0, 0))
			changed(editor);
	} else if (document_index(&tab->document, EDITOR_INDEX_BYTES)) {
		tab->indexing = false;
		loaded(tab);
	}
	if (indexing_tab(editor))
		poor_loop_timer_arm(loop, timer, 0);
}

static void index_file(struct editor_tab *tab)
{
	document_begin(&tab->document, tab->file.done);
	tab->indexing = true;
	poor_loop_timer_arm(tab->editor->loop, &tab->editor->indexing, 0);
}

static struct editor_tab *twin(const struct editor *editor, const struct editor_tab *tab)
{
	foreach_array_const_ref(arrview_first(editor->tab_count, editor->order), other)
		if (*other != tab && (*other)->file.identified && (*other)->file.inode == tab->file.inode &&
		    (*other)->file.device == tab->file.device)
			return *other;
	return nullptr;
}

/* A tab doesn't read the file of another tab, or save over it */
static int file_identified(struct file *file)
{
	struct editor_tab *tab = container_of(file, struct editor_tab, file);

	return twin(tab->editor, tab) ? EEXIST : 0;
}

/* A file that another tab has under another path goes to that tab, with the jump to it if it still shows */
static void give_way(struct editor_tab *tab)
{
	struct editor *editor = tab->editor;
	struct editor_tab *other = twin(editor, tab);
	bool shown = editor_tab(editor) == tab;

	if (shown && other->loading && (tab->jump || tab->jump_offset != SIZE_MAX)) {
		other->jump = tab->jump;
		other->jump_offset = tab->jump_offset;
		other->jump_line = tab->jump_line;
		other->jump_character = tab->jump_character;
	} else if (shown && !other->loading && tab->jump) {
		reveal(other, lsp_offset(&editor->lsp, &other->document.text, tab->jump_line, tab->jump_character));
	} else if (shown && !other->loading && tab->jump_offset != SIZE_MAX) {
		reveal(other, tab->jump_offset < other->document.text.size ? tab->jump_offset : other->document.text.size);
	}
	tab_close(tab);
	if (shown)
		activate(editor, index_of(editor, other));
}

static void tab_save(struct editor_tab *tab);

static bool saving(const struct editor *editor)
{
	if (editor->saving)
		return true;
	foreach_array_const_ref(editor->tabs, tab)
		if (tab->used && tab->queued)
			return true;
	return false;
}

/*
 * A failed save keeps its tab, and the editor, open. The server hears about a save after the changes that it saved,
 * with the bytes of the file, which stay until the next save. Without room for that, it doesn't.
 */
static void saved(struct editor_tab *tab, int error)
{
	struct editor *editor = tab->editor;

	editor->saving = nullptr;
	history_saved(&tab->document.history, !error);
	if (error == EEXIST) {
		status(editor, "%s isn't saved, another tab has its file", path_name(tab->path));
		tab->close_saved = editor->quitting = false;
		editor->confirm = EDITOR_NOTHING;
	} else if (error) {
		status(editor, "%s: %s", path_name(tab->path), strerror(error));
		tab->close_saved = editor->quitting = false;
		editor->confirm = EDITOR_NOTHING;
	} else {
		status(editor, "%s is saved", path_name(tab->path));
		if (serving(editor) && tab->lsp_version && !tab->closing && lsp_sync(editor, tab))
			lsp_save(&editor->lsp, tab->path, arrview_first(tab->file.size, editor->memory->save));
	}
	if (tab->close_saved && !tab->closing)
		tab_close(tab);
	foreach_array_ref(editor->tabs, next) {
		if (next->used && next->queued && !next->closing) {
			tab_save(next);
			break;
		}
	}
	if (editor->quitting && !saving(editor))
		editor->listener->quit(editor);
}

static void file_done(struct file *file, int error)
{
	struct editor_tab *tab = container_of(file, struct editor_tab, file);

	if (file->writing) {
		saved(tab, error);
	} else if (tab->closing) {
		tab_release(tab);
	} else if (error == EEXIST) {
		give_way(tab);
	} else if (error) {
		status(tab->editor, "%s: %s", path_name(tab->path), strerror(error));
		tab_close(tab);
	} else {
		index_file(tab);
	}
}

/*
 * A copy of the document is written over the file, so that editing can go on meanwhile. One save goes at a time, the
 * others wait for it.
 */
static void tab_save(struct editor_tab *tab)
{
	struct editor *editor = tab->editor;
	const struct text *text = &tab->document.text;
	auto bytes = arrview_first(text->size, editor->memory->save);

	if (tab->document.read_only) {
		status(editor, "%s is read only", path_name(tab->path));
		return;
	}
	if (editor->saving) {
		tab->queued = true;
		return;
	}
	tab->queued = false;
	editor->saving = tab;
	copy_array(bytes, arrview_first(text->size, array_ptr(text->bytes, text->capacity)));
	history_save(&tab->document.history);
	file_write(&tab->file, bytes);
	changed(editor);
}

/* A file that isn't there yet is created when it is saved */
static struct editor_tab *tab_open(struct editor *editor, const char *path)
{
	struct editor_tab *tab = tab_find(editor, path);

	if (tab) {
		activate(editor, index_of(editor, tab));
		return tab;
	}
	if (!(tab = tab_new(editor, path)))
		return nullptr;
	tab->loading = true;
	file_read(&tab->file, array_ptr(tab->document.text.bytes, tab->document.text.capacity));
	return tab;
}

void editor_open(struct editor *editor, const char *path)
{
	tab_open(editor, path);
}

/* The offset goes to a file that is read, the position of the server too */
static void go(struct editor *editor, const char *path, size_t offset, bool lsp, size_t line, size_t character)
{
	struct editor_tab *tab;

	stop_searching(editor);
	if (!(tab = tab_open(editor, path)))
		return;
	if (tab->loading) {
		tab->jump = lsp;
		tab->jump_offset = offset;
		tab->jump_line = line;
		tab->jump_character = character;
		return;
	}
	if (lsp)
		offset = lsp_offset(&editor->lsp, &tab->document.text, line, character);
	reveal(tab, offset < tab->document.text.size ? offset : tab->document.text.size);
	changed(editor);
}

/* Without a tab, a jump leaves nothing to go back to */
static void jump_push(struct editor *editor)
{
	struct editor_tab *tab = editor_tab(editor);

	if (tab)
		jumps_push(&editor->jumps, tab->path, tab->document.view.caret);
}

static void jump_back(struct editor *editor)
{
	struct editor_tab *tab = editor_tab(editor);
	const struct jump *to = jumps_back(&editor->jumps, tab ? tab->path : nullptr, tab ? tab->document.view.caret : 0);

	if (to)
		go(editor, to->path, to->offset, false, 0, 0);
}

static void jump_forward(struct editor *editor)
{
	const struct jump *to = jumps_forward(&editor->jumps);

	if (to)
		go(editor, to->path, to->offset, false, 0, 0);
}

static void locate(struct editor *editor, bool declaration)
{
	struct editor_tab *tab = editor_tab(editor), *stale;

	if (!tab || tab->loading || !lsp_language(tab->path))
		return;
	if (editor->lsp.state != LSP_READY) {
		status(editor, "%s is %s", editor->lsp.argv ? editor->lsp.argv[0] : "The language server",
		       editor->lsp.state == LSP_STARTING ? "starting" : "not running");
		return;
	}
	stale = lsp_sync_all(editor);
	editor->request = stale ? 0 : lsp_locate(&editor->lsp, tab->path, &tab->document.text, tab->document.view.caret,
						 declaration);
	if (!editor->request) {
		status(editor, "%s can't take %s now", editor->lsp.argv[0], path_name((stale ? stale : tab)->path));
		return;
	}
	editor->request_tab = tab;
	editor->request_version = tab->document.version;
	editor->request_caret = tab->document.view.caret;
}

static void lsp_location(struct lsp *lsp, uint64_t id, const char *path, size_t line, size_t character)
{
	struct editor *editor = container_of(lsp, struct editor, lsp);

	struct editor_tab *tab = editor_tab(editor);

	if (!id || id != editor->request)
		return;
	editor->request = 0;
	if (tab != editor->request_tab || tab->document.version != editor->request_version ||
	    tab->document.view.caret != editor->request_caret)
		return;
	if (!path) {
		status(editor, "Nothing to go to");
		return;
	}
	jump_push(editor);
	go(editor, path, 0, true, line, character);
}

/* Documents go to a server that starts, and come back from one that ends */
static void lsp_state(struct lsp *lsp)
{
	struct editor *editor = container_of(lsp, struct editor, lsp);

	if (lsp->state == LSP_READY)
		lsp_sync_all(editor);
	else if (lsp->state != LSP_STARTING)
		foreach_array_ref(arrview_first(editor->tab_count, editor->order), tab)
			(*tab)->lsp_version = 0;
	if (lsp->state != LSP_READY) {
		editor->request = 0;
		editor->highlight_tab = nullptr;
	}
	highlight(editor);
	changed(editor);
}

/* Tokens of an older version are dropped and asked for again. After a failed request, the next edit asks again. */
static void lsp_tokens(struct lsp *lsp, bool found)
{
	struct editor *editor = container_of(lsp, struct editor, lsp);
	struct editor_tab *tab = editor->highlight_tab;
	struct lsp_token token;

	editor->highlight_tab = nullptr;
	if (tab && tab->document.version == editor->highlight_version) {
		for (tab->token_count = 0; found && tab->token_count < ARRAY_SIZE(*tab->tokens) &&
					   lsp_token(lsp, &tab->document.text, &token);
		     tab->token_count++)
			(*tab->tokens)[tab->token_count] = (struct editor_token){
				.line = token.line,
				.column = token.column,
				.length = token.length < UINT16_MAX ? token.length : UINT16_MAX,
				.class = token.class,
			};
		tab->tokens_version = tab->document.version;
		text_mark(&tab->document.text, EDITOR_MARK_TOKENS);
		changed(editor);
	}
	highlight(editor);
}

/* All tokens are asked for again, and an answer that may be older than the change goes nowhere */
static void lsp_refresh(struct lsp *lsp)
{
	struct editor *editor = container_of(lsp, struct editor, lsp);

	foreach_array_ref(arrview_first(editor->tab_count, editor->order), tab)
		(*tab)->tokens_version = 0;
	editor->highlight_tab = nullptr;
	highlight(editor);
}

static void lsp_written(struct lsp *lsp)
{
	highlight(container_of(lsp, struct editor, lsp));
}

static const struct lsp_listener lsp_listener = {
	.state = lsp_state,
	.location = lsp_location,
	.tokens = lsp_tokens,
	.refresh = lsp_refresh,
	.written = lsp_written,
};

static void tree_changed(struct tree *tree)
{
	changed(container_of(tree, struct editor, tree));
}

static const struct tree_listener tree_listener = {
	.changed = tree_changed,
};

/* The paste goes to the selection that it was asked for, or to the query, see editor_paste() */
static void ask_paste(struct editor *editor, bool primary)
{
	struct editor_tab *tab = editor_tab(editor);

	if (!tab || tab->loading || editor->paste_tab)
		return;
	editor->paste_tab = tab;
	editor->paste_query = editor->searching;
	editor->paste_version = tab->document.version;
	editor->paste_anchor = tab->document.view.anchor;
	editor->paste_caret = tab->document.view.caret;
	if (!editor->listener->paste(editor, primary))
		editor->paste_tab = nullptr;
}

/* Returns whether the document that keys edit has a selection and it was copied */
static bool copy(struct editor *editor, bool primary)
{
	const struct document *document = focus(editor);
	size_t start, end;

	if (!document || !view_selection(&document->view, &start, &end))
		return false;
	return editor->listener->copy(editor, primary, end - start,
				      arrview(start, end - start, array_ptr(document->text.bytes, document->text.size)));
}

/* Matches are counted in slices between loop passes, so that a big document doesn't stop the editor */
static void count_matches(struct editor *editor)
{
	if (!find_counted(&editor->find))
		poor_loop_timer_arm(editor->loop, &editor->counting, 0);
}

static void on_count(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct editor *editor = container_of(timer, struct editor, counting);

	if (find_count(&editor->find, EDITOR_COUNT_BYTES))
		changed(editor);
	else
		poor_loop_timer_arm(loop, timer, 0);
}

/* The gutter grows with the lines, and a query that changed is looked for again */
static void update(struct editor *editor, bool done)
{
	if (!done)
		return;
	if (editor->searching && find_update(&editor->find))
		count_matches(editor);
	if (indexing_tab(editor))
		poor_loop_timer_arm(editor->loop, &editor->indexing, 0);
	editor_layout(editor);
	highlight(editor);
	changed(editor);
}

static size_t unsaved_tabs(const struct editor *editor)
{
	size_t count = 0;

	foreach_array_const_ref(arrview_first(editor->tab_count, editor->order), tab)
		count += history_unsaved(&(*tab)->document.history);
	return count;
}

void editor_quit(struct editor *editor)
{
	size_t unsaved = unsaved_tabs(editor);

	if (unsaved && editor->confirm != EDITOR_QUITTING) {
		editor->confirm = EDITOR_QUITTING;
		status(editor, "%zu %s changes that aren't saved: quit again to lose them", unsaved,
		       unsaved == 1 ? "document has" : "documents have");
		return;
	}
	if (saving(editor)) {
		editor->quitting = true;
		status(editor, "Quitting once the files are saved");
		return;
	}
	editor->listener->quit(editor);
}

static void close_tab(struct editor *editor, struct editor_tab *tab, enum editor_confirm confirm)
{
	if (history_unsaved(&tab->document.history) && (confirm != EDITOR_CLOSING || editor->confirm_tab != tab)) {
		editor->confirm = EDITOR_CLOSING;
		editor->confirm_tab = tab;
		status(editor, "%s has changes that aren't saved: close it again to lose them", path_name(tab->path));
		return;
	}
	if (editor->saving == tab || tab->queued) {
		tab->close_saved = true;
		return;
	}
	tab_close(tab);
}

/* Input and pastes, which may edit, take back the status and what waits for a definition, a confirmation or saves */
static void interrupt(struct editor *editor)
{
	editor->request = 0;
	editor->confirm = EDITOR_NOTHING;
	editor->quitting = false;
	foreach_array_ref(editor->tabs, tab)
		tab->close_saved = false;
	if (editor->status[0]) {
		editor->status[0] = 0;
		poor_loop_timer_disarm(&editor->prompting);
		changed(editor);
	}
}

/* Going to another tab with the wheel is input too */
static void cycle(struct editor *editor, bool back)
{
	if (editor->tab_count < 2)
		return;
	interrupt(editor);
	activate(editor, (editor->active + (back ? editor->tab_count - 1 : 1)) % editor->tab_count);
}

/* A modifier alone is no key, so Ctrl can be let go between two Ctrl+W. The ranges are those of libxkbcommon. */
static bool modifier(xkb_keysym_t sym)
{
	return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) ||
	       sym == XKB_KEY_Mode_switch || sym == XKB_KEY_Num_Lock;
}

static bool printable(size_t size, const char (*text)[size])
{
	foreach_array_const_ref(text, c)
		if ((unsigned char)*c < 0x20 || *c == 0x7f)
			return false;
	return size;
}

static size_t tree_visible(const struct editor *editor)
{
	return editor->tree_rect.height > editor->padding ? (editor->tree_rect.height - editor->padding) / editor->cell_height : 0;
}

static void tree_open(struct editor *editor, uint32_t node)
{
	char path[PATH_MAX];

	if (editor->tree.nodes[node].directory) {
		tree_toggle(&editor->tree, node);
		changed(editor);
	} else if (tree_path(&editor->tree, node, path) && tab_open(editor, path)) {
		editor->browsing = false;
	}
}

static void browse(struct editor *editor)
{
	struct editor_tab *tab = editor_tab(editor);
	struct tree *tree = &editor->tree;
	uint32_t node = tab ? tree_nearest(tree, tab->path) : 0;

	stop_searching(editor);
	editor->browsing = editor->show_tree = true;
	editor_layout(editor);
	tree_go(tree, node ? node : tree_cursor(tree), tree_visible(editor));
	changed(editor);
}

static void stop_browsing(struct editor *editor)
{
	if (!editor->browsing)
		return;
	editor->browsing = false;
	changed(editor);
}

static bool tree_key(struct editor *editor, xkb_keysym_t sym, size_t size, const char (*text)[size])
{
	struct tree *tree = &editor->tree;
	uint32_t node = tree_cursor(tree);
	size_t visible = tree_visible(editor);
	ptrdiff_t page = visible > 1 ? visible - 1 : 1;

	if (sym == XKB_KEY_Escape) {
		stop_browsing(editor);
		return false;
	}
	if (node == TREE_NONE)
		return false;
	tree_go(tree, node, visible);
	changed(editor);
	switch (sym) {
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		tree_move(tree, sym == XKB_KEY_Up ? -1 : 1, visible);
		return true;
	case XKB_KEY_Page_Up:
	case XKB_KEY_Page_Down:
		tree_move(tree, sym == XKB_KEY_Page_Up ? -page : page, visible);
		return true;
	case XKB_KEY_Home:
	case XKB_KEY_End:
		tree_move(tree, sym == XKB_KEY_Home ? -PTRDIFF_MAX : PTRDIFF_MAX, visible);
		return false;
	case XKB_KEY_Right:
		if (tree->nodes[node].directory && !tree->nodes[node].expanded)
			tree_open(editor, node);
		else if (tree->nodes[node].expanded && tree->nodes[node].count)
			tree_go(tree, tree->nodes[node].first, visible);
		return true;
	case XKB_KEY_Left:
		if (tree->nodes[node].expanded)
			tree_open(editor, node);
		else if (tree->nodes[node].parent)
			tree_go(tree, tree->nodes[node].parent, visible);
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		tree_open(editor, node);
		return false;
	}
	if (printable(size, text))
		tree_seek_impl(tree, size, text, visible);
	return false;
}

static const struct {
	xkb_keysym_t sym;
	enum document_motion motion;
} MOTIONS[] = {
	{ XKB_KEY_Left, DOCUMENT_LEFT }, { XKB_KEY_Right, DOCUMENT_RIGHT }, { XKB_KEY_Up, DOCUMENT_UP },
	{ XKB_KEY_Down, DOCUMENT_DOWN }, { XKB_KEY_Page_Up, DOCUMENT_PAGE_UP }, { XKB_KEY_Page_Down, DOCUMENT_PAGE_DOWN },
	{ XKB_KEY_Home, DOCUMENT_HOME }, { XKB_KEY_End, DOCUMENT_END },
};

static bool motion_of(xkb_keysym_t sym, enum document_motion *motion)
{
	foreach_array_const_ref(MOTIONS, entry) {
		if (entry->sym == sym) {
			*motion = entry->motion;
			return true;
		}
	}
	return false;
}

static void start_searching(struct editor *editor, struct editor_tab *tab)
{
	if (editor->searching) {
		document_select_all(&editor->find.query);
		changed(editor);
		return;
	}
	editor->browsing = false;
	editor->drag = EDITOR_IDLE;
	editor->searching = true;
	find_start(&editor->find, &tab->document);
	count_matches(editor);
	changed(editor);
}

static void search_again(struct editor *editor, struct editor_tab *tab, bool back)
{
	const struct search *search = &editor->find.search;

	if (!search->size) {
		status(editor, "Nothing to find");
		return;
	}
	if (find_again(&editor->find, &tab->document, back) == SIZE_MAX) {
		status(editor, "\"%.*s\" isn't in %s", (int)search->size, search->query, path_name(tab->path));
		return;
	}
	changed(editor);
}

/* Commands with Control, which don't repeat unless they edit. The document scrolls while the query is edited. */
static bool control_key(struct editor *editor, struct editor_tab *tab, xkb_keysym_t sym, bool shift, enum editor_confirm confirm)
{
	struct document *document = editor_browsing(editor) ? nullptr : focus(editor);

	switch (sym) {
	case XKB_KEY_q:
		editor->confirm = confirm;
		editor_quit(editor);
		return false;
	case XKB_KEY_b:
		editor->show_tree = !editor->show_tree;
		editor->browsing = false;
		update(editor, true);
		return false;
	case XKB_KEY_e:
		if (editor_browsing(editor))
			stop_browsing(editor);
		else if (editor->started)
			browse(editor);
		return false;
	case XKB_KEY_s:
		if (tab && !tab->loading)
			tab_save(tab);
		return false;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
	case XKB_KEY_Page_Down:
	case XKB_KEY_Page_Up:
		cycle(editor, sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_Page_Up || shift);
		return false;
	case XKB_KEY_w:
		if (tab)
			close_tab(editor, tab, confirm);
		return false;
	case XKB_KEY_f:
		if (tab && !tab->loading)
			start_searching(editor, tab);
		return false;
	}
	if (!document)
		return false;
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (!editor->searching)
			locate(editor, shift);
		return false;
	case XKB_KEY_z:
		update(editor, shift ? document_redo(document) : document_undo(document));
		return true;
	case XKB_KEY_y:
		update(editor, document_redo(document));
		return true;
	case XKB_KEY_a:
		update(editor, document_select_all(document));
		copy(editor, true);
		return false;
	case XKB_KEY_c:
	case XKB_KEY_Insert:
	case XKB_KEY_x:
		if (copy(editor, false)) {
			if (sym == XKB_KEY_x)
				update(editor, document_erase(document, false, false));
		} else if (document->view.anchor != document->view.caret) {
			status(editor, "The clipboard can't take the selection now");
		}
		return false;
	case XKB_KEY_v:
		ask_paste(editor, false);
		return false;
	case XKB_KEY_Left:
	case XKB_KEY_Right:
		update(editor, document_move(document, sym == XKB_KEY_Left ? DOCUMENT_WORD_LEFT : DOCUMENT_WORD_RIGHT, shift));
		if (shift)
			copy(editor, true);
		return true;
	case XKB_KEY_Home:
	case XKB_KEY_End:
		update(editor, document_move(document, sym == XKB_KEY_Home ? DOCUMENT_TOP : DOCUMENT_BOTTOM, shift));
		if (shift)
			copy(editor, true);
		return false;
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		update(editor, view_scroll(&tab->document.view, sym == XKB_KEY_Up ? -1 : 1, 0));
		return true;
	case XKB_KEY_BackSpace:
	case XKB_KEY_Delete:
		update(editor, document_erase(document, sym == XKB_KEY_Delete, true));
		return true;
	}
	return false;
}

bool editor_key(struct editor *editor, xkb_keysym_t sym, uint32_t modifiers, size_t size, const char (*text)[size])
{
	struct editor_tab *tab = editor_tab(editor);
	struct document *document = focus(editor);
	bool shift = modifiers & SEAT_SHIFT;
	enum editor_confirm confirm = editor->confirm;
	enum document_motion motion;

	if (modifier(sym))
		return false;
	interrupt(editor);
	if (modifiers & SEAT_CONTROL)
		return control_key(editor, tab, sym, shift, confirm);
	if (modifiers & SEAT_ALT) {
		if (sym == XKB_KEY_Left)
			jump_back(editor);
		else if (sym == XKB_KEY_Right)
			jump_forward(editor);
		return false;
	}
	if (editor_browsing(editor))
		return tree_key(editor, sym, size, text);
	if (editor->searching) {
		switch (sym) {
		case XKB_KEY_Escape:
			stop_searching(editor);
			return false;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
		case XKB_KEY_F3:
			if (find_step(&editor->find, shift)) {
				count_matches(editor);
				changed(editor);
			}
			return true;
		case XKB_KEY_Tab:
		case XKB_KEY_ISO_Left_Tab:
			return false;
		}
	}
	if (!document)
		return false;
	if (motion_of(sym, &motion)) {
		update(editor, document_move(document, motion, shift));
		if (shift)
			copy(editor, true);
		return true;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		update(editor, document_place(document, document->view.caret, false));
		return false;
	case XKB_KEY_BackSpace:
	case XKB_KEY_Delete:
		update(editor, document_erase(document, sym == XKB_KEY_Delete, false));
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		update(editor, document_newline(document));
		return true;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
		update(editor, document_indent(document, sym == XKB_KEY_ISO_Left_Tab || shift));
		return true;
	case XKB_KEY_Insert:
		if (shift)
			ask_paste(editor, false);
		return false;
	case XKB_KEY_F3:
		search_again(editor, tab, shift);
		return true;
	}
	if (!printable(size, text))
		return false;
	if (document_insert(document, text, true))
		update(editor, true);
	else if (!document->read_only && !editor->searching)
		status(editor, "No room in %s", path_name(tab->path));
	return true;
}

static bool inside(const struct editor_rect *rect, double x, double y)
{
	return x >= rect->x && x < rect->x + rect->width && y >= rect->y && y < rect->y + rect->height;
}

static bool on_splitter(const struct editor *editor, double x, double y)
{
	int32_t grip = GRIP * editor->line;

	return editor->show_tree && y < editor->status_rect.y && x >= editor->tree_rect.width - grip &&
	       x < editor->tree_rect.width + editor->line + grip;
}

static uint32_t tree_hit(const struct editor *editor, double y)
{
	double row = (y - editor->tree_rect.y - editor->padding) / editor->cell_height;

	return row < 0 ? TREE_NONE : tree_row(&editor->tree, editor->tree.top + (size_t)row);
}

/* Without scrolling, a second click of a double click hits the same row */
static void tree_click(struct editor *editor, double y)
{
	uint32_t node = tree_hit(editor, y);

	if (node == TREE_NONE)
		return;
	tree_place(&editor->tree, node);
	editor->browsing = true;
	tree_open(editor, node);
}

/* The mark at the end of a tab closes it */
static void tabs_click(struct editor *editor, double x, uint32_t button, enum editor_confirm confirm)
{
	int32_t start, end;

	foreach_array_index(arrview_first(editor->tab_count, editor->order), i) {
		if (!editor_tab_extent(editor, i, &start, &end) || x < start || x >= end)
			continue;
		if (button == BTN_MIDDLE || (button == BTN_LEFT && x >= end - 2 * editor->cell_width))
			close_tab(editor, editor->order[i], confirm);
		else if (button == BTN_LEFT)
			activate(editor, i);
		return;
	}
}

/* Two clicks in a row select a word, three a line, of the document that keys edit, which shows in 'rect' */
static void click(struct editor *editor, const struct editor_rect *rect, double x, double y, uint32_t button, uint32_t time,
		  uint32_t modifiers)
{
	struct document *document = focus(editor);
	size_t offset;
	bool near;

	if (!document)
		return;
	offset = view_hit(&document->view, x - rect->x, y - rect->y);
	if (button == BTN_MIDDLE) {
		update(editor, document_place(document, offset, false));
		ask_paste(editor, true);
		return;
	}
	if (button != BTN_LEFT)
		return;
	near = document == editor->click_document && fabs(x - editor->click_x) <= editor->cell_width / 2.0 &&
	       fabs(y - editor->click_y) <= editor->cell_width / 2.0;
	editor->clicks = near && time - editor->click_time < EDITOR_CLICK_MS ? editor->clicks % 3 + 1 : 1;
	editor->click_time = time;
	editor->click_document = document;
	editor->click_x = x;
	editor->click_y = y;
	if ((modifiers & SEAT_CONTROL) && !editor->searching) {
		update(editor, document_place(document, offset, false));
		locate(editor, modifiers & SEAT_SHIFT);
		return;
	}
	if (editor->clicks == 2)
		update(editor, document_select_word(document, offset));
	else if (editor->clicks == 3)
		update(editor, document_select_line(document, offset));
	else
		update(editor, document_place(document, offset, modifiers & SEAT_SHIFT));
	editor->drag = EDITOR_SELECTING;
}

void editor_button(struct editor *editor, double x, double y, uint32_t button, bool pressed, uint32_t time, uint32_t modifiers)
{
	enum editor_confirm confirm = editor->confirm;

	if (!pressed) {
		if (editor->drag == EDITOR_SELECTING)
			copy(editor, true);
		editor->drag = EDITOR_IDLE;
		return;
	}
	interrupt(editor);
	if (!inside(&editor->status_rect, x, y))
		stop_searching(editor);
	if (button == BTN_SIDE || button == BTN_BACK) {
		jump_back(editor);
	} else if (button == BTN_EXTRA || button == BTN_FORWARD) {
		jump_forward(editor);
	} else if (button == BTN_LEFT && on_splitter(editor, x, y)) {
		editor->drag = EDITOR_RESIZING;
	} else if (inside(&editor->tree_rect, x, y)) {
		if (button == BTN_LEFT && editor->started)
			tree_click(editor, y);
	} else if (inside(&editor->tabs_rect, x, y)) {
		stop_browsing(editor);
		tabs_click(editor, x, button, confirm);
	} else if (inside(&editor->gutter_rect, x, y) || inside(&editor->text_rect, x, y)) {
		stop_browsing(editor);
		click(editor, &editor->text_rect, x, y, button, time, modifiers);
	} else if (editor->searching && inside(&editor->query_rect, x, y)) {
		click(editor, &editor->query_rect, x, y, button, time, modifiers);
	}
}

void editor_motion(struct editor *editor, double x, double y)
{
	struct document *document = focus(editor);
	const struct editor_rect *rect = editor->searching ? &editor->query_rect : &editor->text_rect;
	size_t columns;

	if (editor->drag == EDITOR_SELECTING && document) {
		update(editor, view_drag(&document->view, x - rect->x, y - rect->y));
	} else if (editor->drag == EDITOR_RESIZING) {
		columns = tree_columns(editor, x > 2 * editor->padding ? (x - 2 * editor->padding) / editor->cell_width : 0);
		if (columns != editor->tree_columns) {
			editor->tree_columns = columns;
			update(editor, true);
		}
	}
	if (editor->drag == EDITOR_RESIZING || (editor->drag == EDITOR_IDLE && on_splitter(editor, x, y)))
		editor->listener->cursor(editor, POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
	else if (editor->drag == EDITOR_SELECTING || inside(&editor->text_rect, x, y) || inside(&editor->gutter_rect, x, y) ||
		 (editor->searching && inside(&editor->query_rect, x, y)))
		editor->listener->cursor(editor, POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
	else
		editor->listener->cursor(editor, POOR_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

void editor_leave(struct editor *editor)
{
	editor->drag = EDITOR_IDLE;
}

void editor_scroll(struct editor *editor, double x, double y, ptrdiff_t rows, ptrdiff_t columns)
{
	struct editor_tab *tab = editor_tab(editor);

	if (inside(&editor->tree_rect, x, y)) {
		if (!editor->started)
			return;
		tree_scroll(&editor->tree, rows, tree_visible(editor));
		changed(editor);
	} else if (inside(&editor->tabs_rect, x, y)) {
		if (rows)
			cycle(editor, rows < 0);
	} else if (tab && !tab->loading) {
		update(editor, view_scroll(&tab->document.view, rows, columns));
	}
}

/* A paste comes later from other clients: it goes nowhere if its document changed meanwhile */
void editor_paste_impl(struct editor *editor, size_t size, const char (*bytes)[size])
{
	struct editor_tab *tab = editor->paste_tab;
	struct document *document;
	size_t anchor, caret;

	editor->paste_tab = nullptr;
	if (!tab || !size)
		return;
	if (editor->paste_query) {
		if (searching_in(editor, tab)) {
			interrupt(editor);
			update(editor, find_insert_impl(&editor->find, size, bytes));
		}
		return;
	}
	document = &tab->document;
	if (document->version != editor->paste_version) {
		status(editor, "Nothing is pasted, %s changed meanwhile", path_name(tab->path));
		return;
	}
	anchor = document->view.anchor;
	caret = document->view.caret;
	view_select(&document->view, editor->paste_anchor, editor->paste_caret);
	if (document_insert_impl(document, size, bytes, false)) {
		interrupt(editor);
		if (searching_in(editor, tab))
			stop_searching(editor);
		update(editor, true);
		return;
	}
	view_select(&document->view, anchor, caret);
	if (!document->read_only)
		status(editor, "No room for %zu bytes in %s", size, path_name(tab->path));
}

void editor_resize(struct editor *editor, int32_t width, int32_t height, int32_t cell_width, int32_t cell_height,
		   int32_t padding, uint32_t scale)
{
	editor->width = width;
	editor->height = height;
	editor->cell_width = cell_width;
	editor->cell_height = cell_height;
	editor->padding = padding;
	editor->scale = scale;
	editor->line = scale >= 240 ? scale / 120 : 1;
	editor_layout(editor);
	changed(editor);
}

void editor_focus(struct editor *editor, bool focused)
{
	if (focused == editor->focused)
		return;
	editor->focused = focused;
	changed(editor);
}

void editor_init(struct editor *editor, struct poor_loop *loop, const struct editor_listener *listener, struct editor_memory *memory)
{
	editor->loop = loop;
	editor->listener = listener;
	editor->memory = memory;
	editor->saving = nullptr;
	editor->tab_count = editor->active = editor->tab_first = 0;
	jumps_init(&editor->jumps, memory->jumps);
	editor->request = 0;
	editor->paste_tab = nullptr;
	editor->width = editor->height = 0;
	editor->cell_width = editor->cell_height = 1;
	editor->padding = 0;
	editor->line = 1;
	editor->scale = 120;
	editor->click_time = editor->clicks = 0;
	editor->click_document = nullptr;
	editor->tree_columns = EDITOR_TREE_COLUMNS;
	editor->drag = EDITOR_IDLE;
	editor->confirm = EDITOR_NOTHING;
	editor->focused = editor->browsing = editor->quitting = false;
	editor->show_tree = true;
	editor->status[0] = 0;
	editor->root[0] = 0;
	editor->started = false;
	foreach_array_ref(editor->tabs, tab)
		tab->used = false;
	poor_loop_timer_init(&editor->highlight, on_highlight);
	poor_loop_timer_init(&editor->indexing, on_index);
	poor_loop_timer_init(&editor->counting, on_count);
	poor_loop_timer_init(&editor->prompting, on_prompt);
	find_init(&editor->find, &memory->find);
	editor->searching = editor->paste_query = false;
	editor->highlight_tab = nullptr;
	lsp_init(&editor->lsp, loop, &lsp_listener, &memory->lsp);
	editor_layout(editor);
	retitle(editor);
}

void editor_start(struct editor *editor, const char *root, const char *const *server)
{
	snprintf_array(editor->root, "%s", root);
	tree_init(&editor->tree, editor->loop, root, &tree_listener, &editor->memory->tree);
	editor->started = true;
	retitle(editor);
	changed(editor);
	if (server)
		lsp_start(&editor->lsp, root, server);
}

/* Saves go on */
void editor_stop(struct editor *editor)
{
	if (editor->started)
		tree_stop(&editor->tree);
	poor_loop_timer_disarm(&editor->highlight);
	poor_loop_timer_disarm(&editor->indexing);
	stop_searching(editor);
	editor->request = 0;
	lsp_stop(&editor->lsp);
	foreach_array_ref(editor->tabs, tab) {
		if (!tab->used || !tab->loading)
			continue;
		tab->closing = true;
		tab->indexing = false;
		file_cancel(&tab->file);
	}
}

bool editor_busy(const struct editor *editor)
{
	foreach_array_const_ref(editor->tabs, tab)
		if (tab->used && tab->file.busy)
			return true;
	return (editor->started && tree_busy(&editor->tree)) || lsp_busy(&editor->lsp);
}
