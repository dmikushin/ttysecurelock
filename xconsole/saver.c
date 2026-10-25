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

/*! \brief The lock's end of the saver channel (see saver_proto.h).
 *
 * A thread of the main program: accepts saver modules, keeps the cells each
 * one drew, and paints them on the lock VT where its window is visible. It
 * holds the shared table's lock while painting, like every drawing process
 * of the lock. Saver modules get no fd of the lock VT: they cannot read the
 * keyboard, change the VT's modes or termios, or write terminal control
 * sequences; the cells they send are code points and colours, and only
 * printable one-cell characters are written.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../logging.h"
#include "saver_proto.h"
#include "xconsole_internal.h"

#define MAX_CONNS 8
#define TICK_MS 100

typedef struct {
  int fd;
  Window w;
  int cols, rows;
  XcGridCell *grid;
  uint64_t damage;
  int hidden;  // blanked while the prompt shows
} Conn;

// ---------------------------------------------------------------------------
// The rain font: the saver's glyphs on the lock VT while it shows.

typedef struct {
  struct console_font_op op;  // charcount, width, height, data
  size_t size;
  struct unipair *map;
  unsigned short nmap;
} VtFont;

static VtFont console_font;  // the lock VT's own, kept at XOpenDisplay
static VtFont rain_font;     // the console font with the saver's glyphs

// The console writes a wide character (kanji) into two cells; the rain's are
// one cell wide. Such characters are written as a private-use alias
// (U+E000 + k) that the rain font maps to the same glyph; GIO_UNIMAP's
// inverse (vcs) still names the character itself, the lower code point.
#define ALIAS_BASE 0xe000
static uint32_t alias_cp[XC_SAVER_MAX_GLYPHS];  // sorted
static unsigned nalias;

uint32_t XcRainAliasLocked(uint32_t cp) {
  if (!xc.shm->rain_font) return cp;
  unsigned lo = 0, hi = nalias;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    if (alias_cp[mid] < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < nalias && alias_cp[lo] == cp ? ALIAS_BASE + lo : cp;
}

//! Whether the saver may keep cp in a cell: printable in one cell, or one
//! of its own glyphs.
static int Keepable(uint32_t cp) {
  if (XcPrintableCell(cp)) return 1;
  for (unsigned k = 0; k < nalias; ++k) {
    if (alias_cp[k] == cp) return 1;
  }
  return 0;
}

static size_t FontSize(unsigned count, unsigned width, unsigned height) {
  return (size_t)count * height * ((width + 7) / 8);
}

void XcSaverSaveFont(void) {
  VtFont *f = &console_font;
  unsigned char *data = malloc(FontSize(512, 64, 128));
  struct unipair *map = malloc(sizeof(*map) * 65535);
  if (data == NULL || map == NULL) {
    free(data);
    free(map);
    return;
  }
  // GET_TALL lays the glyphs out with the height asked for: ask for the
  // font's size first, then for its glyphs at that height.
  memset(&f->op, 0, sizeof(f->op));
  f->op.op = KD_FONT_OP_GET_TALL;
  f->op.width = 64;
  f->op.height = 128;
  f->op.charcount = 512;
  f->op.data = NULL;
  int sized = ioctl(xc.vt_fd, KDFONTOP, &f->op) == 0;
  f->op.op = KD_FONT_OP_GET_TALL;
  f->op.charcount = 512;
  f->op.data = data;
  struct unimapdesc ud = {65535, map};
  if (!sized || ioctl(xc.vt_fd, KDFONTOP, &f->op) < 0 ||
      ioctl(xc.vt_fd, GIO_UNIMAP, &ud) < 0) {
    LogErrno("xconsole: reading the console font (no rain font)");
    free(data);
    free(map);
    f->op.data = NULL;
    return;
  }
  f->size = FontSize(f->op.charcount, f->op.width, f->op.height);
  f->map = map;
  f->nmap = ud.entry_ct;
}

static int LoadFont(const VtFont *f) {
  struct console_font_op op = f->op;
  op.op = KD_FONT_OP_SET_TALL;
  op.flags = 0;
  struct unimapinit init = {0, 0, 0};
  struct unimapdesc ud = {f->nmap, f->map};
  if (ioctl(xc.vt_fd, KDFONTOP, &op) < 0 ||
      ioctl(xc.vt_fd, PIO_UNIMAPCLR, &init) < 0 ||
      ioctl(xc.vt_fd, PIO_UNIMAP, &ud) < 0) {
    LogErrno("xconsole: loading a font on the lock VT");
    return 0;
  }
  return 1;
}

void XcRainFontLocked(int on) {
  if (xc.shm->rain_font == on) return;
  if (on && rain_font.op.data == NULL) return;
  if (!on) {
    // The console font back: failing that the prompt would show the rain's
    // glyphs, so retry; the flag says what the VT has.
    for (int tries = 0; tries < 3; ++tries) {
      if (LoadFont(&console_font)) {
        xc.shm->rain_font = 0;
        return;
      }
    }
    return;
  }
  if (LoadFont(&rain_font)) {
    xc.shm->rain_font = 1;
  } else {
    LoadFont(&console_font);
  }
}

void XcRainFontOffForPrompt(void) {
  if (!xc.shm->rain_font) return;
  if (xc.is_main) {
    XcLock();
    XcRainFontLocked(0);
    XcUnlock();
    return;
  }
  // Asked again every 100 ms (a datagram may not get through), for up to
  // 2 s: the thread answers within a tick.
  uint32_t m = XC_SAVER_PROMPT;
  for (int i = 0; i < 1000 && xc.shm->rain_font; ++i) {
    if (i % 50 == 0) {
      send(xc.saver_fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
  }
  if (xc.shm->rain_font) Log("xconsole: the rain font is still loaded");
}

/*! \brief The rain font: the console font, with the saver's glyphs in slots
 * that no ASCII character uses (and in new slots up to 512), and a Unicode
 * map sending the saver's code points there. While it is loaded only the
 * rain shows; the prompt gets the console font back first. */
