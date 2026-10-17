/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <spawn.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "logger.h"
#include "lsp.h"

constexpr size_t HEADER_BYTES = 32;
constexpr size_t SMALL_BYTES = 1 << 20;
constexpr size_t URI_BYTES = 3 * PATH_MAX + 8;

poor_loop_group_attach(lsp_output, struct lsp, writer, write_op, on_write);
poor_loop_group_attach(lsp_input, struct lsp, reader, read_op, on_read);

static void set_state(struct lsp *lsp, enum lsp_state state)
{
	lsp->state = state;
	lsp->listener->state(lsp);
}

static void write_more(struct lsp *lsp)
{
	unsigned busy = lsp->fill ^ 1;
	auto unsent = arrview(lsp->written, lsp->out_size[busy] - lsp->written, lsp->memory->out[busy]);

	io_uring_prep_write_array(need_sqe(poor_loop_group_get_sqe_or_submit(lsp->loop, &lsp->writer, &lsp->write_op)),
				  lsp->writer.fd, unsent, -1);
}

static void flush(struct lsp *lsp)
{
	if (!lsp->holding || lsp->write_op.pending || !lsp->out_size[lsp->fill])
		return;
	lsp->fill ^= 1;
	lsp->written = 0;
	write_more(lsp);
}

/* The pipe closes once what is in it is written, or once the write that goes on ends if it has to close now */
static void close_writer(struct lsp *lsp, bool now)
{
	if (now) {
		lsp->out_size[0] = lsp->out_size[1] = 0;
		if (lsp->write_op.pending)
			io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(lsp->loop)), &lsp->write_op, 0);
	}
	if (!lsp->holding || (lsp->write_op.pending && !now) || lsp->out_size[0] || lsp->out_size[1])
		return;
	lsp->holding = false;
	if (poor_loop_group_release(&lsp->writer))
		poor_loop_group_call_done(lsp_output, lsp->loop, &lsp->writer);
}

POOR_LOOP_GROUP_OP(on_write, loop, writer, op, cqe)
{
	struct lsp *lsp = container_of(writer, struct lsp, writer);
	unsigned busy = lsp->fill ^ 1;

	if (cqe->res <= 0) {
		close_writer(lsp, true);
		return;
	}
	if ((lsp->written += cqe->res) < lsp->out_size[busy]) {
		write_more(lsp);
		return;
	}
	lsp->out_size[busy] = 0;
	flush(lsp);
	if (lsp->state == LSP_STOPPING)
		close_writer(lsp, false);
}

POOR_LOOP_FD_DONE(lsp_output, loop, writer)
{
	container_of(writer, struct lsp, writer)->writer_open = false;
}

/*
 * A message goes after room for its header, in the buffer that fills. A text leaves room for the small messages after
 * it, like closing all documents.
 */
static struct json_writer *message(struct lsp *lsp, bool text)
{
	size_t end = text ? LSP_OUT_BYTES - SMALL_BYTES : LSP_OUT_BYTES, start = lsp->out_size[lsp->fill] + HEADER_BYTES;

	if (start > end)
		start = end;
	json_writer_init(&lsp->message, arrview(start, end - start, lsp->memory->out[lsp->fill]));
	return &lsp->message;
}

static struct json_writer *request(struct lsp *lsp, uint64_t id, const char *method)
{
	struct json_writer *writer = message(lsp, false);

	json_printf(writer, "{\"jsonrpc\":\"2.0\",\"id\":%" PRIu64 ",\"method\":\"%s\"", id, method);
	return writer;
}

static struct json_writer *notification(struct lsp *lsp, const char *method, bool text)
{
	struct json_writer *writer = message(lsp, text);

	json_printf(writer, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"", method);
	return writer;
}

static bool post(struct lsp *lsp)
{
	auto buffer = &lsp->memory->out[lsp->fill];
	size_t start = lsp->out_size[lsp->fill], size = lsp->message.size, length;
	char header[HEADER_BYTES];

	if (lsp->message.overflow) {
		log_printf("poor_text_viewer: a message to %s doesn't fit\n", lsp->argv[0]);
		return false;
	}
	length = snprintf_array(header, "Content-Length: %zu\r\n\r\n", size);
	auto body = arrview(start + HEADER_BYTES, size, buffer);
	memmove(arrview(start + length, size, buffer), body, ARRAY_SIZE_BYTES(body));
	copy_array(arrview(start, length, buffer), arrview_first(length, header));
	lsp->out_size[lsp->fill] += length + size;
	flush(lsp);
	return true;
}

