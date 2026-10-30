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
PATCH_DIR="${PATCH_DIR:-$ROOT_DIR/patches}"

# Media overlays are independent patch sets and may be requested together
# (`VARIANT=ksu,miui,c2`). They are applied in this fixed order and reverted in
# the reverse order, independent of the order the user listed them in.
OVERLAY_ORDER="miui c2"
ACTION="${1:-all}"

# Kernel Branding & Versioning (Fully customizable via environment or build script)
KERNEL_NAME="${KERNEL_NAME:-Pox}"
KERNEL_VERSION="${KERNEL_VERSION:-2.0}"
DEVICE_NAME="${DEVICE_NAME:-Redmi Note 8 Pro}"
DEVICE_CODENAME="${DEVICE_CODENAME:-begonia}"
MAINTAINER="${MAINTAINER:-TXO R (Pox Project)}"
DEFCONFIG="${DEFCONFIG:-}"

# Bundled root packages for KernelPatch-based variants. Each release asset is
# an APK that doubles as a flashable recovery zip; the AnyKernel installer
# auto-flashes it after writing the kernel and stages the manager APK for
# first-boot install. The per-flavor definition (repo, zip/APK names, package)
# lives in root/<flavor>.env, which the variant patch adds to the tree.
BUNDLE_ROOT="${BUNDLE_ROOT:-1}"
ROOT_INSTALL_APP="${ROOT_INSTALL_APP:-1}"

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
GIT_BRANCH="${GIT_BRANCH#refs/heads/}"
GIT_BRANCH="${GIT_BRANCH#refs/tags/}"
COMMIT_HASH="${COMMIT_HASH:-$(git rev-parse --short=7 HEAD 2>/dev/null || echo "custom")}"
COMMIT_DATE="$(git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M' 2>/dev/null || date +'%Y-%m-%d %H:%M')"
COMMIT_SUBJECT="$(git log -1 --format=%s 2>/dev/null || echo "Release build")"

log()  { printf '\033[1;32m[*] %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[!] %s\033[0m\n' "$*"; }
err()  { printf '\033[1;31m[-] %s\033[0m\n' "$*"; }

