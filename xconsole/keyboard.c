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
static int super_down;
static unsigned char key_down[NKEYS / 8];
static int saved_kbd_mode = K_UNICODE;

// Last translations, looked up by XLookupString / XmbLookupString.
#define TEXT_RING 64
static struct {
  unsigned long serial;
  KeySym keysym;
  char text[16];
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

//! Keys X maps to fixed keysyms whatever the layout (evdev keycodes).
static const struct {
  unsigned char keycode;
  KeySym keysym;
  const char *text;
} named_keys[] = {
    {1, XK_Escape, "\033"},
    {14, XK_BackSpace, "\b"},
    {15, XK_Tab, "\t"},
    {28, XK_Return, "\r"},
    {96, XK_KP_Enter, "\r"},
    {111, XK_Delete, "\177"},
    {29, XK_Control_L, ""},
    {97, XK_Control_R, ""},
    {42, XK_Shift_L, ""},
    {54, XK_Shift_R, ""},
    {56, XK_Alt_L, ""},
    {100, XK_Alt_R, ""},
    {125, XK_Super_L, ""},
    {126, XK_Super_R, ""},
    {58, XK_Caps_Lock, ""},
    {69, XK_Num_Lock, ""},
    {70, XK_Scroll_Lock, ""},
    {127, XK_Menu, ""},
    {59, XK_F1, ""},
    {60, XK_F2, ""},
    {61, XK_F3, ""},
    {62, XK_F4, ""},
    {63, XK_F5, ""},
    {64, XK_F6, ""},
    {65, XK_F7, ""},
    {66, XK_F8, ""},
    {67, XK_F9, ""},
    {68, XK_F10, ""},
    {87, XK_F11, ""},
    {88, XK_F12, ""},
    {102, XK_Home, ""},
    {103, XK_Up, ""},
    {104, XK_Prior, ""},
    {105, XK_Left, ""},
    {106, XK_Right, ""},
    {107, XK_End, ""},
    {108, XK_Down, ""},
    {109, XK_Next, ""},
    {110, XK_Insert, ""},
    {119, XK_Pause, ""},
    {99, XK_Print, ""},
    {113, 0x1008ff12, ""},  // XF86AudioMute
    {114, 0x1008ff11, ""},  // XF86AudioLowerVolume
    {115, 0x1008ff13, ""},  // XF86AudioRaiseVolume
    {163, 0x1008ff17, ""},  // XF86AudioNext
    {164, 0x1008ff14, ""},  // XF86AudioPlay
    {165, 0x1008ff16, ""},  // XF86AudioPrev
    {224, 0x1008ff03, ""},  // XF86MonBrightnessDown
    {225, 0x1008ff02, ""},  // XF86MonBrightnessUp
};

static const struct {
  KeySym keysym;
  const char *name;
} keysym_names[] = {
    {XK_Escape, "Escape"},
    {XK_BackSpace, "BackSpace"},
    {XK_Tab, "Tab"},
    {XK_Return, "Return"},
    {XK_KP_Enter, "KP_Enter"},
    {XK_Delete, "Delete"},
    {XK_Control_L, "Control_L"},
    {XK_Control_R, "Control_R"},
    {XK_Shift_L, "Shift_L"},
    {XK_Shift_R, "Shift_R"},
    {XK_Alt_L, "Alt_L"},
    {XK_Alt_R, "Alt_R"},
    {XK_Super_L, "Super_L"},
    {XK_Super_R, "Super_R"},
    {XK_Caps_Lock, "Caps_Lock"},
    {XK_Num_Lock, "Num_Lock"},
    {XK_Scroll_Lock, "Scroll_Lock"},
    {XK_Menu, "Menu"},
    {XK_F1, "F1"},
    {XK_F2, "F2"},
    {XK_F3, "F3"},
    {XK_F4, "F4"},
    {XK_F5, "F5"},
    {XK_F6, "F6"},
    {XK_F7, "F7"},
    {XK_F8, "F8"},
    {XK_F9, "F9"},
    {XK_F10, "F10"},
    {XK_F11, "F11"},
    {XK_F12, "F12"},
    {XK_Home, "Home"},
    {XK_Up, "Up"},
    {XK_Prior, "Prior"},
    {XK_Left, "Left"},
    {XK_Right, "Right"},
    {XK_End, "End"},
    {XK_Down, "Down"},
    {XK_Next, "Next"},
    {XK_Insert, "Insert"},
    {XK_Pause, "Pause"},
    {XK_Print, "Print"},
    {0x1008ff12, "XF86AudioMute"},
    {0x1008ff11, "XF86AudioLowerVolume"},
    {0x1008ff13, "XF86AudioRaiseVolume"},
    {0x1008ff17, "XF86AudioNext"},
    {0x1008ff14, "XF86AudioPlay"},
    {0x1008ff16, "XF86AudioPrev"},
    {0x1008ff03, "XF86MonBrightnessDown"},
    {0x1008ff02, "XF86MonBrightnessUp"},
};

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

static KeySym CharKeysym(const char *text, size_t len) {
  // Decode the (single) character; Latin-1 keysyms equal the code point.
  unsigned int c = (unsigned char)text[0];
  if (c >= 0xc0 && len >= 2) {
    if (c < 0xe0) {
      c = ((c & 0x1f) << 6) | (text[1] & 0x3f);
    } else if (c < 0xf0 && len >= 3) {
      c = ((c & 0x0f) << 12) | ((text[1] & 0x3f) << 6) | (text[2] & 0x3f);
    }
  }
  if (c < 0x20) return c + 0x60;  // Control characters: their letter.
  if (c < 0x100) return c;
  return 0x01000000 | c;
}

/*! \brief One key event: update the keyboard state like the kernel does,
 * and queue the X event. */
static void KeyEvent(Display *dpy, unsigned int keycode, int down) {
  if (keycode >= NKEYS) return;
  int rep = down && (key_down[keycode / 8] & (1 << (keycode % 8)));
  if (down) {
    key_down[keycode / 8] |= 1 << (keycode % 8);
  } else {
    key_down[keycode / 8] &= ~(1 << (keycode % 8));
  }

  unsigned int state = 0;  // X modifier state before this event
  if (shift_state & (1 << KG_SHIFT)) state |= ShiftMask;
  if (xc.shm->leds & LED_CAP) state |= LockMask;
  if (shift_state & ((1 << KG_CTRL) | (1 << KG_CTRLL) | (1 << KG_CTRLR))) {
    state |= ControlMask;
  }
  if (shift_state & (1 << KG_ALT)) state |= Mod1Mask;
  if (xc.shm->leds & LED_NUM) state |= Mod2Mask;
  if (super_down) state |= Mod4Mask;
  state |= (unsigned int)(xc.shm->group & 3) << 13;  // XkbBuildCoreState

  char text[16];
  size_t len = 0;
  KeySym keysym = NoSymbol;
  const char *named_text = NULL;
  for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); ++i) {
    if (named_keys[i].keycode == keycode) {
      keysym = named_keys[i].keysym;
      named_text = named_keys[i].text;
      break;
    }
  }

  if (keycode == 125 || keycode == 126) {
    // The Super keys are X's Mod4; the console keymap may have them bound
    // to something else (a hotkey daemon's shift), which we do not apply.
    super_down = down;
  } else {
    // The AltGr lock is the XKB group, which helpers may change.
    lockstate = (lockstate & ~(1u << KG_ALTGR)) |
                ((unsigned int)(xc.shm->group & 1) << KG_ALTGR);
    unsigned int shift_final = (shift_state | slockstate) ^ lockstate;
    unsigned short sym = K_HOLE;
    if (shift_final < NMAPS && have_map[shift_final]) {
      sym = keymaps[shift_final][keycode];
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
        case KT_PAD:
          if (down && (xc.shm->leds & LED_NUM)) {
            static const char pad_chars[] = "0123456789+-*/\r,.?()#";
            if (value < sizeof(pad_chars) - 1) {
              EmitChar((unsigned char)pad_chars[value], text, &len);
            }
          }
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
          // KT_FN, KT_CUR, KT_CONS, KT_ASCII, KT_BRL: no characters.
          break;
      }
      if (type != KT_SLOCK && down) slockstate = 0;
    }
  }

  if (named_text != NULL) {
    // X's fixed text for these keys, whatever the console keymap says.
    len = 0;
    if (down) {
      len = strlen(named_text);
      memcpy(text, named_text, len);
    }
  }
  if (keysym == NoSymbol && len > 0) keysym = CharKeysym(text, len);

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xkey.type = down ? KeyPress : KeyRelease;
  ev.xkey.serial = next_serial++;
  ev.xkey.display = dpy;
  ev.xkey.window = XC_ROOT_WINDOW;
  ev.xkey.root = XC_ROOT_WINDOW;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  ev.xkey.time = (Time)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
  ev.xkey.state = state;
  ev.xkey.keycode = keycode + 8;  // X keycodes are evdev + 8.
  ev.xkey.same_screen = True;
  size_t slot = ev.xkey.serial % TEXT_RING;
  text_ring[slot].serial = ev.xkey.serial;
  text_ring[slot].keysym = keysym;
  memcpy(text_ring[slot].text, text, len);
  text_ring[slot].text[len] = 0;
  XcQueueEvent(&ev);
}

