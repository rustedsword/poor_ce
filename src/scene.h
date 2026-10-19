/* SPDX-License-Identifier: MIT */
#ifndef SCENE_H
#define SCENE_H

#include "atlas.h"
#include "editor.h"
#include "frame.h"
#include "theme.h"

constexpr uint32_t SCENE_BACKGROUND = QUAD_RGB(THEME_BACKGROUND);

/* The quads of everything that shows. A full atlas starts over once. */
void scene_build(const struct editor *editor, struct atlas *atlas, struct frame *frame);

#endif
