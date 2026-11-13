# WebOfShadowsRecomp for the PS5 - build for the console with clang 20 (sourced).
#
# The payload SDK compiles with the newest LLVM it finds (its prospero-llvm-config
# tries llvm-config-21 first). A title whose runtime and code clang 21 compiled
# came out with twice the code and was refused by firmware 13.00's loader
# (EXECFAIL error=12, then the console powers off); the same sources compiled
# with clang 20 run. LLVM_CONFIG chooses the SDK's LLVM, PS5_CLANG the clang
# PS5_Vulkan's link recipe drives.
#
# The clang 20 is LLVM's own release of 20.1.8 in ../llvm-20 (wos/tools/get-llvm20.sh):
# Ubuntu 26.04's clang-20 package crashes on RADV and the runtime.
# WOS_SYSTEM_CLANG20=1 uses the system's clang-20 instead.
#
# Sets wos_clang_id, which the build folders are stamped with: one built by
# another compiler is built again.
#
# SPDX-License-Identifier: MIT
wos_llvm20=${WOS_LLVM20:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)/llvm-20}
if [[ ${WOS_SYSTEM_CLANG20:-0} == 1 ]]; then
    wos_llvm_config=$(command -v llvm-config-20 || true)
    wos_clang=$(command -v clang-20 || true)
    if [[ -z $wos_llvm_config || -z $wos_clang ]]; then
        echo "WOS_SYSTEM_CLANG20=1, but there is no clang-20 (sudo apt install clang-20 lld-20 llvm-20)" >&2
        exit 2
    fi
elif [[ -x $wos_llvm20/bin/clang-20 && -x $wos_llvm20/bin/llvm-config ]]; then
    wos_llvm_config=$wos_llvm20/bin/llvm-config
    wos_clang=$wos_llvm20/bin/clang-20
else
    echo "clang 20 for the console is missing ($wos_llvm20): run wos/tools/get-llvm20.sh (or wos/tools/setup.sh)" >&2
    exit 2
fi
wos_clang_id=$("$wos_clang" --version | head -1)
export LLVM_CONFIG=$wos_llvm_config
export PS5_CLANG=$wos_clang
