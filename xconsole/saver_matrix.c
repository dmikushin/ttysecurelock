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

/*! \brief saver_matrix: the "Matrix" digital rain of cmatrix as an
 * xsecurelock saver module on the console.
 *
 * Usage (as any saver: saver_multiplex runs it with $XSCREENSAVER_WINDOW):
 * XSECURELOCK_SAVER=saver_matrix. cmatrix (a separate GPL-3 program,
 * https://github.com/abishekvashok/cmatrix, run as an external process, not
 * linked) runs on a pseudo-terminal of its own; libvterm (MIT) keeps that
 * terminal's screen, and the changed cells go to the lock through the saver
 * channel (xconsole/saver_proto.h), which paints them in the saver window.
 * cmatrix never gets the lock VT: no keyboard, no VT modes, no termios, no
 * control sequences reach the console; its stdin is the pseudo-terminal,
 * where nothing is ever typed.
 *
 * XSECURELOCK_MATRIX_OPTIONS: cmatrix's options (default "-ab -u 5"); its
 * colour options have no effect, the lock's green ramp colours the rain.
 *
 * Characters: the film's code is mirrored half-width katakana, digits and a
 * few kanji. cmatrix draws ASCII (its -c mode draws CJK punctuation, not
 * katakana); saver_matrix keeps its digits and shows its other characters as
 * katakana (7 in 10) or kanji, chosen per character and cell, so a character
 * that changes in place changes, one that stays stays. The glyphs come from
 * RAIN_FONT (a subset of Noto Sans CJK JP, scripts/gen-rain-font.py),
 * rendered with FreeType at the console's cell size, mirrored, half-width
 * katakana at their size and kanji squeezed into one cell; the lock loads
 * them on its VT while the rain shows (saver_proto.h, XC_SAVER_FONT). If the
 * font is missing or the lock refuses it, the rain stays ASCII.
 *
 * `saver_matrix --sheet WxH FILE.pgm [FONT]`: the glyphs at a cell size, as
 * a PGM image (to look at them).
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vterm.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "saver_proto.h"

#define DEFAULT_OPTIONS "-ab -u 5"

static int conn = -1;
static int cell_w, cell_h;    // the console font's cell, from the lock
static int font_w, font_h;    // the cell our glyphs were sent for
static int font_ok;           // the lock shows our glyphs
static FT_Face face;
static uint32_t kata[64], kanji[512];
static int nkata, nkanji;
static int cols, rows;
static unsigned char *dirty;
static VTerm *vt;
static VTermScreen *screen;
static pid_t child;

static void Die(const char *what) {
  fprintf(stderr, "saver_matrix: %s: %s\n", what, strerror(errno));
  if (child > 0) kill(child, SIGTERM);
  sleep(2);  // as upstream's saver_child after a failed exec: no busy loop
  exit(1);
}

static int Damage(VTermRect r, void *user) {
  (void)user;
  for (int y = r.start_row; y < r.end_row && y < rows; ++y) {
    for (int x = r.start_col; x < r.end_col && x < cols; ++x) {
      dirty[y * cols + x] = 1;
    }
  }
  return 1;
}

static const VTermScreenCallbacks callbacks = {.damage = Damage};

//! A libvterm colour as a console colour (0-15).
static uint8_t ConsoleColor(const VTermColor *c, int fg) {
  if (fg ? VTERM_COLOR_IS_DEFAULT_FG(c) : VTERM_COLOR_IS_DEFAULT_BG(c)) {
    return fg ? 7 : 0;
  }
  if (VTERM_COLOR_IS_INDEXED(c)) {
    return c->indexed.idx < 16 ? c->indexed.idx : (fg ? 7 : 0);
  }
  // RGB: the VGA colour with the nearest components (cmatrix uses none).
  int bright = c->rgb.red > 170 || c->rgb.green > 170 || c->rgb.blue > 170;
  return (uint8_t)((c->rgb.red > 85) | (c->rgb.green > 85) << 1 |
                   (c->rgb.blue > 85) << 2 | bright << 3);
}

/*! \brief What the lock shows of a cell. cmatrix's own colours are not
 * used: the console's palette may not tell its greens apart (Nord: green and
 * bright green are one colour). Each trail is coloured by the lock's green
 * ramp instead: its newest cell (the head) brightest, the older cells above
 * it darker and darker up to the oldest, at the trail's top. A cell that
 * changes in place (cmatrix's glitter) keeps its age. */
