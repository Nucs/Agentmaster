<!-- Agentmaster 0.5.4 release notes — PRE-RELEASE. Tag v0.5.4 -> b6c756c19 (HEAD).
     Supersedes the never-published 0.5.3 (tagged then cancelled/deleted before any draft existed).
     Cumulative since v0.5.2 (the last PUBLISHED/stable release) — 27 commits = the 0.5.3 feature set
     + 4 new: durable per-session title store (b6c756c19), Triage-Board refresh button (bff9c6eb3),
     scope-toggle emphasis (2ba424a7c), recap appended to the tab tooltip uncapped (fb7660b21).
     PRERELEASE mechanics: publish --prerelease, NOT --latest (0.5.2 stays stable Latest).
     Asset names: tag v0.5.4 -> 0.5.4.0. -->
## Agentmaster 0.5.4 (pre-release)

A fork of **Windows Terminal** that turns it into a manager for multiple **AI coding agents** (**Claude Code** + **Codex**). **0.5.4 is a pre-release** — a large feature drop for early adopters: rich tab-strip hover tooltips, Claude's idle **recap** surfaced across the UI, a Waiting-for-you **"unread inbox"** model, an **updater overhaul** (with one-click uninstall), **durable session titles**, per-tab prompt navigation, and more. Enable **Settings → Updates → Allow pre-release versions** to receive it automatically, or install from the assets below. The stable **0.5.2** remains the default for everyone else.

## New in 0.5.4

- **Rich, session-aware tab-strip tooltips.** Hovering any tab the Fleet Observer classifies now shows a compact session **card** instead of the bare title: live **state** + *why* it needs you, the **full title**, `agent · model · effort`, the **full working dir + branch**, **queued count + the next prompt**, **Autopilot** progress, the **last user message paired with the agent's last reply**, recency/age — and the idle **recap** at the bottom (uncapped). Every line is conditional, so a quiet tab stays short; a shell/unlinked tab keeps a light `○ kind · unlinked` twin.
- **Claude's idle "recap," surfaced everywhere.** When a conversation sits idle, Claude Code writes a one-paragraph *"what we did / what's next"* recap; 0.5.4 surfaces it (in full) on the **Triage-Board card tooltip**, the **tab-strip tooltip**, the **summary panel**, the **Sessions detail**, the **copyable Summary**, and the **`agentmaster show` CLI** — so you can tell dense, same-named sessions apart at a glance.
- **Durable per-session titles.** A session's name — the one you set, or the one derived from its conversation — is now kept in a **durable per-session store**, so it persists **across windows and across app restarts**. A rename sticks everywhere, every run, instead of only for the live session.
- **Waiting-for-you is now an "unread inbox."** A finished turn persists in the Waiting-for-you column like an unread item: it stays until you've actually **looked at the tab** (or a generous, configurable timeout lapses — now **1 hour** by default, adjustable 1 min – 3 days or **Never**), you can **Mark Unread** to pin it (sticky across every window), and each card shows a **1px countdown bar** draining over the timeout. Claude's ~5-minute server cache is split out into a separate, cosmetic **⚡ "still cached"** hint.
- **Updater overhaul + one-click uninstall.** The in-app updater now ships a real **embedded installer** (mirroring the standalone installer's full recovery), **self-heals the `0x80073CFB`** "a packaged version can't replace your loose install" conflict, adds an **"Uninstall Agentmaster"** button in Settings, and its **"What's New"** + changelog links open the specific GitHub release page. A **dev build no longer offers a release as a self-update** (the updater manages the Release install).
- **Per-tab badge: prompt navigation + next-prompt preview.** The link badge gains **↑ / ↓ buttons** that scroll the terminal to your previous / next off-screen sent prompt (the same as Alt+↑/↓, with the summary-row highlight), and a **third row that previews the next queued prompt** — so you see *what's* next, not just *how many*.
- **Sessions: auto-hide deleted + a "Hidden" reveal filter.** Permanently deleting a session now also **hides it from the Sessions browser** (the transcript on disk is still kept and resumable); a new **"Hidden"** checkbox reveals the hidden set (dimmed) with a per-row **Unhide**.
- **Autopilot now drives adopted sessions.** Toggling Autopilot — or an idle-start — on an **adopted** session now sends its queued prompts. The scheduler gates on whether it *can* drive a session, not how it was started (fixes *"switching Autopilot off and back doesn't send pending messages"*).
- **"Claude not found" handled on every launch path.** Adopt, Open New Session Here, Restore here, Resume here, Fork here, and double-click resume/fork now **prompt** (instead of silently doing nothing) when no native `claude.exe` is present — and the gate **auto-recovers** the moment you install Claude (no restart, no manual re-check).
- **Manager header polish.** The Triage Board gains a **↻ refresh** button (the twin of the Explorer Tree's) to re-pull the fleet on demand, and the **LOCAL/GLOBAL scope toggle** is now visually emphasized over its sibling header buttons.

## Fixes

- **No more hover flicker on cards or rows.** Triage-Board cards/rows no longer flicker their hover ring (and the linked tab-strip pill) as the pointer crosses inner labels, and the **Sessions / Archive** page rows no longer flicker tooltips or swallow a click — each row is now one clean hit target with a single consolidated tooltip (every field still shown on hover).

## Capabilities

- **Real sessions, real control** — each session is a live agent (`claude.exe` / Codex) on a ConPTY with shared stdin; state comes from hooks + an out-of-band observer, never screen-scraping.
- **Manager tab** (pinned, leftmost) — a **sortable, refreshable Triage Board** (color-coded cards, recap-on-hover, Waiting-for-you countdowns), an **Explorer Tree** (LOCAL · GLOBAL · EXTERNAL scopes + sorting), and a **Flight Plan** (per-session prompt queue + history). **Autopilot** auto-sends queued prompts on turn-complete with backstops (max sends, stop-on-error, pause, question-guard, Enter-retry) and drives **adopted** sessions too.
- **Per-tab lens** — every classified tab carries a top-right link badge (status · `model · effort · kind` · workdir/branch · **next queued prompt**) with always-on actions (Open Path, Copy menu, prompt ↑/↓, summary pencil) and a toggleable **summary panel** (recap · plan · tasks · messages · files · live times) with per-prompt **Jump**; a rich session card appears on tab-strip hover.
- **Fleet Observer** — detects and manages **every** session, including a hand-typed `claude`/Codex in any tab (no hooks needed), and observes external ones read-only, with **Adopt** (fork-a-copy or resume).
- **Sessions & Archive pages** — full-window browsers over every on-disk conversation and every archived session: indexed + ripgrep search (title · dir · branch · prompts · content), time range, Resume / **Fork** / Jump / bulk-open / reopen-whole-window, hide/unhide. Session titles persist across runs.
- **Workspace persistence** — windows reopen at their geometry with sessions resumed and shell tabs replayed at their cwd; permanent per-directory tab colors; the `agentmaster <verb>` CLI introspects the fleet from any shell.
- **Coexists with Windows Terminal** — installs side-by-side under its own package identity; your real Windows Terminal install is left untouched.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.2** stays the default Latest.

**One command (PowerShell)** — install 0.5.4 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.4
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.4.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.4.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.4.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
