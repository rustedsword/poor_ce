/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <poor_stdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"
#include "lsp.h"
#include "test-loop.h"
#include "test-runner.h"

/* Where the last request led, and the tokens of 'text' that the last answer had */
static struct {
	uint64_t id;
	size_t line, character, token_count;
	unsigned states, locations, answers, refreshes, writes;
	const struct text *text;
	struct lsp_token tokens[8];
	char path[PATH_MAX];
	bool found;
} result;

static void state(struct lsp *)
{
	result.states++;
}

static void location(struct lsp *, uint64_t id, const char *path, size_t line, size_t character)
{
	result.locations++;
	result.id = id;
	result.found = path;
	result.line = line;
	result.character = character;
	snprintf(result.path, sizeof(result.path), "%s", path ? path : "");
}

static void tokens(struct lsp *lsp, bool found)
{
	result.answers++;
	result.found = found;
	for (result.token_count = 0; found && result.token_count < ARRAY_SIZE(result.tokens) &&
				     lsp_token(lsp, result.text, &result.tokens[result.token_count]);
	     result.token_count++)
		;
}

static void refresh(struct lsp *)
{
	result.refreshes++;
}

static void written(struct lsp *)
{
	result.writes++;
}

static struct lsp_memory memory;

static const struct lsp_listener listener = {
	.state = state,
	.location = location,
	.tokens = tokens,
	.refresh = refresh,
	.written = written,
};

/* A document in memory of the test */
struct sample {
	struct text text;
	char bytes[64];
	uint32_t lines[8];
	uint8_t states[8];
};

static void sample_init(struct sample *sample, const char *contents)
{
	bool fits;

	text_init(&sample->text, sample->bytes, sample->lines, sample->states);
	copy_array(sample->bytes, array_ptr(contents, strlen(contents)));
	text_begin(&sample->text, strlen(contents));
	assert(text_index(&sample->text, SIZE_MAX, &fits) && fits);
}

static const char *server(void)
{
	const char *path = getenv("FAKE_LSP");

	assert(path);
	return path;
}

/*
 * The server asks something and sends a message that doesn't fit before it answers. Definitions come as locations,
 * declarations as links, and the server exits after the shutdown.
 */
TEST(lsp_locates)
{
	static struct lsp lsp;
	static struct sample sample;
	const char *argv[] = { server(), nullptr };
	struct poor_loop loop;
	uint64_t id;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	result.states = result.locations = 0;
	sample_init(&sample, "int x;\nint y = x;\n");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	assert(lsp.state == LSP_STARTING && !lsp_locate(&lsp, "/project dir/a.c", &sample.text, 0, false));
	run_until(&loop, lsp.state == LSP_READY);
	assert(!lsp.utf16 && result.states == 2);
	assert(lsp_open(&lsp, "/project dir/a.c", "c", 1, &sample.text));
	id = lsp_locate(&lsp, "/project dir/a.c", &sample.text, 15, false);
	assert(id);
	run_until(&loop, result.locations == 1);
	assert(result.id == id && result.found && !strcmp(result.path, "/project dir/tar get.h"));
	assert(result.line == 2 && result.character == 4);
	assert(lsp_change(&lsp, "/project dir/a.c", 2, &sample.text, 0));
	id = lsp_locate(&lsp, "/project dir/a.c", &sample.text, 15, true);
	run_until(&loop, result.locations == 2);
	assert(result.id == id && result.line == 5 && result.character == 1);
	assert(lsp_offset(&lsp, &sample.text, 1, 4) == 11 && lsp_offset(&lsp, &sample.text, 1, 100) == 17);
	assert(lsp_offset(&lsp, &sample.text, 9, 0) == sample.text.size);
	lsp_close(&lsp, "/project dir/a.c");
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_EXITED);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

static bool file_has_impl(const char *path, size_t size, const char (*bytes)[size])
{
	char contents[256];
	FILE *file = fopen(path, "rb");
	size_t length;

	if (!file)
		return false;
	length = fread(contents, 1, sizeof(contents), file);
	fclose(file);
	return length == size && !memcmp(contents, *bytes, size);
}

#define file_has(path, bytes) file_has_impl(path, ARRAY_SIZE(bytes), &auto_arr(bytes))

static void synced(struct lsp *lsp, struct poor_loop *loop, const struct text *text)
{
	unsigned locations = result.locations;

	assert(lsp_locate(lsp, "/project dir/a.c", text, 0, false));
	run_until(loop, result.locations > locations);
}