/* A file:// URI of an absolute path: bytes other than unreserved ones and slashes are escaped */
static void uri(struct json_writer *writer, const char *path)
{
	char escaped[URI_BYTES];
	size_t length = 0;

	foreach_array_const_ref(array_ptr(path, strlen(path)), c) {
		unsigned char u = *c;

		if (length + 4 > ARRAY_SIZE(escaped))
			break;
		if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || strchr("/-._~", u))
			escaped[length++] = u;
		else
			length += snprintf_array(arrview_cfront(length, escaped), "%%%02X", u);
	}
	escaped[length] = 0;
	json_printf(writer, "\"file://%s\"", escaped);
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
		return (c | 0x20) - 'a' + 10;
	return -1;
}

static const char *uri_path(struct lsp *lsp, const char *uri)
{
	size_t length = 0;

	if (strncmp(uri, "file://", 7))
		return nullptr;
	for (const char *c = uri + 7; *c && length + 1 < ARRAY_SIZE(lsp->path); c++) {
		if (*c == '%' && hex_digit(c[1]) >= 0 && hex_digit(c[2]) >= 0) {
			lsp->path[length++] = hex_digit(c[1]) << 4 | hex_digit(c[2]);
			c += 2;
		} else {
			lsp->path[length++] = *c;
		}
	}
	lsp->path[length] = 0;
	return lsp->path;
}

/* Positions the server can take: UTF-8 if it can, UTF-16 otherwise */
static void initialize(struct lsp *lsp)
{
	struct json_writer *writer;

	lsp->initialize_id = lsp->next_id++;
	writer = request(lsp, lsp->initialize_id, "initialize");
	json_printf(writer, ",\"params\":{\"processId\":%d,\"clientInfo\":{\"name\":\"poor_text_viewer\"},\"rootUri\":", getpid());
	uri(writer, lsp->root);
	json_printf(writer, ",\"capabilities\":{\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
			    "\"textDocument\":{\"definition\":{\"linkSupport\":false},\"declaration\":{\"linkSupport\":false}}}}}");
	post(lsp);
}

static void initialized(struct lsp *lsp, size_t size, const char (*body)[size], struct json root)
{
	struct json result, capabilities, encoding;

	if (!json_member(body, root, "result", &result)) {
		log_printf("poor_text_viewer: %s didn't start\n", lsp->argv[0]);
		lsp_stop(lsp);
		return;
	}
	lsp->utf16 = !json_member(body, result, "capabilities", &capabilities) ||
		     !json_member(body, capabilities, "positionEncoding", &encoding) || !json_is(body, encoding, "\"utf-8\"");
	json_printf(notification(lsp, "initialized", false), ",\"params\":{}}");
	post(lsp);
	set_state(lsp, LSP_READY);
}

/* A result is a location, or a list of locations or of links to them, of which the first one counts */
static void located(struct lsp *lsp, uint64_t id, size_t size, const char (*body)[size], struct json root)
{
	struct json result, location, link, range, start, value;
	int64_t line, character;
	const char *path = nullptr;
	char text[URI_BYTES];

	if (!json_member(body, root, "result", &result))
		result = root;
	if (!json_element(body, result, 0, &location))
		location = result;
	if (json_member(body, location, "uri", &link)
		    ? json_member(body, location, "range", &range)
		    : json_member(body, location, "targetUri", &link) &&
			      json_member(body, location, "targetSelectionRange", &range)) {
		if (json_member(body, range, "start", &start) && json_member(body, start, "line", &value) &&
		    json_integer(body, value, &line) && json_member(body, start, "character", &value) &&
		    json_integer(body, value, &character) && line >= 0 && character >= 0 &&
		    json_string(body, link, text) != SIZE_MAX)
			path = uri_path(lsp, text);
	}
	lsp->listener->location(lsp, id, path, path ? line : 0, path ? character : 0);
}

