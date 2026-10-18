// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See SessionSearch.h / SESSIONS.md §6.
#define NOMINMAX
#include "SessionSearch.h"

#include <windows.h>

#include <algorithm>
#include <atomic> // the grep's reader threads
#include <cwctype> // towlower
#include <optional>
#include <thread>
#include <unordered_set>

#include "ClaudeSpawn.h" // LogSwallowedException (the grep's reader threads)
#include "Json.h" // history.jsonl line parsing
#include "RegexUtil.h" // detail::BuildRegex, kRegexMaxPatternChars (the grep's regex)

namespace
{
    using namespace Agentmaster;

    // Keep one rg invocation's command line safely under the 32k CreateProcess limit.
    constexpr size_t kMaxCmdlineChars = 12000;
    // Bound a single rg run (a wedge must never hang the search thread forever).
    constexpr DWORD kRgTimeoutMs = 30000;
    // Per-line text budgets for the in-process content scan (snippets only need the head).
    constexpr size_t kScanTextBudget = 4096;

    std::wstring Utf8ToWide(const char* data, size_t len)
    {
        if (len == 0)
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring w(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), w.data(), n);
        return w;
    }

    // Quote one argument for a CreateProcess command line (standard MSVC rules, simplified:
    // wrap in quotes, double trailing backslashes, escape embedded quotes).
    std::wstring QuoteArg(std::wstring_view a)
    {
        std::wstring out = L"\"";
        size_t backslashes = 0;
        for (const wchar_t c : a)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (c == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
            }
            else
            {
                out.append(backslashes, L'\\');
                out.push_back(c);
            }
            backslashes = 0;
        }
        out.append(backslashes * 2, L'\\');
        out.push_back(L'"');
        return out;
    }

    // Run a console tool with no window, capture its stdout (UTF-8 bytes). False on spawn
    // failure or timeout (the child is terminated). Exit code is NOT an error signal here —
    // rg exits 1 on "no matches", which is a perfectly good empty result.
    //   exitCode — optional: receives the child's exit code when the call returns true, for a
    //              caller that must tell rg's error (2) from its "no match" (1). STILL_ACTIVE
    //              (259) when the child had not ended 5 s after it closed its output.
    bool RunToolCapture(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs, std::string& out, DWORD* exitCode = nullptr)
    {
        out.clear();
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!::CreatePipe(&readEnd, &writeEnd, &sa, 0))
        {
            return false;
        }
        ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        std::wstring cmdline = QuoteArg(exe) + L" " + args; // CreateProcessW mutates the buffer
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd;
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE); // rg noise goes wherever ours does
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL ok = ::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        ::CloseHandle(writeEnd); // ours would keep the pipe open past child exit
        if (!ok)
        {
            ::CloseHandle(readEnd);
            return false;
        }
        ::CloseHandle(pi.hThread);

        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        char buf[64 << 10];
        for (;;)
        {
            DWORD got = 0;
            if (!::ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) || got == 0)
            {
                break; // EOF: the child closed stdout (exited)
            }
            out.append(buf, got);
            if (::GetTickCount64() > deadline)
            {
                ::TerminateProcess(pi.hProcess, 1);
                ::CloseHandle(readEnd);
                ::CloseHandle(pi.hProcess);
                return false;
            }
        }
        ::CloseHandle(readEnd);
        ::WaitForSingleObject(pi.hProcess, 5000);
        if (exitCode)
        {
            DWORD code = STILL_ACTIVE;
            *exitCode = ::GetExitCodeProcess(pi.hProcess, &code) ? code : STILL_ACTIVE;
        }
        ::CloseHandle(pi.hProcess);
        return true;
    }

    // Split captured tool stdout into trimmed non-empty lines (UTF-8 -> wide).
    std::vector<std::wstring> CaptureLines(const std::string& bytes)
    {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (size_t i = 0; i <= bytes.size(); ++i)
        {
            if (i < bytes.size() && bytes[i] != '\n')
            {
                continue;
            }
            std::string_view line(bytes.data() + start, i - start);
            start = i + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                out.push_back(Utf8ToWide(line.data(), line.size()));
            }
        }
        return out;
    }

    // `rg -il` over a path batch: the files containing at least one match. nullopt-equivalent
    // (false) on spawn failure — or a batch rg could not search at all — so callers fall back to
    // scanning everything.
    // The `--regexp` arguments of an rg call: one per pattern, so a file matches when it holds ANY
    // of them (a grep's alternation: one literal per alternative).
    std::wstring RgRegexpArgs(const std::vector<std::wstring>& regexes)
    {
        std::wstring args;
        for (const auto& re : regexes)
        {
            args += L" --regexp " + QuoteArg(re);
        }
        return args;
    }

    // `rg -il` over a path batch: the files holding at least one match of ANY of `regexes`. False
    // on spawn failure — or a batch rg could not search at all — so callers fall back to scanning
    // everything.
    bool RgFilesWithMatchesAny(const std::wstring& rg, const std::vector<std::wstring>& regexes, const std::vector<std::wstring>& paths, std::vector<std::wstring>& matched);

    bool RgFilesWithMatches(const std::wstring& rg, const std::wstring& regex, const std::vector<std::wstring>& paths, std::vector<std::wstring>& matched)
    {
        return RgFilesWithMatchesAny(rg, std::vector<std::wstring>{ regex }, paths, matched);
    }

    // ONE `rg -il` over a whole projects folder: every transcript directly in a project folder
    // (`<root>\<project>\<id>.jsonl`, depth 2 — never a subagent's file, which sits deeper) that
    // holds a match of ANY of `regexes`. One process instead of one per 64 files: rg reads the
    // folder with all its threads (9.3 GB in 1.3 s warm, measured 2026-10-18). False on spawn
    // failure, a timeout, or an error with no output; the caller then filters in batches.
    bool RgFilesWithMatchesInTree(const std::wstring& rg, const std::vector<std::wstring>& regexes, const std::wstring& root, std::vector<std::wstring>& matched)
    {
        const std::wstring args = L"-il --no-config --no-messages --max-depth 2 -g " + QuoteArg(L"*.jsonl") + RgRegexpArgs(regexes) + L" -- " + QuoteArg(root);
        std::string out;
        DWORD exitCode = 0;
        if (!RunToolCapture(rg, args, kRgTimeoutMs, out, &exitCode))
        {
            return false;
        }
        if (exitCode == 2 && out.empty())
        {
            return false; // rg could not search at all (RgFilesWithMatchesAny's rule)
        }
        for (auto& line : CaptureLines(out))
        {
            matched.push_back(std::move(line));
        }
        return true;
    }

    bool RgFilesWithMatchesAny(const std::wstring& rg, const std::vector<std::wstring>& regexes, const std::vector<std::wstring>& paths, std::vector<std::wstring>& matched)
    {
        size_t i = 0;
        while (i < paths.size())
        {
            std::wstring args = L"-il --no-config --no-messages" + RgRegexpArgs(regexes) + L" --";
            size_t taken = 0;
            while (i < paths.size() && args.size() + paths[i].size() + 3 < kMaxCmdlineChars)
            {
                args += L" " + QuoteArg(paths[i]);
                ++i;
                ++taken;
            }
            if (taken == 0)
            {
                ++i; // one pathological path longer than the whole budget: skip it
                continue;
            }
            std::string out;
            DWORD exitCode = 0;
            if (!RunToolCapture(rg, args, kRgTimeoutMs, out, &exitCode))
            {
                return false;
            }
            // rg's exit codes: 0 = a match, 1 = none, 2 = an error. An error with NO output is a
            // batch rg could not search at all (a pattern it refused, files it could not open):
            // reading it as "no file matched" would hide every match of the batch, so the caller
            // scans in-process instead. An error WITH output is a batch where some files failed
            // (one deleted since the listing): the files it printed are real matches, and the
            // in-process scan could not open the failed ones either.
            if (exitCode == 2 && out.empty())
            {
                return false;
            }
            for (auto& line : CaptureLines(out))
            {
                matched.push_back(std::move(line));
            }
        }
        return true;
    }

    // Stream a (large) text file line by line with bounded memory — the history.jsonl
    // fallback scanner. cb gets each complete line as wide text.
    void ForEachFileLine(const std::wstring& path, const std::function<void(std::wstring_view)>& cb)
    {
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        std::string carry;
        char buf[256 << 10];
        for (;;)
        {
            DWORD got = 0;
            if (!::ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0)
            {
                break;
            }
            carry.append(buf, got);
            size_t start = 0;
            for (size_t i = 0; i < carry.size(); ++i)
            {
                if (carry[i] != '\n')
                {
                    continue;
                }
                std::string_view line(carry.data() + start, i - start);
                while (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                if (!line.empty())
                {
                    const std::wstring wide = Utf8ToWide(line.data(), line.size());
                    cb(wide);
                }
                start = i + 1;
            }
            carry.erase(0, start);
            if (carry.size() > (8u << 20))
            {
                carry.clear(); // corrupt unterminated run — same guard as the transcript scan
            }
        }
        if (!carry.empty())
        {
            const std::wstring wide = Utf8ToWide(carry.data(), carry.size());
            cb(wide);
        }
        ::CloseHandle(h);
    }

    // Single-line-collapse + trim for snippet display.
    std::wstring CollapseWs(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        bool ws = false;
        for (const wchar_t c : s)
        {
            if (c == L'\n' || c == L'\r' || c == L'\t' || c == L' ')
            {
                ws = !out.empty();
                continue;
            }
            if (ws)
            {
                out.push_back(L' ');
                ws = false;
            }
            out.push_back(c);
        }
        return out;
    }

    // The directory part of a path ("" when no separator) and the leaf — the 📁/📄 haystacks.
    std::wstring_view DirPart(std::wstring_view p)
    {
        const size_t cut = p.find_last_of(L"\\/");
        return cut == std::wstring_view::npos ? std::wstring_view{} : p.substr(0, cut);
    }
    std::wstring_view LeafPart(std::wstring_view p)
    {
        const size_t cut = p.find_last_of(L"\\/");
        return cut == std::wstring_view::npos ? p : p.substr(cut + 1);
    }

    // ===== grep (GrepTranscripts) ==============================================================

    // ripgrep is worth a spawn only for a literal this long (non-space characters): a shorter one
    // is in most transcripts, so the filter costs a process per batch and drops almost nothing.
    constexpr size_t kGrepMinRgLiteral = 3;
    // A ripgrep batch: this many files, or this many bytes of transcript, whichever comes first.
    // Small enough that a batch's files are read soon after it is filtered (a grep that runs out
    // of time has read the newest sessions), large enough that a spawn is not paid per file.
    constexpr size_t kGrepBatchFiles = 64;
    constexpr int64_t kGrepBatchBytes = 256LL << 20;
    // A line longer than this has no newline in sight: skipped, like the content scan's guard.
    constexpr size_t kGrepMaxLineBytes = 8u << 20;

    // True when text[i] is a character that JSON writes verbatim inside a string (so a transcript's
    // raw line holds it as it is): not a quote, not a backslash, not a control character, and not
    // half of a surrogate pair that misses its other half (JSON.stringify writes those as \uXXXX).
    //   width — receives how many UTF-16 units the character takes (2 for a surrogate pair).
    bool JsonVerbatimAt(std::wstring_view text, size_t i, size_t& width) noexcept
    {
        width = 1;
        const wchar_t c = text[i];
        if (c < 0x20 || c == L'"' || c == L'\\')
        {
            return false;
        }
        if (c >= 0xD800 && c <= 0xDBFF)
        {
            if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
            {
                width = 2;
                return true;
            }
            return false; // a high surrogate with no low one after it
        }
        return c < 0xDC00 || c > 0xDFFF; // false: a low surrogate on its own
    }

    // The longest run of `text` that JSON writes verbatim (JsonVerbatimAt), or empty.
    //   throws — std::bad_alloc.
    std::wstring LongestJsonVerbatimRun(std::wstring_view text)
    {
        size_t bestAt = 0;
        size_t bestLen = 0;
        size_t runAt = 0;
        size_t i = 0;
        while (i < text.size())
        {
            size_t width = 1;
            const bool verbatim = JsonVerbatimAt(text, i, width);
            i += width;
            if (!verbatim)
            {
                runAt = i; // the run starts again after the escaped character
                continue;
            }
            if (i - runAt > bestLen)
            {
                bestAt = runAt;
                bestLen = i - runAt;
            }
        }
        return std::wstring{ text.substr(bestAt, bestLen) };
    }

    // The line reader's key: the longest printable-ASCII run of `literal`, lower-cased, as bytes.
    // The reader compares raw UTF-8 bytes with ASCII case folding, which only ASCII letters
    // survive (a non-ASCII byte never equals an ASCII one, so a multi-byte character can never
    // produce a false hit). Empty when the run is only spaces — it would select every line.
    //   throws — std::bad_alloc.
    std::string AsciiLowerNeedle(std::wstring_view literal)
    {
        size_t bestAt = 0;
        size_t bestLen = 0;
        size_t runAt = 0;
        for (size_t i = 0; i <= literal.size(); ++i)
        {
            const bool ascii = i < literal.size() && literal[i] >= 0x20 && literal[i] < 0x7F;
            if (ascii)
            {
                continue;
            }
            if (i - runAt > bestLen)
            {
                bestAt = runAt;
                bestLen = i - runAt;
            }
            runAt = i + 1;
        }
        std::string out;
        out.reserve(bestLen);
        bool onlySpaces = true;
        for (size_t i = bestAt; i < bestAt + bestLen; ++i)
        {
            const wchar_t c = literal[i];
            onlySpaces = onlySpaces && c == L' ';
            out.push_back(static_cast<char>(c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c));
        }
        return onlySpaces ? std::string{} : out;
    }

    // The number of non-space characters of `text` (how selective a literal is).
    size_t NonSpaceCount(std::wstring_view text) noexcept
    {
        size_t n = 0;
        for (const wchar_t c : text)
        {
            n += (c != L' ' && c != L'\t') ? 1 : 0;
        }
        return n;
    }

    // Skip the character class that starts at p[at] == '[': the index of its closing ']', or npos
    // when it does not close or is EMPTY (`[]`, `[^]` — ECMAScript and POSIX read them
    // differently, so the literal reader never depends on which reading std::regex took). POSIX
    // bracket expressions inside it ([:alpha:], [.x.], [=e=]) are skipped whole, so their ']' does
    // not end the class early.
    size_t SkipRegexClass(std::wstring_view p, size_t at) noexcept
    {
        size_t j = at + 1;
        if (j < p.size() && p[j] == L'^')
        {
            ++j;
        }
        if (j < p.size() && p[j] == L']')
        {
            return std::wstring_view::npos;
        }
        for (; j < p.size(); ++j)
        {
            const wchar_t c = p[j];
            if (c == L'\\')
            {
                ++j; // the escaped character; the loop steps past it
                continue;
            }
            if (c == L'[' && j + 1 < p.size() && (p[j + 1] == L':' || p[j + 1] == L'.' || p[j + 1] == L'='))
            {
                const wchar_t closer[3] = { p[j + 1], L']', L'\0' };
                const size_t close = p.find(closer, j + 2);
                if (close == std::wstring_view::npos)
                {
                    return std::wstring_view::npos;
                }
                j = close + 1; // the ']' of ":]"; the loop steps past it
                continue;
            }
            if (c == L']')
            {
                return j;
            }
        }
        return std::wstring_view::npos;
    }

    // Read the quantifier at p[at], if there is one: `*`, `+`, `?`, `{n}`, `{n,}` or `{n,m}`, each
    // maybe followed by the lazy `?`.
    //   next     — receives the index after it.
    //   minCount — receives its minimum repeat count (0 = the atom before it may be absent).
    //   ok       — receives false for a `{` this reader does not understand as a quantifier.
    //   returns  — false when p[at] starts no quantifier (then `next` / `minCount` are untouched).
    bool ReadRegexQuantifier(std::wstring_view p, size_t at, size_t& next, size_t& minCount, bool& ok) noexcept
    {
        ok = true;
        if (at >= p.size())
        {
            return false;
        }
        const wchar_t c = p[at];
        if (c == L'*' || c == L'?')
        {
            minCount = 0;
            next = at + 1;
        }
        else if (c == L'+')
        {
            minCount = 1;
            next = at + 1;
        }
        else if (c == L'{')
        {
            size_t j = at + 1;
            size_t n = 0;
            bool digits = false;
            while (j < p.size() && p[j] >= L'0' && p[j] <= L'9')
            {
                n = (std::min)(n * 10 + static_cast<size_t>(p[j] - L'0'), size_t{ 1000000 });
                digits = true;
                ++j;
            }
            if (j < p.size() && p[j] == L',')
            {
                ++j;
                while (j < p.size() && p[j] >= L'0' && p[j] <= L'9')
                {
                    ++j;
                }
            }
            if (!digits || j >= p.size() || p[j] != L'}')
            {
                ok = false;
                return true;
            }
            minCount = n;
            next = j + 1;
        }
        else
        {
            return false;
        }
        if (next < p.size() && p[next] == L'?')
        {
            ++next; // lazy
        }
        return true;
    }

    // The runs of plain characters at the TOP level of the ECMAScript pattern `p`, each of which
    // every match contains (GrepRequiredLiteral). A group (capturing, `(?:`, a lookaround), a class,
    // `.`, `^`, `$` or a class escape (`\d`, `\b`, ...) ends a run; a quantifier that allows zero
    // ends it BEFORE its character; one that needs at least one keeps the character but ends the
    // run after it (what follows may be another copy).
    //   runs    — receives the runs, in pattern order (possibly none).
    //   returns — false when no run can be relied on: a top-level `|` (an alternative needs none
    //             of them), or a construct this reader does not follow — it then reports nothing
    //             rather than guess, and the grep reads every candidate.
    //   throws  — std::bad_alloc.
    bool RegexLiteralRuns(std::wstring_view p, std::vector<std::wstring>& runs)
    {
        std::wstring cur;
        const auto flush = [&] {
            if (!cur.empty())
            {
                runs.push_back(std::move(cur));
                cur.clear();
            }
        };
        size_t i = 0;
        while (i < p.size())
        {
            const wchar_t c = p[i];
            bool literal = false;
            wchar_t ch = 0;
            size_t next = i + 1;
            switch (c)
            {
            case L'|':
                return false;
            case L'(':
            {
                // Skipped whole, with any `|` inside it: the group's text is not reliably in a match.
                size_t depth = 0;
                size_t j = i;
                for (; j < p.size(); ++j)
                {
                    if (p[j] == L'\\')
                    {
                        ++j;
                        continue;
                    }
                    if (p[j] == L'[')
                    {
                        j = SkipRegexClass(p, j);
                        if (j == std::wstring_view::npos)
                        {
                            return false;
                        }
                        continue;
                    }
                    if (p[j] == L'(')
                    {
                        ++depth;
                    }
                    else if (p[j] == L')' && --depth == 0)
                    {
                        break;
                    }
                }
                if (j >= p.size())
                {
                    return false; // unbalanced
                }
                next = j + 1;
                break;
            }
            case L')':
                return false;
            case L'[':
            {
                const size_t j = SkipRegexClass(p, i);
                if (j == std::wstring_view::npos)
                {
                    return false;
                }
                next = j + 1;
                break;
            }
            case L'.':
            case L'^':
            case L'$':
                break; // varying text, or none
            case L'\\':
            {
                if (i + 1 >= p.size())
                {
                    return false;
                }
                const wchar_t e = p[i + 1];
                next = i + 2;
                const bool alnum = (e >= L'0' && e <= L'9') || (e >= L'a' && e <= L'z') || (e >= L'A' && e <= L'Z');
                if (!alnum)
                {
                    literal = true; // `\.`, `\(`, `\\`, `\/`: the character itself
                    ch = e;
                }
                else if (e == L'c')
                {
                    next = i + 3; // \cX: a control character
                }
                else if (e == L'x')
                {
                    next = i + 4; // \xHH
                }
                else if (e == L'u')
                {
                    next = i + 6; // \uHHHH
                }
                else if (e >= L'0' && e <= L'9')
                {
                    while (next < p.size() && p[next] >= L'0' && p[next] <= L'9')
                    {
                        ++next; // \0, or a backreference: the text it repeats varies
                    }
                }
                // \d \w \s \b \B \D \W \S \n \t \r \f \v: a class, an assertion or a control
                // character — none of them is a plain character a raw line holds verbatim.
                next = (std::min)(next, p.size());
                break;
            }
            case L'*':
            case L'+':
            case L'?':
            case L'{':
                return false; // a quantifier where an atom belongs: not a pattern this reader follows
            default:
                literal = true;
                ch = c;
                break;
            }
            size_t minCount = 1;
            size_t afterQuantifier = next;
            bool quantifierOk = true;
            const bool quantified = ReadRegexQuantifier(p, next, afterQuantifier, minCount, quantifierOk);
            if (!quantifierOk)
            {
                return false;
            }
            if (quantified)
            {
                next = afterQuantifier;
            }
            if (!literal || (quantified && minCount == 0))
            {
                flush(); // not plain text, or a character that may be absent
            }
            else
            {
                cur.push_back(ch);
                if (quantified)
                {
                    flush(); // present at least once, but the next character may be another copy of it
                }
            }
            i = next;
        }
        flush();
        return true;
    }

    // The index of the ')' that closes the group opening at p[at] == '(' (escapes and classes
    // inside it skipped), or npos when it does not close.
    size_t MatchingRegexParen(std::wstring_view p, size_t at) noexcept
    {
        size_t depth = 0;
        for (size_t j = at; j < p.size(); ++j)
        {
            if (p[j] == L'\\')
            {
                ++j;
                continue;
            }
            if (p[j] == L'[')
            {
                j = SkipRegexClass(p, j);
                if (j == std::wstring_view::npos)
                {
                    return std::wstring_view::npos;
                }
                continue;
            }
            if (p[j] == L'(')
            {
                ++depth;
            }
            else if (p[j] == L')' && --depth == 0)
            {
                return j;
            }
        }
        return std::wstring_view::npos;
    }

    // `p` split at its TOP-LEVEL `|` (not inside a group or a class, not escaped).
    //   parts   — receives the alternatives (one part when there is no top-level `|`).
    //   returns — false for a pattern this reader cannot follow (an unbalanced group or class).
    //   throws  — std::bad_alloc.
    bool SplitRegexAlternatives(std::wstring_view p, std::vector<std::wstring_view>& parts)
    {
        size_t start = 0;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const wchar_t c = p[i];
            if (c == L'\\')
            {
                ++i;
                continue;
            }
            if (c == L'[')
            {
                i = SkipRegexClass(p, i);
                if (i == std::wstring_view::npos)
                {
                    return false;
                }
                continue;
            }
            if (c == L'(')
            {
                i = MatchingRegexParen(p, i);
                if (i == std::wstring_view::npos)
                {
                    return false;
                }
                continue;
            }
            if (c == L'|')
            {
                parts.push_back(p.substr(start, i - start));
                start = i + 1;
            }
        }
        parts.push_back(p.substr(start));
        return true;
    }

    // The shortest piece of `pieces`, in non-space characters (how selective an any-of set is: a
    // line passes when it holds any piece, so the set is as weak as its weakest member).
    size_t WeakestPiece(const std::vector<std::wstring>& pieces) noexcept
    {
        size_t weakest = SIZE_MAX;
        for (const auto& piece : pieces)
        {
            weakest = (std::min)(weakest, NonSpaceCount(piece));
        }
        return pieces.empty() ? 0 : weakest;
    }

    // The text of an alternative that is PLAIN text only — characters, and escaped characters that
    // stand for themselves (`\.`, `\(`, `\\`) — with the escapes undone. False for anything else
    // (a quantifier, a group, a class, `.`, an anchor, a class escape such as `\d`).
    //   out — receives the text when it returns true.
    //   throws — std::bad_alloc.
    bool PlainAlternativeText(std::wstring_view alt, std::wstring& out)
    {
        out.clear();
        for (size_t i = 0; i < alt.size(); ++i)
        {
            const wchar_t c = alt[i];
            if (c == L'\\')
            {
                if (i + 1 >= alt.size())
                {
                    return false;
                }
                const wchar_t e = alt[i + 1];
                const bool alnum = (e >= L'0' && e <= L'9') || (e >= L'a' && e <= L'z') || (e >= L'A' && e <= L'Z');
                if (alnum)
                {
                    return false; // \d \w \s \b ...: not a character of its own
                }
                out.push_back(e);
                ++i;
                continue;
            }
            if (c == L'(' || c == L')' || c == L'[' || c == L']' || c == L'{' || c == L'}' || c == L'*' || c == L'+' || c == L'?' || c == L'.' || c == L'^' ||
                c == L'$' || c == L'|')
            {
                return false;
            }
            out.push_back(c);
        }
        return true;
    }

    std::vector<std::wstring> RegexAnyOfLiterals(std::wstring_view p, int depth);

    // The any-of keys of ONE alternative (a pattern with no top-level `|`), found by walking its
    // atoms left to right. A SEGMENT is text every match holds in one piece; plain characters
    // extend it, a required group of PLAIN alternatives multiplies it ("qm (create|start)" gives
    // "qm create" and "qm start" — far more selective than "qm " or the common words alone), and
    // anything that varies (a class, `.`, a class escape, an optional or repeated atom) ends it.
    // A required group of other alternatives is searched on its own (RegexAnyOfLiterals). Of all
    // the segments and groups, the set whose shortest member is longest wins (fewer members on a
    // tie). Zero-width atoms (`\b`, `\B`, `^`, `$`) neither extend nor end a segment.
    //   returns — the best set, or none (nothing in the alternative is held verbatim by every match).
    //   throws  — std::bad_alloc.
    std::vector<std::wstring> SequenceAnyOfLiterals(std::wstring_view p, int depth)
    {
        constexpr size_t kMaxProduct = 32; // a segment multiplied past this many members is closed and restarted
        std::vector<std::wstring> best;
        std::vector<std::wstring> cur{ std::wstring{} };
        const auto consider = [&best](std::vector<std::wstring> set) {
            if (set.empty())
            {
                return;
            }
            const size_t w = WeakestPiece(set);
            const size_t bw = WeakestPiece(best);
            if (w == 0)
            {
                return; // a member with no text holds nothing
            }
            if (best.empty() || w > bw || (w == bw && set.size() < best.size()))
            {
                best = std::move(set);
            }
        };
        // Close the current segment: each member cut to its longest JSON-verbatim run (a raw line holds that much of it
        // as it is; a cut member is still in every match it was in), then offered as a set.
        const auto flush = [&]() {
            std::vector<std::wstring> set;
            bool usable = true;
            for (const auto& member : cur)
            {
                std::wstring piece = LongestJsonVerbatimRun(member);
                if (piece.empty())
                {
                    usable = false; // one member has no verbatim text: the set cannot be required
                    break;
                }
                if (std::find(set.begin(), set.end(), piece) == set.end())
                {
                    set.push_back(std::move(piece));
                }
            }
            if (usable)
            {
                consider(std::move(set));
            }
            cur.assign(1, std::wstring{});
        };
        const auto appendAll = [&cur](wchar_t ch) {
            for (auto& member : cur)
            {
                member.push_back(ch);
            }
        };
        size_t i = 0;
        while (i < p.size())
        {
            const wchar_t c = p[i];
            if (c == L'(')
            {
                const size_t close = MatchingRegexParen(p, i);
                if (close == std::wstring_view::npos)
                {
                    return {};
                }
                size_t bodyAt = i + 1;
                bool lookaround = false;
                if (bodyAt < p.size() && p[bodyAt] == L'?')
                {
                    if (bodyAt + 1 < p.size() && p[bodyAt + 1] == L':')
                    {
                        bodyAt += 2; // a non-capturing group
                    }
                    else
                    {
                        lookaround = true; // (?= (?! (?<= (?<!: consumes nothing
                    }
                }
                size_t next = close + 1;
                size_t minCount = 1;
                bool quantifierOk = true;
                const bool quantified = ReadRegexQuantifier(p, close + 1, next, minCount, quantifierOk);
                if (!quantifierOk)
                {
                    return {};
                }
                const std::wstring_view body = p.substr(bodyAt, close > bodyAt ? close - bodyAt : 0);
                std::vector<std::wstring_view> alts;
                std::vector<std::wstring> plain;
                bool allPlain = !lookaround && SplitRegexAlternatives(body, alts);
                for (const auto alt : alts)
                {
                    std::wstring text;
                    if (!allPlain || !PlainAlternativeText(alt, text))
                    {
                        allPlain = false;
                        break;
                    }
                    plain.push_back(std::move(text));
                }
                if (allPlain && !quantified && !plain.empty())
                {
                    // Exactly one of the alternatives, right here: the segment goes on as the product.
                    if (cur.size() * plain.size() > kMaxProduct)
                    {
                        flush();
                    }
                    std::vector<std::wstring> product;
                    product.reserve(cur.size() * plain.size());
                    for (const auto& head : cur)
                    {
                        for (const auto& tail : plain)
                        {
                            product.push_back(head + tail);
                        }
                    }
                    cur = std::move(product);
                }
                else
                {
                    flush();
                    if (!lookaround && minCount >= 1)
                    {
                        // A group every match passes through, of other shapes: its own keys are a candidate set.
                        consider(RegexAnyOfLiterals(body, depth + 1));
                    }
                }
                i = next;
                continue;
            }
            if (c == L'[')
            {
                const size_t j = SkipRegexClass(p, i);
                if (j == std::wstring_view::npos)
                {
                    return {};
                }
                flush(); // varying text
                size_t next = j + 1;
                size_t minCount = 1;
                bool quantifierOk = true;
                ReadRegexQuantifier(p, j + 1, next, minCount, quantifierOk);
                if (!quantifierOk)
                {
                    return {};
                }
                i = next;
                continue;
            }
            if (c == L'^' || c == L'$')
            {
                ++i; // zero width: the text on both sides is still one piece
                continue;
            }
            if (c == L')' || c == L'|' || c == L'*' || c == L'+' || c == L'?' || c == L'{')
            {
                return {}; // unbalanced, or a quantifier where an atom belongs: not a pattern this reader follows
            }
            bool literal = true;
            wchar_t ch = c;
            size_t next = i + 1;
            if (c == L'.')
            {
                literal = false;
            }
            else if (c == L'\\')
            {
                if (i + 1 >= p.size())
                {
                    return {};
                }
                const wchar_t e = p[i + 1];
                next = i + 2;
                const bool alnum = (e >= L'0' && e <= L'9') || (e >= L'a' && e <= L'z') || (e >= L'A' && e <= L'Z');
                if (e == L'b' || e == L'B')
                {
                    i = next; // a word boundary: zero width
                    continue;
                }
                if (!alnum)
                {
                    ch = e; // `\.`, `\(`, `\\`: the character itself
                }
                else
                {
                    literal = false; // \d \w \s, \xHH, \uHHHH, a backreference, \cX: varying, or a character a raw line escapes
                    if (e == L'c')
                    {
                        next = i + 3;
                    }
                    else if (e == L'x')
                    {
                        next = i + 4;
                    }
                    else if (e == L'u')
                    {
                        next = i + 6;
                    }
                    else if (e >= L'0' && e <= L'9')
                    {
                        while (next < p.size() && p[next] >= L'0' && p[next] <= L'9')
                        {
                            ++next;
                        }
                    }
                    next = (std::min)(next, p.size());
                }
            }
            size_t minCount = 1;
            size_t afterQuantifier = next;
            bool quantifierOk = true;
            const bool quantified = ReadRegexQuantifier(p, next, afterQuantifier, minCount, quantifierOk);
            if (!quantifierOk)
            {
                return {};
            }
            if (!literal || (quantified && minCount == 0))
            {
                flush(); // varying, or a character that may be absent
            }
            else
            {
                appendAll(ch);
                if (quantified)
                {
                    flush(); // present at least once, but the next character may be another copy of it
                }
            }
            i = quantified ? afterQuantifier : next;
        }
        flush();
        return best;
    }

    // GrepRequiredLiterals' recursion over an ECMAScript pattern (see the header for the rule): a
    // top-level alternation is the union of its alternatives' own keys (an alternative with none
    // makes the whole set useless); one alternative is walked by SequenceAnyOfLiterals.
    //   depth — the nesting reached; past 3 the reader gives up (an empty answer).
    //   throws — std::bad_alloc.
    std::vector<std::wstring> RegexAnyOfLiterals(std::wstring_view p, int depth)
    {
        if (depth > 3)
        {
            return {};
        }
        std::vector<std::wstring_view> alternatives;
        if (!SplitRegexAlternatives(p, alternatives))
        {
            return {};
        }
        if (alternatives.size() == 1)
        {
            return SequenceAnyOfLiterals(p, depth);
        }
        std::vector<std::wstring> any;
        for (const auto alt : alternatives)
        {
            std::vector<std::wstring> keys = RegexAnyOfLiterals(alt, depth + 1);
            if (keys.empty())
            {
                return {};
            }
            for (auto& key : keys)
            {
                if (std::find(any.begin(), any.end(), key) == any.end())
                {
                    any.push_back(std::move(key));
                }
            }
        }
        return any;
    }

    // ASCII case folding for the line reader's byte compare.
    constexpr char FoldAsciiByte(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    struct FoldedByteHash
    {
        size_t operator()(char c) const noexcept
        {
            return std::hash<char>{}(FoldAsciiByte(c));
        }
    };
    struct FoldedByteEqual
    {
        bool operator()(char a, char b) const noexcept
        {
            return FoldAsciiByte(a) == FoldAsciiByte(b);
        }
    };

    // Closes a Win32 handle when it goes out of scope (an exception from a line callback must not
    // leak the transcript's handle).
    struct HandleCloser
    {
        HANDLE h;
        ~HandleCloser()
        {
            ::CloseHandle(h);
        }
    };

    // Calls `onLine` with every complete line of `path` as raw bytes (a trailing "\r" dropped,
    // empty lines skipped), reading a megabyte at a time. A trailing line with no newline yet (an
    // append in flight) is left out, like the content scan does; a line past kGrepMaxLineBytes is
    // skipped.
    //   stop    — asked before each read: true ends the walk.
    //   onLine  — returns false to end the walk at once (the per-line stop: one megabyte can hold
    //             hundreds of messages, and a pathological regex takes seconds on each).
    //   returns — 0 = read to its end; 1 = ended by `stop` or `onLine`; -1 = the file could not be
    //             opened.
    //   throws  — whatever `onLine` throws; std::bad_alloc.
    int ForEachTranscriptLineBytes(const std::wstring& path, const std::function<bool()>& stop, const std::function<bool(std::string_view)>& onLine)
    {
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return -1;
        }
        const HandleCloser closer{ h };
        std::string carry;
        std::string chunk(1u << 20, '\0');
        for (;;)
        {
            if (stop && stop())
            {
                return 1;
            }
            DWORD got = 0;
            if (!::ReadFile(h, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0)
            {
                return 0;
            }
            carry.append(chunk.data(), got);
            size_t start = 0;
            for (;;)
            {
                const size_t nl = carry.find('\n', start);
                if (nl == std::string::npos)
                {
                    break;
                }
                std::string_view line(carry.data() + start, nl - start);
                while (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                if (!line.empty() && !onLine(line))
                {
                    return 1;
                }
                start = nl + 1;
            }
            carry.erase(0, start);
            if (carry.size() > kGrepMaxLineBytes)
            {
                carry.clear(); // one unterminated run past the guard: skipped (its tail parses as nothing)
            }
        }
    }

    // One grep's fixed parts, shared by its per-file reads — and, since 2026-10-18, by its reader
    // THREADS: everything here is read-only while the files are read (a const std::wregex and the
    // searchers' const operator() are safe to share), except tooComplex, which is atomic. Not
    // copyable: each searcher points into its needle.
    struct GrepRun
    {
        using Searcher = std::boyer_moore_horspool_searcher<std::string::const_iterator, FoldedByteHash, FoldedByteEqual>;

        const GrepOptions* options{};
        const std::wregex* re{}; // the compiled regex, or null for a literal pattern
        // A literal pattern's forms, folded like FoldLower folds the text: the pattern as written,
        // and its unescaped spelling when that differs (GrepUnescapePlainPattern). A text matches
        // when it holds either.
        std::vector<std::wstring> patternLowers;
        // AsciiLowerNeedle of each key of GrepRequiredLiterals: a line is parsed only when it
        // holds ONE of them (any-of). Empty = parse every line.
        std::vector<std::string> needles;
        std::vector<Searcher> searchers; // one per needle, pointing into it
        std::atomic<int> tooComplex{ 0 };

        GrepRun() = default;
        GrepRun(const GrepRun&) = delete;
        GrepRun& operator=(const GrepRun&) = delete;
    };

    // Where the grep's pattern first matches `text`.
    //   pos, len — receive the match's place when it returns true.
    //   returns  — true on a match. A regex that gives up on this text (std::regex_error:
    //              error_complexity / error_stack) reads as no match and is counted in
    //              run.tooComplex, which the outcome reports.
    //   throws   — std::bad_alloc.
    bool FindGrepMatch(GrepRun& run, const std::wstring& text, size_t& pos, size_t& len)
    {
        if (run.re)
        {
            try
            {
                std::wsmatch m;
                if (!std::regex_search(text, m, *run.re))
                {
                    return false;
                }
                pos = static_cast<size_t>(m.position(0));
                len = static_cast<size_t>(m.length(0));
                return true;
            }
            catch (const std::regex_error&)
            {
                // Expected control flow (the Rule #18 exemption RegexUtil.h documents): MSVC's
                // regex throws error_complexity / error_stack on a pathological pattern. Not
                // silent: the count reaches the model in the grep's result ("too_complex").
                ++run.tooComplex;
                return false;
            }
        }
        const std::wstring lowered = FoldLower(text); // FoldLower maps one unit to one, so positions carry over
        for (const auto& form : run.patternLowers)
        {
            const size_t at = lowered.find(form);
            if (at != std::wstring::npos)
            {
                pos = at;
                len = form.size();
                return true;
            }
        }
        return false;
    }

    // True when `line` (a raw transcript line) holds one of the run's needles OUTSIDE its `cwd`
    // field. Every line of a session launched in a folder carries that folder's path as "cwd", so a
    // folder name used as a grep word ("proxmox") would otherwise send EVERY line of every such
    // session through the decode and the JSON parse — the reason a grep for a project's name ran
    // out of time (SESSIONS.md §9.15). The cwd value never holds message text, so skipping a line
    // whose only copies sit in it loses no match. True for a run without needles (parse it).
    bool LineHoldsNeedleOutsideCwd(const GrepRun& run, std::string_view line)
    {
        if (run.searchers.empty())
        {
            return true;
        }
        static constexpr std::string_view kCwdKey = "\"cwd\":\"";
        for (size_t k = 0; k < run.searchers.size(); ++k)
        {
            auto from = line.begin();
            for (;;)
            {
                const auto hit = std::search(from, line.end(), run.searchers[k]);
                if (hit == line.end())
                {
                    break;
                }
                const size_t pos = static_cast<size_t>(hit - line.begin());
                // Inside the cwd value when the nearest `"cwd":"` before the hit has no closing quote
                // between it and the hit. A quote inside a JSON string is escaped (\"), and an escape
                // is told from a path's own trailing backslash ("K:\\" ends in an escaped backslash)
                // by the parity of the backslashes before it: an odd run escapes the quote.
                const size_t key = line.substr(0, pos).rfind(kCwdKey);
                bool insideCwd = false;
                if (key != std::string_view::npos)
                {
                    insideCwd = true;
                    for (size_t j = key + kCwdKey.size(); j < pos; ++j)
                    {
                        if (line[j] != '"')
                        {
                            continue;
                        }
                        size_t slashes = 0;
                        for (size_t b = j; b > key + kCwdKey.size() && line[b - 1] == '\\'; --b)
                        {
                            ++slashes;
                        }
                        if (slashes % 2 == 0)
                        {
                            insideCwd = false; // the value closed before the hit
                            break;
                        }
                    }
                }
                if (!insideCwd)
                {
                    return true;
                }
                from = hit + 1;
            }
        }
        return false;
    }

    // The files a line's tool calls CHANGED: (tool name, path) for every Edit / Write / MultiEdit /
    // NotebookEdit call of an assistant line (its file_path, or a notebook's notebook_path). Empty
    // for any other line, or one that does not parse. The `in:"edits"` mode's haystack.
    //   throws — std::bad_alloc.
    std::vector<std::pair<std::wstring, std::wstring>> EditedPathsOfLine(std::string_view line)
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        // Cheap gates before the parse: an assistant line with a tool call.
        if (line.find("\"tool_use\"") == std::string_view::npos || line.find("\"assistant\"") == std::string_view::npos)
        {
            return out;
        }
        const auto obj = json::Parse(Utf8ToWide(line.data(), line.size()));
        if (!obj || obj->type != json::Value::Type::Obj || obj->StrAt(L"type") != L"assistant")
        {
            return out;
        }
        const auto* msg = obj->Find(L"message");
        const auto* content = (msg && msg->type == json::Value::Type::Obj) ? msg->Find(L"content") : nullptr;
        if (!content || content->type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const auto& blk : content->arr)
        {
            if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_use")
            {
                continue;
            }
            const std::wstring name = blk.StrAt(L"name");
            if (name != L"Edit" && name != L"Write" && name != L"MultiEdit" && name != L"NotebookEdit")
            {
                continue;
            }
            const auto* in = blk.Find(L"input");
            if (!in || in->type != json::Value::Type::Obj)
            {
                continue;
            }
            std::wstring path = in->StrAt(L"file_path");
            if (path.empty())
            {
                path = in->StrAt(L"notebook_path");
            }
            if (!path.empty())
            {
                out.emplace_back(name, std::move(path));
            }
        }
        return out;
    }

    // Grep one transcript into `hit`.
    //   stop    — the grep's stop check (cancel / budget), asked every megabyte AND before every
    //             line that is parsed: MSVC's regex gives up on a catastrophic pattern only after
    //             about 4 s per message in a debug build (measured; ~3 s when it does not give up),
    //             so a once-a-megabyte check could overrun the budget by minutes.
    //   returns — ForEachTranscriptLineBytes' code: 0 read to its end, 1 stopped, -1 not opened.
    //   throws  — std::bad_alloc.
    int GrepOneTranscript(const TranscriptRef& ref, GrepRun& run, const std::function<bool()>& stop, GrepSessionHit& hit)
    {
        const GrepOptions& o = *run.options;
        const auto record = [&](bool user, int64_t timestampMs, const std::wstring& text) {
            size_t pos = 0;
            size_t len = 0;
            if (!FindGrepMatch(run, text, pos, len))
            {
                return;
            }
            ++hit.hits;
            hit.userHits += user ? 1 : 0;
            if (hit.excerpts.size() < o.maxExcerpts)
            {
                hit.excerpts.push_back(GrepExcerpt{ user, timestampMs, MakeGrepExcerpt(text, pos, len, o.contextChars) });
            }
        };
        return ForEachTranscriptLineBytes(ref.path, stop, [&](std::string_view line) -> bool {
            // The byte prefilter: a line without any of the required literals (outside its cwd
            // field) cannot hold a match, so it is never decoded or parsed (the JSON parse is what
            // a grep spends its time on).
            if (!LineHoldsNeedleOutsideCwd(run, line))
            {
                return true;
            }
            if (stop && stop())
            {
                return false; // out of time, or cancelled: before the costly part of this line
            }
            if (o.editsOnly)
            {
                // `in:"edits"`: the pattern against the paths this line's Edit / Write / MultiEdit /
                // NotebookEdit calls changed — "<tool> <path>" is the excerpt.
                for (const auto& [tool, path] : EditedPathsOfLine(line))
                {
                    size_t pos = 0;
                    size_t len = 0;
                    if (!FindGrepMatch(run, path, pos, len))
                    {
                        continue;
                    }
                    ++hit.hits;
                    if (hit.excerpts.size() < o.maxExcerpts)
                    {
                        int64_t ts = 0;
                        if (const auto at = line.find("\"timestamp\":\""); at != std::string_view::npos)
                        {
                            // The line's own time, for the excerpt's "at" (a decode of the whole line is
                            // not worth it for one field).
                            const size_t from = at + 13;
                            const size_t to = line.find('"', from);
                            if (to != std::string_view::npos)
                            {
                                ts = ParseTranscriptTimestamp(Utf8ToWide(line.data() + from, to - from));
                            }
                        }
                        hit.excerpts.push_back(GrepExcerpt{ false, ts, tool + L" " + path });
                    }
                    break; // one call per line counts once, like a message
                }
                return true;
            }
            const std::wstring wide = Utf8ToWide(line.data(), line.size());
            const TranscriptLineFacts f = ClassifyTranscriptLine(wide, o.user ? kGrepTextBudgetChars : 0, o.agent ? kGrepTextBudgetChars : 0);
            if (o.user && !f.userText.empty())
            {
                record(true, f.timestampMs, f.userText);
            }
            if (o.agent && !f.agentText.empty())
            {
                record(false, f.timestampMs, f.agentText);
            }
            return true;
        });
    }
}

