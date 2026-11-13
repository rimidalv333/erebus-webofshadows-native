#!/usr/bin/env bash
# WebOfShadowsRecomp for the PS5 - everything the title builds against, set up once.
#
#   wos/tools/setup.sh           clone what is missing beside this repository, then build
#                                the console's toolchain pieces (a long first run)
#   wos/tools/setup.sh --check   only report what is there and what is not
#
# Beside this repository (its parent folder) it puts:
#
#   rexglue-sdk          KongaTime/rexglue-sdk: ReXGlue with the PS5 target, the runtime
#                        and the emulated Xenos GPU the title links
#   WebOfShadowsRecomp   toowasmir/WebOfShadowsRecomp: the game's PC recompilation, whose
#                        game/ folder takes your default.xex for codegen
#   PS5_PayloadSDK       KongaTime/PS5_PayloadSDK, with mihawk-99's as a second remote:
#                        the runtime's SDK pin is on the first, PS5_Vulkan's on the second
#   PS5_Vulkan, PS5_Mesa mihawk-99's: RADV for the console, the link recipe, the
#                        packaging tools and the console tools (ps5/tools/bootstrap.sh)
#
# then builds: PS5_Vulkan's SDK, native tool, libc.prx, control payload and RADV
# release archive (ps5/tools/bootstrap.sh), this title's SDK (ps5/tools/setup-sdk.sh),
# and the runtime with its GPU for the console (wos/tools/build-rexglue.sh).
#
# Needs: git, curl, xz (it fetches LLVM's release of clang 20.1.8 into ../llvm-20), CMake 3.25+, Ninja, meson, Python 3 with
# numpy, Pillow and mako, glslangValidator, rsync, curl, wget, unzip, zstd, make.
# On Ubuntu 24.04: sudo apt install clang-20 lld-20 llvm-20 cmake ninja-build meson
# python3-numpy python3-pil python3-mako glslang-tools rsync curl wget unzip zstd make
#
# SPDX-License-Identifier: MIT
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(dirname "$root")
check=false
case ${1:-} in
    --check) check=true ;;
    "") ;;
    *) echo "usage: ${0##*/} [--check]" >&2; exit 2 ;;
esac
step() { printf '==> %s\n' "$*"; }
# The console's clang: LLVM's release of 20.1.8 in ../llvm-20
if ! $check; then
    bash "$root/wos/tools/get-llvm20.sh"
fi
# shellcheck source=/dev/null
source "$root/wos/tools/clang20.sh"

clone() {
    local name=$1 url=$2
    if [[ -d $parent/$name/.git ]]; then
        echo "  ok      $name ($(git -C "$parent/$name" rev-parse --short HEAD))"
    elif $check; then
        echo "  missing $name: git clone $url $parent/$name"
    else
        echo "  cloning $name"
        git clone -q "$url" "$parent/$name"
    fi
}

