/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_array.h>
#include <poor_stdio.h>
#include <poor_unistd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A language server for the tests, on stdin and stdout. After the first document it asks the client something and
 * sends a notification that is too big for it. Definitions are at line 2, character 4 of "tar get.h" in the root,
 * declarations at line 5, character 1, as a link. Messages that are too big for it are skipped. With "hang" it doesn't
 * shut down. With "orphan" it exits once it is asked to initialize, a child of it answers later. With "linger" a child
 * of it holds its output for a while after it exits.
 */
static char message[1 << 16];
static char root[4096];
static char big[(1 << 20) + 4096];

static size_t receive(void)
{
	char header[256];
	size_t length = 0, size;

	for (;;) {
		if (read(STDIN_FILENO, &header[length], 1) != 1)
			exit(0);
		if (++length >= 4 && !memcmp(&header[length - 4], "\r\n\r\n", 4))
			break;
		if (length == sizeof(header))
			exit(1);
	}
	header[length] = 0;
	size = strtoul(strstr(header, "Content-Length: ") + 16, nullptr, 10);
	for (length = 0; length < size;) {
		size_t at = size < sizeof(message) ? length : 0, left = size - length;
		ssize_t n = read(STDIN_FILENO, &message[at], left < sizeof(message) - at ? left : sizeof(message) - at);

		if (n <= 0)
			exit(1);
		length += n;
	}
	message[size < sizeof(message) ? size : 0] = 0;
	return size;
}

static void put(const char *bytes)
{
	for (size_t length = strlen(bytes), done = 0; done < length;) {
		ssize_t n = write(STDOUT_FILENO, bytes + done, length - done);

		if (n <= 0)
			exit(1);
		done += n;
	}
}

static void respond(const char *body)
{
	char header[64];

	snprintf_array(header, "Content-Length: %zu\r\n\r\n", strlen(body));
	put(header);
	put(body);
}

static const char *method(void)
{
	static char name[128];
	const char *start = strstr(message, "\"method\":\"");
	size_t length;

	if (!start)
		return "";
	start += 10;
	length = strcspn(start, "\"");
	snprintf_array(name, "%.*s", (int)length, start);
	return name;
}

static long id(void)
{
	const char *start = strstr(message, "\"id\":");

	return start ? strtol(start + 5, nullptr, 10) : -1;
}

int main(int argc, char **argv)
{
	bool hang = argc > 1 && !strcmp(argv[1], "hang"), orphan = argc > 1 && !strcmp(argv[1], "orphan");
	bool linger = argc > 1 && !strcmp(argv[1], "linger"), asked = false;
	char body[8192];

	for (;;) {
		receive();
		if (!strcmp(method(), "initialize")) {
			const char *uri = strstr(message, "\"rootUri\":\"") + 11;

			snprintf_array(root, "%.*s", (int)strcspn(uri, "\""), uri);
			snprintf_array(body, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"capabilities\":{\"positionEncoding\":\"utf-8\"}}}",
				       id());
			if (orphan && fork())
				return 0;
			if (orphan)
				usleep(100'000);
			respond(body);
			if (linger && !fork()) {
				close(STDERR_FILENO);
				sleep(10);
				return 0;
			}
		} else if (!strcmp(method(), "textDocument/didOpen") && !asked) {
			asked = true;
			respond("{\"jsonrpc\":\"2.0\",\"id\":\"question\",\"method\":\"workspace/configuration\",\"params\":{}}");
			snprintf_array(big, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":\"%*s\"}",
				       1 << 20, "");
			respond(big);
		} else if (!strcmp(method(), "textDocument/definition")) {
			snprintf_array(body, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":[{\"uri\":\"%s/tar%%20get.h\",\"range\":{"
				       "\"start\":{\"line\":2,\"character\":4},\"end\":{\"line\":2,\"character\":9}}}]}", id(), root);
			respond(body);
		} else if (!strcmp(method(), "textDocument/declaration")) {
			snprintf_array(body, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":[{\"targetUri\":\"%s/tar%%20get.h\","
				       "\"targetRange\":{\"start\":{\"line\":5,\"character\":0},\"end\":{\"line\":6,\"character\":0}},"
				       "\"targetSelectionRange\":{\"start\":{\"line\":5,\"character\":1},\"end\":{\"line\":5,\"character\":2}}}]}",
				       id(), root);
			respond(body);
		} else if (!strcmp(method(), "shutdown") && !hang) {
			snprintf_array(body, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id());
			respond(body);
		} else if (!strcmp(method(), "exit")) {
			return 0;
		} else if (strstr(message, "\"id\":\"question\"") && !strstr(message, "\"result\":null")) {
			return 1;
		}
	}
}
