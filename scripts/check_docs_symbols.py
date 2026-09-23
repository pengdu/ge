#!/usr/bin/env python3
"""Single-source-of-truth check between docs/13 class catalogues and the code.

Every class named in the first column of the 7.1/7.2/7.2a tables must exist
as a `class`/`struct` declaration under include/ or src/. Docs-first
navigation (humans and search tools) then never hits a symbol that was
renamed or folded away -- the failure mode of W-2.

Run from the repository root (ctest wires this up as ge_docs_symbols).
"""

import re
import sys
from pathlib import Path

DOCS = Path("docs/13-接口与类设计.md")
SOURCE_ROOTS = [Path("include"), Path("src")]
DECL = re.compile(r"^(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)", re.MULTILINE)


def catalogued_classes() -> list[str]:
    text = DOCS.read_text(encoding="utf-8")
    section = re.search(r"### 7\.1 .*### 7\.3", text, re.DOTALL)
    if section is None:
        raise SystemExit(f"{DOCS}: 7.1..7.2a catalogue section not found")
    names: list[str] = []
    for line in section.group(0).splitlines():
        if not line.startswith("| `"):
            continue
        first_cell = line.split("|")[1]
        names += re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", first_cell)
    if not names:
        raise SystemExit(f"{DOCS}: catalogue tables are empty -- check the section markers")
    return names


def declared_classes() -> set[str]:
    declared: set[str] = set()
    for root in SOURCE_ROOTS:
        for path in root.rglob("*.h"):
            declared.update(DECL.findall(path.read_text(encoding="utf-8")))
    return declared


def main() -> int:
    declared = declared_classes()
    missing = [name for name in catalogued_classes() if name not in declared]
    if missing:
        print("docs/13 class catalogue references symbols absent from the code:")
        for name in missing:
            print(f"  - {name}")
        print("Update the 7.1/7.2/7.2a tables (or the code) and rerun.")
        return 1
    print("docs/13 class catalogue matches the code.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
