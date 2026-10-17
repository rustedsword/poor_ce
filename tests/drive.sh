#!/bin/sh
# Try the editor on a KDE Plasma desktop: raise its window at GEOMETRY, check that it is the active window, give it
# input with fake-input and capture it with spectacle. Positions of the input are pixels of the capture.
#
#   tests/drive.sh CAPTURE.png [COMMAND...]
#
# BUILD is the build directory, GEOMETRY is X,Y,WIDTH,HEIGHT of the window frame in logical pixels.
set -eu

build=${BUILD:-build}
geometry=${GEOMETRY:-0,0,1280,800}
capture=$1
shift
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

IFS=, read -r x y width height <<GEOMETRY
$geometry
GEOMETRY

# The editor is the window of the one process of it
pid=$(pidof poor_text_viewer || true)
case $pid in
'' | *' '*)
	echo "drive.sh: one editor has to run, not '$pid'" >&2
	exit 1
	;;
esac

# The desktop is the box around the enabled outputs, the scale is that of the output with the window
outputs=$(kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g' | awk '
	/^Output:/ { enabled = 0 }
	/^\tenabled/ { enabled = 1 }
	/^\tGeometry:/ && enabled { split($2, at, ","); split($3, size, "x"); position = at[1] " " at[2] " " size[1] " " size[2] }
	/^\tScale:/ && enabled { print position, $2 }')
desktop=$(echo "$outputs" | awk '{ if ($1 + $3 > w) w = $1 + $3; if ($2 + $4 > h) h = $2 + $4 } END { print w "x" h }')
scale=$(echo "$outputs" | awk -v x="$x" -v y="$y" '
	x >= $1 && x < $1 + $3 && y >= $2 && y < $2 + $4 { print $5; exit }')

cat >"$work/raise.js" <<SCRIPT
for (const window of workspace.windowList()) {
	if (window.resourceClass == "poor_text_viewer" && window.pid == $pid) {
		window.minimized = false;
		window.frameGeometry = { x: $x, y: $y, width: $width, height: $height };
		workspace.activeWindow = window;
	}
}
SCRIPT
cat >"$work/check.js" <<SCRIPT
const window = workspace.activeWindow;
if (window && window.resourceClass == "poor_text_viewer" && window.pid == $pid)
	callDBus("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript", "$work/marker.js", "drive-active");
SCRIPT
: >"$work/marker.js"

kwin() {
	qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting."$@"
}

run_script() {
	kwin unloadScript drive >/dev/null
	id=$(kwin loadScript "$1" drive)
	qdbus6 org.kde.KWin /Scripting/Script"$id" org.kde.kwin.Script.run >/dev/null
}

# Scripts of KWin answer nothing, but the check loads the marker if the editor is the active window
editor_active() {
	kwin unloadScript drive-active >/dev/null
	run_script "$work/check.js"
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		if [ "$(kwin isScriptLoaded drive-active)" = true ]; then
			kwin unloadScript drive-active >/dev/null
			kwin unloadScript drive >/dev/null
			return 0
		fi
		sleep 0.1
	done
	kwin unloadScript drive >/dev/null
	return 1
}

run_script "$work/raise.js"
sleep 0.5
kwin unloadScript drive >/dev/null
if ! editor_active; then
	echo "drive.sh: the active window isn't the editor" >&2
	exit 1
fi

# Positions are pixels of the capture only if the window got the size
spectacle -b -n -a -o "$work/active.png" >/dev/null 2>&1
size=$(od -An -tu1 -j16 -N8 "$work/active.png" | awk '{ printf "%dx%d", $1 * 16777216 + $2 * 65536 + $3 * 256 + $4, $5 * 16777216 + $6 * 65536 + $7 * 256 + $8 }')
expected=$(awk -v w="$width" -v h="$height" -v s="$scale" 'BEGIN { printf "%dx%d", w * s, h * s }')
if [ "$size" != "$expected" ]; then
	echo "drive.sh: the editor is $size, it would be $expected" >&2
	exit 1
fi
if [ $# -gt 0 ]; then
	"$build/tests/fake-input" -d "$desktop" -o "$x,$y" -s "$scale" "$@"
	sleep 0.4
	if ! editor_active; then
		echo "drive.sh: the editor lost the focus, some input may have gone to another window" >&2
		exit 1
	fi
fi
spectacle -b -n -a -o "$capture" >/dev/null 2>&1
