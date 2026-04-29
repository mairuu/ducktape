#!/usr/bin/env python3
"""
scripts/gen_ccjson.py
Optional helper – generates compile_commands.json from scratch.
Used by: make CCJSON_BACKEND=python
Or run directly: python3 scripts/gen_ccjson.py --help
"""

import argparse
import glob
import json
import os
import shlex


def main():
    p = argparse.ArgumentParser(description="Generate compile_commands.json")
    p.add_argument("--root",     default=os.getcwd(), help="project root (abs path)")
    p.add_argument("--srcdir",   default="src",       help="source directory")
    p.add_argument("--builddir", default="build",     help="build output directory")
    p.add_argument("--cc",       default="cc",        help="C compiler")
    p.add_argument("--cflags",   default="-std=c23 -Wall -Wextra -Iinclude",
                   help="compiler flags (space-separated string)")
    p.add_argument("--out",      default="compile_commands.json")
    args = p.parse_args()

    root     = os.path.abspath(args.root)
    srcdir   = os.path.join(root, args.srcdir)
    builddir = os.path.join(root, args.builddir)
    cflags   = shlex.split(args.cflags)

    entries = []
    for src in sorted(glob.glob(os.path.join(srcdir, "**", "*.c"), recursive=True)):
        rel   = os.path.relpath(src, root)
        stem  = os.path.splitext(os.path.basename(src))[0]
        obj   = os.path.join(builddir, stem + ".o")
        cmd   = [args.cc] + cflags + ["-c", src, "-o", obj]
        entries.append({
            "directory": root,
            "file":      src,
            "output":    obj,
            "arguments": cmd,
        })

    out_path = os.path.join(root, args.out)
    with open(out_path, "w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")

    print(f"Wrote {len(entries)} entries to {out_path}")


if __name__ == "__main__":
    main()
