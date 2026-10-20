/* SPDX-License-Identifier: MIT */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

/* 0xRRGGBB colors of the theme that the 'theme' option picks */
#ifdef THEME_WHITE
constexpr uint32_t THEME_BACKGROUND = 0xffffff;
constexpr uint32_t THEME_PANEL = 0xf3f3f3;
constexpr uint32_t THEME_BORDER = 0xe0e0e0;
constexpr uint32_t THEME_TEXT = 0x1e1e1e;
constexpr uint32_t THEME_DIM = 0x717171;
constexpr uint32_t THEME_LINE_NUMBER = 0x9a9a9a;
constexpr uint32_t THEME_CURRENT_LINE = 0xf2f2f2;
constexpr uint32_t THEME_SELECTION = 0xadd6ff;
constexpr uint32_t THEME_SELECTION_INACTIVE = 0xe5ebf1;
constexpr uint32_t THEME_CARET = 0x000000;
constexpr uint32_t THEME_ACCENT = 0x005fb8;
constexpr uint32_t THEME_TREE_ACTIVE = 0xdde6f3;
constexpr uint32_t THEME_KEYWORD = 0x0000ff;
constexpr uint32_t THEME_CONTROL = 0xaf00db;
constexpr uint32_t THEME_JUMP = THEME_CONTROL;
constexpr uint32_t THEME_TYPE = 0x267f99;
constexpr uint32_t THEME_FUNCTION = 0x795e26;
constexpr uint32_t THEME_CONSTANT = 0x0070c1;
constexpr uint32_t THEME_NUMBER = 0x098658;
constexpr uint32_t THEME_STRING = 0xa31515;
constexpr uint32_t THEME_COMMENT = 0x008000;
constexpr uint32_t THEME_PREPROCESSOR = 0xaf00db;
constexpr uint32_t THEME_ATTRIBUTE = 0x7a3e9d;
constexpr uint32_t THEME_MACRO = 0x8a1bff;
constexpr uint32_t THEME_GLOBAL = 0xce5c00;
constexpr uint32_t THEME_LOCAL = 0x001080;
constexpr uint32_t THEME_PARAMETER = 0x808080;
constexpr uint32_t THEME_INACTIVE = 0xa3a3a3;
#elifdef THEME_BLACK
constexpr uint32_t THEME_BACKGROUND = 0x000000;
constexpr uint32_t THEME_PANEL = 0x000000;
constexpr uint32_t THEME_BORDER = 0x2b2b2b;
constexpr uint32_t THEME_TEXT = 0xd4d4d4;
constexpr uint32_t THEME_DIM = 0x8b8b8b;
constexpr uint32_t THEME_LINE_NUMBER = 0x6e7681;
constexpr uint32_t THEME_CURRENT_LINE = 0x161616;
constexpr uint32_t THEME_SELECTION = 0x264f78;
constexpr uint32_t THEME_SELECTION_INACTIVE = 0x3a3d41;
constexpr uint32_t THEME_CARET = 0xffffff;
constexpr uint32_t THEME_ACCENT = 0x0078d4;
constexpr uint32_t THEME_TREE_ACTIVE = 0x04395e;
constexpr uint32_t THEME_KEYWORD = 0x569cd6;
constexpr uint32_t THEME_CONTROL = 0xc586c0;
constexpr uint32_t THEME_JUMP = THEME_CONTROL;
constexpr uint32_t THEME_TYPE = 0x4ec9b0;
constexpr uint32_t THEME_FUNCTION = 0xdcdcaa;
constexpr uint32_t THEME_CONSTANT = 0x4fc1ff;
constexpr uint32_t THEME_NUMBER = 0xb5cea8;
constexpr uint32_t THEME_STRING = 0xce9178;
constexpr uint32_t THEME_COMMENT = 0x6a9955;
constexpr uint32_t THEME_PREPROCESSOR = 0xc586c0;
constexpr uint32_t THEME_ATTRIBUTE = 0xd7ba7d;
constexpr uint32_t THEME_MACRO = 0xbeb7ff;
constexpr uint32_t THEME_GLOBAL = 0xff8c00;
constexpr uint32_t THEME_LOCAL = 0x9cdcfe;
constexpr uint32_t THEME_PARAMETER = 0xb2b2b2;
constexpr uint32_t THEME_INACTIVE = 0x6e6e6e;
#else // THEME_NANO, the default
constexpr uint32_t THEME_BACKGROUND = 0x000000;
constexpr uint32_t THEME_PANEL = 0x000000;
constexpr uint32_t THEME_BORDER = 0x2b2b2b;
constexpr uint32_t THEME_TEXT = 0xffffff;
constexpr uint32_t THEME_DIM = 0x8b8b8b;
constexpr uint32_t THEME_LINE_NUMBER = 0x6e7681;
constexpr uint32_t THEME_CURRENT_LINE = 0x161616;
constexpr uint32_t THEME_SELECTION = 0x264f78;
constexpr uint32_t THEME_SELECTION_INACTIVE = 0x3a3d41;
constexpr uint32_t THEME_CARET = 0xffffff;
constexpr uint32_t THEME_ACCENT = 0x0078d4;
constexpr uint32_t THEME_TREE_ACTIVE = 0x04395e;
constexpr uint32_t THEME_KEYWORD = 0x18b218;
constexpr uint32_t THEME_CONTROL = 0xffff54;
constexpr uint32_t THEME_JUMP = 0xb218b2;
constexpr uint32_t THEME_TYPE = THEME_KEYWORD;
constexpr uint32_t THEME_FUNCTION = 0x18b2b2;
constexpr uint32_t THEME_CONSTANT = 0xff5454;
constexpr uint32_t THEME_NUMBER = THEME_TEXT;
constexpr uint32_t THEME_STRING = THEME_CONTROL;
constexpr uint32_t THEME_COMMENT = 0x5454ff;
constexpr uint32_t THEME_PREPROCESSOR = 0x54ffff;
constexpr uint32_t THEME_ATTRIBUTE = THEME_FUNCTION;
constexpr uint32_t THEME_MACRO = THEME_CONSTANT;
constexpr uint32_t THEME_GLOBAL = 0xff8c00;
constexpr uint32_t THEME_LOCAL = THEME_TEXT;
constexpr uint32_t THEME_PARAMETER = 0xb2b2b2;
constexpr uint32_t THEME_INACTIVE = 0x686868;
#endif

#endif
