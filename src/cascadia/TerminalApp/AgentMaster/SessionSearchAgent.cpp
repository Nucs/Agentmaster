// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
// The Sessions browser's AI search; see SessionSearchAgent.h and SESSIONS.md §9.
#include "SessionSearchAgent.h"

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "Engine.h" // SharedEngine — the shared agent's registry
#include "SessionRegistry.h" // LiveTabBindings / HeadlinesOf — the open sessions
#include "SessionStore.h" // WarmSessionStore — the store's first read, kept off the search's path

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>

namespace
{
    using namespace Agentmaster;

    int64_t NowUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // The period a catalog is gathered for before any search asked for one: the search tool's
    // own default, so the first search of a request finds its catalog ready.
    constexpr int64_t kDefaultCatalogMs = static_cast<int64_t>(kSessionSearchDefaultDays) * 86400000LL;

    // One step of the index keeper reads sidecar files for this long, then publishes what it
    // has. Short enough that a search which waits for "a bit more index" gets it soon; long
    // enough that a step's fixed cost (the directory listing, the copies from the step before:
    // about 0.1 s for 1,500 sessions) stays a small part of it.
    constexpr int64_t kCatalogStepBudgetMs = 2500;

    // The FIRST step of a run, when there is no index at all: much shorter, so that the first
    // search has the newest sessions whatever the rest costs. Measured on a machine with 1,506
    // sessions in 90 days: a sidecar that Windows has cached reads in 0.3 ms, the first touch
    // of one takes 10 to 20 ms (an on-access scan), so a cold index needs 15 to 30 s. In 400 ms
    // the first step has the newest 20 to 40 sessions cold, and all of them warm.
    constexpr int64_t kCatalogFirstStepBudgetMs = 400;

    // How long a search waits for an index that covers its period and is fresh, when an older
    // or shorter one is there. After that it searches what there is, and the result says that
    // older sessions are still being indexed. The model, and behind it the user, waits here:
    // only the first search of a request waits (CatalogFor).
    constexpr int64_t kCatalogSearchWaitMs = 2000;

    // How long a search waits for the FIRST index of a run (there is nothing to search before
    // it). The first step, plus room for one very large transcript that a step cannot split
    // (a transcript with no sidecar is parsed whole: seconds for hundreds of megabytes).
    constexpr int64_t kCatalogFirstWaitMs = 15000;

    // The shown list as JSON (the "view" file, version 1): what a page needs to show the
    // conversation's last result again after a restart.
    std::wstring SerializeView(const SessionSearchSnapshot& s)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"version", Value::MkNum(1));
        root.Set(L"question", Value::MkStr(s.question));
        root.Set(L"note", Value::MkStr(s.note));
        root.Set(L"answer", Value::MkStr(s.answer));
        root.Set(L"answerIsError", Value::MkBool(s.answerIsError));
        root.Set(L"hasShown", Value::MkBool(s.hasShown));
        Value ids = Value::MkArr();
        for (const auto& id : s.shownIds)
        {
            ids.Push(Value::MkStr(id));
        }
        root.Set(L"shownIds", std::move(ids));
        return json::Dump(root);
    }

    // Read a view file's text into `s`. Anything that is not a version-1 view leaves `s` as it is.
    void DeserializeViewInto(std::wstring_view text, SessionSearchSnapshot& s)
    {
        const auto parsed = json::Parse(text);
        if (!parsed || parsed->type != json::Value::Type::Obj || parsed->I64At(L"version") != 1)
        {
            return;
        }
        s.question = parsed->StrAt(L"question");
        s.note = parsed->StrAt(L"note");
        s.answer = parsed->StrAt(L"answer");
        s.answerIsError = parsed->BoolAt(L"answerIsError");
        s.hasShown = parsed->BoolAt(L"hasShown");
        s.shownIds.clear();
        if (const auto* ids = parsed->Find(L"shownIds"); ids && ids->type == json::Value::Type::Arr)
        {
            for (const auto& id : ids->arr)
            {
                if (id.type == json::Value::Type::Str && !id.str.empty())
                {
                    s.shownIds.push_back(id.str);
                }
            }
        }
    }

    // Write `text` to `path` as UTF-8, replacing the file through a temp file and a rename (a
    // crash leaves the old file or the new one).
    //   returns — true when the file now holds the text.
    bool WriteViewFile(const std::wstring& path, const std::wstring& text) noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path{ path }.parent_path(), ec);
            const std::string bytes = HarnessToUtf8(text);
            const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
            const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD wrote = 0;
            const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
            ::CloseHandle(h);
            if (!ok || wrote != bytes.size() || !::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent: write the view file");
            return false;
        }
    }

    std::wstring ReadViewFile(const std::wstring& path) noexcept
    {
        try
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (!f)
            {
                return {};
            }
            const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return HarnessFromUtf8(bytes);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent: read the view file");
            return {};
        }
    }

    void LogAgent(const std::wstring& message) noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", L"[ai-search] " + message + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS the
            // logging path; reporting its own failure would call AppendStateLog again.
        }
    }
}

