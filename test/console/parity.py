#!/usr/bin/python3
"""Parity test: xsecurelock 1.9.0 under X against ttysecurelock on the console.

Run as root on a machine nobody is using at the console (it switches VTs):
    sudo python3 test/console/parity.py [--only NAME] [--out DIR]

For each scenario the same evdev key sequence is played twice:
  X:        the installed xsecurelock (/usr/bin/xsecurelock) under Xorg with
            the user's XKB setup (~/.config/i3/config: us,ru, grp:caps_toggle,
            caps:none), keys through XTEST (xkey), every string auth_x11 draws
            logged by an LD_PRELOAD shim (xshim.so);
  console:  the installed ttysecurelock locking VT 12 (owned for the test by
            a throwaway user), keys through a uinput keyboard, the screen read
            from /dev/vcsu<N> and /dev/vcsa<N> every 20 ms.
Both runs get the same environment and the same rand() (randshim.so). A frame
is the list of non-empty text lines of the auth screen, top to bottom, each
with its warning flag (drawn in XSECURELOCK_AUTH_WARNING_COLOR). Compared:
the sequence of distinct frames of the whole scenario and the frame at the end
of each step. Positions are not compared (pixels against text cells).

The throwaway user is created and deleted here; its faillock record is reset.
The X server is a real Xorg on VT 7 (Xvfb cannot change its keymap).
Takes /tmp/vt12-test.lock and refuses to run while a real lock is active.
"""
import argparse
import fcntl
import glob
import json
import os
import pwd
import re
import signal
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XSECURELOCK = "/usr/bin/xsecurelock"
TTYSECURELOCK = "/usr/local/bin/ttysecurelock"
VT = 12
TTY = f"/dev/tty{VT}"
USER = "ttyslparity"
PASSWORD = "abпароль"
XDISPLAY = ":5"
XVT = 7
XKB = [["-layout", "us,ru", "-option", "", "-option", "grp:caps_toggle",
        "-option", "caps:none"]]

# evdev keycodes
ESC, BS, TAB, ENTER, CTRL, SHIFT, CAPS, NUM = 1, 14, 15, 28, 29, 42, 58, 69
KEY = {c: k for c, k in zip("qwertyuiop", range(16, 26))}
KEY.update({c: k for c, k in zip("asdfghjkl", range(30, 39))})
KEY.update({c: k for c, k in zip("zxcvbnm", range(44, 51))})
KEY.update({str(d % 10): 1 + d for d in range(1, 11)})
ALL_KEYS = sorted(set(KEY.values()) | {ESC, BS, TAB, ENTER, CTRL, SHIFT, CAPS,
                                       NUM})

BASE_ENV = {
    "LANG": "en_US.UTF-8",
    "PATH": "/usr/local/bin:/usr/bin",
    # Deterministic screens: no blinking, no random burn-in moves.
    "XSECURELOCK_AUTH_CURSOR_BLINK": "0",
    "XSECURELOCK_BURNIN_MITIGATION_DYNAMIC": "0",
}


def tap(code):
    return [(code, 1), (code, 0)]


def chord(mod, code):
    return [(mod, 1), (code, 1), (code, 0), (mod, 0)]


def text(s):
    ev = []
    for c in s:
        ev += tap(KEY[c])
    return ev


def password_keys():
    # "ab" in the Latin layout, "пароль" (keys g f h j k m) in the Russian
    # one, back to Latin, Return.
    return (text("ab") + tap(CAPS) + text("gfhjkm") + tap(CAPS) + tap(ENTER))


def prompt_steps():
    return [
        ("a", tap(KEY["a"])),
        ("b", tap(KEY["b"])),
        ("backspace", tap(BS)),
        ("caps: russian", tap(CAPS)),
        ("f in russian", tap(KEY["f"])),
        ("caps: latin", tap(CAPS)),
        ("shift+caps: caps lock on", chord(SHIFT, CAPS)),
        ("g with caps lock", tap(KEY["g"])),
        ("shift+caps: caps lock off", chord(SHIFT, CAPS)),
        ("num lock on", tap(NUM)),
        ("num lock off", tap(NUM)),
        ("ctrl+tab", chord(CTRL, TAB)),
        ("ctrl+tab again", chord(CTRL, TAB)),
        ("ctrl+backspace", chord(CTRL, BS)),
        ("x", tap(KEY["x"])),
        ("escape", tap(ESC), 4.0),  # auth_x11 takes ~2 s to go
        ("wake", tap(SHIFT), 2.0),  # the key that wakes the prompt is not typed
        ("password", password_keys(), 4.0),
    ]


