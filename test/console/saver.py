#!/usr/bin/python3
"""Test of the console saver channel with saver_matrix on a real VT.

Run as root on a machine nobody is using at the console (it switches VTs):
    sudo python3 test/console/saver.py

Locks VT 12 for a throwaway user whose ~/.config/ttysecurelock/environment
sets XSECURELOCK_SAVER=saver_matrix and a short XSECURELOCK_AUTH_TIMEOUT (so
it also tests that the settings file reaches the lock; the lock runs with an
empty environment), and checks with /dev/vcsu and /dev/vcsa of the lock VT:
  1. the rain shows: the screen changes over time, most text cells are green;
  2. a key shows the auth prompt on top: the user name is on the screen and
     no green cell is left while it shows;
  3. after XSECURELOCK_AUTH_TIMEOUT the rain is back;
  4. cmatrix and saver_matrix hold no fd of the lock VT, the window table,
     /dev/input or the console; the user cannot read /dev/input or the
     lock's memory; the lock VT stays in K_MEDIUMRAW with the lock's termios;
  5. CPU of cmatrix, saver_matrix and the lock over 20 s (reported);
  6. SIGKILL of cmatrix and saver_matrix by the user: still locked, the rain
     comes back (saver_multiplex restarts it), the prompt still works;
  7. wrong password keeps it locked; the right one unlocks (exit 0).
Takes /tmp/vt12-test.lock and refuses to run while a real lock is active.
"""
import collections
import fcntl
import os
import pwd
import shutil
import signal
import struct
import subprocess
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import security as s  # noqa: E402  (its keyboard, VT and process helpers)
from parity import font_shift  # noqa: E402

USER, PASSWORD = s.USER, s.PASSWORD
AUTH_TIMEOUT = 6
KDGKBMODE, K_MEDIUMRAW = 0x4B44, 2
GREEN = (2, 10)

failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}",
          flush=True)
    failures += not ok


def screen(vt):
    """(text cells, colours): the characters and foreground colours (VGA
    order: green is 2 as in ANSI; with a 512-glyph font the kernel drops the
    intensity bit, so bright green shows as 2 and white as 7)."""
    shift = font_shift(vt)
    raw = open(f"/dev/vcsa{vt}", "rb").read()
    rows, cols = raw[0], raw[1]
    cells = raw[4:4 + 2 * rows * cols]
    chars = open(f"/dev/vcsu{vt}", "rb").read()
    text = [chr(c) for c in struct.unpack(f"{rows * cols}I",
                                          chars[:4 * rows * cols])]
    fgs = [(cells[2 * i + 1] >> shift) & (0x07 if shift else 0x0f)
           for i in range(rows * cols)]
    return text, fgs, cols


def rain_stats(vt):
    text, fgs, _ = screen(vt)
    lit = [i for i, c in enumerate(text) if c not in " \0"]
    green = [i for i in lit if fgs[i] in GREEN]
    rain_stats.colours = collections.Counter(fgs[i] for i in lit)
    return text, len(lit), len(green)


def cpu_ticks(pid):
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def pids_named(root, name):
    return [p for p, c in s.children(root) if c == name]


def start_lock():
    os.chown(s.TTY, pwd.getpwnam(USER).pw_uid, -1)
    if s.active_vt() != s.VT:
        subprocess.run(["chvt", str(s.VT)], check=True, timeout=5)
    proc = subprocess.Popen(
        ["env", "-i", "LANG=en_US.UTF-8", "PATH=/usr/local/bin:/usr/bin",
         s.TTYSECURELOCK, "--user", USER], start_new_session=True,
        stderr=open("/tmp/ttysecurelock-saver.stderr", "a"))
    for _ in range(250):
        if s.active_vt() != s.VT:
            break
        time.sleep(0.02)
    return proc


def rain_shows(vt, name):
    t1, lit1, green1 = rain_stats(vt)
    time.sleep(1.0)
    t2, lit2, green2 = rain_stats(vt)
    changed = sum(a != b for a, b in zip(t1, t2))
    check(f"{name}: the rain shows (text cells change)", changed > 20,
          f"{changed} cells changed in 1 s, {lit2} lit")
    # cmatrix: green drops with bold (bright green) cells and a white head.
    others = set(rain_stats.colours) - set(GREEN) - {7, 15}
    check(f"{name}: ... in green", lit2 > 0 and green2 >= 0.5 * lit2 and
          not others, f"{green2} of {lit2} lit cells green; colours "
          f"{dict(rain_stats.colours)}")


