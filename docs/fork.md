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
`XSECURELOCK_*` environment variables, read by xsecurelock's own code with
its defaults; see "Configuration" below for what each one does here, and
"Settings file" for how they reach a lock started by a daemon.

## Settings file

tty-i3-keys starts the lock from a system service, whose environment is not
the user's. So `ttysecurelock --user NAME` (`xconsole/entry.c`) also reads
`~NAME/.config/ttysecurelock/environment`: lines `XSECURELOCK_NAME=value` in
the syntax of environment.d(5) (`#` comments, blank lines, a value may be in
single or double quotes; no `$` expansion). Only `XSECURELOCK_*` names are
taken (others are logged and ignored), except those that decide who checks
the password or what runs with the lock VT: `XSECURELOCK_AUTH`, `_AUTHPROTO`,
`_PAM_SERVICE`, `_GLOBAL_SAVER`, `_SWITCH_USER_COMMAND`, `_KEY_*_COMMAND`
and `_DEBUG_*` are ignored in the file (every
process of the user can write it, and on the console such a process cannot
otherwise undo a lock: killing it fails closed); they still work from the
environment of whoever starts the lock. A variable already in the
environment wins. The file is read by a child process that has dropped to NAME's uid,
gids and groups before opening it, so root never opens a user's file; the
parent only parses the bytes (at most 64 KiB). A file of its own, not
`~/.config/environment.d`: those variables also reach the user's X session,
where an X xsecurelock would look for the console's savers (corentin has
xsecurelock installed). Example:

    XSECURELOCK_SAVER=saver_matrix


## Configuration