namespace Agentmaster
{
    struct SessionSearchAgent::Impl
    {
        SessionSearchAgentOptions options;
        std::wstring viewPath; // empty when nothing is persisted

        // `mutex` guards everything from here to `keeper`.
        mutable std::mutex mutex;
        SessionSearchSnapshot snapshot;
        uint64_t latestTurn{ 0 }; // the newest request's turn id: only its end changes the answer
        struct ListenerEntry
        {
            uint64_t token{};
            Listener fn;
        };
        std::vector<ListenerEntry> listeners;
        uint64_t nextListenerToken{ 1 };
        std::wstring claudeExePath;
        bool shuttingDown{ false };
        // ---- the index keeper (see KeeperRun) ----
        std::shared_ptr<const SessionCatalog> catalog; // the newest session index, or null
        // The earliest instant a search of this run asked for; 0 = nobody asked since the index
        // was released. It only moves back: the keeper reads the widest period ever asked, so
        // a search for a shorter one never throws a long index away.
        int64_t wantSince{ 0 };
        bool keeperRunning{ false }; // a keeper task is gathering (at most one at a time)
        // The request (a requestGeneration value) whose search already waited the whole
        // kCatalogSearchWaitMs for a longer index and did not get it. Its further searches do
        // not wait again: three searches of one request must not wait three times.
        uint64_t waitedGeneration{ ~uint64_t{ 0 } };
        std::future<void> keeper; // that task, kept so that Shutdown can wait for it

        // Raised when a new index was published or the keeper ended. Waited on with `mutex`.
        std::condition_variable catalogChanged;
        // Bumped to stop the keeper's running step (ReleaseCatalog, Shutdown). Atomic: the
        // gather reads it between two sidecar files, without the mutex.
        std::atomic<uint64_t> keeperEpoch{ 0 };
        // Bumped whenever the running request is not wanted any more (a newer request, an
        // interrupt, a reset, the shutdown). A search reads it when it starts and stops when it
        // changed (SessionSearchToolset::Callbacks::generation).
        std::atomic<uint64_t> requestGeneration{ 0 };

        std::unique_ptr<SessionSearchToolset> toolset;
        // LAST: its threads call the toolset and this object; Shutdown stops it first.
        std::unique_ptr<HarnessConversation> conversation;

        // -------------------------------------------------------------------------------

        SessionCatalogSources Sources() const
        {
            return SessionCatalogSources{ options.openSessions };
        }

