// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionStore: a generalized, DURABLE per-session key/value store under the ACTIVE
// PROFILE (<profile>/session-store/<sid>.json — one independent file per session). It is the
// cross-window, cross-run memory of facts we want to keep ABOUT a session that do NOT live in its
// transcript — first and foremost its TITLE, so a name assigned/known in ANY window persists across
// windows and OUTLIVES the live session: the Sessions browser reads it for closed/historical rows,
// and it is an O(1) sessionId -> data lookup (just open <sid>.json). Generalized on purpose — any
// future per-session datum (pinned, notes, tags, color override, ...) rides the same store by
// adding a KEY, with no schema/format change and no migration.
//
// How it differs from the two existing per-session-ish files:
//   - sessions.json — the HEAVY managed-fleet record (auto testing, autorunner, geometry refs),
//     loaded as one document. The store is a LIGHT KV any tool can read by id WITHOUT loading the
//     fleet, and it SURVIVES a record being deleted from the fleet (a historical title we keep).
//   - sessions-index/<sid>.json — the rebuildable SEARCH/STATS cache, (size,mtime)-invalidated and
//     derived from the transcript. The store is AUTHORITATIVE user/app data, never derived.
//
// One file per session => O(1) by id and cross-window-safe with NO whole-DB write contention
// (independent files; an atomic temp+rename per write, deduped so an unchanged re-publish is a
// single small read). Values are strings — JSON-encode richer data into a value when a future key
// needs structure. The store is sparse per KEY (a session without a tag has no `tags` key), but NOT in
// files: every launched session gets a `title` record, so a profile holds thousands of them (the
// release profile: 2,181 on 2026-10-10). A full LoadAll* scan is cheap all the same, because of the
// SCAN CACHE (below, "whole-directory reads"): one directory listing per call, and a file is opened
// only when it is new or changed since this process last read it.
//
// Pure C++/Win32 (the .cpp is <PrecompiledHeader>NotUsing and links into the standalone test
// harness). The *In overloads take an explicit store dir for the tests; the bare wrappers resolve
// <AgentmasterStateDir>\session-store.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Agentmaster
{
    // A session's whole stored record: key -> value. std::map => deterministic key order, so the
    // on-disk file is stable (clean diffs, no spurious rewrites from member reordering).
    using SessionStoreRecord = std::map<std::wstring, std::wstring>;

    // The TITLE field key (the first consumer). New per-session data adds its own key here.
    inline constexpr const wchar_t* kSessionStoreTitleKey = L"title";

    // The FAVORITE field key (FAVORITES.md). A durable per-session star: the user's "keep/find
    // this" marker that replaced archiving. Value is "1" when favorited; empty removes the key (and
    // the file, when it becomes empty), so the store stays sparse — only favorited (or titled)
    // sessions get a file. Keyed by the session id the Sessions page uses (the Claude conversation
    // uuid), so a favorite survives Close and applies to never-managed on-disk sessions alike.
    inline constexpr const wchar_t* kSessionStoreFavoriteKey = L"favorite";

    // The TAGS field key (bookmark tags). A durable, ordered list of user-named tags on a session —
    // the tab strip renders one small bookmark glyph per tag at the bottom of the session's tab
    // header, and the tab context menu's "Tags" panel adds/toggles them. The value is the tag list
    // JSON-encoded into ONE string (the store's documented "richer data" escape hatch: values are
    // strings) via EncodeTagList/DecodeTagList; an empty list removes the key, keeping the store
    // sparse. The GLOBAL tag universe is the union of every session's tags PLUS the durable
    // KNOWN-TAG registry (tags.json, below): untagging a tag's last carrier no longer vanishes it —
    // it stays listed at 0 carriers, re-appliable, until EXPLICITLY removed (the tag list row's ✕).
    // The AppSettings::maxTags cap gates only the creation of a NEW name.
    inline constexpr const wchar_t* kSessionStoreTagsKey = L"tags";

    // The COMMAND-PROGRESS field key (COMMANDS.md §3a): CommandWatch's durable per-session
    // progress marking — "v1;p=<firedWatermarkMs>;a=<cmd>@<ts>,…" (EncodeCommandProgress). The
    // fired watermark makes a transcript-history replay after a restart/resume unable to re-fire
    // an already-processed /handover(-here) (the double-processing guard), and the armed markers
    // let an await that was pending at crash/shutdown REVIVE on the next replay (bounded by its
    // original 15-min deadline). Written by the engine's CommandWatch through its injected store
    // seam; an empty value removes the key (the store stays sparse).
    inline constexpr const wchar_t* kSessionStoreCommandProgressKey = L"cmdProgress";

    // The DRAFT field keys (PENDING_INPUT.md §8c): the durable per-session cache of the session's
    // UNSENT input-box draft + the unix-ms stamp of when it was last observed. The draft already
    // persists on the fleet record (sessions.json), and THAT stays the authoritative copy — this is
    // the per-session mirror the user asked for: it is keyed by session id alone, so it survives the
    // fleet record being dropped (a resume-fresh stale-drop) and is an O(1) read for any surface /
    // tool that knows only the id. `draftAtUnixMs` is the same honesty device as
    // SessionInfo::pendingInputUnixMs — its AGE is what distinguishes a live draft from a carried
    // memory. Written by exactly ONE seam (TerminalPage::_ScanPendingInputImpl -> _PersistSessionDraft)
    // so the two copies cannot disagree, and the draft's paste placeholders are EXPANDED against the
    // paste-cache before storing when they resolve (so the stored value is the whole prompt, not a
    // "[Pasted text #N +M lines]" label); an unresolvable marker falls back to the rendered text
    // rather than losing the memory. An empty draft removes BOTH keys, keeping the store sparse.
    inline constexpr const wchar_t* kSessionStoreDraftKey = L"draft";
    inline constexpr const wchar_t* kSessionStoreDraftAtKey = L"draftAtUnixMs";

    // ===== testable core (explicit store dir) ================================================

    // The whole record for `sessionId` (empty map if it has no stored file / unreadable / bad id).
    SessionStoreRecord LoadSessionStoreIn(const std::wstring& storeDir, const std::wstring& sessionId);

    // One field; "" if absent.
    std::wstring GetSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& key);

    // Set ONE field via an atomic read-modify-write of <storeDir>\<sid>.json. An empty `value`
    // REMOVES the key (and DELETES the file when the record becomes empty). A no-op write — the
    // field already equals `value`, or an empty value with the key already absent — does NO disk
    // write (so a steady-state re-publish from the registry observer costs one small read). Creates
    // the store dir on demand. Returns true on success or a no-op; false on a bad id / write error.
    bool SetSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& key, const std::wstring& value);

    // Set SEVERAL fields in ONE read-modify-write (the single-field setter's semantics per entry: an
    // empty value removes that key; the file is deleted when the record becomes empty). Use this when
    // two keys are ONE fact — the draft and its observation stamp — so a crash between two separate
    // writes can never leave a draft carrying the wrong (or no) timestamp. Deduped like the single
    // setter: when every entry already holds its value there is no disk write.
    bool SetSessionStoreFieldsIn(const std::wstring& storeDir, const std::wstring& sessionId, const SessionStoreRecord& fields);

    // Every session that has a NON-empty value for `key`, gathered in ONE directory scan
    // (sid -> value). Sparse: only sessions with a stored file are visited.
    // Served through the scan cache (the next section): ONE directory listing, and a record file is
    // opened only when it is new or changed since this process last read it. The first call of a process
    // for a directory therefore reads every file once (the user reported more than 5 s for 2,181
    // records) — a UI thread must not make that call; see SessionStoreScanIsWarmIn.
    //   throws - std::bad_alloc.
    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& key);

    // ===== whole-directory reads: the SCAN CACHE (tags perf, 2026-10-10) =====================
    //
    // Every LoadAll* function (LoadAllSessionStoreFieldIn and the typed wrappers over it: titles,
    // favorites, tags) reads the whole store directory. Until 2026-10-10 each call opened, read and
    // JSON-parsed EVERY record file. The Tags panel made that call on the UI thread when it opened and
    // after every toggle, and a hover on a tag ribbon made it again: ~85 ms per call on the release
    // profile's 2,181 records, measured with a warm disk cache. The user reported more than 5 s for the
    // first open of the panel (not measured; a cold disk cache is the likely reason).
    // Now a call lists the directory once (~3.5 ms for those 2,181 records, no file opened) and takes
    // each record from the in-process record cache when the listing's (size, mtime) equals the cached
    // stamp. Only a new or changed record is opened. The per-session reads (GetSessionTagsIn, ...) use
    // the same cache, so the two can never disagree.
    // What is left is the FIRST read of a directory in a process, which has nothing cached. These
    // functions let a caller keep that read off its thread.

    // Has this process completed one whole-directory read of `storeDir` (a LoadAll* call or a
    // WarmSessionStoreIn)? When true, the next LoadAll* opens only records that changed since. When
    // false, it opens every record: a UI thread should then show a placeholder, run WarmSessionStoreIn on
    // a background thread, and read when that returns (the Tags panel and the tag hover panel do).
    // ⚠ "Warm" is a statement about cost, never about freshness: a warm read still lists the directory
    // and validates every record's stamp.
    //   storeDir - the store directory; an empty one is never warm.
    //   returns  - true once a whole-directory read completed. A directory that does not exist counts
    //              (there is nothing in it to read), from the first read that found it missing.
    //   throws   - std::bad_alloc.
    bool SessionStoreScanIsWarmIn(const std::wstring& storeDir);

    // Read every record of `storeDir` into the cache and mark the directory warm. Call it on a
    // BACKGROUND thread: the first call of a process opens every record file. A later call costs one
    // directory listing. Safe to call from several threads at once (two of them may then read the same
    // file twice; the cache ends up the same).
    //   storeDir - the store directory; an empty one does nothing.
    //   returns  - the number of record files the directory holds (0 for an empty or missing directory).
    //   throws   - std::bad_alloc. The directory is then not marked warm; the records read so far stay
    //              cached.
    size_t WarmSessionStoreIn(const std::wstring& storeDir);

    // How many record files this process has opened to parse since it started. A diagnostic counter:
    // the engine's warm-up logs it, and the harness reads it to prove the scan cache's promises (a
    // repeated whole-directory read opens no file; one changed record costs one read). Never use it to
    // decide behavior.
    //   returns - the count; it only grows.
    //   throws  - nothing.
    uint64_t SessionStoreRecordFileReads() noexcept;

    // How many times the record cache has STAT'ed a record file to validate a cached entry. The twin of
    // SessionStoreRecordFileReads for the cheaper cost: a per-session read (GetSessionTagsIn, ...) pays one
    // stat; a warm whole-directory read pays NONE, because it trusts the directory listing's stamp. The
    // harness uses it to pin that fast path — a whole-directory read that fell back to one stat per record
    // would still open no file, and so pass the other counter, while costing ~40 ms for 2,181 records.
    // Diagnostic only, like its twin.
    //   returns - the count; it only grows.
    //   throws  - nothing.
    uint64_t SessionStoreRecordFileStats() noexcept;

    // TEST SEAM — the harness only; the app never calls it. Sets a function the record cache calls on a
    // cache MISS, between parsing the record file and storing the result, on the reading thread with no
    // lock held (so the hook may call the store). It exists to stage, deterministically, the interleaving
    // the cache's write-order guard closes: a write of this process that lands while a reader still holds
    // the older content it parsed. Without the guard that reader would store the older content over the
    // write, and when the new file has the size and the timestamp of the old one, the stale record would
    // be served until the file changed again.
    // ⚠ Process-wide and not scoped: a test must reset it to nullptr before it returns, or every later
    // cache miss of the process calls the hook.
    //   hook   - the function to call, or nullptr to remove it.
    //   throws - nothing.
    void SetSessionStoreParseHookForTests(void (*hook)()) noexcept;

    // ===== live wrappers (resolve <AgentmasterStateDir>\session-store) =======================

    SessionStoreRecord LoadSessionStore(const std::wstring& sessionId);
    std::wstring GetSessionStoreField(const std::wstring& sessionId, const std::wstring& key);
    bool SetSessionStoreField(const std::wstring& sessionId, const std::wstring& key, const std::wstring& value);
    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreField(const std::wstring& key);
    // The live store's SessionStoreScanIsWarmIn / WarmSessionStoreIn (the same contracts). The engine
    // starts one WarmSessionStore on a background thread at start-up (Engine.cpp), so the first Tags
    // panel of a run normally finds the store warm.
    bool SessionStoreScanIsWarm();
    size_t WarmSessionStore();

    // ===== typed convenience: the TITLE =====================================================

    std::wstring GetStoredSessionTitle(const std::wstring& sessionId);
    bool SetStoredSessionTitle(const std::wstring& sessionId, const std::wstring& title);
    std::unordered_map<std::wstring, std::wstring> LoadAllStoredSessionTitles();

    // ===== typed convenience: the FAVORITE (FAVORITES.md) ===================================

    // Is this session favorited? (== a non-empty stored "favorite" value.)
    bool IsSessionFavorite(const std::wstring& sessionId);
    // Toggle the star: on => store "1", off => remove the key (the file is GC'd when it empties).
    bool SetSessionFavorite(const std::wstring& sessionId, bool favorite);
    // The set of every favorited session id, in ONE sparse directory scan (only favorited/titled
    // sessions have a file). The Sessions page reads this off-thread to drive the star + filter.
    std::unordered_set<std::wstring> LoadAllFavoriteSessions();

    // ===== typed convenience: the DRAFT (PENDING_INPUT.md §8c) ===============================
    //
    // The unsent input-box draft + when it was last observed. Set an EMPTY text to clear (both keys
    // are removed in one write, so a draft can never outlive its stamp or vice versa); `atUnixMs` is
    // ignored for a clear. Getting a draft that was never stored yields "" / 0.
    std::wstring GetStoredSessionDraftIn(const std::wstring& storeDir, const std::wstring& sessionId);
    int64_t GetStoredSessionDraftAtIn(const std::wstring& storeDir, const std::wstring& sessionId);
    bool SetStoredSessionDraftIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& text, int64_t atUnixMs);

    std::wstring GetStoredSessionDraft(const std::wstring& sessionId);
    int64_t GetStoredSessionDraftAt(const std::wstring& sessionId);
    bool SetStoredSessionDraft(const std::wstring& sessionId, const std::wstring& text, int64_t atUnixMs);

    // ===== typed convenience: the TAGS (bookmark tags on a session's tab) ===================
    //
    // Pure primitives first (all unit-tested standalone):

    // Canonicalize a user-typed tag name: control chars stripped, surrounding whitespace trimmed,
    // internal runs kept as typed, capped at kMaxTagNameLength chars. "" == not a usable tag.
    inline constexpr size_t kMaxTagNameLength = 48;
    std::wstring NormalizeTagName(const std::wstring& raw);

    // Case-fold a (normalized) tag name to its case-INSENSITIVE identity key: "Bug" and "bug" are
    // the same tag (first-seen/most-active display casing wins — see CollectGlobalTags).
    std::wstring FoldTagName(const std::wstring& name);

    // Tag list <-> the ONE store value: a JSON string array ("" for an empty list, so the key is
    // removed and the store stays sparse). Decode is tolerant — non-array / malformed input yields
    // an empty list; entries are re-normalized + case-insensitively deduped preserving order.
    std::wstring EncodeTagList(const std::vector<std::wstring>& tags);
    std::vector<std::wstring> DecodeTagList(const std::wstring& value);

    // The GLOBAL tag universe, derived from every session's tags: fold-merged case-insensitively,
    // each tag stamped with the MAX lastActivity across the sessions carrying it (0 for a session
    // absent from `activityBySession`) + how many sessions carry it. Sorted by lastActivity DESC
    // (the tab menu's ordering), folded-name ASC as the deterministic tiebreak. The display `name`
    // is the casing used by the highest-activity carrier (lexicographically smallest on a tie).
    // The 3-arg overload additionally folds in `knownTags` (the durable registry, below): a known
    // tag NO session carries still lists — sessionCount 0, activity 0 (so it sorts last) — with the
    // registry's casing; a known tag that IS carried takes its carriers' casing/count as usual
    // (never duplicated).
    struct GlobalTagInfo
    {
        std::wstring name;
        int64_t lastActivityUnixMs{ 0 };
        uint32_t sessionCount{ 0 };
    };
    std::vector<GlobalTagInfo> CollectGlobalTags(
        const std::unordered_map<std::wstring, std::vector<std::wstring>>& tagsBySession,
        const std::unordered_map<std::wstring, int64_t>& activityBySession);
    std::vector<GlobalTagInfo> CollectGlobalTags(
        const std::unordered_map<std::wstring, std::vector<std::wstring>>& tagsBySession,
        const std::unordered_map<std::wstring, int64_t>& activityBySession,
        const std::vector<std::wstring>& knownTags);

    // ===== the KNOWN-TAG registry (tags.json — a tag survives 0 carriers) ====================
    //
    // TAG REMOVAL IS A BIG DEAL, so it is never implicit: untagging a tag's last session must NOT
    // vanish the tag from the universe — it stays listed (0 carriers), re-appliable next time, until
    // the user EXPLICITLY deletes it via the ✕ on its tag-list row. The registry is what carries a
    // 0-carrier tag across that gap: a PROFILE-LEVEL JSON array of display-cased names
    // (<stateDir>\tags.json, beside tag-colors.json), CI-deduped. Tags are registered when created
    // (the editor's "+") and — self-healing for pre-registry tags — at the moment of any untag (the
    // only moment the derived union could lose one). The ✕ only UNREGISTERS: a tag that gained a
    // carrier meanwhile (another window) simply stays alive via the derived union — a carried tag
    // can never be deleted. Its color entry (tag-colors.json) is kept on purpose: a re-created tag
    // REGAINS its color (the documented tag-colors feature).
    // LoadKnownTagsIn is CACHED like LoadAllTagColorsIn (one stat per read, a re-parse only after the
    // file's stamp moved; Register / Unregister write through and read the file itself for their
    // read-modify-write).
    std::vector<std::wstring> LoadKnownTagsIn(const std::wstring& stateDir); // display-cased, CI-deduped, registration order
    bool RegisterKnownTagIn(const std::wstring& stateDir, const std::wstring& tag); // add (no-op success when already known); false on bad input / write error
    bool UnregisterKnownTagIn(const std::wstring& stateDir, const std::wstring& tag); // the explicit ✕ (CI; no-op success when absent)

    // ---- live wrappers (resolve AgentmasterStateDir()) ----
    std::vector<std::wstring> LoadKnownTags();
    bool RegisterKnownTag(const std::wstring& tag);
    bool UnregisterKnownTag(const std::wstring& tag);

    // ---- testable store cores (explicit store dir) ----
    std::vector<std::wstring> GetSessionTagsIn(const std::wstring& storeDir, const std::wstring& sessionId);
    bool SetSessionTagsIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::vector<std::wstring>& tags);
    // Add/remove ONE tag (case-insensitive identity; add appends at the end, keeping the session's
    // tag order = the order the user added them). Return true when the stored list CHANGED.
    bool AddSessionTagIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& tag);
    bool RemoveSessionTagIn(const std::wstring& storeDir, const std::wstring& sessionId, const std::wstring& tag);
    // Every tagged session (sid -> its decoded tag list), in ONE directory scan through the scan cache
    // (LoadAllSessionStoreFieldIn: a listing, and only new or changed records opened). The Tags panel
    // and the tag hover panel call it on the UI thread, so they first ask SessionStoreScanIsWarm.
    std::unordered_map<std::wstring, std::vector<std::wstring>> LoadAllSessionTagsIn(const std::wstring& storeDir);

    // ---- live wrappers (resolve <AgentmasterStateDir>\session-store) ----
    std::vector<std::wstring> GetSessionTags(const std::wstring& sessionId);
    bool SetSessionTags(const std::wstring& sessionId, const std::vector<std::wstring>& tags);
    bool AddSessionTag(const std::wstring& sessionId, const std::wstring& tag);
    bool RemoveSessionTag(const std::wstring& sessionId, const std::wstring& tag);
    std::unordered_map<std::wstring, std::vector<std::wstring>> LoadAllSessionTags();

    // ===== typed convenience: the TAG COLORS (the tag editor's color picker) =================
    //
    // A PROFILE-LEVEL map (<stateDir>\tag-colors.json — FOLDED tag name -> "#AARRGGBB"), NOT a
    // per-session store key: a tag's color is a property of the TAG's global (case-insensitive)
    // identity, worn identically by every bookmark badge / hover panel / tooltip chip that renders
    // it, like dir-colors.json is for folders. Written by the tag editor's color picker (a NEW tag
    // takes the picked color; an explicit swatch pick may recolor an existing tag on re-add). A tag
    // with NO entry falls back to the stable name-hash color (AgentStatusColors.h TagColorFor), so
    // pre-picker tags keep their historical colors with no migration. An empty value REMOVES the
    // entry (back to the hash); an entry whose tag later vanished from the universe is kept — it is
    // harmless (a tiny file) and a re-created tag REGAINING its old color is a feature. Values are
    // shape-validated ("#RRGGBB" / "#AARRGGBB") on write AND on load, so a hand-mangled file can
    // never feed a garbage color into the UI. Concurrency: the same atomic temp+rename write as the
    // per-session records (read-modify-write of one small file; last writer wins).
    // Reads are CACHED (tags perf, 2026-10-10): LoadAllTagColorsIn parses the file once and then answers
    // from memory for the cost of one stat, until the file's (size, mtime) moves; SetTagColorIn writes
    // through, and its own read-modify-write always reads the file itself. Every tag surface calls
    // LoadAllTagColorsIn (each tagged tab's badge re-assert, every board rebuild, every Sessions render),
    // so it must stay this cheap: never add a per-call file read back.
    std::map<std::wstring, std::wstring> LoadAllTagColorsIn(const std::wstring& stateDir); // folded name -> "#AARRGGBB"
    std::wstring GetTagColorIn(const std::wstring& stateDir, const std::wstring& tag); // "" when unset (use the hash fallback)
    bool SetTagColorIn(const std::wstring& stateDir, const std::wstring& tag, const std::wstring& hexOrEmpty); // "" removes; invalid hex/tag -> false, no write

    // ---- live wrappers (resolve AgentmasterStateDir() — the file sits at the profile root) ----
    std::map<std::wstring, std::wstring> LoadAllTagColors();
    std::wstring GetTagColor(const std::wstring& tag);
    bool SetTagColor(const std::wstring& tag, const std::wstring& hex);

    // ===== the TAG-CHANGE EPOCH (SIDE_PANEL.md §2c) ===========================================
    //
    // A process-wide counter that moves after every write that CHANGED a session's tag list (the
    // `tags` key, through SetSessionStoreFieldIn / SetSessionStoreFieldsIn — so every Add/Remove/Set
    // wrapper above) or a tag's color (SetTagColorIn). A reader that caches tags — the side panel,
    // whose card list keeps each card's tags between updates — compares the epoch with the one it
    // read at and re-reads only when it moved: an unchanged epoch PROVES no tag or tag color changed
    // in this process since, so the cache answers with no session-store stat per card (the stat per
    // card per update the side panel used to pay, ~466 per update on the release's biggest window).
    // Every window runs in this one process (the WindowEmperor), so another window's tag edit moves
    // the epoch too and every cached view sees it. NOT covered: a write by ANOTHER process to the same
    // profile — a configuration the profile mutex already warns about (PROFILES.md), and no other
    // process writes tags today (the agentmaster CLI only reads).
    // The bump comes AFTER the write and its record-cache update, and a reader must load the epoch
    // BEFORE it reads tags: a write landing between the two then leaves the reader holding the older
    // epoch, so its next comparison sees the move and re-reads — a cache can be refreshed once too
    // often, never kept stale.
    //   returns — the current epoch; 0 until the first tag or tag-color change of this process.
    //   throws  — nothing.
    uint64_t SessionTagsEpoch() noexcept;

    // Agentmaster (SESSIONS_DB.md §7.6): the FACTS EPOCH — moves after every write that CHANGES a session's
    // title, favorite or tag list (the facts the session database mirrors into its am_* tables). The database's
    // refresh tick compares it and sends `factsChanged` only when it moved, instead of re-reading the profile
    // every tick. Same reader contract and the same limit as SessionTagsEpoch (this process's writes only).
    //   returns — the current epoch; 0 until the first such change of this process.
    //   throws  — nothing.
    uint64_t SessionFactsEpoch() noexcept;
}