typedef struct {
  uint32_t cp;
  uint32_t birth;  // the frame its character appeared on a blank cell
  uint8_t bg;
  uint8_t lit;
  uint8_t level;  // ramp level, 0 = head
  uint8_t send;
} Cell;

static Cell *cells;
static unsigned char *touched;  // per column
static uint32_t frame;

//! Ramp levels of a column: per trail (run of lit cells), the newest cell
//! is the head (level 0); the others get 1 to the darkest by their distance
//! from it, spread over the trail's length.
static void Levels(int col) {
  for (int r = 0; r < rows;) {
    if (!cells[r * cols + col].lit) {
      ++r;
      continue;
    }
    int start = r;
    while (r < rows && cells[r * cols + col].lit) ++r;
    int len = r - start, head = start;
    for (int i = start; i < r; ++i) {
      if (cells[i * cols + col].birth >= cells[head * cols + col].birth) head = i;
    }
    for (int i = start; i < r; ++i) {
      int d = i > head ? i - head : head - i;
      int level = d == 0 ? 0
                         : 1 + (d - 1) * (XC_SAVER_RAMP_LEVELS - 1) /
                                   (len > 1 ? len - 1 : 1);
      if (level >= XC_SAVER_RAMP_LEVELS) level = XC_SAVER_RAMP_LEVELS - 1;
      Cell *c = &cells[i * cols + col];
      if (c->level != level) {
        c->level = (uint8_t)level;
        c->send = 1;
      }
    }
  }
}

//! The character shown for cmatrix's: digits as they are, other characters
//! as katakana or kanji while the lock shows our glyphs.
static uint32_t Shown(const Cell *c, int col) {
  if (!font_ok || !c->lit || c->cp >= 0x80 || (c->cp >= '0' && c->cp <= '9') ||
      nkata == 0) {
    return c->cp;
  }
  uint32_t h = c->cp * 2654435761u ^ (uint32_t)col * 40503u ^ c->birth * 9973u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  if (nkanji > 0 && h % 10 >= 7) return kanji[(h / 10) % (uint32_t)nkanji];
  return kata[(h / 10) % (uint32_t)nkata];
}

//! A glyph of the rain: mirrored like the film's, half-width katakana at
//! their size, kanji squeezed to the cell's width; centred. 1 bit per pixel,
//! rows of (w + 7) / 8 bytes, the leftmost pixel in the top bit.
static int RenderGlyph(uint32_t cp, int w, int h, unsigned char *out) {
  int pitch = (w + 7) / 8;
  memset(out, 0, (size_t)(pitch * h));
  int half = cp >= 0xff61 && cp <= 0xff9f;
  int ppem = half ? 2 * w : (3 * h) / 4;
  if (!half && ppem > 2 * w) ppem = 2 * w;
  double sx = half ? 1.0 : (double)w / ppem;
  FT_Matrix m = {(FT_Fixed)(-sx * 65536), 0, 0, 0x10000};  // mirrored
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)ppem) ||
      (FT_Set_Transform(face, &m, NULL), 0) ||
      FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP)) {
    return 0;
  }
  FT_Bitmap *b = &face->glyph->bitmap;
  int x0 = (w - (int)b->width) / 2, y0 = (h - (int)b->rows) / 2;
  int lit = 0;
  for (int y = 0; y < (int)b->rows; ++y) {
    for (int x = 0; x < (int)b->width; ++x) {
      int X = x0 + x, Y = y0 + y;
      if (X < 0 || X >= w || Y < 0 || Y >= h) continue;
      if (b->buffer[y * b->pitch + x] >= 112) {
        out[Y * pitch + X / 8] |= (unsigned char)(0x80 >> (X % 8));
        ++lit;
      }
    }
  }
  return lit > 0;
}

