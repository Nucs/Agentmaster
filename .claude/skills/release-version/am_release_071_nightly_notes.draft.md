<!-- Agentmaster 0.7.1-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.0..6695ac853 = 22 commits. RELEASING from 6695ac853 (= origin/agentmaster tip).
     Engine gate at that tree: 2985 checks, 0 failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the
     MSIX Identity Version (ver4 stays numeric 0.7.1.0, so ASSET NAMES carry no suffix) and auto-marks
     the release prerelease. The in-app updater offers it ONLY to users who accepted the cog's
     warning-gated "Allow updating to nightly builds (unstable)" switch — pre-release-opted users skip it.
     The one-command installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.1-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.1.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.1-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

**A snapshot of mainline the day after 0.7.0**, carrying 22 commits of in-flight work: the tab hover-card became a real scrollable surface, the Triage Board grew tag-filter chips, the unsent-draft pipeline learned to reconcile with the Manager's compose box and to restore itself into a reopened tab, and completion toasts now tell you *which* request came back. Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## New

- **The tab hover card scrolls.** The rich tab tooltip's body now takes the mouse wheel (with a slim auto-hiding scrollbar that appears only while you scroll), so a session with a long summary is readable instead of clipped.
- **A MAIL button on the tab overlay.** Queue the tab's **unsent draft** straight into the Tests Autorunner from the badge — the Manager compose row's "add to queue" envelope, on the tab itself. The draft stays where it is (it's a copy). Dev/debug builds only, Claude only.
- **Bookmark-tag filter chips on the Triage Board.** The Sessions browser's tag chips now lead the board header too, right after **Clear** — except they combine with **OR** (triage is "show me anything tagged release *or* hotfix"), they're persisted per window, and an **Untagged** chip leads the row (on by default) as the one switch that says "only tagged cards."
- **The tab color picker opens ready.** Custom — and its advanced RGB/HSV/Hex inputs — start expanded instead of collapsed, each toggle is remembered, and a new **Use Tab Color** button seeds the custom picker with the color the tab already wears.
- **Completion toasts say which request finished.** A third toast line carries the truncated prompt whose turn just completed, so a fleet-wide pop tells you *what* came back — and the toast now stays on screen ~25 s instead of the short default.
- **The Manager's compose box reconciles with your tab draft.** Click into it and it pulls the tab's unsent prompt in; keep typing in the terminal and it *extends* in place; type something genuinely different and a **pull button** offers the replace explicitly (never silently overwriting text you composed). Every draft change is also cached in a durable per-session store.
- **A reopened session gets its unsent draft typed back.** The remembered draft is re-filled into the fresh input box (fill-only, never sent, verified by reading the box back), so reopening a tab no longer loses the message you'd half-written. On by default; off in Settings.
- **Copy a single prompt from the summary panel.** Right-click any numbered prompt → **Copy Prompt** for that exact prompt, verbatim.
- **The updater's "Not now" is now a real choice.** The button became the default radio **"Remind me next restart,"** sitting alongside tomorrow / 3 / 7 / 30 days / skip — and "tomorrow" is proven to resolve to the next **local** 08:00.
- **Briefings are never deleted by default.** `/handover`'s delete-after-hand-off is now off on every install (a briefing is a document you may still want), and its old hard-coded 10-minute wait became a real **24-hour** setting — a background successor may not be visited for hours.

## Fixes

- **A UI freeze when arming SemiAuto.** The autorunner wrote to the registry on every pass, spinning a notify → observe → advance loop at roughly 3 ms per cycle that locked up the release UI; those writes are now change-gated.
- **Tab tooltips that never opened.** A tooltip attached mid-hover would stay dark forever, and every tab recycle detached it — both fixed at the root, so hover cards appear reliably.
- **A two-file `/handover` losing its second successor.** The seal that pairs a command's writes with its turn fired on a 20-second write-silence fallback while Claude was still generating the second briefing (transcripts are byte-silent mid-stream), so the second successor was never spawned. The seal now holds while the turn is demonstrably still in flight.
- **The draft swap restores your text properly.** Stashing your unsent draft aside while a queued prompt sends now uses Claude's own **Ctrl+S** whole-box stash instead of Ctrl+U, which was line-scoped and handed the text back with the cursor stranded mid-string.
- **Board chips stay put.** The tag chip strip is ordered alphabetically rather than by activity, so it can't reshuffle under your pointer between aiming and clicking, and the scope / "Show all" controls no longer hide under the tooltip panel.
- **Summary-panel context menus are contained.** A throw in a XAML event handler is a process fail-fast, not a lost menu — all three handlers are now guarded.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.1-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
