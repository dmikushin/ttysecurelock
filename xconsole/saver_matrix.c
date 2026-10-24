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
 * XSECURELOCK_MATRIX_OPTIONS: cmatrix's options (default "-ab -u 5").
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

#include "saver_proto.h"

#define DEFAULT_OPTIONS "-ab -u 5"

static int conn = -1;
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

static void SendCells(void) {
  static char buf[XC_SAVER_MAX_MSG];
  XcSaverCells h = {XC_SAVER_CELLS, 0};
  for (int idx = 0; idx <= cols * rows; ++idx) {
    if (idx < cols * rows && dirty[idx]) {
      dirty[idx] = 0;
      VTermPos pos = {idx / cols, idx % cols};
      VTermScreenCell vc;
      XcSaverCell cell;
      memset(&cell, 0, sizeof(cell));
      cell.col = (uint16_t)pos.col;
      cell.row = (uint16_t)pos.row;
      cell.cp = ' ';
      cell.fg = 7;
      if (vterm_screen_get_cell(screen, pos, &vc)) {
        if (vc.chars[0] != 0 && vc.width == 1) cell.cp = vc.chars[0];
        cell.fg = ConsoleColor(&vc.fg, 1);
        cell.bg = ConsoleColor(&vc.bg, 0);
        if (vc.attrs.reverse) {
          uint8_t t = cell.fg;
          cell.fg = cell.bg;
          cell.bg = t;
        }
      }
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
  dirty = calloc((size_t)(c * r), 1);
  if (dirty == NULL) Die("calloc");
  cols = c;
  rows = r;
  vterm_set_size(vt, rows, cols);
  vterm_screen_reset(screen, 1);
  memset(dirty, 1, (size_t)(cols * rows));
}

static int ReadSize(int timeout_ms, int *c, int *r) {
  struct pollfd p = {conn, POLLIN, 0};
  if (poll(&p, 1, timeout_ms) <= 0) return 0;
  XcSaverSize m;
  ssize_t n = recv(conn, &m, sizeof(m), 0);
  if (n != (ssize_t)sizeof(m) || m.type != XC_SAVER_SIZE || m.cols > 4096 ||
      m.rows > 4096) {
    errno = EPROTO;
    Die("lock closed the saver channel");
  }
  *c = (int)m.cols;
  *r = (int)m.rows;
  return 1;
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

int main(void) {
  Connect();
  int c, r;
  if (!ReadSize(5000, &c, &r)) {
    errno = ETIMEDOUT;
    Die("the lock did not accept the saver");
  }

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

  char buf[65536];
  for (;;) {
    struct pollfd p[2] = {{master, POLLIN, 0}, {conn, POLLIN, 0}};
    if (poll(p, 2, -1) < 0) {
      if (errno == EINTR) continue;
      Die("poll");
    }
    if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      if (!ReadSize(0, &c, &r)) {
        errno = EPIPE;
        Die("lock closed the saver channel");
      }
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
        int status;
        waitpid(child, &status, 0);
        child = 0;
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
