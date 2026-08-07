#!/usr/bin/env python3
"""Verify and stage a Chromime font pack into an application resource tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


FONT_EXTENSIONS = {".otc", ".otf", ".ttc", ".ttf"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def contained_path(root: Path, relative_value: str) -> Path:
    relative = Path(relative_value)
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise ValueError(f"unsafe relative path: {relative_value}")
    candidate = (root / relative).resolve()
    if candidate != root and root not in candidate.parents:
        raise ValueError(f"path leaves pack directory: {relative_value}")
    return candidate


def verify(profile_path: Path, pack_dir: Path) -> tuple[dict, dict]:
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    font_pack = profile["font_pack"]
    manifest_path = pack_dir / "manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    actual_manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    if actual_manifest_hash != font_pack["manifest_sha256"]:
        raise ValueError("manifest SHA-256 does not match the profile")

    manifest = json.loads(manifest_bytes)
    if (
        manifest["pack_id"] != font_pack["id"]
        or pack_dir.name != font_pack["id"]
    ):
        raise ValueError("profile, manifest, and pack directory IDs differ")
    expected_manifest = f"packs/{font_pack['id']}/manifest.json"
    if font_pack["manifest"].replace("\\", "/") != expected_manifest:
        raise ValueError("profile manifest path does not match the staged layout")

    def verify_entries(entries: list[dict], kind: str) -> set[Path]:
        verified: set[Path] = set()
        if not entries:
            raise ValueError(f"manifest has no {kind} entries")
        for entry in entries:
            path = contained_path(pack_dir, entry["path"])
            if not path.is_file():
                raise ValueError(f"missing pack file: {entry['path']}")
            if path.stat().st_size != entry["size"]:
                raise ValueError(f"size mismatch: {entry['path']}")
            if sha256(path) != entry["sha256"]:
                raise ValueError(f"SHA-256 mismatch: {entry['path']}")
            verified.add(path)
        return verified

    listed_files = verify_entries(manifest["files"], "font")
    listed_licenses = verify_entries(manifest["licenses"], "license")
    listed_fonts = {
        path for path in listed_files if path.suffix.lower() in FONT_EXTENSIONS
    }

    actual_fonts = {
        path.resolve()
        for path in (pack_dir / "fonts").rglob("*")
        if path.is_file() and path.suffix.lower() in FONT_EXTENSIONS
    }
    extras = actual_fonts - listed_fonts
    if extras:
        raise ValueError(f"unlisted font file: {min(extras)}")
    actual_licenses = {
        path.resolve()
        for path in (pack_dir / "licenses").rglob("*")
        if path.is_file()
    }
    if actual_licenses != listed_licenses:
        raise ValueError("license directory does not match the manifest")

    available_families = {
        face["family"]
        for entry in manifest["files"]
        for face in entry["faces"]
    }
    fallback = profile["fallback"]
    required_families = {
        *profile["generic_families"].values(),
        *profile["aliases"].values(),
        fallback["default_text_family"],
        fallback["last_resort_family"],
        fallback["emoji_family"],
        fallback["math_family"],
    }
    for families in fallback["cjk_by_language"].values():
        required_families.update(families)
    missing_families = sorted(required_families - available_families)
    if missing_families:
        raise ValueError(
            "profile references font families absent from the manifest: "
            + ", ".join(missing_families)
        )
    return profile, manifest


def stage(args: argparse.Namespace) -> Path:
    profile_path = args.profile.resolve()
    pack_dir = args.pack_dir.resolve()
    _, manifest = verify(profile_path, pack_dir)

    resources_dir = args.resources_dir.resolve()
    destination = resources_dir / "chromime-fonts"
    if destination.exists():
        raise ValueError(f"destination already exists: {destination}")
    resources_dir.mkdir(parents=True, exist_ok=True)

    temporary = Path(tempfile.mkdtemp(prefix="chromime-fonts-", dir=resources_dir))
    try:
        staged_pack = temporary / "packs" / manifest["pack_id"]
        staged_pack.mkdir(parents=True)
        copy_function = os.link if args.hardlink else shutil.copy2
        copy_function(pack_dir / "manifest.json", staged_pack / "manifest.json")
        for entry in [*manifest["files"], *manifest["licenses"]]:
            source = contained_path(pack_dir, entry["path"])
            destination_file = staged_pack / Path(entry["path"])
            destination_file.parent.mkdir(parents=True, exist_ok=True)
            copy_function(source, destination_file)
        shutil.copy2(profile_path, temporary / "active-profile.json")
        temporary.replace(destination)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return destination


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--pack-dir", type=Path, required=True)
    parser.add_argument("--resources-dir", type=Path, required=True)
    parser.add_argument("--hardlink", action="store_true")
    return parser.parse_args()


def main() -> int:
    try:
        destination = stage(parse_args())
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
