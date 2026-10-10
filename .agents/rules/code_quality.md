---
description: Code quality and architectural constraints for kernel modifications
globs: "**/*"
---

# Anti-Spaghetti Code Standards

1. **Linux Kernel Standards**:
   - 8-space tabs, standard Linux kernel style (`Documentation/process/coding-style.rst`).
   - Clean, minimal diffs targeting the exact root cause.
   - No copy-pasted blocks or redundant duplicate implementations.

2. **Hardware & Architecture Invariants**:
   - `UTS_RELEASE` $\le$ 64 characters: Clamp kernel release names in `build.sh` to never overflow.
   - No `.rodata` on vendor-writable sysctls: Variables modified by userspace HALs (e.g., `sysctl_sched_migration_cost`) must NEVER be `const` or `const_debug`.
   - USB SDP limits: Standard PC USB host ports (`STANDARD_HOST`) must strictly limit charging input current to 500mA (500000 µA). High currents (3.0A / 18W) are strictly for dedicated AC chargers.
   - Sultan Debloat Isolation: `CONFIG_SULTAN_DEBLOAT=y` is strictly exclusive to `onyx-sultan*` branches. It must NEVER be added to clean `onyx*` branches.
   - Root Mechanism Isolation: APatch and KernelSU-Next must be maintained on separate branches without mixing or leftover dead hooks.
