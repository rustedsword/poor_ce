/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "syntax.h"

/* Where a line starts: in code, or in what a line before began, and whether in a directive */
enum : uint8_t {
	STATE_CODE,
	STATE_COMMENT,
	STATE_STRING,
	STATE_CHARACTER,
	STATE_LINE_COMMENT,
	STATE_MODE = 7,
	STATE_DIRECTIVE = 8,
};

struct word {
	const char *name;
	enum syntax_class class;
};

/* Sorted by strcmp() */
static const struct word words[] = {
	{ "FILE", SYNTAX_TYPE }, { "_Alignas", SYNTAX_KEYWORD }, { "_Alignof", SYNTAX_KEYWORD }, { "_Atomic", SYNTAX_KEYWORD },
	{ "_BitInt", SYNTAX_TYPE }, { "_Bool", SYNTAX_TYPE }, { "_Complex", SYNTAX_TYPE }, { "_Decimal128", SYNTAX_TYPE },
	{ "_Decimal32", SYNTAX_TYPE }, { "_Decimal64", SYNTAX_TYPE }, { "_Float128", SYNTAX_TYPE },
	{ "_Float16", SYNTAX_TYPE }, { "_Float32", SYNTAX_TYPE }, { "_Float64", SYNTAX_TYPE }, { "_Generic", SYNTAX_KEYWORD },
	{ "_Imaginary", SYNTAX_TYPE }, { "_Noreturn", SYNTAX_KEYWORD }, { "_Static_assert", SYNTAX_KEYWORD },
	{ "_Thread_local", SYNTAX_KEYWORD }, { "__alignof__", SYNTAX_KEYWORD }, { "__asm__", SYNTAX_KEYWORD },
	{ "__attribute__", SYNTAX_KEYWORD }, { "__auto_type", SYNTAX_KEYWORD }, { "__builtin_va_list", SYNTAX_TYPE },
	{ "__extension__", SYNTAX_KEYWORD }, { "__func__", SYNTAX_CONSTANT }, { "__inline", SYNTAX_KEYWORD },
	{ "__inline__", SYNTAX_KEYWORD }, { "__int128", SYNTAX_TYPE }, { "__label__", SYNTAX_KEYWORD },
	{ "__restrict", SYNTAX_KEYWORD }, { "__restrict__", SYNTAX_KEYWORD }, { "__thread", SYNTAX_KEYWORD },
	{ "__typeof__", SYNTAX_KEYWORD }, { "__volatile__", SYNTAX_KEYWORD }, { "alignas", SYNTAX_KEYWORD },
	{ "alignof", SYNTAX_KEYWORD }, { "asm", SYNTAX_KEYWORD }, { "auto", SYNTAX_KEYWORD }, { "bool", SYNTAX_TYPE },
	{ "break", SYNTAX_CONTROL }, { "case", SYNTAX_CONTROL }, { "char", SYNTAX_TYPE }, { "const", SYNTAX_KEYWORD },
	{ "constexpr", SYNTAX_KEYWORD }, { "continue", SYNTAX_CONTROL }, { "default", SYNTAX_CONTROL },
	{ "do", SYNTAX_CONTROL }, { "double", SYNTAX_TYPE }, { "else", SYNTAX_CONTROL }, { "enum", SYNTAX_KEYWORD },
	{ "extern", SYNTAX_KEYWORD }, { "false", SYNTAX_CONSTANT }, { "float", SYNTAX_TYPE }, { "for", SYNTAX_CONTROL },
	{ "goto", SYNTAX_CONTROL }, { "if", SYNTAX_CONTROL }, { "inline", SYNTAX_KEYWORD }, { "int", SYNTAX_TYPE },
	{ "long", SYNTAX_TYPE }, { "nullptr", SYNTAX_CONSTANT }, { "register", SYNTAX_KEYWORD },
	{ "restrict", SYNTAX_KEYWORD }, { "return", SYNTAX_CONTROL }, { "short", SYNTAX_TYPE }, { "signed", SYNTAX_TYPE },
	{ "sizeof", SYNTAX_KEYWORD }, { "static", SYNTAX_KEYWORD }, { "static_assert", SYNTAX_KEYWORD },
	{ "struct", SYNTAX_KEYWORD }, { "switch", SYNTAX_CONTROL }, { "thread_local", SYNTAX_KEYWORD },
	{ "true", SYNTAX_CONSTANT }, { "typedef", SYNTAX_KEYWORD }, { "typeof", SYNTAX_KEYWORD },
	{ "typeof_unqual", SYNTAX_KEYWORD }, { "union", SYNTAX_KEYWORD }, { "unsigned", SYNTAX_TYPE },
	{ "va_list", SYNTAX_TYPE }, { "void", SYNTAX_TYPE }, { "volatile", SYNTAX_KEYWORD }, { "while", SYNTAX_CONTROL },
};

struct span {
	const char *bytes;
	size_t size;
};

