#!/usr/bin/env bash
#
# build.sh - Portable Pox Kernel builder & AnyKernel3 flashable zip packager
# For Redmi Note 8 Pro (begonia, MT6785)
#
# Dependencies are fully self-contained inside ./kerdevdep
# Builds and flashable zips are placed in ./build
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/out}"
KERDEVDEP="${KERDEVDEP:-$ROOT_DIR/kerdevdep}"

# Kernel Branding & Versioning (Fully customizable via environment or build script)
KERNEL_NAME="${KERNEL_NAME:-Pox}"
KERNEL_VERSION="${KERNEL_VERSION:-0.9}"
DEVICE_NAME="${DEVICE_NAME:-Redmi Note 8 Pro}"
DEVICE_CODENAME="${DEVICE_CODENAME:-begonia}"
MAINTAINER="${MAINTAINER:-TXO R (Pox Project)}"
DEFCONFIG="${DEFCONFIG:-begonia_apatch_defconfig}"
JOBS="${JOBS:-$(nproc)}"
EXTRA_FLAGS="${EXTRA_FLAGS:-}"
DATE="$(date +%Y%m%d-%H%M)"

# Git Metadata: Branch & Commit ID
GIT_BRANCH="${GIT_BRANCH:-$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")}"
COMMIT_HASH="${COMMIT_HASH:-$(git rev-parse --short=7 HEAD 2>/dev/null || echo "custom")}"
COMMIT_DATE="$(git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M' 2>/dev/null || date +'%Y-%m-%d %H:%M')"
COMMIT_SUBJECT="$(git log -1 --format=%s 2>/dev/null || echo "Release build")"

# Version Name / Codename (Rocks theme: Granite, Obsidian, Onyx)
if [[ -z "${VERSION_NAME:-}" ]]; then
    case "$GIT_BRANCH" in
        main|granite)
            VERSION_NAME="Granite"
            BRANCH_DESC="Rock-Solid Stability Edition"
            ;;
        memory-enhanced|obsidian)
            VERSION_NAME="Obsidian"
            BRANCH_DESC="iOS-Style Compressed Memory Edition"
            ;;
        gaming|onyx|*)
            VERSION_NAME="Onyx"
            BRANCH_DESC="Zero Frame-Drop Gaming Edition"
            ;;
    esac
else
    BRANCH_DESC="${BRANCH_DESC:-Custom Edition}"
fi
BRANCH_CODENAME="$VERSION_NAME"

# Derive dynamic localversion string: name, version, edition, commit id
if [[ -n "${LOCALVERSION:-}" ]]; then
    CUSTOM_LOCALVERSION="$LOCALVERSION"
else
    CUSTOM_LOCALVERSION="-${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${COMMIT_HASH}"
fi

# Package zip base name: name, version, edition, commit id, device
if [[ -n "${PACKAGE_NAME:-}" ]]; then
    ZIP_BASE="$PACKAGE_NAME"
elif [[ "$KERNEL_NAME" == *"$DEVICE_CODENAME"* ]]; then
    ZIP_BASE="${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${COMMIT_HASH}"
else
    ZIP_BASE="${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${COMMIT_HASH}-${DEVICE_CODENAME}"
fi

log()  { printf '\033[1;32m[*] %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[!] %s\033[0m\n' "$*"; }
err()  { printf '\033[1;31m[-] %s\033[0m\n' "$*"; }

# 1. Ensure kerdevdep is bootstrapped
if [[ ! -f "$KERDEVDEP/env.sh" || ! -x "$KERDEVDEP/clang/bin/clang" ]]; then
    log "Bootstrapping self-contained dependencies in $KERDEVDEP ..."
    bash "$KERDEVDEP/setup_kerdevdep.sh"
fi

# 2. Source kerdevdep environment
# shellcheck source=/dev/null
source "$KERDEVDEP/env.sh"

