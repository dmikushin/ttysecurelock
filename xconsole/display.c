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

/*! \brief The display, windows, drawing, colors, fonts and events of
 * xconsole (see xconsole_internal.h).
 *
 * Coordinates are pixels of a screen of cols x rows console font cells, so
 * xsecurelock's pixel arithmetic (box sizes, borders, burn-in offsets,
 * centring) keeps its meaning; drawing lands on the cells under it. Windows
 * stack and clip as in X: a window's background is painted where it is
 * visible, and text drawn into a window shows only where no window above
 * covers it.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/tiocl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <locale.h>
#include <wchar.h>

#include "../logging.h"
#include "saver_proto.h"
#include "xconsole_internal.h"

#define DISPLAY_PREFIX ":xconsole:"

XcConn xc = {0, -1, -1, NULL, -1, 0, -1, -1, -1};

Window xc_saver_window = None;
Window xc_auth_window = None;

// The Display: Xlib's public macros (DefaultScreen, ConnectionNumber, ...)
// read the fields of _XPrivDisplay, so that is what it is.
static struct {
  __typeof__(*(_XPrivDisplay)0) d;
  Screen screen;
  Visual visual;
} the_display;

static Display *const dpy_ptr = (Display *)&the_display;

// Events (main program only): a ring that grows, so that nothing is
// dropped however much arrives before the program reads it.
typedef struct {
  XEvent ev;
  XcKeyText key;  // for key events: what XLookupString will give
} QueuedEvent;
static QueuedEvent *event_queue;
static int eq_cap, eq_head, eq_len;

// Input masks selected by this process.
static struct {
  Window w;
  long mask;
} selected[XC_MAX_WINDOWS];

static XID next_local_id = 0x40000000;  // pixmaps, cursors, fonts

void XcLock(void) {
  int r = pthread_mutex_lock(&xc.shm->lock);
  if (r == EOWNERDEAD) pthread_mutex_consistent(&xc.shm->lock);
}

void XcUnlock(void) { pthread_mutex_unlock(&xc.shm->lock); }

XcWindow *XcFindWindow(Window w) {
  if (w == None) return NULL;
  for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
    if (xc.shm->windows[i].id == w) return &xc.shm->windows[i];
  }
  return NULL;
}

void XcQueueEvent(const XEvent *ev) { XcQueueKeyEvent(ev, NULL); }

void XcQueueKeyEvent(const XEvent *ev, const XcKeyText *key) {
  if (!xc.is_main) return;
  if (eq_len == eq_cap) {
    int cap = eq_cap ? eq_cap * 2 : 256;
    QueuedEvent *q = malloc(sizeof(QueuedEvent) * (size_t)cap);
    if (q == NULL) {
      Log("xconsole: out of memory for events");
      return;
    }
    for (int i = 0; i < eq_len; ++i) q[i] = event_queue[(eq_head + i) % eq_cap];
    free(event_queue);
    event_queue = q;
    eq_cap = cap;
    eq_head = 0;
  }
  QueuedEvent *e = &event_queue[(eq_head + eq_len) % eq_cap];
  e->ev = *ev;
  if (key != NULL) {
    e->key = *key;
  } else {
    memset(&e->key, 0, sizeof(e->key));
  }
  ++eq_len;
}

static long SelectedMask(Window w) {
  for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
    if (selected[i].w == w) return selected[i].mask;
  }
  return 0;
}

static void QueueWindowEvent(int type, Window w) {
  if (!xc.is_main) return;
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.xany.display = dpy_ptr;
  ev.xany.window = w;
  if (type == MapNotify || type == UnmapNotify) {
    if (!(SelectedMask(w) & StructureNotifyMask)) return;
    // xany.window is the event window in these events; the window that was
    // (un)mapped or configured is a field of its own, which main.c reads.
    ev.xmap.event = w;
    ev.xmap.window = w;  // XUnmapEvent has it at the same place
  } else if (type == VisibilityNotify) {
    if (!(SelectedMask(w) & VisibilityChangeMask)) return;
    ev.xvisibility.state = VisibilityUnobscured;
  } else if (type == ConfigureNotify) {
    if (!(SelectedMask(w) & StructureNotifyMask)) return;
    XcWindow *win = XcFindWindow(w);
    if (win == NULL) return;
    ev.xconfigure.event = w;
    ev.xconfigure.window = w;
    ev.xconfigure.x = win->x;
    ev.xconfigure.y = win->y;
    ev.xconfigure.width = win->w;
    ev.xconfigure.height = win->h;
  }
  XcQueueEvent(&ev);
}

// ---------------------------------------------------------------------------
// Painting. All with the shared table locked.

typedef struct {
  XcWindow *win;
  int c0, r0, c1, r1;  // visible cell rectangle (clipped to ancestors)
} PaintEntry;

static PaintEntry paint[XC_MAX_WINDOWS];
static int CellWidth(unsigned int cp);
static unsigned long NearestColor(int r, int g, int b);
static int npaint;

static int Viewable(const XcWindow *w) {
  while (w != NULL) {
    if (w->id == XC_ROOT_WINDOW) return 1;
    if (!w->mapped) return 0;
    w = XcFindWindow(w->parent);
  }
  return 0;
}

static void AbsPos(const XcWindow *w, int *x, int *y) {
  *x = 0;
  *y = 0;
  while (w != NULL && w->id != XC_ROOT_WINDOW) {
    *x += w->x;
    *y += w->y;
    w = XcFindWindow(w->parent);
  }
}

static void CellRect(const XcWindow *w, int *c0, int *r0, int *c1, int *r1) {
  int cw = xc.shm->cell_w, ch = xc.shm->cell_h;
  int x, y;
  AbsPos(w, &x, &y);
  *c0 = x >= 0 ? x / cw : -((-x + cw - 1) / cw);
  *r0 = y >= 0 ? y / ch : -((-y + ch - 1) / ch);
  *c1 = (x + w->w + cw - 1) / cw;
  *r1 = (y + w->h + ch - 1) / ch;
}

static void AddPaint(XcWindow *w, int c0, int r0, int c1, int r1) {
  int a0, b0, a1, b1;
  CellRect(w, &a0, &b0, &a1, &b1);
  if (a0 < c0) a0 = c0;
  if (b0 < r0) b0 = r0;
  if (a1 > c1) a1 = c1;
  if (b1 > r1) b1 = r1;
  paint[npaint].win = w;
  paint[npaint].c0 = a0;
  paint[npaint].r0 = b0;
  paint[npaint].c1 = a1;
  paint[npaint].r1 = b1;
  ++npaint;
  // Children, bottom to top.
  uint64_t done_below = 0;
  int me = npaint - 1;
  for (;;) {
    XcWindow *next = NULL;
    for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
      XcWindow *c = &xc.shm->windows[i];
      if (c->id == 0 || c->parent != w->id || !c->mapped) continue;
      if (c->stack <= done_below) continue;
      if (next == NULL || c->stack < next->stack) next = c;
    }
    if (next == NULL) break;
    done_below = next->stack;
    AddPaint(next, paint[me].c0, paint[me].r0, paint[me].c1, paint[me].r1);
  }
}

static void BuildPaintOrder(void) {
  npaint = 0;
  XcWindow *root = XcFindWindow(XC_ROOT_WINDOW);
  AddPaint(root, 0, 0, xc.shm->cols, xc.shm->rows);
}

static int PaintIndex(const XcWindow *w) {
  for (int i = 0; i < npaint; ++i) {
    if (paint[i].win == w) return i;
  }
  return -1;
}

//! Whether cell (c, r) of paint entry i is covered by something above it.
static int Covered(int i, int c, int r) {
  for (int j = i + 1; j < npaint; ++j) {
    if (c >= paint[j].c0 && c < paint[j].c1 && r >= paint[j].r0 &&
        r < paint[j].r1) {
      return 1;
    }
  }
  return 0;
}

static unsigned long EffectiveBackground(const XcWindow *w) {
  while (w != NULL) {
    if (w->background != (unsigned long)-1) return w->background;
    w = XcFindWindow(w->parent);
  }
  return the_display.screen.black_pixel;
}

static void Sgr(char *buf, size_t *len, unsigned long fg, unsigned long bg) {
  *len += (size_t)sprintf(buf + *len, "\033[0;%lu;%lum",
                          fg < 8 ? 30 + fg : 90 + (fg - 8),
                          bg < 8 ? 40 + bg : 100 + (bg - 8));
}

static void Out(const char *buf, size_t len) {
  // One write per operation: the tty layer keeps it whole, so several
  // processes drawing at once do not interleave escape sequences.
  while (len > 0) {
    ssize_t n = write(xc.draw_fd, buf, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    buf += n;
    len -= (size_t)n;
  }
}

//! Repaint the cells of a rectangle with whatever is on top there.
static void RepaintCells(int c0, int r0, int c1, int r1) {
  if (c0 < 0) c0 = 0;
  if (r0 < 0) r0 = 0;
  if (c1 > xc.shm->cols) c1 = xc.shm->cols;
  if (r1 > xc.shm->rows) r1 = xc.shm->rows;
  if (c0 >= c1 || r0 >= r1) return;
  ++xc.shm->damage;
  size_t cap = (size_t)(r1 - r0) * ((size_t)(c1 - c0) * 24 + 32) + 16;
  char *buf = malloc(cap);
  if (buf == NULL) return;
  size_t len = 0;
  for (int r = r0; r < r1; ++r) {
    unsigned long cur = (unsigned long)-1;
    len += (size_t)sprintf(buf + len, "\033[%d;%dH", r + 1, c0 + 1);
    for (int c = c0; c < c1; ++c) {
      unsigned long bg = the_display.screen.black_pixel;
      for (int j = npaint - 1; j >= 0; --j) {
        if (c >= paint[j].c0 && c < paint[j].c1 && r >= paint[j].r0 &&
            r < paint[j].r1) {
          bg = EffectiveBackground(paint[j].win);
          break;
        }
      }
      if (bg != cur) {
        Sgr(buf, &len, bg, bg);
        cur = bg;
      }
      buf[len++] = ' ';
    }
  }
  Out(buf, len);
  free(buf);
}

int XcViewable(const XcWindow *w) { return Viewable(w); }

unsigned long XcWindowBackground(const XcWindow *w) {
  return EffectiveBackground(w);
}

int XcWindowCellsLocked(const XcWindow *w, int *c0, int *r0, int *c1,
                        int *r1) {
  CellRect(w, c0, r0, c1, r1);
  if (*c0 < 0) *c0 = 0;
  if (*r0 < 0) *r0 = 0;
  if (*c1 > xc.shm->cols) *c1 = xc.shm->cols;
  if (*r1 > xc.shm->rows) *r1 = xc.shm->rows;
  return *c0 < *c1 && *r0 < *r1;
}

int XcPrintableCell(uint32_t cp) {
  if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp >= 0x110000 ||
      (cp >= 0xd800 && cp < 0xe000)) {
    return 0;
  }
  return CellWidth(cp) == 1;
}

static size_t PutUtf8(char *buf, uint32_t cp) {
  if (cp < 0x80) {
    buf[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    buf[0] = (char)(0xc0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    buf[0] = (char)(0xe0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    buf[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  buf[0] = (char)(0xf0 | (cp >> 18));
  buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  buf[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

// The saver channel's green ramp (saver_proto.h), brightest first: a
// near-white green for the head of a trail, then greens darker and darker,
// the last one barely above black. Shown with palette entries 1-6 of the lock
// VT, set with the console's own escape (ESC ] P n rrggbb), which changes
// that VT's palette only, never the other consoles' (PIO_CMAP would).
#define RAMP_FIRST 1
static const unsigned char ramp_rgb[XC_SAVER_RAMP_LEVELS][3] = {
    {0xd0, 0xff, 0xd0}, {0x30, 0xf0, 0x30}, {0x10, 0xc0, 0x10},
    {0x08, 0x90, 0x08}, {0x04, 0x60, 0x04}, {0x02, 0x38, 0x02}};

void XcSetRampLocked(int on) {
  if (xc.shm->ramp_on == on) return;
  char buf[XC_SAVER_RAMP_LEVELS * 12];
  size_t len = 0;
  for (int k = 0; k < XC_SAVER_RAMP_LEVELS; ++k) {
    const unsigned char *rgb = on ? ramp_rgb[k] : xc.shm->palette[RAMP_FIRST + k];
    len += (size_t)sprintf(buf + len, "\033]P%X%02x%02x%02x", RAMP_FIRST + k,
                           rgb[0], rgb[1], rgb[2]);
  }
  Out(buf, len);
  xc.shm->ramp_on = on;
}

//! A saver cell's colour as a console colour: the ramp's entries while it
//! is on, else the console colour nearest to the ramp colour.
static unsigned long SaverColor(uint8_t c) {
  if (c < 16) return c;
  int k = c - XC_SAVER_RAMP;
  if (k < 0 || k >= XC_SAVER_RAMP_LEVELS) return 7;
  if (xc.shm->ramp_on) return (unsigned long)(RAMP_FIRST + k);
  return NearestColor(ramp_rgb[k][0], ramp_rgb[k][1], ramp_rgb[k][2]);
}

void XcPaintGridLocked(const XcWindow *w, int cols, int rows,
                       const XcGridCell *grid, const uint32_t *list, int n) {
  if (!Viewable(w)) return;
  BuildPaintOrder();
  int i = PaintIndex(w);
  int wc0, wr0, wc1, wr1;
  if (i < 0) return;
  CellRect(w, &wc0, &wr0, &wc1, &wr1);
  int total = n < 0 ? cols * rows : n;
  // At most a cursor move, a colour change and 4 bytes per cell.
  char *buf = malloc((size_t)total * 40 + 16);
  if (buf == NULL) return;
  size_t len = 0;
  int at_c = -1, at_r = -1;
  unsigned long cur_fg = (unsigned long)-1, cur_bg = (unsigned long)-1;
  for (int k = 0; k < total; ++k) {
    uint32_t idx = n < 0 ? (uint32_t)k : list[k];
    if (idx >= (uint32_t)(cols * rows)) continue;
    int c = wc0 + (int)idx % cols, r = wr0 + (int)idx / cols;
    if (c < paint[i].c0 || c >= paint[i].c1 || r < paint[i].r0 ||
        r >= paint[i].r1 || Covered(i, c, r)) {
      continue;
    }
    const XcGridCell *g = &grid[idx];
    if (c != at_c || r != at_r) {
      len += (size_t)sprintf(buf + len, "\033[%d;%dH", r + 1, c + 1);
      cur_fg = cur_bg = (unsigned long)-1;
    }
    unsigned long fg = SaverColor(g->fg), bg = SaverColor(g->bg);
    if (fg != cur_fg || bg != cur_bg) {
      Sgr(buf, &len, fg, bg);
      cur_fg = fg;
      cur_bg = bg;
    }
    uint32_t cp = XcRainAliasLocked(g->cp);
    len += PutUtf8(buf + len, XcPrintableCell(cp) ? cp : ' ');
    at_c = c + 1;
    at_r = r;
  }
  Out(buf, len);
  free(buf);
}

static void RepaintWindowArea(const XcWindow *w) {
  int c0, r0, c1, r1;
  CellRect(w, &c0, &r0, &c1, &r1);
  BuildPaintOrder();
  RepaintCells(c0, r0, c1, r1);
}

//! Paint a window's visible cells with its background (XClearWindow).
static void ClearVisible(XcWindow *w) {
  if (w->background == (unsigned long)-1 || !Viewable(w)) return;
  BuildPaintOrder();
  int i = PaintIndex(w);
  if (i < 0) return;
  PaintEntry *p = &paint[i];
  if (p->c0 >= p->c1 || p->r0 >= p->r1) return;
  size_t cap = (size_t)(p->r1 - p->r0) * ((size_t)(p->c1 - p->c0) * 24 + 32);
  char *buf = malloc(cap + 16);
  if (buf == NULL) return;
  size_t len = 0;
  for (int r = p->r0; r < p->r1; ++r) {
    int at = -1;
    for (int c = p->c0; c < p->c1; ++c) {
      if (Covered(i, c, r)) {
        at = -1;
        continue;
      }
      if (at != c) {
        len += (size_t)sprintf(buf + len, "\033[%d;%dH", r + 1, c + 1);
        Sgr(buf, &len, w->background, w->background);
      }
      buf[len++] = ' ';
      at = c + 1;
    }
  }
  Out(buf, len);
  free(buf);
}

// ---------------------------------------------------------------------------
// Display.

static int ParseConnection(const char *name, int *draw_fd, int *shm_fd,
                           int *saver_fd) {
  if (name == NULL || strncmp(name, DISPLAY_PREFIX, strlen(DISPLAY_PREFIX))) {
    return 0;
  }
  return sscanf(name + strlen(DISPLAY_PREFIX), "%d:%d:%d", draw_fd, shm_fd,
                saver_fd) == 3;
}

static void FontCell(int fd, int *cw, int *ch) {
  struct console_font_op op;
  memset(&op, 0, sizeof(op));
  op.op = KD_FONT_OP_GET_TALL;
  op.width = 64;
  op.height = 128;
  if (ioctl(fd, KDFONTOP, &op) < 0) {
    memset(&op, 0, sizeof(op));
    op.op = KD_FONT_OP_GET;
    op.width = 64;
    op.height = 32;
    if (ioctl(fd, KDFONTOP, &op) < 0) {
      op.width = 8;
      op.height = 16;
    }
  }
  *cw = (int)op.width;
  *ch = (int)op.height;
}

static void ScreenSize(void) {
  struct winsize ws;
  if (ioctl(xc.vt_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    xc.shm->cols = ws.ws_col;
    xc.shm->rows = ws.ws_row;
  } else {
    xc.shm->cols = 80;
    xc.shm->rows = 25;
  }
  FontCell(xc.vt_fd, &xc.shm->cell_w, &xc.shm->cell_h);
}

static unsigned long NearestColor(int r, int g, int b) {
  unsigned long best = 0;
  long best_d = -1;
  for (int i = 0; i < 16; ++i) {
    long dr = r - xc.shm->palette[i][0], dg = g - xc.shm->palette[i][1],
         db = b - xc.shm->palette[i][2];
    long d = dr * dr + dg * dg + db * db;
    if (best_d < 0 || d < best_d) {
      best_d = d;
      best = (unsigned long)i;
    }
  }
  return best;
}

static void FillScreen(void) {
  XcLock();
  int w = xc.shm->cols * xc.shm->cell_w, h = xc.shm->rows * xc.shm->cell_h;
  XcUnlock();
  the_display.screen.width = w;
  the_display.screen.height = h;
  the_display.screen.mwidth = w / 4;
  the_display.screen.mheight = h / 4;
  the_display.screen.root = XC_ROOT_WINDOW;
  the_display.screen.root_depth = 4;
  the_display.screen.root_visual = &the_display.visual;
  the_display.screen.cmap = 1;
  the_display.screen.black_pixel = NearestColor(0, 0, 0);
  the_display.screen.white_pixel = NearestColor(255, 255, 255);
  the_display.screen.display = dpy_ptr;
  the_display.visual.visualid = 1;
  the_display.visual.class = PseudoColor;
  the_display.visual.bits_per_rgb = 8;
  the_display.visual.map_entries = 16;
  the_display.d.default_screen = 0;
  the_display.d.nscreens = 1;
  the_display.d.screens = &the_display.screen;
  the_display.d.vendor = "ttysecurelock xconsole";
  the_display.d.proto_major_version = 11;
  the_display.d.min_keycode = 8;
  the_display.d.max_keycode = 255;
}

static Display *OpenMain(void) {
  xc.is_main = 1;
  xc.shm_fd = memfd_create("xconsole", 0);  // inherited by helpers
  if (xc.shm_fd < 0 || ftruncate(xc.shm_fd, sizeof(XcShared)) < 0) {
    LogErrno("memfd");
    return NULL;
  }
  xc.shm = mmap(NULL, sizeof(XcShared), PROT_READ | PROT_WRITE, MAP_SHARED,
                xc.shm_fd, 0);
  if (xc.shm == MAP_FAILED) {
    LogErrno("mmap");
    return NULL;
  }
  memset(xc.shm, 0, sizeof(*xc.shm));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&xc.shm->lock, &attr);
  xc.shm->magic = XC_MAGIC;
  xc.shm->next_xid = 0x00200000;
  xc.shm->next_stack = 1;
  xc.shm->natoms = 0;

  if (!XcVtOpen()) return NULL;
  // The saver channel: saver modules connect to the lock through the end
  // every helper inherits (saver.c); the lock keeps the other.
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
    LogErrno("socketpair");
    return NULL;
  }
  fcntl(sv[0], F_SETFD, FD_CLOEXEC);
  xc.saver_fd = sv[0];
  XcSaverSaveFont();
  ScreenSize();
  unsigned char cmap[48];
  if (ioctl(xc.vt_fd, GIO_CMAP, cmap) == 0) {
    memcpy(xc.shm->palette, cmap, sizeof(cmap));
  } else {
    static const unsigned char vga[16][3] = {
        {0, 0, 0},       {170, 0, 0},    {0, 170, 0},    {170, 85, 0},
        {0, 0, 170},     {170, 0, 170},  {0, 170, 170},  {170, 170, 170},
        {85, 85, 85},    {255, 85, 85},  {85, 255, 85},  {255, 255, 85},
        {85, 85, 255},   {255, 85, 255}, {85, 255, 255}, {255, 255, 255}};
    memcpy(xc.shm->palette, vga, sizeof(vga));
  }
  FillScreen();
  XcWindow *root = &xc.shm->windows[0];
  root->id = XC_ROOT_WINDOW;
  root->parent = None;
  root->w = the_display.screen.width;
  root->h = the_display.screen.height;
  root->mapped = 1;
  root->background = the_display.screen.black_pixel;
  root->owner = getpid();

  char name[64];
  snprintf(name, sizeof(name), DISPLAY_PREFIX "%d:%d:%d", xc.draw_fd,
           xc.shm_fd, sv[1]);
  setenv("DISPLAY", name, 1);
  the_display.d.fd = xc.vt_fd;
  the_display.d.display_name = strdup(name);

  static const char init[] = "\033[0m\033[?25l\033[H\033[2J";
  Out(init, sizeof(init) - 1);
  BuildPaintOrder();
  RepaintCells(0, 0, xc.shm->cols, xc.shm->rows);

  if (!XcVtDropPrivileges()) return NULL;
  // After dropping privileges: capabilities are per thread.
  if (!XcSaverServerStart()) return NULL;
  return dpy_ptr;
}

static Display *OpenHelper(int draw_fd, int shm_fd, int saver_fd) {
  // A helper's own children (authproto_pam, the saver run by
  // saver_multiplex, e.g. sleep) do not get the lock VT or the window table,
  // and a dumpable one cannot hand them to the user's other processes
  // through /proc/<pid>/fd. They keep the saver channel (saver_fd): a saver
  // module draws into its saver window through the lock (saver.c).
  fcntl(draw_fd, F_SETFD, FD_CLOEXEC);
  fcntl(shm_fd, F_SETFD, FD_CLOEXEC);
  xc.is_main = 0;
  xc.saver_fd = saver_fd;
  xc.draw_fd = draw_fd;
  xc.shm_fd = shm_fd;
  xc.shm = mmap(NULL, sizeof(XcShared), PROT_READ | PROT_WRITE, MAP_SHARED,
                shm_fd, 0);
  if (xc.shm == MAP_FAILED || xc.shm->magic != XC_MAGIC) {
    Log("Not a ttysecurelock display");
    return NULL;
  }
  FillScreen();
  the_display.d.fd = draw_fd;
  the_display.d.display_name = getenv("DISPLAY");
  return dpy_ptr;
}

Display *XOpenDisplay(const char *name) {
  if (xc.shm != NULL) return dpy_ptr;
  if (name == NULL) name = getenv("DISPLAY");
  int draw_fd, shm_fd, saver_fd;
  if (ParseConnection(name, &draw_fd, &shm_fd, &saver_fd)) {
    return OpenHelper(draw_fd, shm_fd, saver_fd);
  }
  return OpenMain();
}

int XCloseDisplay(Display *dpy) {
  (void)dpy;
  if (xc.is_main) {
    XcSaverServerStop();
    XcVtRelease(1);
  }
  return 0;
}

//! Windows of processes that are gone are destroyed, as the X server
//! destroys the windows of a client that disconnects.
static void ReapWindows(void) {
  for (int i = 1; i < XC_MAX_WINDOWS; ++i) {
    XcWindow *w = &xc.shm->windows[i];
    if (w->id == 0 || w->owner == getpid()) continue;
    if (kill(w->owner, 0) < 0 && errno == ESRCH) {
      XDestroyWindow(dpy_ptr, w->id);
    }
  }
}

static void CheckScreenSize(void) {
  struct winsize ws;
  if (ioctl(xc.vt_fd, TIOCGWINSZ, &ws) < 0) return;
  XcLock();
  int changed = ws.ws_col != xc.shm->cols || ws.ws_row != xc.shm->rows;
  if (changed) {
    xc.shm->cols = ws.ws_col;
    xc.shm->rows = ws.ws_row;
    FontCell(xc.vt_fd, &xc.shm->cell_w, &xc.shm->cell_h);
    XcWindow *root = XcFindWindow(XC_ROOT_WINDOW);
    root->w = xc.shm->cols * xc.shm->cell_w;
    root->h = xc.shm->rows * xc.shm->cell_h;
  }
  XcUnlock();
  if (changed) {
    FillScreen();
    QueueWindowEvent(ConfigureNotify, XC_ROOT_WINDOW);
  }
}

//! Read all the keyboard input there is (waiting up to timeout_ms for
//! some, less if a key repeat is due first), and queue the key repeats due.
static void ReadInput(int timeout_ms) {
  int repeat_ms = XcKeyboardTimeoutMs();
  if (repeat_ms >= 0 && (timeout_ms < 0 || repeat_ms < timeout_ms)) {
    timeout_ms = repeat_ms;
  }
  struct pollfd pfd = {xc.vt_fd, POLLIN, 0};
  while (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
    unsigned char buf[256];
    ssize_t n = read(xc.vt_fd, buf, sizeof(buf));
    if (n <= 0) break;
    XcKeyboardInput(dpy_ptr, buf, (size_t)n);
    timeout_ms = 0;
  }
  XcKeyboardTick(dpy_ptr);
}

int XPending(Display *dpy) {
  (void)dpy;
  if (!xc.is_main) return 0;
  ReapWindows();
  CheckScreenSize();
  ReadInput(0);
  return eq_len;
}

int XNextEvent(Display *dpy, XEvent *ev) {
  (void)dpy;
  while (xc.is_main && eq_len == 0) {
    ReadInput(1000);
    CheckScreenSize();
  }
  if (eq_len == 0) {
    memset(ev, 0, sizeof(*ev));
    return 0;
  }
  *ev = event_queue[eq_head].ev;
  if (ev->type == KeyPress || ev->type == KeyRelease) {
    XcKeyDequeued(ev->xkey.serial, &event_queue[eq_head].key);
  }
  eq_head = (eq_head + 1) % eq_cap;
  --eq_len;
  return 0;
}

Bool XFilterEvent(XEvent *ev, Window w) {
  (void)ev;
  (void)w;
  return False;
}

int XFlush(Display *dpy) {
  (void)dpy;
  return 1;
}

// ---------------------------------------------------------------------------
// Windows.

Window XCreateWindow(Display *dpy, Window parent, int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width, int depth, unsigned int class,
                     Visual *visual, unsigned long valuemask,
                     XSetWindowAttributes *attributes) {
  (void)dpy;
  (void)border_width;
  (void)depth;
  (void)class;
  (void)visual;
  XcLock();
  Window id = None;
  for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
    XcWindow *w = &xc.shm->windows[i];
    if (w->id != 0) continue;
    id = (Window)xc.shm->next_xid++;
    memset(w, 0, sizeof(*w));
    w->id = id;
    w->parent = parent;
    w->x = x;
    w->y = y;
    w->w = (int)width;
    w->h = (int)height;
    w->background = (valuemask & CWBackPixel) && attributes != NULL
                        ? attributes->background_pixel
                        : (unsigned long)-1;
    w->stack = xc.shm->next_stack++;
    w->owner = getpid();
    break;
  }
  XcUnlock();
  if (id == None) Log("xconsole: too many windows");
  return id;
}

int XDestroyWindow(Display *dpy, Window w) {
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win == NULL || w == XC_ROOT_WINDOW) {
    XcUnlock();
    return 0;
  }
  int was_viewable = Viewable(win);
  int c0, r0, c1, r1;
  CellRect(win, &c0, &r0, &c1, &r1);
  // The whole subtree goes.
  Window doomed[XC_MAX_WINDOWS];
  int n = 0;
  doomed[n++] = w;
  for (int k = 0; k < n; ++k) {
    for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
      if (xc.shm->windows[i].id != 0 &&
          xc.shm->windows[i].parent == doomed[k] && n < XC_MAX_WINDOWS) {
        doomed[n++] = xc.shm->windows[i].id;
      }
    }
  }
  for (int k = 0; k < n; ++k) {
    XcWindow *d = XcFindWindow(doomed[k]);
    if (d != NULL) memset(d, 0, sizeof(*d));
  }
  if (was_viewable) {
    BuildPaintOrder();
    RepaintCells(c0, r0, c1, r1);
  }
  XcUnlock();
  (void)dpy;
  return 1;
}

static int SetMapped(Window w, int mapped, int raise) {
  // The prompt is about to show: the lock VT gets the console font back
  // first (only the main program may load fonts; saver.c).
  if (mapped && w == xc.shm->auth_window) XcRainFontOffForPrompt();
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win == NULL) {
    XcUnlock();
    return 0;
  }
  int changed = win->mapped != mapped;
  win->mapped = mapped;
  if (raise) win->stack = xc.shm->next_stack++;
  // The prompt shows: its colours are the console's, before it is painted.
  if (changed && mapped && w == xc.shm->auth_window) XcSetRampLocked(0);
  if (changed || raise) RepaintWindowArea(win);
  if (changed && mapped && w == xc.shm->auth_window) {
    // The prompt shows: the saver window goes blank now, not when the saver
    // channel's thread next looks (saver.c).
    XcWindow *saver = XcFindWindow(xc.shm->saver_window);
    if (saver != NULL && Viewable(saver)) RepaintWindowArea(saver);
  }
  XcUnlock();
  if (changed) {
    QueueWindowEvent(mapped ? MapNotify : UnmapNotify, w);
    if (mapped) QueueWindowEvent(VisibilityNotify, w);
  }
  return 1;
}

int XMapWindow(Display *dpy, Window w) {
  (void)dpy;
  return SetMapped(w, 1, 0);
}

int XMapRaised(Display *dpy, Window w) {
  (void)dpy;
  return SetMapped(w, 1, 1);
}

int XUnmapWindow(Display *dpy, Window w) {
  (void)dpy;
  return SetMapped(w, 0, 0);
}

int XRaiseWindow(Display *dpy, Window w) {
  (void)dpy;
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win != NULL) {
    win->stack = xc.shm->next_stack++;
    if (Viewable(win)) RepaintWindowArea(win);
  }
  XcUnlock();
  return 1;
}

int XRestackWindows(Display *dpy, Window *windows, int n) {
  (void)dpy;
  // windows[0] is the top one.
  XcLock();
  for (int i = n - 1; i >= 0; --i) {
    XcWindow *win = XcFindWindow(windows[i]);
    if (win != NULL) win->stack = xc.shm->next_stack++;
  }
  XcUnlock();
  return 1;
}

int XConfigureWindow(Display *dpy, Window w, unsigned int mask,
                     XWindowChanges *changes) {
  if ((mask & CWStackMode) && changes->stack_mode == Above) {
    XRaiseWindow(dpy, w);
  }
  return 1;
}

//! The children of w that covered it exactly (old size) get its new size,
//! and so on down (XMoveResizeWindow).
static void FollowResize(Window w, int ow, int oh, int nw, int nh, int depth) {
  if (depth >= XC_MAX_WINDOWS) return;
  for (int k = 0; k < XC_MAX_WINDOWS; ++k) {
    XcWindow *c = &xc.shm->windows[k];
    if (c->id != 0 && c->parent == w && c->x == 0 && c->y == 0 &&
        c->w == ow && c->h == oh) {
      FollowResize(c->id, ow, oh, nw, nh, depth + 1);
      c->w = nw;
      c->h = nh;
    }
  }
}

int XMoveResizeWindow(Display *dpy, Window w, int x, int y,
                      unsigned int width, unsigned int height) {
  (void)dpy;
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win == NULL) {
    XcUnlock();
    return 0;
  }
  if (win->x == x && win->y == y && win->w == (int)width &&
      win->h == (int)height) {
    XcUnlock();
    return 1;
  }
  int viewable = Viewable(win);
  int c0, r0, c1, r1;
  CellRect(win, &c0, &r0, &c1, &r1);
  // Children that covered the window exactly keep covering it, and theirs
  // them. This stands in for XRandR, which the console lacks: under X a
  // screen change makes saver_multiplex recreate its per-monitor windows
  // (and savers) at the new size; here the screen is one monitor, and its
  // window follows the windows main.c resizes with the screen (a rotated
  // console).
  FollowResize(w, win->w, win->h, (int)width, (int)height, 0);
  win->x = x;
  win->y = y;
  win->w = (int)width;
  win->h = (int)height;
  if (viewable) {
    BuildPaintOrder();
    RepaintCells(c0, r0, c1, r1);
    RepaintWindowArea(win);
  }
  XcUnlock();
  QueueWindowEvent(ConfigureNotify, w);
  return 1;
}

int XChangeWindowAttributes(Display *dpy, Window w, unsigned long valuemask,
                            XSetWindowAttributes *attributes) {
  (void)dpy;
  if (valuemask & CWBackPixel) {
    XcLock();
    XcWindow *win = XcFindWindow(w);
    if (win != NULL) win->background = attributes->background_pixel;
    XcUnlock();
  }
  return 1;
}

int XClearWindow(Display *dpy, Window w) {
  (void)dpy;
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win != NULL) ClearVisible(win);
  XcUnlock();
  return 1;
}

Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *xwa) {
  memset(xwa, 0, sizeof(*xwa));
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win == NULL) {
    XcUnlock();
    return 0;
  }
  xwa->x = win->x;
  xwa->y = win->y;
  xwa->width = win->w;
  xwa->height = win->h;
  xwa->depth = 4;
  xwa->visual = &the_display.visual;
  xwa->root = XC_ROOT_WINDOW;
  xwa->class = InputOutput;
  xwa->map_state = Viewable(win) ? IsViewable
                   : win->mapped ? IsUnviewable
                                 : IsUnmapped;
  xwa->override_redirect = True;
  xwa->screen = &the_display.screen;
  XcUnlock();
  (void)dpy;
  return 1;
}

Status XQueryTree(Display *dpy, Window w, Window *root_return,
                  Window *parent_return, Window **children_return,
                  unsigned int *nchildren_return) {
  (void)dpy;
  XcLock();
  XcWindow *win = XcFindWindow(w);
  if (win == NULL) {
    XcUnlock();
    return 0;
  }
  *root_return = XC_ROOT_WINDOW;
  *parent_return = win->parent;
  Window *children = malloc(sizeof(Window) * XC_MAX_WINDOWS);
  unsigned int n = 0;
  uint64_t done_below = 0;
  for (;;) {  // bottom to top, as X lists them
    XcWindow *next = NULL;
    for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
      XcWindow *c = &xc.shm->windows[i];
      if (c->id == 0 || c->parent != w || c->stack <= done_below) continue;
      if (next == NULL || c->stack < next->stack) next = c;
    }
    if (next == NULL) break;
    done_below = next->stack;
    if (children != NULL) children[n++] = next->id;
  }
  XcUnlock();
  *children_return = children;
  *nchildren_return = n;
  return 1;
}

int XSelectInput(Display *dpy, Window w, long mask) {
  (void)dpy;
  for (int i = 0; i < XC_MAX_WINDOWS; ++i) {
    if (selected[i].w == w || selected[i].w == None) {
      selected[i].w = w;
      selected[i].mask = mask;
      return 1;
    }
  }
  return 1;
}

Bool XQueryPointer(Display *dpy, Window w, Window *root_return,
                   Window *child_return, int *root_x, int *root_y,
                   int *win_x, int *win_y, unsigned int *mask) {
  (void)dpy;
  (void)w;
  // No pointer on the console: report it at the top left corner.
  *root_return = XC_ROOT_WINDOW;
  *child_return = None;
  *root_x = *root_y = *win_x = *win_y = 0;
  *mask = 0;
  return True;
}

// ---------------------------------------------------------------------------
// Colors, GCs, fonts, text.

struct _XGC {
  XExtData *ext_data;
  GContext gid;
  unsigned long foreground;
  unsigned long background;
};

// The X server's colour names (xserver os/oscolor.c).
typedef struct {
  unsigned char red, green, blue;
  unsigned short name;
} XcBuiltinColor;
#include "xcolors.inc"

//! xserver's OsLookupColor (os/oscolor.c): case-insensitive, whole name.
static int OsLookupColor(const char *name, unsigned short *pred,
                         unsigned short *pgreen, unsigned short *pblue) {
  size_t len = strlen(name);
  int low = 0, high = (int)(sizeof(BuiltinColors) / sizeof(*BuiltinColors)) - 1;
  while (high >= low) {
    int mid = (low + high) / 2;
    const XcBuiltinColor *c = &BuiltinColors[mid];
    int r = strncasecmp(&BuiltinColorNames[c->name], name, len);
    if (r == 0 && len == strlen(&BuiltinColorNames[c->name])) {
      *pred = (unsigned short)(c->red * 0x101);
      *pgreen = (unsigned short)(c->green * 0x101);
      *pblue = (unsigned short)(c->blue * 0x101);
      return 1;
    }
    if (r < 0)
      low = mid + 1;
    else
      high = mid - 1;
  }
  return 0;
}

/*! \brief A colour spec as XAllocNamedColor resolves it: Xcms first (the
 * spec lowercased; "#" and "rgb:" as libX11's XcmsLRGB_RGB_ParseString,
 * "rgbi:" as XcmsLRGB_RGBi_ParseString, src/xcms/LRGB.c), then the server's
 * colour names. The device-independent Xcms spaces (CIEXYZ:, CIEuvY:,
 * CIExyY:, CIELab:, CIELuv:, TekHVC:) are not supported.
 * \return 1 and 16-bit RGB on success. */
