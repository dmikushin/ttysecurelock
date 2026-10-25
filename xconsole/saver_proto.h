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

/*! \brief The saver channel: how a saver module draws on the lock VT.
 *
 * A saver module never gets the lock VT. It finds the channel in $DISPLAY
 * (":xconsole:<vt>:<table>:<channel>", the third number) and its window in
 * $XSCREENSAVER_WINDOW, as an X saver does. It connects by sending a
 * XcSaverHello datagram on the channel with one end of a SOCK_SEQPACKET
 * socket pair attached (SCM_RIGHTS); the kernel adds its credentials. The
 * lock accepts it if the window lies in xsecurelock's saver window and was
 * created by the saver's parent (saver_multiplex, or xsecurelock itself), and
 * answers with the window's size in cells. Then the saver sends cells
 * (printable characters and colours, no terminal control sequences) and the
 * lock paints them where the window is visible, under the auth window like
 * everything else; it keeps them, and repaints them when the window becomes
 * visible again (after the auth prompt). When the window's size changes, the
 * lock sends the new size.
 */

#ifndef XCONSOLE_SAVER_PROTO_H
#define XCONSOLE_SAVER_PROTO_H

#include <stdint.h>

#define XC_SAVER_MAGIC 0x56534358u  // "XCSV"
#define XC_SAVER_VERSION 3u
#define XC_SAVER_MAX_MSG 196608  // a font message; cells are fewer
#define XC_SAVER_MAX_CELLS 5460
#define XC_SAVER_MAX_GLYPHS 400

//! Saver -> lock, on the channel, with the connection's fd attached.
typedef struct {
  uint32_t magic;
  uint32_t version;
  uint64_t window;
} XcSaverHello;

//! Lock -> saver: the window's size in cells (also the answer to hello),
//! and the console font's cell in pixels.
#define XC_SAVER_SIZE 0x5349u  // "IS"
typedef struct {
  uint32_t type;
  uint32_t cols, rows;
  uint32_t cell_w, cell_h;
} XcSaverSize;

/*! Saver -> lock: glyphs for characters the console font lacks, for the
 * lock VT while the saver shows: XcSaverFont, n code points (uint32_t, in
 * the BMP, not ASCII), then n glyphs of cell_h rows of (cell_w + 7) / 8
 * bytes, the leftmost pixel in the top bit, as KDFONTOP takes them. The lock
 * answers with XcSaverFontOk. It loads them, with the console font's own
 * glyphs for everything else, as the font of the lock VT only while the
 * saver shows, and puts the console font back before the prompt shows. */
#define XC_SAVER_FONT 0x4f46u     // "FO"
#define XC_SAVER_FONT_OK 0x4b4fu  // "OK"
typedef struct {
  uint32_t type;
  uint32_t n;
  uint32_t width, height;
} XcSaverFont;
typedef struct {
  uint32_t type;
  uint32_t ok;
} XcSaverFontOk;

/*! Helper -> lock, on the channel: the prompt is about to show; the lock
 * puts the console font back (the helper waits for XcShared.rain_font to be
 * 0). Anyone with the channel may ask; it only ends the rain font early. */
#define XC_SAVER_PROMPT 0x50524f4du  // "MORP"

//! Saver -> lock: XcSaverCells followed by n XcSaverCell.
#define XC_SAVER_CELLS 0x4543u  // "CE"
typedef struct {
  uint32_t type;
  uint32_t n;
} XcSaverCells;

typedef struct {
  uint16_t col, row;
  uint32_t cp;  // Unicode code point; anything not printable shows as ' '
  uint8_t fg, bg;  // console colours 0-15, or XC_SAVER_RAMP + level
  uint8_t pad[2];
} XcSaverCell;

/*! The lock's green ramp, for the Matrix rain: XC_SAVER_RAMP + 0 is the
 * brightest (a trail's head), + XC_SAVER_RAMP_LEVELS - 1 the darkest. The
 * lock shows it with palette entries of the lock VT that it sets while the
 * saver shows (display.c, XcSetRampLocked). */
#define XC_SAVER_RAMP 16
#define XC_SAVER_RAMP_LEVELS 6

#endif