def main():
    if os.geteuid() != 0:
        sys.exit("run as root")
    lockf = open("/tmp/vt12-test.lock", "w")
    fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if s.lock_pids():
        sys.exit("a ttysecurelock is running (a real lock?): refusing")
    if shutil.which("cmatrix") is None:
        sys.exit("cmatrix is not installed")
    orig_vt = s.active_vt()
    subprocess.run(["useradd", "-m", "-s", "/usr/bin/nologin", "-c",
                    "ttysecurelock saver test", USER], check=True)
    home = pwd.getpwnam(USER).pw_dir
    kb = None
    proc = None
    try:
        subprocess.run(["chpasswd"], input=f"{USER}:{PASSWORD}", text=True,
                       check=True)
        conf = os.path.join(home, ".config", "ttysecurelock")
        s.as_user("mkdir", "-p", conf)
        s.as_user("sh", "-c", f"printf '%s\\n' '# test settings' "
                  f"'XSECURELOCK_SAVER=saver_matrix' "
                  f"'XSECURELOCK_AUTH_TIMEOUT=\"{AUTH_TIMEOUT}\"' "
                  f"'PATH=/nonexistent' > {conf}/environment")
        kb = s.keyboard()
        proc = start_lock()
        time.sleep(3.0)
        lock_vt = s.active_vt()
        check("locked on a VT from the top", lock_vt >= 13 and
              proc.poll() is None, f"VT {lock_vt}")

        # 1. Rain.
        rain_shows(lock_vt, "idle")

        # 1b. A rotated console (fbcon rotation changes the VT's size): the
        # rain fills the new screen.
        rotate = "/sys/class/graphics/fbcon/rotate"
        if os.path.exists(rotate):
            old = open(rotate).read().strip()
            _, _, cols0 = screen(lock_vt)
            open(rotate, "w").write("1" if old == "0" else "0")
            time.sleep(10.0)  # cmatrix starts its drops again at the top
            text, _, cols1 = screen(lock_vt)
            rows1 = len(text) // cols1
            outside = sum(1 for i, ch in enumerate(text)
                          if ch not in " \0" and (i % cols1 >= cols0 or
                                                   i // cols1 >= rows1 // 2))
            check("rotated console: the rain fills the new size",
                  cols1 != cols0 and outside > 100,
                  f"{cols0} -> {cols1} columns, {outside} lit cells beyond "
                  "the old width or in the lower half")
            open(rotate, "w").write(old)
            time.sleep(3.0)
            rain_shows(lock_vt, "rotated back")

        # 4. Isolation of the saver.
        tree = s.children(proc.pid)
        savers = [(p, c) for p, c in tree if c in ("cmatrix", "saver_matrix")]
        check("cmatrix and saver_matrix run",
              {c for _, c in savers} == {"cmatrix", "saver_matrix"},
              str(savers))
        for pid, comm in savers:
            targets = []
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    targets.append(os.readlink(f"/proc/{pid}/fd/{fd}"))
                except OSError:
                    pass
            bad = [t for t in targets if t.startswith(("/dev/tty", "/dev/input",
                                                      "/dev/vcs", "/memfd:"))
                   or t == "/dev/console"]
            check(f"{comm} holds no fd of the lock VT, table or input",
                  not bad, " ".join(sorted(targets)))
        inputs = sorted(f for f in os.listdir("/dev/input")
                        if f.startswith("event"))
        readable = [f for f in inputs if s.as_user(
            "dd", f"if=/dev/input/{f}", "of=/dev/null", "bs=24", "count=0",
            "status=none").returncode == 0]
        check("the user (and so the saver) cannot open /dev/input",
              not readable, " ".join(readable))
        m = s.as_user("dd", f"if=/proc/{proc.pid}/mem", "of=/dev/null",
                      "bs=1", "count=1", "status=none")
        check("the user cannot read the lock's memory", m.returncode != 0)
        fd = os.open(f"/dev/tty{lock_vt}", os.O_RDONLY | os.O_NOCTTY)
        mode = struct.unpack("i", fcntl.ioctl(fd, KDGKBMODE, b"\0" * 4))[0]
        attrs = termios.tcgetattr(fd)
        os.close(fd)
        check("the lock VT keyboard stays in K_MEDIUMRAW", mode == K_MEDIUMRAW,
              f"mode {mode}")
        check("... with raw termios (no echo, no canonical mode, no signals)",
              not attrs[3] & (termios.ECHO | termios.ICANON | termios.ISIG))

        # 5. CPU.
        watch = {"ttysecurelock": proc.pid}
        for pid, comm in savers:
            watch[comm] = pid
        t0 = {k: cpu_ticks(p) for k, p in watch.items()}
        time.sleep(20.0)
        hz = os.sysconf("SC_CLK_TCK")
        for k, p in watch.items():
            pct = 100.0 * (cpu_ticks(p) - t0[k]) / hz / 20.0
            print(f"INFO  CPU {k}: {pct:.1f}% of one core", flush=True)
            check(f"CPU of {k} under 10% of one core", pct < 10.0,
                  f"{pct:.1f}%")

        # 2. The prompt on top.
        s.chord(kb, s.SHIFT)
        time.sleep(1.5)
        for k in range(3):
            text, lit, green = rain_stats(lock_vt)
            check(f"prompt shown ({k}): the user name is on the screen",
                  USER in "".join(text))
            check(f"prompt shown ({k}): no rain over it", green == 0,
                  f"{green} green cells")
            time.sleep(1.0)

        # 3. Back after the auth timeout.
        time.sleep(AUTH_TIMEOUT + 2)
        check("after the auth timeout the prompt is gone",
              USER not in "".join(screen(lock_vt)[0]))
        rain_shows(lock_vt, "after the auth timeout")

        # 6. Killing the saver.
        # cmatrix first: killing saver_matrix takes cmatrix with it.
        for pid, comm in sorted(savers, key=lambda x: x[1]):
            r = s.as_user("kill", "-KILL", str(pid))
            check(f"the user kills {comm}", r.returncode == 0, r.stderr)
            time.sleep(1.0)
            check("... still locked", s.still_locked(proc, lock_vt))
        time.sleep(4.0)
        rain_shows(lock_vt, "after killing the saver")
        s.chord(kb, s.SHIFT)
        time.sleep(1.5)
        check("... the prompt still shows",
              USER in "".join(screen(lock_vt)[0]))

        # 7. Passwords.
        s.chord(kb, s.KEY_A)
        s.type_text(kb, "1111")
        time.sleep(5.0)
        check("wrong password: still locked", s.still_locked(proc, lock_vt))
        s.chord(kb, s.SHIFT)
        time.sleep(1.0)
        s.type_text(kb, PASSWORD)
        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            rc = None
        time.sleep(0.5)
        check("right password: the lock exits 0", rc == 0, f"exit {rc}")
        check("... back on the locked VT", s.active_vt() == s.VT,
              f"VT {s.active_vt()}")
        time.sleep(1.0)
        left = [l for l in subprocess.run(
            ["pgrep", "-a", "-u", USER], capture_output=True,
            text=True).stdout.splitlines()
            if any(n in l for n in ("cmatrix", "saver_", "pgrp_placeholder"))]
        check("... no saver process left", not left, "; ".join(left))
    finally:
        if kb is not None:
            fcntl.ioctl(kb, s.UI_DEV_DESTROY)
        for pid in s.lock_pids():
            os.kill(pid, signal.SIGKILL)
            time.sleep(0.3)
            s.release_console()
        subprocess.run(["pkill", "-KILL", "-u", USER])
        time.sleep(0.5)
        subprocess.run(["faillock", "--user", USER, "--reset"])
        for _ in range(10):  # tty-i3-keys may run i3blocks for VT 12's owner
            subprocess.run(["pkill", "-KILL", "-u", USER])
            time.sleep(0.3)
            if subprocess.run(["userdel", "-r", USER],
                              stderr=subprocess.DEVNULL).returncode in (0, 6):
                break
        os.chown(s.TTY, 0, -1)
        if s.active_vt() != orig_vt:
            subprocess.run(["chvt", str(orig_vt)], timeout=5)
    print(f"{'FAIL' if failures else 'PASS'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
