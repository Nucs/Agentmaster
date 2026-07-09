// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster ProcessInspect -- Fleet Observer primitives (6 partial files)
// Out-of-band process/transcript inspection (OBSERVER.md): PEB reads, transcript resolution +
// content, Codex rollout, the session-end.js summary analyzer, and window activation. Plain C++
// (no WinRT/PCH). Split from the former 4950-line ProcessInspect.cpp by section; all share the
// ProcessInspect.Internal.h primitives. The public API is declared in ProcessInspect.h.
//
// Partial files in this group (★ marks THIS file):
//   ProcessInspect.cpp             - CORE: process enumeration (Toolhelp) + PEB facts read/classify
//   ProcessInspect.Internal.h      - shared file-local primitives: x64 PEB reads, string/file helpers, transcript globbing (anonymous namespace, a per-TU copy)
//   ProcessInspect.Transcript.cpp  - Claude transcript resolution + timing/title/prompts + git plumbing + Codex (C1) facts/rollout
//   ProcessInspect.Content.cpp     - conversation-text read + the session-end.js summary analyzer + Codex (C2) rollout-tail state
//   ProcessInspect.Window.cpp      - Bring Window To Front (its own UIA-helper anon ns + the public window/tab-pick API)
// ★ ProcessInspect.Summary.cpp     - the shared summary-box renderers (the per-tab overlay + the Sessions page)
// ======================================================================================
//
// Agentmaster engine TU. ProcessInspect SUMMARY: the shared summary-box renderers (RenderSessionSummaryBox / RenderCodexSummaryBox + table-rule stripping) used by the per-tab overlay AND the Sessions page -- one renderer, no drift. Partial TU of ProcessInspect.cpp.
#include "ProcessInspect.h"

#include <windows.h>
#include <appmodel.h> // GetPackageFamilyName (host terminal: real WT vs Agentmaster vs Dev)
#include <tlhelp32.h> // CreateToolhelp32Snapshot

// Bring Window To Front (BringClaudeWindowToFront): COM + UI Automation client for the WT tab
// pick. Raw COM via WRL ComPtr — still no WinRT, still standalone-harness friendly. The explicit
// objbase/oleauto includes keep this TU independent of WIN32_LEAN_AND_MEAN trimming windows.h.
#include <objbase.h> // CoInitializeEx / CoCreateInstance
#include <oleauto.h> // SysStringLen / SysFreeString (UIA names are BSTRs)
#include <UIAutomation.h> // IUIAutomation* (tab enumeration + SelectionItem.Select)
#include <wrl/client.h> // Microsoft::WRL::ComPtr

#include <algorithm>
#include <cwctype> // towlower (tab-name heuristics)
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root
#include "Json.h" // transcript line parsing (ReadTranscriptInfo)
#include "TranscriptStore.h" // IsNoiseUserPrompt + PickDisplayTitle (the shared prompt-noise + title-precedence rules)
#include "ProcessInspect.Internal.h" // the shared file-local PEB/string/file primitives

namespace Agentmaster
{
    // ===== Agentmaster: shared summary-box renderers (TAB_OVERLAY.md summary panel + the Sessions
    // page detail). Moved here from AgentTabOverlay.cpp so the overlay and the Sessions page share
    // ONE renderer (single source of truth — they can never drift). Pure string work; the only OS
    // dependency lives in the callers (AnalyzeSessionTranscript / path resolution), so these are
    // safe to invoke off the UI thread.
    namespace
    {
        // The project-folder name = basename(dirname(transcriptPath)) — session-end.js getFolderName.
        std::wstring SummaryFolderFromPath(const std::wstring& p)
        {
            const auto s1 = p.find_last_of(L"/\\");
            if (s1 == std::wstring::npos)
            {
                return {};
            }
            const std::wstring dir = p.substr(0, s1);
            const auto s2 = dir.find_last_of(L"/\\");
            return s2 == std::wstring::npos ? dir : dir.substr(s2 + 1);
        }

