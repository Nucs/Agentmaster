# 21 · Smaller features across the releases (MSC)

> Conventions and fixtures: [README.md](README.md). Spec: CLAUDE.md *Bookmark TAGS* (and its *What the tag surfaces
> cost*), *C1 UI* (sorts, the timing line, the launch bar), *Settings cog* (launch models, Specify a model), *Settings cog*
> (`trustWorkspaceOnLaunch`); UI_THREAD.md §11; HOOKS.md *Workspace trust*. Each flow is short; together they cover the
> items of the release notes that no other file owns.

---

### MSC-01 · Tag filter chips on the Triage Board

> **Since** 0.7.7 · **P1** · **Needs** F-S2 + 4 more sessions, tags `release` (2 sessions) and `hotfix` (1) · **Spec** CLAUDE.md *Bookmark TAGS* (Everywhere else)

**Phases**
1. **The row.** *Expect:* after **Clear**: an `Untagged` chip with a HOLLOW ribbon (picked: an accent border), then `hotfix`,
   `release` in ALPHABETICAL order; no background fill on a picked chip.
2. **OR.** *Do:* pick `release` and `hotfix`. *Expect:* untagged cards still shown (Untagged on) plus both tags' cards.
3. **Only tagged.** *Do:* Untagged off. *Expect:* only the three tagged cards; a trailing ✕ appears.
4. **✕.** *Expect:* picks dropped AND Untagged back on (the default view).
5. **Per window, persisted.** *Do:* set a filter in window A; restart. *Expect:* A reopens with it; window B unaffected.

**Edge cases**
- **E1 A picked tag whose last carrier closes** → the chip stays (clickable off), never pruned by itself.
- **E2 No tag exists and Untagged is on** → the row collapses; Untagged OFF with no tags → the row still shows (so it can be
  turned back on).
- **E3 A rebuild while aiming** (a session changes state) → the chips do not reorder under the pointer.

---

### MSC-02 · The Tags panel opens at once and never loses a tag

> **Since** 0.7.9 (speed) / ≤ 0.7.7 (the panel) · **P1** · **Needs** a profile with 2,000 session-store files, 9 tagged · **Spec** CLAUDE.md *Bookmark TAGS*; UI_THREAD.md §11