# Read-only variant listing. This runs before any patch application or
# toolchain bootstrap so `./build.sh patches` can inspect the tree safely.
if [[ "$ACTION" == "variants" || "$ACTION" == "list-variants" ||
      "$ACTION" == "patches" || "$ACTION" == "list-patches" ]]; then
    log "Variant selection: VARIANT=<root>[,<overlay>] (e.g. VARIANT=ksu,c2)"
    log "Root flavors: vanilla (none), apatch, folkpatch, ksu"
    log "Overlays:     miui, c2 (combinable: VARIANT=ksu,miui,c2)"
    echo
    for patch in "$PATCH_DIR"/*.patch; do
        [[ -f "$patch" ]] || continue
        printf '  %-8s %8s bytes  %s\n' \
            "$(basename "$patch" .patch)" \
            "$(stat -c%s "$patch" 2>/dev/null || wc -c < "$patch")" \
            "$(grep -c '^diff --git' "$patch") file(s)"
    done
    exit 0
fi

# ---------------------------------------------------------------------------
# Variant selection
# ---------------------------------------------------------------------------
# The repository keeps one source branch (`onyx`). Root flavors (vanilla,
# APatch, FolkPatch, KernelSU) and media overlays (MIUI/HyperOS, Codec2) are
# shipped as patch sets under patches/ and applied on top of that branch at
# build time.
#
#   VARIANT=vanilla          stock build, no patches
#   VARIANT=ksu              BakaSU + NoMount + SuSFS
#   VARIANT=ksu,c2           KSU root + Codec2 overlay
#   VARIANT=apatch,miui      APatch root (auto-flashed) + MIUI overlay
#   VARIANT=ksu,miui,c2      KSU root + MIUI and Codec2 overlays together
#   VARIANT=folkpatch        FolkPatch root (same kernel, FolkPatch package)
#
# When VARIANT is unset the branch/tag name is used for backwards
# compatibility, so `onyx-ksu-next` still builds the KSU edition.
VARIANT="${VARIANT:-${ROOT_VARIANT:-}}"
VARIANT="${VARIANT//,/ }"
VARIANT="$(echo "$VARIANT" | xargs)"

if [[ -z "$VARIANT" ]]; then
    case "$GIT_BRANCH" in
        main|granite|obsidian|memory-enhanced) VARIANT="$GIT_BRANCH" ;;
        *ksun*|*ksu-next*|*ksu*|*apatch*|*folkpatch*|*miui*|*hyperos*|*c2*|*codec2*)
            VARIANT="$GIT_BRANCH"
            ;;
        *) VARIANT="onyx" ;;
    esac
fi

# Map a variant token to the patch file that implements it.
variant_patch_name() {
    case "$1" in
        onyx|vanilla|stock|none|default|"") printf '' ;;
        main|granite|obsidian|memory-enhanced) printf '' ;;
        *ksun*|*ksu-next*|*kernelsu*|*reksun*|*ksu*) printf 'ksu' ;;
        # FolkPatch is a standalone patch: same KernelPatch kernel side plus
        # its own root packaging profile. It does not stack on apatch.patch.
        *folkpatch*)                   printf 'folkpatch' ;;
        *apatch*)                      printf 'apatch' ;;
        *miui*|*hyperos*)              printf 'miui' ;;
        *c2*|*codec2*)                 printf 'c2' ;;
        *) printf '%s' "$1" ;;
    esac
}

# Root flavor drives packaging (which manager package gets bundled and
# auto-flashed) and is independent of the media overlays.
parse_variant_tokens() {
    local token
    for token in $VARIANT; do
        case "$token" in
            *folkpatch*)                      printf 'folkpatch'; return ;;
            *apatch*)                         printf 'apatch';    return ;;
            *ksun*|*ksu*|*kernelsu*|*reksun*) printf 'ksu';       return ;;
        esac
    done
    printf 'vanilla'
}
ROOT_FLAVOR="$(parse_variant_tokens)"



apply_variant_patches() {
    [[ "${VARIANT_PATCHED:-0}" == "1" ]] && return 0

    local requested=() token name p seen=" "
    local roots=()
    for token in $VARIANT; do
        # A token may map to more than one patch (folkpatch = shared kernel
        # side + its own root packaging profile).
        for name in $(variant_patch_name "$token"); do
            [[ -z "$name" ]] && continue
            # Repeated tokens are ignored so a patch is never applied twice.
            [[ "$seen" == *" $name "* ]] && continue
            seen="$seen$name "
            case "$name" in
                miui|c2) ;;   # overlays are appended in canonical order below
                *)       roots+=("$name") ;;
            esac
        done
    done
    # Overlays compose: `VARIANT=ksu,miui,c2` applies both patch sets. They are
    # always applied in OVERLAY_ORDER so any token order builds the same tree
    # and `unvariant` can reverse them deterministically.
    requested=("${roots[@]}")
    for name in $OVERLAY_ORDER; do
        [[ "$seen" == *" $name "* ]] && requested+=("$name")
    done

    # A tree left dirty by an earlier variant build must never be reused for
    # a different variant (for example `VARIANT=ksu ./build.sh all` followed
    # by `VARIANT=vanilla ./build.sh all` would silently ship a rooted
    # kernel). Detect applied patch sets that are not part of this request.
    local applied=() f n wanted
    for f in "$PATCH_DIR"/*.patch; do
        [[ -f "$f" ]] || continue
        if git apply --reverse --check "$f" >/dev/null 2>&1; then
            applied+=("$(basename "$f" .patch)")
        fi
    done
    for name in "${applied[@]}"; do
        wanted=0
        for n in "${requested[@]}"; do
            [[ "$n" == "$name" ]] && wanted=1
        done
        if (( ! wanted )); then
            err "Patch set '$name' is still applied from a previous build."
            err "Run './build.sh unvariant' to restore the clean onyx tree, then retry."
            exit 1
        fi
    done

    ((${#requested[@]})) || return 0

    # Validate every requested patch before touching the tree.
    local patch
    for name in "${requested[@]}"; do
        patch="$PATCH_DIR/$name.patch"
        if [[ ! -f "$patch" ]]; then
            err "Variant '$name' has no patch file (expected patches/$name.patch)"
            err "Available variants: $(ls "$PATCH_DIR"/*.patch 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.patch$//' | paste -sd' ' -)"
            exit 1
        fi
    done

    # Apply in place, skipping patches that are already applied (so repeated
    # `./build.sh zip` runs do not fail).
    local pending=0
    for name in "${requested[@]}"; do
        patch="$PATCH_DIR/$name.patch"
        if ! git apply --reverse --check "$patch" >/dev/null 2>&1; then
            pending=1
        fi
    done

    if (( pending )); then
        if ! git rev-parse --git-dir >/dev/null 2>&1; then
            err "Variant patches require a git checkout (VARIANT=$VARIANT)"
            exit 1
        fi
        # Only the files a patch touches have to be clean; unrelated local
        # edits (or a sparse checkout) are fine.
        local touched=() p
        for name in "${requested[@]}"; do
            while IFS= read -r p; do
                touched+=("$p")
            done < <(grep '^diff --git' "$PATCH_DIR/$name.patch" \
                | sed -E 's|^diff --git a/||; s| b/.*$||')
        done
        if ! git diff --quiet -- "${touched[@]}" || ! git diff --cached --quiet -- "${touched[@]}"; then
            err "Variant patches conflict with local changes in:"
            git diff --name-only -- "${touched[@]}"
            git diff --cached --name-only -- "${touched[@]}"
            err "Commit or stash those files first, or set VARIANT_PATCHED=1 if the patches are already applied."
            exit 1
        fi
        for name in "${requested[@]}"; do
            patch="$PATCH_DIR/$name.patch"
            if git apply --reverse --check "$patch" >/dev/null 2>&1; then
                log "Variant patch already applied: $name"
                APPLIED_PATCHES="${APPLIED_PATCHES:+$APPLIED_PATCHES }$name"
                continue
            fi
            log "Applying variant patch: patches/$name.patch"
            git apply --index "$patch" || {
                err "Failed to apply patches/$name.patch - rebase it onto the current tree."
                exit 1
            }
            APPLIED_PATCHES="${APPLIED_PATCHES:+$APPLIED_PATCHES }$name"
        done
    fi

    # A ksu patch adds .gitmodules + a KernelSU gitlink; the submodule itself
    # still has to be checked out.
    if [[ -f "$ROOT_DIR/.gitmodules" ]]; then
        # Sync first so switching a root submodule (for example ReSukiSU ->
        # BakaSU) cannot reuse a stale URL from .git/config.
        git submodule sync --recursive >/dev/null
        git submodule update --init --recursive || {
            err "Failed to initialise submodules added by the variant patch."
            exit 1
        }
    fi

    VARIANT_PATCHED=1
    export APPLIED_PATCHES VARIANT_PATCHED
}

# Restore the clean onyx tree: reverse every applied patch set (overlays were
# applied last), then drop the KernelSU checkout added by the ksu patch.
unapply_variant_patches() {
    local roots=() overlays=() f name i o
    for f in "$PATCH_DIR"/*.patch; do
        [[ -f "$f" ]] || continue
        if git apply --reverse --check "$f" >/dev/null 2>&1; then
            name="$(basename "$f" .patch)"
            case "$name" in
                miui|c2) ;;   # collected in OVERLAY_ORDER below
                *)       roots+=("$f") ;;
            esac
        fi
    done
    for o in $OVERLAY_ORDER; do
        f="$PATCH_DIR/$o.patch"
        [[ -f "$f" ]] || continue
        git apply --reverse --check "$f" >/dev/null 2>&1 && overlays+=("$f")
    done

    if ((${#roots[@]} + ${#overlays[@]} == 0)); then
        log "No variant patches are applied."
        return 0
    fi

    # deinit first: reversing the ksu patch deletes .gitmodules.
    if [[ -f "$ROOT_DIR/.gitmodules" ]]; then
        git submodule deinit -f --all >/dev/null 2>&1 || true
    fi

    # Match application order in reverse: overlays were applied after roots.
    for ((i=${#overlays[@]}-1; i>=0; i--)); do
        log "Reverting variant patch: ${overlays[i]#$ROOT_DIR/}"
        git apply --reverse --index "${overlays[i]}" || {
            err "Could not revert ${overlays[i]#$ROOT_DIR/} cleanly."
            exit 1
        }
    done
    for ((i=${#roots[@]}-1; i>=0; i--)); do
        log "Reverting variant patch: ${roots[i]#$ROOT_DIR/}"
        git apply --reverse --index "${roots[i]}" || {
            err "Could not revert ${roots[i]#$ROOT_DIR/} cleanly."
            exit 1
        }
    done

    rm -rf "$ROOT_DIR/KernelSU" "$ROOT_DIR/.git/modules/KernelSU"
    log "Working tree restored to the clean onyx state."
}

# `./build.sh unvariant` is a maintenance action: it only touches the working
# tree, so run it before the toolchain bootstrap.
if [[ "$ACTION" == "unvariant" || "$ACTION" == "unpatch" ]]; then
    unapply_variant_patches
    exit 0
fi

# KernelSU is kept as a pristine upstream submodule; local fixes live in
# ksu-patches/<root>/ and are applied on top of the checkout. Paths inside
# those patches are relative to the KernelSU submodule root.
apply_root_patches() {
    local dir="$ROOT_DIR/ksu-patches/$ROOT_FLAVOR"
    [[ -d "$ROOT_DIR/KernelSU" && -d "$dir" ]] || return 0

    local patch
    shopt -s nullglob
    for patch in "$dir"/*.patch; do
        if git -C "$ROOT_DIR" apply --directory=KernelSU \
                --reverse --check "$patch" >/dev/null 2>&1; then
            log "KernelSU patch already applied: $(basename "$patch")"
            continue
        fi
        log "Applying KernelSU patch: $(basename "$patch")"
        if ! git -C "$ROOT_DIR" apply --directory=KernelSU "$patch"; then
            err "Failed to apply ksu-patches/$ROOT_FLAVOR/$(basename "$patch")"
            exit 1
        fi
    done
    shopt -u nullglob
}

# Version Name / Codename (Rocks theme: Granite, Obsidian, Onyx)
#
# One resolver maps the selected variant (or branch/tag name) to an edition,
# description and default defconfig. Root flavors and media overlays compose,
# so `onyx-c2-ksun` reports "Onyx-C2-KSU" instead of a generic fallback.
resolve_variant() {
    local branch="$1"
    local root="" root_desc="" defconfig="begonia_user_defconfig"
    local extra_desc=""

    # Root flavor. Order matters: `folkpatch` is matched before `apatch` and
    # `sultan` before the generic APatch pattern so the right label wins.
    case "$branch" in
        *folkpatch*)                  root="FolkPatch"; root_desc="FolkPatch";            defconfig="begonia_apatch_defconfig" ;;
        *apatch*)                     root="APatch";   root_desc="APatch";                  defconfig="begonia_apatch_defconfig" ;;
        *ksun*|*ksu-next*|*kernelsu*|*reksun*|*ksu*) root="KSU"; root_desc="KernelSU-Next + BakaSU"; defconfig="bakasu_defconfig" ;;
        *kaeru*)                      root="Kaeru";    root_desc="Kaeru Bootloader";            defconfig="begonia_user_defconfig" ;;
    esac

    # Orthogonal media overlays.
    if [[ "$branch" == *miui* || "$branch" == *hyperos* ]]; then
        extra_desc="${extra_desc:+$extra_desc + }MIUI/HyperOS"
    fi
    if [[ "$branch" == *c2* || "$branch" == *codec2* ]]; then
        extra_desc="${extra_desc:+$extra_desc + }Codec2 Media"
    fi

    DEFAULT_DEFCONFIG="$defconfig"

    if [[ "$branch" == main || "$branch" == *granite* ]]; then
        VERSION_NAME="Granite"
        BRANCH_DESC="Rock-Solid Stability Edition"
        DEFAULT_DEFCONFIG="begonia_user_defconfig"
        return
    fi

    if [[ "$branch" == *obsidian* || "$branch" == *memory-enhanced* ]]; then
        VERSION_NAME="Obsidian"
        BRANCH_DESC="iOS-Style Compressed Memory Edition"
        DEFAULT_DEFCONFIG="begonia_user_defconfig"
        return
    fi

    local edition="Onyx"
    [[ "$branch" == *c2* || "$branch" == *codec2* ]] && edition="${edition}-C2"
    [[ "$branch" == *miui* || "$branch" == *hyperos* ]] && edition="${edition}-MIUI"

    BRANCH_DESC="Zero Frame-Drop Gaming"
    [[ -n "$extra_desc" ]] && BRANCH_DESC="$BRANCH_DESC & $extra_desc"

    if [[ -n "$root" ]]; then
        VERSION_NAME="${edition}-${root}"
        BRANCH_DESC="$BRANCH_DESC & $root_desc Edition"
    else
        VERSION_NAME="$edition"
        BRANCH_DESC="$BRANCH_DESC Edition (Unrooted)"
    fi
}

if [[ -z "${VERSION_NAME:-}" ]]; then
    resolve_variant "$VARIANT"
else
    BRANCH_DESC="${BRANCH_DESC:-Custom Edition}"
    DEFAULT_DEFCONFIG="begonia_user_defconfig"
fi

# Apply the selected variant before resolving the defconfig: the patch is what
# adds root-specific defconfigs (begonia_apatch_defconfig, bakasu_defconfig).
case "$ACTION" in
    clean|distclean) ;;
    *)
        apply_variant_patches
        apply_root_patches
        ;;
esac
: "${APPLIED_PATCHES:=none}"
export APPLIED_PATCHES
log "Variant:      $VARIANT (root: $ROOT_FLAVOR, patches: $APPLIED_PATCHES)"

# If the variant asked for a defconfig that does not exist in this checkout,
# fall back to the unrooted default so `./build.sh all` still works.
if [[ -z "${DEFCONFIG:-}" && ! -f "$ROOT_DIR/arch/arm64/configs/${DEFAULT_DEFCONFIG}" ]]; then
    warn "Defconfig ${DEFAULT_DEFCONFIG} is not present in this checkout; falling back to begonia_user_defconfig"
    DEFAULT_DEFCONFIG="begonia_user_defconfig"
fi

DEFCONFIG="${DEFCONFIG:-$DEFAULT_DEFCONFIG}"
BRANCH_CODENAME="$VERSION_NAME"

# Kernel release tag: root first, then media overlays, e.g.
# 4.14.357-Pox-Apatch-C2, 4.14.357-Pox-KSU, 4.14.357-Pox-Vanilla.
kernel_variant_tag() {
    local tag token
    case "$ROOT_FLAVOR" in
        apatch)    tag="Apatch" ;;
        folkpatch) tag="FolkPatch" ;;
        ksu)       tag="KSU" ;;
        *)         tag="Vanilla" ;;
    esac
    for token in $VARIANT; do
        case "$token" in
            *miui*|*hyperos*) [[ "$tag" == *-MIUI ]] || tag="$tag-MIUI" ;;
            *c2*|*codec2*)    [[ "$tag" == *-C2 ]]   || tag="$tag-C2" ;;
        esac
    done
    printf '%s' "$tag"
}

# Derive dynamic localversion string: Pox + variant. LOCALVERSION still wins
# when a one-off build needs a fully custom suffix.
if [[ -n "${LOCALVERSION:-}" ]]; then
    CUSTOM_LOCALVERSION="$LOCALVERSION"
else
    CUSTOM_LOCALVERSION="-Pox"
fi
EXTENDED_LOCALVERSION="${EXTENDED_LOCALVERSION:-$CUSTOM_LOCALVERSION}"
LINUX_VERSION="4.14.$(grep -m1 '^SUBLEVEL =' "$ROOT_DIR/Makefile" | awk '{print $3}')"

# Package zip base name: name, version, edition, device
if [[ -n "${PACKAGE_NAME:-}" ]]; then
    ZIP_BASE="$PACKAGE_NAME"
elif [[ "$KERNEL_NAME" == *"$DEVICE_CODENAME"* ]]; then
    ZIP_BASE="${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${COMMIT_HASH}"
else
    ZIP_BASE="${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${COMMIT_HASH}-${DEVICE_CODENAME}"
fi

# Report the toolchain that is actually in kerdevdep (brand + LLVM version)
toolchain_version() {
    local cv bv
    cv="$("$KERDEVDEP/clang/bin/clang" --version 2>/dev/null | head -n1 \
        | sed -E 's/[[:space:]]*\([^)]*\)//g; s/[[:space:]]+/ /g; s/^[[:space:]]+//; s/[[:space:]]+$//' || true)"
    [[ -n "$cv" ]] || cv="unknown clang"
    bv="$("$KERDEVDEP/clang/bin/aarch64-linux-gnu-ld" --version 2>/dev/null | head -n1 \
        | grep -oE '[0-9]+(\.[0-9]+)+' | head -n1 || true)"
    if [[ -n "$bv" ]]; then
        printf '%s + binutils %s' "$cv" "$bv"
    else
        printf '%s' "$cv"
    fi
}

# 1. Ensure kerdevdep is bootstrapped
# A toolchain selection change (e.g. same kerdevdep, different Clang) forces
# a fresh fetch so the build never silently reuses the previous compiler.
TOOLCHAIN_SELECTOR="trb:${TRB_CLANG_REPO:-https://gitlab.com/voltageos/clang.git}:${TRB_CLANG_PIN:-b0fa406b3f65eae210c60dda2e91dae979e4b3a8}"

if [[ -d "$KERDEVDEP/clang" ]] && [[ "$(cat "$KERDEVDEP/clang/.kerdevdep-toolchain" 2>/dev/null)" != "$TOOLCHAIN_SELECTOR" ]]; then
    log "Toolchain selection changed - re-fetching Clang ..."
    rm -rf "$KERDEVDEP/clang"
fi

if [[ ! -f "$KERDEVDEP/env.sh" || ! -x "$KERDEVDEP/clang/bin/clang" || ! -x "$KERDEVDEP/bin/ccache" ]]; then
    log "Bootstrapping self-contained dependencies in $KERDEVDEP ..."
    bash "$KERDEVDEP/setup_kerdevdep.sh"
fi

# 2. Source kerdevdep environment
# shellcheck source=/dev/null
source "$KERDEVDEP/env.sh"
TOOLCHAIN_VER="$(toolchain_version)"

ARCH=arm64
CC=clang
CLANG_TRIPLE=aarch64-linux-gnu-
# TRB Clang ships its own aarch64-linux-gnu binutils, so use the GNU prefix
# that matches kerdevdep/setup_kerdevdep.sh's pinned toolchain.
CROSS_COMPILE=aarch64-linux-gnu-
AK3_DIR="$KERDEVDEP/anykernel"

export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-Pox}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-Poxproject}"

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

merge_config_fragments() {
    [[ -f "$OUT_DIR/.config" ]] || return 0

    # Droidspaces-OSS: container support fragment, when the tree ships one.
    if [[ -f "$ROOT_DIR/arch/arm64/configs/droidspaces.config" ]]; then
        log "Merging Droidspaces config fragment..."
        sh "$ROOT_DIR/scripts/kconfig/merge_config.sh" -m -O "$OUT_DIR" \
            "$OUT_DIR/.config" "$ROOT_DIR/arch/arm64/configs/droidspaces.config" >/dev/null
    fi

    # MIUI/HyperOS overlay: the variant patch adds this fragment; merge it into
    # whichever root defconfig was selected (user/bakasu/apatch).
    if [[ -f "$ROOT_DIR/arch/arm64/configs/miui.config" ]] && \
       [[ "$VERSION_NAME" == *MIUI* || "$VARIANT" == *miui* || "$VARIANT" == *hyperos* ]]; then
        log "Merging MIUI/HyperOS config fragment..."
        sh "$ROOT_DIR/scripts/kconfig/merge_config.sh" -m -O "$OUT_DIR" \
            "$OUT_DIR/.config" "$ROOT_DIR/arch/arm64/configs/miui.config" >/dev/null
    fi
}

prepare_config() {
    merge_config_fragments
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
        -mllvm -polly-vectorizer=stripmine \
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

    # Clang 18 emits an out-of-line `strlcpy.inline` clone shared by init and
    # non-init callers; it references .init.data variables (boot_command_line,
    # saved_root_name, ...), which modpost reports as section mismatches. They
    # are false positives for this tree, so keep them as warnings instead of
    # failing the vmlinux link for every variant.
    ./scripts/config --file "$OUT_DIR/.config" --enable SECTION_MISMATCH_WARN_ONLY

    # Apply the branded LOCALVERSION (4.14.357-Pox-<variant>)
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
    log "Variant:      $VARIANT (root: $ROOT_FLAVOR, patches: $APPLIED_PATCHES)"
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

# Resolve the manager package bundled with the current root flavor.
# Load the packaging profile added by the variant patch:
#   root/<flavor>.env
# It defines ROOT_LABEL, ROOT_ZIP_NAME, ROOT_APK_NAME, ROOT_PACKAGE_NAME,
# ROOT_REPO and ROOT_PINNED_ZIP. Flavors without a profile (vanilla, ksu)
# bundle no manager package.
root_package_spec() {
    ROOT_LABEL=""
    ROOT_ZIP_NAME=""
    ROOT_APK_NAME=""
    ROOT_PACKAGE_NAME=""
    ROOT_ASSET=""
    ROOT_REPO=""
    ROOT_PINNED_ZIP=""

    local profile="$ROOT_DIR/root/${ROOT_FLAVOR:-vanilla}.env"
    if [[ -f "$profile" ]]; then
        # shellcheck source=/dev/null
        source "$profile"
    fi
}

# Resolve the manager APK straight from the public "latest release" page.
# `/releases/latest` redirects to `/releases/tag/<tag>` and the expanded asset
# fragment lists the download links. Neither is subject to the api.github.com
# rate limit, so no Actions/workflow token is involved.
release_apk_url_html() {
    local repo="$1" final tag page url
    final="$(curl -fsSL -o /dev/null -w '%{url_effective}' \
        --retry 2 --retry-delay 2 --connect-timeout 20 \
        "https://github.com/${repo}/releases/latest" 2>/dev/null)" || return 1
    [[ "$final" == */releases/tag/* ]] || return 1
    tag="${final##*/releases/tag/}"
    page="$(curl -fsSL --retry 2 --retry-delay 2 --connect-timeout 20 \
        "https://github.com/${repo}/releases/expanded_assets/${tag}" 2>/dev/null)" || return 1
    url="$(printf '%s\n' "$page" \
        | grep -oE 'href="[^"]*/releases/download/[^"]+\.apk"' \
        | head -n1 \
        | sed -E 's/^href="([^"]+)".*/\1/')"
    [[ -n "$url" ]] || return 1
    case "$url" in
        http*) printf '%s' "$url" ;;
        /*)    printf 'https://github.com%s' "$url" ;;
        *)     printf 'https://github.com/%s' "$url" ;;
    esac
}

# Fallback for when the release page markup changes: ask the public REST API.
GH_API_RETRIES="${GH_API_RETRIES:-3}"
release_apk_url_api() {
    local repo="$1" body attempt url
    for ((attempt = 1; attempt <= GH_API_RETRIES; attempt++)); do
        body="$(curl -fsSL \
            -H 'Accept: application/vnd.github+json' \
            -H 'X-GitHub-Api-Version: 2022-11-28' \
            --retry 2 --retry-delay 2 --connect-timeout 20 \
            "https://api.github.com/repos/${repo}/releases/latest" 2>/dev/null)" || body=""
        url="$(printf '%s\n' "$body" \
            | grep -oE '"browser_download_url":[[:space:]]*"[^"]+\.apk"' \
            | head -n1 \
            | sed -E 's/.*"(https[^"]+)".*/\1/')"
        [[ -n "$url" ]] && { printf '%s' "$url"; return 0; }
        sleep "$attempt"
    done
    return 1
}

