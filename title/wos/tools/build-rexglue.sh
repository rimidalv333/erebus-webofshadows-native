#!/usr/bin/env bash
# WebOfShadowsRecomp on the PS5 - cross-build the ReXGlue SDK's runtime and its
# emulated Xenos GPU for the console.
#
#   wos/tools/build-rexglue.sh              install the payload SDK (once), configure (once),
#                                           build rexruntime and rexgpu-xenos, then check
#                                           their imports
#   wos/tools/build-rexglue.sh --configure  configure again from scratch
#
# EdgeOfTimeRecomp's ps5/tools/build-rexglue.sh (KongaTime, MIT), with the emulated
# GPU built as well: Edge of Time draws with a renderer of its own, Web of Shadows
# with the runtime's GPU emulation, which a title links in (librexgpu-xenos.a).
#
# Checked out beside this repository (or named by the variables):
#
#   ../PS5_PayloadSDK   KongaTime/PS5_PayloadSDK, holding the pinned revision below
#                       (PS5_PAYLOAD_SDK_FORK)
#   ../rexglue-sdk      KongaTime/rexglue-sdk, the ReXGlue fork with the PS5 target, at
#                       the pinned revision or later (REXGLUE_SRC)
#
# The SDK goes to this repository's .deps/native/ps5-payload-sdk (the title's own
# pin, ps5/tools/setup-sdk.sh), the build to ../rexglue-ps5-build (REXGLUE_PS5_BUILD),
# the libraries to ../rexglue-sdk/out/ps5-amd64. The last step lists every symbol the
# libraries need that neither they nor the modules a title links by default provide:
# anything there besides what the linker defines would stop the title's link.
#
# Needs clang 20 or later (llvm-config-20), CMake 3.25+, Ninja, wget, unzip, make.
#
# Copyright (C) 2026 KongaTime
# SPDX-License-Identifier: MIT
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(dirname "$root")
sdk_fork=${PS5_PAYLOAD_SDK_FORK:-$parent/PS5_PayloadSDK}
rexglue=${REXGLUE_SRC:-$parent/rexglue-sdk}
cache=$root/.deps/native
sdk=$cache/ps5-payload-sdk
build=${REXGLUE_PS5_BUILD:-$parent/rexglue-ps5-build}

# The payload SDK fork at the revision PS5_RPCS3Title pins (1de8b37: pathconf, sbrk)
sdk_revision=1de8b37d583de476ad07cde9a0d562b75fb1be8f
# KongaTime/rexglue-sdk main: ReXGlue nightly-20261002-bd833a2a with the PS5 target and
# the emulated GPU linked into a title (0e77ed8) and presenting on the display
# (4522a48, 33e9706), as this title was first built against
rexglue_revision=f47ccdde34b3a992f7872a27619fc588040c229f

# The submodules the runtime builds from (thirdparty/CMakeLists.txt, Vulkan on, Tracy off)
submodules=(cli11 libmspack FFmpeg tomlplusplus simde xxHash spdlog fmt utfcpp imgui sdl3 o1heap
    vulkan-headers vulkan-memory-allocator spirv-headers spirv-tools glslang)

configure=false
for argument in "$@"; do
    case $argument in
        --configure) configure=true ;;
        *) echo "usage: ${0##*/} [--configure]" >&2; exit 2 ;;
    esac
done

step() { printf '==> %s\n' "$*"; }
# shellcheck source=/dev/null
source "$root/wos/tools/clang20.sh"
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

# The payload SDK, at its pin
if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    git -C "$sdk_fork" cat-file -e "$sdk_revision^{commit}" 2>/dev/null ||
        fail "no PS5_PayloadSDK checkout with $sdk_revision at $sdk_fork (set PS5_PAYLOAD_SDK_FORK)"
    step "installing the payload SDK at ${sdk_revision:0:7}"
    mkdir -p "$cache"
    sdk_tree=$(mktemp -d)
    trap 'rm -rf -- "$sdk_tree"' EXIT
    git -C "$sdk_fork" archive "$sdk_revision" | tar -x -C "$sdk_tree"
    bash "$sdk_tree/platform/tools/setup-sdk.sh" "$sdk" "$sdk_revision" "$cache"
