// LD_PRELOAD shim for the parity test: the same rand() sequence in every
// process and every run (srand is ignored), so that xsecurelock under X and
// ttysecurelock on the console make the same "random" choices (password
// cursor position, emoji, burn-in offsets) and their screens can be compared
// exactly. Only calls from the program itself take values from that
// sequence: libraries that differ between the two (Xft and fontconfig under
// X) get theirs from another one.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdlib.h>

static unsigned int program_state = 12345, library_state = 54321;

static int Next(unsigned int *state) {
  *state = *state * 1103515245u + 12345u;
  return (int)((*state >> 16) & 0x7fff);
}

static int FromProgram(void *caller) {
  Dl_info info;
  struct link_map *map = NULL;
  if (!dladdr1(caller, &info, (void **)&map, RTLD_DL_LINKMAP) || map == NULL) {
    return 0;
  }
  return map->l_name == NULL || map->l_name[0] == 0;  // the executable
}

int rand(void) {
  return Next(FromProgram(__builtin_return_address(0)) ? &program_state
                                                      : &library_state);
}

void srand(unsigned int seed) { (void)seed; }
