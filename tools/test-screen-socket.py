#!/usr/bin/env python3
"""Check screen's ancillary-header traversal against a boundary matrix."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    source = (root / "src/applets/system/screen/socket.c").read_text()
    start = source.index("static struct cmsghdr *NextControlMessage(")
    end = source.index("\n}\n", start) + 3
    helper = source[start:end]
    harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

HELPER

int main(void)
{
    union {
        struct cmsghdr align;
        unsigned char data[256];
    } buffer = {0};
    const size_t offsets[] = {0, CMSG_SPACE(sizeof(int))};
    const size_t payloads[] = {0, 1, sizeof(int), 2 * sizeof(int)};
    struct msghdr msg = {0};
    size_t cases = 0;

    msg.msg_control = buffer.data;
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        size_t offset = offsets[i];
        struct cmsghdr *current = (struct cmsghdr *)(buffer.data + offset);
        for (size_t j = 0; j < sizeof(payloads) / sizeof(payloads[0]); ++j) {
            size_t length = CMSG_LEN(payloads[j]);
            size_t space = CMSG_SPACE(payloads[j]);
            const size_t remaining[] = {
                length - 1, length, space,
                space + sizeof(*current) - 1,
                space + sizeof(*current),
                space + CMSG_SPACE(sizeof(int))
            };
            current->cmsg_len = length;
            for (size_t k = 0; k < sizeof(remaining) / sizeof(remaining[0]); ++k) {
                msg.msg_controllen = offset + remaining[k];
                struct cmsghdr *expected = NULL;
                if (remaining[k] >= space + sizeof(*current))
                    expected = (struct cmsghdr *)(buffer.data + offset + space);
                assert(NextControlMessage(&msg, current) == expected);
                ++cases;
            }
        }
        const size_t invalid[] = {0, CMSG_LEN(0) - 1, sizeof(buffer.data), SIZE_MAX};
        msg.msg_controllen = sizeof(buffer.data) - 1;
        for (size_t j = 0; j < sizeof(invalid) / sizeof(invalid[0]); ++j) {
            current->cmsg_len = invalid[j];
            assert(NextControlMessage(&msg, current) == NULL);
            ++cases;
        }
    }
    assert(cases == 56);
    return 0;
}
""".replace("HELPER", helper)
    with tempfile.TemporaryDirectory(prefix="bx-screen-socket-") as tmp:
        source_path = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        source_path.write_text(harness)
        command = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(
            command + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O3",
                       str(source_path), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("screen ancillary-header matrix: 56 cases passed")


if __name__ == "__main__":
    main()