/* Requests of the server get no result */
static void dispatch(struct lsp *lsp, size_t size, const char (*body)[size])
{
	struct json root, id, method;
	int64_t number;

	if (!json_parse(body, &root) || !json_member(body, root, "id", &id))
		return;
	if (json_member(body, root, "method", &method)) {
		json_printf(message(lsp, false), "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":null}", (int)(id.end - id.start),
			    &(*body)[id.start]);
		post(lsp);
		return;
	}
	if (!json_integer(body, id, &number))
		return;
	if ((uint64_t)number == lsp->initialize_id) {
		initialized(lsp, size, body, root);
	} else if ((uint64_t)number == lsp->shutdown_id) {
		json_printf(notification(lsp, "exit", false), "}");
		post(lsp);
		set_state(lsp, LSP_STOPPING);
		close_writer(lsp, false);
	} else {
		located(lsp, number, size, body, root);
	}
}

static size_t content_length(size_t size, const char (*header)[size])
{
	static constexpr char name[] = "Content-Length:";
	constexpr size_t name_size = sizeof(name) - 1;
	size_t at = 0, value;

	while (at < size) {
		auto line = arrview_cfront(at, header);
		const char *end = memmem(*line, ARRAY_SIZE(line), "\r\n", 2);
		size_t length = end ? (size_t)(end - *line) : ARRAY_SIZE(line);

		if (length > name_size && !strncasecmp(*line, name, name_size)) {
			value = 0;
			foreach_array_const_ref(arrview(name_size, length - name_size, line), c) {
				if (*c >= '0' && *c <= '9' && value < SIZE_MAX / 10)
					value = value * 10 + (*c - '0');
				else if (*c != ' ')
					return SIZE_MAX;
			}
			return value;
		}
		at += length + 2;
	}
	return SIZE_MAX;
}

/* Messages that will never fit are skipped as they come */
static void receive(struct lsp *lsp)
{
	size_t used = 0, header, length;

	while (used < lsp->in_size) {
		auto rest = arrview(used, lsp->in_size - used, lsp->memory->in);
		const char *end;

		if (lsp->skip) {
			length = lsp->skip < ARRAY_SIZE(rest) ? lsp->skip : ARRAY_SIZE(rest);
			lsp->skip -= length;
			used += length;
			continue;
		}
		end = memmem(*rest, ARRAY_SIZE(rest), "\r\n\r\n", 4);
		if (!end) {
			if (ARRAY_SIZE(rest) == ARRAY_SIZE(lsp->memory->in))
				used = lsp->in_size;
			break;
		}
		header = end + 4 - *rest;
		length = content_length(header - 4, arrview_first(header - 4, rest));
		if (length == SIZE_MAX || length > ARRAY_SIZE(lsp->memory->in) - header) {
			used += header;
			lsp->skip = length == SIZE_MAX ? 0 : length;
			continue;
		}
		if (ARRAY_SIZE(rest) - header < length)
			break;
		dispatch(lsp, length, arrview(header, length, rest));
		used += header + length;
	}
	auto kept = arrview(used, lsp->in_size - used, lsp->memory->in);
	memmove(arrview_first(ARRAY_SIZE(kept), lsp->memory->in), kept, ARRAY_SIZE_BYTES(kept));
	lsp->in_size -= used;
}

static void read_more(struct lsp *lsp)
{
	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(lsp->loop, &lsp->reader, &lsp->read_op)),
				 lsp->reader.fd, arrview_cfront(lsp->in_size, lsp->memory->in), -1);
}

/* What is read after the server exited comes too late */
POOR_LOOP_GROUP_OP(on_read, loop, reader, op, cqe)
{
	struct lsp *lsp = container_of(reader, struct lsp, reader);

	if (cqe->res <= 0 || lsp->state == LSP_EXITED)
		return;
	lsp->in_size += cqe->res;
	receive(lsp);
	read_more(lsp);
}

POOR_LOOP_FD_DONE(lsp_input, loop, reader)
{
	container_of(reader, struct lsp, reader)->reader_open = false;
}

/* The pipes close too: the server can't hang on its input, and processes it left may hold its output */
static void on_wait(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *)
{
	struct lsp *lsp = container_of(op, struct lsp, wait_op);

	lsp->running = false;
	poor_loop_timer_disarm(&lsp->timer);
	if (!lsp->stopping)
		log_printf("poor_text_viewer: %s exited\n", lsp->argv[0]);
	close_writer(lsp, true);
	if (lsp->read_op.pending)
		io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(lsp->loop)), &lsp->read_op, 0);
	set_state(lsp, LSP_EXITED);
}

/* kill() is the one way to end a server that hangs, io_uring can't send signals */
static void on_timeout(struct poor_loop *, struct poor_loop_timer *timer)
{
	struct lsp *lsp = container_of(timer, struct lsp, timer);

	if (lsp->running)
		kill(lsp->pid, SIGKILL);
	close_writer(lsp, true);
}

