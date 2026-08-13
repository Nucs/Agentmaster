<!-- Agentmaster 0.7.4-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.3-prerelease-nightly..7dc5254b7 = 11 commits (3 docs incl. the 0.7.3 notes archive,
     1 dev-only test SKILL claude-wrap-ConPTY; 6 user-facing). Baseline = the previous release of ANY
     kind = the 0.7.3 nightly (release-notes §2). RELEASING from 7dc5254b7 (= origin/agentmaster tip
     after push).
     Engine gate at that tree: 3244 checks, 0 failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the MSIX
     Identity Version (ver4 stays numeric 0.7.4.0, so ASSET NAMES carry no suffix) and auto-marks the release
     prerelease. The in-app updater offers it ONLY to users who accepted the cog's warning-gated "Allow
     updating to nightly builds (unstable)" switch — pre-release-opted users skip it. The one-command
     installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.4-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.4.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.4-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

**Placing a queued prompt is now verified end-to-end — it can no longer merge into your draft or invisible terminal content, and the autorunner stops giving up on false signals.** Building directly on 0.7.3's delivery gate, this nightly turns the send into **fill → read the box back → submit**: the prompt is typed, confirmed present exactly as written, and only then does the Enter fire. A tri-state box read distinguishes empty / draft / *menu-open* (an earlier blind write once merged a 13-character prompt with ~4 KB of TUI-internal content no read had seen), and a confirm-and-retry pass means the Tests Autorunner keeps retrying a delivery instead of pausing itself to Off on a false "lost" or "box not visible" signal — and never overrides a mode you set by hand. Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## New

- **Drag a tab onto another window to dock it there.** Cross-window docking is back: releasing a dragged tab over another Agentmaster window's tab strip now **moves** the tab into that window at the aimed slot (with the target's own insertion caret previewing the drop), instead of always tearing out into a new window. This closes the follow-up left open when the native drag pipeline was retired to kill the tab-drag crash — rebuilt on the same-process, pointer-owned gesture.
- **Batch-close a folder's sessions from the tab strip too.** The WT tab right-click **Close ▸** submenu now carries **Of Same Folder** and **Other of Same Folder**, matching the Triage-Board / Explorer-tree menu added in 0.7.3 — so you can close every session in a working directory (or all but this one) from wherever you right-click.

## Fixes

- **Verified prompt placement — no more merged or lost sends.** A queued prompt is now **filled into the box, read back to confirm it landed exactly, and only then submitted** — closing the class where a send merged with an unsent draft or with invisible terminal content and was silently recorded as typed. The box read is tri-state (empty / draft / menu-open), so a send declines when a menu is up instead of pasting into it, and a merge caught before commit is undone rather than sent.
- **The autorunner stops giving up on false signals.** After the verified-placement wave, two of its new "pause to Off" paths fired on false readings — a delivery still queued behind a wedged dispatcher was judged "lost" one second after a timeout, and a box-not-visible escalation paused a session on a clock that predated the user's own re-arm. Delivery is now confirmed-and-retried, a Sent-but-never-injected prompt is reclaimed and resent, and a manual **Full** is never overridden by a stale give-up.
- **The Enter-retry watchdog won't submit a foreign draft.** The lost-send reconciler and a draft guard mean the watchdog's rescue Enter never fires into a box holding text that isn't the prompt it's watching (which would have submitted your half-typed message), plus hardening against empty duplicate-hook "phantom" submit events.
- **A session stuck red "Error" through a whole retry turn.** An errored session you retried — with the agent visibly working again — could stay **Error** for the entire retry turn (measured at 35 minutes) because the pull-recovery edge was dead code and the scanner re-asserted Error 38 ms after the retry's real submit. It now recovers to Running on the retry, and won't re-assert Error on a superseded tail.
- **The MAIL button's move no longer double-sends.** Moving a tab's draft into the queue cleared the box with Claude's Ctrl+S *stash* — which Claude auto-restores into the box ~0.4 s after the next submit, re-planting the moved draft right after its queued copy was delivered (reading as a double-send). The move now uses a real discard, so the draft is gone once queued.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.4-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.4.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.4.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.4.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
