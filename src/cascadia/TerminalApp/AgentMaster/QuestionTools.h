// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — QUESTION TOOLS (HOOKS.md "Question tools", SETTINGS.md "Claude").
//
// A question tool asks the user something and WAITS for the answer, the way Claude Code's own AskUserQuestion does.
// While a session's newest tool call is one of them and has no result yet, the session is blocked on the user, not
// working: the scanner reads it as NeedsApproval (recon-block), so the Triage Board, the tab's dot, the toast and the
// flash ring say "needs you", and the Prompt Queue Runner never sends into it (Rule #1).
//
// AskUserQuestion is BUILT IN: it is always a question, whatever the list says (claude's own heartbeat also reports it
// as "waiting"). The list adds the tools Claude Code knows nothing about — an MCP tool such as the
// ask-user-html-question mod's mcp__ask-user-html-question__AskUserHtmlQuestion, during which claude's heartbeat reads
// "busy" for the whole question (measured 2026-11-14: an 8-minute question, "busy" throughout, its call line written
// 0.2 s after the question opened and the answer only when the user answered).
//
// The list is the cog's QUESTION TOOLS box (AppSettings::questionTools): one pattern per line, a full tool name or
// one with '*' standing for any run of characters, matched case-sensitively against the WHOLE name (the way Claude
// Code compares tool names). '#' starts a note. Only the characters Claude Code leaves in a tool name are accepted
// (letters, digits, '_', '-'; an MCP server's or tool's other characters become '_') plus '*'.
//
// PURE (no Win32, no WinRT) apart from one process-wide slot holding the list in effect, so the scanner thread, the
// summary analyzer and the CLI all judge a tool by the same list. The cog's Save and the engine's init set it
// (SetQuestionToolPatterns); a process that never sets it judges by the shipped default.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Agentmaster
{
    // The one question tool that needs no line: Claude Code's own AskUserQuestion. Always a question, whatever the list
    // says, so a cleared box can never stop the oldest, most-used question from reading NeedsApproval.
    inline constexpr std::wstring_view kBuiltInQuestionTool = L"AskUserQuestion";

    // The list a settings.json without the key gets (AppSettings::questionTools' initializer, Persistence's
    // presence-gated load) and the one a process that never applied settings judges by: the ask-user-html-question
    // mod's tool, the question tool this machine ships. A user who clears the box keeps it cleared (the key is then
    // present and empty).
    inline constexpr std::wstring_view kDefaultQuestionTools = L"mcp__ask-user-html-question__AskUserHtmlQuestion";

    // At most this many patterns apply; a line past the cap is reported (Warn) and ignored. Bounds the per-tool-call
    // match cost on the scanner thread (every assistant line's tool names are matched against every pattern).
    inline constexpr size_t kMaxQuestionTools = 64;

    // A pattern longer than this is refused (Error). No real tool name comes near it (Claude Code's own limit for a mod
    // tool's name part is 64 characters); the bound only stops a pasted paragraph from becoming a "tool name".
    inline constexpr size_t kMaxQuestionToolPatternChars = 256;

    // PURE: may this character appear in a question-tool pattern?
    //   c - one UTF-16 code unit of a pattern line.
    // Returns true for ASCII letters, digits, '_', '-' (the characters Claude Code keeps in a tool name) and '*' (the
    // wildcard); false for anything else, which the lexer reports as an Error naming the character. Never throws.
    inline bool IsQuestionToolPatternChar(wchar_t c) noexcept
    {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L'*';
    }

    // PURE: does `name` match the glob `pattern`? '*' stands for any run of characters (the empty run too); every other
    // character matches itself, case-SENSITIVELY, and the pattern must cover the WHOLE name (anchored at both ends), so
    // "mcp__a__Ask" never matches inside "mcp__a__AskLater". The iterative two-pointer form with one backtrack point:
    // linear in practice, never recursive, so a hostile pattern ("*a*a*a*…") cannot blow the stack.
    //   pattern - one lexed pattern (any characters work here; the lexer is what restricts them).
    //   name    - a tool_use block's `name`, as the transcript records it.
    // Returns true when the pattern covers the whole name; an empty pattern matches only the empty name. Never throws.
    inline bool QuestionToolGlobMatch(std::wstring_view pattern, std::wstring_view name) noexcept
    {
        constexpr size_t npos = std::wstring_view::npos;
        size_t p = 0;
        size_t n = 0;
        size_t starAt = npos; // the pattern index of the newest '*' (the backtrack point)
        size_t starName = 0; // the name index that '*' currently stops before
        while (n < name.size())
        {
            if (p < pattern.size() && pattern[p] == L'*')
            {
                // A '*' first matches the empty run; a later mismatch widens it by one character (below).
                starAt = p++;
                starName = n;
            }
            else if (p < pattern.size() && pattern[p] == name[n])
            {
                ++p;
                ++n;
            }
            else if (starAt != npos)
            {
                // Mismatch after a '*': let that '*' swallow one more character of the name and retry from just after it.
                p = starAt + 1;
                n = ++starName;
            }
            else
            {
                return false; // a literal mismatch with no '*' to widen
            }
        }
        // The name is used up: only trailing '*' (each matching the empty run) may remain in the pattern.
        while (p < pattern.size() && pattern[p] == L'*')
        {
            ++p;
        }
        return p == pattern.size();
    }

    // The verdict on one line of the cog's QUESTION TOOLS box. Ordered by severity: the lexer's `worst` is the maximum,
    // and the cog paints the box's border from it (neutral / green / amber / red).
    enum class QuestionToolLineKind
    {
        Ignored, // a blank line or a '#' note: nothing applies, nothing to report
        Ok, // a pattern that applies
        Warn, // applies (or is harmlessly skipped) but worth saying: AskUserQuestion listed, a repeat, past the cap, too broad
        Error, // refused: a character no tool name holds, nothing but '*', or too long
    };

    // One line's diagnosis, for the status line under the box.
    struct QuestionToolLineDiag
    {
        uint32_t line{}; // 1-based line number in the box, so the status line can point at it
        QuestionToolLineKind kind{ QuestionToolLineKind::Ignored };
        std::wstring pattern; // the trimmed line ("" for a blank line)
        std::wstring message; // a human sentence for Warn / Error, "" otherwise
    };

    // Everything the lexer learned from the box: the patterns that apply, per-line verdicts and the counts the cog's
    // status line is built from.
    struct QuestionToolsLex
    {
        std::vector<std::wstring> patterns; // what applies, in line order: valid, not AskUserQuestion, no repeats, at most kMaxQuestionTools
        std::vector<QuestionToolLineDiag> lines; // one entry per non-blank line, in order
        uint32_t ok{}; // lines whose pattern applies (== patterns.size(); a too-broad Warn line is counted here too)
        uint32_t warn{}; // Warn lines
        uint32_t error{}; // Error lines (refused)
        QuestionToolLineKind worst{ QuestionToolLineKind::Ok }; // the most severe verdict (Ok when every line is fine or the box is empty)
        uint32_t firstIssueLine{}; // the 1-based line of the first Warn / Error, 0 when none
        std::wstring firstIssue; // that line's message, "" when none
    };

    namespace detail
    {
        // Claude Code's own tools that run without waiting for the user. A pattern that matches one of them is almost
        // certainly too broad (a long Bash would read Needs approval after two quiet seconds), so the lexer warns. The
        // list needs no completeness: it only catches the obvious mistakes ("B*", "*e*", "*Search").
        inline constexpr std::wstring_view kWorkingBuiltInTools[] = {
            L"Agent", L"Bash", L"Edit", L"Glob", L"Grep", L"NotebookEdit", L"Read", L"Skill", L"Task", L"TodoWrite", L"ToolSearch", L"WebFetch", L"WebSearch", L"Write",
        };

        // Trim ASCII spaces and tabs from both ends of a line.
        //   s - the raw line.
        // Returns the trimmed view (a view into `s`). Never throws.
        inline std::wstring_view TrimQuestionToolLine(std::wstring_view s) noexcept
        {
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
            {
                s.remove_suffix(1);
            }
            return s;
        }

        // A character as the status line names it ("a space", "a tab", or the character in quotes).
        //   c - the offending character.
        // Returns the phrase. Throws std::bad_alloc only.
        inline std::wstring DescribeQuestionToolChar(wchar_t c)
        {
            if (c == L' ')
            {
                return L"a space";
            }
            if (c == L'\t')
            {
                return L"a tab";
            }
            return std::wstring{ L"\x201C" } + c + L"\x201D";
        }
    }

    // PURE: read the cog's QUESTION TOOLS box (or the stored AppSettings::questionTools, which is the box's text
    // verbatim) into the patterns that apply and a verdict per line. One rule set serves the live border and status
    // line AND the engine (ParseQuestionToolPatterns is this function's `patterns`), so what the box calls green is
    // exactly what the scanner applies.
    //   text - the box's text. Lines break at '\n', '\r' or "\r\n" (a XAML TextBox stores '\r'; a hand-edited
    //          settings.json may hold either). Each line is trimmed of spaces and tabs at both ends.
    // Returns the lex: Ignored for a blank line or a '#' note; Error (refused, never applied) for a character other than
    // letters, digits, '_', '-' and '*', a line of nothing but '*' (it would make every tool call read Needs approval),
    // or a line longer than kMaxQuestionToolPatternChars; Warn (not added) for AskUserQuestion itself (built in), a
    // pattern listed twice, or a pattern past kMaxQuestionTools; Warn (but applied) for a pattern that also matches one
    // of Claude Code's own working tools; Ok otherwise. Throws std::bad_alloc only.
    inline QuestionToolsLex LexQuestionToolsText(std::wstring_view text)
    {
        QuestionToolsLex out;
        const auto note = [&out](QuestionToolLineDiag diag) {
            if (diag.kind == QuestionToolLineKind::Warn)
            {
                ++out.warn;
            }
            else if (diag.kind == QuestionToolLineKind::Error)
            {
                ++out.error;
            }
            if (static_cast<int>(diag.kind) > static_cast<int>(out.worst))
            {
                out.worst = diag.kind;
            }
            if ((diag.kind == QuestionToolLineKind::Warn || diag.kind == QuestionToolLineKind::Error) && out.firstIssueLine == 0)
            {
                out.firstIssueLine = diag.line;
                out.firstIssue = diag.message;
            }
            out.lines.push_back(std::move(diag));
        };

        uint32_t lineNo = 0;
        size_t start = 0;
        while (start <= text.size())
        {
            // Find this line's end: the next '\r' or '\n'. A "\r\n" pair is ONE break, so a CRLF file numbers its
            // lines like the box shows them.
            size_t end = start;
            while (end < text.size() && text[end] != L'\r' && text[end] != L'\n')
            {
                ++end;
            }
            ++lineNo;
            const std::wstring_view line = detail::TrimQuestionToolLine(text.substr(start, end - start));
            if (end >= text.size())
            {
                start = text.size() + 1; // the last line: stop after it
            }
            else
            {
                start = end + ((text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n') ? 2 : 1);
            }

            if (line.empty() || line.front() == L'#')
            {
                if (!line.empty())
                {
                    out.lines.push_back(QuestionToolLineDiag{ lineNo, QuestionToolLineKind::Ignored, std::wstring{ line }, {} });
                }
                continue; // a blank line or a note: nothing to apply
            }

            QuestionToolLineDiag diag{ lineNo, QuestionToolLineKind::Ok, std::wstring{ line }, {} };
            // Refusals first: a refused line never applies, and its message names the first reason.
            if (line.size() > kMaxQuestionToolPatternChars)
            {
                diag.kind = QuestionToolLineKind::Error;
                diag.message = L"longer than " + std::to_wstring(kMaxQuestionToolPatternChars) + L" characters: no tool name is that long";
                note(std::move(diag));
                continue;
            }
            bool refused = false;
            bool onlyStars = true;
            for (const wchar_t c : line)
            {
                if (!IsQuestionToolPatternChar(c))
                {
                    diag.kind = QuestionToolLineKind::Error;
                    diag.message = detail::DescribeQuestionToolChar(c) + L" cannot be part of a tool name (letters, digits, _, - and * only)";
                    refused = true;
                    break;
                }
                onlyStars = onlyStars && c == L'*';
            }
            if (!refused && onlyStars)
            {
                diag.kind = QuestionToolLineKind::Error;
                diag.message = L"\x201C" + diag.pattern + L"\x201D matches every tool: every tool call would read Needs approval";
                refused = true;
            }
            if (refused)
            {
                note(std::move(diag));
                continue;
            }

            // Lines that are fine but add nothing: the built-in question, a repeat, a pattern past the cap.
            if (line == kBuiltInQuestionTool)
            {
                diag.kind = QuestionToolLineKind::Warn;
                diag.message = L"AskUserQuestion is built in: it is always a question without a line";
                note(std::move(diag));
                continue;
            }
            bool repeat = false;
            for (const auto& applied : out.patterns)
            {
                repeat = repeat || applied == line;
            }
            if (repeat)
            {
                diag.kind = QuestionToolLineKind::Warn;
                diag.message = L"\x201C" + diag.pattern + L"\x201D is listed twice";
                note(std::move(diag));
                continue;
            }
            if (out.patterns.size() >= kMaxQuestionTools)
            {
                diag.kind = QuestionToolLineKind::Warn;
                diag.message = L"past the " + std::to_wstring(kMaxQuestionTools) + L"-tool limit: ignored";
                note(std::move(diag));
                continue;
            }

            // Applies. A pattern that also covers one of Claude Code's own working tools is applied as typed but called
            // out: that tool would read Needs approval whenever it runs for two quiet seconds.
            out.patterns.push_back(diag.pattern);
            ++out.ok;
            for (const auto builtIn : detail::kWorkingBuiltInTools)
            {
                if (QuestionToolGlobMatch(line, builtIn))
                {
                    diag.kind = QuestionToolLineKind::Warn;
                    diag.message = L"\x201C" + diag.pattern + L"\x201D also matches Claude Code's own " + std::wstring{ builtIn } +
                                   L" tool, which never waits for you";
                    break;
                }
            }
            note(std::move(diag));
        }
        return out;
    }

    // PURE: the patterns that apply for this box text — exactly LexQuestionToolsText(text).patterns. What the engine
    // installs (SetQuestionToolPatterns) on init and on every cog Save.
    //   text - the stored AppSettings::questionTools (the box's text verbatim).
    // Returns the applied patterns, in line order (never AskUserQuestion: it is built in). Throws std::bad_alloc only.
    inline std::vector<std::wstring> ParseQuestionToolPatterns(std::wstring_view text)
    {
        return LexQuestionToolsText(text).patterns;
    }

    // PURE: is `toolName` a question tool under `patterns`? The built-in AskUserQuestion always is.
    //   toolName - a tool_use block's name ("" is never a question tool).
    //   patterns - the applied patterns (ParseQuestionToolPatterns' result).
    // Returns true when the tool waits for the user. Never throws.
    inline bool IsQuestionToolIn(std::wstring_view toolName, const std::vector<std::wstring>& patterns) noexcept
    {
        if (toolName.empty())
        {
            return false;
        }
        if (toolName == kBuiltInQuestionTool)
        {
            return true;
        }
        for (const auto& pattern : patterns)
        {
            if (QuestionToolGlobMatch(pattern, toolName))
            {
                return true;
            }
        }
        return false;
    }

    namespace detail
    {
        // The process-wide list in effect. Starts EMPTY (nullptr), which means "never set": IsQuestionTool then judges by
        // the shipped default, without allocating. A function-local static so the header needs no .cpp (one slot per
        // module: TerminalApp.dll, the CLI exe and the test harness each have their own, each set by its own startup).
        // std::atomic<std::shared_ptr> makes a swap from the UI thread (the cog's Save) safe against a concurrent read on
        // the scanner thread: a reader keeps the snapshot it loaded alive while it matches against it.
        // Returns the slot. Never throws (constant initialization).
        inline std::atomic<std::shared_ptr<const std::vector<std::wstring>>>& QuestionToolPatternsSlot() noexcept
        {
            static std::atomic<std::shared_ptr<const std::vector<std::wstring>>> slot;
            return slot;
        }
    }

    // Install the question tools this process judges by (the engine's init and the cog's Save; the CLI at startup). The
    // scanner judges a session's pending tool call against the list on EVERY pass, so a change applies to a question
    // that is already open, within one pass.
    //   patterns - the applied patterns (ParseQuestionToolPatterns of AppSettings::questionTools); empty = no tool
    //              beyond the built-in AskUserQuestion.
    // Throws std::bad_alloc only (the snapshot's allocation); on a throw the previous list stays in effect.
    inline void SetQuestionToolPatterns(std::vector<std::wstring> patterns)
    {
        auto snapshot = std::make_shared<const std::vector<std::wstring>>(std::move(patterns));
        detail::QuestionToolPatternsSlot().store(std::move(snapshot));
    }

    // The question tools in effect, as one immutable snapshot.
    // Returns the installed list, or the shipped default (ParseQuestionToolPatterns(kDefaultQuestionTools)) when nothing
    // was installed in this process. Throws std::bad_alloc only (only in the never-installed case, which allocates the
    // default's copy).
    inline std::shared_ptr<const std::vector<std::wstring>> QuestionToolPatterns()
    {
        if (auto snapshot = detail::QuestionToolPatternsSlot().load())
        {
            return snapshot;
        }
        return std::make_shared<const std::vector<std::wstring>>(ParseQuestionToolPatterns(kDefaultQuestionTools));
    }

    // Is `toolName` a question tool under the list in effect in this process? The built-in AskUserQuestion always is.
    // What the scanner (IsInteractiveTool, SessionScanner.h), the summary analyzer's "last user msg" clock and the CLI ask.
    //   toolName - a tool_use block's name ("" is never a question tool).
    // Returns true when the tool waits for the user. Never throws: a never-installed list is judged against the shipped
    // default directly (no allocation), and an installed one through the snapshot the atomic load hands out.
    inline bool IsQuestionTool(std::wstring_view toolName) noexcept
    {
        if (toolName.empty())
        {
            return false;
        }
        if (toolName == kBuiltInQuestionTool)
        {
            return true;
        }
        const auto snapshot = detail::QuestionToolPatternsSlot().load();
        if (!snapshot)
        {
            // Never installed: the shipped default is one plain name, so a direct match equals the parsed list's verdict.
            return QuestionToolGlobMatch(kDefaultQuestionTools, toolName);
        }
        return IsQuestionToolIn(toolName, *snapshot);
    }
}
