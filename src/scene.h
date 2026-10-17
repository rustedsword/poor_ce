/* SPDX-License-Identifier: MIT */
#ifndef SCENE_H
#define SCENE_H

#include "atlas.h"
#include "editor.h"
#include "quad.h"

/* The color that the scene goes over */
constexpr uint32_t SCENE_BACKGROUND = QUAD_RGB(0xffffff);

/* The quads of everything that shows, how many of them there are. A full atlas starts over once. */
size_t scene_build_impl(const struct editor *editor, struct atlas *atlas, size_t capacity, struct quad (*quads)[capacity]);
#define scene_build(editor, atlas, quads) scene_build_impl(editor, atlas, ARRAY_SIZE(quads), &auto_arr(quads))

#endif
