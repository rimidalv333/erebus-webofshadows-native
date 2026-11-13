#!/usr/bin/env bash
# WebOfShadowsRecomp for the PS5 - recompile the player's own default.xex into C++.
#
#   wos/tools/codegen.sh            build the rexglue tool for this machine (once),
#                                   then run its codegen on WebOfShadowsRecomp
#   wos/tools/codegen.sh --rebuild  build the rexglue tool again first
#
# Needs, beside this repository (or named by the variables):
#
#   ../WebOfShadowsRecomp   toowasmir/WebOfShadowsRecomp (WEBOFSHADOWSRECOMP_SRC), with the
#                           game's default.xex in its game/ folder: from your own copy of
#                           Spider-Man: Web of Shadows for the Xbox 360 (USA, title ID
#                           41560815), extracted from the disc image
#   ../rexglue-sdk          KongaTime/rexglue-sdk (REXGLUE_SRC), whose codegen template
#                           knows the PS5
#
# The rexglue tool is built for this machine (not the console) in
# ../rexglue-host-build (REXGLUE_HOST_BUILD), into rexglue-sdk/out/linux-amd64/;
# REXGLUE_CLI names one to use instead.
# The generated code goes to WebOfShadowsRecomp's generated/default, where the
# title's build (wos/wos_game.cmake) takes it. It is made from your game's code:
# keep it to yourself, as WebOfShadowsRecomp's .gitignore keeps the game's files.
#
# WOS_JOBS limits the compilers run at once (for a WSL short of memory).
#
# Needs clang 20 or later, CMake 3.25+ and Ninja.
#
# SPDX-License-Identifier: MIT
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(dirname "$root")
wos=${WEBOFSHADOWSRECOMP_SRC:-$parent/WebOfShadowsRecomp}
rexglue=${REXGLUE_SRC:-$parent/rexglue-sdk}
host_build=${REXGLUE_HOST_BUILD:-$parent/rexglue-host-build}
rebuild=false
for argument in "$@"; do
    case $argument in
        --rebuild) rebuild=true ;;
        *) echo "usage: ${0##*/} [--rebuild]" >&2; exit 2 ;;
    esac
done

step() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -f $wos/src/stubs/unregistered_func_handler.h ]] ||
    fail "no WebOfShadowsRecomp checkout at $wos (git clone https://github.com/toowasmir/WebOfShadowsRecomp, or set WEBOFSHADOWSRECOMP_SRC)"
[[ -f $rexglue/CMakeLists.txt ]] ||
    fail "no ReXGlue fork at $rexglue (git clone https://github.com/KongaTime/rexglue-sdk, or set REXGLUE_SRC)"

# The game's executable, in whatever case the disc image gave it
xex=$(find "$wos/game" -maxdepth 1 -type f -iname default.xex -print -quit 2>/dev/null || true)
[[ -n $xex ]] || fail "no default.xex in $wos/game: extract your Xbox 360 disc image there (extract-xiso, or the PC release's launcher)"
xex_name=$(basename "$xex")
step "the game's executable: $xex ($(stat -c %s "$xex") bytes)"