ARCH=arm64
CC=clang
CLANG_TRIPLE=aarch64-linux-gnu-
CROSS_COMPILE=aarch64-linux-android-
AK3_DIR="$KERDEVDEP/anykernel"

export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-TXO_R}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-${KERNEL_NAME}Kernel}"

ACTION="${1:-all}"

clean_build() {
    log "Cleaning build outputs in $OUT_DIR ..."
    rm -rf "$OUT_DIR"
    log "Clean complete."
}

distclean_build() {
    log "Removing entire build directory $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    log "Distclean complete."
}

prepare_config() {
    local test_src="$BUILD_DIR/.tc-test.c"
    mkdir -p "$BUILD_DIR"
    printf 'int x;\n' > "$test_src"

    if [[ "${KEEP_CUSTOM_FLAGS:-0}" == "1" ]]; then
        log "KEEP_CUSTOM_FLAGS=1 - keeping custom -mllvm flags"
        rm -f "$test_src"
        return
    fi

    if ! "$KERDEVDEP/clang/bin/clang" --target=aarch64-linux-gnu \
        -mllvm -polly -mllvm -polly-postopts=1 -mllvm -polly-ast-use-context \
        -mllvm -polly-detect-keep-going -mllvm -polly-vectorizer=stripmine \
        -mllvm -polly-invariant-load-hoisting -c "$test_src" -o /dev/null 2>/dev/null; then
        log "Toolchain lacks patched LLVM Polly - disabling CONFIG_LLVM_POLLY"
        ./scripts/config --file "$OUT_DIR/.config" --disable LLVM_POLLY
    fi

    if ! "$KERDEVDEP/clang/bin/clang" --target=aarch64-linux-gnu \
        -mllvm -unroll-threshold=1200 -mllvm -unroll-threshold=900 \
        -mllvm -inline-threshold=2000 -mllvm -inline-threshold=1300 \
        -c "$test_src" -o /dev/null 2>/dev/null; then
        log "Toolchain rejects repeated -mllvm thresholds - disabling CONFIG_INLINE_OPTIMIZATION"
        ./scripts/config --file "$OUT_DIR/.config" --disable INLINE_OPTIMIZATION
    fi

    # Dynamically apply LOCALVERSION based on KERNEL_NAME, KERNEL_VERSION, VERSION_NAME, GIT_BRANCH, COMMIT_HASH
    log "Setting CONFIG_LOCALVERSION=\"$CUSTOM_LOCALVERSION\" in .config"
    ./scripts/config --file "$OUT_DIR/.config" --set-str LOCALVERSION "$CUSTOM_LOCALVERSION"

    # shellcheck disable=SC2086
    make O="$OUT_DIR" ARCH="$ARCH" CC="$CC" \
        CLANG_TRIPLE="$CLANG_TRIPLE" CROSS_COMPILE="$CROSS_COMPILE" \
        $EXTRA_FLAGS olddefconfig
    rm -f "$test_src"
}

run_menuconfig() {
    mkdir -p "$OUT_DIR"
    if [[ ! -f "$OUT_DIR/.config" ]]; then
        log "Generating defconfig ($DEFCONFIG) ..."
        # shellcheck disable=SC2086
        make O="$OUT_DIR" ARCH="$ARCH" CC="$CC" \
            CLANG_TRIPLE="$CLANG_TRIPLE" CROSS_COMPILE="$CROSS_COMPILE" \
            $EXTRA_FLAGS "$DEFCONFIG"
        prepare_config
    fi
    make O="$OUT_DIR" ARCH="$ARCH" CC="$CC" \
        CLANG_TRIPLE="$CLANG_TRIPLE" CROSS_COMPILE="$CROSS_COMPILE" \
        menuconfig
}

