# 17 · The Sessions browser (SES)

> Conventions and fixtures: [README.md](README.md). Spec: SESSIONS.md §1 (the page), §5 (identity and times), §6
> (mechanics), §10 (the folder filter); FAVORITES.md; CLAUDE.md *The Sessions browser*. Needs **F-CORP** (a seeded
> corpus of transcripts under the test profile's `CLAUDE_CONFIG_DIR\projects`: ~200 sessions over 90 days, several
> folders, forks, a session with Hebrew prompts, a 50 MB transcript, one with a 1.2 MB single line, sessions whose
> inferred folder differs from their launch folder). The AI search on this page is [13-ai-search.md](13-ai-search.md).

**What shipped.** The page predates 0.7.7 (it replaced the Archive page; FAVORITES.md); it is the only way back to a
closed session, so its base behaviors are a regression surface for every release. 0.7.9: fork / open-new submenus with
models and presets, the fork carries the unsent prompt, the tag filter chips. 0.7.10: **📂 Folders ▾** and "Works in →".
0.8.0: a content search no longer hides matches when ripgrep refuses a batch of files.

## Lifecycle model

```text
 Sessions (toolbar, rightmost) ─► the page over the window ─► GATHER (the window's transcripts + sidecar indexes, off-thread)
     ─► rows (★ · title · tags · folder chip · created · active) ─► type: FAST phase (titles, paths, history) ─► SLOW phase
        (ripgrep-prefiltered content, scope-attributed) ─► ranked: name > folder/path > content
     ─► a row: select (detail) · Resume here · Fork here ▸ · Open New Session Here ▸ · Edit Title · ★ · Tags · Hide · Filter ▸
     ─► ← Back / a tab switch / Home ─► closed
```

---

### SES-01 · Open the page and browse

> **Since** ≤ 0.7.7 · **P0** · **Needs** F-CORP · **Spec** SESSIONS.md §1, §6

**Phases**
1. **Open.** *Do:* Agent Manager → **Sessions** (the toolbar's rightmost button). *Expect:* a full-window page with the
   search box focused (typing filters at once); the rows of the last month, newest activity first; `[1 month]`.
   *Check:* [LOG] `[nav] sessions-page open-begin` ↔ `shown`.
2. **Rows.** *Expect per row:* ☆/★, the title (`customTitle > aiTitle > summary > first real prompt`), tag ribbons, the
   folder's color chip (solid = open in this app, dim = on disk), a presence ring for a running claude, created (a fork:
   its own birth) and last activity (line-derived, not the file time), msgs·tools.
3. **Window.** *Do:* click `[1 month]` repeatedly. *Expect:* 1 day → 3 days → 7 days → 14 days → 1 month → 3 months → 1
   day; the list follows. *Do:* hover it. *Expect:* the From / To popup; typed dates apply.
4. **Keyboard.** *Do:* Down from nothing selected. *Expect:* the first row; Up from nothing → the last; then ±1, wrapping;
   the detail pane follows.
5. **Leave.** *Do:* ← Back / click a tab / Home. *Expect:* the page closes; nothing else changed.

**Edge cases**
- **E1 ↻ after a new session started** → the new session appears.
- **E2 A 50 MB transcript** → its row appears; the gather stays responsive (the UI thread answers within 250 ms).
- **E3 A transcript with a single 1.2 MB line** → read whole; its row correct.
- **E4 A session whose transcript was deleted from disk** → not listed ("not found → don't show").
- **E5 The page open while a session in it finishes a turn** → its row's activity updates on ↻.

---

### SES-02 · Search: scopes, terms, ranking

> **Since** ≤ 0.7.7 · **P0** · **Needs** F-CORP · **Spec** SESSIONS.md §1 (search bar), §6

**Phases**
1. **Defaults.** *Expect:* 📁 📄 👤 on, 🤖 and (F) off.
2. **A name.** *Do:* type a word that is one session's title and appears in ten others' prompts. *Expect:* the titled
   session first (tier 0), path matches next (tier 1), content-only matches last (tier 2), the column sort applied
   within each tier; the content matches arrive after a moment (the slow phase). *Check:* [LOG] `[nav] search q=… scopes=…
   fast=N`, then `search-done content=M`.
3. **AND terms and quotes.** *Do:* `lexer token` → sessions holding both words (each may be in a different field);
   `"lexer token"` → only the exact phrase.
4. **A session id.** *Do:* paste a session's full id. *Expect:* that session and its forks.
5. **🤖.** *Do:* turn 🤖 on, search a word only Claude wrote. *Expect:* found; the detail pane's snippet tagged 🤖.
6. **Retype.** *Do:* type fast, changing the query 5 times in 1 s. *Expect:* only the last query's results (earlier phases
   cancelled).

**Edge cases**
- **E1 Hebrew query** → finds the Hebrew session.
- **E2 `src/foo` vs a `src\foo` folder** → matches (slash-insensitive).
- **E3 (F) fuzzy** → subsequence matches; never inside quotes; never for a GUID.
- **E4 All scopes off** → title + folder + the first prompt only.

---

### SES-03 · Content search never hides matches

> **Since** 0.8.0 · **P1** · **Needs** F-CORP + a batch ripgrep refuses (a file it cannot read: an exclusive lock held by the
> test, or a path too long for one command line batch) · **Spec** CLAUDE.md *The Sessions browser* (two-phase search)

**Phases**
1. **Refused batch.** *Do:* lock one transcript exclusively; search a word in several transcripts of that batch. *Expect:*
   the other files of the batch still match (searched in-process); the locked one is skipped, not every file beside it.
2. **No ripgrep at all.** *Do:* start the app with no `rg` on PATH. *Expect:* the slow phase still finds content (the
   in-process fallback), slower.

---

