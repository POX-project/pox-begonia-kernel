---
description: Rules for modifying code and managing active tasks
globs: "**/*"
---

# Active Task & Edit Safety Rule

1. **NEVER modify files while a task is running**:
   - When any build (`./build.sh`), compiler (`clang`, `make`), package step, flashing process (`adb sideload`), or benchmark script is active, all file modifications (edits, writes, deletions, checkouts, rebases) are STRICTLY FORBIDDEN.
   - Wait for the background task to finish and verify exit code 0 before touching any code.

2. **Clean working state**:
   - Always verify the working tree is clean (`git status`) before starting a build.
   - Never leave uncommitted scratch edits before initiating long compilation jobs.
