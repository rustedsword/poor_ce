/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poor_stdio.h>
#include <poor_unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "editor.h"
#include "font.h"
#include "logger.h"
#include "path.h"
#include "scene.h"
#include "seat.h"
#include "test-loop.h"
#include "test-runner.h"

static struct editor editor;
static struct editor_memory memory;

/* What the editor asked for */
static struct {
	unsigned changes, quits, copies, pastes;
	bool primary, refused;
	char copied[64];
} events;

static void changed(struct editor *)
{
	events.changes++;
}

static void title(struct editor *, const char *)
{
}

/* The clipboard takes nothing while copies are 'refused' */
static bool copy(struct editor *, bool primary, size_t size, const char (*bytes)[size])
{
	if (events.refused)
		return false;
	events.copies++;
	events.primary = primary;
	snprintf(events.copied, sizeof(events.copied), "%.*s", (int)size, *bytes);
	return true;
}

static bool paste(struct editor *, bool primary)
{
	events.pastes++;
	events.primary = primary;
	return true;
}

static void cursor(struct editor *, uint32_t)
{
}

static void quit(struct editor *)
{
	events.quits++;
}

static const struct editor_listener listener = {
	.changed = changed,
	.title = title,
	.copy = copy,
	.paste = paste,
	.cursor = cursor,
	.quit = quit,
};

/* A project in a directory of its own */
struct project {
	char root[64];
	char path[PATH_MAX];
};

static void write_file(struct project *project, const char *name, const char *contents)
{
	int fd;

	snprintf_array(project->path, "%s/%s", project->root, name);
	assert((fd = open(project->path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0);
	assert(write_array(fd, array_ptr(contents, strlen(contents))) == (ssize_t)strlen(contents));
	close(fd);
}

static bool file_is(struct project *project, const char *name, const char *contents)
{
	char bytes[256];
	ssize_t size;
	int fd;

	snprintf_array(project->path, "%s/%s", project->root, name);
	if ((fd = open(project->path, O_RDONLY | O_CLOEXEC)) < 0)
		return false;
	size = read_array(fd, bytes);
	close(fd);
	return size == (ssize_t)strlen(contents) && !memcmp(bytes, contents, size);
}

static void project_init(struct project *project)
{
	snprintf_array(project->root, "/tmp/editor-test-XXXXXX");
	assert(mkdtemp(project->root));
	write_file(project, "main.c", "int main(void)\n{\n\treturn x;\n}\n");
	write_file(project, "tar get.h", "/* */\n\n    int x;\n");
	snprintf_array(project->path, "%s/sub", project->root);
	assert(!mkdir(project->path, 0755));
	write_file(project, "sub/notes.txt", "notes\n");
	write_file(project, ".hidden", "");
}

static void project_fini(struct project *project)
{
	static const char *const names[] = { "main.c", "tar get.h", "sub/notes.txt", ".hidden", ".hidden.swp", "new.c", "big.c", "sub" };

	foreach_array_const_ref(names, name) {
		snprintf_array(project->path, "%s/%s", project->root, *name);
		if (unlink(project->path))
			rmdir(project->path);
	}
	rmdir(project->root);
}

static bool text_is(const struct editor_tab *tab, const char *contents)
{
	const struct text *text = &tab->document.text;

	return text->size == strlen(contents) && !memcmp(text->bytes, contents, text->size);
}

static bool caret_of(const struct editor_tab *tab, size_t anchor, size_t caret)
{
	return tab->document.view.anchor == anchor && tab->document.view.caret == caret;
}

static void type(struct editor *editor, xkb_keysym_t sym, uint32_t modifiers, const char *text)
{
	editor_key(editor, sym, modifiers, strlen(text), array_ptr(text, strlen(text)));
}

static struct editor_tab *loaded(struct editor *editor, struct poor_loop *loop)
{
	struct editor_tab *tab;

	run_until(loop, (tab = editor_tab(editor)) && !tab->loading);
	return tab;
}

static void stop(struct editor *editor, struct poor_loop *loop)
{
	editor_stop(editor);
	run_until(loop, !editor_busy(editor));
	run_until(loop, !log_pending());
	loop_fini(loop);
}

/*
 * Edits go to the file with Ctrl+S, while editing goes on. A tab with changes closes and the editor quits only when
 * that is asked twice in a row.
 */
TEST(editor_saves)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	assert(tab->document.code && editor.tab_count == 1 && !strcmp(editor.title, "main.c - ce"));
	type(&editor, XKB_KEY_End, 0, "");
	type(&editor, XKB_KEY_space, 0, " ");
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_y, 0, "y");
	assert(tab->saving && history_modified(&tab->document.history));
	run_until(&loop, !tab->saving);
	assert(file_is(&project, "main.c", "int main(void) x\n{\n\treturn x;\n}\n") && !strcmp(editor.status, "main.c is saved"));
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.tab_count == 1 && editor.confirm == EDITOR_CLOSING);
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	assert(!events.quits && editor.confirm == EDITOR_QUITTING);
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	assert(events.quits == 1);
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(!editor.tab_count);
	snprintf_array(project.path, "%s/new.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	assert(!tab->document.text.size);
	type(&editor, XKB_KEY_a, 0, "a");
	type(&editor, XKB_KEY_Return, 0, "\r");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	run_until(&loop, !tab->saving);
	assert(file_is(&project, "new.c", "a\n") && !history_modified(&tab->document.history));
	stop(&editor, &loop);
	project_fini(&project);
}

/* A tab or the editor that is closed while saves go on closes once they succeed, unless input comes first */
TEST(editor_closes_after_saves)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;
	unsigned quits = events.quits;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	loaded(&editor, &loop);
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.tab_count == 1 && editor.confirm == EDITOR_NOTHING);
	run_until(&loop, !editor.tab_count);
	assert(file_is(&project, "main.c", "xint main(void)\n{\n\treturn x;\n}\n"));
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	type(&editor, XKB_KEY_y, 0, "y");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_z, 0, "z");
	run_until(&loop, !tab->saving);
	assert(editor.tab_count == 1 && history_modified(&tab->document.history));
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	assert(editor.quitting && events.quits == quits);
	run_until(&loop, events.quits == quits + 1);
	assert(file_is(&project, "main.c", "yzxint main(void)\n{\n\treturn x;\n}\n"));
	stop(&editor, &loop);
	project_fini(&project);
}

/*
 * A tab with changes closes once that is asked for it twice in a row, with keys or clicks, Ctrl may be let go in
 * between. A scroll to another tab asks again, for closing and quitting, even if it scrolls back.
 */