static int ParseColor(const char *spec, unsigned short *r, unsigned short *g,
                      unsigned short *b) {
  char lowered[256];
  size_t n = strlen(spec);
  if (n >= sizeof(lowered)) return 0;
  for (size_t i = 0; i <= n; ++i) lowered[i] = (char)tolower((unsigned char)spec[i]);
  const char *p = lowered;
  if (*p == '#') {
    ++p;
    n = strlen(p);
    if (n != 3 && n != 6 && n != 9 && n != 12) return 0;
    n /= 3;
    unsigned short v[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
      for (size_t i = 0; i < n; ++i) {
        char c = *p++;
        v[k] = (unsigned short)(v[k] << 4);
        if (c >= '0' && c <= '9')
          v[k] |= (unsigned short)(c - '0');
        else if (c >= 'a' && c <= 'f')
          v[k] |= (unsigned short)(c - ('a' - 10));
        else
          return 0;
      }
      // Shift instead of scale, to match old broken semantics (libX11).
      v[k] = (unsigned short)(v[k] << (16 - 4 * n));
    }
    *r = v[0];
    *g = v[1];
    *b = v[2];
    return 1;
  }
  if (!strncmp(p, "rgb:", 4)) {
    p += 4;
    unsigned short v[3];
    for (int k = 0; k < 3; ++k, ++p) {
      int digits = 0;
      unsigned long x = 0;
      while (*p != '/' && *p != '\0') {
        if (++digits > 4) return 0;
        char c = *p++;
        x <<= 4;
        if (c >= '0' && c <= '9')
          x |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
          x |= (unsigned long)(c - ('a' - 10));
        else
          return 0;
      }
      if (digits == 0) return 0;
      if (digits < 4) x = x * 0xFFFF / ((1ul << (digits * 4)) - 1);
      v[k] = (unsigned short)x;
      if (k < 2 && *p != '/') return 0;
    }
    *r = v[0];
    *g = v[1];
    *b = v[2];
    return 1;
  }
  if (!strncmp(p, "rgbi:", 5)) {
    double v[3];
    if (sscanf(p + 5, "%lf/%lf/%lf", &v[0], &v[1], &v[2]) != 3) return 0;
    for (int k = 0; k < 3; ++k) {
      if (v[k] < 0.0 || v[k] > 1.0) return 0;
    }
    // Linear, where Xcms goes through the screen's intensity tables.
    *r = (unsigned short)(v[0] * 65535.0 + 0.5);
    *g = (unsigned short)(v[1] * 65535.0 + 0.5);
    *b = (unsigned short)(v[2] * 65535.0 + 0.5);
    return 1;
  }
  static const char *const xcms_spaces[] = {"ciexyz:", "cieuvy:", "ciexyy:",
                                            "cielab:", "cieluv:", "tekhvc:"};
  for (size_t i = 0; i < sizeof(xcms_spaces) / sizeof(*xcms_spaces); ++i) {
    if (!strncmp(p, xcms_spaces[i], strlen(xcms_spaces[i]))) {
      Log("xconsole: colour space of \"%s\" is not supported", spec);
      return 0;
    }
  }
  return OsLookupColor(spec, r, g, b);
}