build_kernel() {
    log "================================================="
    log "Building $KERNEL_NAME"
    log "Version:      $KERNEL_VERSION"
    log "Version Name: $VERSION_NAME ($BRANCH_DESC)"
    log "Branch:       $GIT_BRANCH"
    log "Commit ID:    $COMMIT_HASH"
    log "Device:       $DEVICE_NAME ($DEVICE_CODENAME)"
    log "Maintainer:   $MAINTAINER"
    log "Localversion: $CUSTOM_LOCALVERSION"
    log "Defconfig:    $DEFCONFIG"
    log "Output Dir:   $BUILD_DIR"
    log "Object Dir:   $OUT_DIR"
    log "Jobs:         $JOBS"
    log "Toolchain:    $KERDEVDEP"
    log "================================================="

    local bcc="$CC"
    if command -v ccache >/dev/null 2>&1 && [[ "${CCACHE:-1}" == "1" ]]; then
        bcc="ccache $CC"
        export CCACHE_DIR="${CCACHE_DIR:-$BUILD_DIR/.ccache}"
        mkdir -p "$CCACHE_DIR"
        log "Using ccache (cache dir: $CCACHE_DIR)"
    fi

    mkdir -p "$OUT_DIR" "$BUILD_DIR"
    cd "$ROOT_DIR"

    if [[ ! -f "$OUT_DIR/.config" ]]; then
        log "Configuring with $DEFCONFIG ..."
        # shellcheck disable=SC2086
        make O="$OUT_DIR" ARCH="$ARCH" CC="$bcc" \
            CLANG_TRIPLE="$CLANG_TRIPLE" CROSS_COMPILE="$CROSS_COMPILE" \
            $EXTRA_FLAGS "$DEFCONFIG"
        prepare_config
    else
        log "Reusing existing .config in $OUT_DIR"
        ./scripts/config --file "$OUT_DIR/.config" --set-str LOCALVERSION "$CUSTOM_LOCALVERSION"
        # shellcheck disable=SC2086
        make O="$OUT_DIR" ARCH="$ARCH" CC="$bcc" \
            CLANG_TRIPLE="$CLANG_TRIPLE" CROSS_COMPILE="$CROSS_COMPILE" \
            $EXTRA_FLAGS olddefconfig
        # Ensure version headers are regenerated so UTS_RELEASE and compile.h always match
        rm -f "$OUT_DIR/include/config/kernel.release" "$OUT_DIR/include/generated/utsrelease.h" "$OUT_DIR/include/generated/compile.h" "$OUT_DIR/init/version.o"
    fi

    if grep -q '^CONFIG_KALLSYMS_ALL=y$' "$OUT_DIR/.config"; then
        log "CONFIG_KALLSYMS_ALL=y verified (APatch supported)."
    fi

    log "Starting kernel compilation..."
    # shellcheck disable=SC2086
    make O="$OUT_DIR" ARCH="$ARCH" CC="$bcc" \
        CLANG_TRIPLE="$CLANG_TRIPLE" CROSS_COMPILE="$CROSS_COMPILE" \
        $EXTRA_FLAGS -j"$JOBS"

    local image="$OUT_DIR/arch/arm64/boot/Image.gz-dtb"
    if [[ ! -f "$image" ]]; then
        err "Build failed: $image was not produced."
        exit 1
    fi

    cp -f "$image" "$BUILD_DIR/Image.gz-dtb"
    log "Kernel image saved to: $BUILD_DIR/Image.gz-dtb"
}

