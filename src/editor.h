/* SPDX-License-Identifier: MIT */
#ifndef EDITOR_H
#define EDITOR_H

#include <limits.h>
#include <poor_loop_fd.h>
#include <xkbcommon/xkbcommon.h>

#include "document.h"
#include "file.h"
#include "find.h"
#include "jumps.h"
#include "lsp.h"
#include "tree.h"

constexpr size_t EDITOR_TABS = 16;
constexpr size_t EDITOR_BYTES = 16 << 20;
constexpr size_t EDITOR_LINES = 1 << 20;
constexpr size_t EDITOR_JUMPS = 64;
constexpr size_t EDITOR_STATUS = 256;
constexpr size_t EDITOR_TREE_COLUMNS = 28;
constexpr size_t EDITOR_TREE_MIN = 8;
constexpr uint32_t EDITOR_CLICK_MS = 400;
constexpr size_t EDITOR_TOKENS = 1 << 17;
constexpr uint64_t EDITOR_HIGHLIGHT_NS = 250'000'000;
constexpr uint64_t EDITOR_PROMPT_NS = 2'000'000'000;
constexpr size_t EDITOR_DETAILS = 256;
constexpr char EDITOR_PROMPT[] = "Find: ";
constexpr size_t EDITOR_INDEX_BYTES = 256 << 10;
constexpr size_t EDITOR_COUNT_BYTES = 1 << 20;

enum : size_t {
	EDITOR_MARK_TOKENS,
	EDITOR_MARK_SERVER,
	EDITOR_MARKS,
};

static_assert(EDITOR_MARKS == TEXT_MARKS);

struct editor;

/* A token of the language server: its line when the tokens came, its byte column and length, and its class */
struct editor_token {
	uint32_t line, column;
	uint16_t length;
	enum syntax_class class;
};

/*
 * The document of a file in a tab. The server has 'lsp_version' of the document, none for 0. Once the file is read,
 * the caret goes to 'jump_offset', or to 'jump_line' and 'jump_character' of the server. A tab that is closed while
 * it is saved closes once the save succeeds, unless input comes first. A tab that opens a file of another tab under
 * another path gives way to that tab, and one that would save over it doesn't. The 'tokens' of the server are of
 * 'tokens_version' of the document.
 */
struct editor_tab {
	struct editor *editor;
	struct document document;
	struct file file;
	struct editor_token (*tokens)[EDITOR_TOKENS];
	size_t digits, jump_offset, jump_line, jump_character, token_count;
	uint64_t lsp_version, tokens_version;
	bool used, loading, indexing, queued, closing, jump, close_saved;
	char path[PATH_MAX];
};

/* The memory of the documents of the tabs, of the search, of the copy that is saved, of the jumps, the tree and the server */
struct editor_memory {
	struct {
		char bytes[EDITOR_BYTES];
		uint32_t lines[EDITOR_LINES];
		uint8_t states[EDITOR_LINES];
		struct history_edit edits[HISTORY_EDITS];
		char history[HISTORY_BYTES];
		struct editor_token tokens[EDITOR_TOKENS];
	} documents[EDITOR_TABS];
	struct find_memory find;
	char save[EDITOR_BYTES];
	struct jump jumps[EDITOR_JUMPS];
	struct tree_memory tree;
	struct lsp_memory lsp;
};

struct editor_rect {
	int32_t x, y, width, height;
};

enum editor_drag {
	EDITOR_IDLE,
	EDITOR_SELECTING,
	EDITOR_RESIZING,
};

/* A command that waits for its key once more, since documents would lose their changes */
enum editor_confirm {
	EDITOR_NOTHING,
	EDITOR_CLOSING,
	EDITOR_QUITTING,
};

struct editor_listener {
	/* Something shows differently */
	void (*changed)(struct editor *editor);
	void (*title)(struct editor *editor, const char *title);
	/* Returns whether the clipboard or the primary selection has the bytes */
	bool (*copy)(struct editor *editor, bool primary, size_t size, const char (*bytes)[size]);
	/* The clipboard or the primary selection goes to editor_paste(), unless this returns false */
	bool (*paste)(struct editor *editor, bool primary);
	void (*cursor)(struct editor *editor, uint32_t shape);
	void (*quit)(struct editor *editor);
};

