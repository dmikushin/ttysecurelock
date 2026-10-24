// LD_PRELOAD shim for the parity test: logs every string xsecurelock's
// auth_x11 draws under X (Xft and core fonts) and every window clear, one
// line each, to $XSHIM_LOG: "<ms> CLEAR <window>" or
// "<ms> TEXT <window> <x> <y> <r>,<g>,<b> <utf-8 text>".
#define _GNU_SOURCE
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static void Emit(const char *line, size_t n) {
  const char *p = getenv("XSHIM_LOG");
  if (!p) return;
  int fd = open(p, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) return;
  if (write(fd, line, n) < 0) {}
  close(fd);
}

static long Ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void Text(unsigned long w, int x, int y, int r, int g, int b,
                 const char *s, int len) {
  char buf[4096];
  int n = snprintf(buf, sizeof(buf), "%ld TEXT %lu %d %d %d,%d,%d ", Ms(), w,
                   x, y, r, g, b);
  if (len > (int)sizeof(buf) - n - 2) len = (int)sizeof(buf) - n - 2;
  for (int i = 0; i < len; ++i) buf[n++] = s[i] == '\n' ? ' ' : s[i];
  buf[n++] = '\n';
  Emit(buf, (size_t)n);
}

void XftDrawStringUtf8(XftDraw *draw, const XftColor *color, XftFont *pub,
                       int x, int y, const FcChar8 *string, int len) {
  static void (*real)(XftDraw *, const XftColor *, XftFont *, int, int,
                      const FcChar8 *, int);
  if (!real) real = dlsym(RTLD_NEXT, "XftDrawStringUtf8");
  Text(XftDrawDrawable(draw), x, y, color->color.red >> 8,
       color->color.green >> 8, color->color.blue >> 8, (const char *)string,
       len);
  real(draw, color, pub, x, y, string, len);
}

int XDrawString(Display *d, Drawable w, GC gc, int x, int y, const char *s,
                int len) {
  static int (*real)(Display *, Drawable, GC, int, int, const char *, int);
  if (!real) real = dlsym(RTLD_NEXT, "XDrawString");
  XGCValues v;
  XGetGCValues(d, gc, GCForeground, &v);
  Text(w, x, y, (int)(v.foreground >> 16) & 255, (int)(v.foreground >> 8) & 255,
       (int)v.foreground & 255, s, len);
  return real(d, w, gc, x, y, s, len);
}

int XClearWindow(Display *d, Window w) {
  static int (*real)(Display *, Window);
  if (!real) real = dlsym(RTLD_NEXT, "XClearWindow");
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%ld CLEAR %lu\n", Ms(), w);
  Emit(buf, (size_t)n);
  return real(d, w);
}

// auth_x11 ending takes its window (and the auth screen) away.
__attribute__((destructor)) static void Exit(void) {
  char comm[32] = "";
  int fd = open("/proc/self/comm", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, comm, sizeof(comm) - 1);
    if (n > 0) comm[n - 1] = 0;
    close(fd);
  }
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "%ld EXIT %s\n", Ms(), comm);
  Emit(buf, (size_t)n);
}
