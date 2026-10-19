/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>

#include "damage.h"
#include "test-runner.h"

static struct frame old, frame;

static void start(struct frame *f, int32_t width, int32_t height)
{
	f->count = f->group_count = 0;
	f->width = width;
	f->height = height;
	f->generation = 1;
}

static void group(struct frame *f)
{
	f->groups[f->group_count++] = f->count;
}

static void put(struct frame *f, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color)
{
	f->quads[f->count++] = (struct quad){ .x = x, .y = y, .width = width, .height = height, .u = QUAD_SOLID, .v = 0,
					      .color = color };
}

static bool has(const struct damage_region *region, int32_t x, int32_t y, int32_t width, int32_t height)
{
	foreach_array_const_ref(arrview_first(region->count, region->rects), rect)
		if (rect->x == x && rect->y == y && rect->width == width && rect->height == height)
			return true;
	return false;
}

TEST(first_frames_damage_everything)
{
	struct damage_region region;

	start(&frame, 100, 50);
	group(&frame);
	put(&frame, 10, 10, 8, 8, 1);
	damage_diff(nullptr, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 0, 100, 50));
}

TEST(same_frames_damage_nothing)
{
	struct damage_region region;

	start(&old, 100, 50);
	group(&old);
	put(&old, 10, 10, 8, 8, 1);
	put(&old, 30, 20, 40, 8, 2);
	frame = old;
	damage_diff(&old, &frame, &region);
	assert(!region.count);
}

TEST(moves_damage_where_quads_were_and_are)
{
	struct damage_region region;

	start(&old, 300, 200);
	group(&old);
	put(&old, 10, 10, 2, 16, 1);
	start(&frame, 300, 200);
	group(&frame);
	put(&frame, 200, 100, 2, 16, 1);
	damage_diff(&old, &frame, &region);
	assert(region.count == 2 && has(&region, 10, 10, 2, 16) && has(&region, 200, 100, 2, 16));
}

TEST(inserts_damage_what_they_move)
{
	struct damage_region region;

	start(&old, 300, 200);
	group(&old);
	start(&frame, 300, 200);
	group(&frame);
	for (int32_t i = 0; i < 5; i++) {
		put(&old, 10 * i, 20, 10, 16, 'a' + i);
		if (i == 2)
			put(&frame, 20, 20, 10, 16, 'x');
		put(&frame, 10 * (i + (i >= 2)), 20, 10, 16, 'a' + i);
	}
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 20, 20, 40, 16));
}

TEST(changes_damage_only_what_changed)
{
	struct damage_region region;

	start(&old, 300, 200);
	group(&old);
	start(&frame, 300, 200);
	group(&frame);
	for (int32_t i = 0; i < 5; i++) {
		put(&old, 10 * i, 20, 10, 16, 1);
		put(&frame, 10 * i, 20, 10, 16, i == 2 ? 2 : 1);
	}
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 20, 20, 10, 16));
}

TEST(groups_keep_changes_apart)
{
	struct damage_region region;

	start(&old, 300, 200);
	start(&frame, 300, 200);
	for (int32_t row = 0; row < 3; row++) {
		group(&old);
		group(&frame);
		put(&old, 0, 40 * row, 100, 16, 1);
		put(&frame, 0, 40 * row, row == 2 ? 50 : 100, 16, 1);
	}
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 80, 100, 16));
	group(&frame);
	put(&frame, 0, 150, 10, 10, 1);
	damage_diff(&old, &frame, &region);
	assert(region.count == 2 && has(&region, 0, 80, 100, 16) && has(&region, 0, 150, 10, 10));
}

TEST(order_damages)
{
	struct damage_region region;

	start(&old, 100, 100);
	group(&old);
	put(&old, 0, 0, 32, 32, 1);
	put(&old, 16, 16, 32, 32, 2);
	start(&frame, 100, 100);
	group(&frame);
	put(&frame, 16, 16, 32, 32, 2);
	put(&frame, 0, 0, 32, 32, 1);
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 0, 48, 48));
}

TEST(other_sizes_and_atlases_damage_everything)
{
	struct damage_region region;

	start(&old, 100, 50);
	group(&old);
	put(&old, 10, 10, 8, 8, 1);
	frame = old;
	frame.generation = 2;
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 0, 100, 50));
	frame = old;
	frame.width = 120;
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 0, 120, 50));
}

TEST(damage_stays_on_the_frame)
{
	struct damage_region region;

	start(&old, 100, 50);
	group(&old);
	put(&old, -50, -50, 20, 20, 1);
	put(&old, 100, 10, 20, 20, 1);
	start(&frame, 100, 50);
	group(&frame);
	put(&frame, -60, -50, 20, 20, 1);
	put(&frame, 110, 10, 20, 20, 1);
	damage_diff(&old, &frame, &region);
	assert(!region.count);
	put(&frame, -5, -5, 10, 10, 1);
	put(&frame, 95, 45, 10, 10, 2);
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 0, 100, 50));
}

TEST(touching_changes_merge)
{
	struct damage_region region;

	start(&old, 300, 200);
	start(&frame, 300, 200);
	for (int32_t row = 0; row < 4; row++) {
		group(&old);
		group(&frame);
		put(&old, 0, 16 * row, 100, 16, 1);
		put(&frame, 0, 16 * row, 100, 16, row == 1 || row == 2 ? 2 : 1);
	}
	damage_diff(&old, &frame, &region);
	assert(region.count == 1 && has(&region, 0, 16, 100, 32));
	group(&old);
	group(&frame);
	put(&old, 100, 64, 10, 10, 1);
	damage_diff(&old, &frame, &region);
	assert(region.count == 2 && has(&region, 100, 64, 10, 10));
}
