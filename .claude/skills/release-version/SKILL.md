---
name: release-version
description: |
  Deploy or cut a versioned release of Agentmaster — the local deploy inner-loop
  (close → build → relaunch) AND the public GitHub release pipeline (tag or dispatch
  release.yml → monitor → verify the 4 signed assets → apply notes → publish). Encodes
  the hard rules: hold the build mutex, pin `-R Nucs/Agentmaster` (gh defaults to the
  upstream microsoft/terminal remote), confirm the version with the user first, build the
  RELEASE identity, and ALWAYS pre-validate with a clean local Release build (incremental
  Debug builds mask clean-build errors the runner WILL hit). Use when the user asks to
  release a new version, cut/publish a release, ship vX.Y.Z, or deploy the dev instance.
keywords: release, deploy, version, publish, ship, vX.Y.Z, release.yml, ci.yml, msixbundle, gh release, git tag, AgentmasterDev, agentmaster.exe, draft release, build mutex
keywords-sparse: cut a release, release a new version, publish vX.Y.Z, ship it, deploy the app, redeploy
keywords-regex: \brelease\b|\bdeploy\b|\bpublish\b|v\d+\.\d+\.\d+|release\.yml|\.msixbundle
---

# Agentmaster — deploy & release a version

Two distinct workflows. **Path A (local deploy)** = the inner loop that gets your code
into the running dev instance (standing-authorized, no prompt). **Path B (public release)**
= cut a versioned GitHub Release (outward-facing — confirm the version with the user FIRST,
never self-initiate). They share the same pre-flight.

The authoritative source is **`CLAUDE.md`** → *Deploy & run*, *Concurrency lock*, and
*Releasing a public version*. This skill is the runnable checklist; if it ever disagrees with
CLAUDE.md, CLAUDE.md wins — re-read it (it changes).

For the LOCAL machine's build flavors, the production (Release-layout) install, the
self-kill check before closing an instance, and the build/registration troubleshooting
matrix (PRI210, 0x80070020 re-register collisions, stale mutex), see the **`build-install`**
skill — it owns the local how; this skill owns the release pipeline. NOTE: Path B's
pre-validation build (B1) relinks the PRODUCTION instance's Release layout in place —
close production first (path filter `'*\bin\x64\Release\*'`), and re-stamp the layout
version afterwards if you re-register it (build-install §4).

---

## Hard rules (both paths)

1. **Hold the `build-launch` mutex** around any full-exe build, launch, deploy, or closing our
   instance. (A lib-only compile-check does NOT need it.) It's a filesystem lock shared by all
   agents — persists across separate Bash calls.
   ```bash
   TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "release $(git rev-parse --short HEAD)") || exit 1
   # ... whole cycle ...
   bash tools/am-lock.sh release --token "$TOKEN"      # ALWAYS release, even on failure
   ```
2. **`gh` defaults to the WRONG repo.** This checkout has an `upstream` remote =
   `microsoft/terminal`, and `gh` picks it. **Always pass `-R Nucs/Agentmaster`** (or run
   `gh repo set-default Nucs/Agentmaster` once per session). Symptom if you forget: "release not
   found" / `HTTP 404 ... microsoft/terminal`.
3. **`gh` HTTP-trace noise can corrupt `--json`/`--jq`.** If a `--jq` query exits 1 with no
   output, fall back to the plain table form (`gh release view vX.Y.Z -R Nucs/Agentmaster`) and
   read it, or `gh api .../releases/<id>/assets | jq ...`.
4. **Two package identities, one branding** — don't conflate them:
   - **Release** = `Agentmaster` / `agentmaster.exe`, `Package-Rel.appxmanifest`, built with
     `/p:AgentmasterPackageIdentity=Release`. This is what `release.yml` ships.
   - **Dev** = `AgentmasterDev` / `agentmasterdev.exe`, `Package-Dev.appxmanifest` (the default).
     This is the **local loose layout**.
5. **Close ONLY our instance** — path-filter `ExecutablePath -like 'K:\source\Agentmaster\*'`.
   NEVER blanket-kill `WindowsTerminal.exe` (spares the user's Store WT).

---

## Pre-flight (do this before EITHER path)

```bash
cd /k/source/Agentmaster
git status --porcelain          # note modified AND untracked (??) — untracked source isn't in a tag/CI checkout
git log --oneline -5
git fetch origin --quiet; git status -sb | head -1   # branch vs origin (ahead/behind)
```
If there's WIP that committed `.cpp` depends on (a classic break: a committed `TerminalPage.Agent*.cpp`
references a declaration that only lives in an uncommitted `TerminalPage.h` / `AgentManagerContent.h`),
that WIP MUST be committed or the clean build fails. Check it deliberately.