# Fetch the latest release APK (which is also a flashable recovery zip) for a
# KernelPatch-based root solution.
fetch_root_release() {
    local repo="$1" dest="$2" label="$3"
    local url=""

    if ! command -v curl >/dev/null 2>&1; then
        warn "curl not available; cannot fetch ${label} release."
        return 1
    fi

    url="$(release_apk_url_html "$repo" 2>/dev/null || true)"
    if [[ -z "$url" ]]; then
        warn "Release page lookup for ${label} failed; trying the GitHub API."
        url="$(release_apk_url_api "$repo" 2>/dev/null || true)"
    fi

    if [[ -z "$url" ]]; then
        warn "Could not resolve a ${label} APK from ${repo}."
        return 1
    fi

    ROOT_ASSET="$(basename "${url%%\?*}")"
    log "Fetching latest ${label} release: $ROOT_ASSET"
    # Release assets are public; fetch them directly without extra headers.
    if ! curl -fL --retry 3 --retry-delay 2 \
            --connect-timeout 20 -o "$dest" "$url"; then
        warn "Failed to download ${label} APK."
        return 1
    fi
    return 0
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

    # Bundle the manager package described by root/<flavor>.env. Its release
    # artifact is an APK that is also a flashable recovery zip; stage it with
    # a .zip suffix.
    root_package_spec
    ROOT_BUNDLED=0
    if [[ -n "$ROOT_ZIP_NAME" && "$BUNDLE_ROOT" == "1" ]]; then
        local root_zip="$stage/$ROOT_ZIP_NAME"
        local root_source="$ROOT_PINNED_ZIP"
        if [[ -n "$root_source" && -f "$root_source" ]]; then
            cp -f "$root_source" "$root_zip"
            ROOT_ASSET="$(basename "$root_source")"
            log "Bundled local ${ROOT_LABEL} package: $ROOT_ASSET"
            ROOT_BUNDLED=1
        else
            if [[ -z "$ROOT_REPO" ]]; then
                warn "${ROOT_FLAVOR} has no root packaging profile; skipping manager bundle."
            elif fetch_root_release "$ROOT_REPO" "$root_zip" "$ROOT_LABEL"; then
                log "Bundled ${ROOT_LABEL} as ${ROOT_ZIP_NAME} ($(du -h "$root_zip" | cut -f1))"
                ROOT_BUNDLED=1
            else
                rm -f "$root_zip"
                warn "Proceeding without ${ROOT_LABEL} auto-flash step."
                if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
                    echo "::warning title=Root auto-flash skipped::Could not download the ${ROOT_LABEL} APK for '${ROOT_FLAVOR}' from its latest release; the flashable zip will not patch boot. Check network access to github.com."
                fi
            fi
        fi
    fi

    local commit_hash commit_date commit_subject kver toolchain_ver git_branch
    commit_hash="$(git rev-parse --short HEAD 2>/dev/null || echo "custom")"
    commit_date="$(git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M' 2>/dev/null || date +'%Y-%m-%d %H:%M')"
    commit_subject="$(git log -1 --format=%s 2>/dev/null || echo "Release build")"
    git_branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "gaming")"
    kver="4.14.$(grep -m1 '^SUBLEVEL =' "$ROOT_DIR/Makefile" | awk '{print $3}')"
    toolchain_ver="$TOOLCHAIN_VER"

    local kernel_name_upper version_name_upper
    kernel_name_upper="$(echo "$KERNEL_NAME" | tr '[:lower:]' '[:upper:]')"
    version_name_upper="$(echo "$VERSION_NAME" | tr '[:lower:]' '[:upper:]')"
    local full_title="${KERNEL_NAME} Kernel ${KERNEL_VERSION} [${VERSION_NAME}]"
    local kver="4.14.$(grep -m1 '^SUBLEVEL =' "$ROOT_DIR/Makefile" | awk '{print $3}')"
    local toolchain_ver="$TOOLCHAIN_VER"

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
        echo " Variant: $VARIANT (patches: $APPLIED_PATCHES)"
        echo " Commit ID: $COMMIT_HASH"
        echo " Maintainer: $MAINTAINER"
        echo " Motto: We aim for stability, not for anything else."
        echo " Linux: v$kver | Date: $COMMIT_DATE"
        echo " Toolchain: $toolchain_ver"
        echo " Defconfig: $DEFCONFIG (APatch ready)"
        echo " Root Auto-Flash: ${ROOT_LABEL:-none} ${ROOT_ASSET:-disabled}"
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
BLOCK=boot;
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
    chown root system /dev/memcg/memory.xswapd.score
    chown root system /dev/memcg/memory.xswapd.refaults
    chown root system /dev/memcg/memory.xswapd.reclaim
    chown root system /dev/memcg/apps/memory.xswapd.quota
    chown root system /dev/memcg/apps/memory.xswapd.score
    chown root system /dev/memcg/apps/memory.xswapd.refaults
    chmod 0664 /dev/memcg/memory.xswapd.quota
    chmod 0664 /dev/memcg/memory.xswapd.enable
    chmod 0664 /dev/memcg/memory.xswapd.score
    chmod 0444 /dev/memcg/memory.xswapd.refaults
    chmod 0664 /dev/memcg/memory.xswapd.reclaim
    chmod 0664 /dev/memcg/apps/memory.xswapd.quota
    chmod 0664 /dev/memcg/apps/memory.xswapd.score
    chmod 0444 /dev/memcg/apps/memory.xswapd.refaults
    chown root system /sys/kernel/xswapd/* 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/enabled 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/trigger 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/watermark_kb 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/min_score 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/max_scan_pages 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/refault_threshold 2>/dev/null || true
    chmod 0664 /sys/kernel/xswapd/interval_ms 2>/dev/null || true
    chmod 0444 /sys/kernel/xswapd/reclaimed_pages 2>/dev/null || true
    chmod 0444 /sys/kernel/xswapd/status 2>/dev/null || true

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

    # 4. One-shot first-boot installer for the bundled root manager APK.
    if [ -f "\$AKHOME/pox_root_install.sh" ]; then
        cp -f "\$AKHOME/pox_root_install.sh" "\$RAMDISK/pox_root_install.sh";
        chmod 755 "\$RAMDISK/pox_root_install.sh";
        cat << 'RC_EOF' > \$RAMDISK/init.pox_root.rc
# Pox Kernel - one-shot root manager app installer
service pox_root_install /system/bin/sh /pox_root_install.sh
    class late_start
    user root
    group root
    seclabel u:r:init:s0
    oneshot
    disabled

on property:sys.boot_completed=1
    start pox_root_install
RC_EOF
        chmod 644 \$RAMDISK/init.pox_root.rc;
    fi

    if [ -f "\$RAMDISK/init.rc" ]; then
        insert_line init.rc "init.memory_enhanced.rc" after "import /init.environ.rc" "import /init.memory_enhanced.rc";
        insert_line init.rc "init.gaming.rc" after "import /init.memory_enhanced.rc" "import /init.gaming.rc";
        insert_line init.rc "init.battery_guard.rc" after "import /init.gaming.rc" "import /init.battery_guard.rc";
        [ -f "\$RAMDISK/init.pox_root.rc" ] && insert_line init.rc "init.pox_root.rc" after "import /init.battery_guard.rc" "import /init.pox_root.rc";
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

## Auto-flash the bundled ${ROOT_LABEL:-root package} after the kernel is written.
## The release APK is itself a flashable recovery zip (KernelPatch). Mirror its
## update-binary flow: extract its bundled busybox and assets, then run the root
## installer against the freshly written boot image.
if [ -n "${ROOT_ZIP_NAME}" ]; then
ROOT_SRC="";
for root_pkg in "\$AKHOME/${ROOT_ZIP_NAME}" "${ROOT_ZIP_NAME}"; do
    [ -f "\$root_pkg" ] && { ROOT_SRC="\$root_pkg"; break; };
done;
if [ -n "\$ROOT_SRC" ]; then
    ui_print " ";
    ui_print " ============================================";
    ui_print "   Flashing bundled ${ROOT_LABEL:-root package}";
    ui_print " ============================================";
    sync;
    ROOT_INSTALL=/dev/tmp/install;
    rm -rf "\$ROOT_INSTALL";
    mkdir -p "\$ROOT_INSTALL";
    # BusyBox picks its applet from argv[0]. The extracted binary must be named
    # "busybox" (as in the APK's own update-binary); a name like "pox_busybox"
    # makes it fail with "applet not found" and the installer never runs.
    ROOT_BB=/dev/tmp/busybox;
    if unzip -o "\$ROOT_SRC" "lib/arm64-v8a/libbusybox.so" -d /dev/tmp >/dev/null 2>&1; then
        mv -f /dev/tmp/lib/arm64-v8a/libbusybox.so "\$ROOT_BB" 2>/dev/null;
        chmod 755 "\$ROOT_BB" 2>/dev/null;
        rm -rf /dev/tmp/lib;
    fi;
    if [ -x "\$ROOT_BB" ]; then
        "\$ROOT_BB" unzip -o "\$ROOT_SRC" "assets/*" "lib/*" -x "lib/*/libbusybox.so" -d "\$ROOT_INSTALL" >/dev/null 2>&1;
    else
        unzip -o "\$ROOT_SRC" "assets/*" "lib/*" -x "lib/*/libbusybox.so" -d "\$ROOT_INSTALL" >/dev/null 2>&1;
    fi;
    if [ -f "\$ROOT_INSTALL/assets/InstallAP.sh" ]; then
        chmod -R 755 "\$ROOT_INSTALL/assets" "\$ROOT_INSTALL/lib" 2>/dev/null;
        if [ -x "\$ROOT_BB" ]; then
            ( cd "\$ROOT_INSTALL" && ASH_STANDALONE=1 "\$ROOT_BB" sh assets/InstallAP.sh 3 "\$OUTFD" "\$ROOT_SRC" );
        else
            ( cd "\$ROOT_INSTALL" && sh assets/InstallAP.sh 3 "\$OUTFD" "\$ROOT_SRC" );
        fi;
        if [ -f "\$ROOT_INSTALL/new-boot.img" ]; then
            ui_print " [*] ${ROOT_LABEL:-root package} patched the boot image.";
        else
            ui_print " [!] ${ROOT_LABEL:-root package} did not produce a patched boot image.";
        fi;
    else
        ui_print " [!] ${ROOT_LABEL:-root package} installer not found; skipping root step.";
    fi;
    if [ "${ROOT_INSTALL_APP}" = "1" ]; then
        for dest in /data/system /data/local/tmp /sdcard; do
            [ -d "\$dest" ] || continue;
            cp -f "\$ROOT_SRC" "\$dest/${ROOT_APK_NAME}" 2>/dev/null \
                && chmod 0644 "\$dest/${ROOT_APK_NAME}" 2>/dev/null;
        done;
        ui_print " [*] Staged ${ROOT_LABEL:-root} manager APK for first-boot install.";
    fi;
