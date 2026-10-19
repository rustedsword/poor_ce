/* SPDX-License-Identifier: MIT */
#ifndef TREE_H
#define TREE_H

#include <limits.h>
#include <linux/stat.h>
#include <poor_loop_fd.h>

constexpr size_t TREE_NODES = 1 << 16;
constexpr size_t TREE_NAMES = 2 << 20;
constexpr size_t TREE_QUEUE = 64;
constexpr size_t TREE_DIRENTS = 1 << 16;
constexpr size_t TREE_EVENTS = 1 << 14;
constexpr uint64_t TREE_SETTLE_NS = 100'000'000;
constexpr uint32_t TREE_NONE = UINT32_MAX;

poor_loop_fd_declare(tree_directory);
poor_loop_fd_declare(tree_watches);

/*
 * A file or a directory, whose children are the nodes from 'first' on, sorted, once it was read. 'unknown' ones get
 * stat. A directory that was read has a 'watch' of inotify, and is 'stale' once it changed. 'inode' and 'device' tell
 * it from another one that takes its name, 'device' is 0 for one on the device of its parent that didn't get stat.
 */
struct tree_node {
	uint64_t inode;
	uint32_t parent, first, count;
	uint32_t name, device;
	int32_t watch;
	uint8_t depth;
	bool directory, expanded, loaded, unknown, stale;
};

struct tree;

struct tree_listener {
	/* A directory was read */
	void (*changed)(struct tree *tree);
};

/*
 * The nodes and their names, each with a 0 at its end, in one of two pools: the nodes that are still needed move to
 * the other one once too many are left behind, 'moved' tells where. Entries of directories and their changes are
 * read into 'dirents' and 'events'.
 */
struct tree_memory {
	struct {
		struct tree_node nodes[TREE_NODES];
		char names[TREE_NAMES];
	} pools[2];
	uint32_t moved[TREE_NODES];
	alignas(8) char dirents[TREE_DIRENTS];
	alignas(8) char events[TREE_EVENTS];
};

/*
 * The files under the directory 'root', which is node 0, without the hidden ones, in memory of the caller.
 * Directories are read when they expand, one at a time, while the others wait in 'queue', and read again a while
 * after they changed. A directory that is read gets the nodes from 'block' on, its nodes from before are 'left'.
 * 'top' is the first row that shows.
 */
struct tree {
	struct poor_loop *loop;
	const struct tree_listener *listener;
	struct tree_memory *memory;
	struct tree_node *nodes;
	char *names;
	tree_directory directory;
	tree_watches watches;
	struct poor_loop_op open_op, stat_op, event_op;
	struct poor_loop_timer settle;
	struct statx statx;
	uint32_t queue[TREE_QUEUE];
	uint32_t loading, stating, block, block_count;
	size_t queued, node_count, names_used, left, top;
	unsigned pool;
	bool directory_open, watching, stopped;
	char root[PATH_MAX];
	char path[PATH_MAX];
};

void tree_init(struct tree *tree, struct poor_loop *loop, const char *root, const struct tree_listener *listener,
	       struct tree_memory *memory);
/* Expand a directory, which is read if it wasn't, or collapse it */
void tree_toggle(struct tree *tree, uint32_t node);
/* The row after the one of 'node', or TREE_NONE: the children of an expanded directory come before its siblings */
uint32_t tree_next(const struct tree *tree, uint32_t node);
size_t tree_rows(const struct tree *tree);
/* The node in a row, or TREE_NONE */
uint32_t tree_row(const struct tree *tree, size_t row);
/* The row of a node, or SIZE_MAX if it doesn't show */
size_t tree_row_of(const struct tree *tree, uint32_t node);
/* The node of an absolute path that was read, or TREE_NONE */
uint32_t tree_find(const struct tree *tree, const char *path);
/* The node of a path if it shows, or the last one on the way to it that does, 0 if none does */
uint32_t tree_nearest(const struct tree *tree, const char *path);
const char *tree_name(const struct tree *tree, uint32_t node);
/* The absolute path of a node, false if it doesn't fit */
[[nodiscard]] bool tree_path_impl(const struct tree *tree, uint32_t node, size_t size, char (*path)[size]);
#define tree_path(tree, node, path) tree_path_impl(tree, node, ARRAY_SIZE(path), &auto_arr(path))
/* Stop reading, wait until tree_busy() is false before the memory goes */
void tree_stop(struct tree *tree);
bool tree_busy(const struct tree *tree);

#endif
