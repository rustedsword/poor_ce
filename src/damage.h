/* SPDX-License-Identifier: MIT */
#ifndef DAMAGE_H
#define DAMAGE_H

#include <poor_damage_region.h>

#include "frame.h"

constexpr size_t DAMAGE_RECTS = 4;

poor_damage_region_declare(damage_region, DAMAGE_RECTS);

/* Where 'frame' differs from 'old', all of it without 'old' */
void damage_diff(const struct frame *old, const struct frame *frame, struct damage_region *region);

#endif