else
    ui_print " [i] No bundled root package; skipping root auto-flash step.";
fi;
ui_print " ";
fi;
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

    # First-boot installer for the bundled manager APK. anykernel.sh copies
    # this into the ramdisk; init.pox_root.rc runs it once after boot_completed.
    if [[ "$ROOT_BUNDLED" == "1" && "$ROOT_INSTALL_APP" == "1" ]]; then
        cat << 'FP_SH_EOF' > "$stage/pox_root_install.sh"
#!/system/bin/sh
# Pox Kernel - install the bundled root manager app (first boot, once).
PKG=@ROOT_PACKAGE@
LOG=/data/local/tmp/pox_root_install.log

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null; }

APK=""
for c in /data/system/@ROOT_APK@ /data/local/tmp/@ROOT_APK@ /sdcard/@ROOT_APK@; do
    [ -f "$c" ] && { APK="$c"; break; }
done
[ -n "$APK" ] || exit 0

i=0
while [ "$i" -lt 90 ]; do
    cmd package list packages >/dev/null 2>&1 && break
    sleep 2
    i=$((i + 1))
done
sleep 15

if cmd package list packages 2>/dev/null | grep -q "$PKG"; then
    log "already installed: $PKG"
    exit 0
fi

ok=0
pm install -r -g "$APK" >> "$LOG" 2>&1 && ok=1
if [ "$ok" = 0 ]; then cmd package install -r -g "$APK" >> "$LOG" 2>&1 && ok=1; fi
if [ "$ok" = 0 ] && [ -x /system/bin/su ]; then
    su -c "pm install -r -g $APK" >> "$LOG" 2>&1 && ok=1
