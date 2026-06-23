# Agentmaster — Refactor: kill "Archive", introduce "Favorite", unify on the Sessions page

> **Status: IMPLEMENTED (engine + UI; lib-compiled green + engine-tested 1101/1101). The base
> refactor is built + dev-deployed + verified engine-live; the "★ Favorite & Close" close-dialog
> follow-on (§4/§5) is lib-compiled green + pending the next deploy.** The user-facing "Archive"
> concept is removed — a managed tab's lifecycle verbs
> are now **Close** and **Favorite**, and the **Sessions page is the sole browser** for history +
> every other session on the machine. Favorited sessions are the "keep/find this" signal (replacing
> the implicit "I archived it"), persisted via the existing per-session `SessionStore` (the title
> store, built for exactly this reuse — a new `favorite` key). What shipped: the `SessionStore`
> favorite key + helpers (`IsSessionFavorite`/`SetSessionFavorite`/`LoadAllFavoriteSessions`); the
> Sessions page's leftmost **★ column** + **[ ] Favorite** filter + row "Favorite/Unfavorite" menu;
> the session **tab right-click** Favorite/Unfavorite (beside Close); **Close = always archive** (the
> 3-way Delete/Archive/Cancel confirm became **Close · ★ Favorite & Close · Cancel**, batch → **Close
> All · ★ Favorite & Close All · Cancel All**, all Delete UI removed); and the full **Archive page +
> "Archived" button + retired overlay + per-window reopen** removed (the toolbar "Reopen Windows (N)"
> stays).
> Companions: [`SESSIONS.md`](./SESSIONS.md) (the Sessions browser + `~/.claude` map) ·
> [`PERSISTENCE.md`](./PERSISTENCE.md) (window records / reopen) · [`STATE.md`](./STATE.md) ·
> [`DESIGN.md`](./DESIGN.md).

---

## 0. Why this is safe (the load-bearing fact)