TEST(editor_confirms_closing)
{
	static struct project project;
	struct editor_tab *code, *header;
	struct poor_loop loop;
	unsigned quits = events.quits;
	int32_t start, end;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	type(&editor, XKB_KEY_a, 0, "a");
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	header = loaded(&editor, &loop);
	type(&editor, XKB_KEY_b, 0, "b");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.confirm == EDITOR_CLOSING);
	editor_scroll(&editor, editor.tabs_rect.x + 1, editor.tabs_rect.y + 1, 1, 0);
	editor_scroll(&editor, editor.tabs_rect.x + 1, editor.tabs_rect.y + 1, -1, 0);
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.tab_count == 2 && editor.confirm == EDITOR_CLOSING);
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	editor_scroll(&editor, editor.tabs_rect.x + 1, editor.tabs_rect.y + 1, 1, 0);
	editor_scroll(&editor, editor.tabs_rect.x + 1, editor.tabs_rect.y + 1, -1, 0);
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	assert(events.quits == quits && editor.confirm == EDITOR_QUITTING);
	editor_scroll(&editor, editor.tabs_rect.x + 1, editor.tabs_rect.y + 1, 1, 0);
	assert(editor_tab(&editor) == code);
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.tab_count == 2 && !strcmp(editor.status, "main.c has changes that aren't saved: close it again to lose them"));
	type(&editor, XKB_KEY_Control_L, 0, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.tab_count == 1 && editor_tab(&editor) == header && editor_tab_extent(&editor, 0, &start, &end));
	editor_button(&editor, end - editor.cell_width, editor.tabs_rect.y + 1, BTN_LEFT, true, 0, 0);
	editor_button(&editor, end - editor.cell_width, editor.tabs_rect.y + 1, BTN_LEFT, false, 0, 0);
	assert(editor.tab_count == 1 && editor.confirm == EDITOR_CLOSING);
	editor_button(&editor, start + 1, editor.tabs_rect.y + 1, BTN_MIDDLE, true, 0, 0);
	assert(!editor.tab_count && !editor.status[0]);
	stop(&editor, &loop);
	project_fini(&project);
}

/* A save that fails keeps the changes and their tab, and the editor */
TEST(editor_keeps_unsaved)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;
	unsigned quits = events.quits;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/gone/new.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	type(&editor, XKB_KEY_a, 0, "a");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	run_until(&loop, !tab->saving);
	assert(editor_tab(&editor) == tab && history_unsaved(&tab->document.history));
	assert(!strcmp(editor.status, "new.c: No such file or directory"));
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	run_until(&loop, !tab->saving);
	assert(!editor.quitting && events.quits == quits && editor.tab_count == 1);
	type(&editor, XKB_KEY_q, SEAT_CONTROL, "");
	assert(editor.confirm == EDITOR_QUITTING && events.quits == quits);
	stop(&editor, &loop);
	project_fini(&project);
}

/* A paste goes to the selection that it was asked for, unless its document changes first */
TEST(editor_pastes)
{
	static struct project project;
	struct editor_tab *code, *header;
	struct poor_loop loop;
	unsigned pastes = events.pastes;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	header = loaded(&editor, &loop);
	type(&editor, XKB_KEY_Right, SEAT_SHIFT, "");
	type(&editor, XKB_KEY_Right, SEAT_SHIFT, "");
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	assert(events.pastes == pastes + 1 && !events.primary);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	assert(editor_tab(&editor) == code);
	editor_paste(&editor, arrview_str("x"));
	assert(text_is(header, "x */\n\n    int x;\n") && text_is(code, "int main(void)\n{\n\treturn x;\n}\n"));
	editor_paste(&editor, arrview_str("x"));
	assert(text_is(header, "x */\n\n    int x;\n") && text_is(code, "int main(void)\n{\n\treturn x;\n}\n"));
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_a, 0, "a");
	editor_paste(&editor, arrview_str("x"));
	assert(text_is(code, "aint main(void)\n{\n\treturn x;\n}\n") && !strcmp(editor.status, "Nothing is pasted, main.c changed meanwhile"));
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor_tab(&editor) == header && events.pastes == pastes + 3);
	editor_paste(&editor, arrview_str("x"));
	assert(text_is(header, "x */\n\n    int x;\n"));
	stop(&editor, &loop);
	project_fini(&project);
}

/* A document waits to go to the server while there is no room for it, and nothing is asked about it meanwhile */
TEST(editor_waits_for_room)
{
	static struct project project;
	static char contents[(13 << 20) + 1];
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;
	uint64_t version;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	memset(contents, 'a', sizeof(contents) - 1);
	for (size_t at = 63; at < sizeof(contents) - 1; at += 64)
		contents[at] = '\n';
	write_file(&project, "big.c", contents);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	run_until(&loop, editor.lsp.state == LSP_READY);
	snprintf_array(project.path, "%s/big.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_F12, 0, "");
	assert(editor.request && tab->lsp_version == tab->document.version);
	version = tab->lsp_version;
	type(&editor, XKB_KEY_y, 0, "y");
	type(&editor, XKB_KEY_F12, 0, "");
	assert(!editor.request && tab->lsp_version == version && strstr(editor.status, "can't take big.c now"));
	run_until(&loop, !editor.lsp.write_op.pending);
	type(&editor, XKB_KEY_F12, 0, "");
	assert(editor.request && tab->lsp_version == tab->document.version);
	run_until(&loop, editor_tab(&editor) != tab);
	stop(&editor, &loop);
	project_fini(&project);
}

/*
 * The tree has the files of the root without the hidden ones, directories first. A click on a directory expands it,
 * a click on a file opens it, in a tab after the active one.
 */
TEST(editor_tree)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	double row;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	run_until(&loop, tree->nodes[0].loaded);
	assert(tree_rows(tree) == 3 && !strcmp(tree_name(tree, tree_row(tree, 0)), "sub"));
	assert(!strcmp(tree_name(tree, tree_row(tree, 1)), "main.c") && !strcmp(tree_name(tree, tree_row(tree, 2)), "tar get.h"));
	row = editor.tree_rect.y + editor.padding + editor.cell_height / 2;
	editor_button(&editor, 20, row, BTN_LEFT, true, 0, 0);
	editor_button(&editor, 20, row, BTN_LEFT, false, 0, 0);
	run_until(&loop, tree->nodes[tree_row(tree, 0)].loaded);
	assert(tree_rows(tree) == 4 && !strcmp(tree_name(tree, tree_row(tree, 1)), "notes.txt"));
	editor_button(&editor, 20, row + editor.cell_height, BTN_LEFT, true, 0, 0);
	assert(!loaded(&editor, &loop)->document.code && !strcmp(path_name(editor_tab(&editor)->path), "notes.txt"));
	editor_button(&editor, 20, row + 2 * editor.cell_height, BTN_LEFT, true, 0, 0);
	assert(editor.tab_count == 2 && editor.active == 1 && loaded(&editor, &loop)->document.code);
	snprintf_array(project.path, "%s/sub/notes.txt", project.root);
	assert(tree_find(tree, project.path) == tree_row(tree, 1));
	stop(&editor, &loop);
	project_fini(&project);
}

