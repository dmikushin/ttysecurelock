#!/usr/bin/python3
"""Make the rain font of saver_matrix: a subset of Noto Sans CJK JP.

    scripts/gen-rain-font.py [SOURCE.ttc]   (default: Arch's noto-fonts-cjk)

Writes xconsole/rain/rain-font.otf with the glyphs the console's Matrix rain
uses, besides the digits of the console font itself:
  - the half-width katakana U+FF66-U+FF9D (58), as in the film's code;
  - the 240 kanji of grades 1 and 2 of the kyouiku kanji (MEXT's
    gakunenbetsu kanji haitou hyou, as listed on English Wikipedia,
    "Kyouiku kanji"), in xconsole/rain/kanji.txt; it includes 日, the one
    kanji of the film's code.
Noto Sans CJK is under the SIL Open Font License 1.1
(xconsole/rain/OFL.txt); the subset stays under it.
"""
import os
import sys

from fontTools import subset

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                    "xconsole", "rain")
src = sys.argv[1] if len(sys.argv) > 1 else \
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc"
kanji = "".join(open(os.path.join(here, "kanji.txt")).read().split())
assert len(kanji) == 240 and len(set(kanji)) == 240, len(kanji)
chars = [chr(c) for c in range(0xFF66, 0xFF9E)] + list(kanji)

opts = subset.Options()
opts.font_number = 0          # Noto Sans CJK JP in the collection
opts.layout_features = []
opts.name_IDs = ["*"]         # keep the copyright and licence names
opts.hinting = True
opts.notdef_outline = True
font = subset.load_font(src, opts)
sub = subset.Subsetter(opts)
sub.populate(text="".join(chars))
sub.subset(font)
out = os.path.join(here, "rain-font.otf")
font.save(out)
cmap = font.getBestCmap()
missing = [c for c in chars if ord(c) not in cmap]
assert not missing, missing
print(f"{out}: {len(cmap)} characters, {os.path.getsize(out)} bytes")
