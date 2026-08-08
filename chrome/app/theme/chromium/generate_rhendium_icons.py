#!/usr/bin/env python3
"""Generate Rhendium runtime branding assets from the canonical PNG logo.

This script requires Pillow. Pass a new source image with --source to update the
canonical master and regenerate every packaged desktop icon.
"""

from __future__ import annotations

import argparse
import base64
import shutil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps


THEME_DIR = Path(__file__).resolve().parent
SRC_ROOT = THEME_DIR.parents[3]
MASTER = THEME_DIR / "rhendium_logo_master.png"
RESAMPLING = Image.Resampling.LANCZOS


def logo_on_canvas(source: Image.Image, size: int, scale: float = 1.0) -> Image.Image:
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    logo_size = max(1, round(size * scale))
    logo = source.resize((logo_size, logo_size), RESAMPLING)
    offset = ((size - logo_size) // 2, (size - logo_size) // 2)
    canvas.alpha_composite(logo, offset)
    return canvas


def save_png(source: Image.Image, path: Path, size: int, scale: float = 1.0) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    logo_on_canvas(source, size, scale).save(path, "PNG", optimize=True)


def generate_pngs(source: Image.Image) -> None:
    for size in (16, 24, 48, 64, 128, 256):
        save_png(source, THEME_DIR / f"product_logo_{size}.png", size)

    linux_dir = THEME_DIR / "linux"
    for size in (24, 48, 64, 128, 256):
        save_png(source, linux_dir / f"product_logo_{size}.png", size)

    scaled_assets = {
        SRC_ROOT / "chrome/app/theme/default_100_percent/chromium/product_logo_16.png": 16,
        SRC_ROOT / "chrome/app/theme/default_100_percent/chromium/product_logo_32.png": 32,
        SRC_ROOT / "chrome/app/theme/default_100_percent/chromium/linux/product_logo_16.png": 16,
        SRC_ROOT / "chrome/app/theme/default_100_percent/chromium/linux/product_logo_32.png": 32,
        SRC_ROOT / "chrome/app/theme/default_200_percent/chromium/product_logo_16.png": 32,
        SRC_ROOT / "chrome/app/theme/default_200_percent/chromium/product_logo_32.png": 64,
    }
    for path, pixel_size in scaled_assets.items():
        save_png(source, path, pixel_size)

    mono = logo_on_canvas(source, 22, 16 / 22)
    alpha = mono.getchannel("A")
    gray = ImageOps.grayscale(mono)
    mono = Image.merge("RGBA", (gray, gray, gray, alpha))
    mono.save(THEME_DIR / "product_logo_22_mono.png", "PNG", optimize=True)

    tiles_dir = THEME_DIR / "win/tiles"
    save_png(source, tiles_dir / "Logo.png", 600, 220 / 600)
    save_png(source, tiles_dir / "SmallLogo.png", 176, 118 / 176)

    mac_appicon_dir = THEME_DIR / "mac/Assets.xcassets/AppIcon.appiconset"
    for size in (16, 32, 64, 128, 256, 512, 1024):
        save_png(source, mac_appicon_dir / f"appicon_{size}.png", size, 0.85)

    mac_iconset_dir = THEME_DIR / "mac/Assets.xcassets/Icon.iconset"
    save_png(source, mac_iconset_dir / "icon_256x256.png", 256)
    save_png(source, mac_iconset_dir / "icon_256x256@2x.png", 512)


def generate_windows_icon(source: Image.Image) -> None:
    icon = logo_on_canvas(source, 256)
    icon.save(
        THEME_DIR / "win/chromium.ico",
        "ICO",
        sizes=[(16, 16), (32, 32), (48, 48), (256, 256)],
    )


def generate_macos_icon(source: Image.Image) -> None:
    images = [logo_on_canvas(source, size, 0.85) for size in (32, 64, 128, 256, 512, 1024)]
    images[-1].save(
        THEME_DIR / "mac/app.icns",
        "ICNS",
        append_images=images[:-1],
    )


def generate_wordmarks(source: Image.Image, font_path: Path) -> None:
    for scale, directory in (
        (1, SRC_ROOT / "chrome/app/theme/default_100_percent/chromium"),
        (2, SRC_ROOT / "chrome/app/theme/default_200_percent/chromium"),
    ):
        height = 22 * scale
        logo_size = 22 * scale
        gap = 4 * scale
        font = ImageFont.truetype(str(font_path), 15 * scale)
        probe = Image.new("RGBA", (1, 1))
        probe_draw = ImageDraw.Draw(probe)
        bounds = probe_draw.textbbox((0, 0), "Rhendium", font=font)
        text_width = bounds[2] - bounds[0]
        text_height = bounds[3] - bounds[1]
        width = logo_size + gap + text_width

        for suffix, color in (("", (32, 33, 36, 255)), ("_white", (255, 255, 255, 255))):
            canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            canvas.alpha_composite(source.resize((logo_size, logo_size), RESAMPLING))
            draw = ImageDraw.Draw(canvas)
            text_y = (height - text_height) // 2 - bounds[1]
            draw.text((logo_size + gap - bounds[0], text_y), "Rhendium", font=font, fill=color)
            canvas.save(directory / f"product_logo_name_22{suffix}.png", "PNG", optimize=True)


def generate_svgs(source_path: Path) -> None:
    encoded = base64.b64encode(source_path.read_bytes()).decode("ascii")
    data_uri = f"data:image/png;base64,{encoded}"
    product_svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">\n'
        f'  <image width="256" height="256" href="{data_uri}"/>\n'
        '</svg>\n'
    )
    animation_svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 160">\n'
        f'  <image width="160" height="160" href="{data_uri}" opacity="0">\n'
        '    <animate attributeName="opacity" values="0;1" dur="0.6s" fill="freeze"/>\n'
        '  </image>\n'
        '</svg>\n'
    )
    (THEME_DIR / "product_logo.svg").write_text(product_svg, encoding="utf-8", newline="\n")
    (THEME_DIR / "product_logo_animation.svg").write_text(
        animation_svg, encoding="utf-8", newline="\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=MASTER,
        help="PNG source; when different from the master, it becomes the new master",
    )
    parser.add_argument(
        "--wordmark-font",
        type=Path,
        help="Noto Sans font used to regenerate the Rhendium wordmark assets",
    )
    args = parser.parse_args()

    source_path = args.source.resolve()
    if source_path != MASTER.resolve():
        shutil.copyfile(source_path, MASTER)

    source = Image.open(MASTER).convert("RGBA")
    if source.width != source.height:
        raise ValueError(f"The source logo must be square, got {source.size}")

    generate_pngs(source)
    generate_windows_icon(source)
    generate_macos_icon(source)
    generate_svgs(MASTER)
    if args.wordmark_font:
        generate_wordmarks(source, args.wordmark_font.resolve())


if __name__ == "__main__":
    main()
