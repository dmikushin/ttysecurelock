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

/*! \brief Key events and XKB state from the console keyboard.
 *
 * The lock VT's keyboard is read in K_MEDIUMRAW mode (keycodes), so that key
 * presses and releases with their modifiers reach the program as X key
 * events do. Keycodes are translated with the console's own keymap, read
 * from the kernel when the lock starts, following the kernel's keyboard
 * handler (drivers/tty/vt/keyboard.c: kbd_keycode() and the k_* handlers):
 * the characters are those the console would type. Keys that X maps to
 * fixed keysyms independently of the layout (Return, BackSpace, Tab,
 * Escape, Delete, cursor and function keys, modifiers, media keys) get those
 * keysyms, so xsecurelock's key handling sees what it sees under X.
 *
 * XKB view: the group is the console's AltGr lock (the second layout of a
 * keymap like ruwin_cplk, toggled by CapsLock); the indicators are the
 * console keyboard LEDs (Caps Lock, Num Lock, Scroll Lock).
 */

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XKBstr.h>
#include <X11/keysym.h>
#include <errno.h>
#include <linux/kd.h>
#include <stdio.h>
#include <linux/keyboard.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <glob.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "../logging.h"
#include "xconsole_internal.h"

#define NKEYS 256
#define NMAPS 256

static unsigned short keymaps[NMAPS][NKEYS];
static unsigned char have_map[NMAPS];
static struct kbdiacruc accent_table[256];
static unsigned int accent_table_size;

static unsigned char shift_down[NR_SHIFT];
static unsigned int shift_state;
static unsigned int lockstate;   // KG_ALTGR bit mirrors shm->group
static unsigned int slockstate;
static unsigned int diacr;
static unsigned char key_down[NKEYS / 8];
static int saved_kbd_mode = K_UNICODE;

// Translations of the key events that left the queue last, looked up by
// XLookupString / XmbLookupString (the queue keeps those of the others).
#define TEXT_RING 64
static struct {
  unsigned long serial;
  XcKeyText key;
} text_ring[TEXT_RING];
static unsigned long next_serial = 1;

/*! \brief The indicators of an X server with the evdev rules, in its order
 * (xkeyboard-config keycodes/evdev; 12-14 are virtual, from compat/basic,
 * compat/iso9995 and compat/mousekeys), as xkbcomp shows them for the user's
 * "us,ru" keymap. Index i is bit i of XkbGetIndicatorState. */
static const char *const indicator_names[] = {
    "Caps Lock", "Num Lock", "Scroll Lock", "Compose",  "Kana",
    "Sleep",     "Suspend",  "Mute",        "Misc",     "Mail",
    "Charging",  "Shift Lock", "Group 2",   "Mouse Keys"};
#define IND_SHIFT_LOCK 11
#define IND_GROUP_2 12

// X's own data for the user's keymap: fixed keys, key types, autorepeat,
// modifier map, keysym names, Unicode keysyms (scripts/gen-xconsole-tables.py).
#include "xkeys.inc"

// X's autorepeat (xserver xkb/xkbAccessX.c: XkbDfltRepeatDelay/Interval).
#define REPEAT_DELAY_MS 660
#define REPEAT_INTERVAL_MS 40
static unsigned int repeat_key;  // 0: none
static long long repeat_next_ms;

// X keycodes are evdev keycodes + 8, at most 255.
#define MAX_EVDEV_KEYCODE (255 - 8)

static const struct XcFixedKey *FixedKey(unsigned int keycode) {
  for (size_t i = 0; i < sizeof(xc_fixed_keys) / sizeof(xc_fixed_keys[0]);
       ++i) {
    if (xc_fixed_keys[i].code == keycode) return &xc_fixed_keys[i];
  }
  return NULL;
}

//! The X modifiers of console shift bits (locks and latches).
static unsigned int KgMods(unsigned int kg) {
  unsigned int m = 0;
  if (kg & ((1u << KG_SHIFT) | (1u << KG_SHIFTL) | (1u << KG_SHIFTR))) {
    m |= ShiftMask;
  }
  if (kg & ((1u << KG_CTRL) | (1u << KG_CTRLL) | (1u << KG_CTRLR))) {
    m |= ControlMask;
  }
  if (kg & (1u << KG_ALT)) m |= Mod1Mask;
  if (kg & (1u << KG_ALTGR)) m |= Mod5Mask;
  return m;
}

/*! \brief libX11's _XTranslateKeySym (src/KeyBind.c) for keysyms that are
 * not characters of the layout: the text X gives them. */
static size_t TranslateKeySym(KeySym symbol, unsigned int modifiers,
                              char *buffer) {
  unsigned long hiBytes = symbol >> 8;
  if (!symbol) return 0;
  if (!((hiBytes == 0) ||
        ((hiBytes == 0xFF) &&
         (((symbol >= XK_BackSpace) && (symbol <= XK_Clear)) ||
          (symbol == XK_Return) || (symbol == XK_Escape) ||
          (symbol == XK_KP_Space) || (symbol == XK_KP_Tab) ||
          (symbol == XK_KP_Enter) ||
          ((symbol >= XK_KP_Multiply) && (symbol <= XK_KP_9)) ||
          (symbol == XK_KP_Equal) || (symbol == XK_Delete))))) {
    return 0;
  }
  unsigned char c;
  if (symbol == XK_KP_Space)
    c = XK_space & 0x7F;
  else if (hiBytes == 0xFF)
    c = symbol & 0x7F;
  else
    c = symbol & 0xFF;
  if (modifiers & ControlMask) {
    if ((c >= '@' && c < '\177') || c == ' ')
      c &= 0x1F;
    else if (c == '2')
      c = '\000';
    else if (c >= '3' && c <= '7')
      c -= ('3' - '\033');
    else if (c == '8')
      c = '\177';
    else if (c == '/')
      c = '_' & 0x1F;
  }
  buffer[0] = (char)c;
  return 1;
}