/*
 * F12 sends the document to the server and goes where the server says, Alt+Left goes back. The selection becomes the
 * primary selection.
 */
TEST(editor_goes_to_definitions)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(tab->lsp_version == tab->document.version);
	assert(document_place(&tab->document, 25, false));
	type(&editor, XKB_KEY_F12, 0, "");
	assert(editor.request);
	run_until(&loop, !editor.request && editor_tab(&editor) != tab);
	tab = loaded(&editor, &loop);
	assert(!strcmp(path_name(tab->path), "tar get.h") && tab->document.view.caret == 11 && editor.tab_count == 2);
	type(&editor, XKB_KEY_Right, SEAT_SHIFT, "");
	assert(events.copies && events.primary && !strcmp(events.copied, "i"));
	type(&editor, XKB_KEY_Left, SEAT_ALT, "");
	tab = editor_tab(&editor);
	assert(!strcmp(path_name(tab->path), "main.c") && tab->document.view.caret == 25);
	type(&editor, XKB_KEY_Right, SEAT_ALT, "");
	assert(!strcmp(path_name(editor_tab(&editor)->path), "tar get.h"));
	stop(&editor, &loop);
	assert(editor.lsp.state == LSP_EXITED);
	project_fini(&project);
}

struct pause {
	struct poor_loop_timer timer;
	bool over;
};

static void pause_over(struct poor_loop *, struct poor_loop_timer *timer)
{
	container_of(timer, struct pause, timer)->over = true;
}

/* Time for what comes with nothing to wait for */
static void pause_for(struct poor_loop *loop, uint64_t ns)
{
	struct pause pause;

	poor_loop_timer_init(&pause.timer, pause_over);
	pause.over = false;
	poor_loop_timer_arm(loop, &pause.timer, poor_loop_now() + ns);
	run_until(loop, pause.over);
}

static bool caret_shows(const struct view *view, size_t column)
{
	return view->left <= column && column - view->left < (size_t)((view->width - view->padding) / view->cell_width);
}

/* The caret of a paste stays in sight when the lines of the paste widen the gutter, also in a tab that isn't shown */
TEST(editor_shows_pasted_carets)
{
	static struct project project;
	static char contents[2 * 9999 + 300];
	struct editor_tab *code, *header;
	struct poor_loop loop;

	for (size_t at = 0; at < 2 * 9999; at += 2)
		memcpy(&contents[at], "x\n", 2);
	memset(&contents[2 * 9999], 'a', 300);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	editor_paste(&editor, contents);
	assert(code->document.text.line_count > 10'000 && caret_shows(&code->document.view, 300));
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	header = loaded(&editor, &loop);
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	editor_paste(&editor, contents);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	assert(editor_tab(&editor) == header && header->document.text.line_count > 10'000);
	assert(caret_shows(&header->document.view, 300));
	stop(&editor, &loop);
	project_fini(&project);
}

/* A jump into a file that is read shows where it went with the gutter of all lines of the file */
TEST(editor_jumps_into_long_files)
{
	static struct project project;
	static char contents[300 + 2 * 9999 + 1];
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	memset(contents, 'a', 300);
	for (size_t at = 300; at < sizeof(contents) - 1; at += 2)
		memcpy(&contents[at], "\nx", 2);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	write_file(&project, "big.c", contents);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/big.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(tab->document.text.line_count == 10'000 && document_place(&tab->document, 250, false));
	type(&editor, XKB_KEY_F12, 0, "");
	run_until(&loop, !editor.request && editor_tab(&editor) != tab);
	loaded(&editor, &loop);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(editor.tab_count == 1);
	type(&editor, XKB_KEY_Left, SEAT_ALT, "");
	tab = loaded(&editor, &loop);
	assert(!strcmp(path_name(tab->path), "big.c") && tab->document.view.caret == 250);
	assert(caret_shows(&tab->document.view, 250));
	stop(&editor, &loop);
	project_fini(&project);
}

/* A definition that comes once the editor stops opens nothing, which the stop would wait for */
TEST(editor_drops_definitions_when_stopping)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(document_place(&tab->document, 25, false));
	type(&editor, XKB_KEY_F12, 0, "");
	stop(&editor, &loop);
	assert(editor.lsp.state == LSP_EXITED && editor.tab_count == 1);
	project_fini(&project);
}

/* A definition asked for before another tab came goes nowhere, even after going back to the tab it was asked in */
TEST(editor_drops_definitions_after_navigation)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *header, *code;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	header = loaded(&editor, &loop);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(document_place(&code->document, 25, false));
	type(&editor, XKB_KEY_F12, 0, "");
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	assert(editor_tab(&editor) == header);
	type(&editor, XKB_KEY_Return, 0, "\r");
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	pause_for(&loop, 300'000'000);
	assert(editor_tab(&editor) == code && editor.tab_count == 2 && !editor.jump_count);
	stop(&editor, &loop);
	project_fini(&project);
}

/*
 * A tab doesn't save over the file of another tab, which it couldn't know before the file was there. The other tab
 * still saves.
 */
TEST(editor_saves_once)
{
	static struct project project;
	struct editor_tab *first, *second;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/new.c", project.root);
	editor_open(&editor, project.path);
	first = loaded(&editor, &loop);
	snprintf_array(project.path, "%s/sub/../new.c", project.root);
	editor_open(&editor, project.path);
	second = loaded(&editor, &loop);
	assert(first != second && editor.tab_count == 2);
	type(&editor, XKB_KEY_b, 0, "b");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	run_until(&loop, !second->saving);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_a, 0, "a");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	run_until(&loop, !first->saving);
	assert(file_is(&project, "new.c", "b") && history_modified(&first->document.history));
	assert(!strcmp(editor.status, "new.c isn't saved, another tab has its file"));
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_c, 0, "c");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	run_until(&loop, !second->saving);
	assert(file_is(&project, "new.c", "bc") && !history_modified(&second->document.history));
	stop(&editor, &loop);
	project_fini(&project);
}

