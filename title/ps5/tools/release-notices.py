#!/usr/bin/env python3
"""LEGAL.txt and licenses/ beside eboot.bin, for a build made by the WoS-PS5 release's
build.sh. The release's sources are plain copies (no git history), so the notices are
taken from the files themselves, with the revisions the release pins.

    release-notices.py <app folder> <PS5_Vulkan> <RADV archive>

SPDX-License-Identifier: MIT
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

PS5 = Path(__file__).resolve().parent.parent
ROOT = PS5.parent          # the title
WORK = ROOT.parent         # the build folder: the dependencies beside it
NOTICE = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)([._-]|$)", re.I)
SKIP = {".git", "build", "out", "test", "tests", "doc", "docs", "examples"}


def head(repo):
    r = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 and (Path(repo) / ".git").exists() else "bundled"


def copy(src, dest):
    src = Path(src)
    if not src.is_file():
        raise SystemExit(f"missing notice: {src}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)


def notices(tree, dest, depth=4):
    """Every license/copying/notice file in a source tree (and licenses/ folders), to depth"""
    tree, found = Path(tree), 0
    def walk(d, level):
        nonlocal found
        for p in sorted(d.iterdir()):
            if p.is_dir() and not p.is_symlink():
                if p.name in SKIP or level >= depth:
                    continue
                if p.name.lower() in ("licenses", "licences", "license"):
                    shutil.copytree(p, dest / p.relative_to(tree), dirs_exist_ok=True)
                    found += 1
                else:
                    walk(p, level + 1)
            elif p.is_file() and NOTICE.match(p.name):
                copy(p, dest / p.relative_to(tree))
                found += 1
    walk(tree, 0)
    if not found:
        raise SystemExit(f"no license files in {tree}")


def main():
    app, vulkan, archive = (Path(a).resolve() for a in sys.argv[1:4])
    out = app / "licenses"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    rexglue = WORK / "rexglue-sdk"
    eot = WORK / "EdgeOfTimeRecomp"
    sdk = WORK / "PS5_PayloadSDK"
    mesa = WORK / "PS5_Mesa"
    prov = (archive.parent.parent / "PROVENANCE.txt").read_text()
    mesa_rev = re.search(r"^revision: ([0-9a-f]{40})", prov, re.M).group(1)
    parts = []

    def part(ident, name, licence, source):
        parts.append((name, licence, f"licenses/{ident}/", source))

    # the title, and the libraries in its tree
    t = out / "title"
    copy(ROOT / "LICENSE.md", t / "LICENSE.md")
    for path, name in [("external/imgui/LICENSE.txt", "imgui-LICENSE.txt"), ("external/ktx/LICENSE.md", "ktx-LICENSE.md"),
                       ("external/ktx/NOTICE.md", "ktx-NOTICE.md"), ("external/tinygltf/LICENSE", "tinygltf-LICENSE"),
                       ("ps5/third_party/volk/LICENSE.md", "volk-LICENSE.md")]:
        copy(ROOT / path, t / name)
    part("title", "the title: its program, the foundation (Sascha Willems' Vulkan examples, the PS5 layer) and the "
         "libraries in its tree (Dear ImGui, libktx, tinygltf, volk, the Vulkan headers, glm)",
         "MIT; Apache-2.0 (libktx); Apache-2.0 OR MIT (the Vulkan headers)", "the WoS-PS5 release (title/)")

    # ReXGlue with this title's patch, and its third-party code
    notices(rexglue, out / "rexglue", depth=3)
    part("rexglue", "ReXGlue SDK and runtime (Xenia's Xbox 360 GPU emulation) and its third-party libraries",
         "BSD-3-Clause; each library its own (licenses/rexglue/thirdparty/)",
         f"https://github.com/KongaTime/rexglue-sdk at {head(rexglue)}, with wos/patches/rexglue/wos-rexglue.patch")
    # Edge of Time's renderer: plume, XenosRecomp's zstd and smol-v, dxc (build only)
    notices(eot, out / "edgeoftime", depth=5)
    part("edgeoftime", "the native renderer's base: EdgeOfTimeRecomp (KongaTime), plume, zstd, smol-v",
         "MIT; zstd BSD-3-Clause; each its own (licenses/edgeoftime/)", "EdgeOfTimeRecomp, bundled in the release")

    (out / "webofshadows").mkdir()
    (out / "webofshadows/NOTICE.txt").write_text(
        "WebOfShadowsRecomp (toowasmir) carries no licence file. This title compiles one of its headers\n"
        "(src/stubs/unregistered_func_handler.h), bundled in the release with credit to its author.\n")
    part("webofshadows", "WebOfShadowsRecomp's unregistered guest function handler (compiled in)",
         "none stated by its author", "WebOfShadowsRecomp (toowasmir), bundled in the release")

    copy(sdk / "LICENSE", out / "platform/GPL-3.0.txt")
    copy(sdk / "platform/src/regex/COPYRIGHT.musl", out / "platform/musl-regex-COPYRIGHT.txt")
    copy(sdk / "platform/src/dlmalloc/SOURCE", out / "platform/dlmalloc-SOURCE.txt")
    part("platform", "the PS5 platform layer and SDK headers (PS5_PayloadSDK) with musl's regex and dlmalloc",
         "GPL-3.0-or-later; musl's regex MIT; dlmalloc public domain",
         f"https://github.com/mihawk-99/PS5_PayloadSDK at {head(sdk)}")

    copy(vulkan / "LICENSE", out / "ps5-vulkan/GPL-3.0.txt")
    part("ps5-vulkan", "PS5_Vulkan: the RADV link recipe, C runtime start-up, AGC stubs, sce_module/libc.prx",
         "GPL-3.0-or-later", f"https://github.com/mihawk-99/PS5_Vulkan at {head(vulkan)}")

    copy(mesa / "docs/license.rst", out / "radv/Mesa-license.rst")
    shutil.copytree(mesa / "licenses", out / "radv/licenses")
    part("radv", "RADV, Mesa's Vulkan driver (ACO, NIR, Mesa's Vulkan runtime), from PS5_Mesa",
         "MIT, with other licences per file", f"https://github.com/mihawk-99/PS5_Mesa at {mesa_rev}")

    copy(PS5 / "licenses/llvm-runtime/LICENSE.TXT", out / "llvm-runtime/LICENSE.TXT")
    part("llvm-runtime", "LLVM libc++, libc++abi, libunwind and compiler-rt builtins (linked into eboot.bin)",
         "Apache-2.0 WITH LLVM-exception", "ps5-payload-dev SDK v0.42")

    (app / "LEGAL.txt").write_text(
        "Spider-Man: Web of Shadows (PS5) - legal\n========================================\n\n"
        "Built by the player from their own copy of the game. No game data, firmware or keys are included;\n"
        "use only a game you own. Not affiliated with or endorsed by Activision, Marvel, Sony Interactive\n"
        "Entertainment or Microsoft. \"PlayStation\" and \"PS5\" are trademarks of Sony Interactive Entertainment.\n"
        "Vulkan is a registered trademark of the Khronos Group; the RADV port is not a conformant product.\n\n"
        "The program links GPL-3.0-or-later code (the PS5 platform layer, PS5_Vulkan), so the combined work is\n"
        "GPL-3.0-or-later. Each component and its licence: licenses/README.txt. Licence texts give no rights\n"
        "to the original game or to the code, shaders and art generated from it.\n")
    lines = ["The parts of this title", "=======================", ""]
    for name, licence, texts, source in parts:
        lines += [name, f"  licence: {licence}", f"  texts:   {texts}", f"  source:  {source}", ""]
    (out / "README.txt").write_text("\n".join(lines))
    print(f"==> notices: LEGAL.txt, licenses/ ({len(parts)} parts)")


if __name__ == "__main__":
    main()
