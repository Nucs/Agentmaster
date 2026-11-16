<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.7.7-prerelease release notes — PRE-RELEASE (the stable 0.7.0 stays Latest).
     Range: v0.7.0..447ae26e3 = 89 commits (73 user-facing, 16 docs/chore/refactor/merge).
     Baseline = the last NON-nightly release = 0.7.0. The 0.7.1–0.7.6 nightlies are INVISIBLE to
     baselining (release-notes §2), so this pre-release RE-LISTS their work (almost nobody installed
     them) + the 11 commits past the 0.7.6 nightly tip (5bf3d08e6): the interrupt hold, the MSI-like
     portable install, the HIDDEN-lens perf fix, the orphan reaper, Deactivate, Transcript Followup.
     RELEASING from 447ae26e3 (= origin/agentmaster tip).
     Tag v0.7.7-prerelease -> ver4 0.7.7.0, rawver 0.7.7-prerelease. nightly=false, so release.yml
     does NOT auto-mark prerelease — mark it at publish. The -Version installer arg maps to the TAG,
     so it carries the FULL suffix (0.7.7-prerelease); asset names stay numeric ver4 (0.7.7.0).
     Publish: gh release edit v0.7.7-prerelease -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.7.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.7-prerelease

**0.7.7 is a pre-release** consolidating the whole 0.7.x development line — the 0.7.1–0.7.6 nightlies plus a final batch — on top of the 0.7.0 stable. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.7.0** remains the default Latest for everyone else.

## What's new

- **Deactivate a tab.** Deactivate ▸ in the tab menu stops a session's claude or codex while its tab keeps its place, title, color and queue, and entering the tab resumes the conversation.
- **A portable copy installs like an app, or stays portable.** Its first launch offers a per-user install with icons, a folder right-click verb, an Apps & Features entry and the CLI on PATH, a folder you pick, or staying portable.
- **The app is now `Agentmaster.exe`.** Task Manager and the zip say Agentmaster, and a `WindowsTerminal.exe` forwarder keeps old shortcuts, pins and scripts working.
- **Cross-window tab docking.** A tab dragged onto another Agentmaster window's tab strip moves into that window at the aimed slot.
- **Batch-close a folder's sessions.** Close ▸ Of Same Folder and Other of Same Folder close every session of a folder, across windows, after one confirm.
- **Transcript Followup.** Every copy menu copies the conversation folded per turn, your message and the final reply, as a brief to paste into a fresh session.
- **Unsent drafts, end to end.** The overlay's mail button moves a tab's unsent prompt into the queue, the compose box takes in a live draft, and a reopened session gets its draft typed back.
- **Esc parks the autorunner.** Interrupting a turn holds that session's queue until your next message, instead of sending the next queued prompt into the session you just stopped.
- **Tag filter chips on the Triage Board.** The board's header filters its cards by tag, any of the picked ones, with an Untagged chip for the rest.
- **Completion toasts say which request finished.** A third line shows the prompt whose turn just completed, and the toast stays about 25 s.
- **The summary lists loaded skills.** A Skills Loaded section joins the summary, and a scratchpad file is labeled `scratchpad/<name>`.
- **A locked terminal shows the "unavailable" cursor.** While a pane is read-only, such as during a queued prompt's delivery, the pointer over it is a circle with a slash.
- **The tab color picker opens ready.** Custom and its RGB / HSV / Hex inputs start expanded, and Use Tab Color seeds it with the tab's current color.
- **Copy a single prompt from the summary.** A right-click on a numbered prompt copies that exact prompt.
- **The updater's "Not now" is a real choice.** Remind me next restart is the default, beside tomorrow at 08:00, 3, 7 or 30 days, or skipping the version.
- **A sensible `EDITOR` default.** Sessions get `EDITOR=edit` when Microsoft's edit.exe is on PATH and nothing else claimed `EDITOR`.

## What's fixed

- **Three UI-freeze classes are healed.** A Remote Desktop theme storm, a "window stops accepting clicks" wedge that now heals itself, and the hidden Manager rebuilding on every change.
- **The WinUI tab-drag crash is gone.** Native tab drag is off by construction, and reorder and tear-out are Agentmaster's own pointer gesture.
- **Orphaned console groups are reaped.** A session whose terminal host was killed no longer lives on forever with a dead console.
- **Auto Testing delivery is atomic and verified.** A queued prompt is filled, read back and only then submitted, so it no longer stacks, double-sends or merges into your draft.
- **Sessions no longer stick in the wrong state.** A retried API error returns to Running, and a transcript line over 1 MiB no longer strands a session on Running.
- **Clearing a draft leaves no empty lines and no double send.** The automatic clear keeps going until one empty row remains, and the mail button's move is a real discard.
- **Smaller fixes.** Tab hover tooltips open again, a two-file /handover keeps its second successor, and the overlay's file lists stay one line per item.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs 0.7.7-prerelease (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.7-prerelease
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.7.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained — its first launch offers a per-user install or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.7.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.7.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
