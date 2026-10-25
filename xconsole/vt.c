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

/*! \brief The lock VT: allocation, locking, release, privileges.
 *
 * Locking: a free VT is allocated, put in VT_PROCESS mode (every switch
 * request asks us, and we refuse) and brought to the front; then switching
 * is disabled with VT_LOCKSWITCH. If the program dies, VT_LOCKSWITCH stays
 * set: the console stays locked (fail closed).
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/kd.h>
#include <linux/tiocl.h>
#include <linux/vt.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

#include "../logging.h"
#include "xconsole_internal.h"

// The lock VT is taken from this range, top down (see FreeHighVT).
#define MAX_LOCK_VT 63
#define MIN_LOCK_VT 13

// VT_PROCESS signals. Not SIGUSR1/SIGUSR2: xsecurelock's main program uses
// those itself.
#define REL_SIGNAL (SIGRTMAX - 1)
#define ACQ_SIGNAL (SIGRTMAX)

static struct termios saved_tio;
static int have_saved_tio;
static uid_t lock_uid;
static gid_t lock_gid;
static char lock_user[256];
const char *xc_lock_user;

// VT_PROCESS: the kernel asks before switching away (relsig) and tells us
// when we are switched to (acqsig). Leaving is always refused.
static void OnRelease(int sig) {
  (void)sig;
  ioctl(xc.vt_fd, VT_RELDISP, 0);
}
static void OnAcquire(int sig) {
  (void)sig;
  ioctl(xc.vt_fd, VT_RELDISP, VT_ACKACQ);
}

static int ActiveVT(void) {
  struct vt_stat st;
  return ioctl(xc.tty0, VT_GETSTATE, &st) == 0 ? st.v_active : -1;
}

/*! \brief A free VT for the lock, from the top (63) down to 13.
 *
 * Not VT_OPENQRY's first free VT: that is usually within logind's autovt
 * range (NAutoVTs, 6 by default), and switching to it makes logind start a
 * getty there, which hangs the lock VT up. A VT is free if the kernel has
 * not allocated it: /dev/vcsN of an unallocated VT fails with ENXIO (or is
 * missing), and, unlike /dev/ttyN, opening it allocates nothing.
 *
 * \return the VT number, or -1.
 */
static int FreeHighVT(void) {
  for (int n = MAX_LOCK_VT; n >= MIN_LOCK_VT; --n) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/tty%d", n);
    if (access(path, F_OK) < 0) continue;
    snprintf(path, sizeof(path), "/dev/vcs%d", n);
    int fd = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);  // allocated: in use
      continue;
    }
    if (errno == ENXIO || errno == ENOENT) return n;
  }
  return -1;
}

/*! \brief Take a free VT for the display. Must run as root.
 *
 * The user to run as is the owner of the VT in front: the console session
 * being locked, as xsecurelock locks the session of the user running it.
 *
 * \return 1 on success.
 */
