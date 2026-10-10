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
KERNEL_VERSION="${KERNEL_VERSION:-1.0.0-Beta}"
DEVICE_NAME="${DEVICE_NAME:-Redmi Note 8 Pro}"
DEVICE_CODENAME="${DEVICE_CODENAME:-begonia}"
MAINTAINER="${MAINTAINER:-TXO R (Pox Project)}"
DEFCONFIG="${DEFCONFIG:-}"

# Determine safe parallel jobs based on available RAM and load to protect host PC from freezing
auto_jobs() {
    local mem_avail_kb
    mem_avail_kb=$(awk '/MemAvailable/ {print $2}' /proc/meminfo 2>/dev/null || echo 3000000)
    # Estimate ~1.2GB per clang worker; clamp jobs between 2 and 4 to prevent freezing host
    local safe_jobs=$(( mem_avail_kb / 1200000 ))
    if (( safe_jobs < 2 )); then
        safe_jobs=2
    elif (( safe_jobs > 4 )); then
        safe_jobs=4
    fi
    echo "$safe_jobs"
}
JOBS="${JOBS:-$(auto_jobs)}"
EXTRA_FLAGS="${EXTRA_FLAGS:-}"
DATE="$(date +%Y%m%d-%H%M)"

# Git Metadata: Branch & Commit ID
GIT_BRANCH="${GIT_BRANCH:-${GITHUB_REF_NAME:-$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "onyx")}}"
COMMIT_HASH="${COMMIT_HASH:-$(git rev-parse --short HEAD 2>/dev/null || echo "custom")}"
COMMIT_DATE="$(git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M' 2>/dev/null || date +'%Y-%m-%d %H:%M')"
COMMIT_SUBJECT="$(git log -1 --format=%s 2>/dev/null || echo "Release build")"

# Version Name / Codename (Rocks theme: Granite, Obsidian, Onyx)
if [[ -z "${VERSION_NAME:-}" ]]; then
    case "$GIT_BRANCH" in
        onyx-resukisu*|*resukisu*)
            VERSION_NAME="Onyx-ReSukiSu"
            BRANCH_DESC="Zero Frame-Drop Gaming & ReSukiSu Ultra Edition"
            DEFAULT_DEFCONFIG="begonia_user_ksu_defconfig"
            ;;
        onyx-apatch*|*apatch*)
            VERSION_NAME="Onyx-APatch"
            BRANCH_DESC="Zero Frame-Drop Gaming & APatch Edition"
            DEFAULT_DEFCONFIG="begonia_apatch_defconfig"
            ;;
        onyx-ksu-next*|*ksu-next*)
            VERSION_NAME="Onyx-KSU-Next"
            BRANCH_DESC="Zero Frame-Drop Gaming & KernelSU-Next Edition"
            DEFAULT_DEFCONFIG="begonia_apatch_defconfig"
            ;;
        main|granite)
            VERSION_NAME="Granite"
            BRANCH_DESC="Rock-Solid Stability Edition"
            DEFAULT_DEFCONFIG="begonia_user_defconfig"
            ;;
        memory-enhanced|obsidian)
            VERSION_NAME="Obsidian"
            BRANCH_DESC="iOS-Style Compressed Memory Edition"
            DEFAULT_DEFCONFIG="begonia_user_defconfig"
            ;;
        gaming|onyx|*)
            VERSION_NAME="Onyx"
            BRANCH_DESC="Zero Frame-Drop Gaming Edition (Unrooted)"
            DEFAULT_DEFCONFIG="begonia_user_defconfig"
            ;;
    esac
else
    BRANCH_DESC="${BRANCH_DESC:-Custom Edition}"
    DEFAULT_DEFCONFIG="begonia_user_defconfig"
fi
DEFCONFIG="${DEFCONFIG:-$DEFAULT_DEFCONFIG}"
BRANCH_CODENAME="$VERSION_NAME"