static void PixelColor(unsigned long pixel, XColor *c) {
  pixel &= 15;
  c->pixel = pixel;
  c->red = (unsigned short)(xc.shm->palette[pixel][0] * 257);
  c->green = (unsigned short)(xc.shm->palette[pixel][1] * 257);
  c->blue = (unsigned short)(xc.shm->palette[pixel][2] * 257);
  c->flags = DoRed | DoGreen | DoBlue;
}

/*! \brief As X: the nearest console colour to the named one. On failure
 * both colours are set to black (X leaves them as they were; auth_x11 does
 * not check the result, and an uninitialised colour must not decide what
 * the prompt looks like). */
Status XAllocNamedColor(Display *dpy, Colormap cmap, const char *name,
                        XColor *screen_def, XColor *exact_def) {
  (void)dpy;
  (void)cmap;
  unsigned short r, g, b;
  if (name == NULL || !ParseColor(name, &r, &g, &b)) {
    PixelColor(the_display.screen.black_pixel, screen_def);
    *exact_def = *screen_def;
    return 0;
  }
  exact_def->red = r;
  exact_def->green = g;
  exact_def->blue = b;
  exact_def->flags = DoRed | DoGreen | DoBlue;
  PixelColor(NearestColor(r >> 8, g >> 8, b >> 8), screen_def);
  exact_def->pixel = screen_def->pixel;
  return 1;
}

