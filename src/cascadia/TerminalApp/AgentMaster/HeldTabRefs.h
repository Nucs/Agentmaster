// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — HELD TAB REFS (doc/agentmaster/PERSISTENCE.md §16; the owner's question of 2026-10-28: "is there ANY other
// chance we will EVER override accidentally those files").
//
// A window's record (windows/<id>.json) is rebuilt from its LIVE tab strip at every save (TerminalPage::_CaptureWindowRecord).
// So a saved tab that a reopened window could not bring back used to vanish from the workspace at the window's next save:
// Claude Code not found at that start (every Claude tab of every window — the whole workspace), a preset whose key is
// missing from Credential Manager, a pane that failed to build, or a sessions.json that could not be read. A window that
// brought back no tab at all then counted as "Manager-only" and DELETED its record, and the Telegram bot, seeing those
// sessions closed, deleted their topics. None of those causes is the user closing anything.
//
// The restore now HOLDS such a ref: the window keeps it, every save puts it back into the record at about its old place,
// a window holding refs is never "Manager-only", and the engine tells the bot to keep the held sessions' topics. The ref is
// let go only when its tab exists again (this window or another one hosts the session) — or when the user closes the
// window, which demotes the whole record to recoverable as any close does. The next start tries the held refs again.
//
// This header is the decision half, plain C++ and free of WinRT so the m5 harness tests all of it (tests_restorewake.cpp):
//   * ShouldHoldRestoreRef — WHICH refs a restore keeps.
//   * MergeHeldTabRefs     — how a save puts them back.
//   * HeldSessionIds       — the session ids the engine publishes for the bot's hold (Engine::NoteWindowHeldSessions).
// The glue is TerminalPage's (TerminalPage.AgentWindowRecord.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "SessionModels.h" // TabEntry, TabKind

namespace Agentmaster
{
    /// <summary>
    /// What became of one saved tab ref at a window's restore (TerminalPage::_RestoreWindowTabs). Only the restore's own
    /// failures are held (<see cref="ShouldHoldRestoreRef"/>); the other outcomes are the ref's honest end.
    /// </summary>
    enum class RestoreRefOutcome
    {
        Restored, // the tab came back
        HostedHere, // this window already hosts the session: the ref is a duplicate of a tab that exists
        LiveElsewhere, // another window hosts the session: that window's record holds the tab
        UnknownSession, // sessions.json has no record of the session
        LaunchRefused, // the launch made no tab: Claude Code not found, a preset that cannot launch, a pane that failed to build
        ShellUnreadable, // a shell tab whose saved startup actions could not be parsed
        ShellEmpty, // a shell tab saved with no startup actions: there is nothing to rebuild
    };

    /// <summary>
    /// A saved tab ref that the restore could not bring back and keeps in the window's record until its tab exists again.
    /// </summary>
    struct HeldTabRef
    {
        /// <summary>The ref's index in the record it was restored from: where <see cref="MergeHeldTabRefs"/> puts it back.</summary>
        size_t recordIndex{ 0 };
        /// <summary>The ref exactly as it was saved (kind, session id, shell actions).</summary>
        TabEntry entry;
        /// <summary>Why it did not come back — for the log line, and what a later reader of the record should expect.</summary>
        RestoreRefOutcome why{ RestoreRefOutcome::LaunchRefused };
    };

    /// <summary>
    /// Decides whether a restore keeps a saved tab ref it could not bring back. Keeping is the safe side: a held ref costs
    /// one line in the record and a retry at the next start, while a dropped one is a tab gone from the workspace for good.
    /// </summary>
    /// <param name="outcome">What became of the ref.</param>
    /// <param name="fleetLoadUsable">
    /// Whether sessions.json could be used at this start (LoadSessionsForStartup's <c>usable</c>). An unknown session is held
    /// only when it could not: then "unknown" means "the fleet did not load", not "the session is gone".
    /// </param>
    /// <returns>
    /// True to hold the ref: a launch that made no tab, a shell tab whose actions could not be read, an unknown session after
    /// a failed fleet load. False otherwise: the tab came back, it is a duplicate, another window hosts it, its session is
    /// truly unknown, or a shell ref that holds nothing.
    /// </returns>
    /// <remarks>Never throws.</remarks>
    inline constexpr bool ShouldHoldRestoreRef(RestoreRefOutcome outcome, bool fleetLoadUsable) noexcept
    {
        switch (outcome)
        {
        case RestoreRefOutcome::LaunchRefused:
        case RestoreRefOutcome::ShellUnreadable:
            return true;
        case RestoreRefOutcome::UnknownSession:
            return !fleetLoadUsable;
        case RestoreRefOutcome::Restored:
        case RestoreRefOutcome::HostedHere:
        case RestoreRefOutcome::LiveElsewhere:
        case RestoreRefOutcome::ShellEmpty:
        default:
            return false;
        }
    }

