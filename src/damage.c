/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <string.h>

#include "damage.h"

static int32_t lesser(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

static int32_t greater(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

static struct poor_damage_rect cover_impl(const struct frame *frame, size_t count, const struct quad (*quads)[count])
{
	int32_t left = INT32_MAX, top = INT32_MAX, right = 0, bottom = 0;

	foreach_array_const_ref(quads, quad) {
		int32_t x = greater(quad->x, 0), y = greater(quad->y, 0);
		int32_t end_x = lesser(quad->x + quad->width, frame->width);
		int32_t end_y = lesser(quad->y + quad->height, frame->height);

		if (x >= end_x || y >= end_y)
			continue;
		left = lesser(left, x);
		top = lesser(top, y);
		right = greater(right, end_x);
		bottom = greater(bottom, end_y);
	}
	return (struct poor_damage_rect){ left, top, right - left, bottom - top };
}

#define cover(frame, quads) cover_impl(frame, ARRAY_SIZE(quads), &auto_arr(quads))

static size_t group_start(const struct frame *frame, size_t group)
{
	return group < frame->group_count ? frame->groups[group] : frame->count;
}

static size_t group_end(const struct frame *frame, size_t group)
{
	return group + 1 < frame->group_count ? frame->groups[group + 1] : frame->count;
}

/* What lies between the quads that a group starts and ends with in both frames changed, where it was and where it is */
static void diff_group(struct damage_region *region, const struct frame *old, const struct frame *frame, size_t group)
{
	size_t old_start = group_start(old, group), old_count = group_end(old, group) - old_start;
	size_t start = group_start(frame, group), count = group_end(frame, group) - start;
	auto was = arrview(old_start, old_count, old->quads);
	auto is = arrview(start, count, frame->quads);
	size_t first = 0, last = 0;

	while (first < old_count && first < count && !memcmp(&(*was)[first], &(*is)[first], sizeof(struct quad)))
		first++;
	while (last < old_count - first && last < count - first &&
	       !memcmp(&(*was)[old_count - 1 - last], &(*is)[count - 1 - last], sizeof(struct quad)))
		last++;
	poor_damage_region_add(region, cover(frame, arrview(first, old_count - first - last, was)));
	poor_damage_region_add(region, cover(frame, arrview(first, count - first - last, is)));
}

void damage_diff(const struct frame *old, const struct frame *frame, struct damage_region *region)
{
	size_t groups;

	if (!old || old->width != frame->width || old->height != frame->height || old->generation != frame->generation) {
		poor_damage_region_whole(region, frame->width, frame->height);
		return;
	}
	poor_damage_region_init(region);
	groups = old->group_count > frame->group_count ? old->group_count : frame->group_count;
	for (size_t group = 0; group < groups; group++)
		diff_group(region, old, frame, group);
}
