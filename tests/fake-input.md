# Trying the editor with fake input

`fake-input` is a virtual keyboard and mouse made through `/dev/uinput`: the compositor takes its events for those of
a person. `drive.sh` uses it on a KDE Plasma desktop to try the editor for real and capture its window.

## Needs

- Write access to `/dev/uinput`, for example `sudo setfacl -m u:$USER:rw /dev/uinput` until the next boot.
- For `drive.sh`: KWin, `qdbus6`, `spectacle` and `kscreen-doctor` of Plasma 6.
- `build/tests/fake-input`, which is built with the tests: `ninja -C build`.

## drive.sh

    tests/drive.sh CAPTURE.png [COMMAND]...

It brings the window of the editor, of which one has to run, to the front at `GEOMETRY`, asks KWin whether the active
window is the one of that process, gives it the commands and captures it into `CAPTURE.png`. Without commands it only
captures. If the active window isn't the editor, nothing is sent: input goes to whatever window has the focus. If the
editor doesn't have the focus after the commands, some of them may have gone to another window, so it fails without a
capture.

Positions are pixels of the capture, so they can be read from the last one. On a display with a scale of 2 the
capture of the default window of 1280x800 is 2560x1600 pixels.

| Variable   | Default        | Meaning                                       |
|------------|----------------|-----------------------------------------------|
| `BUILD`    | `build`        | build directory with `tests/fake-input`       |
| `GEOMETRY` | `0,0,1280,800` | X,Y,WIDTH,HEIGHT of the window, logical pixels |

## Commands

| Command             | What it does                                         |
|---------------------|------------------------------------------------------|
| `key NAME[+NAME]`   | press keys together: `key ctrl+s`, `key alt+left`     |
| `down NAME`         | hold a key, for clicks with Ctrl; `up NAME` lets it go |
| `type TEXT`         | type ASCII text with the keys of a US layout          |
| `move X Y`          | move the pointer                                      |
| `click X Y`         | left click, and `middle`, `right` and `double`        |
| `drag X Y X Y`      | drag with the left button                             |
| `wheel N`           | turn the wheel by N notches, positive is up           |
| `sleep MS`          | wait, for example for clangd                          |
| `run COMMAND`       | run a shell command and wait for it, like a capture   |

Key names: letters, digits, `f1`-`f12`, `ctrl`, `shift`, `alt`, `altgr`, `super`, `esc`, `tab`, `enter`, `space`,
`backspace`, `delete`, `insert`, `home`, `end`, `pageup`, `pagedown`, `left`, `right`, `up`, `down`, `minus`, `equal`,
`leftbrace`, `rightbrace`, `semicolon`, `apostrophe`, `grave`, `backslash`, `comma`, `dot`, `slash`.

## Example

    $ build/src/ce . &
    $ tests/drive.sh /tmp/shot.png                    # look where things are
    $ tests/drive.sh /tmp/shot.png click 133 463      # open a file in the tree
    $ tests/drive.sh /tmp/shot.png down ctrl click 1510 1027 up ctrl sleep 1500
    $ tests/drive.sh /tmp/shot.png key alt+left       # back from the definition
    $ tests/drive.sh /tmp/shot.png key end type ' // note' key ctrl+z key ctrl+z   # typing undoes by words

`fake-input` works without `drive.sh` too. Its positions are logical pixels of the whole desktop, whose size it
needs, unless `-o X,Y` moves their origin and `-s SCALE` divides them:

    $ build/tests/fake-input -d 1280x800 move 640 400 wheel -3

## When done

While input is faked, the desktop isn't the user's. Quit the editor once you are done with it and send a critical
notification, which stays until it is closed, so that the user knows the desktop is theirs again:

    $ kill -TERM $(pidof ce)
    $ notify-send -u critical ce 'Done with the desktop, it is yours again'