fi
clang_major=$("$sdk/bin/prospero-llvm-config" --version | cut -d. -f1)
(( clang_major == 20 )) || fail "the SDK found clang $clang_major; the runtime is built with clang 20 (install llvm-20)"
# A build folder another clang configured is configured again (its objects are
# that compiler's)
if [[ -f $build/configure.log ]] && ! grep -qxF "$wos_clang_id" "$build/.wos-clang" 2>/dev/null; then
    configure=true
fi

# The ReXGlue fork
[[ -f $rexglue/CMakeLists.txt ]] ||
    fail "no ReXGlue fork at $rexglue (git clone https://github.com/KongaTime/rexglue-sdk, or set REXGLUE_SRC)"
git -C "$rexglue" merge-base --is-ancestor "$rexglue_revision" HEAD 2>/dev/null ||
    fail "$rexglue does not contain the pinned fork revision ${rexglue_revision:0:7} (pull KongaTime/rexglue-sdk)"
# This title's fixes to the fork (wos/patches/rexglue), applied to its working tree
# once and in order; .wos-patches in the fork lists those applied (a patch applied
# before that list existed is recognised by reversing it)
applied_list=$rexglue/.wos-patches
# When the patches change (a new release), the fork is put back to its pinned revision
# first and patched again (thirdparty/, the bundled sources, is left alone)
patch_sum=$(cat "$root"/wos/patches/rexglue/*.patch 2>/dev/null | sha256sum | cut -c1-64)
if [[ -f $applied_list && $(cat "$rexglue/.wos-patches-sum" 2>/dev/null) != "$patch_sum" ]]; then
    git -C "$rexglue" checkout -q "$rexglue_revision" -- . ':!thirdparty'
    git -C "$rexglue" clean -fdq -e thirdparty/ -- . ':!thirdparty'
    git -C "$rexglue" checkout -q "$rexglue_revision" -- thirdparty/CMakeLists.txt 2>/dev/null || true
    rm -f "$applied_list"
fi
for patch in "$root"/wos/patches/rexglue/*.patch; do
    [[ -f $patch ]] || continue
    name=$(basename "$patch")
    grep -qxF "$name" "$applied_list" 2>/dev/null && continue
    if ! git -C "$rexglue" apply --reverse --check "$patch" 2>/dev/null; then
        git -C "$rexglue" apply "$patch" ||
            fail "could not apply $name to $rexglue (is it at ${rexglue_revision:0:7}?)"
        patched=1
    fi
    echo "$name" >> "$applied_list"
done
[[ ${patched:-0} == 1 ]] && step "applied this title's changes to the ReXGlue fork"
printf '%s\n' "$patch_sum" > "$rexglue/.wos-patches-sum"
for submodule in "${submodules[@]}"; do
    # a release bundles the sources (no .git): those are used as they are
    if [[ ! -e $rexglue/thirdparty/$submodule/.git ]] &&
            [[ -z $(ls -A "$rexglue/thirdparty/$submodule" 2>/dev/null) ]]; then
        step "fetching ReXGlue's $submodule"
        GIT_LFS_SKIP_SMUDGE=1 git -C "$rexglue" submodule update --init --depth 1 "thirdparty/$submodule"
    fi
    # ReXGlue's CMake takes a submodule without .git for one never fetched: bundled
    # sources get an empty repository of their own so it accepts them
    [[ -e $rexglue/thirdparty/$submodule/.git ]] || git init -q "$rexglue/thirdparty/$submodule"
done

# Configure and build
mkdir -p "$build"
if $configure || [[ ! -f $build/build.ninja ]]; then
    rm -rf "$build"
    mkdir -p "$build"
    echo "$wos_clang_id" > "$build/.wos-clang"
    flags="-march=znver2 -fno-omit-frame-pointer"
    step "configuring ReXGlue at $(git -C "$rexglue" rev-parse --short HEAD) (clang $clang_major)"
    cmake -S "$rexglue" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF \
        >"$build/configure.log" 2>&1 || {
        grep -n "CMake Error" -A6 "$build/configure.log" >&2
        fail "configure failed: $build/configure.log"; }
fi
step "building rexruntime and rexgpu-xenos (the emulated GPU)"
ninja -C "$build" -k 0 rexruntime rexgpu-xenos >"$build/build.log" 2>&1 ||
    fail "the build failed: $build/build.log ($(grep -c 'error:' "$build/build.log") errors)"
[[ -f $rexglue/out/ps5-amd64/librexgpu-xenos.a ]] ||
    fail "no librexgpu-xenos.a in $rexglue/out/ps5-amd64: the fork is older than 0e77ed8 (pull KongaTime/rexglue-sdk)"

# What the libraries import that nothing provides. rex::ui::GetWindowedAppCreator
# is the app's (REX_DEFINE_APP), which the PS5 entry, rex_ps5_app_main, calls.
step "checking the runtime's imports"
nm=$sdk/bin/prospero-nm
out=$rexglue/out/ps5-amd64
work=$(mktemp -d)
trap 'rm -rf -- "${sdk_tree:-}" "$work"' EXIT
for archive in "$out"/*.a; do
    "$nm" -g --defined-only "$archive" 2>/dev/null | awk 'NF >= 3 { print $3 }'
done | sort -u >"$work/defined"
for archive in "$out"/*.a; do
    "$nm" -g --undefined-only "$archive" 2>/dev/null | awk 'NF && $NF !~ /:$/ { print $NF }'
done | sort -u >"$work/undefined"
{
    # The modules a title links by default (the SDK's compiler wrapper), the
    # system modules the runtime's PS5 drivers call (sound, the pad, the user
    # service: a title's link takes every SDK module it needs), and its static
    # libraries
    for module in libkernel_web libSceLibcInternal libc_stub_weak libkernel_stub_weak libSceNet \
            libSceAudioOut libScePad libSceUserService libSceSystemService; do
        "$nm" -D --defined-only "$sdk/target/lib/$module.so" | awk '{ print $NF }'
    done
    # (not libc.a: a title takes its C library from the console's modules and
    # the platform layer, so ReXGlue's ffs failed the title's link while libc.a
    # had it)
    for library in libc++.a libc++abi.a libc++experimental.a libunwind.a libpthread.a libps5platform.a; do
        "$nm" -g --defined-only "$sdk/target/lib/$library" 2>/dev/null | awk 'NF >= 3 { print $3 }'
    done
} | sort -u >"$work/console"
# __emutls_get_address is Clang's builtins', which a title's link recipe takes;
# __real_abort is what a title's --wrap=abort gives the fault records'
# __wrap_abort (the fork's src/core/abort_prospero.cpp); vk_icdGetInstanceProcAddr
# and wsi_videoout_set_flip_rate are RADV's, which every title links (the fork's
# GPU in the title, librexgpu-xenos, calls them)
comm -23 "$work/undefined" "$work/defined" | comm -23 - "$work/console" |
    grep -vxE '_GLOBAL_OFFSET_TABLE_|__dso_handle|__emutls_get_address|__real_abort|_ZN3rex2ui21GetWindowedAppCreatorEv|vk_icdGetInstanceProcAddr|wsi_videoout_set_flip_rate' \
    >"$work/unprovided" || true
# What the platform layer has as ps5_<name>: a title binds those (--defsym), in
# PS5_Vulkan's recipe or its own list (wos/wos.cmake);
# link-title.sh refuses a title that leaves one unbound
"$nm" -g --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null |
    awk 'NF >= 3 && $3 ~ /^ps5_/ { print substr($3, 5) }' | sort -u >"$work/bindable"
comm -12 "$work/unprovided" "$work/bindable" >"$work/bind"
comm -23 "$work/unprovided" "$work/bindable" >"$work/missing"
if [[ -s $work/bind ]]; then
    echo "imported, and bound by a title to the platform layer's ps5_ functions:" $(cat "$work/bind")
fi
if [[ -s $work/missing ]]; then
    echo "imported, and provided by nothing a title links:" >&2
    sed 's/^/  /' "$work/missing" >&2
    fail "$(wc -l <"$work/missing") unresolved symbols"
fi
step "rexruntime and rexgpu-xenos built for the PS5 ($out), every import resolved"