        // Escape a message to ONE line (newlines/tabs -> \n / \t, like session-end.js) + truncate.
        // This detail box is ALWAYS one-line, so it always de-noises embedded tables first (the
        // wrap-off behaviour the overlay panel applies conditionally) — StripSummaryTableRules.
        // `maxChars` caps the escaped result (default 240 for the numbered messages); pass 0 for NO
        // cap — the recap is rendered in FULL.
        // Agentmaster: `wrapNewlines` (the GLOBAL summaryPanelWrapNewlines toggle) — false (default, the
        // session-end.js look) collapses a message's newlines/tabs to a literal "\n"/"\t"; true PRESERVES
        // them so a multi-line prompt reads as multiple lines. `truncate` (summaryPanelTruncate) — true
        // (default) caps at maxChars (the historic 240) with a trailing "..."; false shows the WHOLE
        // message. The defaults reproduce the prior behavior, so existing callers (the Manager Auto-Testing
        // summary, the tests) are unchanged; the Sessions page passes the live toggles. (Mirrors
        // AgentTabOverlay's SummaryEscapeMsg — the two copies are the documented "converge on a quiet day"
        // duplication, like the StateColor table.)
        std::wstring SummaryEscapeMsg(const std::wstring& mIn, bool wrapNewlines = false, bool truncate = true, size_t maxChars = 240)
        {
            const std::wstring m = StripSummaryTableRules(mIn);
            std::wstring esc;
            for (const wchar_t ch : m)
            {
                if (ch == L'\n')
                    esc += wrapNewlines ? L"\n" : L"\\n";
                else if (ch == L'\r')
                    ; // dropped
                else if (ch == L'\t')
                    esc += wrapNewlines ? L"\t" : L"\\t";
                else
                    esc += ch;
            }
            if (truncate && maxChars != 0 && esc.size() > maxChars)
            {
                esc = esc.substr(0, maxChars - 3) + L"...";
            }
            return esc;
        }
    }

    // Is `line` a table DATA row eligible for de-framing? On true, `splitBox` says which bar to split
    // on: box verticals (│ ┃ ║) vs markdown '|'. A box vertical ANYWHERE => a box row (│ never occurs in
    // prose, so splitting is always safe). Otherwise a bar-FRAMED markdown row (trimmed, first AND last
    // char '|', >=2 pipes) — the frame requirement keeps a stray prose/code pipe ("foo | grep", "| head")
    // verbatim. (file-local helper for StripSummaryTableRules)
    static bool SummaryIsTableDataRow(const std::wstring& line, bool& splitBox)
    {
        size_t b = 0, e = line.size();
        while (b < e && (line[b] == L' ' || line[b] == L'\t' || line[b] == L'\r'))
        {
            ++b;
        }
        while (e > b && (line[e - 1] == L' ' || line[e - 1] == L'\t' || line[e - 1] == L'\r'))
        {
            --e;
        }
        if (b >= e)
        {
            return false;
        }
        for (size_t i = b; i < e; ++i)
        {
            const wchar_t c = line[i];
            if (c == 0x2502 || c == 0x2503 || c == 0x2551) // │ ┃ ║
            {
                splitBox = true;
                return true;
            }
        }
        if (line[b] == L'|' && line[e - 1] == L'|')
        {
            int pipes = 0;
            for (size_t i = b; i < e; ++i)
            {
                if (line[i] == L'|')
                {
                    ++pipes;
                }
            }
            if (pipes >= 2)
            {
                splitBox = false;
                return true;
            }
        }
        return false;
    }

