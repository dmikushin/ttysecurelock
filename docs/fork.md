# ttysecurelock: xsecurelock on the Linux text console

ttysecurelock is a fork of [xsecurelock](https://github.com/google/xsecurelock)
v1.9.0. It changes only what X forces: xsecurelock's own code (`main.c`,
`helpers/auth_x11.c`, `helpers/saver_multiplex.c`, `helpers/authproto_pam.c`,
`auth_child.c`, `saver_child.c`, `env_settings.c`, ...) is compiled
**unchanged**, against the real X11 headers, and linked against `libxconsole.a`
(`xconsole/`) instead of libX11. `libxconsole` implements the part of Xlib and
XKB that xsecurelock uses on top of a Linux VT. So the screens, texts, prompt
modes, keyboard layout and lock indicators, PAM conversation, timeouts and key
handling are xsecurelock's own code, not a copy of it.

Build: `./autogen.sh && ./configure --enable-console
--with-pam-service-name=system-auth && make && sudo make install`. This
installs `/usr/local/bin/ttysecurelock` and the helpers in
`/usr/local/libexec/ttysecurelock` (mode 0711).

Run (as root; normally the hotkey daemon tty-i3-keys does it on Win+Escape):
`ttysecurelock --user NAME [xsecurelock arguments]`. NAME must own the VT in
front; the lock refuses otherwise. The settings are xsecurelock's
`XSECURELOCK_*` environment variables, with the same defaults.

## The seam

`xconsole/entry.c` is the program's `main`: it takes `--user NAME` and calls
upstream `main` (compiled with `-Dmain=xsecurelock_main`, a build flag, not an
edit). Everything else happens behind the Xlib calls:

| Function | On the console |
|---|---|
| `XOpenDisplay` | In the main program (no `$DISPLAY` of ours): checks it runs as root and that NAME owns the VT in front, takes a free VT from the top (63 down to 13; `VT_OPENQRY` would land in logind's autovt range and get a getty), puts it in `VT_PROCESS` mode (every release request is refused), raw termios, reads the console keymap and the keyboard state of the VT in front (below), creates the shared window table (memfd), sets `$DISPLAY=:xconsole:<vt fd>:<memfd>` for the helpers, then drops to NAME's uid keeping only `CAP_SYS_TTY_CONFIG`, non-dumpable. In a helper: maps the table from the fds in `$DISPLAY` and marks those fds close-on-exec, so the helper's own children (authproto_pam, the saver's `sleep`) do not get them. |
| `XCloseDisplay` | Main program, reached only after a successful authentication: keyboard mode back, `VT_UNLOCKSWITCH`, `VT_AUTO`, back to the original VT, hands the console mouse selection to it (the kernel will not free a VT that owns it), `VT_DISALLOCATE` the lock VT. If the program dies instead, nothing of this happens: switching stays disabled (fail closed). |
| `XGrabKeyboard` | Brings the lock VT to the front (repeated until it really is), keyboard in `K_MEDIUMRAW`, `VT_LOCKSWITCH`. |
| `XGrabPointer`, `XGrabServer`, `XUngrabServer`, `XSetInputFocus`, `XGetInputFocus`, cursors, pixmaps, `XUndefineCursor` | No pointer, no other clients: succeed, do nothing. `XQueryPointer` reports the top left corner. |
| `XCreateWindow`, `XDestroyWindow`, `XMapWindow`, `XMapRaised`, `XUnmapWindow`, `XRaiseWindow`, `XRestackWindows`, `XConfigureWindow`, `XMoveResizeWindow`, `XChangeWindowAttributes`, `XGetWindowAttributes`, `XQueryTree`, `XClearWindow` | A window table shared by all processes of the lock; windows stack and clip as in X and are painted as console cells. Coordinates are pixels of a screen of console-font cells, so xsecurelock's pixel arithmetic keeps its meaning. Windows of processes that are gone are destroyed, as X destroys a disconnected client's windows. |
| `XSelectInput`, `XPending`, `XNextEvent`, `XFilterEvent`, `XFlush` | Events of the main program: key events read from the lock VT, Map/Unmap/Visibility/ConfigureNotify of its own windows, root ConfigureNotify when the console is resized. |
| `XLookupString`, `XmbLookupString`, `XKeysymToString` | The characters the console itself would type: the keycode is translated with the kernel keymap (read with `KDGKBENT` through a VT in `K_UNICODE` mode), following the kernel's `kbd_keycode()`/`k_*` handlers (shift/AltGr/Ctrl/Alt maps, shift and AltGr locks, Caps Lock on letters, dead keys and the accent table, keypad with Num Lock). Keys X maps to fixed keysyms whatever the layout (Return, BackSpace, Tab, Escape, Delete, cursor, function, modifier and media keys) get those keysyms and texts. |
| `XkbQueryExtension`, `XkbGetMap`, `XkbGetControls`, `XkbGetNames`, `XkbGetState`, `XkbGetIndicatorState`, `XkbLockGroup`, `XkbFreeKeyboard` | The XKB view of the console keyboard. Groups: 2 if the keymap has an AltGr lock (ruwin_cplk: CapsLock), named as X names the layouts from the letters the tables type ("English (US)", "Russian", ...). Group = the AltGr lock. Indicators: the 14 of an X server with the evdev rules, in its order ("Caps Lock", "Num Lock", "Scroll Lock", ..., "Shift Lock", "Group 2", "Mouse Keys"); lit from the console LED flags, the shift lock and the group. `XkbLockGroup` (auth_x11's Ctrl-Tab) sets the group. At lock time all of it is taken from the VT in front: its LED flags with `KDGKBLED`, its AltGr lock through the kernel's `kbd-altgrlock` LED trigger on a key-less uinput LED (there is no ioctl for a VT's lock state). |
| `XAllocNamedColor`, `XQueryColor`, `XFreeColors`, `XCreateGC`, `XFreeGC` | Colours are the console's 16 (palette read with `GIO_CMAP`); a named colour (`#rrggbb`, `#rgb`, X names, `/usr/share/X11/rgb.txt`) gets the nearest one. |
| `XLoadQueryFont`, `XTextWidth`, `XDrawString` | The console font as a core font one cell wide; its height is a cell minus auth_x11's line spacing, so its lines are one row apart. Text is UTF-8, written to the cells under it where no window above covers them. |
| `XInternAtom`, `XGetAtomName`, `XChangeProperty`, `XSetWMProperties`, `XStringListToTextProperty`, `XAllocClassHint`, `XGetClassHint`, `XmuClientWindow`, `XFree` | Atoms in the shared table; window manager properties are accepted and ignored (there is no window manager). |
| `XForceScreenSaver` | Blanks/unblanks the console (`TIOCL_BLANKSCREEN`). |
| `XBell`, `XGetKeyboardControl`, `XChangeKeyboardControl` | `KDMKTONE` with the bell pitch and duration (XSECURELOCK_AUTH_SOUNDS). |
| `XSetErrorHandler`, `XGetErrorText`, `XOpenIM`, `XCreateIC` | Trivial. |

## Build glue

`configure.ac`: `--enable-console`; with it libX11/Xmu are not required, XKB
is "available" (xconsole provides it), every other X extension (XComposite,
XRandR, XScreenSaver, XSync, XF86Misc, DPMS, Xft, fontconfig) and the savers
that need X (mplayer, mpv, xscreensaver) are off. `Makefile.am`: builds
`libxconsole.a` and links the programs against it, renames upstream `main`,
installs the program as `ttysecurelock` and the helpers in
`$(libexecdir)/ttysecurelock`, mode 0711; `dimmer` and the X debugging tools
are not built.

`scripts/fork-check.sh` lists every file changed since upstream (commit
2cf9729, an unchanged v1.9.0) and fails if one outside `xconsole/`,
`configure.ac`, `Makefile.am`, `docs/`, `scripts/`, `test/console/`,
`.gitignore` changed. Result: see the end of this file.

## What X forces, and what could not be kept

- **No other X clients.** XComposite (obscurer window), XRandR (one auth
  window per monitor: the console is one screen), XScreenSaver/XSync idle
  detection (`until_nonidle`, `XSECURELOCK_IDLE_TIMERS`), DPMS
  (`XSECURELOCK_BLANK_DPMS_STATE`: the console only blanks), dimmer
  (`XSECURELOCK_DIM_*`), and the X savers (mpv, mplayer, xscreensaver) do not
  exist. `saver_blank` and `saver_multiplex` work.
- **Fonts.** No Xft/fontconfig: `XSECURELOCK_FONT` is ignored, the console
  font is used, one cell per character (wide glyphs, emoji and kaomoji the
  font lacks show as the console's replacement glyph; the screen holds the
  right code points). Colours are reduced to the console's 16.
- **No pointer**: mouse motion and buttons do not wake the prompt; with
  `XSECURELOCK_SINGLE_AUTH_WINDOW` the window is on the only screen anyway.
- **Keysyms.** Characters outside Latin-1 get Unicode keysyms
  (`0x1000000 + code point`), not X's legacy ones (e.g. `Cyrillic_a`):
  this matters only for `XSECURELOCK_KEY_<name>_COMMAND` with such names.
  Ctrl-Tab, Ctrl-Backspace, Ctrl-Alt-O/Win-O and the fixed keys are the same.
- **The layout after unlock.** Under X the layout chosen at the prompt stays
  after unlocking (XKB state is global). The console's lock state is per VT
  and the kernel offers no way to set another VT's lock state, so the session
  VT keeps the layout it had when it was locked.
- **Killing the lock.** Under X, killing xsecurelock unlocks the screen. On
  the console the lock VT stays in front with switching disabled (fail
  closed); root has to release it (`VT_UNLOCKSWITCH`, e.g. from ssh).
- **Who runs it.** xsecurelock runs as the session user. ttysecurelock must
  start as root to take and lock a VT, then runs as the user (with
  `CAP_SYS_TTY_CONFIG` only, non-dumpable).

Upstream behaviour kept as it is, including bugs: the first key that wakes the
prompt is not typed; keys typed while the prompt is closing after Escape are
lost; the `disco` prompt computes its length as `stride * DANCERS *
strlen(dancer) + strlen(combiner)` (auth_x11.c) — 1505 bytes for a 258-byte
buffer — and so writes a NUL past the buffer and draws stack garbage after the
dancers, under X as well.
