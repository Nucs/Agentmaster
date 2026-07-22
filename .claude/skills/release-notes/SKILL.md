---
name: release-notes
description: |
  Author and assemble Agentmaster GitHub release notes — the house format (tight intro +
  "New"/"Fixes" + "Install" with the exact asset names), turning a commit delta into dense
  themed bullets, and the three framings: NIGHTLY vs PRE-RELEASE vs STABLE. Encodes the
  mechanics done repeatedly: derive the changelog since the right baseline (nightlies are
  INVISIBLE to baselines), the copy-paste Install block, the "comprehensive changelog since
  last stable" ASSEMBLY (a stable release folds every intervening pre-release's notes in
  full — never nightlies), stripping the draft's HTML-comment header before publishing,
  applying via `gh release edit --notes-file`, and CONVERTING a live release between stable
  and pre-release — including the gotcha that `--latest=false` does NOT move the Latest
  badge (you must explicitly `--latest` the release you want as Latest). Use when writing
  or updating a release's notes, choosing nightly/stable/pre-release framing, assembling
  the full changelog for a version, or flipping a published release's stable/pre-release status.
keywords: release notes, changelog, notes-file, gh release edit, pre-release, stable, nightly, latest badge, New in, Fixes, Install, asset names, msixbundle, portable zip, since last stable, draft notes, Agentmaster
---

# Agentmaster — authoring & assembling release notes

This skill owns the **content + framing + application** of a release's notes. The **pipeline**
(tag → CI → verify assets → publish) is the **`release-version`** skill; this is the notes half
of its B6/B7. If they disagree, `CLAUDE.md` → *Releasing a public version* wins.

Every `gh` call needs **`-R Nucs/Agentmaster`** (the checkout's `upstream` remote hijacks the
default). If a `--json`/`--jq` query exits 1 with no output, the gh HTTP-trace noise corrupted it —
fall back to the plain table form (`gh release view vX.Y.Z -R Nucs/Agentmaster`) or `gh api … | jq`.

---

## 1. Where notes live + the two artifacts

- **Draft (repo archive):** `.claude/skills/release-version/am_release_<NNN>_notes.draft.md`
  where `<NNN>` is the version without dots (`060`, `061`, `062`, `063`). It carries a leading
  **HTML-comment metadata header** (range, release mechanics, asset names) that is **stripped
  before publishing**. Commit it (the project keeps a per-version notes archive).
- **Published body (temp):** the draft minus the comment header → applied to the GitHub release
  with `gh release edit … --notes-file`.

The `<NNN>` file name and the committed archive are the convention — match it.

---

## 2. Pick the baseline (what the notes cover)