Honoured as under X (the code reading them is upstream's):
`XSECURELOCK_AUTH`, `_AUTHPROTO`, `_AUTH_CURSOR_BLINK`, `_AUTH_TIMEOUT`,
`_BLANK_TIMEOUT` (blanks the console), `_DATETIME_FORMAT`,
`_DEBUG_ALLOW_LOCKING_IF_INEFFECTIVE`, `_DISCARD_FIRST_KEYPRESS` (by default
the key that wakes the prompt is not typed, as under X), `_FORCE_GRAB` (the
grab cannot fail on the console), `_GLOBAL_SAVER`, `_KEY_<keysym>_COMMAND`
(X's keysym names, see "Keys"), `_NO_PAM_RHOST`, `_PAM_SERVICE`,
`_PASSWORD_PROMPT` (all nine modes), `_SAVER_DELAY_MS`,
`_SAVER_RESET_ON_AUTH_CLOSE`, `_SAVER_STOP_ON_BLANK`, `_SHOW_DATETIME`,
`_SHOW_HOSTNAME`, `_SHOW_KEYBOARD_LAYOUT`, `_SHOW_LOCKS_AND_LATCHES`,
`_SHOW_USERNAME`, `_SINGLE_AUTH_WINDOW`, `_SWITCH_USER_COMMAND`,
`_WAIT_TIME_MS`.

Honoured, reduced by the console: `_BACKGROUND_COLOR`,
`_AUTH_BACKGROUND_COLOR`, `_AUTH_FOREGROUND_COLOR`, `_AUTH_WARNING_COLOR`
(parsed as XAllocNamedColor does, then the nearest of the console's 16
colours; see "Colours"), `_AUTH_SOUNDS` (the PC speaker tone),
`_BURNIN_MITIGATION` and `_BURNIN_MITIGATION_DYNAMIC` (offsets in pixels move
the prompt by whole cells), `_SAVER` (`saver_blank`, and the console's own
`saver_matrix`; X savers cannot run, see "Savers"), `_DEBUG_WINDOW_INFO`
(calls xwininfo/xprop, which find no X).

Added by the console: `XSECURELOCK_MATRIX_OPTIONS`, cmatrix's options for
`saver_matrix` (default `-ab -u 5`).

No effect, their X feature does not exist on the console (compiled out):
`_BLANK_DPMS_STATE` (no DPMS), `_COMPOSITE_OBSCURER`, `_NO_COMPOSITE`
(XComposite), `_NO_XRANDR`, `_NO_XRANDR15` (XRandR), `_DIM_ALPHA`,
`_DIM_COLOR`, `_DIM_FPS`, `_DIM_MAX_FILL_SIZE`,
`_DIM_OVERRIDE_COMPOSITOR_DETECTION`, `_DIM_TIME_MS` (dimmer), `_IDLE_TIMERS`
(until_nonidle: XScreenSaver/XSync idle timers), `_IMAGE_DURATION_SECONDS`,
`_LIST_VIDEOS_COMMAND`, `_VIDEOS_FLAGS` (saver_mpv), `_XSCREENSAVER_PATH`
(saver_xscreensaver), `_FONT` (Xft/core fonts: the console font is used).

## The seam

`xconsole/entry.c` is the program's `main`: it takes `--user NAME` and calls
upstream `main` (compiled with `-Dmain=xsecurelock_main`, a build flag, not an
edit). Everything else happens behind the Xlib calls:

| Function | On the console |
|---|---|
| `XOpenDisplay` | In the main program (no `$DISPLAY` of ours): checks it runs as root and that NAME owns the VT in front, takes a free VT from the top (63 down to 13; `VT_OPENQRY` would land in logind's autovt range and get a getty), puts it in `VT_PROCESS` mode (every release request is refused), raw termios, reads the console keymap and the keyboard state of the VT in front (below), creates the shared window table (memfd), sets `$DISPLAY=:xconsole:<vt fd>:<memfd>:<saver channel fd>` for the helpers, then drops to NAME's uid keeping only `CAP_SYS_TTY_CONFIG`, non-dumpable, and starts the saver channel's thread (see "Savers"). In a helper: maps the table from the fds in `$DISPLAY` and marks the VT and table fds close-on-exec, so the helper's own children (authproto_pam, the saver modules) do not get them; they keep only the saver channel. |
| `XCloseDisplay` | Main program, reached only after a successful authentication: keyboard mode back, `VT_UNLOCKSWITCH`, `VT_AUTO`, back to the original VT, hands the console mouse selection to it (the kernel will not free a VT that owns it), `VT_DISALLOCATE` the lock VT. If the program dies instead, nothing of this happens: switching stays disabled (fail closed). |
| `XGrabKeyboard` | Brings the lock VT to the front (repeated until it really is), keyboard in `K_MEDIUMRAW`, `VT_LOCKSWITCH`. |
| `XGrabPointer`, `XGrabServer`, `XUngrabServer`, `XSetInputFocus`, `XGetInputFocus`, cursors, pixmaps, `XUndefineCursor` | No pointer, no other clients: succeed, do nothing. `XQueryPointer` reports the top left corner. |
| `XMoveResizeWindow` of a window with children | A child that covered it exactly (at 0,0, same size) is resized with it. This stands in for XRandR: under X a screen change makes saver_multiplex recreate its per-monitor window (and saver) at the new size; here the screen is one monitor and its window follows the saver window, which main.c resizes with the screen (a rotated console). |
| `XSetWMProperties` | Ignored, except that the main program notes its windows named "saver" and "auth" (wm_properties.c names them) for the saver channel. |
| `XCreateWindow`, `XDestroyWindow`, `XMapWindow`, `XMapRaised`, `XUnmapWindow`, `XRaiseWindow`, `XRestackWindows`, `XConfigureWindow`, `XMoveResizeWindow`, `XChangeWindowAttributes`, `XGetWindowAttributes`, `XQueryTree`, `XClearWindow` | A window table shared by all processes of the lock; windows stack and clip as in X and are painted as console cells. Coordinates are pixels of a screen of console-font cells, so xsecurelock's pixel arithmetic keeps its meaning. Windows of processes that are gone are destroyed, as X destroys a disconnected client's windows. |
| `XSelectInput`, `XPending`, `XNextEvent`, `XFilterEvent`, `XFlush` | Events of the main program: key events read from the lock VT (all that is there, into a queue that grows: nothing is dropped), X's autorepeat, Map/Unmap/Visibility/ConfigureNotify of its own windows, root ConfigureNotify when the console is resized. See "Events". |
| `XLookupString`, `XmbLookupString`, `XKeysymToString` | See "Keys". |
| `XkbQueryExtension`, `XkbGetMap`, `XkbGetControls`, `XkbGetNames`, `XkbGetState`, `XkbGetIndicatorState`, `XkbLockGroup`, `XkbFreeKeyboard` | The XKB view of the console keyboard. Groups: 2 if the keymap has an AltGr lock (ruwin_cplk: CapsLock). Their names are xkeyboard-config's (rules/evdev.xml) for the system's XKB layouts: XkbLayout/XkbVariant in /etc/X11/xorg.conf.d, else the X layout systemd's kbd-model-map gives the KEYMAP of /etc/vconsole.conf, taken in order when there are as many as groups. Without such a configuration (corentin, yoga3, 3a5000: the layouts come from the user's setxkbmap, which the console cannot see) they are guessed from the letters each table types ("English (US)", "Russian", "Ukrainian", "Belarusian"; any other Latin layout shows as "English (US)") and the lock logs that it guessed. Group = the AltGr lock. Indicators: the 14 of an X server with the evdev rules, in its order ("Caps Lock", "Num Lock", "Scroll Lock", ..., "Shift Lock", "Group 2", "Mouse Keys"); lit from the console LED flags, the shift lock and the group. `XkbGetState`: locked modifiers from the LEDs (Lock, Mod2) and the console's locks (Shift, Control, Mod1, Mod5 for its Shift/Ctrl/Alt/AltGr locks; the AltGr lock is the group here), latched modifiers from its sticky shifts (`KT_SLOCK`), as XSECURELOCK_SHOW_LOCKS_AND_LATCHES shows them. `XkbLockGroup` (auth_x11's Ctrl-Tab) sets the group. At lock time all of it is taken from the VT in front: its LED flags with `KDGKBLED`, its AltGr lock through the kernel's `kbd-altgrlock` LED trigger on a key-less uinput LED (there is no ioctl for a VT's lock state). The kernel publishes there the lock state of the console that received the last key (`kbd` in drivers/tty/vt/keyboard.c); for a lock started by a hotkey pressed on the user's VT, that is the VT in front. |
| `XAllocNamedColor`, `XQueryColor`, `XFreeColors`, `XCreateGC`, `XFreeGC` | See "Colours". |
| `XLoadQueryFont`, `XTextWidth`, `XDrawString` | The console font as a core font one cell wide; its height is a cell minus auth_x11's line spacing, so its lines are one row apart. Text is UTF-8, written to the cells under it where no window above covers them; a character takes the cells the console gives it (2 for double-width ones, 0 for combining marks). |
| `XInternAtom` (with `only_if_exists`), `XGetAtomName`, `XChangeProperty`, `XSetWMProperties`, `XStringListToTextProperty`, `XAllocClassHint`, `XGetClassHint`, `XmuClientWindow`, `XFree` | Atoms in the shared table; window manager properties are accepted and ignored (there is no window manager). |
| `XForceScreenSaver` | Blanks/unblanks the console (`TIOCLINUX` `TIOCL_BLANKSCREEN`/`TIOCL_UNBLANKSCREEN`; the kernel allows them on the caller's controlling tty only, so the lock VT is made the main program's controlling tty: it leads a session, as tty-i3-keys starts it, or calls setsid()). Key input unblanks it, as an X server does on input: main.c only notes that the screen is no longer blanked (ScreenNoLongerBlanked), and a console blanked this way ignores the keyboard until unblanked. The lock also unblanks at start, in case a lock died blanked. |
| `XBell`, `XGetKeyboardControl`, `XChangeKeyboardControl` | `KDMKTONE` with the bell pitch and duration (XSECURELOCK_AUTH_SOUNDS). |
| `XSetErrorHandler`, `XGetErrorText`, `XOpenIM`, `XCreateIC` | Trivial. |