//! The keysym of a fixed key at the level its key type gives the state.
static KeySym FixedKeysym(const struct XcFixedKey *k, unsigned int state) {
  const struct XcKeyType *t = &xc_key_types[k->type];
  unsigned int mods = state & t->mods;
  int level = 1;
  for (int i = 0; i < t->n; ++i) {
    if (t->map[i].mods == mods) {
      level = t->map[i].level;
      break;
    }
  }
  if (level > 5 || k->syms[level - 1] == 0) return NoSymbol;
  return k->syms[level - 1];
}

//! The keysym X gives a character (keysymdef.h), else its Unicode keysym.
static KeySym UnicodeKeysym(unsigned int cp) {
  if ((cp >= 0x20 && cp < 0x7f) || (cp >= 0xa0 && cp < 0x100)) return cp;
  size_t lo = 0, hi = sizeof(xc_unicode_keysyms) / sizeof(xc_unicode_keysyms[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (xc_unicode_keysyms[mid].cp < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < sizeof(xc_unicode_keysyms) / sizeof(xc_unicode_keysyms[0]) &&
      xc_unicode_keysyms[lo].cp == cp) {
    return xc_unicode_keysyms[lo].keysym;
  }
  return 0x01000000 | cp;
}

static char *ReadFile(const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) return NULL;
  size_t cap = 1 << 16, n = 0;
  char *buf = malloc(cap + 1);
  while (buf != NULL) {
    size_t r = fread(buf + n, 1, cap - n, f);
    n += r;
    if (n < cap) break;
    char *bigger = realloc(buf, cap * 2 + 1);
    if (bigger == NULL) {
      free(buf);
      buf = NULL;
      break;
    }
    buf = bigger;
    cap *= 2;
  }
  fclose(f);
  if (buf != NULL) buf[n] = 0;
  return buf;
}

//! Split "a,b" into up to max fields of size XC_ATOM_NAME.
static int SplitList(const char *list, char out[][XC_ATOM_NAME], int max) {
  int n = 0;
  while (n < max) {
    size_t l = strcspn(list, ",");
    if (l >= XC_ATOM_NAME) l = XC_ATOM_NAME - 1;
    memcpy(out[n], list, l);
    out[n][l] = 0;
    ++n;
    list += strcspn(list, ",");
    if (*list != ',') break;
    ++list;
  }
  return n;
}

//! The value of `Option "name" "value"` in an xorg.conf text, or 0.
static int XorgOption(const char *text, const char *name, char *out,
                      size_t size) {
  char key[64];
  snprintf(key, sizeof(key), "\"%s\"", name);
  const char *p = text;
  while ((p = strstr(p, key)) != NULL) {
    const char *line = p;
    while (line > text && line[-1] != '\n') --line;
    const char *hash = memchr(line, '#', (size_t)(p - line));
    p += strlen(key);
    if (hash != NULL) continue;  // commented out
    const char *q = strchr(p, '"');
    const char *nl = strchr(p, '\n');
    if (q == NULL || (nl != NULL && q > nl)) continue;
    const char *e = strchr(q + 1, '"');
    if (e == NULL) continue;
    size_t l = (size_t)(e - q - 1);
    if (l >= size) l = size - 1;
    memcpy(out, q + 1, l);
    out[l] = 0;
    return 1;
  }
  return 0;
}

/*! \brief The XKB layouts configured for the system, as an X server would
 * get them without a user's setxkbmap: XkbLayout/XkbVariant of
 * /etc/X11/xorg.conf.d (what localectl set-x11-keymap writes), else the X
 * layout systemd's kbd-model-map gives the console keymap of
 * /etc/vconsole.conf (what localectl uses).
 * \return the number of layouts (0: none configured). */
static int ConfiguredLayouts(char layouts[][XC_ATOM_NAME],
                             char variants[][XC_ATOM_NAME], int max,
                             char *source, size_t source_size) {
  char layout[256] = "", variant[256] = "";
  glob_t g;
  if (glob("/etc/X11/xorg.conf.d/*.conf", 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc && !layout[0]; ++i) {
      char *text = ReadFile(g.gl_pathv[i]);
      if (text == NULL) continue;
      if (XorgOption(text, "XkbLayout", layout, sizeof(layout))) {
        XorgOption(text, "XkbVariant", variant, sizeof(variant));
        snprintf(source, source_size, "%s", g.gl_pathv[i]);
      }
      free(text);
    }
    globfree(&g);
  }
  if (!layout[0]) {
    char *vc = ReadFile("/etc/vconsole.conf");
    char keymap[128] = "";
    if (vc != NULL) {
      for (char *line = strtok(vc, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "KEYMAP=", 7)) {
          snprintf(keymap, sizeof(keymap), "%s", line + 7);
          keymap[strcspn(keymap, "\" \t")] = 0;
        }
      }
      free(vc);
    }
    char *map = keymap[0] ? ReadFile("/usr/share/systemd/kbd-model-map") : NULL;
    if (map != NULL) {
      for (char *line = strtok(map, "\n"); line; line = strtok(NULL, "\n")) {
        char f[5][128];
        if (line[0] == '#' ||
            sscanf(line, "%127s %127s %127s %127s %127s", f[0], f[1], f[2],
                   f[3], f[4]) < 4) {
          continue;
        }
        if (strcmp(f[0], keymap)) continue;
        snprintf(layout, sizeof(layout), "%s", f[1]);
        snprintf(variant, sizeof(variant), "%s", strcmp(f[3], "-") ? f[3] : "");
        snprintf(source, source_size,
                 "/usr/share/systemd/kbd-model-map for KEYMAP=%s", keymap);
        break;
      }
      free(map);
    }
  }
  if (!layout[0]) return 0;
  int n = SplitList(layout, layouts, max);
  for (int i = 0; i < max; ++i) variants[i][0] = 0;
  if (variant[0]) SplitList(variant, variants, max);
  return n;
}

