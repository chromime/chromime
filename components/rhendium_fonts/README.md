# Rhendium deterministic fonts

Rhendium ordinary web content uses an explicit, versioned font pack. It must
not enumerate, match, activate, or fall back to fonts installed on the host
operating system. Browser-owned UI remains on Chromium's platform font path.

## Security and determinism boundary

The following rules are product invariants, not user preferences:

- Host font discovery is denied for eligible ordinary web content.
- CSS `@font-face local()` sources are skipped.
- A configured font must resolve to a file in a verified font-pack manifest.
- Every font file is verified by SHA-256 before it can enter the renderer font
  cache.
- Missing coverage ends in the pack's last-resort face or deterministic tofu;
  it never falls through to DirectWrite, CoreText, or Fontconfig.
- Configuration and font-pack changes take effect only after browser restart.

Downloaded web fonts remain permitted, but are separate from local font
lookup and must use Rhendium's common Fontations-backed creation path.

## Storage model

Small configuration and lock files live in the Chromium repository. Font
binaries do not: the default pack is built from pinned upstream archives and
published as an immutable Rhendium font-pack artifact.

An installed application has this logical layout:

```text
<application resources>/rhendium-fonts/
  active-profile.json
  packs/
    noto-canonical-2026.08.01-1/
      manifest.json
      licenses/
      fonts/
```

Platform packaging maps `<application resources>` as follows:

- Windows: beside `chrome.exe`, under `rhendium-fonts/`.
- Linux: under the versioned Rhendium library directory, under
  `rhendium-fonts/`.
- macOS: inside `Rhendium.app/Contents/Resources/rhendium-fonts/`.

The same manifest and font bytes are installed on all three platforms.

Rhendium loads `<application resources>/rhendium-fonts/active-profile.json`
automatically and refuses to start if neither that resource nor an explicit
profile is available. A user can select another profile with the explicit
`--rhendium-font-config=<absolute path>` command-line option. Relative pack
paths are resolved relative to the configuration file, not the current
directory.

## Profiles

The built-in `noto-kde-canonical-v1` profile is the default. Additional
profiles can reproduce the *character* of another Linux distribution by
shipping another exact, licensed font pack and changing generic-family and
fallback mappings. They are not allowed to consult that distribution's
installed fonts and should be described as “inspired” unless the complete
distribution version and rendering environment are pinned.

For example, an Ubuntu-inspired profile can bundle Ubuntu Sans and Ubuntu
Mono as its primary families while retaining the pinned Noto pack for global
coverage. A DejaVu-inspired profile can do the same with DejaVu. The profile
must produce the same output on Windows, macOS, and Linux because it uses the
same bytes everywhere.

All profiles use the same rendering protocol: text gamma 1.2, text contrast
0.2, slight bytecode hinting, grayscale antialiasing, fractional glyph
positioning, no LCD subpixel rendering, no autohinter, and embedded bitmaps
enabled. Rhendium also uses one cross-platform ascent/descent rule and one
maximum-character-width calculation for ordinary web content. These metrics
keep text baselines and the intrinsic width of form controls independent of
the Blink platform port. A profile may change bundled families and
deterministic fallback order, but it may not inherit or override rendering
settings from the host OS. This restriction keeps an Ubuntu-inspired or
DejaVu-inspired profile reproducible on every supported operating system.

See `font_config.schema.json` for the configuration contract.
