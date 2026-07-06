// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly). Includes only what it needs.
#include "SessionRegistry.h"

#include <algorithm>
#include <chrono>

#include "ClaudeSpawn.h" // AppendStateLog (the --fork-session source-id-echo suppression trace)

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

    // Agentmaster: case-insensitive equality for a WT_SESSION / tabToken GUID string. The hook wire
    // carries the token as-is from the WT_SESSION env, while the Fleet Observer reads the same value
    // out of the PEB — the two can differ in case, so a tab can't be matched by ordinal compare.
    bool TabTokenEq(const std::wstring& a, const std::wstring& b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            wchar_t ca = a[i];
            wchar_t cb = b[i];
            if (ca >= L'A' && ca <= L'Z')
            {
                ca = static_cast<wchar_t>(ca - L'A' + L'a');
            }
            if (cb >= L'A' && cb <= L'Z')
            {
                cb = static_cast<wchar_t>(cb - L'A' + L'a');
            }
            if (ca != cb)
            {
                return false;
            }
        }
        return true;
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
            if (it == _sessions.end())
            {
                _order.push_back(id);
            }
            _sessions[id] = std::move(info);
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
            _lastHumanInput.erase(id);
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
            s.live = false;
            s.pendingConfirmPromptId.clear();
            archived.push_back(s);
        }
        return archived;
    }

    void SessionRegistry::OnHookEvent(const HookMessage& msg)
    {
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
                for (const auto& [sid, fs] : _sessions)
                {
                    if (fs.live && !fs.forkParentId.empty() &&
                        TabTokenEq(fs.forkParentId, msg.sessionId) &&
                        TabTokenEq(fs.tabToken, msg.tabToken))
                    {
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

        SessionInfo snapshot;
        bool found = false;
        bool triggerAdvance = false;

        {
            std::lock_guard guard{ _mtx };
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
                _order.push_back(msg.sessionId);
                it = _sessions.emplace(msg.sessionId, std::move(created)).first;
            }

            auto& s = it->second;
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
            // Agentmaster (--fork-session source-id echo): this hook is for the session's OWN id, so the
            // fork has settled past its startup window (where it echoed the SOURCE id). Retire the
            // one-shot echo guard — a LATER deliberate in-session /resume back to that exact source id
            // should re-home normally, not be mistaken for the (already-passed) startup echo. The single
            // startup SessionStart echo arrives BEFORE any own-id hook, so it was already suppressed.
            if (!s.forkParentId.empty())
            {
                s.forkParentId.clear();
            }
            // Ordered transition (HookEvents.h): stale-Stop + type-ahead aware. The wire `ts` is
            // the hook's FIRE time — events can arrive out of order (the Stop forwarder does
            // transcript work first), and a prompt typed mid-turn produces a follow-on turn with
            // NO further UserPromptSubmit — both used to wrongly land WaitingForInput mid-turn.
            const auto ordered = NextSessionStateOrdered(s.state, msg, s.turns);
            s.state = ordered.state;
            // Agentmaster (API-error triage): preserve the failure reason WHILE in Error so the
            // Triage-Board Error card can show what died (the scanner's recon-error synth carries it on
            // the apiError message); clear it the instant the session leaves Error (recovery), so a
            // recovered/normal card never shows a stale error. Only the apiError synth sets these.
            if (msg.apiError)
            {
                s.errorMessage = msg.errorMessage;
                s.errorStatus = msg.errorStatus;
            }
            else if (s.state != SessionState::Error)
            {
                s.errorMessage.clear();
                s.errorStatus = 0;
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
            // as a Sent/Typed entry so the Auto Testing's "sent" summary is complete).
            if (msg.event == HookEvent::UserPromptSubmit && !msg.promptText.empty())
            {
                const int64_t now = NowMs();
                bool isEcho = false;
                for (auto& p : s.queue)
                {
                    if (p.origin == PromptOrigin::Autorun && p.status == PromptStatus::Sent && !p.echoed &&
                        p.text == msg.promptText && p.sentAtUnixMs != 0 && (now - p.sentAtUnixMs) >= 0 &&
                        (now - p.sentAtUnixMs) < kEchoWindowMs)
                    {
                        p.echoed = true; // consume exactly one echo per injected prompt
                        isEcho = true;
                        break;
                    }
                }
                if (!isEcho)
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
                }
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
            }
        }
    }

    void SessionRegistry::ObserveClaude(const ObservedClaude& o)
    {
        if (o.sessionId.empty())
        {
            return; // a correlated-but-never-prompted claude has no conversation id yet (§11d)
        }
        SessionInfo snapshot;
        bool created = false;
        bool changed = false;
        std::vector<SessionInfo> superseded; // stale prior conversations of the same claude on this ConPTY (notified outside the lock)
        {
            std::lock_guard guard{ _mtx };
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
                _order.push_back(o.sessionId);
                it = _sessions.emplace(o.sessionId, std::move(s)).first;
                created = true;
            }
            auto& s = it->second;
            const auto prevPid = s.pid; // Agentmaster: the pid we last knew, captured BEFORE the enrichment below overwrites it

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
            mutate(it->second);
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
        mutate(it->second);
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
            it->second.started = started;
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
            if (it == _sessions.end() || it->second.pendingInput == text)
            {
                return false; // unknown id, or no change at all
            }
            const bool wasPending = !it->second.pendingInput.empty();
            const bool nowPending = !text.empty();
            it->second.pendingInput = text;
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

    void SessionRegistry::NoteExternalPrompt(const std::wstring& id, const std::wstring& text)
    {
        if (text.empty())
        {
            return;
        }
        SessionInfo snapshot;
        bool changed = false;
        {
            std::lock_guard guard{ _mtx };
            const auto it = _sessions.find(id);
            if (it == _sessions.end())
            {
                return;
            }
            auto& s = it->second;
            // Idempotent: a message already recorded (status Sent — a Typed capture or an injected
            // Flight prompt's echo, which also shows up as a user line in the transcript) must not
            // be duplicated. Pending plan items are NOT "recorded messages", so they don't suppress
            // recording a human message that happens to match a queued prompt's text.
            for (const auto& p : s.queue)
            {
                if (p.status == PromptStatus::Sent && p.text == text)
                {
                    return;
                }
            }
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
            snapshot = s;
            changed = true;
        }
        if (changed)
        {
            _notify(snapshot, HookEvent::UserPromptSubmit);
        }
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
            return false;
        }
        return true;
    }

    bool SessionRegistry::HasInjector(const std::wstring& id) const
    {
        std::lock_guard guard{ _mtx };
        return _injectors.find(id) != _injectors.end();
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
