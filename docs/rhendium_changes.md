# Rhendium changes to Chromium

Rhendium is a Chromium fork for **deterministic Chromium rendering**. Its
release contract is that eligible ordinary web content uses the same verified
font bytes, family selection, fallback order, font metrics, and text raster
parameters on Windows, Linux, and macOS. A pinned KDE Chromium environment is
the visual design reference; Rhendium's versioned cross-platform result is the
release authority.

This document records the fork-specific behavior that must be preserved when
updating or rebasing Chromium. It describes the initial implementation based on
Chromium 153.0.7995.0. Future rendering changes should update this document in
the same commit.

## Product boundary

Rhendium applies its deterministic font policy to ordinary Blink web content.
It deliberately does not replace the platform font path for the browser frame,
menus, native dialogs, DevTools, `chrome://` and other WebUI pages, extension
UI, or other browser-owned surfaces. Those retain Chromium's normal
platform-specific appearance.

The current deterministic contract includes these invariants:

- Eligible web renderers do not enumerate, match, activate, or fall back to
  fonts installed on the host operating system.
- CSS `@font-face local()` sources are unavailable in eligible web content.
- Downloaded `@font-face` data remains supported through the common
  Fontations-backed web-font path.
- Bundled fonts, manifests, profiles, and licenses are versioned independently
  of the browser executable. Every configured font file is verified by SHA-256
  before use.
- Unknown families and missing glyphs use the configured deterministic fallback
  order. Exhausted coverage ends in the configured last-resort face or tofu,
  never DirectWrite, CoreText, or Fontconfig.
- Text uses grayscale antialiasing, fractional positioning, slight bytecode
  hinting, gamma 1.2, contrast 0.2, no LCD subpixel rendering, no autohinter,
  and common ascent, descent, and maximum-character-width calculations.
- Browser launches use software compositing by default so ordinary page
  screenshots follow the same compositor path on every supported platform.
  Chromium's bundled SwiftShader remains available for WebGL-only content.
- Profile or pack changes require a browser restart.

These guarantees do not by themselves make GPU-dependent WebGL, WebGPU, video,
HDR, color-management, animation timing, or other hardware-dependent output
pixel-identical. Those surfaces need separate protocol rules and tests.

## Configuration and font-pack tooling

`tools/rhendium_fonts/` builds and stages the canonical font pack:

- `build_noto_pack.py` downloads pinned official Noto inputs, selects the
  canonical hinted primary Latin faces and compact global fallback set, and
  emits a manifest containing source hashes, file hashes, names, styles,
  variation axes, color-font tables, collection indices, coverage, and license
  information.
- `stage_font_pack.py` verifies the profile, manifest, every font, and every
  license before creating an application-ready `rhendium-fonts/` directory.
- `requirements.txt` isolates the required FontTools version from Chromium's
  general Python environment.

`components/rhendium_fonts/` contains the runtime contract:

- `font_config.schema.json` defines the configuration format.
- `profiles/noto-kde-canonical-v1.json` is the canonical Noto profile.
- `profiles/custom-profile.example.json` demonstrates how a different exact
  font pack can change family mappings while preserving cross-platform
  rendering parameters.
- `rhendium_font_manager.*` validates the configuration and manifest, verifies
  font bytes, constructs the restricted `SkFontMgr`, resolves aliases and
  fallback from recorded coverage, and rejects host-dependent rendering
  settings.
- `rhendium_font_manager_unittest.cc` covers loading, aliases, fallback,
  Unicode paths, modified or unlisted fonts, and invalid rendering profiles.

The logical installed layout is:

```text
<application resources>/rhendium-fonts/
  active-profile.json
  packs/
    <font-pack-id>/
      manifest.json
      licenses/
      fonts/
```

On macOS, application resources are inside
`Rhendium.app/Contents/Resources/`. On Windows they are beside `chrome.exe`;
Linux packages place them in the versioned browser library directory. Release
browser archives intentionally omit this directory because all operating
systems share one separately published font-pack archive.

## Startup and command-line contract

Rhendium adds the public switch:

```text
--rhendium-font-config=/absolute/path/to/active-profile.json
```

`chrome/app/chrome_main_delegate.cc` supplies the profile under application
resources when the switch is absent. Browser startup fails when the selected
path is relative or missing. This fail-closed behavior prevents an accidental
return to host fonts.

The standalone browser archive therefore needs an explicit switch unless the
font pack has been installed in its application-resource location. The
`@rhendium/browser` and `@rhendium/playwright` packages supply the external
profile automatically.

Rhendium also adds `--disable-gpu` and `--enable-unsafe-swiftshader` to browser
launches by default. This keeps software compositing while allowing Chromium's
bundled SwiftShader to support pages that require WebGL. Chromium's existing
`--enable-gpu` switch is the explicit opt-out for workloads that need hardware
GPU acceleration; those launches are outside Rhendium's pixel-consistency
contract. Supplying `--enable-gpu` with `--disable-gpu` is an unsupported
conflicting configuration and fails startup.

## Renderer scoping and service isolation