int XcVtOpen(void) {
  if (geteuid() != 0) {
    Log("ttysecurelock must be started as root (it takes a VT); it then "
        "runs as the owner of the console in front");
    return 0;
  }
  xc.tty0 = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (xc.tty0 < 0) {
    LogErrno("open /dev/tty0");
    return 0;
  }
  xc.orig_vt = ActiveVT();
  char path[32];
  struct stat sb;
  snprintf(path, sizeof(path), "/dev/tty%d", xc.orig_vt);
  if (xc.orig_vt <= 0 || stat(path, &sb) < 0 || sb.st_uid == 0) {
    Log("The console in front belongs to root: nobody to lock for");
    return 0;
  }
  struct passwd *pw = getpwuid(sb.st_uid);
  if (pw == NULL || strlen(pw->pw_name) >= sizeof(lock_user)) {
    Log("Unknown owner of the console in front");
    return 0;
  }
  if (xc_lock_user == NULL || strcmp(pw->pw_name, xc_lock_user)) {
    Log("The console in front (tty%d) belongs to %s, not to %s: not locking",
        xc.orig_vt, pw->pw_name, xc_lock_user ? xc_lock_user : "(nobody)");
    return 0;
  }
  lock_uid = pw->pw_uid;
  lock_gid = pw->pw_gid;
  strcpy(lock_user, pw->pw_name);
  setenv("USER", pw->pw_name, 1);
  setenv("LOGNAME", pw->pw_name, 1);
  setenv("HOME", pw->pw_dir, 1);

  int vt = FreeHighVT();
  if (vt <= 0) {
    Log("No free VT");
    return 0;
  }
  // As a session's leader without a controlling tty, the lock VT becomes
  // ours: that permits the console ioctls the display needs (blanking,
  // TIOCLINUX). Started by tty-i3-keys (start_new_session) we already lead
  // a session, and setsid() fails.
  int ctty = setsid() >= 0 || getsid(0) == getpid();
  snprintf(path, sizeof(path), "/dev/tty%d", vt);
  xc.vt_fd = open(path, O_RDWR | O_CLOEXEC | (ctty ? 0 : O_NOCTTY));
  if (xc.vt_fd < 0) {
    LogErrno("open %s", path);
    return 0;
  }
  Log("xconsole: lock VT tty%d %s our controlling tty", vt,
      tcgetsid(xc.vt_fd) == getsid(0) ? "is" : "is NOT (no blanking)");
  // Helpers draw on the VT through this one; they cannot read from it.
  xc.draw_fd = open(path, O_WRONLY | O_NOCTTY);
  if (xc.draw_fd < 0) {
    LogErrno("open %s", path);
    return 0;
  }
  xc.vt = vt;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_flags = SA_RESTART;
  sa.sa_handler = OnRelease;
  sigaction(REL_SIGNAL, &sa, NULL);
  sa.sa_handler = OnAcquire;
  sigaction(ACQ_SIGNAL, &sa, NULL);
  struct vt_mode mode = {VT_PROCESS, 0, REL_SIGNAL, ACQ_SIGNAL, 0};
  if (ioctl(xc.vt_fd, VT_SETMODE, &mode) < 0) {
    LogErrno("VT_SETMODE");
    return 0;
  }
  have_saved_tio = tcgetattr(xc.vt_fd, &saved_tio) == 0;
  if (have_saved_tio) {
    struct termios tio = saved_tio;
    cfmakeraw(&tio);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(xc.vt_fd, TCSAFLUSH, &tio);
  }
  // The keymap is read while the VT is still in Unicode mode: through a VT
  // in another mode the kernel reports Unicode keysyms as holes.
  // The keyboard state (layout, Caps Lock, Num Lock) starts as the console
  // in front has it.
  snprintf(path, sizeof(path), "/dev/tty%d", xc.orig_vt);
  int orig_fd = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
  int ok = XcKeyboardInit(xc.vt_fd, orig_fd);
  if (orig_fd >= 0) close(orig_fd);
  if (!ok) return 0;
  return 1;
}

/*! \brief Become the user owning the console, as xsecurelock runs as the
 * user of the X session; keep only CAP_SYS_TTY_CONFIG (VT_LOCKSWITCH and the
 * VT ioctls), which helpers do not inherit.
 */
int XcVtDropPrivileges(void) {
  if (prctl(PR_SET_KEEPCAPS, 1) < 0 || initgroups(lock_user, lock_gid) < 0 ||
      setresgid(lock_gid, lock_gid, lock_gid) < 0 ||
      setresuid(lock_uid, lock_uid, lock_uid) < 0) {
    LogErrno("Dropping privileges");
    return 0;
  }
  struct __user_cap_header_struct hdr = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct data[2];
  memset(data, 0, sizeof(data));
  data[CAP_TO_INDEX(CAP_SYS_TTY_CONFIG)].effective =
      CAP_TO_MASK(CAP_SYS_TTY_CONFIG);
  data[CAP_TO_INDEX(CAP_SYS_TTY_CONFIG)].permitted =
      CAP_TO_MASK(CAP_SYS_TTY_CONFIG);
  if (syscall(SYS_capset, &hdr, data) < 0) {
    LogErrno("capset");
    return 0;
  }
  prctl(PR_SET_KEEPCAPS, 0);
  // Not ptrace-able by the user's other processes (it holds the keyboard).
  prctl(PR_SET_DUMPABLE, 0);
  if (setuid(0) == 0) {
    Log("Could get root back after dropping it - refusing");
    return 0;
  }
  return 1;
}