## Savers

A saver module never gets the lock VT or the window table (its parent,
saver_multiplex, closes them on exec). It draws through the **saver
channel** (`xconsole/saver_proto.h`, `xconsole/saver.c`): a datagram socket
whose other end belongs to a thread of the main program. The saver connects
by sending its window id ($XSCREENSAVER_WINDOW) with one end of a new
SOCK_SEQPACKET pair attached; the kernel attaches its pid and uid
(SO_PASSCRED). The lock accepts it if the uid is the lock's, the window lies
inside xsecurelock's "saver" window and was created by the saver's parent
(saver_multiplex, or xsecurelock for an `XSECURELOCK_GLOBAL_SAVER`); at most
8 savers. It answers with the window's size in cells, and again whenever
that changes. The saver sends cells: (column, row, code point, foreground,
background), at most 5460 per message. The lock keeps them in a grid per
saver and writes them itself, only printable one-cell characters (anything
else becomes a space) in the 16 console colours, only where the window is
visible (the auth window and everything above clip them, as under X); it
repaints the grid when the screen under the window was repainted (a window
unmapped or moved). The saver cannot read the keyboard, change the VT's
modes, termios or font, or write control sequences (cursor reports, palette,
LEDs) to the console, and it cannot draw outside the saver window. Killing
it ends its connection and nothing else: upstream's saver_multiplex starts
it again.

