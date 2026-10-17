/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "lsp.h"
#include "test-loop.h"
#include "test-runner.h"

/* Where the last request led */
static struct {
	uint64_t id;
	size_t line, character;
	unsigned states, locations;
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

static struct lsp_memory memory;

static const struct lsp_listener listener = {
	.state = state,
	.location = location,
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
	text_init(&sample->text, sample->bytes, sample->lines, sample->states);
	copy_array(sample->bytes, array_ptr(contents, strlen(contents)));
	assert(text_load(&sample->text, strlen(contents)));
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
	assert(lsp_change(&lsp, "/project dir/a.c", 2, &sample.text));
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

	loop_init(&loop);
	log_init(&loop, "lsp-test");
	text_init(&big.text, big.bytes, big.lines, big.states);
	memset(big.bytes, 'a', sizeof(big.bytes));
	assert(text_load(&big.text, sizeof(big.bytes)));
	lsp_init(&lsp, &loop, &listener, &memory);
	lsp_start(&lsp, "/project dir", argv);
	run_until(&loop, lsp.state == LSP_READY && !lsp.write_op.pending);
	assert(lsp_open(&lsp, "/project dir/big.c", "c", 1, &big.text));
	assert(lsp_change(&lsp, "/project dir/big.c", 2, &big.text) && !lsp_change(&lsp, "/project dir/big.c", 3, &big.text));
	filled = lsp.out_size[lsp.fill];
	lsp_close(&lsp, "/project dir/big.c");
	assert(lsp.out_size[lsp.fill] > filled);
	run_until(&loop, !lsp.write_op.pending);
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
