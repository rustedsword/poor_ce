/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poor_stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "logger.h"
#include "tree.h"

constexpr uint32_t WATCHED = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

poor_loop_group_attach(tree_directory, struct tree, directory, stat_op, on_stat);
poor_loop_group_attach(tree_watches, struct tree, watches, event_op, on_event);

static void start(struct tree *tree);

/* Directories first, then by name */
static int order(const struct tree *tree, const struct tree_node *x, const struct tree_node *y)
{
	int folded;

	if (x->directory != y->directory)
		return x->directory ? -1 : 1;
	folded = strcasecmp(&tree->names[x->name], &tree->names[y->name]);
	return folded ? folded : strcmp(&tree->names[x->name], &tree->names[y->name]);
}

static void swap(struct tree_node *x, struct tree_node *y)
{
	struct tree_node t = *x;

	*x = *y;
	*y = t;
}

/* The node at 'root' goes down the heap until its children come before it */
static void sift(const struct tree *tree, size_t count, struct tree_node (*nodes)[count], size_t root)
{
	for (size_t child; (child = 2 * root + 1) < count; root = child) {
		if (child + 1 < count && order(tree, &(*nodes)[child], &(*nodes)[child + 1]) < 0)
			child++;
		if (order(tree, &(*nodes)[root], &(*nodes)[child]) >= 0)
			return;
		swap(&(*nodes)[root], &(*nodes)[child]);
	}
}

/* A heapsort needs no memory, unlike qsort_r() of glibc */
static void sort(const struct tree *tree, size_t count, struct tree_node (*nodes)[count])
{
	for (size_t root = count / 2; root--;)
		sift(tree, count, nodes, root);
	for (size_t end = count; end > 1; end--) {
		swap(&(*nodes)[0], &(*nodes)[end - 1]);
		sift(tree, end - 1, arrview_first(end - 1, nodes), 0);
	}
}

/* Returns whether the directory waits to be read, it doesn't while it is read or if there is no room */
static bool enqueue(struct tree *tree, uint32_t node)
{
	if (tree->loading == node)
		return false;
	foreach_array_const_ref(arrview_first(tree->queued, tree->queue), queued)
		if (*queued == node)
			return true;
	if (tree->queued == TREE_QUEUE)
		return false;
	tree->queue[tree->queued++] = node;
	return true;
}

static void requeue(struct tree *tree, uint32_t from, uint32_t to)
{
	size_t kept = 0;

	foreach_array_ref(arrview_first(tree->queued, tree->queue), queued)
		if (*queued != from || to != TREE_NONE)
			tree->queue[kept++] = *queued == from ? to : *queued;
	tree->queued = kept;
}

/*
 * inotify_add_watch() and inotify_rm_watch() are the one way to watch directories, io_uring can't. A directory keeps
 * its watch, one that replaced another gets a new one.
 */
static void watch(struct tree *tree, uint32_t node)
{
	int32_t old = tree->nodes[node].watch;

	tree->nodes[node].watch = tree->watching ? inotify_add_watch(tree->watches.fd, tree->path, WATCHED) : -1;
	if (old >= 0 && old != tree->nodes[node].watch)
		inotify_rm_watch(tree->watches.fd, old);
}

/* A directory that is gone takes its watch and those of its directories along */
static void forget(struct tree *tree, uint32_t node)
{
	struct tree_node *n = &tree->nodes[node];

	if (n->directory && n->loaded) {
		foreach_array_index(arrview(n->first, n->count, array_ptr(tree->nodes, TREE_NODES)), i)
			forget(tree, n->first + i);
		tree->left += n->count;
	}
	if (n->watch >= 0 && tree->watching)
		inotify_rm_watch(tree->watches.fd, n->watch);
	n->watch = -1;
	requeue(tree, node, TREE_NONE);
}

