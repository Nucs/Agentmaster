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
// needs structure. The store stays SPARSE: only sessions we actually title/track get a file, so a
// full LoadAll* scan is cheap.
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

    // Every session that has a NON-empty value for `key`, gathered in ONE directory scan
    // (sid -> value). Sparse: only sessions with a stored file are visited.
    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreFieldIn(const std::wstring& storeDir, const std::wstring& key);

    // ===== live wrappers (resolve <AgentmasterStateDir>\session-store) =======================

    SessionStoreRecord LoadSessionStore(const std::wstring& sessionId);
    std::wstring GetSessionStoreField(const std::wstring& sessionId, const std::wstring& key);
    bool SetSessionStoreField(const std::wstring& sessionId, const std::wstring& key, const std::wstring& value);
    std::unordered_map<std::wstring, std::wstring> LoadAllSessionStoreField(const std::wstring& key);

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
    // Every tagged session (sid -> its decoded tag list), in ONE sparse directory scan.
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
    std::map<std::wstring, std::wstring> LoadAllTagColorsIn(const std::wstring& stateDir); // folded name -> "#AARRGGBB"
    std::wstring GetTagColorIn(const std::wstring& stateDir, const std::wstring& tag); // "" when unset (use the hash fallback)
    bool SetTagColorIn(const std::wstring& stateDir, const std::wstring& tag, const std::wstring& hexOrEmpty); // "" removes; invalid hex/tag -> false, no write

    // ---- live wrappers (resolve AgentmasterStateDir() — the file sits at the profile root) ----
    std::map<std::wstring, std::wstring> LoadAllTagColors();
    std::wstring GetTagColor(const std::wstring& tag);
    bool SetTagColor(const std::wstring& tag, const std::wstring& hex);
}