**THE RULE — and it DIFFERS for pre-release vs stable (this is the whole point):**
- A **PRE-RELEASE** vX.Y.Z → notes show **ONLY that prerelease's OWN explicit delta** — what changed
  vs the **immediately-previous release** (the prior prerelease, or the stable it built on). Each
  prerelease's notes stand alone as "what's new in THIS one." **Do NOT fold in earlier prereleases**,
  and do NOT re-list changes that already shipped in an earlier prerelease. (User's rule, stated
  2026-07: *"all the prereleases … are supposed to only show what has changed in that prerelease
  explicitly."*)
- A **STABLE** vX.Y.Z → notes = the **complete changelog since the last stable** — you **fold in every
  intervening pre-release's notes in full** (§6 assembly). **The accumulation happens ONLY at the
  prerelease→stable promotion** — that is *when we actually take care of accumulating all prerelease
  notes into the same release.* Example: 0.6.0 stable folded the entire 0.5.6→0.5.9 prerelease line.

- **Last stable** = the newest release in `gh release list -R Nucs/Agentmaster` NOT marked
  `Pre-release` (it wears `Latest`); intervening prereleases do not reset it. It is the baseline for
  the STABLE fold — **NOT** for a prerelease. A **prerelease baselines off the PREVIOUS release**,
  whatever version that was (e.g. a reunification prerelease that ships a previously-abandoned batch
  lists only that batch as ITS delta, minus anything already shipped in an intervening hotfix).
- **NIGHTLIES are INVISIBLE to baselining, in BOTH directions.** A nightly (tag contains `nightly`,
  e.g. `v0.6.10-prerelease-nightly` — an unstable dev snapshot only nightly-opted users ever see):
  - Its OWN notes may be terse — a short delta vs the previous release of ANY kind (or even the
    workflow's default body): nightlies exist to be installed by testers, not read.
  - A later **pre-release/stable NEVER baselines off a nightly** — its delta is measured against the
    previous NON-nightly release, so commits that first shipped in a nightly still appear in the next
    real release's notes (almost nobody installed the nightly; "already shipped" doesn't apply).
  - The **STABLE fold (§6) folds pre-releases only, never nightlies** — a nightly's content reaches
    the fold through the pre-release/stable that re-ships it.

Get the delta and omit-list — `<prev>` is the release RIGHT BEFORE `<target>` (prerelease: the prior
prerelease/stable; stable: the last stable, then fold each intervening prerelease's body per §6):
```bash
git log --oneline 'v<prev>^{commit}'..<target> 2>/dev/null          # the changelog for THIS release
git log --oneline 'v<prev>^{commit}'..<target> | grep -icE ' (docs|refactor|chore|test)\(|Merge '  # omit these from user notes
```
Read the FULL message of the big commits before writing (`git show -s --format='%s%n%b' <sha> | head -30`) —
the one-line subject rarely captures the user-facing story.

---

## 3. Turn commits into notes

- **Synthesize into THEMES, not a commit dump.** Group related commits (e.g. all the pending-input
  commits → one "3 dots" bullet). A 74-commit release becomes ~6–10 "New" bullets + ~5 "Fixes".
- **Omit** pure `docs`/`refactor`/`chore`/`test`/merge commits (internal; no user-visible change).
- **Voice:** capability-first, bolded feature name, one dense sentence of what+why. Match the prior
  drafts (`am_release_057_notes.draft.md` etc.) — read one before writing.
- **Sections:** `## New in X.Y.Z` (or `## New`) then `## Fixes`. A tiny hotfix can be just `## Fixes`
  + `## New`. Flag anything users MUST know (a license change, a feature now dev-only, a removal).

---

## 4. The house format (copy-paste)

```markdown
<!-- Agentmaster X.Y.Z release notes — <STABLE|PRE-RELEASE>.
     Range: v<prev>..<target> = N commits. RELEASING from <sha>. Tag vX.Y.Z -> X.Y.Z.0.
     <STABLE: publish --draft=false --latest (REPLACES <prevstable> as Latest; no --prerelease).>
     <PRE:    publish --draft=false --prerelease --latest=false (the stable <prevstable> stays Latest).>
     Assets: Agentmaster_X.Y.Z.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster X.Y.Z<  (pre-release)>

**X.Y.Z is a <stable release|pre-release>** building on <prev>. <one-paragraph headline of the top themes>.
< PRE-RELEASE only: >Enable **Settings → Updates → "Allow pre-release versions"** to receive it
automatically, or install from the assets below. The stable **<prevstable>** remains the default for everyone else.
< STABLE only: >This build becomes the new default **Latest**.

## New in X.Y.Z
- **Feature.** …

## Fixes
- **Fix.** …

## Install< (pre-release)>

< PRE-RELEASE only: >> **This is a pre-release.** It is **not** offered as an automatic update
unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **<prevstable>** stays the default Latest.

**One command (PowerShell)** — installs X.Y.Z (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version X.Y.Z
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).< STABLE: > X.Y.Z is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_X.Y.Z.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_X.Y.Z.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_X.Y.Z.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
```

Notes: assets are always `Agentmaster_<X.Y.Z>.0.*` (**ver4** = X.Y.Z.0) + a single `Agentmaster.cer`.
The `< … >` markers above are placeholders, not literal — keep the STABLE **or** PRE-RELEASE variant.

**NIGHTLY variant** (see §5 for the framing block): title `## Agentmaster X.Y.Z-prerelease-nightly
(nightly)`; the intro/Install blockquote becomes the NIGHTLY warning (`> ⚠️ **NIGHTLY build** — an
unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to
help test.`) and the enable path is **Settings → Updates → "Allow updating to nightly builds
(unstable)"** (a warning confirm gates it; even pre-release-opted users skip nightlies otherwise).
Asset names STAY numeric (`Agentmaster_X.Y.Z.0.*` — prep strips the tag suffix for ver4), but the
one-command installer must pass the **FULL suffixed version**: `-Version X.Y.Z-prerelease-nightly`
(`-Version` maps to the tag, which carries the suffix). `release.yml` already writes a serviceable
nightly body (warning blockquote + install block) — curated notes are OPTIONAL for a nightly.

---

## 5. Stable vs pre-release — framing AND mechanics

| | Pre-release | Stable |
|---|---|---|
| Title | `## Agentmaster X.Y.Z (pre-release)` | `## Agentmaster X.Y.Z` |
| Intro | "**X.Y.Z is a pre-release** … The stable **<prev>** remains the default." | "**X.Y.Z is a stable release** … the new default **Latest**." |
| Install | keeps the `> **This is a pre-release.**` blockquote + `## Install (pre-release)` | drops the blockquote; `## Install`; adds "now the default Latest" |
| Publish | `gh release edit vX.Y.Z -R Nucs/Agentmaster --draft=false --prerelease --latest=false` | `gh release edit vX.Y.Z -R Nucs/Agentmaster --draft=false --latest --prerelease=false` |

**NIGHTLY framing (the third tier, BELOW pre-release):** a nightly is cut by TAG NAME
(`vX.Y.Z-prerelease-nightly` — anything containing `nightly`; release-version B0/B3) and
`release.yml` **auto-marks it prerelease** with a warning blockquote already in the body. Mechanics:
same publish flags as a pre-release (`--draft=false --prerelease --latest=false`, NEVER `--latest`);
title suffix `(nightly)`; the enable path in any text is the cog's warning-gated nightly switch, not
the pre-release toggle (the in-app updater offers a nightly ONLY to nightly-opted users — even
pre-release-opted users skip it). Notes may stay terse/default (§2 — nightlies are invisible to
baselines, so nothing downstream depends on their bodies). **Never CONVERT a nightly to
stable/pre-release**: the nightly gating keys on the TAG, which `gh release edit` can't rename — a
build worth promoting is re-tagged and re-released as a real `vX.Y.Z`.

**CONVERTING a live release (the flip):** change **both** the notes framing (title/intro/Install)
and the flags. **Gotcha proven today:** on a pre-release→stable→pre-release flip, `--latest=false`
alone does **NOT** move the "Latest" badge off the release — you must **explicitly mark the release
you want as Latest**:
```bash
# demote 0.6.2 back to pre-release AND restore 0.6.1 as Latest:
gh release edit v0.6.2 -R Nucs/Agentmaster --prerelease --latest=false
gh release edit v0.6.1 -R Nucs/Agentmaster --latest --prerelease=false   # <-- this is what actually moves the badge
gh release list -R Nucs/Agentmaster --limit 4 | grep -vE '^\*'           # confirm: 0.6.2 Pre-release, 0.6.1 Latest
```
When you reframe a live release, restore the prior notes cleanly — the previous body is recoverable
from git (`git show <notes-commit>:<draft path>`), so revert-and-reapply rather than hand-editing.

---

## 6. Comprehensive assembly — a STABLE release's full changelog since last stable

**This section is STABLE-only — it is _the_ prerelease→stable accumulation step.** A prerelease never
does this (it shows only its own delta, §2); accumulating all the prerelease notes into one body is
exactly what you do WHEN you cut the stable release. Its body must include the **complete** changelog:
the new stable work on top, then each intervening pre-release's **full** New/Fixes folded in. Pull the
authoritative **published** bodies and transform them (strip each Install, demote `##`→`###`, wrap
under a per-version banner, one Install at the end):

```bash
SC=/tmp; for V in 0.5.9 0.5.8 0.5.7 0.5.6; do
  gh release view v$V -R Nucs/Agentmaster --json body --jq '.body' 2>/dev/null > "$SC/pub_$V.md"
done
{
  # 1. the new stable's own intro + New + Fixes (from your draft, minus the comment/Install)
  awk '/^## Agentmaster/{f=1} /^## Install/{f=0} f' your_draft.md
  echo; echo '---'; echo; echo '# Full changelog since the last stable (<prevstable>)'; echo
  # 2. each pre-release, verbatim minus its Install, headers demoted, under a version banner
  for V in 0.5.9 0.5.8 0.5.7 0.5.6; do
    awk '/^## Install/{exit} {print}' "$SC/pub_$V.md" \
      | sed -E -e 's/^## Agentmaster /## /' \
               -e 's/Enable \*\*Settings.*everyone else\.//' \
               -e 's/^## New in [0-9.]+.*/### New/' -e 's/^## Fixes.*/### Fixes/' -e 's/^## Thanks.*/### Thanks/'
    echo
  done
  # 3. one stable Install (from your draft)
  awk '/^## Install/{f=1} f' your_draft.md
} > "$SC/full_body.md"
grep -c '^## Install' "$SC/full_body.md"   # want 1; and 0 stray "remains the default"
```
(The user requirement that surfaced this: *"the release notes must contain all notes since last
stable release."*)

---

## 7. Apply + publish

Strip the draft's HTML-comment header, then apply and publish (per §5 flags):
```bash
awk 'f{print} /^## Agentmaster X.Y.Z/{f=1; print}' draft.md > /tmp/body.md   # drop the <!-- … --> header
grep -c '<!--' /tmp/body.md        # want 0
gh release edit vX.Y.Z -R Nucs/Agentmaster --notes-file /tmp/body.md
# then the stable OR pre-release publish line from §5, then verify:
gh release view vX.Y.Z -R Nucs/Agentmaster | grep -iE '^draft:|^prerelease:'
gh release list -R Nucs/Agentmaster --limit 4 | grep -vE '^\*'
```
`softprops/action-gh-release` writes a **terse default body** — always override it with `--notes-file`.
Verify the markers landed (`gh release view … --json body --jq '.body' | grep -c '<your headline phrase>'`).

Finally, commit the draft archive (single `git add` + `git commit`, extensive message, **no**
`Co-Authored-By`/"Generated with" trailers).

---

## Gotchas recap
- `-R Nucs/Agentmaster` on **every** `gh` call; `--jq` exiting 1 empty ⇒ use the plain form.
- A **PRE-RELEASE**'s notes show ONLY its OWN delta (vs the immediately-previous release) — never fold
  in earlier prereleases, never re-list an already-shipped change; each prerelease stands alone (§2).
- A **STABLE** release's notes must carry the **whole** changelog since the last stable — fold in the
  intervening pre-releases (§6), don't just condense them to a recap. This fold is the ONLY place
  accumulation happens (the prerelease→stable moment).
- A **NIGHTLY** is invisible to baselining in both directions (§2): its own notes may be terse, the
  next real release baselines off the previous NON-nightly release (re-listing nightly-shipped work
  is CORRECT), and the stable fold never includes it. Same publish flags as a pre-release, never
  `--latest`, never convert-in-place (the gating is the TAG).
- Flip framing **and** flags together; `--latest=false` alone won't move the badge — `--latest` the
  release you want (§5).
- Strip the `<!-- … -->` draft header before `--notes-file`.
- Asset names are `Agentmaster_X.Y.Z.0.*` (ver4) + `Agentmaster.cer`; the one-command installer uses
  `-Version X.Y.Z` (ver3) — **except a nightly, whose `-Version` is the FULL suffixed
  `X.Y.Z-prerelease-nightly`** (it maps to the tag; the assets stay numeric ver4).
- The `> **This is a pre-release.**` blockquote + `(pre-release)` title suffix belong to pre-releases
  ONLY — a "0.6.2 is now the default Latest" line on a pre-release is the classic reframe-miss; a
  nightly wears the ⚠️ NIGHTLY blockquote instead (§4).
