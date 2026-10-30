/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <poor_stdio.h>
#include <string.h>

#include "jumps.h"

void jumps_init_impl(struct jumps *jumps, size_t capacity, struct jump (*places)[capacity])
{
	assert(capacity >= 2);
	jumps->places = *places;
	jumps->capacity = capacity;
	jumps->count = jumps->at = 0;
}

void jumps_push(struct jumps *jumps, const char *path, size_t offset)
{
	auto places = array_ptr(jumps->places, jumps->capacity);
	struct jump *jump;

	if (jumps->at == jumps->capacity) {
		memmove(*places, arrview_cfront(1, places), ARRAY_SIZE_BYTES(arrview_cfront(1, places)));
		jumps->at--;
	}
	jump = &(*places)[jumps->at++];
	jump->offset = offset;
	snprintf_array(jump->path, "%s", path);
	jumps->count = jumps->at;
}

const struct jump *jumps_back(struct jumps *jumps, const char *path, size_t offset)
{
	if (!jumps->at)
		return nullptr;
	if (jumps->at == jumps->count && path) {
		jumps_push(jumps, path, offset);
		jumps->at--;
	}
	return &jumps->places[--jumps->at];
}

const struct jump *jumps_forward(struct jumps *jumps)
{
	if (jumps->at + 1 >= jumps->count)
		return nullptr;
	return &jumps->places[++jumps->at];
}
