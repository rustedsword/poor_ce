/* SPDX-License-Identifier: MIT */
#ifndef LSP_H
#define LSP_H

#include <limits.h>
#include <poor_loop_fd.h>
#include <signal.h>
#include <sys/types.h>

#include "json.h"
#include "text.h"

constexpr size_t LSP_OUT_BYTES = 24 << 20;
constexpr size_t LSP_IN_BYTES = 1 << 20;
constexpr uint64_t LSP_STOP_NS = 2'000'000'000;

poor_loop_fd_declare(lsp_output);
poor_loop_fd_declare(lsp_input);

enum lsp_state {
	LSP_OFF,
	LSP_STARTING,
	LSP_READY,
	LSP_STOPPING,
	LSP_EXITED,
};

struct lsp;

struct lsp_listener {
	/* Documents can be opened once the server is ready */
	void (*state)(struct lsp *lsp);
	/* Where the request 'id' leads: 'path' is absolute, or nullptr for nowhere */
	void (*location)(struct lsp *lsp, uint64_t id, const char *path, size_t line, size_t character);
};

/* Messages to the server fill one buffer of 'out' while the other one is written, messages from it go to 'in' */
struct lsp_memory {
	char out[2][LSP_OUT_BYTES];
	char in[LSP_IN_BYTES];
};

/*
 * A language server, like clangd, at the other ends of two pipes. Messages from it that don't fit are skipped.
 * Positions are in bytes, or in UTF-16 if the server can't do UTF-8.
 */
struct lsp {
	struct poor_loop *loop;
	const struct lsp_listener *listener;
	struct lsp_memory *memory;
	lsp_output writer;
	lsp_input reader;
	struct poor_loop_op pipe_op, write_op, read_op, wait_op;
	struct poor_loop_timer timer;
	struct json_writer message;
	siginfo_t info;
	pid_t pid;
	int pipes[2][2];
	const char *const *argv;
	enum lsp_state state;
	uint64_t next_id, initialize_id, shutdown_id;
	size_t out_size[2], written, in_size, skip;
	unsigned fill, pipes_made;
	bool utf16, writer_open, reader_open, holding, running, stopping;
	char root[PATH_MAX];
	char path[PATH_MAX];
};

void lsp_init(struct lsp *lsp, struct poor_loop *loop, const struct lsp_listener *listener, struct lsp_memory *memory);
/* Run the server 'argv' for the project at the absolute path 'root' */
void lsp_start(struct lsp *lsp, const char *root, const char *const *argv);
/* Returns whether the document goes to the server, it doesn't while there is no room for it */
[[nodiscard]] bool lsp_open(struct lsp *lsp, const char *path, const char *language, uint64_t version, const struct text *text);
[[nodiscard]] bool lsp_change(struct lsp *lsp, const char *path, uint64_t version, const struct text *text);
void lsp_close(struct lsp *lsp, const char *path);
/* Ask where the symbol at 'offset' is defined or declared. Returns the id of the request, 0 if it wasn't sent. */
uint64_t lsp_locate(struct lsp *lsp, const char *path, const struct text *text, size_t offset, bool declaration);
/* The offset of a position from the server */
size_t lsp_offset(const struct lsp *lsp, const struct text *text, size_t line, size_t character);
/* Shut the server down, wait until lsp_busy() is false before the memory goes */
void lsp_stop(struct lsp *lsp);
bool lsp_busy(const struct lsp *lsp);

#endif
