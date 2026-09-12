# 9. The desktop: Liquid Platinum on Wayland

MaryOS boots to a login prompt (`vm.sh`) or to a desktop (`ui.sh`) from the same
image. The desktop is `maryui-desktop`, a Wayland compositor built on wlroots
that draws the whole screen itself — wallpaper, an ambient clock, window frames,
Spotlight with the app commands as pills, the Finder's context menu, and five
built-in apps — from the Liquid Platinum design system. There is no menu bar:
File / Edit / View / Window / Help ride inside Spotlight. Icons are lit metal
objects (a key light, a rim, the brushed grain, one contact shadow) above 19px
and plain glyphs at or below it. This chapter
explains where that code lives, how the builder compiles it, how the image
starts it, and how to change it.

## MaryUI and the parity contract

The design system is the sibling repository **MaryUI**. Its `web/` directory is
the reference: a React desktop with a window manager, a spring-and-slosh motion
engine, 24 components and the Finder, Gallery, About and TextEdit apps, all driven by
`web/tokens/tokens.json` (W3C design tokens). Its `linux/` directory is the same
system in C:

- `libmaryui` — Cairo and Pango drawing, the feTurbulence brush and wallpaper,
  an immediate-mode UI (`lp_ctx`), one C file per web component, the ported
  window-manager reducer and motion physics, Spotlight's model, the filesystem
  model behind the Finder (`lp_files`), and the five apps;
- `maryui-desktop` — the compositor: outputs, input, the scene graph, chromes
  (Cairo buffers shown as scene nodes), windows, xdg-shell clients with
  server-side decorations, and the frame loop that steps the motion engine.

`tokens.json` is the contract. `npm run tokens` in `MaryUI/web` regenerates
`linux/include/maryui/lp_tokens.h` and `lp_icons.h`; the ported files carry
the same tests, case for case; `MaryUI/linux/PARITY.md` lists every web file,
its C twin and every deliberate deviation, and `make parity` there fails when a
component is missing on either side. Customising MaryOS's look means editing
MaryUI: tokens for colours, sizes and springs; `src/components/` for the
metal; `src/apps/` for the built-in apps; `src/core/lp_desktop.c` for the menus
and shortcuts.

## Where the sources come from

The kit imports MaryUI in two ways, resolved by one rule everywhere (`build.sh`,
`run-in-docker.sh`, the Swift `KitPaths`):

| Source | When | How |
|---|---|---|
| `linux/maryui/` (git submodule, pinned) | default | `git submodule update --init`; the builder reads `linux/maryui/linux` |
| `$MARYUI_DIR/linux` (a sibling checkout) | `MARYUI_DIR` is set | live edits: `MARYUI_DIR=~/repos/MaryUI make ui`; `run-in-docker.sh` mounts it read-only at `/maryui` |

`maryos doctor` reports which one is in use and whether `out/ui` is built;
`maryos config` prints the MaryUI path and the built desktop's commit. The
submodule carries MaryUI's `linux/` tree, so `make ui` works from a clean clone;
`MARYUI_DIR` is for editing the design system and the distro together.

## The `ui` stage

`builder/build.sh ui` (`lib/ui.sh`) is the stage between `rootfs` and `target`:

1. `make -C $MARYUI_SRC O=/build/ui all` in the arm64 builder container, which
   has the wlroots, Wayland, Cairo, Pango and font development packages;
2. `make test` — the C tests (`MARYUI_SKIP_TESTS=1` skips them);
3. `make install DESTDIR=out/ui.tmp PREFIX=/usr`, `lp-render --all` into
   `renders/` (PNGs of the brush, both wallpapers, the clock, a window, the
   Finder, every Gallery tab, TextEdit, and Spotlight as the dock and with
   results, for comparing with the web), and
   `usr/share/maryui/maryui.env` with the MaryUI commit and build time;
4. the two 1280×800 wallpapers copied out of `renders/` into
   `usr/share/maryui/`. They are the only renders that ship: `lp_wallpaper`
   looks there before rendering its own, and the molten one costs about 7.5
   seconds under llvmpipe, so baking it here means neither a first boot nor a
   dev-mode restart ever pays for it;
