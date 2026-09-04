#!/usr/bin/env python3

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message):
    raise AssertionError(message)


def require(condition, message):
    if not condition:
        fail(message)


def extract_function(source, name):
    match = re.search(
        r"(?:^|\n)(?:static\s+)?(?:void|int|bool)\s+" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        fail(f"function {name} not found")

    start = match.start()
    brace = source.find("{", match.start(), match.end())
    depth = 0
    i = brace
    state = "code"
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "/" and n == "/":
                state = "line_comment"
                i += 1
            elif c == "/" and n == "*":
                state = "block_comment"
                i += 1
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return source[start : i + 1]
        elif state == "string":
            if c == "\\":
                i += 1
            elif c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 1
            elif c == "'":
                state = "code"
        elif state == "line_comment":
            if c == "\n":
                state = "code"
        elif state == "block_comment":
            if c == "*" and n == "/":
                state = "code"
                i += 1
        i += 1
    fail(f"unterminated function {name}")


def parse_define(source, name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+([^\n/]+)", source, re.MULTILINE)
    if match is None:
        fail(f"define {name} not found")
    value = match.group(1).strip()
    value = value.replace("(", "").replace(")", "")
    if "*" in value:
        parts = [int(part.strip()) for part in value.split("*")]
        result = 1
        for part in parts:
            result *= part
        return result
    return int(value, 0)


def static_checks(root):
    screen = root / "src" / "applets" / "system" / "screen"
    vendor = root / "src" / "vendor" / "gnu" / "screen"
    ansi = (vendor / "ansi.c").read_text()
    display = (screen / "display.c").read_text()
    policy = (screen / "screen_osc52_policy.c").read_text()
    window_c = (screen / "window.c").read_text()
    window_h = (screen / "window.h").read_text()
    screen_h = (screen / "screen.h").read_text()
    comm_c = (screen / "comm.c").read_text()
    comm_h = (screen / "comm.h").read_text()

    reset = extract_function(ansi, "ResetAnsiState")
    require("CStrRelease(&win->w_cstr);" in reset, "ResetAnsiState must release an incomplete control string")

    relay = extract_function(display, "DisplayOSC52")
    require("D_CXT" not in relay, "OSC 52 relay must not depend on the unrelated xterm D_CXT heuristic")
    require('AddStr("\\033]52;");' in relay, "OSC 52 relay prefix missing")
    require('AddStr("\\033\\\\");' in relay, "OSC 52 relay ST terminator missing")
    require("Flush(0);" in relay, "OSC 52 relay must backpressure instead of discarding a partial sequence")

    osc52 = extract_function(policy, "ScreenOsc52Relay")
    require(
        "ScreenOsc52Relay(win, p);" in ansi,
        "ANSI parser must delegate OSC 52 policy",
    )
    require(
        "defosc52" not in ansi and "DisplayOSC52(" not in ansi,
        "ANSI parser must not own OSC 52 authorization or relay policy",
    )
    require("D_CXT" not in osc52, "OSC 52 display selection must not depend on D_CXT")
    read_guard = osc52.find("data_length == 1 && data[0] == '?'")
    policy_guard = osc52.find("if (!defosc52)")
    relay_call = osc52.find("DisplayOSC52(")
    require(0 <= read_guard < policy_guard < relay_call, "OSC 52 reads must be rejected before write policy and relay")

    require("inline_buf[CTRLSTR_INLINE_SIZE + 1]" in window_h, "inline control-string buffer must reserve the NUL byte")
    append = extract_function(ansi, "CStrAppend")
    require("newcap + 1" in append, "heap control-string allocation must reserve the NUL byte")
    string_end = extract_function(ansi, "StringEnd")
    overflow_pos = string_end.find("if (cs->overflow)")
    nul_pos = string_end.find("cs->data[cs->len] = '\\0';")
    require(0 <= overflow_pos < nul_pos, "overflow must be checked before writing the terminator")
    string_start = extract_function(ansi, "StringStart")
    require("DCS_MAX_WIRE_SIZE" in string_start, "DCS passthrough must use the expanded bounded limit")
    free_window = extract_function(window_c, "FreeWindow")
    reset_window = extract_function(window_c, "ResetWindow")
    require("CStrRelease(&window->w_cstr);" in free_window, "window destruction must release control strings")
    require("CStrRelease(&win->w_cstr);" in reset_window, "window reset must release control strings")

    command_count = len(re.findall(r'^\s*\{\s*"[^"]+"\s*,', comm_c, re.MULTILINE))
    last = parse_define(comm_h, "RC_LAST")
    zombie_timeout = parse_define(comm_h, "RC_ZOMBIE_TIMEOUT")
    require(command_count > 0, "Screen command table is empty")
    require(last == command_count - 1, f"RC_LAST={last}, but command table has {command_count} entries")
    require(zombie_timeout == last, "RC_ZOMBIE_TIMEOUT must be the final command index")
    require("RC_OSC52READ" not in comm_h and '"osc52read"' not in comm_c, "clipboard read configuration must stay removed")

    return (
        parse_define(screen_h, "CTRLSTR_INLINE_SIZE"),
        parse_define(screen_h, "OSC52_MAX_WIRE_SIZE"),
        extract_function(ansi, "CStrInit"),
        extract_function(ansi, "CStrRelease"),
        append,
    )


def compile_boundary_harness(inline_size, osc52_limit, init_fn, release_fn, append_fn):
    cc = os.environ.get("CC", "cc")
    compiler = shutil.which(cc)
    if compiler is None:
        fail(f"C compiler not found: {cc}")

    harness = f'''#include <assert.h>\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdlib.h>\n#include <string.h>\n\n#define CTRLSTR_INLINE_SIZE {inline_size}\n#define OSC52_MAX_WIRE_SIZE {osc52_limit}\n\nstruct control_string {{\n    char inline_buf[CTRLSTR_INLINE_SIZE + 1];\n    char *data;\n    size_t len;\n    size_t cap;\n    size_t limit;\n    bool heap;\n    bool overflow;\n    unsigned int osc_cmd;\n    bool osc_cmd_valid;\n    bool osc_cmd_complete;\n}};\n\nvoid CStrRelease(struct control_string *);\n\n{init_fn}\n\n{release_fn}\n\n{append_fn}\n\nstatic void append_n(struct control_string *s, size_t n)\n{{\n    for (size_t i = 0; i < n; i++)\n        CStrAppend(s, 'x');\n}}\n\nint main(void)\n{{\n    struct control_string s = {{0}};\n\n    CStrInit(&s);\n    append_n(&s, CTRLSTR_INLINE_SIZE);\n    assert(!s.overflow);\n    assert(!s.heap);\n    assert(s.len == CTRLSTR_INLINE_SIZE);\n    s.data[s.len] = '\\0';\n    CStrAppend(&s, 'x');\n    assert(s.overflow);\n    assert(s.len == CTRLSTR_INLINE_SIZE);\n    CStrRelease(&s);\n\n    memset(&s, 0, sizeof(s));\n    CStrInit(&s);\n    s.limit = OSC52_MAX_WIRE_SIZE;\n    append_n(&s, OSC52_MAX_WIRE_SIZE);\n    assert(!s.overflow);\n    assert(s.heap);\n    assert(s.len == OSC52_MAX_WIRE_SIZE);\n    assert(s.cap == OSC52_MAX_WIRE_SIZE);\n    s.data[s.len] = '\\0';\n    CStrAppend(&s, 'x');\n    assert(s.overflow);\n    assert(s.len == OSC52_MAX_WIRE_SIZE);\n    CStrRelease(&s);\n    assert(!s.heap);\n    assert(s.data == s.inline_buf);\n\n    return 0;\n}}\n'''

    with tempfile.TemporaryDirectory(prefix="bx-screen-osc52-") as tmp:
        source = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        source.write_text(harness)
        subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0", str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    inline_size, osc52_limit, init_fn, release_fn, append_fn = static_checks(root)
    compile_boundary_harness(inline_size, osc52_limit, init_fn, release_fn, append_fn)
    print("screen OSC 52 regressions: ok")


if __name__ == "__main__":
    main()