TEST(lsp_sends_changed_lines)
{
	static struct lsp lsp;
	static struct sample sample;
	char directory[] = "/tmp/lsp-test-XXXXXX", copy[PATH_MAX], changes[PATH_MAX];
	const char *argv[] = { server(), "incremental", copy, nullptr };
	struct text *text = &sample.text;
	struct poor_loop loop;
	size_t none = 0;

	assert(mkdtemp(directory));
	snprintf_array(copy, "%s/copy", directory);
	snprintf_array(changes, "%s/copy.changes", directory);
	loop_init(&loop);
	log_init(&loop, "lsp-test");
	sample_init(&sample, "int x;\nint y = x;\nint z;\n");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	run_until(&loop, lsp.state == LSP_READY);
	assert(lsp.incremental && lsp_open(&lsp, "/project dir/a.c", "c", 1, text));
	text_mark(text, 1);
	assert(text_replace(text, 7, 18, arrview_str("long w;\nlong v = 1;\n")) && text_replace(text, 4, 5, arrview_str("X")));
	assert(lsp_change(&lsp, "/project dir/a.c", 2, text, 1));
	text_mark(text, 1);
	synced(&lsp, &loop, text);
	assert(file_has(copy, arrview_str("int X;\nlong w;\nlong v = 1;\nint z;\n")));
	assert(file_has(changes, arrview_str("lines\n")));
	assert(text_replace(text, text->size, text->size, arrview_str("int u;")));
	assert(lsp_change(&lsp, "/project dir/a.c", 3, text, 1));
	text_mark(text, 1);
	synced(&lsp, &loop, text);
	assert(file_has(copy, arrview_str("int X;\nlong w;\nlong v = 1;\nint z;\nint u;")));
	assert(file_has(changes, arrview_str("lines\ntext\n")));
	assert(text_replace(text, text_line_start(text, 1), text_line_start(text, 3), arrview_first(none, "")));
	assert(lsp_change(&lsp, "/project dir/a.c", 4, text, 1));
	synced(&lsp, &loop, text);
	assert(file_has(copy, arrview_str("int X;\nint z;\nint u;")) && file_has(changes, arrview_str("lines\ntext\nlines\n")));
	unlink(copy);
	unlink(changes);
	rmdir(directory);
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

static bool token_is(size_t index, size_t line, size_t column, size_t length, enum syntax_class class)
{
	const struct lsp_token *token = &result.tokens[index];

	return token->line == line && token->column == column && token->length == length && token->class == class;
}

/*
 * Tokens have the classes of the types of the legend of the server, in bytes. Types that keep the lexical highlighting
 * don't come. A save comes with the text that it saved if the server asks for it, then the server can ask for all
 * tokens again.
 */
TEST(lsp_highlights)
{
	static struct lsp lsp;
	static struct sample sample;
	static const char source[] = "int main(void)\n{\n\treturn x;\n}\n";
	const char *argv[] = { server(), nullptr };
	char directory[] = "/tmp/lsp-test-XXXXXX", path[PATH_MAX];
	struct poor_loop loop;
	FILE *file;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	sample_init(&sample, source);
	result.text = &sample.text;
	result.answers = 0;
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	assert(!lsp_highlight(&lsp, "/project dir/a.c"));
	run_until(&loop, lsp.state == LSP_READY);
	assert(lsp.highlights && lsp_open(&lsp, "/project dir/a.c", "c", 1, &sample.text));
	assert(lsp_highlight(&lsp, "/project dir/a.c"));
	run_until(&loop, result.answers == 1);
	assert(result.found && result.token_count == 4 && token_is(0, 0, 0, 3, SYNTAX_TYPE) &&
	       token_is(1, 0, 4, 4, SYNTAX_FUNCTION) && token_is(2, 2, 8, 1, SYNTAX_MACRO) && token_is(3, 3, 0, 1, SYNTAX_INACTIVE));
	assert(mkdtemp(directory));
	snprintf_array(path, "%s/saved file.c", directory);
	assert((file = fopen(path, "w")) && fputs(source, file) >= 0 && !fclose(file));
	result.refreshes = 0;
	assert(lsp.save_text);
	lsp_save(&lsp, path, arrview_str("int main(void)\n{\n\treturn y;\n}\n"));
	lsp_save(&lsp, path, arrview_str(source));
	assert(lsp_highlight(&lsp, "/project dir/a.c"));
	run_until(&loop, result.answers == 2);
	assert(result.refreshes == 1);
	unlink(path);
	rmdir(directory);
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/*
 * Variables are local, unless the modifiers of the legend tell that they last as long as the program: then they are
 * global, or constant if they are read only too
 */
TEST(lsp_tells_variables_apart)
{
	static struct lsp lsp;
	static struct sample sample;
	const char *argv[] = { server(), "variables", nullptr };
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	sample_init(&sample, "f(p, l, c, s, g, k);\n");
	result.text = &sample.text;
	result.answers = 0;
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	run_until(&loop, lsp.state == LSP_READY);
	assert(lsp_open(&lsp, "/project dir/a.c", "c", 1, &sample.text) && lsp_highlight(&lsp, "/project dir/a.c"));
	run_until(&loop, result.answers == 1);
	assert(result.found && result.token_count == 7 && token_is(0, 0, 0, 1, SYNTAX_FUNCTION) &&
	       token_is(1, 0, 2, 1, SYNTAX_PARAMETER) && token_is(2, 0, 5, 1, SYNTAX_LOCAL));
	assert(token_is(3, 0, 8, 1, SYNTAX_LOCAL) && token_is(4, 0, 11, 1, SYNTAX_GLOBAL) &&
	       token_is(5, 0, 14, 1, SYNTAX_GLOBAL) && token_is(6, 0, 17, 1, SYNTAX_CONSTANT));
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/*
 * A document that doesn't fit beside those that wait for the server isn't sent, until they are. Documents leave room
 * for closing them.
 */
TEST(lsp_fills_up)
{
	static struct lsp lsp;
	static struct {
		struct text text;
		char bytes[47 << 18];
		uint32_t lines[2];
		uint8_t states[2];
	} big;
	const char *argv[] = { server(), nullptr };
	struct poor_loop loop;
	size_t filled;
	bool fits;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	text_init(&big.text, big.bytes, big.lines, big.states);
	memset(big.bytes, 'a', sizeof(big.bytes));
	text_begin(&big.text, sizeof(big.bytes));
	assert(text_index(&big.text, SIZE_MAX, &fits) && fits);
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	run_until(&loop, lsp.state == LSP_READY && !lsp.write_op.pending);
	assert(lsp_open(&lsp, "/project dir/big.c", "c", 1, &big.text));
	assert(lsp_change(&lsp, "/project dir/big.c", 2, &big.text, 0) &&
	       !lsp_change(&lsp, "/project dir/big.c", 3, &big.text, 0));
	filled = lsp.out_size[lsp.fill];
	lsp_close(&lsp, "/project dir/big.c");
	assert(lsp.out_size[lsp.fill] > filled);
	result.writes = 0;
	run_until(&loop, !lsp.write_op.pending);
	assert(result.writes);
	assert(lsp_open(&lsp, "/project dir/big.c", "c", 3, &big.text));
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_EXITED);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/* The server gets invalid UTF-8 as U+FFFD of three bytes, and positions count it so */
TEST(lsp_counts_replacements)
{
	static struct lsp lsp;
	static struct sample sample;
	const char *argv[] = { server(), nullptr };
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	sample_init(&sample, "\xff" "x = y;\n");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	run_until(&loop, lsp.state == LSP_READY);
	assert(!lsp.utf16 && lsp_offset(&lsp, &sample.text, 0, 3) == 1 && lsp_offset(&lsp, &sample.text, 0, 7) == 5);
	assert(lsp_open(&lsp, "/project dir/a.c", "c", 1, &sample.text));
	run_until(&loop, !lsp.write_op.pending);
	assert(lsp_locate(&lsp, "/project dir/a.c", &sample.text, 5, false));
	assert(memmem(lsp.memory->out[lsp.fill ^ 1], lsp.out_size[lsp.fill ^ 1], "\"character\":7}", 14));
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/* A server that exits before its answer to the initialization is read doesn't get ready */
TEST(lsp_exits_before_answers)
{
	static struct lsp lsp;
	const char *argv[] = { server(), "orphan", nullptr };
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/", argv);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_EXITED);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/* A server that exits is done with, even if a process it left holds its output */
TEST(lsp_leaves_what_the_server_left)
{
	static struct lsp lsp;
	const char *argv[] = { server(), "linger", nullptr };
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/", argv);
	run_until(&loop, lsp.state == LSP_READY);
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_EXITED);
	kill(-lsp.pid, SIGKILL);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/* A server that stays after it is told to exit is killed soon, long before one that doesn't answer the shutdown */
TEST(lsp_ends_servers_that_stay)
{
	static struct lsp lsp;
	const char *argv[] = { server(), "stay", nullptr };
	struct poor_loop loop;
	uint64_t start;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/", argv);
	run_until(&loop, lsp.state == LSP_READY);
	start = poor_loop_now();
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_EXITED && poor_loop_now() - start < LSP_STOP_NS / 2);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}

/* A server that isn't there is off, one that doesn't shut down is killed */
TEST(lsp_fails)
{
	static struct lsp lsp;
	const char *missing[] = { "/nonexistent/server", nullptr }, *hanging[] = { server(), "hang", nullptr };
	struct poor_loop loop;

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/", missing);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_OFF);
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/", hanging);
	run_until(&loop, lsp.state == LSP_READY);
	lsp_stop(&lsp);
	run_until(&loop, !lsp_busy(&lsp));
	assert(lsp.state == LSP_EXITED);
	run_until(&loop, !log_pending());
	loop_fini(&loop);
}
