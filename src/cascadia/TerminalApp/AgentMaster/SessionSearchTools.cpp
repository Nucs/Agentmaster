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
        LR"("query":{"type":"string","description":"Words that must ALL match a session: its title, folder, branch, the paths of files it touched, or a prompt the user typed. Use 1 to 3 distinctive words and leave filler words out. Words in \"double quotes\" match as one exact phrase. Leave it out to list sessions by the filters only."},)"
        LR"("days":{"type":"integer","description":"Only sessions active in the last N days. Default 90. Leave it out unless the user names a period."},)"
        LR"("after":{"type":"string","description":"Only sessions active on or after this local date, YYYY-MM-DD. Overrides days."},)"
        LR"("before":{"type":"string","description":"Only sessions active on or before this local date, YYYY-MM-DD."},)"
        LR"("dir":{"type":"string","description":"Part of the working folder's path."},)"
        LR"("branch":{"type":"string","description":"Part of the git branch name."},)"
        LR"("tag":{"type":"string","description":"A bookmark tag the session carries (the whole tag name)."},)"
        LR"("favorite":{"type":"boolean","description":"true: only starred sessions."},)"
        LR"("open":{"type":"boolean","description":"true: only sessions that have a tab open now."},)"
        LR"("deep":{"type":"boolean","description":"true: also search the full text of the conversations (the agent's replies and tool output). Slower and broader. Use it only when a normal search found nothing useful."},)"
        LR"("sort":{"type":"string","enum":["relevance","active","created"],"description":"relevance (default): title matches first, newest first inside a group. active / created: newest first."},)"
        LR"("limit":{"type":"integer","description":"Rows to return, 1 to 50. Default 15."})"
        LR"(},"required":[]})";
    // The schema above and the system prompt below say "90" in words the model reads.
    static_assert(kSessionSearchDefaultDays == 90, "kSearchSchema and kSystemPrompt state the default period: change their text too");

    // The grep tool (SESSIONS.md §9.12): GrepTranscripts with the search's period and folder filters.
    // `context` and `limit` say their bounds in words the model reads; ParseSessionGrepParams clamps
    // to the same numbers.
    const wchar_t kGrepSchema[] =
        LR"({"type":"object","properties":{)"
        LR"("pattern":{"type":"string","description":"The text to find, case-insensitive: words the user remembers, an error message, a version, a file name. Plain text unless regex is true."},)"
        LR"("regex":{"type":"boolean","description":"true: pattern is a regular expression (ECMAScript; . does not cross a line break, [\\s\\S] does). Keep a plain word in it: a pattern with a literal piece is much faster."},)"
        LR"("ids":{"type":"array","items":{"type":"string"},"description":"Only these sessions: ids from search or grep results. Leave it out to grep every session of the period."},)"
        LR"("days":{"type":"integer","description":"Without ids: only sessions active in the last N days. Default 90."},)"
        LR"("after":{"type":"string","description":"Without ids: only sessions active on or after this local date, YYYY-MM-DD. Overrides days."},)"
        LR"("before":{"type":"string","description":"Without ids: only sessions active on or before this local date, YYYY-MM-DD."},)"
        LR"("dir":{"type":"string","description":"Without ids: part of the working folder's path."},)"
        LR"("in":{"type":"string","enum":["all","user","claude"],"description":"all (default); user: only what the user typed; claude: only Claude's side (its replies and thinking, tool calls and their output)."},)"
        LR"("context":{"type":"integer","description":"Characters shown on each side of a match, 40 to 400. Default 120."},)"
        LR"("limit":{"type":"integer","description":"Sessions to return, 1 to 20. Default 10. Sessions where the user typed the text come first."})"
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
        LR"("ids":{"type":"array","items":{"type":"string"},"description":"The ids of the sessions to show, best match first, exactly as a result returned them. An empty list shows nothing: use it when nothing fits."},)"
        LR"("top":{"type":"integer","description":"Instead of ids: show the first N rows of your latest search, in its order."},)"
        LR"("note":{"type":"string","description":"A caption of a few words for the list, in the user's language."},)"
        LR"("why":{"type":"object","additionalProperties":{"type":"string"},"description":"For each shown id (the key, written as you pass it): one line of evidence you saw in read or grep, under 120 characters, in the user's language. The user's own words when you have them (You asked: \"...\"), else Claude's (Claude: \"...\"), else what the session shows; for a weak match, say so."})"
        LR"(},"required":[]})";

    // show's description, the same in both modes. Surface-neutral since 2026-10-17: the Telegram Search topic shows the
    // same call as one card per session (MESSAGING.md 5.4), so "the screen" / "the list" would be wrong there.
    const wchar_t kShowDescription[] =
        L"Show sessions to the user, in the given order, each with its why: the Sessions browser lists them, a phone gets "
        L"one card each. Replaces what was shown before. Call it once per request, after reading the candidates.";

    // ASCII on purpose: it travels through a file and the API unchanged whatever the code page.
    //
    // Rule 1 is the user's (2026-10-13): the model's FIRST step mentions which kinds of tool calls it can use
    // for the request, out of everything the two tools can do, and how it will use them in different ways.
    //   - It is written in the SAME message as the first tool calls, so it costs output tokens of that step
    //     (measured with the live test: SESSIONS.md 9.9), never an extra model request.
    //   - With thinking off it is the one place where the model lays out its search before it acts; the rule
    //     lists every kind of call so that the plan is chosen from all of them, not only the first one that
    //     comes to mind (the 52 s request of 2026-10-12 opened with deep:true, the slowest kind).
    //   - The page does not show it: the strip shows the turn's final sentence (the result line carries only
    //     the last message's text — checked live). It stays in the conversation record (HarnessRecord, the
    //     review's source) and in the live test's trace.
    //   - ⚠ It made the final reply wordier: in the first live run after it, both answers had two or three
    //     sentences, against the one-sentence rule (every answer before it had one). That rule (rule 6 since
    //     2026-10-17, rule 5 before) therefore says that the mention is not the reply, that a second sentence is
    //     not wanted, and shows one answer as an example (the wording alone still left 3 of 5 answers with two
    //     sentences; with the example, 1 of 6 — an answer to a question about content, with one caveat sentence;
    //     SESSIONS.md 9.9).
    //
    // Rules 4 to 6 are the user's (2026-10-17; SESSIONS.md 9.12): READ every candidate before it is shown (and grep
    // for the user's own words), give each shown session a `why` — one line of evidence the model saw — and answer
    // with a VERDICT sentence that says what was found. Before, the model showed what a search returned on its
    // titles and snippets alone, and its sentence named the screen and the best match's title ("Three sessions are
    // on the screen; ..."): wrong for the Telegram Search topic, which has no screen, and a repeat of the title the
    // list already shows. The verdict's example keeps the measured lesson above: one sentence, shown.
    // ⚠ The m5 suite pins rule 1 and the rule count (tests_harness.cpp); SESSIONS.md 9.3 lists the rules.
    const wchar_t kSystemPrompt[] =
        L"You are the search assistant of Agentmaster, an app that manages Claude Code sessions. The user looks for past "
        L"Claude Code sessions (conversations) on this computer, from the Sessions browser or from a phone. You find them, "
        L"confirm them by reading them, and show them with one line of evidence each. The user sees the sessions you show, "
        L"the evidence you give each one, and your one-sentence answer. The user does not see your tool calls or their "
        L"results.\n"
        L"\n"
        L"Tools:\n"
        L"- mcp__sessions__search finds sessions. Every word of `query` must match somewhere in a session: its title, folder, "
        L"branch, the paths of files it touched, or a prompt the user typed. Filters narrow it: days, after, before, dir, "
        L"branch, tag, favorite, open. It returns rows with a short id.\n"
        L"- mcp__sessions__grep finds exact text, or a regex, in the conversations' full text (the user's prompts, Claude's "
        L"replies, tool calls and their output) and returns each match in its context, with who wrote it and when. Give it "
        L"ids to look inside your candidates, or a period and dir to look everywhere.\n"
        L"- mcp__sessions__read reads up to 8 sessions as compact Markdown: each turn's user message and Claude's final reply. "
        L"A long session is cut to its first and last turns.\n"
        L"- mcp__display__show shows sessions to the user, in your order, and replaces what was shown before. Give it the "
        L"short ids from results, best match first, or top:N to show the first N rows of your latest search as they are, and "
        L"a `why` for each.\n"
        L"\n"
        L"Rules:\n"
        L"1. Your first step, for every request: before your first tool call, in the same message, mention in one or two short "
        L"lines which kinds of tool calls you can use for this request, out of everything your tools can do (search by words; "
        L"by folder, branch, tag, favorite or open; by a period with days, after or before; the full text with deep:true; grep "
        L"for exact words or a regex; read to confirm; sort and limit; show by ids or top:N), and how you will use them in "
        L"different ways (the words or phrasings you will try, the filters, a longer period, grep for the user's own words, "
        L"deep:true last). Then make the calls of that step at once.\n"
        L"2. Act at once. Never ask a question before you search. For every request: the first step of rule 1 (its mention and "
        L"its searches), then read the candidates, then call show exactly once, then reply.\n"
        L"3. Be fast. One search is usually enough: 1 to 3 distinctive words, no filler words. Leave `days` out (a search then "
        L"covers the last 90 days) unless the user names a period. If a search finds nothing useful, try again with other or "
        L"fewer words, then a longer period (days:365), then grep or deep:true. When the user remembers exact words, an error "
        L"or a version, grep for them. Make independent calls in one step.\n"
        L"4. Confirm before you show: read every session you intend to show (one read call, up to 8 ids), and leave out the "
        L"ones that are not about the request. Grep with ids when you need the exact place.\n"
        L"5. Show only what fits the request, best first: usually 1 to 8 sessions. Give each a `why`: one line of evidence you "
        L"saw in read or grep, under 120 characters, in the user's language. The user's own words when you have them (You "
        L"asked: \"...\"), else Claude's (Claude: \"...\"), else what the session shows; for a weak match say so (Mentions a "
        L"release only in passing). When nothing fits, call show with an empty ids list.\n"
        L"6. After show returns, reply with exactly ONE short sentence, under 20 words, in the user's language, that answers "
        L"the request: how many sessions you found and what they are about, for example: Two sessions are about releasing "
        L"Agentmaster. Never mention a screen or a list. The mention of rule 1 is not your reply: no second sentence, no "
        L"markdown, no ids.\n"
        L"7. A follow-up refers to what you showed before (\"only the open ones\", \"older ones\", \"the second one\").\n"
        L"8. Times in the results are local time. Each result gives the current local time as `now`.\n"
        L"9. A result with an `indexing` field does not have the older sessions yet. Do not search again for them: show what "
        L"fits, and say in your sentence that older sessions are still being indexed.\n";

    // The same role and rules for the SESSION DATABASE's tools (SESSIONS_DB.md §8.2): rule 1 lists the database's
    // kinds of calls, rule 3 widens with `scopes` (the database's content search is cheap, so a longer period comes
    // before more scopes), rule 4 confirms with this toolset's own `read` (or the database's messages around a
    // ref). Rules 2 and 5-9 are the page search's, word for word (rule 9 reads the same `indexing` field, which the
    // database adds while its first build runs). ASCII on purpose, like kSystemPrompt. Since 2026-10-17 the old
    // "check a doubtful candidate with session or messages" of rule 3 is rule 4's "read every candidate".
    // ⚠ The m5 suite pins its rule count, that it names every tool of SessionDbToolDefs.h, and that it never says
    //   `deep` (the database has scopes instead).
    const wchar_t kDatabaseSystemPrompt[] =
        L"You are the search assistant of Agentmaster, an app that manages Claude Code sessions. The user looks for past "
        L"Claude Code sessions (conversations) on this computer, from the Sessions browser or from a phone. You find them "
        L"with the tools of the session database, confirm them by reading them, and show them with one line of evidence "
        L"each. The user sees the sessions you show, the evidence you give each one, and your one-sentence answer. The user "
        L"does not see your tool calls or their results.\n"
        L"\n"
        L"Tools:\n"
        L"- mcp__agentmaster__search finds sessions. Every word of `query` must match somewhere in a session: its title, folder, "
        L"branch, the paths of files it touched, or a message in `scopes` (by default title, path, prompt and reply; tool adds "
        L"tool calls and their output; subagent adds the subagents' work). Filters narrow it: days, after, before, dir, branch, "
        L"tag, favorite, open. Rows carry a short id, why they matched, and snippets with a ref.\n"
        L"- To look further: mcp__agentmaster__grep finds exact text or a regex with its context; mcp__agentmaster__who_touched "
        L"lists the sessions that touched a file or folder; mcp__agentmaster__timeline lists what ran in a period; "
        L"mcp__agentmaster__session gives one session's facts; mcp__agentmaster__messages reads its conversation (around a ref); "
        L"mcp__agentmaster__files lists the files it touched; mcp__agentmaster__related finds its forks and relatives; "
        L"mcp__agentmaster__usage counts tokens; mcp__agentmaster__sql runs one read-only query.\n"
        L"- mcp__agentmaster__read reads up to 8 sessions as compact Markdown: each turn's user message and Claude's final "
        L"reply. A long session is cut to its first and last turns.\n"
        L"- mcp__display__show shows sessions to the user, in your order, and replaces what was shown before. Give it the "
        L"short ids from any result, best match first, or top:N to show the first N rows of your latest search as they are, "
        L"and a `why` for each.\n"
        L"\n"
        L"Rules:\n"
        L"1. Your first step, for every request: before your first tool call, in the same message, mention in one or two short "
        L"lines which kinds of tool calls you can use for this request, out of everything your tools can do (search by words "
        L"and scopes; by folder, branch, tag, favorite or open; by a period with days, after or before; grep for exact text; "
        L"who_touched for a file; timeline for a period; read, session or messages to check a candidate; show by ids or "
        L"top:N), and how you will use them in different ways (the words or phrasings you will try, the filters, a longer "
        L"period, more scopes). Then make the calls of that step at once.\n"
        L"2. Act at once. Never ask a question before you search. For every request: the first step of rule 1 (its mention and "
        L"its searches), then read the candidates, then call show exactly once, then reply.\n"
        L"3. Be fast. One search is usually enough: 1 to 3 distinctive words, no filler words. Leave `days` out (a search then "
        L"covers the last 90 days) unless the user names a period. If a search finds nothing useful, try again with other or "
        L"fewer words, then a longer period (days:365), then more scopes (tool). When the user remembers exact words, an "
        L"error or a version, grep for them. Make independent calls in one step.\n"
        L"4. Confirm before you show: read every session you intend to show (one read call, up to 8 ids), and leave out the "
        L"ones that are not about the request. Use grep, or messages around a ref, when you need the exact place.\n"
        L"5. Show only what fits the request, best first: usually 1 to 8 sessions. Give each a `why`: one line of evidence you "
        L"saw in read or grep, under 120 characters, in the user's language. The user's own words when you have them (You "
        L"asked: \"...\"), else Claude's (Claude: \"...\"), else what the session shows; for a weak match say so (Mentions a "
        L"release only in passing). When nothing fits, call show with an empty ids list.\n"
        L"6. After show returns, reply with exactly ONE short sentence, under 20 words, in the user's language, that answers "
        L"the request: how many sessions you found and what they are about, for example: Two sessions are about releasing "
        L"Agentmaster. Never mention a screen or a list. The mention of rule 1 is not your reply: no second sentence, no "
        L"markdown, no ids.\n"
        L"7. A follow-up refers to what you showed before (\"only the open ones\", \"older ones\", \"the second one\").\n"
        L"8. Times in the results are local time. Each result gives the current local time as `now`.\n"
        L"9. A result with an `indexing` field does not have the older sessions yet. Do not search again for them: show what "
        L"fits, and say in your sentence that older sessions are still being indexed.\n";

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
            p.days = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, 3650));
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
            p.limit = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, 50));
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
            const int64_t active = SessionSearchLastActivityMs(e);
            if (active < since || (params.beforeUnixMs != 0 && active > params.beforeUnixMs))
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
        const size_t limit = static_cast<size_t>(std::clamp(params.limit, 1, 50));
        if (rows.size() > limit)
        {
            rows.resize(limit);
        }
        outcome.rows = std::move(rows);
        return outcome;
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
        else
        {
            out += L" \x00B7 last " + std::to_wstring(params.days) + (params.days == 1 ? L" day" : L" days");
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
        std::wstring window = params.afterUnixMs != 0 ? (L"from " + LocalDateText(params.afterUnixMs)) : (L"last " + std::to_wstring(params.days) + L" days");
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
        if (outcome.matched == 0)
        {
            // With a partial index, a longer period cannot help: do not suggest it.
            root.Set(L"hint", Value::MkStr((partial || storeFilterBlind) ? L"Nothing matched in the indexed sessions. Try other or fewer words." : (params.deep ? L"Nothing matched. Try other or fewer words, or a longer period (days)." : L"Nothing matched. Try other or fewer words, a longer period (days), or deep:true.")));
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
        p.user = in != L"claude";
        p.agent = in != L"user";
        if (const auto* v = arguments.Find(L"context"); v && v->type == Type::Num)
        {
            p.context = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 40, 400));
        }
        if (const auto* v = arguments.Find(L"limit"); v && v->type == Type::Num)
        {
            p.limit = static_cast<int>(std::clamp<int64_t>(v->AsI64(), 1, 20));
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
        const SessionSearchOutcome outcome = RunSessionSearch(*catalog, *params, now, _callbacks.content, cancelled);
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
                // Where its transcript is: `read` and a `grep` with ids open it without a new catalog.
                _known[id] = KnownSession{ entry.path, Cut(OneLine(SessionSearchTitleOf(entry)), 120), SessionSearchLastActivityMs(entry), entry.sizeBytes };
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
            window = params->afterUnixMs != 0 ? (L"from " + LocalDateText(params->afterUnixMs)) : (L"last " + std::to_wstring(params->days) + L" days");
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
            const std::wstring dirLower = FoldLower(params->dir);
            for (const auto& e : catalog->entries)
            {
                const int64_t active = SessionSearchLastActivityMs(e);
                if (active < since || (params->beforeUnixMs != 0 && active > params->beforeUnixMs))
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
                    facts.emplace(s.sessionId, KnownSession{ e.path, Cut(OneLine(SessionSearchTitleOf(e)), 120), SessionSearchLastActivityMs(e), e.sizeBytes });
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
        if (order.size() > static_cast<size_t>(params.limit))
        {
            order.resize(static_cast<size_t>(params.limit));
        }

        Value root = Value::MkObj();
        root.Set(L"now", Value::MkStr(LocalDateTimeText(now)));
        root.Set(L"pattern", Value::MkStr(params.pattern));
        if (params.regex)
        {
            root.Set(L"regex", Value::MkBool(true));
        }
        root.Set(L"in", Value::MkStr(params.user && params.agent ? L"all" : (params.user ? L"user" : L"claude")));
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
                    _known[s.sessionId] = f; // `read` and a later grep with ids open it from here
                }
                if (!f.title.empty())
                {
                    o.Set(L"title", Value::MkStr(f.title));
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
        constexpr size_t kMaxShown = 100;
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
        _callbacks.onShow(resolved, note, whys);

        std::wstring text = L"Shown " + std::to_wstring(resolved.size()) + (resolved.size() == 1 ? L" session." : L" sessions.");
        if (!unknown.empty())
        {
            text += L" Not shown, unknown ids:";
            for (const auto& u : unknown)
            {
                text += L" " + u;
            }
            text += L".";
        }
        text += L" Now reply to the user with one short sentence.";
        return { text, false };
    }

    HarnessTool SessionSearchToolset::MakeReadTool()
    {
        HarnessTool read;
        read.name = kReadTool;
        read.description = L"Read up to 8 sessions as compact Markdown: each turn's user message and Claude's final reply (more with level). A long session is cut to its first and last turns. Read every session you intend to show, to confirm that it is about the request. Changes nothing the user sees.";
        read.inputSchemaJson = kReadSchema;
        read.readOnly = true; // it only reads; one model step may run it beside a search
        read.handler = [this](const json::Value& arguments) { return Read(arguments); };
        return read;
    }

    std::vector<HarnessToolServer> SessionSearchToolset::Servers()
    {
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
            return servers;
        }

        HarnessTool search;
        search.name = kSearchTool;
        search.description = L"Find Claude Code sessions on this computer. Returns matching sessions as JSON rows (id, title, dir, branch, active, created, msgs, tools, match), best match first. Changes nothing the user sees.";
        search.inputSchemaJson = kSearchSchema;
        search.readOnly = true; // several searches of one model step may run at the same time
        search.handler = [this](const json::Value& arguments) { return Search(arguments); };

        HarnessTool grep;
        grep.name = kGrepTool;
        grep.description = L"Find exact text, or a regex, in the conversations' full text: the user's prompts, Claude's replies and thinking, tool calls and their output. Returns, per session, how many messages match and the first matches in context, each with who wrote it (user or claude) and when. Use it for evidence: words the user remembers, an error, a version. Changes nothing the user sees.";
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
        return servers;
    }
}
