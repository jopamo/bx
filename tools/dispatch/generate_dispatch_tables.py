#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


NAME_RE = re.compile(r"^[A-Za-z0-9_.+\\[-][A-Za-z0-9_.+\\[-]*$")
SYMBOL_RE = re.compile(r"^bx_[A-Za-z0-9_]+_main$")
CAPABILITY_TOKENS = {
    "filesystem-read": "BX_APPLET_CAP_FILESYSTEM_READ",
    "filesystem-write": "BX_APPLET_CAP_FILESYSTEM_WRITE",
    "recursive-traversal": "BX_APPLET_CAP_RECURSIVE_TRAVERSAL",
    "child-execution": "BX_APPLET_CAP_CHILD_EXECUTION",
    "network-access": "BX_APPLET_CAP_NETWORK_ACCESS",
    "terminal-control": "BX_APPLET_CAP_TERMINAL_CONTROL",
    "privilege-sensitive": "BX_APPLET_CAP_PRIVILEGE_SENSITIVITY",
    "raw-output": "BX_APPLET_CAP_RAW_OUTPUT",
}
EXECUTION_CLASS_TOKENS = {
    "exec-only": "BX_APPLET_EXEC_ONLY",
    "child-in-process-safe": "BX_APPLET_CHILD_IN_PROCESS_SAFE",
    "parent-shell-safe": "BX_APPLET_PARENT_SHELL_SAFE",
}


def read_manifest(path: Path) -> dict[str, object]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise SystemExit(f"dispatch manifest is not an object: {path}")
    if data.get("manifest_version") != 2:
        raise SystemExit("dispatch manifest manifest_version must be 2")
    if data.get("capability_flags") != list(CAPABILITY_TOKENS):
        raise SystemExit("dispatch manifest capability_flags must match generator capability order")
    if data.get("execution_classes") != list(EXECUTION_CLASS_TOKENS):
        raise SystemExit("dispatch manifest execution_classes must match generator policy order")
    return data


