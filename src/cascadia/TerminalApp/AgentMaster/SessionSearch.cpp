// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See SessionSearch.h / SESSIONS.md §6.
#define NOMINMAX
#include "SessionSearch.h"

#include <windows.h>

#include <algorithm>
#include <cwctype> // towlower

#include "Json.h" // history.jsonl line parsing

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
    bool RunToolCapture(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs, std::string& out)
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
    // (false) on spawn failure so callers fall back to scanning everything.
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
            if (!RunToolCapture(rg, args, kRgTimeoutMs, out))
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
            snip.insert(snip.begin(), L'…');
        }
        return snip;
    }

    std::vector<std::wstring> SearchIndexFast(const std::vector<SessionIndexEntry>& entries, const SessionQuery& q)
    {
        std::vector<std::wstring> out;
        out.reserve(entries.size());
        if (q.text.empty())
        {
            for (const auto& e : entries)
            {
                out.push_back(e.sessionId); // window-only listing
            }
            return out;
        }
        const std::wstring needle = FoldLower(q.text);
        for (const auto& e : entries)
        {
            const auto& st = e.stats;
            bool hit =
                // Baseline (both message scopes OFF — and always useful): title + directory.
                MatchesQueryText(FoldLower(st.customTitle), needle, q.fuzzy) ||
                MatchesQueryText(FoldLower(st.aiTitle), needle, q.fuzzy) ||
                MatchesQueryText(FoldLower(st.summary), needle, q.fuzzy) ||
                MatchesQueryText(FoldLower(st.firstUserPrompt), needle, q.fuzzy) ||
                MatchesQueryText(FoldLower(st.cwd), needle, q.fuzzy);
            if (!hit && (q.scopeDirs || q.scopeFiles))
            {
                for (const auto& p : st.pathsAccessed)
                {
                    const std::wstring lowered = FoldLower(p);
                    if (q.scopeDirs && MatchesQueryText(DirPart(lowered), needle, q.fuzzy))
                    {
                        hit = true;
                        break;
                    }
                    if (q.scopeFiles && MatchesQueryText(LeafPart(lowered), needle, q.fuzzy))
                    {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit)
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
        if (q.text.empty())
        {
            return out;
        }
        const std::wstring needle = FoldLower(q.text);
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
            if (display.empty() || !MatchesQueryText(FoldLower(display), needle, q.fuzzy))
            {
                return;
            }
            auto& hit = out[sid];
            hit.sessionId = sid;
            ++hit.hitCount;
            if (hit.snippets.size() < maxSnippetsPerSession)
            {
                hit.snippets.push_back(MakeSnippet(display, needle, q.fuzzy, 160));
            }
        };

        const auto& rg = ResolveRipgrep();
        if (!rg.empty())
        {
            // rg filters the (5 MB+, ever-growing) file to candidate lines; each candidate is
            // still verified by the in-process matcher (identical semantics, exact attribution).
            std::string raw;
            const std::wstring args = L"-i --no-config --no-messages --no-line-number --regexp " +
                                      QuoteArg(BuildSearchRegex(q.text, q.fuzzy)) + L" -- " + QuoteArg(historyPath);
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
        if (q.text.empty() || (!q.scopeUser && !q.scopeAgent))
        {
            return out; // content search only exists for the message scopes
        }
        const std::wstring needle = FoldLower(q.text);

        // ripgrep file-level filter: only files with at least one RAW match get the precise
        // in-process scan. Raw-match is a superset of any scoped match (the scoped texts are
        // substrings of their lines), so nothing is missed.
        std::vector<const TranscriptRef*> candidates;
        const auto& rg = ResolveRipgrep();
        bool filtered = false;
        if (!rg.empty())
        {
            std::vector<std::wstring> paths;
            paths.reserve(refs.size());
            for (const auto& r : refs)
            {
                paths.push_back(r.path);
            }
            std::vector<std::wstring> matched;
            if (RgFilesWithMatches(rg, BuildSearchRegex(q.text, q.fuzzy), paths, matched))
            {
                for (const auto& m : matched)
                {
                    for (const auto& r : refs)
                    {
                        if (m.size() == r.path.size() && ::_wcsicmp(m.c_str(), r.path.c_str()) == 0)
                        {
                            candidates.push_back(&r);
                            break;
                        }
                    }
                }
                filtered = true;
            }
        }
        if (!filtered)
        {
            for (const auto& r : refs)
            {
                candidates.push_back(&r); // no rg: scan everything (slower, same results)
            }
        }

        for (const auto* ref : candidates)
        {
            if (cancelled && cancelled())
            {
                break;
            }
            SessionHit hit;
            hit.sessionId = ref->sessionId;
            ScanTranscript(ref->path, 0, q.scopeUser ? kScanTextBudget : 0, q.scopeAgent ? kScanTextBudget : 0, [&](const TranscriptLineFacts& f) {
                if (q.scopeUser && !f.userText.empty() && MatchesQueryText(FoldLower(f.userText), needle, q.fuzzy))
                {
                    ++hit.hitCount;
                    if (hit.snippets.size() < maxSnippetsPerSession)
                    {
                        hit.snippets.push_back(L"👤 " + MakeSnippet(f.userText, needle, q.fuzzy, 160));
                    }
                }
                if (q.scopeAgent && !f.agentText.empty() && MatchesQueryText(FoldLower(f.agentText), needle, q.fuzzy))
                {
                    ++hit.hitCount;
                    if (hit.snippets.size() < maxSnippetsPerSession)
                    {
                        hit.snippets.push_back(L"🤖 " + MakeSnippet(f.agentText, needle, q.fuzzy, 160));
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
}