/* The tab that shows stays when another one turns out to have a file that a tab has, as when files open at the start */
TEST(editor_keeps_the_shown_tab)
{
	static struct project project;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	snprintf_array(project.path, "%s/alias.c", project.root);
	assert(!symlink("main.c", project.path));
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	snprintf_array(project.path, "%s/alias.c", project.root);
	editor_open(&editor, project.path);
	snprintf_array(project.path, "%s/sub/notes.txt", project.root);
	editor_open(&editor, project.path);
	run_until(&loop, editor.tab_count == 2 && !editor_tab(&editor)->loading);
	assert(!strcmp(path_name(editor_tab(&editor)->path), "notes.txt"));
	stop(&editor, &loop);
	snprintf_array(project.path, "%s/alias.c", project.root);
	unlink(project.path);
	project_fini(&project);
}

/* A file opens in one tab, whatever path leads to it */
TEST(editor_opens_files_once)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	snprintf_array(project.path, "%s/alias.c", project.root);
	assert(!symlink("main.c", project.path));
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/sub/../main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	type(&editor, XKB_KEY_x, 0, "x");
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	run_until(&loop, editor.tab_count == 1);
	assert(editor_tab(&editor) == tab && text_is(tab, "xint main(void)\n{\n\treturn x;\n}\n"));
	snprintf_array(project.path, "%s/alias.c", project.root);
	editor_open(&editor, project.path);
	run_until(&loop, editor.tab_count == 1);
	assert(editor_tab(&editor) == tab);
	stop(&editor, &loop);
	snprintf_array(project.path, "%s/alias.c", project.root);
	unlink(project.path);
	project_fini(&project);
}

/* A definition that comes after the document changed, or after another tab came, goes nowhere */
TEST(editor_drops_stale_definitions)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *notes, *code;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/sub/notes.txt", project.root);
	editor_open(&editor, project.path);
	notes = loaded(&editor, &loop);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(document_place(&code->document, 25, false));
	type(&editor, XKB_KEY_F12, 0, "");
	type(&editor, XKB_KEY_z, 0, "z");
	run_until(&loop, !editor.request);
	assert(editor_tab(&editor) == code && editor.tab_count == 2 && !editor.jump_count);
	type(&editor, XKB_KEY_F12, 0, "");
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	run_until(&loop, !editor.request);
	assert(editor_tab(&editor) == notes && editor.tab_count == 2 && !editor.jump_count);
	stop(&editor, &loop);
	project_fini(&project);
}

/* Alt+Left without tabs goes back to where the last jump came from */
TEST(editor_goes_back_without_tabs)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(document_place(&tab->document, 25, false));
	type(&editor, XKB_KEY_F12, 0, "");
	run_until(&loop, !editor.request && editor_tab(&editor) != tab);
	loaded(&editor, &loop);
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(!editor.tab_count);
	type(&editor, XKB_KEY_Left, SEAT_ALT, "");
	tab = loaded(&editor, &loop);
	assert(!strcmp(path_name(tab->path), "main.c") && tab->document.view.caret == 25 && !editor.jump_at);
	stop(&editor, &loop);
	project_fini(&project);
}

/* A definition is asked for once the server has every document that changed */
TEST(editor_sends_changes)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *code, *header;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	header = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	type(&editor, XKB_KEY_Return, 0, "\r");
	assert(header->lsp_version && header->lsp_version != header->document.version);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	assert(editor_tab(&editor) == code && document_place(&code->document, 25, false));
	type(&editor, XKB_KEY_F12, 0, "");
	assert(editor.request && header->lsp_version == header->document.version);
	run_until(&loop, !editor.request && editor_tab(&editor) == header);
	stop(&editor, &loop);
	project_fini(&project);
}

/* Going back to a document that changed since the jump lands between characters */
TEST(editor_goes_back_between_characters)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	write_file(&project, "main.c", "a\xc3\xa9\xe2\x82\xacx;\n");
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, editor.lsp.state == LSP_READY);
	assert(document_place(&tab->document, 3, false));
	type(&editor, XKB_KEY_F12, 0, "");
	run_until(&loop, !editor.request && editor_tab(&editor) != tab);
	loaded(&editor, &loop);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_Home, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_Delete, 0, "");
	assert(editor_tab(&editor) == tab && text_is(tab, "\xc3\xa9\xe2\x82\xacx;\n"));
	type(&editor, XKB_KEY_Left, SEAT_ALT, "");
	assert(editor_tab(&editor) == tab && tab->document.view.caret == 2);
	stop(&editor, &loop);
	project_fini(&project);
}

/* A caret far to the right of what shows doesn't show, where its position would wrap around either */
TEST(editor_hides_far_caret)
{
	static struct project project;
	static struct atlas atlas;
	static struct frame frame;
	static char contents[8001];
	struct editor_tab *tab;
	struct poor_loop loop;
	struct font font;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	memset(contents, 'a', sizeof(contents) - 1);
	write_file(&project, "big.c", contents);
	assert(font_init(&font, 15));
	atlas_init(&atlas, &font);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	editor_focus(&editor, true);
	snprintf_array(project.path, "%s/big.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	assert(document_place(&tab->document, (UINT16_MAX + 1 + 100) / editor.cell_width, false));
	editor_scroll(&editor, editor.text_rect.x + 1, editor.text_rect.y + 1, 0, -100000);
	assert(!tab->document.view.left);
	scene_build(&editor, &atlas, &frame);
	assert(frame.count < ARRAY_SIZE(frame.quads));
	foreach_array_const_ref(arrview_first(frame.count, frame.quads), quad)
		assert(quad->u != QUAD_SOLID || quad->color != QUAD_RGB(THEME_CARET));
	stop(&editor, &loop);
	font_fini(&font);
	project_fini(&project);
}

/* The color of the character at 'column' of 'line' of the shown document */
static uint32_t color_at(struct atlas *atlas, size_t line, size_t column)
{
	static struct frame frame;
	const struct view *view = &editor_tab(&editor)->document.view;
	int32_t x = editor.text_rect.x + view->padding + (column - view->left) * editor.cell_width;
	int32_t y = editor.text_rect.y + view->padding + (line - view->top) * editor.cell_height;

	scene_build(&editor, atlas, &frame);
	foreach_array_const_ref(arrview_first(frame.count, frame.quads), quad)
		if (quad->u != QUAD_SOLID && quad->x == x && quad->y == y)
			return quad->color;
	return 0;
}

/*
 * Words show as the server tells once the shown document stays as it is for a moment. Lines that edits changed show as
 * they look until the next tokens come, the others keep theirs.
 */
TEST(editor_highlights)
{
	static struct project project;
	static struct atlas atlas;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;
	struct font font;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	assert(font_init(&font, 15));
	atlas_init(&atlas, &font);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, tab->tokens_version == tab->document.version);
	assert(tab->token_count == 4 && color_at(&atlas, 2, 15) == QUAD_RGB(THEME_MACRO));
	assert(color_at(&atlas, 3, 0) == QUAD_RGB(THEME_INACTIVE));
	type(&editor, XKB_KEY_Return, 0, "\r");
	assert(color_at(&atlas, 3, 15) == QUAD_RGB(THEME_MACRO) && color_at(&atlas, 4, 0) == QUAD_RGB(THEME_INACTIVE));
	assert(document_place(&tab->document, text_line_start(&tab->document.text, 4), false));
	type(&editor, XKB_KEY_a, 0, "a");
	assert(color_at(&atlas, 4, 1) == QUAD_RGB(THEME_TEXT) && tab->tokens_version != tab->document.version);
	run_until(&loop, tab->tokens_version == tab->document.version);
	stop(&editor, &loop);
	font_fini(&font);
	project_fini(&project);
}