static void OpenFace(const char *path) {
  static FT_Library lib;
  if (FT_Init_FreeType(&lib) || FT_New_Face(lib, path, 0, &face)) {
    fprintf(stderr, "saver_matrix: no rain font %s: ASCII rain\n", path);
    face = NULL;
    return;
  }
  FT_UInt gi;
  for (FT_ULong cp = FT_Get_First_Char(face, &gi); gi != 0;
       cp = FT_Get_Next_Char(face, cp, &gi)) {
    if (cp >= 0xff61 && cp <= 0xff9f) {
      if (nkata < 64) kata[nkata++] = (uint32_t)cp;
    } else if (cp >= 0x80 && cp <= 0xffff && nkanji < 512 &&
               nkata + nkanji < XC_SAVER_MAX_GLYPHS - 64) {
      kanji[nkanji++] = (uint32_t)cp;
    }
  }
}

//! Our glyphs to the lock, for its cell size.
static void SendFont(void) {
  if (face == NULL || cell_w <= 0 || cell_h <= 0 || cell_w > 64 ||
      cell_h > 128 || (cell_w == font_w && cell_h == font_h)) {
    return;
  }
  font_w = cell_w;
  font_h = cell_h;
  font_ok = 0;
  size_t glyph = (size_t)((cell_w + 7) / 8 * cell_h);
  uint32_t n = 0;
  static uint32_t cps[XC_SAVER_MAX_GLYPHS];
  static unsigned char glyphs[XC_SAVER_MAX_GLYPHS * 8 * 128];
  for (int k = 0; k < nkata + nkanji && n < XC_SAVER_MAX_GLYPHS; ++k) {
    uint32_t cp = k < nkata ? kata[k] : kanji[k - nkata];
    if (RenderGlyph(cp, cell_w, cell_h, glyphs + n * glyph)) cps[n++] = cp;
  }
  XcSaverFont h = {XC_SAVER_FONT, n, (uint32_t)cell_w, (uint32_t)cell_h};
  size_t len = sizeof(h) + n * sizeof(uint32_t) + n * glyph;
  char *buf = malloc(len);
  if (buf == NULL) return;
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(h), cps, n * sizeof(uint32_t));
  memcpy(buf + sizeof(h) + n * sizeof(uint32_t), glyphs, n * glyph);
  int sz = 4 * XC_SAVER_MAX_MSG;
  setsockopt(conn, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
  if (send(conn, buf, len, MSG_NOSIGNAL) != (ssize_t)len) {
    fprintf(stderr, "saver_matrix: sending the rain font: %s\n",
            strerror(errno));
  }
  free(buf);
}

static void SendCells(void) {
  static char buf[XC_SAVER_MAX_MSG];
  ++frame;
  memset(touched, 0, (size_t)cols);
  for (int idx = 0; idx < cols * rows; ++idx) {
    if (!dirty[idx]) continue;
    dirty[idx] = 0;
    VTermPos pos = {idx / cols, idx % cols};
    VTermScreenCell vc;
    uint32_t cp = ' ';
    uint8_t bg = 0;
    if (vterm_screen_get_cell(screen, pos, &vc)) {
      if (vc.chars[0] != 0 && vc.width == 1) cp = vc.chars[0];
      bg = ConsoleColor(vc.attrs.reverse ? &vc.fg : &vc.bg, vc.attrs.reverse);
    }
    Cell *c = &cells[idx];
    uint8_t lit = cp != ' ';
    if (lit && !c->lit) c->birth = frame;
    c->cp = cp;
    c->bg = bg;
    c->lit = lit;
    c->send = 1;
    touched[pos.col] = 1;
  }
  for (int col = 0; col < cols; ++col) {
    if (touched[col]) Levels(col);
  }
  XcSaverCells h = {XC_SAVER_CELLS, 0};
  for (int idx = 0; idx <= cols * rows; ++idx) {
    if (idx < cols * rows && cells[idx].send) {
      Cell *c = &cells[idx];
      c->send = 0;
      XcSaverCell cell;
      memset(&cell, 0, sizeof(cell));
      cell.col = (uint16_t)(idx % cols);
      cell.row = (uint16_t)(idx / cols);
      cell.cp = Shown(c, idx % cols);
      cell.fg = c->lit ? (uint8_t)(XC_SAVER_RAMP + c->level) : 7;
      cell.bg = c->bg;
      memcpy(buf + sizeof(h) + h.n * sizeof(cell), &cell, sizeof(cell));
      ++h.n;
    }
    if (h.n == XC_SAVER_MAX_CELLS || (idx == cols * rows && h.n > 0)) {
      memcpy(buf, &h, sizeof(h));
      size_t len = sizeof(h) + h.n * sizeof(XcSaverCell);
      if (send(conn, buf, len, MSG_NOSIGNAL) != (ssize_t)len) Die("send");
      h.n = 0;
    }
  }
}