The console's rule, not X's: while the auth window is mapped, every saver
window is blank (the process that maps it repaints the saver window at once,
and the channel paints nothing while it is mapped) (under X the saver goes on around the prompt box). The saver
keeps running behind it; the lock keeps its cells and shows them again when
the prompt is gone (after `XSECURELOCK_AUTH_TIMEOUT`).

`saver_matrix` (`xconsole/saver_matrix.c`) is the "Matrix" rain: it runs
[cmatrix](https://github.com/abishekvashok/cmatrix) on a pseudo-terminal of
its own (stdin, stdout, stderr; TERM=linux; size = the window's cells) and
keeps that terminal's screen with libvterm (MIT); the changed cells go
through the channel, coloured by the lock's green ramp (below), not by
cmatrix. cmatrix is GPL-3 and runs as a separate program found in
`$PATH` (packaged as `cmatrix` on Arch, CachyOS and Arch LoongArch); nothing
of it is copied or linked into this Apache-2.0 tree.

Colours: the console palette may not tell greens apart (the Nord palette of
corentin, yoga3 and 3a5000 has green = bright green), so a saver may send
six more colours, the **green ramp** (`XC_SAVER_RAMP` + 0-5): `#d0ffd0`
(the head), `#30f030`, `#10c010`, `#089008`, `#046004`, `#023802`. While a
saver shows, the lock sets them as palette entries 1-6 of the lock VT with
the console's escape `ESC ] P n rrggbb`, which changes that VT's palette
only (not the other consoles', as PIO_CMAP would); while the ramp is off a
ramp colour is drawn as the nearest console colour. The entries go back to
the console's palette (read with GIO_CMAP at lock start) when the auth
window is mapped (by the process mapping it, before the prompt is painted),
when the channel thread sees the prompt, and on unlock. A killed lock leaves
the ramp only on its own VT, which stays in front (fail closed) and is not
the session's. saver_matrix colours each trail (a vertical run of cmatrix's
characters) by age: its newest cell (the head) gets level 0, the cells above
it levels 1 to 5 by their distance from the head over the trail's length,
so the oldest cells at the top are the darkest; a cell whose character
changes in place keeps its age.

Characters: as in the film, mirrored half-width katakana, digits and kanji.
cmatrix draws ASCII (its `-c` draws CJK punctuation U+3000-303F, not
katakana); saver_matrix keeps its digits and shows its other characters as
katakana (7 in 10) or kanji, chosen per character and cell. The glyphs are
in `xconsole/rain/rain-font.otf` (installed in `$(datadir)/ttysecurelock`):
a 53 KB subset, made by `scripts/gen-rain-font.py`, of Noto Sans CJK JP
Medium (SIL Open Font License 1.1, `xconsole/rain/OFL.txt`; Reserved Font
Name "Source", which the subset does not use): the 56 half-width katakana
U+FF66-FF9D and the 240 kanji of grades 1-2 of the kyouiku kanji (MEXT's
gakunenbetsu kanji haitou hyou, as listed on English Wikipedia,
`xconsole/rain/kanji.txt`; it includes 日, the film's one kanji).
saver_matrix renders them with FreeType at the console font's cell size
(the lock sends it), mirrored, katakana at their size, kanji squeezed to the
cell's width (3/4 of its height), and sends them (`XC_SAVER_FONT`). The lock
builds the rain font from the lock VT's own font (read with KDFONTOP and
GIO_UNIMAP at lock start): every glyph of it, the saver's in slots no ASCII
character uses and in new slots up to 512, and its Unicode map plus the
saver's characters. The console writes a wide character (kanji) into two
cells, so the lock writes each kanji as a private-use alias U+E000 + k that
the rain font maps to the same glyph (/dev/vcsu then reads the alias). It
loads the rain font (KDFONTOP and PIO_UNIMAP, on the lock VT only) while the
saver shows, and puts the console font and map back: when the prompt is
about to show (the process mapping the auth window asks the lock's thread
through the channel and waits until it is done, as only the main program
may load fonts), when the thread sees the prompt, and on unlock. A killed
lock leaves the rain font only on its own VT. Without the font file, or if
the lock refuses the glyphs, the rain stays ASCII. It never sees the lock VT: its terminal is the pty, where nothing is
typed. If the pty closes (saver_matrix killed), cmatrix gets SIGHUP; it also
has SIGTERM as parent-death signal. When the window's size changes (a rotated
console), saver_matrix starts a new cmatrix on a new pty of the new size:
cmatrix 2.0 as packaged resizes its screen on SIGWINCH but its rain stays in
the old number of rows. Measured on 3a5000 (Loongson 3A5000,
lock VT 53, `-ab -u 5`), 20 s: cmatrix 2.1%, saver_matrix 0.8%, the lock
0.8% of one core (`test/console/saver.py`).

## Build glue

`configure.ac`: `--enable-console`; with it libX11/Xmu are not required, libvterm
is (for `saver_matrix`), XKB
is "available" (xconsole provides it), every other X extension (XComposite,
XRandR, XScreenSaver, XSync, XF86Misc, DPMS, Xft, fontconfig) and the savers
that need X (mplayer, mpv, xscreensaver) are off. `Makefile.am`: builds `libxconsole.a` and links the programs against it,
renames upstream `main`, installs the program as `ttysecurelock` and the
helpers in `$(libexecdir)/ttysecurelock` (configure's `helpersdir`; upstream's
two uses of `$(pkglibexecdir)` now say `$(helpersdir)`, which is
`$(pkglibexecdir)` without `--enable-console`), mode 0711; `dimmer` and the X
debugging tools are not built. The `version.c` rule writes an empty version
when there is neither git nor a version.c (a tarball of the tree), instead of
failing. A clean `git clone` and a `git archive` tree both build with
`./autogen.sh && ./configure --enable-console ... && make`, without automake
warnings.

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
  exist. `saver_blank`, `saver_multiplex` and `saver_matrix` work.
- **Fonts.** No Xft/fontconfig: `XSECURELOCK_FONT` is ignored and the
  console font is used. Characters take the cells the console gives them: 2
  for double-width ones (emoji), 0 for combining marks (drawn into the cell
  before); glyphs the font lacks show as its replacement glyph, the screen
  holds the right code points. Colours are reduced to the console's 16.
- **No pointer**: mouse motion and buttons do not wake the prompt; with
  `XSECURELOCK_SINGLE_AUTH_WINDOW` the window is on the only screen anyway.
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

Kernel facilities used and their limits: Magic SysRq is the kernel's (16 =
sync only on corentin, yoga3, 3a5000), as under X; Ctrl+Alt+Del is handled
by the kernel/systemd before the keyboard reaches the lock, as under X.