# Release builder: every repository's URL can be overridden (WOS_URL_*), so a release
# can build from its own forks; the defaults are the upstream repositories.
url_rexglue=${WOS_URL_REXGLUE:-https://github.com/KongaTime/rexglue-sdk.git}
url_payloadsdk=${WOS_URL_PAYLOADSDK:-https://github.com/mihawk-99/PS5_PayloadSDK.git}
url_payloadsdk_mihawk=${WOS_URL_PAYLOADSDK_MIHAWK:-https://github.com/mihawk-99/PS5_PayloadSDK.git}
url_vulkan=${WOS_URL_VULKAN:-https://github.com/mihawk-99/PS5_Vulkan.git}
url_mesa=${WOS_URL_MESA:-https://github.com/mihawk-99/PS5_Mesa.git}

step "the repositories beside this one ($parent)"
clone rexglue-sdk "$url_rexglue"
# A pinned ReXGlue revision (WOS_REXGLUE_REVISION): the one this release was built with
if [[ -n ${WOS_REXGLUE_REVISION:-} && -d $parent/rexglue-sdk/.git ]] && ! $check; then
    rexglue_git=(git -C "$parent/rexglue-sdk")
    "${rexglue_git[@]}" cat-file -e "$WOS_REXGLUE_REVISION^{commit}" 2>/dev/null || "${rexglue_git[@]}" fetch -q origin
    if [[ $("${rexglue_git[@]}" rev-parse HEAD) != "$("${rexglue_git[@]}" rev-parse "$WOS_REXGLUE_REVISION")" ]]; then
        "${rexglue_git[@]}" checkout -q "$WOS_REXGLUE_REVISION"
        rm -f "$parent/rexglue-sdk/.wos-patches"
    fi
    echo "  ok      rexglue-sdk at ${WOS_REXGLUE_REVISION:0:7}"
fi
# WebOfShadowsRecomp's sources: the release bundles the part codegen and the title use
if [[ -f $parent/WebOfShadowsRecomp/src/stubs/unregistered_func_handler.h ]]; then
    echo "  ok      WebOfShadowsRecomp (sources in $parent/WebOfShadowsRecomp)"
elif [[ -n ${WOS_URL_WEBOFSHADOWS:-} ]]; then
    clone WebOfShadowsRecomp "$WOS_URL_WEBOFSHADOWS"
else
    echo "  missing WebOfShadowsRecomp: its sources go in $parent/WebOfShadowsRecomp (the release builder puts them there)"
fi
clone PS5_PayloadSDK "$url_payloadsdk"
if [[ -d $parent/PS5_PayloadSDK/.git ]] && ! $check; then
    git -C "$parent/PS5_PayloadSDK" remote get-url mihawk > /dev/null 2>&1 ||
        git -C "$parent/PS5_PayloadSDK" remote add mihawk "$url_payloadsdk_mihawk"
    # both pins (this title's and PS5_Vulkan's): fetched only when missing (a release
    # bundles them, with no network)
    payload_pins=(1de8b37d583de476ad07cde9a0d562b75fb1be8f)
    vulkan_sdk_pin=$(sed -n 's/^sdk_revision=//p' "$parent/PS5_Vulkan/tools/setup-native-dependencies.sh" 2>/dev/null || true)
    [[ -n $vulkan_sdk_pin ]] && payload_pins+=("$vulkan_sdk_pin")
    for pin in "${payload_pins[@]}"; do
        if ! git -C "$parent/PS5_PayloadSDK" cat-file -e "$pin^{commit}" 2>/dev/null; then
            git -C "$parent/PS5_PayloadSDK" fetch -q origin 2>/dev/null || true
            git -C "$parent/PS5_PayloadSDK" fetch -q mihawk 2>/dev/null || true
            git -C "$parent/PS5_PayloadSDK" cat-file -e "$pin^{commit}" 2>/dev/null ||
                echo "  missing PS5_PayloadSDK revision $pin" >&2
        fi
    done
    echo "  ok      PS5_PayloadSDK has both forks' revisions"
fi
clone PS5_Vulkan "$url_vulkan"
# PS5_Vulkan and the RADV it pins at the revisions PS5_EdgeOfTimeTitle's release
# was built with: a title linked with mihawk-99's later revisions (f410e67, RADV
# 1ba9874) is refused by firmware 13.00's loader (EXECFAIL error=12, then the
# console powers off); these are console-checked there
ps5_vulkan_revision=5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5
if [[ -d $parent/PS5_Vulkan/.git ]] && ! $check; then
    vulkan_git=(git -C "$parent/PS5_Vulkan")
    "${vulkan_git[@]}" remote get-url kongatime > /dev/null 2>&1 ||
        "${vulkan_git[@]}" remote add kongatime "$url_vulkan"
    "${vulkan_git[@]}" cat-file -e "$ps5_vulkan_revision^{commit}" 2>/dev/null || "${vulkan_git[@]}" fetch -q kongatime
    if [[ $("${vulkan_git[@]}" rev-parse HEAD) != "$ps5_vulkan_revision" ]]; then
        echo "  PS5_Vulkan: checking out ${ps5_vulkan_revision:0:7} (the revision Edge of Time's release was built with)"
        "${vulkan_git[@]}" checkout -q "$ps5_vulkan_revision"
        # what the other revision built: the native tool, libc.prx and RADV
        rm -rf "$parent/PS5_Vulkan/build" "$parent/PS5_Vulkan/.deps/work/radv-clc-build" \
            "$parent/PS5_Vulkan/.deps/work/radv-build-ps5-release" "$parent/PS5_Vulkan/.deps/native/radv-release"
    fi
    echo "  ok      PS5_Vulkan at ${ps5_vulkan_revision:0:7}"
    # RADV is the console's code too: what another compiler built (or half built)
    # is built again
    radv_work=$parent/PS5_Vulkan/.deps/work/radv-build-ps5-release
    radv_release=$parent/PS5_Vulkan/.deps/native/radv-release
    radv_stamp=$radv_work.wos-clang
    if [[ -d $radv_work || -d $radv_release ]] && ! grep -qxF "$wos_clang_id" "$radv_stamp" 2>/dev/null; then
        echo "  RADV: built with another compiler; building it again with $wos_clang_id"
        rm -rf "$radv_work" "$radv_release"
    fi
    mkdir -p "$(dirname "$radv_stamp")"
    echo "$wos_clang_id" > "$radv_stamp"
fi
if [[ ! -d $parent/PS5_Mesa/.git ]]; then
    if $check; then
        echo "  missing PS5_Mesa: git clone --filter=blob:none $url_mesa $parent/PS5_Mesa"
    else
        echo "  cloning PS5_Mesa (its history only; the build exports the pinned revision)"
        git clone -q --filter=blob:none --no-checkout "$url_mesa" "$parent/PS5_Mesa"
    fi
else
    echo "  ok      PS5_Mesa"
fi

if $check; then
    bash "$root/ps5/tools/bootstrap.sh" --check || true
    for archive in librexruntime.a librexgpu-xenos.a; do
        [[ -f $parent/rexglue-sdk/out/ps5-amd64/$archive ]] && echo "  ok      $archive" ||
            echo "  missing $archive: wos/tools/build-rexglue.sh"
    done
    exit 0
fi

# RADV's host half (mesa_clc) finds LLVM 20's tools and SPIR-V translator through
# llvm-config: Ubuntu keeps them in /usr/lib/llvm-20/bin
[[ -d /usr/lib/llvm-20/bin ]] && export PATH="/usr/lib/llvm-20/bin:$PATH"
[[ -x $HOME/.local/bin/meson ]] && export PATH="$HOME/.local/bin:$PATH"
step "PS5_Vulkan, RADV and this title's SDK (ps5/tools/bootstrap.sh; RADV takes a while)"
# RADV builds two halves: its host tools (mesa_clc) with meson against the system's
# LLVM, and the driver for the console with the payload SDK and LLVM_CONFIG's clang.
# Meson takes LLVM_CONFIG too, and the console's clang 20 has no libraries for it:
# PS5_Vulkan's build-radv.sh builds the host half without it (a local change to that
# checkout, made once)
radv_script=$parent/PS5_Vulkan/tools/build-radv.sh
if ! grep -q 'env -u LLVM_CONFIG "$meson" setup "$clc_build"' "$radv_script"; then
    sed -i -e 's|^    "$meson" setup "$clc_build"|    env -u LLVM_CONFIG "$meson" setup "$clc_build"|' \
        -e 's|^"$ninja" -C "$clc_build"|env -u LLVM_CONFIG "$ninja" -C "$clc_build"|' "$radv_script"
    [[ $(grep -c 'env -u LLVM_CONFIG' "$radv_script") == 2 ]] ||
        { echo "error: could not adapt $radv_script (not the revision this expects)" >&2; exit 1; }
    echo "  ok      PS5_Vulkan's build-radv.sh builds RADV's host tools with the system's LLVM"
fi
# RADV's host tools must use LLVM 20 (its SPIR-V translator, llvm-spirv-20, is 20): meson
# takes the newest llvm-config it finds by name (llvm-config-21 if LLVM 21 is installed
# too) whatever PATH says, so a native file names LLVM 20's
clc_native=$parent/PS5_Vulkan/.deps/work/wos-clc-native.ini
mkdir -p "$(dirname "$clc_native")"
printf "[binaries]\nllvm-config = '%s'\n" "/usr/lib/llvm-20/bin/llvm-config" > "$clc_native"
export WOS_CLC_NATIVE=$clc_native
if ! grep -q 'WOS_CLC_NATIVE' "$radv_script"; then
    sed -i 's|env -u LLVM_CONFIG "$meson" setup "$clc_build" "$source_tree"|env -u LLVM_CONFIG "$meson" setup ${WOS_CLC_NATIVE:+--native-file "$WOS_CLC_NATIVE"} "$clc_build" "$source_tree"|' "$radv_script"
    grep -q 'WOS_CLC_NATIVE' "$radv_script" ||
        { echo "error: could not adapt $radv_script for LLVM 20 (not the revision this expects)" >&2; exit 1; }
fi
# a host-tools build that never finished configuring (e.g. with the wrong LLVM) starts again
clc_build_dir=$parent/PS5_Vulkan/.deps/work/radv-clc-build
[[ -d $clc_build_dir && ! -f $clc_build_dir/build.ninja ]] && rm -rf "$clc_build_dir"
bootstrap_ok=true
bash "$root/ps5/tools/bootstrap.sh" || bootstrap_ok=false
if [[ ! -f $parent/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a ]]; then
    echo "error: RADV did not build (the errors are above, and in $parent/PS5_Vulkan/.deps/work/radv-*.log)" >&2
    exit 1
fi
if ! $bootstrap_ok; then
    echo "error: bootstrap.sh reported something to do (above)" >&2
    exit 1
fi

step "the ReXGlue runtime and its emulated GPU, for the console"
bash "$root/wos/tools/build-rexglue.sh"

cat << EOF

Set up. Next:
  1. Put your game's files (extracted from your own disc image) in
     $parent/WebOfShadowsRecomp/game/, then:   wos/tools/codegen.sh
  2. Build the title:                          ps5/tools/build.sh
     (or the test app first: WOS_TITLE_APP=test ps5/tools/build.sh; WOS_TITLE_APP=game goes back)
  3. The title folder is dist/PPSA99310: add the game's files to its gamedata/
     and copy it to the console's /data/homebrew/.
EOF
