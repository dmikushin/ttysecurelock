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
 *
 * Settings: xsecurelock's XSECURELOCK_* environment variables, from the
 * environment, else from NAME's ~/.config/ttysecurelock/environment (read as
 * NAME; see docs/fork.md).
 */

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../logging.h"
#include "xconsole_internal.h"

int xsecurelock_main(int argc, char **argv);

#define SETTINGS_FILE "/.config/ttysecurelock/environment"
#define SETTINGS_MAX 65536

//! Whether name is XSECURELOCK_ followed by [A-Z0-9_]+.
static int SettingName(const char *name) {
  if (strncmp(name, "XSECURELOCK_", 12) || !name[12]) return 0;
  for (const char *p = name + 12; *p; ++p) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) {
      return 0;
    }
  }
  return 1;
}

/*! \brief Settings the file may not give: who checks the password
 * (_AUTH, _AUTHPROTO, _PAM_SERVICE), what runs with the lock VT and the
 * window table (_GLOBAL_SAVER, _SWITCH_USER_COMMAND, _KEY_*_COMMAND), and the
 * debugging switches. The file is
 * writable by every process of the user, which cannot otherwise get a
 * locked console back (killing the lock fails closed); with these it could
 * make the next lock accept anything. They are still taken from the
 * environment of whoever starts the lock (root). */
static int FileMayGive(const char *name) {
  static const char *const denied[] = {
      "XSECURELOCK_AUTH", "XSECURELOCK_AUTHPROTO", "XSECURELOCK_PAM_SERVICE",
      "XSECURELOCK_GLOBAL_SAVER", NULL};
  for (int i = 0; denied[i] != NULL; ++i) {
    if (!strcmp(name, denied[i])) return 0;
  }
  size_t len = strlen(name);
  if (!strcmp(name, "XSECURELOCK_SWITCH_USER_COMMAND") ||
      (!strncmp(name, "XSECURELOCK_KEY_", 16) && len > 8 &&
       !strcmp(name + len - 8, "_COMMAND"))) {
    return 0;
  }
  return strncmp(name, "XSECURELOCK_DEBUG_", 18) != 0;
}

/*! \brief The user's settings: XSECURELOCK_* lines of
 * ~user/.config/ttysecurelock/environment, as environment.d(5) writes them
 * (KEY=VALUE, # comments, a value may be in quotes; no $ expansion). The
 * file is read by a child process running as the user, so root never opens
 * the user's files; variables already in the environment are kept.
 */
static void ImportUserSettings(const char *user) {
  struct passwd *pw = getpwnam(user);
  if (pw == NULL) return;  // the lock itself refuses an unknown user
  uid_t uid = pw->pw_uid;
  gid_t gid = pw->pw_gid;
  char path[4096];
  if (snprintf(path, sizeof(path), "%s" SETTINGS_FILE, pw->pw_dir) >=
      (int)sizeof(path)) {
    return;
  }
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) return;
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return;
  }
  if (pid == 0) {
    close(p[0]);
    if (initgroups(user, gid) < 0 || setresgid(gid, gid, gid) < 0 ||
        setresuid(uid, uid, uid) < 0) {
      _exit(1);
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) _exit(0);
    static char buf[SETTINGS_MAX];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0 && write(p[1], buf, (size_t)n) != n) _exit(1);
    _exit(0);
  }
  close(p[1]);
  static char data[SETTINGS_MAX + 1];
  size_t len = 0;
  ssize_t n;
  while (len < SETTINGS_MAX &&
         (n = read(p[0], data + len, SETTINGS_MAX - len)) > 0) {
    len += (size_t)n;
  }
  close(p[0]);
  int status;
  waitpid(pid, &status, 0);
  data[len] = 0;
  for (char *line = data, *next; line != NULL && *line; line = next) {
    next = strchr(line, '\n');
    if (next != NULL) *next++ = 0;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '#' || *line == 0) continue;
    char *eq = strchr(line, '=');
    if (eq == NULL) continue;
    *eq = 0;
    char *value = eq + 1;
    size_t vl = strlen(value);
    while (vl > 0 && (value[vl - 1] == ' ' || value[vl - 1] == '\t' ||
                      value[vl - 1] == '\r')) {
      value[--vl] = 0;
    }
    if (vl >= 2 && (value[0] == '"' || value[0] == '\'') &&
        value[vl - 1] == value[0]) {
      value[vl - 1] = 0;
      ++value;
    }
    if (!SettingName(line) || !FileMayGive(line)) {
      Log("%s: ignoring %s (see docs/fork.md, \"Settings file\")",
          SETTINGS_FILE + 1, line);
      continue;
    }
    setenv(line, value, 0);
  }
}

int main(int argc, char **argv) {
  // $DISPLAY names an existing lock's shared state to its helpers; the main
  // program always makes a new one.
  unsetenv("DISPLAY");
  if (argc >= 3 && !strcmp(argv[1], "--user")) {
    xc_lock_user = argv[2];
    ImportUserSettings(xc_lock_user);
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