    // De-frame a table DATA row: split on the bar char, trim each cell, drop empty cells, rejoin the
    // cell text with " · " (U+00B7). Caller guarantees SummaryIsTableDataRow(line, splitBox) was true.
    // Defensive fallback to the trimmed line if every cell was empty (a bar-only skeleton — which the
    // rule filter should already have dropped). (file-local helper for StripSummaryTableRules)
    static std::wstring SummaryDeframeRow(const std::wstring& line, bool splitBox)
    {
        size_t b = 0, e = line.size();
        while (b < e && (line[b] == L' ' || line[b] == L'\t' || line[b] == L'\r'))
        {
            ++b;
        }
        while (e > b && (line[e - 1] == L' ' || line[e - 1] == L'\t' || line[e - 1] == L'\r'))
        {
            --e;
        }
        const auto isBar = [splitBox](wchar_t c) {
            return splitBox ? (c == 0x2502 || c == 0x2503 || c == 0x2551) : (c == L'|');
        };
        std::wstring out, cur;
        bool first = true;
        const auto flush = [&]() {
            size_t cb = 0, ce = cur.size();
            while (cb < ce && (cur[cb] == L' ' || cur[cb] == L'\t'))
            {
                ++cb;
            }
            while (ce > cb && (cur[ce - 1] == L' ' || cur[ce - 1] == L'\t'))
            {
                --ce;
            }
            if (ce > cb) // drop empty cells (incl. the leading/trailing frame's empties)
            {
                if (!first)
                {
                    out += L' ';
                    out += static_cast<wchar_t>(0x00B7); // ·
                    out += L' ';
                }
                out.append(cur, cb, ce - cb);
                first = false;
            }
            cur.clear();
        };
        for (size_t i = b; i < e; ++i)
        {
            if (isBar(line[i]))
            {
                flush();
            }
            else
            {
                cur.push_back(line[i]);
            }
        }
        flush();
        if (out.empty())
        {
            return line.substr(b, e - b);
        }
        return out;
    }