int XQueryColor(Display *dpy, Colormap cmap, XColor *def) {
  (void)dpy;
  (void)cmap;
  PixelColor(def->pixel, def);
  return 1;
}

int XFreeColors(Display *dpy, Colormap cmap, unsigned long *pixels,
                int npixels, unsigned long planes) {
  (void)dpy;
  (void)cmap;
  (void)pixels;
  (void)npixels;
  (void)planes;
  return 1;
}

GC XCreateGC(Display *dpy, Drawable d, unsigned long valuemask,
             XGCValues *values) {
  (void)dpy;
  (void)d;
  GC gc = calloc(1, sizeof(*gc));
  if (gc == NULL) return NULL;
  gc->foreground = the_display.screen.white_pixel;
  gc->background = the_display.screen.black_pixel;
  if (values != NULL && (valuemask & GCForeground)) {
    gc->foreground = values->foreground;
  }
  if (values != NULL && (valuemask & GCBackground)) {
    gc->background = values->background;
  }
  return gc;
}

int XFreeGC(Display *dpy, GC gc) {
  (void)dpy;
  free(gc);
  return 1;
}

/*! \brief The console font as an X core font.
 *
 * Its height is one cell less auth_x11's 4 pixels of line spacing
 * (LINE_SPACING), so that its lines of text are exactly one row apart.
 */