5. an atomic rename to `out/ui`.

`build.sh all` runs it once before the targets. The `target` stage then installs
`packages/desktop.list`, copies `overlay-desktop/`, unpacks `out/ui` into the
rootfs (`ui_install`, then `ldconfig`) and runs `hooks/desktop/`. The image
manifest gains a `desktop:` line with the MaryUI commit. Because all of this
happens at the target stage, the cached base rootfs is untouched: a desktop
change costs a `make ui` (seconds) or a target build (minutes), never a
bootstrap.

```
out/ui/usr/bin/maryui-desktop, lp-render, lp-input
out/ui/usr/lib/libmaryui.so.0, pkgconfig/maryui.pc
out/ui/usr/include/maryui/*.h
out/ui/usr/share/maryui/maryui.env, README.md, PARITY.md
out/ui/renders/*.png
```

## What the image carries

- **`packages/desktop.list`**: `libwlroots12t64`, libseat, libinput, xkbcommon,
  Wayland, pixman, Cairo, Pango, fontconfig, the DRM/GBM/EGL/GLES libraries
  (`libegl-mesa0` among them: the molten wallpaper renders through a surfaceless
  EGL context, v3d on a Pi and llvmpipe in the VM), `libpam-systemd` and
  `polkitd` for the logind session, the fonts (Inter, URW base35 for P052,
  JetBrains Mono) and `foot`, the terminal.
- **`usr/share/maryui/`**: `README.md`, `PARITY.md`, `maryui.env` and the two
  prerendered 1280×800 wallpapers.
- **`overlay-desktop/usr/lib/maryos/desktop`**: the launcher. It sets
  `WLR_RENDERER=pixman` when the GPU is virtio (no 3D in Virtualization.framework),
  then either `exec`s `/usr/bin/maryui-desktop` or, when the kernel command line
  has `maryos.ui=dev`, runs the binary from `/mnt/maryos-out/ui` and restarts it
  whenever that file's mtime changes (`binary changed; restarting`).
- **`overlay-desktop/etc/pam.d/maryos-desktop`**: a PAM service that opens a
  logind session for the desktop's user without a password prompt
  (`pam_permit` for auth, the common account and session stacks).
- **`hooks/desktop/60-desktop.sh`**: writes and enables
  `maryos-desktop.service`, sets the default target back to `multi-user.target`
  (Ubuntu's `graphical.target` default would otherwise start the desktop on every
  boot), and refreshes the font cache.

The unit is the whole display-manager replacement:

```
[Unit]     After=systemd-user-sessions.service systemd-logind.service maryos-firstboot.service getty@tty1.service
           Wants=systemd-logind.service   Conflicts=getty@tty1.service
[Service]  User=mary  PAMName=maryos-desktop  TTYPath=/dev/tty1  TTYReset=yes  TTYVHangup=yes  TTYVTDisallocate=yes
           StandardInput=tty-fail  StandardOutput=journal+console  UtmpIdentifier=tty1  UtmpMode=user
           Environment=XDG_SESSION_TYPE=wayland  ExecStart=/usr/lib/maryos/desktop  Restart=on-failure
[Install]  WantedBy=graphical.target  Alias=display-manager.service
```

`PAMName` + `TTYPath` give the process a logind session on seat0, VT 1, which is
what libseat needs to take the GPU and input devices without root;
`Conflicts=getty@tty1` frees the VT; the alias makes the first-boot service's
`Before=display-manager.service` ordering apply.

## Two boot modes

| Command | Kernel arguments added | Result |
|---|---|---|
| `./vm.sh` (`maryos vm run`) | — | `multi-user.target`: login prompt on `tty1` and `hvc0` |
| `./ui.sh` (`maryos vm run --desktop --dev`) | `systemd.unit=graphical.target maryos.ui=dev` | the desktop from `out/ui` over virtiofs, restarted on change; the login prompt still on `hvc0` |
| `./ui.sh --image` (`--desktop`) | `systemd.unit=graphical.target` | the desktop embedded in the image, on `tty1` |

