#!/usr/bin/python3
"""Security test of ttysecurelock on a real VT.

Run as root on a machine nobody is using at the console (it switches VTs):
    sudo python3 test/console/security.py

Locks VT 12 (owned for the test by a throwaway user with a known password)
with the installed ttysecurelock, drives a uinput keyboard and checks:
  1. the lock takes a VT from the top (>= 13) and brings it to the front;
  2. no way out: chvt 1, root's VT_ACTIVATE, Ctrl+Alt+F1, Alt+F1, Alt+Right,
     Ctrl+C, Ctrl+Z, Ctrl+\\ leave the lock VT in front and the lock running;
  3. the user's own processes cannot read the lock's memory or fds
     (/proc/<pid>/fd, /proc/<pid>/mem of the main program, auth_x11,
     authproto_pam); the helpers that can be opened are reported;
  4. a wrong password keeps it locked;
  5. the right password unlocks: exit 0, back on VT 12, the lock VT freed,
     VT switching works again;
  6. fail closed: SIGKILL of the main program by root, and SIGTERM and
     SIGKILL by the locked user himself, leave the lock VT in front with
     switching disabled (the test then releases it as root).
Takes /tmp/vt12-test.lock and refuses to run while a real lock is active.
"""
import fcntl
import glob
import os
import pwd
import signal
import struct
import subprocess
import sys
import time

TTYSECURELOCK = "/usr/local/bin/ttysecurelock"
VT = 12
TTY = f"/dev/tty{VT}"
USER = "ttyslsectest"
PASSWORD = "73914286"
ESC, ENTER, CTRL, ALT, SHIFT, F1, RIGHT = 1, 28, 29, 56, 42, 59, 106
KEY_C, KEY_Z, BACKSLASH, KEY_A = 46, 44, 43, 30
DIGIT = {str(d % 10): 1 + d for d in range(1, 11)}
KEYS = sorted({ESC, ENTER, CTRL, ALT, SHIFT, F1, RIGHT, KEY_C, KEY_Z,
               BACKSLASH, KEY_A, *DIGIT.values()})
UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
VT_ACTIVATE, VT_UNLOCKSWITCH = 0x5606, 0x560C
EVENT = struct.Struct("llHHi")

failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}",
          flush=True)
    failures += not ok


def keyboard():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, 1)
    for k in KEYS:
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack(
        "HHHH80sI", 6, 1, 1, 1, b"ttysecurelock security keyboard", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.0)
    return fd


def emit(kb, code, down):
    os.write(kb, EVENT.pack(0, 0, 1, code, down))
    os.write(kb, EVENT.pack(0, 0, 0, 0, 0))
    time.sleep(0.02)


def chord(kb, *keys):
    for k in keys:
        emit(kb, k, 1)
    for k in reversed(keys):
        emit(kb, k, 0)


def type_text(kb, s):
    for c in s:
        chord(kb, DIGIT[c])
    chord(kb, ENTER)


def active_vt():
    return int(open("/sys/class/tty/tty0/active").read().strip()[3:])


def tty0_ioctl(req, arg):
    fd = os.open("/dev/tty0", os.O_RDWR | os.O_NOCTTY)
    try:
        fcntl.ioctl(fd, req, arg)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


def release_console():
    """Root's way out of a lock that failed closed (test machine only)."""
    tty0_ioctl(VT_UNLOCKSWITCH, 1)
    subprocess.run(["chvt", str(VT)], timeout=5)


def lock_pids():
    out = []
    for comm in glob.glob("/proc/[0-9]*/comm"):
        try:
            if open(comm).read().strip() == "ttysecurelock":
                out.append(int(comm.split("/")[2]))
        except OSError:
            pass
    return out


def children(pid):
    """All descendants of pid: (pid, comm)."""
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        for task in glob.glob(f"/proc/{p}/task/*/children"):
            try:
                kids = [int(k) for k in open(task).read().split()]
            except OSError:
                continue
            for k in kids:
                try:
                    out.append((k, open(f"/proc/{k}/comm").read().strip()))
                except OSError:
                    continue
                todo.append(k)
    return out


def as_user(*argv):
    return subprocess.run(["runuser", "-u", USER, "--", *argv],
                          capture_output=True, text=True, timeout=10)


def start_lock(kb):
    os.chown(TTY, pwd.getpwnam(USER).pw_uid, -1)
    if active_vt() != VT:
        subprocess.run(["chvt", str(VT)], check=True, timeout=5)
    proc = subprocess.Popen(
        ["env", "-i", "LANG=en_US.UTF-8", "PATH=/usr/local/bin:/usr/bin",
         TTYSECURELOCK, "--user", USER], start_new_session=True,
        stderr=open("/tmp/ttysecurelock-security.stderr", "a"))
    for _ in range(250):
        if active_vt() != VT:
            break
        time.sleep(0.02)
    time.sleep(1.0)
    chord(kb, SHIFT)            # wake the prompt (this key is not typed)
    time.sleep(1.5)
    return proc


def still_locked(proc, lock_vt):
    return active_vt() == lock_vt and proc.poll() is None