namespace Agentmaster
{
    std::wstring FoldLower(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        for (const wchar_t c : s)
        {
            out.push_back(static_cast<wchar_t>(::towlower(c)));
        }
        return out;
    }

    bool IsGuidToken(std::wstring_view token)
    {
        if (token.size() >= 2 && token.front() == L'{' && token.back() == L'}')
        {
            token = token.substr(1, token.size() - 2);
        }
        if (token.size() != 36)
        {
            return false;
        }
        for (size_t i = 0; i < token.size(); ++i)
        {
            const wchar_t c = token[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != L'-')
                {
                    return false;
                }
                continue;
            }
            const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
            if (!hex)
            {
                return false;
            }
        }
        return true;
    }

    std::vector<SearchTerm> ParseSessionQuery(std::wstring_view text)
    {
        std::vector<SearchTerm> out;
        const auto isWs = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        size_t i = 0;
        while (i < text.size())
        {
            if (isWs(text[i]))
            {
                ++i;
                continue;
            }
            SearchTerm t;
            if (text[i] == L'"')
            {
                // "quoted phrase" — one EXACT term, spaces kept; unterminated runs to the end.
                const size_t close = text.find(L'"', i + 1);
                const size_t end = (close == std::wstring_view::npos) ? text.size() : close;
                t.text.assign(text.substr(i + 1, end - (i + 1)));
                t.exact = true;
                i = (close == std::wstring_view::npos) ? text.size() : close + 1;
            }
            else
            {
                size_t end = i;
                while (end < text.size() && !isWs(text[end]))
                {
                    ++end;
                }
                std::wstring_view tok = text.substr(i, end - i);
                i = end;
                if (IsGuidToken(tok))
                {
                    if (tok.front() == L'{')
                    {
                        tok = tok.substr(1, tok.size() - 2);
                    }
                    t.isGuid = true;
                }
                t.text.assign(tok);
            }
            if (t.text.empty())
            {
                continue; // "" — an empty term matches everything; drop it
            }
            t.textLower = FoldLower(t.text);
            out.push_back(std::move(t));
        }
        return out;
    }

    std::wstring BuildSearchRegex(std::wstring_view text, bool fuzzy)
    {
        static constexpr std::wstring_view kSpecials = LR"(\.^$|()[]{}*+?-)";
        std::wstring out;
        out.reserve(text.size() * (fuzzy ? 5 : 2));
        bool first = true;
        for (const wchar_t c : text)
        {
            if (fuzzy && (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'))
            {
                continue; // fuzzy ignores whitespace — the gap covers it
            }
            if (fuzzy && !first)
            {
                out += L".*?"; // lazy gap between every query char
            }
            if (kSpecials.find(c) != std::wstring_view::npos)
            {
                out.push_back(L'\\');
            }
            out.push_back(c);
            first = false;
        }
        return out;
    }

    bool MatchesQueryText(std::wstring_view textLower, std::wstring_view queryLower, bool fuzzy)
    {
        if (queryLower.empty())
        {
            return true;
        }
        if (!fuzzy)
        {
            return textLower.find(queryLower) != std::wstring_view::npos;
        }
        // Subsequence: every non-space query char in order (== the `.*?`-joined rg regex).
        size_t t = 0;
        for (const wchar_t qc : queryLower)
        {
            if (qc == L' ' || qc == L'\t' || qc == L'\n' || qc == L'\r')
            {
                continue;
            }
            while (t < textLower.size() && textLower[t] != qc)
            {
                ++t;
            }
            if (t == textLower.size())
            {
                return false;
            }
            ++t;
        }
        return true;
    }

    bool MatchesPathQuery(std::wstring_view textLower, std::wstring_view queryLower, bool fuzzy)
    {
        // A needle with no path separator can never be affected by folding '/'<->'\': the
        // separators surround the match but are never part of it (substring), and a slash in the
        // haystack is skipped either way (subsequence). So skip normalization for the common
        // plain-word case — it stays allocation-free, and the Windows cwd (full of backslashes)
        // is only re-spelled when the query itself carries a separator.
        if (queryLower.find_first_of(L"/\\") == std::wstring_view::npos)
        {
            return MatchesQueryText(textLower, queryLower, fuzzy);
        }
        const auto toForward = [](std::wstring_view s) {
            std::wstring out{ s };
            for (wchar_t& c : out)
            {
                if (c == L'\\')
                {
                    c = L'/';
                }
            }
            return out;
        };
        // The temporaries outlive the call (full-expression lifetime), so the string_views are safe.
        return MatchesQueryText(toForward(textLower), toForward(queryLower), fuzzy);
    }

    std::wstring MakeSnippet(std::wstring_view text, std::wstring_view queryLower, bool fuzzy, size_t maxChars)
    {
        if (maxChars == 0)
        {
            return {};
        }
        size_t at = std::wstring_view::npos;
        if (!fuzzy && !queryLower.empty())
        {
            const std::wstring lowered = FoldLower(text);
            at = lowered.find(queryLower);
        }
        const size_t begin = (at == std::wstring_view::npos) ? 0 : (at > 40 ? at - 40 : 0);
        std::wstring snip = CollapseWs(text.substr(begin, maxChars + 80));
        if (snip.size() > maxChars)
        {
            snip.resize(maxChars);
            snip += L"…";
        }
        if (begin > 0)
        {
            snip.insert(0, L"…");
        }
        return snip;
    }

    std::vector<std::wstring> SearchIndexFast(const std::vector<SessionIndexEntry>& entries, const SessionQuery& q)
    {
        std::vector<std::wstring> out;
        out.reserve(entries.size());
        const auto terms = ParseSessionQuery(q.text);
        if (terms.empty())
        {
            for (const auto& e : entries)
            {
                out.push_back(e.sessionId); // window-only listing (empty / whitespace / "" query)
            }
            return out;
        }
        for (const auto& e : entries)
        {
            const auto& st = e.stats;
            // Fold each haystack ONCE per entry; every term tries them all (AND across terms,
            // OR across fields — neighbor terms may hit different fields). cwd is the ALWAYS-ON
            // directory baseline; the TITLE fields are gated on q.scopeTitle (default ON) and
            // include the runtime liveTitle overlay (an OPEN session's real tab title, so a
            // renamed session is findable by the name the page shows).
            const std::wstring cwdLower = FoldLower(st.cwd);
            const std::wstring titleHay[] = {
                FoldLower(st.customTitle),
                FoldLower(st.aiTitle),
                FoldLower(st.summary),
                FoldLower(st.firstUserPrompt),
                FoldLower(e.liveTitle),
            };
            const std::wstring idLower = FoldLower(e.sessionId);
            const std::wstring forkLower = FoldLower(st.forkedFromId);
            bool all = true;
            for (const auto& t : terms)
            {
                const bool fz = TermIsFuzzy(t, q.fuzzy);
                // A whole-GUID token matches the session IDENTITY outright — its own id, or its
                // fork-parent id (so a parent's guid surfaces the forks too). The text haystacks
                // below still apply (additive).
                bool hit = t.isGuid && (t.textLower == idLower || (!forkLower.empty() && t.textLower == forkLower));
                if (!hit)
                {
                    hit = MatchesPathQuery(cwdLower, t.textLower, fz); // cwd: always a match target, slash-insensitive
                }
                if (!hit && q.scopeTitle)
                {
                    for (const auto& h : titleHay)
                    {
                        if (MatchesQueryText(h, t.textLower, fz))
                        {
                            hit = true;
                            break;
                        }
                    }
                }
                if (!hit && (q.scopeDirs || q.scopeFiles))
                {
                    for (const auto& p : st.pathsAccessed)
                    {
                        const std::wstring lowered = FoldLower(p);
                        if (q.scopeDirs && MatchesPathQuery(DirPart(lowered), t.textLower, fz)) // dirs: slash-insensitive
                        {
                            hit = true;
                            break;
                        }
                        if (q.scopeFiles && MatchesQueryText(LeafPart(lowered), t.textLower, fz)) // a leaf never holds a separator
                        {
                            hit = true;
                            break;
                        }
                    }
                }
                if (!hit)
                {
                    all = false;
                    break;
                }
            }
            if (all)
            {
                out.push_back(e.sessionId);
            }
        }
        return out;
    }

    const std::wstring& ResolveRipgrep()
    {
        static const std::wstring resolved = [] {
            wchar_t buf[MAX_PATH]{};
            const DWORD n = ::SearchPathW(nullptr, L"rg.exe", nullptr, MAX_PATH, buf, nullptr);
            return (n > 0 && n < MAX_PATH) ? std::wstring{ buf, n } : std::wstring{};
        }();
        return resolved;
    }

    std::unordered_map<std::wstring, SessionHit> SearchHistoryPrompts(const std::wstring& historyPath, const SessionQuery& q, size_t maxSnippetsPerSession)
    {
        std::unordered_map<std::wstring, SessionHit> out;
        const auto terms = ParseSessionQuery(q.text);
        const SearchTerm* snip = nullptr; // first text term — anchors the snippets
        const SearchTerm* sel = nullptr; // longest text term — the most selective rg pattern
        for (const auto& t : terms)
        {
            if (t.isGuid)
            {
                continue;
            }
            if (!snip)
            {
                snip = &t;
            }
            if (!sel || t.text.size() > sel->text.size())
            {
                sel = &t;
            }
        }
        if (!snip)
        {
            return out; // empty or guid-only query — identity matching is the fast phase's job
        }
        const auto onLine = [&](std::wstring_view line) {
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                return;
            }
            const std::wstring sid = parsed->StrAt(L"sessionId");
            if (sid.empty())
            {
                return; // ancient pre-sessionId lines — unattributable
            }
            const std::wstring display = parsed->StrAt(L"display");
            if (display.empty())
            {
                return;
            }
            const std::wstring displayLower = FoldLower(display);
            const std::wstring sidLower = FoldLower(sid);
            for (const auto& t : terms)
            {
                // AND: a guid term is alternatively satisfied by the line's own session id
                // ("<guid> word" = search "word" within that session); any term may hit the
                // prompt text itself.
                if ((t.isGuid && t.textLower == sidLower) ||
                    MatchesQueryText(displayLower, t.textLower, TermIsFuzzy(t, q.fuzzy)))
                {
                    continue;
                }
                return;
            }
            auto& hit = out[sid];
            hit.sessionId = sid;
            ++hit.hitCount;
            if (hit.snippets.size() < maxSnippetsPerSession)
            {
                hit.snippets.push_back(MakeSnippet(display, snip->textLower, TermIsFuzzy(*snip, q.fuzzy), 160));
            }
        };

        const auto& rg = ResolveRipgrep();
        if (!rg.empty())
        {
            // rg filters the (5 MB+, ever-growing) file to candidate lines. One pattern can't
            // AND several terms (Rust regex: no lookahead), so it filters on the LONGEST text
            // term — a strict superset — and each candidate line is still verified against ALL
            // terms by the in-process matcher (identical semantics, exact attribution).
            std::string raw;
            const std::wstring args = L"-i --no-config --no-messages --no-line-number --regexp " +
                                      QuoteArg(BuildSearchRegex(sel->text, TermIsFuzzy(*sel, q.fuzzy))) + L" -- " + QuoteArg(historyPath);
            if (RunToolCapture(rg, args, kRgTimeoutMs, raw))
            {
                for (const auto& line : CaptureLines(raw))
                {
                    onLine(line);
                }
                return out;
            }
            // spawn failure -> fall through to the in-process scan
        }
        ForEachFileLine(historyPath, onLine);
        return out;
    }

    std::vector<SessionHit> SearchTranscriptsSlow(const std::vector<TranscriptRef>& refs, const SessionQuery& q, size_t maxSnippetsPerSession, const std::function<bool()>& cancelled)
    {
        std::vector<SessionHit> out;
        if (!q.scopeUser && !q.scopeAgent)
        {
            return out; // content search only exists for the message scopes
        }
        const auto terms = ParseSessionQuery(q.text);
        const SearchTerm* snip = nullptr; // first text term — anchors the snippets
        for (const auto& t : terms)
        {
            if (!t.isGuid)
            {
                snip = &t;
                break;
            }
        }
        if (!snip)
        {
            return out; // empty or guid-only query — identity matching is the fast phase's job
        }

        // ripgrep file-level filter, one round per TEXT term with the path list intersected
        // (one pattern can't AND several terms): a message matching ALL terms needs every text
        // term raw-present in its file, so each round narrows the next. Raw-match is a superset
        // of any scoped match (the scoped texts are substrings of their lines), so nothing is
        // missed. A guid term is satisfiable by file IDENTITY alone — it never filters files.
        // A spawn failure keeps the current (still-correct) superset for the in-process scan.
        std::vector<const TranscriptRef*> candidates;
        candidates.reserve(refs.size());
        for (const auto& r : refs)
        {
            candidates.push_back(&r);
        }
        const auto& rg = ResolveRipgrep();
        if (!rg.empty())
        {
            for (const auto& t : terms)
            {
                if (t.isGuid || candidates.empty())
                {
                    continue;
                }
                const std::wstring regex = BuildSearchRegex(t.text, TermIsFuzzy(t, q.fuzzy));
                std::vector<std::wstring> matched;
                // Many candidates in ONE projects folder (`<root>\<project>\<id>.jsonl`, every session of a period):
                // one ripgrep over the whole folder, the grep's way (GrepTranscripts; 0.7 s warm for 12 GB, measured
                // 2026-10-18), instead of one process per command line of paths - about 30 of them, one after another,
                // per term over every session. The folder's other files that match are dropped by the intersection.
                std::wstring root;
                if (candidates.size() > kGrepBatchFiles)
                {
                    for (const auto* r : candidates)
                    {
                        const std::wstring_view folder = DirPart(DirPart(r->path));
                        if (folder.empty() || (!root.empty() && FoldLower(root) != FoldLower(folder)))
                        {
                            root.clear();
                            break;
                        }
                        if (root.empty())
                        {
                            root.assign(folder);
                        }
                    }
                }
                bool filtered = !root.empty() && RgFilesWithMatchesInTree(rg, { regex }, root, matched);
                if (!filtered)
                {
                    // Few candidates, several folders, or the folder-wide run failed: the path lists, batched.
                    matched.clear();
                    std::vector<std::wstring> paths;
                    paths.reserve(candidates.size());
                    for (const auto* r : candidates)
                    {
                        paths.push_back(r->path);
                    }
                    filtered = RgFilesWithMatches(rg, regex, paths, matched);
                }
                if (!filtered)
                {
                    break; // rg unavailable mid-run: scan the current superset in-process
                }
                // The candidates rg named, in the candidates' order (a hash set of the folded paths: a folder-wide run
                // names thousands of files, and the pairwise compare this was before cost candidates x matches).
                std::unordered_set<std::wstring> hits;
                hits.reserve(matched.size());
                for (const auto& m : matched)
                {
                    hits.insert(FoldLower(m));
                }
                std::vector<const TranscriptRef*> next;
                next.reserve((std::min)(matched.size(), candidates.size()));
                for (const auto* r : candidates)
                {
                    if (hits.count(FoldLower(r->path)) != 0)
                    {
                        next.push_back(r);
                    }
                }
                candidates = std::move(next);
            }
        }

        const bool snipFuzzy = TermIsFuzzy(*snip, q.fuzzy);
        // The line prefilter (2026-10-18, the grep's own: LineHoldsNeedleOutsideCwd's needles and searchers): a message
        // matches only when its text holds EVERY term, and its raw JSON line then holds every plain term's
        // JSON-verbatim ASCII piece too, so a line missing one is never decoded or parsed - the parse is what this scan
        // spends its time on (the eval's deep search for "home icon return" over every session took 79 s). A fuzzy term
        // (letters in order, no contiguous piece) and a guid term (it may be satisfied by the file's own id) require
        // nothing; a term with no such piece (only non-ASCII text) requires nothing either.
        using LineSearcher = std::boyer_moore_horspool_searcher<std::string::const_iterator, FoldedByteHash, FoldedByteEqual>;
        std::vector<std::string> needles;
        for (const auto& t : terms)
        {
            if (t.isGuid || TermIsFuzzy(t, q.fuzzy))
            {
                continue;
            }
            if (std::string needle = AsciiLowerNeedle(GrepRequiredLiteral(t.text, false)); !needle.empty())
            {
                needles.push_back(std::move(needle));
            }
        }
        std::vector<LineSearcher> searchers;
        searchers.reserve(needles.size());
        for (const auto& needle : needles)
        {
            searchers.emplace_back(needle.cbegin(), needle.cend()); // `needles` is not resized after this: the searchers' pointers stay good
        }
        const auto lineHoldsEveryNeedle = [&searchers](std::string_view line) {
            for (const auto& searcher : searchers)
            {
                if (std::search(line.begin(), line.end(), searcher) == line.end())
                {
                    return false;
                }
            }
            return true;
        };
        // The scan's stop check: the caller's cancel, asked by every reader between files and every megabyte inside one.
        std::atomic<bool> stopped{ false };
        const std::function<bool()> stop = [&stopped, &cancelled]() -> bool {
            if (stopped.load(std::memory_order_relaxed))
            {
                return true;
            }
            if (cancelled && cancelled())
            {
                stopped.store(true, std::memory_order_relaxed);
                return true;
            }
            return false;
        };
        // One candidate, read to its end (or until the stop). The per-message logic is the one this scan always had.
        const auto scanOne = [&](const TranscriptRef& ref, SessionHit& hit) {
            hit.sessionId = ref.sessionId;
            // Terms this file's IDENTITY satisfies, resolved once: a guid term equal to the
            // session id holds for EVERY message in the file ("<guid> word" = search "word"
            // within that session); a guid that is NOT this file's id must appear in the
            // message text like any other term.
            const std::wstring sidLower = FoldLower(ref.sessionId);
            std::vector<char> idSat(terms.size(), 0);
            for (size_t i = 0; i < terms.size(); ++i)
            {
                idSat[i] = (terms[i].isGuid && terms[i].textLower == sidLower) ? 1 : 0;
            }
            const auto matchesAllTerms = [&](const std::wstring& textLower) {
                for (size_t i = 0; i < terms.size(); ++i)
                {
                    if (!idSat[i] && !MatchesQueryText(textLower, terms[i].textLower, TermIsFuzzy(terms[i], q.fuzzy)))
                    {
                        return false;
                    }
                }
                return true;
            };
            // ForEachTranscriptLineBytes, not ScanTranscript: the raw line comes first, so the prefilter can drop it
            // before the decode. The lines it hands over are ScanTranscript's (complete lines, CR stripped, empty ones
            // skipped), and each kept one is classified exactly as ScanTranscript did.
            (void)ForEachTranscriptLineBytes(ref.path, stop, [&](std::string_view line) -> bool {
                if (!lineHoldsEveryNeedle(line))
                {
                    return true;
                }
                const std::wstring wide = Utf8ToWide(line.data(), line.size());
                if (wide.empty())
                {
                    return true;
                }
                const TranscriptLineFacts f = ClassifyTranscriptLine(wide, q.scopeUser ? kScanTextBudget : 0, q.scopeAgent ? kScanTextBudget : 0);
                if (q.scopeUser && !f.userText.empty() && matchesAllTerms(FoldLower(f.userText)))
                {
                    ++hit.hitCount;
                    if (hit.snippets.size() < maxSnippetsPerSession)
                    {
                        hit.snippets.push_back(L"👤 " + MakeSnippet(f.userText, snip->textLower, snipFuzzy, 160));
                    }
                }
                if (q.scopeAgent && !f.agentText.empty() && matchesAllTerms(FoldLower(f.agentText)))
                {
                    ++hit.hitCount;
                    if (hit.snippets.size() < maxSnippetsPerSession)
                    {
                        hit.snippets.push_back(L"🤖 " + MakeSnippet(f.agentText, snip->textLower, snipFuzzy, 160));
                    }
                }
                return true;
            });
        };

        // The reads: each candidate by one of several threads (the grep's pool size: half the processors, 2 to 8),
        // each writing only its own slot; the hits are put together in the candidates' order afterwards, so the result
        // is the one the sequential scan gave. A cancel ends every reader at its next check; the slots read so far count.
        std::vector<SessionHit> slots(candidates.size());
        std::atomic<size_t> nextSlot{ 0 };
        const auto reader = [&]() noexcept {
            try
            {
                for (;;)
                {
                    if (stop())
                    {
                        return;
                    }
                    const size_t k = nextSlot.fetch_add(1);
                    if (k >= slots.size())
                    {
                        return;
                    }
                    scanOne(*candidates[k], slots[k]);
                }
            }
            catch (...)
            {
                // A reader must never let an exception out (std::terminate): the scan ends with what was read, which
                // is what a cancelled scan returns too.
                stopped.store(true);
                LogSwallowedException(L"SearchTranscriptsSlow: a reader thread");
            }
        };
        const unsigned hw = std::thread::hardware_concurrency();
        const size_t threads = (std::min)(std::clamp<size_t>(hw / 2, 2, 8), slots.size());
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (size_t t = 1; t < threads; ++t)
        {
            try
            {
                pool.emplace_back(reader);
            }
            catch (...)
            {
                // No more threads (std::system_error): the ones started, and this one, read everything.
                LogSwallowedException(L"SearchTranscriptsSlow: start a reader thread");
                break;
            }
        }
        if (!slots.empty())
        {
            reader(); // this thread reads too
        }
        for (auto& th : pool)
        {
            th.join();
        }
        for (auto& hit : slots)
        {
            if (hit.hitCount > 0)
            {
                out.push_back(std::move(hit));
            }
        }
        return out;
    }

    std::wstring GrepRequiredLiteral(std::wstring_view pattern, bool regex)
    {
        std::vector<std::wstring> runs;
        if (!regex)
        {
            runs.emplace_back(pattern);
        }
        else if (!RegexLiteralRuns(pattern, runs))
        {
            return {};
        }
        // Every run is in every match; the longest JSON-verbatim piece of any of them is the most
        // selective key that a raw line is still certain to hold.
        std::wstring best;
        for (const auto& run : runs)
        {
            std::wstring piece = LongestJsonVerbatimRun(run);
            if (piece.size() > best.size())
            {
                best = std::move(piece);
            }
        }
        return best;
    }

    PeriodActivity ReadPeriodActivity(const std::wstring& path, int64_t fromUnixMs, int64_t toUnixMs, int64_t ownSinceUnixMs, const std::function<bool()>& cancelled)
    {
        PeriodActivity out;
        // The TOP-LEVEL "type" and "timestamp" of a raw line, read in one pass that tracks the JSON nesting (outside
        // strings): a message's own content may hold objects with keys of the same names (a tool call's input), and a
        // quoted copy inside a string is escaped (\"), so only depth 1 is the line's own.
        const auto topLevel = [](std::string_view line, std::string_view& type, std::string_view& timestamp) {
            type = {};
            timestamp = {};
            int depth = 0;
            bool inString = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (inString)
                {
                    if (c == '\\')
                    {
                        ++i; // the escaped character
                    }
                    else if (c == '"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (c == '{' || c == '[')
                {
                    ++depth;
                    continue;
                }
                if (c == '}' || c == ']')
                {
                    --depth;
                    continue;
                }
                if (c != '"')
                {
                    continue;
                }
                // A string at depth 1 may be a key with a string value: "key":"value".
                const size_t keyEnd = line.find('"', i + 1);
                if (keyEnd == std::string_view::npos)
                {
                    return;
                }
                const std::string_view key = line.substr(i + 1, keyEnd - i - 1);
                if (depth == 1 && keyEnd + 2 < line.size() && line[keyEnd + 1] == ':' && line[keyEnd + 2] == '"' && (key == "type" || key == "timestamp"))
                {
                    const size_t valueAt = keyEnd + 3;
                    const size_t valueEnd = line.find('"', valueAt);
                    if (valueEnd == std::string_view::npos)
                    {
                        return;
                    }
                    (key == "type" ? type : timestamp) = line.substr(valueAt, valueEnd - valueAt);
                    if (!type.empty() && !timestamp.empty())
                    {
                        return;
                    }
                    i = valueEnd; // past the value
                    continue;
                }
                inString = true; // any other string: skip it whole (a key holds no escaped quote that matters here)
            }
        };
        const int code = ForEachTranscriptLineBytes(path, cancelled, [&](std::string_view line) -> bool {
            std::string_view type;
            std::string_view timestamp;
            topLevel(line, type, timestamp);
            const bool user = type == "user";
            if ((!user && type != "assistant") || timestamp.empty() || timestamp.size() > 40)
            {
                return true; // not a conversation line, or no time: not activity
            }
            const int64_t ts = ParseTranscriptTimestamp(Utf8ToWide(timestamp.data(), timestamp.size()));
            if (ts == 0 || ts < ownSinceUnixMs || ts < fromUnixMs || ts > toUnixMs)
            {
                return true;
            }
            ++out.messages;
            if (user && line.find("\"tool_result\"") == std::string_view::npos && line.find("\"isMeta\":true") == std::string_view::npos)
            {
                ++out.prompts;
            }
            return true;
        });
        out.read = code != -1;
        return out;
    }

    std::wstring GrepUnescapePlainPattern(std::wstring_view pattern)
    {
        static constexpr std::wstring_view kEscapable = L".()[]{}?*+|^$-";
        std::wstring out;
        out.reserve(pattern.size());
        for (size_t i = 0; i < pattern.size(); ++i)
        {
            if (pattern[i] == L'\\' && i + 1 < pattern.size() && kEscapable.find(pattern[i + 1]) != std::wstring_view::npos)
            {
                continue; // the backslash of `\.`; the character itself is kept by the next step
            }
            out.push_back(pattern[i]);
        }
        return out;
    }

    std::vector<std::wstring> GrepRequiredLiterals(std::wstring_view pattern, bool regex)
    {
        std::vector<std::wstring> out;
        if (!regex)
        {
            // Plain text: itself (its longest JSON-verbatim piece), and its unescaped spelling when the model escaped
            // a regex character in it — both are matched (FindGrepMatch), so both are keys.
            for (const std::wstring& form : { std::wstring{ pattern }, GrepUnescapePlainPattern(pattern) })
            {
                std::wstring piece = LongestJsonVerbatimRun(form);
                if (piece.empty())
                {
                    return {}; // a form with no verbatim piece (only quotes / backslashes): no key holds for it
                }
                if (std::find(out.begin(), out.end(), piece) == out.end())
                {
                    out.push_back(std::move(piece));
                }
            }
            return out;
        }
        return RegexAnyOfLiterals(pattern, 0);
    }

    std::wstring MakeGrepExcerpt(std::wstring_view text, size_t matchPos, size_t matchLen, size_t contextChars)
    {
        matchPos = (std::min)(matchPos, text.size());
        matchLen = (std::min)(matchLen, text.size() - matchPos);
        const size_t quoted = (std::min)(matchLen, kGrepMaxQuotedMatchChars);
        size_t begin = matchPos > contextChars ? matchPos - contextChars : 0;
        // A match quoted only in part ends the excerpt inside it (with "…"); a whole one gets its
        // right context too.
        size_t end = (std::min)(text.size(), matchPos + quoted + (quoted < matchLen ? 0 : contextChars));
        // Never split a surrogate pair: a cut between its halves moves outward to keep the pair.
        if (begin > 0 && begin < text.size() && text[begin] >= 0xDC00 && text[begin] <= 0xDFFF)
        {
            --begin;
        }
        if (end > 0 && end < text.size() && text[end - 1] >= 0xD800 && text[end - 1] <= 0xDBFF)
        {
            ++end;
        }
        std::wstring out;
        if (begin > 0)
        {
            out += L"\x2026";
        }
        out += CollapseWs(text.substr(begin, end - begin));
        if (end < text.size())
        {
            out += L"\x2026";
        }
        return out;
    }

    GrepOutcome GrepTranscripts(const std::vector<TranscriptRef>& refs, const GrepOptions& options, const std::function<bool()>& cancelled)
    {
        GrepOutcome out;
        if (options.pattern.empty())
        {
            out.error = L"The pattern is empty: give it the text to find.";
            return out;
        }
        if (options.pattern.size() > kRegexMaxPatternChars)
        {
            out.error = L"The pattern is longer than " + std::to_wstring(kRegexMaxPatternChars) + L" characters: give it a shorter piece of the text.";
            return out;
        }
        GrepRun run;
        run.options = &options;
        std::wregex re;
        if (options.regex)
        {
            if (!detail::BuildRegex(options.pattern, /*caseInsensitive*/ true, re))
            {
                out.error = L"The pattern is not a valid regular expression (ECMAScript syntax). Fix it, or set regex to false to find it as plain text.";
                return out;
            }
            // A regex that matches empty text matches every message (the first position of each):
            // a hit count that means nothing. Refused, with what to do instead.
            bool matchesEmpty = false;
            try
            {
                matchesEmpty = std::regex_search(std::wstring{}, re);
            }
            catch (const std::regex_error&)
            {
                // Expected control flow (RegexUtil.h's Rule #18 exemption): a regex that gives up
                // on empty text is not one that matches it — the grep goes on, and each message
                // it gives up on is counted in tooComplex.
            }
            if (matchesEmpty)
            {
                out.error = L"The regex can match empty text, so it would match every message: make it require something (for example + instead of *).";
                return out;
            }
            run.re = &re;
        }
        else
        {
            // The pattern as written, and its unescaped spelling when the model escaped a regex character in plain
            // text ("TabDragMath\.h"): a message holding either matches (GrepUnescapePlainPattern).
            run.patternLowers.push_back(FoldLower(options.pattern));
            if (const std::wstring unescaped = FoldLower(GrepUnescapePlainPattern(options.pattern)); unescaped != run.patternLowers.front() && !unescaped.empty())
            {
                run.patternLowers.push_back(unescaped);
            }
        }
        // The prefilter keys: one piece every match holds, or one per alternative of an alternation (any-of). A key
        // whose ASCII needle is empty (only non-ASCII text, or only spaces) cannot be required, so then no line is
        // skipped at all.
        const std::vector<std::wstring> literals = GrepRequiredLiterals(options.pattern, options.regex);
        bool needleless = literals.empty();
        for (const auto& literal : literals)
        {
            std::string needle = AsciiLowerNeedle(literal);
            if (needle.empty())
            {
                needleless = true;
                break;
            }
            run.needles.push_back(std::move(needle));
        }
        if (needleless)
        {
            run.needles.clear();
        }
        run.searchers.reserve(run.needles.size());
        for (const auto& needle : run.needles)
        {
            run.searchers.emplace_back(needle.cbegin(), needle.cend()); // `needles` is not resized after this: the searchers' pointers stay good
        }

        // When the budget started. Not const: the ONE tree-wide ripgrep below restarts it (see there).
        ULONGLONG started = ::GetTickCount64();
        std::atomic<bool> stopped{ false }; // a reader saw the budget end or the cancel: the others stop at their next check
        const auto stop = [&]() -> bool {
            if (stopped.load(std::memory_order_relaxed))
            {
                return true;
            }
            if ((cancelled && cancelled()) || (options.budgetMs > 0 && ::GetTickCount64() - started >= static_cast<ULONGLONG>(options.budgetMs)))
            {
                stopped.store(true, std::memory_order_relaxed);
                return true;
            }
            return false;
        };

        // 1. The candidates: the refs whose file holds a key (ripgrep), in the refs' order. Every key must be long
        // enough to drop files (a short one is in most transcripts), or ripgrep is not worth a process.
        const auto& rg = ResolveRipgrep();
        bool useRg = !rg.empty() && !literals.empty() && !needleless;
        for (const auto& literal : literals)
        {
            useRg = useRg && NonSpaceCount(literal) >= kGrepMinRgLiteral;
        }
        std::vector<std::wstring> rgRegexes;
        if (useRg)
        {
            for (const auto& literal : literals)
            {
                rgRegexes.push_back(BuildSearchRegex(literal, false));
            }
        }
        std::vector<const TranscriptRef*> candidates;
        candidates.reserve(refs.size());
        size_t filteredUpTo = 0; // refs before this index went through the filter (or were taken unfiltered)
        // ONE ripgrep over the whole projects folder when the refs are many and all sit in one: `<root>\<project>\
        // <id>.jsonl`. The batches below take the rest (or everything, when the refs are few or spread).
        if (useRg && refs.size() > kGrepBatchFiles)
        {
            std::wstring root;
            bool oneRoot = true;
            for (const auto& ref : refs)
            {
                const std::wstring_view project = DirPart(ref.path);
                const std::wstring_view folder = DirPart(project);
                if (folder.empty())
                {
                    oneRoot = false;
                    break;
                }
                if (root.empty())
                {
                    root.assign(folder);
                }
                else if (FoldLower(root) != FoldLower(folder))
                {
                    oneRoot = false;
                    break;
                }
            }
            std::vector<std::wstring> matched;
            if (oneRoot && !root.empty() && RgFilesWithMatchesInTree(rg, rgRegexes, root, matched))
            {
                std::unordered_set<std::wstring> hits;
                hits.reserve(matched.size());
                for (const auto& m : matched)
                {
                    hits.insert(FoldLower(m));
                }
                for (const auto& ref : refs)
                {
                    if (hits.count(FoldLower(ref.path)) != 0)
                    {
                        candidates.push_back(&ref); // in the refs' order, not rg's
                    }
                }
                filteredUpTo = refs.size();
                out.prefiltered = true;
                // The read budget starts NOW. That ripgrep reads every transcript of the folder (12 GB here) and has its
                // own timeout (kRgTimeoutMs); with the files out of the cache it can take as long as the whole budget, and
                // the readers then started with none left: the 2026-10-18 eval's grep for "proxmox" over every session
                // answered "stopped" having read 0 of its 346 candidates, where the same call reads them all in 3 s warm.
                started = ::GetTickCount64();
            }
        }
        // The batches: ripgrep over at most kGrepBatchFiles files at a time (a batch rg cannot do is taken whole, and
        // rg is not asked again).
        while (filteredUpTo < refs.size())
        {
            if (stop())
            {
                out.complete = false;
                break;
            }
            size_t end = filteredUpTo;
            int64_t bytes = 0;
            while (end < refs.size() && end - filteredUpTo < kGrepBatchFiles && (end == filteredUpTo || bytes + refs[end].sizeBytes <= kGrepBatchBytes))
            {
                bytes += refs[end].sizeBytes;
                ++end;
            }
            bool filtered = false;
            if (useRg)
            {
                std::vector<std::wstring> paths;
                paths.reserve(end - filteredUpTo);
                for (size_t k = filteredUpTo; k < end; ++k)
                {
                    paths.push_back(refs[k].path);
                }
                std::vector<std::wstring> matched;
                if (RgFilesWithMatchesAny(rg, rgRegexes, paths, matched))
                {
                    std::unordered_set<std::wstring> hits;
                    hits.reserve(matched.size());
                    for (const auto& m : matched)
                    {
                        hits.insert(FoldLower(m));
                    }
                    for (size_t k = filteredUpTo; k < end; ++k)
                    {
                        if (hits.count(FoldLower(refs[k].path)) != 0)
                        {
                            candidates.push_back(&refs[k]);
                        }
                    }
                    filtered = true;
                    out.prefiltered = true;
                }
                else
                {
                    useRg = false; // spawn failure, timeout, or a batch rg could not search
                }
            }
            if (!filtered)
            {
                for (size_t k = filteredUpTo; k < end; ++k)
                {
                    candidates.push_back(&refs[k]);
                }
            }
            filteredUpTo = end;
        }
        out.candidates = candidates.size();

        // 2. The reads: each candidate by one of several threads, the next one taken in order, so a grep that runs out
        // of time has read the FIRST candidates (the newest sessions, for a newest-first caller). Each thread writes only
        // its own slots; the outcome is assembled in the candidates' order afterwards.
        struct Slot
        {
            GrepSessionHit hit;
            int read{ 2 }; // 2 = never taken; else GrepOneTranscript's code (0 read to its end, 1 stopped, -1 not opened)
        };
        std::vector<Slot> slots(candidates.size());
        std::atomic<size_t> nextSlot{ 0 };
        std::atomic<bool> readerFailed{ false };
        const auto reader = [&]() noexcept {
            try
            {
                for (;;)
                {
                    if (stop())
                    {
                        return;
                    }
                    const size_t k = nextSlot.fetch_add(1);
                    if (k >= slots.size())
                    {
                        return;
                    }
                    slots[k].hit.sessionId = candidates[k]->sessionId;
                    slots[k].read = GrepOneTranscript(*candidates[k], run, stop, slots[k].hit);
                    if (slots[k].read == 1)
                    {
                        return; // stopped mid-file: the budget or the cancel
                    }
                }
            }
            catch (...)
            {
                // A reader must never let an exception out (std::terminate). Its unread candidates make the outcome
                // incomplete, which the model is told — never a silent "no match".
                readerFailed.store(true);
                stopped.store(true);
                LogSwallowedException(L"GrepTranscripts: a reader thread");
            }
        };
        size_t threads = options.threads;
        if (threads == 0)
        {
            const unsigned hw = std::thread::hardware_concurrency();
            threads = std::clamp<size_t>(hw / 2, 2, 8);
        }
        threads = (std::min)(threads, slots.size());
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (size_t t = 1; t < threads; ++t)
        {
            try
            {
                pool.emplace_back(reader);
            }
            catch (...)
            {
                // No more threads (std::system_error): the ones started, and this one, read everything.
                LogSwallowedException(L"GrepTranscripts: start a reader thread");
                break;
            }
        }
        if (!slots.empty())
        {
            reader(); // this thread reads too
        }
        for (auto& th : pool)
        {
            th.join();
        }
        for (auto& slot : slots)
        {
            if (slot.read == 0)
            {
                ++out.scanned;
            }
            if (slot.read == 1 || slot.read == 2)
            {
                out.complete = false; // stopped half way, or never reached
            }
            if (slot.hit.hits > 0)
            {
                out.sessions.push_back(std::move(slot.hit)); // a file stopped half way keeps what it found
            }
        }
        if (readerFailed.load())
        {
            out.complete = false;
        }
        out.notReached = refs.size() - filteredUpTo;
        out.tooComplex = run.tooComplex.load();
        return out;
    }
}