/* An answer with tokens that comes once the editor stops asks for no more, so nothing is left for the loop */
TEST(editor_stops_highlighting)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, tab->tokens_version == tab->document.version);
	type(&editor, XKB_KEY_a, 0, "a");
	run_until(&loop, editor.highlight_tab == tab);
	type(&editor, XKB_KEY_b, 0, "b");
	stop(&editor, &loop);
	project_fini(&project);
}

/*
 * A document that changed goes to the server even if another one shows, whose tokens may depend on it. After a save,
 * the server can tell that the tokens of all documents may have changed, then they are asked for again.
 */
TEST(editor_highlights_with_other_documents)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *code, *header;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	header = loaded(&editor, &loop);
	run_until(&loop, header->tokens_version == header->document.version);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	code = loaded(&editor, &loop);
	run_until(&loop, code->tokens_version == code->document.version);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	run_until(&loop, header->lsp_version == header->document.version);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	run_until(&loop, !header->saving);
	run_until(&loop, !code->tokens_version);
	type(&editor, XKB_KEY_Tab, SEAT_CONTROL, "");
	run_until(&loop, code->tokens_version == code->document.version);
	stop(&editor, &loop);
	project_fini(&project);
}

/*
 * A save tells the server what it saved, after the changes that it saved, and files that include the document may
 * change. Typing goes on meanwhile.
 */
TEST(editor_tells_saves)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, tab->tokens_version == tab->document.version);
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_y, 0, "y");
	run_until(&loop, !tab->saving);
	assert(tab->lsp_version == tab->document.version);
	run_until(&loop, !tab->tokens_version);
	run_until(&loop, tab->tokens_version == tab->document.version);
	stop(&editor, &loop);
	project_fini(&project);
}

/* Messages that aren't of the editor fill up the buffer for the server, which reads big ones late */
static void fill(struct lsp *lsp)
{
	static struct {
		struct text text;
		char bytes[22 << 20];
		uint32_t lines[1];
		uint8_t states[1];
	} big;

	text_init(&big.text, big.bytes, big.lines, big.states);
	memset(big.bytes, 'a', sizeof(big.bytes));
	big.text.size = 2 << 20;
	assert(lsp_open(lsp, "/a.c", "c", 1, &big.text));
	big.text.size = sizeof(big.bytes);
	assert(lsp_open(lsp, "/b.c", "c", 1, &big.text));
	big.text.size = 0;
	while (lsp_open(lsp, "/c.c", "c", 1, &big.text))
		;
}

/* A document that doesn't fit beside the messages that wait for the server goes to it once they are written, without input */
TEST(editor_waits_for_the_server)
{
	static struct project project;
	const char *server[] = { getenv("FAKE_LSP"), "slow", nullptr };
	struct editor_tab *tab;
	struct poor_loop loop;

	assert(server[0]);
	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, server);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, tab->lsp_version == tab->document.version && !editor.lsp.write_op.pending);
	fill(&editor.lsp);
	type(&editor, XKB_KEY_x, 0, "x");
	run_until(&loop, tab->lsp_version == tab->document.version);
	stop(&editor, &loop);
	project_fini(&project);
}

static void click(double x, double y, uint32_t time)
{
	editor_button(&editor, x, y, BTN_LEFT, true, time, 0);
	editor_button(&editor, x, y, BTN_LEFT, false, time, 0);
}

/* A drag ends when the pointer leaves without the release of its button */
TEST(editor_ends_drags_when_left)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;
	size_t columns;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	editor_button(&editor, editor.text_rect.x + 5, editor.text_rect.y + 5, BTN_LEFT, true, 0, 0);
	editor_motion(&editor, editor.text_rect.x + 45, editor.text_rect.y + 5);
	assert(caret_of(tab, 0, 4));
	editor_leave(&editor);
	editor_motion(&editor, editor.text_rect.x + 5, editor.text_rect.y + 45);
	assert(caret_of(tab, 0, 4) && editor.drag == EDITOR_IDLE);
	editor_button(&editor, editor.tree_rect.width, 100, BTN_LEFT, true, 0, 0);
	assert(editor.drag == EDITOR_RESIZING);
	columns = editor.tree_columns;
	editor_leave(&editor);
	editor_motion(&editor, 500, 100);
	assert(editor.drag == EDITOR_IDLE && editor.tree_columns == columns);
	stop(&editor, &loop);
	project_fini(&project);
}

/* Clicks in a row near each other select a word and then a line, a click elsewhere places the caret */
TEST(editor_counts_clicks)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;
	double x, y;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	x = editor.text_rect.x + editor.padding + 2;
	y = editor.text_rect.y + editor.padding + editor.cell_height / 2;
	click(x + editor.cell_width, y, 1000);
	click(x + 5 * editor.cell_width, y, 1100);
	assert(caret_of(tab, 5, 5));
	click(x + 5 * editor.cell_width + 1, y, 1200);
	assert(caret_of(tab, 4, 8));
	click(x + 5 * editor.cell_width, y + 1, 1300);
	assert(caret_of(tab, 0, 15));
	stop(&editor, &loop);
	project_fini(&project);
}