**Phases**
1. **Cold open.** *Do:* right after the app starts (before the engine's warm-up line), open the tab menu → Tags. *Expect:* the
   panel at once with "Loading tags…", the **+** hidden and Enter adding nothing until the list arrives. *Check:* [LOG]
   `[tags] the session store was not read yet when a tag panel opened: …`; later `[engine] session store warmed: N
   record(s), …`.
2. **Warm open.** *Expect:* under 100 ms; the list `[✓][ribbon][name][·count]`, "K of N tags".
3. **Add.** *Do:* type `perf` + Enter. *Expect:* a new tag with the pre-picked random swatch; the swatch re-rolled for the
   next add; the tab's ribbon appears at once; other surfaces (Sessions page, board, side panel) follow within ~40 ms of the
   last edit.
4. **Untag the last carrier.** *Expect:* `perf` stays listed at `·0`; only now a ✕ shows on its row; ✕ deletes it.
5. **Recolor.** *Do:* tap a swatch, re-add an existing name. *Expect:* that tag recolored everywhere (one color per tag).

**Edge cases**
- **E1 The 21st name with Max tags = 20** → refused; applying an existing tag still works.
- **E2 Tooltips in the panel** → open after 3 s of rest; a pending one never opens after the panel closed.
- **E3 Hover a tab's ribbon** → the hover panel lists every carrier with its state; a click on a live one jumps to its tab
  (in another window too).

---

### MSC-03 · MOST RECENT sorts by turns; a new session starts at the top

> **Since** 0.7.9 · **P1** · **Needs** mock, F-S2 + 3 sessions · **Spec** CLAUDE.md *C1 UI* (the sort toggle)

**Phases**
1. **Pick MOST RECENT** on the board (and the side panel follows). *Do:* session A runs a long turn of 30 tool calls
   (`[[mock bash "1"; bash "2"; …; reply "a"]]`) while B's turn starts after A's. *Expect:* B above A during A's tool calls
   (tool calls and side-file writes never move a card); when A's turn ends, A on top.
2. **New session / fork.** *Do:* launch a session; fork one. *Expect:* each new card at the TOP in MOST RECENT (and under the
   Running pin in MOST ACTIVE) — in the tree, the board and the side panel; the side panel's ⌖ then keeps it in view.
3. **The setting persists** (`boardSort`, `treeSort`) across a restart.

---

### MSC-04 · The timing line

> **Since** 0.7.9 · **P2** · **Needs** seeded sessions of known ages (F-CORP) · **Spec** CLAUDE.md *C1 UI* (the timing line)

**Phases**
1. **Ages.** *Expect:* a session created 4 days 1 hour ago, active 2 min 4 s ago reads `created 4d1h, active 2m4s ago`; one
   created 3 min ago reads `created now`; 40 min ago `created recently`; never a month unit; `active … ago` left out when
   there is no activity.
2. **It moves.** *Expect:* the board's line updates every 30 s without a rebuild (focus and scroll kept).

---

### MSC-05 · "Specify a model…" remembers; the launch folder list grows

> **Since** 0.7.9 · **P1** · **Needs** mock, F-W2 · **Spec** CLAUDE.md *Settings cog* (launch models)

**Phases**
1. **Pre-filled.** *Do:* Open New Session Here ▸ Specify… → type `claude-sonnet-5-5` → Enter. *Expect:* the session runs on it
   ([MOCK]); the next Specify… (in ANOTHER window, and after a restart) opens pre-filled with it, selected so typing replaces
   it; its drop-down lists the configured models (Fable, Opus, Sonnet) then the remembered ids.
2. **Keys act on the key-up.** *Do:* in a terminal tab behind, open Specify…, press Escape. *Expect:* the prompt closes and NO
   Esc reaches the terminal (a running turn is not interrupted — [TR] no interrupt marker); Enter launches and no CR reaches
   the terminal.
3. **Autocomplete does not rewrite.** *Do:* with `claude-opus-4-1` committed and a fetched `claude-opus-4-1-20250805` in the
   list, open the prompt. *Expect:* the box reads `claude-opus-4-1`.
4. **Blank.** *Do:* clear the box, Enter. *Expect:* refused with a hint (never Default by accident).
5. **The folder list.** *Do:* focus the launch box in a tall window. *Expect:* the drop-down grows with its rows up to 90% of
   the window's height, never past the window's bottom.

**Edge cases**
- **E1 Fetch list without an API key** → the Codex list loads; the status says Anthropic's list needs `ANTHROPIC_API_KEY` /
  `ANTHROPIC_AUTH_TOKEN`; the remembered ids are replaced by the fetched ones (configured models always shown).
- **E2 The launch-model list edited in the cog** → offered at the next menu open, live; a duplicate name or an id with a space
  colors the box amber with a status line.

---

### MSC-06 · The Settings card

> **Since** 0.8.0 (width) · **P2** · **Needs** a 1280 × 800 window · **Spec** CLAUDE.md *Settings cog*

**Phases**
1. **One row of tabs.** *Do:* ⚙. *Expect:* all ten tabs on one row ("Claude" and "About" included); the card a modal over the
   page with a dimmed backdrop; a text box takes typing.
2. **Save / Cancel / Reset.** *Do:* change a setting → Cancel → reopen. *Expect:* unchanged. The Commands tab's Reset → its
   controls back to defaults, nothing saved until Save.
3. **Out-of-cog settings survive a Save.** *Do:* pick a transcript level (TRN-01), drag the summary panel's size (see TRN-K1),
   flip the AI search's thinking level; then Save in the cog. *Expect:* none of them reset.

---

### MSC-07 · `EDITOR` points at Microsoft Edit when nothing claimed it

> **Since** 0.7.7 · **P2** · **Needs** `edit.exe` on the guest's PATH · **Spec** ClaudeSpawn.cpp (the computed EDITOR default)

**Phases**
1. **Default.** *Do:* launch a session; send `[[mock bash "echo $EDITOR"; expect "edit"; reply "ok"]]` (the Bash tool
   inherits claude's environment). *Expect:* the mock's `expect` passes; [OS] the claude process's environment holds
   `EDITOR=edit`.
2. **Claimed.** *Do:* set `EDITOR=notepad` in the cog's env (or the parent environment). *Expect:* `notepad`; Agentmaster
   never overrides it.
3. **No edit.exe** → no `EDITOR` set by Agentmaster.

---

### MSC-08 · The launch bar

> **Since** ≤ 0.7.7 (one-click Launch with the menu open: 0.8.0) · **P0** · **Needs** mock · **Spec** CLAUDE.md *C1 UI* (the Launch cwd box); SIDE_PANEL.md §2e

**Phases**
1. **A folder.** *Do:* type an existing folder. *Expect:* a neutral underline, **Launch Claude**; click → a session there.
2. **A session id.** *Do:* paste a session id that exists on disk. *Expect:* green, **Resume session** + **Fork**; Resume →
   that session's tab (or a jump to it when open).
3. **A new folder.** *Do:* `C:\e2e\new\x`. *Expect:* amber, **Create & Launch Claude**; the folder made, the session launched.
4. **Invalid.** *Do:* `relative\x` / a missing id. *Expect:* red, disabled.
5. **The hover menu.** *Do:* hover Launch (mouse). *Expect:* the models and presets menu at once; ONE click on Launch while it
   is open launches once (0.8.0); a double-click launches once.
6. **Codex.** *Do:* toggle to Codex. *Expect:* directory-only (no green id, no Fork); "Launch Codex".

---

### MSC-09 · A managed launch never stops at the trust dialog

> **Since** ≤ 0.7.7 · **P0** · **Needs** a fresh Claude Code config (`CLAUDE_CONFIG_DIR`) without trust entries (the E2E seeds never pre-trust; E2E_TESTING.md Appendix A) · **Spec** HOOKS.md *Workspace trust*

**Phases**
1. **A new folder.** *Do:* launch in `C:\e2e\w\untrusted`. *Expect:* the session reaches its input box without a trust dialog;
   its card and dot appear (it fires hooks). *Check:* [LOG] `[trust] …`; [OS] `~/.claude.json` `projects["C:/e2e/w/untrusted"].
   hasTrustDialogAccepted == true`, every other byte of the file unchanged (the float stats too).
2. **The home folder.** *Do:* launch with an empty launch box (`%USERPROFILE%`). *Expect:* no dialog, on every launch.
3. **Off.** *Do:* the cog's "Trust the working directory automatically" off; a new folder. *Expect:* the dialog appears in the
   tab (and the tab shows `○ claude · unlinked` until answered); `~/.claude.json` untouched.

**Edge cases**
- **E1 Claude holds its config lock** (`~/.claude.json.lock` present) → no write; the dialog may show once; the next launch
  writes.
- **E2 An unparseable `~/.claude.json`** → never rewritten.