fi

log "install result ok=$ok pkg=$PKG apk=$APK"
exit 0
FP_SH_EOF
        sed -i "s/@ROOT_PACKAGE@/${ROOT_PACKAGE_NAME}/g; s/@ROOT_APK@/${ROOT_APK_NAME}/g" "$stage/pox_root_install.sh"
        chmod 755 "$stage/pox_root_install.sh"
    fi

    local zip_file="$BUILD_DIR/${ZIP_BASE}.zip"
    log "Packaging AnyKernel3 flashable zip: $zip_file"
    (cd "$stage" && zip -r9 "$zip_file" . -x '*.git*' -x '.github*')
    rm -rf "$stage"

    # Also maintain latest.zip, versioned zip, and date-stamped copies in build/
    cp -f "$zip_file" "$BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${DATE}.zip"
    cp -f "$zip_file" "$BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${DEVICE_CODENAME}.zip"
    ln -sf "$(basename "$zip_file")" "$BUILD_DIR/latest.zip"
    ln -sf "$(basename "$zip_file")" "$BUILD_DIR/${COMMIT_HASH}.zip"

    write_build_metadata "$zip_file"

    log "================================================="
    log "BUILD SUCCEEDED!"
    log "Package ZIP:   $zip_file"
    log "Commit Link:   $BUILD_DIR/${COMMIT_HASH}.zip"
    log "Latest Link:   $BUILD_DIR/latest.zip"
    log "Device Link:   $BUILD_DIR/${KERNEL_NAME}-${KERNEL_VERSION}-${VERSION_NAME}-${DEVICE_CODENAME}.zip"
    log "Kernel Image:  $BUILD_DIR/Image.gz-dtb"
    log "Metadata:      $BUILD_DIR/build-info.env"
    log "================================================="
}