/* Ctrl+X erases the selection once the clipboard has it */
TEST(editor_cuts)
{
	static struct project project;
	struct editor_tab *tab;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	type(&editor, XKB_KEY_Right, SEAT_CONTROL | SEAT_SHIFT, "");
	events.refused = true;
	type(&editor, XKB_KEY_x, SEAT_CONTROL, "");
	events.refused = false;
	assert(text_is(tab, "int main(void)\n{\n\treturn x;\n}\n"));
	assert(!strcmp(editor.status, "The clipboard can't take the selection now"));
	type(&editor, XKB_KEY_x, SEAT_CONTROL, "");
	assert(text_is(tab, " main(void)\n{\n\treturn x;\n}\n") && !events.primary && !strcmp(events.copied, "int"));
	stop(&editor, &loop);
	project_fini(&project);
}

/* Tabs that open before the window has a size show once it has one */
TEST(editor_tabs)
{
	static struct project project;
	struct poor_loop loop;
	int32_t start, end;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	snprintf_array(project.path, "%s/tar get.h", project.root);
	editor_open(&editor, project.path);
	loaded(&editor, &loop);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	assert(editor.tab_count == 2 && editor.active == 1 && !editor.tab_first);
	assert(editor_tab_extent(&editor, 0, &start, &end) && start == editor.tabs_rect.x);
	assert(editor_tab_extent(&editor, 1, &start, &end) && end <= editor.tabs_rect.x + editor.tabs_rect.width);
	stop(&editor, &loop);
	project_fini(&project);
}

/* A narrowed window keeps half of it for the text, the tree gets its width back when the window does */
TEST(editor_fits_the_tree)
{
	static struct project project;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	editor_button(&editor, editor.tree_rect.width, 100, BTN_LEFT, true, 0, 0);
	editor_motion(&editor, 400, 100);
	editor_button(&editor, 400, 100, BTN_LEFT, false, 0, 0);
	assert(editor.tree_rect.width == 39 * 10 + 8);
	editor_resize(&editor, 300, 600, 10, 20, 4, 120);
	assert(editor.tree_rect.width == 15 * 10 + 8 && editor.text_rect.width > 0);
	editor_button(&editor, editor.tree_rect.width, 100, BTN_LEFT, true, 0, 0);
	assert(editor.drag == EDITOR_RESIZING);
	editor_button(&editor, editor.tree_rect.width, 100, BTN_LEFT, false, 0, 0);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	assert(editor.tree_rect.width == 39 * 10 + 8);
	stop(&editor, &loop);
	project_fini(&project);
}

static bool rows_are(const struct tree *tree, size_t count, const char *const (*names)[count])
{
	if (tree_rows(tree) != count)
		return false;
	foreach_array_index(*names, i)
		if (strcmp(tree_name(tree, tree_row(tree, i)), (*names)[i]))
			return false;
	return true;
}

#define rows(tree, ...) rows_are(tree, ARRAY_SIZE(((const char *const[]){ __VA_ARGS__ })), &(const char *const[]){ __VA_ARGS__ })

/* Directories come first, then files by their names regardless of case, then with it */
TEST(editor_tree_sorts)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	char name[16];

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	for (int i = 0; i < 64; i++) {
		snprintf_array(name, "%c%02d.c", i % 2 ? 'F' : 'f', (i * 37) % 64);
		write_file(&project, name, "");
	}
	write_file(&project, "ab.c", "");
	write_file(&project, "Ab.c", "");
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	run_until(&loop, tree->nodes[0].loaded);
	assert(tree_rows(tree) == 69 && !strcmp(tree_name(tree, tree_row(tree, 0)), "sub"));
	assert(!strcmp(tree_name(tree, tree_row(tree, 1)), "Ab.c") && !strcmp(tree_name(tree, tree_row(tree, 2)), "ab.c"));
	for (size_t row = 2; row < tree_rows(tree); row++) {
		const char *before = tree_name(tree, tree_row(tree, row - 1)), *name = tree_name(tree, tree_row(tree, row));

		assert(strcasecmp(before, name) < 0 || (!strcasecmp(before, name) && strcmp(before, name) < 0));
	}
	stop(&editor, &loop);
	for (int i = 0; i < 64; i++) {
		snprintf_array(project.path, "%s/%c%02d.c", project.root, i % 2 ? 'F' : 'f', (i * 37) % 64);
		unlink(project.path);
	}
	snprintf_array(project.path, "%s/ab.c", project.root);
	unlink(project.path);
	snprintf_array(project.path, "%s/Ab.c", project.root);
	unlink(project.path);
	project_fini(&project);
}

/* Paths under the root of the filesystem have one slash */
TEST(editor_tree_at_root)
{
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	char path[PATH_MAX];
	uint32_t etc;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, "/", nullptr);
	run_until(&loop, tree->nodes[0].loaded);
	assert((etc = tree_find(tree, "/etc")) != TREE_NONE && tree_path(tree, etc, path) && !strcmp(path, "/etc"));
	assert(tree_path(tree, 0, path) && !strcmp(path, "/"));
	stop(&editor, &loop);
}

/* A directory that is replaced while it shows is watched again */
TEST(editor_tree_rewatches)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	run_until(&loop, tree->nodes[0].loaded);
	tree_toggle(tree, tree_row(tree, 0));
	run_until(&loop, rows(tree, "sub", "notes.txt", "main.c", "tar get.h"));
	snprintf_array(project.path, "%s/sub/notes.txt", project.root);
	assert(!unlink(project.path));
	snprintf_array(project.path, "%s/sub", project.root);
	assert(!rmdir(project.path) && !mkdir(project.path, 0755));
	run_until(&loop, rows(tree, "sub", "main.c", "tar get.h"));
	write_file(&project, "sub/notes.txt", "");
	run_until(&loop, rows(tree, "sub", "notes.txt", "main.c", "tar get.h"));
	stop(&editor, &loop);
	project_fini(&project);
}

