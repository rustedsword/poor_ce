/* SPDX-License-Identifier: MIT */
#ifndef LSP_H
#define LSP_H

#include <limits.h>
#include <poor_loop_fd.h>
#include <signal.h>
#include <sys/types.h>

#include "json.h"
#include "syntax.h"
#include "text.h"

constexpr size_t LSP_OUT_BYTES = 24 << 20;
constexpr size_t LSP_IN_BYTES = 1 << 20;
constexpr uint64_t LSP_STOP_NS = 2'000'000'000;
constexpr uint64_t LSP_EXIT_NS = 100'000'000;
constexpr size_t LSP_LEGEND = 64;

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
	/* The answer to the last lsp_highlight(): if 'found', lsp_token() returns its tokens while this runs */
	void (*tokens)(struct lsp *lsp, bool found);
	/* The tokens of any document may have changed, like after a change of a header */
	void (*refresh)(struct lsp *lsp);
	/* A buffer of messages was written, there is room for more */
	void (*written)(struct lsp *lsp);
};

/* A token of the server: its line, byte column and length, and its class */
struct lsp_token {
	size_t line, column, length;
	enum syntax_class class;
};

/* Messages to the server fill one buffer of 'out' while the other one is written, messages from it go to 'in' */
struct lsp_memory {
	char out[2][LSP_OUT_BYTES];
	char in[LSP_IN_BYTES];
};

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
	uint64_t next_id, initialize_id, shutdown_id, highlight_id;
	size_t out_size[2], written, in_size, skip;
	unsigned fill, pipes_made;
	/* The class of each token type of the server, SYNTAX_CLASSES for types that keep the lexical highlighting */
	enum syntax_class legend[LSP_LEGEND];
	/* The bits of the token modifiers of the server that make a variable of a class */
	uint32_t modifiers[SYNTAX_CLASSES];
	/* The answer that lsp_token() reads: where the last token was and how far its line was walked */
	const char *answer;
	size_t answer_size, token_at, token_line, token_character, walk_line, walk_offset, walk_units;
	struct json token_data;
	bool utf16, highlights, saves, save_text, incremental, writer_open, reader_open, holding, running, stopping;
	char root[PATH_MAX];
	char path[PATH_MAX];
};

void lsp_init(struct lsp *lsp, struct poor_loop *loop, const struct lsp_listener *listener, struct lsp_memory *memory);
/* Run the server 'argv' for the project at the absolute path 'root' */
void lsp_start(struct lsp *lsp, const char *root, const char *const *argv);
/* Returns whether the document goes to the server, it doesn't while there is no room for it */
[[nodiscard]] bool lsp_open(struct lsp *lsp, const char *path, const char *language, uint64_t version, const struct text *text);
[[nodiscard]] bool lsp_change(struct lsp *lsp, const char *path, uint64_t version, const struct text *text, size_t mark);
void lsp_close(struct lsp *lsp, const char *path);
/* Tell the server that the document was saved with 'text', so that it reparses the files that include it */
void lsp_save_impl(struct lsp *lsp, const char *path, size_t size, const char (*text)[size]);
#define lsp_save(lsp, path, text) lsp_save_impl(lsp, path, ARRAY_SIZE(text), &auto_arr(text))
/* Ask where the symbol at 'offset' is defined or declared. Returns the id of the request, 0 if it wasn't sent. */
uint64_t lsp_locate(struct lsp *lsp, const char *path, const struct text *text, size_t offset, bool declaration);
/* The offset of a position from the server */
size_t lsp_offset(const struct lsp *lsp, const struct text *text, size_t line, size_t character);
/* Ask for the semantic tokens of a document, for the 'tokens' listener. Returns whether the request was sent. */
[[nodiscard]] bool lsp_highlight(struct lsp *lsp, const char *path);
/* The next token of the answer, in bytes of 'text', the document it is for */
[[nodiscard]] bool lsp_token(struct lsp *lsp, const struct text *text, struct lsp_token *token);
/* Shut the server down, wait until lsp_busy() is false before the memory goes */
void lsp_stop(struct lsp *lsp);
bool lsp_busy(const struct lsp *lsp);

#endif