/*! \brief Make the lock VT the foreground one, then lock switching.
 *
 * Until the lock VT is in front, a switch from the keyboard (Ctrl+Alt+Fn)
 * can still happen and would win over our VT_ACTIVATE; so activation is
 * repeated until the lock VT is really in front. From then on its VT_PROCESS
 * mode refuses every switch, and VT_LOCKSWITCH stops them earlier.
 *
 * \return 1 if locked in front.
 */
int XcVtBringToFront(void) {
  int vt = xc.vt;
  for (int tries = 0; tries < 20; ++tries) {
    if (ActiveVT() != vt) {
      ioctl(xc.tty0, VT_UNLOCKSWITCH, 1);
      ioctl(xc.tty0, VT_ACTIVATE, vt);
      for (int i = 0; i < 20 && ActiveVT() != vt; ++i) {
        usleep(50000);
      }
      if (ActiveVT() != vt) continue;
    }
    XcKeyboardSetMode(xc.vt_fd);
    if (ioctl(xc.tty0, VT_LOCKSWITCH, 1) < 0) {
      LogErrno("VT_LOCKSWITCH");  // VT_PROCESS still refuses every switch.
    }
    if (ActiveVT() == vt) return 1;
  }
  ioctl(xc.tty0, VT_UNLOCKSWITCH, 1);
  return 0;
}

/*! \brief Make the console in front own the mouse selection.
 *
 * The kernel will not free the console that owns the selection, and a
 * console mouse daemon (gpm, console-mouse) drawing its pointer on the lock
 * VT makes that VT the owner: put a pointer on the console in front and
 * clear it at once.
 */
static void HandOverSelection(void) {
  for (unsigned short mode = TIOCL_SELPOINTER; mode <= TIOCL_SELCLEAR;
       ++mode) {
    unsigned short sel[5] = {1, 1, 1, 1, mode};
    char arg[1 + sizeof(sel)];
    arg[0] = TIOCL_SETSEL;
    memcpy(arg + 1, sel, sizeof(sel));
    ioctl(xc.tty0, TIOCLINUX, arg);
  }
}

/*! \brief Undo the lock VT: keyboard, switching, mode, terminal, and, if
 * unlocked, go back to the previous VT and free the lock VT. */
void XcVtRelease(int go_back) {
  int vt = xc.vt;
  XcKeyboardRestore(xc.vt_fd);
  ioctl(xc.tty0, VT_UNLOCKSWITCH, 1);
  struct vt_mode automode = {VT_AUTO, 0, 0, 0, 0};
  ioctl(xc.vt_fd, VT_SETMODE, &automode);
  static const char reset[] = "\033[0m\033[H\033[2J\033[?25h";
  if (write(xc.vt_fd, reset, sizeof(reset) - 1) < 0) {
    // Nothing to do about it.
  }
  if (have_saved_tio) tcsetattr(xc.vt_fd, TCSAFLUSH, &saved_tio);
  if (go_back && ActiveVT() != xc.orig_vt) {
    ioctl(xc.tty0, VT_ACTIVATE, xc.orig_vt);
    for (int i = 0; i < 40 && ActiveVT() != xc.orig_vt; ++i) usleep(50000);
  }
  close(xc.vt_fd);
  close(xc.draw_fd);
  if (go_back) HandOverSelection();
  // Another program may still hold the lock VT open for a moment.
  for (int i = 0; i < 10 && ioctl(xc.tty0, VT_DISALLOCATE, vt) < 0; ++i) {
    usleep(100000);
  }
}
