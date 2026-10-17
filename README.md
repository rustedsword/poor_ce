# poor_wayland_text_viewer

An ultra simple editor for C projects: a file tree, tabs, highlighting of C and going to definitions through clangd.
It runs on [poor_wayland](https://github.com/rustedsword/poor_wayland) and
[poor_wayland_swapchain](https://github.com/rustedsword/poor_wayland_swapchain), draws with Vulkan and does all of
its I/O through io_uring. The keys show in its window while no file is open.

## Build

It needs Linux 6.16, liburing 2.12, a compositor with linux-dmabuf 4 and linux-drm-syncobj-v1, a Vulkan 1.4 driver, a
C23 compiler, xkbcommon, FreeType and glslangValidator. The submodules are used for what isn't installed.

    $ meson setup build/
    $ ninja -C build/

## Run

    $ build/src/poor_text_viewer [DIRECTORY] [FILE]...

The directory, the current one by default, is the project. clangd, if it is installed, finds its
`compile_commands.json` there or in `build/`.

The font is Noto Sans Mono, licensed under the SIL Open Font License, see [data/OFL.txt](data/OFL.txt).