---

## Path A — Local deploy (inner loop)

Standing-authorized ("always auto deploy") — run without prompting. Hold the mutex around the
whole cycle.

```bash
# 0. mutex
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
```
```powershell
# 1. close ONLY our dev instance (spares the Store WT)
Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" |
  ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | % { Stop-Process -Id $_.ProcessId -Force }
# 2. build (full exe link)
pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore
# 3. relaunch the DEV identity
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"   # or: agentmasterdev
```
```bash
# 4. release the mutex
bash tools/am-lock.sh release --token "$TOKEN"
```
- Re-register the loose layout ONLY when `Package-Dev.appxmanifest` changes:
  `Add-AppxPackage -Register ".\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion`
- **One-time identity-split migration** (first deploy after the profiles split): the old loose
  registration still owns `Agentmaster_56k4f06dsfp9r` (now the RELEASE family). `Remove-AppxPackage
  Agentmaster_56k4f06dsfp9r`, then re-register (becomes `AgentmasterDev`). First launch shows the
  **profile picker** — Browse to `~/.agentmaster`, or Development + migrate to `~/.agentmaster-dev`.
  (A GUI picker can't be answered headlessly; set env `AGENTMASTER_PROFILE=<dir>` to bypass, or let
  the user click it.) See `PROFILES.md` §3.
- State for the dev instance lives in its profile (default `~/.agentmaster-dev/`, or wherever the
  picker pointed — check `~/.agentmaster.profiles`). Tail `<profile>/hooks.log` for
  `[engine] bridge listening …` + `[SessionStart]`.

---

## Path B — Public release (the main event)

### B0. Confirm the version with the user
It's outward-facing — never decide the version yourself. Default scheme: a tag `vX.Y.Z` →
`ver3 = X.Y.Z`, `ver4 = X.Y.Z.0` (the manifest `Identity Version`). Decide whether to release
HEAD as-is or commit pending WIP first.

### B0.5. Map the topology — diverged tags & "reunification" releases
A version's tag is a **snapshot**, not necessarily a point on mainline. Prereleases and hotfixes
routinely **diverge**: a hotfix cut off an old stable base, or a rebased/force-pushed branch, leaves
a tagged commit that is **NOT an ancestor of `agentmaster` HEAD**. Before cutting, map where the
release sits so the notes baseline and the "what does this contain" story are correct:
```bash
git fetch --tags --force origin
for t in $(git tag --list 'v*' | sort -V | tail -6); do
  git merge-base --is-ancestor "$t" HEAD 2>/dev/null \
    && echo "$t  ON-mainline (ancestor of HEAD)" \
    || echo "$t  DIVERGED (its commit is not on HEAD)"
done
git rev-list --count "v<lastStable>"..HEAD     # how far mainline is past the last STABLE
git rev-parse HEAD origin/agentmaster          # is local ahead of origin? (push mainline before tagging)
```
- **Diverged tags are history** — leave them on GitHub; don't assume their commit is reachable from
  mainline, and don't try to reconcile them into the branch. Their WORK usually still lives on
  mainline (re-integrated / continued under different hashes).
- **"Reunification" release:** when several prereleases/hotfixes diverged off an old stable base and
  mainline is now the **superset** of all of them, releasing mainline REUNIFIES the line. But the
  NOTES still follow the §2 rule by release TYPE: a reunification **prerelease** lists only ITS OWN
  delta — the work it newly ships (e.g. a previously-abandoned batch), MINUS anything already shipped
  in an intervening hotfix prerelease; the comprehensive fold waits for the eventual STABLE. Today's
  case: 0.6.2 (abandoned prerelease) + 0.6.3/0.6.4 (diverged hotfixes) all sat off the 0.6.1 base;
  mainline carried the full superset, so v0.6.5 released mainline as one **prerelease** whose notes are
  just the 0.6.2 batch + newer state/triage commits (NOT the 23 GB leak / hourly-update / crash-loop
  that already shipped in 0.6.3/0.6.4) — even though 0.6.2–0.6.4 are "newer" tags.
