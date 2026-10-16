// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingSearchBridge: the Telegram bot's "Search" topic agent (MESSAGING.md §5.4, D24, §21.7).
// The process's ONE telegram-search conversation — the Sessions page's search agent (Sonnet 5.5, its two tool servers,
// its system prompt) in a conversation of its own, so a search from the phone never changes the list on the PC's
// Sessions page — and the bridges that attach the messaging service to it, behind the plain Msg::SearchBridge seam.
// Like the page's SharedSessionSearchAgent(), the conversation is made on first use and lives for the process: a stop
// and a start of the bot attach a new bridge to the same conversation, its record under <profile>\harness\ makes it
// carry on after an app restart, and the user's one action on it is to CLEAR it (§21.7). It is not a Claude Code
// session (no transcript: --no-session-persistence), so it is never a search result and nothing wakes, resumes or forks
// it. App-only: SessionSearchAgent needs the SDK unity file, which the test harness never builds, so this TU is in
// TerminalAppLib.vcxproj only (plain C++, no WinRT, no PCH) and Engine.cpp never names it.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "AgentMaster/Messaging.h" // SearchBridge, SearchBridgeEvent

namespace Agentmaster::Msg
{
    /// <summary>
    /// Makes a bridge to the Search topic's conversation, making the conversation itself on the first call of the process
    /// (it starts no process: the first Ask does). The bridge registers `sink` with the conversation and removes it when
    /// destroyed; the conversation outlives it.
    /// </summary>
    /// <param name="sink">Where the conversation's events go (Searching / Shown / Answer / Failed), on the conversation's
    /// threads; the service marshals nothing (it has no UI thread) and must not block in it.</param>
    /// <param name="thinkingWord">The thinking level ("off" … "max"; the user's rule is "high"; an unknown word reads as
    /// high), applied to the conversation from its next request.</param>
    /// <returns>The bridge; never null.</returns>
    /// <exception cref="std::bad_alloc">If the bridge or (on the first call) the conversation cannot be allocated.</exception>
    /// <exception cref="std::system_error">If the conversation's worker thread cannot start (first call only).</exception>
    std::unique_ptr<SearchBridge> MakeTelegramSearchBridge(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord);
}