void XcKeyboardInput(Display *dpy, const unsigned char *buf, size_t len) {
  // K_MEDIUMRAW: one byte per event, keycode | 0x80 on release; keycodes
  // above 127 as 0x00/0x80 followed by two bytes of 7 bits each.
  static unsigned char pending[3];
  static size_t npending;
  for (size_t i = 0; i < len; ++i) {
    pending[npending++] = buf[i];
    if ((pending[0] & 0x7f) == 0) {
      if (npending < 3) continue;
      unsigned int keycode = ((pending[1] & 0x7f) << 7) | (pending[2] & 0x7f);
      KeyEvent(dpy, keycode, !(pending[0] & 0x80));
    } else {
      KeyEvent(dpy, pending[0] & 0x7f, !(pending[0] & 0x80));
    }
    npending = 0;
  }
}

// ---------------------------------------------------------------------------
// Xlib key lookup.

static int LookupText(const XKeyEvent *ev, char *buf, int nbytes,
                      KeySym *keysym) {
  size_t slot = ev->serial % TEXT_RING;
  if (text_ring[slot].serial != ev->serial) {
    if (keysym) *keysym = NoSymbol;
    return 0;
  }
  if (keysym) *keysym = text_ring[slot].keysym;
  if (ev->type != KeyPress) return 0;
  int n = (int)strlen(text_ring[slot].text);
  if (n > nbytes) n = nbytes;
  memcpy(buf, text_ring[slot].text, (size_t)n);
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

char *XKeysymToString(KeySym keysym) {
  for (size_t i = 0; i < sizeof(keysym_names) / sizeof(keysym_names[0]);
       ++i) {
    if (keysym_names[i].keysym == keysym) return (char *)keysym_names[i].name;
  }
  static char buf[16];
  if (keysym >= 0x20 && keysym < 0x7f) {
    buf[0] = (char)keysym;
    buf[1] = 0;
    return buf;
  }
  if ((keysym & 0xff000000) == 0x01000000) {
    snprintf(buf, sizeof(buf), "U%04lX", keysym & 0xffffff);
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
  XcUnlock();
  if (leds & LED_CAP) state->locked_mods |= LockMask;
  if (leds & LED_NUM) state->locked_mods |= Mod2Mask;
  if (locks & (1u << KG_SHIFT)) state->locked_mods |= ShiftMask;
  state->mods = state->locked_mods;
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