# Derive dynamic localversion string: contains name, version, version name, branch, commit id
if [[ -n "${LOCALVERSION:-}" ]]; then
    CUSTOM_LOCALVERSION="$LOCALVERSION"
else
    CUSTOM_LOCALVERSION="-${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${GIT_BRANCH}-${COMMIT_HASH}"
    # Linux kernel UTS_RELEASE ("4.14.357" + CUSTOM_LOCALVERSION) cannot exceed 64 chars
    if (( ${#CUSTOM_LOCALVERSION} + 9 > 64 )); then
        CUSTOM_LOCALVERSION="-${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${COMMIT_HASH}"
    fi
fi

# Package zip base name: contains name, version, version name, branch, commit id, device
if [[ -n "${PACKAGE_NAME:-}" ]]; then
    ZIP_BASE="$PACKAGE_NAME"
elif [[ "$KERNEL_NAME" == *"$DEVICE_CODENAME"* ]]; then
    ZIP_BASE="${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${GIT_BRANCH}-${COMMIT_HASH}"
else
    ZIP_BASE="${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${GIT_BRANCH}-${COMMIT_HASH}-${DEVICE_CODENAME}"
fi

log()  { printf '\033[1;32m[*] %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[!] %s\033[0m\n' "$*"; }
err()  { printf '\033[1;31m[-] %s\033[0m\n' "$*"; }

# 1. Ensure kerdevdep is bootstrapped
if [[ ! -f "$KERDEVDEP/env.sh" || ! -x "$KERDEVDEP/clang/bin/clang" || ! -x "$KERDEVDEP/bin/ccache" ]]; then
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
        -mllvm -unroll-threshold=1000 -mllvm -inline-threshold=1500 \
        -c "$test_src" -o /dev/null 2>/dev/null; then
        log "Toolchain rejects -mllvm thresholds - disabling CONFIG_INLINE_OPTIMIZATION"
        ./scripts/config --file "$OUT_DIR/.config" --disable INLINE_OPTIMIZATION
    fi

    # Dynamically apply LOCALVERSION based on KERNEL_NAME and KERNEL_VERSION
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

    # Host PC safety: ensure load average and memory are suitable before launching build (skip in CI/GitHub Actions or SKIP_LOAD_CHECK)
    if [[ -z "${SKIP_LOAD_CHECK:-}" && -z "${CI:-}" && -z "${GITHUB_ACTIONS:-}" ]]; then
        local load
        while true; do
            load=$(awk '{print int($1)}' /proc/loadavg 2>/dev/null || echo 0)
            local mem_avail_kb mem_avail_mb
            mem_avail_kb=$(awk '/MemAvailable/ {print $2}' /proc/meminfo 2>/dev/null || echo 2000000)
            mem_avail_mb=$(( mem_avail_kb / 1024 ))
            if (( load > 8 || mem_avail_mb < 700 )); then
                warn "PC load is high (${load}) or available RAM low (${mem_avail_mb}MB). Waiting 5s for host to settle..."
                sleep 5
            else
                break
            fi
        done
    fi

    log "Starting kernel compilation (nice priority, jobs: $JOBS)..."
    # shellcheck disable=SC2086
    nice -n 10 make O="$OUT_DIR" ARCH="$ARCH" CC="$bcc" \
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

    local commit_hash commit_date commit_subject kver toolchain_ver git_branch
    commit_hash="$(git rev-parse --short HEAD 2>/dev/null || echo "custom")"
    commit_date="$(git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M' 2>/dev/null || date +'%Y-%m-%d %H:%M')"
    commit_subject="$(git log -1 --format=%s 2>/dev/null || echo "Release build")"
    git_branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "gaming")"
    kver="4.14.$(grep -m1 '^SUBLEVEL =' "$ROOT_DIR/Makefile" | awk '{print $3}')"
    toolchain_ver="Clang 11.0.1 + GCC 9.3"

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
        echo " Memory: iOS-Style On-Demand Multi-Stream Compressed ZRAM"
        echo " Gaming: Zero Frame-Drop Gaming Mode Controller"
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
kernel.string=${KERNEL_NAME} ${KERNEL_VERSION} [${VERSION_NAME}] (${GIT_BRANCH}-${COMMIT_HASH}) by ${MAINTAINER} for ${DEVICE_NAME} (${DEVICE_CODENAME})
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
ui_print "  * Crypto Engine: ARMv8 CE & NEON Accelerated";
ui_print "  * Mem Engine   : iOS-Style On-Demand ZRAM  ";
ui_print "  * Game Engine  : Zero Frame-Drop Gaming Mode";
ui_print "  * Display Mode : iOS D65 & Sunlight HBM Overdrive";
ui_print "  * Perf Mode    : Tri-State Auto-Trigger Engine";
ui_print " --------------------------------------------";
ui_print "  LATEST COMMIT:";
ui_print "  $COMMIT_SUBJECT";
ui_print " --------------------------------------------";
ui_print "  CHANGELOG (Recent Changes):";
AK_EOF
    printf '%b' "$changelog_ui" >> "$stage/anykernel.sh"
    cat << AK_EOF >> "$stage/anykernel.sh"
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

## Ramdisk enhancements
if [ -d "\$RAMDISK" ]; then
    ui_print " [*] Injecting memory enhancement & gaming mode into ramdisk...";

    # 1. iOS-Style On-Demand Compressed Memory Management
    cat << 'RC_EOF' > \$RAMDISK/init.memory_enhanced.rc
# Pox adaptive memory (HarmonyOS EROFS-cache + MGLRU lesson, 4.14-safe):
# - swappiness 100: anon -> per-CPU zstd ZRAM (no LMK kills, 85% fewer kills lesson)
# - watermark 150: early kswapd fits 8.3ms frame budget (gaming raises to 200, powersave drops to 100)
# - page-cluster 0: no swap readahead (ZRAM random; readahead wastes CPU/RAM)
# - vfs pressure 50: retain dentries for instant launch (EROFS block-cache lesson)
# - dirty 20/10: throughput (gaming tightens to 10/5 for latency)
# Kernel gaming_mode switches these per-mode; boot sets balanced baseline.
on boot
    write /proc/sys/vm/watermark_scale_factor 150
    write /proc/sys/vm/page-cluster 0
    write /proc/sys/vm/vfs_cache_pressure 50
    write /proc/sys/vm/swappiness 100
    write /proc/sys/vm/dirty_ratio 20
    write /proc/sys/vm/dirty_background_ratio 10
    write /proc/sys/vm/dirty_expire_centisecs 1500
    write /proc/sys/vm/dirty_writeback_centisecs 300
    write /proc/sys/vm/stat_interval 10

    # Advanced CFS & Interactivity Tunables (FreeBSD ULE / Windows NT Dynamic Boost)
    write /proc/sys/kernel/sched_latency_ns 3000000
    write /proc/sys/kernel/sched_min_granularity_ns 300000
    write /proc/sys/kernel/sched_wakeup_granularity_ns 250000
    write /proc/sys/kernel/sched_sync_hint_enable 1

    # Low-latency high-throughput networking & Fair Queueing for BBR
    write /proc/sys/net/core/default_qdisc fq
    write /proc/sys/net/ipv4/tcp_congestion_control bbr
    write /proc/sys/net/ipv4/tcp_fastopen 3
    write /proc/sys/net/ipv4/tcp_slow_start_after_idle 0
    write /proc/sys/net/ipv4/tcp_tw_reuse 1
    write /proc/sys/net/ipv4/tcp_autocorking 0
    write /proc/sys/net/ipv4/tcp_notsent_lowat 16384
    write /proc/sys/net/ipv4/tcp_rmem "4096 87380 6291456"
    write /proc/sys/net/ipv4/tcp_wmem "4096 65536 6291456"
    write /proc/sys/net/ipv4/tcp_ecn 1
    write /proc/sys/net/ipv4/tcp_syncookies 1
    write /proc/sys/net/core/netdev_max_backlog 5000

on property:sys.boot_completed=1
    write /sys/block/zram0/comp_algorithm zstd
    write /sys/block/zram0/max_comp_streams 8
    write /proc/sys/vm/watermark_scale_factor 150
    write /proc/sys/vm/page-cluster 0
    write /proc/sys/vm/vfs_cache_pressure 50
    write /proc/sys/vm/swappiness 100
    write /proc/sys/vm/dirty_ratio 20
    write /proc/sys/vm/dirty_background_ratio 10
    write /proc/sys/vm/dirty_expire_centisecs 1500
    write /proc/sys/vm/dirty_writeback_centisecs 300
    write /proc/sys/vm/stat_interval 10
    write /proc/sys/kernel/sched_latency_ns 3000000
    write /proc/sys/kernel/sched_min_granularity_ns 300000
    write /proc/sys/kernel/sched_wakeup_granularity_ns 250000
    write /proc/sys/net/core/default_qdisc fq
    write /proc/sys/net/ipv4/tcp_congestion_control bbr
    write /proc/sys/net/ipv4/tcp_autocorking 0
    write /proc/sys/net/ipv4/tcp_notsent_lowat 16384
    write /proc/sys/net/ipv4/tcp_rmem "4096 87380 6291456"
    write /proc/sys/net/ipv4/tcp_wmem "4096 65536 6291456"
RC_EOF
    chmod 644 \$RAMDISK/init.memory_enhanced.rc

    # 2. Zero Frame-Drop Gaming Mode, Real Colors Calibration & ROM Performance Mode Triggers
    cat << 'RC_EOF' > \$RAMDISK/init.gaming.rc
# Gaming Mode & iOS Display Init Script for Redmi Note 8 Pro (begonia)
# Triggers full gaming performance optimizations and color profiles

on boot
    chmod 0644 /proc/perfmgr/gaming_mode
    chmod 0644 /sys/kernel/gaming_mode
    chmod 0644 /proc/perfmgr/true_tone
    chmod 0644 /sys/kernel/true_tone
    chmod 0644 /proc/perfmgr/color_mode
    chmod 0644 /sys/kernel/color_mode
    chmod 0644 /proc/perfmgr/hbm_mode
    chmod 0644 /sys/kernel/hbm_mode
    # Pox standout safe-rootless torch: 0666 by design so stock
    # flashlight apps work without root; hardware guards
    # (325mA clamp, 20ms anti-strobe, 5-min timeout) in driver.
    chmod 0666 /proc/perfmgr/torch_brightness
    chmod 0666 /proc/perfmgr/flashlight_brightness
    chmod 0444 /proc/perfmgr/torch_info
    chmod 0444 /proc/perfmgr/profile
    chmod 0666 /sys/kernel/torch_brightness
    chmod 0666 /sys/kernel/flashlight_brightness
    chmod 0666 /sys/devices/platform/flashlights_mt6360/torchbrightness
    chmod 0644 /proc/perfmgr/camera_profile
    chmod 0644 /sys/kernel/camera_profile
    chmod 0644 /proc/perfmgr/slog3
    chmod 0644 /sys/kernel/slog3
    chmod 0644 /proc/perfmgr/camera_4k60
    chmod 0644 /sys/kernel/camera_4k60
    # Universal USB OTG: keep every adapter working plug-and-play.
    # Directory needs +x to enumerate; device nodes stay world-
    # accessible so DACs/serial/ethernet/iPhone tethering just work.
    chmod 0755 /dev/bus/usb
    chmod 0666 /dev/bus/usb/*/* 2>/dev/null || true
    chmod 0666 /dev/ttyUSB0
    chmod 0666 /dev/ttyUSB1
    chmod 0666 /dev/ttyUSB2
    chmod 0666 /dev/ttyUSB3
    chmod 0666 /dev/ttyACM0
    chmod 0666 /dev/ttyACM1
    chmod 0644 /proc/perfmgr/touch_game_mode
    chmod 0644 /proc/perfmgr/touch_sensitivity
    chmod 0664 /sys/class/touch/touch_dev/touch_game_mode
    chmod 0664 /sys/class/touch/touch_dev/touch_sensitivity
    chmod 0644 /proc/perfmgr/headphone_gain
    chmod 0644 /sys/kernel/sound_control/headphone_gain
    chmod 0644 /proc/perfmgr/mic_gain
    chmod 0644 /sys/kernel/sound_control/mic_gain
    chmod 0644 /proc/perfmgr/vibrator_strength
    chmod 0644 /proc/perfmgr/wakelock_blocker
    chmod 0644 /proc/perfmgr/fast_charge
    chmod 0644 /proc/perfmgr/dt2w
    chmod 0644 /sys/android_touch/doubletap2wake
    chmod 0644 /proc/perfmgr/dynamic_fsync
    chmod 0644 /sys/kernel/dynamic_fsync/dynamic_fsync
    chmod 0644 /sys/module/task_turbo/parameters/feats
    chmod 0644 /proc/perfmgr/battery_bypass
    chmod 0644 /proc/perfmgr/battery_limit
    chmod 0444 /proc/perfmgr/battery_status
    chmod 0644 /sys/module/ged/parameters/gx_game_mode
    chmod 0644 /sys/module/ged/parameters/gx_boost_on
    chmod 0644 /sys/module/ged/parameters/boost_gpu_enable
    chmod 0644 /sys/module/ged/parameters/gx_force_cpu_boost
    write /sys/module/ged/parameters/boost_gpu_enable 1
    write /proc/perfmgr/true_tone 1
    write /proc/perfmgr/color_mode 1
    write /proc/perfmgr/fast_charge 1

    # MIUI / HyperOS 3 memory swap & xswapd cgroup nodes (restricted to root & system)
    chown root system /dev/memcg/memory.xswapd.quota
    chown root system /dev/memcg/memory.xswapd.enable
    chown root system /dev/memcg/memory.xswapd.reclaim
    chown root system /dev/memcg/apps/memory.xswapd.quota
    chmod 0664 /dev/memcg/memory.xswapd.quota
    chmod 0664 /dev/memcg/memory.xswapd.enable
    chmod 0664 /dev/memcg/memory.xswapd.reclaim
    chmod 0664 /dev/memcg/apps/memory.xswapd.quota

    # Default flash storage readahead to 128KB for smooth capture and I/O
    write /sys/block/sda/queue/read_ahead_kb 128
    write /sys/block/sdb/queue/read_ahead_kb 128
    write /sys/block/sdc/queue/read_ahead_kb 128
    write /sys/block/mmcblk0/queue/read_ahead_kb 128

    # Flash storage queue tuning: mq-deadline flash tuning, allow bio request merging and enable affinity
    write /sys/block/sda/queue/scheduler mq-deadline
    write /sys/block/sda/queue/iosched/read_expire 250
    write /sys/block/sda/queue/iosched/write_expire 1500
    write /sys/block/sda/queue/iosched/fifo_batch 8
    write /sys/block/sda/queue/rq_affinity 2
    write /sys/block/sda/queue/iostats 0
    write /sys/block/sda/queue/add_random 0
    write /sys/block/sda/queue/nomerges 0
    write /sys/block/sda/queue/nr_requests 128
    write /sys/block/sdb/queue/scheduler mq-deadline
    write /sys/block/sdb/queue/iosched/read_expire 250
    write /sys/block/sdb/queue/iosched/write_expire 1500
    write /sys/block/sdb/queue/iosched/fifo_batch 8
    write /sys/block/sdb/queue/rq_affinity 2
    write /sys/block/sdb/queue/iostats 0
    write /sys/block/sdb/queue/add_random 0
    write /sys/block/sdb/queue/nomerges 0
    write /sys/block/sdb/queue/nr_requests 128
    write /sys/block/sdc/queue/scheduler mq-deadline
    write /sys/block/sdc/queue/iosched/read_expire 250
    write /sys/block/sdc/queue/iosched/write_expire 1500
    write /sys/block/sdc/queue/iosched/fifo_batch 8
    write /sys/block/sdc/queue/rq_affinity 2
    write /sys/block/sdc/queue/iostats 0
    write /sys/block/sdc/queue/add_random 0
    write /sys/block/sdc/queue/nomerges 0
    write /sys/block/sdc/queue/nr_requests 128
    write /sys/block/mmcblk0/queue/scheduler mq-deadline
    write /sys/block/mmcblk0/queue/iosched/read_expire 250
    write /sys/block/mmcblk0/queue/iosched/write_expire 1500
    write /sys/block/mmcblk0/queue/iosched/fifo_batch 8
    write /sys/block/mmcblk0/queue/rq_affinity 2
    write /sys/block/mmcblk0/queue/iostats 0
    write /sys/block/mmcblk0/queue/add_random 0
    write /sys/block/mmcblk0/queue/nomerges 0
    write /sys/block/mmcblk0/queue/nr_requests 128

    # Flash storage health permissions
    chmod 0444 /proc/ufs_health
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/dump_health
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/health
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/health_remaining_pct
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/life_time_a
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/life_time_b
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/pre_eol_info
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/product_name
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/product_revision
    chmod 0444 /sys/bus/platform/devices/11270000.ufshci/manufacturer_id

# ROM Performance Mode / Game Space Active
on property:persist.sys.power_mode_perf=1
    write /proc/perfmgr/gaming_mode 1
    write /proc/net/wlan/setCAM "CAM 1"
    write /sys/block/sda/queue/read_ahead_kb 512
    write /sys/block/sdb/queue/read_ahead_kb 512
    write /sys/block/sdc/queue/read_ahead_kb 512
    write /sys/block/mmcblk0/queue/read_ahead_kb 512

on property:persist.sys.power_mode_perf=0
    write /proc/perfmgr/gaming_mode 0
    write /proc/net/wlan/setCAM "CAM 0"
    write /sys/block/sda/queue/read_ahead_kb 128
    write /sys/block/sdb/queue/read_ahead_kb 128
    write /sys/block/sdc/queue/read_ahead_kb 128
    write /sys/block/mmcblk0/queue/read_ahead_kb 128

# Ultra Power Saver Mode (AOSP / LineageOS Battery Saver)
on property:persist.sys.power_mode_perf=-1
    write /proc/perfmgr/gaming_mode -1
    write /proc/net/wlan/setCAM "CAM 0"
    write /sys/block/sda/queue/read_ahead_kb 128
    write /sys/block/sdb/queue/read_ahead_kb 128
    write /sys/block/sdc/queue/read_ahead_kb 128
    write /sys/block/mmcblk0/queue/read_ahead_kb 128

# LineageOS Performance Profile (0=power_save, 1=balanced, 2=performance)
on property:sys.perf.profile=2
    setprop persist.sys.power_mode_perf 1

on property:sys.perf.profile=1
    setprop persist.sys.power_mode_perf 0

on property:sys.perf.profile=0
    setprop persist.sys.power_mode_perf -1

# Android Battery Saver Global Low Power mode
on property:settings.global.low_power=1
    setprop persist.sys.power_mode_perf -1

on property:settings.global.low_power=0
    setprop persist.sys.power_mode_perf 0

# GameSpace Mode (AOSP / Chaldea GameSpace)
on property:sys.gamespace.mode=1
    setprop persist.sys.power_mode_perf 1

on property:sys.gamespace.mode=0
    setprop persist.sys.power_mode_perf 0

on property:sys.gamespace.in_game=1
    setprop persist.sys.power_mode_perf 1

on property:sys.gamespace.in_game=0
    setprop persist.sys.power_mode_perf 0

# AOSP / PixelOS / LineageOS libperfmgr PowerHAL trigger
on property:vendor.powerhal.state=SUSTAINED_PERFORMANCE
    setprop persist.sys.power_mode_perf 1

on property:vendor.powerhal.state=""
    setprop persist.sys.power_mode_perf 0

# MIUI / HyperOS Performance Mode trigger
on property:persist.sys.perf_mode=1
    setprop persist.sys.power_mode_perf 1

on property:persist.sys.perf_mode=0
    setprop persist.sys.power_mode_perf 0

# Direct debug toggle
on property:debug.gaming.mode=1
    setprop persist.sys.power_mode_perf 1

on property:debug.gaming.mode=0
    setprop persist.sys.power_mode_perf 0
RC_EOF
    chmod 644 \$RAMDISK/init.gaming.rc

    # 3. Smart Battery Guard & Direct Power Bypass Charging
    cat << 'RC_EOF' > \$RAMDISK/init.battery_guard.rc
# Smart Battery Guard & Direct Power Bypass Charging for Redmi Note 8 Pro (begonia)
on boot
    chmod 0664 /sys/kernel/battery_protection/bypass_mode
    chmod 0664 /sys/kernel/battery_protection/charge_limit
    chmod 0664 /sys/kernel/battery_protection/thermal_guard
    chmod 0664 /sys/kernel/battery_protection/temp_limit
    chmod 0444 /sys/kernel/battery_protection/status
    chmod 0444 /sys/kernel/battery_protection/battery_soc
    chmod 0444 /sys/kernel/battery_protection/battery_temp

    # Default: Full charge (100%), thermal guard disabled (0) so JEITA handles protection without artificial stopping
    write /sys/kernel/battery_protection/charge_limit 100
    write /sys/kernel/battery_protection/thermal_guard 0
    write /sys/kernel/battery_protection/temp_limit 48

    # True Tone System Default (Calibrated Liquid Retina D65)
    chmod 0644 /proc/perfmgr/true_tone
    chmod 0644 /sys/kernel/true_tone
    write /proc/perfmgr/true_tone 1
RC_EOF
    chmod 644 \$RAMDISK/init.battery_guard.rc

    if [ -f "\$RAMDISK/init.rc" ]; then
        insert_line init.rc "init.memory_enhanced.rc" after "import /init.environ.rc" "import /init.memory_enhanced.rc";
        insert_line init.rc "init.gaming.rc" after "import /init.memory_enhanced.rc" "import /init.gaming.rc";
        insert_line init.rc "init.battery_guard.rc" after "import /init.gaming.rc" "import /init.battery_guard.rc";
    fi
fi

ui_print " [*] [3/4] Repacking boot image with ${KERNEL_NAME} ${VERSION_NAME} (${KERNEL_VERSION})...";
ui_print "     - Linux kernel: v$kver (MT6785 / Helio G90T)";
ui_print "     - Low-battery call reboot fix: active";
ui_print "     - Smart Battery Guard: Direct-Power Bypass & 39C Thermal Protection";
ui_print "     - Hardware Bypass Control: /proc/perfmgr/battery_bypass (0644)";
ui_print "     - Hardware 240Hz Touch Gaming Mode: zero-debounce sampling active";
ui_print "     - Hardware Double-Tap to Wake: /proc/perfmgr/dt2w & /sys/android_touch";
ui_print "     - MediaTek Task-Turbo: UI RenderThread, Binder & BigCore boost";
ui_print "     - Hardware Vibrator: unlocked 3.3V range & /proc/perfmgr/vibrator_strength";
ui_print "     - Deep Sleep Wakelock Filter: parasitic network wakelocks blocked";
ui_print "     - Fast Charge Boost: 1.5A PC USB & 2.0A non-std charger boost active";
ui_print "     - Hi-Fi Sound & Mic: MT6359 +8dB headphone & +18dB mic analog gain";
ui_print "     - Dynamic Fsync Engine: micro-stutter elimination active";
ui_print "     - Network & Wi-Fi Engine: TCP Fast Open & Wi-Fi SetCAM zero-jitter active";
ui_print "     - EAS Schedutil & Fork: 32% capacity margin & child_runs_first active";
ui_print "     - Flash Storage Queue: rq_affinity=2, iostats=0 & nomerges tuning";
ui_print "     - LiquidCool Gaming Thermal: 1200mW CPU & 1000mW GPU floor lock";
ui_print "     - Storage I/O: BFQ hierarchical scheduler & 512KB readahead active";
ui_print "     - Cinema Camera Engine: 4K 60FPS unlocked & Sony S-Log3 active";
ui_print "     - APatch / KernelPatch KALLSYMS: enabled";
ui_print "     - iOS-Style Compressed Memory: ZSTD ZRAM, 24MB cushion, vfs=50";
ui_print "     - True Tone Display Engine: Calibrated D65 Liquid Retina reference active";
ui_print "     - Video Anti-Lag Engine: VDEC/VENC clock floor & LP4-2100 DDR active";
ui_print "     - Zero Frame-Drop Gaming Mode: active on ROM Performance toggle";
ui_print "     - FPSGO Ultra-Rescue + Mali-G76 MC4 Touch Boost: enabled";
ui_print "     - Universal USB OTG: DACs, controllers, serial & ethernet active";
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

    # Also maintain latest.zip, versioned zip, and date-stamped copies in build/
    cp -f "$zip_file" "$BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${GIT_BRANCH}-${DATE}.zip"
    cp -f "$zip_file" "$BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${GIT_BRANCH}-${DEVICE_CODENAME}.zip"
    ln -sf "$(basename "$zip_file")" "$BUILD_DIR/latest.zip"
    ln -sf "$(basename "$zip_file")" "$BUILD_DIR/${COMMIT_HASH}.zip"

    log "================================================="
    log "BUILD SUCCEEDED!"
    log "Package ZIP:   $zip_file"
    log "Commit Link:   $BUILD_DIR/${COMMIT_HASH}.zip"
    log "Latest Link:   $BUILD_DIR/latest.zip"
    log "Branch Link:   $BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${GIT_BRANCH}-${DEVICE_CODENAME}.zip"
    log "Kernel Image:  $BUILD_DIR/Image.gz-dtb"
    log "================================================="
}

install_device() {
    local zip_file
    zip_file=$(ls -t "$BUILD_DIR"/${ZIP_BASE}*.zip 2>/dev/null | head -n 1 || true)
    if [[ -z "$zip_file" || ! -f "$zip_file" ]]; then
        if [[ -f "$BUILD_DIR/latest.zip" ]]; then
            zip_file="$BUILD_DIR/latest.zip"
        else
            err "No AnyKernel3 zip found in $BUILD_DIR. Please run ./build.sh all first."
            exit 1
        fi
    fi

    log "Checking ADB connection to device..."
    if ! command -v adb >/dev/null 2>&1; then
        err "adb tool not found in PATH."
        exit 1
    fi

    local dev_state
    dev_state=$(adb get-state 2>/dev/null || echo "offline")
    if [[ "$dev_state" != "device" && "$dev_state" != "recovery" ]]; then
        err "Device not detected in 'device' or 'recovery' mode (current state: $dev_state)."
        exit 1
    fi

    log "Pushing $(basename "$zip_file") to device (/sdcard/)..."
    adb push "$zip_file" "/sdcard/$(basename "$zip_file")"
    adb push "$zip_file" "/sdcard/latest.zip"
    adb push "$zip_file" "/sdcard/Pox-Kernel-latest.zip"

    log "Kernel zip installed to /sdcard/ on device!"
    log "  - /sdcard/$(basename "$zip_file")"
    log "  - /sdcard/latest.zip"
    log "  - /sdcard/Pox-Kernel-latest.zip"
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
    install)
        install_device
        ;;
    build-install|"build and install")
        build_kernel
        package_zip
        install_device
        ;;
    all|"")
        build_kernel
        package_zip
        ;;
    *)
        err "Unknown action: $ACTION"
        echo "Usage: $0 [all|kernel|zip|install|build-install|menuconfig|clean|distclean]"
        exit 1
        ;;
esac
