#!/usr/bin/python3
"""End-to-end test of ttysecurelock on the real console. Run as root
(sudo make test) from a desktop or console session.

A throwaway user with a known password owns VT 12; from there the lock is
started the way a hotkey daemon would. A uinput keyboard then tries every
way out (Ctrl+Alt+F1, root's VT_ACTIVATE, Ctrl+C, signals from the user),
types a wrong password, then the right one. The real keyboards are grabbed
meanwhile, so what the person at the machine types does not mix in. The
test user is deleted afterwards and the original VT restored.
"""
import errno
import fcntl
import glob
import os
import pwd
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = os.path.join(HERE, "..", "ttysecurelock")
AUTHPROTO = os.path.join(HERE, "..", "authproto_pam")
VT = 12
TEST_USER = "ttysecurelocktest"
PASSWORD = "73914286"  # digits: the same in the Latin and Cyrillic layouts

EV_SYN, EV_KEY = 0, 1
KEY_A, KEY_C, KEY_ENTER, KEY_F1 = 30, 46, 28, 59
KEY_LEFTCTRL, KEY_LEFTALT = 29, 56
DIGIT_KEYS = {str(d % 10): 1 + d for d in range(1, 11)}  # KEY_1 = 2 .. KEY_0 = 11
KEYS = (KEY_A, KEY_C, KEY_ENTER, KEY_F1, KEY_LEFTCTRL, KEY_LEFTALT,
        *DIGIT_KEYS.values())
UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
VT_ACTIVATE = 0x5606
EVENT = struct.Struct("llHHi")


