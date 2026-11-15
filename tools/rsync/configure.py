#!/usr/bin/env python3
"""Generate rsync's feature header without changing the source tree.

Apple's configure checks compile and link, but do not run target binaries.
The same probes work with Meson's host and cross compilers.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: configure.py CONFIGURE OUTPUT CC [CC_ARGUMENTS...]", file=sys.stderr)
        return 2

    configure = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    env = os.environ.copy()
    env["CC"] = " ".join(sys.argv[3:])

    # Oconfigure uses feature_probes.c and writes config.h, Makefile.configure, and
    # temporary probe files. Keep them out of the source tree.
    with tempfile.TemporaryDirectory(prefix="bx-openrsync-config-") as workdir:
        shutil.copyfile(
            configure.parent / "feature_probes.c", Path(workdir) / "feature_probes.c"
        )
        result = subprocess.run(
            [str(configure)],
            cwd=workdir,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            print(result.stdout, file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            return result.returncode

        generated = Path(workdir) / "config.h"
        if not generated.is_file():
            print("openrsync configure did not generate config.h", file=sys.stderr)
            return 1
        # Include renames after the HAVE_* checks but before fallback
        # prototypes.  Otherwise bx's other BSD-derived libraries would
        # collide with openrsync's getprogname, strtonum, and friends.
        header = generated.read_text(encoding="utf-8")
        marker = "#define HAVE___PROGNAME "
        lines = header.splitlines(keepends=True)
        positions = [i for i, line in enumerate(lines) if line.startswith(marker)]
        if len(positions) != 1:
            print("openrsync feature header format changed", file=sys.stderr)
            return 1
        lines.insert(positions[0] + 1, '#include "bx_rsync_names.h"\n')
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
