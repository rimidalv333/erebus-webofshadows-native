#!/usr/bin/env bash
# PS5 Vulkan Template - build the title into dist/<TITLE_ID>/.
#
#   ps5/tools/build.sh
#
# Configures ps5/ with the payload SDK's CMake toolchain (once), compiles what
# changed, links with RADV and packages the title folder (tools/link-title.sh),
# then lays the assets in (tools/build-assets.py).
#
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan): the RADV
#                    release archive, the link recipe, the native tool and libc.prx
#
# The payload SDK is this title's own pin of my fork (tools/setup-sdk.sh).
#   PS5_CLANG        the host clang the compiler wrappers drive (default: clang)
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: MIT

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$ps5/tools/setup-sdk.sh")
build="$root/build/ps5"
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}
# WebOfShadowsRecomp: everything for the console with clang 20 (wos/tools/clang20.sh)
if [[ -f $root/wos/tools/clang20.sh ]]; then
    # shellcheck source=/dev/null
    source "$root/wos/tools/clang20.sh"
fi

# WebOfShadowsRecomp: the PS5_Vulkan revision the title is console-checked with
# (wos/tools/setup.sh); WOS_ANY_PS5_VULKAN=1 builds with another anyway
wos_ps5_vulkan=5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5
if [[ -d $root/wos && -z ${WOS_ANY_PS5_VULKAN:-} ]] &&
        [[ $(git -C "$vulkan" rev-parse HEAD 2>/dev/null) != "$wos_ps5_vulkan" ]]; then
    echo "PS5_Vulkan at $vulkan is not at ${wos_ps5_vulkan:0:7}, the revision this title runs with on the" >&2
    echo "console (later ones make an eboot.bin firmware 13.00 refuses): run wos/tools/setup.sh" >&2
    exit 1
fi
# ...and RADV built at the revision that PS5_Vulkan pins, not one left from another
wos_radv=7b59ef27c1b09b9671bc4153c41940c3155c3af2
if [[ -d $root/wos && -z ${WOS_ANY_PS5_VULKAN:-} ]] &&
        ! grep -q "^revision: $wos_radv" "$vulkan/.deps/native/radv-release/PROVENANCE.txt" 2>/dev/null; then
    echo "RADV in $vulkan/.deps/native/radv-release is not PS5 Mesa ${wos_radv:0:7}: run wos/tools/setup.sh" >&2
    exit 1
fi
# ...all of it built by the one clang (wos/tools/clang20.sh): RADV, the runtime and
# this title's own build folder, which another compiler's is started again
if [[ -d $root/wos && -n ${wos_clang_id:-} ]]; then
    for stamp in "$vulkan/.deps/work/radv-build-ps5-release.wos-clang" \
            "${REXGLUE_PS5_BUILD:-$(dirname "$root")/rexglue-ps5-build}/.wos-clang"; do
        if ! grep -qxF "$wos_clang_id" "$stamp" 2>/dev/null; then
            echo "RADV or the runtime was built by another compiler than $wos_clang_id ($stamp): run wos/tools/setup.sh" >&2
            exit 1
        fi
    done
    if [[ -d $build ]] && ! grep -qxF "$wos_clang_id" "$build/.wos-clang" 2>/dev/null; then
        echo "build/ps5 was built by another compiler: starting it again"
        rm -rf "$build"
    fi
    mkdir -p "$build"
    echo "$wos_clang_id" > "$build/.wos-clang"
fi
if [[ ! -f $build/build.ninja ]]; then
    mkdir -p "$build"
    cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DPS5_VULKAN_DIR="$vulkan" -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
        ${WOS_TITLE_APP:+-DWOS_TITLE_APP="$WOS_TITLE_APP"} > "$build.configure.log" 2>&1 \
        || { cat "$build.configure.log" >&2; exit 1; }
fi
# WebOfShadowsRecomp: WOS_TITLE_APP=test or game picks what the title runs (the
# choice stays in the build folder until changed again)
if [[ -n ${WOS_TITLE_APP:-} ]]; then
    cmake -S "$ps5" -B "$build" -DWOS_TITLE_APP="$WOS_TITLE_APP" > "$build.configure.log" 2>&1 \
        || { cat "$build.configure.log" >&2; exit 1; }
fi
# The game's recompiled code is large: each compile can take a GB or two, so by
# default as many at once as there are 2 GB of memory (WOS_JOBS chooses)
jobs=${WOS_JOBS:-}
if [[ -z $jobs ]]; then
    memory_gb=$(awk '/MemTotal/ { print int($2 / 1048576) }' /proc/meminfo)
    jobs=$(( memory_gb / 2 ))
    (( jobs < 1 )) && jobs=1
    (( jobs > $(nproc) )) && jobs=$(nproc)
fi
# WebOfShadowsRecomp: WOS_NATIVE=1 builds the native-renderer title (PPSA99311)
# from the same tree - its own param.json and a toml with wos_native on. Unset =
# the normal emulation title (PPSA99310). link-title.sh (run by the 'title'
# target, and inheriting WOS_NATIVE from here) honours the same switch.
wos_param="$ps5/sce_sys/param.json"
wos_toml="$root/wos/wos_recomp.toml"
if [[ -n ${WOS_NATIVE:-} ]]; then
    wos_param="$ps5/sce_sys/param.native.json"
    wos_toml="$root/wos/wos_recomp.native.toml"
    # The 'title' target is tracked by its build-dir output (eboot.elf), but the
    # signed per-title eboot.bin in dist/<id>/ is a side effect keyed on the param
    # we swap here, which ninja cannot see. The code is identical to the emulation
    # build, so ninja would say "no work to do" and never sign the native title's
    # eboot.bin. Remove the tracked output so the link/sign step re-runs and
    # packages dist/PPSA99311/ (link-title.sh reads the native param via WOS_NATIVE).
    rm -f "$build/eboot.elf"
fi
"$ninja" -C "$build" -j "$jobs" title
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$wos_param")
python3 "$ps5/tools/build-assets.py" --install "$root/dist/$title_id"
# WebOfShadowsRecomp: the settings beside eboot.bin, and the folder the player's
# game files go in (wos/README-gamedata.txt says how)
if [[ -d $root/wos ]]; then
    app="$root/dist/$title_id"
    cp "$wos_toml" "$app/wos_recomp.toml"
    mkdir -p "$app/gamedata"
    cp "$root/wos/README-gamedata.txt" "$app/gamedata/PUT_YOUR_GAME_FILES_HERE.txt"
    # The PlayStation button prompts (drawn by the script, nothing of the game's)
    if python3 -c 'import numpy, PIL' 2>/dev/null; then
        python3 "$root/wos/tools/ps_buttons.py" "$app/mods/textures" >/dev/null &&
            echo "==> $title_id: PlayStation button prompts in mods/textures"
    else
        echo "==> note: python3 numpy and Pillow are missing - no PlayStation button prompts" \
             "(sudo apt install python3-numpy python3-pil)"
    fi
    echo "==> $title_id: ready in $app (add the game's files to gamedata/)"
fi
