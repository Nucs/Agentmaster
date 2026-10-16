// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See SessionSearch.h / SESSIONS.md §6.
#define NOMINMAX
#include "SessionSearch.h"

#include <windows.h>

#include <algorithm>
#include <cwctype> // towlower
#include <optional>
#include <unordered_set>

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
    bool RgFilesWithMatches(const std::wstring& rg, const std::wstring& regex, const std::vector<std::wstring>& paths, std::vector<std::wstring>& matched)
    {
        size_t i = 0;
        while (i < paths.size())
        {
            std::wstring args = L"-il --no-config --no-messages --regexp " + QuoteArg(regex) + L" --";
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

    // One grep's fixed parts, shared by its per-file reads. Not copyable: the searcher points
    // into `needle`.
    struct GrepRun
    {
        const GrepOptions* options{};
        const std::wregex* re{}; // the compiled regex, or null for a literal pattern
        std::wstring patternLower; // a literal pattern, folded like FoldLower folds the text
        std::string needle; // AsciiLowerNeedle of the required literal; empty = parse every line
        std::optional<std::boyer_moore_horspool_searcher<std::string::const_iterator, FoldedByteHash, FoldedByteEqual>> searcher;
        int tooComplex{};

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
        const size_t at = lowered.find(run.patternLower);
        if (at == std::wstring::npos)
        {
            return false;
        }
        pos = at;
        len = run.patternLower.size();
        return true;
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
            // The byte prefilter: a line without the required literal cannot hold a match, so it
            // is never decoded or parsed (the JSON parse is what a grep spends its time on).
            if (run.searcher && std::search(line.begin(), line.end(), *run.searcher) == line.end())
            {
                return true;
            }
            if (stop && stop())
            {
                return false; // out of time, or cancelled: before the costly part of this line
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
                std::vector<std::wstring> paths;
                paths.reserve(candidates.size());
                for (const auto* r : candidates)
                {
                    paths.push_back(r->path);
                }
                std::vector<std::wstring> matched;
                if (!RgFilesWithMatches(rg, BuildSearchRegex(t.text, TermIsFuzzy(t, q.fuzzy)), paths, matched))
                {
                    break; // rg unavailable mid-run: scan the current superset in-process
                }
                std::vector<const TranscriptRef*> next;
                next.reserve(matched.size());
                for (const auto& m : matched)
                {
                    for (const auto* r : candidates)
                    {
                        if (m.size() == r->path.size() && ::_wcsicmp(m.c_str(), r->path.c_str()) == 0)
                        {
                            next.push_back(r);
                            break;
                        }
                    }
                }
                candidates = std::move(next);
            }
        }

        const bool snipFuzzy = TermIsFuzzy(*snip, q.fuzzy);
        for (const auto* ref : candidates)
        {
            if (cancelled && cancelled())
            {
                break;
            }
            SessionHit hit;
            hit.sessionId = ref->sessionId;
            // Terms this file's IDENTITY satisfies, resolved once: a guid term equal to the
            // session id holds for EVERY message in the file ("<guid> word" = search "word"
            // within that session); a guid that is NOT this file's id must appear in the
            // message text like any other term.
            const std::wstring sidLower = FoldLower(ref->sessionId);
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
            ScanTranscript(ref->path, 0, q.scopeUser ? kScanTextBudget : 0, q.scopeAgent ? kScanTextBudget : 0, [&](const TranscriptLineFacts& f) {
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
            });
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
            run.patternLower = FoldLower(options.pattern);
        }
        const std::wstring literal = GrepRequiredLiteral(options.pattern, options.regex);
        run.needle = AsciiLowerNeedle(literal);
        if (!run.needle.empty())
        {
            run.searcher.emplace(run.needle.cbegin(), run.needle.cend());
        }

        const ULONGLONG started = ::GetTickCount64();
        const auto stop = [&]() -> bool {
            if (cancelled && cancelled())
            {
                return true;
            }
            return options.budgetMs > 0 && ::GetTickCount64() - started >= static_cast<ULONGLONG>(options.budgetMs);
        };

        // ripgrep filters on the literal one batch at a time, and each batch's files are read
        // before the next batch is filtered: a grep that runs out of time has read the FIRST refs
        // (the newest sessions, for a newest-first caller), not filtered everything and read
        // nothing. A batch that rg cannot do is read in full, and rg is not asked again.
        const auto& rg = ResolveRipgrep();
        bool useRg = !rg.empty() && NonSpaceCount(literal) >= kGrepMinRgLiteral;
        const std::wstring rgRegex = useRg ? BuildSearchRegex(literal, false) : std::wstring{};
        size_t next = 0; // the first ref not yet filtered
        while (next < refs.size() && out.complete)
        {
            if (stop())
            {
                out.complete = false;
                break;
            }
            size_t end = next;
            int64_t bytes = 0;
            while (end < refs.size() && end - next < kGrepBatchFiles && (end == next || bytes + refs[end].sizeBytes <= kGrepBatchBytes))
            {
                bytes += refs[end].sizeBytes;
                ++end;
            }
            std::vector<const TranscriptRef*> batch;
            batch.reserve(end - next);
            bool filtered = false;
            if (useRg)
            {
                std::vector<std::wstring> paths;
                paths.reserve(end - next);
                for (size_t k = next; k < end; ++k)
                {
                    paths.push_back(refs[k].path);
                }
                std::vector<std::wstring> matched;
                if (RgFilesWithMatches(rg, rgRegex, paths, matched))
                {
                    std::unordered_set<std::wstring> hits;
                    hits.reserve(matched.size());
                    for (const auto& m : matched)
                    {
                        hits.insert(FoldLower(m));
                    }
                    for (size_t k = next; k < end; ++k)
                    {
                        if (hits.count(FoldLower(refs[k].path)) != 0)
                        {
                            batch.push_back(&refs[k]); // in the refs' order, not rg's
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
                for (size_t k = next; k < end; ++k)
                {
                    batch.push_back(&refs[k]);
                }
            }
            next = end;
            out.candidates += batch.size();
            for (const auto* ref : batch)
            {
                if (stop())
                {
                    out.complete = false;
                    break;
                }
                GrepSessionHit hit;
                hit.sessionId = ref->sessionId;
                const int read = GrepOneTranscript(*ref, run, stop, hit);
                if (read == 0)
                {
                    ++out.scanned;
                }
                if (hit.hits > 0)
                {
                    out.sessions.push_back(std::move(hit)); // a file stopped half way keeps what it found
                }
                if (read == 1)
                {
                    out.complete = false;
                    break;
                }
            }
        }
        out.notReached = refs.size() - next;
        out.tooComplex = run.tooComplex;
        return out;
    }
}