### SES-04 · The folder filter

> **Since** 0.7.10 · **P1** · **Needs** F-CORP (with inferred folders) · **Spec** SESSIONS.md §10

**Phases**
1. **Open.** *Do:* `📂 Folders ▾`. *Expect:* a find box; the list split in half: RECENT (each row's age) over MOST USED (each
   row's count); a hint line; Include subfolders; Clear. *Check:* [LOG] `[sessions-folders] inferred N session folder(s)
   in X ms (P .git probe(s), git arm on)`.
2. **Pick two.** *Do:* click two folders. *Expect:* the dropdown stays open; the list holds sessions of EITHER folder (OR);
   the toggle reads `2 folders` with the accent; the `✕ filter` chip reads `folder A or B`; the count line `· filtered`.
3. **Combine.** *Do:* type a search word. *Expect:* sessions of A or B that ALSO match (AND).
4. **Typed path.** *Do:* type `C:\e2e\w\deep` + Enter. *Expect:* that folder picked even if no list shows it.
5. **Subfolders.** *Do:* pick `C:\e2e\w`, turn Include subfolders on. *Expect:* `C:\e2e\w\t1` sessions included; never
   `C:\e2e\wx`.
6. **Works in.** *Expect:* a session whose tool calls worked elsewhere shows `Works in → <folder> (inferred from its tool
   calls)` in its tooltip and the detail pane.
7. **Close / clear.** *Do:* Esc (focus back to the search box); the chip's ✕. *Expect:* the dropdown closed; the folders
   dropped with every other facet; Include subfolders kept.

**Edge cases**
- **E1 A restart** → the picks are gone (not persisted).
- **E2 A session in the temp folder only** → its temp paths never vote; it counts under its launch folder.
- **E3 Up / Down while the dropdown is open** → move inside the dropdown, not the rows.

---

### SES-05 · Bring a session back: resume, fork, open new

> **Since** ≤ 0.7.7 (submenus with models and presets, fork carry 0.7.9) · **P0** · **Needs** mock, F-CORP, F-PRE · **Spec** SESSIONS.md §6.6; CLAUDE.md *The Sessions browser*

**Phases**
1. **Resume here.** *Do:* a closed session's row → Resume here (or double-click). *Expect:* a tab in this window resumes the
   SAME id (`claude --resume <id>`); the page closes; the tab is titled with the session's title and colored by its
   folder; its Prompt Queue comes back (Pending rows kept, Sent rows never replayed). *Check:* [LOG] `[nav] sessions
   resume-click row=<sid8>` ↔ `resume-done`; [OS] the claude command line holds `--resume <id>`; [MOCK] the next turn's
   history is the old conversation.
2. **An open session.** *Do:* Resume on an open session's row. *Expect:* jumps to its tab (`resume -> jump`), opens nothing.
3. **Fork here ▸ Default.** *Do:* on a LIVE session's row. *Expect:* a new tab `<title> (fork)` with a new id; the source
   keeps running; forking the fork → `(fork 2)`. *Check:* [LOG] `[nav] sessions fork-click …` → `[sessions-page->fork]` →
   `[fork] <new> (forked from <src>)` → `fork-done new=… from=…`.
4. **Fork here ▸ a model / a preset / Specify….** *Expect:* the fork runs on it ([MOCK] the model); Specify… opens the
   prompt pre-filled with the last committed model.
5. **Open New Session Here ▸.** *Expect:* a fresh session in the row's folder (the inferred one when it differs).
6. **The unsent prompt travels.** *Do:* type a draft in a live session's box (no Enter); Fork here from its row. *Expect:*
   the fork's box holds the same draft, never sent (DRF-07).

**Edge cases**
- **E1 A session whose transcript holds no conversation** (never prompted) → Resume starts a FRESH session in its folder;
   the stale record dropped ([LOG] `[restore-fresh]`).
- **E2 A preset session whose key is gone** → refused with "This preset can't start" (PRE-03); nothing opens.
- **E3 A Codex session** → resumes through `codex resume <uuid>` (transcript-gated).
- **E4 Resume a session whose turn died on an API error** → it continues (INT-03).
- **E5 Bulk: resume 5 rows quickly** → 5 tabs; background tabs start when first visited.

---

### SES-06 · Rename, favorite, hide, tags, Filter ▸

> **Since** ≤ 0.7.7 · **P1** · **Needs** F-CORP · **Spec** SESSIONS.md §1; FAVORITES.md §5

**Phases**
1. **Rename in place.** *Do:* re-click the selected row (slow double-click) → type → Enter. *Expect:* the title cell becomes a
   box; the new title shows at once, in the open tab too, and after a restart; Escape cancels; a blank title is refused.
2. **★.** *Do:* click ☆. *Expect:* ★ (yellow); `[ ] Favorite` filter shows only starred rows; an open session's tab wears the
   crown (or star, per the cog).
3. **Hide.** *Do:* right-click → Hide from list. *Expect:* gone; the count says `· 1 hidden`; `[ ] Hidden` shows it dimmed with
   Unhide; the cog's Reset hidden sessions brings every one back.
4. **Tags.** *Do:* right-click → Tags → add `release`. *Expect:* a ribbon in the Tags column; the tag chip row under the search
   toggles filters by it (AND with the search).
5. **Filter ▸.** *Do:* By Same Directory, then By Same Week. *Expect:* AND-stacked; `✕ filter: …` chip; ✓ on a re-open of the
   submenu; a re-click toggles one off; Clear filters drops all.

**Edge cases**
- **E1 Rename a never-managed on-disk session** → stored in the session store; shown after a restart.
- **E2 Rename while the session runs in another window** → that window's tab title follows.
- **E3 Filter facets after a restart** → gone; hidden sessions stay hidden.
