---
name: release-notes
description: |
  Author and assemble Agentmaster GitHub release notes — the house format (a 1-2 sentence
  intro, then ONE-ROW changes under "What's new" / "What's fixed" — a bold lead and about a
  sentence each, nothing long-form below them — then "Worth knowing" and "Install" with the
  exact asset names), turning a commit delta into one row per user-visible change, and the
  three framings: NIGHTLY vs PRE-RELEASE vs STABLE. Encodes the
  mechanics done repeatedly: derive the changelog since the right baseline (nightlies are
  INVISIBLE to baselines), the copy-paste Install block, the "comprehensive changelog since
  last stable" MERGE (a stable release merges every intervening pre-release's notes into ONE
  changelog — one What's new, one What's fixed, one "Worth knowing" — that names no pre-release
  version; never nightlies), stripping the draft's HTML-comment header before publishing,
  applying via `gh release edit --notes-file` (an OLDER release's notes with an explicit
  make_latest=false), and CONVERTING a live release between stable
  and pre-release — including the gotcha that `--latest=false` does NOT move the Latest
  badge (you must explicitly `--latest` the release you want as Latest). Use when writing
  or updating a release's notes, choosing nightly/stable/pre-release framing, assembling
  the full changelog for a version, or flipping a published release's stable/pre-release status.
keywords: release notes, changelog, notes-file, gh release edit, pre-release, stable, nightly, latest badge, What's new, What's fixed, one-row, one row per change, Worth knowing, Install, asset names, msixbundle, portable zip, since last stable, draft notes, make_latest, Agentmaster
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
- **A published body that is REPLACED** gets a dated note at the top of its draft's comment header:
  the owner's words, what changed, and the commit whose draft still holds the old body (every draft
  from v0.1.0 to v0.9.0 carries the 2026-11-16 one-row rewrite's note; v0.9.0's also the 11-13 merge).
  A release without a draft gets one holding its published body BEFORE its body is replaced, in a
  commit of its own (3da02fdbc did that for 18 releases), so the published text always survives.

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
- A **STABLE** vX.Y.Z → notes = the **complete changelog since the last stable**, written as **ONE
  merged changelog**: every intervening pre-release's items **merged into the stable's own sections**
  (one What's new, one What's fixed, one "Worth knowing when you upgrade"), each item in its final state, and **no
  pre-release version named anywhere** (§6). **The accumulation happens ONLY at the prerelease→stable
  promotion** — that is *when we actually take care of accumulating all prerelease notes into the same
  release.* The owner's rule of 2026-11-13, after v0.9.0 first shipped with 0.8.2-prerelease's body
  folded under its own banner: *"merge the release notes (all of them) since last release (non
  prerelease), no mentioning of prerelease versions but 0.9.0"* — v0.9.0's published body was replaced
  with the merge (`.claude/skills/release-version/am_release_090_notes.draft.md` is the model).

- **Last stable** = the newest release in `gh release list -R Nucs/Agentmaster` NOT marked
  `Pre-release` (it wears `Latest`); intervening prereleases do not reset it. It is the baseline for
  the STABLE merge — **NOT** for a prerelease. A **prerelease baselines off the PREVIOUS release**,
  whatever version that was (e.g. a reunification prerelease that ships a previously-abandoned batch
  lists only that batch as ITS delta, minus anything already shipped in an intervening hotfix).
- **NIGHTLIES are INVISIBLE to baselining, in BOTH directions.** A nightly (tag contains `nightly`,
  e.g. `v0.6.10-prerelease-nightly` — an unstable dev snapshot only nightly-opted users ever see):
  - Its OWN notes may be terse — a short delta vs the previous release of ANY kind (or even the
    workflow's default body): nightlies exist to be installed by testers, not read.
  - A later **pre-release/stable NEVER baselines off a nightly** — its delta is measured against the
    previous NON-nightly release, so commits that first shipped in a nightly still appear in the next
    real release's notes (almost nobody installed the nightly; "already shipped" doesn't apply).
  - The **STABLE merge (§6) merges pre-releases only, never nightlies** — a nightly's content reaches
    the merge through the pre-release/stable that re-ships it.

