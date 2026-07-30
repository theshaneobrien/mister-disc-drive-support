#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".github"))

from db_helpers import (  # noqa: E402
    DirectFile,
    build_direct_database,
    generation_timestamp,
    generator_parser,
    git_file_revision,
    github_latest_release,
    github_raw_url,
    http_get_bytes,
    matching_release_asset,
    release_asset_url,
    write_bundle,
)


FOLDER = "mister-disc"
UPSTREAM = "theshaneobrien/mister-disc-drive-support"

# anchored: releases now carry more assets (translate install zip, MGL zip)
# and an unanchored "MiSTer-disc" could match "MiSTer-disc-RA" first
PLAIN_ASSET = re.compile(r"^MiSTer-disc$")
RA_ASSET = re.compile(r"^MiSTer-disc-RA$")

MGL_FILES = (
    "3DO.mgl",
    "Mega CD.mgl",
    "Neo Geo CD.mgl",
    "Philips CD-i.mgl",
    "PlayStation.mgl",
    "Saturn.mgl",
    "TurboGrafx-CD.mgl",
)

# on-the-fly translation payload, vendored under payload/ in this entry
# (same SD-root layout as the upstream release's install zip). Note what is
# deliberately NOT here: translate.ini (user-owned, holds the API key -
# translate_start.sh creates it from a template on first run) and
# hotkey.cfg (written by the SetTranslateHotkey script).
TRANSLATE_FILES = (
    "translate/translate_daemon.py",
    "translate/translate_start.sh",
    "translate/README.md",
    "Scripts/SetTranslateHotkey.sh",
)


def mgl_destination(name: str) -> str:
    return f"_Disc_Cores/{name}"


def validate_main_binary(name: str, data: bytes) -> None:
    if not data.startswith(b"\x7fELF"):
        raise RuntimeError(f"{name} is not an ELF binary")
    if not 500_000 < len(data) < 8_000_000:
        raise RuntimeError(f"{name} has an implausible size: {len(data)}")


def validate_mgl(name: str, data: bytes) -> None:
    text = data.decode("utf-8")
    for needle in (
        "<mistergamedescription>",
        "<rbf>_Console/",
        'same_dir="1"',
        ">CD-",
    ):
        if needle not in text:
            raise RuntimeError(f"{name} is missing {needle!r}")


def validate_script(name: str, data: bytes) -> None:
    if not (data.startswith(b"#!") or name.endswith(".md")):
        raise RuntimeError(f"{name} does not look like a script (missing shebang)")
    if len(data) < 100:
        raise RuntimeError(f"{name} is implausibly small: {len(data)}")


def main() -> int:
    args = generator_parser(FOLDER, "Generate the MiSTer Disc database").parse_args()
    release = github_latest_release(UPSTREAM)

    plain = matching_release_asset(release, PLAIN_ASSET)
    ra = matching_release_asset(release, RA_ASSET)
    plain_url = release_asset_url(plain)
    ra_url = release_asset_url(ra)
    plain_data = http_get_bytes(plain_url)
    ra_data = http_get_bytes(ra_url)
    validate_main_binary("MiSTer-disc", plain_data)
    validate_main_binary("MiSTer-disc-RA", ra_data)
    if plain_data == ra_data:
        raise RuntimeError("Plain and RA builds are identical: wrong release layout")

    # binaries land under their release names - the ini routing lines
    # (MAIN=MiSTer-disc / main=MiSTer-disc-RA) reference these exact names,
    # so they are permanent API
    direct_files = [
        DirectFile(path="MiSTer-disc", url=plain_url, data=plain_data),
        DirectFile(path="MiSTer-disc-RA", url=ra_url, data=ra_data),
    ]
    for name in MGL_FILES:
        path = Path(__file__).with_name("mgl") / name
        data = path.read_bytes()
        validate_mgl(name, data)
        direct_files.append(
            DirectFile(
                path=mgl_destination(name),
                url=github_raw_url(
                    args.repository,
                    git_file_revision(path),
                    f"{FOLDER}/mgl/{name}",
                ),
                data=data,
            )
        )
    for rel in TRANSLATE_FILES:
        path = Path(__file__).with_name("payload") / rel
        data = path.read_bytes()
        validate_script(rel, data)
        direct_files.append(
            DirectFile(
                path=rel,
                url=github_raw_url(
                    args.repository,
                    git_file_revision(path),
                    f"{FOLDER}/payload/{rel}",
                ),
                data=data,
            )
        )

    database = build_direct_database(
        folder=FOLDER,
        repository=args.repository,
        timestamp=generation_timestamp(args.timestamp),
        filter_terms=(FOLDER,),
        direct_files=tuple(direct_files),
    )
    write_bundle(database, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
