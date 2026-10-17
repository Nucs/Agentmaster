// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingSearchBridge (see the header): the process's ONE telegram-search conversation, and the bridges
// that attach the messaging service to it.

#include "MessagingSearchBridge.h"

#include <mutex>
#include <vector>

#include "AgentMaster/AgentHarnessCore.h" // ParseHarnessThinkingLevel
#include "AgentMaster/ClaudeSpawn.h" // AgentmasterStateDir, LogSwallowedException
#include "AgentMaster/Engine.h" // SharedEngine (the registry's open sessions, claudeExePath)
#include "AgentMaster/Persistence.h" // LoadAppSettings (the session database mode, the start thinking level)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionSearchAgent.h"
#include "AgentMaster/TranscriptStore.h" // PickDisplayTitle

namespace Agentmaster::Msg
{
    namespace
    {
        // The rows a Shown / Answer event carries, built from the agent's index entries (the same title
        // precedence the Sessions page shows: the live tab title, else custom > ai > summary > first prompt).
        //   ids     — the shown sessions, in the model's order.
        //   entries — their index entries as the agent sent them (an id without one gets a row with no facts).
        //   reasons — the model's why per session, index-aligned with `ids` (shorter = "" for the rest).
        //   returns — one row per id, in order.
        //   throws  — std::bad_alloc (ForwardAgentEvent catches and logs it).
        std::vector<SearchRow> RowsFrom(const std::vector<std::wstring>& ids, const std::vector<SessionIndexEntry>& entries, const std::vector<std::wstring>& reasons)
        {
            std::vector<SearchRow> rows;
            rows.reserve(ids.size());
            for (size_t i = 0; i < ids.size(); ++i)
            {
                const auto& id = ids[i];
                SearchRow r;
                r.sessionId = id;
                // §21.10: the model's evidence for this session, index-aligned with the ids ("" when it gave none).
                r.why = i < reasons.size() ? reasons[i] : std::wstring{};
                for (const auto& e : entries)
                {
                    if (e.sessionId == id)
                    {
                        // The title and the folder keep this bridge's own rules (no "(no prompt yet)" placeholder, no
                        // project-folder fallback): a pick's Resume launches with them, and a launch must get "" (it then
                        // derives a name / finds the folder in the transcript) rather than a placeholder or a lossy path.
                        r.title = !e.liveTitle.empty() ? e.liveTitle : PickDisplayTitle(e.stats.customTitle, e.stats.aiTitle, e.stats.summary, e.stats.firstUserPrompt);
                        r.dir = e.stats.cwd;
                        // The times and the count are the Sessions page's own (SessionSearchTools.h): what a card says
                        // matches the page's Active / Created / Msgs columns.
                        r.lastActivityMs = SessionSearchLastActivityMs(e);
                        r.createdMs = SessionSearchCreatedMs(e);
                        r.branch = e.stats.gitBranch;
                        r.prompts = e.stats.userPrompts;
                        break;
                    }
                }
                rows.push_back(std::move(r));
            }
            return rows;
        }

        /// <summary>The thinking level a word names; the Search topic's default (high, the user's rule D24) for an empty or unknown word.</summary>
        /// <param name="word">"off" … "max" (AppSettings::messagingSearchThinking).</param>
        /// <returns>The level.</returns>
        HarnessThinkingLevel ThinkingLevelFromWord(std::wstring_view word) noexcept
        {
            if (const auto level = ParseHarnessThinkingLevel(word))
            {
                return *level;
            }
            return HarnessThinkingLevel::High;
        }

        /// <summary>
        /// The process's ONE telegram-search conversation (§21.7), made on the first call and never destroyed — the
        /// Sessions page's SharedSessionSearchAgent() twin. Leaked on purpose for the same reason: the app ends with
        /// TerminateProcess, and its claude.exe child is ended by the harness's kill-on-close job; a static destructor
        /// would join threads at a moment the process may no longer run them. It persists its record and its last shown
        /// list (<profile>\harness\telegram-search*.json), so it carries on after a stop of the bot, a park and an app
        /// restart; Reset is the user's way to clear it.
        /// </summary>
        /// <returns>The conversation.</returns>
        /// <exception cref="std::bad_alloc">If it cannot be allocated (first call only; a later call tries again).</exception>
        /// <exception cref="std::system_error">If its worker thread cannot start (first call only; a later call tries again).</exception>
        SessionSearchAgent& TelegramSearchAgent()
        {
            static SessionSearchAgent* const agent = [] {
                SessionSearchAgentOptions options;
                options.name = L"telegram-search"; // its own conversation, never the page's (D24)
                options.persist = true;
                options.thinkingLevel = HarnessThinkingLevel::High; // the user's rule for the Search topic
                try
                {
                    // The start level the cog holds now (a bridge sets the level again, live); the session database's
                    // tools when it is On AND its service can start, read once like the page's agent (SESSIONS_DB.md
                    // §8.2: a change of the mode reaches the agent at the next start of the app; a service without
                    // duckdb.dll keeps the page search, SearchDatabaseProfileIfUsable).
                    const AppSettings settings = LoadAppSettings();
                    options.thinkingLevel = ThinkingLevelFromWord(settings.messagingSearchThinking);
                    options.sessionDbProfile = SearchDatabaseProfileIfUsable(settings.sessionDbMode == SessionDbMode::On, L"telegram-search");
                }
                catch (...)
                {
                    // Rule #18. Recovery: the conversation starts at high thinking on the page search.
                    LogSwallowedException(L"telegram-search: read the settings");
                }
                // SESSIONS.md §9.14: the window and tab tools over the app's windows — the same as the page's agent. From a
                // phone no request has an origin window, so with several windows open the model must ask which one.
                options.tabTools = MakeEngineTabTools();
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
                return new SessionSearchAgent{ std::move(options) };
            }();
            return *agent;
        }