Get the delta and omit-list — `<prev>` is the release RIGHT BEFORE `<target>` (prerelease: the prior
prerelease/stable; stable: the last stable, then merge each intervening prerelease's items per §6):
```bash
git log --oneline 'v<prev>^{commit}'..<target> 2>/dev/null          # the changelog for THIS release
git log --oneline 'v<prev>^{commit}'..<target> | grep -icE ' (docs|refactor|chore|test)\(|Merge '  # omit these from user notes
```
Read the FULL message of the big commits before writing (`git show -s --format='%s%n%b' <sha> | head -30`) —
the one-line subject rarely captures the user-facing story.

---

## 3. Turn commits into notes — ONE ROW PER CHANGE

**The owner's rule of 2026-11-16**, after the 0.8.0–0.9.0 bodies had grown into walls of text: *"one-row
changes in the release notes, like we do in the head of all release notes … sentences that start with a
bold and then, essentially a sentence, maybe a sentence and a half, or a bit more if you really have
to"*. Shown three layouts of v0.9.0 rendered by GitHub (the rows on top with the long details kept
open, folded behind a `<details>` line, or removed), the owner picked **"Head only"**: the rows ARE the
notes. Every release on GitHub, v0.1.0 through v0.9.0 and the nightlies, was rewritten that way the
same day (the owner: "Change the releases on gh too, finish"); their drafts
(`.claude/skills/release-version/am_release_<NNN>_notes.draft.md`) are the models — read one before
writing (`090` for a stable, `082` for a pre-release, `078_nightly` for a nightly).

- **One row = one user-visible change:** `- **Lead.** One sentence.` The sentence says what the change
  does for the user: a sentence and a half at most, a bit more only when the reader cannot act without
  it (the Settings path of an opt-in feature, or of the switch that turns a default-on change off).
  No sub-bullets, no second paragraph, no "see above / below" (nothing is above or below a row).
- **The lead:** a feature row names the feature (**Pin to Top.**, **Name a window.**); a fix row states
  the outcome in the present (**Long prompts are delivered again.**, **Window switches no longer freeze
  big workspaces.**). **Bold only the lead:** an inner bold competes with the leads the eye scans, so a
  Settings path or a UI name inside the sentence stays plain (code spans for commands are fine).
- **A change, not a commit.** Group the commits behind one change into its row (all the pending-input
  commits → one "unsent draft" row); split a feature that does several things a user looks for
  separately (the side panel's filters, Pin to Top and ⋯ menu are three rows). A big stable release
  can carry 20+ "What's new" rows and 10+ "What's fixed" rows: fine, each is still one row.
- **Omit** pure `docs`/`refactor`/`chore`/`test`/merge commits (internal; no user-visible change).
- **Sections:** `## What's new`, then `## What's fixed` (a tiny hotfix can have `## What's fixed`
  alone), then `## Worth knowing when you upgrade` for anything users MUST know (a default that changes
  behavior, a new switch that ships on, a license change, a removal, data that moves), then `## Install`.
  **Never a long-form section below the rows:** no "New in X.Y.Z" prose, no per-version changelog, no
  `<details>` fold. A feature's detail lives in its doc under `doc/agentmaster/`. (The in-app update
  prompt renders these same bodies, minus `## Install`: the rows read well there too.)

---

## 4. The house format (copy-paste)