XFontStruct *XLoadQueryFont(Display *dpy, const char *name) {
  (void)dpy;
  (void)name;
  XFontStruct *f = calloc(1, sizeof(*f));
  if (f == NULL) return NULL;
  XcLock();
  int cw = xc.shm->cell_w, ch = xc.shm->cell_h;
  XcUnlock();
  f->fid = (Font)next_local_id++;
  f->direction = FontLeftToRight;
  f->ascent = ch - 4;
  f->descent = 0;
  f->max_bounds.ascent = (short)(ch - 4);
  f->max_bounds.descent = 0;
  f->max_bounds.width = (short)cw;
  f->min_bounds = f->max_bounds;
  f->min_char_or_byte2 = 0;
  f->max_char_or_byte2 = 255;
  return f;
}

//! Whether c is a combining mark (drawn in the previous cell).
static int Combining(unsigned int c) {
  return (c >= 0x300 && c <= 0x36f) || (c >= 0x483 && c <= 0x489) ||
         (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
         (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe20 && c <= 0xfe2f);
}

//! Decode one UTF-8 character; returns its length in bytes (>= 1).
static int Utf8Next(const char *s, int len, unsigned int *cp) {
  unsigned char c = (unsigned char)s[0];
  int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
  if (n > len) n = len;
  unsigned int v = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f)
                                                           : (c & 0x07);
  for (int i = 1; i < n; ++i) v = (v << 6) | ((unsigned char)s[i] & 0x3f);
  *cp = v;
  return n;
}