def grab_keyboards():
    """Grab the real keyboards for the test, so what the person at the
    machine types does not mix into the VT under test. The grabs end when
    the fds close, at the latest when this process exits."""
    fds = []
    for path in glob.glob("/dev/input/event*"):
        # Virtual (uinput) keyboards belong to other tests: leave them.
        sysfs = os.path.realpath(f"/sys/class/input/{os.path.basename(path)}")
        if "/devices/virtual/" in sysfs:
            continue
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            bits = bytearray(96)
            fcntl.ioctl(fd, 0x80604521, bits)       # EVIOCGBIT(EV_KEY, 96)
            if bits[KEY_A // 8] >> (KEY_A % 8) & 1:
                fcntl.ioctl(fd, 0x40044590, 1)      # EVIOCGRAB
                fds.append(fd)
            else:
                os.close(fd)
        except OSError:
            pass
    return fds


def uinput():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for k in KEYS:
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("4H80sI", 3, 1, 1, 1,
                                              b"ttysecurelock test keyboard", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def emit(fd, code, value):
    os.write(fd, EVENT.pack(0, 0, EV_KEY, code, value))
    os.write(fd, EVENT.pack(0, 0, EV_SYN, 0, 0))
    time.sleep(0.08)


def chord(kb, *keys):
    for k in keys:
        emit(kb, k, 1)
    for k in reversed(keys):
        emit(kb, k, 0)


def type_text(kb, text):
    for ch in text:
        chord(kb, DIGIT_KEYS[ch])
    chord(kb, KEY_ENTER)


def active_vt():
    return int(open("/sys/class/tty/tty0/active").read().strip()[3:])


def allocated(vt):
    """Has the kernel allocated VT `vt`? (VT_GETSTATE's bitmask covers only
    VTs below 16; /dev/vcsN of a free VT fails with ENXIO or is gone.)"""
    try:
        os.close(os.open(f"/dev/vcs{vt}", os.O_RDONLY))
        return True
    except OSError as e:
        if e.errno in (errno.ENXIO, errno.ENOENT):
            return False
        raise


def wait(predicate, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.1)
    return predicate()


def check(name, got, expected):
    ok = got == expected
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
    if not ok:
        print(f"  expected {expected!r}\n  got      {got!r}")
    return 0 if ok else 1


def main():
    # VT tests of other projects use VT 12 and grab keyboards too: one at a
    # time, by the shared lock file.
    lockfile = open("/tmp/vt12-test.lock", "w")
    fcntl.flock(lockfile, fcntl.LOCK_EX)
    original = active_vt()
    subprocess.run(["useradd", "-M", "-s", "/usr/bin/nologin", "-c",
                    "ttysecurelock test, deleted after the test", TEST_USER],
                   check=True)
    failures = 0
    lock = None
    grabs = []
    kb = None
    try:
        subprocess.run(["chpasswd"], input=f"{TEST_USER}:{PASSWORD}",
                       text=True, check=True)
        test_user = pwd.getpwnam(TEST_USER)
        # VT 12: the test user's console, as after a login there.
        subprocess.run(["openvt", "-c", str(VT), "-f", "--", "sleep", "60"],
                       check=True)
        os.chown(f"/dev/tty{VT}", test_user.pw_uid, -1)
        subprocess.run(["chvt", str(VT)], check=True)
        grabs = grab_keyboards()
        kb = uinput()
        time.sleep(0.5)

        # Started as a hotkey daemon would: root, no --user (owner of VT 12).
        lock = subprocess.Popen([LOCK, "--authproto", AUTHPROTO])
        failures += check("lock VT in front",
                          wait(lambda: active_vt() != VT, 5), True)
        lock_vt = active_vt()
        # Above logind's autovt range, where a getty would hang it up.
        failures += check("lock VT taken from the top (>= 13)", lock_vt >= 13,
                          True)

        chord(kb, KEY_LEFTCTRL, KEY_LEFTALT, KEY_F1)
        with open("/dev/tty0", "rb") as tty0:
            try:
                fcntl.ioctl(tty0, VT_ACTIVATE, VT)
            except OSError:
                pass
        chord(kb, KEY_LEFTCTRL, KEY_C)
        for sig in ("HUP", "INT", "TERM", "KILL"):
            subprocess.run(["setpriv", f"--reuid={test_user.pw_uid}",
                            f"--regid={test_user.pw_gid}", "--clear-groups",
                            "kill", f"-{sig}", str(lock.pid)],
                           capture_output=True)
        time.sleep(1.0)
        failures += check("Ctrl+Alt+F1, root VT_ACTIVATE, Ctrl+C, the user's "
                          "signals: still locked",
                          (active_vt(), lock.poll()), (lock_vt, None))

        # The helper runs as the user, who can SIGSTOP it: the lock must
        # notice, kill it and ask again, not hang without a prompt.
        def helpers():
            return subprocess.run(["pgrep", "-u", TEST_USER, "-f", "authproto_pam"],
                                  capture_output=True, text=True).stdout.split()
        first = helpers()
        subprocess.run(["setpriv", f"--reuid={test_user.pw_uid}",
                        f"--regid={test_user.pw_gid}", "--clear-groups",
                        "kill", "-STOP", *first], capture_output=True)
        replaced = wait(lambda: helpers() and helpers() != first, 5)
        failures += check("helper SIGSTOPped by the user: replaced, still locked",
                          (bool(first), replaced, active_vt(), lock.poll()),
                          (True, True, lock_vt, None))

        type_text(kb, "11111111")
        time.sleep(4.0)  # PAM's delay after a failure
        failures += check("wrong password: still locked",
                          (active_vt(), lock.poll()), (lock_vt, None))

        type_text(kb, PASSWORD)
        try:
            code = lock.wait(15)
        except subprocess.TimeoutExpired:
            code = None
        time.sleep(0.5)
        failures += check("right password: unlocked, VT 12 back",
                          (code, active_vt()), (0, VT))
        failures += check("lock VT freed", allocated(lock_vt), False)
    finally:
        if lock and lock.poll() is None:
            lock.kill()
            lock.wait()
            with open("/dev/tty0", "rb") as tty0:
                fcntl.ioctl(tty0, 0x560C, 0)       # VT_UNLOCKSWITCH
        if kb is not None:
            fcntl.ioctl(kb, UI_DEV_DESTROY)
        for fd in grabs:
            os.close(fd)
        subprocess.run(["chvt", str(original)])
        subprocess.run(["pkill", "-u", TEST_USER])
        subprocess.run(["userdel", TEST_USER])
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
