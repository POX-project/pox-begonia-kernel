#!/usr/bin/env bash
# Source this file to set up the kernel build environment
KERDEVDEP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export PATH="$KERDEVDEP_DIR/bin:$KERDEVDEP_DIR/clang/bin:$PATH"
export LD_LIBRARY_PATH="$KERDEVDEP_DIR/lib:$KERDEVDEP_DIR/lib/x86_64-linux-gnu:$KERDEVDEP_DIR/usr/lib:$KERDEVDEP_DIR/usr/lib/x86_64-linux-gnu:$KERDEVDEP_DIR/clang/lib:${LD_LIBRARY_PATH:-}"
if [[ -d "$KERDEVDEP_DIR/usr/share/bison" ]]; then
    export BISON_PKGDATADIR="$KERDEVDEP_DIR/usr/share/bison"
    export M4="$KERDEVDEP_DIR/bin/m4"
fi

# Default cross-compilation variables (TRB Clang bundles its own aarch64/arm binutils)
export ARCH=arm64
export CC=clang
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-TXO_R}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-PoxKernel}"
