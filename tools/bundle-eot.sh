#!/usr/bin/env bash
# Put the part of EdgeOfTimeRecomp the native renderer builds from into
# WoS-PS5/deps/EdgeOfTimeRecomp, from a working ~/ps5 (run by the release maker;
# make-deps.sh runs it too):
#
#   tools/bundle-eot.sh [~/ps5]
#
# plume (with its PS5 patch applied) and its volk / Vulkan-Headers / VMA, XenosRecomp's
# headers, zstd, smol-v and dxc (with its libraries), renderdoc's header, the licenses.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
src=$(cd -- "${1:-$HOME/ps5}" && pwd)/EdgeOfTimeRecomp
release=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dest=$release/deps/EdgeOfTimeRecomp
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
x=$src/thirdparty/XenosRecomp
for need in thirdparty/plume/contrib/volk/volk.h thirdparty/XenosRecomp/thirdparty/zstd/lib/zstd.h \
        thirdparty/XenosRecomp/thirdparty/smol-v/source/smolv.cpp ps5/patches/plume-ps5.patch \
        thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-linux; do
    [[ -e $src/$need ]] || fail "$src has no $need (git -C $src submodule update --init --recursive)"
done
ls "$x"/thirdparty/dxc-bin/lib/x64/libdxcompiler.so* > /dev/null 2>&1 ||
    fail "$x/thirdparty/dxc-bin/lib/x64 has no libdxcompiler.so"
rm -rf "$dest.partial"
mkdir -p "$dest.partial/ps5/patches" "$dest.partial/thirdparty/XenosRecomp/thirdparty"
cp "$src/LICENSE" "$dest.partial/"
cp "$src/ps5/patches/plume-ps5.patch" "$dest.partial/ps5/patches/"
r() { rsync -a --exclude .git "$@"; }
r "$src/thirdparty/plume/" "$dest.partial/thirdparty/plume/"
r "$src/thirdparty/renderdoc/" "$dest.partial/thirdparty/renderdoc/"
# XenosRecomp: everything but its other submodules (and the tool's own build)
r --exclude '/thirdparty/*' --exclude '/build*' "$x/" "$dest.partial/thirdparty/XenosRecomp/"
for sub in dxc-bin smol-v zstd; do
    r "$x/thirdparty/$sub/" "$dest.partial/thirdparty/XenosRecomp/thirdparty/$sub/"
done
# plume carries its PS5 support (the title's CMake checks for it)
if ! grep -q createProsperoDisplaySurface "$dest.partial/thirdparty/plume/plume_vulkan.cpp"; then
    (cd "$dest.partial/thirdparty/plume" && patch -s -p1 < "$dest.partial/ps5/patches/plume-ps5.patch")
fi
rm -rf "$dest"
mv "$dest.partial" "$dest"
du -sh "$dest" | sed 's/^/  EdgeOfTimeRecomp: /'
