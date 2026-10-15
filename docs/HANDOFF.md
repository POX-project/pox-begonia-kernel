# Handoff — Pox Kernel `onyx-kaeru` (TRB Clang 18 round)

**Date:** 2026-10-15 · **Workspace:** ephemeral Colab — nothing local survives session end
**Repo:** `POX-project/pox-begonia-kernel` · **Branch:** `onyx-kaeru` @ `07904385f098`

---

## 1. Status: complete and verified

| Item | Result |
|---|---|
| Clone `onyx-kaeru` | done |
| Apply 3 commits from `pacman-Sy/pox-kernel-begonia` | done (2 deviations, §3) |
| Toolchain pin | commit `07904385f098`, verified in CI log |
| Push | `e96991fefe93..07904385f098` fast-forward, no force |
| CI build | run `38737009559` **success**, 23m4s, all steps green |
| Release asset replaced | kaeru only, others untouched |
| `.sha256` published + verified | round-trip `sha256sum -c` passes |
| Release notes updated | 1 line, verified via API |
| Diff/changelog docs vs source | gist, content verified via API |

---

## 2. The four commits now on `onyx-kaeru`

| SHA | Author | Subject |
|---|---|---|
| `d01c2b007af1` | pacman-Sy | fix fortify OOB errors exposed by Clang 18 object-size analysis |
| `6718f7cc0cb9` | pacman-Sy | kaeru: build Onyx+Nomount VFS base with TRB Clang 18 |
| `8b027708061f` | pacman-Sy | workflow: use libncurses-dev on ubuntu-24.04 |
| `07904385f098` | methylamphetamine | kerdevdep: pin TRB Clang toolchain to an exact revision |

The three `pacman-Sy` commits preserve original authorship (`git am --keep-non-patch`); I am committer only on the fourth.

### What the fortify commit actually fixes
Three genuine OOB reads that Clang 18's stricter `__builtin_object_size` surfaced:
- `kernel/power/process.c` — `memcmp(p->comm, "mfp-daemon", 13)` read 2 bytes past a 10-byte literal → count `10`
- `.../gen4m/os/linux/gl_qa_agent.c` — `u4LqReport[]` declared `uint8_t` but indexed and `memcpy`'d as `uint32_t` → element type `uint32_t`
- `.../mt6785/dispsys/ddp_dsi.c` — `memcpy(dst, "", 64)` copied 64 bytes from an empty literal → `memset(dst, 0, 64)`

Without these the final `vmlinux` link fails (`__read_overflow2` undefined).

---

## 3. Deviations from the original request — read before rebasing

**a) Commit order had to change.** Requested order was `b24e886 → 2d6b7a1 → 2e1472c`. That cannot work: `2d6b7a1`'s hunk context requires the `libncurses6-dev libncurses6 pahole` line that only `2e1472c` creates. Applied chronologically: `b24e886 → 2e1472c → 2d6b7a1`.

**b) Two hunks were dropped (semantic, not mechanical).** `2e1472c` adds a `kaeru)` case meaning *TRB Clang edition*. The `onyx-kaeru` branch **already** defines `kaeru` as *Kaeru bootloader* (`build.sh:65`, `build-release.yml:93`). Bash `case` is first-match-wins, and `*kaeru*` precedes the patch's `kaeru)`, so a literal apply produced **unreachable dead code** in both files. I removed both blocks; the existing `Onyx-Kaeru` bootloader edition is untouched.

Net effect: TRB Clang still applies (that lives in `env.sh` / `build.sh` / `setup_kerdevdep.sh`, independent of the case label), but there is **no separate `kaeru` branch**. If a distinct `kaeru` = TRB-edition branch is ever wanted, the patterns must be reordered so `kaeru)` precedes `*kaeru*`.

**c) Toolchain was pinned, per your decision.** `2e1472c` replaced a pinned AOSP tarball with `git clone --branch test` of `voltageos/clang` — a mutable third-party branch compiled by CI that holds `contents: write` and publishes artifacts. Now fetched blobless, checked out detached at `b0fa406b3f65eae210c60dda2e91dae979e4b3a8`, with `rev-parse` verification and a loud failure if the pin becomes unreachable.

To update the pin:
```sh
git ls-remote https://gitlab.com/voltageos/clang.git test
# then set TRB_CLANG_PIN in kerdevdep/setup_kerdevdep.sh
```

---

## 4. Release `v1.0.0-Beta` — current state

Single shared release, 9 assets. One per branch plus the manager APK.

```
Pox-0.9-Onyx-Kaeru-0790438-begonia.zip          21,889,681 B   <- replaced
Pox-0.9-Onyx-Kaeru-0790438-begonia.zip.sha256         105 B   <- added
Pox-1.0.0-Beta-Onyx-APatch-ed1ba40-begonia.zip  40,964,698 B
Pox-1.0.0-Beta-Onyx-BakaSU-582353f-begonia.zip  21,858,136 B
Pox-1.0.0-Beta-Onyx-cf04d49-begonia.zip         23,946,768 B
Pox-1.0.0-Beta-Onyx-KSU-Next-356f550-begonia.zip 21,882,352 B
Pox-1.0.0-Beta-Onyx-ReSukiSU-926c734-begonia.zip 21,867,707 B
Pox-1.0.0-Beta-Onyx-Sultan-APatch-0120e85-begonia.zip 40,930,386 B
Pox-Kernel-Manager-v1.0.0-Beta.apk               18,661,668 B
```

New kaeru SHA-256:
```
d14954d9481c1f731e0e9ad145fb23fdb450212facfd756523dae73e5f45b989
```

Release notes: exactly one line changed (the Onyx-Kaeru table row → new filename + hash + `— TRB Clang 18 toolchain`).

