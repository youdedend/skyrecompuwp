#!/usr/bin/env python3
"""Turn clang-cl diagnostics into GitHub annotations.

Why this exists: CI job logs are not readable from the environment this port is developed in.
Annotations are, so they are the only channel that carries a compiler error back. Everything
this script prints therefore goes through the ::error/::warning workflow-command syntax.

Usage:
    python annotate-diagnostics.py diag.txt [--root upstream] [--status 1]

Exit code is always 0: this reports, it does not gate. The workflow decides, from --status.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# clang-cl / MSVC styles, both seen in practice:
#   C:\path\file.c(12,5): error: message
#   C:\path\file.c(12,5): warning: message [-Wflag]
DIAG = re.compile(r"^(?P<path>.+?)\((?P<line>\d+),(?P<col>\d+)\):\s*(?P<sev>error|warning|note):\s*(?P<msg>.*)$")
#   file.c:12:5: error: message    (clang when driven as clang rather than clang-cl)
DIAG_COLON = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+):\s*(?P<sev>error|warning|note):\s*(?P<msg>.*)$")
#   file.cpp(159): error C2064: message      (MSVC, no column)
DIAG_NOC0L = re.compile(r"^(?P<path>.+?)\((?P<line>\d+)\):\s*(?P<sev>error|warning|note)\s+[A-Z]+\d+:\s*(?P<msg>.*)$")

MAX_ANNOTATIONS = 50   # GitHub caps annotations per step; beyond this, summarise.


def escape(text: str) -> str:
    """Workflow-command escaping: % first, then newlines."""
    return text.replace("%", "%25").replace("\r", "").replace("\n", " ")


def normalise(path: str, root: str) -> str:
    p = os.path.abspath(path).replace("\\", "/")
    r = os.path.abspath(root).replace("\\", "/")
    if p.startswith(r + "/"):
        return p[len(r) + 1:]
    # Keep something useful even when the path is outside the checkout (SDK headers, for example).
    return p if len(p) < 160 else "..." + p[-157:]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("diag")
    ap.add_argument("--root", default="upstream")
    ap.add_argument("--status", type=int, default=None, help="compiler exit status; gates nothing here")
    args = ap.parse_args()

    if not os.path.exists(args.diag):
        print(f"::error::no compiler output captured ({args.diag} missing)")
        return 0

    text = open(args.diag, encoding="utf-8", errors="replace").read()
    stripped = text.strip()

    if not stripped:
        # A clean compile writes nothing. Only treat silence as a failure when the compiler also
        # reported a non-zero status - which is the "clang-cl never ran" case.
        if args.status in (None, 0):
            print("::notice::uwp.c compiled clean in the UWP configuration (0 diagnostics)")
        else:
            print(f"::error::the compiler exited {args.status} without printing anything - "
                  f"it probably never ran (missing clang-cl, or the environment step failed)")
        return 0

    found = []
    for line in text.splitlines():
        for rx in (DIAG, DIAG_COLON, DIAG_NOC0L):
            m = rx.match(line.strip())
            if m:
                found.append(m)
                break

    errors = [m for m in found if m.group("sev") == "error"]
    warnings = [m for m in found if m.group("sev") == "warning"]

    for m in found[:MAX_ANNOTATIONS]:
        sev = "error" if m.group("sev") == "error" else "warning"
        if m.group("sev") == "note":
            continue                      # notes attach to their diagnostic; noise in annotations
        col = m.groupdict().get("col")
        where = f"line={m.group('line')}" + (f",col={col}" if col else "")
        print(f"::{sev} file={normalise(m.group('path'), args.root)},{where}::{escape(m.group('msg'))}")

    if len(found) > MAX_ANNOTATIONS:
        print(f"::error::{len(found) - MAX_ANNOTATIONS} further diagnostics not shown (cap {MAX_ANNOTATIONS})")

    if not found:
        # Unparseable failure: surface the raw output, which is usually the real story
        # ("clang-cl: command not found", a broken environment, a missing header form).
        print(f"::error::compiler failed with unrecognised output; first 900 characters follow%0A"
              f"{escape(stripped[:900])}")
    else:
        label = {"diag.txt": "uwp.c", "entry-diag.txt": "uwp_entry.cpp"}.get(os.path.basename(args.diag),
                                                                              os.path.basename(args.diag))
        print(f"::error::{label}: {len(errors)} error(s), {len(warnings)} warning(s)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
