<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.7.0 release notes — STABLE (becomes Latest; the 0.6.x line graduates).
     STABLE = the comprehensive changelog since the last stable (0.6.1): a curated Highlights reel +
     the 0.7.0-only batch (v0.6.9..24d4a993c = 28 code commits) + the full per-version fold of every
     intervening pre-release (0.6.3..0.6.9). release-notes skill §6.
     RELEASING from 24d4a993c (= origin/agentmaster tip). Tag v0.7.0 -> ver4 0.7.0.0.
     Engine gate at that tree: 2840 checks, 0 failures.
     Publish: gh release edit v0.7.0 -R Nucs/Agentmaster --draft=false --latest --prerelease=false
       (REPLACES 0.6.1 as Latest). Assets: Agentmaster_0.7.0.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.0

**0.7.0 is a stable release — the entire 0.6.x pre-release line plus a final batch, now the default for everyone.** This build becomes the new default **Latest**; below are the highlights since the last stable (0.6.1).

## What's new

- **The `/handover` family.** `/handover` has Claude write a briefing that becomes a fresh session's first message, `/handover-here` puts the successor in the current tab, and `/handover-standby` types it there without sending.
- **Windows notifications.** Every managed session raises a toast when it leaves Running, and a click brings its window to the front and jumps to its tab.
- **Launch on any model.** Every Open New Session Here and Fork session is a picker: Default, your configured models, and Specify… for any model id.
- **Workspace pre-trust.** Agentmaster trusts the working folder before a launch, so Claude's startup trust question can no longer park an unattended tab.
- **Tab-title naming.** An untitled tab's title can come from the folder's last word, its name, two folders, capitals or the git branch, with a live preview.
- **The live model on every card.** Each session shows the model it is actually on, read from the transcript, on its board card, tab tooltip and overlay.
- **Debug Mode and a one-click Claude install.** Settings → About → Enable Debug Mode unlocks Auto Testing in a release install, and the "Claude not found" prompt installs Claude in one click.
- **Tab display options.** A "Remove colors" mode, a hidden profile icon, a strip that scrolls to the selected session's tab, and the spark crown while a session's prompt cache is warm.
- **Smarter state and tab colors.** A running shell, background agent or teammate keeps a session Running, a repo and its worktrees share one color, and a session launched in your home folder infers where it works.
- **The installer asks instead of elevating, and a nightly channel.** The one-command installer offers an administrator install, a no-admin portable install or cancel, and nightlies sit below pre-releases behind a warning.

## What's fixed

- **The 23 GB memory leak is gone.** A reference-cycle leak in the Manager grew about 85 MB a minute under a busy fleet until the UI froze.
- **No more resume crash loop.** Heavy sessions no longer die in a self-sustaining loop seconds after launch.
- **The 0.6.8 UI freeze is fixed.** The summary panel no longer re-resolves every session on the UI thread.
- **Automatic updates work.** 0.6.x installs never prompted for an update, because the preferences sat at the wrong level of settings.json.
- **An answered question no longer leaves the card orange.** A session goes back to Running within a heartbeat instead of a median 39 s.
- **A fatally errored session no longer shows a green Done.** The error now outranks the "session ended" label.
- **Auto Testing runs when Debug Mode is on.** Debug Mode arms the Tests Autorunner, and adopted or /clear-re-homed sessions get your default mode.
- **Closing a window or dragging a tab no longer crashes.** UI-thread teardown is guarded, and so is the tab-drag lookup.
- **Restart session really restarts.** No "259" banner, no false archive, a fork keeps its branch, and /model no longer flips an idle session to Running.
- **Smaller fixes.** A runaway terminal auto-scroll stops, and a turn under 10 s raises no toast.

## Install

**One command (PowerShell)** — installs 0.7.0 (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.0
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.7.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.7.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
