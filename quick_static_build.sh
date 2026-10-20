#!/bin/sh
# An optimized static build in build-release: quick_static_build.sh [THEME] [FONT_SIZE]. A number is the font size, a
# word the theme, and what isn't given stays as meson_options.txt has it.
set -eu

theme= font_size=
for argument; do
	case $argument in
	*[!0-9]*) theme=$argument ;;
	*) font_size=$argument ;;
	esac
done
meson setup --wipe build-release --buildtype=release ${theme:+"-Dtheme=$theme"} ${font_size:+"-Dfont_size=$font_size"} \
	-Db_lto=true -Db_ndebug=if-release -Dtests=false -Ddefault_library=static \
	--force-fallback-for=poor_loop,poor_base,liburing
ninja -C build-release
