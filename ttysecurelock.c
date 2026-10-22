/*
Copyright 2014 Google Inc. All rights reserved.
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

/*! \brief ttysecurelock: xsecurelock's authentication path on the Linux
 * text console.
 *
 * It takes the place of xsecurelock's main program and of auth_x11: it
 * locks the console and is the user interface, while the password is
 * checked by xsecurelock's unchanged authproto_pam helper, spoken to over
 * its unchanged pipe protocol (helpers/authproto.h).
 *
 * It must be started as root (by a privileged hotkey daemon). Running as
 * root, it cannot be killed by the user's own processes, which is stronger
 * than xsecurelock in X; authproto_pam runs as the locked user, so PAM checks
 * that user's password exactly as it does for xsecurelock.
 *
 * Locking: a free VT is allocated and brought to the front; it is put in
 * VT_PROCESS mode (every switch request asks us, and we refuse) and switching
 * is disabled with VT_LOCKSWITCH. The keyboard is read raw from that VT: no
 * signal keys, no echo. On success the previous VT comes back and the lock
 * VT is freed.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <linux/vt.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "env_info.h"
#include "helpers/authproto.h"
#include "logging.h"
#include "mlock_page.h"
#include "util.h"

#define INPUT_MAX_BYTES 256

static int vt_fd = -1;  // The lock VT.
static int lock_vt = 0;
static int orig_vt = 0;
static struct termios saved_tio;
static int have_saved_tio;

static int WaitReadable(int fd, pid_t pid);
static int HelperStopped(pid_t pid);
static pid_t helper_pid;  // The authproto_pam of the current attempt.
static char header[512];  // "Locked: user@host"
static char status_line[512];
static int status_is_error;

// VT_PROCESS: the kernel asks before switching away (relsig) and tells us
// when we are switched to (acqsig). Leaving is always refused.
static void OnRelease(int sig) {
  (void)sig;
  ioctl(vt_fd, VT_RELDISP, 0);
}
static void OnAcquire(int sig) {
  (void)sig;
  ioctl(vt_fd, VT_RELDISP, VT_ACKACQ);
}

static void Out(const char *s) {
  size_t len = strlen(s);
  while (len > 0) {
    ssize_t n = write(vt_fd, s, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    s += n;
    len -= (size_t)n;
  }
}

static void SetStatus(const char *msg, int is_error) {
  snprintf(status_line, sizeof(status_line), "%s", msg);
  status_is_error = is_error;
}

/*! \brief Redraw the lock screen with the prompt and the input so far. */
static void Draw(const char *prompt, const char *input, int echo) {
  char line[1024];
  Out("\033[H\033[2J\n\n");
  snprintf(line, sizeof(line), "    \033[1m%s\033[0m\n\n", header);
  Out(line);
  if (status_line[0]) {
    snprintf(line, sizeof(line), "    %s%s\033[0m\n\n",
             status_is_error ? "\033[1;31m" : "\033[33m", status_line);
    Out(line);
  }
  if (!prompt) return;
  snprintf(line, sizeof(line), "    %s ", prompt);
  Out(line);
  if (echo) {
    Out(input);
  } else {
    // One asterisk per character, as auth_x11's "asterisks" prompt.
    for (const char *p = input; *p; ++p) {
      if ((*p & 0xC0) != 0x80) Out("*");
    }
  }
}

/*! \brief Discard the rest of an escape sequence (arrow keys etc.). */
static void SkipEscape(void) {
  struct pollfd pfd = {vt_fd, POLLIN, 0};
  char c;
  if (poll(&pfd, 1, 50) <= 0 || read(vt_fd, &c, 1) != 1) return;
  if (c != '[' && c != 'O') return;
  while (poll(&pfd, 1, 50) > 0 && read(vt_fd, &c, 1) == 1) {
    if (c >= 0x40 && c <= 0x7E) return;
  }
}

/*! \brief Ask the user for a line, like auth_x11's Prompt().
 *
 * \return 1 with *response set (mlocked, caller frees), 0 if cancelled.
 */