`ui.sh` compiles the desktop on every run (`maryos build --stage ui`, about ten
seconds when nothing changed, starting Docker Desktop if it is not up) and stops
if that fails, so it never boots a stale desktop. With a VM already running it
only compiles, and the running VM restarts its desktop on the new build.
`--no-build` skips the compile; `--rebuild` rebuilds the image around the fresh
desktop and boots it (`--rebuild` implies `--image` and `--fresh`, so the VM does
not reuse a disk older than the image). `--dev` and `--rebuild-ui` are accepted
and are now the default. On the Pi, `cmdline.txt` ends with `systemd.unit=graphical.target`, so a
card boots to the desktop. The Swift side models this as `VMBootMode`
(`console`, `desktop(dev:)`) and appends the arguments to `boot.json`'s command
line.

## The dev loop

```sh
cd linux
./ui.sh                                            # compile, then boot to the desktop, running out/ui
# edit C in MaryUI/linux (or tokens in MaryUI/web, then `npm run tokens` there)
MARYUI_DIR=~/repos/MaryUI ./ui.sh                  # recompile; the running VM picks it up (or: make ui)
# within three seconds the launcher logs "binary changed; restarting" and the new desktop is up
```

`make desktop` and `make ui-dev` both run `./ui.sh`; `./ui.sh --image` boots the
desktop the image carries instead. `journalctl -u maryos-desktop -b` in the guest (over `hvc0` or ssh) has the
compositor's log: the output mode, the renderer, `xdg: foot mapped as w3`, and
with `MARYUI_DEBUG=frames` in the service environment (`systemctl set-environment
MARYUI_DEBUG=frames; systemctl restart maryos-desktop`) the motion engine's
`motion: wake` / `motion: idle` lines and a frame-time histogram every five
seconds while frames run.

## Spotlight

`Ctrl+Space` (or `Super+Space`) opens Spotlight: a platinum panel centred on the
desktop, its pill-shaped search bar reading “Say “Hey Mary” or type something…”.
The bar is also the dock. With nothing typed, a row of tiles below it shows the
pinned applications — each `lp_app` with `dock` set: Finder and TextEdit today,
plus Terminal — with a dot under the ones that have a window open. Every other
app (Gallery, About, and the system apps as they arrive) is one keystroke away:
typing ranks all of them — titles that start with the text first, then words that
start with it, then anything containing it; open windows are listed too
(“Window · Finder”), eight results at most. `↑`/`↓` move the selection (and `←`/`→`
in the dock), `Enter` launches — an app opens, a window comes forward, Terminal
spawns `foot` until the native Terminal app is registered and then opens that —
`Esc` or a click outside closes.
While it is up, the window shortcuts stand down and clients receive no keys.

It lives in its own scene layer above the menus (`z.spotlight`), appears with the
menu's fade-and-scale over 120 ms (none under Reduce Motion), and is hit only
inside the panel: its shadow falls through to whatever lies beneath. The model
(open, query, selection, ranking) is `lp_spotlight.c`, the same code as the web's
`spotlight.ts`; the panel is a component (`Spotlight.c`) that other distros can
paint anywhere; `src/compositor/spotlight.c` is the host.

## Finder

The Finder is a file manager for the guest's filesystem. Its window has a toolbar
(Back and Forward, icon or list view, search), a source-list sidebar, the folder's
contents, a path bar of crumbs and a status bar. The sidebar's Favorites are the
user's home (shown by name, `mary`), Desktop, Documents and Downloads — the image
creates them and drops a `Welcome to MaryOS.txt` into Documents
(`hooks/desktop/65-user-dirs.sh`); its Locations are the system volume (named after
`/etc/os-release`), anything mounted under `/media`, `/mnt` or `/run/media` (the VM's
`maryos-out` share appears there) and the Trash. The window's title is the folder's
name; the crumbs are clickable; `⌘N` opens another window at home.

