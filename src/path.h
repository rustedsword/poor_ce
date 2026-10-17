/* SPDX-License-Identifier: MIT */
#ifndef PATH_H
#define PATH_H

#include <poor_array.h>
#include <stddef.h>

/*
 * 'path' after 'base', an absolute path without links like the one of getcwd(), without . and repeated slashes. ..
 * goes with the name of 'base' before it, the kernel follows the others. False if it doesn't fit 'out'.
 */
[[nodiscard]] bool path_join_impl(size_t size, char (*out)[size], const char *base, const char *path);
#define path_join(out, base, path) path_join_impl(ARRAY_SIZE(out), &auto_arr(out), base, path)
/* 'path' under 'root', or all of it if it isn't under it */
const char *path_relative(const char *root, const char *path);
const char *path_name(const char *path);

#endif