static int Prompt(const char *message, char **response, int echo) {
  char *buf = malloc(INPUT_MAX_BYTES + 1);
  if (!buf) {
    LogErrno("malloc");
    return 0;
  }
  if (MLOCK_PAGE(buf, INPUT_MAX_BYTES + 1) < 0) {
    LogErrno("mlock");
  }
  size_t len = 0;
  buf[0] = 0;
  Draw(message, buf, echo);
  for (;;) {
    // While waiting for keys, also watch the helper: stopped by the user, it
    // would never take the answer; give up this attempt at once.
    struct pollfd pfd = {vt_fd, POLLIN, 0};
    int r = poll(&pfd, 1, 500);
    if (r == 0 || (r < 0 && errno == EINTR)) {
      if (HelperStopped(helper_pid)) {
        explicit_bzero(buf, INPUT_MAX_BYTES + 1);
        free(buf);
        return 0;
      }
      continue;
    }
    unsigned char c;
    ssize_t n = read(vt_fd, &c, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n != 1) {
      // The VT is gone (or hung up): nothing to type into. Cancel this
      // attempt; the caller keeps the console locked and starts over.
      explicit_bzero(buf, INPUT_MAX_BYTES + 1);
      free(buf);
      sleep(1);
      return 0;
    }
    if (c == '\r' || c == '\n') {
      *response = buf;
      return 1;
    } else if (c == 0x7F || c == 0x08) {  // Backspace: one UTF-8 character.
      while (len > 0 && (buf[len - 1] & 0xC0) == 0x80) --len;
      if (len > 0) --len;
      buf[len] = 0;
    } else if (c == 0x15) {  // Ctrl+U: clear.
      explicit_bzero(buf, len);
      len = 0;
      buf[0] = 0;
    } else if (c == 0x1B) {  // Esc clears too, as in xsecurelock.
      SkipEscape();
      explicit_bzero(buf, len);
      len = 0;
      buf[0] = 0;
    } else if (c < 0x20) {
      continue;  // Other control keys (Ctrl+C, Ctrl+Z, ...) do nothing.
    } else if (len < INPUT_MAX_BYTES) {
      buf[len++] = (char)c;
      buf[len] = 0;
    }
    Draw(message, buf, echo);
  }
}

/*! \brief One authentication attempt through authproto_pam, run as the
 * locked user. Mirrors auth_x11's Authenticate().
 *
 * \return 1 if the helper exited with status 0 (authenticated).
 */
static int Authenticate(const struct passwd *pw, const char *authproto) {
  int requestfd[2], responsefd[2];
  if (pipe2(requestfd, O_CLOEXEC) || pipe2(responsefd, O_CLOEXEC)) {
    LogErrno("pipe");
    return 0;
  }
  pid_t pid = fork();
  if (pid < 0) {
    LogErrno("fork");
    return 0;
  }
  if (pid == 0) {
    // Child: become the locked user, keep only the protocol pipes.
    // Only the protocol pipes reach code running as the user: no fd the
    // starting daemon may have leaked, and not its stderr (a root-owned log
    // stream).
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0 || dup2(responsefd[0], 0) < 0 ||
        dup2(requestfd[1], 1) < 0 || dup2(devnull, 2) < 0) {
      _exit(1);
    }
    close_range(3, ~0U, 0);
    for (int sig = 1; sig < NSIG; ++sig) signal(sig, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    if (initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) ||
        setuid(pw->pw_uid) || setuid(0) == 0) {
      _exit(1);  // Could not drop root, or could get it back: refuse.
    }
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("PATH", "/usr/local/bin:/usr/bin", 1);
    execl(authproto, authproto, (char *)NULL);
    _exit(1);
  }
  close(requestfd[1]);
  close(responsefd[0]);
  helper_pid = pid;

  for (;;) {
    char *message = NULL;
    char *response = NULL;
    if (!WaitReadable(requestfd[0], pid)) {
      kill(pid, SIGKILL);
      SetStatus("The authentication helper stopped; try again.", 1);
      break;
    }
    char type = ReadPacket(requestfd[0], &message, 1);
    if (type == PTYPE_INFO_MESSAGE || type == PTYPE_ERROR_MESSAGE) {
      SetStatus(message, type == PTYPE_ERROR_MESSAGE);
    } else if (type == PTYPE_PROMPT_LIKE_USERNAME ||
               type == PTYPE_PROMPT_LIKE_PASSWORD) {
      int echo = type == PTYPE_PROMPT_LIKE_USERNAME;
      if (Prompt(message, &response, echo)) {
        WritePacket(responsefd[1],
                    echo ? PTYPE_RESPONSE_LIKE_USERNAME
                         : PTYPE_RESPONSE_LIKE_PASSWORD,
                    response);
        explicit_bzero(response, strlen(response));
        free(response);
      } else {
        WritePacket(responsefd[1], PTYPE_RESPONSE_CANCELLED, "");
      }
      SetStatus("Processing...", 0);
      Draw(NULL, "", 0);
    } else {
      if (type != 0) Log("Unknown message type %02x", (int)type);
      if (message) {
        explicit_bzero(message, strlen(message));
        free(message);
      }
      break;
    }
    explicit_bzero(message, strlen(message));
    free(message);
  }
  close(requestfd[0]);
  close(responsefd[1]);

  int status;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      LogErrno("waitpid");
      return 0;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*! \brief Wait until the helper has something to say.
 *
 * The helper runs as the locked user, who can SIGSTOP it: then nothing ever
 * arrives and the pipe never closes. Give up if it is stopped, or silent for
 * HELPER_TIMEOUT_S (PAM itself answers within seconds).
 *
 * \return 1 if readable (data or EOF), 0 if the helper must be killed.
 */