```markdown
<!-- Agentmaster X.Y.Z release notes — <STABLE|PRE-RELEASE>.
     Range: v<prev>..<target> = N commits. RELEASING from <sha>. Tag vX.Y.Z -> X.Y.Z.0.
     <STABLE: publish --draft=false --latest (REPLACES <prevstable> as Latest; no --prerelease).>
     <PRE:    publish --draft=false --prerelease --latest=false (the stable <prevstable> stays Latest).>
     Assets: Agentmaster_X.Y.Z.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip,
     Agentmaster.cer. -->
## Agentmaster X.Y.Z<  (pre-release)>

< PRE-RELEASE: >**X.Y.Z is a pre-release** building on <prev>. Enable **Settings → About → "Allow updating to
pre-release versions"** to receive it automatically, or install from the assets below. The stable
**<prevstable>** remains the default Latest for everyone else.
< STABLE: >**X.Y.Z is a stable release** and the new default **Latest**. Agentmaster offers it as an update
by itself, or install it from the assets below.
<The intro is framing only: no summary of the themes (the rows below ARE the summary, §3), and a STABLE
names no other Agentmaster version (§6).>

## What's new

- **The feature's name.** What it does for you, in about one sentence (with the Settings path when you must turn it on).
- …

## What's fixed

- **The outcome, in the present tense.** What went wrong and what happens now, in about one sentence.
- …

## Worth knowing when you upgrade< from <prev>>

- ⚠️ **What changes for you.** What to do about it, with the Settings path.

## Install< (pre-release)>

< PRE-RELEASE only: >> **This is a pre-release.** It is **not** offered as an automatic update
unless you enable **Settings → About → "Allow updating to pre-release versions."** The stable **<prevstable>** stays the default Latest.

**One command (PowerShell)** — installs X.Y.Z (administrator rights not needed; it asks where Agentmaster should live: just for you, for all users of this PC, or as the app package):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version X.Y.Z
```
Add `-Scope User` (or `Machine` / `Package`) to skip the question, and/or `-Launch` to start it right after.< STABLE: > X.Y.Z is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_X.Y.Z.0_x64.zip` (or `_arm64`), unzip anywhere, run `Agentmaster.exe`. Its first launch offers a per-user install, or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_X.Y.Z.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_X.Y.Z.0.msixbundle`.

