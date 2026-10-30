#!/usr/bin/env python3
"""Fail the build when the runtime calls APIs that a UWP AppContainer does not allow.

Why this exists: the v0.1 smoke package installed and then died at activation, and the console
gave no reason. An AppContainer rejects forbidden Win32 calls at run time, not at compile time,
so a bad call compiles cleanly and fails on the console after a manual deploy round trip.
This turns that into a build-time error.

Usage:
    python check-uwp-allowlist.py <src-dir>                # report
    python check-uwp-allowlist.py <src-dir> --fail         # exit 1 on any violation
    python check-uwp-allowlist.py <src-dir> --json         # machine readable

Exit codes: 0 clean (or report mode), 1 violations found with --fail, 2 usage error.

Code that a UWP build compiles out (#elif defined(_WIN32) and friends) is evaluated with the UWP
macro set and skipped, so the report stays meaningful after the port's patches are applied.

The lists below are the AppContainer-forbidden set this port actually hits, not an exhaustive
UWP API catalogue. Every entry was confirmed against the real source tree; see
PORT-INVENTORY.md for the per-call-site analysis.
"""

from __future__ import annotations

import argparse
import json
import fnmatch
import os
import re
import sys

# API -> (reason, severity)
#   hard    : forbidden in an AppContainer; the call cannot work
#   soft    : works on desktop, silently degraded or unavailable on Xbox; needs a decision
FORBIDDEN = {
    # process creation / image replacement
    "CreateProcessA": ("process creation", "hard"),
    "CreateProcessW": ("process creation", "hard"),
    "CreateProcessAsUserW": ("process creation", "hard"),
    "ShellExecuteA": ("shell / process launch", "hard"),
    "ShellExecuteW": ("shell / process launch", "hard"),
    "ShellExecuteExA": ("shell / process launch", "hard"),
    "ShellExecuteExW": ("shell / process launch", "hard"),
    "WinExec": ("process launch", "hard"),
    "system": ("process launch via CRT", "hard"),
    "popen": ("process launch via CRT", "hard"),
    # dynamic loading from arbitrary paths (LoadPackagedLibrary is the allowed form)
    "LoadLibraryA": ("arbitrary DLL load", "hard"),
    "LoadLibraryW": ("arbitrary DLL load", "hard"),
    "LoadLibraryExA": ("arbitrary DLL load", "hard"),
    "LoadLibraryExW": ("arbitrary DLL load", "hard"),
    "GetProcAddress": ("pairs with arbitrary load", "soft"),
    # UI the sandbox does not own
    "MessageBoxA": ("blocking desktop dialog", "hard"),
    "MessageBoxW": ("blocking desktop dialog", "hard"),
    "CreateWindowExA": ("desktop window creation", "hard"),
    "CreateWindowExW": ("desktop window creation", "hard"),
    "SystemParametersInfoA": ("desktop settings", "soft"),
    "SystemParametersInfoW": ("desktop settings", "soft"),
    # registry
    "RegOpenKeyExA": ("registry", "hard"),
    "RegOpenKeyExW": ("registry", "hard"),
    "RegSetValueExA": ("registry", "hard"),
    "RegSetValueExW": ("registry", "hard"),
    "RegQueryValueExA": ("registry", "hard"),
    "RegQueryValueExW": ("registry", "hard"),
    # shell / known folders: use WinRT storage instead
    "SHGetKnownFolderPath": ("desktop known folders", "hard"),
    "SHGetFolderPathA": ("desktop known folders", "hard"),
    "SHGetFolderPathW": ("desktop known folders", "hard"),
    "SHFileOperationA": ("shell file op", "hard"),
    "SHFileOperationW": ("shell file op", "hard"),
    # console and power
    "AllocConsole": ("console allocation", "hard"),
    "FreeConsole": ("console management", "hard"),
    "AttachConsole": ("console management", "hard"),
    "GetConsoleProcessList": ("console management", "hard"),
    "SetConsoleOutputCP": ("console management", "hard"),
    "timeBeginPeriod": ("winmm timer resolution", "soft"),
    "timeEndPeriod": ("winmm timer resolution", "soft"),
    "SetProcessInformation": ("power throttling", "soft"),
    # shared memory / handles between processes
    "CreateFileMappingA": ("cross-process shared memory", "hard"),
    "CreateFileMappingW": ("cross-process shared memory", "hard"),
    "OpenFileMappingA": ("cross-process shared memory", "hard"),
    "OpenFileMappingW": ("cross-process shared memory", "hard"),
    "DuplicateHandle": ("handle sharing", "hard"),
}