def main():
    if os.geteuid() != 0:
        sys.exit("run as root")
    lockf = open("/tmp/vt12-test.lock", "w")
    fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if lock_pids():
        sys.exit("a ttysecurelock is running (a real lock?): refusing")
    orig_vt = active_vt()
    subprocess.run(["useradd", "-M", "-s", "/usr/bin/nologin", "-c",
                    "ttysecurelock security test", USER], check=True)
    kb = None
    try:
        subprocess.run(["chpasswd"], input=f"{USER}:{PASSWORD}", text=True,
                       check=True)
        kb = keyboard()

        # 1-5: a lock, every way out, the user's processes, the passwords.
        proc = start_lock(kb)
        lock_vt = active_vt()
        check("the lock takes a VT from the top", lock_vt >= 13,
              f"VT {lock_vt}")
        try:
            subprocess.run(["chvt", "1"], timeout=2)
        except subprocess.TimeoutExpired:
            pass                # chvt waits for a switch that never comes
        check("chvt 1 refused", still_locked(proc, lock_vt))
        tty0_ioctl(VT_ACTIVATE, 1)
        time.sleep(0.5)
        check("root's VT_ACTIVATE refused", still_locked(proc, lock_vt))
        for keys, name in (((CTRL, ALT, F1), "Ctrl+Alt+F1"),
                           ((ALT, F1), "Alt+F1"),
                           ((ALT, RIGHT), "Alt+Right"),
                           ((CTRL, KEY_C), "Ctrl+C"),
                           ((CTRL, KEY_Z), "Ctrl+Z"),
                           ((CTRL, BACKSLASH), "Ctrl+\\")):
            chord(kb, *keys)
            time.sleep(0.5)
            check(f"{name} inert", still_locked(proc, lock_vt))

        procs = [(proc.pid, "ttysecurelock")] + children(proc.pid)
        for pid, comm in procs:
            r = as_user("ls", f"/proc/{pid}/fd")
            m = as_user("dd", f"if=/proc/{pid}/mem", "of=/dev/null",
                        "bs=1", "count=1", "status=none")
            readable = r.returncode == 0
            if comm in ("ttysecurelock", "auth_x11", "authproto_pam"):
                check(f"the user cannot open the fds of {comm}",
                      not readable, r.stderr.strip() or "listed")
                check(f"the user cannot read the memory of {comm}",
                      m.returncode != 0)
            else:
                print(f"INFO  the user {'CAN' if readable else 'cannot'} "
                      f"open the fds of {comm} (pid {pid})", flush=True)

        chord(kb, KEY_A)
        type_text(kb, "1111")
        time.sleep(5.0)
        check("wrong password: still locked", still_locked(proc, lock_vt))
        type_text(kb, PASSWORD)
        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            rc = None
        time.sleep(0.5)
        check("right password: the lock exits 0", rc == 0, f"exit {rc}")
        check("... back on the locked VT", active_vt() == VT,
              f"VT {active_vt()}")
        freed = True
        try:
            os.close(os.open(f"/dev/vcs{lock_vt}", os.O_RDONLY))
            freed = False
        except OSError:
            pass
        check("... the lock VT is freed", freed)
        tty0_ioctl(VT_ACTIVATE, orig_vt)
        time.sleep(0.5)
        check("... VT switching works again", active_vt() == orig_vt)
        subprocess.run(["chvt", str(VT)], timeout=5)
        if rc is None:
            proc.kill()
            release_console()

        # 6: fail closed.
        for who, sig in (("root", signal.SIGKILL), ("the user", signal.SIGTERM),
                         ("the user", signal.SIGKILL)):
            proc = start_lock(kb)
            lock_vt = active_vt()
            if who == "root":
                os.kill(proc.pid, sig)
                sent = True
            else:
                sent = as_user("kill", f"-{sig.name[3:]}",
                               str(proc.pid)).returncode == 0
            proc.wait(timeout=5) if sent else None
            time.sleep(0.5)
            try:
                subprocess.run(["chvt", "1"], timeout=2)
            except subprocess.TimeoutExpired:
                pass
            chord(kb, CTRL, ALT, F1)
            time.sleep(0.5)
            check(f"{sig.name} by {who}: "
                  f"{'the lock died' if sent else 'not permitted'}, "
                  f"the console stays on the lock VT",
                  active_vt() == lock_vt, f"VT {active_vt()}")
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            # Its helpers run on; they must not hold the user's session.
            subprocess.run(["pkill", "-KILL", "-u", USER])
            release_console()
            check("... released by root (test only)", active_vt() == VT)
    finally:
        if kb is not None:
            fcntl.ioctl(kb, UI_DEV_DESTROY)
        for pid in lock_pids():
            os.kill(pid, signal.SIGKILL)
            time.sleep(0.3)
            release_console()
        subprocess.run(["pkill", "-KILL", "-u", USER])
        time.sleep(0.5)
        subprocess.run(["faillock", "--user", USER, "--reset"])
        subprocess.run(["userdel", USER])
        os.chown(TTY, 0, -1)
        if active_vt() != orig_vt:
            subprocess.run(["chvt", str(orig_vt)], timeout=5)
    print(f"{'FAIL' if failures else 'PASS'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