static int MakeRainFont(const XcSaverFont *h, const uint32_t *cps,
                        const unsigned char *glyphs) {
  const VtFont *cf = &console_font;
  if (cf->op.data == NULL || h->width != cf->op.width ||
      h->height != cf->op.height || h->n == 0 ||
      h->n > XC_SAVER_MAX_GLYPHS) {
    return 0;
  }
  static unsigned char ascii[512];
  memset(ascii, 0, sizeof(ascii));
  for (unsigned k = 0; k < cf->nmap; ++k) {
    if (cf->map[k].unicode < 0x80 && cf->map[k].fontpos < 512) {
      ascii[cf->map[k].fontpos] = 1;
    }
  }
  ascii[0] = 1;
  static unsigned short slot[XC_SAVER_MAX_GLYPHS];
  static unsigned char taken[512];
  memset(taken, 0, sizeof(taken));
  unsigned n = 0;
  for (int pos = 511; pos >= 0 && n < h->n; --pos) {
    if ((unsigned)pos >= cf->op.charcount || !ascii[pos]) {
      slot[n++] = (unsigned short)pos;
      taken[pos] = 1;
    }
  }
  if (n < h->n) return 0;
  for (unsigned k = 0; k < h->n; ++k) {
    if (cps[k] < 0x80 || cps[k] > 0xffff ||
        (cps[k] >= ALIAS_BASE && cps[k] < ALIAS_BASE + XC_SAVER_MAX_GLYPHS)) {
      return 0;
    }
  }
  size_t glyph = FontSize(1, h->width, h->height);
  unsigned char *data = calloc(1, FontSize(512, h->width, h->height));
  struct unipair *map = malloc(sizeof(*map) * ((size_t)cf->nmap + 2 * h->n));
  if (data == NULL || map == NULL) {
    free(data);
    free(map);
    return 0;
  }
  memcpy(data, cf->op.data, cf->size);
  for (unsigned k = 0; k < h->n; ++k) {
    memcpy(data + slot[k] * glyph, glyphs + k * glyph, glyph);
  }
  unsigned m = 0;
  for (unsigned k = 0; k < cf->nmap; ++k) {
    if (cf->map[k].fontpos < 512 && taken[cf->map[k].fontpos]) continue;
    int dup = 0;
    for (unsigned j = 0; j < h->n && !dup; ++j) {
      dup = cf->map[k].unicode == cps[j];
    }
    if (!dup) map[m++] = cf->map[k];
  }
  for (unsigned k = 0; k < h->n; ++k) {
    map[m].unicode = (unsigned short)cps[k];
    map[m++].fontpos = slot[k];
  }
  // Aliases for the wide ones, in code point order.
  nalias = 0;
  for (unsigned k = 0; k < h->n; ++k) {
    if (!XcPrintableCell(cps[k])) {
      unsigned at = nalias++;
      while (at > 0 && alias_cp[at - 1] > cps[k]) {
        alias_cp[at] = alias_cp[at - 1];
        --at;
      }
      alias_cp[at] = cps[k];
    }
  }
  for (unsigned a = 0; a < nalias; ++a) {
    for (unsigned k = 0; k < h->n; ++k) {
      if (cps[k] == alias_cp[a]) {
        map[m].unicode = (unsigned short)(ALIAS_BASE + a);
        map[m++].fontpos = slot[k];
        break;
      }
    }
  }
  free(rain_font.op.data);
  free(rain_font.map);
  rain_font.op = cf->op;
  rain_font.op.charcount = 512;
  rain_font.op.data = data;
  rain_font.size = FontSize(512, h->width, h->height);
  rain_font.map = map;
  rain_font.nmap = (unsigned short)m;
  return 1;
}