/*
 * Tabs of files, in the order of 'order', the tree of the root directory and the language server of the project.
 * The regions of the window are in buffer pixels, 'line' is how thick lines are. A paste goes to the selection of
 * 'paste_tab' that it was asked for, if the document stays as it was. The editor quits once the files that are saved
 * are, if 'quitting'. A close that waits for its confirmation is of 'confirm_tab'. Clicks in a row near the last one,
 * at 'click_x', 'click_y' of 'click_document', select words and lines. The answer to 'request' goes somewhere if
 * 'request_tab' stays active, at 'request_version' and 'request_caret'. While 'searching', keys and the pastes that
 * they ask for, 'paste_query', edit the query of 'find', which shows in 'query_rect'.
 */
struct editor {
	struct poor_loop *loop;
	const struct editor_listener *listener;
	struct editor_memory *memory;
	struct tree tree;
	struct lsp lsp;
	struct find find;
	struct jumps jumps;
	struct editor_tab *order[EDITOR_TABS];
	struct editor_tab *saving;
	struct poor_loop_timer highlight, indexing, counting, prompting;
	size_t tab_count, active, tab_first;
	uint64_t request, request_version, paste_version, highlight_version;
	struct editor_tab *request_tab, *paste_tab, *confirm_tab, *highlight_tab;
	const struct document *click_document;
	size_t request_caret, paste_anchor, paste_caret;
	struct editor_rect tree_rect, tabs_rect, gutter_rect, text_rect, status_rect, query_rect;
	int32_t width, height, cell_width, cell_height, padding, line;
	uint32_t scale, click_time, clicks;
	double click_x, click_y;
	size_t tree_columns;
	enum editor_drag drag;
	enum editor_confirm confirm;
	bool started, focused, show_tree, browsing, quitting, searching, paste_query;
	char root[PATH_MAX];
	char status[EDITOR_STATUS];
	char title[PATH_MAX];
	struct editor_tab tabs[EDITOR_TABS];
};

void editor_init(struct editor *editor, struct poor_loop *loop, const struct editor_listener *listener, struct editor_memory *memory);
/* The tree of the absolute path 'root', and the language server 'server' of the files under it, if there is one */
void editor_start(struct editor *editor, const char *root, const char *const *server);
/* Open the file at the absolute path 'path', or go to its tab */
void editor_open(struct editor *editor, const char *path);
void editor_resize(struct editor *editor, int32_t width, int32_t height, int32_t cell_width, int32_t cell_height,
		   int32_t padding, uint32_t scale);
/* Where everything goes, after the window or the tabs changed */
void editor_layout(struct editor *editor);
void editor_focus(struct editor *editor, bool focused);
struct editor_tab *editor_tab(const struct editor *editor);
bool editor_browsing(const struct editor *editor);
/* What the status bar shows on the right: the matches, where the caret is and the server. Returns its cells. */
size_t editor_details_impl(const struct editor *editor, size_t size, char (*out)[size]);
#define editor_details(editor, out) editor_details_impl(editor, ARRAY_SIZE(out), &auto_arr(out))
/* Where a tab of 'order' shows in the tab bar, false if it doesn't */
bool editor_tab_extent(const struct editor *editor, size_t index, int32_t *start, int32_t *end);
/* Returns whether the key repeats */
bool editor_key(struct editor *editor, xkb_keysym_t sym, uint32_t modifiers, size_t size, const char (*text)[size]);
void editor_button(struct editor *editor, double x, double y, uint32_t button, bool pressed, uint32_t time, uint32_t modifiers);
void editor_motion(struct editor *editor, double x, double y);
/* The pointer left, which ends a drag without the release of its button */
void editor_leave(struct editor *editor);
void editor_scroll(struct editor *editor, double x, double y, ptrdiff_t rows, ptrdiff_t columns);
void editor_paste_impl(struct editor *editor, size_t size, const char (*bytes)[size]);
#define editor_paste(editor, bytes) editor_paste_impl(editor, ARRAY_SIZE(bytes), &auto_arr(bytes))
/*
 * Quit, if no document would lose its changes, or if this is asked for the second time in a row. Quitting waits for
 * the saves that go on, and doesn't happen if one of them fails or input comes first.
 */
void editor_quit(struct editor *editor);
/* Stop reading and the server, saves go on: wait until editor_busy() is false before the memory goes */
void editor_stop(struct editor *editor);
bool editor_busy(const struct editor *editor);

#endif