        /// <summary>
        /// What one bridge shares with its listener: the service's sink and the rows of the request in flight. Owned by
        /// the bridge through a shared_ptr that the listener captures WEAKLY, because the conversation outlives every
        /// bridge and RemoveListener does not wait for a call that is already running: such a call holds the state alive
        /// through its own lock(), and any later call finds it gone.
        /// </summary>
        struct BridgeState
        {
            std::function<void(const SearchBridgeEvent&)> sink;
            std::mutex mtx; // guards lastRows
            std::vector<SearchRow> lastRows; // the latest Shown of the request in flight; handed to its Answer, then cleared
        };

        /// <summary>
        /// Maps one conversation event to the service's four: Searching, Shown (with its rows, remembered for the
        /// Answer), Answer (the model's plan above its reply, with the request's last shown rows, or none when it showed
        /// nothing) and Failed. State changes, candidates, compactions, resets and the narration events themselves show
        /// nothing in a topic (the narration reaches it through the Answer).
        /// </summary>
        /// <param name="st">The bridge's state.</param>
        /// <param name="e">The event, on the conversation's threads.</param>
        void ForwardAgentEvent(BridgeState& st, const SessionSearchEvent& e) noexcept
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
                    out.rows = RowsFrom(e.sessionIds, e.entries, e.reasons);
                    std::lock_guard lk{ st.mtx };
                    st.lastRows = out.rows;
                    break;
                }
                case SessionSearchEventKind::Answer:
                {
                    out.kind = SearchBridgeEvent::Kind::Answer;
                    // SESSIONS.md §9.14: the model's plan ("I'll 1. ... 2. ... 3. ...", written before its first tool call)
                    // above its reply. The service is quiet while a request runs (it posts only the answer), so this is
                    // the one place the phone sees what the model set out to do; the page shows the same plan live.
                    out.text = e.snapshot.narration.empty() ? e.text : (e.snapshot.narration + L"\n\n" + e.text);
                    std::lock_guard lk{ st.mtx };
                    out.rows = std::move(st.lastRows);
                    st.lastRows.clear();
                    break;
                }
                case SessionSearchEventKind::Failed:
                    out.kind = SearchBridgeEvent::Kind::Failed;
                    out.text = e.text;
                    break;
                default:
                    return; // state changes, candidates, compactions, resets, narration (it rides the Answer): nothing to show in a topic
                }
                if (st.sink)
                {
                    st.sink(out);
                }
            }
            catch (...)
            {
                LogSwallowedException(L"telegram-search: an agent event");
            }
        }

        // A bridge: this start of the bot's attachment to the process's conversation (see the header).
        class Bridge final : public SearchBridge
        {
        public:
            Bridge(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord) :
                _state(std::make_shared<BridgeState>())
            {
                _state->sink = std::move(sink);
                SessionSearchAgent& agent = TelegramSearchAgent();
                // The level the cog holds NOW: an earlier start of the bot may have made the conversation with another one.
                // Before the listener, so the StateChanged event this raises reaches nobody.
                agent.SetThinkingLevel(ThinkingLevelFromWord(thinkingWord));
                std::weak_ptr<BridgeState> weak = _state;
                _token = agent.AddListener([weak](const SessionSearchEvent& e) {
                    if (const auto st = weak.lock())
                    {
                        ForwardAgentEvent(*st, e);
                    }
                });
            }

            ~Bridge() override
            {
                try
                {
                    // Detach only: the conversation is the process's, and the next start of the bot attaches to it again.
                    TelegramSearchAgent().RemoveListener(_token);
                }
                catch (...)
                {
                    LogSwallowedException(L"telegram-search: detach a bridge");
                }
            }

            uint64_t Ask(std::wstring text) override
            {
                SessionSearchAgent& agent = TelegramSearchAgent();
                agent.Prewarm(true); // the index and the process, if a park ended them
                return agent.Ask(std::move(text));
            }

            void Reset() override
            {
                TelegramSearchAgent().Reset();
                std::lock_guard lk{ _state->mtx };
                _state->lastRows.clear();
            }

            void Park(int64_t delayMs) override
            {
                SessionSearchAgent& agent = TelegramSearchAgent();
                agent.ReleaseCatalog();
                agent.ParkProcessAfter(delayMs);
            }

            void SetThinkingLevel(std::wstring word) noexcept override
            {
                // TelegramSearchAgent() cannot throw here: this bridge's constructor made the conversation.
                TelegramSearchAgent().SetThinkingLevel(ThinkingLevelFromWord(word));
            }

        private:
            std::shared_ptr<BridgeState> _state;
            uint64_t _token{ 0 };
        };
    }

    std::unique_ptr<SearchBridge> MakeTelegramSearchBridge(std::function<void(const SearchBridgeEvent&)> sink, std::wstring thinkingWord)
    {
        return std::make_unique<Bridge>(std::move(sink), std::move(thinkingWord));
    }
}
