// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingSearchBridge (see the header): a SessionSearchAgent behind the service's seam.

#include "MessagingSearchBridge.h"

#include <mutex>
#include <vector>

#include "AgentMaster/AgentHarnessCore.h" // ParseHarnessThinkingLevel
#include "AgentMaster/ClaudeSpawn.h" // AgentmasterStateDir, LogSwallowedException
#include "AgentMaster/Engine.h" // SharedEngine (the registry's open sessions, claudeExePath)
#include "AgentMaster/Persistence.h" // LoadAppSettings (the session database mode)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionSearchAgent.h"
#include "AgentMaster/TranscriptStore.h" // PickDisplayTitle

namespace Agentmaster::Msg
{
    namespace
    {
        // The rows a Shown / Answer event carries, built from the agent's index entries (the same title
        // precedence the Sessions page shows: the live tab title, else custom > ai > summary > first prompt).
        std::vector<SearchRow> RowsFrom(const std::vector<std::wstring>& ids, const std::vector<SessionIndexEntry>& entries)
        {
            std::vector<SearchRow> rows;
            rows.reserve(ids.size());
            for (const auto& id : ids)
            {
                SearchRow r;
                r.sessionId = id;
                for (const auto& e : entries)
                {
                    if (e.sessionId == id)
                    {
                        r.title = !e.liveTitle.empty() ? e.liveTitle : PickDisplayTitle(e.stats.customTitle, e.stats.aiTitle, e.stats.summary, e.stats.firstUserPrompt);
                        r.dir = e.stats.cwd;
                        r.lastActivityMs = e.stats.lastTimestampMs;
                        break;
                    }
                }
                rows.push_back(std::move(r));
            }
            return rows;
        }

        class Bridge final : public SearchBridge
        {
        public:
            Bridge(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord) :
                _sink(std::move(sink))
            {
                SessionSearchAgentOptions options;
                options.name = L"telegram-search"; // its own conversation: <profile>\harness\telegram-search.json (D24)
                options.persist = true;
                options.thinkingLevel = HarnessThinkingLevel::High; // the user's rule for the Search topic
                if (const auto level = ParseHarnessThinkingLevel(thinkingWord))
                {
                    options.thinkingLevel = *level;
                }
                try
                {
                    if (LoadAppSettings().sessionDbMode == SessionDbMode::On)
                    {
                        options.sessionDbProfile = AgentmasterStateDir(); // SESSIONS_DB.md §8.2: the database's tools when it is On
                    }
                }
                catch (...)
                {
                    LogSwallowedException(L"telegram-search: read the settings");
                }
                options.claudeExe = [] { return SharedEngine().claudeExePath; };
                options.openSessions = [] {
                    std::vector<std::pair<std::wstring, std::wstring>> open;
                    const auto registry = SharedEngine().registry;
                    if (!registry)
                    {
                        return open;
                    }
                    std::vector<std::wstring> ids;
                    for (const auto& binding : registry->LiveTabBindings())
                    {
                        ids.push_back(binding.id);
                    }
                    for (auto& headline : registry->HeadlinesOf(ids))
                    {
                        open.emplace_back(std::move(headline.id), std::move(headline.title));
                    }
                    return open;
                };
                _agent = std::make_unique<SessionSearchAgent>(std::move(options));
                _token = _agent->AddListener([this](const SessionSearchEvent& e) { _onEvent(e); });
            }

            ~Bridge() override
            {
                try
                {
                    if (_agent)
                    {
                        _agent->RemoveListener(_token);
                        _agent->Shutdown();
                    }
                }
                catch (...)
                {
                    LogSwallowedException(L"telegram-search: shutdown");
                }
            }

            uint64_t Ask(std::wstring text) override
            {
                _agent->Prewarm(true);
                return _agent->Ask(std::move(text));
            }

            void Reset() override
            {
                _agent->Reset();
                std::lock_guard lk{ _mtx };
                _lastRows.clear();
            }

            void Park(int64_t delayMs) override
            {
                _agent->ReleaseCatalog();
                _agent->ParkProcessAfter(delayMs);
            }

        private:
            void _onEvent(const SessionSearchEvent& e) noexcept
            {
                try
                {
                    SearchBridgeEvent out;
                    out.turn = e.turn;
                    switch (e.kind)
                    {
                    case SessionSearchEventKind::Searching:
                        out.kind = SearchBridgeEvent::Kind::Searching;
                        out.text = e.text;
                        break;
                    case SessionSearchEventKind::Shown:
                    {
                        out.kind = SearchBridgeEvent::Kind::Shown;
                        out.text = e.text;
                        out.rows = RowsFrom(e.sessionIds, e.entries);
                        std::lock_guard lk{ _mtx };
                        _lastRows = out.rows;
                        break;
                    }
                    case SessionSearchEventKind::Answer:
                    {
                        out.kind = SearchBridgeEvent::Kind::Answer;
                        out.text = e.text;
                        std::lock_guard lk{ _mtx };
                        out.rows = _lastRows;
                        _lastRows.clear();
                        break;
                    }
                    case SessionSearchEventKind::Failed:
                        out.kind = SearchBridgeEvent::Kind::Failed;
                        out.text = e.text;
                        break;
                    default:
                        return; // state changes, compactions, resets: nothing to show in a topic
                    }
                    if (_sink)
                    {
                        _sink(out);
                    }
                }
                catch (...)
                {
                    LogSwallowedException(L"telegram-search: an agent event");
                }
            }

            std::function<void(const SearchBridgeEvent&)> _sink;
            std::unique_ptr<SessionSearchAgent> _agent;
            uint64_t _token{ 0 };
            std::mutex _mtx;
            std::vector<SearchRow> _lastRows;
        };
    }

    std::unique_ptr<SearchBridge> MakeTelegramSearchBridge(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord)
    {
        return std::make_unique<Bridge>(std::move(sink), std::move(thinkingWord));
    }
}