// ---------------------------------------------------------------------------

static Conn conns[MAX_CONNS];
static int nconns;
static int stopping;  // with the table locked
static pthread_t thread;

//! The parent of a process, from /proc (0 if unknown).
static pid_t ParentOf(pid_t pid) {
  char path[64], buf[512];
  snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = 0;
  char *p = strrchr(buf, ')');  // the command name may contain anything
  int ppid;
  if (p == NULL || sscanf(p + 1, " %*c %d", &ppid) != 1) return 0;
  return (pid_t)ppid;
}

//! Whether w lies in xsecurelock's saver window (locked).
static int InSaverWindow(Window w) {
  if (xc_saver_window == None) return 0;
  // Bounded: the table is shared with the helpers, a parent cycle must not
  // hang the lock.
  XcWindow *p = XcFindWindow(w);
  for (int depth = 0; p != NULL && depth < XC_MAX_WINDOWS;
       ++depth, p = XcFindWindow(p->parent)) {
    if (p->id == xc_saver_window) return 1;
    if (p->id == XC_ROOT_WINDOW) break;
  }
  return 0;
}

static void CloseConn(int i) {
  close(conns[i].fd);
  free(conns[i].grid);
  conns[i] = conns[--nconns];
}

static int SendSize(Conn *c) {
  XcSaverSize m = {XC_SAVER_SIZE, (uint32_t)c->cols, (uint32_t)c->rows,
                   (uint32_t)xc.shm->cell_w, (uint32_t)xc.shm->cell_h};
  return send(c->fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT) ==
         (ssize_t)sizeof(m);
}

//! Follow the window: gone or moved out of the saver window -> 0; resized
//! -> new grid and size sent; the screen repainted under it -> repaint.
static int UpdateConn(Conn *c) {
  XcWindow *win = XcFindWindow(c->w);
  if (win == NULL || !InSaverWindow(c->w)) return 0;
  int c0, r0, c1, r1, cols = 0, rows = 0;
  if (XcWindowCellsLocked(win, &c0, &r0, &c1, &r1)) {
    cols = c1 - c0;
    rows = r1 - r0;
  }
  if (cols != c->cols || rows != c->rows || c->grid == NULL) {
    XcGridCell *g = calloc((size_t)(cols * rows) + 1, sizeof(*g));
    if (g == NULL) return 0;
    for (int k = 0; k < cols * rows; ++k) {
      g[k].cp = ' ';
      g[k].fg = 7;
    }
    for (int r = 0; r < rows && r < c->rows; ++r) {
      for (int k = 0; k < cols && k < c->cols; ++k) {
        g[r * cols + k] = c->grid[r * c->cols + k];
      }
    }
    free(c->grid);
    c->grid = g;
    c->cols = cols;
    c->rows = rows;
    if (!SendSize(c)) return 0;
  }
  // The console's rule (not X's, where the saver goes on around the prompt
  // box): while the prompt shows, the saver window is blank; the saver runs
  // on, the lock keeps its cells and shows them when the prompt is gone.
  XcWindow *auth = XcFindWindow(xc_auth_window);
  int hide = auth != NULL && XcViewable(auth);
  if (hide) {
    XcRainFontLocked(0);
    XcSetRampLocked(0);
  }
  if (hide && !c->hidden) {
    XcGridCell *blank = calloc((size_t)(c->cols * c->rows) + 1, sizeof(*blank));
    if (blank == NULL) return 0;
    for (int k = 0; k < c->cols * c->rows; ++k) {
      blank[k].cp = ' ';
      blank[k].fg = 7;
      blank[k].bg = (uint8_t)(XcWindowBackground(win) & 15);
    }
    XcPaintGridLocked(win, c->cols, c->rows, blank, NULL, -1);
    free(blank);
  }
  if (!hide && (c->hidden || c->damage != xc.shm->damage ||
                !xc.shm->ramp_on ||
                (rain_font.op.data != NULL && !xc.shm->rain_font))) {
    XcRainFontLocked(1);
    XcSetRampLocked(1);
    XcPaintGridLocked(win, c->cols, c->rows, c->grid, NULL, -1);
  }
  c->hidden = hide;
  c->damage = xc.shm->damage;
  return 1;
}

