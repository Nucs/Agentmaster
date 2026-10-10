// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly). Includes only what it needs.
#include "SessionRegistry.h"

#include <algorithm>
#include <chrono>
#include <cstdio> // swprintf (the [ups] disposition trace's FNV-1a fingerprint)

#include "ClaudeSpawn.h" // AppendStateLog (the --fork-session source-id-echo suppression trace)
#include "TranscriptStore.h" // IsNoiseUserPrompt — keep teammate/control protocol out of the Typed record

namespace
{
    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // A short single-line label for a captured prompt (mirrors the UI's Add-prompt rule).
    std::wstring MakeLabel(const std::wstring& text)
    {
        std::wstring label = text.substr(0, 56);
        std::replace(label.begin(), label.end(), L'\n', L' ');
        std::replace(label.begin(), label.end(), L'\r', L' ');
        return label;
    }

    // A Flight prompt we injected echoes back as a UserPromptSubmit with the same text. We
    // only treat an incoming prompt as that echo when a matching Flight prompt was Sent very
    // recently (so a stale never-echoed prompt, or a reloaded old one, can't swallow a fresh
    // human message that happens to repeat the text).
    constexpr int64_t kEchoWindowMs = 15000;

    // (FoldCrToLf — the DELIVERY.md RC6 newline-fold for every echo/dedupe text compare — moved to
    // SessionModels.h: the scheduler's Enter-retry draft guard needed the identical fold, and two
    // hand-synced copies of a compare rule is how the RC6 bug happened in the first place.)

    // Agentmaster (DELIVERY_PLAN.md R3 — the [ups] disposition trace): a short FNV-1a fingerprint of
    // a prompt's FOLDED text, so hooks.log can say which message a UserPromptSubmit carried (and two
    // deliveries of the same text read as the same hash) without logging the prompt body itself.
    std::wstring FoldedPromptHash(const std::wstring& foldedText)
    {
        uint32_t h = 2166136261u;
        for (const wchar_t c : foldedText)
        {
            h ^= static_cast<uint32_t>(c);
            h *= 16777619u;
        }
        wchar_t buf[12];
        ::swprintf(buf, 12, L"%08x", h);
        return buf;
    }

    // TabTokenEq (the case-insensitive WT_SESSION/tabToken compare this TU leans on everywhere)
    // moved to SessionModels.h — the restart seam's tabToken fallback needed it too.

    // Agentmaster (UI_THREAD.md §10a #4 — the lean reads): is `p` an OPEN prompt — one the SessionScanner can
    // still act on? Pending and the legacy Held wait to be sent; a Sent prompt that is not yet echoed is a
    // delivery the scanner must confirm or judge lost. Everything else is completed history.
    //   p       — a queued prompt.
    //   returns — true for Pending, Held, and Sent-but-unechoed. Never throws.
    bool IsOpenScanPrompt(const Agentmaster::QueuedPrompt& p) noexcept
    {
        using Agentmaster::PromptStatus;
        return p.status == PromptStatus::Pending || p.status == PromptStatus::Held ||
               (p.status == PromptStatus::Sent && !p.echoed);
    }

    // Agentmaster (UI_THREAD.md §10a #4; SIDE_PANEL.md §2c): WHICH members a lean copy leaves out of a
    // stored record. Each lean read names the narrowest kind that still holds everything its caller reads.
    enum class LeanCopyKind : uint8_t
    {
        WithoutQueue, // GetWithoutQueue: the prompt queue only
        Scan, // SnapshotLiveForScan: the queue (refilled with its OPEN prompts) + all four display texts
        Cards, // SnapshotLiveForCards: the queue + lastAssistantText, recap and pendingPasteRefs; pendingInput KEPT
    };

    // Agentmaster (UI_THREAD.md §10a #4): moves the members a lean copy must not copy OUT of a stored session
    // record, and moves them back when it goes out of scope — also when the copy made in between throws.
    // Copying a SessionInfo while its queue (and, per LeanCopyKind, some of its display texts) is parked here
    // copies every other member exactly as Get does, so a member added to SessionInfo later is carried by the
    // lean reads without anyone listing it. std::vector / std::wstring move construction and move assignment
    // are noexcept with std::allocator, so parking and restoring cannot fail; each parked member is cleared
    // after its move, so the copy sees it EMPTY whatever state the move left. Use it only under the registry
    // lock: the members are out of the record while the guard lives.
    class ParkedSessionMembers
    {
    public:
        // Parks `owner.queue` always; lastAssistantText, recap and pendingPasteRefs for the Scan and Cards
        // kinds; pendingInput for the Scan kind only (a card shows the unsent-draft dots, so it needs to know
        // whether a draft exists — the draft is short, unlike the transcript texts).
        //   owner — the stored record; the parked members move out now and back in the destructor.
        //   kind  — which members to park (LeanCopyKind).
        ParkedSessionMembers(Agentmaster::SessionInfo& owner, LeanCopyKind kind) noexcept :
            _owner{ owner },
            _parkTexts{ kind == LeanCopyKind::Scan || kind == LeanCopyKind::Cards },
            _parkPendingInput{ kind == LeanCopyKind::Scan },
            _queue(std::move(owner.queue))
        {
            owner.queue.clear();
            if (_parkTexts)
            {
                _lastAssistantText = std::move(owner.lastAssistantText);
                owner.lastAssistantText.clear();
                _recap = std::move(owner.recap);
                owner.recap.clear();
                _pendingPasteRefs = std::move(owner.pendingPasteRefs);
                owner.pendingPasteRefs.clear();
            }
            if (_parkPendingInput)
            {
                _pendingInput = std::move(owner.pendingInput);
                owner.pendingInput.clear();
            }
        }

        // Puts every parked member back into the stored record (noexcept moves), on the normal path and
        // during unwinding alike.
        ~ParkedSessionMembers()
        {
            _owner.queue = std::move(_queue);
            if (_parkTexts)
            {
                _owner.lastAssistantText = std::move(_lastAssistantText);
                _owner.recap = std::move(_recap);
                _owner.pendingPasteRefs = std::move(_pendingPasteRefs);
            }
            if (_parkPendingInput)
            {
                _owner.pendingInput = std::move(_pendingInput);
            }
        }

        ParkedSessionMembers(const ParkedSessionMembers&) = delete;
        ParkedSessionMembers& operator=(const ParkedSessionMembers&) = delete;

        // The parked queue in its stored order — what the scan copy picks its open prompts from.
        //   returns — a view that is valid until the guard is destroyed. Never throws.
        const std::vector<Agentmaster::QueuedPrompt>& Queue() const noexcept
        {
            return _queue;
        }

    private:
        Agentmaster::SessionInfo& _owner;
        bool _parkTexts; // lastAssistantText, recap and pendingPasteRefs are parked
        bool _parkPendingInput; // pendingInput is parked
        std::vector<Agentmaster::QueuedPrompt> _queue;
        std::wstring _lastAssistantText;
        std::wstring _recap;
        std::wstring _pendingInput;
        std::wstring _pendingPasteRefs;
    };

    // Agentmaster (UI_THREAD.md §10a #4): a copy of a stored record that never copies its prompt queue — the
    // shared body of SessionRegistry::GetWithoutQueue (WithoutQueue), SnapshotLiveForScan (Scan: the four
    // display texts are not copied either, and the copy's queue is refilled with the stored record's OPEN
    // prompts, IsOpenScanPrompt, in their order) and SnapshotLiveForCards (Cards: lastAssistantText, recap and
    // pendingPasteRefs are not copied, pendingInput is; the queue stays empty).
    // The caller must hold the registry mutex: the stored record is a non-const element of the registry's map,
    // reached through a const accessor, and the const_cast lets the parking guard move its members out and
    // back. Every reader of a stored record takes that same mutex, so no thread can observe the parked state.
    //   stored  — the registry's own record.
    //   kind    — which members to leave out (LeanCopyKind).
    //   returns — the lean copy.
    //   throws  — std::bad_alloc; the guard's destructor restores the stored record first.
    Agentmaster::SessionInfo CopyLeanSessionLocked(const Agentmaster::SessionInfo& stored, LeanCopyKind kind)
    {
        auto& owner = const_cast<Agentmaster::SessionInfo&>(stored);
        ParkedSessionMembers parked{ owner, kind };
        Agentmaster::SessionInfo copy = owner; // every member except the parked ones, which read empty right now
        if (kind == LeanCopyKind::Scan)
        {
            for (const auto& p : parked.Queue())
            {
                if (IsOpenScanPrompt(p))
                {
                    copy.queue.push_back(p);
                }
            }
        }
        return copy; // the guard restores the stored members after the return value is built
    }

    // Agentmaster: a session's CONVERSATION-activity freshness — the newest of its real activity
    // signals (an authoritative hook, the transcript mtime, the hook-driven activity anchor).
    // Deliberately EXCLUDES lastObservedUnixMs (which only means "the survey looked at it just now",
    // and would otherwise make whichever id the observer resolved this tick always 'win'). The
    // genuinely-current conversation on a ConPTY always has the freshest such activity, so this is
    // the "newest wins" tiebreaker when two records claim one (tabToken, pid).
    int64_t TabSessionFreshness(const Agentmaster::SessionInfo& s) noexcept
    {
        int64_t f = s.lastHookUnixMs;
        if (s.convLastActivityUnixMs > f)
        {
            f = s.convLastActivityUnixMs;
        }
        if (s.lastActivityUnixMs > f)
        {
            f = s.lastActivityUnixMs;
        }
        return f;
    }
}

namespace Agentmaster
{
    void SessionRegistry::_notify(const SessionInfo& snapshot, HookEvent cause)
    {
        std::vector<RegistryObserver> observers;
        {
            std::lock_guard guard{ _mtx };
            observers.reserve(_observers.size());
            for (const auto& o : _observers)
            {
                observers.push_back(o.second);
            }
        }
        for (auto& ob : observers)
        {
            if (ob)
            {
                try
                {
                    ob(snapshot, cause);
                }
                catch (...)
                {
                    // Forensics: a throwing lens observer silently stops THAT window refreshing its
                    // board/tree/tab-dot while the others keep updating — the hardest kind of "the UI
                    // is stale in one window only" report to chase without the throw site.
                    LogSwallowedException(L"SessionRegistry::_notify observer");
                }
            }
        }
    }