- **A prerelease baselines off the PREVIOUS release; a STABLE baselines off the last stable.** `Latest`
  = the newest **non**-prerelease; intervening prereleases do NOT advance it. That last-stable is the
  baseline for the STABLE fold (§6) only — a prerelease's delta is measured against whatever release
  came right before it, not off the last stable or the highest version number (release-notes §2).
- **Push mainline before tagging** so `origin/agentmaster` reflects the released tree (the install
  one-liner pulls `tools/*.ps1` from the branch). A clean fast-forward (`git merge-base --is-ancestor
  origin/agentmaster HEAD`) is a plain `git push origin agentmaster`.

### B1. PRE-VALIDATE with a clean local Release build — REQUIRED
This is the single most important step. **The CI/release runner does a from-scratch Release x64
build; your incremental Debug builds MASK errors it will hit** (e.g. a dead `using namespace
::Microsoft::Console;` in a TU that doesn't include `utils.hpp` — compiles in stale Debug, fails
`C2039`/`C2871` on a clean build). Catching it locally saves a ~22-min runner failure cycle.

A **lib-only** check is NOT sufficient — building `TerminalAppLib.vcxproj` alone in a fresh Release
obj tree fails on missing GENERATED headers (`ITerminalHandoff.h`) because it skips dependency IDL
generation. Do the **full** package build (it builds dependencies in order):
```bash
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "validate release $(git rev-parse --short HEAD)") || exit 1
# Release is a different output tree (bin\x64\Release) from the running Debug instance — no need to close it.
pwsh -ExecutionPolicy Bypass -File ./tools/Build-Agentmaster.ps1 -NoRestore -Configuration Release 2>&1 \
  | grep -iE 'error C|error MSB|error LNK|fatal error|Build OK|Build FAILED' | grep -ivE 'PRI263|: warning' | tail -40
bash tools/am-lock.sh release --token "$TOKEN"
```
Expect `Build OK`. The wrapper skips the ~156s `.appxsym` symbol-package zip by default (see the
`build-install` skill §2 — it was 77% of a Debug package build), so a first cold Release build is
~5–10 min and incremental rebuilds drop to **~30s–2 min** — fix-rebuild loops are cheap. (Skipping
appxsym is safe for pre-validation: it catches compile/link errors, and the symbol-zip itself never
fails. NOTE the CI `release.yml` does NOT pass the flag, so it still spends ~156s/arch generating an
`.appxsym` nobody consumes — a separate, untouched CI-speedup opportunity.) `cl /MP` here reveals
errors roughly one file at a time — fix, rebuild, repeat until green. (Optional belt-and-suspenders:
the engine harness — `tests/run-m5-tests.bat`, ~1700 checks, no lock needed — but it validates the
pure-C++ ENGINE TUs only, NOT the WinRT/XAML app TUs where clean-build-only errors bite, so it is
NOT a substitute for B1.)