static void Accept(int srv) {
  XcSaverHello hello;
  struct iovec iov = {&hello, sizeof(hello)};
  union {
    char buf[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(4 * sizeof(int))];
    struct cmsghdr align;
  } u;
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);
  ssize_t n = recvmsg(srv, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  if (n < 0) return;
  int fd = -1, nfds = 0;
  int fds[4];
  struct ucred cred = {0, (uid_t)-1, (gid_t)-1};
  int have_cred = 0;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL;
       c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET) continue;
    if (c->cmsg_type == SCM_RIGHTS) {
      nfds = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      if (nfds > 4) nfds = 4;
      memcpy(fds, CMSG_DATA(c), (size_t)nfds * sizeof(int));
    } else if (c->cmsg_type == SCM_CREDENTIALS) {
      memcpy(&cred, CMSG_DATA(c), sizeof(cred));
      have_cred = 1;
    }
  }
  if (nfds >= 1) fd = fds[0];
  for (int k = 1; k < nfds; ++k) close(fds[k]);
  if (n == (ssize_t)sizeof(uint32_t) && hello.magic == XC_SAVER_PROMPT) {
    if (fd >= 0) close(fd);
    XcLock();
    XcRainFontLocked(0);
    XcUnlock();
    return;
  }
  int type = 0;
  socklen_t tl = sizeof(type);
  pid_t parent = have_cred ? ParentOf(cred.pid) : 0;
  if (fd < 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      n != (ssize_t)sizeof(hello) || hello.magic != XC_SAVER_MAGIC ||
      hello.version != XC_SAVER_VERSION || !have_cred ||
      cred.uid != getuid() || parent == 0 ||
      getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) < 0 ||
      type != SOCK_SEQPACKET || nconns == MAX_CONNS) {
    Log("xconsole: refused a saver connection (pid %d)", (int)cred.pid);
    if (fd >= 0) close(fd);
    return;
  }
  XcLock();
  XcWindow *win = XcFindWindow((Window)hello.window);
  int ok = !stopping && win != NULL && InSaverWindow(win->id) &&
           win->owner == parent;
  if (ok) {
    Conn *c = &conns[nconns++];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->w = win->id;
    c->damage = xc.shm->damage - 1;  // paint at once (blank)
    if (!UpdateConn(c)) {
      free(c->grid);
      --nconns;
      ok = 0;
    }
  }
  XcUnlock();
  if (!ok) {
    Log("xconsole: saver (pid %d) may not draw into window %#llx",
        (int)cred.pid, (unsigned long long)hello.window);
    close(fd);
  }
}