**Note:** the kaeru filename embeds the commit SHA (`CUSTOM_LOCALVERSION` in `build.sh:97`) and CI uploads with `--clobber`, which only matches identical filenames. So **every future kaeru build adds a new asset rather than replacing the old one.** Someone must delete the superseded asset each round. That is a workflow gap, not a one-off.

---

## 5. Outstanding — none blocking, all real

1. **Rotate the GitHub PAT.** The `ghp_` classic PAT supplied in-session was used to push, edit the release, and create gists. It is still live and was stored at `/tmp/opencode/.gh_token` (0600, now gone with the workspace). Revoke at `https://github.com/settings/tokens`. Its value is deliberately **not** reproduced in this document.
2. **`onyx-resukisu` will fail to build.** `build.sh:53` sets `DEFAULT_DEFCONFIG="begonia_user_ksu_defconfig"`, which does not exist — only `begonia_apatch_defconfig` and `begonia_user_defconfig` are present in `arch/arm64/configs/`. Untouched; outside the three commits.
3. **Other 7 checksums in the release table are unverified.** The note under the table claims all were recomputed from attached assets; only kaeru actually was. The assets are unchanged so they are probably valid. Can be verified by downloading each and hashing.
4. **Node 20 deprecation.** Runner warns `actions/cache@v4`, `actions/checkout@v4`, `actions/upload-artifact@v4` target Node 20 and are force-run on Node 24. Bump to `@v5`.
5. **CI runs on every branch push** (`branches: ['**']`) with `cancel-in-progress: true` per ref. Pushing a docs branch would trigger a full kernel build and add a junk `Onyx` asset to the shared release. Avoid, or add a paths/branch filter.
6. **Cross-repo diff pipeline is reusable** — see §7.

---

## 6. Diff analysis vs source

Base: `Saikrishna1504/kernel_xiaomi_mt6785` @ `ef187181c597` (`main`, 2026-10-14).
(The other candidate, `device_redmi_begonia-kernel`, is stale since 2024-09 and shares the same pre-divergence ancestry.)

`onyx-kaeru` divergence: **176 files, +14,379 / −1,140, zero binaries.**

Across all 11 branches: 139 branch-unique commits (computed as *not reachable from any other branch tip* — a real lineage changelog, not inferred from branch names). Contributors: `Saikrishna1504`, `TXO R`, `methylamphetamine`/`methylphenethamine`, `pacman-Sy`.

---

## 7. Reproduction notes

**Auth** — do not hardcode the token in `.git/config` or remote URLs. Use an askpass helper:
```sh
printf '#!/bin/sh\ncase "$1" in\n  *Username*) echo "x-access-token" ;;\n  *) cat /path/to/token ;;\nesac\n' > askpass.sh
chmod 700 askpass.sh
GIT_ASKPASS=$PWD/askpass.sh GIT_TERMINAL_PROMPT=0 git push origin onyx-kaeru:onyx-kaeru
```

**Repo moved** — canonical remote is `https://github.com/POX-project/pox-begonia-kernel.git` (was `pox-project/...`). Local remotes: `origin`, and `sai` if the diff work is redone.

**3-way merge against a diverged branch** — `git am -3` fails confusingly when the patch's base blobs are absent ("repository lacks the necessary blob", then misleading line-offset errors). Fix by adding the fork as a remote and fetching the actual commits, so the real base blobs exist:
```sh
git remote add fork https://github.com/pacman-Sy/pox-kernel-begonia.git
git fetch --no-tags fork <sha> [<sha> ...]
git am -3 --keep-non-patch <patch>
```
Then resolve conflicts deliberately.

**Fetching the 1.4 GB source repo cheaply** — depth-1 + blob-limited partial clone; Git then fetches only blobs the diff actually needs:
```sh
git fetch --depth 1 --filter=blob:limit=2m <url> main
git diff --numstat <src> <dst>     # 0 0 (not binary) for every file here
git diff --raw -z <src> <dst>      # blob SHAs; -z splits meta/path on NUL, not TAB
```

**Gotchas hit**
- Unquoted `--format=%h|%an|%s` inside `bash -c` makes the shell pipe it — silently returns empty output. Always quote format strings.
- `api.github.com` and `raw.githubusercontent.com` were reachable from this sandbox; `gist.github.com` / `gist.githubusercontent.com` returned 404 for **both secret and public** gists. Gist content was therefore verified through the API, not by fetching raw URLs.
- `gh repo create` is blocked for this account ("cannot create a repository"). Repo-based hosting is not available; a docs branch in the kernel repo is the only alternative and costs a CI build (§5.5).
- `gh gist edit -f` only edits existing files; adding needs `-a`.
- `Image.gz-dtb` in the AnyKernel3 zip may begin with DTB magic (`0xd00dfeed`), not gzip — don't assume `gzip.decompress` works.

---

## 8. Artefacts (ephemeral — recreate if needed)

Paths existed only inside this Colab instance:

```
/tmp/opencode/pox-begonia-kernel/           clone
/tmp/opencode/patches/*.patch              the 3 upstream patches
/tmp/opencode/out/POX-vs-SAI-CHANGELOG.md  full analysis (35,452 B)
/tmp/opencode/out/POX-vs-SAI-onyx-kaeru.patch  complete diff (656,618 B)
/tmp/opencode/out/POX-RELEASE-SUMMARY.md   Telegraph-ready summary (3,131 B)
/tmp/opencode/askpass.sh                   token helper
```

Both gists were verified byte-identical to their local sources via the GitHub API. Treat the gist links as **unverified from the authoring sandbox** — confirm they resolve before distributing.
