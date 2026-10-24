/*
Copyright 2026 Dmitry Mikushin.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/*! \brief xconsole: the subset of Xlib (and XKB) that xsecurelock uses,
 * implemented on the Linux text console.
 *
 * This is the platform seam of the ttysecurelock fork. xsecurelock's own
 * code (main.c, helpers/auth_x11.c, helpers/saver_multiplex.c, ...) is
 * compiled unchanged against the real X11 headers and linked against this
 * library instead of libX11:
 *
 * - The "display" is a free VT, taken by the main program at XOpenDisplay
 *   (which must run as root for that) and locked at XGrabKeyboard
 *   (VT_PROCESS + VT_LOCKSWITCH); then the main program continues as the
 *   user owning the console that was in front, like xsecurelock runs as the
 *   user of the X session.
 * - Windows are rectangles in a table shared by all processes of the lock
 *   (helpers get it through $DISPLAY, as X clients do); they are painted on
 *   the VT as character cells, in "pixels" of the console font's cell size.
 * - Key events come from the lock VT's keyboard in K_MEDIUMRAW mode and are
 *   translated with the console's own keymap; XKB group = the console's
 *   AltGr lock, XKB indicators = the console keyboard LEDs.
 */

#ifndef XCONSOLE_INTERNAL_H
#define XCONSOLE_INTERNAL_H

#include <X11/Xlib.h>
#include <pthread.h>
#include <stdint.h>

#define XC_MAGIC 0x58434f4e  // "XCON"
#define XC_MAX_WINDOWS 64
#define XC_MAX_ATOMS 128
#define XC_ATOM_NAME 64
#define XC_FIRST_ATOM 69  // After the predefined atoms (XA_LAST_PREDEFINED).
#define XC_ROOT_WINDOW 1

typedef struct {
  Window id;  // 0: free slot
  Window parent;
  int x, y, w, h;  // relative to the parent, in pixels
  int mapped;
  unsigned long background;  // palette index
  uint64_t stack;            // higher = above its siblings
  pid_t owner;               // process that created it
} XcWindow;

//! State shared by every process of the lock (memfd, inherited).
typedef struct {
  uint32_t magic;
  pthread_mutex_t lock;
  int cols, rows;         // screen in cells
  int cell_w, cell_h;     // font cell in pixels
  uint64_t next_xid;
  uint64_t next_stack;
  XcWindow windows[XC_MAX_WINDOWS];
  int natoms;
  char atoms[XC_MAX_ATOMS][XC_ATOM_NAME];
  unsigned char palette[16][3];

  // Keyboard state, owned by the main program; group may be set by helpers
  // (XkbLockGroup, auth_x11's Ctrl-Tab).
  int num_groups;
  int group;
  unsigned int leds;  // bit 0 Scroll, 1 Num, 2 Caps (console LED flags)
  unsigned int locks;    // the console's lock state (1 << KG_SHIFT, ...)
  unsigned int latches;  // the console's sticky (latched) shifts
  char group_names[2][XC_ATOM_NAME];

  // Bumped whenever cells are repainted with window backgrounds (a window
  // mapped, unmapped, moved, raised or destroyed): what a saver had there
  // is gone, and the lock repaints it (saver.c).
  uint64_t damage;
  // xsecurelock's "saver" and "auth" windows, for any process that maps the
  // auth window to blank the saver window at once (display.c). The saver
  // channel's checks use the main program's own copies, not these.
  Window saver_window, auth_window;
} XcShared;

//! Per-process connection.
typedef struct {
  int is_main;      // owns the VT and the keyboard
  int draw_fd;      // write-only fd of the lock VT
  int shm_fd;
  XcShared *shm;
  // Main program only:
  int vt_fd;        // read/write fd of the lock VT (keyboard, ioctls)
  int orig_vt;
  int tty0;
  // The lock VT. Kept here, not in the shared table: every process of the
  // lock can write that, and so can whoever can open the fds of a helper
  // (a saver like `sleep` is dumpable); the VT handling must not trust it.
  int vt;
  // The saver channel (saver.c): in the main program, its own end (saver
  // modules connect there); in helpers, the end they pass on to savers.
  int saver_fd;
} XcConn;

extern XcConn xc;

//! The user to lock for (--user), who must own the VT in front.
extern const char *xc_lock_user;

//! The translation of a key event, queued with it.
typedef struct {
  KeySym keysym;
  char text[16];
} XcKeyText;

// display.c
void XcLock(void);
void XcUnlock(void);
XcWindow *XcFindWindow(Window w);
void XcQueueEvent(const XEvent *ev);
void XcQueueKeyEvent(const XEvent *ev, const XcKeyText *key);
Atom XcInternAtom(const char *name);
//! The main program's saver window (the one xsecurelock names "saver").
extern Window xc_saver_window;
//! The main program's auth window (the prompt; xsecurelock's "auth").
extern Window xc_auth_window;
//! A saver's cell, as the lock keeps it (saver.c).
typedef struct {
  uint32_t cp;
  uint8_t fg, bg;
} XcGridCell;
int XcViewable(const XcWindow *w);
unsigned long XcWindowBackground(const XcWindow *w);
//! A window's cell rectangle on the screen, clipped to it; 0 if empty.
int XcWindowCellsLocked(const XcWindow *w, int *c0, int *r0, int *c1,
                        int *r1);
//! Paint cells of a saver grid (cols x rows, at the window's top left
//! cell) where the window is visible: the listed ones, or all if n < 0.
void XcPaintGridLocked(const XcWindow *w, int cols, int rows,
                       const XcGridCell *grid, const uint32_t *list, int n);
//! Whether a code point may be put in a cell as is (one cell, printable).
int XcPrintableCell(uint32_t cp);

// saver.c: saver modules draw into their saver window through the lock.
int XcSaverServerStart(void);  // main program, after dropping privileges
void XcSaverServerStop(void);

// keyboard.c
int XcKeyboardInit(int vt_fd, int orig_fd);
void XcKeyboardSetMode(int vt_fd);
void XcKeyboardRestore(int vt_fd);
//! Feed raw K_MEDIUMRAW bytes; queues KeyPress/KeyRelease events.
void XcKeyboardInput(Display *dpy, const unsigned char *buf, size_t len);
//! A key event leaves the queue: keep its translation for XLookupString.
void XcKeyDequeued(unsigned long serial, const XcKeyText *key);
//! X's autorepeat: queue the repeats that are due.
void XcKeyboardTick(Display *dpy);
//! Milliseconds until the next repeat, or -1.
int XcKeyboardTimeoutMs(void);

// vt.c
int XcVtOpen(void);
int XcVtBringToFront(void);
void XcVtRelease(int go_back);
int XcVtDropPrivileges(void);

#endif
