#!/usr/bin/env python3

import datetime
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


VERSION_RE = re.compile(r"([0-9a-fA-F]{7,64})-(\d{8})")
GIT_METADATA_RE = re.compile(r"([0-9a-fA-F]{7,64}) (\d{4}-\d{2}-\d{2})")


def normalize_version(commit: str, date: str) -> str:
    datetime.date.fromisoformat(date)
    return f"{commit[:12].lower()}-{date.replace('-', '')}"


def explicit_version(value: str) -> Optional[str]:
    if not value:
        return None
    match = VERSION_RE.fullmatch(value)
    if match is None:
        raise ValueError("build_version must have the form <commit-hash>-<YYYYMMDD>")
    date = match.group(2)
    datetime.date(int(date[:4]), int(date[4:6]), int(date[6:]))
    return f"{match.group(1).lower()}-{date}"


def archive_version(source_root: Path) -> Optional[str]:
    metadata_path = source_root / "src" / "bx" / "version.git"
    try:
        metadata = metadata_path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return None
    match = GIT_METADATA_RE.fullmatch(metadata)
    if match is None:
        return None
    return normalize_version(match.group(1), match.group(2))


def checkout_version(source_root: Path) -> Optional[str]:
    git = shutil.which("git")
    if git is None:
        return None

    top_level = subprocess.run(
        [git, "-C", str(source_root), "rev-parse", "--show-toplevel"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if top_level.returncode != 0:
        return None
    try:
        if Path(top_level.stdout.strip()).resolve() != source_root.resolve():
            return None
    except OSError:
        return None

    metadata = subprocess.run(
        [git, "-C", str(source_root), "show", "-s", "--format=%H %cs", "HEAD"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if metadata.returncode != 0:
        return None
    match = GIT_METADATA_RE.fullmatch(metadata.stdout.strip())
    if match is None:
        return None
    return normalize_version(match.group(1), match.group(2))


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {Path(sys.argv[0]).name} SOURCE_ROOT BUILD_VERSION", file=sys.stderr)
        return 2

    source_root = Path(sys.argv[1])
    try:
        version = explicit_version(sys.argv[2])
    except (ValueError, OverflowError) as error:
        print(f"bx version: {error}", file=sys.stderr)
        return 1

    if version is None:
        version = archive_version(source_root)
    if version is None:
        version = checkout_version(source_root)
    if version is None:
        print(
            "bx version: no Git metadata; use an export-subst archive or set -Dbuild_version=<commit-hash>-<YYYYMMDD>",
            file=sys.stderr,
        )
        return 1

    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
