// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
// The Sessions AI search's tools; see SessionSearchTools.h, SESSIONS.md §9 and SDK_HARNESS.md §7.
#include "SessionSearchTools.h"

#include "ClaudeSpawn.h" // ClaudeProjectsDir, LogSwallowedException, ResolveClaudeTranscriptPath
#include "SessionDbToolDefs.h" // the session database's tools (the database mode's server)
#include "SessionStore.h" // stored titles, tags, favorites
#include "TranscriptTurns.h" // RenderTranscriptTurnsFile: the read tool's compact transcript

#include <windows.h>

#include <algorithm>
#include <atomic> // the period check's reader threads
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cwchar>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace Agentmaster;

    int64_t SystemNowUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Cut `text` to `maxChars` UTF-16 units, ending a cut text in "..." and never splitting a
    // surrogate pair.
    std::wstring Cut(std::wstring text, size_t maxChars)
    {
        if (text.size() <= maxChars)
        {
            return text;
        }
        size_t keep = maxChars > 3 ? maxChars - 3 : 0;
        if (keep > 0 && text[keep - 1] >= 0xD800 && text[keep - 1] <= 0xDBFF)
        {
            --keep;
        }
        text.resize(keep);
        text += L"...";
        return text;
    }

    // Line breaks and tabs to single spaces, so a title or a snippet is one line of JSON text.
    std::wstring OneLine(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        bool lastSpace = true;
        for (const wchar_t c : text)
        {
            const bool space = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
            if (space)
            {
                if (!lastSpace)
                {
                    out.push_back(L' ');
                }
                lastSpace = true;
                continue;
            }
            out.push_back(c);
            lastSpace = false;
        }
        while (!out.empty() && out.back() == L' ')
        {
            out.pop_back();
        }
        return out;
    }

    std::wstring Trim(std::wstring_view text)
    {
        size_t a = 0, b = text.size();
        while (a < b && (text[a] == L' ' || text[a] == L'\t' || text[a] == L'\r' || text[a] == L'\n'))
        {
            ++a;
        }
        while (b > a && (text[b - 1] == L' ' || text[b - 1] == L'\t' || text[b - 1] == L'\r' || text[b - 1] == L'\n'))
        {
            --b;
        }
        return std::wstring{ text.substr(a, b - a) };
    }

    // The local broken-down time of a Unix ms instant. false when the C runtime cannot convert it.
    bool LocalTm(int64_t unixMs, struct tm& out)
    {
        const __time64_t t = unixMs / 1000;
        return _localtime64_s(&out, &t) == 0;
    }

    // "2026-10-08 14:32" (local), or empty for 0 / an instant the runtime cannot convert.
    std::wstring LocalDateTimeText(int64_t unixMs)
    {
        struct tm lt
        {
        };
        if (unixMs <= 0 || !LocalTm(unixMs, lt))
        {
            return {};
        }
        wchar_t buf[24]{};
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        return buf;
    }

    // "2026-10-08" (local), or empty.
    std::wstring LocalDateText(int64_t unixMs)
    {
        struct tm lt
        {
        };
        if (unixMs <= 0 || !LocalTm(unixMs, lt))
        {
            return {};
        }
        wchar_t buf[16]{};
        swprintf_s(buf, L"%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        return buf;
    }

    // How long ago, in the Sessions page's compact form: "now", "5m", "3h", "2d", "3mo", "1y".
    std::wstring AgoText(int64_t unixMs, int64_t nowUnixMs)
    {
        if (unixMs <= 0)
        {
            return {};
        }
        const int64_t s = (std::max)(int64_t{ 0 }, (nowUnixMs - unixMs) / 1000);
        if (s < 60)
        {
            return L"now";
        }
        if (s < 3600)
        {
            return std::to_wstring(s / 60) + L"m";
        }
        if (s < 86400)
        {
            return std::to_wstring(s / 3600) + L"h";
        }
        if (s < 30LL * 86400)
        {
            return std::to_wstring(s / 86400) + L"d";
        }
        if (s < 365LL * 86400)
        {
            return std::to_wstring(s / (30LL * 86400)) + L"mo";
        }
        return std::to_wstring(s / (365LL * 86400)) + L"y";
    }

    // "2026-10-12 01:08 (5m ago)", "... (now)" within the last minute, the bare time when no age can be
    // said: the `active` field of a search row (FormatSessionSearchResult writes the same form inline),
    // for grep's rows and read's headers.
    std::wstring ActiveWithAgo(int64_t activeUnixMs, int64_t nowUnixMs)
    {
        const std::wstring ago = AgoText(activeUnixMs, nowUnixMs);
        return LocalDateTimeText(activeUnixMs) + (ago.empty() ? std::wstring{} : (ago == L"now" ? std::wstring{ L" (now)" } : (L" (" + ago + L" ago)")));
    }

    // A local calendar date "YYYY-MM-DD" -> the Unix ms of its local midnight (endOfDay: of its
    // last millisecond). 0 when the text is not such a date. DST-safe: the C runtime resolves the
    // offset of that local wall-clock time (tm_isdst -1).
    int64_t ParseLocalDate(std::wstring_view text, bool endOfDay)
    {
        const std::wstring t = Trim(text);
        if (t.size() != 10 || t[4] != L'-' || t[7] != L'-')
        {
            return 0;
        }
        int v[3]{};
        const size_t starts[3] = { 0, 5, 8 };
        const size_t lengths[3] = { 4, 2, 2 };
        for (int i = 0; i < 3; ++i)
        {
            for (size_t k = 0; k < lengths[i]; ++k)
            {
                const wchar_t c = t[starts[i] + k];
                if (c < L'0' || c > L'9')
                {
                    return 0;
                }
                v[i] = v[i] * 10 + (c - L'0');
            }
        }
        if (v[0] < 1971 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31)
        {
            return 0;
        }
        struct tm lt
        {
        };
        lt.tm_year = v[0] - 1900;
        lt.tm_mon = v[1] - 1;
        lt.tm_mday = v[2];
        lt.tm_isdst = -1;
        if (endOfDay)
        {
            lt.tm_hour = 23;
            lt.tm_min = 59;
            lt.tm_sec = 59;
        }
        const __time64_t tt = _mktime64(&lt);
        if (tt == static_cast<__time64_t>(-1))
        {
            return 0;
        }
        return static_cast<int64_t>(tt) * 1000 + (endOfDay ? 999 : 0);
    }

    // <claude home>\history.jsonl — the file of every typed prompt (the sibling of projects\).
    std::wstring HistoryPath()
    {
        std::wstring projects = ClaudeProjectsDir();
        while (!projects.empty() && (projects.back() == L'\\' || projects.back() == L'/'))
        {
            projects.pop_back();
        }
        const size_t cut = projects.find_last_of(L"\\/");
        return cut == std::wstring::npos ? std::wstring{} : projects.substr(0, cut) + L"\\history.jsonl";
    }

    // True when every term of the query matches the session's NAME: its title, its branch, or
    // (an id term) its own or its fork parent's id. The Sessions page's tier-0 rule.
    bool NameMatches(const SessionIndexEntry& e, const std::vector<SearchTerm>& terms)
    {
        if (terms.empty())
        {
            return false;
        }
        const std::wstring title = FoldLower(SessionSearchTitleOf(e));
        const std::wstring branch = FoldLower(e.stats.gitBranch);
        const std::wstring id = FoldLower(e.sessionId);
        const std::wstring parent = FoldLower(e.stats.forkedFromId);
        for (const auto& t : terms)
        {
            const bool hit = (t.isGuid && (t.textLower == id || (!parent.empty() && t.textLower == parent))) ||
                             MatchesQueryText(title, t.textLower, false) ||
                             MatchesQueryText(branch, t.textLower, false);
            if (!hit)
            {
                return false;
            }
        }
        return true;
    }

    const wchar_t* TierWord(SessionMatchTier tier) noexcept
    {
        switch (tier)
        {
        case SessionMatchTier::Name:
            return L"title";
        case SessionMatchTier::Path:
            return L"path";
        case SessionMatchTier::Prompt:
            return L"prompt";
        case SessionMatchTier::Text:
            return L"text";
        case SessionMatchTier::Listed:
        default:
            return L"";
        }
    }

    const wchar_t kSearchSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("query":{"type":"string","description":"Words that must ALL match in one kind of place: the session's title, first prompt, folder and the paths of files it touched (each word may hit a different one), or ONE prompt the user typed (with deep, ONE message). Any case; a word also matches inside longer words (drag finds dragging). Use 1 to 3 distinctive words and leave filler words out; put a folder in dir and a branch in branch, not here. Words in \"double quotes\" match as one exact phrase. Leave it out to list sessions by the filters only."},)"
        LR"("days":{"type":"integer","description":"Only sessions active in the last N days. Default: every session, as far back as they go. Leave it out unless the user names a period."},)"
        LR"("after":{"type":"string","description":"A period's first local date, YYYY-MM-DD: only sessions last active on or after it. Overrides days."},)"
        LR"("before":{"type":"string","description":"A period's last local date, YYYY-MM-DD: only sessions that started on or before it. With after, every session active at some time in the period is in, also one that started before it or went on after it."},)"
        LR"("dir":{"type":"string","description":"Part of the working folder's path."},)"
        LR"("branch":{"type":"string","description":"Part of the git branch name."},)"
        LR"("tag":{"type":"string","description":"A bookmark tag the session carries (the whole tag name)."},)"
        LR"("favorite":{"type":"boolean","description":"true: only starred sessions."},)"
        LR"("open":{"type":"boolean","description":"true: only sessions that have a tab open now."},)"
        LR"("deep":{"type":"boolean","description":"true: also search the full text of the conversations (the agent's replies and tool output), every word in ONE message. Slower and broader (tens of seconds over every session): narrow it with dir or a period, and use it only when a normal search and grep found nothing useful."},)"
        LR"("sort":{"type":"string","enum":["relevance","active","created"],"description":"relevance (default): title matches first, newest first inside a group. active / created: newest first."},)"
        LR"("limit":{"type":"integer","description":"Rows to return, 1 to 200. Default 30. When matched is larger than returned, call again with a larger limit: every session that fits must reach the user."})"
        LR"(},"required":[]})";
    // The schema above and the system prompt below state these in words the model reads.
    static_assert(kSessionSearchDefaultDays == 0, "kSearchSchema, kGrepSchema and the system prompts say the default is every session: change their text too");
    static_assert(kSessionSearchDefaultLimit == 30 && kSessionSearchMaxLimit == 200, "kSearchSchema and the system prompts state the search's limits: change their text too");
    static_assert(kSessionGrepDefaultLimit == 20 && kSessionGrepMaxLimit == 100, "kGrepSchema and the system prompts state the grep's limits: change their text too");

    // The grep tool (SESSIONS.md §9.12): GrepTranscripts with the search's period and folder filters.
    // `context` and `limit` say their bounds in words the model reads; ParseSessionGrepParams clamps
    // to the same numbers.
    const wchar_t kGrepSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("pattern":{"type":"string","description":"The text to find, case-insensitive: words the user remembers, an error message's most telling fragment, a version, a file name. Plain text, matched as written (spaces included), unless regex is true."},)"
        LR"("regex":{"type":"boolean","description":"true: pattern is a regular expression (ECMAScript; . does not cross a line break, [\\s\\S] does). One regex covers every form of a word: put the varying part in a group next to a plain piece of 3 or more letters, as in drag(ged|ging)? or tab[ _-]?drag. Alternatives that each keep such a piece are fast too (0xC0000374|heap.?corrupt). A pattern with an alternative that has no such piece is slow: every session is read, and it can run out of time."},)"
        LR"("ids":{"type":"array","items":{"type":"string"},"description":"Only these sessions: ids from search or grep results. Leave it out to grep every session of the period."},)"
        LR"("days":{"type":"integer","description":"Without ids: only sessions active in the last N days. Default: every session."},)"
        LR"("after":{"type":"string","description":"Without ids: a period's first local date, YYYY-MM-DD (sessions last active on or after it). Overrides days."},)"
        LR"("before":{"type":"string","description":"Without ids: a period's last local date, YYYY-MM-DD (sessions that started on or before it)."},)"
        LR"("dir":{"type":"string","description":"Without ids: part of the working folder's path."},)"
        LR"("in":{"type":"string","enum":["all","user","claude","edits"],"description":"all (default); user: only what the user typed; claude: only Claude's side (its replies and thinking, tool calls and their output); edits: only the paths of the files Claude edited or wrote (Edit, Write, MultiEdit and NotebookEdit calls) - which sessions CHANGED a file."},)"
        LR"("context":{"type":"integer","description":"Characters shown on each side of a match, 40 to 400. Default 120."},)"
        LR"("limit":{"type":"integer","description":"Sessions to return, 1 to 100. Default 20. Sessions where the user typed the text come first. When matched is larger than returned, call again with a larger limit."})"
        LR"(},"required":["pattern"]})";

    // The read tool (SESSIONS.md §9.12): the Copy ▸ Transcript levels a confirming read needs.
    // The numbers are kSessionReadMaxIds / kSessionReadDefaultChars and the clamp of `chars`.
    const wchar_t kReadSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("ids":{"type":"array","items":{"type":"string"},"description":"1 to 8 ids from search or grep results: the sessions to read."},)"
        LR"("level":{"type":"string","enum":["minimal","compact","default"],"description":"minimal: each turn's user message and Claude's final reply. compact (default): also compactions, the user's answers and slash commands. default: also every tool call on one line."},)"
        LR"("chars":{"type":"integer","description":"Characters per session, 1000 to 40000. Default 6000. A longer session is cut to its first and last turns."})"
        LR"(},"required":["ids"]})";
    static_assert(kSessionReadMaxIds == 8 && kSessionReadDefaultChars == 6000, "kReadSchema and the system prompts state these numbers: change their text too");

    const wchar_t kShowSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("ids":{"type":"array","items":{"type":"string"},"description":"The ids of the sessions to show, best match first, exactly as a result returned them: up to 300, every one that fits. An empty list shows nothing: use it when nothing fits."},)"
        LR"("top":{"type":"integer","description":"Instead of ids: show the first N rows of your latest search, in its order."},)"
        LR"("note":{"type":"string","description":"A caption of a few words for the list, in the user's language."},)"
        LR"("why":{"type":"object","additionalProperties":{"type":"string"},"description":"For a shown id (the key, written as you pass it): one line of evidence you saw in read or grep, under 120 characters, in the user's language. The user's own words when you have them (You asked: \"...\"), else Claude's (Claude: \"...\"), else what the session shows; for a weak match, say so. A session you give no why gets the evidence your search and grep results showed for it (its folder, a path, a quote)."})"
        LR"(},"required":[]})";

    // show's description, the same in both modes. Surface-neutral since 2026-10-17: the Telegram Search topic shows the
    // same call as one card per session (MESSAGING.md 5.4), so "the screen" / "the list" would be wrong there. Since
    // 2026-10-18 it asks for EVERY session that fits, and says that a session with no why gets the evidence the results
    // showed (Show's auto-why), instead of "after reading the candidates" (a hundred sessions cannot all be read).
    const wchar_t kShowDescription[] =
        L"Show sessions to the user, in the given order, each with its why (yours, or the evidence your results showed for it): "
        L"the Sessions browser lists them, a phone gets one card each. Replaces what was shown before. Call it once per "
        L"request, with every session that fits.";

    // ===== the system prompts (SESSIONS.md §9.14, §9.15) =================================================================
    //
    // The user's request of 2026-10-17: "# Instructional Behaviors", then "## Tools, covering smartly and extensively
    // used", then "## Workflow Guidelines" with 1. 2. ... numbering that covers every kind of conversation about sessions
    // and search — and a model that is DESCRIPTIVE instead of hasty ("telling user, i'll 1., 2., 3.... and helpful").
    // It replaced the nine rules of 2026-10-13 / 2026-10-17, whose reply rule ("exactly ONE short sentence, under 20
    // words") was the hasty part. What carried over, re-worded:
    //   - the plan comes BEFORE the first tool call, in the SAME message, so it costs output tokens of that step but no
    //     extra model request (SESSIONS.md 9.3), and is chosen from everything the tools can do (the 52 s request of
    //     2026-10-12 opened with the slowest kind of call, deep:true); since 2026-10-17 the user SEES it: the page shows
    //     it while the request runs, the Telegram topic puts it above the answer (SessionSearchSnapshot::narration);
    //   - a `why` per shown session, the user's own words first (SESSIONS.md 9.12);
    //   - never a word about "the screen": the Telegram topic has none, and the answer is read there too;
    //   - the `indexing` note, follow-ups, local times.
    // New with the tabs server: the window choice (with several windows the user names one — the tools refuse
    // otherwise), the placement next to the tab a new one belongs with, deactivate only on an explicit request, one per
    // request.
    //
    // "## The Search Procedure" (the user's request of 2026-10-18: "Searches must never miss", optimized round by round
    // on an eval of five distinctly different searches on Sonnet 5.5 at medium thinking, then checked on four held-out
    // ones; SESSIONS.md §9.15): an explicit ORDER of work for every finding request, because the eval showed where a
    // model with tools but no procedure loses sessions — it searched 90 days, took the first 15 of 145 matches, showed
    // only what it had read (8 of 104), dropped forks that share a title, judged a period by titles, and showed near
    // misses for an exact request. Its seven steps: the request's facets (a project name is a subject AND a folder); a
    // wide net cast in the FIRST step (search, dir listing, other words, grep in:user / errors / in:edits, the period
    // search); every list completed (`more`, `also`, `stopped`, an index not ready) and the union of all lists kept;
    // evidence tiers (strong = folder, title, the user's prompt, an edit, activity in the period; to check = only
    // Claude's text or a path); a checklist before show (forks and copies kept, small sessions kept); every session that
    // fits shown in ONE call, near misses only named; a reply with counts, groups and what was left out. Measured: mean
    // recall 55% (the old prompt and engine) -> 73% (the engine fixes alone) -> 100% on all nine queries, precision 100%.
    // The completeness duty is stated first, in the introduction, with its reason (a session left out is lost to the user).
    //
    // "## Tools" teaches HOW each tool matches and what a call costs, not only what it does (the user's review of
    // 2026-10-18: "Review if we are using/specifying how to use each ## Tools smartly. For example, grep various forms
    // of what we are looking for"). The rules it states are the code's, and a change to the matching below must change
    // that text too (SESSIONS.md 9.14 "What ## Tools teaches"):
    //   - every finder ignores case and matches a word INSIDE longer words (MatchesQueryText, and the database's
    //     Dictionary::TermsContaining, are substring matches), so one stem covers a word's forms; the words of one query
    //     are an AND, so other words for the same thing (Claude's terms, a synonym, the other language) are separate
    //     calls of one step;
    //   - all words of a query must match in ONE kind of place (SearchIndexFast: the names and paths, each word in any
    //     of them; SearchHistoryPrompts: ONE prompt; SearchTranscriptsSlow and the database's blocks: ONE message), so a
    //     folder belongs in `dir`, never in `query`;
    //   - one grep regex covers every form of a word; the page grep narrows by a plain piece of 3+ letters every match
    //     holds, or — since 2026-10-18 — by one piece per alternative of an alternation (GrepRequiredLiterals), so
    //     0xC0000374|heap.?corrupt is fast and only an alternative with no piece reads every session (it can stop at
    //     kSessionGrepBudgetMs); the database refuses a regex without a single plain piece unless it names ids (the
    //     service's LongestRegexLiteral);
    //   - the page search covers every session by default (kSessionSearchDefaultDays 0), the database's search 90 days
    //     unless days is given — so the database's net passes days:3650;
    //   - read keeps a long session's first and last turns (CutRenderedTranscript), so the middle is reached by a grep
    //     with the session's id, then a read with more chars;
    //   - every tab after a new one moves one pos to the right, so several opens in one window pass `after` as a
    //     session id.
    //
    // Composed from shared pieces at compile time (adjacent wide literals behind macros), so the two modes cannot drift:
    // the introduction, the behaviors, the procedure's head and tail, the matching rules and the grep patterns, read, the
    // show + tabs tools and most workflows are ONE text; only the procedure's net (steps 2 and 3), the finding tools and
    // the workflows that use them (3, 4, 8 and 16) differ per mode. The pieces were generated from the eval's winning
    // prompt (the page prompt they compose is that file, character for character), so a change made here should be
    // measured with harness_live --eval --agent-prompt-file before it ships (SESSIONS.md §9.15).
    // ASCII on purpose: it travels through a file and the API unchanged whatever the code page (the m5 suite checks it).
    // MSVC allows a wide literal of about 32,767 characters after concatenation (C2026); each prompt stays well under it
    // (the m5 suite's bound is 30,000).
    // ⚠ Part of every request's prompt and of the prompt cache's key. The m5 suite pins the four headings, the plan
    //   rule, the procedure's steps, the matching rules and the grep patterns, every tool name, the workflow count, and
    //   (database) that the word deep never appears; SESSIONS.md 9.14 / 9.15 list the behaviors, the procedure, what
    //   ## Tools teaches, and the workflows.

#define AM_PROMPT_INTRO \
    L"You are the sessions assistant of Agentmaster, a Windows app that runs many Claude Code sessions, each in a tab of " \
    L"one of its windows. The user writes to you from the Sessions browser on the PC or from the Search topic of a Telegram " \
    L"chat on a phone. You find past and open Claude Code sessions (conversations) on this computer, read them to know what " \
    L"they are about, show them with one line of evidence each, answer questions about them and about the open windows and " \
    L"tabs, and - when the user asks - open a session again in a tab (resume), branch it into a new conversation (fork), or " \
    L"deactivate a tab.\n" \
    L"\n" \
    L"Your first duty is COMPLETENESS: every session that fits a request must reach the user. A session you leave out is " \
    L"lost to the user: they cannot know it exists, and they asked you because they do not remember. Your second duty is " \
    L"evidence: never show a session you have no evidence for. Speed comes third.\n" \
    L"\n"

#define AM_PROMPT_BEHAVIORS \
    L"# Instructional Behaviors\n" \
    L"\n" \
    L"1. Be descriptive and helpful, never hasty. Open every request with a short plan, before your first tool call and in " \
    L"the same message: tell the user in a few numbered lines what you are going to do, for example: I'll 1. search every " \
    L"session for the tab drag crash and its other wordings, 2. check the evidence of each match, 3. show all that fit. " \
    L"Choose the plan from everything your tools can do (see The Search Procedure and Tools), not only from the first idea, " \
    L"then make the calls of its first step at once. The user sees this plan while you work.\n" \
    L"2. The user sees only your messages and the sessions you show. Your tool calls and their results stay hidden, so " \
    L"every finding, every action you took and everything you could not do must be in your own words.\n" \
    L"3. End every request with a reply that answers it: first what you found or did, then what the user can do next. The " \
    L"sessions you show appear right after your reply, in your order (as numbered cards on a phone, as rows on the PC): " \
    L"refer to them by number (\"1 is where you fixed it, 2 is the follow-up\"); for a long list, say how they group (\"1 to " \
    L"40 are the proxmox folder's own sessions, 41 to 45 planned compute on that machine\") rather than describing each one, " \
    L"and do not repeat their titles, dates and folders line by line. Write it as plain text, at most about 1200 " \
    L"characters: short paragraphs, and numbered lines that start with the number (\"1. \", \"2. \"). The reply is shown " \
    L"exactly as you write it, also on a phone where every Markdown mark appears as a raw character, so never write ** or " \
    L"__ around words, # before a line, a table, a code block, or \"- \" bullets. For example:\n" \
    L"   Found 23 sessions. 1 to 4 fixed the gRPC gateway; 5 to 12 tuned the daily_buy optimizer; 13 to 23 are smaller " \
    L"questions about the same strategy.\n" \
    L"   Shall I read 5 to 12 to compare their results?\n" \
    L"4. Offer the likely next step at the end, as a question the user can answer in one word: \"Shall I resume 1 next to " \
    L"your Agentmaster tab?\", \"Shall I also show the 12 that only mention it in passing?\".\n" \
    L"5. Act at once on a clear request; never ask before you have searched. Ask only for a decision that is the user's: " \
    L"which window, which of several equally likely sessions, or whether to cut off a running turn. Ask it as a short " \
    L"numbered choice that the user can answer with a number or one word, and end your message with it.\n" \
    L"6. Evidence first. Show a session only with evidence that it fits: its folder, its title, a prompt the user typed, a " \
    L"quote you saw in grep or read, an edit of the asked file, its activity in the asked period. Never invent a session, " \
    L"an id, a window, a tab or a quote: use only what a tool returned, and say how sure you are when the evidence is thin. " \
    L"Never judge a session by its title alone when the request is about what happened in it.\n" \
    L"7. The workspace is the user's. Resume or fork only the sessions the user asked for (the tools refuse more than 5 new " \
    L"tabs per request). Deactivate a tab only when the user explicitly asked to deactivate (park, put to sleep) that very " \
    L"tab, and only one tab per request: when the user names several, deactivate the first, say so, and ask them to confirm " \
    L"the next one. Never deactivate on your own initiative. No tool closes or deletes anything.\n" \
    L"8. A follow-up refers to what you showed or did before (\"the second one\", \"only the open ones\", \"older ones\", \"put it " \
    L"next to that tab\"): build on your earlier results instead of starting over.\n" \
    L"9. Answer in the user's language. Times in results are local time; each result gives the current local time as `now`.\n" \
    L"10. A tool result that says the session index is still being read searched nothing: call the same tool again at once " \
    L"(the call waits for the index); never answer from it and never call show before the index is ready. A result with an " \
    L"`indexing` field is incomplete in the same way.\n" \
    L"\n"

#define AM_PROMPT_PROC_HEAD \
    L"## The Search Procedure\n" \
    L"\n" \
    L"Follow it for every request that asks for sessions (\"find ...\", \"which sessions ...\", \"what did I work on ...\", \"the " \
    L"session where ...\"). It is what makes a search complete.\n" \
    L"\n" \
    L"1. Read the facets of the request: the subject (a topic, a feature, a machine, a tool), the words the user remembers, " \
    L"an error text or code, a file, a folder or project, a period, a mark (open, starred, a tag, a branch). A word that " \
    L"names one of the user's projects or folders (proxmox, agentmaster, catalog) is BOTH a subject and a folder: its " \
    L"folder's sessions are all about it.\n"

#define AM_PROMPT_PROC_NET_PAGE \
    L"2. Cast a wide net: in your FIRST step make every call that can find a fitting session, all at once (they run in " \
    L"parallel):\n" \
    L"   - search with the subject's stem and no days: every session is searched by default, as far back as they go;\n" \
    L"   - when the subject is also a project or folder name: search with dir set to it and no query, limit 200: it lists " \
    L"every session launched there;\n" \
    L"   - other words for the subject (the term Claude would have used, a synonym, the user's other language): each its " \
    L"own search, or one grep regex for all of them;\n" \
    L"   - words the user remembers: grep in:user, one regex for their forms;\n" \
    L"   - an error text, a code, a version or an identifier: grep (it reads Claude's side, where errors usually are). A " \
    L"grep over every session reads gigabytes when its words are short or common (pve, lxc, fix): use such words only " \
    L"narrowed by dir or a period, or with ids;\n" \
    L"   - a file: search with the file's name finds the sessions that touched it; for \"changed\", \"edited\", \"wrote\", \"fixed " \
    L"in\", grep the file's name with in:edits;\n" \
    L"   - a period: search with after and before (plus dir when a folder is named), no query, sort:active, limit 200.\n" \
    L"   Use days, after or before only when the user named a period.\n" \
    L"3. Make every list complete before you judge it. A search result with `more` returned only part of its matches: call " \
    L"it again with a larger limit (up to 200). A grep result with `more` lists the rest in `also` (id, title, folder, how " \
    L"many messages match): they are candidates too - grep their ids when you need their context. A grep with `stopped` did " \
    L"not read every session: run it again narrowed by dir, a period or ids, until nothing is cut. An index that is still " \
    L"being read: call again. A long list is no reason to narrow: judge every row of it. Every row says its folder, and a " \
    L"grep's `folders` counts its matches per folder, so for a request about one project (of Agentmaster, in TraderLab) " \
    L"pick the rows by their dir. To narrow a list that is cut, repeat the SAME call - the same pattern or words - with " \
    L"dir, a period or ids added: grep 0xC0000374|heap.?corrupt again with dir:agentmaster, never 0xC0000374 alone. A call " \
    L"with fewer or other words is not a narrower call but a different search, and it loses every session that says it in " \
    L"the other words. Your candidates are the union of EVERY list you got: the sessions only an earlier, broader call " \
    L"found stay candidates.\n"

#define AM_PROMPT_PROC_NET_DATABASE \
    L"2. Cast a wide net: in your FIRST step make every call that can find a fitting session, all at once (they run in " \
    L"parallel):\n" \
    L"   - search with the subject's stem and days:3650 (the database's search looks back only 90 days unless you pass " \
    L"days);\n" \
    L"   - when the subject is also a project or folder name: search with dir set to it, no query, days:3650 and limit 50: " \
    L"it lists the sessions launched there;\n" \
    L"   - other words for the subject (the term Claude would have used, a synonym, the user's other language): each its " \
    L"own search;\n" \
    L"   - words the user remembers: grep with roles [\"prompt\",\"queued\"] and days:3650, one regex for their forms;\n" \
    L"   - an error text, a code, a version or an identifier: grep with days:3650 (it reads Claude's side and tool output, " \
    L"where errors usually are);\n" \
    L"   - a file: who_touched with the file's name; for \"changed\", \"edited\", \"wrote\", \"fixed in\", who_touched with op " \
    L"edit, then op write;\n" \
    L"   - a period: timeline for it, or search with after and before (plus dir when a folder is named), no query, limit " \
    L"50.\n" \
    L"   Use a shorter period only when the user named one.\n" \
    L"3. Make every list complete before you judge it. A result with `next` holds only part of its rows: call it again with " \
    L"cursor set to that value until there is no `next`. To narrow a list that is too long, add dir or a period and keep " \
    L"the same words and pattern: a call with fewer or other words is not a narrower call but a different search. Your " \
    L"candidates are the union of EVERY list you got: the sessions only an earlier, broader call found stay candidates - a " \
    L"later call can miss a session that says it in other words.\n"

#define AM_PROMPT_PROC_TAIL \
    L"4. Sort the candidates by their evidence:\n" \
    L"   - strong: launched in the asked folder; its title or first prompt names the subject; a prompt the user typed holds " \
    L"it; Claude changed the asked file; it was active inside the asked period. Keep it.\n" \
    L"   - to check: only Claude's text, a tool's output or a touched path names the subject. Look at the context the " \
    L"result showed (a snippet, a match, a first prompt). When it does not settle the question - a path match shows no " \
    L"context at all - grep the subject in those candidates' ids (several calls in one step when they are many) or read " \
    L"them: never leave a candidate out on a guess. Keep it when the subject was part of the work it did; leave it out when " \
    L"it was a passing mention (a path in a listing, a word in a log, an old crash in a list of crashes, another project's " \
    L"file in a long list).\n" \
    L"   Read (one read call takes up to 8 sessions; make several calls in one step for more) the candidates you are unsure " \
    L"about and the ones your reply describes. You do not need to read every strong one: its evidence already shows that it " \
    L"fits.\n" \
    L"5. Before you show, check: Did any call use a period the user did not ask for? Is any list still cut? Did I search " \
    L"the subject's other words? Did I keep every strong candidate of every list, small or empty-looking ones included (a " \
    L"session where the user asked one question is still a session about it)? Did I keep every fork and copy? Sessions with " \
    L"the same title or the same first prompt are forks or continuations of one conversation, but each is a session of its " \
    L"own that the user may want - show each one, never only one of them. Is anything I show only a passing mention? Fix " \
    L"what fails first.\n" \
    L"6. Show every session that fits, in ONE show call, best first: the sessions most about the request first, then the " \
    L"related ones - every one of them, never a sample or one representative of a group of similar sessions (when five " \
    L"forks fit, all five go in). Match what the request asks for: when it asks for one particular thing - the session " \
    L"where the user said certain words, the sessions that edited a file, the ones that hit a certain error - show the " \
    L"sessions with evidence of exactly that, and do not show near misses (a session about the same feature where the user " \
    L"said something else, one that only read the file, one that hit another error): name them in your reply and offer to " \
    L"show them. The user's words count in whatever form they wrote them: a line of a table or list they pasted, a note " \
    L"next to a screen they copied (a grep match by user is their words). Give a why where you have better evidence than " \
    L"the search row: a quote from grep, or - for every session you read - one line of what it did, in the user's words " \
    L"when it has them; every session you give no why gets the evidence your results showed for it.\n" \
    L"7. Reply with how many fit and how they group, what the main ones did, and what you left out and why (\"12 more only " \
    L"mention proxmox in passing\"), then the likely next step.\n" \
    L"\n"

#define AM_PROMPT_TOOLS_HEAD \
    L"## Tools\n" \
    L"\n" \
    L"How the finding tools match words, search and grep alike: case never matters, and a word also matches inside longer " \
    L"words - drag finds dragging and dragged, compil finds compile, compiler and compiling, scanner finds SessionScanner. " \
    L"So one short stem covers a word's forms. Every word of one search must match, so other words for the same thing are " \
    L"separate searches: make them in the same step - the user's words and the words Claude would have used (crash and " \
    L"access violation, tooltip and hover card), English and the user's language when the session may have been held in " \
    L"either. Prefer a rare word to a common one: a word that is in most sessions (fix, code, file) narrows nothing.\n" \
    L"\n" \
    L"Patterns for grep. Plain text (the default) matches as written, any case, spaces included, with nothing to escape. A " \
    L"regex (regex:true) catches every form of one thing in ONE call:\n" \
    L"- endings and spellings: drag(ged|ging|s)?, (re)?compil(e|ed|er|ing), colou?r\n" \
    L"- joined, split or hyphenated words: tab[ _-]?drag finds tab drag, tab-drag, tab_drag and TabDrag\n" \
    L"- two words close together, in that order: drag.{0,40}crash (. never crosses a line break)\n" \
    L"- either slash in a path: source.agentmaster (. is any one character)\n" \
    L"Keep a plain piece of 3 or more letters or digits outside every group and alternative where you can: drag(ged|ging)? " \
    L"has one (drag), while (dragged|dragging) and a top-level a|b have only one piece per alternative; what that costs is " \
    L"under grep below. Words with nothing in common (synonyms, two languages) go in separate grep calls in the same step, " \
    L"or in one regex of alternatives where grep below allows it. In a regex, escape . ( ) [ ] { } ? * + | ^ $ and the " \
    L"backslash when you mean the character itself (0\\.6\\.8, foo\\(\\)); plain text needs no escaping. From an error message, " \
    L"grep its most telling fragment, never all of it: paths, numbers and quotes differ from run to run.\n" \
    L"\n"

#define AM_PROMPT_READ_BODY \
    L"reads up to 8 sessions in one call as compact Markdown, turn by turn: level minimal = each turn's user message and " \
    L"Claude's final reply; compact (the default) adds compactions, the user's answers and slash commands; default adds " \
    L"every tool call on one line (what Claude ran, read and edited). chars is the size per session, 1000 to 40000 (default " \
    L"6000): a longer session keeps its first and last turns and says how many it left out in the middle. Read all your " \
    L"candidates in one call; the defaults are enough to confirm one. To answer what a session did or decided, use level " \
    L"default; for something in the middle of a long session, grep it (with its id) to find the place, then read it alone " \
    L"with more chars. It reads the session's own conversation, not its subagents' work.\n"

#define AM_PROMPT_TOOLS_PAGE \
    L"Finding and reading sessions. These change nothing the user sees: use them freely, and make independent calls in one " \
    L"step.\n" \
    L"- mcp__sessions__search finds sessions. All words of `query` must match in ONE kind of place: the session's names and " \
    L"places (its title, first prompt, folder and the paths of files it touched; each word may hit a different one), or ONE " \
    L"prompt the user typed, or with deep:true ONE message of the conversation. The git branch is not among them: it is " \
    L"only the branch filter. So put a folder in dir and a branch in branch, never in query: \"agentmaster tooltip\" misses a " \
    L"session about tooltips whose title does not say tooltip, while dir:agentmaster with query:tooltip finds it through " \
    L"its prompts. Use 1 to 3 distinctive words, no filler words; \"double quotes\" match an exact phrase, spaces included; a " \
    L"whole session id finds that session and its forks. It searches EVERY session by default; narrow it with days, after " \
    L"and before (YYYY-MM-DD: a session active at any time in the period is in, also one that started before it or went on " \
    L"after it), dir, branch, tag, favorite and open; sort by relevance (the default: name matches first, then paths, " \
    L"prompts and other text, newest first in each), active or created; limit 1 to 200 (default 30). A result with `more` " \
    L"holds only part of the matches: raise limit. Each row has a short id, the title, the folder, the branch, the times, " \
    L"the sizes, how it matched (title, path, prompt or text, with snippets), for a path match the `path` that matched and " \
    L"- when that is a file it touched - its first_prompt (what the session was for, in the user's words), and - when you " \
    L"gave after or before - in_period: how many of its own messages and prompts fall inside the period (none = it only " \
    L"spans the period). deep:true also reads Claude's replies and tool output, for words that only Claude wrote, or " \
    L"several words that must meet in one message in any order: it can take tens of seconds over every session, so narrow " \
    L"it with dir or a period, and try grep first for exact text.\n" \
    L"- mcp__sessions__grep finds exact text or a regex (ECMAScript) in the conversations' full text: the user's prompts, " \
    L"Claude's replies and thinking, tool calls and their output. For each matching session it returns its folder (dir), " \
    L"how many messages match and the first matches in context, each with who wrote it (user or claude) and when; sessions " \
    L"where the user typed it come first; past limit (1 to 100, default 20) the other matching sessions are listed in " \
    L"`also` as one line each (id, title, folder, how many messages match); a result with 10 or more matches counts them " \
    L"per folder in `folders`. in:user finds only what the user typed (the best evidence that a session is the one they " \
    L"remember), in:claude only what Claude did, in:edits only the paths of files Claude edited or wrote - the sessions " \
    L"that CHANGED a file. Give ids to look inside your candidates (fast and complete), or a period and dir to look through " \
    L"everything (every session by default); context 40 to 400 characters on each side of a match (default 120; raise it to " \
    L"quote more). A regex whose every alternative keeps a plain piece of 3 or more letters or digits is narrowed by those " \
    L"pieces, so 0xC0000374|heap.?corrupt stays fast; a pattern with an alternative that has none (\\d+, a single letter) " \
    L"cannot be narrowed: every session is read in full, and the grep may stop at its 20 s budget. A result with stopped " \
    L"did not read every session: narrow it with ids, dir or a shorter period. too_complex asks for a simpler regex.\n" \
    L"- mcp__sessions__read " AM_PROMPT_READ_BODY \
    L"\n"

#define AM_PROMPT_TOOLS_DATABASE \
    L"Finding and reading sessions in the session database. These change nothing the user sees: use them freely, and make " \
    L"independent calls in one step.\n" \
    L"- mcp__agentmaster__search finds sessions. All words of `query` must match in ONE kind of place: the session's names " \
    L"and places (title, folder, branch and the paths of files it touched; each word may hit a different one), or ONE " \
    L"message of the chosen `scopes`. So put a folder in dir and a branch in branch, never in query. scopes: title, path, " \
    L"prompt and reply by default; add tool for tool calls and their output (broad and noisy: code, commands, error " \
    L"output), subagent for the subagents' work. Use 1 to 3 distinctive words; \"double quotes\" match a phrase; a whole " \
    L"session id finds that session and its forks; fuzzy:true also finds a misspelled word (its letters in order); a query " \
    L"whose words all have 1 or 2 characters searches only names, folders and prompts. Narrow it with days (default: the " \
    L"last 90 days - pass days:3650 to search every session), after and before (YYYY-MM-DD), dir, branch, tag, favorite and " \
    L"open; sort by relevance (the default: name matches first, then paths, prompts and other text, the best first in " \
    L"each), active or created; limit 1 to 50 (default 15), and cursor (the result's next) for the rows after them. Rows " \
    L"carry a short id, why they matched, and up to two snippets whose ref opens their context in messages. It is fast " \
    L"(under 0.2 s): search again with other words rather than giving up.\n" \
    L"- mcp__agentmaster__grep finds exact text or an RE2 regex (no lookarounds, no backreferences) in the conversations, " \
    L"with the words around each match: in the sessions of ids (up to 50), or in every session of a period and dir (days " \
    L"defaults to 90: pass days:3650 for every session). By default it reads the user's prompts, Claude's replies, tool " \
    L"calls, their output and system text; add thinking or subagent to roles when the answer may be there. A regex with no " \
    L"plain piece of 3 or more letters or digits (see Patterns for grep) is refused unless you name the sessions in ids. " \
    L"case_sensitive:true only when case matters (an identifier); context 0 to 400 characters (default 80); limit 1 to 200 " \
    L"matches (default 30), then cursor.\n" \
    L"- mcp__agentmaster__who_touched lists the sessions that read, edited or wrote a file, or anything under a folder, " \
    L"newest first: a full path, a folder, or a file name alone (TerminalPage.cpp). op narrows it to read, edit, write, " \
    L"search or shell (edit and write: who changed it). It looks back 365 days by default.\n" \
    L"- mcp__agentmaster__timeline lists, day by day (group hour for a single day), the sessions that were active in a " \
    L"period, with their prompts: from and to (YYYY-MM-DD; default the last 7 days), dir, and limit sessions per day or " \
    L"hour.\n" \
    L"- mcp__agentmaster__session gives one session's facts without its messages: titles, folder, branch, times, size, " \
    L"models, tokens, lineage, tags, star, whether a tab has it open, the files it touched most. Cheaper than a read: use " \
    L"it to check a candidate's folder, times or lineage.\n" \
    L"- mcp__agentmaster__messages reads one session's conversation in order, a page at a time (limit up to 100 blocks, " \
    L"cursor for the next page): around a ref from search or grep for the context of a hit, from a ref onward, or " \
    L"order:desc for the end first (what was decided last). roles adds tool calls, tool output, thinking or the subagents' " \
    L"work; full:true keeps long blocks whole.\n" \
    L"- mcp__agentmaster__files lists the files and folders one session read, edited, wrote, searched or named in a " \
    L"command; op and under (a folder) narrow it.\n" \
    L"- mcp__agentmaster__related finds a session's lineage (the session it was forked from, its forks, the plan, /clear or " \
    L"/handover chain) and the sessions that share the most files or rare words with it; kinds picks lineage, files or " \
    L"words.\n" \
    L"- mcp__agentmaster__usage counts tokens by session, day, model or folder (group_by) over days (default 30) or after " \
    L"and before, or for one session (id). The cost appears only where Claude Code recorded it: never estimate one.\n" \
    L"- mcp__agentmaster__sql runs one read-only SELECT (DuckDB) over the database's views, for counts and unusual filters " \
    L"that no other tool answers: at most 200 rows within 2 s, so count and group in the query instead of fetching rows.\n" \
    L"- mcp__agentmaster__read " AM_PROMPT_READ_BODY \
    L"\n"

#define AM_PROMPT_SHOW_AND_TABS \
    L"Showing sessions:\n" \
    L"- mcp__display__show shows sessions to the user in your order and replaces what was shown before (rows in the " \
    L"Sessions browser on the PC, one card each on a phone). Pass the short ids from any result (search, grep, read, list " \
    L"or another tool), best first, up to 300, or top:N for the first N rows of your latest search; a note of a few words; " \
    L"and a why where you have one: one line of evidence you saw in read or grep, under 120 characters, in the user's " \
    L"language - the user's own words when you have them (You asked: \"...\"), else Claude's (Claude: \"...\"), else what the " \
    L"session shows; say so for a weak match. A session you give no why gets the evidence your results showed for it. Call " \
    L"it once per request, after the procedure's check. When a search request found nothing that fits, call it with an " \
    L"empty ids list.\n" \
    L"\n" \
    L"Windows and tabs. list only reads; resume, fork and deactivate change the user's workspace, so use them only when the " \
    L"user asked:\n" \
    L"- mcp__tabs__list lists the Agentmaster windows and their tabs. Per window: its number (window), id, tab count, " \
    L"whether the user used it last (last_used) and whether this request came from it (asked_from). Per tab: its position " \
    L"(pos), title, kind (claude, codex or shell), session id, state (running, waiting for you, needs approval, error, idle " \
    L"or done), dormant (its agent starts when the tab is entered), its folder (dir), where it really works (works_in, when " \
    L"that differs), branch, last_turn (when its newest turn started), active (its newest activity) and selected. Narrow it " \
    L"with window, dir (matches dir or works_in), title and limit (default: the 40 most recently active tabs of each " \
    L"window, in strip order); to find one tab, narrow with title or dir rather than raising limit. Before you open a tab, " \
    L"list the window it goes in and compare works_in (else dir), branch and active to choose the tab it goes after.\n" \
    L"- mcp__tabs__resume opens a closed session again in a tab (the same conversation, its context intact). An open " \
    L"session is never opened twice: the result says where its tab is, and focus:true brings that tab to the front. With " \
    L"more than one window open, window is required; without it the tool refuses and describes the windows, and you must " \
    L"ask the user. after places the new tab right after a tab of that window (its pos or its session id); without after it " \
    L"goes at the end of the strip. Every tab after a new one moves one pos to the right, so when you open several tabs in " \
    L"one window, pass after as a session id, or list again between them. The tab opens in the background unless focus is " \
    L"true, which switches the window to it.\n" \
    L"- mcp__tabs__fork branches a session into a NEW conversation that starts with the whole history of the source; the " \
    L"source is untouched, open or closed. The same window, after and focus rules apply; without after, the fork goes right " \
    L"after its source's tab when the source is open in that window, as the tab menu's Fork session does. model starts it " \
    L"on another model (an id, or an alias such as opus or sonnet).\n" \
    L"- mcp__tabs__deactivate deactivates (parks) one tab: its claude process stops, the tab keeps its place, title, color " \
    L"and prompt queue, and the conversation resumes when the user enters the tab. One tab per request; take its id from " \
    L"list, which also shows its state. It refuses a tab whose turn is running or waiting for approval unless cut_off_turn " \
    L"is true, because that turn would be cut off: ask the user first.\n" \
    L"\n"

#define AM_PROMPT_WORKFLOWS_HEAD \
    L"## Workflow Guidelines\n" \
    L"\n" \
    L"1. A session the user describes (\"the session where I fixed the tab drag crash\"): the Search Procedure - search every " \
    L"session for 1 to 3 distinctive words of the subject, in the same step with other words for it (the term Claude would " \
    L"have used, a synonym, the other language) and a grep for the most telling words; check the candidates' evidence and " \
    L"read the best ones (one read call, up to 8 ids); show every one that is really about it, best first; reply with what " \
    L"each main one did.\n" \
    L"2. Exact words, an error message, a version, a command or a file name the user remembers: grep for it over every " \
    L"session - one regex for all its forms (see Patterns for grep), in the user's prompts when the user typed it -, look " \
    L"at the matches' context, read the sessions you are unsure of, and show every one that fits.\n"

#define AM_PROMPT_WORKFLOW_3_PAGE \
    L"3. A file or a folder: sessions that CHANGED a file (\"which sessions edited SessionScanner.cpp?\"): grep the file's " \
    L"name with in:edits; sessions that worked with it: search with the file's name as a word; sessions of a folder (\"my " \
    L"sessions in the catalog folder\"): search with dir and no query, limit 200 - and the folder's name as a query too, for " \
    L"sessions elsewhere that worked on it.\n"

#define AM_PROMPT_WORKFLOW_3_DATABASE \
    L"3. A file or a folder (\"the session that changed SessionScanner.cpp\", \"my sessions in the catalog folder\"): " \
    L"who_touched with the file name, the path or the folder (op edit or write for the sessions that changed it); search " \
    L"with dir and days:3650 for the sessions that ran in a folder; files tells what one session touched.\n"

#define AM_PROMPT_WORKFLOW_4_PAGE \
    L"4. A period (\"yesterday\", \"last week\", \"between October 3 and 5\", \"what did I do this morning?\"): search with after " \
    L"and before (and dir when a folder is named), no query, sort:active, limit 200 - every session whose activity overlaps " \
    L"the period is in, also one that started before it or went on after it, and each row's in_period says how many of its " \
    L"own messages and prompts fall INSIDE the period. Show every session with in_period messages, small ones included (a " \
    L"session where the user asked one question is work they did); leave out the ones whose in_period is none (they only " \
    L"span the period) and the empty ones with no prompt, and say how many you left out. The user asks what they DID, and a " \
    L"title is not an answer: read the sessions before you summarize - level minimal, chars 2500, up to 8 ids per call, " \
    L"several calls in one step (for more than 24 sessions, the 24 with the most prompts in the period) - then reply with a " \
    L"summary grouped by topic that says what was done, and show the sessions behind it.\n"

#define AM_PROMPT_WORKFLOW_4_DATABASE \
    L"4. A period (\"yesterday\", \"last week\", \"on October 3\", \"what did I do this morning?\"): timeline for the period (with " \
    L"dir when a folder is named); show every session it lists, small ones included (a session where the user asked one " \
    L"question is work they did). The user asks what they DID, and a title is not an answer: read (or session) the busiest " \
    L"ones before you summarize, then reply with a summary grouped by topic, and show the sessions behind it.\n"

#define AM_PROMPT_WORKFLOWS_5_TO_7 \
    L"5. A mark or a state (\"my open sessions\", \"the starred ones\", \"tagged release\", \"on branch X\"): search with open, " \
    L"favorite, tag or branch. For what runs in the tabs right now, list the tabs (workflow 15).\n" \
    L"6. A question about what a session did or decided (\"did that session finish the release?\", \"what did we decide about " \
    L"the cache?\"): find it, read it at the level that shows the answer (level default and more chars for details; grep for " \
    L"the exact place), and answer with the evidence, quoting briefly; show the session too.\n" \
    L"7. A follow-up that narrows or changes the last result (\"only the open ones\", \"the older ones\", \"sort by date\", \"not " \
    L"that one\"): work from what you showed, search again only for sessions you have not seen yet, and show the new list.\n"

#define AM_PROMPT_WORKFLOW_8_PAGE \
    L"8. Nothing fits: try a shorter stem, other words for it and the other language (several searches in one step), and " \
    L"fewer words; then grep with a regex for the forms; then deep:true, narrowed by dir or a period. When there is still " \
    L"nothing, call show with an empty list and tell the user what you tried and what would help (other words, a period, a " \
    L"folder).\n"

#define AM_PROMPT_WORKFLOW_8_DATABASE \
    L"8. Nothing fits: try a shorter stem, other words for it and the other language (several searches in one step), fewer " \
    L"words, and fuzzy:true for a word that may be misspelled; make sure every call had days:3650; then more scopes (tool) " \
    L"and grep with a regex for the forms. When there is still nothing, call show with an empty list and tell the user what " \
    L"you tried and what would help (other words, a period, a folder).\n"

#define AM_PROMPT_WORKFLOWS_9_TO_15 \
    L"9. Several sessions fit equally well and the user wants one (to resume, fork or read): show them and ask which one, " \
    L"by number.\n" \
    L"10. Resume a session (\"open it again\", \"continue the session where ...\"): find and confirm it first unless it was " \
    L"just shown; list the tabs; when it is already open, say where (window and tab) and offer to bring it to the front; " \
    L"else choose the window (workflow 12) and the place (workflow 13), resume it, and reply with where it opened and that " \
    L"it waits in the background.\n" \
    L"11. Fork a session (\"fork it\", \"branch from that session\", \"a copy on opus\"): the same steps with fork. Say that the " \
    L"fork is a new conversation with the whole history of the source, and that the source keeps running untouched. Pass " \
    L"model when the user asked for one.\n" \
    L"12. The window: with one window open there is nothing to choose. With several, use the window the user named (\"window " \
    L"2\"; \"this window\" is the one the request came from; \"the other one\"). Otherwise ask: list each window by number with " \
    L"its tab count and its selected tab, mark the one the request came from or, from a phone, the one used last as the " \
    L"likely choice, and wait for the answer. Keep using the chosen window for the rest of the conversation unless the user " \
    L"names another.\n" \
    L"13. The place in the window: put the new tab right after the tab it belongs with - a fork right after its source (the " \
    L"default); else after the most recently active tab that works in the same folder (works_in, else dir); else after the " \
    L"tab of a related session (the same branch, a similar title); else at the end. Say in a few words where you put it and " \
    L"why.\n" \
    L"14. Deactivate a tab (\"deactivate the release tab\", \"park it\"): only on such an explicit request. Find the tab with " \
    L"list (by title, folder or session); when more than one fits, ask which. When the tab is running or waiting for " \
    L"approval, tell the user that its turn would be cut off and ask to confirm; only then call again with " \
    L"cut_off_turn:true. One tab per request: deactivate the first and ask the user to confirm each next one. Say that the " \
    L"tab resumes when it is entered.\n" \
    L"15. A question about the workspace (\"which tabs are open in window 2?\", \"what is running right now?\", \"which tab " \
    L"works in the catalog folder?\", \"what needs me?\"): list the tabs, narrowed with window, dir or title, and answer from " \
    L"the list; show the sessions too when the user may want to open them.\n"

#define AM_PROMPT_WORKFLOW_16_PAGE \
    L"16. Token use, cost, or a session's relatives (its forks, the session it was forked from): this search has no tool " \
    L"for them; say so, and read the session when its own text can answer (a fork's first messages are its source's).\n"

#define AM_PROMPT_WORKFLOW_16_DATABASE \
    L"16. Token use or cost (\"how many tokens did I use this week?\"): usage with the right group_by and period. A session's " \
    L"relatives (\"what was this forked from?\", \"its forks\"): related.\n"

#define AM_PROMPT_WORKFLOWS_TAIL \
    L"17. A pasted session id: search with it as the query (or read it), show it, and resume or fork it when asked.\n" \
    L"18. A request outside sessions and their tabs (\"fix this bug\", \"write a script\"): say that you find, explain and " \
    L"manage sessions, and offer to find a session about it or to resume one to work in.\n" \
    L"19. A tool error: when it says to try again, try once more; else tell the user plainly what failed (a window did not " \
    L"answer, the session database is not running) and what they can do.\n"

    // The page search's prompt: the shared pieces with the sessions server's tools, its way of casting the net (every
    // session by default, `more` / `also`, in:edits, in_period) and the workflows that use them.
    const wchar_t kSystemPrompt[] =
        AM_PROMPT_INTRO
        AM_PROMPT_BEHAVIORS
        AM_PROMPT_PROC_HEAD
        AM_PROMPT_PROC_NET_PAGE
        AM_PROMPT_PROC_TAIL
        AM_PROMPT_TOOLS_HEAD
        AM_PROMPT_TOOLS_PAGE
        AM_PROMPT_SHOW_AND_TABS
        AM_PROMPT_WORKFLOWS_HEAD
        AM_PROMPT_WORKFLOW_3_PAGE
        AM_PROMPT_WORKFLOW_4_PAGE
        AM_PROMPT_WORKFLOWS_5_TO_7
        AM_PROMPT_WORKFLOW_8_PAGE
        AM_PROMPT_WORKFLOWS_9_TO_15
        AM_PROMPT_WORKFLOW_16_PAGE
        AM_PROMPT_WORKFLOWS_TAIL;

    // The session database's prompt: the same shared pieces with the database's tools, its way of casting the net
    // (days:3650, the `next` cursor, who_touched, timeline) and the workflows that use them (more scopes, usage, related).
    // ⚠ It must never contain the word deep (the database's search has scopes).
    const wchar_t kDatabaseSystemPrompt[] =
        AM_PROMPT_INTRO
        AM_PROMPT_BEHAVIORS
        AM_PROMPT_PROC_HEAD
        AM_PROMPT_PROC_NET_DATABASE
        AM_PROMPT_PROC_TAIL
        AM_PROMPT_TOOLS_HEAD
        AM_PROMPT_TOOLS_DATABASE
        AM_PROMPT_SHOW_AND_TABS
        AM_PROMPT_WORKFLOWS_HEAD
        AM_PROMPT_WORKFLOW_3_DATABASE
        AM_PROMPT_WORKFLOW_4_DATABASE
        AM_PROMPT_WORKFLOWS_5_TO_7
        AM_PROMPT_WORKFLOW_8_DATABASE
        AM_PROMPT_WORKFLOWS_9_TO_15
        AM_PROMPT_WORKFLOW_16_DATABASE
        AM_PROMPT_WORKFLOWS_TAIL;

    // The pieces are text, not an interface: nothing else may use them.
#undef AM_PROMPT_INTRO
#undef AM_PROMPT_BEHAVIORS
#undef AM_PROMPT_PROC_HEAD
#undef AM_PROMPT_PROC_NET_PAGE
#undef AM_PROMPT_PROC_NET_DATABASE
#undef AM_PROMPT_PROC_TAIL
#undef AM_PROMPT_TOOLS_HEAD
#undef AM_PROMPT_READ_BODY
#undef AM_PROMPT_TOOLS_PAGE
#undef AM_PROMPT_TOOLS_DATABASE
#undef AM_PROMPT_SHOW_AND_TABS
#undef AM_PROMPT_WORKFLOWS_HEAD
#undef AM_PROMPT_WORKFLOW_3_PAGE
#undef AM_PROMPT_WORKFLOW_3_DATABASE
#undef AM_PROMPT_WORKFLOW_4_PAGE
#undef AM_PROMPT_WORKFLOW_4_DATABASE
#undef AM_PROMPT_WORKFLOWS_5_TO_7
#undef AM_PROMPT_WORKFLOW_8_PAGE
#undef AM_PROMPT_WORKFLOW_8_DATABASE
#undef AM_PROMPT_WORKFLOWS_9_TO_15
#undef AM_PROMPT_WORKFLOW_16_PAGE
#undef AM_PROMPT_WORKFLOW_16_DATABASE
#undef AM_PROMPT_WORKFLOWS_TAIL

    // The prompts state these numbers in words the model reads: change their text with them.
    static_assert(kMaxTabOpensPerRequest == 5, "AM_PROMPT_BEHAVIORS says the tools refuse more than 5 new tabs per request");
    static_assert(kTabListDefaultLimit == 40, "AM_PROMPT_SHOW_AND_TABS says list shows the 40 most recently active tabs by default");
    static_assert(kSessionShowMaxIds == 300, "AM_PROMPT_SHOW_AND_TABS says show takes up to 300 ids");
    static_assert(kSessionGrepBudgetMs == 20000, "AM_PROMPT_TOOLS_PAGE says a grep may stop at its 20 s budget");

    // The tabs server's schemas (SESSIONS.md §9.14). Their bounds and defaults are stated in words the model reads;
    // ListTabs / OpenTab / DeactivateTab apply the same numbers. No description may speak of a "screen" (the m5 suite
    // checks every tool): a request from a phone has none.
    const wchar_t kTabsListSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("window":{"type":"string","description":"Only this window: its number from an earlier list, or its id."},)"
        LR"("dir":{"type":"string","description":"Only tabs whose folder (dir or works_in) contains this text. '/' and '\\' are the same."},)"
        LR"("title":{"type":"string","description":"Only tabs whose title contains this text, any case."},)"
        LR"("limit":{"type":"integer","description":"Tabs per window, 1 to 200. Default 40: the selected tab and the most recently active ones, listed in strip order."})"
        LR"(},"required":[]})";

    const wchar_t kTabsResumeSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("id":{"type":"string","description":"The session to resume: a short id from a result, or a whole session id."},)"
        LR"("window":{"type":"string","description":"The window to open it in: its number or id from list. Required when more than one window is open."},)"
        LR"("after":{"type":"string","description":"Put the new tab right after this tab of that window: its pos (a number) or its session id. Leave it out to put it at the end."},)"
        LR"("focus":{"type":"boolean","description":"true: switch the window to the new tab and bring the window to the front. Default false: a background tab."})"
        LR"(},"required":["id"]})";

    const wchar_t kTabsForkSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("id":{"type":"string","description":"The session to fork: a short id from a result, or a whole session id. It may be open or closed."},)"
        LR"("window":{"type":"string","description":"The window to open the fork in: its number or id from list. Required when more than one window is open."},)"
        LR"("after":{"type":"string","description":"Put the fork right after this tab of that window: its pos (a number) or its session id. Leave it out: right after the source's tab when the source is open in that window, else at the end."},)"
        LR"("model":{"type":"string","description":"Start the fork on this model: an id or an alias such as opus or sonnet. Leave it out for the usual model."},)"
        LR"("focus":{"type":"boolean","description":"true: switch the window to the fork and bring the window to the front. Default false: a background tab."})"
        LR"(},"required":["id"]})";

    const wchar_t kTabsDeactivateSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("id":{"type":"string","description":"The session whose tab to deactivate: its id from list or another result."},)"
        LR"("cut_off_turn":{"type":"boolean","description":"true only after the user confirmed it: deactivate even though the tab's turn is running or waiting for approval (that turn is cut off)."})"
        LR"(},"required":["id"]})";

    // `ascii` (a SessionDbToolDefs.h string: ASCII by its contract) as a wide string; a byte above 0x7F becomes '?'
    // rather than a guess at an encoding. Never throws beyond std::bad_alloc.
    std::wstring WidenAscii(const char* ascii)
    {
        std::wstring out;
        for (const char* p = ascii; p && *p; ++p)
        {
            const auto b = static_cast<unsigned char>(*p);
            out.push_back(b < 0x80 ? static_cast<wchar_t>(b) : L'?');
        }
        return out;
    }
}