//! The text of the first <tag>...</tag> in [p, end), or 0.
static int XmlText(const char *p, const char *end, const char *tag, char *out,
                   size_t size) {
  char open[64], close[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *a = strstr(p, open);
  if (a == NULL || a >= end) return 0;
  a += strlen(open);
  const char *b = strstr(a, close);
  if (b == NULL || b > end) return 0;
  size_t l = (size_t)(b - a);
  if (l >= size) l = size - 1;
  memcpy(out, a, l);
  out[l] = 0;
  return 1;
}

/*! \brief The human name xkeyboard-config gives a layout (and variant): the
 * group name an X server has for it (rules/evdev.xml). */
static int LayoutDescription(const char *xml, const char *layout,
                             const char *variant, char *out, size_t size) {
  for (const char *p = strstr(xml, "<layout>"); p != NULL;
       p = strstr(p + 1, "<layout>")) {
    const char *end = strstr(p, "</layout>");
    if (end == NULL) break;
    char name[64];
    if (!XmlText(p, end, "name", name, sizeof(name)) || strcmp(name, layout)) {
      continue;
    }
    if (!variant[0]) return XmlText(p, end, "description", out, size);
    for (const char *v = strstr(p, "<variant>"); v != NULL && v < end;
         v = strstr(v + 1, "<variant>")) {
      const char *vend = strstr(v, "</variant>");
      if (vend == NULL || vend > end) break;
      if (XmlText(v, vend, "name", name, sizeof(name)) &&
          !strcmp(name, variant)) {
        return XmlText(v, vend, "description", out, size);
      }
    }
    return 0;
  }
  return 0;
}

/*! \brief Group names from the system's XKB configuration, mapped to
 * xkeyboard-config's names. \return 1 if there is one for every group. */
static int ConfiguredGroupNames(int groups) {
  char layouts[4][XC_ATOM_NAME], variants[4][XC_ATOM_NAME], source[512] = "";
  int n = ConfiguredLayouts(layouts, variants, 4, source, sizeof(source));
  if (n == 0) {
    Log("No XKB layout is configured for the system (xorg.conf.d, "
        "kbd-model-map); guessing the layout names from the console keymap");
    return 0;
  }
  if (n != groups) {
    Log("%s gives %d XKB layouts for a console keymap with %d; guessing the "
        "layout names from the console keymap", source, n, groups);
    return 0;
  }
  char *xml = ReadFile("/usr/share/X11/xkb/rules/evdev.xml");
  if (xml == NULL) {
    LogErrno("Reading /usr/share/X11/xkb/rules/evdev.xml");
    return 0;
  }
  char names[2][XC_ATOM_NAME];
  int ok = 1;
  for (int i = 0; i < groups && i < 2 && ok; ++i) {
    ok = LayoutDescription(xml, layouts[i], variants[i], names[i],
                           XC_ATOM_NAME);
    if (!ok) {
      Log("xkeyboard-config has no layout %s(%s) of %s; guessing the layout "
          "names from the console keymap", layouts[i], variants[i], source);
    }
  }
  free(xml);
  if (!ok) return 0;
  XcLock();
  for (int i = 0; i < groups && i < 2; ++i) {
    memcpy(xc.shm->group_names[i], names[i], XC_ATOM_NAME);
  }
  XcUnlock();
  return 1;
}

//! The XKB name of the layout typed in keymap table t.
static void GroupName(int t, char *out, size_t size) {
  static const unsigned char letter_keys[] = {
      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 30, 31, 32, 33, 34,
      35, 36, 37, 38, 39, 40, 44, 45, 46, 47, 48, 49, 50, 51, 52};
  int latin = 0, cyrillic = 0, has_yery = 0, has_yi = 0, has_short_u = 0;
  for (size_t i = 0; i < sizeof(letter_keys); ++i) {
    unsigned short v = keymaps[t][letter_keys[i]];
    unsigned int cp;
    if (v >= 0xf000) {
      cp = v ^ 0xf000;
    } else if ((v >> 8) == KT_LATIN || (v >> 8) == KT_LETTER) {
      cp = v & 0xff;
    } else {
      continue;
    }
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) ++latin;
    if (cp >= 0x400 && cp <= 0x4ff) ++cyrillic;
    if (cp == 0x44b) has_yery = 1;      // ы
    if (cp == 0x457) has_yi = 1;        // ї
    if (cp == 0x45e) has_short_u = 1;  // ў
  }
  const char *name = "Unknown";
  if (cyrillic > latin) {
    name = has_yi       ? "Ukrainian"
           : has_short_u ? "Belarusian"
           : has_yery    ? "Russian"
                         : "Cyrillic";
  } else if (latin > 0) {
    name = "English (US)";
  }
  strncpy(out, name, size - 1);
  out[size - 1] = 0;
}