Upstream behaviour kept as it is, including bugs: the first key that wakes the
prompt is not typed; keys typed while the prompt is closing after Escape are
lost; the `disco` prompt computes its length as `stride * DANCERS *
strlen(dancer) + strlen(combiner)` (auth_x11.c) — 1505 bytes for a 258-byte
buffer — and so writes a NUL past the buffer and draws stack garbage after the
dancers, under X as well.

## Keys

- **Layout keys** type what the console itself would type: the keycode is
  translated with the kernel keymap (read with `KDGKBENT` through a VT in
  `K_UNICODE` mode), following the kernel's `kbd_keycode()`/`k_*` handlers
  (shift/AltGr/Ctrl/Alt maps, shift and AltGr locks, Caps Lock on letters,
  dead keys and the accent table). Their keysym is the one X has for that
  character (keysymdef.h: `Cyrillic_a` for а, Latin-1 as is, else
  `0x1000000 + code point`), without Control and Alt as in X.
- **Fixed keys** (Escape, BackSpace, Tab, Return, Delete, cursor, function,
  keypad, modifier, media and all XF86 keys): the keysyms, key types (levels:
  Shift, Num Lock for the keypad, Ctrl+Alt on F-keys giving XF86Switch_VT_n,
  Alt on Print, Ctrl on Pause, ...) and texts are X's, generated by
  `scripts/gen-xconsole-tables.py` from the keymap Xorg compiles for the
  user's setxkbmap (`test/console/user-keymap.xkb`, dumped with xkbcomp),
  the text by libX11's `_XTranslateKeySym`.