namespace Agentmaster
{
    // ===== the catalog ======================================================================

    std::wstring SessionSearchTitleOf(const SessionIndexEntry& entry)
    {
        if (!entry.liveTitle.empty())
        {
            return entry.liveTitle;
        }
        std::wstring title = PickDisplayTitle(entry.stats.customTitle, entry.stats.aiTitle, entry.stats.summary, entry.stats.firstUserPrompt);
        if (title.empty())
        {
            title = L"(no prompt yet)";
        }
        return title;
    }

    int64_t SessionSearchLastActivityMs(const SessionIndexEntry& entry) noexcept
    {
        return entry.stats.lastTimestampMs != 0 ? entry.stats.lastTimestampMs : entry.mtimeMs;
    }

    int64_t SessionSearchCreatedMs(const SessionIndexEntry& entry) noexcept
    {
        if (!entry.stats.forkedFromId.empty())
        {
            return entry.birthMs; // a fork's line timestamps are its parent's
        }
        return entry.stats.firstTimestampMs != 0 ? entry.stats.firstTimestampMs : entry.birthMs;
    }

    std::wstring SessionSearchDirOf(const SessionIndexEntry& entry)
    {
        if (!entry.stats.cwd.empty())
        {
            return entry.stats.cwd;
        }
        // <projects>\<encoded cwd>\<id>.jsonl: the folder's name is the lossy fallback.
        const size_t fileCut = entry.path.find_last_of(L"\\/");
        if (fileCut == std::wstring::npos || fileCut == 0)
        {
            return {}; // no folder part
        }
        const size_t dirCut = entry.path.find_last_of(L"\\/", fileCut - 1);
        const size_t start = dirCut == std::wstring::npos ? 0 : dirCut + 1;
        return entry.path.substr(start, fileCut - start);
    }

