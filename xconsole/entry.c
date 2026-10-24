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

/*! \brief The ttysecurelock entry point: `ttysecurelock --user NAME [xsecurelock
 * arguments]`.
 *
 * Runs xsecurelock's unchanged main() (compiled with -Dmain=xsecurelock_main)
 * on the console platform. Started as root (it takes a VT); NAME must be the
 * owner of the VT in front, the session being locked: the caller (the root
 * hotkey daemon tty-i3-keys) names whom it means to lock, and the lock
 * refuses if the console in front is someone else's by then. The rest of the
 * command line is xsecurelock's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../logging.h"
#include "xconsole_internal.h"

int xsecurelock_main(int argc, char **argv);

int main(int argc, char **argv) {
  // $DISPLAY names an existing lock's shared state to its helpers; the main
  // program always makes a new one.
  unsetenv("DISPLAY");
  if (argc >= 3 && !strcmp(argv[1], "--user")) {
    xc_lock_user = argv[2];
    argv[2] = argv[0];
    return xsecurelock_main(argc - 2, argv + 2);
  }
  if (argc >= 2 &&
      (!strcmp(argv[1], "--help") || !strcmp(argv[1], "--version"))) {
    return xsecurelock_main(argc, argv);
  }
  fprintf(stderr,
          "Usage: %s --user NAME [xsecurelock arguments]\n"
          "Locks the Linux console for NAME, the owner of the VT in front.\n",
          argv[0]);
  return 1;
}