/*! \brief Cells a character takes: 0 for a combining mark (the console
 * draws it into the previous cell), 2 for a double-width one (the console
 * pads it with a zero-width cell), else 1. */
static int CellWidth(unsigned int cp) {
  if (Combining(cp)) return 0;
  if (cp < 0x1100) return 1;
  // The console's widths are Unicode's whatever the program's locale is.
  static locale_t utf8 = (locale_t)0;
  if (utf8 == (locale_t)0) utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", 0);
  if (utf8 == (locale_t)0) return 1;
  locale_t old = uselocale(utf8);
  int w = wcwidth((wchar_t)cp);
  uselocale(old);
  return w == 2 ? 2 : 1;
}

static int TextCells(const char *s, int len) {
  int cells = 0;
  for (int i = 0; i < len;) {
    unsigned int cp;
    i += Utf8Next(s + i, len - i, &cp);
    cells += CellWidth(cp);
  }
  return cells;
}

int XTextWidth(XFontStruct *font, const char *string, int count) {
  (void)font;
  return TextCells(string, count) * xc.shm->cell_w;
}

int XDrawString(Display *dpy, Drawable d, GC gc, int x, int y,
                const char *string, int length) {
  (void)dpy;
  XcLock();
  XcWindow *win = XcFindWindow(d);
  if (win == NULL || !Viewable(win)) {
    XcUnlock();
    return 0;
  }
  BuildPaintOrder();
  int i = PaintIndex(win);
  int ax, ay;
  AbsPos(win, &ax, &ay);
  ax += x;
  ay += y;
  int cw = xc.shm->cell_w, ch = xc.shm->cell_h;
  // Glyphs sit on the baseline: the row holding the pixel above it.
  int row = (ay - 1) >= 0 ? (ay - 1) / ch : -1;
  int col = ax >= 0 ? (2 * ax + cw) / (2 * cw) : -1;
  unsigned long bg = EffectiveBackground(win);
  size_t cap = (size_t)length * 24 + 64;
  char *buf = malloc(cap);
  if (buf == NULL || i < 0) {
    free(buf);
    XcUnlock();
    return 0;
  }
  size_t len = 0;
  int at = -1;   // the cell the console cursor is at after our last output
  int c = col;   // the next character's cell
  int prev = -1; // the previous character's cell, if it was drawn
  for (int k = 0; k < length;) {
    unsigned int cp;
    int n = Utf8Next(string + k, length - k, &cp);
    int w = CellWidth(cp);
    int visible = 1;
    for (int x = c; x < c + (w ? w : 1); ++x) {
      visible = visible && x >= paint[i].c0 && x < paint[i].c1 &&
                row >= paint[i].r0 && row < paint[i].r1 && !Covered(i, x, row);
    }
    if (w == 0) {
      // A combining mark goes with the character before it, if drawn.
      if (prev >= 0 && at == c) {
        memcpy(buf + len, string + k, (size_t)n);
        len += (size_t)n;
      }
    } else if (visible) {
      if (at != c) {
        len += (size_t)sprintf(buf + len, "\033[%d;%dH", row + 1, c + 1);
        Sgr(buf, &len, gc->foreground, bg);
      }
      memcpy(buf + len, string + k, (size_t)n);
      len += (size_t)n;
      at = c + w;
      prev = c;
    } else {
      prev = -1;
      at = -1;
    }
    c += w;
    k += n;
  }
  Out(buf, len);
  free(buf);
  XcUnlock();
  return 0;
}

