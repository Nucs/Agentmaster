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
//   - sessions.json — the HEAVY managed-fleet record (flight plan, autopilot, geometry refs),
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

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
}
