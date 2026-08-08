# Rhendium font-pack builder

`build_noto_pack.py` creates the default immutable Rhendium font pack from
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
python -m pip install -r tools/rhendium_fonts/requirements.txt
python tools/rhendium_fonts/build_noto_pack.py `
  --downloads C:\rhendium\font-packs\downloads `
  --output C:\rhendium\font-packs\build
```

## Stage a verified pack

Release packaging and local browser tests use the same staging command:

```powershell
python tools/rhendium_fonts/stage_font_pack.py `
  --profile components/rhendium_fonts/profiles/noto-kde-canonical-v1.json `
  --pack-dir C:\rhendium\font-packs\build\noto-canonical-2026.08.01-1 `
  --resources-dir out\BuildCheck
```

This creates `rhendium-fonts/active-profile.json` plus the versioned pack under
the selected resources directory. It verifies the manifest and every listed
font and license before staging. Add `--hardlink` for a local Windows/Linux
build on the same filesystem; release packaging should use the default
independent copy.
