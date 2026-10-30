#!/usr/bin/env python3
"""Embed third_party/smaa/SMAA.hlsl as the C string runtime/nvn/smaa_data.c includes.

The Makefile does this with `tr -d '\\r' | tr '\\200-\\377' '?' | sed -e 's/\\\\/\\\\\\\\/g'
-e 's/"/\\\\"/g' -e 's/^/"/' -e 's/$/\\\\n"/'`. That pipeline is why the Windows CMake build needs
its own copy: a build host is not guaranteed to have tr and sed, and CMake has no portable string
escape. Python is already a build dependency (`make gen` is Python), so it is the honest place to
put the same four transformations - in the same order, byte for byte:

  1. CR removed (a CR inside a string literal ends it early on some compilers)
  2. every non-ASCII byte becomes '?' (the shader's comments are not UTF-8)
  3. backslash doubled, double quote backslashed
  4. the line wrapped in quotes with a trailing \\n

Usage: gen-smaa-inc.py <SMAA.hlsl> <out.inc>
"""

import sys


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    with open(argv[1], "rb") as f:
        raw = f.read()
    raw = raw.replace(b"\r", b"")
    raw = bytes(b if b < 0x80 else 0x3F for b in raw)  # 0x3F is '?'
    text = raw.decode("ascii")
    out = []
    for line in text.split("\n"):
        if line == "" and len(out) and out[-1] is None:
            continue
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('"%s\\n"' % escaped)
    # The trailing element of split("\n") on a file that ends with a newline is not a line of its
    # own; dropping it keeps the output identical to the sed pipeline.
    if text.endswith("\n"):
        out.pop()
    with open(argv[2], "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