/*
 * posix_spawnp() is the one way to run the server, io_uring can't. It gets the signals that the app blocks or
 * ignores, and a process group of its own, so that Ctrl+C in a terminal leaves it to the app.
 */
static void spawn(struct lsp *lsp)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t none, defaults;
	int ret;

	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, lsp->pipes[0][0], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&actions, lsp->pipes[1][1], STDOUT_FILENO);
	posix_spawnattr_init(&attributes);
	sigemptyset(&none);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGPIPE);
	posix_spawnattr_setsigmask(&attributes, &none);
	posix_spawnattr_setsigdefault(&attributes, &defaults);
	posix_spawnattr_setpgroup(&attributes, 0);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
	ret = posix_spawnp(&lsp->pid, lsp->argv[0], &actions, &attributes, (char *const *)lsp->argv, environ);
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	poor_loop_close_fd_or_submit(lsp->loop, lsp->pipes[0][0], 0);
	poor_loop_close_fd_or_submit(lsp->loop, lsp->pipes[1][1], 0);
	if (ret) {
		log_printf("poor_text_viewer: %s: %s\n", lsp->argv[0], strerror(ret));
		poor_loop_close_fd_or_submit(lsp->loop, lsp->pipes[0][1], 0);
		poor_loop_close_fd_or_submit(lsp->loop, lsp->pipes[1][0], 0);
		set_state(lsp, LSP_OFF);
		return;
	}
	lsp->running = true;
	io_uring_prep_waitid(need_sqe(poor_loop_get_sqe_or_submit(lsp->loop, &lsp->wait_op)), P_PID, lsp->pid, &lsp->info,
			     WEXITED, 0);
	poor_loop_fd_init(&lsp->writer, lsp->pipes[0][1]);
	poor_loop_group_acquire(&lsp->writer);
	lsp->writer_open = lsp->holding = true;
	poor_loop_fd_init(&lsp->reader, lsp->pipes[1][0]);
	lsp->reader_open = true;
	read_more(lsp);
	initialize(lsp);
}

/* Two pipes, one after the other: the server reads the first one and writes the second one */
static void on_pipe(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct lsp *lsp = container_of(op, struct lsp, pipe_op);

	if (cqe->res < 0)
		log_printf("poor_text_viewer: pipe: %s\n", strerror(-cqe->res));
	else
		lsp->pipes_made++;
	if (cqe->res >= 0 && lsp->pipes_made < 2 && !lsp->stopping) {
		io_uring_prep_pipe(need_sqe(poor_loop_get_sqe_or_submit(lsp->loop, &lsp->pipe_op)), lsp->pipes[1], O_CLOEXEC);
		return;
	}
	if (cqe->res >= 0 && !lsp->stopping) {
		spawn(lsp);
		return;
	}
	foreach_array_const_ref(arrview_first(lsp->pipes_made, lsp->pipes), pipe)
		foreach_array_const_ref(pipe, fd)
			poor_loop_close_fd_or_submit(lsp->loop, *fd, 0);
	set_state(lsp, LSP_OFF);
}

void lsp_init(struct lsp *lsp, struct poor_loop *loop, const struct lsp_listener *listener, struct lsp_memory *memory)
{
	lsp->loop = loop;
	lsp->listener = listener;
	lsp->memory = memory;
	poor_loop_op_init(&lsp->pipe_op, on_pipe);
	poor_loop_op_init(&lsp->write_op, on_write);
	poor_loop_op_init(&lsp->read_op, on_read);
	poor_loop_op_init(&lsp->wait_op, on_wait);
	poor_loop_timer_init(&lsp->timer, on_timeout);
	lsp->argv = nullptr;
	lsp->state = LSP_OFF;
	lsp->next_id = 1;
	lsp->initialize_id = lsp->shutdown_id = 0;
	lsp->out_size[0] = lsp->out_size[1] = lsp->written = lsp->in_size = lsp->skip = 0;
	lsp->fill = lsp->pipes_made = 0;
	lsp->utf16 = lsp->writer_open = lsp->reader_open = lsp->holding = lsp->running = lsp->stopping = false;
}

void lsp_start(struct lsp *lsp, const char *root, const char *const *argv)
{
	snprintf_array(lsp->root, "%s", root);
	lsp->argv = argv;
	set_state(lsp, LSP_STARTING);
	io_uring_prep_pipe(need_sqe(poor_loop_get_sqe_or_submit(lsp->loop, &lsp->pipe_op)), lsp->pipes[0], O_CLOEXEC);
}

