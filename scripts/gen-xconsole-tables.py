#!/usr/bin/python3
"""Generate xconsole/xtables.inc: the X data xconsole needs, copied from X's
own sources instead of written by hand.

Usage: scripts/gen-xconsole-tables.py KEYMAP.xkb KEYSYMDEF.h XF86KEYSYM.h \
           OSCOLOR.c xconsole/xkeys.inc xconsole/xcolors.inc

  KEYMAP.xkb    the keymap an X server compiled for the user's setup
                (`xkbcomp -xkb $DISPLAY` under Xorg after the user's
                setxkbmap; test/console/user-keymap.xkb): the keysyms, key
                types and autorepeat of every key that is not a layout key;
  KEYSYMDEF.h   /usr/include/X11/keysymdef.h: keysym names and the keysym X
                uses for a Unicode character (the "U+XXXX" comments);
  XF86KEYSYM.h  /usr/include/X11/XF86keysym.h: the XF86 keysym names;
  OSCOLOR.c     xserver's os/oscolor.c (the release the user's Xorg runs):
                the colour names the server resolves.
"""
import re
import sys

(keymap_path, keysymdef_path, xf86_path, oscolor_path, keys_out,
 colors_out) = sys.argv[1:7]
keymap = open(keymap_path).read()

# --- Keysym names ------------------------------------------------------------
names = {}       # name -> value
order = []       # (value, name) in header order
unicode_ks = {}  # code point -> keysym (non-deprecated mapping, first one)
define = re.compile(r"^#define (XK|XF86XK)_([A-Za-z0-9_]+)\s+0x([0-9a-fA-F]+)"
                    r"\s*(/\*(.*)\*/)?")
for path in (keysymdef_path, xf86_path):
    for line in open(path):
        m = define.match(line)
        if not m:
            continue
        prefix, name, value, comment = (m.group(1), m.group(2),
                                        int(m.group(3), 16), m.group(5) or "")
        if prefix == "XF86XK":
            name = "XF86" + name
        names[name] = value
        order.append((value, name))
        u = re.match(r"\s*U\+([0-9A-F]{4,6}) ", comment)
        if u and prefix == "XK" and "deprecated" not in comment.lower():
            cp = int(u.group(1), 16)
            if cp >= 0x100 and cp not in unicode_ks and value < 0x1000000:
                unicode_ks[cp] = value

# XKeysymToString: libX11 returns the first name of a value in its table.
first_name = {}
for value, name in order:
    first_name.setdefault(value, name)

# --- The keymap ---------------------------------------------------------------
codes = {}
for m in re.finditer(r"<([A-Za-z0-9+\-]+)>\s*=\s*(\d+);", keymap):
    codes[m.group(1)] = int(m.group(2))
for m in re.finditer(r"alias <([A-Za-z0-9+\-]+)>\s*=\s*<([A-Za-z0-9+\-]+)>;",
                     keymap):
    if m.group(2) in codes:
        codes[m.group(1)] = codes[m.group(2)]

symbols = keymap[keymap.index("xkb_symbols"):]
modmap = {}
for m in re.finditer(r"modifier_map (\w+) \{([^}]*)\}", symbols):
    for key in re.findall(r"<([^>]+)>", m.group(2)):
        modmap[key] = m.group(1)

keys = {}  # name -> (type or None, [syms of group 1])
for m in re.finditer(r"key\s+<([^>]+)>\s*\{(.*?)\};", symbols, re.S):
    body = m.group(2)
    t = re.search(r'type(\[group1\])?\s*=\s*"([^"]+)"', body)
    groups = re.findall(r"(?:symbols\[Group(\d)\]\s*=\s*)?\[([^\]]*)\]", body)
    if not groups:
        continue
    syms = [s.strip() for s in groups[0][1].split(",")]
    keys[m.group(1)] = (t.group(2) if t else None, syms)

REAL = {"Shift": 1, "Lock": 2, "Control": 4, "Mod1": 8, "Mod2": 16,
        "Mod3": 32, "Mod4": 64, "Mod5": 128}
vmod_key = {"NumLock": "Num_Lock", "Alt": "Alt_L", "Super": "Super_L",
            "LevelThree": "ISO_Level3_Shift", "Meta": "Meta_L",
            "Hyper": "Hyper_L", "LevelFive": "ISO_Level5_Shift",
            "ScrollLock": "Scroll_Lock"}
sym_mod = {}
for key, mod in modmap.items():
    if key in keys:
        for s in keys[key][1]:
            sym_mod.setdefault(s, mod)


def mask(expr):
    out = 0
    for part in expr.split("+"):
        part = part.strip()
        if part in ("none", ""):
            continue
        if part in REAL:
            out |= REAL[part]
        elif part in vmod_key and vmod_key[part] in sym_mod:
            out |= REAL[sym_mod[vmod_key[part]]]
    return out


types = {}
tsection = keymap[keymap.index("xkb_types"):keymap.index("xkb_compatibility")]
for m in re.finditer(r'type "([^"]+)" \{(.*?)\};', tsection, re.S):
    mods = re.search(r"modifiers=\s*([^;]+);", m.group(2))
    entries = [(mask(e.group(1)), int(e.group(2))) for e in
               re.finditer(r"map\[([^\]]+)\]=\s*Level(\d+);", m.group(2))]
    types[m.group(1)] = (mask(mods.group(1)) if mods else 0, entries)

