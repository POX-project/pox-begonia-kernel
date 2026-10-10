---
description: Git hygiene, commit message formatting, and pull request standards
globs: "**/*"
---

# Anti-Spaghetti Git & Commit Hygiene

1. **Conventional Commits Mandatory**:
   - Format: `<type>(<scope>): <summary>`
   - Allowed types: `feat`, `fix`, `refactor`, `perf`, `chore`, `docs`, `ci`
   - Allowed scopes: `sched`, `power`, `fs`, `build`, `sultan`, `apatch`, `ksu`, `dts`, etc.
   - Example: `fix(sched): remove const_debug from sysctl_sched_migration_cost`

2. **Atomic & Bisectable Commits**:
   - Exactly one logical change per commit.
   - Never create kitchen-sink commits bundling unrelated changes.
   - Every commit must leave the tree in a compilable state.
   - Mandatory detailed body explaining the technical rationale.

3. **No WIP / Lazy Commits**:
   - Never commit messages like "fix", "wip", "update", "temp", "changes".
   - Maintain clean linear history (rebase, no unnecessary merge commits).