`_ResumeSessionFromDisk` (the Sessions page's "Resume here") already routes a **known** closed
record through `_RestoreArchivedSession` → `_LaunchClaudeSession(..., *info)`, which **rehydrates
its Flight Plan queue + autopilot**. So the Sessions page's Resume *already subsumes* the Archive
page's "Restore here" — removing the Archive page loses **no** resume/restore capability. The
Archive page is genuinely redundant once Sessions is the sole browser.

---

## 1. The new model (what changes conceptually)

| Concept | Today | After |
|---|---|---|
| A managed tab's lifecycle verbs | 3-way close confirm: **Delete / Archive / Cancel** | **Close & Favorite.** **Close** replaces Archive — it *always archives* (keep the record, `live=false`, keep Flight Plan + autopilot); it never deletes. **Favorite** marks the session as a keeper. |
| "Archived" sessions | A separate view behind the **Archived (N)** toolbar button (`TerminalPage.AgentArchivePage.cpp`) | **No separate view.** They appear in the **Sessions page** like any on-disk session (they have transcripts → already enumerated). |
| "I care about this" marker | implicit (you archived it) | explicit **Favorite ★** (persisted, filterable) |
| Delete-permanently | board / tree / tab / Archive-page actions | **Removed** (we always archive; nothing is destroyed) |
| Reopen window | toolbar "Reopen Windows (N)" **+** Archive-page per-window reopen | **toolbar "Reopen Windows (N)" only** (unchanged) |

Net: **the Sessions page becomes the sole history/browser**, the only lifecycle verbs on a session
are **Close** and **Favorite**, and Favorite (a persisted star) replaces archiving as the
"keep/find this" signal.

---

## 2. Favorite persistence — reuse `SessionStore` (the title store)

Add one key to the generalized per-session KV store (`<profile>/session-store/<sid>.json`), exactly
as it was "designed for reuse" (its header: *"any future per-session datum (pinned, notes, tags,
…) rides the same store by adding a KEY, with no schema/format change and no migration"*):

```cpp
// AgentMaster/SessionStore.h
inline constexpr const wchar_t* kSessionStoreFavoriteKey = L"favorite"; // value "1"; empty removes (file GC'd when empty)
bool  IsSessionFavorite(const std::wstring& sid);                 // == !GetSessionStoreField(sid,"favorite").empty()
bool  SetSessionFavorite(const std::wstring& sid, bool on);       // SetSessionStoreField(sid,"favorite", on? L"1" : L"")
std::unordered_set<std::wstring> LoadAllFavoriteSessions();       // keys of LoadAllSessionStoreField("favorite")
```

Why `SessionStore` and **not** `SessionInfo.favorite`/`sessions.json` nor
`AppSettings.hiddenSessionIds`:

- **Survives Close and works for never-managed on-disk sessions** (no `SessionInfo` needed) — the
  same property the durable *title* already has.
- **One sparse scan** (`LoadAllSessionStoreField`) — only favorited sessions have a file; the
  Sessions page already runs the identical scan for titles (`LoadAllStoredSessionTitles`).
- **No schema migration** anywhere. `sessions.json`, `settings.json`, `windows/<id>.json` are
  untouched.

The favorite is keyed by the session id the Sessions page uses (the Claude conversation uuid ==
`SessionInfo.id` for a managed Claude tab). Codex (the durable handle ≠ rollout uuid, and the
Sessions page is Claude-only) is out of scope here — a future extension.

---

## 3. Sessions page (`TerminalPage.AgentSessionsPage.cpp`)

**Star column (leftmost).** Prepend a new col 0 to `SessAddColumns` (~22px, non-sortable):

- `☆` hollow (off) / `★` filled **yellow** (on). **Clicking the star toggles the favorite** →
  defer one tick (the page's pointer-handler discipline) → `_ToggleSessionFavorite(id)` → re-render.
- The existing columns shift right by one: chip → col 1, Title → 2, Directory → 3, Branch → 4,
  Created → 5, Active → 6, Msgs·Tools → 7, Hits → 8. The sort `switch` cases, the `addHeader`
  calls, and the `_sessionsSortColumn` default all shift `+1` (mechanical).

**Filter checkbox `[ ] Favorite`.** A third `CheckBox` in the search bar next to `[Open] [Hidden]`
(`_sessFavOnlyBtn`), default OFF, flips through the existing `_sessionsSearchThrottled`. At the
`_RenderSessionsTable` chokepoint, when checked, keep only rows in `_sessionsFavorites` — composes
**AND** with the search text, the `[Open]`/`[Hidden]` toggles, and the row-filter facets (the same
chokepoint they all flow through). The count line notes `· favorites`.

**Favorites obey the time window** like any other row — there is **no** special all-time bypass for
favorited ids (a favorite outside the selected window shows when the window is widened). *(This is
deliberate; do not re-introduce a window bypass.)*

**Load the set off-thread.** In `_RefreshSessionsRows`' background pass, alongside
`LoadAllStoredSessionTitles()`, call `LoadAllFavoriteSessions()` into `_sessionsFavorites` (a
`std::unordered_set<std::wstring>` member), so the star render + the filter are in-memory and free
at the debounce.

**Row context menu.** Add a `Favorite` / `Unfavorite` `MenuFlyoutItem` (mirrors the Hide/Unhide
toggle), deferred like the rest.

---

## 4. The "Close" verb (`TerminalPage.AgentSessions.cpp`, `TabManagement.cpp`)

- `_ArchiveAndCloseClaudeTab` → **always archive**: drop the Delete/Archive/Cancel dialog; keep the
  archive bookkeeping (flip `live=false`, unbind injector, `SaveSessions`, close the tab).
  `confirmBeforeKill` (relabeled **"Confirm before closing"**) gates a **three-way** confirm —
  **Close · ★ Favorite & Close · Cancel** (mapped to `Primary` / `Secondary` / `None`). **Favorite &
  Close** stars the session (`SetSessionFavorite(id, true)`) *before* the archive bookkeeping, so it
  surfaces in the Sessions page's ★ column / `[ ] Favorite` filter afterward; plain **Close** just
  archives. The default button is **Cancel** (safe). Non-destructive either way, so the confirm can
  be turned off — but with it off there is no dialog, hence no Favorite-&-Close button (star from the
  Sessions page or the tab menu instead — the accepted trade-off of putting it on the dialog).
- Batch close (`TabManagement.cpp`): "🗑 Delete All / Archive All / Cancel All" → **"Close All · ★
  Favorite & Close All · Cancel All"**. `Secondary` sets a `favoriteAll` flag; the apply loop then
  `SetSessionFavorite(id, true)` on every managed session before `_ArchiveAndCloseClaudeTab(…,
  skipConfirm=true)`. A pure shell-tab batch is unaffected (keeps upstream's single confirm).
- **Remove all "Delete permanently" UI**: the board-card + tree-row `_MakeSessionMenu`, the tab
  menu, (and the gone Archive page). Keep `_RemoveSessionRecord` *internal-only* for the
  resume-fresh stale-record drop, but **remove its `_AddSessionIdToHiddenList` auto-hide coupling**
  (no Delete → no auto-hide; a closed session must stay visible in Sessions).
- The tab/board liveness sweep + the window-teardown archive already keep the record — unchanged
  ("always archiving").

---

## 5. The Favorite verbs (where you toggle the star)

Entry points (per the chosen surfaces):

1. **Clicking the star** in the Sessions page's leftmost column (§3).
2. **The session tab's right-click context menu** — add **Favorite / Unfavorite** next to **Close**,
   shown when the tab is a managed Claude session (`_ClaudeSessionForTab(tab)` non-empty), wired to
   `_ToggleSessionFavorite(sid)`. *Simple interaction with the session (its tab) makes it eligible
   to favorite.*
3. **The close confirm's "★ Favorite & Close" button** (§4) — keep + close in one gesture, at the
   moment you're closing (single + batch). Closing never auto-favorites; this is the explicit
   "keep this one" choice surfaced right where you make the close decision.

Plus the Sessions row right-click menu's `Favorite`/`Unfavorite` item (§3).

The board card / tree row menu lose **Archive + Delete** → gain just **Close**; they do **not** get
Favorite (per the chosen surfaces). The per-tab overlay HUD is unchanged (no star).

---

## 6. Remove Archive (page + button + retired overlay)

- **`AgentManagerContent.cpp`**: delete `_archivedBtn`, `_UpdateArchivedButton`,
  `SetOpenArchiveHandler`/`_openArchiveHandler`, the retired `_BuildArchiveOverlay` /
  `_BuildArchivedList` block (~5282–5640), and `SetReopenWindowHandler(int)` (per-window reopen —
  only the Archive page used it). Toolbar becomes **Launch · Fork · Reopen Windows(N) · ⚙ · Pause
  Autopilot · Sessions**.
- **Delete `TerminalPage.AgentArchivePage.cpp`** + its `TerminalAppLib.vcxproj` registration + all
  `_archive*` members/declarations in `TerminalPage.h` (`_BuildArchivePageShell`,
  `_ShowArchivePage`, `_GatherArchiveRows`, `_RenderArchiveTable`, `_ArchiveRow`, the
  registry-observer token, throttles, etc.).
- **`TerminalPage.AgentEngine.cpp`** (`_WireAgentManagerContent`): drop the `SetOpenArchiveHandler`
  wiring.
- `AppSettings.archiveSplitFraction` becomes unused — keep the field for back-compat (harmless),
  drop on a later cleanup.

---

## 7. What does NOT change

`sessions.json` schema · `windows/<id>.json` + the reopen mechanism (`RecoverableWindows`,
`wt -w -1 -s <idx>`) · the two-phase search · **Hide-from-list** (stays as a manual browse
preference; only the *auto-hide-on-delete* goes, since there is no Delete) · the Triage Board /
Explorer tree (a closed session simply leaves the board → appears in Sessions).

---

## 8. Files touched (summary)

| File | Change |
|---|---|
| `AgentMaster/SessionStore.{h,cpp}` | add `favorite` key + 3 helpers |
| `TerminalPage.AgentSessionsPage.cpp` | star column, `[ ] Favorite` checkbox, `_sessionsFavorites`, row menu, `_ToggleSessionFavorite`; drop a row whose transcript is missing |
| `TerminalPage.AgentSessions.cpp` | `_ArchiveAndCloseClaudeTab` → always-archive; decouple auto-hide; drop the Delete UI paths |
| `TabManagement.cpp` | batch confirm → Close All / Cancel; tab context menu Favorite + Close (Claude tabs) |
| `Tab.{h,cpp}` | hook to surface Favorite / Close in the tab context menu (if built there) |
| `AgentManagerContent.cpp` | remove the Archived button + retired overlay + per-window reopen; board/tree menu Archive/Delete → Close |
| `TerminalPage.AgentArchivePage.cpp` | **delete** |
| `TerminalPage.h`, `TerminalAppLib.vcxproj` | drop archive declarations + the TU registration; add the favorite members |
| `AgentMaster/tests/m5_tests.cpp` | add SessionStore favorite tests; fix the Archive/Delete-string asserts |
| `doc/agentmaster/SESSIONS.md`, `CLAUDE.md` | document Favorite; retire Archive |

---

## 9. Missing transcripts ("not found → don't show")

Claude Code does **not** sweep on a 30-day cadence in practice — `cleanupPeriodDays` is effectively
"never" on this machine, so a session's transcript persists for a very, very long time. But a
transcript *can* eventually disappear (an actual sweep after a very long time, a manual delete, a
project purge).

**Rule: if a session's transcript is not found on disk, it is simply not shown.** The Sessions page
already enumerates rows from on-disk transcripts (`EnumerateTranscripts`), so a session whose
`.jsonl` is absent is naturally **not listed** — no special-casing, and no broken "Resume" (you
can't click what isn't shown). This applies uniformly to favorited and to closed ("archived")
records alike: the favorite/record may still sit in its store, but with no transcript there is
nothing to list or resume, so it disappears from the browser. No GC pass is required for
correctness; missing-transcript rows just don't render.

---

## 10. Optional / follow-ups (not required)

- **`sessions.json` growth.** "Always archiving, never delete" means `sessions.json` only grows
  (each record is tiny — id/title/dir/branch/queue/autopilot). A natural optional bound: on load,
  prune `!live && !favorite` records whose transcript is absent (§9). Not needed for correctness —
  missing rows already don't render — purely housekeeping.
- **"Reset favorites"** button in the Settings cog (symmetry with "Reset hidden sessions"): clear
  every `favorite` key. Optional.
- Favorite on the **board card / tree row** menu (trivial via the shared `_MakeSessionMenu`) — left
  out per the chosen surfaces; easy to add later.
- **Codex favorites** — the Sessions page is Claude-only today; a future extension.

---

## 11. Test impact

- `AgentMaster/tests/m5_tests.cpp`: add `SessionStore` favorite round-trips (`SetSessionFavorite`
  on/off, `IsSessionFavorite`, `LoadAllFavoriteSessions` over a temp store dir — the `*In`
  overloads), and update any asserts that match the Archive/Delete confirm strings.
- The engine test harness stays standalone-buildable (SessionStore is plain C++/Win32, `.cpp`
  `<PrecompiledHeader>NotUsing`).
