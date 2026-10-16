# poor_wayland_text_viewer

A text viewer on [poor_wayland](https://github.com/rustedsword/poor_wayland) and
[poor_wayland_swapchain](https://github.com/rustedsword/poor_wayland_swapchain):
it shows a UTF-8 file in an xdg-shell window, drawn with Vulkan on the GPU of
the compositor and presented with explicit synchronization. Text is selected
with the pointer, which makes it the primary selection, and Ctrl+C copies it to
the clipboard. The wheel and the arrows, PageUp, PageDown, Space, Home and End
scroll, Ctrl+A selects all, Escape clears the selection and Ctrl+Q quits. The
pointer is an I-beam through cursor-shape-v1, the compositor draws the
decorations if it has xdg-decoration, and fractional scaling goes through the
viewport.

Everything runs on io_uring through poor_loop, reading the file included. The
font is Noto Sans Mono, embedded with `#embed` and drawn with FreeType.

It needs Linux 6.16 or newer, a compositor with linux-dmabuf 4 and
linux-drm-syncobj-v1, a Vulkan 1.3 driver, a C23 compiler, xkbcommon, FreeType
and glslangValidator. Installed poor_wayland, poor_wayland_swapchain and
wayland-protocols are used if there are, the submodules otherwise.

    $ meson setup build/
    $ ninja -C build/
    $ build/src/poor_text_viewer FILE

The font is licensed under the SIL Open Font License, see
[data/OFL.txt](data/OFL.txt).
