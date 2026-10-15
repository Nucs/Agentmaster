// Agentmaster (duplicate conversations, OBSERVER.md §20): which tab HOSTS a Claude conversation when two
// or more tabs run it at once.
//
// Agentmaster binds one conversation (a session id) to one tab. Claude Code itself has no such rule: a
// `/resume` of a conversation that is already open, a second `claude -c` in the same folder, or a Restart
// beside a copy that keeps running all give one conversation several live claude processes, each in its
// own tab, each reporting the same session id in its presence heartbeat. Before this header the Fleet
// Observer correlated every such tab to the session and the UI lane bound each one in turn, MOVING the
// session to whichever tab it saw last; the next pass moved it back. On the release of 2026-10-14 that
// loop ran for 2.8 days (about 175,000 binds), each bind rebuilt the tab's overlay and saved
// sessions.json, and it fed the Agent Manager page a registry change every second (UI_THREAD.md §12).
//
// The survey now gives each duplicated conversation ONE host (PickConversationHost) and names the other
// tabs as duplicates (FindDuplicateConversations); see ProcessObserver.cpp for how the result is used.
//
// Pure, header-only, no WinRT and no OS calls, so the standalone test harness (TestConversationHosts in
// tests_state.cpp) covers it directly.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Agentmaster
{
    // One tab whose claude reports a conversation, as PickConversationHost weighs it.
    struct ConversationHostCandidate
    {
        // The tab's WT_SESSION (the roster key; a plain lowercase GUID).
        std::wstring wtSession;
        // True when the tab's window binds THIS conversation to the tab (its roster boundSessionId is the
        // conversation's id). The strongest claim: keeping the host there means a running conversation
        // never changes tabs on its own.
        bool boundToSession{};
        // True when the registry's recorded host for the conversation (SessionInfo::tabToken: the last
        // bind, hook or observation) names this tab.
        bool recordedHost{};
        // The tab's claude process start time, Unix ms; 0 when unknown (a denied PEB read). The earliest
        // start is the copy that opened the conversation first.
        int64_t processStartUnixMs{};
    };

    // A claude process in a tab and the conversation it reports — the input FindDuplicateConversations
    // groups.
    struct ConversationClaim
    {
        // The tab's WT_SESSION.
        std::wstring wtSession;
        // The conversation the tab's claude runs now (presence heartbeat first). An empty id is never
        // grouped: a claude that has not been prompted yet has no conversation to share.
        std::wstring sessionId;
        // The session the tab's window binds to the tab (roster boundSessionId), empty when unbound.
        std::wstring boundSessionId;
        // The tab's claude process start time, Unix ms; 0 when unknown.
        int64_t processStartUnixMs{};
    };

    // One conversation that runs in two or more tabs: the tab that hosts it and the others.
    struct DuplicateConversation
    {
        // The duplicated conversation's session id.
        std::wstring sessionId;
        // The WT_SESSION of the tab chosen to host it.
        std::wstring hostWt;
        // The WT_SESSIONs of the other tabs that run it, in ascending order.
        std::vector<std::wstring> duplicateWts;
    };

    namespace conversation_hosts_detail
    {
        // ASCII case-insensitive equality — WT_SESSION GUIDs reach the registry in either case (the roster
        // lowercases them; a hook's tabToken is whatever the forwarder read from the environment).
        //   a, b — the strings to compare.
        //   returns — true when they are equal ignoring ASCII case.
        inline bool EqualsNoCaseAscii(const std::wstring& a, const std::wstring& b) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                wchar_t x = a[i];
                wchar_t y = b[i];
                if (x >= L'A' && x <= L'Z')
                {
                    x = static_cast<wchar_t>(x - L'A' + L'a');
                }
                if (y >= L'A' && y <= L'Z')
                {
                    y = static_cast<wchar_t>(y - L'A' + L'a');
                }
                if (x != y)
                {
                    return false;
                }
            }
            return true;
        }

        // Strict weak order of two candidates by host preference; true when `a` is the better host.
        // Rules, in order: bound to the conversation, the recorded host, the earlier-started claude (an
        // unknown start sorts after every known one), then the lower WT_SESSION — the last rule only
        // makes the choice deterministic, so two surveys of the same facts always pick the same tab.
        //   a, b — the candidates to compare.
        //   returns — true when `a` ranks strictly before `b`.
        inline bool BetterHost(const ConversationHostCandidate& a, const ConversationHostCandidate& b) noexcept
        {
            if (a.boundToSession != b.boundToSession)
            {
                return a.boundToSession;
            }
            if (a.recordedHost != b.recordedHost)
            {
                return a.recordedHost;
            }
            const bool aKnown = a.processStartUnixMs > 0;
            const bool bKnown = b.processStartUnixMs > 0;
            if (aKnown != bKnown)
            {
                return aKnown;
            }
            if (aKnown && a.processStartUnixMs != b.processStartUnixMs)
            {
                return a.processStartUnixMs < b.processStartUnixMs;
            }
            return a.wtSession < b.wtSession;
        }
    }

    // Choose the tab that hosts a conversation among the tabs that run it.
    //   candidates — every tab whose claude reports the conversation (any order).
    //   returns — the index of the chosen candidate, or `candidates.size()` when there are none.
    // Never throws. The choice is stable: a tab already bound to the conversation keeps it, so a session
    // can only move when its host stops running it (the claude exits, or switches conversation).
    inline size_t PickConversationHost(const std::vector<ConversationHostCandidate>& candidates) noexcept
    {
        size_t best = candidates.size();
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (best == candidates.size() || conversation_hosts_detail::BetterHost(candidates[i], candidates[best]))
            {
                best = i;
            }
        }
        return best;
    }

    // Find every conversation that two or more tabs run, and choose each one's host.
    //   claims — one entry per tab whose claude reports a conversation (claims with an empty session id
    //            are ignored; a tab listed twice counts once).
    //   recordedHostOf — callable `std::wstring(const std::wstring& sessionId)` returning the registry's
    //            recorded host WT_SESSION for a conversation (empty when none). Called once per duplicated
    //            conversation, only for those.
    //   returns — one entry per duplicated conversation, in ascending session-id order; empty when every
    //            conversation runs in one tab (the normal case).
    // Throws only what the callable or an allocation throws.
    template<typename RecordedHostFn>
    std::vector<DuplicateConversation> FindDuplicateConversations(const std::vector<ConversationClaim>& claims, RecordedHostFn&& recordedHostOf)
    {
        // Group by conversation (the index map finds a group; the vector keeps the members). The output is
        // sorted by session id at the end, so the result — and the log lines built from it — never
        // depend on the claims' order.
        std::vector<std::pair<std::wstring, std::vector<const ConversationClaim*>>> groups;
        std::unordered_map<std::wstring, size_t> groupIndex;
        for (const auto& c : claims)
        {
            if (c.sessionId.empty() || c.wtSession.empty())
            {
                continue;
            }
            const auto [it, inserted] = groupIndex.emplace(c.sessionId, groups.size());
            if (inserted)
            {
                groups.emplace_back(c.sessionId, std::vector<const ConversationClaim*>{});
            }
            auto& members = groups[it->second].second;
            bool seen = false;
            for (const auto* m : members)
            {
                if (conversation_hosts_detail::EqualsNoCaseAscii(m->wtSession, c.wtSession))
                {
                    seen = true; // the same tab twice (a merged roster quirk) is still one host
                    break;
                }
            }
            if (!seen)
            {
                members.push_back(&c);
            }
        }

        std::vector<DuplicateConversation> out;
        for (const auto& [sessionId, members] : groups)
        {
            if (members.size() < 2)
            {
                continue; // the normal case: one tab, nothing to decide
            }
            const std::wstring recorded = recordedHostOf(sessionId);
            std::vector<ConversationHostCandidate> candidates;
            candidates.reserve(members.size());
            for (const auto* m : members)
            {
                ConversationHostCandidate cand;
                cand.wtSession = m->wtSession;
                cand.boundToSession = !m->boundSessionId.empty() && conversation_hosts_detail::EqualsNoCaseAscii(m->boundSessionId, sessionId);
                cand.recordedHost = !recorded.empty() && conversation_hosts_detail::EqualsNoCaseAscii(recorded, m->wtSession);
                cand.processStartUnixMs = m->processStartUnixMs;
                candidates.push_back(std::move(cand));
            }
            const size_t host = PickConversationHost(candidates);
            DuplicateConversation d;
            d.sessionId = sessionId;
            d.hostWt = candidates[host].wtSession;
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                if (i != host)
                {
                    d.duplicateWts.push_back(candidates[i].wtSession);
                }
            }
            std::sort(d.duplicateWts.begin(), d.duplicateWts.end());
            out.push_back(std::move(d));
        }
        std::sort(out.begin(), out.end(), [](const DuplicateConversation& a, const DuplicateConversation& b) { return a.sessionId < b.sessionId; });
        return out;
    }
}