static void document(struct json_writer *writer, const char *path)
{
	json_printf(writer, "{\"uri\":");
	uri(writer, path);
}

bool lsp_open(struct lsp *lsp, const char *path, const char *language, uint64_t version, const struct text *text)
{
	struct json_writer *writer;

	if (lsp->state != LSP_READY)
		return false;
	writer = notification(lsp, "textDocument/didOpen", true);
	json_printf(writer, ",\"params\":{\"textDocument\":");
	document(writer, path);
	json_printf(writer, ",\"languageId\":\"%s\",\"version\":%" PRIu64 ",\"text\":", language, version);
	json_quote(writer, arrview_first(text->size, array_ptr(text->bytes, text->capacity)));
	json_printf(writer, "}}}");
	return post(lsp);
}

bool lsp_change(struct lsp *lsp, const char *path, uint64_t version, const struct text *text)
{
	struct json_writer *writer;

	if (lsp->state != LSP_READY)
		return false;
	writer = notification(lsp, "textDocument/didChange", true);
	json_printf(writer, ",\"params\":{\"textDocument\":");
	document(writer, path);
	json_printf(writer, ",\"version\":%" PRIu64 "},\"contentChanges\":[{\"text\":", version);
	json_quote(writer, arrview_first(text->size, array_ptr(text->bytes, text->capacity)));
	json_printf(writer, "}]}}");
	return post(lsp);
}

void lsp_close(struct lsp *lsp, const char *path)
{
	struct json_writer *writer;

	if (lsp->state != LSP_READY)
		return;
	writer = notification(lsp, "textDocument/didClose", false);
	json_printf(writer, ",\"params\":{\"textDocument\":");
	document(writer, path);
	json_printf(writer, "}}}");
	post(lsp);
}

/* The code units of a character for the server, which got invalid UTF-8 as U+FFFD */
static size_t units(const struct lsp *lsp, uint32_t codepoint, size_t length)
{
	if (lsp->utf16)
		return codepoint >= 0x10000 ? 2 : 1;
	return codepoint == TEXT_REPLACEMENT && length == 1 ? 3 : length;
}

uint64_t lsp_locate(struct lsp *lsp, const char *path, const struct text *text, size_t offset, bool declaration)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t line = text_line_of(text, offset), at = text_line_start(text, line), character = 0, length;
	struct json_writer *writer;
	uint64_t id;

	if (lsp->state != LSP_READY)
		return 0;
	for (; at < offset; at += length) {
		uint32_t codepoint = utf8_decode(arrview(at, offset - at, all), &length);

		character += units(lsp, codepoint, length);
	}
	id = lsp->next_id++;
	writer = request(lsp, id, declaration ? "textDocument/declaration" : "textDocument/definition");
	json_printf(writer, ",\"params\":{\"textDocument\":");
	document(writer, path);
	json_printf(writer, "},\"position\":{\"line\":%zu,\"character\":%zu}}}", line, character);
	return post(lsp) ? id : 0;
}

size_t lsp_offset(const struct lsp *lsp, const struct text *text, size_t line, size_t character)
{
	auto all = array_ptr(text->bytes, text->size);
	size_t start, end, at, counted = 0, length;

	if (line >= text->line_count)
		return text->size;
	start = text_line_start(text, line);
	end = text_line_end(text, line);
	for (at = start; at < end && counted < character; at += length) {
		uint32_t codepoint = utf8_decode(arrview(at, end - at, all), &length);

		counted += units(lsp, codepoint, length);
	}
	return at;
}

/* A server that doesn't exit in time is killed */
void lsp_stop(struct lsp *lsp)
{
	if (lsp->stopping)
		return;
	lsp->stopping = true;
	if (!lsp->running)
		return;
	lsp->shutdown_id = lsp->next_id++;
	json_printf(request(lsp, lsp->shutdown_id, "shutdown"), "}");
	if (!post(lsp))
		close_writer(lsp, true);
	poor_loop_timer_arm(lsp->loop, &lsp->timer, poor_loop_now() + LSP_STOP_NS);
}

bool lsp_busy(const struct lsp *lsp)
{
	return lsp->pipe_op.pending || lsp->writer_open || lsp->reader_open || lsp->running || poor_loop_timer_armed(&lsp->timer);
}