/* A link to a directory that comes to point to another one shows that one, and follows it */
TEST(editor_tree_follows_links)
{
	static struct project project;
	static const char *const made[] = { "link", "one/x.txt", "two/y.txt", "two/z.txt", "one", "two" };
	struct poor_loop loop;
	struct tree *tree = &editor.tree;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	snprintf_array(project.path, "%s/one", project.root);
	assert(!mkdir(project.path, 0755));
	snprintf_array(project.path, "%s/two", project.root);
	assert(!mkdir(project.path, 0755));
	write_file(&project, "one/x.txt", "");
	write_file(&project, "two/y.txt", "");
	snprintf_array(project.path, "%s/link", project.root);
	assert(!symlink("one", project.path));
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	run_until(&loop, tree->nodes[0].loaded);
	assert(rows(tree, "link", "one", "sub", "two", "main.c", "tar get.h"));
	tree_toggle(tree, tree_row(tree, 0));
	run_until(&loop, rows(tree, "link", "x.txt", "one", "sub", "two", "main.c", "tar get.h"));
	snprintf_array(project.path, "%s/link", project.root);
	assert(!unlink(project.path) && !symlink("two", project.path));
	run_until(&loop, rows(tree, "link", "y.txt", "one", "sub", "two", "main.c", "tar get.h"));
	write_file(&project, "two/z.txt", "");
	run_until(&loop, rows(tree, "link", "y.txt", "z.txt", "one", "sub", "two", "main.c", "tar get.h"));
	stop(&editor, &loop);
	foreach_array_const_ref(made, name) {
		snprintf_array(project.path, "%s/%s", project.root, *name);
		if (unlink(project.path))
			rmdir(project.path);
	}
	project_fini(&project);
}

/*
 * The tree follows the directories that were read: new files come, deleted and renamed ones go, and what was read
 * under directories that stay stays with them. Changes leave nodes behind, which don't pile up.
 */
TEST(editor_tree_follows)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	char from[PATH_MAX], to[PATH_MAX];

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	run_until(&loop, tree->nodes[0].loaded);
	tree_toggle(tree, tree_row(tree, 0));
	run_until(&loop, rows(tree, "sub", "notes.txt", "main.c", "tar get.h"));
	write_file(&project, "new.c", "");
	write_file(&project, ".hidden.swp", "");
	write_file(&project, "sub/more.txt", "");
	run_until(&loop, rows(tree, "sub", "more.txt", "notes.txt", "main.c", "new.c", "tar get.h"));
	snprintf_array(project.path, "%s/main.c", project.root);
	unlink(project.path);
	snprintf_array(from, "%s/sub", project.root);
	snprintf_array(to, "%s/dir", project.root);
	assert(!rename(from, to));
	run_until(&loop, rows(tree, "dir", "new.c", "tar get.h"));
	assert(!rename(to, from));
	run_until(&loop, rows(tree, "sub", "new.c", "tar get.h"));
	for (int i = 0; i < 6; i++) {
		write_file(&project, "main.c", "");
		run_until(&loop, tree_rows(tree) == 4);
		snprintf_array(project.path, "%s/main.c", project.root);
		unlink(project.path);
		run_until(&loop, tree_rows(tree) == 3);
	}
	assert(tree->node_count < 20);
	stop(&editor, &loop);
	snprintf_array(project.path, "%s/sub/more.txt", project.root);
	unlink(project.path);
	project_fini(&project);
}

static bool cursor_is(const char *name)
{
	uint32_t node = editor_tree_cursor(&editor);

	return node != TREE_NONE && !strcmp(tree_name(&editor.tree, node), name);
}

TEST(editor_tree_keys)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	struct editor_tab *tab;
	unsigned pastes = events.pastes;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	run_until(&loop, tree->nodes[0].loaded);
	assert(editor_browsing(&editor) && cursor_is("sub"));
	type(&editor, XKB_KEY_Down, 0, "");
	type(&editor, XKB_KEY_Down, 0, "");
	type(&editor, XKB_KEY_Down, 0, "");
	assert(cursor_is("tar get.h"));
	type(&editor, XKB_KEY_Home, 0, "");
	type(&editor, XKB_KEY_Right, 0, "");
	run_until(&loop, rows(tree, "sub", "notes.txt", "main.c", "tar get.h"));
	assert(cursor_is("sub"));
	type(&editor, XKB_KEY_Right, 0, "");
	assert(cursor_is("notes.txt"));
	type(&editor, XKB_KEY_Left, 0, "");
	assert(cursor_is("sub"));
	type(&editor, XKB_KEY_Left, 0, "");
	assert(rows(tree, "sub", "main.c", "tar get.h") && cursor_is("sub"));
	type(&editor, XKB_KEY_t, 0, "t");
	assert(cursor_is("tar get.h"));
	type(&editor, XKB_KEY_m, SEAT_SHIFT, "M");
	assert(cursor_is("main.c"));
	type(&editor, XKB_KEY_Return, 0, "\r");
	tab = loaded(&editor, &loop);
	assert(!editor_browsing(&editor) && !strcmp(path_name(tab->path), "main.c"));
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(editor_browsing(&editor) && cursor_is("main.c"));
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_v, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_Escape, 0, "");
	assert(!editor_browsing(&editor) && events.pastes == pastes && text_is(tab, "int main(void)\n{\n\treturn x;\n}\n"));
	type(&editor, XKB_KEY_x, 0, "x");
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_s, SEAT_CONTROL, "");
	run_until(&loop, !tab->saving);
	assert(file_is(&project, "main.c", "xint main(void)\n{\n\treturn x;\n}\n"));
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(!editor_browsing(&editor));
	type(&editor, XKB_KEY_b, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(editor.show_tree && editor_browsing(&editor));
	type(&editor, XKB_KEY_b, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_b, SEAT_CONTROL, "");
	assert(editor.show_tree && !editor_browsing(&editor));
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	click(editor.text_rect.x + 5, editor.text_rect.y + 5, 0);
	assert(!editor_browsing(&editor));
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	type(&editor, XKB_KEY_w, SEAT_CONTROL, "");
	assert(!editor.tab_count && editor_browsing(&editor));
	type(&editor, XKB_KEY_Up, 0, "");
	assert(cursor_is("sub"));
	stop(&editor, &loop);
	project_fini(&project);
}

