#!/usr/bin/env python3
"""WebOfShadowsRecomp for the PS5 - plume's VulkanMemoryAllocator, kept apart from the runtime's.

plume (Edge of Time's renderer) compiles VulkanMemoryAllocator's implementation into
plume_vulkan.cpp; so does the ReXGlue runtime's Vulkan GPU, which the title also links
(the emulated GPU, when the native renderer is off), each at its own VMA version. One
executable cannot hold both under the same names, so every name plume's object defines
for VMA (its C API, vma*, and its classes, Vma*) takes a prefix; the object's own
references follow, so plume keeps using its own VMA.

usage: prefix_vma.py --bin <SDK bin with llvm-nm, llvm-objcopy> --prefix <p> <in.o> <out.o>

SPDX-License-Identifier: MIT
"""
import argparse
import os
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()
    symbols = subprocess.run([os.path.join(args.bin, "llvm-nm"), "-g", "--defined-only", args.input],
                             check=True, capture_output=True, text=True).stdout
    renames = []
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        name = fields[2]
        if "Vma" in name or name.startswith("vma") or "VMA" in name:
            renames.append(f"{name} {args.prefix}{name}")
    table = args.output + ".renames"
    with open(table, "w") as f:
        f.write("\n".join(renames) + "\n")
    subprocess.run([os.path.join(args.bin, "llvm-objcopy"), f"--redefine-syms={table}", args.input,
                    args.output], check=True)
    print(f"prefix_vma: {len(renames)} VMA names in {os.path.basename(args.input)} take {args.prefix}")


if __name__ == "__main__":
    main()
