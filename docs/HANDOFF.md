# Handoff — Pox Kernel (charging-bypass fix · identity rewrite · 1.0.1-Beta round)

**Date:** 2026-10-16 · **Workspace:** ephemeral Colab — nothing local survives session end
**Repo:** `POX-project/pox-begonia-kernel` · **Branch when writing:** `docs/handoff` (this document replaces the onyx-kaeru round notes, still in history at `7379f737ab5a`)

---

## 1. Status: complete and verified

| Item | Result |
|---|---|
| Charging-bypass kernel fix authored | `76ec379a03ab`, root cause in §3 |
| Fix cherry-picked (`-x`) + pushed to all 9 feature branches | done, all tips verified via `ls-remote` |
| Contributor identity rewrite (`methylamphetamine`/`methylphenethamine` → `methylphetamine`) | done on 7 branches, tree-identical, force-with-lease pushed |
| `onyx-c2-ksun` branch created (KSU-Next base + 3 Saikrishna1504 ports + bypass fix) | done, pushed |
| `onyx-c2-ksun` misnamed/unrooted CI build discovered + fixed in `build.sh`/workflow | done, rebuild **success** (run `38907194901`), rooted defconfig confirmed in log |
| New release `v1.0.1-Beta` created with onyx-family-only assets | done, 9 assets, checksums verified |
| `v1.0.0-Beta` cleaned (stale/old-SHA/handoff assets removed) | done, mirrors new release |
| `Pox-0.9-Granite-0f7e764` release deleted (tag kept) | done |
| `hos3-fix` CI build | **failed** (run `38902712274`) — uninvestigated, excluded from releases anyway, §8 |

---

## 2. What shipped this round

1. **Charging-bypass fix** — the dual-switch charger state machine could be knocked out of `CHR_ERROR` by asynchronous charger events while a charge-disable (bypass) request was active, silently re-enabling charging. Fixed and shipped to every branch that has the feature (§3).
2. **Identity rewrite** — all commits authored/committed as `methylamphetamine` or `methylphenethamine` (and their noreply email) were rewritten to the token-owner identity (§4). Applied to 7 branches; `granite`/`main`/`obsidian`/`onyx`/`hos3-fix` had no affected commits.
3. **`onyx-c2-ksun`** — new edition: `onyx-ksu-next` base (post-rewrite `a17a72d86b9a`) + 3 ported Saikrishna1504 commits + bypass fix (§5).
4. **Releases** — `v1.0.1-Beta` published (mainline style, similar body to `v1.0.0-Beta`), old release cleaned, Granite release deleted. Explicit exclusion directive from owner: **no hos3-fix, granite, obsidian, docs/handoff assets in releases** (§6).

---

## 3. Charging-bypass fix — what the bug was

File: `drivers/power/supply/mediatek/charger_begonia/mtk_dual_switch_charging.c`
Commit: `76ec379a03ab` — *fix(power): keep charging off while charge disable is active*

