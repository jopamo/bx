#!/usr/bin/env python3
"""Exercise local openrsync transfers without relying on an installed rsync.

Usage: python3 tools/rsync/smoke.py --bx /path/to/bx
       python3 tools/rsync/smoke.py --standalone /path/to/openrsync
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
from pathlib import Path


def run(binary: Path, standalone: bool, source: Path, destination: Path,
        environment: dict[str, str], *options: str) -> None:
    args = [str(binary)]
    if not standalone:
        args.append("rsync")
    args.extend(["-av", *options])
    if standalone:
        args.append(f"--rsync-path={binary}")
    args.extend([str(source) + os.sep, str(destination) + os.sep])
    subprocess.run(args, env=environment, check=True, timeout=30)


def main() -> None:
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--bx", type=Path)
    group.add_argument("--standalone", type=Path)
    options = parser.parse_args()
    standalone = options.standalone is not None
    binary = (options.standalone or options.bx).resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="bx-rsync-smoke-") as temporary:
        root = Path(temporary)
        source = root / "source"
        destination = root / "destination"
        (source / "nested").mkdir(parents=True)
        destination.mkdir()
        (source / "nested" / "payload").write_bytes(b"a" * 65536)
        (source / "file with spaces").write_bytes(b"rsync test\n")
        (source / "link").symlink_to("nested/payload")

        # Fail if the applet delegates a local transfer to a PATH rsync.
        external = root / "bin"
        external.mkdir()
        trap = external / "rsync"
        trap.write_text("#!/bin/sh\nexit 97\n", encoding="utf-8")
        trap.chmod(0o755)
        environment = os.environ.copy()
        environment["PATH"] = str(external) + os.pathsep + environment.get("PATH", "")

        run(binary, standalone, source, destination, environment)
        assert (destination / "nested" / "payload").read_bytes() == b"a" * 65536
        assert (destination / "file with spaces").read_bytes() == b"rsync test\n"
        assert (destination / "link").is_symlink()
        assert os.readlink(destination / "link") == "nested/payload"

        run(binary, standalone, source, destination, environment)
        (destination / "stale").write_bytes(b"remove me")
        (source / "nested" / "payload").write_bytes(b"b" * 12345)
        run(binary, standalone, source, destination, environment, "--delete")
        assert not (destination / "stale").exists()
        assert (destination / "nested" / "payload").read_bytes() == b"b" * 12345

    print("openrsync smoke test passed")


if __name__ == "__main__":
    main()
