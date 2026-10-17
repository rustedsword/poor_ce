/* SPDX-License-Identifier: MIT */
#ifndef SYNTAX_H
#define SYNTAX_H

#include <stddef.h>
#include <stdint.h>

#include "text.h"

enum syntax_class : uint8_t {
	SYNTAX_PLAIN,
	SYNTAX_KEYWORD,
	SYNTAX_CONTROL,
	SYNTAX_TYPE,
	SYNTAX_FUNCTION,
	SYNTAX_CONSTANT,
	SYNTAX_NUMBER,
	SYNTAX_STRING,
	SYNTAX_COMMENT,
	SYNTAX_PREPROCESSOR,
	SYNTAX_ATTRIBUTE,
	SYNTAX_CLASSES,
};

struct syntax_token {
	size_t start, end;
	enum syntax_class class;
};

/*
 * The tokens of a line of C that aren't plain, from the state that the line starts in. 'blank' is true while only
 * whitespace and comments came, 'tag' after struct, union and enum, and 'include' in #include and #embed.
 */
struct syntax {
	const char *bytes;
	size_t size, at;
	uint8_t state;
	bool blank, tag, attribute, include;
};

void syntax_init_impl(struct syntax *syntax, size_t size, const char (*line)[size], uint8_t state);
#define syntax_init(syntax, line, state) syntax_init_impl(syntax, ARRAY_SIZE(line), &auto_arr(line), state)
bool syntax_next(struct syntax *syntax, struct syntax_token *token);
/* The state that the next line starts in, once the tokens ran out */
uint8_t syntax_end(const struct syntax *syntax);
/* The states of the lines after 'first' that changed, up to 'last' at least */
void syntax_update(struct text *text, size_t first, size_t last);

#endif
