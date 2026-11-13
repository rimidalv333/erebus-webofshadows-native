#!/usr/bin/env bash
# WebOfShadowsRecomp for the PS5 - the clang the console's code is built with.
#
#   wos/tools/get-llvm20.sh     put LLVM's own release of clang 20.1.8 in ../llvm-20
#                               (once; wos/tools/setup.sh runs it)
#
# The title is built with one exact compiler: LLVM's release build of 20.1.8,
# from github.com/llvm/llvm-project, not a distribution's. Ubuntu 26.04's
# clang-20 (20.1.8-2ubuntu8) crashes compiling RADV and the runtime (exit
# code 139), and a newer clang makes a title firmware 13.00 refuses.
#
# The release is 2 GB; it is streamed and only the tools the build drives are
# kept (about 650 MB): clang, lld, llvm-config, llvm-ar/nm/objcopy/strip/readobj
# and clang's own headers. Needs curl, tar and xz.
#
# SPDX-License-Identifier: MIT
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(dirname "$root")
dest=${WOS_LLVM20:-$parent/llvm-20}
version=20.1.8
name=LLVM-$version-Linux-X64
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name.tar.xz
sha256=1ead36b3dfcb774b57be530df42bec70ab2d239fbce9889447c7a29a4ddc1ae6

# LLVM's lld wants libxml2.so.2 (for Windows manifests only), which Ubuntu 26.04
# no longer has (its libxml2 is .so.16). There, the system's lld 20 links instead:
# the linker only places the code clang made.
use_working_lld() {
    local dir=$1
    "$dir/bin/lld" -flavor gnu --version > /dev/null 2>&1 && return 0
    local system_lld
    system_lld=$(command -v /usr/lib/llvm-20/bin/lld || command -v ld.lld-20 || true)
    if [[ -z $system_lld ]] || ! "$system_lld" -flavor gnu --version > /dev/null 2>&1; then
        echo "error: LLVM's lld does not run here (it needs libxml2.so.2), and there is no lld 20 to use instead:" >&2
        echo "       sudo apt install lld-20" >&2
        return 1
    fi
    rm -f "$dir/bin/lld"
    ln -s "$(readlink -f "$system_lld")" "$dir/bin/lld"
    echo "  ok      LLVM's lld needs libxml2.so.2, which this system has not: linking with $("$dir/bin/ld.lld" --version | head -1)"
}

if [[ -x $dest/bin/clang-20 && -f $dest/.wos-llvm && -e $dest/lib/clang/20/lib/linux/libclang_rt.builtins-x86_64.a ]] &&
        [[ $(cat "$dest/.wos-llvm") == "$sha256" ]]; then
    use_working_lld "$dest" || exit 1
    echo "  ok      LLVM $version ($dest)"
    exit 0
fi
for tool in curl tar xz sha256sum; do
    command -v "$tool" > /dev/null || { echo "error: $tool is needed (sudo apt install curl tar xz-utils coreutils)" >&2; exit 1; }
done

echo "==> LLVM $version for the console, from llvm.org's release (a 2 GB download; about 650 MB is kept in $dest)"
tmp=$dest.partial
rm -rf "$tmp"
mkdir -p "$tmp"
sum_file=$(mktemp)
trap 'rm -f "$sum_file"' EXIT
members=()
for tool in clang-20 clang clang++ clang-cpp lld ld.lld llvm-config llvm-ar llvm-ranlib llvm-nm \
        llvm-objcopy llvm-strip llvm-readobj llvm-readelf; do
    members+=("$name/bin/$tool")
done
curl -fL --retry 3 --progress-bar "$url" |
    tee >(sha256sum | cut -d' ' -f1 > "$sum_file") |
    tar -xJ -C "$tmp" --strip-components=1 --wildcards "${members[@]}" "$name/lib/clang/*"
sleep 1 # the checksum's subshell finishing
got=$(cat "$sum_file")
if [[ $got != "$sha256" ]]; then
    rm -rf "$tmp"
    echo "error: the download's checksum is $got, not $sha256; run this again" >&2
    exit 1
fi
# PS5_Vulkan's link recipe takes clang's builtins (for __emutls_get_address) where
# Ubuntu's clang keeps them, lib/linux/; LLVM's release has them per target
builtins=$tmp/lib/clang/20/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a
[[ -f $builtins ]] || { echo "error: the download has no $builtins" >&2; exit 1; }
mkdir -p "$tmp/lib/clang/20/lib/linux"
ln -s ../x86_64-unknown-linux-gnu/libclang_rt.builtins.a "$tmp/lib/clang/20/lib/linux/libclang_rt.builtins-x86_64.a"
"$tmp/bin/clang-20" --version | grep -q "clang version $version" || { echo "error: the clang in the download does not run" >&2; exit 1; }
use_working_lld "$tmp" || exit 1
echo "$sha256" > "$tmp/.wos-llvm"
rm -rf "$dest"
mv "$tmp" "$dest"
echo "  ok      LLVM $version in $dest ($(du -sh "$dest" | cut -f1))"