/*! \brief The AltGr lock of the console in front: 1 if set, 0 if not or
 * unknown.
 *
 * The kernel has no ioctl for a VT's lock state, but it publishes the lock
 * state of the console in front through the LED triggers kbd-shiftlock,
 * kbd-altgrlock, ... (drivers/tty/vt/keyboard.c, kbd_propagate_led_state).
 * A uinput device with one LED and no keys gets an LED class device from
 * input-leds; with its trigger set to kbd-altgrlock, its brightness is the
 * AltGr lock. Nothing physical lights up and no key can come from it.
 */
static int ReadAltGrLock(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    LogErrno("open /dev/uinput (reading the console's layout)");
    return 0;
  }
  int result = 0;
  struct uinput_setup setup;
  memset(&setup, 0, sizeof(setup));
  setup.id.bustype = BUS_VIRTUAL;
  strcpy(setup.name, "ttysecurelock layout probe");
  char sysname[64] = "";
  if (ioctl(fd, UI_SET_EVBIT, EV_LED) < 0 ||
      ioctl(fd, UI_SET_LEDBIT, LED_CAPSL) < 0 ||
      ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0 ||
      ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
    LogErrno("uinput (reading the console's layout)");
    close(fd);
    return 0;
  }
  // input-leds registers the LED asynchronously, and the LED class sets its
  // default trigger (kbd-capslock) after the sysfs files appear: set ours,
  // read the brightness, and accept it only if ours was the trigger all
  // along.
  char trigger_path[128], brightness_path[128];
  snprintf(trigger_path, sizeof(trigger_path),
           "/sys/class/leds/%s::capslock/trigger", sysname);
  snprintf(brightness_path, sizeof(brightness_path),
           "/sys/class/leds/%s::capslock/brightness", sysname);
  static const char trigger[] = "kbd-altgrlock";
  int known = 0;
  for (int i = 0; i < 200 && !known; ++i) {
    int tfd = open(trigger_path, O_WRONLY | O_CLOEXEC);
    if (tfd < 0) {
      usleep(5000);
      continue;
    }
    ssize_t w = write(tfd, trigger, sizeof(trigger) - 1);
    close(tfd);
    if (w < 0) {
      usleep(5000);
      continue;
    }
    int b = 0;
    FILE *f = fopen(brightness_path, "r");
    if (f != NULL) {
      if (fscanf(f, "%d", &b) != 1) b = -1;
      fclose(f);
    }
    char current[1024] = "";
    f = fopen(trigger_path, "r");
    if (f != NULL) {
      size_t n = fread(current, 1, sizeof(current) - 1, f);
      current[n] = 0;
      fclose(f);
    }
    if (b >= 0 && strstr(current, "[kbd-altgrlock]") != NULL) {
      known = 1;
      result = b > 0;
    }
    if (getenv("XSECURELOCK_DEBUG_CONSOLE_LAYOUT")) {
      Log("layout probe %d: brightness %d, trigger %s", i, b,
          strstr(current, "[kbd-altgrlock]") ? "ours" : "not ours");
    }
  }
  if (!known) {
    Log("Could not read the console's layout through %s; assuming the first",
        trigger_path);
  }
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return result;
}