// ---------------------------------------------------------------------------
// Atoms and properties.

static const char *const predefined_atoms[] = {
    NULL,           "PRIMARY",   "SECONDARY",       "ARC",
    "ATOM",         "BITMAP",    "CARDINAL",        "COLORMAP",
    "CURSOR",       "CUT_BUFFER0", "CUT_BUFFER1",   "CUT_BUFFER2",
    "CUT_BUFFER3",  "CUT_BUFFER4", "CUT_BUFFER5",   "CUT_BUFFER6",
    "CUT_BUFFER7",  "DRAWABLE",  "FONT",            "INTEGER",
    "PIXMAP",       "POINT",     "RECTANGLE",       "RESOURCE_MANAGER",
    "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING",
    "VISUALID",     "WINDOW",    "WM_COMMAND",      "WM_HINTS",
    "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME",
    "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE",
    "NORM_SPACE",   "MAX_SPACE", "END_SPACE",       "SUPERSCRIPT_X",
    "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",  "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT",
    "ITALIC_ANGLE", "X_HEIGHT",  "QUAD_WIDTH",      "WEIGHT",
    "POINT_SIZE",   "RESOLUTION", "COPYRIGHT",      "NOTICE",
    "FONT_NAME",    "FAMILY_NAME", "FULL_NAME",     "CAP_HEIGHT",
    "WM_CLASS",     "WM_TRANSIENT_FOR",
};

Atom XcInternAtom(const char *name) {
  for (size_t i = 1; i < sizeof(predefined_atoms) / sizeof(*predefined_atoms);
       ++i) {
    if (!strcmp(predefined_atoms[i], name)) return (Atom)i;
  }
  XcLock();
  Atom a = None;
  for (int i = 0; i < xc.shm->natoms; ++i) {
    if (!strcmp(xc.shm->atoms[i], name)) {
      a = (Atom)(XC_FIRST_ATOM + i);
      break;
    }
  }
  if (a == None && xc.shm->natoms < XC_MAX_ATOMS &&
      strlen(name) < XC_ATOM_NAME) {
    strcpy(xc.shm->atoms[xc.shm->natoms], name);
    a = (Atom)(XC_FIRST_ATOM + xc.shm->natoms++);
  }
  XcUnlock();
  return a;
}

Atom XInternAtom(Display *dpy, const char *name, Bool only_if_exists) {
  (void)dpy;
  if (only_if_exists) {
    for (size_t i = 1;
         i < sizeof(predefined_atoms) / sizeof(*predefined_atoms); ++i) {
      if (!strcmp(predefined_atoms[i], name)) return (Atom)i;
    }
    Atom a = None;
    XcLock();
    for (int i = 0; i < xc.shm->natoms; ++i) {
      if (!strcmp(xc.shm->atoms[i], name)) a = (Atom)(XC_FIRST_ATOM + i);
    }
    XcUnlock();
    return a;
  }
  return XcInternAtom(name);
}

char *XGetAtomName(Display *dpy, Atom atom) {
  (void)dpy;
  if (atom > 0 &&
      atom < sizeof(predefined_atoms) / sizeof(*predefined_atoms)) {
    return strdup(predefined_atoms[atom]);
  }
  char *name = NULL;
  XcLock();
  if (atom >= XC_FIRST_ATOM &&
      atom < (Atom)(XC_FIRST_ATOM + xc.shm->natoms)) {
    name = strdup(xc.shm->atoms[atom - XC_FIRST_ATOM]);
  }
  XcUnlock();
  return name;
}

int XChangeProperty(Display *dpy, Window w, Atom property, Atom type,
                    int format, int mode, const unsigned char *data,
                    int nelements) {
  (void)dpy;
  (void)w;
  (void)property;
  (void)type;
  (void)format;
  (void)mode;
  (void)data;
  (void)nelements;
  return 1;
}

int XFree(void *data) {
  free(data);
  return 1;
}