Items sort the Finder's way (case-insensitive, digit runs as numbers, folders and
files mixed) and the list's header sorts by Name, Date Modified, Size or Kind; sizes
and dates read “6 KB”, “1.1 MB”, “Today, 4:12 PM”, “Yesterday, 3:48 PM”, “Sep 5, 2026”.
The search filters by name; `⇧⌘.` shows dotfiles.

Opening: a double-click, `⌘O` or `⌘↓` enters a folder or opens a text file in
TextEdit; other files say so in the status bar. `⌘↑` goes to the enclosing folder and selects where you came from,
`⌘[` / `⌘]` walk the history, the Go menu and `⇧⌘H` / `⇧⌘D` / `⇧⌘O` / `⌥⌘L` jump to
the favorites. Selecting: click, `⌘`-click to add, `⇧`-click for a range, a marquee
dragged over empty space, `⌘A`, the arrow keys (two-dimensional in icon view),
Home/End, and type-ahead (type the first letters of a name).

Managing: `⇧⌘N` makes “untitled folder” and starts renaming it; Return renames the
selection inline (Esc cancels, a click elsewhere commits); `⌘⌫` moves the selection
to the Trash (`~/.local/share/Trash`, with `.trashinfo` records, as freedesktop.org
specifies; inside the Trash the same key deletes for good, and File › Empty Trash
clears it); `⌘D` duplicates (“x copy.txt”, “x copy 2.txt”); `⌘C` / `⌘X` and `⌘V`
copy or move through a process-wide file clipboard, `⌥⌘V` moves what was copied; `⌘I`
opens an Info window (kind, size, where, dates, owner, permissions, an Open
button). A right-click opens a context menu with the same commands on the item
(or on the folder, from empty space). Every operation is in `lp_files`
(MaryUI `src/core/lp_files.c`), which returns `-errno` and never touches the UI;
errors land in the status bar.

Drag and drop: press an item and move, and the selection lifts off as a ghost (its
tile, a count badge for several) that follows the pointer above everything.
Folders, the sidebar's places and the empty area of any Finder window light up
with an accent ring as targets; releasing moves the items (copies with Alt held,
or when the target is on another device — the ghost shows a “+”), dropping on the
Trash trashes them, Esc or a right-click cancels. The compositor runs the drag as a
grab (`src/compositor/drag.c`): app windows under the pointer receive `HOVER`,
`LEAVE` and `DROP` events through `lp_input.drag`, nothing is focused or raised by
a hover, and the release never reaches the window the drag began in.

Live refresh: each Finder window watches its folder with inotify
(`src/compositor/files.c`); events are coalesced for 50 ms and broadcast as
`lp_desktop_files_changed`, so a file saved from TextEdit or touched in a terminal
appears without any input, and an idle desktop still schedules no frames.

The apps grew four hooks for this (`lp_app.h`): `open` (a path handed over at open
time — `lp_desktop_open_path` sends folders to the Finder and text files to
TextEdit), `command` and `menu_entries` (the focused app's File, Edit, View and
the new Go menu, and the context menu, all routed as `LP_CMD_APP`), and `notify`.

## TextEdit

A plain-text editor. Spotlight opens it on `~/Documents/Untitled.txt`; the Finder
opens it on any text file (by extension, or by sniffing for UTF-8 without NUL
bytes). A toolbar holds the document's name and a Save button, the body is a
TextArea, and a status bar counts words and characters and shows `Ln, Col`; an
edited document carries a `•` before its name in the title bar. `⌘/Ctrl+S` (or
Save) writes the document back where it came from — a new one goes to
`~/Documents/<name>`, with `.txt` added only when the name has no extension — and
tells every Finder window showing that folder; `⌘/Ctrl+O` opens a Finder window
on the document's folder.

The TextArea is the library's own: a UTF-8 document with a caret and an anchor,
wrapped through Pango, click to place, drag to select, `↑`/`↓` by visual line,
Home/End, `⌘/Ctrl+A/C/X/V` through an in-process clipboard (nothing crosses to
Wayland clients yet), the wheel to scroll. Two compositor gaps closed with it:
keys a chrome consumes now repeat while held (clients always repeated their own),
and the pointer shows an I-beam over text.