def load_entries(manifest: dict[str, object], field: str) -> list[dict[str, object]]:
    raw = manifest.get(field)
    if not isinstance(raw, list):
        raise SystemExit(f"dispatch manifest missing list: {field}")
    entries: list[dict[str, object]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise SystemExit(f"{field}[{index}] must be an object")
        name = item.get("name")
        main = item.get("main")
        capabilities = item.get("capabilities")
        execution_class = item.get("execution_class")
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise SystemExit(f"{field}[{index}] has invalid applet name")
        if not isinstance(main, str) or not SYMBOL_RE.match(main):
            raise SystemExit(f"{field}[{index}] has invalid main symbol")
        if not isinstance(capabilities, list):
            raise SystemExit(f"{field}[{index}] must include capabilities list")
        if not isinstance(execution_class, str):
            raise SystemExit(f"{field}[{index}] must include execution_class")
        if execution_class not in EXECUTION_CLASS_TOKENS:
            raise SystemExit(
                f"{field}[{index}].execution_class is not an allowed execution class"
            )
        capability_names: list[str] = []
        for cap_index, capability in enumerate(capabilities):
            if not isinstance(capability, str) or capability not in CAPABILITY_TOKENS:
                raise SystemExit(f"{field}[{index}].capabilities[{cap_index}] is not an allowed capability")
            if capability in capability_names:
                raise SystemExit(f"{field}[{index}].capabilities duplicates {capability!r}")
            capability_names.append(capability)
        entries.append({
            "name": name,
            "main": main,
            "capabilities": capability_names,
            "execution_class": execution_class,
        })
    return entries


def validate_entries(boot_critical: list[dict[str, object]], applets: list[dict[str, object]]) -> None:
    if not boot_critical or boot_critical[0]["name"] != "switch_root":
        raise SystemExit("boot-critical dispatch order must begin with switch_root")

    seen: dict[str, str] = {}
    execution_classes_by_main: dict[str, str] = {}
    for group, entries in (("boot_critical", boot_critical), ("applets", applets)):
        for entry in entries:
            name = str(entry["name"])
            main = str(entry["main"])
            previous = seen.get(name)
            if previous is not None:
                raise SystemExit(
                    f"duplicate dispatch applet name {name!r} in {group}; first seen in {previous}"
                )
            seen[name] = group
            execution_class = str(entry["execution_class"])
            previous_class = execution_classes_by_main.get(main)
            if previous_class is not None and previous_class != execution_class:
                raise SystemExit(
                    f"aliases sharing {main} must share one execution class"
                )
            execution_classes_by_main[main] = execution_class


def dispatch_names(boot_critical: list[dict[str, object]], applets: list[dict[str, object]]) -> list[str]:
    return [str(entry["name"]) for entry in boot_critical] + [str(entry["name"]) for entry in applets]


def c_string(value: str) -> str:
    return json.dumps(value)


def c_capabilities(capabilities: object) -> str:
    if not isinstance(capabilities, list) or not capabilities:
        return "BX_APPLET_CAP_NONE"
    return " | ".join(CAPABILITY_TOKENS[str(capability)] for capability in capabilities)


def c_identifier(prefix: str, index: int, suffix: str) -> str:
    return f"{prefix}_{index}_{suffix}"


def string_list_var(prefix: str, index: int, suffix: str, values: list[str], lines: list[str]) -> str:
    if not values:
        return "NULL"

    var_name = c_identifier(prefix, index, suffix)
    lines.append(f"static const char* const {var_name}[] = {{")
    for value in values:
        lines.append(f"    {c_string(value)},")
    lines.append("};")
    lines.append("")
    return var_name


def aliases_by_name(entries: list[dict[str, object]]) -> dict[str, list[str]]:
    names_by_main: dict[str, list[str]] = {}
    for entry in entries:
        names_by_main.setdefault(str(entry["main"]), []).append(str(entry["name"]))
    return {
        str(entry["name"]): sorted(name for name in names_by_main[str(entry["main"])] if name != entry["name"])
        for entry in entries
    }


def metadata_record(
    *,
    entry: dict[str, object],
    boot_critical: bool,
    aliases: list[str],
) -> dict[str, object]:
    return {
        "name": str(entry["name"]),
        "boot_critical": boot_critical,
        "execution_class": str(entry["execution_class"]),
        "capabilities": [str(capability) for capability in entry["capabilities"]],
        "aliases": aliases,
    }


def write_dispatch_c(
    *,
    output: Path,
    manifest_path: Path,
    boot_critical: list[dict[str, object]],
    applets: list[dict[str, object]],
) -> None:
    raw_entries = boot_critical + applets
    aliases = aliases_by_name(raw_entries)
    entries = [(entry, True) for entry in boot_critical]
    entries.extend((entry, False) for entry in applets)
    metadata_records = [
        metadata_record(
            entry=entry,
            boot_critical=bool(boot),
            aliases=aliases[str(entry["name"])],
        )
        for entry, boot in entries
    ]

    lines: list[str] = []
    lines.append("/* Generated by tools/dispatch/generate_dispatch_tables.py. */")
    lines.append(f"/* Source: {manifest_path.as_posix()} */")
    lines.append("#include <stddef.h>")
    lines.append("#include <string.h>")
    lines.append("")
    lines.append('#include "bx/applet_metadata.h"')
    lines.append('#include "applets.h"')
    lines.append('#include "dispatch/dispatch.h"')
    lines.append("")
    lines.append("static const struct bx_applet bx_applets[] = {")
    for entry, boot in entries:
        lines.append(
            "    {"
            f".name = {c_string(str(entry['name']))}, "
            f".main = {entry['main']}, "
            f".capabilities = {c_capabilities(entry['capabilities'])}, "
            f".boot_critical = {'true' if boot else 'false'}, "
            f".execution_class = "
            f"{EXECUTION_CLASS_TOKENS[str(entry['execution_class'])]}"
            "},"
        )
    lines.append("};")
    lines.append("")
    for index, record in enumerate(metadata_records):
        string_list_var(
            "bx_applet_metadata",
            index,
            "capabilities",
            [str(item) for item in record["capabilities"]],
            lines,
        )
        string_list_var(
            "bx_applet_metadata",
            index,
            "aliases",
            [str(item) for item in record["aliases"]],
            lines,
        )
    lines.append("static const struct bx_applet_metadata bx_applet_metadata[] = {")
    for index, record in enumerate(metadata_records):
        capabilities_var = c_identifier("bx_applet_metadata", index, "capabilities")
        aliases_var = c_identifier("bx_applet_metadata", index, "aliases")
        capabilities = record["capabilities"]
        aliases_for_record = record["aliases"]
        lines.append("    {")
        lines.append(f"        .name = {c_string(str(record['name']))},")
        lines.append(f"        .boot_critical = {'true' if record['boot_critical'] else 'false'},")
        lines.append(
            f"        .execution_class = "
            f"{c_string(str(record['execution_class']))},"
        )
        lines.append(f"        .capabilities = {capabilities_var if capabilities else 'NULL'},")
        lines.append(f"        .capability_count = {len(capabilities)}u,")
        lines.append(f"        .aliases = {aliases_var if aliases_for_record else 'NULL'},")
        lines.append(f"        .alias_count = {len(aliases_for_record)}u,")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("size_t bx_applet_count(void) {")
    lines.append("    return sizeof(bx_applets) / sizeof(bx_applets[0]);")
    lines.append("}")
    lines.append("")
    lines.append("const struct bx_applet* bx_applet_at(size_t index) {")
    lines.append("    if (index >= bx_applet_count()) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    return &bx_applets[index];")
    lines.append("}")
    lines.append("")
    lines.append("const struct bx_applet* bx_applet_find(const char* name) {")
    lines.append("    if (name == NULL) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    for (size_t i = 0; i < bx_applet_count(); i++) {")
    lines.append("        if (strcmp(bx_applets[i].name, name) == 0) {")
    lines.append("            return &bx_applets[i];")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")
    lines.append("size_t bx_applet_metadata_count(void) {")
    lines.append("    return sizeof(bx_applet_metadata) / sizeof(bx_applet_metadata[0]);")
    lines.append("}")
    lines.append("")
    lines.append("const struct bx_applet_metadata* bx_applet_metadata_at(size_t index) {")
    lines.append("    if (index >= bx_applet_metadata_count()) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    return &bx_applet_metadata[index];")
    lines.append("}")
    lines.append("")
    lines.append("const struct bx_applet_metadata* bx_applet_metadata_find(const char* name) {")
    lines.append("    if (name == NULL) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    for (size_t i = 0; i < bx_applet_metadata_count(); i++) {")
    lines.append("        if (strcmp(bx_applet_metadata[i].name, name) == 0) {")
    lines.append("            return &bx_applet_metadata[i];")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")
    output.write_text("\n".join(lines), encoding="utf-8")


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "--meson-list":
        manifest_path = Path(argv[2])
        manifest = read_manifest(manifest_path)
        boot_critical = load_entries(manifest, "boot_critical")
        applets = load_entries(manifest, "applets")
        validate_entries(boot_critical, applets)
        sys.stdout.write("\n".join(dispatch_names(boot_critical, applets)) + "\n")
        return 0

    if len(argv) != 3:
        raise SystemExit(f"usage: {Path(argv[0]).name} APPLETS_JSON OUTPUT_C")

    manifest_path = Path(argv[1])
    output = Path(argv[2])
    manifest = read_manifest(manifest_path)
    boot_critical = load_entries(manifest, "boot_critical")
    applets = load_entries(manifest, "applets")
    validate_entries(boot_critical, applets)
    write_dispatch_c(
        output=output,
        manifest_path=manifest_path,
        boot_critical=boot_critical,
        applets=applets,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