MODES = ["cursor", "asterisks", "hidden", "disco", "emoji", "emoticon",
         "kaomoji", "time", "time_hex"]


def scenarios():
    out = []
    for mode in MODES:
        out.append({"name": f"prompt-{mode}",
                    "env": {"XSECURELOCK_PASSWORD_PROMPT": mode},
                    "steps": prompt_steps()})
    out.append({"name": "initial-russian", "env": {}, "initial_group": 1,
                "steps": [("a", tap(KEY["a"])),
                          ("caps", tap(CAPS)),
                          ("password", text("ab") + tap(CAPS) +
                           text("gfhjkm") + tap(CAPS) + tap(ENTER), 4.0)]})
    out.append({"name": "datetime", "env": {"XSECURELOCK_SHOW_DATETIME": "1"},
                "steps": [("a", tap(KEY["a"])),
                          ("password", password_keys(), 4.0)]})
    out.append({"name": "auth-timeout", "env": {"XSECURELOCK_AUTH_TIMEOUT": "3"},
                "steps": [("a", tap(KEY["a"])), ("wait", [], 6.0),
                          ("b", tap(KEY["b"])),
                          ("password", password_keys(), 4.0)]})
    wrong = text("wrong") + tap(ENTER)
    out.append({"name": "pam-failures",
                "env": {"XSECURELOCK_PASSWORD_PROMPT": "asterisks"},
                "steps": [("wrong 1", wrong, 6.0), ("wrong 2", wrong, 6.0),
                          ("wrong 3", wrong, 6.0),
                          ("right while faillocked", password_keys(), 6.0),
                          ("faillock reset", [], 1.0, "reset"),
                          ("right", password_keys(), 4.0)]})
    out.append({"name": "blink", "env": {"XSECURELOCK_AUTH_CURSOR_BLINK": "1"},
                "steps": [("a", tap(KEY["a"])), ("wait", [], 3.0),
                          ("password", password_keys(), 4.0)],
                "blink": True})
    return out


def faillock_reset():
    subprocess.run(["faillock", "--user", USER, "--reset"], check=False)


# --- Frames -----------------------------------------------------------------

def normalize(frame, scenario):
    """Values that legitimately differ between two runs: time stamps."""
    out = []
    for t, w in frame:
        if scenario["name"] in ("datetime", "prompt-time", "prompt-time_hex"):
            t = re.sub(r"0x[0-9a-f]+", "0xH", t)
            t = re.sub(r"\d", "D", t)
        if scenario.get("blink"):
            t = re.sub(r"[|-]", "C", t)
        if scenario["name"] == "prompt-disco" and "♪" in t:
            # Upstream's disco prompt claims a length past what it builds
            # (auth_x11.c: stride * DANCERS * strlen(dancer)) and so draws
            # whatever follows in its stack buffer: compare what it builds.
            t = t[:t.rindex("♪") + 1]
        out.append((t, w))
    return out


def dedup(frames):
    out = []
    for t, f in frames:
        if not out or out[-1][1] != f:
            out.append((t, f))
    return out


# --- X ----------------------------------------------------------------------