        // Tell every listener. Fills e.snapshot. Never throws: a listener that throws is logged.
        void Notify(SessionSearchEvent e) noexcept
        {
            try
            {
                std::vector<Listener> targets;
                {
                    std::lock_guard lock{ mutex };
                    e.snapshot = snapshot;
                    targets.reserve(listeners.size());
                    for (const auto& l : listeners)
                    {
                        targets.push_back(l.fn);
                    }
                }
                // Outside the lock: a listener may call back into the agent.
                for (const auto& fn : targets)
                {
                    try
                    {
                        fn(e);
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"SessionSearchAgent listener");
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::Notify");
            }
        }

        // Write (or, for an empty conversation, delete) the view file.
        void SaveView() noexcept
        {
            if (viewPath.empty())
            {
                return;
            }
            try
            {
                SessionSearchSnapshot copy;
                {
                    std::lock_guard lock{ mutex };
                    copy = snapshot;
                }
                if (copy.question.empty() && !copy.hasShown)
                {
                    ::DeleteFileW(viewPath.c_str());
                    return;
                }
                if (!WriteViewFile(viewPath, SerializeView(copy)))
                {
                    LogAgent(L"the view file was NOT saved: " + viewPath);
                }
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::SaveView");
            }
        }

        // The index keeper: the ONE place that reads the session index for the AI search. It
        // runs as a background task while there is something to read, and reads in STEPS: each
        // step takes everything the step before had (no file read) and reads new sidecar files
        // for kCatalogStepBudgetMs, newest sessions first, then publishes the result. So:
        //   - a search never reads the disk itself; it takes the newest published index;
        //   - an index that needs minutes the first time (every transcript whose sidecar is
        //     missing or out of date is parsed once) is searchable after the first step, the
        //     newest sessions first, and reaches further back with every step;
        //   - a refresh of a complete index is one short step (the listing, plus the sessions
        //     that changed).
        // It ends when the index is complete for the widest period asked, when the index was
        // released and nobody asked since, when a gather failed, or at the shutdown.
        // Never throws: a task that threw would end silently with keeperRunning left true, and
        // no index would ever be read again.
        void KeeperRun() noexcept
        {
            try
            {
                const auto started = std::chrono::steady_clock::now();
                int steps = 0;
                bool announced = false; // the "being built" line was written for this run
                bool storeWarmTried = false; // the session store's first read was tried in this run (once is enough: a failure must not loop)
                for (;;)
                {
                    std::shared_ptr<const SessionCatalog> previous;
                    int64_t since = 0;
                    uint64_t epoch = 0;
                    {
                        std::lock_guard lock{ mutex };
                        if (shuttingDown || wantSince == 0)
                        {
                            keeperRunning = false;
                            break;
                        }
                        previous = catalog;
                        since = wantSince;
                        // Read under the lock, like ReleaseCatalog bumps it: the step belongs
                        // to exactly the state it copied.
                        epoch = keeperEpoch.load();
                    }
                    std::shared_ptr<const SessionCatalog> step;
                    try
                    {
                        SessionCatalogGatherOptions gather;
                        // No index yet: publish the newest sessions fast, then take normal steps.
                        gather.budgetMs = previous ? kCatalogStepBudgetMs : kCatalogFirstStepBudgetMs;
                        gather.previous = std::move(previous);
                        gather.cancelled = [this, epoch] { return keeperEpoch.load() != epoch; };
                        step = GatherSessionCatalog(since, Sources(), gather);
                        ++steps;
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"SessionSearchAgent: the session index gather");
                    }
                    if (step && keeperEpoch.load() == epoch) // not a step that a release cut short
                    {
                        // The log lines come BEFORE the last locked section: after it this task
                        // must return at once (EnsureCatalog may replace its future, which
                        // waits for the task, while it holds the mutex).
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                        if (!step->Complete() && !announced)
                        {
                            announced = true;
                            LogAgent(L"the session index is being built in the background: " + std::to_wstring(step->entries.size()) + L" session(s) so far, the last " +
                                     std::to_wstring((step->gatheredUnixMs - step->coveredSinceUnixMs) / 86400000LL) + L" of " +
                                     std::to_wstring((step->gatheredUnixMs - step->sinceUnixMs) / 86400000LL) + L" day(s) are searchable");
                        }
                        else if (step->Complete() && (announced || ms > 1500))
                        {
                            // Worth a line: time a search may have waited, or the end of a build.
                            LogAgent(L"the session index is complete: " + std::to_wstring(step->entries.size()) + L" session(s), " + std::to_wstring(steps) + L" step(s), " + std::to_wstring(ms) + L" ms");
                            announced = false;
                        }
                    }
                    // The step left the stored titles, tags and favorites out (the session store
                    // was not read yet in this process): it is published as it is, and the store
                    // is read AFTER the publish, here on this background thread, so the step
                    // after it has them. In the app the engine reads the store at start-up, so
                    // this runs only for a request made in the first seconds of a run.
                    const bool needStore = step && !step->storeOverlay && !storeWarmTried;
                    bool more = false;
                    {
                        std::lock_guard lock{ mutex };
                        // Released (or shut down) while the step ran: its result must not bring
                        // back the memory that ReleaseCatalog freed.
                        const bool released = keeperEpoch.load() != epoch;
                        if (step && !released)
                        {
                            catalog = step;
                        }
                        if (shuttingDown)
                        {
                            more = false;
                        }
                        else if (released)
                        {
                            more = wantSince != 0; // somebody asked again after the release: read it again
                        }
                        else if (!step)
                        {
                            more = false; // the gather failed: the next request starts a new keeper
                        }
                        else
                        {
                            // Not everything is read yet, the store is still to be read, or a
                            // search asked for a longer period while this step ran.
                            more = !step->Complete() || needStore || wantSince < step->sinceUnixMs;
                        }
                        if (!more)
                        {
                            keeperRunning = false;
                        }
                    }
                    catalogChanged.notify_all();
                    if (!more)
                    {
                        break;
                    }
                    if (needStore)
                    {
                        storeWarmTried = true;
                        try
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            const size_t records = WarmSessionStore();
                            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                            if (ms > 1500)
                            {
                                LogAgent(L"the session store was read for the index: " + std::to_wstring(records) + L" record(s), " + std::to_wstring(ms) + L" ms");
                            }
                        }
                        catch (...)
                        {
                            // The next steps then stay without the stored titles, tags and
                            // favorites; the next run of the keeper tries again.
                            LogSwallowedException(L"SessionSearchAgent: read the session store");
                        }
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::KeeperRun");
                try
                {
                    std::lock_guard lock{ mutex };
                    keeperRunning = false;
                }
                catch (...)
                {
                    LogSwallowedException(L"SessionSearchAgent::KeeperRun: clear the running flag");
                }
                catalogChanged.notify_all();
            }
        }

        // Make sure that a fresh index from `sinceUnixMs` on exists or is on its way: start the
        // keeper when none runs and the current index is too old or too short. Returns at once
        // (it is called from a UI thread, through Prewarm and Ask). Never throws.
        void EnsureCatalog(int64_t sinceUnixMs) noexcept
        {
            try
            {
                std::lock_guard lock{ mutex };
                if (shuttingDown)
                {
                    return;
                }
                if (wantSince == 0 || sinceUnixMs < wantSince)
                {
                    wantSince = sinceUnixMs;
                }
                if (keeperRunning)
                {
                    return; // it reads wantSince before every step
                }
                // Half the maximum age: an index read now must still be good when the model's
                // first request has come back (about a second later).
                if (catalog && catalog->coveredSinceUnixMs <= wantSince && NowUnixMs() - catalog->gatheredUnixMs <= kSessionCatalogMaxAgeMs / 2)
                {
                    return;
                }
                // Replacing `keeper` waits for the task before this one. That task has cleared
                // keeperRunning as its last act under this mutex, so it is returning: the wait
                // is a few instructions long, not a gather.
                keeperRunning = true;
                try
                {
                    keeper = std::async(std::launch::async, [this] { KeeperRun(); });
                }
                catch (...)
                {
                    keeperRunning = false;
                    throw;
                }
            }
            catch (...)
            {
                // No thread for the task. A search then uses the index there is, or reports
                // that there is none; the next request tries again.
                LogSwallowedException(L"SessionSearchAgent::EnsureCatalog");
            }
        }

        // The index for a search from `sinceUnixMs` on. Called on the tool-call thread, with
        // the model waiting. It reads no file: it asks the keeper for the index and waits a
        // bounded time for one that covers the period and is fresh.
        //   cancelled — empty = never; else true ends the wait (the search is not wanted).
        //   returns   — the newest index: complete for the period when the keeper got there in
        //               time, else a shorter or older one (the search result says what it
        //               lacks); null when there is none at all.
        std::shared_ptr<const SessionCatalog> CatalogFor(int64_t sinceUnixMs, const std::function<bool()>& cancelled) noexcept
        {
            try
            {
                EnsureCatalog(sinceUnixMs);
                const auto started = std::chrono::steady_clock::now();
                const uint64_t generation = requestGeneration.load();
                std::unique_lock lock{ mutex };
                for (;;)
                {
                    const bool good = catalog && catalog->coveredSinceUnixMs <= sinceUnixMs && NowUnixMs() - catalog->gatheredUnixMs <= kSessionCatalogMaxAgeMs;
                    if (good || shuttingDown || !keeperRunning)
                    {
                        break; // it is there, or nothing better is coming
                    }
                    if (cancelled && cancelled())
                    {
                        break;
                    }
                    if (catalog && waitedGeneration == generation)
                    {
                        break; // this request waited once already: search what there is
                    }
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                    if (waited >= (catalog ? kCatalogSearchWaitMs : kCatalogFirstWaitMs))
                    {
                        waitedGeneration = generation;
                        break;
                    }
                    // Short slices: `cancelled` has no notification of its own.
                    catalogChanged.wait_for(lock, std::chrono::milliseconds{ 100 });
                }
                return catalog;
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::CatalogFor");
                return nullptr;
            }
        }

        // The index entries of `ids`, in order, without their touched-path lists.
        std::vector<SessionIndexEntry> EntriesFor(const std::vector<std::wstring>& ids) const
        {
            std::shared_ptr<const SessionCatalog> current;
            {
                std::lock_guard lock{ mutex };
                current = catalog;
            }
            std::vector<SessionIndexEntry> out;
            if (!current || ids.empty())
            {
                return out;
            }
            std::unordered_map<std::wstring, size_t> byId;
            byId.reserve(current->entries.size());
            for (size_t i = 0; i < current->entries.size(); ++i)
            {
                byId.emplace(current->entries[i].sessionId, i);
            }
            out.reserve(ids.size());
            for (const auto& id : ids)
            {
                if (const auto it = byId.find(id); it != byId.end())
                {
                    out.push_back(current->entries[it->second]);
                    // A row needs the title, folder, times and counts — not up to 512 paths.
                    out.back().stats.pathsAccessed.clear();
                    out.back().stats.pathsAccessed.shrink_to_fit();
                }
            }
            return out;
        }

        // ---- the toolset's callbacks (the SDK's reader thread) ----

        void OnSearching(const std::wstring& summary)
        {
            uint64_t turn = 0;
            {
                std::lock_guard lock{ mutex };
                snapshot.activity = L"Searching: " + summary;
                turn = latestTurn;
            }
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::Searching;
            e.turn = turn;
            e.text = summary;
            Notify(std::move(e));
        }

        void OnCandidates(const std::vector<std::wstring>& ids)
        {
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::Candidates;
            {
                std::lock_guard lock{ mutex };
                e.turn = latestTurn;
            }
            e.sessionIds = ids;
            e.entries = EntriesFor(ids);
            Notify(std::move(e));
        }

        void OnShow(const std::vector<std::wstring>& ids, const std::wstring& note)
        {
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::Shown;
            e.text = note;
            e.sessionIds = ids;
            e.entries = EntriesFor(ids);
            {
                std::lock_guard lock{ mutex };
                snapshot.hasShown = true;
                snapshot.shownIds = ids;
                snapshot.note = note;
                snapshot.activity.clear();
                e.turn = latestTurn;
            }
            LogAgent(L"shown " + std::to_wstring(ids.size()) + L" session(s)" + (note.empty() ? std::wstring{} : (L": " + note)));
            SaveView();
            Notify(std::move(e));
        }

        // ---- the conversation's events (its worker thread and the SDK's reader thread) ----

        void OnHarnessEvent(const HarnessEvent& h)
        {
            SessionSearchEvent e;
            e.turn = h.turn;
            bool notify = false;
            bool save = false;
            {
                std::lock_guard lock{ mutex };
                if (h.epoch < snapshot.epoch)
                {
                    return; // an event of a conversation that was reset away
                }
                snapshot.state = h.state;
                snapshot.usage = h.usage;
                switch (h.kind)
                {
                case HarnessEventKind::State:
                    e.kind = SessionSearchEventKind::StateChanged;
                    notify = true;
                    break;
                case HarnessEventKind::Ready:
                    snapshot.model = h.model;
                    e.kind = SessionSearchEventKind::StateChanged;
                    notify = true;
                    break;
                case HarnessEventKind::Compacting:
                    snapshot.activity = L"Compacting the conversation\x2026";
                    e.kind = SessionSearchEventKind::Compacting;
                    notify = true;
                    break;
                case HarnessEventKind::Compacted:
                    ++snapshot.compactions;
                    snapshot.activity = L"Thinking\x2026";
                    e.kind = SessionSearchEventKind::StateChanged;
                    notify = true;
                    break;
                case HarnessEventKind::TurnCompleted:
                    if (h.turn < latestTurn)
                    {
                        break; // an older request (interrupted or replaced): the newest one owns the answer
                    }
                    latestTurn = h.turn;
                    snapshot.busy = false;
                    snapshot.activity.clear();
                    snapshot.answerIsError = h.isError;
                    snapshot.answer = !h.text.empty() ? h.text : (h.interrupted ? std::wstring{ L"Stopped." } : std::wstring{});
                    e.kind = h.isError ? SessionSearchEventKind::Failed : SessionSearchEventKind::Answer;
                    e.text = snapshot.answer;
                    notify = true;
                    save = true;
                    break;
                case HarnessEventKind::Error:
                    // A start failure. A request that waited for the process gets its own
                    // TurnCompleted right after; this covers a failed Prewarm.
                    snapshot.activity.clear();
                    snapshot.answerIsError = true;
                    snapshot.answer = h.text;
                    e.kind = SessionSearchEventKind::Failed;
                    e.text = h.text;
                    notify = true;
                    break;
                case HarnessEventKind::Notice:
                    if (h.isError)
                    {
                        snapshot.answerIsError = true;
                        snapshot.answer = h.text;
                        e.kind = SessionSearchEventKind::Failed;
                        e.text = h.text;
                        notify = true;
                    }
                    break;
                case HarnessEventKind::UserMessage: // Ask already reported it
                case HarnessEventKind::AssistantDelta:
                case HarnessEventKind::AssistantText: // the turn's end carries the final text
                case HarnessEventKind::ToolCall: // the toolset's callbacks report searches and shows
                case HarnessEventKind::ToolResult:
                default:
                    break;
                }
            }
            if (save)
            {
                SaveView();
            }
            if (notify)
            {
                Notify(std::move(e));
            }
        }
    };

