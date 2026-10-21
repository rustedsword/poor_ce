#!/bin/sh
# Make the social preview of the repository on a KDE Plasma desktop: ce with a big font in a small C project, its tree
# as wide as the longest name, next to the icon, the name and what ce is. GitHub takes it only in the settings of the
# repository. clangd, if it is installed, colors what it knows. The drag of the tree needs fake-input of the tests, see
# tests/fake-input.md.
#
#   tests/social-preview.sh [PREVIEW]
#
# BUILD is a build directory for FONT_SIZE, which is set up if it isn't there, GEOMETRY the window in logical pixels.
set -eu

cd "$(dirname "$0")/.."
size=${FONT_SIZE:-20}
build=${BUILD:-build-social}
geometry=${GEOMETRY:-0,0,760,420}
preview=$(realpath -m "${1:-data/social-preview.png}")
work=$(mktemp -d)
project=$work/count
pid=
trap 'if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi; rm -rf "$work"' EXIT

[ -d "$build" ] || meson setup "$build"
meson configure "$build" -Dfont_size="$size"
ninja -C "$build" src/ce tests/fake-input
mkdir -p "$project"

cat >"$project/.clangd" <<'EOF'
CompileFlags:
  Add: [-std=c23]
EOF

cat >"$project/Makefile" <<'EOF'
CFLAGS = -std=c23 -Wall -Wextra -O2

count: main.o count.o
	$(CC) $(LDFLAGS) -o $@ main.o count.o
EOF

cat >"$project/count.h" <<'EOF'
#ifndef COUNT_H
#define COUNT_H

#include <stddef.h>
#include <stdio.h>

struct count {
	size_t lines, words, bytes;
	bool in_word;
};

#define COUNT_INIT ((struct count){})

void count_char(struct count *count, int c);
void count_print(const struct count *count, FILE *file);

#endif
EOF

cat >"$project/count.c" <<'EOF'
#include <ctype.h>

#include "count.h"

void count_char(struct count *count, int c)
{
	bool space = isspace(c);

	count->bytes++;
	count->lines += c == '\n';
	count->words += !space && !count->in_word;
	count->in_word = !space;
}

void count_print(const struct count *count, FILE *file)
{
	fprintf(file, "%zu %zu %zu\n", count->lines, count->words, count->bytes);
}
EOF

cat >"$project/main.c" <<'EOF'
#include <stdio.h>
#include "count.h"

int main(void)
{
	struct count count = COUNT_INIT;
	for (int c; (c = getchar()) != EOF;)
		count_char(&count, c);
	count_print(&count, stdout);
	return 0;
}
EOF

# The splitter goes where the longest name of the tree ends: ce measures in pixels of the capture, which have the scale
# of the output with the window, a cell of its font is 0.6 of the size
IFS=, read -r x y width _ <<EOF
$geometry
EOF
scale=$(kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g' | awk -v x="$x" -v y="$y" '
	/^Output:/ { enabled = 0 }
	/^\tenabled/ { enabled = 1 }
	/^\tGeometry:/ && enabled { split($2, at, ","); split($3, size, "x"); inside = x >= at[1] && x < at[1] + size[1] && y >= at[2] && y < at[2] + size[2] }
	/^\tScale:/ && enabled && inside { print $2; exit }')
longest=$(ls "$project" | awk '{ if (length > n) n = length } END { print n }')
drag=$(awk -v size="$size" -v scale="$scale" -v width="$width" -v columns="$((longest + 3))" 'BEGIN {
	cell = int(0.6 * int(size * scale + 0.5) + 0.5)
	padding = int(4 * scale + 0.5)
	half = int(width * scale / 2 / cell)
	printf "%d 100 %d 100", (half < 28 ? half : 28) * cell + 2 * padding, 2 * padding + columns * cell + cell / 2 }')

"$build/src/ce" "$project" "$project/count.c" "$project/main.c" &
pid=$!
sleep 5
BUILD=$build GEOMETRY=$geometry BORDER=0 tests/drive.sh "$work/ce.png" drag $drag sleep 300
magick -respect-parenthesis -size 1280x640 xc:'#0d110e' \
	\( "$work/ce.png" -resize 760x -bordercolor '#323b33' -border 2 \) -gravity west -geometry +38+0 -composite \
	+gravity \( -background none -density 384 data/ce.svg -resize 96x96 \) -geometry +840+178 -composite \
	-font data/NotoSansMono-Regular.ttf -fill '#eef3e8' -pointsize 104 -annotate +950+258 ce \
	-fill '#a3ada2' -pointsize 26 -annotate +844+340 'Ultra simple text editor' -annotate +844+378 'with a nuclear core' \
	-fill '#b9f500' -pointsize 21 -annotate +844+440 'C · Wayland · Vulkan · io_uring' \
	-depth 8 -alpha off "$preview"