    // Agentmaster: see ProcessInspect.h. Collapse an embedded table for one-line display — DROP its
    // horizontal RULE rows AND DE-FRAME its data rows (strip │/| bars + padding -> cells joined by " · ").
    std::wstring StripSummaryTableRules(const std::wstring& msg)
    {
        // A physical line is a droppable table RULE row iff, after trimming leading/trailing spaces /
        // tabs / CR, it is non-empty, composed ENTIRELY of table-structure chars, and rule-shaped (has
        // a box-drawing char, or a >=3 run of -/=/~). Any other char (letter, digit, or punctuation
        // outside the markdown set) marks it a DATA row -> kept.
        const auto isRuleRow = [](const std::wstring& line) -> bool {
            size_t b = 0, e = line.size();
            while (b < e && (line[b] == L' ' || line[b] == L'\t' || line[b] == L'\r'))
            {
                ++b;
            }
            while (e > b && (line[e - 1] == L' ' || line[e - 1] == L'\t' || line[e - 1] == L'\r'))
            {
                --e;
            }
            if (b >= e)
            {
                return false; // blank line -> not a rule (passes through verbatim)
            }
            bool sawBox = false;
            int run = 0, maxRun = 0; // longest run of FILL chars (a >=3 run is an ASCII horizontal rule)
            for (size_t i = b; i < e; ++i)
            {
                const wchar_t c = line[i];
                const bool box = (c >= 0x2500 && c <= 0x257F); // box-drawing block: ─ │ ┼ ├ ┤ ┌ … ═ ╪ …
                // FILL = the chars a horizontal rule / thematic break is drawn from; a run of >=3 = a rule.
                // -=~ (markdown/setext + box ASCII) plus #*_ (markdown thematic breaks — all observed live).
                const bool fill = (c == L'-' || c == L'=' || c == L'~' || c == L'#' || c == L'*' || c == L'_');
                // GLUE = the rest of the table vocabulary: cell bars / alignment / corner (never a fill run).
                const bool glue = (c == L'+' || c == L':' || c == L'|');
                if (!box && !fill && !glue && c != L' ' && c != L'\t')
                {
                    return false; // a content char -> a DATA row (keep it)
                }
                if (box)
                {
                    sawBox = true;
                }
                if (fill)
                {
                    if (++run > maxRun)
                    {
                        maxRun = run;
                    }
                }
                else
                {
                    run = 0;
                }
            }
            return sawBox || maxRun >= 3;
        };

        // Split on '\n' (each piece keeps its own trailing '\r'; isRuleRow trims it). Classify once.
        std::vector<std::wstring> lines;
        {
            std::wstring cur;
            for (const wchar_t ch : msg)
            {
                if (ch == L'\n')
                {
                    lines.push_back(std::move(cur));
                    cur.clear();
                }
                else
                {
                    cur.push_back(ch);
                }
            }
            lines.push_back(std::move(cur));
        }
        // Per line: 0 = keep verbatim, 1 = drop (rule row), 2 = de-frame (box bars), 3 = de-frame ('|').
        std::vector<char> action(lines.size(), 0);
        bool anyDropped = false, anyKept = false, anyDeframe = false;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (isRuleRow(lines[i]))
            {
                action[i] = 1;
                anyDropped = true;
                continue;
            }
            anyKept = true;
            bool splitBox = false;
            if (SummaryIsTableDataRow(lines[i], splitBox))
            {
                action[i] = splitBox ? 2 : 3;
                anyDeframe = true;
            }
        }
        // Every line was a rule (a degenerate all-grid message) -> leave it unchanged so a numbered
        // bullet never renders empty. Nothing to drop AND nothing to de-frame -> a cheap no-op too.
        if (!anyKept || (!anyDropped && !anyDeframe))
        {
            return msg;
        }
        // Rebuild: drop rule rows, de-frame data rows, keep the rest verbatim (trailing '\r' -> LF),
        // joined by '\n'.
        std::wstring out;
        out.reserve(msg.size());
        bool first = true;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (action[i] == 1)
            {
                continue;
            }
            const std::wstring& ln = lines[i];
            size_t len = ln.size();
            if (len > 0 && ln[len - 1] == L'\r')
            {
                --len;
            }
            const std::wstring kept = ln.substr(0, len);
            if (!first)
            {
                out += L'\n';
            }
            if (action[i] == 2 || action[i] == 3)
            {
                out += SummaryDeframeRow(kept, action[i] == 2);
            }
            else
            {
                out += kept;
            }
            first = false;
        }
        return out;
    }

    static std::wstring RenderSessionSummaryBoxImpl(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full, bool wrapNewlines, bool truncate);
    // Agentmaster (extra-safe): the renderers do NOT self-contain (the tab-tooltip lane's hardening
    // comment said exactly that) yet run on background threads / fire_and_forget coroutines with no
    // frame to catch a throw — a std::bad_alloc mid-string-build was a process kill (the v0.6.x
    // resume crash-loop class). Contain + return empty (the caller's "nothing to render" path).
    std::wstring RenderSessionSummaryBox(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full, bool wrapNewlines, bool truncate)
    {
        try
        {
            return RenderSessionSummaryBoxImpl(a, id, cwd, transcriptPath, resumeCmd, liveGlyph, liveLabel, planFile, full, wrapNewlines, truncate);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] RenderSessionSummaryBox: swallowed exception (no crash)\n");
            return {};
        }
    }

    static std::wstring RenderSessionSummaryBoxImpl(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full, bool wrapNewlines, bool truncate)
    {
        std::wstring glyph = liveGlyph, label = liveLabel;
        const bool isPlan = a.hasPlanContent || a.hasExitPlanMode;
        if (a.hasPlanContent)
        {
            glyph = L"\U0001F680"; // 🚀
            label = L"plan-start";
        }
        else if (a.hasExitPlanMode)
        {
            glyph = L"\U0001F4CB"; // 📋
            label = L"plan-end";
        }

        std::wstring o;
        const auto line = [&o](const std::wstring& s) { o += s; o += L"\n"; };
        // A section divider: a lone sentinel line, suppressed at the very top (a leading rule with
        // nothing above it reads as a stray bar). The display turns it into a full-width Border rule.
        const auto sep = [&o]() { if (!o.empty()) { o += kSummarySepMark; o += L"\n"; } };

        // Header: a live-state line when full AND a live state was passed, OR a plan signal. A caller
        // with no live state (the Sessions page passes an empty label) suppresses the otherwise-
        // redundant header while still surfacing plan-start/plan-end (isPlan overrides glyph+label).
        if ((full && !label.empty()) || isPlan)
        {
            line(glyph + L"  " + label);
        }
        if (full)
        {
            line(id);
        }
        if (a.hasPlanContent && !a.parentSessionId.empty())
        {
            line(L"Parent: " + a.parentSessionId);
            if (!planFile.empty())
            {
                line(L"Plan:   " + planFile);
            }
        }
        else if (!planFile.empty())
        {
            line(L"Plan:   " + planFile);
        }
        if (full)
        {
            line(L"Dir:    " + cwd);
            if (const std::wstring folder = SummaryFolderFromPath(transcriptPath); !folder.empty())
            {
                line(L"Folder: " + folder);
            }
            line(L"Resume: " + resumeCmd);
        }
        if (full && !a.branch.empty())
        {
            line(L"Branch: " + a.branch);
        }
        if (a.tasksCompleted > 0 || a.tasksPending > 0)
        {
            line(L"Tasks:  " + std::to_wstring(a.tasksCompleted) + L" done / " + std::to_wstring(a.tasksPending) + L" pending");
        }
        // Agentmaster: the Claude Code idle RECAP — its own section directly above the Messages list (so
        // it reads as the "where we are / what's next" header over the prompt history). Shown in BOTH the
        // displayed panel (full=false) and the copyable Summary (full=true).
        if (!a.awaySummary.empty())
        {
            sep();
            line(L"Recap: " + SummaryEscapeMsg(a.awaySummary, wrapNewlines, /*truncate*/ false)); // label INLINE; the recap is ALWAYS shown in FULL (never capped by the truncate toggle — only the numbered messages honor it)
        }
        if (!a.userMsgs.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : a.userMsgs)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m, wrapNewlines, truncate));
            }
        }
        if (!a.filesRead.empty())
        {
            sep();
            line(L"Files Read:");
            for (const auto& f : a.filesRead)
            {
                line(L"* " + f);
            }
        }
        if (!a.filesCreated.empty())
        {
            sep();
            line(L"Files Created:");
            for (const auto& f : a.filesCreated)
            {
                line(L"* " + f);
            }
        }
        if (!a.filesEdited.empty())
        {
            sep();
            line(L"Files Edited:");
            for (const auto& f : a.filesEdited)
            {
                line(L"* " + f);
            }
        }
        while (!o.empty() && o.back() == L'\n')
        {
            o.pop_back();
        }
        return o;
    }

    static std::wstring RenderCodexSummaryBoxImpl(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full);
    // Agentmaster (extra-safe): same containment as RenderSessionSummaryBox above.
    std::wstring RenderCodexSummaryBox(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full)
    {
        try
        {
            return RenderCodexSummaryBoxImpl(info, id, cwd, transcriptPath, resumeCmd, liveGlyph, liveLabel, full);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] RenderCodexSummaryBox: swallowed exception (no crash)\n");
            return {};
        }
    }

    static std::wstring RenderCodexSummaryBoxImpl(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full)
    {
        std::wstring o;
        const auto line = [&o](const std::wstring& s) { o += s; o += L"\n"; };
        const auto sep = [&o]() { if (!o.empty()) { o += kSummarySepMark; o += L"\n"; } };

        if (full)
        {
            line(liveGlyph + L"  " + liveLabel + L"  \x00B7 codex");
            line(id);
            line(L"Dir:    " + cwd);
            if (const std::wstring folder = SummaryFolderFromPath(transcriptPath); !folder.empty())
            {
                line(L"Folder: " + folder);
            }
            line(L"Resume: " + resumeCmd);
            std::wstring me;
            const auto add = [&me](const std::wstring& p) { if (!p.empty()) { if (!me.empty()) me += L" \x00B7 "; me += p; } };
            add(info.model);
            add(info.effort);
            add(info.sandbox);
            if (!me.empty())
            {
                line(L"Model:  " + me);
            }
            if (!info.gitBranch.empty())
            {
                line(L"Branch: " + info.gitBranch);
            }
        }
        if (!info.userPrompts.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : info.userPrompts)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m));
            }
        }
        while (!o.empty() && o.back() == L'\n')
        {
            o.pop_back();
        }
        return o;
    }
}