#define HELPER_TIMEOUT_S 60
static int WaitReadable(int fd, pid_t pid) {
  for (int waited_ms = 0; waited_ms < HELPER_TIMEOUT_S * 1000;
       waited_ms += 500) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int r = poll(&pfd, 1, 500);
    if (r > 0) return 1;
    if (r < 0 && errno != EINTR) return 0;
    if (HelperStopped(pid)) return 0;
  }
  return 0;
}

static int HelperStopped(pid_t pid) {
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  return waitid(P_PID, (id_t)pid, &info, WSTOPPED | WNOHANG | WNOWAIT) == 0 &&
         info.si_pid == pid && info.si_code == CLD_STOPPED;
}

static int ActiveVT(int tty0) {
  struct vt_stat st;
  return ioctl(tty0, VT_GETSTATE, &st) == 0 ? st.v_active : -1;
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
static int BringToFront(int tty0) {
  for (int tries = 0; tries < 20; ++tries) {
    ioctl(tty0, VT_UNLOCKSWITCH, 1);
    ioctl(tty0, VT_ACTIVATE, lock_vt);
    for (int i = 0; i < 20 && ActiveVT(tty0) != lock_vt; ++i) {
      usleep(50000);
    }
    if (ActiveVT(tty0) != lock_vt) continue;
    if (ioctl(tty0, VT_LOCKSWITCH, 1) < 0) {
      LogErrno("VT_LOCKSWITCH");  // VT_PROCESS still refuses every switch.
    }
    if (ActiveVT(tty0) == lock_vt) return 1;
  }
  ioctl(tty0, VT_UNLOCKSWITCH, 1);
  return 0;
}

/*! \brief Undo the lock VT: switching, mode, terminal, and, if unlocked,
 * go back to the previous VT and free the lock VT. */
static void Release(int tty0, int go_back) {
  ioctl(tty0, VT_UNLOCKSWITCH, 1);
  struct vt_mode automode = {VT_AUTO, 0, 0, 0, 0};
  ioctl(vt_fd, VT_SETMODE, &automode);
  Out("\033[H\033[2J");
  if (have_saved_tio) tcsetattr(vt_fd, TCSAFLUSH, &saved_tio);
  if (go_back && ActiveVT(tty0) != orig_vt) {
    ioctl(tty0, VT_ACTIVATE, orig_vt);
    for (int i = 0; i < 40 && ActiveVT(tty0) != orig_vt; ++i) usleep(50000);
  }
  close(vt_fd);
  // Another program may still hold the lock VT open for a moment.
  for (int i = 0; i < 10 && ioctl(tty0, VT_DISALLOCATE, lock_vt) < 0; ++i) {
    usleep(100000);
  }
}

static int OpenTty(int n) {
  char path[32];
  snprintf(path, sizeof(path), "/dev/tty%d", n);
  return open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
}

static void Usage(const char *me) {
  fprintf(stderr,
          "Usage: %s [--user NAME] [--authproto PATH]\n"
          "Lock the Linux console until NAME's password is entered.\n"
          "Must run as root. NAME defaults to the owner of the active VT.\n",
          me);
}

int main(int argc, char **argv) {
  setlocale(LC_CTYPE, "");
  const char *user = NULL;
  const char *authproto = HELPER_PATH "/authproto_pam";
  static const struct option options[] = {
      {"user", required_argument, NULL, 'u'},
      {"authproto", required_argument, NULL, 'a'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, "u:a:h", options, NULL)) != -1) {
    switch (opt) {
      case 'u':
        user = optarg;
        break;
      case 'a':
        authproto = optarg;
        break;
      default:
        Usage(argv[0]);
        return opt == 'h' ? 0 : 2;
    }
  }
  if (geteuid() != 0) {
    Log("Must run as root (it is started by a privileged hotkey daemon)");
    return 1;
  }
  // Fds leaked by the starting daemon are of no use here.
  close_range(3, ~0U, 0);
  // Nobody but root may inspect our memory (it holds the password).
  prctl(PR_SET_DUMPABLE, 0);

  // Keys and hangups must not end the lock. SIGTERM/SIGKILL stay: only
  // root can send them to a root process.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_flags = SA_RESTART;
  sa.sa_handler = OnRelease;
  sigaction(SIGUSR1, &sa, NULL);
  sa.sa_handler = OnAcquire;
  sigaction(SIGUSR2, &sa, NULL);

  int tty0 = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (tty0 < 0) {
    LogErrno("open /dev/tty0");
    return 1;
  }
  struct vt_stat st;
  if (ioctl(tty0, VT_GETSTATE, &st) < 0) {
    LogErrno("VT_GETSTATE");
    return 1;
  }
  orig_vt = st.v_active;

  struct passwd *pw;
  if (user) {
    pw = getpwnam(user);
  } else {
    char path[32];
    struct stat sb;
    snprintf(path, sizeof(path), "/dev/tty%d", orig_vt);
    if (stat(path, &sb) < 0 || sb.st_uid == 0) {
      Log("The active VT belongs to root: nobody to lock for (use --user)");
      return 1;
    }
    pw = getpwuid(sb.st_uid);
  }
  if (!pw || pw->pw_uid == 0) {
    Log("Unknown or root user");
    return 1;
  }
  char host[256];
  if (!GetHostName(host, sizeof(host))) snprintf(host, sizeof(host), "?");
  snprintf(header, sizeof(header), "Locked: %s@%s", pw->pw_name, host);
  struct passwd locked = *pw;  // getpw* results are overwritten by PAM.
  locked.pw_name = strdup(pw->pw_name);
  locked.pw_dir = strdup(pw->pw_dir);

  if (ioctl(tty0, VT_OPENQRY, &lock_vt) < 0 || lock_vt <= 0) {
    Log("No free VT");
    return 1;
  }
  vt_fd = OpenTty(lock_vt);
  if (vt_fd < 0) {
    LogErrno("open lock VT");
    return 1;
  }
  struct vt_mode mode = {VT_PROCESS, 0, SIGUSR1, SIGUSR2, 0};
  if (ioctl(vt_fd, VT_SETMODE, &mode) < 0) {
    LogErrno("VT_SETMODE");
    Release(tty0, 0);
    return 1;
  }
  have_saved_tio = tcgetattr(vt_fd, &saved_tio) == 0;
  if (have_saved_tio) {
    struct termios tio = saved_tio;
    tio.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP | BRKINT);
    tio.c_lflag &= ~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(vt_fd, TCSAFLUSH, &tio);
  }
  if (!BringToFront(tty0)) {
    Log("Could not bring the lock VT to the front");
    Release(tty0, 0);
    return 1;
  }

  SetStatus("", 0);
  while (!Authenticate(&locked, authproto)) {
    if (!status_is_error) SetStatus("Authentication failed.", 1);
  }

  Release(tty0, 1);
  return 0;
}
