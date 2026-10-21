#!/bin/sh
# Make the animation of the README on a KDE Plasma desktop: ce starts in a small C project with the keys showing, opens
# a file from the tree with the keyboard, gets a line typed and goes to the definition of a function and back. clangd,
# if it is installed, colors what it knows. The input needs fake-input of the tests, see tests/fake-input.md.
#
#   tests/animation.sh [ANIMATION]
#
# BUILD is the build directory, which is set up if it isn't there.
set -eu

cd "$(dirname "$0")/.."
build=${BUILD:-build}
geometry=${GEOMETRY:-0,0,880,544}
animation=$(realpath -m "${1:-data/demo.webp}")
project=/tmp/count
pid=

[ -d "$build" ] || meson setup "$build"
ninja -C "$build" src/ce tests/fake-input
# The status bar shows the path of the project, which is the same in every animation
mkdir "$project"
work=$(mktemp -d)
frames=$work/frames
trap 'if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi; rm -rf "$work" "$project"' EXIT
mkdir -p "$project/include" "$project/src" "$project/.build" "$frames"

cat >"$project/.clangd" <<'EOF'
CompileFlags:
  CompilationDatabase: .build
EOF

# clangd indexes the files of the database, so definitions in other files are found
cat >"$project/.build/compile_commands.json" <<EOF
[
	{ "directory": "$project", "file": "src/count.c", "command": "cc -std=c23 -Iinclude -c src/count.c" },
	{ "directory": "$project", "file": "src/main.c", "command": "cc -std=c23 -Iinclude -c src/main.c" },
	{ "directory": "$project", "file": "src/print.c", "command": "cc -std=c23 -Iinclude -c src/print.c" }
]
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
	fprintf(file, "%zu lines\n", count->lines);
	fprintf(file, "%zu words\n", count->words);
	fprintf(file, "%zu bytes\n", count->bytes);
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

# Input for drive.sh, quoted for eval, and the frames that it captures with the milliseconds that they show
steps=
frames_shown=
count=0

input() {
	for arg; do
		steps="$steps '$(printf %s "$arg" | sed "s/'/'\\\\''/g")'"
	done
}

frame() {
	count=$((count + 1))
	input run "spectacle -b -n -a -o $frames/$(printf %03d $count).png"
	frames_shown="$frames_shown -d $1 $frames/$(printf %03d $count).png"
}

frame 600
input key down
frame 600
input key right
frame 600
input key right
frame 600
input key down
frame 700
input key enter sleep 1500
frame 1500
input key ctrl+end key up key up key up key end
frame 700
input key enter
frame 300
text='fputs("done\n", stderr);'
while [ -n "$text" ]; do
	rest=${text#?}
	input type "${text%"$rest"}"
	frame 90
	text=$rest
done
input sleep 1500
frame 1500
input key up key home
frame 900
input key ctrl+enter sleep 1000
frame 1200
input key down
frame 500
input key down
frame 1000
input key alt+left sleep 300
frame 2500

"$build/src/ce" "$project" &
pid=$!
sleep 3
eval "set -- $steps"
BUILD=$build GEOMETRY=$geometry BORDER=0 tests/drive.sh "$work/last.png" "$@"
img2webp -loop 0 -min_size -lossless $frames_shown -o "$animation"