static int word_order(const void *key, const void *element)
{
	const struct span *span = key;
	const char *name = ((const struct word *)element)->name;
	int order = strncmp(span->bytes, name, span->size);

	return order ? order : name[span->size] ? -1 : 0;
}

static bool identifier(char c)
{
	unsigned char u = c;

	return u == '_' || u == '$' || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u >= 0x80;
}

static bool digit(char c)
{
	return c >= '0' && c <= '9';
}

static bool blank(char c)
{
	return c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

static char peek(const struct syntax *syntax, size_t i)
{
	return syntax->at + i < syntax->size ? syntax->bytes[syntax->at + i] : 0;
}

static void set_mode(struct syntax *syntax, uint8_t mode)
{
	syntax->state = (syntax->state & ~STATE_MODE) | mode;
}

static void token_set(struct syntax_token *token, size_t start, size_t end, enum syntax_class class)
{
	*token = (struct syntax_token){ .start = start, .end = end, .class = class };
}

static void comment(struct syntax *syntax, struct syntax_token *token, size_t start)
{
	auto rest = arrview_cfront(syntax->at, array_ptr(syntax->bytes, syntax->size));
	const char *end = memmem(*rest, ARRAY_SIZE(rest), "*/", 2);

	syntax->at = end ? (size_t)(end + 2 - syntax->bytes) : syntax->size;
	set_mode(syntax, end ? STATE_CODE : STATE_COMMENT);
	token_set(token, start, syntax->at, SYNTAX_COMMENT);
}

/* A string or character literal goes on in the next line after a backslash */
static void quoted(struct syntax *syntax, struct syntax_token *token, size_t start, char quote)
{
	set_mode(syntax, quote == '"' ? STATE_STRING : STATE_CHARACTER);
	while (syntax->at < syntax->size) {
		char c = syntax->bytes[syntax->at++];

		if (c == '\\' && syntax->at < syntax->size) {
			syntax->at++;
		} else if (c == quote) {
			set_mode(syntax, STATE_CODE);
			break;
		}
	}
	token_set(token, start, syntax->at, SYNTAX_STRING);
}

/* The length of the prefix and the quote of a string or character literal that starts here, or 0 */
static size_t literal(const struct syntax *syntax)
{
	char c = peek(syntax, 0), quote;
	size_t prefix = c == 'u' && peek(syntax, 1) == '8' ? 2 : c == 'u' || c == 'U' || c == 'L' ? 1 : 0;

	quote = peek(syntax, prefix);
	return quote == '"' || quote == '\'' ? prefix + 1 : 0;
}

/* The name of a directive goes with its #. The file names of #include and #embed are strings. */
static void directive(struct syntax *syntax, struct syntax_token *token)
{
	static const char *const includes[] = { "include", "include_next", "embed", "import" };
	size_t start = syntax->at++, name;

	while (blank(peek(syntax, 0)))
		syntax->at++;
	name = syntax->at;
	while (identifier(peek(syntax, 0)))
		syntax->at++;
	auto word = arrview(name, syntax->at - name, array_ptr(syntax->bytes, syntax->size));
	foreach_array_const_ref(includes, include)
		if (strlen(*include) == ARRAY_SIZE(word) && !memcmp(*include, *word, ARRAY_SIZE(word)))
			syntax->include = true;
	syntax->state |= STATE_DIRECTIVE;
	token_set(token, start, syntax->at, SYNTAX_PREPROCESSOR);
}

/* A preprocessing number: exponents have signs, and quotes separate digits */
static void number(struct syntax *syntax)
{
	syntax->at++;
	for (;;) {
		char c = peek(syntax, 0), before = syntax->bytes[syntax->at - 1];

		if ((c == '+' || c == '-') && (before == 'e' || before == 'E' || before == 'p' || before == 'P'))
			syntax->at++;
		else if (c == '\'' && identifier(peek(syntax, 1)))
			syntax->at += 2;
		else if (identifier(c) || c == '.')
			syntax->at++;
		else
			return;
	}
}

static bool capitals(size_t size, const char (*word)[size])
{
	bool letter = false;

	foreach_array_const_ref(word, c) {
		if (*c >= 'A' && *c <= 'Z')
			letter = true;
		else if (*c != '_' && !digit(*c))
			return false;
	}
	return letter;
}

/* Words that the language doesn't know are guessed from how they look and what comes around them */
static enum syntax_class classify(struct syntax *syntax, size_t start)
{
	auto word = arrview(start, syntax->at - start, array_ptr(syntax->bytes, syntax->size));
	struct span key = { .bytes = *word, .size = ARRAY_SIZE(word) };
	const struct word *known = bsearch(&key, words, ARRAY_SIZE(words), sizeof(*words), word_order);
	size_t size = ARRAY_SIZE(word), next = syntax->at;
	bool tag = syntax->tag;

	syntax->tag = known && (!strcmp(known->name, "struct") || !strcmp(known->name, "union") || !strcmp(known->name, "enum"));
	if (syntax->state & STATE_DIRECTIVE && size == 7 && !memcmp(*word, "defined", size))
		return SYNTAX_PREPROCESSOR;
	if (known)
		return known->class;
	if (tag)
		return SYNTAX_TYPE;
	if (syntax->attribute)
		return SYNTAX_ATTRIBUTE;
	if (capitals(size, word))
		return SYNTAX_CONSTANT;
	if (size > 2 && (*word)[size - 2] == '_' && (*word)[size - 1] == 't')
		return SYNTAX_TYPE;
	while (next < syntax->size && blank(syntax->bytes[next]))
		next++;
	return next < syntax->size && syntax->bytes[next] == '(' ? SYNTAX_FUNCTION : SYNTAX_PLAIN;
}

void syntax_init_impl(struct syntax *syntax, size_t size, const char (*line)[size], uint8_t state)
{
	syntax->bytes = *line;
	syntax->size = size;
	syntax->at = 0;
	syntax->state = state;
	syntax->blank = !(state & STATE_DIRECTIVE);
	syntax->tag = syntax->attribute = syntax->include = false;
}

/* Comments count as whitespace, so a directive may come after one */
bool syntax_next(struct syntax *syntax, struct syntax_token *token)
{
	while (syntax->at < syntax->size) {
		uint8_t mode = syntax->state & STATE_MODE;
		size_t start = syntax->at, prefix;
		char c = peek(syntax, 0);

		if (mode == STATE_COMMENT) {
			comment(syntax, token, start);
			return true;
		}
		if (mode == STATE_STRING || mode == STATE_CHARACTER) {
			quoted(syntax, token, start, mode == STATE_STRING ? '"' : '\'');
			syntax->blank = false;
			return true;
		}
		if (mode == STATE_LINE_COMMENT || (c == '/' && peek(syntax, 1) == '/')) {
			syntax->at = syntax->size;
			set_mode(syntax, STATE_LINE_COMMENT);
			token_set(token, start, syntax->at, SYNTAX_COMMENT);
			return true;
		}
		if (c == '/' && peek(syntax, 1) == '*') {
			syntax->at += 2;
			comment(syntax, token, start);
			return true;
		}
		if (blank(c)) {
			syntax->at++;
			continue;
		}
		if (c == '#' && syntax->blank) {
			syntax->blank = false;
			directive(syntax, token);
			return true;
		}
		syntax->blank = false;
		if ((prefix = literal(syntax))) {
			syntax->at += prefix;
			syntax->tag = false;
			quoted(syntax, token, start, syntax->bytes[syntax->at - 1]);
			return true;
		}
		if (c == '<' && syntax->include) {
			auto rest = arrview_cfront(start, array_ptr(syntax->bytes, syntax->size));
			const char *end = memchr(*rest, '>', ARRAY_SIZE(rest));

			syntax->at = end ? (size_t)(end + 1 - syntax->bytes) : syntax->size;
			token_set(token, start, syntax->at, SYNTAX_STRING);
			return true;
		}
		if (digit(c) || (c == '.' && digit(peek(syntax, 1)))) {
			number(syntax);
			syntax->tag = false;
			token_set(token, start, syntax->at, SYNTAX_NUMBER);
			return true;
		}
		if (identifier(c)) {
			while (identifier(peek(syntax, 0)))
				syntax->at++;
			token_set(token, start, syntax->at, classify(syntax, start));
			if (token->class != SYNTAX_PLAIN)
				return true;
			continue;
		}
		if ((c == '[' || c == ']') && peek(syntax, 1) == c) {
			syntax->attribute = c == '[';
			syntax->at += 2;
		} else {
			syntax->at++;
		}
		syntax->tag = false;
	}
	return false;
}

/* A block comment goes on by itself, the rest only after a backslash that ends the line */
uint8_t syntax_end(const struct syntax *syntax)
{
	bool continued = syntax->size && syntax->bytes[syntax->size - 1] == '\\';

	if ((syntax->state & STATE_MODE) != STATE_COMMENT && !continued)
		return STATE_CODE;
	return syntax->state;
}

static uint8_t end_state(const struct text *text, size_t line)
{
	size_t start = text_line_start(text, line);
	struct syntax_token token;
	struct syntax syntax;

	syntax_init(&syntax, arrview(start, text_line_end(text, line) - start, array_ptr(text->bytes, text->size)),
		    text->states[line]);
	while (syntax_next(&syntax, &token))
		;
	return syntax_end(&syntax);
}

void syntax_update(struct text *text, size_t first, size_t last)
{
	for (size_t line = first; line + 1 < text->line_count; line++) {
		uint8_t state = end_state(text, line);

		if (line >= last && text->states[line + 1] == state)
			return;
		text->states[line + 1] = state;
	}
}