    std::wstring SessionRegistry::Upsert(SessionInfo info)
    {
        auto id = info.id;
        SessionInfo snapshot;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            // Agentmaster (the STATE SETTLE hold — DELIVERY.md §16): the registry OWNS the settle clocks,
            // so the incoming record's stateChangedAtUnixMs is never trusted — it is usually a snapshot
            // copy, and re-upserting a stale copy must not move the clock either way. A NEW record's
            // current status begins now; a REPLACED record keeps the clock it had unless this upsert
            // changed its status, or flipped it open/archived or launched/dormant (a resume Upserts the
            // archived record back with live=true — exactly the "just changed" moment the hold is for).
            const int64_t settleNow = NowMs();
            if (it == _sessions.end())
            {
                _order.push_back(id);
                info.stateChangedAtUnixMs = settleNow;
            }
            else
            {
                const auto before = CaptureSettleFacts(it->second);
                info.stateChangedAtUnixMs = it->second.stateChangedAtUnixMs;
                StampSettleClock(info, before, settleNow);
            }
            // A prompt arriving with no queue stamp is new input as far as the hold is concerned (a
            // freshly built queue, or one reloaded from disk — the field is transient).
            StampNewlyQueuedPrompts(info.queue, settleNow);
            _sessions[id] = std::move(info);
            // Agentmaster (bounded queue history): a queue persisted before the cap existed (or one
            // carrying a long session's worth of Typed captures) trims on load; the append seams in
            // OnHookEvent / NoteExternalPrompt keep it bounded live. TrimQueueHistory never drops
            // queued WORK — only the oldest completed history (SessionModels.h).
            TrimQueueHistory(_sessions[id].queue);
            snapshot = _sessions[id];
        }
        _notify(snapshot, HookEvent::Unknown);
        return id;
    }

    void SessionRegistry::Remove(const std::wstring& id)
    {
        SessionInfo snapshot;
        bool existed = false;
        {
            std::lock_guard guard{ _mtx };
            if (const auto it = _sessions.find(id); it != _sessions.end())
            {
                snapshot = it->second; // capture BEFORE erase so the notify can identify what left
                existed = true;
            }
            _sessions.erase(id);
            _injectors.erase(id);
            _submitters.erase(id); // PENDING_INPUT.md §9 — same lifetime as the injector
            _pressers.erase(id); // DELIVERY_PLAN.md R8 — same lifetime as the submitter
            _lastHumanInput.erase(id);
            // PENDING_INPUT.md §2c: a removed session's pastes can never back a placeholder again —
            // release them (and their share of the fleet-wide cap) with the record. A restore-fresh
            // re-key MOVES the journal to the new id BEFORE this Remove (MovePasteJournal).
            if (const auto jt = _pasteJournals.find(id); jt != _pasteJournals.end())
            {
                for (const auto& e : jt->second)
                {
                    _pasteJournalChars -= (std::min)(_pasteJournalChars, e.text.size());
                }
                _pasteJournals.erase(jt);
            }
            _retiredHostTokens.erase(id); // RETIRED HOSTS: a removed session has no host left to veto
            _order.erase(std::remove(_order.begin(), _order.end(), id), _order.end());
        }
        // Agentmaster: notify like Upsert/Update do — a removal IS a fleet change. Remove was
        // the ONLY data mutator that silently skipped _notify, so a resume-fresh that drops a stale
        // archived record (TerminalPage _LaunchClaudeSession -> Remove(restored->id)) left a GHOST
        // "Archived" row on every OTHER window's observer-driven Manager/Archive list until some
        // unrelated event happened to rebuild it. Fire outside the lock (observers may re-enter). Mark
        // the snapshot not-live so the Scheduler's OnObserved short-circuits instead of trying to
        // advance a session that no longer exists.
        if (existed)
        {
            snapshot.live = false;
            _notify(snapshot, HookEvent::Unknown);
        }
    }

    bool SessionRegistry::Contains(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        return _sessions.find(id) != _sessions.end();
    }

    std::optional<SessionInfo> SessionRegistry::Get(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<SessionInfo> SessionRegistry::Snapshot() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<SessionInfo> out;
        out.reserve(_order.size());
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it != _sessions.end())
            {
                out.push_back(it->second);
            }
        }
        return out;
    }

    std::vector<SessionInfo> SessionRegistry::SnapshotLive() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<SessionInfo> out;
        // Size the vector once (UI_THREAD.md §10a #4): growing it by doubling MOVES every record already
        // copied (~30 members each) on each reallocation — ~21% of the scanner thread's time when it used this
        // read. Counting the live records first is a walk over flags, no copies.
        size_t liveCount = 0;
        for (const auto& entry : _sessions)
        {
            if (entry.second.live)
            {
                ++liveCount;
            }
        }
        out.reserve(liveCount);
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it != _sessions.end() && it->second.live)
            {
                out.push_back(it->second);
            }
        }
        return out;
    }

    std::optional<SessionInfo> SessionRegistry::GetWithoutQueue(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end())
        {
            return std::nullopt;
        }
        return CopyLeanSessionLocked(it->second, LeanCopyKind::WithoutQueue);
    }

    std::wstring SessionRegistry::GetTabToken(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        return it == _sessions.end() ? std::wstring{} : it->second.tabToken;
    }

    std::vector<SessionTabBinding> SessionRegistry::LiveTabBindings() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<SessionTabBinding> out;
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it != _sessions.end() && it->second.live)
            {
                const SessionInfo& s = it->second;
                out.push_back(SessionTabBinding{ s.id, s.workingDir, s.tabToken });
            }
        }
        return out;
    }

    std::vector<std::wstring> SessionRegistry::ManagedCodexTabTokens() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<std::wstring> out;
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                continue;
            }
            const SessionInfo& s = it->second;
            // The same filter the observer applied to its full snapshot: a codex WE launched (not external),
            // still open, whose ConPTY is known. An external codex stays in the census on purpose.
            if (s.kind == AgentKind::Codex && s.live && !s.external && !s.tabToken.empty())
            {
                out.push_back(s.tabToken);
            }
        }
        return out;
    }

    std::vector<SessionHeadline> SessionRegistry::HeadlinesOf(const std::vector<std::wstring>& ids) const
    {
        std::vector<SessionHeadline> out;
        out.reserve(ids.size()); // before the lock: the one allocation that needs no registry state
        std::lock_guard guard{ _mtx };
        for (const auto& id : ids)
        {
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                continue; // unknown (never loaded, or removed): the caller falls back to its own source
            }
            const SessionInfo& s = it->second;
            // (std::max) parenthesized: windows.h's max macro.
            out.push_back(SessionHeadline{ s.id, s.title, s.state, s.live, (std::max)(s.lastActivityUnixMs, s.convLastActivityUnixMs) });
        }
        return out;
    }

    std::vector<SessionInfo> SessionRegistry::SnapshotLiveForScan() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<SessionInfo> out;
        // Size once, as in SnapshotLive: a reallocation would move every lean record already built.
        size_t liveCount = 0;
        for (const auto& entry : _sessions)
        {
            if (entry.second.live)
            {
                ++liveCount;
            }
        }
        out.reserve(liveCount);
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it != _sessions.end() && it->second.live)
            {
                out.push_back(CopyLeanSessionLocked(it->second, LeanCopyKind::Scan));
            }
        }
        return out;
    }

    std::vector<SessionInfo> SessionRegistry::SnapshotLiveForCards() const
    {
        std::lock_guard guard{ _mtx };
        std::vector<SessionInfo> out;
        // Size once, as in SnapshotLive: a reallocation would move every lean record already built.
        size_t liveCount = 0;
        for (const auto& entry : _sessions)
        {
            if (entry.second.live)
            {
                ++liveCount;
            }
        }
        out.reserve(liveCount);
        for (const auto& id : _order)
        {
            const auto it = _sessions.find(id);
            if (it != _sessions.end() && it->second.live)
            {
                out.push_back(CopyLeanSessionLocked(it->second, LeanCopyKind::Cards));
            }
        }
        return out;
    }

    size_t SessionRegistry::Count() const
    {
        std::lock_guard guard{ _mtx };
        return _sessions.size();
    }

    std::vector<SessionInfo> SessionRegistry::_SupersedeStaleTabSiblings(const std::wstring& winnerId, const std::wstring& tabToken, uint32_t pid, int64_t winnerFreshness)
    {
        std::vector<SessionInfo> archived;
        if (tabToken.empty() || pid == 0)
        {
            return archived; // need a ConPTY id AND a known process to assert "same claude, stale conversation"
        }
        for (auto& [id, s] : _sessions)
        {
            if (id == winnerId || !s.live)
            {
                continue;
            }
            // A DIFFERENT live process on the same WT_SESSION is a child sub-claude that inherited the
            // parent's env (Bash-tool `claude`, `claude -p`, a hook) — a real, concurrent process; never
            // archive it. An unknown-pid (never-observed) sibling IS archivable: it is a stale leftover
            // record of this same conversation lineage, not a live tab.
            if (s.pid != 0 && s.pid != pid)
            {
                continue;
            }
            if (!TabTokenEq(s.tabToken, tabToken))
            {
                continue;
            }
            // Newest wins: a sibling that is genuinely fresher than the confirmed-current conversation
            // is NOT stale — never let a mis-resolved/late observation archive the live conversation.
            if (TabSessionFreshness(s) > winnerFreshness)
            {
                continue;
            }
            // Archiving flips a settle fact (live) — restart the record's settle clock like every other
            // registry seam does (DELIVERY.md §16), so a later revival never inherits a pre-archive anchor.
            const auto settleBefore = CaptureSettleFacts(s);
            s.live = false;
            StampSettleClock(s, settleBefore, NowMs());
            archived.push_back(s);
        }
        return archived;
    }

    void SessionRegistry::OnHookEvent(const HookMessage& msg)
    {
        // Agentmaster (RETIRED HOSTS — RetireHostToken): a hook whose ConPTY WE replaced (a Deactivate parked
        // a fresh connection in the tab, or a Restart started one) is the closing claude's last word about a
        // session that now lives on a different connection. ANY event from it is noise — a SessionEnd would
        // mark a parked tab Done, a Stop/Notification would move its state, and every one of them would
        // write tabToken back to the dead ConPTY (the liveness sweep then finds no pane holding the token).
        // Drop it whole, before every other rule. (The [stale-end] guard below stays for the supersedes we
        // did not retire explicitly — an in-session re-home.) The in-lock re-check further down closes the
        // microsecond window where a retirement lands between this check and the state mutation.
        if (!msg.tabToken.empty())
        {
            bool fromRetiredHost = false;
            {
                std::lock_guard guard{ _mtx };
                fromRetiredHost = _FindRetiredHostTokenLocked(msg.sessionId, msg.tabToken, NowMs()) != nullptr;
            }
            if (fromRetiredHost)
            {
                AppendStateLog(L"hooks.log", L"[retired-host] dropped " + std::wstring{ HookEventName(msg.event) } + L" for " + msg.sessionId + L" from replaced ConPTY " + msg.tabToken + L" (its closing claude \x2014 the session now lives on a new connection)\n");
                return;
            }
        }

        // Agentmaster (--fork-session source-id echo, SessionModels.h SessionInfo::forkParentId): a
        // `claude --resume <src> --fork-session --session-id <new>` fork fires its FIRST SessionStart
        // hook under the SOURCE id <src>, NOT the freshly minted <new> we registered + bound to this
        // ConPTY at launch. Were it processed, the unknown <src> would be adopted as a new session and
        // the bind/re-home path would re-home the fork's tab off <new> onto <src> (the inactive source
        // it branched from), orphaning the real fork — every later hook lands on <new>. Recognize the
        // echo precisely: a LIVE fork session on the SAME ConPTY (the eagerly stamped tabToken) carries
        // forkParentId == <src>. The tabToken match is the discriminator, so a genuinely-open <src> in
        // another tab (its own, different ConPTY) is unaffected. Ignore the event whole — no phantom
        // record, no adoption fan-out. Scoped to SessionStart (rare), so the extra scan is off the
        // hot path. (Mirrors the observer's id resolution, which already prefers --session-id over
        // --resume; the push side needed the same.)
        if (msg.event == HookEvent::SessionStart && !msg.tabToken.empty())
        {
            bool forkSourceEcho = false;
            {
                std::lock_guard guard{ _mtx };
                for (auto& [sid, fs] : _sessions)
                {
                    // The guard is armed only while the fork's startup echo is still EXPECTED
                    // (!forkEchoConsumed): the echo fires exactly once, so consuming it here lets a
                    // LATER deliberate `/resume <src>` in the fork's tab re-home normally — even on a
                    // fork that never produced content (which keeps forkParentId for the re-fork seam).
                    if (fs.live && !fs.forkParentId.empty() && !fs.forkEchoConsumed &&
                        TabTokenEq(fs.forkParentId, msg.sessionId) &&
                        TabTokenEq(fs.tabToken, msg.tabToken))
                    {
                        fs.forkEchoConsumed = true; // one-shot: the startup echo has now happened
                        forkSourceEcho = true;
                        break;
                    }
                }
            }
            if (forkSourceEcho)
            {
                AppendStateLog(L"hooks.log", L"[fork-echo] ignored source-id SessionStart " + msg.sessionId + L" (its fork already owns ConPTY " + msg.tabToken + L")\n");
                return;
            }
        }

        // Agentmaster (the summary's conversation chain — SessionInfo::continuedFromId): the /clear END half.
        // Remember which conversation a /clear just ended on this ConPTY, BEFORE the stale-end guard below
        // can drop the event: that guard drops a SessionEnd whose record now names another ConPTY, but the
        // conversation still ENDED on this one. The SessionStart(source "clear") that follows on the same
        // ConPTY pairs with this entry (the START half, inside the main lock below). One entry per ConPTY
        // (a newer /clear there replaces it), expired entries pruned, the oldest dropped past the cap.
        if (msg.event == HookEvent::SessionEnd && msg.source == kHookSourceClear && !msg.tabToken.empty() && !msg.sessionId.empty())
        {
            std::lock_guard guard{ _mtx };
            const int64_t now = NowMs();
            std::erase_if(_recentClearEnds, [&](const RecentClearEnd& e) {
                return now - e.endedAtUnixMs > kRecentClearEndTtlMs || TabTokenEq(e.tabToken, msg.tabToken);
            });
            if (_recentClearEnds.size() >= kRecentClearEndsMax)
            {
                _recentClearEnds.erase(_recentClearEnds.begin());
            }
            _recentClearEnds.push_back(RecentClearEnd{ msg.tabToken, msg.sessionId, now });
        }

        // Agentmaster (restart/re-home stale death notice): a SessionEnd arriving from a ConPTY that is
        // NO LONGER this session's host is the OLD process's dying breath after an in-place "Restart
        // session" (the restart seam re-stamped tabToken to the NEW connection before killing the old
        // one) — or any other supersede. Applying it would mark the freshly-restarted session Done AND
        // steal tabToken back to the dead ConPTY, which the liveness sweep then can't find in the tab
        // (foundSessionConn=false) => the live session is wrongly archived seconds after its restart
        // (the observed [restart] -> [SessionEnd] -> [liveness] dead cascade). Drop the event whole.
        // Scoped to SessionEnd only: other mismatched-token hooks stay accepted — tabToken tracking
        // across an in-session /resume re-home depends on them.
        if (msg.event == HookEvent::SessionEnd && !msg.tabToken.empty())
        {
            bool staleHost = false;
            {
                std::lock_guard guard{ _mtx };
                if (const auto it = _sessions.find(msg.sessionId); it != _sessions.end())
                {
                    const auto& cur = it->second.tabToken;
                    staleHost = !cur.empty() && !TabTokenEq(cur, msg.tabToken);
                }
            }
            if (staleHost)
            {
                AppendStateLog(L"hooks.log", L"[stale-end] ignored SessionEnd for " + msg.sessionId + L" from superseded ConPTY " + msg.tabToken + L" (session now hosted elsewhere \x2014 restarted/re-homed)\n");
                return;
            }
        }

        SessionInfo snapshot;
        bool found = false;
        bool triggerAdvance = false;
        std::wstring upsTrace; // DELIVERY_PLAN.md R3: the [ups] disposition line, filled under the lock, logged after it
        std::wstring mergeTrace; // DELIVERY_PLAN.md R7: the [merge-detected] line (both logs), filled under the lock
        std::wstring holdTrace; // DELIVERY.md §14: the [interrupt-resume] line (both logs), filled under the lock
        std::wstring lineageTrace; // the summary's conversation chain: the [lineage] /clear pairing line, filled under the lock

        {
            std::lock_guard guard{ _mtx };
            // Agentmaster (RETIRED HOSTS): the race belt for the pre-check at the top — a Deactivate/Restart
            // that retired this hook's host AFTER that check but before this lock. Silent: the window is
            // microseconds wide and the pre-check logs every ordinary drop.
            if (!msg.tabToken.empty() && _FindRetiredHostTokenLocked(msg.sessionId, msg.tabToken, NowMs()) != nullptr)
            {
                return;
            }
            auto it = _sessions.find(msg.sessionId);
            if (it == _sessions.end())
            {
                // A SessionStart may race ahead of our own Upsert (or arrive for a session
                // we restored without re-registering). Create a minimal record rather than
                // dropping authoritative state. Any other event for an unknown session is
                // ignored (we have no connection to bind it to).
                if (msg.event != HookEvent::SessionStart)
                {
                    return;
                }
                SessionInfo created;
                created.id = msg.sessionId;
                created.workingDir = msg.cwd;
                created.state = SessionState::Idle;
                // A hook means a real claude is running, so this session is OPEN (live), not
                // archived — it belongs on the Triage Board immediately.
                created.live = true;
                // Mark it external/observe-only; the adoption handler (fired below, outside
                // the lock) tries to bind it to its ConPTY for full control.
                created.external = true;
                // Agentmaster (adopt/re-home autorunner default — SetDefaultAutorunnerMode): a record
                // minted HERE is a newly-OPENED session (a hand-typed adopted claude, or the NEW
                // conversation id an in-session /clear or /resume mints on a re-homed tab), so it gets
                // the same cog default MODE the launch/restore seams stamp — it used to default Off,
                // silently exempting exactly these sessions from the Tests Autorunner. Config only,
                // creation only; never SessionState.
                created.autorunner.mode = _defaultAutorunnerMode;
                // The STATE SETTLE clock (DELIVERY.md §16): a record created here is a session whose
                // current status begins now — an adopted claude's queue waits out a full window first.
                created.stateChangedAtUnixMs = NowMs();
                _order.push_back(msg.sessionId);
                it = _sessions.emplace(msg.sessionId, std::move(created)).first;
            }

            auto& s = it->second;
            // Captured BEFORE the ordered transition below, so a real status change restarts the settle
            // clock (DELIVERY.md §16) while a no-op transition — a stale or type-ahead Stop that keeps the
            // state, a duplicate UserPromptSubmit on a Running turn — leaves it alone.
            const auto settleBefore = CaptureSettleFacts(s);
            // Provenance (Fleet Observer, OBSERVER.md §9): a hook is the authoritative PUSH. Record
            // that this session is hook-wired this run + when the last push arrived, so the observer
            // can LOG provenance (push vs pull) without ever overriding state.
            s.hookWired = true;
            s.lastHookUnixMs = (msg.ts != 0) ? msg.ts : NowMs();
            // Remember the hosting ConPTY (WT_SESSION) from EVERY hook — the stable tab identity the
            // app reconciles against (survives an in-session /resume that changes the session id).
            if (!msg.tabToken.empty())
            {
                s.tabToken = msg.tabToken;
            }
            // Agentmaster (the summary's conversation chain — SessionInfo::continuedFromId): a SessionStart
            // whose payload source is "clear" IS a /clear — a NEW conversation id on the SAME claude process
            // and ConPTY. Pair it with the conversation it replaced and record the link ONCE, so the summary
            // can list the conversation the /clear left behind: no transcript records it, and a /resume
            // switches the id the same way while continuing nothing — the hook's source is the only proof.
            // Recorded HERE rather than on the tab re-home: the Fleet Observer can re-home the tab before
            // this hook lands. The pairing, in order of certainty:
            //   1. EXACT — the conversation whose SessionEnd(reason "clear") just arrived on this ConPTY
            //      (_recentClearEnds; Claude finishes the old conversation's SessionEnd hooks first). The
            //      entry is consumed: one /clear ends one conversation.
            //   2. FALLBACK (that SessionEnd was lost — a forwarder killed at its timeout) — the OTHER record
            //      carrying this ConPTY: a live one beats an archived one (an archived record may still
            //      carry the token of an earlier conversation in the same tab), then the most recent
            //      hook/activity.
            // No candidate => no link (never guess).
            if (msg.event == HookEvent::SessionStart && msg.source == kHookSourceClear &&
                !msg.tabToken.empty() && s.continuedFromId.empty())
            {
                std::wstring predecessorId;
                bool exact = false;
                const int64_t now = NowMs();
                for (auto e = _recentClearEnds.begin(); e != _recentClearEnds.end(); ++e)
                {
                    if (TabTokenEq(e->tabToken, msg.tabToken) && e->sessionId != s.id && now - e->endedAtUnixMs <= kRecentClearEndTtlMs)
                    {
                        predecessorId = e->sessionId;
                        exact = true;
                        _recentClearEnds.erase(e); // consumed: the iterator is not used again
                        break;
                    }
                }
                if (predecessorId.empty())
                {
                    const SessionInfo* predecessor = nullptr;
                    const auto rank = [](const SessionInfo& x) {
                        return std::make_pair(x.live ? 1 : 0, std::max(x.lastHookUnixMs, x.lastActivityUnixMs));
                    };
                    for (const auto& [sid, other] : _sessions)
                    {
                        if (sid == s.id || !TabTokenEq(other.tabToken, msg.tabToken))
                        {
                            continue;
                        }
                        if (predecessor == nullptr || rank(other) > rank(*predecessor))
                        {
                            predecessor = &other;
                        }
                    }
                    if (predecessor != nullptr)
                    {
                        predecessorId = predecessor->id;
                    }
                }
                if (!predecessorId.empty())
                {
                    s.continuedFromId = predecessorId;
                    s.continuedVia = std::wstring{ kHookSourceClear };
                    lineageTrace = L"[lineage] " + ShortId(s.id) + L" continues " + ShortId(predecessorId) +
                                   L" (/clear on ConPTY " + msg.tabToken +
                                   (exact ? L", paired by its SessionEnd(clear))\n" : L", no SessionEnd(clear) seen: paired by the ConPTY's other record)\n");
                }
            }
            // Agentmaster (--fork-session source-id echo): this hook is for the session's OWN id, so the
            // fork has settled past its startup window (where it echoed the SOURCE id). Retire the
            // one-shot echo guard — a LATER deliberate in-session /resume back to that exact source id
            // should re-home normally, not be mistaken for the (already-passed) startup echo.
            s.forkEchoConsumed = true;
            // The RE-FORK link (forkParentId) is retired separately, and only by an own-id hook that
            // proves the fork PRODUCED CONTENT (its own transcript exists now, so it must be resumed,
            // never re-forked). SessionStart and SessionEnd prove nothing of the sort — a relaunch of a
            // still-transcript-less fork fires SessionStart, and the dying process fires SessionEnd —
            // and clearing on them destroyed the link exactly when the restart/restore re-fork seams
            // needed it (a restarted never-messaged fork came back as an EMPTY conversation).
            if (!s.forkParentId.empty() &&
                msg.event != HookEvent::SessionStart && msg.event != HookEvent::SessionEnd)
            {
                s.forkParentId.clear();
            }
            // Ordered transition (HookEvents.h): stale-Stop + type-ahead aware. The wire `ts` is
            // the hook's FIRE time — events can arrive out of order (the Stop forwarder does
            // transcript work first), and a prompt typed mid-turn produces a follow-on turn with
            // NO further UserPromptSubmit — both used to wrongly land WaitingForInput mid-turn.
            const auto ordered = NextSessionStateOrdered(s.state, msg, s.turns);
            s.state = ordered.state;
            // Stamped with the registry's ARRIVAL clock, not the hook's wire ts: the hold must measure
            // from when WE learned of the change (the send-spacing lesson — never trust a wire-derived
            // time to gate a send), and arrival can only start the window later, never earlier.
            StampSettleClock(s, settleBefore, NowMs());
            // Agentmaster (the COMPACTION WINDOW — HOOKS.md "Compaction"): PreCompact OPENS it (the
            // scanner's CompactionInFlight then keeps the silent 1-3+ minute summarization from reading
            // as a finished turn); PostCompact CLOSES it, and so does anything that proves the window is
            // over or moot — the trailing SessionStart (compact or otherwise), SessionEnd, a Stop (real or
            // a synthesized release; a real one cannot overtake the PreCompact that follows it, because
            // claude AWAITS a Stop hook — it may block the stop — before it runs a queued /compact), or an
            // API-error synth (a failed compaction). Deliberately NOT a
            // UserPromptSubmit: a message the user types during a compaction fires its hook, but claude is
            // still compacting, so the window must keep holding the reconcilers.
            if (msg.event == HookEvent::PreCompact)
            {
                s.compactingSinceUnixMs = (msg.ts != 0) ? msg.ts : NowMs();
            }
            else if (msg.event == HookEvent::PostCompact || msg.event == HookEvent::SessionStart ||
                     msg.event == HookEvent::SessionEnd || msg.event == HookEvent::Stop || msg.apiError ||
                     msg.turnCutOff) // a cut-off turn: whatever compaction was in flight died with its process
            {
                s.compactingSinceUnixMs = 0;
            }
            // Agentmaster (API-error triage): preserve the failure reason WHILE in Error so the
            // Triage-Board Error card can show what died (the scanner's recon-error synth carries it on
            // the apiError message); clear it the instant the session leaves Error (recovery), so a
            // recovered/normal card never shows a stale error. Only the apiError synth sets these —
            // and, since 2026-10-09, the CUT-OFF synth (msg.turnCutOff, HOOKS.md "Cut-off turns"), whose
            // message says that the last turn did not finish and whose status is 0 (no HTTP code).
            if (msg.apiError || msg.turnCutOff)
            {
                s.errorMessage = msg.errorMessage;
                s.errorStatus = msg.errorStatus;
                // A fresh Error entry consumes any stale manual dismissal (the triage "Move to
                // Idle/Done" on a PRIOR error): this error is a new fact the user hasn't acked, so
                // the scanner's dismissal gate must never suppress it off a leftover flag (e.g. a
                // rewind back onto an undismissed error after an earlier one was dismissed).
                s.errorDismissed = false;
            }
            else if (s.state != SessionState::Error)
            {
                s.errorMessage.clear();
                s.errorStatus = 0;
            }
            // Agentmaster (CUT-OFF TURNS — HOOKS.md "Cut-off turns"): a session that is Running has a
            // turn in a live process (a real prompt, the scanner's recon-run off fresh transcript lines,
            // or claude's own working heartbeat). Whatever turn was in flight when the record was last
            // restored is behind it, so the restore-time hint is spent. Cleared on EVERY path into
            // Running, on purpose: left standing, it would turn the NEXT at-rest moment of the new turn
            // (a permission prompt, a bare prompt after an Esc) into a false "cut off".
            // Not cleared by SessionStart / SessionEnd / a Stop: none of them proves that a turn ran.
            if (s.state == SessionState::Running)
            {
                s.midTurnAtRestore = false;
            }
            if (msg.ts > s.lastActivityUnixMs)
            {
                s.lastActivityUnixMs = msg.ts; // monotonic: a stale event must not regress the decay anchor
            }
            // Agentmaster (⚡ server-cache hint): the API-turn evidence stamp — the same monotonic
            // wire-ts discipline as the decay anchor above, but ONLY for events that mean Claude just
            // made a real API request (IsApiTurnEvidence: never SessionStart/SessionEnd, never a
            // synthesized quiescent Stop). Keeping the two timestamps apart is the whole point: the
            // anchor is "when did this card last demand attention" (SessionStart + the UI triage
            // moves stamp it), this is "when was the server-side prompt cache last touched"
            // (ServerCacheStillWarm reads it) — conflating them showed ⚡ on adopt/launch/promote.
            if (msg.ts > s.lastTurnUnixMs && IsApiTurnEvidence(msg))
            {
                s.lastTurnUnixMs = msg.ts;
            }
            // Agentmaster (MOST RECENT sort): the two TURN-BOUNDARY stamps — the PUSH half (the observer's
            // transcript-tail read + the scanner's pull-echo are the pull half; every writer is a monotonic
            // MAX so none regresses another). START = a prompt-CARRYING UserPromptSubmit — the same
            // condition that stamps turns.lastPromptUnixMs (an EMPTY twin / the recon-run synth is
            // state-only). END = a REAL Stop — never a synthesized quiescent one, which is
            // reconciliation-timed, not turn-timed (the interrupt's end is stamped from the marker's own
            // line by the tail read instead). Tool events deliberately touch NEITHER: that is the whole
            // difference from lastTurnUnixMs above and from MOST ACTIVE.
            if (msg.event == HookEvent::UserPromptSubmit && !msg.promptText.empty() && msg.ts > s.turnStartUnixMs)
            {
                s.turnStartUnixMs = msg.ts;
            }
            if (msg.event == HookEvent::Stop && !msg.quiescentStop && msg.ts > s.turnEndUnixMs)
            {
                s.turnEndUnixMs = msg.ts;
            }
            if (s.workingDir.empty() && !msg.cwd.empty())
            {
                s.workingDir = msg.cwd;
            }
            if (msg.event == HookEvent::Stop && !ordered.staleStop)
            {
                // A stale Stop describes an OLDER turn — its question bit must not overwrite the
                // current turn's (the duplicate [Stop][Stop] pairs a slow forwarder produces).
                s.lastMessageWasQuestion = msg.lastMessageIsQuestion;
            }

            // The Auto Testing reflects EVERY message a session received. A UserPromptSubmit is
            // either the echo of a prompt WE just injected (suppress it — it is already in the
            // queue as Sent), or a prompt the human typed straight into the ConPTY (record it
            // as a Sent/Typed entry so the Auto Testing's "sent" summary is complete) — UNLESS it
            // is machine-injected protocol/control traffic (IsNoiseUserPrompt, the SAME filter the
            // scanner's back-fill applies at SessionScanner.cpp): a TEAMMATE-message delivery fires
            // a REAL UserPromptSubmit on the lead ("Another Claude session sent a message:\n
            // <teammate-message …>" — measured live, one per teammate report/idle notification), and
            // recording each as a "typed" prompt filled the SENT list (and sessions.json) with
            // protocol spam nobody typed. The STATE transition above is untouched — the wake turn is
            // real — only the Typed RECORD is filtered; the echo scan still runs first so an
            // injected prompt's pickup bookkeeping can never be skipped.
            if (msg.event == HookEvent::UserPromptSubmit && !msg.promptText.empty())
            {
                const int64_t now = NowMs();
                bool isEcho = false;
                // Newline-fold BOTH sides (DELIVERY.md RC6): a \r-composed multi-line prompt is
                // injected \n-folded (BuildPromptFill), so its echo can only match folded. Folded
                // once per event, per candidate — the scan is bounded by the Sent-unechoed set.
                const std::wstring echoFolded = FoldCrToLf(msg.promptText); // the [ups] fingerprint's input (unchanged: two deliveries of the same BYTES share a hash)
                // DELIVERY.md §15: the MATCH uses the full compare key instead — invisible characters
                // dropped + NFC on top of the fold. The prompt was pasted SANITIZED (claude's own
                // invisible-character rule, PromptFillBody) and claude submits the NFC form its editor
                // holds, so the echo of a prompt carrying a ZWSP, a stray RLM or non-canonical Hebrew
                // niqqud differs from p.text byte-wise; an exact compare left such a prompt unechoed
                // forever — the lost-send verdict's Failed + autorunner pause on a DELIVERED prompt.
                const std::wstring echoKey = NormalizePromptTextForCompare(msg.promptText);
                for (auto& p : s.queue)
                {
                    if (p.origin == PromptOrigin::Autorun && p.status == PromptStatus::Sent && !p.echoed &&
                        p.sentAtUnixMs != 0 && (now - p.sentAtUnixMs) >= 0 &&
                        (now - p.sentAtUnixMs) < kEchoWindowMs && NormalizePromptTextForCompare(p.text) == echoKey)
                    {
                        p.echoed = true; // consume exactly one echo per injected prompt
                        isEcho = true;
                        break;
                    }
                }
                // Agentmaster (DELIVERY_PLAN.md R7 — the MERGE classifier): a non-echo message that
                // ENDS WITH an awaited Sent+unechoed prompt's text is a MERGED submit — pre-existing
                // box content + our pasted prompt committed as ONE message (Incident 3's 4205-char
                // Typed row, DELIVERY.md §11). The transcript row IS the proof, so the verdict is
                // immediate (no reason to wait out the lost-send settle): the prompt is Failed (it
                // never became ITS OWN message; `echoed` stays false), the autorunner pauses (the
                // stop-on-error idiom — the queue past a mangled step is suspect), and the merged
                // message still records as the Typed row it really is. Same candidate window as the
                // echo scan.
                std::wstring mergedPromptId;
                if (!isEcho)
                {
                    for (auto& p : s.queue)
                    {
                        if (p.origin == PromptOrigin::Autorun && p.status == PromptStatus::Sent && !p.echoed &&
                            p.sentAtUnixMs != 0 && (now - p.sentAtUnixMs) >= 0 &&
                            (now - p.sentAtUnixMs) < kEchoWindowMs && PromptSwallowedByMessage(msg.promptText, p.text))
                        {
                            p.status = PromptStatus::Failed;
                            s.autorunner.mode = AutorunnerMode::Off;
                            // The INTERRUPT HOLD (DELIVERY.md §14): a backstop pause is STICKY — cancel any
                            // pending resume, or this very (merged) message would un-pause it below.
                            ClearInterruptHold(s.autorunner);
                            mergedPromptId = p.id;
                            mergeTrace = L"[merge-detected] " + ShortId(msg.sessionId) + L" prompt " + ShortId(p.id) +
                                         L" \"" + p.label + L"\" swallowed into a " + std::to_wstring(msg.promptText.size()) +
                                         L"-char message (pre-existing box content + the pasted prompt submitted as ONE - marked Failed, autorunner paused)\n";
                            break;
                        }
                    }
                }
                const bool isNoise = !isEcho && IsNoiseUserPrompt(msg.promptText);
                if (!isEcho && !isNoise)
                {
                    QueuedPrompt typed;
                    typed.id = L"typed-" + std::to_wstring(now) + L"-" + std::to_wstring(_typedSeq++);
                    typed.label = MakeLabel(msg.promptText);
                    typed.text = msg.promptText;
                    typed.status = PromptStatus::Sent;
                    typed.origin = PromptOrigin::Typed;
                    typed.echoed = true; // it IS the message; no further echo expected
                    typed.attempts = 1;
                    typed.sentAtUnixMs = now;
                    s.queue.push_back(std::move(typed));
                    TrimQueueHistory(s.queue); // bounded history: only the OLDEST completed entries drop, never queued work
                }
                // Agentmaster (the INTERRUPT HOLD — DELIVERY.md §14): a REAL message — the human's typed
                // prompt (recorded as Typed above) or the echo of a prompt they Send-now'd — is "the user's
                // next message" after an interrupt, so a parked autorunner RESUMES to the mode it held
                // (ResumeInterruptHold; while held, mode is Off, so the only sends that can produce an
                // echo are the user's own). Never on protocol noise (a teammate delivery is not the user
                // talking), never when this very message drew the MERGE verdict above (a backstop pause
                // wins — it cleared the hold), and never for a UPS whose fire time predates the marker
                // (a late-forwarded submit of the very turn the user killed).
                if (mergedPromptId.empty() && (isEcho || !isNoise))
                {
                    const AutorunnerMode resumed = ResumeInterruptHold(s.autorunner, msg.ts);
                    if (resumed != AutorunnerMode::Off)
                    {
                        holdTrace = L"[interrupt-resume] " + ShortId(msg.sessionId) + L" autorunner Off -> " + AutorunnerModeLabel(resumed) +
                                    (isEcho ? L" (your message arrived - the Send-now echo)\n" : L" (your message arrived)\n");
                    }
                }
                upsTrace = L"[ups] " + ShortId(msg.sessionId) + L" chars=" + std::to_wstring(msg.promptText.size()) +
                           L" hash=" + FoldedPromptHash(echoFolded) +
                           (isEcho ? L" -> echo consumed" :
                                     (isNoise ? L" -> noise (not recorded)" :
                                                (!mergedPromptId.empty() ? L" -> recorded as Typed (MERGED - swallowed prompt " + ShortId(mergedPromptId) + L")" :
                                                                           L" -> recorded as Typed"))) +
                           L"\n";
            }
            else if (msg.event == HookEvent::UserPromptSubmit)
            {
                // Agentmaster (DELIVERY_PLAN.md R3 — instrument, don't guess): an EMPTY-prompt
                // UserPromptSubmit. The live phantom twins (DELIVERY.md §10) were exactly this shape —
                // matched no transcript message, recorded no row — and the ordered machine now keeps
                // them out of the turn accounting (no lastPromptUnixMs stamp, no type-ahead count;
                // HookEvents.h). The scanner's recon-run synth also rides this branch by design.
                upsTrace = L"[ups] " + ShortId(msg.sessionId) + L" chars=0 -> EMPTY (state-only; no stamp, no type-ahead, no record)\n";
            }

            snapshot = s;
            found = true;
            // Only a clean turn-complete advances the Auto Testing (Correctness Rule #1). The
            // ordered machine narrows this further: a Stop consumed by a queued type-ahead
            // prompt stays Running (the next turn is already starting — injecting now would
            // interleave), and a STALE Stop must not re-fire an advance for a turn that
            // already advanced.
            triggerAdvance = ordered.turnComplete;
        }

        if (!upsTrace.empty())
        {
            // DELIVERY_PLAN.md R3 (instrument first): one line per UserPromptSubmit naming its
            // disposition — echo-consumed / recorded / noise / EMPTY — plus size + a folded-text
            // fingerprint, so a phantom/duplicate UPS is self-evident from hooks.log (two deliveries
            // of one message share a hash; the empty twins read chars=0). Logged outside the lock.
            AppendStateLog(L"hooks.log", upsTrace);
        }
        if (!mergeTrace.empty())
        {
            // R7: the merge verdict goes to BOTH logs (the lost-send precedent) — it is an
            // autorunner-pausing event AND a delivery forensic.
            AppendStateLog(L"hooks.log", mergeTrace);
            AppendStateLog(L"autorunner.log", mergeTrace);
        }
        if (!holdTrace.empty())
        {
            // §14: an autorunner MODE change — both logs, like the [interrupt-hold] that armed it.
            AppendStateLog(L"hooks.log", holdTrace);
            AppendStateLog(L"autorunner.log", holdTrace);
        }
        if (!lineageTrace.empty())
        {
            // The summary's conversation chain: which session a /clear continued (the link the summary's
            // history then lists). Logged outside the lock, like every trace above.
            AppendStateLog(L"hooks.log", lineageTrace);
        }
        if (found)
        {
            _notify(snapshot, msg.event);
        }
        // Agentmaster: fire the bind/adoption handler on EVERY SessionStart — not only when this
        // call CREATED the record — so the app layer can reconcile the tab<->session binding by the
        // STABLE WT_SESSION tabToken. This covers (a) adopting a hand-typed `+`-tab claude (a new,
        // unknown id), AND (b) RE-HOMING a tab whose claude switched conversation id via the
        // in-session `/resume` (the session id changes; the ConPTY / tabToken does not — and the new
        // id may even be a previously-known/archived one, which never created a record here). The
        // reconcile is idempotent: a SessionStart for an already-bound session fast-returns.
        if (msg.event == HookEvent::SessionStart)
        {
            // Fan out to every window's bind handler (M9: the registry is a process-wide singleton).
            // Snapshot under the lock, invoke outside it; whichever window hosts this session's tab
            // binds / re-homes it, the rest no-op.
            std::vector<AdoptionHandler> adopters;
            {
                std::lock_guard guard{ _mtx };
                adopters.reserve(_adopters.size());
                for (const auto& a : _adopters)
                {
                    adopters.push_back(a.second);
                }
            }
            for (auto& adopt : adopters)
            {
                if (adopt)
                {
                    try
                    {
                        adopt(msg.sessionId, msg.cwd, msg.tabToken);
                    }
                    catch (...)
                    {
                        // Forensics: the hook-push adoption fan-out. A throw here means a hand-typed
                        // claude never gets bound to its tab (no injector => no Send-now/autorunner).
                        LogSwallowedException(L"SessionRegistry::OnHookEvent adopt");
                    }
                }
            }
        }
        if (triggerAdvance && _advance)
        {
            try
            {
                _advance(msg.sessionId);
            }
            catch (...)
            {
                // Forensics: the turn-complete -> Tests Autorunner advance seam. A throw here stalls
                // the queue for that session with no visible cause (it just never sends the next one).
                LogSwallowedException(L"SessionRegistry::OnHookEvent advance");
            }
        }
        else if (triggerAdvance)
        {
            // Agentmaster (no-silent-inertness): a clean turn-complete landed but NO advance handler
            // is wired — this process runs a registry with no scheduler behind it. Since 2026-10-09
            // that is never the app (Engine.cpp wires SetAdvanceHandler in every build); it is a host
            // that links the engine without SharedEngine() — the test harness, the CLI. Until then it
            // was every release build without Debug Mode / --debug, and that is where this breadcrumb
            // came from: a session that actually HAS queued work sat Pending forever with ZERO
            // per-session trace — the "queued while Running, stuck Pending, never sent" report:
            // queued in a --debug run, the app relaunched without the flag, and every later
            // turn-complete was dropped right here silently. Leave a breadcrumb ONLY when there is
            // Pending/Held work (a session with an empty queue — the normal case — still logs nothing).
            size_t pendingWork = 0;
            for (const auto& p : snapshot.queue)
            {
                if (p.status == PromptStatus::Pending || p.status == PromptStatus::Held)
                {
                    ++pendingWork;
                }
            }
            if (pendingWork > 0)
            {
                AppendStateLog(L"hooks.log",
                               L"[advance-dropped] " + msg.sessionId + L" pending=" + std::to_wstring(pendingWork) +
                                   L" (turn-complete, but no advance handler is wired in this process - the prompt queue runner is not running here, so the queue cannot auto-send)\n");
            }
        }
    }

    void SessionRegistry::ObserveClaude(const ObservedClaude& o)
    {
        if (o.sessionId.empty())
        {
            return; // a correlated-but-never-prompted claude has no conversation id yet (§11d)
        }
        // Agentmaster (RETIRED HOSTS — RetireHostToken; the 2026-08-26 deactivate unbind): an observation
        // of the claude a Deactivate/Restart just CLOSED. Until the hosting window republishes its tab
        // roster (its next ~2.5 s probe), the observer's full survey — every 2 s — still finds that dying
        // process under the OLD shell of the OLD roster entry and reports it for this session with the OLD
        // token. Merging it would put tabToken / pid / presenceStatus of a dead process back on a record
        // that now names the new connection: the liveness sweep then finds no pane holding that token and
        // archives the session, and a stale "busy" heartbeat could keep the scanner reading it as working.
        // Drop the whole observation; log it once per retired token (the survey repeats until the process
        // is gone).
        if (!o.tabToken.empty())
        {
            bool fromRetiredHost = false;
            bool logDrop = false;
            {
                std::lock_guard guard{ _mtx };
                if (const auto* retired = _FindRetiredHostTokenLocked(o.sessionId, o.tabToken, NowMs()))
                {
                    fromRetiredHost = true;
                    logDrop = !retired->observeDropLogged;
                    retired->observeDropLogged = true; // mutable latch, under _mtx
                }
            }
            if (fromRetiredHost)
            {
                if (logDrop)
                {
                    AppendStateLog(L"hooks.log", L"[retired-host] dropped the observation of pid " + std::to_wstring(o.pid) + L" for " + o.sessionId + L" on replaced ConPTY " + o.tabToken + L" (its closing claude, seen through the not-yet-republished roster)\n");
                }
                return;
            }
        }
        SessionInfo snapshot;
        bool created = false;
        bool changed = false;
        std::vector<SessionInfo> superseded; // stale prior conversations of the same claude on this ConPTY (notified outside the lock)
        {
            std::lock_guard guard{ _mtx };
            // Agentmaster (RETIRED HOSTS): the race belt for the pre-check above (a retirement that landed
            // between that check and this lock). Silent — the pre-check logs the ordinary drops.
            if (!o.tabToken.empty() && _FindRetiredHostTokenLocked(o.sessionId, o.tabToken, NowMs()) != nullptr)
            {
                return;
            }
            auto it = _sessions.find(o.sessionId);
            if (it == _sessions.end())
            {
                // First sight of a claude we did NOT Launch (a Manager-Launched one is already
                // registered, so it'd be found). Mirror the hook SessionStart creation: minimal
                // external + live record. ObserveClaude NEVER sets SessionState — leave it Idle
                // (the default); push hooks + the transcript tail own state (Rule #1/#7).
                SessionInfo s;
                s.id = o.sessionId;
                s.workingDir = o.cwd;
                s.state = SessionState::Idle;
                s.external = true;
                s.live = true;
                // Agentmaster (adopt/re-home autorunner default — SetDefaultAutorunnerMode): mirror
                // the hook SessionStart creation above — a first-sight observed claude is a newly-
                // OPENED session, so it gets the cog default MODE (it used to default Off, silently
                // exempting observer-adopted sessions from the Tests Autorunner). Config at CREATION
                // only — enrichment merges below never touch autorunner state, and ObserveClaude
                // still NEVER sets SessionState.
                s.autorunner.mode = _defaultAutorunnerMode;
                // The STATE SETTLE clock (DELIVERY.md §16): the hook-adopt twin — a first-sight session's
                // current status begins now.
                s.stateChangedAtUnixMs = NowMs();
                _order.push_back(o.sessionId);
                it = _sessions.emplace(o.sessionId, std::move(s)).first;
                created = true;
            }
            auto& s = it->second;
            const auto prevPid = s.pid; // Agentmaster: the pid we last knew, captured BEFORE the enrichment below overwrites it
            // ObserveClaude never sets SessionState, but it can REVIVE an archived record (live=false ->
            // true, below) — a settle fact, so the clock restarts then (DELIVERY.md §16).
            const auto settleBefore = CaptureSettleFacts(s);

            // Enrichment merge (facts, never state). Track whether anything MEANINGFUL changed so a
            // steady-state re-observe is a no-op (no observer / persist / UI churn each heartbeat).
            const auto assign = [&changed](auto& field, const auto& value) {
                if (field != value)
                {
                    field = value;
                    changed = true;
                }
            };
            if (!o.tabToken.empty())
            {
                assign(s.tabToken, o.tabToken); // the correlation key; never clobber with empty
            }
            assign(s.amSession, o.amSession);
            if (!o.ownerWindowId.empty())
            {
                assign(s.ownerWindowId, o.ownerWindowId); // never clobber a known owner with empty
            }
            assign(s.runningApp, o.runningApp);
            assign(s.pid, o.pid);
            assign(s.liveCwd, o.cwd);
            assign(s.model, o.model);
            assign(s.effort, o.effort);
            if (!o.gitBranch.empty())
            {
                assign(s.branch, o.gitBranch); // Agentmaster: live writer for the git branch (the
                // per-tab overlay's "<workdir folder>/<branch>" row); never clobber a known branch
                // with empty (transcript momentarily unresolved / no branch recorded yet).
            }
            assign(s.permissionMode, o.permissionMode);
            assign(s.background, o.background);
            assign(s.sessionName, o.sessionName);
            // Presence heartbeat (busy/idle/waiting/shell) — a display FACT, never SessionState
            // (Rule #13). A flip notifies (turn-cadence, not per-survey noise) so a hook-less
            // session's status chip updates live.
            assign(s.presenceStatus, o.presenceStatus);
            // The "waiting" detail rides the same flip. Deliberately assigned UNCONDITIONALLY
            // (assign writes empties): it must CLEAR the moment claude leaves "waiting", or a
            // stale "input needed" would outlive the question it described.
            assign(s.presenceWaitingFor, o.presenceWaitingFor);
            // Agentmaster: do NOT bounce a session we INTENTIONALLY archived back to live while
            // its claude.exe is still winding down. A tab-close / window-teardown archive flips live=false,
            // but the process lingers (and may still sit in a window's published roster) for up to ~1
            // survey, so the very next ObserveClaude would correlate the DYING process and revive a
            // phantom live card until it finally exits. The lingering process keeps its pid, so revive
            // only on a genuinely DIFFERENT pid (a real re-run of this conversation) — never the same
            // (dying) one. Facts are still enriched above either way; the no-tab archive branch
            // (_ArchiveClaudeSession) additionally ProcessAlive-guards. (A first-sight external claude is
            // created live in the branch above — this gate only governs reviving an EXISTING !live record.)
            if (!s.live)
            {
                if (o.pid != 0 && o.pid != prevPid)
                {
                    s.live = true; // an observed claude on a NEW pid is genuinely running (Rule #7)
                    changed = true;
                }
            }
            StampSettleClock(s, settleBefore, NowMs()); // a revival above restarts the settle clock; nothing else here moves a settle fact
            if (s.workingDir.empty() && !o.cwd.empty())
            {
                s.workingDir = o.cwd; // the persisted M-axis; liveCwd alone tracks a live `cd`
                changed = true;
            }
            // Provenance timestamp: always refreshed, but it alone is NOT a "change" (it must not
            // trigger the observer/persist cascade every heartbeat).
            s.lastObservedUnixMs = o.observedUnixMs;
            // Transcript timing (conversation age + last activity). Refreshed silently like
            // lastObservedUnixMs — NOT a "change" (mtime ticks on every write; the Manager computes
            // the "ago" live from these each refresh, so they need not drive the cascade). Never
            // clobber a known value with 0 (transcript momentarily unresolved).
            if (o.createdUnixMs != 0)
            {
                s.convCreatedUnixMs = o.createdUnixMs;
            }
            if (o.lastActivityUnixMs != 0)
            {
                s.convLastActivityUnixMs = o.lastActivityUnixMs;
            }
            if (o.apiActivityUnixMs != 0)
            {
                // The ⚡ half (ServerCacheStillWarm): the PARENT conversation's own last line — no
                // subagent/teammate side-file fold, no mtime fallback (see convApiActivityUnixMs).
                s.convApiActivityUnixMs = o.apiActivityUnixMs;
            }
            // Agentmaster (MOST RECENT sort): the transcript-tail TURN BOUNDARIES — the PULL half of the
            // turnStart/turnEnd stamps (OnHookEvent is the push half, NoteExternalPrompt the pull-echo).
            // Monotonic MAX, never a plain assign: a hook's wire ts can be newer than the last tail read
            // (the survey lags ~2s), and a tail window that missed the prompt line reads 0 — neither may
            // regress the other. Silent like the rest of the transcript timing (not a "change" — no
            // observer/persist cascade per survey): the order is re-read at the next rebuild, and the
            // value rides the next real change into sessions.json (a Stop / archive always follows).
            if (o.turnStartUnixMs > s.turnStartUnixMs)
            {
                s.turnStartUnixMs = o.turnStartUnixMs;
            }
            if (o.turnEndUnixMs > s.turnEndUnixMs)
            {
                s.turnEndUnixMs = o.turnEndUnixMs;
            }

            // One ConPTY = one live conversation (Rule #14 / session-id divergence): this observed claude
            // (pid o.pid) is the CURRENT conversation on its tabToken, resolved from claude's own pid-keyed
            // presence heartbeat. Archive any STALE prior conversation of the SAME process still flagged
            // live on that ConPTY, so the tab reconciler binds exactly one session (the newest) instead of
            // churning between divergent ids and flickering the tab title. Only a live winner supersedes;
            // pid + freshness guards protect a sibling sub-claude / the genuinely-current conversation.
            if (s.live && !s.tabToken.empty())
            {
                superseded = _SupersedeStaleTabSiblings(o.sessionId, s.tabToken, s.pid, TabSessionFreshness(s));
            }

            changed = changed || created;
            snapshot = s;
        }

        if (created)
        {
            // Fire the bind/adoption handlers (outside the lock) so the app correlates the tabToken
            // (WT_SESSION) to its ConPTY and binds an injector — the same promotion path as a hook
            // SessionStart. Fan out to every window (M9); whichever hosts this tab binds it, the rest
            // no-op. Only on creation: a steady-state re-observe must not re-fire adoption.
            std::vector<AdoptionHandler> adopters;
            {
                std::lock_guard guard{ _mtx };
                adopters.reserve(_adopters.size());
                for (const auto& a : _adopters)
                {
                    adopters.push_back(a.second);
                }
            }
            for (auto& adopt : adopters)
            {
                if (adopt)
                {
                    try
                    {
                        adopt(o.sessionId, o.cwd, o.tabToken);
                    }
                    catch (...)
                    {
                        // Forensics: the PULL (Fleet Observer) adoption fan-out — the always-correct
                        // floor beneath the lossy hook push. A throw here is the no-hooks claude's
                        // LAST chance to be bound, so losing it silently loses the session entirely.
                        LogSwallowedException(L"SessionRegistry::ObserveClaude adopt");
                    }
                }
            }
        }
        if (changed)
        {
            _notify(snapshot, HookEvent::Unknown);
        }
        // Each stale sibling archived above (live=false): notify so its hosting window drops the live
        // card / hides the tab-strip dot and persistence saves it as Archived (restorable). Outside the
        // lock, like every other _notify.
        for (const auto& gone : superseded)
        {
            _notify(gone, HookEvent::Unknown);
        }
    }

    // Agentmaster (RETIRED HOSTS): see the declaration. Pruning happens here, on the write path, so the read
    // path (_FindRetiredHostTokenLocked, hit by every hook and every observation) never allocates.
    void SessionRegistry::RetireHostToken(const std::wstring& sessionId, const std::wstring& tabToken)
    {
        if (sessionId.empty() || tabToken.empty())
        {
            return; // nothing names a host — nothing to retire
        }
        const int64_t now = NowMs();
        {
            std::lock_guard guard{ _mtx };
            auto& retired = _retiredHostTokens[sessionId];
            // Drop the expired entries, and any earlier entry for this same token: re-retiring a token (a
            // second park of an already-parked host is refused upstream, but a restart racing a park is
            // not) refreshes its clock instead of stacking a duplicate.
            retired.erase(std::remove_if(retired.begin(), retired.end(), [&](const RetiredHostToken& r) {
                              return now - r.retiredAtUnixMs >= kRetiredHostTokenTtlMs || TabTokenEq(r.token, tabToken);
                          }),
                          retired.end());
            retired.push_back(RetiredHostToken{ tabToken, now, false });
            // Bound the list: the oldest retirement goes first (its dying process is the longest gone).
            while (retired.size() > kRetiredHostTokensPerSession)
            {
                retired.erase(retired.begin());
            }
        }
        AppendStateLog(L"hooks.log", L"[retired-host] " + sessionId + L" no longer hosted by ConPTY " + tabToken + L" \x2014 its closing claude's observations and hooks are ignored for " + std::to_wstring(kRetiredHostTokenTtlMs / 60000) + L" min\n");
    }

    bool SessionRegistry::IsRetiredHostToken(const std::wstring& sessionId, const std::wstring& tabToken) const
    {
        std::lock_guard guard{ _mtx };
        return _FindRetiredHostTokenLocked(sessionId, tabToken, NowMs()) != nullptr;
    }

    // Agentmaster (RETIRED HOSTS): see the declaration. A linear scan of at most
    // kRetiredHostTokensPerSession entries for the one session — the per-hook cost is a map miss for every
    // session that was never parked or restarted.
    const SessionRegistry::RetiredHostToken* SessionRegistry::_FindRetiredHostTokenLocked(const std::wstring& sessionId, const std::wstring& tabToken, int64_t nowUnixMs) const noexcept
    {
        if (tabToken.empty())
        {
            return nullptr; // a token-less hook / observation names no host, so no retirement can apply
        }
        const auto it = _retiredHostTokens.find(sessionId);
        if (it == _retiredHostTokens.end())
        {
            return nullptr;
        }
        for (const auto& r : it->second)
        {
            if (nowUnixMs - r.retiredAtUnixMs < kRetiredHostTokenTtlMs && TabTokenEq(r.token, tabToken))
            {
                return &r;
            }
        }
        return nullptr; // expired entries are ignored here and pruned on the next RetireHostToken
    }

    bool SessionRegistry::Update(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate)
    {
        SessionInfo snapshot;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return false;
            }
            // Agentmaster (the STATE SETTLE hold — DELIVERY.md §16): the generic mutator is where every
            // UI triage move (Move to Waiting-for-you / Idle / Done), the scanner's Waiting -> Idle decay,
            // an archive, the Codex reconcile AND every queue insertion land — so the registry stamps the
            // settle clocks here, after the opaque mutate, instead of trusting each caller to remember.
            const auto settleBefore = CaptureSettleFacts(it->second);
            mutate(it->second);
            const int64_t settleNow = NowMs();
            StampSettleClock(it->second, settleBefore, settleNow);
            StampNewlyQueuedPrompts(it->second.queue, settleNow);
            snapshot = it->second;
        }
        _notify(snapshot, HookEvent::Unknown);
        return true;
    }

    void SessionRegistry::UpdateQuiet(const std::wstring& id, const std::function<void(SessionInfo&)>& mutate)
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end())
        {
            return;
        }
        // Same settle bookkeeping as Update (DELIVERY.md §16) — today's quiet callers touch no settle fact
        // and queue nothing, so this is a cheap no-op, but a quiet path that ever did must not slip past
        // the hold. Stamping a field is not a notify, so the quiet contract below is unchanged.
        const auto settleBefore = CaptureSettleFacts(it->second);
        mutate(it->second);
        const int64_t settleNow = NowMs();
        StampSettleClock(it->second, settleBefore, settleNow);
        StampNewlyQueuedPrompts(it->second.queue, settleNow);
        // Deliberately NO _notify: this path exists precisely to avoid the persist / UI / advance
        // cascade for transient, high-frequency fields (e.g. streamed assistant text).
    }

    void SessionRegistry::SetStarted(const std::wstring& id, bool started)
    {
        SessionInfo snapshot;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end() || it->second.started == started)
            {
                return; // unknown id, or no change -> no observer churn on the re-assert tick
            }
            // A dormant tab whose claude just launched (or a tab just parked by Deactivate) changed a
            // settle fact: restart the settle clock (DELIVERY.md §16), so a freshly activated session's
            // startup churn settles before its queue moves.
            const auto settleBefore = CaptureSettleFacts(it->second);
            it->second.started = started;
            StampSettleClock(it->second, settleBefore, NowMs());
            snapshot = it->second;
        }
        _notify(snapshot, HookEvent::Unknown);
    }

    // Agentmaster (PENDING_INPUT.md): record the UNSENT input-box draft. The field is always updated
    // (change-gated), but a _notify fires ONLY on the BOOLEAN hasPending FLIP — empty<->non-empty —
    // which is exactly "the observer can reliably say 'yes pending' then 'no pending'". This matches the
    // presence heartbeat's "notify on a flip, not per-survey" cadence: a draft moves as the user types,
    // so a per-keystroke notify would needlessly run the persist / board-rebuild / scheduler cascade,
    // but the appear/clear TRANSITIONS are infrequent (turn-cadence) and are what the tab-strip + board
    // animations key on. A text-only edit (still non-empty) updates the field QUIETLY. Returns true iff
    // the boolean flipped (== whether it notified), so the caller logs/acts exactly on a transition.
    // The UI-lane caller (TerminalPage::_ScanPendingInput) debounces the CLEAR so a single mis-read of a
    // mid-repaint frame can't produce a spurious flip. No-op for an unknown id. Thread-safe; notify is
    // raised OUTSIDE the lock (observers may re-enter), like every other _notify.
    bool SessionRegistry::SetPendingInput(const std::wstring& id, const std::wstring& text)
    {
        SessionInfo snapshot;
        bool flipped = false;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return false; // unknown id
            }
            if (it->second.pendingInput == text)
            {
                // No text change — but a non-empty re-observation still REFRESHES the observation
                // stamp (quietly): "now - pendingInputUnixMs stays within a few ticks" is exactly how
                // consumers tell a LIVE draft from a carried MEMORY (a restored/dormant session whose
                // stamp froze at the last real read). PENDING_INPUT.md §5.
                if (!text.empty())
                {
                    it->second.pendingInputUnixMs = NowMs();
                }
                return false;
            }
            const bool wasPending = !it->second.pendingInput.empty();
            const bool nowPending = !text.empty();
            it->second.pendingInput = text;
            it->second.pendingInputUnixMs = nowPending ? NowMs() : 0;
            if (!nowPending)
            {
                it->second.pendingPasteRefs.clear(); // the draft is gone — its paste annotation goes with it
            }
            flipped = (wasPending != nowPending);
            if (flipped)
            {
                snapshot = it->second; // capture for the notify; a text-only edit skips this (stays quiet)
            }
        }
        if (flipped)
        {
            _notify(snapshot, HookEvent::Unknown);
        }
        return flipped;
    }

    // Agentmaster (PENDING_INPUT.md §2b): record the paste-cache resolver's verdict for the current
    // draft. QUIET by design (no _notify): the annotation is display-only and always rides a draft the
    // flip notify already announced; resolving happens off-thread AFTER the draft was committed, so a
    // notify here would just re-run the persist/board cascade for a tooltip line. Change-gated. No-op
    // for an unknown id or a session whose draft has meanwhile cleared (a late resolve must not
    // resurrect an annotation for a sent message).
    void SessionRegistry::SetPendingPasteRefs(const std::wstring& id, const std::wstring& refs)
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end() || it->second.pendingInput.empty() || it->second.pendingPasteRefs == refs)
        {
            return;
        }
        it->second.pendingPasteRefs = refs;
    }

    // Agentmaster (PENDING_INPUT.md §2c — the PASTE JOURNAL): see the header. The caps are enforced in a
    // fixed order so every exit leaves the per-session AND the fleet-wide invariants true: normalize +
    // filter (inline pastes never back a placeholder; a giant one is refused outright), de-dupe the newest
    // entry, append, trim this session (count, then chars), then evict fleet-wide oldest-first. Quiet —
    // a journal is resolver input, not observable state, so nothing notifies.
    void SessionRegistry::NotePastedText(const std::wstring& id, const std::wstring& rawText, int64_t unixMs)
    {
        std::wstring text = NormalizePastedTextLikeClaude(rawText);
        if (text.empty() || text.size() > kPasteJournalMaxEntryChars || !ClaudeWouldCollapsePaste(text))
        {
            return; // nothing claude could render as a placeholder, or too big to hold on to
        }
        const int64_t stamp = unixMs != 0 ? unixMs : NowMs();
        std::lock_guard guard{ _mtx };
        if (_sessions.find(id) == _sessions.end())
        {
            return; // an unknown session — its pastes would be unreachable (and never pruned)
        }
        auto& journal = _pasteJournals[id];
        if (!journal.empty() && journal.back().text == text)
        {
            journal.back().unixMs = stamp; // an identical re-paste: one entry, refreshed (resolution is content-equal either way)
            return;
        }
        _pasteJournalChars += text.size();
        journal.push_back(PasteJournalEntry{ std::move(text), stamp });
        // This session's caps — oldest first (placeholders resolve against the NEWEST same-length pastes).
        size_t sessionChars = 0;
        for (const auto& e : journal)
        {
            sessionChars += e.text.size();
        }
        while (journal.size() > 1 && (journal.size() > kPasteJournalMaxEntriesPerSession || sessionChars > kPasteJournalMaxSessionChars))
        {
            sessionChars -= journal.front().text.size();
            _pasteJournalChars -= (std::min)(_pasteJournalChars, journal.front().text.size());
            journal.erase(journal.begin());
        }
        // The fleet-wide cap: evict the globally OLDEST entry (each journal is time-ordered, so the
        // candidates are the fronts) until under — but never the entry just recorded.
        while (_pasteJournalChars > kPasteJournalMaxTotalChars)
        {
            std::vector<PasteJournalEntry>* oldest = nullptr;
            for (auto& [sid, entries] : _pasteJournals)
            {
                const bool isNewest = (&entries == &journal) && entries.size() == 1;
                if (!entries.empty() && !isNewest && (!oldest || entries.front().unixMs < oldest->front().unixMs))
                {
                    oldest = &entries;
                }
            }
            if (!oldest)
            {
                break; // only the just-recorded entry remains — it alone is under the per-entry cap
            }
            _pasteJournalChars -= (std::min)(_pasteJournalChars, oldest->front().text.size());
            oldest->erase(oldest->begin());
        }
    }

    std::vector<PasteJournalEntry> SessionRegistry::PasteJournal(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _pasteJournals.find(id);
        return it != _pasteJournals.end() ? it->second : std::vector<PasteJournalEntry>{};
    }

    void SessionRegistry::ClearPasteJournal(const std::wstring& id)
    {
        std::lock_guard guard{ _mtx };
        const auto it = _pasteJournals.find(id);
        if (it == _pasteJournals.end())
        {
            return;
        }
        for (const auto& e : it->second)
        {
            _pasteJournalChars -= (std::min)(_pasteJournalChars, e.text.size());
        }
        _pasteJournals.erase(it);
    }

    void SessionRegistry::MovePasteJournal(const std::wstring& fromId, const std::wstring& toId)
    {
        if (fromId == toId)
        {
            return;
        }
        std::lock_guard guard{ _mtx };
        const auto it = _pasteJournals.find(fromId);
        if (it == _pasteJournals.end())
        {
            return;
        }
        auto moved = std::move(it->second);
        _pasteJournals.erase(it);
        auto& target = _pasteJournals[toId];
        for (auto& e : moved)
        {
            target.push_back(std::move(e)); // the fleet total is unchanged — the chars only changed owner
        }
        // Re-apply the per-session count cap on the merged journal (chars follow the same entries).
        while (target.size() > kPasteJournalMaxEntriesPerSession)
        {
            _pasteJournalChars -= (std::min)(_pasteJournalChars, target.front().text.size());
            target.erase(target.begin());
        }
    }

    // Agentmaster (DELIVERY_PLAN.md R5 — the tri-state box read): record the box's observed STATE.
    // Change-gated + QUIET, except the BLOCKED→UNBLOCKED release (NoBox/MenuOpen → anything else),
    // which notifies — that release is what re-fires an advance DecideAdvance held on the box (the
    // delivery gate's close-notify idiom; entering the blocked set needs no notify, holds happen at
    // decide time). The change stamp anchors the scanner's ShouldWarnOnBoxNotVisible warning
    // (and is RE-STAMPED by an explicit autorunner re-arm - DELIVERY.md §12).
    void SessionRegistry::SetPendingBoxState(const std::wstring& id, InputBoxState state)
    {
        SessionInfo snapshot;
        bool release = false;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end() || it->second.pendingBoxState == state)
            {
                return;
            }
            const auto blocked = [](InputBoxState v) noexcept {
                return v == InputBoxState::NoBox || v == InputBoxState::MenuOpen;
            };
            release = blocked(it->second.pendingBoxState) && !blocked(state);
            it->second.pendingBoxState = state;
            it->second.pendingBoxStateUnixMs = NowMs();
            if (release)
            {
                snapshot = it->second;
            }
        }
        if (release)
        {
            _notify(snapshot, HookEvent::Unknown);
        }
    }

    void SessionRegistry::NoteExternalPrompt(const std::wstring& id, const std::wstring& text, int64_t observedUnixMs)
    {
        if (text.empty())
        {
            return;
        }
        SessionInfo snapshot;
        bool changed = false;
        std::wstring pullEchoTrace; // DELIVERY_PLAN.md R1: filled under the lock, logged after it
        std::wstring mergeTrace; // DELIVERY_PLAN.md R7: the pull-side [merge-detected] line
        std::wstring holdTrace; // DELIVERY.md §14: the pull-side [interrupt-resume] line
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return;
            }
            auto& s = it->second;
            // Agentmaster (MOST RECENT sort): a REAL human prompt line (the scanner calls this only for
            // non-noise UserPrompt events) is a turn START — stamp it with the line's own timestamp, the
            // pull twin of OnHookEvent's UserPromptSubmit stamp that a hookless/adopted session never
            // gets, and instant where the observer's tail read lags a survey. Monotonic; bookkeeping-
            // QUIET like the echo consume below (the Typed record / the scanner's state synth notify).
            if (observedUnixMs > s.turnStartUnixMs)
            {
                s.turnStartUnixMs = observedUnixMs;
            }
            // Idempotent: a message already recorded (status Sent — a Typed capture or an injected
            // Flight prompt's echo, which also shows up as a user line in the transcript) must not
            // be duplicated. Pending plan items are NOT "recorded messages", so they don't suppress
            // recording a human message that happens to match a queued prompt's text. Newline-fold
            // both sides (DELIVERY.md RC6) — the transcript text carries \n while a compose-queued
            // prompt's record may carry \r, the same mismatch the echo consume folds for.
            //
            // Agentmaster (DELIVERY_PLAN.md R1 — the PULL echo-consume): a fold-matched Sent+unechoed
            // Autorun prompt is not merely deduped — this transcript user line IS the proof our
            // injection became a message, so mark it `echoed`. That unifies delivery evidence across
            // hooked AND no-hook sessions ("echoed" = it became a message, fed by the push hook OR by
            // this), which is what the pickup guard, the Enter-retry watchdog, and the lost-send
            // verdict (DecideLostSend) all key on. UNLIKE the push consume, there is deliberately NO
            // recency window on the consume: the line's OWN timestamp (observedUnixMs, when the caller
            // has one) is the staleness filter — a replayed OLD identical line (ts before the send)
            // must not vouch for a NEW send, but a scanner that reads the line late (write-lag, a
            // catch-up pass) must still consume it, or the lost-send verdict would fire on a
            // demonstrably-delivered prompt. observedUnixMs==0 (no per-line ts) trusts the match.
            //
            // DELIVERY.md §15: the compare is the full compare KEY (NormalizePromptTextForCompare —
            // the RC6 newline fold plus invisible characters dropped plus NFC). The transcript line
            // is claude's cleaned, NFC submit while an Autorun p.text is our raw queued text, and a
            // push-recorded Typed row is claude's text again — so the key is what lets the pull-echo
            // vouch for a sanitized delivery AND lets a Hebrew prompt's pull twin dedupe against its
            // push row instead of recording it twice.
            const std::wstring textFolded = NormalizePromptTextForCompare(text);
            bool alreadyRecorded = false;
            for (auto& p : s.queue)
            {
                if (p.status != PromptStatus::Sent || NormalizePromptTextForCompare(p.text) != textFolded)
                {
                    continue;
                }
                alreadyRecorded = true;
                if (p.origin == PromptOrigin::Autorun && !p.echoed &&
                    (observedUnixMs == 0 || p.sentAtUnixMs == 0 || observedUnixMs >= p.sentAtUnixMs))
                {
                    p.echoed = true; // the transcript confirmed the send became a message (pull evidence)
                    pullEchoTrace = L"[pull-echo] " + ShortId(id) + L" prompt " + ShortId(p.id) +
                                    L" (transcript confirmed the delivered prompt became a message)\n";
                    break; // consume at most one — mirrors the push consume's one-echo-per-injection rule
                }
            }
            // Agentmaster (DELIVERY_PLAN.md R7 — the merge classifier's PULL twin): a transcript user
            // line that fold-matched nothing but ENDS WITH an awaited Sent+unechoed prompt is the
            // MERGED submit observed from the pull side (a no-hook session, or a push delivery whose
            // payload was dropped). Same verdict as the push side: the prompt is Failed, the
            // autorunner pauses, and the merged line still records below as the Typed row it is.
            // The line's own timestamp is the staleness filter (a replayed OLD line never vouches).
            if (!alreadyRecorded)
            {
                for (auto& p : s.queue)
                {
                    if (p.origin == PromptOrigin::Autorun && p.status == PromptStatus::Sent && !p.echoed &&
                        p.sentAtUnixMs != 0 &&
                        (observedUnixMs == 0 || observedUnixMs >= p.sentAtUnixMs) &&
                        PromptSwallowedByMessage(text, p.text))
                    {
                        p.status = PromptStatus::Failed;
                        s.autorunner.mode = AutorunnerMode::Off;
                        // The INTERRUPT HOLD (DELIVERY.md §14): a backstop pause is STICKY — cancel any
                        // pending resume, or this very (merged) line would un-pause it below.
                        ClearInterruptHold(s.autorunner);
                        mergeTrace = L"[merge-detected] " + ShortId(id) + L" prompt " + ShortId(p.id) +
                                     L" \"" + p.label + L"\" swallowed into a " + std::to_wstring(text.size()) +
                                     L"-char transcript message (pull side - marked Failed, autorunner paused)\n";
                        break;
                    }
                }
            }
            if (!alreadyRecorded)
            {
                // Deduped paths fall through with changed=false — never a duplicate Typed row (and the
                // consume above is bookkeeping-QUIET: no notify, matching the push consume; the state
                // ride-along and the turn boundary come from the scanner's own synths, not from here).
                const int64_t now = NowMs();
                QueuedPrompt typed;
                typed.id = L"recon-" + std::to_wstring(now) + L"-" + std::to_wstring(_typedSeq++);
                typed.label = MakeLabel(text);
                typed.text = text;
                typed.status = PromptStatus::Sent;
                typed.origin = PromptOrigin::Typed;
                typed.echoed = true; // it IS the message; no further echo expected
                typed.attempts = 1;
                typed.sentAtUnixMs = now;
                s.queue.push_back(std::move(typed));
                TrimQueueHistory(s.queue); // bounded history: only the OLDEST completed entries drop, never queued work
                snapshot = s;
                changed = true;
            }
            // Agentmaster (the INTERRUPT HOLD — DELIVERY.md §14): the PULL twin of the push resume. This
            // non-noise user line is "the user's next message" when its OWN transcript timestamp
            // postdates the interrupt marker — the anchor ResumeInterruptHold checks; an unstamped line
            // (observedUnixMs == 0) cannot prove that and never resumes (on a hooked session the push
            // hook resumes; a hookless session with unstamped lines waits for the user's toggle). A merge
            // verdict above wins (it cleared the hold). Runs for a DEDUPED line too (the push already
            // recorded it — a hooked session's normal shape) — idempotent, since a resumed hold is gone.
            if (mergeTrace.empty() && observedUnixMs != 0)
            {
                const AutorunnerMode resumed = ResumeInterruptHold(s.autorunner, observedUnixMs);
                if (resumed != AutorunnerMode::Off)
                {
                    holdTrace = L"[interrupt-resume] " + ShortId(id) + L" autorunner Off -> " + AutorunnerModeLabel(resumed) +
                                L" (your message arrived - transcript line)\n";
                    snapshot = s;
                    changed = true; // a MODE change: the UI toggles repaint + the scheduler's OnObserved sees it
                }
            }
        }
        if (!pullEchoTrace.empty())
        {
            AppendStateLog(L"hooks.log", pullEchoTrace); // outside the lock, like every trace here
        }
        if (!mergeTrace.empty())
        {
            AppendStateLog(L"hooks.log", mergeTrace);
            AppendStateLog(L"autorunner.log", mergeTrace);
        }
        if (!holdTrace.empty())
        {
            AppendStateLog(L"hooks.log", holdTrace);
            AppendStateLog(L"autorunner.log", holdTrace);
        }
        if (changed)
        {
            _notify(snapshot, HookEvent::UserPromptSubmit);
        }
    }

    void SessionRegistry::NoteInterrupt(const std::wstring& id, int64_t markerUnixMs)
    {
        // The replay belt FIRST (no lock): the scanner sights the marker on every read that ends on
        // it — a restored/adopted session's history replay included — and only a RECENT marker may
        // park anything (InterruptMarkerIsFresh; an unstamped one never can).
        const int64_t now = NowMs();
        if (!InterruptMarkerIsFresh(markerUnixMs, now))
        {
            return;
        }
        SessionInfo snapshot;
        AutorunnerMode held = AutorunnerMode::Off;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end() || !it->second.live)
            {
                return; // unknown / archived: nothing to drive, nothing to park
            }
            auto& s = it->second;
            if (!TakeInterruptHold(s.autorunner, markerUnixMs))
            {
                return; // already Off — the user's own Off, or an existing hold (idempotent, quiet)
            }
            held = s.autorunner.interruptHeldMode;
            snapshot = s;
        }
        const std::wstring line = L"[interrupt-hold] " + ShortId(id) + L" autorunner " + AutorunnerModeLabel(held) +
                                  L" -> Off (you interrupted the turn; resumes to " + AutorunnerModeLabel(held) +
                                  L" on your next message - setting the mode yourself cancels that)\n";
        AppendStateLog(L"hooks.log", line);
        AppendStateLog(L"autorunner.log", line);
        // Outside the lock, like every notify: the header toggle / overlay / board card repaint Off,
        // and the scheduler's OnObserved reads Off — BEFORE the interrupt's synthesized Stop can reach
        // the advance seam (the scanner parses the marker before it reconciles, and this runs inside
        // that parse), so DecideAdvance can only ever answer "autorunner off".
        _notify(snapshot, HookEvent::Unknown);
    }

    ObserverToken SessionRegistry::AddObserver(RegistryObserver observer)
    {
        std::lock_guard guard{ _mtx };
        const ObserverToken token = _nextObserverId++;
        _observers.emplace_back(token, std::move(observer));
        return token;
    }

    void SessionRegistry::RemoveObserver(ObserverToken token)
    {
        std::lock_guard guard{ _mtx };
        _observers.erase(
            std::remove_if(_observers.begin(), _observers.end(), [token](const auto& o) { return o.first == token; }),
            _observers.end());
    }

    void SessionRegistry::SetDefaultAutorunnerMode(AutorunnerMode mode)
    {
        std::lock_guard guard{ _mtx };
        _defaultAutorunnerMode = mode;
    }

    void SessionRegistry::SetAdvanceHandler(AdvanceHandler handler)
    {
        std::lock_guard guard{ _mtx };
        _advance = std::move(handler);
    }

    AdoptionToken SessionRegistry::AddAdoptionHandler(AdoptionHandler handler)
    {
        std::lock_guard guard{ _mtx };
        const AdoptionToken token = _nextAdopterId++;
        _adopters.emplace_back(token, std::move(handler));
        return token;
    }

    void SessionRegistry::RemoveAdoptionHandler(AdoptionToken token)
    {
        std::lock_guard guard{ _mtx };
        _adopters.erase(
            std::remove_if(_adopters.begin(), _adopters.end(), [token](const auto& a) { return a.first == token; }),
            _adopters.end());
    }

    void SessionRegistry::SetInjector(const std::wstring& id, Injector injector)
    {
        std::lock_guard guard{ _mtx };
        if (injector)
        {
            _injectors[id] = std::move(injector);
        }
        else
        {
            _injectors.erase(id);
        }
    }

    bool SessionRegistry::Inject(const std::wstring& id, const std::wstring& text) const
    {
        Injector fn;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _injectors.find(id);
            if (it == _injectors.end())
            {
                return false;
            }
            fn = it->second; // copy so we call it outside the lock
        }
        if (!fn)
        {
            return false;
        }
        try
        {
            fn(text);
        }
        catch (...)
        {
            // Forensics: the stdin injector. `false` is the Rule-#4 contract (the caller rolls the
            // prompt back to Pending rather than stranding a phantom `Sent`), so the recovery is
            // correct but the CAUSE was invisible — a dead ConptyConnection and a genuine bug look
            // identical from the queue's side.
            LogSwallowedException(L"SessionRegistry::Inject");
            return false;
        }
        return true;
    }

    bool SessionRegistry::HasInjector(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        return _injectors.find(id) != _injectors.end();
    }

    void SessionRegistry::SetPromptSubmitter(const std::wstring& id, PromptSubmitter submitter)
    {
        std::lock_guard guard{ _mtx };
        if (submitter)
        {
            _submitters[id] = std::move(submitter);
        }
        else
        {
            _submitters.erase(id);
        }
    }

    // Agentmaster (DELIVERY_PLAN.md R8 — the VERIFIED PRESSER): same shape + lifetime as the
    // submitter above.
    void SessionRegistry::SetEnterPresser(const std::wstring& id, EnterPresser presser)
    {
        std::lock_guard guard{ _mtx };
        if (presser)
        {
            _pressers[id] = std::move(presser);
        }
        else
        {
            _pressers.erase(id);
        }
    }

    bool SessionRegistry::PressEnterVerified(const std::wstring& id, const std::wstring& promptId, const std::wstring& promptText)
    {
        EnterPresser presser;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _pressers.find(id);
            if (it == _pressers.end() || !it->second)
            {
                return false; // no presser bound — the caller keeps the historical raw Inject("\r")
            }
            presser = it->second; // copy under the lock, invoke outside it (the Inject recipe)
        }
        try
        {
            presser(id, promptId, promptText);
        }
        catch (...)
        {
            // Rule #18 — and the press dispatch is a rescue path: a throw here means the eaten-CR
            // rescue silently died for this session. The attempt is still spent by the caller, so
            // the give-up ladder eventually surfaces it either way.
            LogSwallowedException(L"SessionRegistry::PressEnterVerified");
        }
        return true;
    }

    // Agentmaster (DELIVERY.md): atomically claim the per-session delivery gate for `tag`. See
    // the header for the contract; the log lines here are the gate's observability.
    bool SessionRegistry::TryOpenDeliveryGate(const std::wstring& id, const std::wstring& tag)
    {
        if (tag.empty())
        {
            return false; // an ownerless gate could never be owner-matched closed — refuse
        }
        bool reclaimed = false;
        std::wstring prevTag;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return false; // unknown session — nothing to deliver into
            }
            auto& s = it->second;
            if (DeliveryGateOpen(s, NowMs()))
            {
                return false; // held (unexpired) — exactly ONE operation may own the box
            }
            if (!s.deliveryPromptId.empty())
            {
                // Open but past kDeliveryGateTimeoutMs (or future-stamped): the holder died
                // without closing (a page torn down mid-swap). Reclaim — fail-open by design.
                reclaimed = true;
                prevTag = s.deliveryPromptId;
            }
            s.deliveryPromptId = tag;
            s.deliveryOpenedUnixMs = NowMs();
        }
        if (reclaimed)
        {
            AppendStateLog(L"hooks.log",
                           L"[gate] " + ShortId(id) + L" reclaimed an EXPIRED delivery gate (was held by " +
                               ShortId(prevTag) + L" past " + std::to_wstring(kDeliveryGateTimeoutMs / 1000) + L"s)\n");
        }
        return true;
    }

    void SessionRegistry::CloseDeliveryGate(const std::wstring& id, const std::wstring& tag)
    {
        SessionInfo snapshot;
        bool closed = false;
        bool stale = false;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return;
            }
            auto& s = it->second;
            if (s.deliveryPromptId.empty())
            {
                return; // already closed (a double close, or an Upsert re-key wiped the transient
                        // fields) — a QUIET no-op, never a phantom wake-up
            }
            if (s.deliveryPromptId != tag)
            {
                stale = true; // someone else's claim (expiry-reclaimed while this holder ran) —
                              // NEVER clear a younger delivery's hold
            }
            else
            {
                s.deliveryPromptId.clear();
                s.deliveryOpenedUnixMs = 0;
                snapshot = s;
                closed = true;
            }
        }
        if (stale)
        {
            AppendStateLog(L"hooks.log", L"[gate] " + ShortId(id) + L" stale close ignored (tag " + ShortId(tag) + L" no longer owns the gate)\n");
            return;
        }
        if (closed)
        {
            // The wake-up (DELIVERY.md): OnObserved re-requests the advance this open gate held,
            // and re-arms the Enter-retry watch if the just-resolved send is still unacknowledged.
            _notify(snapshot, HookEvent::Unknown);
        }
    }

    bool SessionRegistry::DeliveryGateHeld(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        return it != _sessions.end() && DeliveryGateOpen(it->second, NowMs());
    }

    // Agentmaster (DELIVERY.md §12): see the header. Owner-matched re-stamp — an EXPIRED gate whose
    // tag is still ours revives (the holder was alive all along, just starved of a thread); any
    // other tag (or a closed gate) refuses, and the caller must abort its delivery.
    bool SessionRegistry::RevalidateDeliveryGate(const std::wstring& id, const std::wstring& tag)
    {
        if (tag.empty())
        {
            return false;
        }
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end() || it->second.deliveryPromptId != tag)
        {
            return false; // closed, or a newer attempt reclaimed it — this carrier no longer owns the box
        }
        it->second.deliveryOpenedUnixMs = NowMs(); // revive/extend the claim (quiet — nothing changed for observers)
        return true;
    }

    // Agentmaster (DELIVERY.md §12 — INJECTION EVIDENCE): see the header. Quiet by design (the
    // [delivered] log line at the call site is the observability; the echo machinery carries the
    // visible consequences).
    void SessionRegistry::MarkPromptInjected(const std::wstring& id, const std::wstring& promptId)
    {
        if (promptId.empty())
        {
            return;
        }
        std::lock_guard guard{ _mtx };
        const auto it = _sessions.find(id);
        if (it == _sessions.end())
        {
            return;
        }
        for (auto& p : it->second.queue)
        {
            if (p.id == promptId && p.status == PromptStatus::Sent)
            {
                p.injectedAtUnixMs = NowMs();
                break;
            }
        }
    }

    // Agentmaster (PENDING_INPUT.md §9): the ONE send seam. See the header for the contract.
    // Agentmaster (DELIVERY.md): now also the gate's opening seam — exactly one delivery may be
    // in flight per session, decided HERE (synchronously, before anything marshals), so a racing
    // second submit is declined instead of marshalled-then-declined (the mark/decline/rollback
    // livelock the 07:27 incident recorded).
    bool SessionRegistry::SubmitPrompt(const PromptSubmission& submission)
    {
        // Agentmaster (DELIVERY.md §12): stamp a process-monotonic nonce so THIS delivery attempt
        // owns a UNIQUE gate tag — a reclaim + resend of the same prompt mints a new tag, and a
        // stale first attempt then fails its RevalidateDeliveryGate top guard instead of adopting
        // the resend's claim. The stamped COPY is what flows to the submitter/fallback, so the
        // hosting window's close derives the identical tag.
        PromptSubmission stamped = submission;
        {
            std::lock_guard guard{ _mtx };
            stamped.submitNonce = ++_submitSeq;
        }
        const std::wstring tag = DeliveryGateTagFor(stamped);
        if (!TryOpenDeliveryGate(stamped.sessionId, tag))
        {
            AppendStateLog(L"hooks.log",
                           L"[gate] " + ShortId(stamped.sessionId) + L" submit declined: the box is owned (delivery/clear in flight) - prompt " +
                               ShortId(stamped.promptId) + L" stays with its caller's rollback\n");
            return false;
        }
        PromptSubmitter fn;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _submitters.find(stamped.sessionId);
            if (it != _submitters.end())
            {
                fn = it->second; // copy so we call it outside the lock (it may hop to a UI thread)
            }
        }
        if (!fn)
        {
            // No hosting window registered one (an older window, a test/CLI host, a session bound
            // by something other than the launch seam): the historical path, verbatim — and the
            // inject IS the whole delivery here, so the gate resolves synchronously.
            const bool ok = Inject(stamped.sessionId, BuildPromptSubmission(stamped.text));
            if (ok)
            {
                MarkPromptInjected(stamped.sessionId, stamped.promptId); // §12: the inject IS the delivery here
                AppendStateLog(L"hooks.log", L"[delivered] " + ShortId(stamped.sessionId) + L" prompt " + ShortId(stamped.promptId) + L" (direct inject)\n");
            }
            CloseDeliveryGate(stamped.sessionId, tag);
            return ok;
        }
        try
        {
            const bool accepted = fn(stamped);
            if (!accepted)
            {
                CloseDeliveryGate(stamped.sessionId, tag); // nothing was or will be sent
            }
            // accepted == true: the hosting window owns the outcome — it closes the gate when the
            // swap RESOLVES (delivered / aborted), on every exit path (DELIVERY.md §3).
            return accepted;
        }
        catch (...)
        {
            // Same contract + reasoning as Inject's guard above: `false` makes the caller roll the
            // prompt back to Pending, so the queue stays honest, but the throw itself must not be
            // lost (Rule #18) — a submitter that dies looks identical to a torn-down window here.
            LogSwallowedException(L"SessionRegistry::SubmitPrompt");
            CloseDeliveryGate(stamped.sessionId, tag);
            return false;
        }
    }

    void SessionRegistry::RollbackPromptToPending(const std::wstring& id, const std::wstring& promptId, bool refundAutoSend)
    {
        if (promptId.empty())
        {
            return;
        }
        Update(id, [&](SessionInfo& ss) {
            for (auto& p : ss.queue)
            {
                if (p.id != promptId)
                {
                    continue;
                }
                if (p.status != PromptStatus::Sent)
                {
                    break; // already handled elsewhere — never resurrect a resolved prompt
                }
                p.status = PromptStatus::Pending;
                p.echoed = false;
                p.injectedAtUnixMs = 0; // §12: a rolled-back prompt's next send is a fresh delivery
                if (p.attempts > 0)
                {
                    p.attempts -= 1;
                }
                if (refundAutoSend && ss.autorunner.autoSendsThisRun > 0)
                {
                    ss.autorunner.autoSendsThisRun -= 1;
                }
                break;
            }
        });
    }

    void SessionRegistry::NoteHumanInput(const std::wstring& id, int64_t unixMs)
    {
        std::lock_guard guard{ _mtx };
        _lastHumanInput[id] = unixMs;
    }

    int64_t SessionRegistry::LastHumanInputUnixMs(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        const auto it = _lastHumanInput.find(id);
        return it == _lastHumanInput.end() ? 0 : it->second;
    }
}