## The system apps

Linux grows applications the web design preview never will; each is a built-in
`lp_app` in MaryUI (`src/apps/<name>.c`) over a model with its own tests
(`src/core/lp_<name>.c`), recorded as PARITY D15. Only some are pinned to the
dock; the rest are one Spotlight query away.

The plumbing they share is in the desktop model. An app can ask for **event
sources** — `lp_desktop_add_fd` for a pty or an eventfd, `lp_desktop_add_timer`
for a refresh tick — which the compositor backs with its `wl_event_loop`
(`src/compositor/sources.c`) and the tests with a `poll` loop, and which
`lp-render` does not offer, so every app draws without them. **Files route by
kind**: a picture or a PDF opens in Preview and audio or video in the Media
Player once those are registered, and none of them ever falls through to
TextEdit. The libraries the apps build on — vterm, gdk-pixbuf, poppler-glib,
GStreamer, libical, libsystemd — are each optional in MaryUI's Makefile, so the
library still builds on a Mac; the builder image has all of them, and
`distro/packages/desktop.list` carries their runtime halves.

**Calculator** (Spotlight: “calc”) is a fixed 300×420 window: a display well
with the expression above the value, and a keypad of platinum buttons — memory
row, AC/C, ±, %, the four operators and an accent `=`. Precedence is honoured
(2 + 3 × 4 = 14), `%` is a percentage of what it is added to (50 + 10 % = 55),
`=` repeats the last operation, and a division by zero reads `Error` until a
number or a clear. Every key has a keyboard twin (digits, `+ - * x /`, `=` or
`Return`, `%`, `Backspace`, `Esc`), and Edit › Copy / Paste (⌘C / ⌘V) move plain
numbers through the desktop's text clipboard. `lp-render --calculator` paints it
mid-sum.

**Preview** (pinned; the Finder opens every picture and PDF in it) shows one
document per window, fitted to the window without ever enlarging it, on a grey
desk under a toolbar — previous/next document, page up/down for a PDF, zoom
out/in, fit, rotate — and over a status bar with the name, the size in px or pt,
and the scale. Pictures decode through gdk-pixbuf (SVG through librsvg's loader,
EXIF orientation applied, anything beyond 8192px decoded smaller); PDFs render
through poppler-glib; a build without gdk-pixbuf still opens PNGs through Cairo
and says so for anything else. A page is drawn from a cache made at its exact
on-screen size, so a repaint copies pixels rather than resampling them and a
quarter turn is exact; zoomed past 4096² it is rendered straight through the
viewport instead of cached whole. Keys: `←`/`→` through the folder's pictures and
PDFs (wrapping, hidden files skipped), `Page Up`/`Page Down` or `Space` through
pages, ⌘0 actual size, ⌘9 fit, ⌘+ / ⌘− (and ⌘ with the wheel) through fixed steps
from ⅛ to 16× about the centre, ⌘L / ⌘R to turn. View, Go and File carry the same
commands. `lp-render --preview` shows the procedural wallpaper as a picture.

## Inside the compositor

Five scene layers: wallpaper, windows, the clock, menus, Spotlight. Everything the library
paints is a *chrome*: a Cairo buffer wrapped as a `wlr_buffer` and shown as a
`wlr_scene_buffer`; three rotate so the renderer never reads a buffer being
painted. A chrome's paint function runs an EVENT pass on input (no Cairo; hit
tests and state) and a DRAW pass when dirty, clipped to the damage, so a hover
repaints one control and a moving sheen repaints one title bar. Windows are
scene subtrees: the frame chrome plus, for clients, the surface tree at the body.
The window manager (`lp_wm`, the web reducer) owns the state; `mui_windows_sync`
brings the scene in step after every action.

