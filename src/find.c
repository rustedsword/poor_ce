/* SPDX-License-Identifier: MIT */
#include "find.h"

void find_init(struct find *find, struct find_memory *memory)
{
	document_init(&find->query, memory->bytes, memory->lines, memory->states, memory->edits, memory->history);
	find->query.literal = true;
	find->version = find->query.version;
	search_init(&find->search);
}

/* A match that didn't show goes to the upper third, like after a jump */
static void show_match(struct document *document, size_t match, size_t size)
{
	struct view *view = &document->view;
	size_t top = view->top, line = text_line_of(&document->text, match), rows = view_rows(view);

	document_select(document, match, match + size);
	if (view->top != top)
		view->top = line > rows / 3 ? line - rows / 3 : 0;
	view_reveal(view, match);
}

/* The first match after the start of the selection, or the last one before it, around the ends of the document */
static size_t next_match(const struct search *search, const struct document *document, bool back)
{
	const struct text *text = &document->text;
	size_t start, end, from, match;

	view_selection(&document->view, &start, &end);
	if (back) {
		match = search_backward(search, text, 0, start);
		return match != SIZE_MAX ? match : search_backward(search, text, start, text->size);
	}
	from = start + (start != end);
	match = search_forward(search, text, from, text->size);
	return match != SIZE_MAX ? match : search_forward(search, text, 0, from);
}

static void compile(struct find *find)
{
	const struct text *text = &find->query.text;

	search_init(&find->search);
	search_add(&find->search, arrview_first(text->size, array_ptr(text->bytes, text->capacity)));
	find->version = find->query.version;
}

static void recount(struct find *find)
{
	find->counted = find->count = find->index = 0;
}

bool find_insert_impl(struct find *find, size_t size, const char (*bytes)[size])
{
	struct document *query = &find->query;
	size_t start, end, fit;

	view_selection(&query->view, &start, &end);
	fit = search_fit_impl(size, bytes, query->text.capacity - query->text.size + end - start);
	return (fit || start != end) && document_insert(query, arrview_first(fit, bytes), false);
}

void find_start(struct find *find, struct document *target)
{
	const struct text *text = &target->text;
	size_t start, end;

	find->target = target;
	document_select_all(&find->query);
	view_selection(&target->view, &start, &end);
	if (start != end && text_line_of(text, start) == text_line_of(text, end)) {
		find_insert_impl(find, end - start, arrview(start, end - start, array_ptr(text->bytes, text->size)));
		document_select_all(&find->query);
		compile(find);
	}
	find->origin = start;
	find->match = end - start == find->search.size && search_at(&find->search, text, start) ? start : SIZE_MAX;
	recount(find);
}

bool find_update(struct find *find)
{
	const struct text *text = &find->target->text;

	if (find->query.version == find->version)
		return false;
	compile(find);
	find->match = search_forward(&find->search, text, find->origin, text->size);
	if (find->match == SIZE_MAX)
		find->match = search_forward(&find->search, text, 0, find->origin);
	if (find->match == SIZE_MAX)
		document_place(find->target, find->origin, false);
	else
		show_match(find->target, find->match, find->search.size);
	recount(find);
	return true;
}

/* Counted matches come in their order, the next one is after the selected one */
bool find_step(struct find *find, bool back)
{
	size_t match = next_match(&find->search, find->target, back);
	bool counted = find->match != SIZE_MAX && find_counted(find);

	if (match == SIZE_MAX)
		return false;
	find->match = find->origin = match;
	show_match(find->target, match, find->search.size);
	if (!counted)
		recount(find);
	else if (back)
		find->index = find->index > 1 ? find->index - 1 : find->count;
	else
		find->index = find->index < find->count ? find->index + 1 : 1;
	return true;
}

size_t find_again(const struct find *find, struct document *document, bool back)
{
	size_t match = next_match(&find->search, document, back);

	if (match != SIZE_MAX)
		show_match(document, match, find->search.size);
	return match;
}

bool find_count(struct find *find, size_t budget)
{
	const struct text *text = &find->target->text;
	size_t from = find->counted, end = text->size - from > budget ? from + budget : text->size;

	find->count += search_count(&find->search, text, from, end, find->match, &find->index);
	find->counted = end;
	return end == text->size;
}

bool find_counted(const struct find *find)
{
	return find->counted == find->target->text.size;
}