package_zip() {
    if [[ "${SKIP_PACKAGE:-0}" == "1" ]]; then
        log "Skipping zip packaging."
        return
    fi

    local image="$BUILD_DIR/Image.gz-dtb"
    if [[ ! -f "$image" ]]; then
        if [[ -f "$OUT_DIR/arch/arm64/boot/Image.gz-dtb" ]]; then
            cp -f "$OUT_DIR/arch/arm64/boot/Image.gz-dtb" "$image"
        else
            err "Cannot package zip: $image does not exist. Run build first."
            exit 1
        fi
    fi

    local stage="$BUILD_DIR/.anykernel_stage"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp -r "$AK3_DIR/." "$stage/"
    cp "$image" "$stage/Image.gz-dtb"

    local kernel_name_upper version_name_upper
    kernel_name_upper="$(echo "$KERNEL_NAME" | tr '[:lower:]' '[:upper:]')"
    version_name_upper="$(echo "$VERSION_NAME" | tr '[:lower:]' '[:upper:]')"
    local full_title="${KERNEL_NAME} Kernel ${KERNEL_VERSION} [${VERSION_NAME}]"
    local kver="4.14.$(grep -m1 '^SUBLEVEL =' "$ROOT_DIR/Makefile" | awk '{print $3}')"
    local toolchain_ver="Clang 11.0.1 + GCC 9.3"

    # Generate dynamic changelog ui_print statements for TWRP
    local changelog_ui=""
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        local escaped_line
        escaped_line="$(echo "$line" | sed 's/"/\\"/g')"
        changelog_ui+="ui_print \"   * ${escaped_line}\";\n"
    done < <(git log -n 8 --pretty=format:"[%h] %s" 2>/dev/null || echo "[custom] Initial ${KERNEL_NAME} ${VERSION_NAME} release")

    # Generate standalone CHANGELOG.txt for the flashable zip
    {
        echo "========================================================"
        echo " ${kernel_name_upper} KERNEL ${KERNEL_VERSION} [${version_name_upper}] - ${DEVICE_NAME} (${DEVICE_CODENAME})"
        echo " Version: $KERNEL_VERSION"
        echo " Version Name: $VERSION_NAME ($BRANCH_DESC)"
        echo " Branch: $GIT_BRANCH"
        echo " Commit ID: $COMMIT_HASH"
        echo " Maintainer: $MAINTAINER"
        echo " Motto: We aim for stability, not for anything else."
        echo " Linux: v$kver | Date: $COMMIT_DATE"
        echo " Toolchain: $toolchain_ver"
        echo " Defconfig: $DEFCONFIG (APatch ready)"
        echo "========================================================"
        echo ""
        echo "--- Changelog (Recent Commits) ---"
        git log -n 15 --pretty=format:"* %h (%cd) - %s%n  Author: %an%n%b" --date=short 2>/dev/null || git log -n 5 2>/dev/null || true
    } > "$stage/CHANGELOG.txt"

    cat << AK_EOF > "$stage/anykernel.sh"
# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers
# Configured for ${full_title} by ${MAINTAINER}

## AnyKernel setup
properties() { '
kernel.string=${KERNEL_NAME} ${KERNEL_VERSION} [${VERSION_NAME}] (${COMMIT_HASH}) by ${MAINTAINER} for ${DEVICE_NAME} (${DEVICE_CODENAME})
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=${DEVICE_CODENAME}
device.name2=${DEVICE_CODENAME}_in
device.name3=${DEVICE_CODENAME}in
device.name4=
supported.versions=
supported.patchlevels=
'; } # end properties

## shell variables
BLOCK=/dev/block/by-name/boot;
# ${DEVICE_NAME} (${DEVICE_CODENAME}) is an A-only device: a single boot partition,
# no A/B slot suffix. Keep IS_SLOT_DEVICE=0 (AnyKernel3 default for A-only).
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;

## TWRP / Recovery UI Banner & Version Details
ui_print " ";
ui_print " ============================================";
ui_print "   ${kernel_name_upper} KERNEL ${KERNEL_VERSION} [${version_name_upper}]";
ui_print " ============================================";
ui_print "  * Kernel       : ${KERNEL_NAME}            ";
ui_print "  * Version      : ${KERNEL_VERSION}         ";
ui_print "  * Version Name : ${VERSION_NAME} (${BRANCH_DESC})";
ui_print "  * Branch       : ${GIT_BRANCH}             ";
ui_print "  * Commit ID    : ${COMMIT_HASH}            ";
ui_print "  * Device       : ${DEVICE_NAME} (${DEVICE_CODENAME})";
ui_print "  * Maintainer   : ${MAINTAINER}             ";
ui_print "  * Motto        : We aim for stability,     ";
ui_print "                   not for anything else.    ";
ui_print "  * Linux Ver    : $kver                     ";
ui_print "  * Build Date   : $COMMIT_DATE              ";
ui_print "  * Toolchain    : $toolchain_ver            ";
ui_print "  * Features     : APatch / KernelPatch ready";
ui_print " --------------------------------------------";
ui_print "  LATEST COMMIT:";
ui_print "  $COMMIT_SUBJECT";
ui_print " --------------------------------------------";
ui_print "  CHANGELOG (Recent Changes):";
\$(printf '%b' "$changelog_ui")
ui_print " ============================================";
ui_print " ";

## AnyKernel file attributes
ui_print " [*] [1/4] Configuring ramdisk permissions & ownership...";
ui_print "     - Target partition: /dev/block/by-name/boot (A-only)";
chmod -R 750 \$RAMDISK/*;
chown -R root:root \$RAMDISK/*;

## AnyKernel install
ui_print " [*] [2/4] Dumping and unpacking current boot image...";
dump_boot;

ui_print " [*] [3/4] Repacking boot image with ${KERNEL_NAME} ${VERSION_NAME} v${KERNEL_VERSION} (${COMMIT_HASH})...";
ui_print "     - Linux kernel: v$kver (MT6785 / Helio G90T)";
ui_print "     - Low-battery call reboot fix: active";
ui_print "     - Low-battery lag/throttling fix: active";
ui_print "     - APatch / KernelPatch KALLSYMS: enabled";
write_boot;

ui_print " [*] [4/4] Cleaning up temporary installer files...";
ui_print " ";
ui_print " ============================================";
ui_print "   ${kernel_name_upper} ${version_name_upper} ${KERNEL_VERSION} (${GIT_BRANCH}) INSTALLED!";
ui_print "   Commit: ${COMMIT_HASH}";
ui_print "   We aim for stability, not for anything else.";
ui_print "      Reboot and enjoy solid stability.      ";
ui_print " ============================================";
ui_print " ";
## end install
AK_EOF

    local zip_file="$BUILD_DIR/${ZIP_BASE}.zip"
    log "Packaging AnyKernel3 flashable zip: $zip_file"
    (cd "$stage" && zip -r9 "$zip_file" . -x '*.git*' -x '.github*')
    rm -rf "$stage"

    # Also maintain latest.zip and date/edition-stamped copies in build/
    cp -f "$zip_file" "$BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${DATE}.zip"
    cp -f "$zip_file" "$BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${DEVICE_CODENAME}.zip"
    ln -sf "$(basename "$zip_file")" "$BUILD_DIR/latest.zip"
    ln -sf "$(basename "$zip_file")" "$BUILD_DIR/${COMMIT_HASH}.zip"

    log "================================================="
    log "BUILD SUCCEEDED!"
    log "Package ZIP:   $zip_file"
    log "Commit Link:   $BUILD_DIR/${COMMIT_HASH}.zip"
    log "Latest Link:   $BUILD_DIR/latest.zip"
    log "Device Link:   $BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${DEVICE_CODENAME}.zip"
    log "Kernel Image:  $BUILD_DIR/Image.gz-dtb"
    log "================================================="
}

case "$ACTION" in
    clean)
        clean_build
        ;;
    distclean)
        distclean_build
        ;;
    menuconfig)
        run_menuconfig
        ;;
    kernel)
        build_kernel
        ;;
    zip|package)
        package_zip
        ;;
    all|"")
        build_kernel
        package_zip
        ;;
    *)
        err "Unknown action: $ACTION"
        echo "Usage: $0 [all|kernel|zip|menuconfig|clean|distclean]"
        exit 1
        ;;
esac
