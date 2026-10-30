/* SPDX-License-Identifier: MIT */
#ifndef JUMPS_H
#define JUMPS_H

#include <limits.h>
#include <poor_array.h>
#include <stddef.h>

/* Where the caret was before a jump */
struct jump {
	size_t offset;
	char path[PATH_MAX];
};

/*
 * The places that jumps left, in memory of the caller with room for two at least, where a jump went from and to.
 * Those before 'at' go back, the others forward.
 */
struct jumps {
	struct jump *places;
	size_t capacity, count, at;
};

void jumps_init_impl(struct jumps *jumps, size_t capacity, struct jump (*places)[capacity]);
#define jumps_init(jumps, places) jumps_init_impl(jumps, ARRAY_SIZE(places), &auto_arr(places))
/* A jump leaves 'offset' of 'path'. The places that went back are forgotten, and the oldest one if there is no room. */
void jumps_push(struct jumps *jumps, const char *path, size_t offset);
/*
 * The place to go back to, or nullptr. Going back from the newest jump keeps 'offset' of 'path', where it went, to go
 * forward to, unless 'path' is nullptr.
 */
const struct jump *jumps_back(struct jumps *jumps, const char *path, size_t offset);
const struct jump *jumps_forward(struct jumps *jumps);

#endif