int XcKeyboardInit(int vt_fd, int orig_fd) {
  // The keymap is read through a VT in Unicode mode: through a VT in
  // another mode the kernel reports Unicode keysyms as holes (K_HOLE).
  if (ioctl(vt_fd, KDGKBMODE, &saved_kbd_mode) < 0) {
    saved_kbd_mode = K_UNICODE;
  }
  if (saved_kbd_mode != K_UNICODE && ioctl(vt_fd, KDSKBMODE, K_UNICODE) < 0) {
    LogErrno("KDSKBMODE K_UNICODE");
    return 0;
  }
  struct kbentry e;
  for (int t = 0; t < NMAPS; ++t) {
    e.kb_table = t;
    e.kb_index = 0;
    if (ioctl(vt_fd, KDGKBENT, &e) < 0) {
      LogErrno("KDGKBENT");
      return 0;
    }
    if (e.kb_value == K_NOSUCHMAP) continue;
    have_map[t] = 1;
    for (int k = 0; k < NKEYS; ++k) {
      e.kb_index = k;
      keymaps[t][k] = ioctl(vt_fd, KDGKBENT, &e) == 0 ? e.kb_value : K_HOLE;
    }
  }
  struct kbdiacrsuc dia;
  if (ioctl(vt_fd, KDGKBDIACRUC, &dia) == 0) {
    accent_table_size = dia.kb_cnt < 256 ? dia.kb_cnt : 256;
    memcpy(accent_table, dia.kbdiacruc,
           accent_table_size * sizeof(accent_table[0]));
  }
  // Two layouts if the AltGr lock selects a keymap of its own.
  int groups = 1;
  int altgr = 1 << KG_ALTGR;
  if (have_map[altgr]) {
    for (int t = 0; t < NMAPS && groups == 1; ++t) {
      for (int k = 0; k < NKEYS && have_map[t]; ++k) {
        unsigned short v = keymaps[t][k];
        if (v == K(KT_LOCK, KG_ALTGR) || v == K(KT_SLOCK, KG_ALTGR)) {
          groups = 2;
          break;
        }
      }
    }
  }
  // Caps Lock, Num Lock, Scroll Lock and the layout of the console in front.
  unsigned char leds = 0;
  if (orig_fd < 0 || ioctl(orig_fd, KDGKBLED, &leds) < 0) {
    LogErrno("KDGKBLED");
    leds = 0;
  }
  int group = groups > 1 ? ReadAltGrLock() : 0;
  lockstate = (unsigned int)group << KG_ALTGR;
  XcLock();
  xc.shm->num_groups = groups;
  xc.shm->group = group;
  xc.shm->locks = lockstate;
  xc.shm->leds = leds & 7;
  GroupName(0, xc.shm->group_names[0], XC_ATOM_NAME);
  GroupName(altgr, xc.shm->group_names[1], XC_ATOM_NAME);
  XcUnlock();
  // Names from the system's XKB configuration replace the guessed ones.
  ConfiguredGroupNames(groups);
  return 1;
}

void XcKeyboardSetMode(int vt_fd) {
  if (ioctl(vt_fd, KDSKBMODE, K_MEDIUMRAW) < 0) {
    LogErrno("KDSKBMODE");
  }
}

void XcKeyboardRestore(int vt_fd) { ioctl(vt_fd, KDSKBMODE, saved_kbd_mode); }

static void SetLeds(unsigned int leds) {
  XcLock();
  xc.shm->leds = leds & 7;
  XcUnlock();
  ioctl(xc.vt_fd, KDSKBLED, (unsigned long)(leds & 7));
}

