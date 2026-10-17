/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <string.h>

#include "path.h"

/* .. after a name of 'path' stays, since the name may be a link */
bool path_join_impl(size_t size, char (*out)[size], const char *base, const char *path)
{
	const char *parts[] = { *path == '/' ? "" : base, path };
	size_t length = 0, physical = 0;

	foreach_array_const_ref(parts, part) {
		for (const char *name = *part, *end; *name; name = *end ? end + 1 : end) {
			size_t name_size;

			end = strchrnul(name, '/');
			name_size = end - name;
			if (!name_size || (name_size == 1 && *name == '.'))
				continue;
			if (name_size == 2 && name[0] == '.' && name[1] == '.' && length == physical) {
				while (length && (*out)[--length] != '/')
					;
				physical = length;
				continue;
			}
			if (size - length < name_size + 2)
				return false;
			(*out)[length++] = '/';
			copy_array(arrview(length, name_size, out), array_ptr(name, name_size));
			length += name_size;
			if (part == parts)
				physical = length;
		}
	}
	if (!length)
		(*out)[length++] = '/';
	(*out)[length] = 0;
	return true;
}

const char *path_relative(const char *root, const char *path)
{
	size_t length = strlen(root);

	if (!strncmp(root, path, length) && path[length] == '/')
		return path + length + 1;
	return path;
}

const char *path_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}