static char *argv_cmatrix[32];

/*! \brief Start cmatrix on a new pseudo-terminal of the current size;
 * returns its master. Run again on every size change: cmatrix as packaged
 * (2.0, Arch) does not grow its rain on SIGWINCH (rows beyond the old size
 * stay empty), so a new one is started instead. */
static int Spawn(void) {
  int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
    Die("pseudo-terminal");
  }
  const char *slave = ptsname(master);
  if (slave == NULL) Die("ptsname");
  struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
  ioctl(master, TIOCSWINSZ, &ws);
  child = fork();
  if (child < 0) Die("fork");
  if (child == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    setsid();
    int fd = open(slave, O_RDWR);  // its controlling terminal
    if (fd < 0) _exit(127);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    setenv("TERM", "linux", 1);
    execvp("cmatrix", argv_cmatrix);
    _exit(127);
  }
  fprintf(stderr, "saver_matrix: pid %d started cmatrix pid %d (%d x %d)\n",
          (int)getpid(), (int)child, cols, rows);
  return master;
}

static void Stop(int master) {
  close(master);
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    child = 0;
  }
}

static void Resize(int c, int r) {
  if (c < 1) c = 1;
  if (r < 1) r = 1;
  fprintf(stderr, "saver_matrix: %d x %d cells\n", c, r);
  free(dirty);
  free(cells);
  free(touched);
  dirty = calloc((size_t)(c * r), 1);
  cells = calloc((size_t)(c * r), sizeof(*cells));
  touched = calloc((size_t)c, 1);
  if (dirty == NULL || cells == NULL || touched == NULL) Die("calloc");
  cols = c;
  rows = r;
  vterm_set_size(vt, rows, cols);
  vterm_screen_reset(screen, 1);
  memset(dirty, 1, (size_t)(cols * rows));
}

//! A message from the lock: 1 for a size (in *c, *r), 2 for the answer to
//! our font, 0 if none came in time.
static int ReadMessage(int timeout_ms, int *c, int *r) {
  struct pollfd p = {conn, POLLIN, 0};
  if (poll(&p, 1, timeout_ms) <= 0) return 0;
  union {
    XcSaverSize size;
    XcSaverFontOk ok;
  } m;
  ssize_t n = recv(conn, &m, sizeof(m), 0);
  if (n == (ssize_t)sizeof(m.ok) && m.ok.type == XC_SAVER_FONT_OK) {
    font_ok = m.ok.ok != 0;
    if (!font_ok) fprintf(stderr, "saver_matrix: the lock refused the font\n");
    return 2;
  }
  if (n != (ssize_t)sizeof(m.size) || m.size.type != XC_SAVER_SIZE ||
      m.size.cols > 4096 || m.size.rows > 4096) {
    errno = EPROTO;
    Die("lock closed the saver channel");
  }
  *c = (int)m.size.cols;
  *r = (int)m.size.rows;
  cell_w = (int)m.size.cell_w;
  cell_h = (int)m.size.cell_h;
  return 1;
}

//! --sheet WxH FILE: the glyphs at a cell size as a PGM image.
static int Sheet(const char *size, const char *file, const char *font) {
  int w, h;
  if (sscanf(size, "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 64 ||
      h > 128) {
    return 2;
  }
  OpenFace(font);
  if (face == NULL) return 1;
  int n = nkata + nkanji, per = 32, gap = 2;
  int W = per * (w + gap) + gap, H = ((n + per - 1) / per) * (h + gap) + gap;
  unsigned char *img = calloc((size_t)(W * H), 1);
  unsigned char *g = malloc((size_t)((w + 7) / 8 * h));
  for (int k = 0; k < n; ++k) {
    uint32_t cp = k < nkata ? kata[k] : kanji[k - nkata];
    RenderGlyph(cp, w, h, g);
    int ox = gap + (k % per) * (w + gap), oy = gap + (k / per) * (h + gap);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        img[(oy + y) * W + ox + x] =
            (g[y * ((w + 7) / 8) + x / 8] & (0x80 >> (x % 8))) ? 0 : 255;
      }
    }
  }
  FILE *f = fopen(file, "wb");
  if (f == NULL) return 1;
  fprintf(f, "P5\n%d %d\n255\n", W, H);
  fwrite(img, 1, (size_t)(W * H), f);
  fclose(f);
  printf("%d glyphs (%d katakana, %d kanji) at %dx%d\n", n, nkata, nkanji, w,
         h);
  return 0;
}