//! UTF-8 encoding of a code point.
static size_t Utf8(unsigned int c, char *out) {
  if (c < 0x80) {
    out[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    out[0] = (char)(0xc0 | (c >> 6));
    out[1] = (char)(0x80 | (c & 0x3f));
    return 2;
  }
  if (c < 0x10000) {
    out[0] = (char)(0xe0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[2] = (char)(0x80 | (c & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (c >> 18));
  out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
  out[3] = (char)(0x80 | (c & 0x3f));
  return 4;
}

/*! \brief Kernel handle_diacr(): combine the pending dead key with ch.
 *
 * Returns the combination if the accent table has one; ch itself if it is
 * a space or the same dead key; otherwise the dead key is typed first and
 * ch is returned.
 */
static unsigned int HandleDiacr(unsigned int ch, char *text, size_t *len) {
  unsigned int d = diacr;
  diacr = 0;
  for (unsigned int i = 0; i < accent_table_size; ++i) {
    if (accent_table[i].diacr == d && accent_table[i].base == ch) {
      return accent_table[i].result;
    }
  }
  if (ch == ' ' || ch == d) return d;
  *len += Utf8(d, text + *len);
  return ch;
}

//! Kernel k_unicode().
static void EmitChar(unsigned int ch, char *text, size_t *len) {
  if (diacr) ch = HandleDiacr(ch, text, len);
  *len += Utf8(ch, text + *len);
}

//! Kernel k_deadunicode().
static void DeadKey(unsigned int value, char *text, size_t *len) {
  diacr = diacr ? HandleDiacr(value, text, len) : value;
}

static long long NowMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int KeyDown(unsigned int keycode) {
  return (key_down[keycode / 8] >> (keycode % 8)) & 1;
}

/*! \brief The X modifier state: the modifiers of the keys held (X's
 * modifier map; lock keys excluded, their state is the console's LEDs),
 * the console's locks and latches, and the group (XkbBuildCoreState). */
static unsigned int XState(void) {
  unsigned int state = 0;
  for (unsigned int k = 0; k < NKEYS; ++k) {
    if (xc_key_modmap[k] && KeyDown(k)) {
      state |= xc_key_modmap[k] & ~(unsigned int)(LockMask | Mod2Mask);
    }
  }
  if (xc.shm->leds & LED_CAP) state |= LockMask;
  if (xc.shm->leds & LED_NUM) state |= Mod2Mask;
  state |= KgMods(xc.shm->locks & ~(1u << KG_ALTGR));
  state |= KgMods(xc.shm->latches);
  state |= (unsigned int)(xc.shm->group & 3) << 13;
  return state;
}

/*! \brief One key event: update the keyboard state like the kernel does,
 * and queue the X event.
 *
 * The kernel's own key repeats are dropped: X repeats keys itself (see
 * XcKeyboardTick), and a client that does not ask for detectable
 * autorepeat (xsecurelock does not) gets a KeyRelease and a KeyPress per
 * repeat. synthetic marks those. */
static void KeyEvent(Display *dpy, unsigned int keycode, int down,
                     int synthetic) {
  if (keycode >= NKEYS || keycode > MAX_EVDEV_KEYCODE) return;
  int rep = down && KeyDown(keycode);
  if (rep && !synthetic) return;  // the kernel's autorepeat
  if (!synthetic) {
    if (down && xc_key_repeats[keycode]) {
      repeat_key = keycode;
      repeat_next_ms = NowMs() + REPEAT_DELAY_MS;
    } else if (!down && keycode == repeat_key) {
      repeat_key = 0;
    }
  }

  unsigned int state = XState();  // X's state is the one before the event
  if (down) {
    key_down[keycode / 8] |= 1 << (keycode % 8);
  } else {
    key_down[keycode / 8] &= ~(1 << (keycode % 8));
  }

  char text[16];
  size_t len = 0;
  KeySym keysym = NoSymbol;
  const struct XcFixedKey *fixed = FixedKey(keycode);

  if (keycode == 125 || keycode == 126) {
    // The Super keys are X's Mod4 (in the modifier map); the console keymap
    // may have them bound to something else (a hotkey daemon's shift),
    // which we do not apply.
  } else {
    // The AltGr lock is the XKB group, which helpers may change.
    lockstate = (lockstate & ~(1u << KG_ALTGR)) |
                ((unsigned int)(xc.shm->group & 1) << KG_ALTGR);
    unsigned int shift_final = (shift_state | slockstate) ^ lockstate;
    unsigned short sym = K_HOLE;
    if (shift_final < NMAPS && have_map[shift_final]) {
      sym = keymaps[shift_final][keycode];
    }
    // The keysym is the character the key types without Control and Alt,
    // as X's keysym does not depend on them.
    unsigned int plain = shift_final & ~((1u << KG_CTRL) | (1u << KG_ALT) |
                                         (1u << KG_CTRLL) | (1u << KG_CTRLR));
    unsigned short plain_sym = K_HOLE;
    if (plain < NMAPS && have_map[plain]) plain_sym = keymaps[plain][keycode];
    if (plain_sym >= 0xf000) {
      keysym = UnicodeKeysym(plain_sym ^ 0xf000);
    } else if ((plain_sym >> 8) == KT_LATIN || (plain_sym >> 8) == KT_LETTER) {
      unsigned int v = plain_sym & 0xff;
      if ((plain_sym >> 8) == KT_LETTER && (xc.shm->leds & LED_CAP)) {
        unsigned int t = plain ^ (1 << KG_SHIFT);
        if (t < NMAPS && have_map[t]) v = keymaps[t][keycode] & 0xff;
      }
      if (v >= 0x20) keysym = UnicodeKeysym(v);
    }
    if (sym >= 0xf000) {
      // Unicode keysym: a character.
      if (down) EmitChar(sym ^ 0xf000, text, &len);
    } else {
      unsigned int type = sym >> 8, value = sym & 0xff;
      if (type == KT_LETTER) {
        type = KT_LATIN;
        if (xc.shm->leds & LED_CAP) {
          unsigned int t = shift_final ^ (1 << KG_SHIFT);
          if (t < NMAPS && have_map[t]) value = keymaps[t][keycode] & 0xff;
        }
      }
      switch (type) {
        case KT_LATIN:
          if (down) EmitChar(value, text, &len);
          break;
        case KT_META:
          // As X: Alt+key types the key's character.
          if (down) EmitChar(value, text, &len);
          break;
        case KT_DEAD:
          if (down) {
            static const unsigned char ret_diacr[] = {
                '`', '\'', '^', '~', '"', ',', '_', 'U', '.',
                '*', '=',  'c', 'k', 'i', '#', 'o', '!', '?',
                '+', '-',  ')', '(', ':', 'n', ';', '$', '@'};
            if (value < sizeof(ret_diacr)) DeadKey(ret_diacr[value], text, &len);
          }
          break;
        case KT_DEAD2:
          if (down) DeadKey(value, text, &len);
          break;
        case KT_SHIFT:
        case KT_SLOCK:
          if (!rep && value < NR_SHIFT) {
            unsigned int v = value;
            if (type == KT_SHIFT && sym == K_CAPSSHIFT) {
              v = KG_SHIFT;
              if (down) SetLeds(xc.shm->leds & ~LED_CAP);
            }
            if (!down) {
              if (shift_down[v]) shift_down[v]--;
            } else {
              shift_down[v]++;
            }
            if (shift_down[v]) {
              shift_state |= 1u << v;
            } else {
              shift_state &= ~(1u << v);
            }
            if (type == KT_SLOCK && down) slockstate ^= 1u << value;
          }
          break;
        case KT_LOCK:
          if (down && !rep) {
            lockstate ^= 1u << value;
            XcLock();
            xc.shm->locks = lockstate;
            if (value == KG_ALTGR) {
              xc.shm->group = (lockstate >> KG_ALTGR) & 1;
            }
            XcUnlock();
          }
          break;
        case KT_SPEC:
          if (down && !rep) {
            if (value == KVAL(K_CAPS)) {
              SetLeds(xc.shm->leds ^ LED_CAP);
            } else if (value == KVAL(K_CAPSON)) {
              SetLeds(xc.shm->leds | LED_CAP);
            } else if (value == KVAL(K_NUM) || value == KVAL(K_BARENUMLOCK)) {
              SetLeds(xc.shm->leds ^ LED_NUM);
            } else if (value == KVAL(K_ENTER)) {
              if (diacr) {
                len += Utf8(diacr, text + len);
                diacr = 0;
              }
              text[len++] = '\r';
            }
            // Console switching, reboot, SAK, show_* etc.: nothing.
          }
          break;
        default:
          // KT_PAD (the keypad is a fixed key), KT_FN, KT_CUR, KT_CONS,
          // KT_ASCII, KT_BRL: no characters here.
          break;
      }
      if (type != KT_SLOCK && down) slockstate = 0;
      XcLock();
      xc.shm->latches = slockstate;
      XcUnlock();
    }
  }

  if (fixed != NULL) {
    // A key whose keysyms do not depend on the layout: X's keysym for the
    // level the state selects, and X's text for it.
    keysym = FixedKeysym(fixed, state);
    len = down ? TranslateKeySym(keysym, state, text) : 0;
  }

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xkey.type = down ? KeyPress : KeyRelease;
  ev.xkey.serial = next_serial++;
  ev.xkey.display = dpy;
  ev.xkey.window = XC_ROOT_WINDOW;
  ev.xkey.root = XC_ROOT_WINDOW;
  ev.xkey.time = (Time)NowMs();
  ev.xkey.state = state;
  ev.xkey.keycode = keycode + 8;  // X keycodes are evdev + 8.
  ev.xkey.same_screen = True;
  XcKeyText key;
  memset(&key, 0, sizeof(key));
  key.keysym = keysym;
  memcpy(key.text, text, len);
  XcQueueKeyEvent(&ev, &key);
}

void XcKeyboardInput(Display *dpy, const unsigned char *buf, size_t len) {
  static unsigned long total;
  if (getenv("XSECURELOCK_DEBUG_CONSOLE_KEYS") &&
      (total + len) / 500 != total / 500) {
    Log("%lu key events read", total + len);
  }
  total += len;
  // K_MEDIUMRAW: one byte per event, keycode | 0x80 on release; keycodes
  // above 127 as 0x00/0x80 followed by two bytes of 7 bits each.
  static unsigned char pending[3];
  static size_t npending;
  for (size_t i = 0; i < len; ++i) {
    pending[npending++] = buf[i];
    if ((pending[0] & 0x7f) == 0) {
      if (npending < 3) continue;
      unsigned int keycode = ((pending[1] & 0x7f) << 7) | (pending[2] & 0x7f);
      KeyEvent(dpy, keycode, !(pending[0] & 0x80), 0);
    } else {
      KeyEvent(dpy, pending[0] & 0x7f, !(pending[0] & 0x80), 0);
    }
    npending = 0;
  }
}

void XcKeyboardTick(Display *dpy) {
  if (repeat_key == 0) return;
  long long now = NowMs();
  if (now - repeat_next_ms > 1000) repeat_next_ms = now;  // we were stopped
  while (repeat_key != 0 && now >= repeat_next_ms) {
    if (getenv("XSECURELOCK_DEBUG_CONSOLE_KEYS")) {
      Log("repeat of key %u", repeat_key);
    }
    KeyEvent(dpy, repeat_key, 0, 1);
    KeyEvent(dpy, repeat_key, 1, 1);
    repeat_next_ms += REPEAT_INTERVAL_MS;
  }
}

int XcKeyboardTimeoutMs(void) {
  if (repeat_key == 0) return -1;
  long long d = repeat_next_ms - NowMs();
  return d < 0 ? 0 : (int)d;
}

// ---------------------------------------------------------------------------
// Xlib key lookup.

void XcKeyDequeued(unsigned long serial, const XcKeyText *key) {
  size_t slot = serial % TEXT_RING;
  text_ring[slot].serial = serial;
  text_ring[slot].key = *key;
}

static int LookupText(const XKeyEvent *ev, char *buf, int nbytes,
                      KeySym *keysym) {
  size_t slot = ev->serial % TEXT_RING;
  if (text_ring[slot].serial != ev->serial) {
    if (keysym) *keysym = NoSymbol;
    return 0;
  }
  if (keysym) *keysym = text_ring[slot].key.keysym;
  if (ev->type != KeyPress) return 0;
  int n = (int)strnlen(text_ring[slot].key.text, sizeof(text_ring[slot].key.text));
  if (n > nbytes) n = nbytes;
  memcpy(buf, text_ring[slot].key.text, (size_t)n);
  return n;
}

int XLookupString(XKeyEvent *event, char *buffer, int nbytes,
                  KeySym *keysym, XComposeStatus *status) {
  (void)status;
  return LookupText(event, buffer, nbytes, keysym);
}

int XmbLookupString(XIC ic, XKeyPressedEvent *event, char *buffer, int nbytes,
                    KeySym *keysym, Status *status) {
  (void)ic;
  int n = LookupText(event, buffer, nbytes, keysym);
  if (status) {
    KeySym ks = keysym ? *keysym : NoSymbol;
    *status = n > 0 ? (ks != NoSymbol ? XLookupBoth : XLookupChars)
                    : (ks != NoSymbol ? XLookupKeySym : XLookupNone);
  }
  return n;
}

/*! \brief libX11's XKeysymToString: the keysym's name (keysymdef.h,
 * XF86keysym.h), "U%04lX"/"U%06lX" for other Unicode keysyms, else NULL. */
char *XKeysymToString(KeySym keysym) {
  size_t lo = 0, hi = sizeof(xc_keysym_names) / sizeof(xc_keysym_names[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (xc_keysym_names[mid].value < keysym) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < sizeof(xc_keysym_names) / sizeof(xc_keysym_names[0]) &&
      xc_keysym_names[lo].value == keysym) {
    return (char *)xc_keysym_names[lo].name;
  }
  if ((keysym & 0xff000000) == 0x01000000) {
    static char buf[16];
    unsigned long val = keysym & 0xffffff;
    snprintf(buf, sizeof(buf), (val & 0xff0000) ? "U%06lX" : "U%04lX", val);
    return buf;
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// XKB.

Bool XkbQueryExtension(Display *dpy, int *opcode, int *event_base,
                       int *error_base, int *major, int *minor) {
  (void)dpy;
  if (opcode) *opcode = 0;
  if (event_base) *event_base = 0;
  if (error_base) *error_base = 0;
  if (major) *major = 1;
  if (minor) *minor = 0;
  return True;
}

XkbDescPtr XkbGetMap(Display *dpy, unsigned int which, unsigned int device) {
  (void)which;
  (void)device;
  XkbDescPtr xkb = calloc(1, sizeof(*xkb));
  if (xkb == NULL) return NULL;
  xkb->dpy = dpy;
  xkb->min_key_code = 8;
  xkb->max_key_code = 255;
  return xkb;
}

Status XkbGetControls(Display *dpy, unsigned long which, XkbDescPtr xkb) {
  (void)dpy;
  (void)which;
  if (xkb->ctrls == NULL) {
    xkb->ctrls = calloc(1, sizeof(*xkb->ctrls));
    if (xkb->ctrls == NULL) return BadAlloc;
  }
  XcLock();
  xkb->ctrls->num_groups = (unsigned char)xc.shm->num_groups;
  XcUnlock();
  return Success;
}

Status XkbGetNames(Display *dpy, unsigned int which, XkbDescPtr xkb) {
  (void)dpy;
  (void)which;
  if (xkb->names == NULL) {
    xkb->names = calloc(1, sizeof(*xkb->names));
    if (xkb->names == NULL) return BadAlloc;
  }
  char g0[XC_ATOM_NAME], g1[XC_ATOM_NAME];
  int groups;
  XcLock();
  memcpy(g0, xc.shm->group_names[0], sizeof(g0));
  memcpy(g1, xc.shm->group_names[1], sizeof(g1));
  groups = xc.shm->num_groups;
  XcUnlock();
  xkb->names->groups[0] = XcInternAtom(g0);
  if (groups > 1) xkb->names->groups[1] = XcInternAtom(g1);
  xkb->names->symbols = XcInternAtom("linux-console");
  for (size_t i = 0;
       i < sizeof(indicator_names) / sizeof(indicator_names[0]); ++i) {
    xkb->names->indicators[i] = XcInternAtom(indicator_names[i]);
  }
  return Success;
}

Status XkbGetState(Display *dpy, unsigned int device, XkbStatePtr state) {
  (void)dpy;
  (void)device;
  memset(state, 0, sizeof(*state));
  XcLock();
  state->group = state->locked_group = (unsigned char)xc.shm->group;
  unsigned int leds = xc.shm->leds;
  unsigned int locks = xc.shm->locks;
  unsigned int latches = xc.shm->latches;
  XcUnlock();
  if (leds & LED_CAP) state->locked_mods |= LockMask;
  if (leds & LED_NUM) state->locked_mods |= Mod2Mask;
  state->locked_mods |= KgMods(locks & ~(1u << KG_ALTGR));
  state->latched_mods = KgMods(latches);
  state->mods = state->locked_mods | state->latched_mods;
  return Success;
}

Status XkbGetIndicatorState(Display *dpy, unsigned int device,
                            unsigned int *state) {
  (void)dpy;
  (void)device;
  XcLock();
  unsigned int leds = xc.shm->leds;
  int group = xc.shm->group;
  unsigned int locks = xc.shm->locks;
  XcUnlock();
  *state = 0;
  if (leds & LED_CAP) *state |= 1u << 0;
  if (leds & LED_NUM) *state |= 1u << 1;
  if (leds & LED_SCR) *state |= 1u << 2;
  if (locks & (1u << KG_SHIFT)) *state |= 1u << IND_SHIFT_LOCK;
  if (group != 0) *state |= 1u << IND_GROUP_2;
  return Success;
}

Bool XkbLockGroup(Display *dpy, unsigned int device, unsigned int group) {
  (void)dpy;
  (void)device;
  XcLock();
  if (xc.shm->num_groups > 0) {
    xc.shm->group = (int)(group % (unsigned int)xc.shm->num_groups);
  }
  XcUnlock();
  return True;
}

void XkbFreeKeyboard(XkbDescPtr xkb, unsigned int which, Bool free_all) {
  (void)which;
  (void)free_all;
  if (xkb == NULL) return;
  free(xkb->ctrls);
  free(xkb->names);
  free(xkb);
}
