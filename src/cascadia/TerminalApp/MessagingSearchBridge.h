// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingSearchBridge: the Telegram bot's "Search" topic agent (MESSAGING.md §5.4, D24).
// A SessionSearchAgent of its own — the Sessions page's search agent (Sonnet 5.5, its two tool servers, its
// system prompt) in a SEPARATE conversation named "telegram-search", so a search from the phone never
// changes the list on the PC's Sessions page — wrapped in the plain Msg::SearchBridge seam the service
// drives. App-only: SessionSearchAgent needs the SDK unity file, which the test harness never builds, so
// this TU is in TerminalAppLib.vcxproj only (plain C++, no WinRT, no PCH) and Engine.cpp never names it.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "AgentMaster/Messaging.h" // SearchBridge, SearchBridgeEvent

namespace Agentmaster::Msg
{
    /// <summary>
    /// Makes the Search topic's agent. Starts no process: the first Ask does (the agent's record and last
    /// shown list persist under <profile>\harness\telegram-search*.json like the page's).
    /// </summary>
    /// <param name="sink">Where the agent's events go (Searching / Shown / Answer / Failed), on the agent's
    /// threads; the service marshals nothing (it has no UI thread) and must not block in it.</param>
    /// <param name="thinkingWord">The thinking level the agent STARTS with ("off" … "max"; the user's rule
    /// is "high"); an unknown word reads as high.</param>
    /// <returns>The bridge; never null.</returns>
    /// <exception cref="std::bad_alloc">If the agent cannot be allocated.</exception>
    /// <exception cref="std::system_error">If the agent's worker thread cannot start.</exception>
    std::unique_ptr<SearchBridge> MakeTelegramSearchBridge(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord);
}