def x_frames(log):
    """Frames from the shim log: each CLEAR of the auth window starts a
    frame made of the strings drawn after it; auth_x11 exiting blanks the
    screen."""
    frames, cur = [], None

    def flush():
        if cur is not None:
            t, items = cur
            frames.append((t, [(s, w) for _, s, w in
                               sorted(items, key=lambda i: i[0])]))

    for line in open(log, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split(" ", 6)
        t = int(parts[0])
        if parts[1] == "CLEAR":
            flush()
            cur = (t, [])
        elif parts[1] == "EXIT" and parts[2] == "auth_x11":
            flush()
            cur = None
            frames.append((t, []))
        elif parts[1] == "TEXT" and cur is not None:
            y, rgb = int(parts[4]), parts[5]
            s = (parts[6] if len(parts) > 6 else "").strip()
            if s:
                cur[1].append((y, s, rgb == "255,0,0"))
    flush()
    return frames


def run_x(scenario, outdir):
    log = os.path.join(outdir, f"x-{scenario['name']}.log")
    open(log, "w").close()
    os.chmod(log, 0o666)
    # A real Xorg on a spare VT: Xvfb (xorg-server 21.1.25) ignores every
    # keymap change (setxkbmap, xkbcomp, even xmodmap), so it cannot have
    # the user's two layouts.
    xvfb = subprocess.Popen(["Xorg", XDISPLAY, f"vt{XVT}", "-nolisten", "tcp",
                             "-ac", "-noreset", "-logfile",
                             os.path.join(outdir, "xorg.log")],
                            stderr=subprocess.DEVNULL)
    time.sleep(4)
    env = dict(BASE_ENV, DISPLAY=XDISPLAY)
    for args in XKB:
        subprocess.run(["setxkbmap", *args], env=env, check=True)
    if scenario.get("initial_group"):
        subprocess.run([os.path.join(outdir, "xkey"), f"{CAPS}:1", f"{CAPS}:0"],
                       env=env, check=True)
    env.update(scenario["env"])
    env.update(HOME="/tmp", USER=USER, LOGNAME=USER, XSHIM_LOG=log,
               LD_PRELOAD=f"{outdir}/xshim.so {outdir}/randshim.so")
    os.chmod(outdir, 0o755)
    proc = subprocess.Popen(["runuser", "-u", USER, "--", "env", "-i",
                             *[f"{k}={v}" for k, v in env.items()],
                             XSECURELOCK],
                            stderr=open(log + ".stderr", "w"))
    time.sleep(1.5)
    steps = play(scenario, lambda evs: subprocess.run(
        [os.path.join(outdir, "xkey"), *[f"{c}:{d}" for c, d in evs]],
        env=dict(BASE_ENV, DISPLAY=XDISPLAY), check=True))
    try:
        rc = proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = "still running"
    xvfb.terminate()
    xvfb.wait()
    chvt(VT)
    frames = x_frames(log) if os.path.exists(log) else []
    return {"frames": frames, "steps": steps, "exit": rc}


def play(scenario, send):
    """Play the steps; returns [(name, start_ms, end_ms)]."""
    out = []
    for step in scenario["steps"]:
        name, events = step[0], step[1]
        wait = step[2] if len(step) > 2 else 1.0
        start = int(time.time() * 1000)
        if len(step) > 3 and step[3] == "reset":
            faillock_reset()
        if events:
            send(events)
        sent = int(time.time() * 1000)
        time.sleep(wait)
        out.append((name, start, int(time.time() * 1000), sent))
    return out


# --- Console ----------------------------------------------------------------

UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_SET_LEDBIT = 0x40045569
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
UI_GET_SYSNAME_64 = 0x8040552C
EV_SYN, EV_KEY, EV_LED = 0, 1, 0x11
EVENT = struct.Struct("llHHi")
KDSKBLED, KDGKBLED = 0x4B65, 0x4B64


def uinput_keyboard():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for k in ALL_KEYS:
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    setup = struct.pack("HHHH80sI", 0x06, 1, 1, 1,
                        b"ttysecurelock parity keyboard", 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.0)
    return fd


def emit(fd, events):
    for code, down in events:
        os.write(fd, EVENT.pack(0, 0, EV_KEY, code, down))
        os.write(fd, EVENT.pack(0, 0, EV_SYN, 0, 0))
        time.sleep(0.02)


def altgr_lock():
    """The AltGr lock of the console in front, through the kbd-altgrlock LED
    trigger of a uinput LED (as the lock reads it)."""
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_LED)
    fcntl.ioctl(fd, UI_SET_LEDBIT, 1)
    setup = struct.pack("HHHH80sI", 0x06, 1, 1, 1, b"parity layout probe", 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    buf = bytearray(64)
    fcntl.ioctl(fd, UI_GET_SYSNAME_64, buf)
    name = buf.split(b"\0")[0].decode()
    base = f"/sys/class/leds/{name}::capslock"
    for _ in range(100):
        if os.path.exists(base + "/trigger"):
            break
        time.sleep(0.01)
    open(base + "/trigger", "w").write("kbd-altgrlock")
    value = int(open(base + "/brightness").read())
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
    return 1 if value else 0


def active_vt():
    return int(open("/sys/class/tty/tty0/active").read().strip()[3:])


def chvt(n):
    subprocess.run(["chvt", str(n)], check=True)
    for _ in range(100):
        if active_vt() == n:
            return
        time.sleep(0.02)
    raise RuntimeError(f"could not switch to VT {n}")


KDFONTOP = 0x4B72


def font_shift(vt):
    """With a 512-glyph font the kernel keeps the 9th glyph bit in the
    attribute and shifts the colours left by one (con_build_attr)."""
    fd = os.open(f"/dev/tty{vt}", os.O_RDONLY | os.O_NOCTTY)
    op = bytearray(struct.pack("IIIIIP", 1, 0, 64, 128, 512, 0))
    try:
        fcntl.ioctl(fd, KDFONTOP, op)
    except OSError:
        pass
    os.close(fd)
    return 1 if struct.unpack("IIIIIP", op)[4] > 256 else 0


def screen(vt, shift=0):
    attrs = open(f"/dev/vcsa{vt}", "rb").read()
    rows, cols = attrs[0], attrs[1]
    cells = open(f"/dev/vcsu{vt}", "rb").read()
    lines = []
    for r in range(rows):
        s, warn = "", False
        for c in range(cols):
            i = r * cols + c
            ch = chr(struct.unpack_from("<I", cells, 4 * i)[0])
            s += ch
            # vcsa attributes are VGA colours: red is 4 there (ANSI 1).
            fg = (attrs[4 + 2 * i + 1] >> shift) & 0x07
            if ch != " " and fg == 4:
                warn = True
        if s.strip():
            lines.append((s.strip(), warn))
    return lines


class Poller(threading.Thread):
    def __init__(self, vt):
        super().__init__(daemon=True)
        self.vt, self.frames, self.stop = vt, [], False

    def run(self):
        last, stable_since = None, None
        shift = font_shift(self.vt)
        while not self.stop:
            try:
                f = screen(self.vt, shift)
            except OSError:
                break           # the lock VT is gone
            t = int(time.time() * 1000)
            if f != last:
                last, stable_since = f, t
            elif stable_since is not None:
                # Stable for two polls: a whole frame, not one being drawn.
                if not self.frames or self.frames[-1][1] != f:
                    self.frames.append((stable_since, f))
                stable_since = None
            time.sleep(0.02)


def lock_running():
    out = []
    for comm in glob.glob("/proc/[0-9]*/comm"):
        try:
            if open(comm).read().strip() == "ttysecurelock":
                out.append(int(comm.split("/")[2]))
        except OSError:
            pass
    return out


def run_console(scenario, outdir, kb):
    uid = pwd.getpwnam(USER).pw_uid
    os.chown(TTY, uid, -1)
    chvt(VT)
    # Start from the same keyboard state as the X server: no LEDs, Latin
    # (or Russian if the scenario says so).
    fd = os.open(TTY, os.O_RDONLY | os.O_NOCTTY)
    fcntl.ioctl(fd, KDSKBLED, 0)
    os.close(fd)
    want = scenario.get("initial_group", 0)
    if altgr_lock() != want:
        emit(kb, tap(CAPS))
        time.sleep(0.2)
    initial = altgr_lock()
    env = dict(BASE_ENV)
    env.update(scenario["env"])
    env["LD_PRELOAD"] = f"{outdir}/randshim.so"
    log = os.path.join(outdir, f"console-{scenario['name']}.stderr")
    proc = subprocess.Popen(["env", "-i",
                             *[f"{k}={v}" for k, v in env.items()],
                             TTYSECURELOCK, "--user", USER],
                            stderr=open(log, "w"), start_new_session=True)
    for _ in range(250):
        if active_vt() != VT:
            break
        time.sleep(0.02)
    lock_vt = active_vt()
    if lock_vt == VT:
        proc.kill()
        return {"error": "the lock did not take a VT", "initial_group": initial}
    poller = Poller(lock_vt)
    poller.start()
    time.sleep(1.5)
    steps = play(scenario, lambda evs: emit(kb, evs))
    try:
        rc = proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rc = "still running"
    poller.stop = True
    poller.join()
    back = active_vt()
    freed = not os.path.exists(f"/dev/vcs{lock_vt}")
    if not freed:
        try:
            os.close(os.open(f"/dev/vcs{lock_vt}", os.O_RDONLY))
        except OSError:
            freed = True
    if rc == "still running":
        # Must not leave a locked console behind: SIGTERM is not an unlock,
        # so give it the password again before giving up.
        emit(kb, password_keys())
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            # The lock fails closed when killed; this test machine is then
            # released by hand (root's VT_UNLOCKSWITCH), never a user's.
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            fd = os.open("/dev/tty0", os.O_RDWR)
            fcntl.ioctl(fd, 0x560C, 1)      # VT_UNLOCKSWITCH
            os.close(fd)
            chvt(VT)
    return {"frames": poller.frames, "steps": steps, "exit": rc,
            "back_to_vt": back, "lock_vt": lock_vt, "lock_vt_freed": freed,
            "initial_group": initial}


# --- Comparison -------------------------------------------------------------

def at(frames, t):
    f = []
    for ft, fr in frames:
        if ft <= t:
            f = fr
    return f


def compare(scenario, x, c):
    problems = []
    if "error" in c:
        return [c["error"]]
    xn = [(t, normalize(f, scenario)) for t, f in x["frames"]]
    cn = [(t, normalize(f, scenario)) for t, f in c["frames"]]
    xs = [f for _, f in dedup(xn)]
    cs = [f for _, f in dedup(cn)]
    report = []
    for (name, _, xend, xsent), (_, _, cend, csent) in zip(x["steps"],
                                                         c["steps"]):
        # The frame at the end of the step, and the frames the program
        # showed in reaction once the step's keys were all in (frames while
        # keys are still arriving depend on timing, not on the program).
        xf, cf = at(xn, xend - 50), at(cn, cend - 50)
        xr = [f for _, f in dedup([(xsent, at(xn, xsent))] +
                                  [(t, f) for t, f in xn
                                   if xsent < t < xend - 50])]
        cr = [f for _, f in dedup([(csent, at(cn, csent))] +
                                  [(t, f) for t, f in cn
                                   if csent < t < cend - 50])]
        if scenario.get("blink"):
            xr, cr = sorted(set(map(str, xr))), sorted(set(map(str, cr)))
        ok = xf == cf and xr == cr
        report.append({"step": name, "ok": ok, "x": xf, "console": cf,
                       "x_reaction": xr, "console_reaction": cr})
        if xf != cf:
            problems.append(f"step '{name}': X {xf} != console {cf}")
        elif xr != cr:
            problems.append(f"step '{name}': reactions differ: X {xr} != "
                            f"console {cr}")
    if x["exit"] != 0:
        problems.append(f"X xsecurelock exit {x['exit']}")
    if c["exit"] != 0:
        problems.append(f"ttysecurelock exit {c['exit']}")
    if c.get("back_to_vt") != VT:
        problems.append(f"back on VT {c.get('back_to_vt')}, not {VT}")
    if not c.get("lock_vt_freed"):
        problems.append(f"lock VT {c.get('lock_vt')} not freed")
    if c.get("initial_group") != scenario.get("initial_group", 0):
        problems.append("could not set up the initial console layout")
    return problems, report, xs, cs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", action="append")
    ap.add_argument("--out", default="/tmp/ttysecurelock-parity")
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("run as root")
    os.makedirs(args.out, exist_ok=True)
    lockf = open("/tmp/vt12-test.lock", "w")
    fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if lock_running():
        sys.exit("a ttysecurelock is running (a real lock?): refusing")
    for src, out, libs in (("xshim.c", "xshim.so", ["-lXft", "-lX11", "-ldl"]),
                           ("randshim.c", "randshim.so", []),
                           ("xkey.c", "xkey", ["-lXtst", "-lX11"])):
        shared = ["-shared", "-fPIC"] if out.endswith(".so") else []
        subprocess.run(["cc", "-O2", *shared, "-I/usr/include/freetype2",
                        "-o", os.path.join(args.out, out),
                        os.path.join(HERE, src), *libs], check=True)
    orig_vt = active_vt()
    subprocess.run(["useradd", "-M", "-s", "/usr/bin/nologin", "-c",
                    "ttysecurelock parity test", USER], check=True)
    results = {}
    failed = 0
    kb = None
    try:
        subprocess.run(["chpasswd"], input=f"{USER}:{PASSWORD}", text=True,
                       check=True)
        kb = uinput_keyboard()
        for sc in scenarios():
            if args.only and sc["name"] not in args.only:
                continue
            faillock_reset()
            x = run_x(sc, args.out)
            faillock_reset()
            c = run_console(sc, args.out, kb)
            faillock_reset()
            res = compare(sc, x, c)
            if isinstance(res, list):
                problems, report, xs, cs = res, [], [], []
            else:
                problems, report, xs, cs = res
            results[sc["name"]] = {"problems": problems, "steps": report,
                                   "x_frames": xs, "console_frames": cs}
            print(f"{'PASS' if not problems else 'FAIL'} {sc['name']}",
                  flush=True)
            for p in problems:
                print(f"    {p}", flush=True)
            failed += bool(problems)
    finally:
        if kb is not None:
            fcntl.ioctl(kb, UI_DEV_DESTROY)
        faillock_reset()
        subprocess.run(["pkill", "-KILL", "-u", USER], check=False)
        time.sleep(0.5)
        subprocess.run(["userdel", USER], check=False)
        os.chown(TTY, 0, -1)
        if active_vt() != orig_vt:
            chvt(orig_vt)
        with open(os.path.join(args.out, "results.json"), "w") as f:
            json.dump(results, f, ensure_ascii=False, indent=1)
    print(f"{len(results) - failed}/{len(results)} scenarios identical")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