- **Keysym names** (`XKeysymToString`, used for
  `XSECURELOCK_KEY_<name>_COMMAND`): keysymdef.h and XF86keysym.h, first
  name of a value as libX11; `U%04lX`/`U%06lX` for other Unicode keysyms.
- **Keycodes** up to 247 (X keycode 255); above that X has none either.
- **Modifier state** of key events: X's modifier map (Shift, Control, Mod1
  for Alt, Mod4 for Super, ...) for the keys held, Lock and Mod2 from the
  LEDs, the console's locks and latches, the group in bits 13-14.
- **Autorepeat** is X's: the kernel's repeats are dropped, and a held key
  that repeats under X (per the keymap's interpretations: not modifiers or
  lock keys) repeats after 660 ms every 40 ms (xserver
  XkbDfltRepeatDelay/Interval), as a KeyRelease and KeyPress pair each time,
  which is what a client gets without detectable autorepeat (xsecurelock does
  not ask for it). The user's console repeat rate (kbdrate) is not used.

## Colours

`XAllocNamedColor` resolves a spec as libX11 and the X server do: Xcms first
(lowercased; `#rgb`, `#rrggbb`, `#rrrgggbbb`, `#rrrrggggbbbb` and
`rgb:r/g/b` with 1 to 4 hex digits as libX11's XcmsLRGB_RGB_ParseString,
`rgbi:r/g/b` in [0, 1]), then the server's colour names (xserver
os/oscolor.c, its table copied; case-insensitive). The pixel is the nearest of
the console's 16 colours (palette read with `GIO_CMAP`). Not supported: the
device-independent Xcms spaces (CIEXYZ:, CIEuvY:, CIExyY:, CIELab:, CIELuv:,
TekHVC:), which need Xcms's colour characterisation; `rgbi:` is scaled
linearly where Xcms goes through the screen's intensity tables (equal at 0 and
1). A spec that does not resolve gives 0 as under X, and the colours are set
to black: auth_x11 does not check the result and would otherwise use
uninitialised values.

