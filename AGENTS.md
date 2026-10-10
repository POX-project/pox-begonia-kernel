# Agent Rules & Repository Governance

This document establishes the **mandatory, non-negotiable rules** for all AI agents, contributors, and automated systems working on the Pox Kernel repository (`pox-kernel-begonia`).

---

## 1. Concurrency & Mutation Safety: NEVER Edit While a Task is Running
* **Absolute File Freeze**: You must **NEVER** edit, create, delete, rebase, checkout, or stage any file while a build (`build.sh`, `make`), flash (`adb sideload`), test script, benchmark, or background process is actively executing.
* **Corrupted Builds**: Modifying source files during compilation produces non-deterministic artifacts, broken headers, and invalid object states.
* **Verify Idle State**: Before calling any file modification tool, verify that all background tasks and compiler processes have exited cleanly (`exit code 0`).

---

## 2. Anti-Spaghetti Code Standards (Kernel Quality)
* **Linux Kernel Coding Style**: Adhere strictly to upstream kernel conventions (`Documentation/process/coding-style.rst`):
  * 8-space tabs for indentation, never spaces.
  * Opening braces on the same line for functions/blocks following kernel standards.
  * Descriptive variable and symbol naming.
* **Minimal Targeted Diffs**: Do not touch unrelated files or perform gratuitous reformatting. Solve the exact issue with the smallest sound patch.
* **No rodata / Write-to-Const Panics**:
  * Any sysctl or kernel parameter modified by user-space or vendor HALs (such as Xiaomi's `NodeLooperThread` power HAL) must **never** be marked `const` or `const_debug`.
  * E.g., `sysctl_sched_migration_cost` must remain writable (`unsigned int sysctl_sched_migration_cost`), otherwise write attempts trigger an **EL1 Data Abort** panic.
* **Hardware & Electrical Spec Compliance**:
  * Standard PC USB (`STANDARD_HOST`) charging current must strictly respect the 500mA (500000 µA) USB 2.0 SDP specification.
  * Forcing high currents (e.g. 1.2A+) on standard PC USB collapses VBUS voltage and triggers host motherboard overcurrent protection (causing repeated USB disconnect/reconnect cycles).
  * High-power fast charging (3.0A / 18W) is strictly for dedicated AC wall chargers (`STANDARD_CHARGER` / DCP) and dedicated charging ports.
* **Linux 64-Character UTS_RELEASE Hard Limit**:
  * The Linux kernel enforces `UTS_RELEASE` length $\le 64$ characters.
  * Localversion strings must be clamped (e.g., `-Pox-1.0.0-Beta-<Edition>-<hash>`) to never exceed this limit.
* **Strict Branch Isolation**:
  * `CONFIG_SULTAN_DEBLOAT=y` is strictly exclusive to Sultan branches (`onyx-sultan*`). Never enable or leak it into standard `onyx*` branches.
  * APatch, KernelSU-Next, and clean unrooted branches must remain cleanly separated. Do not mix root hooks or leave broken stubs.
* **Zero Leftovers**: Never leave debug printk floods, scratch scripts, or ad-hoc binaries in committed code.

---

## 3. Anti-Spaghetti Commits (Git Hygiene)
* **Strict Conventional Commits**:
  * Format: `<type>(<scope>): <imperative summary>`
  * Valid types: `feat`, `fix`, `refactor`, `perf`, `chore`, `docs`, `ci`.
  * Valid scopes: `sched`, `power`, `fs`, `build`, `sultan`, `apatch`, `ksu`, `dts`, etc.
  * Example: `fix(power): enforce 500mA USB SDP limit to prevent host overcurrent trip`
* **Atomic Commits**: Exactly one logical change per commit. Never combine build script edits, driver fixes, and defconfig updates into a single monolithic commit.
* **No Lazy / WIP Commits**: Commits named `fix`, `wip`, `update`, `temp`, `test`, or `patch` are strictly forbidden.
* **Mandatory Rationale in Commit Body**: Every commit message must include a body explaining **why** the change was made and the technical rationale.
* **Linear History**: Rebase cleanly on the target branch. Do not create merge commits of main into feature branches.
* **Bisect-Friendly**: Every single commit must compile cleanly without breaking the build.

---

## 4. Pull Request (PR) Checklist & Quality Gates
Before submitting any Pull Request:
1. [ ] **Build Validation**: The kernel compiles completely with zero errors and zero fatal warnings using the repo toolchain.
2. [ ] **Packaging**: `build.sh zip` succeeds and produces a valid flashable AnyKernel3 package.
3. [ ] **UTS_RELEASE Check**: `include/generated/utsrelease.h` is $\le 64$ characters.
4. [ ] **Target Branch Verified**: The PR targets the correct branch in the 6-branch matrix (`onyx`, `onyx-apatch`, `onyx-ksu-next`, `onyx-sultan`, `onyx-sultan-apatch`, `onyx-sultan-ksu-next`).
5. [ ] **Clean Git History**: Commits follow Conventional Commits and contain no merge commits or WIP clutter.
6. [ ] **No Regression**: Hardware charging limits, boot stability, and HAL sysctls are verified.