Motion is the web's engine, ported: one loop that steps every window's springs
while any still moves and stops when settled. Drags feed a pointer tracker;
velocity becomes the sheen position, the tilt, a jelly scale about the grab
point, four independent corner radii, the lag of the brushed grain behind the
frame, and the traffic lights' slosh — which is driven by shear, the velocity
the liquid has not caught up with, so the surface stays banked through the
middle of a drag and not only at its ends; zooming flies from the old rect (FLIP); closing
scales to 0.96 and fades; shading crops the frame over 360 ms; menus fade and
slide in. The compositor applies these as node positions, buffer scale and crop
(`wlr_scene_buffer_set_dest_size`, `set_source_box`) and opacity, and paces
frames to the output's refresh rate because a virtual GPU flips at once. Clients
keep their pixels: a flight scales the frame and clips the client; the jelly
leaves them alone. `PARITY.md` in MaryUI names these and the other deviations.

Wayland clients: every xdg-shell toplevel gets a frame with server-side
decorations (a client asking for its own is told SERVER_SIDE); the WM's rect
sizes the client and the client's committed geometry sizes the frame, so a
terminal snaps to its cell grid. File › New Terminal spawns `foot`. Keyboard
focus follows the focused window; pointer focus follows the hit test, which
skips window shadows the way CSS does.

## Testing without a hand on the mouse

`lp-input` (installed with the desktop) creates a uinput pointer and keyboard
inside the guest and plays a script: `lp-input 1280x800 move X Y click X Y
dblclick rclick drag X1 Y1 X2 Y2 keydown alt keyup alt key super+w type "text"
sleep MS mark TEXT` — for
example `key ctrl+space type te key Return type "hello" key ctrl+s` opens
TextEdit from Spotlight and saves a document. It needs
`/dev/uinput`, so `sudo`. `mark` prints a line, which — with the serial console
logged on the Mac — lets a host-side loop take `screencapture -l <window id>`
shots of the VM window a few hundred milliseconds after each action; that is how
the mid-drag jelly, the zoom flight and the shade crop in this chapter's
verification were captured. The compositor logs `libseat … Could not close
device: Device not taken` when those virtual devices vanish; it is noise.

## Settings and the wallpaper cache

Spotlight's View pill writes `$XDG_CONFIG_HOME/maryui/settings.conf` (accent,
folders, liquid merge, wallpaper mode, molten tone, reduced motion, clock) —
MaryPi ships no defaults for it, so an unwritten file means the compiled
defaults, which mirror the web's `settings.ts`. `clock` is the one key the web
does not have: View › Show Clock writes `clock=off` and the time leaves the
corner; it is on by default, and a file without the key keeps it on. The wallpaper is looked up in three places before it is rendered:
`$MARYUI_DATA_DIR` (dev mode points this at `out/ui/usr/share/maryui` over
virtiofs), then `/usr/share/maryui/`, then `$XDG_CACHE_HOME/maryui/`; a render
that had to happen is written to the last of those. That is why the `ui` stage
bakes `wallpaper-1280x800.png` and `molten-platinum-1280x800.png` into the tree:
at the VM's scanout size both are already there, and nothing renders at boot.

## Limitations

