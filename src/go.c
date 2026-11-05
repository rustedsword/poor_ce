/* SPDX-License-Identifier: MIT */
#include "go.h"

void go_init(struct go *go, struct go_memory *memory)
{
	document_init(&go->query, memory->bytes, memory->lines, memory->states, memory->edits, memory->history);
	go->query.literal = true;
}

void go_start(struct go *go, struct document *target)
{
	const struct view *view = &target->view;

	go->target = target;
	go->anchor = view->anchor;
	go->caret = view->caret;
	go->goal = target->goal;
	go->top = view->top;
	go->left = view->left;
	document_select_all(&go->query);
	document_erase(&go->query, false, false);
	go->version = go->query.version;
}

/* The line of the query, which numbers them from 1, or SIZE_MAX for none */
static size_t line_of(const struct go *go)
{
	const struct text *query = &go->query.text;
	size_t count = go->target->text.line_count, number = 0;

	foreach_array_const_ref(arrview_first(query->size, array_ptr(query->bytes, query->capacity)), digit) {
		number = number * 10 + (*digit - '0');
		if (number > count)
			number = count;
	}
	return number ? number - 1 : SIZE_MAX;
}

/* A line that didn't show goes to the upper third, like after a jump */
void go_update(struct go *go)
{
	struct document *target = go->target;
	struct view *view = &target->view;
	size_t top = view->top, rows = view_rows(view), line, start;

	if (go->query.version == go->version)
		return;
	go->version = go->query.version;
	line = line_of(go);
	if (line == SIZE_MAX) {
		go_back(go);
		return;
	}
	start = text_line_start(&target->text, line);
	document_place(target, start, false);
	if (view->top != top)
		view->top = line > rows / 3 ? line - rows / 3 : 0;
	view_reveal(view, start);
}

void go_back(struct go *go)
{
	struct view *view = &go->target->view;

	view_select(view, go->anchor, go->caret);
	go->target->goal = go->goal;
	view->top = go->top;
	view->left = go->left;
	view_scroll(view, 0, 0);
}

bool go_insert_impl(struct go *go, size_t size, const char (*bytes)[size])
{
	size_t length = 0;

	while (length < size && (*bytes)[length] >= '0' && (*bytes)[length] <= '9')
		length++;
	if (!length || (length < size && (*bytes)[length] != '\n' && (*bytes)[length] != '\r'))
		return false;
	return document_insert(&go->query, arrview_first(length, bytes), false);
}
