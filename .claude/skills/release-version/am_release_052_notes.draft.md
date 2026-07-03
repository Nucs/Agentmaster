<!-- Agentmaster 0.5.2 release notes. Tag v0.5.2 -> d63cf610b (the user-specified release commit).
     Range v0.5.1..d63cf610b — 13 commits (12 feat/fix below + 1 docs-only: 777746b13 summary-jump v2 plan, omitted).
     DEFERRED to a later release (NOT in 0.5.2, committed/WIP after d63cf610b):
       * 17f3ed30f fix(autopilot): drive ADOPTED sessions on toggle/idle-start.
       * uncommitted WIP — (a) Waiting-for-you "unread" model (read-gated decay + sticky Mark-Unread + 60m
         decay split from a cosmetic ⚡ serverCache indicator); (b) auto-recovering "Claude not found" gate.
     Asset names: tag v0.5.2 -> 0.5.2.0. -->
## Agentmaster 0.5.2

A fork of **Windows Terminal** that turns it into a manager for multiple **AI coding agents** (**Claude Code** + **Codex**). **0.5.2** is a polish-and-control release: the Manager UI now renders **fully dark** on any theme, you can **navigate between your sent prompts from the keyboard**, the Sessions search gains a **Title scope**, the Launch box gets a **Browse…** dialog, and the Triage Board tidies itself up.

## New in 0.5.2

- **Dark Manager UI on any theme.** The Manager tab and all its surfaces — including its hover tooltips — now render **dark regardless of your Windows / Terminal theme**, fixing the light-gray bleed that showed through the board/tree/flight-plan gaps in light mode.
- **Alt+↑ / Alt+↓ — step between your sent prompts.** On a Claude tab, **Alt+↑** scrolls the terminal to center the nearest sent prompt *above* the viewport and **Alt+↓** the nearest *below* (a boundary sound plays at the ends). It reuses the summary-Jump resolver, so duplicate prompt texts still map to the right occurrence, and it works whether or not the summary panel is open. Rebindable in settings; on a non-Claude tab the keys keep their default pane-navigation behavior.
- **Summary-Jump highlight.** Jumping to a prompt — via the ▸ button or the new Alt-nav — now **highlights that message's row** in the summary panel and brings it into view, so it's obvious where you landed.
- **Sessions: 🏷 Title search scope.** A new **Title** toggle (default on) in the Sessions search bar gates title matching, and an **open session's live (renamed) tab title is now searchable** — so a session you renamed in-app is findable by the name shown.
- **Launch box: Browse… folder dialog.** The path-picker dropdown now pins a **Browse…** row at the top (a native folder dialog) in every mode; the folder you pick is dropped into the box **and joins your recent directories immediately**.
- **"Always display Home button" setting.** A new TABS setting (**on by default**) keeps the Home button — the jump back to the pinned Agent Manager tab — visible whenever you're on another tab, instead of only once the Manager tab scrolls out of view.
- **Triage Board tidies itself.** The **Error column auto-collapses** when no session is errored (and snaps back into place the moment one errors), and a window left holding **only the Manager tab self-closes** (unless it's the last window) and is never saved or restored as a content-less window.

## Fixes

- **Triage cards no longer shift on hover.** The hover/selection outline is now drawn as a non-layout overlay ring, so hovering or selecting a card no longer reflows the board or nudges the title band — and every card is the same size regardless of hover/selected state.
- **Calmer card tooltips.** Triage-Board card tooltips now open after a deliberate **4 s** (instead of the fast global delay), so panning the mouse across the board no longer flashes a tip over every card; the redundant whole-card tooltip was removed (the title band and per-element tips already cover it).

## Capabilities

- **Real sessions, real control** — each session is a live agent (`claude.exe` / Codex) on a ConPTY with shared stdin; state comes from hooks + an out-of-band observer, never screen-scraping.
- **Manager tab** (pinned, leftmost) — a **sortable Triage Board** (color-coded cards), an **Explorer Tree** (LOCAL · GLOBAL · EXTERNAL scopes + sorting), and a **Flight Plan** (per-session prompt queue + history). **Autopilot** auto-sends queued prompts on turn-complete with backstops (max sends, stop-on-error, pause, question-guard, Enter-retry).
- **Per-tab lens** — every classified tab carries a top-right link badge (status · `model · effort · kind` · workdir/branch) with always-on actions (Open Path, Copy menu, summary pencil) and a toggleable **summary panel** (plan · tasks · messages · files · live times) with **per-prompt Jump** + keyboard prompt navigation; the terminal search box docks into the same HUD.
- **Fleet Observer** — detects and manages **every** session, including a hand-typed `claude`/Codex in any tab (no hooks needed), and observes external ones read-only, with **Adopt** (fork-a-copy or resume).
- **Sessions & Archive pages** — full-window browsers over every on-disk conversation and every archived session: indexed + ripgrep search (title · dir · branch · prompts · content), time range, Resume / **Fork** / Jump / bulk-open / reopen-whole-window, hide-from-list.
- **Workspace persistence** — windows reopen at their geometry with sessions resumed and shell tabs replayed at their cwd; permanent per-directory tab colors; the `agentmaster <verb>` CLI introspects the fleet from any shell.
- **Coexists with Windows Terminal** — installs side-by-side under its own package identity; your real Windows Terminal install is left untouched.

## Install

**One command (PowerShell)** — install or upgrade to 0.5.2 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.2
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). *Already on 0.5.0+? Just use **Settings → Check for updates** (or the startup prompt).*

**Portable (no cert):** download `Agentmaster_0.5.2.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.2.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.2.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