**Debugging symbols:** the portable zips carry a *public* PDB beside each binary (every function named in a stack trace); `Agentmaster_X.Y.Z.0_x64_symbols.zip` (or `_arm64`) holds the *full* PDBs, Source-Linked to this release's commit — extract it into the folder that holds `Agentmaster.exe`, or onto your debugger's symbol path (an MSIX install's folder is read-only).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal 1.26 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
```

Notes: assets are always `Agentmaster_<X.Y.Z>.0.*` (**ver4** = X.Y.Z.0) + a single `Agentmaster.cer`; from the
release-symbols change of 2026-10-23 on, that includes `Agentmaster_<X.Y.Z>.0_<arch>_symbols.zip` per architecture
(`doc/agentmaster/SYMBOLS.md`). A release cut before it has no symbols — never promise them in its notes.
The `< … >` markers above are placeholders, not literal — keep the STABLE **or** PRE-RELEASE variant.

**NIGHTLY variant** (see §5 for the framing block): title `## Agentmaster X.Y.Z-prerelease-nightly
(nightly)`; the intro/Install blockquote becomes the NIGHTLY warning (`> ⚠️ **NIGHTLY build** — an
unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to
help test.`) and the enable path is **Settings → About → "Allow updating to nightly builds
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

## 6. Comprehensive merge — a STABLE release's full changelog since last stable

**This section is STABLE-only — it is _the_ prerelease→stable accumulation step.** A prerelease never
does this (it shows only its own delta, §2); accumulating all the prerelease notes into one body is
exactly what you do WHEN you cut the stable release. Two of the owner's requirements shape it:
*"the release notes must contain all notes since last stable release"* and (2026-11-13) *"merge the
release notes (all of them) since last release (non prerelease), no mentioning of prerelease versions
but 0.9.0"*. So the body is ONE changelog over `v<laststable>..<target>`, never a stack of per-version
sections (the per-version banner fold used up to v0.9.0's first body is retired).

**Inputs:** the stable's own delta since the newest pre-release (§2–§3), and each intervening
pre-release's **published** body (the authoritative items, already reviewed):
```bash
SC=<scratchpad>; for V in 0.8.2-prerelease; do   # every pre-release since the last stable, never a nightly
  env -u GH_TOKEN -u GITHUB_TOKEN gh release view v$V -R Nucs/Agentmaster --json body --jq '.body' 2>/dev/null \
    | grep -v '^\* Request' > "$SC/pub_$V.md"
done
```
**The merge** — one intro, `## What's new`, `## What's fixed`, `## Worth knowing when you upgrade`, one
`## Install`, every item ONE row (§3):
- **Every item in its FINAL state, measured against the last stable.** An item a later change reworked
  becomes ONE row (v0.9.0: 0.8.2's "a turn cut by Connection lost restarts by itself" + 0.9.0's widening
  = one "Sessions restart by themselves." row; the side panel's ⋯ menu described as it ends). An item a
  later change REMOVED is left out (v0.9.0: 0.8.2's "The card's screen follows the status", undone by the
  screenshot-on-tap change). Check "no longer / now / used to" wording against the last stable's own
  code: `git merge-base --is-ancestor <commit> v<laststable>`.
- **Group by theme, not by origin:** a pre-release's row and the stable's additions to the same feature
  become one row in its final state, and related rows sit next to each other (the side panel's, Telegram's).
- **No other Agentmaster version in the body** — no pre-release version, no "pre-release" / "folded in",
  and no previous version as a reference point: "0.8.1 asked only …" → "Agentmaster used to ask only …";
  "You will see it at the first update offered after 0.8.2" → "… from the next update on"; the intro is
  "the new default Latest", not "replacing <prev>". Other products' versions stay (Claude Code 2.1.x,
  Windows 10 2004, Windows Terminal v1.24…). The draft's HTML-comment header (stripped before publishing)
  still records the range, the pre-releases merged and what was left out.
- **Worth knowing when you upgrade** merges every pre-release's upgrade warnings with the stable's own,
  de-duplicated, as they apply to someone upgrading from the last stable.

**Checks before applying** (the published body = the draft minus its comment, §7):
```bash
B=$SC/body.md; P=$SC/pub_<V>.md; S=$SC/stable_delta.md   # the merged body, a pre-release body, your delta draft
grep -c '^## Install' "$B"                                  # want 1 (and one each of What's new / What's fixed / Worth knowing)
grep -noiE 'pre-?release|nightly|\b0\.[0-9]+\.[0-9]+' "$B" | grep -v 'X\.Y\.Z'   # want only the release's own X.Y.Z
# every row is ONE row (§3): no sub-bullet under What's new / What's fixed; a long row needs a reason
awk "/^## What's (new|fixed)/{f=1;next} /^## /{f=0} f&&/^ +- /" "$B"             # want nothing
awk "/^## What's (new|fixed)/{f=1;next} /^## /{f=0} f&&/^- /&&length>240" "$B"   # each one: trim, or keep on purpose
# every lead item of the inputs is present, or merged / left out ON PURPOSE (say which in the comment header):
grep -ohE '^ *- (⚠️ )?\*\*[^*]+\*\*' "$P" "$S" | sed -E 's/^ *- //' | sort -u > $SC/old.txt
grep -ohE '^ *- (⚠️ )?\*\*[^*]+\*\*' "$B" | sed -E 's/^ *- //' | sort -u > $SC/new.txt
comm -23 $SC/old.txt $SC/new.txt                            # each line here needs a reason
```

---

## 7. Apply + publish

Strip the draft's HTML-comment header, then apply and publish (per §5 flags):
```bash
awk 'f{print} /^## Agentmaster X.Y.Z/{f=1; print}' draft.md > "$SC/body.md"   # drop the <!-- … --> header
grep -c '<!--' "$SC/body.md"        # want 0
gh release edit vX.Y.Z -R Nucs/Agentmaster --notes-file "$SC/body.md"
# then the stable OR pre-release publish line from §5, then verify:
gh release view vX.Y.Z -R Nucs/Agentmaster | grep -iE '^draft:|^prerelease:'
gh release list -R Nucs/Agentmaster --limit 4 | grep -vE '^\*'
```
`softprops/action-gh-release` writes a **terse default body** — always override it with `--notes-file`.
Verify what landed by reading it back: `diff -B <(env -u GH_TOKEN -u GITHUB_TOKEN gh api
repos/Nucs/Agentmaster/releases/tags/vX.Y.Z --jq .body 2>/dev/null) "$SC/body.md" && echo same` (a draft:
`releases?per_page=6`, selected by `tag_name`).

**Re-applying the notes of an OLDER release** (one that is not the Latest — the 2026-11-16 rewrite of
v0.8.0, v0.8.1 and v0.8.2-prerelease): the update endpoint documents `make_latest` with a default of
`true`, so never leave it out. Use `gh release edit v<old> -R Nucs/Agentmaster --notes-file "$SC/body.md"
--latest=false`, or what the rewrite used, `gh api -X PATCH repos/Nucs/Agentmaster/releases/<id> -F
body=@body.md -f make_latest=false` (`-F …=@file` reads the file; Git Bash rewrites a leading `/` into a
Windows path, so write `repos/…`, never `/repos/…`). Edit the Latest release LAST, with `make_latest=true`,
then check `gh api repos/Nucs/Agentmaster/releases/latest --jq .tag_name` and each release's flags.

Finally, commit the draft archive (single `git add` + `git commit`, extensive message, **no**
`Co-Authored-By`/"Generated with" trailers).

---

## Gotchas recap
- `-R Nucs/Agentmaster` on **every** `gh` call; `--jq` exiting 1 empty ⇒ use the plain form.
- A **PRE-RELEASE**'s notes show ONLY its OWN delta (vs the immediately-previous release) — never fold
  in earlier prereleases, never re-list an already-shipped change; each prerelease stands alone (§2).
- **One row per change** (§3): `- **Lead.** about one sentence` under `## What's new` / `## What's
  fixed`, bold only on the lead, and nothing long-form below the rows (no "New in X.Y.Z" prose, no
  per-version changelog, no `<details>` fold): the owner's "Head only" pick of 2026-11-16.
- A **STABLE** release's notes must carry the **whole** changelog since the last stable as ONE merged
  changelog — the intervening pre-releases' items merged into the stable's own What's new / What's
  fixed / Worth knowing, each in its final state, no pre-release (or other Agentmaster) version named (§6) — never a
  recap, never per-version sections. This merge is the ONLY place accumulation happens (the
  prerelease→stable moment).
- A **NIGHTLY** is invisible to baselining in both directions (§2): its own notes may be terse, the
  next real release baselines off the previous NON-nightly release (re-listing nightly-shipped work
  is CORRECT), and the stable fold never includes it. Same publish flags as a pre-release, never
  `--latest`, never convert-in-place (the gating is the TAG).
- Flip framing **and** flags together; `--latest=false` alone won't move the badge — `--latest` the
  release you want (§5).
- Strip the `<!-- … -->` draft header before `--notes-file`.
- Re-applying an OLDER release's notes: pass `make_latest=false` (`--latest=false`) explicitly, and
  edit the Latest release last (§7).
- Asset names are `Agentmaster_X.Y.Z.0.*` (ver4) + `Agentmaster.cer`; the one-command installer uses
  `-Version X.Y.Z` (ver3) — **except a nightly, whose `-Version` is the FULL suffixed
  `X.Y.Z-prerelease-nightly`** (it maps to the tag; the assets stay numeric ver4).
- The `> **This is a pre-release.**` blockquote + `(pre-release)` title suffix belong to pre-releases
  ONLY — a "0.6.2 is now the default Latest" line on a pre-release is the classic reframe-miss; a
  nightly wears the ⚠️ NIGHTLY blockquote instead (§4).