**Self-kill caveat — when the release HOST is itself a main-tree Release instance.** If your Claude
session runs *inside* the production instance (`*\bin\x64\Release\WindowsTerminal.exe`), you CANNOT
relink the main tree's Release layout — building Release there locks/kills your own host. The
self-kill-safe local build is an **isolated git worktree** (its own `bin\`), but a fresh worktree is
a COLD build (full restore + cppwinrt projection, ~15-25 min for Release) that won't beat CI. In that
case **CI's from-scratch Release build IS the clean-build gate B1 seeks** — trigger it (B3), watch the
**build** step (B4), and spin a worktree only if it fails (incremental fix-rebuilds in a warm worktree
are then fast). Reserve the local pre-validate for when you can build Release without self-kill (a
Debug-hosted dev session, or an already-warm worktree). This is why v0.6.5 leaned on CI, not a local
Release build.

### B2. Commit anything needed for a clean build
Extensive commit message (project convention). If local diverged from origin by a content-identical
amend, force-with-lease is safe; verify first with `git log --oneline origin/agentmaster ^HEAD` (only
the amended commit should be unique) and an empty `git diff <origin-tip> HEAD`. **Do NOT add the
`Co-Authored-By` / "Generated with Claude Code" trailers** (global rule).
```bash
git add -A && git commit -F - <<'MSG'
<subject>

<body>
MSG
git push --force-with-lease origin agentmaster   # or plain push if fast-forward
```

### B3. Trigger the release
```bash
# real release (tag-push):
git tag -a vX.Y.Z -m "Agentmaster X.Y.Z" <commit> && git push --force origin vX.Y.Z
# re-pointing an existing tag: git tag -d vX.Y.Z; git tag -a ...; git push --force origin vX.Y.Z
#   (a force-update of a v* tag DOES re-trigger release.yml)
# OR a draft dry-run without a tag:
gh workflow run release.yml -R Nucs/Agentmaster -f version=X.Y.Z
```

### B4. Monitor (background it)
```bash
gh run list --workflow=release.yml -R Nucs/Agentmaster --limit 3      # grab the run id
gh run watch <run-id> -R Nucs/Agentmaster --exit-status --interval 30 # exits non-zero on failure
```
5 jobs must go green: **prep · build (x64) · build (arm64) · bundle · release** (~22 min total;
the x64+arm64 matrix is the long pole). The compile failure point is the **"Build CascadiaPackage
(Release, …)"** step — an early `gh run view <id> -R Nucs/Agentmaster --json jobs` confirms it cleared.

### B5. Verify the draft release + its 4 assets
```bash
gh release view vX.Y.Z -R Nucs/Agentmaster            # title / tag / draft:true / 4 assets
gh api repos/Nucs/Agentmaster/releases/<id>/assets | jq -r '.[]|"  \(.name): \(.size) bytes"'
```
Expect exactly these, non-empty (rough sizes from v0.1.1):
- `Agentmaster_<ver4>.msixbundle`  (~15 MB)
- `Agentmaster.cer`                (~768 bytes — DER cert; "0 MB" is just rounding, confirm it's > 0)
- `Agentmaster_<ver4>_x64.zip`     (~11 MB)
- `Agentmaster_<ver4>_arm64.zip`   (~11 MB)

### B6. Apply curated release notes
Write dense, capability-focused notes to a temp `.md` (sections: a one-line intro · **New in
X.Y.Z** `-` list · **Capabilities** · **Install** — portable zip + MSIX/cert steps with the exact
asset names). Then:
```bash
gh release edit vX.Y.Z -R Nucs/Agentmaster --notes-file /tmp/am_release_notes.md
```
(`softprops/action-gh-release` writes a terse default body; override it.)

### B7. Publish
Pick the flags by framing (the `release-notes` skill §5 owns stable-vs-prerelease framing):
```bash
# STABLE — becomes the new Latest:
gh release edit vX.Y.Z -R Nucs/Agentmaster --draft=false --latest --prerelease=false
# PRE-RELEASE — the current stable stays Latest:
gh release edit vX.Y.Z -R Nucs/Agentmaster --draft=false --prerelease --latest=false
```
Confirm the state + badge:
```bash
gh release view vX.Y.Z -R Nucs/Agentmaster | grep -iE '^draft:|^prerelease:'
gh release list -R Nucs/Agentmaster --limit 5 | grep -vE '^\*'   # right badge on the right release
```
**Badge gotcha (proven):** `--latest=false` alone does NOT move the Latest badge off a release — to
relocate it you must explicitly `--latest` the release you want as Latest (release-notes §5). After
publishing, refresh `README.md` (Download/capabilities) if the feature set moved.

---

## Signing & identity (FYI — already wired)
The release self-signs at runtime with a `CN=Agentmaster` cert whose subject is **read from the
manifest** (so it always == the Publisher; this is why the Publisher is `CN=Agentmaster`). Users
trust `Agentmaster.cer` once for the `.msixbundle`; the portable `.zip` needs no cert. A real cert
goes via repo secrets `SIGNING_PFX_BASE64` + `SIGNING_PFX_PASSWORD` (subject must still == Publisher).
Fork one-time settings (Actions OFF by default on forks): `gh api -X PUT
repos/<owner>/<repo>/actions/permissions -F enabled=true -f allowed_actions=all` and
`.../permissions/workflow -f default_workflow_permissions=write`.

## Gotchas recap
- `-R Nucs/Agentmaster` on EVERY `gh` call (upstream remote hijacks the default).
- Pre-validate with the **full** Release package build, never the lib alone (generated-header trap).
- Incremental Debug ≠ clean Release — only the from-scratch build catches the errors CI hits.
- Force-updating a tag re-triggers `release.yml`; a draft has no public effect until you publish.
- Local deploy relaunches **`AgentmasterDev`**/`agentmasterdev`, NOT `Agentmaster`/`agentmaster`.
- Always release the build mutex, even when a step fails.