    std::shared_ptr<const SessionCatalog> GatherSessionCatalog(int64_t sinceUnixMs, const SessionCatalogSources& sources, const SessionCatalogGatherOptions& options)
    {
        const auto& previous = options.previous;
        auto catalog = std::make_shared<SessionCatalog>();
        catalog->sinceUnixMs = sinceUnixMs;
        catalog->coveredSinceUnixMs = sinceUnixMs; // raised below when a session is left out
        catalog->gatheredUnixMs = SystemNowUnixMs();
        // Newest modification time first — which is what makes a gather that stops early hold
        // the NEWEST sessions.
        const auto refs = sources.projectsDir.empty() ? EnumerateTranscripts(sinceUnixMs) : EnumerateTranscriptsIn(sources.projectsDir, sinceUnixMs);
        catalog->entries.reserve(refs.size());
        std::unordered_map<std::wstring, size_t> indexById;
        indexById.reserve(refs.size());
        std::unordered_map<std::wstring, size_t> previousById;
        if (previous)
        {
            previousById.reserve(previous->entries.size());
            for (size_t i = 0; i < previous->entries.size(); ++i)
            {
                previousById.emplace(previous->entries[i].sessionId, i);
            }
        }
        // The budget's clock: it starts at the first sidecar read (see budgetMs), so the listing
        // above and the copies from `previous` never use the budget up.
        bool readingStarted = false;
        std::chrono::steady_clock::time_point readingSince{};
        bool stopped = false; // the budget is spent, or the caller cancelled: read no more sidecars
        const auto mayRead = [&]() -> bool {
            if (stopped)
            {
                return false;
            }
            if (options.cancelled && options.cancelled())
            {
                stopped = true;
                return false;
            }
            if (!readingStarted)
            {
                readingStarted = true;
                readingSince = std::chrono::steady_clock::now();
                return true; // the one read a budget always allows
            }
            if (options.budgetMs > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - readingSince).count() >= options.budgetMs)
            {
                stopped = true;
                return false;
            }
            return true;
        };
        for (const auto& ref : refs)
        {
            SessionIndexEntry entry;
            const auto old = previousById.find(ref.sessionId);
            if (old != previousById.end() &&
                previous->entries[old->second].path == ref.path &&
                previous->entries[old->second].sizeBytes == ref.sizeBytes &&
                previous->entries[old->second].mtimeMs == ref.mtimeMs)
            {
                // Unchanged since the older catalog: the same (size, mtime) key the sidecar itself
                // is validated by. Its title overlay is applied again below, from fresh sources.
                // Taken even after the budget is spent: it costs no file read.
                entry = previous->entries[old->second];
                entry.liveTitle.clear();
            }
            else if (!mayRead())
            {
                // Left out. The catalog is then complete only for sessions that changed AFTER
                // this one. (mtime >= last activity always holds, so a bound on the modification
                // time is a bound on the activity time a search filters by.) The maximum makes
                // the bound right whatever the order of the listing.
                catalog->coveredSinceUnixMs = (std::max)(catalog->coveredSinceUnixMs, ref.mtimeMs + 1);
                continue;
            }
            else
            {
                entry = sources.indexDir.empty() ? LoadOrRefreshSessionIndex(ref) : LoadOrRefreshSessionIndexIn(sources.indexDir, ref);
            }
            if (!entry.valid)
            {
                continue; // deleted while we listed
            }
            indexById.emplace(entry.sessionId, catalog->entries.size());
            catalog->entries.push_back(std::move(entry));
        }
        // The session store (titles, tags, favorites). Its first read of a process opens every
        // record file, which a caller that gave a budget or a cancel check cannot wait for: such
        // a gather takes the store only when it was read before (then a read is one directory
        // listing), and says so in storeOverlay when it did not.
        const bool bounded = options.budgetMs > 0 || static_cast<bool>(options.cancelled);
        catalog->storeOverlay = !bounded || SessionStoreScanIsWarm();
        if (catalog->storeOverlay)
        {
            // The same overlays the Sessions page applies, in the same order: a stored title
            // first, then (below) the live tab title of an open session, the freshest name.
            for (const auto& [id, title] : LoadAllStoredSessionTitles())
            {
                if (title.empty())
                {
                    continue;
                }
                if (const auto it = indexById.find(id); it != indexById.end())
                {
                    catalog->entries[it->second].liveTitle = title;
                }
            }
            catalog->tags = LoadAllSessionTags();
            catalog->favorites = LoadAllFavoriteSessions();
        }
        if (sources.openSessions)
        {
            for (auto& [id, title] : sources.openSessions())
            {
                if (!title.empty())
                {
                    if (const auto it = indexById.find(id); it != indexById.end())
                    {
                        catalog->entries[it->second].liveTitle = title;
                    }
                }
                catalog->openIds.insert(std::move(id));
            }
        }
        return catalog;
    }

    // ===== one search =======================================================================

    std::optional<SessionSearchParams> ParseSessionSearchParams(const json::Value& arguments, int64_t nowUnixMs, std::wstring& error)
    {
        using Type = json::Value::Type;
        SessionSearchParams p;
        error.clear();
        const auto wrongType = [&error](const wchar_t* key, const wchar_t* expected) {
            error = std::wstring{ L"The argument \"" } + key + L"\" must be " + expected + L".";
        };
        if (const auto* v = arguments.Find(L"query"); v && v->type != Type::Null)
        {
            if (v->type != Type::Str)
            {
                wrongType(L"query", L"a string");
                return std::nullopt;
            }
            p.query = Trim(v->str);
        }
        if (const auto* v = arguments.Find(L"days"); v && v->type != Type::Null)
        {
            if (v->type != Type::Num)
            {
                wrongType(L"days", L"a whole number");
                return std::nullopt;
            }
            // 0 = every session (the default since 2026-10-18); a negative count reads as 0.
            p.days = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 0, 3650));
        }
        const auto date = [&](const wchar_t* key, bool endOfDay, int64_t& out) -> bool {
            const auto* v = arguments.Find(key);
            if (!v || v->type == Type::Null)
            {
                return true;
            }
            out = v->type == Type::Str ? ParseLocalDate(v->str, endOfDay) : 0;
            if (out == 0)
            {
                wrongType(key, L"a date written YYYY-MM-DD");
                return false;
            }
            return true;
        };
        if (!date(L"after", false, p.afterUnixMs) || !date(L"before", true, p.beforeUnixMs))
        {
            return std::nullopt;
        }
        if (p.afterUnixMs != 0 && p.beforeUnixMs != 0 && p.beforeUnixMs < p.afterUnixMs)
        {
            error = L"\"before\" is earlier than \"after\": swap them.";
            return std::nullopt;
        }
        const auto text = [&](const wchar_t* key, std::wstring& out) -> bool {
            const auto* v = arguments.Find(key);
            if (!v || v->type == Type::Null)
            {
                return true;
            }
            if (v->type != Type::Str)
            {
                wrongType(key, L"a string");
                return false;
            }
            out = Trim(v->str);
            return true;
        };
        if (!text(L"dir", p.dir) || !text(L"branch", p.branch) || !text(L"tag", p.tag))
        {
            return std::nullopt;
        }
        // A boolean given as the wrong type reads as false: nothing is lost by a wider search.
        p.favoriteOnly = arguments.BoolAt(L"favorite");
        p.openOnly = arguments.BoolAt(L"open");
        p.deep = arguments.BoolAt(L"deep");
        if (const std::wstring sort = arguments.StrAt(L"sort"); sort == L"active" || sort == L"created")
        {
            p.sort = sort;
        }
        if (const auto* v = arguments.Find(L"limit"); v && v->type == Type::Num)
        {
            p.limit = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, kSessionSearchMaxLimit));
        }
        (void)nowUnixMs; // the time bounds are resolved by SessionSearchSinceMs, at search time
        return p;
    }

    int64_t SessionSearchSinceMs(const SessionSearchParams& params, int64_t nowUnixMs) noexcept
    {
        if (params.afterUnixMs != 0)
        {
            return params.afterUnixMs;
        }
        if (params.days <= 0)
        {
            return 0; // every session
        }
        return nowUnixMs - static_cast<int64_t>(params.days) * 86400000LL;
    }

    SessionContentSearch DefaultSessionContentSearch()
    {
        SessionContentSearch content;
        content.prompts = [](const SessionQuery& q) {
            return SearchHistoryPrompts(HistoryPath(), q, 2);
        };
        content.transcripts = [](const std::vector<TranscriptRef>& refs, const SessionQuery& q, const std::function<bool()>& cancelled) {
            // SearchTranscriptsSlow calls its predicate between files: it must not be empty.
            static const std::function<bool()> never = [] { return false; };
            return SearchTranscriptsSlow(refs, q, 2, cancelled ? cancelled : never);
        };
        return content;
    }

    SessionSearchOutcome RunSessionSearch(const SessionCatalog& catalog, const SessionSearchParams& params, int64_t nowUnixMs, const SessionContentSearch& content, const std::function<bool()>& cancelled)
    {
        SessionSearchOutcome outcome;
        const auto& entries = catalog.entries;
        const int64_t since = SessionSearchSinceMs(params, nowUnixMs);
        const std::wstring dirLower = FoldLower(params.dir);
        const std::wstring branchLower = FoldLower(params.branch);
        const std::wstring tagFolded = params.tag.empty() ? std::wstring{} : FoldTagName(NormalizeTagName(params.tag));

        // 1. The scope: the time window and the filters. A query never widens it.
        std::vector<char> inScope(entries.size(), 0);
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            // The period OVERLAPS the session's activity: last active at or after its start, started at or before
            // its end (SessionSearchParams::beforeUnixMs).
            const int64_t active = SessionSearchLastActivityMs(e);
            if (active < since || (params.beforeUnixMs != 0 && SessionSearchCreatedMs(e) > params.beforeUnixMs))
            {
                continue;
            }
            if (!dirLower.empty() && !MatchesPathQuery(FoldLower(SessionSearchDirOf(e)), dirLower, false))
            {
                continue;
            }
            if (!branchLower.empty() && !MatchesQueryText(FoldLower(e.stats.gitBranch), branchLower, false))
            {
                continue;
            }
            if (!tagFolded.empty())
            {
                bool carries = false;
                if (const auto it = catalog.tags.find(e.sessionId); it != catalog.tags.end())
                {
                    for (const auto& tag : it->second)
                    {
                        carries = carries || FoldTagName(tag) == tagFolded;
                    }
                }
                if (!carries)
                {
                    continue;
                }
            }
            if (params.favoriteOnly && catalog.favorites.count(e.sessionId) == 0)
            {
                continue;
            }
            if (params.openOnly && catalog.openIds.count(e.sessionId) == 0)
            {
                continue;
            }
            inScope[i] = 1;
            ++outcome.searched;
        }

        // 2. The matches.
        const auto terms = ParseSessionQuery(params.query);
        std::vector<SessionSearchRow> rows;
        if (terms.empty())
        {
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (inScope[i])
                {
                    rows.push_back(SessionSearchRow{ i, SessionMatchTier::Listed, 0, {} });
                }
            }
        }
        else
        {
            // The index: title, folder, touched paths, id. Over the whole catalog (the function
            // takes the entries as they are); the scope is applied to its answer.
            SessionQuery q;
            q.text = params.query;
            q.scopeTitle = true;
            q.scopeDirs = true;
            q.scopeFiles = true;
            std::unordered_set<std::wstring> fast;
            for (auto& id : SearchIndexFast(entries, q))
            {
                fast.insert(std::move(id));
            }
            // The typed prompts: always, they are one prebuilt file.
            std::unordered_map<std::wstring, SessionHit> promptHits;
            if (content.prompts)
            {
                SessionQuery qp = q;
                qp.scopeUser = true;
                promptHits = content.prompts(qp);
            }
            // The transcripts' text: only on request, and only the sessions in scope. The one
            // step of a search that can take many seconds, so the one that can be cancelled.
            std::unordered_map<std::wstring, SessionHit> textHits;
            if (params.deep && content.transcripts && !(cancelled && cancelled()))
            {
                std::vector<TranscriptRef> refs;
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    if (!inScope[i])
                    {
                        continue;
                    }
                    TranscriptRef r;
                    r.sessionId = entries[i].sessionId;
                    r.path = entries[i].path;
                    r.sizeBytes = entries[i].sizeBytes;
                    r.mtimeMs = entries[i].mtimeMs;
                    r.birthMs = entries[i].birthMs;
                    refs.push_back(std::move(r));
                }
                SessionQuery qd = q;
                qd.scopeUser = true;
                qd.scopeAgent = true;
                for (auto& hit : content.transcripts(refs, qd, cancelled))
                {
                    std::wstring id = hit.sessionId;
                    textHits.emplace(std::move(id), std::move(hit));
                }
            }
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (!inScope[i])
                {
                    continue;
                }
                const auto& e = entries[i];
                const bool isFast = fast.count(e.sessionId) != 0;
                const auto prompt = promptHits.find(e.sessionId);
                const auto text = textHits.find(e.sessionId);
                const bool hasPrompt = prompt != promptHits.end();
                const bool hasText = text != textHits.end();
                if (!isFast && !hasPrompt && !hasText)
                {
                    continue;
                }
                SessionSearchRow row;
                row.entryIndex = i;
                row.tier = (isFast && NameMatches(e, terms)) ? SessionMatchTier::Name : (isFast ? SessionMatchTier::Path : (hasPrompt ? SessionMatchTier::Prompt : SessionMatchTier::Text));
                for (const SessionHit* hit : { hasPrompt ? &prompt->second : nullptr, hasText ? &text->second : nullptr })
                {
                    if (!hit)
                    {
                        continue;
                    }
                    row.hits += hit->hitCount;
                    for (const auto& s : hit->snippets)
                    {
                        if (row.snippets.size() < 2)
                        {
                            row.snippets.push_back(s);
                        }
                    }
                }
                rows.push_back(std::move(row));
            }
        }
        outcome.matched = rows.size();

        // 3. The order, then the cut.
        const bool byCreated = params.sort == L"created";
        const bool byTier = params.sort != L"active" && !byCreated;
        std::stable_sort(rows.begin(), rows.end(), [&](const SessionSearchRow& a, const SessionSearchRow& b) {
            if (byTier && a.tier != b.tier)
            {
                return static_cast<int>(a.tier) < static_cast<int>(b.tier);
            }
            const int64_t ta = byCreated ? SessionSearchCreatedMs(entries[a.entryIndex]) : SessionSearchLastActivityMs(entries[a.entryIndex]);
            const int64_t tb = byCreated ? SessionSearchCreatedMs(entries[b.entryIndex]) : SessionSearchLastActivityMs(entries[b.entryIndex]);
            return ta > tb; // newest first
        });
        const size_t limit = static_cast<size_t>(std::clamp(params.limit, 1, kSessionSearchMaxLimit));
        if (rows.size() > limit)
        {
            rows.resize(limit);
        }
        // A Path row's evidence, for the returned rows only (a broad query matches hundreds): the working folder when a
        // query word is in it, else the first touched path that holds one - what the result's `path` says.
        for (auto& row : rows)
        {
            if (row.tier != SessionMatchTier::Path)
            {
                continue;
            }
            const auto& e = entries[row.entryIndex];
            const auto holdsTerm = [&terms](const std::wstring& path) {
                const std::wstring lower = FoldLower(path);
                for (const auto& t : terms)
                {
                    if (!t.isGuid && MatchesPathQuery(lower, t.textLower, false))
                    {
                        return true;
                    }
                }
                return false;
            };
            if (const std::wstring dir = SessionSearchDirOf(e); !dir.empty() && holdsTerm(dir))
            {
                row.evidencePath = dir;
                row.evidenceIsFolder = true;
                continue;
            }
            for (const auto& path : e.stats.pathsAccessed)
            {
                if (holdsTerm(path))
                {
                    row.evidencePath = path;
                    break;
                }
            }
            // A touched path says where the session went, not what it was for; its first prompt does
            // (SessionSearchRow::firstPrompt). Only when no prompt or text snippet says it already: the history-prompt
            // search attributes a prompt to the session the user typed it in, never to a fork that copied it.
            if (row.snippets.empty() && !e.stats.firstUserPrompt.empty())
            {
                const std::wstring& first = e.stats.firstUserPrompt;
                const std::wstring firstLower = FoldLower(first);
                for (const auto& t : terms)
                {
                    if (!t.isGuid && MatchesQueryText(firstLower, t.textLower, false))
                    {
                        // Cut around the word, so a word late in a long prompt is still in what the model reads.
                        row.firstPrompt = MakeSnippet(first, t.textLower, false, 160);
                        row.firstPromptHolds = true;
                        break;
                    }
                }
                if (row.firstPrompt.empty())
                {
                    row.firstPrompt = Cut(OneLine(first), 160);
                }
            }
        }
        outcome.rows = std::move(rows);
        return outcome;
    }

    void CheckSessionSearchPeriodActivity(const SessionCatalog& catalog, const SessionSearchParams& params, int64_t nowUnixMs, SessionSearchOutcome& outcome, const std::function<bool()>& cancelled)
    {
        if ((params.afterUnixMs == 0 && params.beforeUnixMs == 0) || outcome.rows.empty() || outcome.rows.size() > kSessionPeriodCheckMaxRows)
        {
            return; // no explicit period, nothing to check, or a list too long for one read each
        }
        const int64_t from = SessionSearchSinceMs(params, nowUnixMs);
        const int64_t to = params.beforeUnixMs != 0 ? params.beforeUnixMs : nowUnixMs;
        // One transcript per row, several at once (each read is a byte scan of one file; a period's rows are usually a
        // few dozen). Each thread writes only the rows it took.
        std::atomic<size_t> next{ 0 };
        const auto worker = [&]() noexcept {
            try
            {
                for (;;)
                {
                    if (cancelled && cancelled())
                    {
                        return;
                    }
                    const size_t k = next.fetch_add(1);
                    if (k >= outcome.rows.size())
                    {
                        return;
                    }
                    SessionSearchRow& row = outcome.rows[k];
                    const SessionIndexEntry& e = catalog.entries[row.entryIndex];
                    // A fork's copied lines keep the parent's times: the session's own lines start at its file's birth
                    // (a minute of slack for the clock of the file's creation against the first line's).
                    const int64_t ownSince = e.birthMs > 60000 ? e.birthMs - 60000 : 0;
                    row.period = ReadPeriodActivity(e.path, from, to, ownSince, cancelled);
                    row.periodChecked = row.period.read;
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the rows this thread did not finish stay unchecked (the result then says nothing
                // about them), never a wrong "no activity".
                LogSwallowedException(L"CheckSessionSearchPeriodActivity: a reader thread");
            }
        };
        const size_t threads = (std::min)(outcome.rows.size(), std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 2, 8));
        std::vector<std::thread> pool;
        for (size_t t = 1; t < threads; ++t)
        {
            try
            {
                pool.emplace_back(worker);
            }
            catch (...)
            {
                LogSwallowedException(L"CheckSessionSearchPeriodActivity: start a reader thread");
                break; // the threads started, and this one, read the rest
            }
        }
        worker();
        for (auto& th : pool)
        {
            th.join();
        }
    }

    std::wstring SessionSearchHandleOf(const std::wstring& sessionId)
    {
        return sessionId.substr(0, (std::min)(sessionId.size(), size_t{ 8 }));
    }

    std::wstring DescribeSessionSearch(const SessionSearchParams& params)
    {
        std::wstring out = params.query.empty() ? std::wstring{ L"all sessions" } : (L"\x201C" + params.query + L"\x201D");
        if (params.afterUnixMs != 0)
        {
            out += L" \x00B7 from " + LocalDateText(params.afterUnixMs);
        }
        else if (params.days > 0)
        {
            out += L" \x00B7 last " + std::to_wstring(params.days) + (params.days == 1 ? L" day" : L" days");
        }
        else
        {
            out += L" \x00B7 every session";
        }
        if (params.beforeUnixMs != 0)
        {
            out += L" \x00B7 until " + LocalDateText(params.beforeUnixMs);
        }
        if (!params.dir.empty())
        {
            out += L" \x00B7 in " + params.dir;
        }
        if (!params.branch.empty())
        {
            out += L" \x00B7 branch " + params.branch;
        }
        if (!params.tag.empty())
        {
            out += L" \x00B7 tag " + params.tag;
        }
        if (params.favoriteOnly)
        {
            out += L" \x00B7 favorites";
        }
        if (params.openOnly)
        {
            out += L" \x00B7 open";
        }
        if (params.deep)
        {
            out += L" \x00B7 full text";
        }
        return out;
    }

    std::wstring FormatSessionSearchResult(const SessionCatalog& catalog, const SessionSearchParams& params, const SessionSearchOutcome& outcome, int64_t nowUnixMs, const std::function<std::wstring(const std::wstring&)>& handleOf)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"now", Value::MkStr(LocalDateTimeText(nowUnixMs)));
        std::wstring window = params.afterUnixMs != 0 ? (L"from " + LocalDateText(params.afterUnixMs)) : (params.days > 0 ? (L"last " + std::to_wstring(params.days) + L" days") : std::wstring{ L"every session" });
        if (params.beforeUnixMs != 0)
        {
            window += L" until " + LocalDateText(params.beforeUnixMs);
        }
        root.Set(L"window", Value::MkStr(std::move(window)));
        root.Set(L"searched", Value::MkNum(static_cast<double>(outcome.searched)));
        root.Set(L"matched", Value::MkNum(static_cast<double>(outcome.matched)));
        root.Set(L"returned", Value::MkNum(static_cast<double>(outcome.rows.size())));
        // The catalog does not reach back as far as the search asked: say so, or the model
        // reads "nothing matched" as "there is no such session" and tells the user that.
        const bool partial = catalog.coveredSinceUnixMs > SessionSearchSinceMs(params, nowUnixMs);
        // The tags and the favorites are not loaded yet, and this search filters by one of them:
        // its empty result means "cannot be answered yet", not "no such session".
        const bool storeFilterBlind = !catalog.storeOverlay && (!params.tag.empty() || params.favoriteOnly);
        if (partial || storeFilterBlind)
        {
            std::wstring indexing;
            if (partial)
            {
                indexing = L"Sessions last active before " + LocalDateTimeText(catalog.coveredSinceUnixMs) + L" are not in this result: they are still being indexed. A search a little later has them.";
            }
            if (storeFilterBlind)
            {
                indexing += std::wstring{ indexing.empty() ? L"" : L" " } + L"Tags and favorites are still being loaded, so the tag / favorite filter of this search matched nothing: it is still being indexed too.";
            }
            root.Set(L"indexing", Value::MkStr(std::move(indexing)));
        }
        if (outcome.matched > outcome.rows.size())
        {
            // The rows are cut: the model must not read "returned" as everything (the 2026-10-18 eval: 145 matched, 15
            // returned, 8 shown). How to get the rest, in words it can act on.
            root.Set(L"more", Value::MkStr(L"Only " + std::to_wstring(outcome.rows.size()) + L" of the " + std::to_wstring(outcome.matched) +
                                           L" matching sessions are returned. Every one that fits must reach the user: call again with limit up to " +
                                           std::to_wstring(kSessionSearchMaxLimit) + L" (or narrow with dir, a period or more words) to see the rest."));
        }
        if (outcome.matched == 0)
        {
            // With a partial index, a longer period cannot help: do not suggest it.
            const bool everySession = params.afterUnixMs == 0 && params.days <= 0;
            root.Set(L"hint", Value::MkStr((partial || storeFilterBlind) ? L"Nothing matched in the indexed sessions. Try other or fewer words." :
                                                                           (params.deep ? (everySession ? L"Nothing matched. Try other or fewer words." : L"Nothing matched. Try other or fewer words, or a longer period (days).") :
                                                                                          (everySession ? L"Nothing matched. Try other or fewer words, grep, or deep:true." : L"Nothing matched. Try other or fewer words, a longer period (days), or deep:true."))));
        }
        Value sessions = Value::MkArr();
        for (const auto& row : outcome.rows)
        {
            const auto& e = catalog.entries[row.entryIndex];
            Value o = Value::MkObj();
            o.Set(L"id", Value::MkStr(handleOf ? handleOf(e.sessionId) : SessionSearchHandleOf(e.sessionId)));
            o.Set(L"title", Value::MkStr(Cut(OneLine(SessionSearchTitleOf(e)), 120)));
            o.Set(L"dir", Value::MkStr(SessionSearchDirOf(e)));
            if (!e.stats.gitBranch.empty())
            {
                o.Set(L"branch", Value::MkStr(e.stats.gitBranch));
            }
            const int64_t active = SessionSearchLastActivityMs(e);
            // "2026-10-12 01:08 (5m ago)". A session active within the last minute reads "(now)" —
            // "(now ago)" is what the model was given before 2026-10-12 — and a session with no
            // known activity gets no parentheses at all.
            const std::wstring ago = AgoText(active, nowUnixMs);
            o.Set(L"active", Value::MkStr(LocalDateTimeText(active) + (ago.empty() ? std::wstring{} : (ago == L"now" ? std::wstring{ L" (now)" } : (L" (" + ago + L" ago)")))));
            o.Set(L"created", Value::MkStr(LocalDateText(SessionSearchCreatedMs(e))));
            o.Set(L"msgs", Value::MkNum(e.stats.userPrompts));
            o.Set(L"tools", Value::MkNum(e.stats.toolUses));
            if (row.tier != SessionMatchTier::Listed)
            {
                o.Set(L"match", Value::MkStr(TierWord(row.tier)));
            }
            if (!row.evidencePath.empty() && row.evidencePath != SessionSearchDirOf(e))
            {
                // Which touched path matched (the folder is the row's `dir` already): the evidence of a path match.
                o.Set(L"path", Value::MkStr(Cut(row.evidencePath, 200)));
            }
            if (!row.firstPrompt.empty())
            {
                // What a session that matched on a touched path was for, in the user's words (SessionSearchRow::firstPrompt).
                o.Set(L"first_prompt", Value::MkStr(row.firstPrompt));
            }
            if (row.periodChecked)
            {
                // What the session did INSIDE the asked period: its own lines there, or none (it only spans the period).
                if (row.period.messages > 0)
                {
                    Value inPeriod = Value::MkObj();
                    inPeriod.Set(L"messages", Value::MkNum(row.period.messages));
                    inPeriod.Set(L"prompts", Value::MkNum(row.period.prompts));
                    o.Set(L"in_period", std::move(inPeriod));
                }
                else
                {
                    o.Set(L"in_period", Value::MkStr(L"none: not active inside the period, it only spans it"));
                }
            }
            // Flags and lists only when they say something: fewer tokens for the model to read.
            if (catalog.openIds.count(e.sessionId) != 0)
            {
                o.Set(L"open", Value::MkBool(true));
            }
            if (catalog.favorites.count(e.sessionId) != 0)
            {
                o.Set(L"fav", Value::MkBool(true));
            }
            if (!e.stats.forkedFromId.empty())
            {
                o.Set(L"fork", Value::MkBool(true));
            }
            if (const auto it = catalog.tags.find(e.sessionId); it != catalog.tags.end() && !it->second.empty())
            {
                Value tags = Value::MkArr();
                for (const auto& tag : it->second)
                {
                    tags.Push(Value::MkStr(tag));
                }
                o.Set(L"tags", std::move(tags));
            }
            if (row.hits > 0)
            {
                o.Set(L"hits", Value::MkNum(row.hits));
            }
            if (!row.snippets.empty())
            {
                Value snippets = Value::MkArr();
                for (const auto& s : row.snippets)
                {
                    snippets.Push(Value::MkStr(Cut(OneLine(s), 160)));
                }
                o.Set(L"snippets", std::move(snippets));
            }
            sessions.Push(std::move(o));
        }
        root.Set(L"sessions", std::move(sessions));
        return json::Dump(root);
    }

    // ===== grep and read =====================================================================

    std::optional<SessionGrepParams> ParseSessionGrepParams(const json::Value& arguments, std::wstring& error)
    {
        using Type = json::Value::Type;
        SessionGrepParams p;
        error.clear();
        const auto* pattern = arguments.Find(L"pattern");
        if (!pattern || pattern->type != Type::Str || Trim(pattern->str).empty())
        {
            error = L"The argument \"pattern\" is required: the text to find (or a regex with \"regex\": true).";
            return std::nullopt;
        }
        p.regex = arguments.BoolAt(L"regex");
        // Plain text is trimmed (an outer space only narrows what matches, and the model adds none on purpose);
        // a regex is taken as written, its spaces may be part of what it means.
        p.pattern = p.regex ? pattern->str : Trim(pattern->str);
        if (const auto* ids = arguments.Find(L"ids"); ids && ids->type != Type::Null)
        {
            if (ids->type != Type::Arr)
            {
                error = L"The argument \"ids\" must be an array of ids from search or grep results.";
                return std::nullopt;
            }
            for (const auto& v : ids->arr)
            {
                if (v.type == Type::Str && !Trim(v.str).empty() && p.ids.size() < 20)
                {
                    p.ids.push_back(Trim(v.str));
                }
            }
        }
        // The period and the folder are the search's arguments under the same names: one parser, one set of rules
        // and messages (a wrong type names the argument; after/before are local dates).
        const auto period = ParseSessionSearchParams(arguments, 0, error);
        if (!period)
        {
            return std::nullopt;
        }
        p.days = period->days;
        p.afterUnixMs = period->afterUnixMs;
        p.beforeUnixMs = period->beforeUnixMs;
        p.dir = period->dir;
        const std::wstring in = FoldLower(Trim(arguments.StrAt(L"in")));
        // "edits" (2026-10-18): the paths of the files Claude changed, not text (GrepOptions::editsOnly).
        p.editsOnly = in == L"edits" || in == L"edit" || in == L"edited";
        p.user = !p.editsOnly && in != L"claude";
        p.agent = p.editsOnly || in != L"user";
        if (const auto* v = arguments.Find(L"context"); v && v->type == Type::Num)
        {
            p.context = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 40, 400));
        }
        if (const auto* v = arguments.Find(L"limit"); v && v->type == Type::Num)
        {
            p.limit = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, kSessionGrepMaxLimit));
        }
        return p;
    }

    std::optional<SessionReadParams> ParseSessionReadParams(const json::Value& arguments, std::wstring& error)
    {
        using Type = json::Value::Type;
        SessionReadParams p;
        error.clear();
        const auto* ids = arguments.Find(L"ids");
        if (!ids || ids->type != Type::Arr)
        {
            error = L"Pass \"ids\": an array of 1 to " + std::to_wstring(kSessionReadMaxIds) + L" ids from search or grep results.";
            return std::nullopt;
        }
        std::unordered_set<std::wstring> seen;
        for (const auto& v : ids->arr)
        {
            if (v.type != Type::Str)
            {
                continue;
            }
            std::wstring id = Trim(v.str);
            if (id.empty() || !seen.insert(FoldLower(id)).second)
            {
                continue; // a blank entry, or one the model wrote twice
            }
            if (p.ids.size() < kSessionReadMaxIds)
            {
                p.ids.push_back(std::move(id));
            }
            else
            {
                ++p.extraIds;
            }
        }
        if (p.ids.empty())
        {
            error = L"\"ids\" names no session: pass 1 to " + std::to_wstring(kSessionReadMaxIds) + L" ids from search or grep results.";
            return std::nullopt;
        }
        const std::wstring level = FoldLower(Trim(arguments.StrAt(L"level")));
        p.level = level == L"minimal" ? SessionReadLevel::Minimal : (level == L"default" ? SessionReadLevel::Default : SessionReadLevel::Compact);
        if (const auto* v = arguments.Find(L"chars"); v && v->type == Type::Num)
        {
            p.chars = static_cast<size_t>(std::clamp<int64_t>(v->AsI64(), 1000, 40000));
        }
        return p;
    }

    SessionReadCut CutRenderedTranscript(std::wstring_view markdown, size_t budgetChars)
    {
        SessionReadCut cut;
        const size_t budget = (std::max)(budgetChars, size_t{ 1000 });
        // The turns: every line that starts with "### " (TranscriptTurns' turn heading, and "### Work before the
        // first prompt"). A message's own Markdown never starts such a line: the renderer quotes it ("> ...").
        std::vector<size_t> starts;
        for (size_t p = markdown.find(L"### "); p != std::wstring_view::npos; p = markdown.find(L"### ", p + 4))
        {
            if (p == 0 || markdown[p - 1] == L'\n')
            {
                starts.push_back(p);
            }
        }
        cut.turns = starts.size();
        if (markdown.size() <= budget)
        {
            cut.text.assign(markdown);
            return cut;
        }
        // Appends `piece`, or its first `room` characters and a cut marker (never splitting a surrogate pair).
        const auto append = [&](std::wstring_view piece, size_t room) {
            if (piece.size() <= room)
            {
                cut.text.append(piece);
                return;
            }
            size_t keep = room;
            if (keep > 0 && piece[keep - 1] >= 0xD800 && piece[keep - 1] <= 0xDBFF)
            {
                --keep;
            }
            cut.text.append(piece.substr(0, keep));
            cut.text.append(L"\n[... cut ...]\n\n");
            cut.cutInside = true;
        };
        if (starts.empty())
        {
            append(markdown, budget); // no turn to keep whole: the document's head
            return cut;
        }
        const auto turnAt = [&](size_t k) {
            const size_t from = starts[k];
            const size_t to = k + 1 < starts.size() ? starts[k + 1] : markdown.size();
            return markdown.substr(from, to - from);
        };
        // What comes before the first turn (a legend, a note): at most a quarter of the budget.
        append(markdown.substr(0, starts[0]), budget / 4);
        const size_t room = budget > cut.text.size() ? budget - cut.text.size() : 0;
        const size_t frontRoom = room * 2 / 5; // the first turns: what the session was about
        const size_t backRoom = room - frontRoom; // the last turns: how it ended
        // The front: the first turn always (cut if it alone is too long), then more while they fit.
        size_t frontEnd = 0;
        size_t frontUsed = 0;
        while (frontEnd < starts.size())
        {
            const size_t len = turnAt(frontEnd).size();
            if (frontEnd > 0 && frontUsed + len > frontRoom)
            {
                break;
            }
            frontUsed += len;
            ++frontEnd;
        }
        // The back: the last turn always (when it is not a front turn), then earlier ones while they fit.
        size_t backBegin = starts.size();
        size_t backUsed = 0;
        while (backBegin > frontEnd)
        {
            const size_t len = turnAt(backBegin - 1).size();
            if (backBegin < starts.size() && backUsed + len > backRoom)
            {
                break;
            }
            backUsed += len;
            --backBegin;
        }
        for (size_t k = 0; k < frontEnd; ++k)
        {
            append(turnAt(k), k == 0 ? frontRoom : turnAt(k).size()); // only the first can be cut; the rest fit
        }
        cut.omittedTurns = backBegin - frontEnd;
        if (cut.omittedTurns > 0)
        {
            cut.text.append(L"[... " + std::to_wstring(cut.omittedTurns) + (cut.omittedTurns == 1 ? L" turn" : L" turns") + L" left out here ...]\n\n");
        }
        for (size_t k = backBegin; k < starts.size(); ++k)
        {
            append(turnAt(k), k == starts.size() - 1 && backBegin == k ? backRoom : turnAt(k).size()); // a lone last turn can be cut
        }
        return cut;
    }

    // ===== windows and tabs (SESSIONS.md §9.14) =============================================

    namespace
    {
        /// <summary>A window id folded for comparison: lower case, braces and spaces dropped.</summary>
        /// <param name="text">A record id ("{8A2B...}") or what the model wrote.</param>
        /// <returns>The folded text.</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring FoldWindowIdText(std::wstring_view text)
        {
            std::wstring bare;
            bare.reserve(text.size());
            for (const wchar_t c : text)
            {
                if (c != L'{' && c != L'}' && c != L' ' && c != L'\t')
                {
                    bare.push_back(c);
                }
            }
            return FoldLower(bare);
        }

        /// <summary>True when `text` is 1 to 6 ASCII digits — a window or tab NUMBER rather than an id. Six digits at
        /// most, so the conversion can never overflow.</summary>
        /// <param name="text">The trimmed argument.</param>
        /// <returns>Whether it is a small number.</returns>
        bool IsSmallNumber(std::wstring_view text) noexcept
        {
            if (text.empty() || text.size() > 6)
            {
                return false;
            }
            for (const wchar_t c : text)
            {
                if (c < L'0' || c > L'9')
                {
                    return false;
                }
            }
            return true;
        }

        /// <summary>The value of a string IsSmallNumber accepted.</summary>
        /// <param name="text">1 to 6 ASCII digits.</param>
        /// <returns>The number.</returns>
        size_t SmallNumberValue(std::wstring_view text) noexcept
        {
            size_t n = 0;
            for (const wchar_t c : text)
            {
                n = n * 10 + static_cast<size_t>(c - L'0');
            }
            return n;
        }

        /// <summary>
        /// A tool argument the model may send either as a JSON string or as a number ("after": 3 and "after": "3" mean
        /// the same tab), as trimmed text; "" when it is absent or of another type.
        /// </summary>
        /// <param name="arguments">The model's argument object.</param>
        /// <param name="key">The argument's name.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring StringOrNumberArgument(const json::Value& arguments, const wchar_t* key)
        {
            const auto* v = arguments.Find(key);
            if (!v)
            {
                return {};
            }
            if (v->type == json::Value::Type::Str)
            {
                return Trim(v->str);
            }
            if (v->type == json::Value::Type::Num && v->AsI64() >= 0)
            {
                return std::to_wstring(v->AsI64());
            }
            return {};
        }

        /// <summary>A path folded for a substring test: lower case, '/' as '\'.</summary>
        /// <param name="text">A folder path or a piece of one.</param>
        /// <returns>The folded text.</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring FoldPathText(std::wstring_view text)
        {
            std::wstring out = FoldLower(Trim(text));
            std::replace(out.begin(), out.end(), L'/', L'\\');
            return out;
        }

        /// <summary>Where a session's tab is: its window and its entry in that window's tab list.</summary>
        struct TabLocation
        {
            size_t window{ 0 }; // index into the census
            size_t tab{ 0 }; // index into that window's tabs
        };

        /// <summary>The tab of `sessionId` in the census (a session's tab lives in exactly one window).</summary>
        /// <param name="windows">The census.</param>
        /// <param name="sessionId">The whole session id; "" finds nothing.</param>
        /// <returns>Where it is, or nullopt when no window has a tab for it.</returns>
        std::optional<TabLocation> FindSessionTab(const std::vector<WindowCensus>& windows, const std::wstring& sessionId) noexcept
        {
            if (sessionId.empty())
            {
                return std::nullopt;
            }
            for (size_t w = 0; w < windows.size(); ++w)
            {
                for (size_t t = 0; t < windows[w].tabs.size(); ++t)
                {
                    if (windows[w].tabs[t].sessionId == sessionId)
                    {
                        return TabLocation{ w, t };
                    }
                }
            }
            return std::nullopt;
        }

        /// <summary>"window 2, tab 5 "Agentmaster"" — how a result names a tab for the model (and, through it, the user).</summary>
        /// <param name="windows">The census.</param>
        /// <param name="where">The tab.</param>
        /// <returns>The phrase.</returns>
        /// <exception cref="std::bad_alloc">If the string cannot be allocated.</exception>
        std::wstring DescribeTabPlace(const std::vector<WindowCensus>& windows, const TabLocation& where)
        {
            const auto& tab = windows[where.window].tabs[where.tab];
            return L"window " + std::to_wstring(where.window + 1) + L", tab " + std::to_wstring(tab.index + 1) + L" \"" + Cut(OneLine(tab.title), 80) + L"\"";
        }
    }

    std::wstring WindowHandleOf(const std::wstring& windowId)
    {
        // "{8A2B1C3D-...}" -> "8a2b1c3d": the braces are presentation, and the model writes the id back in any case.
        const std::wstring bare = FoldWindowIdText(windowId);
        return bare.size() > 8 ? bare.substr(0, 8) : bare;
    }

    std::wstring DescribeWindowsForModel(const std::vector<WindowCensus>& windows, const std::wstring& originWindowId)
    {
        std::wstring out;
        for (size_t i = 0; i < windows.size(); ++i)
        {
            const auto& w = windows[i];
            out += L"window " + std::to_wstring(i + 1) + L" (id " + WindowHandleOf(w.windowId) + L"): " + std::to_wstring(w.tabs.size()) + (w.tabs.size() == 1 ? L" tab" : L" tabs");
            // The tab a window shows is what the user recognizes it by ("the window with the Agentmaster tab").
            std::wstring selected;
            for (const auto& t : w.tabs)
            {
                if (t.selected)
                {
                    selected = t.title;
                    break;
                }
            }
            if (w.managerPageVisible)
            {
                out += L", showing its Agent Manager page";
            }
            else if (!selected.empty())
            {
                out += L", \"" + Cut(OneLine(selected), 60) + L"\" selected";
            }
            if (w.lastActive)
            {
                out += L"; the user used it last";
            }
            if (!originWindowId.empty() && w.windowId == originWindowId)
            {
                out += L"; this request came from it";
            }
            out += L"\n";
        }
        return out;
    }

    std::optional<size_t> ResolveWindowArgument(const std::vector<WindowCensus>& windows, std::wstring_view argument, const std::wstring& originWindowId, std::wstring& error)
    {
        if (windows.empty())
        {
            error = L"No Agentmaster window answered, so no tab can be opened in one. Try once more; if it fails again, tell the user that the windows did not answer.";
            return std::nullopt;
        }
        std::wstring arg = FoldLower(Trim(argument));
        // Forgiving about the shapes a model writes a number in: "2", "#2", "window 2".
        if (arg.rfind(L"window", 0) == 0)
        {
            arg = Trim(std::wstring_view{ arg }.substr(6));
        }
        if (!arg.empty() && arg.front() == L'#')
        {
            arg = Trim(std::wstring_view{ arg }.substr(1));
        }
        if (arg.empty())
        {
            if (windows.size() == 1)
            {
                return size_t{ 0 }; // one window: nothing to ask
            }
            // The user's rule (2026-10-17): with several windows, the USER names one. The refusal carries everything the
            // model needs to ask well: every window by number, and the likely one marked.
            error = L"Not opened: there are " + std::to_wstring(windows.size()) + L" Agentmaster windows open and the user has not said which one to use. Do not choose one yourself. Ask the user which window, describing them by number as below (the one this request came from, or else the one used last, is the likely choice), then call again with window set to the number or the id:\n" +
                    DescribeWindowsForModel(windows, originWindowId);
            return std::nullopt;
        }
        if (IsSmallNumber(arg))
        {
            const size_t n = SmallNumberValue(arg);
            if (n >= 1 && n <= windows.size())
            {
                return n - 1;
            }
            error = L"There is no window " + arg + L". The open windows are:\n" + DescribeWindowsForModel(windows, originWindowId);
            return std::nullopt;
        }
        // An id: the short one, a longer prefix or the whole one, braces optional, any case (4 characters at least, so a
        // stray word never matches).
        const std::wstring bare = FoldWindowIdText(arg);
        std::optional<size_t> found;
        if (bare.size() >= 4)
        {
            for (size_t i = 0; i < windows.size(); ++i)
            {
                if (FoldWindowIdText(windows[i].windowId).rfind(bare, 0) == 0)
                {
                    if (found)
                    {
                        error = L"The window id " + Cut(arg, 40) + L" fits more than one window: pass its number instead. The open windows are:\n" + DescribeWindowsForModel(windows, originWindowId);
                        return std::nullopt;
                    }
                    found = i;
                }
            }
        }
        if (found)
        {
            return found;
        }
        error = L"No open window has the id " + Cut(arg, 40) + L". The open windows are:\n" + DescribeWindowsForModel(windows, originWindowId);
        return std::nullopt;
    }

    // ===== the tool servers =================================================================

    SessionSearchToolset::SessionSearchToolset(Callbacks callbacks) :
        _callbacks{ std::move(callbacks) }
    {
        // The catalog feeds the page search only: the database mode never asks for one.
        if ((!_callbacks.catalog && !_callbacks.database.call) || !_callbacks.onShow)
        {
            throw std::invalid_argument("SessionSearchToolset needs the catalog (or the database's call) and onShow callbacks.");
        }
        const SessionContentSearch defaults = DefaultSessionContentSearch();
        if (!_callbacks.content.prompts)
        {
            _callbacks.content.prompts = defaults.prompts;
        }
        if (!_callbacks.content.transcripts)
        {
            _callbacks.content.transcripts = defaults.transcripts;
        }
    }

    int64_t SessionSearchToolset::Now() const
    {
        return _callbacks.now ? _callbacks.now() : SystemNowUnixMs();
    }

    const wchar_t* SessionSearchToolset::SystemPrompt() noexcept
    {
        return kSystemPrompt;
    }

    const wchar_t* SessionSearchToolset::DatabaseSystemPrompt() noexcept
    {
        return kDatabaseSystemPrompt;
    }

    std::wstring SessionSearchToolset::DescribeDatabaseCall(const std::string& tool, const json::Value& arguments)
    {
        using Type = json::Value::Type;
        const auto quoted = [](const std::wstring& text) { return L"\"" + Cut(OneLine(text), 60) + L"\""; };
        const auto window = [&arguments]() -> std::wstring {
            const std::wstring after = arguments.StrAt(L"after");
            const std::wstring before = arguments.StrAt(L"before");
            if (!after.empty() || !before.empty())
            {
                return (after.empty() ? std::wstring{ L"..." } : after) + L" to " + (before.empty() ? std::wstring{ L"today" } : before);
            }
            const auto* days = arguments.Find(L"days");
            const int64_t n = days && days->type == Type::Num ? days->AsI64() : 0;
            return n > 0 ? L"last " + std::to_wstring(n) + L" days" : std::wstring{};
        };
        std::wstring text;
        if (tool == "search")
        {
            const std::wstring q = arguments.StrAt(L"query");
            text = q.empty() ? std::wstring{ L"all sessions" } : quoted(q);
            const std::wstring w = window();
            text += L" \x00B7 " + (w.empty() ? std::wstring{ L"last 90 days" } : w);
            if (const std::wstring dir = arguments.StrAt(L"dir"); !dir.empty())
            {
                text += L" \x00B7 in " + Cut(dir, 60);
            }
            if (const auto* scopes = arguments.Find(L"scopes"); scopes && scopes->type == Type::Arr)
            {
                for (const auto& s : scopes->arr)
                {
                    if (s.type == Type::Str && (s.str == L"tool" || s.str == L"subagent"))
                    {
                        text += L" \x00B7 with " + s.str + L" text";
                    }
                }
            }
            return text;
        }
        if (tool == "grep")
        {
            return L"grep " + quoted(arguments.StrAt(L"pattern"));
        }
        if (tool == "who_touched")
        {
            return L"who touched " + quoted(arguments.StrAt(L"path"));
        }
        if (tool == "timeline")
        {
            const std::wstring from = arguments.StrAt(L"from");
            const std::wstring to = arguments.StrAt(L"to");
            return L"timeline " + (from.empty() ? std::wstring{ L"of the last week" } : from + L" to " + (to.empty() ? std::wstring{ L"today" } : to));
        }
        if (tool == "sql")
        {
            return L"a database query";
        }
        if (tool == "usage")
        {
            return L"token use by " + arguments.StrAt(L"group_by", L"session");
        }
        // session / messages / files / related: one session.
        const std::wstring id = Cut(Trim(arguments.StrAt(L"id")), 12);
        return (tool == "files" ? std::wstring{ L"the files of " } : tool == "related" ? std::wstring{ L"the relatives of " } : std::wstring{ L"reading " }) +
               (id.empty() ? std::wstring{ L"a session" } : id);
    }

    void SessionSearchToolset::RememberHandles(const std::vector<std::wstring>& handles, const std::vector<std::wstring>& fullIds)
    {
        std::lock_guard lock{ _mutex };
        for (size_t i = 0; i < handles.size() && i < fullIds.size(); ++i)
        {
            if (!fullIds[i].empty())
            {
                _handles[FoldLower(Trim(handles[i]))] = fullIds[i];
            }
        }
    }

    HarnessToolResult SessionSearchToolset::DatabaseCall(const std::string& tool, const json::Value& arguments)
    {
        // "Is this call still wanted?" (Search's rule: the handler blocks the SDK's reader thread, so a replaced
        // request must not leave a trace.)
        std::function<bool()> cancelled;
        if (_callbacks.generation)
        {
            const uint64_t startGeneration = _callbacks.generation();
            cancelled = [this, startGeneration] { return _callbacks.generation() != startGeneration; };
        }
        if (_callbacks.onSearching)
        {
            _callbacks.onSearching(DescribeDatabaseCall(tool, arguments));
        }
        HarnessToolResult result = _callbacks.database.call(tool, arguments);
        if (cancelled && cancelled())
        {
            return { L"The call was cancelled: the request was stopped or replaced.", true };
        }
        if (result.isError || tool != "search")
        {
            return result;
        }
        // A search's rows: their short ids become show's ids (and top:N its order), and the page previews them.
        std::vector<std::wstring> handles;
        if (const auto parsed = json::Parse(result.text); parsed && parsed->type == json::Value::Type::Obj)
        {
            if (const auto* rows = parsed->Find(L"rows"); rows && rows->type == json::Value::Type::Arr)
            {
                for (const auto& row : rows->arr)
                {
                    if (const std::wstring id = row.StrAt(L"id"); !id.empty())
                    {
                        handles.push_back(id);
                    }
                }
            }
        }
        std::vector<std::wstring> ids;
        if (!handles.empty() && _callbacks.database.resolve)
        {
            const std::vector<std::wstring> full = _callbacks.database.resolve(handles);
            RememberHandles(handles, full);
            for (const auto& id : full)
            {
                if (!id.empty())
                {
                    ids.push_back(id);
                }
            }
        }
        {
            std::lock_guard lock{ _mutex };
            _lastSearchIds = ids;
        }
        if (_callbacks.onCandidates && !ids.empty())
        {
            _callbacks.onCandidates(ids);
        }
        return result;
    }

    void SessionSearchToolset::Reset()
    {
        std::lock_guard lock{ _mutex };
        _handles.clear();
        _lastSearchIds.clear();
        _known.clear();
    }

    std::wstring SessionSearchToolset::AssignHandleLocked(const std::wstring& id)
    {
        // The short id is the first 8 characters. When another session of this
        // conversation already has it, this one gets a longer prefix, until it is unique.
        std::wstring handle;
        for (size_t len = 8; len <= id.size(); len += 5)
        {
            handle = FoldLower(id.substr(0, len));
            const auto it = _handles.find(handle);
            if (it == _handles.end() || it->second == id)
            {
                break;
            }
            handle.clear();
        }
        if (handle.empty())
        {
            handle = FoldLower(id);
        }
        _handles[handle] = id;
        return handle;
    }

    void SessionSearchToolset::NoteEvidenceLocked(const std::wstring& sessionId, std::wstring line, int rank)
    {
        if (sessionId.empty() || line.empty())
        {
            return;
        }
        KnownSession& known = _known[sessionId];
        // A stronger line (a lower rank) replaces a weaker one; an equal one keeps the FIRST (the best-ranked result row).
        if (rank < known.evidenceRank)
        {
            known.evidence = Cut(std::move(line), kSessionWhyMaxChars);
            known.evidenceRank = rank;
        }
    }

    std::vector<std::wstring> SessionSearchToolset::ResolveWrittenIds(const std::vector<std::wstring>& written, std::vector<std::wstring>& unknown)
    {
        if (_callbacks.database.resolve)
        {
            // The database's own tools print short ids no search of this conversation recorded (show's rule).
            std::vector<std::wstring> missing;
            for (const auto& w : written)
            {
                if (ResolveHandle(w).empty())
                {
                    missing.push_back(Trim(w));
                }
            }
            if (!missing.empty())
            {
                RememberHandles(missing, _callbacks.database.resolve(missing));
            }
        }
        std::vector<std::wstring> out;
        std::unordered_set<std::wstring> seen;
        for (const auto& w : written)
        {
            std::wstring full = ResolveHandle(w);
            if (full.empty())
            {
                const std::wstring text = Trim(w);
                if (IsGuidToken(text))
                {
                    // A whole session id that no result printed (the user pasted one): taken as it is, and given a
                    // short id so `show` takes it too. TranscriptPathOf finds its file, or the reader says it is gone.
                    full = FoldLower(text.front() == L'{' ? text.substr(1, text.size() - 2) : text);
                    std::lock_guard lock{ _mutex };
                    AssignHandleLocked(full);
                }
            }
            if (full.empty())
            {
                if (unknown.size() < 5)
                {
                    unknown.push_back(Cut(Trim(w), 40));
                }
                continue;
            }
            if (seen.insert(full).second)
            {
                out.push_back(full);
            }
        }
        return out;
    }

    std::wstring SessionSearchToolset::TranscriptPathOf(const std::wstring& sessionId) const
    {
        {
            std::lock_guard lock{ _mutex };
            if (const auto it = _known.find(sessionId); it != _known.end() && !it->second.path.empty())
            {
                return it->second.path;
            }
        }
        // Not from a result of this conversation (the database mode, an id the model typed whole): where Claude
        // keeps it — or the test's scratch folder.
        return _callbacks.transcriptPath ? _callbacks.transcriptPath(sessionId) : ResolveClaudeTranscriptPath(sessionId);
    }

    std::wstring SessionSearchToolset::ResolveHandle(const std::wstring& handle) const
    {
        const std::wstring key = FoldLower(Trim(handle));
        std::lock_guard lock{ _mutex };
        if (const auto it = _handles.find(key); it != _handles.end())
        {
            return it->second;
        }
        // The model may write a whole id, or more characters than the short id has: accept any
        // id of this conversation that the text is a prefix of, when it names exactly one.
        std::wstring found;
        if (key.size() >= 8)
        {
            for (const auto& entry : _handles)
            {
                const std::wstring& id = entry.second;
                if (FoldLower(id).rfind(key, 0) == 0)
                {
                    if (!found.empty() && found != id)
                    {
                        return {}; // two sessions: not an id
                    }
                    found = id;
                }
            }
        }
        return found;
    }

    HarnessToolResult SessionSearchToolset::Search(const json::Value& arguments)
    {
        const int64_t now = Now();
        std::wstring error;
        const auto params = ParseSessionSearchParams(arguments, now, error);
        if (!params)
        {
            return { error, true };
        }
        // "Is this search still wanted?" The handler runs on the SDK's reader thread, and while
        // it runs that thread can deliver nothing else — not even the interrupt of this very
        // request. So every slow step below asks, and a search that is not wanted any more
        // returns at once.
        std::function<bool()> cancelled;
        if (_callbacks.generation)
        {
            const uint64_t startGeneration = _callbacks.generation();
            cancelled = [this, startGeneration] { return _callbacks.generation() != startGeneration; };
        }
        // The text of a cancelled search. An error result: the model must not treat it as "no
        // session matched" (its turn is being interrupted anyway).
        const auto cancelledResult = [] {
            return HarnessToolResult{ L"The search was cancelled: the request was stopped or replaced.", true };
        };
        if (_callbacks.onSearching)
        {
            _callbacks.onSearching(DescribeSessionSearch(*params));
        }
        const auto catalog = _callbacks.catalog(SessionSearchSinceMs(*params, now), cancelled);
        if (cancelled && cancelled())
        {
            return cancelledResult();
        }
        if (!catalog)
        {
            // No index at all: its first read is still running (or could not start). The model
            // must not answer "no such session".
            return { L"The session index is still being read, so the search could not run. Do not search again now and do not call show: reply that the session index is still being read and that the user can ask again in a few seconds.", true };
        }
        if (catalog->coveredSinceUnixMs > SessionSearchSinceMs(*params, now))
        {
            // NEVER a part of the sessions (the user's rule of 2026-10-18, "searches must never miss"): the index is still
            // being read after the catalog callback's wait (90 s in the app), so nothing is searched. The model calls
            // again — that call waits again — instead of showing what a part of the sessions holds.
            return { L"The session index is still being read: it holds the sessions active since " + LocalDateTimeText(catalog->coveredSinceUnixMs) +
                         L" (" + std::to_wstring(catalog->entries.size()) + L" so far), and this search asks for older ones too. Nothing was searched, so do not answer from it and do not call show yet: call search again with the same arguments (the call waits for the index).",
                     true };
        }
        SessionSearchOutcome outcome = RunSessionSearch(*catalog, *params, now, _callbacks.content, cancelled);
        // A dated search: which rows said anything INSIDE the period (a session that only spans it reads 0) — the model's
        // evidence for "what did I do on ...?" (SESSIONS.md 9.15).
        CheckSessionSearchPeriodActivity(*catalog, *params, now, outcome, cancelled);
        if (cancelled && cancelled())
        {
            // Before the short ids are recorded and the page is told: a cancelled search must
            // leave no trace (its rows may be half of what a full scan finds).
            return cancelledResult();
        }

        std::vector<std::wstring> ids;
        ids.reserve(outcome.rows.size());
        std::unordered_map<std::wstring, std::wstring> handleById;
        {
            std::lock_guard lock{ _mutex };
            for (const auto& row : outcome.rows)
            {
                const SessionIndexEntry& entry = catalog->entries[row.entryIndex];
                const std::wstring& id = entry.sessionId;
                ids.push_back(id);
                handleById[id] = AssignHandleLocked(id);
                // Where its transcript is: `read` and a `grep` with ids open it without a new catalog. Updated in place:
                // the evidence an earlier result recorded stays.
                KnownSession& known = _known[id];
                known.path = entry.path;
                known.title = Cut(OneLine(SessionSearchTitleOf(entry)), 120);
                known.activeMs = SessionSearchLastActivityMs(entry);
                known.sizeBytes = entry.sizeBytes;
                known.dir = SessionSearchDirOf(entry); // a later grep of its id prints it (KnownSession::dir)
                // The row's evidence, as the auto-why of a session the model shows without one (Show).
                if (row.tier == SessionMatchTier::Prompt && !row.snippets.empty())
                {
                    NoteEvidenceLocked(id, L"You asked: \x201C" + Cut(OneLine(row.snippets.front()), 110) + L"\x201D", 0);
                }
                else if (row.tier == SessionMatchTier::Path && row.evidenceIsFolder)
                {
                    // The folder is the evidence; the session's own first prompt says what it did there (a hundred cards of
                    // one folder would otherwise all read the same line).
                    const std::wstring& first = entry.stats.firstUserPrompt;
                    NoteEvidenceLocked(id, L"Launched in " + Cut(row.evidencePath, 60) + (first.empty() ? std::wstring{} : (L"; you asked: \x201C" + Cut(OneLine(first), 90) + L"\x201D")), 2);
                }
                else if (row.tier == SessionMatchTier::Path && row.firstPromptHolds)
                {
                    // The user's own first prompt names the subject: stronger than the path it touched.
                    NoteEvidenceLocked(id, L"You asked: \x201C" + Cut(row.firstPrompt, 110) + L"\x201D", 0);
                }
                else if (row.tier == SessionMatchTier::Path && !row.evidencePath.empty())
                {
                    NoteEvidenceLocked(id, L"Touched " + Cut(row.evidencePath, 110), 3);
                }
                else if (row.tier == SessionMatchTier::Text && !row.snippets.empty())
                {
                    NoteEvidenceLocked(id, L"In the conversation: \x201C" + Cut(OneLine(row.snippets.front()), 100) + L"\x201D", 4);
                }
                else if (row.tier == SessionMatchTier::Name)
                {
                    NoteEvidenceLocked(id, L"Its title: \x201C" + Cut(OneLine(SessionSearchTitleOf(entry)), 100) + L"\x201D", 5);
                }
                else if (row.tier == SessionMatchTier::Listed && !entry.stats.firstUserPrompt.empty())
                {
                    // A listing by the filters alone (a period, a folder, a mark): its first prompt, in the user's words, says
                    // what the session was for — the weakest evidence, replaced by any match a later call shows. A dated
                    // listing says first how much of it fell inside the period.
                    std::wstring line;
                    if (row.periodChecked && row.period.prompts > 0)
                    {
                        line = std::to_wstring(row.period.prompts) + (row.period.prompts == 1 ? L" prompt" : L" prompts") + L" in the period; ";
                    }
                    line += L"You asked: \x201C" + Cut(OneLine(entry.stats.firstUserPrompt), 110) + L"\x201D";
                    NoteEvidenceLocked(id, std::move(line), 6);
                }
            }
            _lastSearchIds = ids;
        }
        if (_callbacks.onCandidates)
        {
            _callbacks.onCandidates(ids);
        }
        const auto handleOf = [&handleById](const std::wstring& id) {
            const auto it = handleById.find(id);
            return it != handleById.end() ? it->second : SessionSearchHandleOf(id);
        };
        return { FormatSessionSearchResult(*catalog, *params, outcome, now, handleOf), false };
    }

    HarnessToolResult SessionSearchToolset::Grep(const json::Value& arguments)
    {
        using json::Value;
        const int64_t now = Now();
        std::wstring error;
        const auto params = ParseSessionGrepParams(arguments, error);
        if (!params)
        {
            return { error, true };
        }
        // "Is this grep still wanted?" (Search's rule: the handler blocks the SDK's reader thread, so a replaced
        // request must stop it, and must leave no trace.)
        std::function<bool()> cancelled;
        if (_callbacks.generation)
        {
            const uint64_t startGeneration = _callbacks.generation();
            cancelled = [this, startGeneration] { return _callbacks.generation() != startGeneration; };
        }
        const auto cancelledResult = [] {
            return HarnessToolResult{ L"The grep was cancelled: the request was stopped or replaced.", true };
        };
        // The period and the folder, by the search's own rules.
        SessionSearchParams period;
        period.days = params->days;
        period.afterUnixMs = params->afterUnixMs;
        period.beforeUnixMs = params->beforeUnixMs;
        period.dir = params->dir;
        const int64_t since = SessionSearchSinceMs(period, now);
        std::wstring window;
        if (params->ids.empty())
        {
            window = params->afterUnixMs != 0 ? (L"from " + LocalDateText(params->afterUnixMs)) : (params->days > 0 ? (L"last " + std::to_wstring(params->days) + L" days") : std::wstring{ L"every session" });
            if (params->beforeUnixMs != 0)
            {
                window += L" until " + LocalDateText(params->beforeUnixMs);
            }
            if (!params->dir.empty())
            {
                window += L" in " + params->dir;
            }
        }
        else
        {
            window = std::to_wstring(params->ids.size()) + (params->ids.size() == 1 ? L" session asked for" : L" sessions asked for");
        }
        if (_callbacks.onSearching)
        {
            _callbacks.onSearching(L"grep \x201C" + Cut(OneLine(params->pattern), 60) + L"\x201D \x00B7 " + window);
        }

        // The transcripts to read, and what each one's row says (title, activity): the ids' (what a result of this
        // conversation recorded), or the catalog's of the period, NEWEST first so a grep that runs out of time has
        // read the newest sessions.
        std::vector<std::wstring> unknown;
        std::vector<TranscriptRef> refs;
        std::unordered_map<std::wstring, KnownSession> facts;
        std::shared_ptr<const SessionCatalog> catalog; // the period's (no ids), kept for the matching rows' facts
        bool partial = false;
        int64_t coveredSince = 0;
        if (!params->ids.empty())
        {
            const std::vector<std::wstring> ids = ResolveWrittenIds(params->ids, unknown);
            if (ids.empty())
            {
                std::wstring text = L"None of these ids is from a result of this conversation:";
                for (const auto& u : unknown)
                {
                    text += L" " + u;
                }
                text += L". Search first, then pass the ids exactly as a result printed them.";
                return { text, true };
            }
            for (const auto& id : ids)
            {
                KnownSession known;
                {
                    std::lock_guard lock{ _mutex };
                    if (const auto it = _known.find(id); it != _known.end())
                    {
                        known = it->second;
                    }
                }
                if (known.path.empty())
                {
                    known.path = TranscriptPathOf(id); // takes _mutex itself: outside the lock above
                }
                if (known.path.empty())
                {
                    if (unknown.size() < 5)
                    {
                        unknown.push_back(SessionSearchHandleOf(id) + L" (its transcript is gone)");
                    }
                    continue;
                }
                TranscriptRef ref;
                ref.sessionId = id;
                ref.path = known.path;
                ref.sizeBytes = known.sizeBytes;
                refs.push_back(std::move(ref));
                facts.emplace(id, std::move(known));
            }
        }
        else
        {
            catalog = _callbacks.catalog ? _callbacks.catalog(since, cancelled) : std::shared_ptr<const SessionCatalog>{};
            if (cancelled && cancelled())
            {
                return cancelledResult();
            }
            if (!catalog)
            {
                // No index at all yet (Search's message): the model must not read "no match" as "no such session".
                return { L"The session index is still being read, so the grep could not run over a period. Grep the ids of a search instead, or reply that the session index is still being read.", true };
            }
            if (catalog->coveredSinceUnixMs > since)
            {
                // Never a part of the sessions (Search's rule): call again, the call waits for the index.
                return { L"The session index is still being read: it holds the sessions active since " + LocalDateTimeText(catalog->coveredSinceUnixMs) +
                             L", and this grep asks for older ones too. Nothing was read, so do not answer from it: call grep again with the same arguments (the call waits for the index), or grep the ids of a search.",
                         true };
            }
            const std::wstring dirLower = FoldLower(params->dir);
            for (const auto& e : catalog->entries)
            {
                // The period OVERLAPS the session's activity (Search's rule).
                const int64_t active = SessionSearchLastActivityMs(e);
                if (active < since || (params->beforeUnixMs != 0 && SessionSearchCreatedMs(e) > params->beforeUnixMs))
                {
                    continue;
                }
                if (!dirLower.empty() && !MatchesPathQuery(FoldLower(SessionSearchDirOf(e)), dirLower, false))
                {
                    continue;
                }
                TranscriptRef ref;
                ref.sessionId = e.sessionId;
                ref.path = e.path;
                ref.sizeBytes = e.sizeBytes;
                ref.mtimeMs = e.mtimeMs;
                ref.birthMs = e.birthMs;
                refs.push_back(std::move(ref));
            }
            partial = catalog->coveredSinceUnixMs > since;
            coveredSince = catalog->coveredSinceUnixMs;
        }

        GrepOptions options;
        options.pattern = params->pattern;
        options.regex = params->regex;
        options.user = params->user;
        options.agent = params->agent;
        options.editsOnly = params->editsOnly;
        options.contextChars = static_cast<size_t>(params->context);
        options.budgetMs = kSessionGrepBudgetMs;
        GrepOutcome outcome = GrepTranscripts(refs, options, cancelled);
        if (catalog)
        {
            // The rows' facts, for the sessions that matched only (a period holds thousands that did not).
            std::unordered_map<std::wstring, size_t> byId;
            byId.reserve(catalog->entries.size());
            for (size_t i = 0; i < catalog->entries.size(); ++i)
            {
                byId.emplace(catalog->entries[i].sessionId, i);
            }
            for (const auto& s : outcome.sessions)
            {
                if (const auto it = byId.find(s.sessionId); it != byId.end())
                {
                    const SessionIndexEntry& e = catalog->entries[it->second];
                    KnownSession known{ e.path, Cut(OneLine(SessionSearchTitleOf(e)), 120), SessionSearchLastActivityMs(e), e.sizeBytes };
                    known.dir = SessionSearchDirOf(e); // the row prints it (KnownSession::dir)
                    facts.emplace(s.sessionId, std::move(known));
                }
            }
        }
        return FinishGrep(*params, std::move(outcome), refs.size(), window, partial, coveredSince, unknown, facts, now, cancelled);
    }

    HarnessToolResult SessionSearchToolset::FinishGrep(const SessionGrepParams& params, GrepOutcome outcome, size_t searched, const std::wstring& window, bool partial, int64_t coveredSince, const std::vector<std::wstring>& unknown, const std::unordered_map<std::wstring, KnownSession>& facts, int64_t now, const std::function<bool()>& cancelled)
    {
        using json::Value;
        if (cancelled && cancelled())
        {
            // Before the short ids are recorded: a cancelled grep must leave no trace (Search's rule).
            return { L"The grep was cancelled: the request was stopped or replaced.", true };
        }
        if (!outcome.error.empty())
        {
            return { outcome.error, true };
        }
        // The order the model reads: sessions where the USER typed the text first (a person remembers a session by
        // their own words), then by matching messages; the refs' order (newest first) breaks the ties.
        std::vector<const GrepSessionHit*> order;
        order.reserve(outcome.sessions.size());
        for (const auto& s : outcome.sessions)
        {
            order.push_back(&s);
        }
        std::stable_sort(order.begin(), order.end(), [](const GrepSessionHit* a, const GrepSessionHit* b) {
            if ((a->userHits > 0) != (b->userHits > 0))
            {
                return a->userHits > 0;
            }
            return a->hits > b->hits;
        });
        // The rows past the limit are not dropped: up to kGrepAlsoMax of them are listed as one compact line each
        // (`also`: short id, title, how many messages match), so every matching session reaches the model - and no
        // session the user wants is lost behind a limit (the 2026-10-18 eval: the session wanted was the 21st of 117).
        constexpr size_t kGrepAlsoMax = 150;
        std::vector<const GrepSessionHit*> rest;
        if (order.size() > static_cast<size_t>(params.limit))
        {
            rest.assign(order.begin() + params.limit, order.begin() + static_cast<std::ptrdiff_t>((std::min)(order.size(), static_cast<size_t>(params.limit) + kGrepAlsoMax)));
            order.resize(static_cast<size_t>(params.limit));
        }

        Value root = Value::MkObj();
        root.Set(L"now", Value::MkStr(LocalDateTimeText(now)));
        root.Set(L"pattern", Value::MkStr(params.pattern));
        if (params.regex)
        {
            root.Set(L"regex", Value::MkBool(true));
        }
        root.Set(L"in", Value::MkStr(params.editsOnly ? L"edits" : (params.user && params.agent ? L"all" : (params.user ? L"user" : L"claude"))));
        root.Set(L"window", Value::MkStr(window));
        root.Set(L"searched", Value::MkNum(static_cast<double>(searched)));
        root.Set(L"read", Value::MkNum(static_cast<double>(outcome.scanned)));
        root.Set(L"matched", Value::MkNum(static_cast<double>(outcome.sessions.size())));
        root.Set(L"returned", Value::MkNum(static_cast<double>(order.size())));
        if (!outcome.complete)
        {
            // The budget ran out: the model must not read "matched N" as all there is.
            const size_t notRead = (outcome.candidates > outcome.scanned ? outcome.candidates - outcome.scanned : 0) + outcome.notReached;
            root.Set(L"stopped", Value::MkStr(L"Out of time after " + std::to_wstring(kSessionGrepBudgetMs / 1000) + L" s: up to " + std::to_wstring(notRead) +
                                              (params.ids.empty() ? L" of the oldest sessions of the period" : L" of the sessions asked for") +
                                              L" were not read, so more sessions may match. Narrow it with ids, dir or a shorter period to read them."));
        }
        if (partial)
        {
            root.Set(L"indexing", Value::MkStr(L"Sessions last active before " + LocalDateTimeText(coveredSince) + L" are not in this result: they are still being indexed. A grep a little later has them."));
        }
        if (outcome.tooComplex > 0)
        {
            root.Set(L"too_complex", Value::MkStr(std::to_wstring(outcome.tooComplex) + L" messages were too long or too complex for this regex and count as no match: make it simpler."));
        }
        if (!unknown.empty())
        {
            Value names = Value::MkArr();
            for (const auto& u : unknown)
            {
                names.Push(Value::MkStr(u));
            }
            root.Set(L"unknown_ids", std::move(names));
        }
        if (outcome.sessions.empty() && outcome.complete)
        {
            root.Set(L"hint", Value::MkStr(params.regex ? L"No message holds it. Try a simpler regex, other words, or a longer period (days)." : L"No message holds it. Try fewer or other words, a longer period (days), or a regex."));
        }
        if (outcome.sessions.size() > order.size())
        {
            // The rows are cut: the model must not read "returned" as everything.
            const size_t unlisted = outcome.sessions.size() - order.size() - rest.size();
            root.Set(L"more", Value::MkStr(L"Only " + std::to_wstring(order.size()) + L" of the " + std::to_wstring(outcome.sessions.size()) +
                                           L" matching sessions have their matches here; " + std::to_wstring(rest.size()) + L" more are listed in `also` (id, title, folder, matching messages)" +
                                           (unlisted > 0 ? L", and " + std::to_wstring(unlisted) + L" are not listed at all" : std::wstring{}) +
                                           L". Every one that fits must reach the user: grep the ids in `also` (or call again with limit up to " + std::to_wstring(kSessionGrepMaxLimit) +
                                           L") to see their matches."));
        }
        if (outcome.sessions.size() >= kSessionGrepFoldersMinMatches)
        {
            // The matching sessions per working folder - ALL of them, not only the rows: a request about one project is
            // judged from the rows by their dir, and this says at once how many each folder holds (the 2026-10-18 eval:
            // twice a grep with 119 matches read as "too big", and a second grep "narrowed" to dir:agentmaster with fewer
            // words lost a session the first one had found). Folded keys, so one folder's spellings count together.
            std::unordered_map<std::wstring, std::pair<std::wstring, size_t>> byDir; // folded dir -> (dir as written, sessions)
            for (const auto& s : outcome.sessions)
            {
                const auto it = facts.find(s.sessionId);
                const std::wstring dir = (it != facts.end() && !it->second.dir.empty()) ? it->second.dir : std::wstring{ L"(folder unknown)" };
                auto& slot = byDir[FoldLower(dir)];
                if (slot.second == 0)
                {
                    slot.first = dir;
                }
                ++slot.second;
            }
            std::vector<std::pair<std::wstring, size_t>> ranked;
            ranked.reserve(byDir.size());
            for (auto& [key, value] : byDir)
            {
                ranked.push_back(std::move(value));
            }
            // Most sessions first; a tie by the folder's name, so the order is the same on every call.
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
            Value folders = Value::MkArr();
            size_t restSessions = 0;
            for (size_t i = 0; i < ranked.size(); ++i)
            {
                if (i < kSessionGrepFoldersMax)
                {
                    folders.Push(Value::MkStr(Cut(ranked[i].first, 120) + L": " + std::to_wstring(ranked[i].second)));
                }
                else
                {
                    restSessions += ranked[i].second;
                }
            }
            if (ranked.size() > kSessionGrepFoldersMax)
            {
                folders.Push(Value::MkStr(std::to_wstring(restSessions) + L" in " + std::to_wstring(ranked.size() - kSessionGrepFoldersMax) + L" other folders"));
            }
            root.Set(L"folders", std::move(folders));
        }
        // The rows' facts first (no lock held), then the short ids and the transcript places (under _mutex).
        std::vector<KnownSession> rowFacts;
        rowFacts.reserve(order.size());
        for (const auto* s : order)
        {
            const auto it = facts.find(s->sessionId);
            rowFacts.push_back(it != facts.end() ? it->second : KnownSession{});
        }
        Value sessions = Value::MkArr();
        {
            std::lock_guard lock{ _mutex };
            for (size_t i = 0; i < order.size(); ++i)
            {
                const GrepSessionHit& s = *order[i];
                const KnownSession& f = rowFacts[i];
                Value o = Value::MkObj();
                o.Set(L"id", Value::MkStr(AssignHandleLocked(s.sessionId)));
                if (!f.path.empty())
                {
                    // `read` and a later grep with ids open it from here. Updated in place: recorded evidence stays.
                    KnownSession& known = _known[s.sessionId];
                    known.path = f.path;
                    if (!f.title.empty())
                    {
                        known.title = f.title;
                    }
                    if (f.activeMs > 0)
                    {
                        known.activeMs = f.activeMs;
                    }
                    if (f.sizeBytes > 0)
                    {
                        known.sizeBytes = f.sizeBytes;
                    }
                    if (!f.dir.empty())
                    {
                        known.dir = f.dir;
                    }
                }
                // The grep's evidence for the auto-why: the user's own words first, an edit of the file, else Claude's.
                for (const auto& x : s.excerpts)
                {
                    if (params.editsOnly)
                    {
                        NoteEvidenceLocked(s.sessionId, L"Claude changed it: " + Cut(OneLine(x.text), 110), 1);
                        break;
                    }
                    if (x.user)
                    {
                        NoteEvidenceLocked(s.sessionId, L"You wrote: \x201C" + Cut(OneLine(x.text), 110) + L"\x201D", 0);
                        break;
                    }
                }
                if (!params.editsOnly && !s.excerpts.empty())
                {
                    NoteEvidenceLocked(s.sessionId, L"Claude: \x201C" + Cut(OneLine(s.excerpts.front().text), 110) + L"\x201D", 4);
                }
                if (!f.title.empty())
                {
                    o.Set(L"title", Value::MkStr(f.title));
                }
                if (!f.dir.empty())
                {
                    // Where it ran: "the sessions of project X" is judged from the rows, not by a second grep (KnownSession::dir).
                    o.Set(L"dir", Value::MkStr(Cut(f.dir, 120)));
                }
                if (f.activeMs > 0)
                {
                    o.Set(L"active", Value::MkStr(ActiveWithAgo(f.activeMs, now)));
                }
                o.Set(L"hits", Value::MkNum(s.hits));
                if (s.userHits > 0)
                {
                    o.Set(L"user_hits", Value::MkNum(s.userHits));
                }
                Value matches = Value::MkArr();
                for (const auto& x : s.excerpts)
                {
                    Value m = Value::MkObj();
                    m.Set(L"by", Value::MkStr(x.user ? L"user" : L"claude"));
                    if (x.timestampMs > 0)
                    {
                        m.Set(L"at", Value::MkStr(LocalDateTimeText(x.timestampMs)));
                    }
                    m.Set(L"text", Value::MkStr(x.text));
                    matches.Push(std::move(m));
                }
                o.Set(L"matches", std::move(matches));
                sessions.Push(std::move(o));
            }
        }
        root.Set(L"sessions", std::move(sessions));
        if (!rest.empty())
        {
            // One compact line per further match: its short id (show, read and grep take it), title and count.
            Value also = Value::MkArr();
            std::lock_guard lock{ _mutex };
            for (const GrepSessionHit* h : rest)
            {
                const auto it = facts.find(h->sessionId);
                std::wstring line = AssignHandleLocked(h->sessionId);
                if (it != facts.end())
                {
                    KnownSession& known = _known[h->sessionId];
                    if (!it->second.path.empty())
                    {
                        known.path = it->second.path;
                    }
                    if (!it->second.title.empty())
                    {
                        known.title = it->second.title;
                        line += L" \x00B7 " + Cut(it->second.title, 70);
                    }
                    if (!it->second.dir.empty())
                    {
                        // The folder too (a row's `dir`): one line still says where the session ran.
                        known.dir = it->second.dir;
                        line += L" \x00B7 " + Cut(it->second.dir, 60);
                    }
                    if (it->second.activeMs > 0)
                    {
                        known.activeMs = it->second.activeMs;
                    }
                }
                line += L" \x00B7 " + std::to_wstring(h->hits) + (h->hits == 1 ? L" match" : L" matches") + (h->userHits > 0 ? L", you wrote it" : L"");
                if (!h->excerpts.empty())
                {
                    if (params.editsOnly)
                    {
                        NoteEvidenceLocked(h->sessionId, L"Claude changed it: " + Cut(OneLine(h->excerpts.front().text), 110), 1);
                    }
                    else
                    {
                        const GrepExcerpt& x = h->excerpts.front();
                        NoteEvidenceLocked(h->sessionId, (x.user ? L"You wrote: \x201C" : L"Claude: \x201C") + Cut(OneLine(x.text), 110) + L"\x201D", x.user ? 0 : 4);
                    }
                }
                also.Push(Value::MkStr(std::move(line)));
            }
            root.Set(L"also", std::move(also));
        }
        return { json::Dump(root), false };
    }

    HarnessToolResult SessionSearchToolset::Read(const json::Value& arguments)
    {
        const int64_t now = Now();
        std::wstring error;
        const auto params = ParseSessionReadParams(arguments, error);
        if (!params)
        {
            return { error, true };
        }
        std::function<bool()> cancelled;
        if (_callbacks.generation)
        {
            const uint64_t startGeneration = _callbacks.generation();
            cancelled = [this, startGeneration] { return _callbacks.generation() != startGeneration; };
        }
        std::vector<std::wstring> unknown;
        const std::vector<std::wstring> ids = ResolveWrittenIds(params->ids, unknown);
        if (ids.empty())
        {
            std::wstring text = L"None of these ids is from a result of this conversation:";
            for (const auto& u : unknown)
            {
                text += L" " + u;
            }
            text += L". Search first, then pass the ids exactly as a result printed them.";
            return { text, true };
        }
        if (_callbacks.onSearching)
        {
            _callbacks.onSearching(L"reading " + (ids.size() == 1 ? std::wstring{ L"1 session" } : std::to_wstring(ids.size()) + L" sessions"));
        }

        TurnsOptions opts = TurnsOptionsForLevel(params->level == SessionReadLevel::Minimal ? TurnsLevel::Minimal : (params->level == SessionReadLevel::Default ? TurnsLevel::Default : TurnsLevel::Compact));
        // ONE session as it is: not the sessions it was forked from (the chain walk reads every older transcript of
        // its project folder) and not its subagents' work (their transcripts can outweigh the session's own). A
        // confirming read needs what the user asked and what Claude answered.
        opts.chain = false;
        opts.agents = false;

        std::vector<KnownSession> facts(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            {
                std::lock_guard lock{ _mutex };
                if (const auto it = _known.find(ids[i]); it != _known.end())
                {
                    facts[i] = it->second;
                }
            }
            if (facts[i].path.empty())
            {
                facts[i].path = TranscriptPathOf(ids[i]); // takes _mutex itself: outside the lock above
            }
        }

        // The renders run in parallel, one thread each (a render reads and parses the whole transcript: 0.1 to 1.5 s
        // measured for the copy menu, seconds for the largest). The threads are DETACHED and share only `renders`:
        // a call that stops waiting (its budget, or the request was replaced) returns at once, and a late render
        // finishes into a state nobody reads any more, which the last thread frees.
        struct Renders
        {
            std::mutex m;
            std::condition_variable cv;
            size_t done{};
            std::vector<std::optional<std::wstring>> texts;
            std::vector<char> finished;
        };
        const auto renders = std::make_shared<Renders>();
        renders->texts.resize(ids.size());
        renders->finished.assign(ids.size(), 0);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (facts[i].path.empty())
            {
                std::lock_guard lk{ renders->m };
                renders->finished[i] = 1; // no transcript: nothing to render (the result says it is gone)
                ++renders->done;
                continue;
            }
            const auto renderOne = [renders, i, path = facts[i].path, opts]() {
                try
                {
                    std::optional<std::wstring> text = RenderTranscriptTurnsFile(std::filesystem::path{ path }, opts, /*utc*/ false);
                    {
                        std::lock_guard lk{ renders->m };
                        renders->texts[i] = std::move(text);
                        renders->finished[i] = 1;
                        ++renders->done;
                    }
                    renders->cv.notify_all();
                }
                catch (...)
                {
                    // A render thread must never let an exception out (std::terminate). Its session then reads
                    // "not read in time" when the call's budget ends.
                    LogSwallowedException(L"SessionSearchToolset::Read: a render thread");
                }
            };
            try
            {
                std::thread{ renderOne }.detach();
            }
            catch (...)
            {
                // No thread for it (std::system_error: the process is out of threads): rendered here, in the call.
                LogSwallowedException(L"SessionSearchToolset::Read: starting a render thread");
                renderOne();
            }
        }
        std::vector<std::optional<std::wstring>> texts(ids.size());
        std::vector<char> finished(ids.size(), 0);
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ kSessionReadBudgetMs };
            std::unique_lock lk{ renders->m };
            while (renders->done < ids.size() && std::chrono::steady_clock::now() < deadline && !(cancelled && cancelled()))
            {
                renders->cv.wait_for(lk, std::chrono::milliseconds{ 100 });
            }
            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (renders->finished[i])
                {
                    finished[i] = 1;
                    texts[i] = std::move(renders->texts[i]); // a finished render's slot is never written again
                }
            }
        }
        if (cancelled && cancelled())
        {
            return { L"The read was cancelled: the request was stopped or replaced.", true };
        }

        std::wstring out;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            std::wstring handle;
            {
                std::lock_guard lock{ _mutex };
                handle = AssignHandleLocked(ids[i]);
            }
            std::wstring header = L"## Session " + handle;
            if (!facts[i].title.empty())
            {
                header += L" \x00B7 " + facts[i].title;
            }
            if (facts[i].activeMs > 0)
            {
                header += L" \x00B7 active " + ActiveWithAgo(facts[i].activeMs, now);
            }
            if (!finished[i])
            {
                out += header + L"\nNot read: rendering it took longer than " + std::to_wstring(kSessionReadBudgetMs / 1000) + L" s. Read it alone, or at level minimal.\n\n";
                continue;
            }
            if (!texts[i] || texts[i]->empty())
            {
                out += header + L"\nNot read: its transcript is gone or could not be read.\n\n";
                continue;
            }
            const SessionReadCut cut = CutRenderedTranscript(*texts[i], params->chars);
            if (cut.turns == 0)
            {
                header += L" \x00B7 no conversation yet";
            }
            else
            {
                header += L" \x00B7 " + std::to_wstring(cut.turns) + (cut.turns == 1 ? L" turn" : L" turns");
                if (cut.omittedTurns > 0)
                {
                    header += L", " + std::to_wstring(cut.omittedTurns) + L" left out in the middle (read it alone with a larger chars to see them)";
                }
            }
            out += header + L"\n\n" + cut.text;
            if (!out.empty() && out.back() != L'\n')
            {
                out += L'\n';
            }
            out += L'\n';
        }
        if (!unknown.empty())
        {
            out += L"Unknown ids (no result of this conversation printed them):";
            for (const auto& u : unknown)
            {
                out += L" " + u;
            }
            out += L".\n";
        }
        if (params->extraIds > 0)
        {
            out += L"Only the first " + std::to_wstring(kSessionReadMaxIds) + L" ids were read: read the other " + std::to_wstring(params->extraIds) + L" in another call.\n";
        }
        return { out, false };
    }

    HarnessToolResult SessionSearchToolset::Show(const json::Value& arguments)
    {
        using Type = json::Value::Type;
        const auto* idsArg = arguments.Find(L"ids");
        const auto* topArg = arguments.Find(L"top");
        const bool hasIds = idsArg && idsArg->type == Type::Arr;
        const bool hasTop = topArg && topArg->type == Type::Num && topArg->AsI64() > 0;
        if (!hasIds && !hasTop)
        {
            return { L"Pass \"ids\" (an array of ids from search results; an empty array shows nothing) or \"top\" (a number).", true };
        }
        constexpr size_t kMaxShown = kSessionShowMaxIds;
        std::vector<std::wstring> resolved;
        std::vector<std::wstring> unknown;
        std::unordered_set<std::wstring> seen;
        if (hasIds && _callbacks.database.resolve)
        {
            // The database's other tools (grep, who_touched, timeline, related, ...) print short ids that no search
            // of this conversation recorded: resolve those in one round trip before reading the list.
            std::vector<std::wstring> missing;
            for (const auto& v : idsArg->arr)
            {
                if (v.type == Type::Str && missing.size() < kMaxShown && ResolveHandle(v.str).empty())
                {
                    missing.push_back(Trim(v.str));
                }
            }
            if (!missing.empty())
            {
                RememberHandles(missing, _callbacks.database.resolve(missing));
            }
        }
        if (hasIds)
        {
            for (const auto& v : idsArg->arr)
            {
                if (v.type != Type::Str)
                {
                    continue;
                }
                const std::wstring full = ResolveHandle(v.str);
                if (full.empty())
                {
                    if (unknown.size() < 5)
                    {
                        unknown.push_back(Cut(Trim(v.str), 40));
                    }
                    continue;
                }
                if (resolved.size() < kMaxShown && seen.insert(full).second)
                {
                    resolved.push_back(full);
                }
            }
        }
        else
        {
            std::lock_guard lock{ _mutex };
            const size_t n = (std::min)(static_cast<size_t>(topArg->AsI64()), (std::min)(_lastSearchIds.size(), kMaxShown));
            resolved.assign(_lastSearchIds.begin(), _lastSearchIds.begin() + static_cast<std::ptrdiff_t>(n));
        }
        if (resolved.empty() && !unknown.empty())
        {
            // Nothing usable: do not clear the screen for a list of ids that mean nothing.
            std::wstring text = _callbacks.database.call ? L"None of these ids names a session:" : L"None of these ids is from a search result of this conversation:";
            for (const auto& u : unknown)
            {
                text += L" " + u;
            }
            text += _callbacks.database.call ? L". Pass the ids exactly as a result printed them." : L". Search first, then pass the ids exactly as search returned them.";
            return { text, true };
        }
        const std::wstring note = Cut(OneLine(arguments.StrAt(L"note")), 160);
        // The evidence per session (SESSIONS.md 9.12): `why` is keyed by the ids as the model wrote them (any case,
        // short or whole); each is resolved like an id and lined up with what is shown. A why for a session that is
        // not shown is dropped; a shown session without one gets "" (the surface shows no evidence line for it).
        std::vector<std::wstring> whys(resolved.size());
        if (const auto* why = arguments.Find(L"why"); why && why->type == Type::Obj)
        {
            std::unordered_map<std::wstring, std::wstring> byId;
            for (const auto& [key, value] : why->members)
            {
                if (value.type != Type::Str)
                {
                    continue;
                }
                const std::wstring full = ResolveHandle(key);
                std::wstring text = Cut(OneLine(value.str), kSessionWhyMaxChars);
                if (!full.empty() && !text.empty())
                {
                    byId.emplace(full, std::move(text)); // the first why written for a session wins
                }
            }
            for (size_t i = 0; i < resolved.size(); ++i)
            {
                if (const auto it = byId.find(resolved[i]); it != byId.end())
                {
                    whys[i] = it->second;
                }
            }
        }
        // A shown session the model wrote no why for gets the evidence a search or grep of this conversation showed
        // for it (KnownSession::evidence): a list of a hundred sessions carries one real line each, and the model writes
        // whys only where it has better evidence (SESSIONS.md §9.15).
        size_t autoWhys = 0;
        {
            std::lock_guard lock{ _mutex };
            for (size_t i = 0; i < resolved.size(); ++i)
            {
                if (!whys[i].empty())
                {
                    continue;
                }
                if (const auto it = _known.find(resolved[i]); it != _known.end() && !it->second.evidence.empty())
                {
                    whys[i] = it->second.evidence;
                    ++autoWhys;
                }
            }
        }
        _callbacks.onShow(resolved, note, whys);

        std::wstring text = L"Shown " + std::to_wstring(resolved.size()) + (resolved.size() == 1 ? L" session." : L" sessions.");
        if (autoWhys > 0)
        {
            text += L" " + std::to_wstring(autoWhys) + L" of them got the evidence your results showed as their why.";
        }
        if (!unknown.empty())
        {
            text += L" Not shown, unknown ids:";
            for (const auto& u : unknown)
            {
                text += L" " + u;
            }
            text += L".";
        }
        // The reply comes next (the Instructional Behaviors of the system prompt): what was found, the shown sessions by
        // number, then the likely next step. The old "one short sentence" request is what the user called hasty.
        text += L" Now write your reply: what you found or did (refer to the shown sessions by number), then the likely next step.";
        return { text, false };
    }

    bool SessionSearchToolset::TakeTabActionSlot(bool deactivate, std::wstring& error)
    {
        // The request the action belongs to: the generation moves with every new request, interrupt and reset, so each
        // request gets its own budget without a callback from the agent.
        const uint64_t generation = _callbacks.generation ? _callbacks.generation() : 0;
        std::lock_guard lock{ _mutex };
        if (generation != _tabActionGeneration)
        {
            _tabActionGeneration = generation;
            _tabOpensThisRequest = 0;
            _tabDeactivatesThisRequest = 0;
        }
        if (deactivate)
        {
            // The user's rule: one at a time. A list of tabs is parked over several requests, each one confirmed.
            if (_tabDeactivatesThisRequest >= 1)
            {
                error = L"Not deactivated: a tab was already deactivated in this request, and only one tab may be deactivated per request. Tell the user which tab you deactivated, and ask them to confirm the next one; deactivate it after their answer.";
                return false;
            }
            ++_tabDeactivatesThisRequest;
            return true;
        }
        if (_tabOpensThisRequest >= kMaxTabOpensPerRequest)
        {
            error = L"Not opened: this request already opened " + std::to_wstring(kMaxTabOpensPerRequest) + L" tabs, the most one request may open. Tell the user what you opened, and that they can ask again for more.";
            return false;
        }
        ++_tabOpensThisRequest;
        return true;
    }

    void SessionSearchToolset::ReturnTabActionSlot(bool deactivate) noexcept
    {
        try
        {
            std::lock_guard lock{ _mutex };
            size_t& count = deactivate ? _tabDeactivatesThisRequest : _tabOpensThisRequest;
            if (count > 0)
            {
                --count;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the slot stays spent — the request may open one tab fewer, never one more.
            LogSwallowedException(L"SessionSearchToolset::ReturnTabActionSlot");
        }
    }

    HarnessToolResult SessionSearchToolset::ListTabs(const json::Value& arguments)
    {
        using json::Value;
        const int64_t now = Now();
        const std::vector<WindowCensus> windows = _callbacks.tabs.census();
        if (windows.empty())
        {
            return { L"No Agentmaster window answered. Try once more; if it fails again, tell the user that the windows did not answer.", true };
        }
        const std::wstring origin = _callbacks.tabs.originWindowId ? _callbacks.tabs.originWindowId() : std::wstring{};
        std::optional<size_t> only;
        if (const std::wstring windowArg = StringOrNumberArgument(arguments, L"window"); !windowArg.empty())
        {
            std::wstring error;
            only = ResolveWindowArgument(windows, windowArg, origin, error);
            if (!only)
            {
                return { error, true };
            }
        }
        const std::wstring dirNeedle = FoldPathText(arguments.StrAt(L"dir"));
        const std::wstring titleNeedle = FoldLower(Trim(arguments.StrAt(L"title")));
        size_t limit = kTabListDefaultLimit;
        if (const auto* l = arguments.Find(L"limit"); l && l->type == Value::Type::Num)
        {
            // Clamped, not refused (the search tools' rule): a list of 200 is more useful than an error.
            limit = static_cast<size_t>((std::clamp)(l->AsI64(), int64_t{ 1 }, static_cast<int64_t>(kTabListMaxLimit)));
        }

        Value root = Value::MkObj();
        root.Set(L"now", Value::MkStr(LocalDateTimeText(now)));
        root.Set(L"windows_open", Value::MkNum(static_cast<double>(windows.size())));
        Value list = Value::MkArr();
        bool anyCut = false;
        for (size_t w = 0; w < windows.size(); ++w)
        {
            if (only && *only != w)
            {
                continue;
            }
            const WindowCensus& win = windows[w];
            // The filters, in the strip's order.
            std::vector<size_t> match;
            for (size_t t = 0; t < win.tabs.size(); ++t)
            {
                const auto& tab = win.tabs[t];
                if (!titleNeedle.empty() && FoldLower(tab.title).find(titleNeedle) == std::wstring::npos)
                {
                    continue;
                }
                if (!dirNeedle.empty() && FoldPathText(tab.dir).find(dirNeedle) == std::wstring::npos && FoldPathText(tab.worksIn).find(dirNeedle) == std::wstring::npos)
                {
                    continue;
                }
                match.push_back(t);
            }
            // Over the limit: the selected tab and the most recently active ones (what a placement decision and a
            // "what is running" question need), then back to the strip's order so pos reads naturally.
            std::vector<size_t> keep = match;
            if (keep.size() > limit)
            {
                std::stable_sort(keep.begin(), keep.end(), [&win](size_t a, size_t b) {
                    const auto& ta = win.tabs[a];
                    const auto& tb = win.tabs[b];
                    if (ta.selected != tb.selected)
                    {
                        return ta.selected;
                    }
                    return ta.lastActivityUnixMs > tb.lastActivityUnixMs;
                });
                keep.resize(limit);
                std::sort(keep.begin(), keep.end());
                anyCut = true;
            }
            Value wobj = Value::MkObj();
            wobj.Set(L"window", Value::MkNum(static_cast<double>(w + 1)));
            wobj.Set(L"id", Value::MkStr(WindowHandleOf(win.windowId)));
            wobj.Set(L"tabs", Value::MkNum(static_cast<double>(win.tabs.size())));
            if (keep.size() != win.tabs.size())
            {
                wobj.Set(L"listed", Value::MkNum(static_cast<double>(keep.size())));
            }
            // Flags only when they say something: fewer tokens for the model to read.
            if (win.lastActive)
            {
                wobj.Set(L"last_used", Value::MkBool(true));
            }
            if (!origin.empty() && win.windowId == origin)
            {
                wobj.Set(L"asked_from", Value::MkBool(true));
            }
            if (win.managerPageVisible)
            {
                wobj.Set(L"agent_manager_page", Value::MkBool(true));
            }
            Value tabs = Value::MkArr();
            for (const size_t t : keep)
            {
                const auto& tab = win.tabs[t];
                Value o = Value::MkObj();
                o.Set(L"pos", Value::MkNum(static_cast<double>(tab.index + 1)));
                o.Set(L"title", Value::MkStr(Cut(OneLine(tab.title), 120)));
                if (tab.sessionId.empty())
                {
                    o.Set(L"kind", Value::MkStr(L"shell"));
                }
                else
                {
                    o.Set(L"kind", Value::MkStr(tab.codex ? L"codex" : L"claude"));
                    // The same short ids as every other result: read, show, resume, fork and deactivate take them.
                    std::wstring handle;
                    {
                        std::lock_guard lock{ _mutex };
                        handle = AssignHandleLocked(tab.sessionId);
                    }
                    o.Set(L"id", Value::MkStr(handle));
                    if (!tab.state.empty())
                    {
                        o.Set(L"state", Value::MkStr(tab.state));
                    }
                    if (!tab.started)
                    {
                        o.Set(L"dormant", Value::MkBool(true));
                    }
                    if (tab.external)
                    {
                        o.Set(L"adopted", Value::MkBool(true));
                    }
                    if (!tab.dir.empty())
                    {
                        o.Set(L"dir", Value::MkStr(tab.dir));
                    }
                    if (!tab.worksIn.empty())
                    {
                        o.Set(L"works_in", Value::MkStr(tab.worksIn));
                    }
                    if (!tab.branch.empty())
                    {
                        o.Set(L"branch", Value::MkStr(tab.branch));
                    }
                    if (tab.lastTurnUnixMs > 0)
                    {
                        o.Set(L"last_turn", Value::MkStr(ActiveWithAgo(tab.lastTurnUnixMs, now)));
                    }
                    if (tab.lastActivityUnixMs > 0)
                    {
                        o.Set(L"active", Value::MkStr(ActiveWithAgo(tab.lastActivityUnixMs, now)));
                    }
                }
                if (tab.selected)
                {
                    o.Set(L"selected", Value::MkBool(true));
                }
                tabs.Push(std::move(o));
            }
            wobj.Set(L"tab_list", std::move(tabs));
            list.Push(std::move(wobj));
        }
        root.Set(L"list", std::move(list));
        if (anyCut)
        {
            root.Set(L"note", Value::MkStr(L"Some windows have more tabs than are listed: narrow with dir or title, or raise limit (up to 200)."));
        }
        return { json::Dump(root), false };
    }

    HarnessToolResult SessionSearchToolset::OpenTab(const json::Value& arguments, WindowToolRequest::Kind kind)
    {
        const bool fork = kind == WindowToolRequest::Kind::Fork;
        if (!_callbacks.tabs.census || !_callbacks.tabs.open)
        {
            return { L"Opening tabs is not available here.", true };
        }
        const std::wstring written = Trim(arguments.StrAt(L"id"));
        if (written.empty())
        {
            return { std::wstring{ L"Pass id: the session to " } + (fork ? L"fork" : L"resume") + L" (a short id from a result, or a whole session id).", true };
        }
        std::vector<std::wstring> unknown;
        const std::vector<std::wstring> ids = ResolveWrittenIds({ written }, unknown);
        if (ids.empty())
        {
            return { L"No session has the id " + Cut(written, 40) + L". Pass an id exactly as a result printed it, or search first.", true };
        }
        const std::wstring sessionId = ids.front();
        const std::vector<WindowCensus> windows = _callbacks.tabs.census();
        const std::wstring origin = _callbacks.tabs.originWindowId ? _callbacks.tabs.originWindowId() : std::wstring{};
        const std::optional<TabLocation> where = FindSessionTab(windows, sessionId);
        const bool focus = arguments.BoolAt(L"focus");

        // A resume of a session that has a tab already: never a second tab of one conversation (two writers of one
        // transcript, OBSERVER.md §20). Say where it is, and bring it forward when asked.
        if (!fork && where)
        {
            const auto& tab = windows[where->window].tabs[where->tab];
            std::wstring text = L"Already open: " + DescribeTabPlace(windows, *where) + L" (window id " + WindowHandleOf(windows[where->window].windowId) + L")" +
                                (tab.started ? std::wstring{} : std::wstring{ L", dormant: it starts when the user enters the tab" }) + L". No new tab was opened.";
            if (focus)
            {
                const bool brought = _callbacks.tabs.focus && _callbacks.tabs.focus(sessionId);
                text += brought ? L" The tab was brought to the front." : L" It could not be brought to the front.";
            }
            return { text, false };
        }

        // The window. With several open and none named, the tool refuses: the user names one (the user's rule).
        std::wstring error;
        const std::optional<size_t> windowIndex = ResolveWindowArgument(windows, StringOrNumberArgument(arguments, L"window"), origin, error);
        if (!windowIndex)
        {
            if (fork && where)
            {
                // The usual answer for a fork, to offer the user: next to its source, as the tab menu's Fork session does.
                error += L"The source session is open in " + DescribeTabPlace(windows, *where) + L": a fork usually goes right after it.\n";
            }
            return { error, true };
        }
        const WindowCensus& target = windows[*windowIndex];

        WindowToolRequest request;
        request.kind = kind;
        request.sessionId = sessionId;
        request.focus = focus;
        if (fork)
        {
            request.model = Trim(arguments.StrAt(L"model"));
        }
        // `after`: a position in the target window, or the session id of one of its tabs.
        std::wstring afterTitle;
        if (const std::wstring after = StringOrNumberArgument(arguments, L"after"); !after.empty())
        {
            if (IsSmallNumber(after))
            {
                const size_t pos = SmallNumberValue(after);
                if (pos < 1 || pos > target.tabs.size())
                {
                    return { L"Window " + std::to_wstring(*windowIndex + 1) + L" has no tab " + after + L" (it has " + std::to_wstring(target.tabs.size()) + L" tabs). Pass a pos from list, or leave after out to put the tab at the end.", true };
                }
                request.afterIndex = static_cast<int32_t>(pos - 1);
                for (const auto& tab : target.tabs)
                {
                    if (tab.index + 1 == pos)
                    {
                        afterTitle = tab.title;
                    }
                }
            }
            else
            {
                std::vector<std::wstring> afterUnknown;
                const std::vector<std::wstring> afterIds = ResolveWrittenIds({ after }, afterUnknown);
                const auto inTarget = std::find_if(target.tabs.begin(), target.tabs.end(), [&afterIds](const WindowTabInfo& tab) {
                    return !afterIds.empty() && tab.sessionId == afterIds.front();
                });
                if (inTarget == target.tabs.end())
                {
                    return { L"The tab to put it after (" + Cut(after, 40) + L") is not in window " + std::to_wstring(*windowIndex + 1) + L". List that window's tabs and pass a pos or a session id from it, or leave after out.", true };
                }
                request.afterSessionId = inTarget->sessionId;
                afterTitle = inTarget->title;
            }
        }
        else if (fork && where && where->window == *windowIndex)
        {
            afterTitle = windows[where->window].tabs[where->tab].title; // the window's default: right after the source
        }

        if (!TakeTabActionSlot(false, error))
        {
            return { error, true };
        }
        const WindowToolOutcome outcome = _callbacks.tabs.open(target.windowId, request);
        if (!outcome.handled || !outcome.ok)
        {
            // A window that refused opened nothing: the slot goes back. A window that did not ANSWER may still open
            // the tab after the timeout, so its slot stays spent and the model is told to look before it retries.
            if (outcome.handled)
            {
                ReturnTabActionSlot(false);
            }
            std::wstring why = !outcome.message.empty() ? outcome.message :
                                                          (outcome.handled ? std::wstring{ L"the window refused it." } :
                                                                             std::wstring{ L"the window did not answer in time. The tab may still open: list the tabs before you try again." });
            return { std::wstring{ fork ? L"Not forked: " : L"Not resumed: " } + why, true };
        }
        const std::wstring opened = outcome.sessionId.empty() ? sessionId : outcome.sessionId;
        std::wstring handle;
        {
            std::lock_guard lock{ _mutex };
            handle = AssignHandleLocked(opened);
        }
        std::wstring text = std::wstring{ fork ? L"Forked" : L"Resumed" } + L" in window " + std::to_wstring(*windowIndex + 1) + L" (id " + WindowHandleOf(target.windowId) + L")";
        if (outcome.index >= 0)
        {
            text += L" as tab " + std::to_wstring(outcome.index + 1);
        }
        if (!afterTitle.empty())
        {
            text += L", right after \"" + Cut(OneLine(afterTitle), 80) + L"\"";
        }
        text += focus ? L"; the window switched to it and came to the front." : L"; it is a background tab, so the user's view did not change.";
        if (fork)
        {
            text += L" The fork's id is " + handle + L"; the source is untouched.";
        }
        else if (opened != sessionId)
        {
            // Rule #6: a transcript that is gone resumes nothing; the window started a fresh session in its folder.
            text += L" Its transcript was gone, so a FRESH session started in its folder instead, id " + handle + L".";
        }
        if (!outcome.message.empty())
        {
            text += L" " + outcome.message;
        }
        return { text, false };
    }

    HarnessToolResult SessionSearchToolset::DeactivateTab(const json::Value& arguments)
    {
        if (!_callbacks.tabs.census || !_callbacks.tabs.deactivate)
        {
            return { L"Deactivating tabs is not available here.", true };
        }
        if (const auto* idArg = arguments.Find(L"id"); idArg && idArg->type == json::Value::Type::Arr)
        {
            return { L"Pass ONE id, as a string: deactivate parks one tab per request.", true };
        }
        const std::wstring written = Trim(arguments.StrAt(L"id"));
        if (written.empty())
        {
            return { L"Pass id: the session whose tab to deactivate (its id from list).", true };
        }
        std::vector<std::wstring> unknown;
        const std::vector<std::wstring> ids = ResolveWrittenIds({ written }, unknown);
        if (ids.empty())
        {
            return { L"No session has the id " + Cut(written, 40) + L". List the tabs and pass an id from the list.", true };
        }
        const std::wstring sessionId = ids.front();
        const std::vector<WindowCensus> windows = _callbacks.tabs.census();
        const std::optional<TabLocation> where = FindSessionTab(windows, sessionId);
        if (!where)
        {
            return { L"That session has no open tab in any window, so there is nothing to deactivate.", true };
        }
        const WindowTabInfo& tab = windows[where->window].tabs[where->tab];
        const std::wstring place = DescribeTabPlace(windows, *where);
        if (tab.external)
        {
            return { L"Not deactivated: " + place + L" runs a claude the user started by hand in a shell tab. Its terminal is the user's own shell, and Deactivate never parks it.", true };
        }
        if (!tab.started)
        {
            return { L"Nothing to do: " + place + L" is already dormant. Its claude is not running, and it starts when the user enters the tab.", false };
        }
        // A turn in flight is cut off by a park (the session then reads Error, HOOKS.md "Cut-off turns"): only on the
        // user's confirmation, which the model passes as cut_off_turn.
        const bool turnInFlight = tab.state == L"running" || tab.state == L"needs approval";
        if (turnInFlight && !arguments.BoolAt(L"cut_off_turn"))
        {
            return { L"Not deactivated: " + place + L" is " + tab.state + L", and deactivating it now cuts that turn off. Tell the user, ask them to confirm, and only then call again with cut_off_turn:true.", true };
        }
        std::wstring error;
        if (!TakeTabActionSlot(true, error))
        {
            return { error, true };
        }
        const WindowToolOutcome outcome = _callbacks.tabs.deactivate(sessionId);
        if (!outcome.handled || !outcome.ok)
        {
            if (outcome.handled)
            {
                ReturnTabActionSlot(true); // the host refused: nothing was parked
            }
            const std::wstring why = !outcome.message.empty() ? outcome.message : (outcome.handled ? std::wstring{ L"its window refused it." } : std::wstring{ L"its window did not answer in time. List the tabs to see whether it was parked." });
            return { L"Not deactivated: " + why, true };
        }
        return { L"Deactivated " + place + L": its claude stopped; the tab keeps its place, title, color and prompt queue, and the conversation resumes when the user enters it." + (outcome.message.empty() ? std::wstring{} : L" " + outcome.message), false };
    }

    std::vector<HarnessTool> SessionSearchToolset::MakeTabTools()
    {
        std::vector<HarnessTool> tools;
        HarnessTool list;
        list.name = kTabsListTool;
        list.description = L"List the Agentmaster windows and their tabs: per tab its position, title, session id, state, folder (dir, works_in), last turn and last activity; per window its number, the selected tab, and whether the user used it last or asked from it. Read it before you resume, fork or deactivate, and to answer what is open or running. Changes nothing.";
        list.inputSchemaJson = kTabsListSchema;
        list.readOnly = true; // it only reads; one model step may run it beside a search
        list.handler = [this](const json::Value& arguments) { return ListTabs(arguments); };
        tools.push_back(std::move(list));

        // The three that change the workspace are NOT read-only: the SDK runs them one at a time, and never beside
        // another call of the same step that could still be reading the tabs they change.
        HarnessTool resume;
        resume.name = kTabsResumeTool;
        resume.description = L"Open a closed session again in a tab (the same conversation, its context intact), in a window you name, right after a tab you choose. Refuses when several windows are open and none is named: then ask the user which window. An already open session is not opened twice: the result says where its tab is. Use it only when the user asked to resume or open a session.";
        resume.inputSchemaJson = kTabsResumeSchema;
        resume.handler = [this](const json::Value& arguments) { return OpenTab(arguments, WindowToolRequest::Kind::Resume); };
        tools.push_back(std::move(resume));

        HarnessTool fork;
        fork.name = kTabsForkTool;
        fork.description = L"Branch a session into a NEW conversation that starts with the source's whole history, as a tab of a window you name, right after a tab you choose (by default right after the source's tab, like the tab menu's Fork session). The source is untouched. Refuses when several windows are open and none is named: then ask the user which window. Use it only when the user asked to fork.";
        fork.inputSchemaJson = kTabsForkSchema;
        fork.handler = [this](const json::Value& arguments) { return OpenTab(arguments, WindowToolRequest::Kind::Fork); };
        tools.push_back(std::move(fork));

        HarnessTool deactivate;
        deactivate.name = kTabsDeactivateTool;
        deactivate.description = L"Deactivate (park) ONE tab: its claude stops, the tab keeps its place and resumes the conversation when the user enters it. Only when the user explicitly asked to deactivate that tab; one tab per request. A tab whose turn is running or waiting for approval needs cut_off_turn:true, after the user confirmed.";
        deactivate.inputSchemaJson = kTabsDeactivateSchema;
        deactivate.handler = [this](const json::Value& arguments) { return DeactivateTab(arguments); };
        tools.push_back(std::move(deactivate));
        return tools;
    }

    HarnessTool SessionSearchToolset::MakeReadTool()
    {
        HarnessTool read;
        read.name = kReadTool;
        read.description = L"Read up to 8 sessions as compact Markdown: each turn's user message and Claude's final reply (more with level). A long session is cut to its first and last turns. Read the candidates whose evidence is not conclusive, the ones your reply describes, and the sessions of a period before you summarize them. Changes nothing the user sees.";
        read.inputSchemaJson = kReadSchema;
        read.readOnly = true; // it only reads; one model step may run it beside a search
        read.handler = [this](const json::Value& arguments) { return Read(arguments); };
        return read;
    }

    std::vector<HarnessToolServer> SessionSearchToolset::Servers()
    {
        // Callbacks::onToolResult (the prompt eval, SESSIONS.md §9.15) sees every call's WHOLE result: each handler is
        // wrapped once, here, after its server got its tools, so no tool can be added without the report. Without the
        // callback the servers are returned as they are (the app: no wrapper in the call path at all).
        const auto tapped = [this](std::vector<HarnessToolServer> servers) {
            if (!_callbacks.onToolResult)
            {
                return servers;
            }
            for (auto& server : servers)
            {
                for (auto& tool : server.tools)
                {
                    tool.handler = [this, name = HarnessToolFullName(server.name, tool.name), inner = std::move(tool.handler)](const json::Value& arguments) {
                        HarnessToolResult result = inner(arguments); // a throw goes to the harness, which answers the model with it
                        try
                        {
                            _callbacks.onToolResult(name, arguments, result);
                        }
                        catch (...)
                        {
                            // Rule #18. Recovery: the model still gets the result; only the eval's copy of it is missing.
                            LogSwallowedException(L"SessionSearchToolset: the onToolResult callback");
                        }
                        return result;
                    };
                }
            }
            return servers;
        };
        if (_callbacks.database.call)
        {
            // The database mode: its read tools, from the one copy of their text the service's registry uses too
            // (SessionDbToolDefs.h), each forwarded; then the same `show`.
            HarnessToolServer db;
            db.name = kDatabaseServer;
            for (const auto& def : SessionDbToolDefs::kTools)
            {
                HarnessTool t;
                t.name = WidenAscii(def.name);
                t.description = WidenAscii(def.description);
                t.inputSchemaJson = WidenAscii(def.inputSchema);
                t.readOnly = true; // every database tool only reads: one model step may run several at once
                t.handler = [this, name = std::string{ def.name }](const json::Value& arguments) { return DatabaseCall(name, arguments); };
                db.tools.push_back(std::move(t));
            }
            // This toolset's own `read` beside them (SESSIONS.md 9.12): a candidate is confirmed the same way in both
            // modes. The database has no tool of that name (SessionDbToolDefs.h), so mcp__agentmaster__read is ours.
            db.tools.push_back(MakeReadTool());
            HarnessTool show;
            show.name = kDisplayTool;
            show.description = kShowDescription;
            show.inputSchemaJson = kShowSchema;
            show.handler = [this](const json::Value& arguments) { return Show(arguments); };
            HarnessToolServer display;
            display.name = kDisplayServer;
            display.tools.push_back(std::move(show));
            std::vector<HarnessToolServer> servers;
            servers.push_back(std::move(db));
            servers.push_back(std::move(display));
            // The window and tab tools (SESSIONS.md 9.14): the same in both modes, last, so the search tools keep their
            // places in the model's list.
            if (UsesTabs())
            {
                HarnessToolServer tabs;
                tabs.name = kTabsServer;
                tabs.tools = MakeTabTools();
                servers.push_back(std::move(tabs));
            }
            return tapped(std::move(servers));
        }

        HarnessTool search;
        search.name = kSearchTool;
        search.description = L"Find Claude Code sessions on this computer: every session by default, as far back as they go. Every word must match (any case, also inside longer words: drag finds dragging), all in one kind of place: the names and paths, one typed prompt, or with deep one message. Returns matching sessions as JSON rows (id, title, dir, branch, active, created, msgs, tools, match; in_period for a dated search), best match first, and `more` when the limit cut them. Changes nothing the user sees.";
        search.inputSchemaJson = kSearchSchema;
        search.readOnly = true; // several searches of one model step may run at the same time
        search.handler = [this](const json::Value& arguments) { return Search(arguments); };

        HarnessTool grep;
        grep.name = kGrepTool;
        grep.description = L"Find exact text, or a regex, in the conversations' full text: the user's prompts, Claude's replies and thinking, tool calls and their output - or, with in:edits, the paths of the files Claude changed. Returns, per session, how many messages match and the first matches in context, each with who wrote it (user or claude) and when; past the limit the other matching sessions are listed in `also`. Use it for evidence: words the user remembers, an error, a version, which sessions changed a file. One regex can cover every form of a word (drag(ged|ging)?, tab[ _-]?drag). Changes nothing the user sees.";
        grep.inputSchemaJson = kGrepSchema;
        grep.readOnly = true; // it only reads; one model step may run it beside a search
        grep.handler = [this](const json::Value& arguments) { return Grep(arguments); };

        HarnessTool show;
        show.name = kDisplayTool;
        show.description = kShowDescription;
        show.inputSchemaJson = kShowSchema;
        show.handler = [this](const json::Value& arguments) { return Show(arguments); };

        HarnessToolServer sessions;
        sessions.name = kSearchServer;
        sessions.tools.push_back(std::move(search));
        sessions.tools.push_back(std::move(grep));
        sessions.tools.push_back(MakeReadTool());
        HarnessToolServer display;
        display.name = kDisplayServer;
        display.tools.push_back(std::move(show));
        std::vector<HarnessToolServer> servers;
        servers.push_back(std::move(sessions));
        servers.push_back(std::move(display));
        if (UsesTabs())
        {
            HarnessToolServer tabs;
            tabs.name = kTabsServer;
            tabs.tools = MakeTabTools();
            servers.push_back(std::move(tabs));
        }
        return tapped(std::move(servers));
    }
}