**Root cause:** `mtk_dual_switch_charging_run()` dispatches on `swchgalg->state` without consulting `info->can_charging`. Asynchronous events — EOC notifier (line 1271), recharge detection, and `mtk_dual_switch_charging_plug_in` (line 935, itself triggered by `_disable_all_charging()`'s PD/PE reset) — moved the state out of `CHR_ERROR` while a charge-disable (bypass) was active. `do_charging(false)` fires only on a *transition*, so nothing re-disabled the charger, and `dual_swchg_turn_on_charging()` re-enabled it with bypass still on. (`swchgalg->disable_charging` is write-only — never read.)

**Fix:** clamp the state every polling tick, before the top-level switch:

```c
if (!info->can_charging)
    swchgalg->state = CHR_ERROR;
```

With `can_charging == false` the state machine cannot leave `CHR_ERROR`, so `_disable_all_charging` holds until the disable is released.

**Not compile-tested locally** — no cross toolchain in this workspace (only host `gcc`). CI built the branch (success) and produces the flashable zip. Device-level validation of the bypass toggle is still recommended.

---

## 4. Identity rewrite — procedure and safety net

**Scope (owner decision "All my variants"):**
- Author & committer name: `methylamphetamine` + `methylphenethamine` → `methylphetamine`
- Email: `methylamphetamine@users.noreply.github.com` → `methylphetamine@users.noreply.github.com`
- `rootrequired@proton.me` (repo-local email) kept; contributor identities (`Saikrishna1504`, `TXO R <ravtx12best@gmail.com>`, `pacman-Sy`) untouched — only their *committer* was renamed where it had been methylamphetamine.

**Method:** single `git filter-branch --env-filter` run over 7 branches with explicit boundaries:

| Branch | Boundary |
|---|---|
| `onyx-ksu-next` | `204fa25872bc..onyx-ksu-next` |
| `onyx-kaeru` | `e96991fefa93..onyx-kaeru` |
| `docs/handoff` | `e96991fefa93..docs/handoff` |
| `onyx-apatch` | `3d4947f095c6..onyx-apatch` |
| `onyx-bakasu` | `bfa9eacc14c1..onyx-bakasu` |
| `onyx-resukisu` | `bfa9eacc14c1..onyx-resukisu` |
| `onyx-sultan-apatch` | `95c80f57ab0c..onyx-sultan-apatch` |

**Verification:** for every branch `git diff <old-tip> <new-tip>` produced empty output (tree-identical), and a log scan found 0 remaining bad identities. Rollback refs remain locally under `refs/original/refs/heads/*` (7 refs). Remote updates used `--force-with-lease`; `onyx`/`hos3-fix` pushed fast-forward (no rewrite there).

**Gap:** rewritten commits' SHAs embedded in *old* release asset filenames (`…-0790438…`, `…-ed1ba40…`, etc.) no longer exist in the repo. Those assets were deleted in this round's cleanup (§6), so the filenames are historical only.

---

## 5. `onyx-c2-ksun` edition

**Composition** (base = `onyx-ksu-next` post-rewrite tip `a17a72d86b9a`, before its own bypass-fix cherry-pick `577c016a89e9`):

| SHA | Author | Subject |
|---|---|---|
| `87a5b2975ae3` | Saikrishna1504 | cpufreq: mtk: Fix uninitialized return value and remove sched_set_boost override |
| `8ef08bda1766` | Saikrishna1504 | media: mtk-vcodec: mt6785: Release MMQoS frequency request when idle |
| `e50f4bc1d8a1` | Saikrishna1504 | drivers: pmic: Restore CC mode on LED off and enable LED in blink_set |
| `76ec379a03ab` | methylphetamine | fix(power): keep charging off while charge disable is active |
| `baf92122cf1d` | methylphetamine | fix(build): map onyx-c2-ksun to KernelSU-Next defconfig and edition name |
| `4939a70f3f32` | methylphetamine | ci(workflow): add onyx-c2-ksun edition metadata for release titles |

Ports applied with `git am --keep-non-patch` (authorship preserved). The vcodec MMQoS port required a manual merge: replaced the min-step PM-QoS downgrade with `PM_QOS_DEFAULT_VALUE` release, gated on an idle sentinel (`freq_idx = -1` when the vote lanes are NULL, i.e. truly idle).

**Build-naming trap (fixed this round):** `build.sh`'s `case "$GIT_BRANCH"` had no `onyx-c2-ksun` entry, so the first build fell through to the unrooted fallback: `begonia_user_defconfig` (no `CONFIG_KSU`/SUSFS) + plain `Onyx` zip label. Fixed by adding the explicit case in `build.sh` (KSU-Next defconfig, `VERSION_NAME="Onyx-c2-KSU-Next"`) and mirroring it in `.github/workflows/build-release.yml`. Rebuild log confirms `Defconfig: begonia_ksu_next_defconfig`. **If more `onyx-*` branches are added, check both `case` statements first.**

---

## 6. Branches, tags and releases (state at writing)

### Branch tips (remote verified via `ls-remote`)

```
docs/handoff        34939848484c   (+ this handoff commit on top)
granite             0f7e764da9ca   untouched — no bypass feature, excluded from releases
hos3-fix            e5667b788d01   bypass fix cherry-picked; CI build FAILED (§8)
main                8ca0974ac378   untouched; v1.0.1-Beta tag points here
obsidian            bc4891b5ff2b   untouched, excluded from releases
onyx                f23770c33b7a   bypass fix cherry-pick
onyx-apatch         6fafc8ade3d2
onyx-bakasu         2335557877dc
onyx-c2-ksun        4939a70f3f32   NEW (§5)
onyx-kaeru          d09695309194
onyx-ksu-next       577c016a89e9
onyx-resukisu       c5cbc3ec9662
onyx-sultan-apatch  751f9508c5ac
```

Bypass-fix cherry-pick SHAs per branch: `onyx-c2-ksun 76ec379a03ab` (original) · `onyx-ksu-next 577c016a89e9` · `onyx-kaeru d09695309194` · `docs/handoff 34939848484c` · `onyx-apatch 6fafc8ade3d2` · `onyx-bakasu 2335557877dc` · `onyx-resukisu c5cbc3ec9662` · `onyx-sultan-apatch 751f9508c5ac` · `onyx f23770c33b7a` · `hos3-fix e5667b788d01`.

### Tags

- `v1.0.0-Beta` → `64d1a25643c1` (old release, still live)
- `v1.0.1-Beta` → `8ca0974ac378` (main tip at creation)
- `Pox-0.9-Granite-0f7e764` → `8ca0974ac378` (release deleted, tag deliberately kept)

### Releases

`v1.0.1-Beta` (id `414974995`) and `v1.0.0-Beta` (id `409148624`) both carry the **same 9 assets** — latest build of each published edition, onyx-family only:

| Asset | SHA-256 |
|---|---|
| `Pox-1.0.0-Beta-Onyx-Sultan-APatch-751f950-begonia.zip` | `c6ffb20a5086aa2e9fd38830b7c2269666318dcb2f7974a3e1458a61b085a000` |
| `Pox-1.0.0-Beta-Onyx-APatch-6fafc8a-begonia.zip` | `f3ee68d4ff134869c1fcd084eafd00bd1bdc0aff3822d888c918ba049e9ce660` |
| `Pox-1.0.0-Beta-Onyx-KSU-Next-577c016-begonia.zip` | `218d150dc211f8f75eef9ae88d1b71bbbe3623a20ae2392419db9088cc077fce` |
| `Pox-1.0.0-Beta-Onyx-ReSukiSU-c5cbc3e-begonia.zip` | `59c83c5c7229296abc5d02384d0247a50fce963683f901f7482bfcb734628b1f` |
| `Pox-1.0.0-Beta-Onyx-BakaSU-2335557-begonia.zip` | `5cc3d73e984dc9b91e18e546ec381da22057921f048fc3a53d68e1aee1ff289e` |
| `Pox-1.0.0-Beta-Onyx-c2-KSU-Next-4939a70-begonia.zip` | `fa74bfeb324ab05b99945bf688b13133df6613e48a2ed3f89dcf259b0cc78d15` |
| `Pox-1.0.0-Beta-Onyx-f23770c-begonia.zip` | `63c06876cb8d62392eb0e0a418c80cd8011d4e8222c4015df30f32f27799484d` |
| `Pox-0.9-Onyx-Kaeru-d096953-begonia.zip` | `196f9091d3a7f9679189a8f1d065cae045e69d4558de59aa7caccb3810ec7d2c` |
| `Pox-Kernel-Manager-v1.0.0-Beta.apk` | `8d6e5bcd434cb783b23ad44e1af4f322a8c8ffad45348a7e023bb415a446a617` |

All digests were taken from the release API `digest` field and round-trip verified for locally downloaded copies before upload. Deleted from `v1.0.0-Beta` this round: 7 old-SHA zips (`0790438`, `ed1ba40`, `582353f`, `cf04d49`, `356f550`, `926c734`, `0120e85`), the handoff-fallback zip (`Pox-0.9-Onyx-3493984`), and the superseded unrooted c2 zip (`…Onyx-76ec379`).

`v1.0.1-Beta` title: `Pox Kernel 1.0.1-Beta [Mainline Release] 🇵🇸`; body adapts the old release notes with a new "Latest Build" section (bypass fix + c2 refresh) and the table above. The `v1.0.0-Beta` **body was not updated** — it still references deleted filenames (§8).

---

## 7. CI behaviour (as observed this round)

- Trigger: push to **any** branch (`branches: ['**']`), build ~12–15 min, `cancel-in-progress: true` per ref.
- Every branch push runs "Upload to Mainline Release (v1.0.0-Beta)" step: `gh release upload … --clobber || true`. Since zip filenames embed the short SHA, `--clobber` only replaces identical names — **every rebuild adds a new asset instead of replacing the old one.** Someone must delete superseded assets each round. The upload target tag is hardcoded `v1.0.0-Beta`.
- Push of a `v*` **tag** creates a separate per-tag single-asset release (`Publish Tagged Release` step, runs only on tags). Creating a tag via the releases API does not trigger the workflow.
- `docs/` and dev branches still get full kernel builds and auto-upload a fallback `Onyx` zip (e.g. `Pox-0.9-Onyx-<sha>` for `docs/handoff`). **This very handoff push triggered one; its uploaded asset was deleted from `v1.0.0-Beta` after the run.**
- Builds this round: 9 of 10 first-pass runs success (`docs/handoff`, `onyx`, `sultan-apatch`, `resukisu`, `bakasu`, `apatch`, `kaeru`, `ksu-next`, `c2-ksun`); `hos3-fix` failed (§8).

---

## 8. Outstanding — none blocking, all real

1. **Rotate the GitHub PAT.** The `ghp_` classic PAT used in-session (stored transiently at `/tmp/opencode/token_owner`-adjacent use, never written into `.git/config` — remote URL was sanitized back to plain HTTPS) can push, edit releases and delete assets. It is still live. Revoke at `https://github.com/settings/tokens`. Value deliberately **not** reproduced here.
2. **`hos3-fix` CI failure.** Run `38902712274` failed (`docs/handoff` run `38902548230` succeeded, so the environment is fine). Not investigated — `hos3-fix` is excluded from releases; debug only if that edition ever ships.
3. **`v1.0.0-Beta` release body is stale.** It still lists old filenames and hashes for assets deleted in the cleanup. Left intentionally (historical), but do not trust its table; use `v1.0.1-Beta` or this document.
4. **CI junk-asset loop.** Any branch push (including this docs branch) adds a fallback-name asset to `v1.0.0-Beta`. Real fix: branch/paths filter in the upload step (e.g. skip for `docs/**`), or make CI name/upload per-edition only. Until then, delete the extra asset after each docs/dev push.
5. **Version prefix mismatch (cosmetic).** All CI package names (and the release-note filenames) still embed `Pox-1.0.0-Beta` / `Pox-0.9` prefixes because `KERNEL_VERSION` is hardcoded in `build.sh` (`1.0.0-Beta`) even though the new release is `v1.0.1-Beta`. Decide whether to bump `KERNEL_VERSION` (renames everything) or leave as-is.
6. **Node 20 deprecation** on `actions/*@v4` runners was still warned earlier; likely still open.
7. **Device-side validation of the bypass fix** on real hardware (toggle bypass in Pox Kernel Manager, confirm charge stays off across plug/unplug/EOC) — not done in this session.

---

## 9. Reproduction notes & gotchas (this sandbox)

- **`gh` CLI is available** (`/usr/bin/gh`) and worked for `release create/upload/edit/delete`, `run view --log`, `api -X DELETE`. Auth via `GH_TOKEN` env; never `gh auth login`.
- **Push pattern:** `git push "https://${OWNER}:${TOKEN}@github.com/POX-project/pox-begonia-kernel.git" <branch>` (owner from `/tmp/opencode/token_owner`). Sanitize `origin` back to plain HTTPS afterwards — leave no token in `.git/config` (done this round).
- **Checksums without downloading:** the release asset API exposes a `digest` field (`sha256:…`) — use it for release tables; still `sha256sum`-verify downloaded files before re-uploading.
- **Release asset "copy" between releases is manual:** download from `v1.0.0-Beta` (`gh release download <tag> -p <name> -D <dir>`) then upload to the new tag. No server-side copy exists (asset URLs need auth and re-upload).
- **Polling:** long foreground loops fine; keep `sleep 45` and an explicit bash `timeout` of up to `900000` ms. Background jobs get killed with the tool call — don't rely on them.
- **`gh release delete <tag> --yes` keeps the git tag** (delete with `--cleanup-tag` only if intended).
- **`git am --keep-non-patch`** for third-party ports (preserves authorship, drops `[PATCH]` cruft); use `-3` + real remote blobs when context mismatches.
- **`git filter-branch`** wants `FILTER_BRANCH_SQUELCH_WARNING=1`; always diff old-vs-new tip per branch to prove tree identity before force-pushing.
- **AGENTS.md mandates** conventional commits **with rationale body**, atomic commits (this round split the edition fix into `baf9212` build.sh + `4939a70` workflow rather than one monolith), kernel 8-tab style, minimal diffs. No compile validation is possible in this workspace (host `gcc` only) — CI is the build gate.

---

## 10. Artefacts (ephemeral — recreate if needed)

```
/content/pox-begonia-kernel/               clone (docs/handoff checked out)
/tmp/opencode/token_owner                  PAT owner username ("methylphetamine")
/tmp/opencode/patches/                     the 3 ported Saikrishna1504 patches (now applied)
/tmp/opencode/old_release.json             full old v1.0.0-Beta API object
/tmp/opencode/old_release_body.md          old release notes (source for new body)
/tmp/opencode/new_release_body.md          v1.0.1-Beta release notes (published)
/tmp/opencode/release_assets/              staged zips + APK + c2 zip (downloaded for upload)
/tmp/opencode/local_digests.txt            sha256 list of staged assets
/tmp/opencode/poll_ci.sh, ci_poll.log      CI polling helper from earlier rounds
```

Local rollback refs: `refs/original/refs/heads/*` (7) from the identity rewrite — keep until the force-with-lease pushes are considered settled.
