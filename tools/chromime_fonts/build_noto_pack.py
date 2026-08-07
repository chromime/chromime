#!/usr/bin/env python3
"""Builds Chromime's deterministic default Noto font pack."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from collections import defaultdict
from pathlib import Path, PurePosixPath
from typing import Iterable

from fontTools.ttLib import TTCollection, TTFont


PACK_ID = "noto-canonical-2026.08.01-1"
SOURCES = {
    "monthly": {
        "file": "noto-monthly-2026.08.01.zip",
        "url": "https://github.com/notofonts/notofonts.github.io/archive/refs/tags/noto-monthly-release-2026.08.01.zip",
        "version": "noto-monthly-release-2026.08.01",
    },
    "cjk_sans": {
        "file": "noto-sans-cjk-2.004-super-otc.zip",
        "url": "https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/00_NotoSansCJK.ttc.zip",
        "version": "Sans2.004",
    },
    "cjk_serif": {
        "file": "noto-serif-cjk-2.003-super-otc.zip",
        "url": "https://github.com/notofonts/noto-cjk/releases/download/Serif2.003/01_NotoSerifCJK.ttc.zip",
        "version": "Serif2.003",
    },
    "cjk_serif_license": {
        "file": "noto-serif-cjk-2.003.LICENSE",
        "url": "https://raw.githubusercontent.com/notofonts/noto-cjk/Serif2.003/Serif/LICENSE",
        "version": "Serif2.003",
    },
    "emoji": {
        "file": "NotoColorEmoji-2.051.ttf",
        "url": "https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.051/fonts/NotoColorEmoji.ttf",
        "version": "v2.051",
    },
    "emoji_license": {
        "file": "NotoColorEmoji-2.051.LICENSE",
        "url": "https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.051/fonts/LICENSE",
        "version": "v2.051",
    },
}

PRIMARY_HINTED_FAMILIES = ("NotoSans", "NotoSerif", "NotoSansMono")
PRIMARY_WEIGHTS = (
    "Thin", "ExtraLight", "Light", "Regular", "Medium", "SemiBold", "Bold",
    "ExtraBold", "Black",
)
VARIABLE_TIERS = (
    ("unhinted", "variable-ttf"),
    ("full", "variable-ttf"),
    ("googlefonts", "variable-ttf"),
    ("unhinted", "slim-variable-ttf"),
    ("full", "slim-variable-ttf"),
    ("googlefonts", "slim-variable-ttf"),
)
STATIC_TIERS = (
    ("hinted", "ttf"),
    ("unhinted", "ttf"),
    ("full", "ttf"),
    ("googlefonts", "ttf"),
    ("unhinted", "otf"),
    ("full", "otf"),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_write_zip_entry(archive: zipfile.ZipFile, entry: zipfile.ZipInfo,
                         destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(entry) as source, destination.open("wb") as target:
        shutil.copyfileobj(source, target, length=1024 * 1024)


def monthly_font_parts(entry_name: str) -> tuple[str, str, str, str] | None:
    parts = PurePosixPath(entry_name).parts
    try:
        fonts_index = parts.index("fonts")
    except ValueError:
        return None
    remaining = parts[fonts_index + 1:]
    if len(remaining) != 4:
        return None
    family, flavor, font_format, filename = remaining
    if Path(filename).suffix.lower() not in (".ttf", ".otf"):
        return None
    return family, flavor, font_format, filename


def primary_hinted_filenames(family: str) -> set[str]:
    filenames = {f"{family}-{weight}.ttf" for weight in PRIMARY_WEIGHTS}
    if family != "NotoSansMono":
        filenames.add(f"{family}-Italic.ttf")
        filenames.update(
            f"{family}-{weight}Italic.ttf"
            for weight in PRIMARY_WEIGHTS if weight != "Regular")
    return filenames


def static_core_entries(entries: list[zipfile.ZipInfo]) -> list[zipfile.ZipInfo]:
    styles = ("-Regular", "-Bold", "-Italic", "-BoldItalic")
    selected = [
        entry for entry in entries
        if PurePosixPath(entry.filename).stem.endswith(styles)
    ]
    if selected:
        return selected
    regular = [
        entry for entry in entries
        if "regular" in PurePosixPath(entry.filename).stem.lower()
    ]
    return regular or [min(entries, key=lambda item: item.filename)]


def select_family_entries(
        family: str,
        tiers: dict[tuple[str, str], list[zipfile.ZipInfo]],
) -> tuple[str, str, list[zipfile.ZipInfo]]:
    if family in PRIMARY_HINTED_FAMILIES:
        tier = ("hinted", "ttf")
        wanted = primary_hinted_filenames(family)
        selected = [
            entry for entry in tiers[tier]
            if PurePosixPath(entry.filename).name in wanted
        ]
        found = {PurePosixPath(entry.filename).name for entry in selected}
        missing = sorted(wanted - found)
        if missing:
            raise RuntimeError(
                f"Missing primary hinted faces for {family}: {', '.join(missing)}")
        return "/".join(tier), "all-normal-width-static-weights", selected

    for tier in VARIABLE_TIERS:
        if tiers[tier]:
            return "/".join(tier), "variable", tiers[tier]
    for tier in STATIC_TIERS:
        if tiers[tier]:
            return "/".join(tier), "static-core-styles", static_core_entries(
                tiers[tier])
    raise RuntimeError(f"No supported font format for {family}")


def extract_monthly(downloads: Path,
                    pack_root: Path) -> dict[str, dict[str, object]]:
    source_path = downloads / SOURCES["monthly"]["file"]
    by_family_tier: dict[str, dict[tuple[str, str], list[zipfile.ZipInfo]]] = (
        defaultdict(lambda: defaultdict(list)))
    with zipfile.ZipFile(source_path) as archive:
        license_entry = None
        for entry in archive.infolist():
            normalized = entry.filename.replace("\\", "/")
            if normalized.endswith("/fonts/LICENSE"):
                license_entry = entry
            parsed = monthly_font_parts(normalized)
            if parsed is None:
                continue
            family, flavor, font_format, _ = parsed
            by_family_tier[family][(flavor, font_format)].append(entry)

        selections: dict[str, dict[str, object]] = {}
        for family in sorted(by_family_tier):
            tiers = by_family_tier[family]
            tier, mode, selected = select_family_entries(family, tiers)
            selections[family] = {
                "tier": tier,
                "mode": mode,
                "files": len(selected),
            }
            for entry in sorted(selected, key=lambda item: item.filename):
                filename = PurePosixPath(entry.filename).name
                safe_write_zip_entry(
                    archive, entry,
                    pack_root / "fonts" / "non-cjk" / family / filename)

        if license_entry is None:
            raise RuntimeError("Monthly Noto archive has no fonts/LICENSE")
        safe_write_zip_entry(archive, license_entry,
                             pack_root / "licenses" / "OFL-Noto.txt")
    return selections


def extract_single_collection(downloads: Path, source_key: str,
                              expected_name: str, destination_name: str,
                              license_name: str, pack_root: Path,
                              license_source_key: str | None = None) -> None:
    source_path = downloads / SOURCES[source_key]["file"]
    with zipfile.ZipFile(source_path) as archive:
        font_entries = [
            entry for entry in archive.infolist()
            if PurePosixPath(entry.filename).name == expected_name
        ]
        if len(font_entries) != 1:
            raise RuntimeError(
                f"Expected one {expected_name} in {source_path}, found "
                f"{len(font_entries)}")
        license_entries = [
            entry for entry in archive.infolist()
            if PurePosixPath(entry.filename).name == "LICENSE"
        ]
        safe_write_zip_entry(archive, font_entries[0],
                             pack_root / "fonts" / "cjk" / destination_name)
        if license_entries:
            safe_write_zip_entry(archive, license_entries[0],
                                 pack_root / "licenses" / license_name)
        elif license_source_key is not None:
            shutil.copy2(downloads / SOURCES[license_source_key]["file"],
                         pack_root / "licenses" / license_name)
        else:
            raise RuntimeError(f"No LICENSE in {source_path}")


def copy_emoji(downloads: Path, pack_root: Path) -> None:
    destination = pack_root / "fonts" / "emoji" / "NotoColorEmoji.ttf"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(downloads / SOURCES["emoji"]["file"], destination)
    shutil.copy2(downloads / SOURCES["emoji_license"]["file"],
                 pack_root / "licenses" / "OFL-Noto-Color-Emoji.txt")


def coverage_ranges(codepoints: Iterable[int]) -> list[str]:
    points = sorted(set(codepoints))
    if not points:
        return []
    ranges: list[str] = []
    start = previous = points[0]
    for point in points[1:]:
        if point == previous + 1:
            previous = point
            continue
        ranges.append(format_range(start, previous))
        start = previous = point
    ranges.append(format_range(start, previous))
    return ranges


def format_range(start: int, end: int) -> str:
    if start == end:
        return f"U+{start:04X}"
    return f"U+{start:04X}-{end:04X}"


def get_name(font: TTFont, name_id: int) -> str:
    name_table = font.get("name")
    if name_table is None:
        return ""
    value = name_table.getDebugName(name_id)
    return value or ""


def classify_family(family: str) -> str:
    if family in ("Noto Sans", "Noto Serif", "Noto Sans Mono"):
        return "primary"
    if family == "Noto Color Emoji":
        return "emoji_text"
    lowered = family.lower()
    if "symbols" in lowered:
        return "symbols"
    if "math" in lowered:
        return "math"
    if "music" in lowered:
        return "music"
    if family.startswith("Noto Sans "):
        return "noto_sans_script"
    if family.startswith("Noto Serif "):
        return "noto_serif_script"
    return "specialist"


def font_face_metadata(font: TTFont, index: int) -> dict[str, object]:
    family = get_name(font, 16) or get_name(font, 1)
    subfamily = get_name(font, 17) or get_name(font, 2)
    os2 = font.get("OS/2")
    post = font.get("post")
    cmap = font.getBestCmap() or {}
    axes = []
    if "fvar" in font:
        axes = [{
            "tag": axis.axisTag,
            "min": axis.minValue,
            "default": axis.defaultValue,
            "max": axis.maxValue,
        } for axis in font["fvar"].axes]
    return {
        "collection_index": index,
        "family": family,
        "subfamily": subfamily,
        "full_name": get_name(font, 4),
        "postscript_name": get_name(font, 6),
        "weight": getattr(os2, "usWeightClass", 400),
        "width": getattr(os2, "usWidthClass", 5),
        "italic_angle": getattr(post, "italicAngle", 0),
        "family_class": classify_family(family),
        "variation_axes": axes,
        "color_tables": sorted(table for table in
                               ("COLR", "CPAL", "CBDT", "CBLC", "sbix", "SVG ")
                               if table in font),
        "coverage": coverage_ranges(cmap.keys()),
    }


def read_faces(path: Path) -> list[dict[str, object]]:
    if path.suffix.lower() in (".ttc", ".otc"):
        collection = TTCollection(path, lazy=True)
        try:
            return [font_face_metadata(font, index)
                    for index, font in enumerate(collection.fonts)]
        finally:
            collection.close()
    font = TTFont(path, lazy=True)
    try:
        return [font_face_metadata(font, 0)]
    finally:
        font.close()


def make_manifest(downloads: Path, pack_root: Path,
                  monthly_selections: dict[str, dict[str, object]],
                  ) -> dict[str, object]:
    source_entries = []
    for key in sorted(SOURCES):
        source = SOURCES[key]
        path = downloads / source["file"]
        source_entries.append({
            "name": key,
            "version": source["version"],
            "url": source["url"],
            "file": source["file"],
            "size": path.stat().st_size,
            "sha256": sha256(path),
        })

    font_entries = []
    font_paths = sorted(
        path for path in (pack_root / "fonts").rglob("*") if path.is_file())
    for index, path in enumerate(font_paths, 1):
        relative = path.relative_to(pack_root).as_posix()
        print(f"[{index}/{len(font_paths)}] {relative}", flush=True)
        font_entries.append({
            "path": relative,
            "size": path.stat().st_size,
            "sha256": sha256(path),
            "faces": read_faces(path),
        })

    license_entries = []
    license_paths = sorted(
        path for path in (pack_root / "licenses").rglob("*")
        if path.is_file())
    for path in license_paths:
        license_entries.append({
            "path": path.relative_to(pack_root).as_posix(),
            "size": path.stat().st_size,
            "sha256": sha256(path),
        })

    return {
        "schema_version": 1,
        "pack_id": PACK_ID,
        "policy": {
            "host_font_access": "deny",
            "css_local_sources": "ignore",
            "missing_glyph": "deterministic_tofu",
        },
        "sources": source_entries,
        "selection": {
            "primary_hinted_families": list(PRIMARY_HINTED_FAMILIES),
            "primary_hinted_policy": "normal-width-static-weights-100-900",
            "non_cjk_families": monthly_selections,
            "cjk": "static-super-otc",
            "emoji": "cbdt-cblc",
        },
        "files": font_entries,
        "licenses": license_entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--downloads", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()

    missing = [source["file"] for source in SOURCES.values()
               if not (args.downloads / source["file"]).is_file()]
    if missing:
        parser.error("Missing source files: " + ", ".join(missing))

    pack_root = args.output / PACK_ID
    if pack_root.exists():
        if not args.replace:
            parser.error(f"Output exists: {pack_root}; pass --replace")
        shutil.rmtree(pack_root)
    pack_root.mkdir(parents=True)

    monthly_selections = extract_monthly(args.downloads, pack_root)
    extract_single_collection(
        args.downloads, "cjk_sans", "NotoSansCJK.ttc",
        "NotoSansCJK.ttc", "OFL-Noto-Sans-CJK.txt", pack_root)
    extract_single_collection(
        args.downloads, "cjk_serif", "NotoSerifCJK.ttc",
        "NotoSerifCJK.ttc", "OFL-Noto-Serif-CJK.txt", pack_root,
        "cjk_serif_license")
    copy_emoji(args.downloads, pack_root)

    manifest = make_manifest(args.downloads, pack_root, monthly_selections)
    manifest_path = pack_root / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(f"manifest={manifest_path}")
    print(f"manifest_sha256={sha256(manifest_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