# Not forbidden, but load-bearing on this port: reported so a reviewer sees them in context.
WATCHLIST = {
    "VirtualAlloc": "guest window reservation / lazy commit",
    "VirtualFree": "guest reset",
    "VirtualProtect": "guest page protection",
    "VirtualQuery": "lazy commit probe",
    "GetWriteWatch": "vertex cache write tracking (may be refused; runtime tolerates -1)",
    "ResetWriteWatch": "vertex cache write tracking",
    "AddVectoredExceptionHandler": "lazy commit fault handler",
    "TerminateProcess": "plat_terminate",
    "CreateWaitableTimerExW": "precise frame pacing",
    "QueryPerformanceCounter": "plat_now_ns",
    "OutputDebugStringA": "diagnostics",
}

SOURCE_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".m", ".mm"}
SKIP_DIRS = {"third_party", "build", ".git", "generated", "obj", "out"}


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments and string/char literals, keeping every newline so that
    reported line numbers match the file. Prose mentioning an API must not be flagged."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                if text[j] == "\\":
                    j += 1
                if j < n and text[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


# ------------------------------------------------------------------------------------------
# A minimal preprocessor. The point: desktop-only code sits behind #elif defined(_WIN32) and is
# compiled out of a UWP build, so flagging it is a false positive. Evaluating the guards the way
# clang-cl would is what makes this check meaningful *after* the port's patches are applied.
#
# The macro set is the UWP configuration this project builds:
UWP_MACROS = {
    "SKYRECOMP_UWP": 1,
    "_WIN32": 1,
    "_CRT_APP": 1,
    "WINAPI_FAMILY": "WINAPI_FAMILY_APP",
    "WINAPI_FAMILY_APP": 1,
    "WINAPI_FAMILY_DESKTOP_APP": 0,
}

TOKEN_RE = re.compile(r"defined\s*\(?\s*([A-Za-z_]\w*)\s*\)?|[A-Za-z_]\w*|&&|\|\||==|!=|!|\(|\)|\d+")


def _eval_pp(expr: str):
    """True/False for a #if expression, or None when it uses something we don't model."""
    expr = expr.strip()
    if not expr:
        return None
    parts = []
    pos = 0
    for m in TOKEN_RE.finditer(expr):
        if expr[pos:m.start()].strip():
            return None                      # unknown text between tokens
        pos = m.end()
        tok = m.group(0)
        if m.group(1) is not None or tok.startswith("defined"):
            name = m.group(1) or ""
            parts.append("1" if UWP_MACROS.get(name, 0) else "0")
        elif tok == "&&":
            parts.append(" and ")
        elif tok == "||":
            parts.append(" or ")
        elif tok == "!":
            parts.append(" not ")
        elif tok in ("==", "!=", "(", ")",):
            parts.append(tok)
        elif tok.isdigit():
            parts.append(tok)
        else:
            v = UWP_MACROS.get(tok)
            if isinstance(v, str):
                parts.append(repr(v))
            else:
                parts.append("1" if v else "0")
    if expr[pos:].strip():
        return None
    try:
        return bool(eval("".join(parts), {"__builtins__": {}}, {}))
    except Exception:
        return None


def live_lines(code: str):
    """Yield (lineno, line) for lines a UWP build actually compiles."""
    stack = []          # entries: [parent_active, any_branch_taken]
    active = True
    for lineno, line in enumerate(code.splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            d = stripped[1:].strip()
            word = d.split(None, 1)[0] if d else ""
            rest = d[len(word):].strip()
            if word in ("if", "ifdef", "ifndef"):
                if not active:
                    stack.append([False, True])
                else:
                    if word == "ifdef":
                        cond = bool(UWP_MACROS.get(rest.split()[0] if rest else "", 0))
                    elif word == "ifndef":
                        cond = not UWP_MACROS.get(rest.split()[0] if rest else "", 0)
                    else:
                        cond = _eval_pp(rest)
                    if cond is None:
                        cond = True          # unknown guard: keep the code visible (conservative)
                    stack.append([True, cond])
                    active = cond
            elif word == "elif":
                if not stack:
                    continue
                parent, taken = stack[-1]
                if not parent:
                    active = False
                elif taken:
                    active = False
                else:
                    cond = _eval_pp(rest)
                    if cond is None:
                        cond = True
                    active = cond
                    stack[-1][1] = taken or cond
            elif word == "else":
                if not stack:
                    continue
                parent, taken = stack[-1]
                active = parent and not taken
                stack[-1][1] = True
            elif word == "endif":
                if stack:
                    stack.pop()
                active = stack[-1][1] if stack and stack[-1][0] else (True if not stack else False)
                if stack and not stack[-1][0]:
                    active = False
            continue
        if active:
            yield lineno, line


def scan(root: str, excludes=()):
    findings, watched = [], []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if os.path.splitext(name)[1].lower() not in SOURCE_EXT:
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if any(fnmatch.fnmatch(rel, pat) for pat in excludes):
                continue
            try:
                raw = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            code = strip_comments(raw)
            for lineno, line in live_lines(code):
                for api, (reason, severity) in FORBIDDEN.items():
                    if re.search(r"\b" + re.escape(api) + r"\s*\(", line):
                        findings.append({
                            "file": rel, "line": lineno, "api": api,
                            "reason": reason, "severity": severity,
                        })
                for api, note in WATCHLIST.items():
                    if re.search(r"\b" + re.escape(api) + r"\s*\(", line):
                        watched.append({"file": rel, "line": lineno, "api": api, "note": note})
    findings.sort(key=lambda f: (f["severity"] != "hard", f["file"], f["line"]))
    return findings, watched


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="source tree to scan (e.g. runtime/)")
    ap.add_argument("--fail", action="store_true", help="exit 1 if any hard violation exists")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--exclude", action="append", default=[], metavar="GLOB",
                    help="skip paths matching GLOB, relative to SRC (repeatable). The UWP build "
                         "does not compile runtime/platform/{windows,linux,macos}.c at all, so a "
                         "real gate passes --exclude 'platform/windows.c'.")
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        print(f"error: not a directory: {args.src}", file=sys.stderr)
        return 2

    findings, watched = scan(args.src, args.exclude)
    hard = [f for f in findings if f["severity"] == "hard"]

    if args.json:
        print(json.dumps({"violations": findings, "watchlist_hits": watched}, indent=2))
    else:
        print(f"UWP AppContainer allowlist check: {args.src}")
        print(f"  hard violations: {len(hard)}")
        print(f"  soft concerns  : {len(findings) - len(hard)}")
        print(f"  watchlist hits : {len(watched)} (allowed; listed for review)")
        if findings:
            print()
            print("VIOLATIONS")
            for f in findings:
                mark = "HARD" if f["severity"] == "hard" else "soft"
                print(f"  [{mark}] {f['file']}:{f['line']}  {f['api']}  ({f['reason']})")
        if watched:
            print()
            print("WATCHLIST")
            for w in watched:
                print(f"  {w['file']}:{w['line']}  {w['api']}  {w['note']}")

    return 1 if (args.fail and hard) else 0


if __name__ == "__main__":
    sys.exit(main())