TEST(editor_tree_cursor_stays)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 100, 10, 20, 4, 120);
	run_until(&loop, tree->nodes[0].loaded);
	type(&editor, XKB_KEY_Return, 0, "\r");
	run_until(&loop, rows(tree, "sub", "notes.txt", "main.c", "tar get.h"));
	type(&editor, XKB_KEY_End, 0, "");
	assert(cursor_is("tar get.h") && tree->top == 1);
	type(&editor, XKB_KEY_Page_Up, 0, "");
	assert(cursor_is("notes.txt") && tree->top == 1);
	type(&editor, XKB_KEY_Up, 0, "");
	assert(cursor_is("sub") && !tree->top);
	type(&editor, XKB_KEY_Page_Down, 0, "");
	assert(cursor_is("main.c") && !tree->top);
	write_file(&project, "a.c", "");
	run_until(&loop, rows(tree, "sub", "notes.txt", "a.c", "main.c", "tar get.h"));
	assert(cursor_is("main.c"));
	type(&editor, XKB_KEY_Up, 0, "");
	type(&editor, XKB_KEY_Up, 0, "");
	snprintf_array(project.path, "%s/sub/notes.txt", project.root);
	assert(cursor_is("notes.txt") && !unlink(project.path));
	run_until(&loop, rows(tree, "sub", "a.c", "main.c", "tar get.h"));
	assert(cursor_is("sub"));
	write_file(&project, "sub/notes.txt", "notes\n");
	run_until(&loop, rows(tree, "sub", "notes.txt", "a.c", "main.c", "tar get.h"));
	editor_open(&editor, project.path);
	loaded(&editor, &loop);
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(editor_browsing(&editor) && cursor_is("notes.txt"));
	type(&editor, XKB_KEY_Left, 0, "");
	type(&editor, XKB_KEY_Left, 0, "");
	type(&editor, XKB_KEY_Down, 0, "");
	type(&editor, XKB_KEY_Escape, 0, "");
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(rows(tree, "sub", "a.c", "main.c", "tar get.h") && cursor_is("sub"));
	stop(&editor, &loop);
	snprintf_array(project.path, "%s/a.c", project.root);
	unlink(project.path);
	project_fini(&project);
}

static uint32_t caret_color(struct atlas *atlas, struct frame *frame)
{
	scene_build(&editor, atlas, frame);
	foreach_array_const_ref(arrview_first(frame->count, frame->quads), quad)
		if (quad->u == QUAD_SOLID && quad->width == 2 * editor.line && quad->height == editor.cell_height)
			return quad->color;
	return 0;
}

static bool framed(struct frame *frame, size_t row, uint32_t color)
{
	int32_t y = editor.tree_rect.y + editor.padding + (row - editor.tree.top) * editor.cell_height;

	foreach_array_const_ref(arrview_first(frame->count, frame->quads), quad)
		if (quad->u == QUAD_SOLID && quad->color == color && quad->x == editor.tree_rect.x && quad->y == y &&
		    quad->width == editor.tree_rect.width && quad->height == editor.line)
			return true;
	return false;
}

TEST(editor_shows_the_tree_cursor)
{
	static struct project project;
	static struct atlas atlas;
	static struct frame frame;
	struct poor_loop loop;
	struct font font;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	assert(font_init(&font, 15));
	atlas_init(&atlas, &font);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	editor_focus(&editor, true);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	loaded(&editor, &loop);
	run_until(&loop, editor.tree.nodes[0].loaded);
	assert(caret_color(&atlas, &frame) == QUAD_RGB(THEME_CARET) && !framed(&frame, 1, QUAD_RGB(THEME_ACCENT)));
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(caret_color(&atlas, &frame) == QUAD_RGB(THEME_DIM) && framed(&frame, 1, QUAD_RGB(THEME_ACCENT)));
	editor_focus(&editor, false);
	assert(caret_color(&atlas, &frame) == QUAD_RGB(THEME_DIM) && framed(&frame, 1, QUAD_RGB(THEME_DIM)));
	editor_focus(&editor, true);
	type(&editor, XKB_KEY_Escape, 0, "");
	assert(caret_color(&atlas, &frame) == QUAD_RGB(THEME_CARET) && !framed(&frame, 1, QUAD_RGB(THEME_ACCENT)));
	stop(&editor, &loop);
	font_fini(&font);
	project_fini(&project);
}

TEST(editor_tree_keys_show_the_cursor)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	char name[16];

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	for (int i = 0; i < 5; i++) {
		snprintf_array(name, "sub/%d.txt", i);
		write_file(&project, name, "");
	}
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 100, 10, 20, 4, 120);
	run_until(&loop, tree->nodes[0].loaded);
	type(&editor, XKB_KEY_Right, 0, "");
	run_until(&loop, tree_rows(tree) == 9);
	editor_scroll(&editor, 20, editor.tree_rect.y + editor.padding, 10, 0);
	assert(cursor_is("sub") && tree->top == 6);
	type(&editor, XKB_KEY_Left, 0, "");
	assert(rows(tree, "sub", "main.c", "tar get.h") && cursor_is("sub") && !tree->top);
	stop(&editor, &loop);
	for (int i = 0; i < 5; i++) {
		snprintf_array(project.path, "%s/sub/%d.txt", project.root, i);
		unlink(project.path);
	}
	project_fini(&project);
}

TEST(editor_tree_takes_keys_on_clicks)
{
	static struct project project;
	struct poor_loop loop;
	struct tree *tree = &editor.tree;
	struct editor_tab *tab;
	double row;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	snprintf_array(project.path, "%s/main.c", project.root);
	editor_open(&editor, project.path);
	tab = loaded(&editor, &loop);
	run_until(&loop, tree->nodes[0].loaded);
	row = editor.tree_rect.y + editor.padding + editor.cell_height / 2;
	click(20, row, 0);
	run_until(&loop, rows(tree, "sub", "notes.txt", "main.c", "tar get.h"));
	assert(editor_browsing(&editor) && cursor_is("sub"));
	type(&editor, XKB_KEY_Down, 0, "");
	type(&editor, XKB_KEY_n, 0, "n");
	assert(cursor_is("notes.txt") && text_is(tab, "int main(void)\n{\n\treturn x;\n}\n"));
	click(20, row + 2 * editor.cell_height, 0);
	assert(!editor_browsing(&editor) && editor_tab(&editor) == tab);
	stop(&editor, &loop);
	project_fini(&project);
}

TEST(editor_tree_browses_from_unread_directories)
{
	static struct project project;
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "editor-test");
	project_init(&project);
	editor_init(&editor, &loop, &listener, &memory);
	editor_start(&editor, project.root, nullptr);
	editor_resize(&editor, 800, 600, 10, 20, 4, 120);
	run_until(&loop, editor.tree.nodes[0].loaded);
	type(&editor, XKB_KEY_End, 0, "");
	snprintf_array(project.path, "%s/sub/notes.txt", project.root);
	editor_open(&editor, project.path);
	loaded(&editor, &loop);
	type(&editor, XKB_KEY_e, SEAT_CONTROL, "");
	assert(editor_browsing(&editor) && cursor_is("sub"));
	stop(&editor, &loop);
	project_fini(&project);
}
