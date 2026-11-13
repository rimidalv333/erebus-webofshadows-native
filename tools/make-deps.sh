#!/usr/bin/env bash
# Fill WoS-PS5/deps/ with everything build.sh needs that is not a system package, from
# a working ~/ps5 setup (run once, by the release maker; players never run this).
#
#   tools/make-deps.sh [~/ps5]
#
# Each repository becomes a small git repository holding only the pinned commit (the
# build's scripts export that commit with git), so the release builds with no access
# to anyone's GitHub. Also bundled: ReXGlue's third-party sources as built, the payload
# SDK release zip, zlib's source and LLVM 20.1.8.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

src=$(cd -- "${1:-$HOME/ps5}" && pwd)
release=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps=$release/deps
say() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

rexglue_rev=f47ccdde34b3a992f7872a27619fc588040c229f
vulkan_rev=5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5
payload_revs=(1de8b37d583de476ad07cde9a0d562b75fb1be8f
    "$(sed -n 's/^sdk_revision=//p' "$src/PS5_Vulkan/tools/setup-native-dependencies.sh")")
mesa_rev=$(sed -n 's/^mesa_revision=//p' "$src/PS5_Vulkan/tools/build-radv.sh")
xiso_url=https://github.com/XboxDev/extract-xiso.git
xiso_rev=3f5b62cfe68f000b0e3c8a30104973f3a297948e
mesa_url=https://github.com/mihawk-99/PS5_Mesa.git

# A git repository at $dest holding the given commits (the first checked out), fetched
# from $from (a local repository or a URL)
pinned() {
    local dest=$1 from=$2
    shift 2
    rm -rf "$dest"
    git init -q "$dest"
    for rev in "$@"; do
        git -C "$dest" fetch -q --depth 1 \
            --upload-pack='git -c uploadpack.allowAnySHA1InWant=true upload-pack' "$from" "$rev" ||
            return 1
    done
    git -C "$dest" -c advice.detachedHead=false checkout -q "$1"
    # keep it small
    git -C "$dest" reflog expire --expire=now --all
    git -C "$dest" gc -q --prune=now
}

mkdir -p "$deps"
say "rexglue-sdk at ${rexglue_rev:0:7}"
pinned "$deps/rexglue-sdk" "$src/rexglue-sdk" "$rexglue_rev" || fail "your rexglue-sdk has no $rexglue_rev"
# its third-party sources, as your build used them (build-rexglue.sh's list, plus inja for the codegen tool)
for sub in cli11 libmspack FFmpeg tomlplusplus simde xxHash spdlog fmt utfcpp imgui sdl3 o1heap \
        vulkan-headers vulkan-memory-allocator spirv-headers spirv-tools glslang inja; do
    [[ -d $src/rexglue-sdk/thirdparty/$sub ]] || fail "your rexglue-sdk has no thirdparty/$sub"
    rsync -a --exclude .git "$src/rexglue-sdk/thirdparty/$sub/" "$deps/rexglue-sdk/thirdparty/$sub/"
done
say "PS5_Vulkan at ${vulkan_rev:0:7}"
pinned "$deps/PS5_Vulkan" "$src/PS5_Vulkan" "$vulkan_rev" || fail "your PS5_Vulkan has no $vulkan_rev"
say "PS5_PayloadSDK at ${payload_revs[0]:0:7} and ${payload_revs[1]:0:7}"
pinned "$deps/PS5_PayloadSDK" "$src/PS5_PayloadSDK" "${payload_revs[@]}" ||
    fail "your PS5_PayloadSDK lacks one of ${payload_revs[*]}"
say "PS5_Mesa at ${mesa_rev:0:7} (your clone is partial: its files may come from GitHub)"
pinned "$deps/PS5_Mesa" "$src/PS5_Mesa" "$mesa_rev" 2> /dev/null ||
    pinned "$deps/PS5_Mesa" "$mesa_url" "$mesa_rev" || fail "could not get PS5 Mesa $mesa_rev"
say "extract-xiso at ${xiso_rev:0:7}"
pinned "$deps/extract-xiso" "$xiso_url" "$xiso_rev" || fail "could not get extract-xiso $xiso_rev"

say "EdgeOfTimeRecomp (the native renderer's third-party code)"
bash "$release/tools/bundle-eot.sh" "$src"
say "downloads"
mkdir -p "$deps/downloads"
zip=$(find "$src/PS5_WebOfShadowsTitle/.deps/native" "$src/PS5_Vulkan/.deps/native" -maxdepth 1 \
    -name ps5-payload-sdk.zip -print -quit 2> /dev/null || true)
[[ -n $zip ]] || fail "no ps5-payload-sdk.zip in your build's .deps/native"
cp "$zip" "$deps/downloads/ps5-payload-sdk.zip"
zlib=$(find "$src/PS5_Vulkan/.deps/native/zlib" -maxdepth 1 -name 'zlib-*.tar.gz' -print -quit 2> /dev/null || true)
[[ -n $zlib ]] || fail "no zlib source archive in $src/PS5_Vulkan/.deps/native/zlib"
cp "$zlib" "$deps/downloads/"
[[ -x $src/llvm-20/bin/clang-20 && -f $src/llvm-20/.wos-llvm ]] || fail "no LLVM 20.1.8 in $src/llvm-20"
rm -rf "$deps/llvm-20"
cp -a "$src/llvm-20" "$deps/llvm-20"
# LLVM's own lld may have been swapped for a link to this machine's lld 20
# (get-llvm20.sh does that where libxml2.so.2 is missing): the builder redoes it
[[ -L $deps/llvm-20/bin/lld ]] && rm -f "$deps/llvm-20/bin/lld" &&
    echo "  note: llvm-20/bin/lld was a link to this machine's lld; the builder restores one"

du -sh "$deps"/* | sed 's/^/  /'
say "done: $deps (now tools/pack-deps.sh, for the repository)"