    /// <summary>
    /// Puts a window's held refs back into a capture of its record, so a save never writes the workspace without them.
    /// First drops every held ref whose tab exists again (<paramref name="resolved"/>), then inserts each remaining one at
    /// its old index, clamped to the capture's end, in ascending index order — the order the restore recorded them in.
    /// </summary>
    /// <param name="captured">The record's tab refs as captured from the live strip; receives the held refs.</param>
    /// <param name="held">
    /// The window's held refs, sorted by <see cref="HeldTabRef::recordIndex"/> (the restore sorts them). Refs that
    /// <paramref name="resolved"/> reports are REMOVED from it, so they never come back into a later save.
    /// </param>
    /// <param name="resolved">
    /// Reports a held ref whose tab exists again — its session is hosted by this window or by another one. Empty = none is.
    /// It must not throw, and must not change <paramref name="held"/>.
    /// </param>
    /// <param name="selectedTabIndex">
    /// The capture's focused SHELL tab, as an index into <paramref name="captured"/> (WindowRecord::selectedTabIndex; -1 =
    /// none). Every ref inserted at or before it moves it one place on, so it still names the same tab. -1 stays -1.
    /// </param>
    /// <returns>
    /// True when <paramref name="held"/> lost a ref, so the caller publishes the window's held session set again
    /// (Engine::NoteWindowHeldSessions); false when it is unchanged.
    /// </returns>
    /// <exception cref="std::bad_alloc">The vectors could not grow. <paramref name="captured"/> may then hold some of the
    /// refs (and <paramref name="selectedTabIndex"/> matches them); <paramref name="held"/> holds every ref
    /// <paramref name="resolved"/> did not report.</exception>
    /// <remarks>
    /// Positions are approximate by design: the restore already brings the Claude tabs back before the shells, and the user
    /// may have opened or closed tabs since. A held ref lands where it was among the tabs that are there now, which is what
    /// the next start's restore needs (the order is a preference; the refs themselves are the data).
    /// </remarks>
    inline bool MergeHeldTabRefs(std::vector<TabEntry>& captured, std::vector<HeldTabRef>& held, const std::function<bool(const TabEntry&)>& resolved, int& selectedTabIndex)
    {
        const size_t before = held.size();
        if (resolved)
        {
            held.erase(std::remove_if(held.begin(), held.end(), [&resolved](const HeldTabRef& h) { return resolved(h.entry); }), held.end());
        }
        for (const auto& h : held)
        {
            // Clamped: the strip may hold fewer tabs than the record did (tabs closed since the restore).
            const size_t at = (std::min)(h.recordIndex, captured.size());
            captured.insert(captured.begin() + static_cast<std::ptrdiff_t>(at), h.entry);
            // The focused shell is kept by INDEX (a shell has no id that survives a restart): a ref inserted at its place or
            // before it pushes it one on, and the index follows, or the reopen would focus the held ref's slot instead.
            if (selectedTabIndex >= 0 && at <= static_cast<size_t>(selectedTabIndex))
            {
                ++selectedTabIndex;
            }
        }
        return held.size() != before;
    }

    /// <summary>
    /// The session ids of a window's held MANAGED refs (Claude and Codex), in hold order — what the engine publishes so the
    /// Telegram bot keeps those sessions' topics (Engine::IsSessionHeldByAWindow). Shell refs carry no session and are left out.
    /// </summary>
    /// <param name="held">The window's held refs.</param>
    /// <returns>The non-empty session ids, without duplicates.</returns>
    /// <exception cref="std::bad_alloc">The result could not be built.</exception>
    inline std::vector<std::wstring> HeldSessionIds(const std::vector<HeldTabRef>& held)
    {
        std::vector<std::wstring> ids;
        for (const auto& h : held)
        {
            if ((h.entry.kind == TabKind::Claude || h.entry.kind == TabKind::Codex) && !h.entry.sessionId.empty() &&
                std::find(ids.begin(), ids.end(), h.entry.sessionId) == ids.end())
            {
                ids.push_back(h.entry.sessionId);
            }
        }
        return ids;
    }

    /// <summary>A short word for a hold reason in the log ("launch refused", "unknown session", "shell unreadable").</summary>
    /// <param name="why">The reason.</param>
    /// <returns>A static, never-empty word.</returns>
    /// <remarks>Never throws.</remarks>
    inline constexpr const wchar_t* RestoreRefOutcomeWord(RestoreRefOutcome why) noexcept
    {
        switch (why)
        {
        case RestoreRefOutcome::Restored:
            return L"restored";
        case RestoreRefOutcome::HostedHere:
            return L"hosted here";
        case RestoreRefOutcome::LiveElsewhere:
            return L"live elsewhere";
        case RestoreRefOutcome::UnknownSession:
            return L"unknown session";
        case RestoreRefOutcome::LaunchRefused:
            return L"launch refused";
        case RestoreRefOutcome::ShellUnreadable:
            return L"shell unreadable";
        case RestoreRefOutcome::ShellEmpty:
        default:
            return L"shell empty";
        }
    }
}