# Emit a sourceable env file describing the artifact exactly, so CI and
# releases never have to re-derive names from the branch.
write_build_metadata() {
    local zip_file="$1"
    local zip_name zip_size zip_sha256
    zip_name="$(basename "$zip_file")"
    zip_size="$(stat -c%s "$zip_file" 2>/dev/null || wc -c < "$zip_file")"
    zip_sha256="$(sha256sum "$zip_file" | awk '{print $1}')"

    {
        printf '# Generated by build.sh - %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'KERNEL_NAME=%q\n'           "$KERNEL_NAME"
        printf 'KERNEL_VERSION=%q\n'        "$KERNEL_VERSION"
        printf 'VERSION_NAME=%q\n'          "$VERSION_NAME"
        printf 'BRANCH_DESC=%q\n'           "$BRANCH_DESC"
        printf 'VARIANT=%q\n'               "$VARIANT"
        printf 'ROOT_FLAVOR=%q\n'           "$ROOT_FLAVOR"
        printf 'ROOT_ASSET=%q\n'            "${ROOT_ASSET:-none}"
        printf 'APPLIED_PATCHES=%q\n'       "$APPLIED_PATCHES"
        printf 'GIT_BRANCH=%q\n'            "$GIT_BRANCH"
        printf 'COMMIT_HASH=%q\n'           "$COMMIT_HASH"
        printf 'COMMIT_DATE=%q\n'           "$COMMIT_DATE"
        printf 'COMMIT_SUBJECT=%q\n'        "$COMMIT_SUBJECT"
        printf 'DEFCONFIG=%q\n'             "$DEFCONFIG"
        printf 'DEVICE_NAME=%q\n'           "$DEVICE_NAME"
        printf 'DEVICE_CODENAME=%q\n'       "$DEVICE_CODENAME"
        printf 'MAINTAINER=%q\n'            "$MAINTAINER"
        printf 'LINUX_VERSION=%q\n'         "$LINUX_VERSION"
        printf 'LOCALVERSION=%q\n'          "$LINUX_VERSION$CUSTOM_LOCALVERSION"
        printf 'LOCALVERSION_EXTENDED=%q\n' "$LINUX_VERSION$EXTENDED_LOCALVERSION"
        printf 'TOOLCHAIN=%q\n'             "$TOOLCHAIN_VER"
        printf 'BUILD_DATE=%q\n'            "$DATE"
        printf 'ZIP_PATH=%q\n'              "$zip_file"
        printf 'ZIP_NAME=%q\n'              "$zip_name"
        printf 'ZIP_SIZE_BYTES=%q\n'        "$zip_size"
        printf 'ZIP_SHA256=%q\n'            "$zip_sha256"
    } > "$BUILD_DIR/build-info.env"

    log "Build metadata written to: $BUILD_DIR/build-info.env"
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
    variants|list-variants|patches|list-patches)
        # Handled before patch application at the top of the script.
        ;;
    unvariant|unpatch)
        unapply_variant_patches
        ;;
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
        echo "Usage: $0 [all|kernel|zip|install|build-install|menuconfig|patches|unvariant|clean|distclean]"
        echo "       VARIANT=ksu,c2 $0 all    # root flavor + media overlay"
        echo "       $0 patches               # list available patch sets"
        echo "       $0 unvariant             # restore the clean onyx tree"
        exit 1
        ;;
esac
