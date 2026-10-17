#!/bin/sh
# Make the screenshot of the README on a KDE Plasma desktop: ce in a small C project with three files open and one of
# its two directories expanded, without the decoration of the window. clangd, if it is installed, colors what it
# knows. The click that expands the directory needs fake-input of the tests, see tests/fake-input.md.
#
#   tests/screenshot.sh [CAPTURE]
#
# BUILD is the build directory, which is set up if it isn't there.
set -eu

cd "$(dirname "$0")/.."
build=${BUILD:-build}
geometry=${GEOMETRY:-0,0,880,544}
capture=$(realpath -m "${1:-data/screenshot.png}")
work=$(mktemp -d)
project=$work/count
pid=
trap 'if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi; rm -rf "$work"' EXIT

[ -d "$build" ] || meson setup "$build"
ninja -C "$build" src/ce tests/fake-input
mkdir -p "$project/include" "$project/src"

cat >"$project/.clangd" <<EOF
CompileFlags:
  Add: [-std=c23, -I$project/include]
EOF

cat >"$project/Makefile" <<'EOF'
CFLAGS = -std=c23 -Wall -Wextra -O2 -Iinclude
OBJECTS = src/main.o src/count.o src/print.o

count: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

$(OBJECTS): include/count.h

clean:
	rm -f count $(OBJECTS)
EOF

cat >"$project/include/count.h" <<'EOF'
#ifndef COUNT_H
#define COUNT_H

#include <stddef.h>
#include <stdio.h>

/* Lines, words and bytes of a text, as wc counts them */
struct count {
	size_t lines, words, bytes;
	bool in_word;
};

#define COUNT_INIT ((struct count){})

void count_char(struct count *count, int c);
void count_print(const struct count *count, FILE *file);

#endif
EOF

cat >"$project/src/count.c" <<'EOF'
#include <ctype.h>

#include "count.h"

void count_char(struct count *count, int c)
{
	bool space = isspace(c);

	count->bytes++;
	if (c == '\n')
		count->lines++;
	if (!space && !count->in_word)
		count->words++;
	count->in_word = !space;
}
EOF

cat >"$project/src/print.c" <<'EOF'
#include "count.h"

void count_print(const struct count *count, FILE *file)
{
	fprintf(file, "%zu %zu %zu\n", count->lines, count->words, count->bytes);
}
EOF

cat >"$project/src/main.c" <<'EOF'
/* Count the lines, words and bytes of the standard input */
#include <stdio.h>

#include "count.h"

int main(void)
{
	struct count count = COUNT_INIT;
	int c;

	while ((c = getchar()) != EOF)
		count_char(&count, c);
	count_print(&count, stdout);
	return 0;
}
EOF

# Positions of drive.sh are pixels of the capture, which has the scale of the output with the window
IFS=, read -r x y _ <<EOF
$geometry
EOF
scale=$(kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g' | awk -v x="$x" -v y="$y" '
	/^Output:/ { enabled = 0 }
	/^\tenabled/ { enabled = 1 }
	/^\tGeometry:/ && enabled { split($2, at, ","); split($3, size, "x"); inside = x >= at[1] && x < at[1] + size[1] && y >= at[2] && y < at[2] + size[2] }
	/^\tScale:/ && enabled && inside { print $2; exit }')

# The last file is the one that shows, clangd gets a moment for it. src is the second row of the tree.
"$build/src/ce" "$project" "$project/include/count.h" "$project/src/count.c" "$project/src/main.c" &
pid=$!
sleep 5
BUILD=$build GEOMETRY=$geometry BORDER=0 tests/drive.sh "$capture" \
	click $(awk -v s="$scale" 'BEGIN { printf "%d %d", 60 * s, 36 * s }')