    SessionSearchAgent::SessionSearchAgent(SessionSearchAgentOptions options) :
        _impl{ std::make_unique<Impl>() }
    {
        Impl* const impl = _impl.get();
        impl->options = std::move(options);
        if (!IsValidHarnessName(impl->options.name))
        {
            throw std::invalid_argument("SessionSearchAgentOptions::name must be 1 to 48 characters of a-z, 0-9 and '-'.");
        }

        SessionSearchToolset::Callbacks callbacks;
        callbacks.catalog = [impl](int64_t since, const std::function<bool()>& cancelled) { return impl->CatalogFor(since, cancelled); };
        callbacks.generation = [impl] { return impl->requestGeneration.load(); };
        callbacks.onSearching = [impl](const std::wstring& summary) { impl->OnSearching(summary); };
        callbacks.onCandidates = [impl](const std::vector<std::wstring>& ids) { impl->OnCandidates(ids); };
        callbacks.onShow = [impl](const std::vector<std::wstring>& ids, const std::wstring& note) { impl->OnShow(ids, note); };
        impl->toolset = std::make_unique<SessionSearchToolset>(std::move(callbacks));

        HarnessOptions h;
        h.name = impl->options.name;
        h.claudeExe = impl->options.claudeExe ? impl->options.claudeExe : std::function<std::wstring()>{ [impl] {
            std::lock_guard lock{ impl->mutex };
            return impl->claudeExePath;
        } };
        h.model = impl->options.model;
        h.effort = impl->options.effort;
        h.thinking = HarnessThinking::Disabled; // the fastest: no thinking blocks before a tool call
        h.systemPrompt = SessionSearchToolset::SystemPrompt();
        h.toolServers = impl->toolset->Servers();
        h.autoCompactTokens = impl->options.autoCompactTokens;
        h.persistRecord = impl->options.persist;
        // One request = search (maybe a few) + show + one sentence. A model that keeps searching
        // is stopped here, with whatever it showed so far.
        h.maxTurnsPerMessage = 12;
        h.apiMaxRetries = 2;
        h.turnSilenceTimeoutMs = 90000;
        impl->conversation = std::make_unique<HarnessConversation>(std::move(h), [impl](const HarnessEvent& e) { impl->OnHarnessEvent(e); });

        // What the last run left: the conversation's record (loaded by the conversation) and,
        // beside it, the last shown list.
        const HarnessRecord record = impl->conversation->Record();
        std::lock_guard lock{ impl->mutex };
        impl->snapshot.epoch = impl->conversation->Epoch();
        impl->snapshot.turns = record.turns;
        impl->snapshot.usage = record.usage;
        impl->snapshot.model = record.model;
        impl->snapshot.compactions = record.compactions;
        impl->latestTurn = record.turns;
        if (impl->options.persist)
        {
            impl->viewPath = HarnessStateDir() + L"\\" + impl->options.name + L".view.json";
            if (!record.Empty())
            {
                // Only together with its record: a view without the conversation it belongs to
                // would show a result nobody can refer to.
                DeserializeViewInto(ReadViewFile(impl->viewPath), impl->snapshot);
            }
        }
    }

