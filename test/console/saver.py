#!/usr/bin/python3
"""Test of the console saver channel with saver_matrix on a real VT.

Run as root on a machine nobody is using at the console (it switches VTs):
    sudo python3 test/console/saver.py

Locks VT 12 for a throwaway user whose ~/.config/ttysecurelock/environment
sets XSECURELOCK_SAVER=saver_matrix and XSECURELOCK_AUTH_TIMEOUT=60 (so it
also tests that the settings file reaches the lock; the lock runs with an
empty environment), and checks with /dev/vcsu, /dev/vcsa and /dev/fb0:
  1. the rain shows: the screen changes over time, its cells are in the
     lock's green ramp (palette entries 1-6), each trail goes from its head
     (brightest) up to darker and darker greens, and the framebuffer shows
     the ramp's colours; a rotated console is filled again;
  2. a key shows the auth prompt on top: the user name is on the screen, no
     rain cell is left, and the framebuffer shows the prompt in the
     console's own palette (the ramp undone);
  3. after XSECURELOCK_AUTH_TIMEOUT (60 s) the rain is back;
  4. cmatrix and saver_matrix hold no fd of the lock VT, the window table,
     /dev/input or the console; the user cannot read /dev/input or the
     lock's memory; the lock VT stays in K_MEDIUMRAW with the lock's termios;
  5. CPU of cmatrix, saver_matrix and the lock over 20 s (reported);
  6. SIGKILL of cmatrix and saver_matrix by the user: still locked, the rain
     comes back (saver_multiplex restarts it), the prompt still works;
  7. wrong password keeps it locked; the right one unlocks (exit 0), and the
     console palette (GIO_CMAP) is as before.
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
AUTH_TIMEOUT = 60  # the user's setting
BLANK_TIMEOUT = 120  # upstream's default is 600; shorter for the test
STDERR = "/tmp/ttysecurelock-saver.stderr"
KDGKBMODE, K_MEDIUMRAW = 0x4B44, 2
GIO_CMAP, KDFONTOP = 0x4B70, 0x4B72
# The lock's green ramp (xconsole/display.c): palette entries 1-6 of the lock
# VT while the rain shows, brightest (a trail's head) first.
RAMP = (1, 2, 3, 4, 5, 6)
RAMP_RGB = ((0xd0, 0xff, 0xd0), (0x30, 0xf0, 0x30), (0x10, 0xc0, 0x10),
            (0x08, 0x90, 0x08), (0x04, 0x60, 0x04), (0x02, 0x38, 0x02))
VGA_TO_ANSI = (0, 4, 2, 6, 1, 5, 3, 7)

failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}",
          flush=True)
    failures += not ok


def screen(vt, with_bg=False):
    """(text cells, colours, columns): the characters and the foreground
    colours as ANSI/palette indices (vcsa holds VGA attributes; with a
    512-glyph font the kernel drops the intensity bit, so 0-7 only). One
    read of vcsa for the colours, so a trail is not torn between reads."""
    shift = font_shift(vt)
    raw = open(f"/dev/vcsa{vt}", "rb").read()
    rows, cols = raw[0], raw[1]
    cells = raw[4:4 + 2 * rows * cols]
    chars = open(f"/dev/vcsu{vt}", "rb").read()
    text = [chr(c) for c in struct.unpack(f"{rows * cols}I",
                                          chars[:4 * rows * cols])]

    def ansi(v):
        v = (v >> shift) & (0x07 if shift else 0x0f)
        return VGA_TO_ANSI[v & 7] + (v & 8)
    fgs = [ansi(cells[2 * i + 1]) for i in range(rows * cols)]
    if with_bg:
        bgs = [VGA_TO_ANSI[(cells[2 * i + 1] >> (4 + shift)) & 7]
               for i in range(rows * cols)]
        # Lit from vcsu: with a 512-glyph font a glyph's low byte may be 32.
        lit = [c not in " \0" for c in text]
        return text, fgs, cols, bgs, lit
    return text, fgs, cols


def trails(vt):
    """Per column, each run of lit cells top to bottom: its ramp levels must
    not get darker towards the bottom (the head, level 0). Returns (runs,
    monotonic runs, runs with a head)."""
    _, fgs, cols, _, lit = screen(vt, with_bg=True)
    rows = len(fgs) // cols
    runs = mono = heads = 0
    for c in range(cols):
        r = 0
        while r < rows:
            if not lit[r * cols + c]:
                r += 1
                continue
            start = r
            while r < rows and lit[r * cols + c]:
                r += 1
            lv = [RAMP.index(fgs[i * cols + c]) if fgs[i * cols + c] in RAMP
                  else 99 for i in range(start, r)]
            runs += 1
            mono += all(a >= b for a, b in zip(lv, lv[1:])) and 99 not in lv
            heads += lv[-1] == 0
    return runs, mono, heads


GIO_UNIMAP, KD_FONT_OP_GET_TALL = 0x4B66, 5
HERE = os.path.dirname(os.path.abspath(__file__))
KANJI = set("".join(open(os.path.join(HERE, "..", "..", "xconsole", "rain",
                                      "kanji.txt")).read().split()))
KATAKANA = {chr(c) for c in range(0xFF66, 0xFF9E)}
RAIN_FONT = "/usr/local/share/ttysecurelock/rain-font.otf"


def vt_font(vt):
    """(width, height, charcount, glyph bytes) of a VT's font."""
    import ctypes
    buf = ctypes.create_string_buffer(512 * 128 * 8)
    fd = os.open(f"/dev/tty{vt}", os.O_RDONLY | os.O_NOCTTY)
    try:
        # GET_TALL lays glyphs out with the height asked for: size first.
        op = bytearray(struct.pack("IIIIIP", KD_FONT_OP_GET_TALL, 0, 64, 128,
                                   512, 0))
        fcntl.ioctl(fd, KDFONTOP, op)
        _, _, w, h, n, _ = struct.unpack("IIIIIP", op)
        op = bytearray(struct.pack("IIIIIP", KD_FONT_OP_GET_TALL, 0, w, h,
                                   512, ctypes.addressof(buf)))
        fcntl.ioctl(fd, KDFONTOP, op)
    finally:
        os.close(fd)
    _, _, w, h, n, _ = struct.unpack("IIIIIP", op)
    return w, h, n, buf.raw[:n * h * ((w + 7) // 8)]


def vt_unimap(vt):
    """The VT's Unicode map as a sorted list of (code point, glyph)."""
    import ctypes
    entries = (ctypes.c_ushort * (2 * 65535))()
    desc = bytearray(struct.pack("HxxxxxxP", 65535, ctypes.addressof(entries)))
    fd = os.open(f"/dev/tty{vt}", os.O_RDONLY | os.O_NOCTTY)
    try:
        fcntl.ioctl(fd, GIO_UNIMAP, desc)
    finally:
        os.close(fd)
    n = struct.unpack_from("H", desc)[0]
    return sorted((entries[2 * i], entries[2 * i + 1]) for i in range(n))


def glyph_bits(font, slot):
    w, h, n, data = font
    pitch = (w + 7) // 8
    g = data[slot * h * pitch:(slot + 1) * h * pitch]
    return [[bool(g[y * pitch + x // 8] & (0x80 >> (x % 8))) for x in range(w)]
            for y in range(h)]


def render(ch, w, h):
    """The half-width katakana ch rendered unmirrored the way saver_matrix
    renders it (pixel size 2w, centred, threshold 112), with PIL."""
    from PIL import Image, ImageDraw, ImageFont
    f = ImageFont.truetype(RAIN_FONT, 2 * w)
    l, t, r, b = f.getbbox(ch)
    im = Image.new("L", (r - l, b - t), 0)
    ImageDraw.Draw(im).text((-l, -t), ch, font=f, fill=255)
    out = [[False] * w for _ in range(h)]
    x0, y0 = (w - im.width) // 2, (h - im.height) // 2
    for y in range(im.height):
        for x in range(im.width):
            if 0 <= x0 + x < w and 0 <= y0 + y < h and \
                    im.getpixel((x, y)) >= 112:
                out[y0 + y][x0 + x] = True
    return out


def iou(a, b):
    """Best overlap of a and b moved by up to 2 pixels (PIL and FreeType
    place a glyph a pixel apart now and then)."""
    h, w = len(a), len(a[0])
    best = 0.0
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            m = [[0 <= y - dy < h and 0 <= x - dx < w and b[y - dy][x - dx]
                  for x in range(w)] for y in range(h)]
            best = max(best, iou0(a, m))
    return best


def iou0(a, b):
    inter = sum(x and y for ra, rb in zip(a, b) for x, y in zip(ra, rb))
    union = sum(x or y for ra, rb in zip(a, b) for x, y in zip(ra, rb))
    return inter / union if union else 0.0


def rain_glyphs(vt, name):
    """The rain shows katakana and kanji, from the lock VT's font, mirrored,
    and the framebuffer shows those glyphs in their cells."""
    text, fgs, cols, bgs, lit = screen(vt, with_bg=True)
    font = vt_font(vt)
    umap = dict(vt_unimap(vt))
    # Kanji are written as private-use aliases of their glyph (the console
    # would give a wide character two cells); vcs names the alias: resolve it
    # through the Unicode map to the kanji with the same glyph.
    by_slot = {pos: chr(cp) for cp, pos in umap.items() if chr(cp) in KANJI}
    def resolve(ch):
        if 0xE000 <= ord(ch) < 0xE200 and umap.get(ord(ch)) in by_slot:
            return by_slot[umap[ord(ch)]]
        return ch
    text = [resolve(ch) for ch in text]
    shown = [ch for ch, l in zip(text, lit) if l]
    kata = sum(ch in KATAKANA for ch in shown)
    kan = sum(ch in KANJI for ch in shown)
    dig = sum(ch.isdigit() for ch in shown)
    other = sum(not (ch in KATAKANA or ch in KANJI or ch.isdigit())
                for ch in shown)
    check(f"{name}: the rain is katakana, kanji and digits (vcsu)",
          kata > 50 and kan > 10 and other <= 0.02 * len(shown),
          f"{kata} katakana, {kan} kanji, {dig} digits, {other} other "
          f"{sorted({hex(ord(ch)) for ch in shown if not (ch in KATAKANA or ch in KANJI or ch.isdigit())})[:8]}")
    w, h = font[0], font[1]
    scores = []
    # Full-size, asymmetric katakana (small ones and ｰ ｪ are near-symmetric).
    for ch in "ｦｱｲｳｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃ":
        g = glyph_bits(font, umap[ord(ch)]) if ord(ch) in umap else None
        if g is None:
            scores.append((ch, None, None))
            continue
        plain = render(ch, w, h)
        mirror = [row[::-1] for row in plain]
        scores.append((ch, iou(g, mirror), iou(g, plain)))
    ok = all(m is not None and m >= 0.9 and m > p + 0.1 for _, m, p in scores)
    check(f"{name}: the lock VT's katakana glyphs are mirrored renderings",
          ok, " ".join(f"{c}:{m:.2f}/{p:.2f}" if m is not None else f"{c}:none"
                       for c, m, p in scores))
    # The framebuffer shows the glyph of the cell's character.
    if not os.path.exists("/dev/fb0"):
        return
    stride = int(open("/sys/class/graphics/fb0/stride").read())
    good = bad = 0
    for _ in range(3):
        a = screen(vt, with_bg=True)
        with open("/dev/fb0", "rb") as f:
            fb = f.read()
        b = screen(vt, with_bg=True)
        if not os.path.exists(f"/tmp/saver-rain-{name[:5]}.png"):
            try:  # the screen as it was, for people to look at
                from PIL import Image
                fw, fh = map(int, open("/sys/class/graphics/fb0/virtual_size")
                             .read().split(","))
                Image.frombytes("RGB", (fw, fh), fb[:fh * stride], "raw",
                                "BGRX", stride).save(
                                    f"/tmp/saver-rain-{name[:5]}.png")
            except Exception as e:  # noqa: BLE001
                print(f"INFO  no screenshot: {e}")
        for i, ch in enumerate(a[0]):
            ch = resolve(ch)
            if ch not in KATAKANA and ch not in KANJI:
                continue
            if (resolve(b[0][i]), b[1][i]) != (ch, a[1][i]) or \
                    ord(ch) not in umap:
                continue
            g = glyph_bits(font, umap[ord(ch)])
            r, c = divmod(i, cols)
            bgc = pal0_g[a[3][i]]
            match = True
            for y in range(h):
                o = (r * h + y) * stride + c * w * 4
                for x in range(w):
                    px = (fb[o + 4 * x + 2], fb[o + 4 * x + 1], fb[o + 4 * x])
                    if g[y][x] == (px == bgc):
                        match = False
            good += match
            bad += not match
    check(f"{name}: the framebuffer shows those glyphs (no hole boxes)",
          good > 20 and bad <= 0.05 * (good + bad), f"{good} cells match, "
          f"{bad} do not")


pal0_g = None


def palette():
    fd = os.open("/dev/tty0", os.O_RDONLY | os.O_NOCTTY)
    buf = bytearray(48)
    fcntl.ioctl(fd, GIO_CMAP, buf)
    os.close(fd)
    return [tuple(buf[3 * i:3 * i + 3]) for i in range(16)]


def font_size(vt):
    fd = os.open(f"/dev/tty{vt}", os.O_RDONLY | os.O_NOCTTY)
    op = bytearray(struct.pack("IIIIIP", 1, 0, 64, 128, 512, 0))
    try:
        fcntl.ioctl(fd, KDFONTOP, op)
    finally:
        os.close(fd)
    return struct.unpack("IIIIIP", op)[2:4]


def cell_pixels(fb, idx, cols, cw, ch, stride):
    """The colours a framebuffer snapshot (fbcon, rotation 0, 32 bits per
    pixel) shows in a text cell."""
    r, c = divmod(idx, cols)
    out = set()
    for y in range(r * ch, r * ch + ch):
        o = y * stride + c * cw * 4
        for x in range(cw):
            b, g, rr = fb[o + 4 * x], fb[o + 4 * x + 1], fb[o + 4 * x + 2]
            out.add((rr, g, b))
    return out


def colours_shown(vt, want, name, limit=40, cover=()):
    """Check with the framebuffer that lit cells show only the colours of
    `want(idx, fg, bg)` -> (allowed foreground rgbs, background rgb), and
    one of those foregrounds; `cover`: colours that must all be seen. The
    rain moves: the framebuffer is read between two reads of vcsa, and only
    cells that did not change between those are used."""
    if not os.path.exists("/dev/fb0"):
        print("INFO  no /dev/fb0: colours on the screen not checked")
        return
    cw, ch = font_size(vt)
    stride = int(open("/sys/class/graphics/fb0/stride").read())
    bad, seen, shown = [], 0, set()
    for _ in range(5):
        a = screen(vt, with_bg=True)
        with open("/dev/fb0", "rb") as f:
            fb = f.read()
        b = screen(vt, with_bg=True)
        text, fgs, cols, bgs, _ = a
        # Brightest first: the heads move every frame, few stay put.
        for i in sorted(range(len(text)), key=lambda i: fgs[i]):
            chr_ = text[i]
            if chr_ in " \0" or (b[0][i], b[1][i], b[3][i]) != \
                    (chr_, fgs[i], bgs[i]):
                continue
            exp = want(i, fgs[i], bgs[i])
            if exp is None:
                continue
            got = cell_pixels(fb, i, cols, cw, ch, stride)
            seen += 1
            shown |= got
            if not got <= set(exp[0]) | {exp[1]} or not got & set(exp[0]):
                bad.append(f"{chr_!r} fg {fgs[i]}: {sorted(got)} want {exp}")
            if seen >= limit:
                break
        if seen >= limit:
            break
    missing = [c for c in cover if c not in shown]
    check(name, seen > 0 and not bad and not missing,
          f"{seen} cells; " + "; ".join(bad[:3]) +
          (f"; colours never seen: {missing}" if missing else ""))


def rain_stats(vt):
    text, fgs, _ = screen(vt)
    lit = [i for i, c in enumerate(text) if c not in " \0"]
    green = [i for i in lit if fgs[i] in RAMP]
    rain_stats.colours = collections.Counter(fgs[i] for i in lit)
    return text, len(lit), len(green)


def wait_for(pred, timeout, step=0.5):
    """Poll pred until it holds or timeout seconds pass; its last value."""
    end = time.time() + timeout
    while True:
        v = pred()
        if v or time.time() > end:
            return v
        time.sleep(step)


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
        stderr=open(STDERR, "a"))
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
    check(f"{name}: ... in the green ramp", lit2 > 0 and green2 >= 0.98 * lit2,
          f"{green2} of {lit2} lit cells; colours {dict(rain_stats.colours)}")
    runs, mono, heads = trails(vt)
    check(f"{name}: trails fade from the head up, monotonically",
          runs > 10 and mono >= 0.95 * runs and heads >= 0.9 * runs,
          f"{mono} of {runs} trails monotonic, {heads} with a head")


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
    pal0 = palette()
    global pal0_g
    pal0_g = pal0
    font12, map12 = vt_font(s.VT), vt_unimap(s.VT)
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
                  f"'XSECURELOCK_BLANK_TIMEOUT={BLANK_TIMEOUT}' "
                  f"'PATH=/nonexistent' > {conf}/environment")
        kb = s.keyboard()
        proc = start_lock()
        time.sleep(3.0)
        lock_vt = s.active_vt()
        check("locked on a VT from the top", lock_vt >= 13 and
              proc.poll() is None, f"VT {lock_vt}")

        # 1. Rain.
        rain_shows(lock_vt, "idle")
        colours_shown(
            lock_vt, lambda i, fg, bg: ([RAMP_RGB[RAMP.index(fg)]], pal0[bg])
            if fg in RAMP else None,
            "rain: the screen shows the ramp's six greens (framebuffer)",
            limit=100000, cover=RAMP_RGB)
        rain_glyphs(lock_vt, "idle")
        check("rain: VT 12 keeps its font and Unicode map meanwhile",
              vt_font(s.VT) == font12 and vt_unimap(s.VT) == map12)

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
        wait_for(lambda: USER in "".join(screen(lock_vt)[0]), 5)
        time.sleep(0.5)
        for k in range(3):
            text, lit, green = rain_stats(lock_vt)
            check(f"prompt shown ({k}): the user name is on the screen",
                  USER in "".join(text))
            check(f"prompt shown ({k}): no rain over it", green == 0,
                  f"{green} green cells")
            time.sleep(1.0)

        # With a 512-glyph font vcsa has no intensity bit: fg or fg + 8.
        colours_shown(
            lock_vt, lambda i, fg, bg: ([pal0[fg], pal0[(fg + 8) % 16]],
                                        pal0[bg]),
            "prompt: the console's own colours, the ramp undone "
            "(framebuffer)")
        check("prompt: the lock VT has the console font again (KDFONTOP)",
              vt_font(lock_vt) == font12)
        check("prompt: ... and the console's Unicode map (GIO_UNIMAP)",
              vt_unimap(lock_vt) == map12)

        # 3. Back after the auth timeout.
        time.sleep(AUTH_TIMEOUT - 10)
        check("after the auth timeout the prompt is gone",
              wait_for(lambda: USER not in "".join(screen(lock_vt)[0]), 15))
        time.sleep(1.0)
        rain_shows(lock_vt, "after the auth timeout")
        rain_glyphs(lock_vt, "after the auth timeout")

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

        # 6b. Long use, as on yoga3: the prompt times out, the lock stays
        # idle until XSECURELOCK_BLANK_TIMEOUT blanks the console (and stops
        # the saver), a key brings the prompt, it times out: the rain is
        # back; and once more.
        time.sleep(AUTH_TIMEOUT - 5)
        wait_for(lambda: USER not in "".join(screen(lock_vt)[0]), 15)
        time.sleep(1.0)
        rain_shows(lock_vt, "cycle 1: the prompt timed out")
        mark = os.path.getsize(STDERR)
        time.sleep(BLANK_TIMEOUT + 5)
        log = open(STDERR, errors="replace").read()[mark:]
        check("blank timeout: the console is blanked",
              "xconsole: console blanked" in log and "TIOCLINUX" not in log,
              " | ".join(l[-80:] for l in log.splitlines()
                         if "blank" in l.lower()))
        check("... the saver is stopped meanwhile",
              not pids_named(proc.pid, "cmatrix"))
        check("... and the lock VT stays in front", s.still_locked(proc,
                                                                  lock_vt))
        for cycle in (2, 3):
            mark = os.path.getsize(STDERR)
            s.chord(kb, s.SHIFT)
            check(f"cycle {cycle}: a key shows the prompt (within 5 s)",
                  wait_for(lambda: USER in "".join(screen(lock_vt)[0]), 5))
            if cycle == 2:
                log = open(STDERR, errors="replace").read()[mark:]
                check("cycle 2: the key unblanked the console",
                      "console unblanked (key input)" in log)
            time.sleep(AUTH_TIMEOUT - 5)
            check(f"cycle {cycle}: the prompt timed out (within "
                  f"{AUTH_TIMEOUT + 10} s)",
                  wait_for(lambda: USER not in "".join(screen(lock_vt)[0]),
                           15))
            time.sleep(1.0)
            rain_shows(lock_vt, f"cycle {cycle}: the prompt timed out")
            rain_glyphs(lock_vt, f"cycle {cycle}")
            log = open(STDERR, errors="replace").read()[mark:]
            print("INFO  log: " + " | ".join(
                l.split("xsecurelock: ")[-1][:70] for l in log.splitlines()),
                flush=True)
        # Each prompt that timed out was a failed PAM authentication: with
        # pam_faillock (deny=3 by default) the account is now locked for a
        # while, and the right password would be refused. Show it, then reset.
        fl = subprocess.run(["faillock", "--user", USER], capture_output=True,
                            text=True).stdout
        print("INFO  faillock after the timeouts: " + " / ".join(
            l.strip() for l in fl.splitlines()[1:]), flush=True)
        subprocess.run(["faillock", "--user", USER, "--reset"])
        s.chord(kb, s.SHIFT)
        time.sleep(1.5)

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
        check("... the console palette (GIO_CMAP) is as before", palette() == pal0)
        check("... VT 12's font and Unicode map are as before",
              vt_font(s.VT) == font12 and vt_unimap(s.VT) == map12)
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
            if proc is not None and pid == proc.pid:
                proc.wait()  # a zombie would still own the VT
            time.sleep(0.3)
            try:
                s.release_console()
            except subprocess.TimeoutExpired:
                print("INFO  chvt timed out")
        subprocess.run(["pkill", "-KILL", "-u", USER])
        time.sleep(0.5)
        subprocess.run(["faillock", "--user", USER, "--reset"])
        # A lock killed while blanked leaves the console dark: unblank.
        fd0 = os.open("/dev/tty0", os.O_RDWR | os.O_NOCTTY)
        fcntl.ioctl(fd0, 0x541C, bytes([4]))  # TIOCLINUX, TIOCL_UNBLANKSCREEN
        os.close(fd0)
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
