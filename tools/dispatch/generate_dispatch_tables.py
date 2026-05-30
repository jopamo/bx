#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any


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


def read_manifest(path: Path) -> dict[str, object]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise SystemExit(f"dispatch manifest is not an object: {path}")
    if data.get("manifest_version") != 1:
        raise SystemExit("dispatch manifest manifest_version must be 1")
    if data.get("capability_flags") != list(CAPABILITY_TOKENS):
        raise SystemExit("dispatch manifest capability_flags must match generator capability order")
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
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise SystemExit(f"{field}[{index}] has invalid applet name")
        if not isinstance(main, str) or not SYMBOL_RE.match(main):
            raise SystemExit(f"{field}[{index}] has invalid main symbol")
        if not isinstance(capabilities, list):
            raise SystemExit(f"{field}[{index}] must include capabilities list")
        capability_names: list[str] = []
        for cap_index, capability in enumerate(capabilities):
            if not isinstance(capability, str) or capability not in CAPABILITY_TOKENS:
                raise SystemExit(f"{field}[{index}].capabilities[{cap_index}] is not an allowed capability")
            if capability in capability_names:
                raise SystemExit(f"{field}[{index}].capabilities duplicates {capability!r}")
            capability_names.append(capability)
        entries.append({"name": name, "main": main, "capabilities": capability_names})
    return entries


def validate_entries(boot_critical: list[dict[str, object]], applets: list[dict[str, object]]) -> None:
    if [entry["name"] for entry in boot_critical[:2]] != ["init", "switch_root"]:
        raise SystemExit("boot-critical dispatch order must begin with init, switch_root")

    seen: dict[str, str] = {}
    for group, entries in (("boot_critical", boot_critical), ("applets", applets)):
        for entry in entries:
            name = str(entry["name"])
            previous = seen.get(name)
            if previous is not None:
                raise SystemExit(
                    f"duplicate dispatch applet name {name!r} in {group}; first seen in {previous}"
                )
            seen[name] = group


def source_root_from_manifest(manifest_path: Path) -> Path:
    resolved = manifest_path.resolve()
    if (
        resolved.name == "applets.json"
        and resolved.parent.name == "dispatch"
        and resolved.parent.parent.name == "src"
    ):
        return resolved.parent.parent.parent
    raise SystemExit(f"cannot derive source root from dispatch manifest path: {manifest_path}")


def load_audit_generator() -> Any:
    audit_tools = Path(__file__).resolve().parents[1] / "audit"
    sys.path.insert(0, str(audit_tools))
    try:
        import generate_applets_json  # type: ignore[import-not-found]
    finally:
        try:
            sys.path.remove(str(audit_tools))
        except ValueError:
            pass
    return generate_applets_json


def classification_records(source_root: Path, names: list[str]) -> dict[str, dict[str, object]]:
    audit_generator = load_audit_generator()
    _path, policy = audit_generator.load_classification_policy(source_root)
    records = audit_generator.validate_classification_policy(policy, inventory_names=set(names))
    return records


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
    classification: dict[str, object],
    audit_generator: Any,
) -> dict[str, object]:
    risk_labels = audit_generator.risk_labels_record(classification)
    audit_requirement = audit_generator.audit_requirement_record(classification)
    return {
        "name": str(entry["name"]),
        "boot_critical": boot_critical,
        "capabilities": [str(capability) for capability in entry["capabilities"]],
        "aliases": aliases,
        "security_risk_labels": [
            str(label)
            for label in risk_labels.get("security_labels", [])
            if isinstance(label, str)
        ],
        "performance_risk_labels": [
            str(label)
            for label in risk_labels.get("performance_labels", [])
            if isinstance(label, str)
        ],
        "audit_levels": [
            str(level)
            for level in audit_requirement.get("levels", [])
            if isinstance(level, str)
        ],
    }


def write_dispatch_c(
    *,
    output: Path,
    manifest_path: Path,
    boot_critical: list[dict[str, object]],
    applets: list[dict[str, object]],
) -> None:
    source_root = source_root_from_manifest(manifest_path)
    raw_entries = boot_critical + applets
    classifications = classification_records(source_root, dispatch_names(boot_critical, applets))
    aliases = aliases_by_name(raw_entries)
    audit_generator = load_audit_generator()
    entries = [
        (str(entry["name"]), str(entry["main"]), entry["capabilities"], True, entry)
        for entry in boot_critical
    ] + [
        (str(entry["name"]), str(entry["main"]), entry["capabilities"], False, entry)
        for entry in applets
    ]
    metadata_records = [
        metadata_record(
            entry=entry,
            boot_critical=bool(boot),
            aliases=aliases[str(entry["name"])],
            classification=classifications[str(entry["name"])],
            audit_generator=audit_generator,
        )
        for _name, _main, _capabilities, boot, entry in entries
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
    for name, main, capabilities, boot, _entry in entries:
        lines.append(
            "    {"
            f".name = {c_string(name)}, "
            f".main = {main}, "
            f".capabilities = {c_capabilities(capabilities)}, "
            f".boot_critical = {'true' if boot else 'false'}"
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
        string_list_var(
            "bx_applet_metadata",
            index,
            "security_risk_labels",
            [str(item) for item in record["security_risk_labels"]],
            lines,
        )
        string_list_var(
            "bx_applet_metadata",
            index,
            "performance_risk_labels",
            [str(item) for item in record["performance_risk_labels"]],
            lines,
        )
        string_list_var(
            "bx_applet_metadata",
            index,
            "audit_levels",
            [str(item) for item in record["audit_levels"]],
            lines,
        )

    lines.append("static const struct bx_applet_metadata bx_applet_metadata[] = {")
    for index, record in enumerate(metadata_records):
        capabilities_var = c_identifier("bx_applet_metadata", index, "capabilities")
        aliases_var = c_identifier("bx_applet_metadata", index, "aliases")
        security_var = c_identifier("bx_applet_metadata", index, "security_risk_labels")
        performance_var = c_identifier("bx_applet_metadata", index, "performance_risk_labels")
        audit_var = c_identifier("bx_applet_metadata", index, "audit_levels")
        capabilities = record["capabilities"]
        aliases_for_record = record["aliases"]
        security_labels = record["security_risk_labels"]
        performance_labels = record["performance_risk_labels"]
        audit_levels = record["audit_levels"]
        lines.append("    {")
        lines.append(f"        .name = {c_string(str(record['name']))},")
        lines.append(f"        .boot_critical = {'true' if record['boot_critical'] else 'false'},")
        lines.append(f"        .capabilities = {capabilities_var if capabilities else 'NULL'},")
        lines.append(f"        .capability_count = {len(capabilities)}u,")
        lines.append(f"        .aliases = {aliases_var if aliases_for_record else 'NULL'},")
        lines.append(f"        .alias_count = {len(aliases_for_record)}u,")
        lines.append(f"        .security_risk_labels = {security_var if security_labels else 'NULL'},")
        lines.append(f"        .security_risk_label_count = {len(security_labels)}u,")
        lines.append(f"        .performance_risk_labels = {performance_var if performance_labels else 'NULL'},")
        lines.append(f"        .performance_risk_label_count = {len(performance_labels)}u,")
        lines.append(f"        .audit_levels = {audit_var if audit_levels else 'NULL'},")
        lines.append(f"        .audit_level_count = {len(audit_levels)}u,")
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
