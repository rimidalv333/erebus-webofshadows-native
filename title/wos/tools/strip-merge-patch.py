#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fix118: route the game's window-strip loop through wos_draw_vertex_list.

sub_8291F508 (draws one render item) walks a list of {first vertex, vertex count} pairs for
the city's facade meshes and calls DrawVertices (sub_82ABECF0) once per pair, at 0x8291F948,
with nothing changed between the calls. This points that one call at wos_draw_vertex_list
(wos_ps5_native.cpp), which gets the list and, with wos_strip_merge = 1, draws the pairs
that follow each other in the vertex buffer as one. With wos_strip_merge = 0 it calls
DrawVertices exactly as before.

Idempotent; touches only the file that holds sub_8291F508 (so a rebuild recompiles one
file). Usage: strip-merge-patch.py <generated/default>
"""
import glob
import os
import re
import sys

DECL = 'extern "C" uint32_t wos_draw_vertex_list(PPCContext &ctx, uint8_t *base, uint32_t list);  // fix118\n'
LABEL = "loc_8291F93C:"


def main():
    out = sys.argv[1]
    for p in sorted(glob.glob(os.path.join(out, "*.cpp"))):
        with open(p, errors="surrogateescape") as f:
            text = f.read()
        if "DEFINE_REX_FUNC(sub_8291F508)" not in text:
            continue
        if "wos_draw_vertex_list(ctx, base," in text:
            print("strip merge: already in place")
            return 0
        lines = text.splitlines(keepends=True)
        start = next(i for i, l in enumerate(lines) if l.startswith("DEFINE_REX_FUNC(sub_8291F508)"))
        label = next((i for i in range(start, len(lines)) if lines[i].strip() == LABEL), None)
        if label is None:
            print("strip merge: the strip loop was not found - left as it is", file=sys.stderr)
            return 1
        # loop: r3 = device, r6 = 4(list), r4 = prim, then the call
        reg = None
        for i in range(label + 1, min(label + 16, len(lines))):
            m = re.match(r"^\s*ctx\.r6\.u64 = REX_LOAD_U32\(((?:ctx\.)?r31)\.u32 \+ 4\);", lines[i])
            if m:
                reg = m.group(1)
            if lines[i].strip() == "sub_82ABECF0(ctx, base);":
                if reg is None:
                    break
                pad = lines[i][: len(lines[i]) - len(lines[i].lstrip())]
                lines[i] = f"{pad}{reg}.u32 = wos_draw_vertex_list(ctx, base, {reg}.u32);  // fix118\n"
                inc = next(k for k, l in enumerate(lines) if l.startswith("#include"))
                lines.insert(inc + 1, DECL)
                with open(p + ".smerge", "w", errors="surrogateescape") as f:
                    f.writelines(lines)
                os.replace(p + ".smerge", p)
                print(f"strip merge: the strip loop now calls wos_draw_vertex_list ({os.path.basename(p)})")
                return 0
        print("strip merge: the strip loop's code is not as expected - left as it is", file=sys.stderr)
        return 1
    print("strip merge: sub_8291F508 not found - left as it is", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