static void Connect(void) {
  const char *display = getenv("DISPLAY");
  const char *window = getenv("XSCREENSAVER_WINDOW");
  int a, b, chan;
  if (display == NULL || window == NULL ||
      sscanf(display, ":xconsole:%d:%d:%d", &a, &b, &chan) != 3) {
    errno = EINVAL;
    Die("not run by ttysecurelock ($DISPLAY, $XSCREENSAVER_WINDOW)");
  }
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
    Die("socketpair");
  }
  XcSaverHello hello = {XC_SAVER_MAGIC, XC_SAVER_VERSION,
                        strtoull(window, NULL, 0)};
  struct iovec iov = {&hello, sizeof(hello)};
  union {
    char buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &sv[1], sizeof(int));
  if (sendmsg(chan, &msg, MSG_NOSIGNAL) < 0) Die("connecting to the lock");
  close(sv[1]);
  close(chan);  // cmatrix gets nothing of the lock
  conn = sv[0];
}

int main(int argc_main, char **argv_main) {
  if ((argc_main == 4 || argc_main == 5) && !strcmp(argv_main[1], "--sheet")) {
    return Sheet(argv_main[2], argv_main[3],
                 argc_main == 5 ? argv_main[4] : RAIN_FONT);
  }
  Connect();
  int c, r;
  if (ReadMessage(5000, &c, &r) != 1) {
    errno = ETIMEDOUT;
    Die("the lock did not accept the saver");
  }
  OpenFace(RAIN_FONT);

  vt = vterm_new(r, c);
  vterm_set_utf8(vt, 1);
  vterm_state_set_bold_highbright(vterm_obtain_state(vt), 1);  // as the VT
  screen = vterm_obtain_screen(vt);
  vterm_screen_set_callbacks(screen, &callbacks, NULL);
  vterm_screen_reset(screen, 1);
  Resize(c, r);

  const char *opts = getenv("XSECURELOCK_MATRIX_OPTIONS");
  if (opts == NULL || !*opts) opts = DEFAULT_OPTIONS;
  char *copy = strdup(opts);
  int argc = 0;
  argv_cmatrix[argc++] = "cmatrix";
  for (char *t = strtok(copy, " \t"); t != NULL && argc < 31;
       t = strtok(NULL, " \t")) {
    argv_cmatrix[argc++] = t;
  }
  argv_cmatrix[argc] = NULL;
  int master = Spawn();
  SendFont();

  char buf[65536];
  for (;;) {
    struct pollfd p[2] = {{master, POLLIN, 0}, {conn, POLLIN, 0}};
    if (poll(p, 2, -1) < 0) {
      if (errno == EINTR) continue;
      Die("poll");
    }
    if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      int what = ReadMessage(0, &c, &r);
      if (what == 0) {
        errno = EPIPE;
        Die("lock closed the saver channel");
      }
      if (what == 2) {  // all cells again, as katakana and kanji (or not)
        for (int k = 0; k < cols * rows; ++k) cells[k].send = 1;
        SendCells();
        continue;
      }
      SendFont();
      if (c != cols || r != rows) {
        Stop(master);
        Resize(c, r);
        SendCells();  // blank until the new cmatrix draws
        master = Spawn();
      }
      continue;
    }
    if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n = read(master, buf, sizeof(buf));
      if (n <= 0) {
        int status = 0;
        waitpid(child, &status, 0);
        child = 0;
        fprintf(stderr, "saver_matrix: cmatrix %s %d\n",
                WIFSIGNALED(status) ? "killed by signal" : "exited with status",
                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
        errno = ECHILD;
        Die("cmatrix exited");
      }
      vterm_input_write(vt, buf, (size_t)n);
      // Answers to its queries (cursor position, device attributes).
      size_t out;
      while ((out = vterm_output_read(vt, buf, sizeof(buf))) > 0) {
        if (write(master, buf, out) < 0) break;
      }
      vterm_screen_flush_damage(screen);
      SendCells();
    }
  }
}