/* The node that was just read like 'old', which was read before */
static uint32_t match(struct tree *tree, uint32_t old)
{
	uint32_t low = tree->block, high = tree->block + tree->block_count;

	while (low < high) {
		uint32_t middle = low + (high - low) / 2;
		int side = order(tree, &tree->nodes[old], &tree->nodes[middle]);

		if (!side)
			return middle;
		if (side < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return TREE_NONE;
}

/* What was read under the children that are still there stays with them, a directory that took a name is read anew */
static void merge(struct tree *tree, uint32_t first, uint32_t count)
{
	auto nodes = array_ptr(tree->nodes, TREE_NODES);

	foreach_array_index(arrview(first, count, nodes), i) {
		uint32_t old = first + i, new = match(tree, old);
		struct tree_node *from = &tree->nodes[old], *to;

		if (new == TREE_NONE) {
			forget(tree, old);
			continue;
		}
		to = &tree->nodes[new];
		to->expanded = from->expanded;
		if (to->inode != from->inode || to->device != from->device) {
			forget(tree, old);
			if (to->expanded && !enqueue(tree, new))
				to->expanded = false;
			continue;
		}
		to->loaded = from->loaded;
		to->first = from->first;
		to->count = from->count;
		to->watch = from->watch;
		to->stale = from->stale;
		from->watch = -1;
		if (to->loaded)
			foreach_array_ref(arrview(to->first, to->count, nodes), child)
				child->parent = new;
		requeue(tree, old, new);
	}
	tree->left += count;
}

static void finish(struct tree *tree)
{
	struct tree_node *directory = &tree->nodes[tree->loading];
	size_t rows;

	sort(tree, tree->block_count, arrview(tree->block, tree->block_count, array_ptr(tree->nodes, TREE_NODES)));
	if (directory->loaded)
		merge(tree, directory->first, directory->count);
	directory->first = tree->block;
	directory->count = tree->block_count;
	directory->loaded = true;
	tree->loading = TREE_NONE;
	rows = tree_rows(tree);
	if (tree->top >= rows)
		tree->top = rows ? rows - 1 : 0;
	tree->listener->changed(tree);
}

static bool add(struct tree *tree, const struct dirent64 *entry)
{
	struct tree_node *directory = &tree->nodes[tree->loading];
	size_t length = strlen(entry->d_name) + 1;

	if (tree->node_count == TREE_NODES || TREE_NAMES - tree->names_used < length || directory->depth == UINT8_MAX) {
		log_printf("poor_text_viewer: %s: too many files\n", tree->path);
		return false;
	}
	copy_array(arrview(tree->names_used, length, array_ptr(tree->names, TREE_NAMES)), array_ptr(entry->d_name, length));
	tree->nodes[tree->node_count++] = (struct tree_node){
		.inode = entry->d_ino,
		.parent = tree->loading,
		.first = 0,
		.count = 0,
		.name = tree->names_used,
		.device = 0,
		.watch = -1,
		.depth = directory->depth + 1,
		.directory = entry->d_type == DT_DIR,
		.expanded = false,
		.loaded = false,
		.unknown = entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN,
		.stale = false,
	};
	tree->names_used += length;
	tree->block_count++;
	return true;
}

/* getdents64() is the one way to read a directory, io_uring can't. What doesn't fit is left out. */
static void read_entries(struct tree *tree)
{
	ssize_t size;

	tree->block = tree->node_count;
	tree->block_count = 0;
	while ((size = getdents64(tree->directory.fd, tree->memory->dirents, sizeof(tree->memory->dirents))) > 0) {
		for (size_t at = 0; at < (size_t)size;) {
			const struct dirent64 *entry = (const struct dirent64 *)&tree->memory->dirents[at];

			at += entry->d_reclen;
			if (entry->d_name[0] != '.' && !add(tree, entry))
				return;
		}
	}
	if (size < 0)
		log_printf("poor_text_viewer: %s: %s\n", tree->path, strerror(errno));
}

/* Links, and entries of unknown type, are what they point to */
static void stat_next(struct tree *tree, uint32_t from)
{
	foreach_array_index(arrview(from, tree->block + tree->block_count - from, array_ptr(tree->nodes, TREE_NODES)), i) {
		if (!tree->nodes[from + i].unknown)
			continue;
		tree->stating = from + i;
		io_uring_prep_statx(need_sqe(poor_loop_group_get_sqe_or_submit(tree->loop, &tree->directory, &tree->stat_op)),
				    tree->directory.fd, &tree->names[tree->nodes[tree->stating].name], 0, STATX_TYPE | STATX_INO,
				    &tree->statx);
		return;
	}
	finish(tree);
}

POOR_LOOP_GROUP_OP(on_stat, loop, directory, op, cqe)
{
	struct tree *tree = container_of(directory, struct tree, directory);
	struct tree_node *node = &tree->nodes[tree->stating];

	node->unknown = false;
	node->directory = !cqe->res && S_ISDIR(tree->statx.stx_mode);
	if (!cqe->res) {
		node->inode = tree->statx.stx_ino;
		node->device = tree->statx.stx_dev_major << 20 | tree->statx.stx_dev_minor;
	}
	if (!tree->stopped)
		stat_next(tree, tree->stating + 1);
}

POOR_LOOP_FD_DONE(tree_directory, loop, directory)
{
	struct tree *tree = container_of(directory, struct tree, directory);

	tree->directory_open = false;
	start(tree);
}

/* The directory stays open while its entries are looked at, and is watched again before they are read */
static void on_open(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct tree *tree = container_of(op, struct tree, open_op);

	if (tree->stopped) {
		if (cqe->res >= 0)
			poor_loop_close_fd_or_submit(tree->loop, cqe->res, 0);
		return;
	}
	if (cqe->res < 0) {
		log_printf("poor_text_viewer: %s: %s\n", tree->path, strerror(-cqe->res));
		tree->block = tree->node_count;
		tree->block_count = 0;
		finish(tree);
		start(tree);
		return;
	}
	poor_loop_fd_init(&tree->directory, cqe->res);
	tree->directory_open = true;
	poor_loop_group_acquire(&tree->directory);
	watch(tree, tree->loading);
	read_entries(tree);
	stat_next(tree, tree->block);
	if (poor_loop_group_release(&tree->directory))
		poor_loop_group_call_done(tree_directory, tree->loop, &tree->directory);
}

/* The nodes that are still needed move to the other pool, children right after their parents */
static void compact(struct tree *tree)
{
	auto pool = &tree->memory->pools[tree->pool ^ 1];
	uint32_t count = 1;
	size_t used = 1;

	pool->nodes[0] = tree->nodes[0];
	pool->names[0] = 0;
	tree->memory->moved[0] = 0;
	for (uint32_t at = 0; at < count; at++) {
		struct tree_node *node = &pool->nodes[at];
		uint32_t first = node->first;

		node->first = count;
		if (!node->loaded)
			continue;
		foreach_array_index(arrview_first(node->count, array_ptr(&tree->nodes[first], TREE_NODES - first)), i) {
			const char *name = &tree->names[tree->nodes[first + i].name];
			size_t length = strlen(name) + 1;

			pool->nodes[count] = tree->nodes[first + i];
			pool->nodes[count].parent = at;
			pool->nodes[count].name = used;
			copy_array(arrview(used, length, pool->names), array_ptr(name, length));
			used += length;
			tree->memory->moved[first + i] = count++;
		}
	}
	foreach_array_ref(arrview_first(tree->queued, tree->queue), queued)
		*queued = tree->memory->moved[*queued];
	tree->pool ^= 1;
	tree->nodes = pool->nodes;
	tree->names = pool->names;
	tree->node_count = count;
	tree->names_used = used;
	tree->left = 0;
}

/* Nodes that were left are cleared out before they outnumber the others, or before the room runs out */
static void start(struct tree *tree)
{
	if (!tree->queued || tree->stopped || tree->loading != TREE_NONE || tree->directory_open)
		return;
	if (tree->left > tree->node_count - tree->left ||
	    (tree->left && (TREE_NODES - tree->node_count < TREE_NODES / 8 || TREE_NAMES - tree->names_used < TREE_NAMES / 8)))
		compact(tree);
	tree->loading = tree->queue[0];
	requeue(tree, tree->loading, TREE_NONE);
	tree->nodes[tree->loading].stale = false;
	if (!tree_path(tree, tree->loading, tree->path)) {
		tree->block = tree->node_count;
		tree->block_count = 0;
		finish(tree);
		start(tree);
		return;
	}
	io_uring_prep_openat(need_sqe(poor_loop_get_sqe_or_submit(tree->loop, &tree->open_op)), AT_FDCWD, tree->path,
			     O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
}

/* Directories that changed are read again, those that can't be yet wait for another while */
static void on_settle(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct tree *tree = container_of(timer, struct tree, settle);
	bool waiting = false;

	foreach_array_index(arrview_first(tree->node_count, array_ptr(tree->nodes, TREE_NODES)), node)
		if (tree->nodes[node].stale && tree->nodes[node].watch >= 0 && !enqueue(tree, node))
			waiting = true;
	if (waiting)
		poor_loop_timer_arm(loop, timer, poor_loop_now() + TREE_SETTLE_NS);
	start(tree);
}

static void stale(struct tree *tree, int32_t watch)
{
	foreach_array_ref(arrview_first(tree->node_count, array_ptr(tree->nodes, TREE_NODES)), node)
		if (watch < 0 ? node->watch >= 0 : node->watch == watch)
			node->stale = true;
	if (!poor_loop_timer_armed(&tree->settle))
		poor_loop_timer_arm(tree->loop, &tree->settle, poor_loop_now() + TREE_SETTLE_NS);
}

static void read_events(struct tree *tree)
{
	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(tree->loop, &tree->watches, &tree->event_op)),
				 tree->watches.fd, tree->memory->events, -1);
}

/* Hidden files don't show. A directory that lost its watch is read again, and after an overflow every one is. */
POOR_LOOP_GROUP_OP(on_event, loop, watches, op, cqe)
{
	struct tree *tree = container_of(watches, struct tree, watches);

	if (cqe->res <= 0 || tree->stopped)
		return;
	for (size_t at = 0; at < (size_t)cqe->res;) {
		const struct inotify_event *event = (const struct inotify_event *)&tree->memory->events[at];

		at += sizeof(*event) + event->len;
		if (event->mask & IN_Q_OVERFLOW)
			stale(tree, -1);
		else if (!event->len || event->name[0] != '.')
			stale(tree, event->wd);
	}
	read_events(tree);
}

POOR_LOOP_FD_DONE(tree_watches, loop, watches)
{
	container_of(watches, struct tree, watches)->watching = false;
}

/* inotify_init1() is the one way to watch, io_uring can't. Without it the tree doesn't follow changes. */
void tree_init(struct tree *tree, struct poor_loop *loop, const char *root, const struct tree_listener *listener,
	       struct tree_memory *memory)
{
	int fd;

	tree->loop = loop;
	tree->listener = listener;
	tree->memory = memory;
	tree->pool = 0;
	tree->nodes = memory->pools[0].nodes;
	tree->names = memory->pools[0].names;
	poor_loop_op_init(&tree->open_op, on_open);
	poor_loop_op_init(&tree->stat_op, on_stat);
	poor_loop_op_init(&tree->event_op, on_event);
	poor_loop_timer_init(&tree->settle, on_settle);
	tree->loading = tree->stating = TREE_NONE;
	tree->queued = tree->top = tree->left = 0;
	tree->node_count = tree->names_used = 1;
	tree->names[0] = 0;
	tree->directory_open = tree->watching = tree->stopped = false;
	snprintf_array(tree->root, "%s", root);
	snprintf_array(tree->path, "%s", root);
	tree->nodes[0] = (struct tree_node){
		.parent = 0,
		.first = 1,
		.count = 0,
		.name = 0,
		.watch = -1,
		.depth = 0,
		.directory = true,
		.expanded = false,
		.loaded = false,
		.unknown = false,
		.stale = false,
	};
	fd = inotify_init1(IN_CLOEXEC);
	if (fd < 0) {
		log_printf("poor_text_viewer: inotify: %s\n", strerror(errno));
	} else {
		poor_loop_fd_init(&tree->watches, fd);
		tree->watching = true;
		read_events(tree);
	}
	tree_toggle(tree, 0);
}

void tree_toggle(struct tree *tree, uint32_t node)
{
	struct tree_node *n = &tree->nodes[node];

	if (!n->directory)
		return;
	n->expanded = !n->expanded;
	if (!n->expanded || n->loaded || tree->loading == node)
		return;
	if (!enqueue(tree, node)) {
		n->expanded = false;
		return;
	}
	start(tree);
}

uint32_t tree_next(const struct tree *tree, uint32_t node)
{
	const struct tree_node *n = &tree->nodes[node];

	if (n->directory && n->expanded && n->count)
		return n->first;
	while (node) {
		const struct tree_node *parent = &tree->nodes[tree->nodes[node].parent];

		if (node + 1 < parent->first + parent->count)
			return node + 1;
		node = tree->nodes[node].parent;
	}
	return TREE_NONE;
}

size_t tree_rows(const struct tree *tree)
{
	size_t rows = 0;

	for (uint32_t node = tree_next(tree, 0); node != TREE_NONE; node = tree_next(tree, node))
		rows++;
	return rows;
}

uint32_t tree_row(const struct tree *tree, size_t row)
{
	uint32_t node = tree_next(tree, 0);

	while (row-- && node != TREE_NONE)
		node = tree_next(tree, node);
	return node;
}

/* How much of the root the paths under it start with, nothing of / */
static size_t prefix(const struct tree *tree)
{
	return strcmp(tree->root, "/") ? strlen(tree->root) : 0;
}

uint32_t tree_find(const struct tree *tree, const char *path)
{
	size_t root = prefix(tree);
	uint32_t node = 0;

	if (strncmp(path, tree->root, root) || path[root] != '/')
		return TREE_NONE;
	for (const char *name = path + root + 1, *end; node != TREE_NONE && *name; name = *end ? end + 1 : end) {
		const struct tree_node *directory = &tree->nodes[node];

		end = strchrnul(name, '/');
		node = TREE_NONE;
		if (!directory->loaded)
			break;
		foreach_array_index(arrview(directory->first, directory->count, array_ptr(tree->nodes, TREE_NODES)), i) {
			const char *child = tree_name(tree, directory->first + i);

			if (strlen(child) == (size_t)(end - name) && !memcmp(child, name, end - name))
				node = directory->first + i;
		}
	}
	return node;
}

const char *tree_name(const struct tree *tree, uint32_t node)
{
	return &tree->names[tree->nodes[node].name];
}

bool tree_path_impl(const struct tree *tree, uint32_t node, size_t size, char (*path)[size])
{
	uint32_t chain[UINT8_MAX + 1];
	size_t depth = 0, length = snprintf_array(path, "%s", tree->root);

	for (uint32_t at = node; at; at = tree->nodes[at].parent)
		chain[depth++] = at;
	if (depth)
		length = prefix(tree);
	while (depth-- && length < size)
		length += snprintf_array(arrview_cfront(length, path), "/%s", tree_name(tree, chain[depth]));
	return length < size;
}

void tree_stop(struct tree *tree)
{
	tree->stopped = true;
	tree->queued = 0;
	poor_loop_timer_disarm(&tree->settle);
	foreach_array_ref(((struct poor_loop_op *[]){ &tree->open_op, &tree->stat_op, &tree->event_op }), op)
		if ((*op)->pending)
			io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(tree->loop)), *op, 0);
}

bool tree_busy(const struct tree *tree)
{
	return tree->open_op.pending || tree->directory_open || tree->watching;
}
