# Chromime font-pack builder

`build_noto_pack.py` creates the default immutable Chromime font pack from
pinned official upstream inputs. It deliberately extracts one canonical font
set rather than every redundant format in the upstream archives.

The primary Noto Sans, Noto Serif, and Noto Sans Mono families use hinted,
normal-width static TTF faces for every CSS weight from 100 through 900.
Sans and Serif retain upright and italic faces; Mono has upright faces because
the pinned Noto release does not provide separate hinted Mono italics. This is
45 files: 30,271,176 installed bytes or 15,303,849 ZIP-compressed bytes in the
pinned upstream archive.

Other non-CJK families use this compact selection policy:

1. unhinted variable TTF;
2. another upstream variable TTF tier if needed;
3. Regular, Bold, Italic, and Bold Italic from a static tier;
4. one deterministic static face when those core styles do not exist.

Condensed and extra-condensed primary faces are deliberately excluded. CJK
uses the official static Super OTC files and emoji uses the official
CBDT/CBLC Noto Color Emoji file.

The output manifest records source hashes, file hashes, collection indices,
names, style metadata, variation axes, color-font tables, and compressed
Unicode coverage ranges. Runtime code must verify the manifest and individual
files before loading them.

The script requires `fonttools==4.60.2`, matching Chromium's existing Perfetto
Python requirement pin. Install `requirements.txt` into an isolated Python
environment; do not add it to Chromium's general build environment.

Example:

```powershell
python -m pip install -r tools/chromime_fonts/requirements.txt
python tools/chromime_fonts/build_noto_pack.py `
  --downloads C:\chromime\font-packs\downloads `
  --output C:\chromime\font-packs\build
```
