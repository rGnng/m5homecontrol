# Remote Dot 2x font

Generated from the supplied `BoutiqueBitmap9x9_1.6.ttf`, Regular weight.
`source.json` records the exact source checksum and conversion settings.
The bitmap derivative is named **Remote Dot 2x**.

The font's typical CJK ink is 9×9 dots within a 10-dot advance. One dot is
100 design units, so the outlines are sampled without hints at 10 pixels per
em, thresholded, then doubled with nearest-neighbor scaling. Each original dot
becomes exactly 2×2 pixels. There is no smoothing or synthetic bold.

Typical CJK letterforms are 18×18 pixels with a 20 px advance. Latin letters
generally use 10×14 px ink and a 12 px advance. The shared storage cell is
20×22 px, including space for voiced marks and descenders, with baseline 16.
It fits both the 24 px status bar and 27 px menu rows.

The generated tables contain 11,813 glyphs plus a sentinel, using 815,166 bytes
of flash. The original TTF is not embedded. All source ASCII, kana, and Han
characters are retained; 66 oversized or overhanging characters are omitted
and listed in `omitted.txt`. Unsupported characters display as `□`. UTF-8
names are retained, and text truncation respects character boundaries.

The supplied version's embedded copyright and license are preserved in
`licenses/BoutiqueBitmap9x9-1.6.txt` and apply to this derivative.

Regenerate from the same supplied font:

```sh
uv run --with pillow==11.3.0 --with fonttools python tools/generate_ui_font.py \
  --font /path/to/BoutiqueBitmap9x9_1.6.ttf
```

Upstream: https://github.com/scott0107000/BoutiqueBitmap9x9