# The rexglue tool, for this machine
cli=${REXGLUE_CLI:-}
if [[ -z $cli ]]; then
    # The SDK writes its binaries to its own out/<platform>/, not the build folder
    cli=$rexglue/out/linux-amd64/rexglue
    if $rebuild || [[ ! -x $cli ]]; then
        # This tool runs on the PC only (its output is C++ text), so any clang 20 or
        # later builds it: the newest first. WOS_HOST_CC/WOS_HOST_CXX name one.
        # (Ubuntu's clang 20.1.8 crashes on the runtime's precompiled header.)
        # After clang-21: the console's clang (LLVM's release, wos/tools/get-llvm20.sh)
        llvm20=${WOS_LLVM20:-$parent/llvm-20}/bin
        cc=${WOS_HOST_CC:-$(command -v clang-21 || command -v "$llvm20/clang" || command -v clang-20 || command -v clang || true)}
        cxx=${WOS_HOST_CXX:-$(command -v clang++-21 || command -v "$llvm20/clang++" || command -v clang++-20 || command -v clang++ || true)}
        [[ -n $cc && -n $cxx ]] || fail "no clang for this machine (install clang 20 or 21)"
        major=$("$cc" --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
        (( major >= 20 )) || fail "$cc is clang $major; the rexglue tool needs 20 or later (install clang-20)"
        step "configuring the rexglue tool for this machine in $host_build"
        # A build folder configured with another compiler starts again
        if [[ -f $host_build/CMakeCache.txt ]] &&
                ! grep -qx "CMAKE_CXX_COMPILER:STRING=$cxx" "$host_build/CMakeCache.txt" &&
                ! grep -qx "CMAKE_CXX_COMPILER:FILEPATH=$cxx" "$host_build/CMakeCache.txt"; then
            rm -rf "$host_build"
        fi
        mkdir -p "$host_build"
        cmake -S "$rexglue" -B "$host_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
            -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
            -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF >"$host_build/configure.log" 2>&1 ||
            fail "configure failed: $host_build/configure.log"
        step "building the rexglue tool with $(basename "$cxx") (WOS_JOBS=${WOS_JOBS:-$(nproc)} at once)"
        ninja -C "$host_build" -j "${WOS_JOBS:-$(nproc)}" rexglue >"$host_build/build.log" 2>&1 ||
            fail "the build failed: $host_build/build.log ($(grep -c 'error:' "$host_build/build.log") errors)"
    fi
fi
[[ -x $cli ]] || fail "no rexglue tool at $cli"

# The manifest, in the checkout, naming the executable as the disc image named it
manifest=$wos/wos_recomp_manifest.toml
sed "s#^file_path = \"game/default.xex\"#file_path = \"game/$xex_name\"#" \
    "$root/wos/codegen/wos_recomp_manifest.toml" >"$manifest"
# fix81: WOS_CODEGEN_FAST=1 - the recompiler keeps the PowerPC condition, count, carry and
# reservation registers in local variables instead of in memory (rexglue's cr_as_local,
# ctr_as_local, xer_as_local, reserved_as_local). WOS_CODEGEN_FAST=2 also keeps the other
# non-argument registers local (non_argument_as_local) - BROKEN on WoS (fix81 test: black screen,
# thousands of null indirect calls; the game passes values in r0/r11/r12 across calls). Unset = the code as before.
# WOS_CODEGEN_OPTS="name name ...": any of the recompiler's options by name, each set true
# (cr_as_local ctr_as_local xer_as_local reserved_as_local non_volatile_as_local skip_lr
# skip_msr non_argument_as_local; the last one needs nonarg-fix.py, run below). Added to WOS_CODEGEN_FAST's.
codegen_opts=${WOS_CODEGEN_OPTS:-}
case ${WOS_CODEGEN_FAST:-0} in
    0) ;;
    1) codegen_opts="cr_as_local ctr_as_local xer_as_local reserved_as_local $codegen_opts" ;;
    2) codegen_opts="cr_as_local ctr_as_local xer_as_local reserved_as_local non_argument_as_local $codegen_opts" ;;
    *) fail "WOS_CODEGEN_FAST must be 0, 1 or 2" ;;
esac
if [[ -n ${codegen_opts// /} ]]; then
    lines=
    for opt in $(printf '%s\n' $codegen_opts | sort -u); do
        case $opt in
            cr_as_local|ctr_as_local|xer_as_local|reserved_as_local|non_argument_as_local|non_volatile_as_local|skip_lr|skip_msr) ;;
            *) fail "unknown recompiler option: $opt" ;;
        esac
        lines="$lines\\n$opt = true"
    done
    sed -i "s#^includes = \[\]#includes = []$lines#" "$manifest"
    step "recompiler options: $(grep -E '^(skip_|[a-z_]+_as_local) = true' "$manifest" | sed 's/ = true//' | tr '\n' ' ')"
fi

step "recompiling the game's code (this takes a while)"
log=$wos/generated/codegen.log
mkdir -p "$wos/generated"
(cd "$wos" && "$cli" --force --log-file "$log" codegen wos_recomp_manifest.toml) ||
    fail "codegen failed: the end of $log says why"

out=$wos/generated/default
if grep -q '^non_argument_as_local = true' "$manifest"; then
    # rexglue's non_argument_as_local loses values the game passes between functions in
    # r11/r12 (SEH blocks, split functions): carry those through ctx (nonarg-fix.py)
    python3 "$(dirname -- "${BASH_SOURCE[0]}")/nonarg-fix.py" "$out" ||
        fail "the non_argument_as_local fix failed"
fi
[[ -f $out/sources.cmake && -f $out/wos_recomp_init.h && -f $out/wos_recomp_pch.h ]] ||
    fail "codegen ran but $out is incomplete (no sources.cmake, wos_recomp_init.h or wos_recomp_pch.h)"
grep -q "REX_PLATFORM_PS5" "$out/wos_recomp_pch.h" ||
    echo "note: $out/wos_recomp_pch.h has no PS5 memory offset; the title's build adds it" >&2
sources=$(find "$out" -name '*.cpp' | wc -l)
functions=$(grep -ho "DEFINE_REX_FUNC(" "$out"/*.cpp 2>/dev/null | wc -l || true)
warnings=$(grep -ci "warn" "$log" 2>/dev/null || true)
step "done: $sources source files, $functions functions, in $out ($warnings warnings in $log)"