`chrome/browser/chrome_content_browser_client.cc` assigns Rhendium mode only
to ordinary renderer processes. WebUI and extension processes are excluded. It
copies the font-profile switch only to selected processes and adds the fixed
Skia gamma and contrast switches.

The Content layer records that choice per child process in
`ChildProcessSecurityPolicy`. FontDataService binding checks this state instead
of trusting a renderer-provided command line. The browser owns separate host
and Rhendium FontDataService instances:

- the host service uses Chromium's platform `SkFontMgr` and local-font matcher;
- the Rhendium service uses the verified restricted font manager and does not
  create a local-font matcher.

The corresponding changes are primarily in:

- `content/browser/renderer_host/render_process_host_impl_receiver_bindings.cc`
- `content/browser/security/cpsp/child_process_security_policy_impl.*`
- `components/services/font_data/font_data_service_impl.*`
- `components/services/font_data/public/mojom/font_data_service.mojom`

This process boundary is security-relevant. New renderer creation or service
binding paths must not let eligible pages obtain the host font service.

## Blink and platform-font integration

Rhendium adds the `RhendiumDeterministicFonts` Blink runtime feature. The
browser-controlled profile switch enables it for selected web renderers and
disables the Font Access runtime feature there.

Blink font-cache changes route generic-family lookup and character fallback to
the configured manager. Platform-specific paths on Windows, Linux, and macOS
skip DirectWrite, Fontconfig, and CoreText matching or native typeface creation
when Rhendium mode is active. Downloaded fonts use the common web-font factory.
The main integration points are:

- `third_party/blink/renderer/platform/fonts/font_cache.*`
- `third_party/blink/renderer/platform/fonts/{linux,mac,win}/`
- `third_party/blink/renderer/platform/fonts/skia/font_cache_skia.cc`
- `third_party/blink/renderer/platform/fonts/web_font_typeface_factory.cc`
- `third_party/blink/renderer/core/css/local_font_face_source.cc`
- `content/child/font_data/font_data_manager.cc`
- `content/renderer/renderer_blink_platform_impl.cc`

Rhendium also normalizes `FontPlatformData`, `SimpleFontData`,
`WebFontRenderStyle`, `FontMetrics`, and Blink's cached Skia text parameters so
ordinary text metrics and raster choices do not inherit different Blink
platform-port defaults.

The renderer does not start macOS CoreText prewarming when Rhendium mode is
active, because that asynchronous initialization can race the restricted font
manager installation. Software raster surfaces preserve the configured gamma
and contrast even when LCD text is disabled. On ARM, eligible renderer and
GPU/Viz processes select Rhendium's source-over row blitter, which uses the
same integer rounding as Skia's x86 SSE2/AVX2 path. These rules keep text,
emoji transparency, and Canvas edges byte-identical without changing browser
UI, WebUI, or extension renderer font behavior.

## Branding and application identity

The Chromium branding resources identify the product as Rhendium. The initial
branding change includes:

- product and installer names, translated Chromium resource strings, About and
  menu text;
- the `org.rhendium.Rhendium` macOS bundle identifier;
- Windows install identity, ProgIDs, URL scheme, and installer constants;
- Linux package name and installation directories;
- Rhendium icons for Windows, Linux, macOS, and shared resources;
- `generate_rhendium_icons.py` and the retained master logo used to regenerate
  raster assets.

Browser UI remains Chromium's native UI implementation; branding does not try
to make browser chrome pixel-identical across operating systems.

## Build and verification targets

The Rhendium code is part of the normal `chrome` target. The focused runtime
unit-test target is:

```sh
autoninja -C out/Release rhendium_font_manager_unittests
out/Release/rhendium_font_manager_unittests
```

Release validation must additionally cover:

1. startup fails without a staged or explicitly selected profile;
2. startup succeeds with the verified canonical font pack;
3. ordinary pages cannot resolve host-only fonts or `local()` sources;
4. WebUI and browser-owned surfaces remain on Chromium's host font path;
5. headed and headless screenshots and layout metrics are compared across all
   supported operating systems under the same scale, viewport, color, and
   locale, with default software compositing; `--enable-gpu` opt-out behavior
   and conflicting GPU switches are tested separately;
6. browser and font archives are separate and their size and SHA-256 values
   match the Playwright build manifest;
7. Playwright launches and shuts down Rhendium without orphan processes.

## Initial fork commit series

The initial implementation was developed as these commits after upstream
Chromium commit `810b6b4567042`:

```text
b6f46ed048197  Chromime: add deterministic Noto font pack tooling
2735648c31fcf  Chromime: add verified cross-platform font manager
cd4d3407c6ec  Chromime: add verified font pack staging
1bd726b480bb  Chromime: include font licenses in verified packs
1e2873c90a59  Chromime: enforce deterministic fonts for web content
f7a7bd605c93  Chromime: fix renderer font service binding
d39e62b52558  Chromime: validate staged font profile families
9cf971ac7031  Enforce cross-platform text raster metrics
6145bff9dbb2  Rebrand project as Rhendium
```

The historical `Chromime` prefix in early commit subjects is the former project
name. Source identifiers added after the rename use `Rhendium`.