    SessionSearchAgent::~SessionSearchAgent()
    {
        Shutdown();
    }

    uint64_t SessionSearchAgent::AddListener(Listener listener)
    {
        if (!listener)
        {
            return 0;
        }
        std::lock_guard lock{ _impl->mutex };
        const uint64_t token = _impl->nextListenerToken++;
        _impl->listeners.push_back(Impl::ListenerEntry{ token, std::move(listener) });
        return token;
    }

    void SessionSearchAgent::RemoveListener(uint64_t token) noexcept
    {
        if (token == 0)
        {
            return;
        }
        try
        {
            std::lock_guard lock{ _impl->mutex };
            auto& list = _impl->listeners;
            list.erase(std::remove_if(list.begin(), list.end(), [token](const Impl::ListenerEntry& l) { return l.token == token; }), list.end());
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent::RemoveListener");
        }
    }

    void SessionSearchAgent::SetClaudeExePath(std::wstring path)
    {
        std::lock_guard lock{ _impl->mutex };
        _impl->claudeExePath = std::move(path);
    }

    void SessionSearchAgent::Prewarm(bool readIndex)
    {
        if (readIndex)
        {
            _impl->EnsureCatalog(NowUnixMs() - kDefaultCatalogMs);
        }
        _impl->conversation->Prewarm();
    }