//! Cells from a saver; 0 if it broke the protocol or its window is gone.
static int Receive(int i, char *buf) {
  ssize_t n = recv(conns[i].fd, buf, XC_SAVER_MAX_MSG + 1, MSG_DONTWAIT);
  if (n < 0) return errno == EAGAIN || errno == EINTR;
  if (n < (ssize_t)sizeof(XcSaverCells) || n > XC_SAVER_MAX_MSG) return 0;
  XcSaverCells h;
  memcpy(&h, buf, sizeof(h));
  if (h.type == XC_SAVER_FONT && n >= (ssize_t)sizeof(XcSaverFont)) {
    XcSaverFont f;
    memcpy(&f, buf, sizeof(f));
    if (f.n > XC_SAVER_MAX_GLYPHS || f.width == 0 || f.width > 64 ||
        f.height == 0 || f.height > 128 ||
        (size_t)n != sizeof(f) + f.n * (sizeof(uint32_t) +
                                         FontSize(1, f.width, f.height))) {
      return 0;
    }
    XcLock();
    int ok = !stopping && MakeRainFont(&f, (const uint32_t *)(buf + sizeof(f)),
                                       (const unsigned char *)buf + sizeof(f) +
                                           f.n * sizeof(uint32_t));
    if (ok && xc.shm->rain_font) {  // a new font while one shows
      xc.shm->rain_font = 0;
      XcRainFontLocked(1);
    }
    XcUnlock();
    XcSaverFontOk r = {XC_SAVER_FONT_OK, (uint32_t)ok};
    return send(conns[i].fd, &r, sizeof(r), MSG_NOSIGNAL | MSG_DONTWAIT) ==
           (ssize_t)sizeof(r);
  }
  if (h.type != XC_SAVER_CELLS || h.n > XC_SAVER_MAX_CELLS ||
      (size_t)n != sizeof(h) + h.n * sizeof(XcSaverCell)) {
    return 0;
  }
  static uint32_t list[XC_SAVER_MAX_CELLS];
  XcLock();
  Conn *c = &conns[i];
  int ok = !stopping && UpdateConn(c);
  int m = 0;
  for (uint32_t k = 0; ok && k < h.n; ++k) {
    XcSaverCell cell;
    memcpy(&cell, buf + sizeof(h) + k * sizeof(cell), sizeof(cell));
    if (cell.col >= c->cols || cell.row >= c->rows) continue;
    uint32_t idx = (uint32_t)cell.row * (uint32_t)c->cols + cell.col;
    XcGridCell *g = &c->grid[idx];
    g->cp = Keepable(cell.cp) ? cell.cp : ' ';
    g->fg = cell.fg < XC_SAVER_RAMP + XC_SAVER_RAMP_LEVELS ? cell.fg : 7;
    g->bg = cell.bg < XC_SAVER_RAMP + XC_SAVER_RAMP_LEVELS ? cell.bg : 0;
    list[m++] = idx;
  }
  if (ok && m > 0 && !c->hidden) {
    XcPaintGridLocked(XcFindWindow(c->w), c->cols, c->rows, c->grid, list, m);
  }
  XcUnlock();
  return ok;
}

static void *Serve(void *arg) {
  (void)arg;
  char *buf = malloc(XC_SAVER_MAX_MSG + 1);
  if (buf == NULL) return NULL;
  for (;;) {
    struct pollfd pfd[MAX_CONNS + 1];
    pfd[0].fd = xc.saver_fd;
    pfd[0].events = POLLIN;
    for (int i = 0; i < nconns; ++i) {
      pfd[i + 1].fd = conns[i].fd;
      pfd[i + 1].events = POLLIN;
    }
    int k = nconns;
    if (poll(pfd, (nfds_t)k + 1, TICK_MS) > 0) {
      for (int i = k - 1; i >= 0; --i) {
        if (pfd[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
          if (!Receive(i, buf)) CloseConn(i);
        }
      }
      if (pfd[0].revents & POLLIN) Accept(xc.saver_fd);
    }
    XcLock();
    int stop = stopping;
    for (int i = nconns - 1; !stop && i >= 0; --i) {
      if (!UpdateConn(&conns[i])) CloseConn(i);
    }
    XcUnlock();
    if (stop) break;
  }
  free(buf);
  return NULL;
}

int XcSaverServerStart(void) {
  int on = 1;
  if (setsockopt(xc.saver_fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0) {
    LogErrno("SO_PASSCRED");
    return 0;
  }
  // Signals stay with xsecurelock's thread.
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  int r = pthread_create(&thread, NULL, Serve, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (r != 0) {
    errno = r;
    LogErrno("pthread_create");
    return 0;
  }
  return 1;
}

void XcSaverServerStop(void) {
  XcLock();
  stopping = 1;
  XcRainFontLocked(0);
  XcSetRampLocked(0);
  XcUnlock();
}
