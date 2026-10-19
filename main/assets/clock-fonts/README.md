# Erquren Clock pinned fonts

These small source fonts reproduce the typography approved in the 960 × 540
clock canvas preview. The firmware consumes the generated C++ arrays only;
it does not parse fonts or require a runtime filesystem.

## Source and licensing

- `preview-dejavu-sans.woff`: the preview's exact `ClockSans` base64 WOFF,
  DejaVu Sans Book 2.37. Used for 256 px time digits and the 20 px footer.
- `preview-noto-sans-cjk-sc.woff`: the preview's exact `ClockCJK` base64 WOFF,
  Noto Sans CJK SC Regular 2.004. Used for the 24 px Chinese date line.
- `dejavu-sans-hyphen.woff`: U+002D from DejaVu Sans 2.37, added for `--:--`.
  The approved preview's subset did not contain that character.
- `LICENSE-DejaVu.txt`: the complete Bitstream Vera and Arev notices embedded
  in the source font; DejaVu changes are public domain.
- `LICENSE-Noto.txt`: SIL Open Font License 1.1.

The font-derived bitmap data retains the corresponding font license. The
surrounding application license does not replace either font license.
Upstream sources: https://dejavu-fonts.github.io/ and
https://github.com/notofonts/noto-cjk . `manifest.json` records font versions,
SHA-256 checksums of all pinned font inputs, and the approved preview HTML.
The approved preview is provenance, not a regeneration dependency.

## Regenerate and verify

From the repository root, using Python 3.10 or later:

```sh
python3 -m pip install --only-binary=:all: -r main/assets/clock-fonts/requirements.txt
python3 tools/generate_clock_fonts.py
python3 tools/generate_clock_fonts.py --check
```

The generator checks source checksums and the native library versions bundled
with the official Pillow wheel. The pinned toolchain is Pillow 12.3.0,
fontTools 4.61.1, FreeType 2.14.3, libraqm 0.10.5, HarfBuzz 14.2.1, and FriBidi
1.0.16. A mismatching native build fails rather than silently changing pixels.
No system font lookup, network request, time-dependent timestamp, or installed
font is used during ordinary regeneration. Output hashes are printed for audit.

The two preview WOFFs were extracted byte-for-byte from the `@font-face` data
URLs. The hyphen supplement was produced with fontTools 4.61.1 using
`TTFont(source, recalcTimestamp=False)`, `subset.Options()` with
`recalc_timestamp=False`, `Subsetter.populate(text='-')`, `Subsetter.subset()`,
and `font.flavor='woff'` before saving. The original full TTF checksum is in
the manifest. All three WOFF inputs are committed, so repeating this extraction
or accessing the original machine is unnecessary.

## Bitmap contract

- `clock_fonts::TIME` contains `-0123456789:`; digits have equal advances.
- `clock_fonts::DATE` contains space, digits, 年月日星期一二三四五六.
- `FOOTER`/`FOOTER_PIXELS` store the entire `Erquren Clock` string as one
  kerning-aware, precomposed image.
- `left` and `top` are relative to an alphabetic baseline; `advance_64` uses
  1/64 px units. Bounds are cropped to actual visible ink after quantization.
- Pixels are row-major 4-bit luminance, black 0 / white 15, high nibble first.
  Each glyph begins on a byte boundary. Rows have no padding. An unused final
  low nibble is white. `offset` is a byte offset into the font's pixel array.
- White pixels are background; the firmware may skip them when compositing
  onto its white clock canvas. A space has zero dimensions and a real advance.

The font outlines, sizes, tabular advances, and footer kerning come from the
approved preview. Browser/Skia and FreeType can differ at individual edge
pixels because of rasterizer and subpixel-origin choices. Firmware output is
deterministic and should be visually compared to the approved layout rather
than claimed to be pixel-identical to every browser.