# Autorepeat: keys matched by a compat interpretation get its repeat flag
# (default False); other keys repeat.
compat = keymap[keymap.index("xkb_compatibility"):keymap.index("xkb_symbols")]
default_repeat = "interpret.repeat= False" not in compat
interp = {}
for m in re.finditer(r"interpret (\w+)\+[^{]*\{(.*?)\};", compat, re.S):
    if m.group(1) == "Any":
        continue
    rep = re.search(r"repeat=\s*(True|False)", m.group(2))
    interp.setdefault(m.group(1), (rep.group(1) == "True") if rep
                      else default_repeat)


def is_layout_sym(s):
    """A keysym a layout key types (letters, digits, punctuation)."""
    if s in ("NoSymbol", "VoidSymbol"):
        return False
    v = names.get(s)
    if v is None:
        return True
    return v < 0xfd00 or 0x1000000 <= v <= 0x110ffff


def default_type(syms):
    if len(syms) == 1:
        return "ONE_LEVEL"
    if all(s.startswith("KP_") for s in syms[:2]):
        return "KEYPAD"
    return "TWO_LEVEL"


out = open(keys_out, "w")
out.write("// Generated by scripts/gen-xconsole-tables.py from %s, X11/keysymdef.h,\n"
          "// X11/XF86keysym.h and xserver's os/oscolor.c. Do not edit.\n\n" % keymap_path)

type_names = sorted({t for t, s in keys.values() if t} |
                    {default_type(s) for t, s in keys.values()})
out.write("static const struct XcKeyType {\n  const char *name;\n"
          "  unsigned int mods;\n  int n;\n  struct { unsigned int mods; int level; }"
          " map[16];\n} xc_key_types[] = {\n")
for name in type_names:
    mods, entries = types[name]
    ent = ", ".join("{0x%02x, %d}" % e for e in entries[:16])
    out.write('    {"%s", 0x%02x, %d, {%s}},\n' % (name, mods, len(entries[:16]), ent))
out.write("};\n\n")

out.write("// Keys whose keysyms do not depend on the layout: evdev keycode, key\n"
          "// type (index above), keysyms by level.\n")
out.write("static const struct XcFixedKey {\n  unsigned short code;\n  "
          "unsigned char type;\n  unsigned long syms[5];\n} xc_fixed_keys[] = {\n")
repeat = {}
for key, (t, syms) in sorted(keys.items(), key=lambda k: codes.get(k[0], 0)):
    if key not in codes:
        continue
    code = codes[key] - 8
    first = syms[0]
    repeat[code] = interp.get(first, True) and key not in modmap
    if code < 0 or any(is_layout_sym(s) for s in syms):
        continue
    if all(s in ("NoSymbol", "VoidSymbol") for s in syms):
        continue
    t = t or default_type(syms)
    vals = ", ".join("0x%x" % names.get(s, 0) for s in syms[:5])
    out.write("    {%d, %d, {%s}},  // <%s> %s\n" % (code, type_names.index(t),
                                                   vals, key, " ".join(syms)))
out.write("};\n\n")

out.write("// Keys that autorepeat (evdev keycodes; X keycode - 8).\n"
          "static const unsigned char xc_key_repeats[256] = {\n")
for code in range(256):
    if repeat.get(code, True):
        out.write("    [%d] = 1,\n" % code)
out.write("};\n\n")

out.write("// X's modifier map: the modifiers a key sets while held (evdev keycodes).\n"
          "static const unsigned char xc_key_modmap[256] = {\n")
for key, mod in sorted(modmap.items(), key=lambda k: codes.get(k[0], 0)):
    if key in codes and 0 <= codes[key] - 8 < 256:
        out.write("    [%d] = 0x%02x,  // <%s> %s\n" % (codes[key] - 8, REAL[mod], key, mod))
out.write("};\n\n")

out.write("// XKeysymToString: keysym -> name, by value.\n"
          "static const struct { unsigned long value; const char *name; }"
          " xc_keysym_names[] = {\n")
for value in sorted(first_name):
    out.write('    {0x%x, "%s"},\n' % (value, first_name[value]))
out.write("};\n\n")

out.write("// The keysym X gives a Unicode character (keysymdef.h), by code point.\n"
          "static const struct { unsigned int cp; unsigned long keysym; }"
          " xc_unicode_keysyms[] = {\n")
for cp in sorted(unicode_ks):
    out.write("    {0x%x, 0x%x},\n" % (cp, unicode_ks[cp]))
out.write("};\n\n")

# The X server's colour names, as the server has them.
src = open(oscolor_path).read()
a = src.index("static const char BuiltinColorNames[]")
b = src.index("Bool", src.index("BuiltinColors[]"))
out.close()
out = open(colors_out, "w")
out.write("// Generated by scripts/gen-xconsole-tables.py. Do not edit.\n")
out.write("// xserver os/oscolor.c (OsLookupColor's table), verbatim.\n")
out.write(src[a:b].replace("static const BuiltinColor", "static const XcBuiltinColor"))