## Events

Map/Unmap/ConfigureNotify of the main program's windows are queued when the
change is made, with both `event` and `window` set to the window (main.c
reads `xmap.window` and `xconfigure.window`: the auth window's mapped state,
and the root's resize, after which it resizes its windows) (an X server sends them after processing the request; the
program reads them later either way). VisibilityNotify is always Unobscured
and there are no FocusIn/FocusOut events: on the lock VT nothing else can map
a window or take the keyboard. Consequences for main.c: its "someone
overlapped the window" and "NotifyUngrab: reacquire grabs" branches are never
taken; `XGetInputFocus` returns None, so the focus juggling in the grab retry
loop (taken only if bringing the lock VT to the front fails) does nothing, and
there is no focus to restore after unlocking.

## Logging

The lock logs to its stderr (the journal of tty-i3-keys, which starts it)
one line per transition, besides upstream's own lines (`AUTH_TIMEOUT hit.`,
`auth child failed with status 1.`, `saver child killed by signal 9.`):
`xconsole: lock VT ttyN is our controlling tty`, `auth window mapped/
unmapped (pid)`, `saver window mapped`, `key input while the prompt is not
shown`, `console blanked/unblanked (why)` or the TIOCLINUX error, `saver
pid P connected`, `saver connection closed: why`, `saver hidden: the prompt
shows` / `shown again`, `green ramp on/off`, `rain font loaded` / `console
font restored`, `prompt: rain font off after N ms`; saver_matrix logs each
cmatrix it starts (pid, size) and how one ended.

PAM: a prompt that times out ends its PAM conversation unanswered, which
pam_faillock records as a failure (as under X). With Arch's defaults
(deny=3 in 15 minutes, unlock_time 600) three prompt timeouts in 15
minutes lock the account for 10 minutes, and the right password is refused
meanwhile (`test/console/saver.py` shows the records with `faillock`).

## Implementation limits (not forced by X, kept small on purpose)

- At most 64 windows and 128 atoms in the shared table (xsecurelock uses a
  handful of each). Overflows are logged (windows) or refused (atoms).
- Two XKB groups at most: the base table and the AltGr-lock table, the model
  of keymaps like ruwin_cplk. Keymaps that switch layouts with other kernel
  locks (ShiftL_Lock, CtrlL_Lock, ...) show one layout.
- Without XRandR, upstream's own fallback (`helpers/monitors.c`,
  GetMonitorsGuess) splits a screen wider than about 2.7:1 into several
  "monitors", as it does under X without RandR.
- `--user NAME` is required: the program starts as root (to take a VT), so it
  must be told whom it locks for, and it refuses if the VT in front belongs to
  someone else. The lock VT is taken from 63 down to 13 only: VTs below are
  logind's autovt range, where switching starts a getty that hangs the lock VT
  up.
- A helper's children do not inherit the lock VT or the window table
  (close-on-exec), which keeps them away from dumpable processes such as
  `sleep` and the saver modules; a saver module draws only through the saver
  channel ("Savers"), so an X saver (mpv, xscreensaver hacks) cannot run.
  The channel fd itself is inherited by every helper's children, and a
  saver module is dumpable: the user's own processes can draw into the saver
  window through it (not elsewhere, and never while the prompt shows).
- `dimmer` and `until_nonidle` (pre-lock dimming and idle detection, both on
  X extensions) are not built.