    uint64_t SessionSearchAgent::Ask(std::wstring text)
    {
        if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        {
            return 0;
        }
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->shuttingDown)
            {
                return 0;
            }
            _impl->snapshot.busy = true;
            _impl->snapshot.question = text;
            _impl->snapshot.answer.clear();
            _impl->snapshot.answerIsError = false;
            _impl->snapshot.activity = L"Thinking\x2026";
        }
        // The index is read while the model reads the request: by the time it calls the search
        // tool (about a second later) the catalog is there.
        _impl->EnsureCatalog(NowUnixMs() - kDefaultCatalogMs);
        // BEFORE the send: a search of the request that this one replaces must see the change
        // and return, or the interrupt that Send makes waits behind it.
        ++_impl->requestGeneration;
        const uint64_t turn = _impl->conversation->Send(text);
        {
            std::lock_guard lock{ _impl->mutex };
            _impl->latestTurn = (std::max)(_impl->latestTurn, turn);
            _impl->snapshot.turns = (std::max)(_impl->snapshot.turns, turn);
        }
        LogAgent(L"ask[" + std::to_wstring(turn) + L"]: " + text.substr(0, 200));
        SessionSearchEvent e;
        e.kind = SessionSearchEventKind::Asked;
        e.turn = turn;
        e.text = std::move(text);
        _impl->Notify(std::move(e));
        return turn;
    }

    void SessionSearchAgent::Interrupt()
    {
        ++_impl->requestGeneration; // first: a running search returns, so the interrupt can be delivered
        _impl->conversation->Interrupt();
    }

    void SessionSearchAgent::Reset()
    {
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->shuttingDown)
            {
                return;
            }
        }
        ++_impl->requestGeneration; // a running search returns at once and records nothing
        _impl->toolset->Reset();
        _impl->conversation->Reset(); // raises the epoch at once; the worker ends the process
        {
            std::lock_guard lock{ _impl->mutex };
            const std::wstring model = _impl->snapshot.model; // the next conversation uses the same one
            _impl->snapshot = SessionSearchSnapshot{};
            _impl->snapshot.epoch = _impl->conversation->Epoch();
            _impl->snapshot.state = _impl->conversation->State();
            _impl->snapshot.model = model;
            _impl->latestTurn = 0;
        }
        _impl->SaveView(); // an empty conversation: deletes the file
        LogAgent(L"reset");
        SessionSearchEvent e;
        e.kind = SessionSearchEventKind::Reset;
        _impl->Notify(std::move(e));
    }

    void SessionSearchAgent::ReleaseCatalog()
    {
        std::shared_ptr<const SessionCatalog> old;
        {
            std::lock_guard lock{ _impl->mutex };
            old = std::move(_impl->catalog);
            _impl->catalog.reset();
            // Nobody wants an index now: the keeper stops at its next sidecar file and
            // publishes nothing. The sidecar files it wrote stay, so the next build continues
            // where this one stopped.
            _impl->wantSince = 0;
            ++_impl->keeperEpoch;
        }
        _impl->catalogChanged.notify_all();
        // `old` is released here, outside the lock: freeing a large catalog takes a moment.
    }

    void SessionSearchAgent::ParkProcessAfter(int64_t delayMs)
    {
        _impl->conversation->ParkAfter(delayMs);
    }

    void SessionSearchAgent::Shutdown() noexcept
    {
        try
        {
            std::future<void> pending;
            {
                std::lock_guard lock{ _impl->mutex };
                _impl->shuttingDown = true;
                ++_impl->keeperEpoch; // the keeper's running step stops at its next sidecar file
                pending = std::move(_impl->keeper);
            }
            ++_impl->requestGeneration; // a running search returns at once
            _impl->catalogChanged.notify_all(); // ... and so does one that waits for an index
            if (_impl->conversation)
            {
                _impl->conversation->Shutdown(); // no event and no tool call after this returns
            }
            if (pending.valid())
            {
                pending.wait(); // the keeper uses this object: it must be gone before the object is
            }
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent::Shutdown");
        }
    }

    SessionSearchSnapshot SessionSearchAgent::Snapshot() const
    {
        std::lock_guard lock{ _impl->mutex };
        return _impl->snapshot;
    }

    std::vector<SessionIndexEntry> SessionSearchAgent::EntriesFor(const std::vector<std::wstring>& ids) const
    {
        return _impl->EntriesFor(ids);
    }

    // The three diagnostics read the conversation object directly: it exists from the
    // constructor to the destructor (Shutdown ends its process, not the object), and each of its
    // accessors takes the conversation's own lock.
    HarnessRecord SessionSearchAgent::Record() const
    {
        return _impl->conversation->Record();
    }

    HarnessInventory SessionSearchAgent::Inventory() const
    {
        return _impl->conversation->Inventory();
    }

    std::vector<std::wstring> SessionSearchAgent::DescribeCommandLine() const
    {
        return _impl->conversation->DescribeCommandLine();
    }

    SessionSearchAgent& SharedSessionSearchAgent()
    {
        // Leaked on purpose (see the header). A function-local static: created once, thread-safe.
        static SessionSearchAgent* const agent = [] {
            SessionSearchAgentOptions options;
            options.openSessions = [] {
                std::vector<std::pair<std::wstring, std::wstring>> open;
                const auto registry = SharedEngine().registry;
                if (!registry)
                {
                    return open;
                }
                // The two lean registry reads: the live ids, then their titles — never a snapshot
                // of whole records (UI_THREAD.md §10a).
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
}