- Virtualization.framework gives a fixed 1280×800 scanout and no 3D, so the VM
  runs the pixman renderer and draws its own cursor (no hardware cursor plane).
  Measured there with `MARYUI_DEBUG=frames`: a drag costs about 2 ms per frame
  (the title strip repaint is most of it), the worst frames — a full-size window
  repainted after a zoom — about 30 ms once, and an idle desktop handles no frames
  at all unless an ambient animation (the Gallery's indeterminate progress bar)
  is on screen, which repaints its own rectangle at 30 Hz. Those numbers predate
  the liquid pass — the merge filter and the corner repaints have not been
  measured against them yet. The Pi uses GLES2 on vc4/v3d (not yet booted).
- The molten wallpaper renders through a *surfaceless EGL* context, which is
  independent of the scene renderer: mesa serves it with llvmpipe in the VM and
  with v3d on a Pi. A 1280×800 frame takes about 7.5 seconds under llvmpipe, so
  the compositor refuses to animate it on a software renderer and shows the
  still bake the `ui` stage shipped. The animated flow — `molten.flow` shader
  seconds per second of window motion, then a full-resolution still after
  `molten.settle-ms` — is written but has never run, because no Pi has booted.
- No clipboard between host and guest, no Xwayland, one keyboard layout (`us`),
  no screen locking, no greeter: the desktop is the session.
- Clients that insist on client-side decorations get a frame around their frame.
- The jelly skew is computed but not applied (scene nodes translate and scale
  only); the moving sheen and the moving grain repaint the title bar, not body
  surfaces; the procedural wallpaper's slow drift and the menu's backdrop blur
  are not ported; View › Raster Wallpaper is remembered but not yet rendered.
- A client window that closes vanishes at once; built-in windows fade out.
- The Finder has no column view, no “Put Back” from the Trash, no icons on the
  wallpaper, and drags stay between built-in windows (no `wl_data_device`, so
  nothing drops into or out of a Wayland client).

## Troubleshooting

**No clock in the corner** — check `clock=` in `~/.config/maryui/settings.conf`
first; View › Show Clock brings it back.

**The window stays platinum with no windows, or black** — `journalctl -u
maryos-desktop -b`. `[libseat] … Permission denied` on `Activate` means
`polkitd` is missing; `no renderer` means the pixman fallback did not engage
(`WLR_RENDERER=pixman` is set by the launcher only for virtio-gpu).

**`loginctl list-sessions` shows no seat0 session** — the unit needs
`libpam-systemd` and the `maryos-desktop` PAM service; `PAMName=login` would try
`pam_lastlog` and ask for a password.

**`vm.sh` boots to the desktop** — the image's default target is
`graphical.target`; the desktop hook sets `multi-user.target` after enabling the
unit. Rebuild the image.

**Dev mode does not restart** — the launcher compares `stat -c %Y` of
`/mnt/maryos-out/ui/usr/bin/maryui-desktop` once a second; `make ui` renames
`out/ui.tmp` over `out/ui`, so the mtime changes only when the build succeeded.
`ls -l /mnt/maryos-out/ui/usr/bin` in the guest shows what it sees.

**Fonts look wrong** — `fc-list | grep -E 'Inter|P052|JetBrains'` in the guest;
the desktop hook runs `fc-cache -f`.

**Ctrl+Alt+Backspace** ends the session (the unit restarts it); `sudo systemctl
stop maryos-desktop` gives `tty1` back to a getty.

## Verified

On September 12, 2026, with MaryUI at 00264b1 in the `--dev` loop: a guest with
no `settings.conf` shows the ambient clock top right; `clock=off` in
`~/.config/maryui/settings.conf` and a restart of `maryos-desktop` bring the
desktop back with the corner empty; `clock=on` and another restart bring the
clock back with the current time. Toggling View › Show Clock live, which disables
the node without a restart, is covered by `test_desktop` and was not driven in the
VM. The notes below predate the menu bar's removal.

On September 8, 2026, in the Virtualization.framework test bed: the desktop boots
in about a second after `graphical.target`; the wallpaper, menu bar and clock;
Finder and Gallery open at their default rects; menus open, switch on hover and
run their commands; windows drag (with the jelly), resize from eight handles,
shade, zoom and restore (with the flight), close (⌘W, the red light, the menu);
the Gallery's five tabs; the Finder's search; `foot` as a decorated client with
keyboard focus, zoom and close; the dev loop restart; an idle desktop handles no
frames, motion frames are paced to the 60 Hz refresh and ambient animations to
30 Hz.

Later the same day, with `lp-input` driving the desktop: `Ctrl+Space` opens
Spotlight's dock over the Finder and the Gallery with the bar focused and the
running dots under both; typing `te` lists TextEdit and Terminal, `↓`/`↑` wrap
between them, `Enter` on Terminal spawns `foot` and on TextEdit opens
`• Untitled`; typing two lines updates the status bar (`4 words · 22 characters ·
Ln 2, Col 12 · Edited`); holding Backspace for a second deletes eight characters;
`Ctrl+S` writes `/home/mary/Documents/Untitled.txt` (14 bytes, read back over
`hvc0`) and drops the marker; a drag selects `Hello Mary` under the I-beam; `rao`
finds `Rao · Window · Finder` and `Enter` brings the Finder forward; a click on
the wallpaper and `Esc` both close the panel; with `foot` focused, `echo hi`
reaches the terminal, and `xyz` typed into Spotlight does not. The journal
stayed clean.

The file manager was verified the same evening, in a VM booted from a fresh
image and running the desktop from `out/ui`: the first Finder window opens on
`/home/mary` and lists Desktop, Documents and Downloads with the welcome note
inside Documents, all created by the image; a double-click (and `⌘O`) enters a
folder, `⌘[`, `⌘]` and `⌘↑` walk back, forward and up, the sidebar and the
crumbs navigate, and the window's title, the crumb trail and the selected
sidebar row follow the folder; both views draw, and the list's header sorts.
`⇧⌘N` makes “untitled folder”, selects it and opens the rename field with the
name already selected, so typing `Ideas` and Return renames it on disk. A
double-click on the welcome note opens it in TextEdit with its 102 words, and
`⌘I` opens the Info window (kind, 0 items, `~`, dates, `mary (mary)`,
`drwxr-xr-x`, an Open button). A right-click on a file opens the context menu
(Open, Get Info · Rename, Duplicate, Move to Trash · Cut, Copy) at the pointer.
A file created in a terminal appears in the open window with no input, and
disappears when it is removed; a folder made the same way appears as a folder.
Dragging the note onto a folder's icon moves it there (`ls` confirms it) and
both windows refresh. The journal stayed clean.

Two compositor bugs were found and fixed while verifying: a chrome that
repainted asked for no output frame, so a change made without moving the pointer
(a double-click, a menu command) sat in the scene until the next event; and a
repaint requested from inside a running pass — an app dispatching `SET_TITLE`,
say — re-entered that pass and left it in the DRAW state. Chromes now schedule a
frame after publishing, and a repaint asked for from inside a pass is left to
the pass that is running.

Scripted input in this environment proved unreliable in two ways worth knowing:
the first `lp-input` session after the compositor restarts is ignored (run a
throwaway one first), and events can arrive tens of seconds after the tool exits,
so a screenshot taken on a `mark` may still show the state before them.

### September 9, 2026 — the liquid pass

MaryUI's `linux/` tree caught up with the web's design pass (`liquid-platinum-linux-parity`,
`cb862f4`) and the submodule was moved onto it, so `make ui` builds from a clean clone again —
`==> maryui ready: … (cb862f43fce0)`, no `MARYUI_DIR` and no `-dirty`.

Verified in the C library on the Mac: 124 tests pass (`test_radius` is new; `test_slosh` carries
the two shear cases from `slosh.test.ts`), `make gen-check` and `make parity` are green, and
`npm test` still passes its 70 on the web side. The refactor that moved `feSpecularLighting` out
of the wallpaper and into `lp_specular_at` was checked by rendering the wallpaper before and
after: the PNGs are byte-identical.

Verified by rendering: the beads are 18px pale glass with a drop shadow and no platinum rim, the
waterline sits where `liquid.fill-traffic` puts it, and hovering the group merges the three
liquids into one lit ribbon while the beads themselves stay put — hovering the first light
bridges only its neighbour, which is the `goo.attract` reach. Finder tiles and Spotlight dock
tiles lost their raised plates.

Verified in the VM (`./ui.sh --dev`, dev mode over virtiofs): the desktop starts on the new
compositor, the wallpaper is the molten still and loads in 21 ms because the `ui` stage baked it
(rendering it there would have taken about 7.5 seconds under llvmpipe), the title bars are 42px
with the lights at their new pitch, and the Finder's folder icons are flat and accent-coloured.
The VM process sat at 0.0% CPU while the desktop was idle, which is what the "an idle desktop
schedules zero animation frames" rule requires of the new corner, grain and lag springs.

Not verified, and worth knowing before trusting them: the animated molten flow (it is gated off
on software renderers, so only a Pi can exercise it), the frame cost of the merge filter and the
corner repaints during a drag against the 2 ms figure above, and the drag-driven slosh and corner
deformation as seen on screen rather than in the unit tests.