XClassHint *XAllocClassHint(void) { return calloc(1, sizeof(XClassHint)); }

Status XGetClassHint(Display *dpy, Window w, XClassHint *hint) {
  (void)dpy;
  (void)w;
  (void)hint;
  return 0;
}

Status XStringListToTextProperty(char **list, int count,
                                 XTextProperty *prop) {
  size_t n = 0;
  for (int i = 0; i < count; ++i) n += strlen(list[i]) + 1;
  prop->value = calloc(1, n + 1);
  if (prop->value == NULL) return 0;
  size_t at = 0;
  for (int i = 0; i < count; ++i) {
    size_t l = strlen(list[i]);
    memcpy(prop->value + at, list[i], l);
    at += l + 1;
  }
  prop->encoding = XA_STRING;
  prop->format = 8;
  prop->nitems = n ? n - 1 : 0;
  return 1;
}

void XSetWMProperties(Display *dpy, Window w, XTextProperty *window_name,
                      XTextProperty *icon_name, char **argv, int argc,
                      XSizeHints *normal_hints, XWMHints *wm_hints,
                      XClassHint *class_hints) {
  (void)dpy;
  (void)window_name;
  // xsecurelock names its windows (wm_properties.c); saver modules may draw
  // inside the one it names "saver" only, and not while the one it names
  // "auth" (the prompt) shows (saver.c).
  if (xc.is_main && class_hints != NULL && class_hints->res_name != NULL &&
      class_hints->res_class != NULL &&
      !strcmp(class_hints->res_class, "xsecurelock")) {
    XcLock();
    if (xc_saver_window == None && !strcmp(class_hints->res_name, "saver")) {
      xc_saver_window = xc.shm->saver_window = w;
    }
    if (xc_auth_window == None && !strcmp(class_hints->res_name, "auth")) {
      xc_auth_window = xc.shm->auth_window = w;
    }
    XcUnlock();
  }
  (void)icon_name;
  (void)argv;
  (void)argc;
  (void)normal_hints;
  (void)wm_hints;
}

Window XmuClientWindow(Display *dpy, Window w) {
  (void)dpy;
  return w;
}

// ---------------------------------------------------------------------------
// Grabs, focus, cursors, pixmaps, input methods, errors, screen saver.

/*! \brief The keyboard grab is the console lock: the lock VT in front, its
 * keyboard read in K_MEDIUMRAW mode, VT switching disabled. */
int XGrabKeyboard(Display *dpy, Window w, Bool owner_events,
                  int pointer_mode, int keyboard_mode, Time time) {
  (void)dpy;
  (void)w;
  (void)owner_events;
  (void)pointer_mode;
  (void)keyboard_mode;
  (void)time;
  if (!xc.is_main) return AlreadyGrabbed;
  return XcVtBringToFront() ? GrabSuccess : AlreadyGrabbed;
}

//! There is no pointer on the console to grab.
int XGrabPointer(Display *dpy, Window w, Bool owner_events,
                 unsigned int event_mask, int pointer_mode, int keyboard_mode,
                 Window confine_to, Cursor cursor, Time time) {
  (void)dpy;
  (void)w;
  (void)owner_events;
  (void)event_mask;
  (void)pointer_mode;
  (void)keyboard_mode;
  (void)confine_to;
  (void)cursor;
  (void)time;
  return GrabSuccess;
}

int XGrabServer(Display *dpy) {
  (void)dpy;
  return 1;
}

int XUngrabServer(Display *dpy) {
  (void)dpy;
  return 1;
}

int XGetInputFocus(Display *dpy, Window *focus, int *revert_to) {
  (void)dpy;
  *focus = None;
  *revert_to = RevertToNone;
  return 1;
}

int XSetInputFocus(Display *dpy, Window focus, int revert_to, Time time) {
  (void)dpy;
  (void)focus;
  (void)revert_to;
  (void)time;
  return 1;
}

Cursor XCreateFontCursor(Display *dpy, unsigned int shape) {
  (void)dpy;
  (void)shape;
  return (Cursor)next_local_id++;
}

Cursor XCreatePixmapCursor(Display *dpy, Pixmap source, Pixmap mask,
                           XColor *fg, XColor *bg, unsigned int x,
                           unsigned int y) {
  (void)dpy;
  (void)source;
  (void)mask;
  (void)fg;
  (void)bg;
  (void)x;
  (void)y;
  return (Cursor)next_local_id++;
}

int XFreeCursor(Display *dpy, Cursor cursor) {
  (void)dpy;
  (void)cursor;
  return 1;
}

int XUndefineCursor(Display *dpy, Window w) {
  (void)dpy;
  (void)w;
  return 1;
}

Pixmap XCreateBitmapFromData(Display *dpy, Drawable d, const char *data,
                             unsigned int width, unsigned int height) {
  (void)dpy;
  (void)d;
  (void)data;
  (void)width;
  (void)height;
  return (Pixmap)next_local_id++;
}

int XFreePixmap(Display *dpy, Pixmap pixmap) {
  (void)dpy;
  (void)pixmap;
  return 1;
}

struct _XIM {
  int unused;
};
struct _XIC {
  int unused;
};

XIM XOpenIM(Display *dpy, struct _XrmHashBucketRec *rdb, char *res_name,
            char *res_class) {
  (void)dpy;
  (void)rdb;
  (void)res_name;
  (void)res_class;
  static struct _XIM im;
  return &im;
}

XIC XCreateIC(XIM im, ...) {
  (void)im;
  static struct _XIC ic;
  return &ic;
}

XErrorHandler XSetErrorHandler(XErrorHandler handler) {
  static XErrorHandler current;
  XErrorHandler old = current;
  current = handler;
  return old;
}

int XGetErrorText(Display *dpy, int code, char *buffer, int length) {
  (void)dpy;
  snprintf(buffer, (size_t)length, "X error %d", code);
  return 0;
}

/*! \brief Screen saver activation blanks the console (TIOCL_BLANKSCREEN on
 * the lock VT, our controlling tty); reset unblanks it. */
int XForceScreenSaver(Display *dpy, int mode) {
  (void)dpy;
  if (!xc.is_main) return 1;
  char arg = mode == ScreenSaverActive ? TIOCL_BLANKSCREEN
                                       : TIOCL_UNBLANKSCREEN;
  if (ioctl(xc.vt_fd, TIOCLINUX, &arg) < 0) {
    LogErrno("TIOCLINUX blank/unblank");
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Keyboard bell (XSECURELOCK_AUTH_SOUNDS).

static int bell_pitch = 400, bell_duration = 100, bell_percent = 50;

int XGetKeyboardControl(Display *dpy, XKeyboardState *state) {
  (void)dpy;
  memset(state, 0, sizeof(*state));
  state->bell_pitch = (unsigned int)bell_pitch;
  state->bell_duration = (unsigned int)bell_duration;
  state->bell_percent = bell_percent;
  return 1;
}

int XChangeKeyboardControl(Display *dpy, unsigned long mask,
                           XKeyboardControl *values) {
  (void)dpy;
  if (mask & KBBellPitch) bell_pitch = values->bell_pitch;
  if (mask & KBBellDuration) bell_duration = values->bell_duration;
  if (mask & KBBellPercent) bell_percent = values->bell_percent;
  return 1;
}

int XBell(Display *dpy, int percent) {
  (void)dpy;
  (void)percent;
  if (bell_pitch <= 0) return 1;
  // KDMKTONE: duration in ms (high 16 bits), PIT counter (low 16 bits).
  unsigned long arg = ((unsigned long)bell_duration << 16) |
                      (unsigned long)(1193182 / bell_pitch);
  ioctl(xc.draw_fd, KDMKTONE, arg);
  return 1;
}

