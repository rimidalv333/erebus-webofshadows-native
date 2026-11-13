#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make rexglue's non_argument_as_local safe for Web of Shadows.

With non_argument_as_local the recompiler keeps r0, r2, r11, r12, f0 and v32-v63 in
C++ locals of each function. That is only right if no value in those registers ever
crosses from one function to another, and on the Xbox 360 some do:

- SEH funclets (__except / __finally blocks, ~450 of them) get their frame in r12
  from the code that calls them (an indirect call);
- a few split-off pieces of functions are entered by a jump with r11 (or f0, v57,
  v63) already set by the code that jumped there;
- the stack probe gets the frame size in r12.

Each of these read a zeroed local instead. This pass, run over the generated code:

1. starts every function's non-argument locals from ctx (the loads the C++ compiler
   finds dead, which is nearly all, cost nothing);
2. stores the caller's non-argument locals to ctx before every indirect call, every
   tail call (a call followed by return) and every call to a function that reads one
   of them before writing it.

Run once on freshly generated code (it marks each file it changed and skips marked
files). Usage: nonarg-fix.py <generated/default>
"""
import glob
import os
import re
import sys

MARK = "// wos: non_argument_as_local fix applied\n"
FN = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
DECL = re.compile(r"^\s+PPC\w*Register (\w+)\{\};")
REG = re.compile(r"(?<![\w.])([rfv]\d+)\.")
ASSIGN = re.compile(r"^\s*(\w+)\.\w+(?:\[[^\]]*\])?\s*=(?!=)(.*)$")
VSTORE = re.compile(r"^\s*simde_mm_store\w*\(\s*(?:\(\w+\*\))?\s*(v\d+)\.\w+\s*,(.*)$")
DIRECT = re.compile(r"^(\s+)(\w+)\(ctx, base\);\s*$")
LR_SAVE = re.compile(r"^\s*REX_STORE_U32\(ctx\.r1\.u32 \+ -8, r12\.u32\);")
INDIRECT = re.compile(r"^(\s+)(?:\{ )?REX_CALL_INDIRECT_FUNC\(")


def non_argument(name):
    m = re.fullmatch(r"([rfv])(\d+)", name)
    if not m:
        return False
    kind, i = m.group(1), int(m.group(2))
    if kind == "r":
        return i in (0, 2, 11, 12)
    if kind == "f":
        return i == 0
    return 32 <= i <= 63


def live_ins(lines):
    """Per function: the non-argument locals read (in text order) before written."""
    result = {}
    cur = None
    for line in lines:
        m = FN.match(line)
        if m:
            cur = m.group(1)
            local, written, live = set(), set(), set()
            result[cur] = live
            continue
        if cur is None:
            continue
        d = DECL.match(line)
        if d:
            if non_argument(d.group(1)):
                local.add(d.group(1))
            continue
        s = line.strip()
        if not local or not s or s.startswith("//"):
            continue
        target, rhs = None, line
        a = ASSIGN.match(line)
        if a:
            target, rhs = a.group(1), a.group(2)
        else:
            v = VSTORE.match(line)
            if v:
                target, rhs = v.group(1), v.group(2)
        if LR_SAVE.match(line):
            continue  # mflr r12 / stw r12,-8(r1): the return address, unused (skip_lr)
        for r in REG.findall(rhs):
            if r in local and r not in written:
                live.add(r)
        if target in local:
            written.add(target)
    return result


def main():
    out = sys.argv[1]
    files = sorted(glob.glob(os.path.join(out, "*.cpp")))
    texts = {}
    needs = {}
    for p in files:
        with open(p, errors="surrogateescape") as f:
            texts[p] = f.readlines()
        if texts[p][:1] == [MARK]:
            continue
        needs.update(live_ins(texts[p]))
    # the vectors any function takes from its caller (a few); the scalars always
    vec_in = {r for live in needs.values() for r in live if r.startswith("v")}
    callee_reads = {fn for fn, live in needs.items() if live}

    changed = 0
    for p in files:
        lines = texts[p]
        if lines[:1] == [MARK] or not any(FN.match(l) for l in lines):
            continue
        res = [MARK]
        i, n = 0, len(lines)
        local = []
        while i < n:
            line = lines[i]
            m = FN.match(line)
            if m:
                fn = m.group(1)
                res.append(line)
                i += 1
                # the prologue and the declaration block
                local = []
                while i < n and (lines[i].strip().startswith("REX_FUNC_PROLOGUE") or
                                 DECL.match(lines[i]) or
                                 re.match(r"^\s+(uint32_t|uint64_t) \w+\{\};", lines[i])):
                    d = DECL.match(lines[i])
                    if d and non_argument(d.group(1)):
                        local.append(d.group(1))
                    res.append(lines[i])
                    i += 1
                for r in local:
                    if r[0] != "v" or r in needs.get(fn, ()):
                        res.append(f"\t{r} = ctx.{r};\n")
                continue
            spill = [r for r in local if r[0] != "v" or r in vec_in]
            if spill:
                ind = INDIRECT.match(line)
                dm = DIRECT.match(line)
                tail = False
                if dm:
                    j = i + 1
                    while j < n and not lines[j].strip():
                        j += 1
                    tail = j < n and lines[j].strip() == "return;"
                if ind or (dm and (tail or dm.group(2) in callee_reads)):
                    pad = (ind or dm).group(1)
                    res.extend(f"{pad}ctx.{r} = {r};\n" for r in spill)
            res.append(line)
            i += 1
        with open(p + ".nafix", "w", errors="surrogateescape") as f:
            f.writelines(res)
        os.replace(p + ".nafix", p)
        changed += 1
    print(f"non_argument_as_local fix: {changed} files, "
          f"{len(callee_reads)} functions take a non-argument register from their caller")


if __name__ == "__main__":
    main()
