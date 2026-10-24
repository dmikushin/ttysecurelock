// Key events into an X server through XTEST, by evdev keycode (the X keycode
// is evdev + 8), so that the X run and the console run of the parity test
// press the very same keys. Usage: xkey CODE:1 CODE:0 ... (1 press, 0 release)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  Display *d = XOpenDisplay(NULL);
  if (d == NULL) {
    fprintf(stderr, "xkey: cannot open display\n");
    return 1;
  }
  for (int i = 1; i < argc; ++i) {
    int code, down;
    if (sscanf(argv[i], "%d:%d", &code, &down) != 2) {
      fprintf(stderr, "xkey: bad event %s\n", argv[i]);
      return 1;
    }
    XTestFakeKeyEvent(d, (unsigned int)code + 8, down, 0);
    XSync(d, False);
    usleep(20000);
  }
  XCloseDisplay(d);
  return 0;
}
