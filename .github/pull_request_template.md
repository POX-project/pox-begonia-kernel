## Pull Request Description

### Summary
<!-- Provide a concise summary of the change and the branch being targeted. -->

### Technical Rationale
<!-- Explain why this change is necessary and the problem it solves. -->

---

## Quality Gate Checklist

### Task & Mutation Safety
- [ ] No file edits or modifications were made while builds/tests were actively executing.
- [ ] The working tree was verified clean before and after changes.

### Kernel Code Quality
- [ ] Conforms to Linux kernel coding style (8-space tabs, clean formatting).
- [ ] No `.rodata` writes: All variables modified by vendor HALs/procfs remain writable.
- [ ] Complies with USB electrical limits (PC USB SDP current limited to 500mA).
- [ ] Kernel version string does NOT exceed the 64-character `UTS_RELEASE` limit.
- [ ] Strict branch architecture maintained (`CONFIG_SULTAN_DEBLOAT` only on Sultan branches).
- [ ] No dead code, scratch scripts, or debug printk spam committed.

### Git & Commit Hygiene
- [ ] Commits strictly adhere to Conventional Commits: `<type>(<scope>): <summary>`.
- [ ] Commits are atomic with explanatory body text.
- [ ] Rebased cleanly onto the target branch with no merge commits.
- [ ] Every commit compiles cleanly (bisectable).

### Verification
- [ ] Kernel compilation tested and passed with 0 errors.
- [ ] AnyKernel3 zip generated successfully via `./build.sh zip`.
