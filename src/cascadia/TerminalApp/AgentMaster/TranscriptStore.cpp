// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See TranscriptStore.h / SESSIONS.md §6.
#define NOMINMAX
#include "TranscriptStore.h"

#include <windows.h>

#include <time.h> // _mkgmtime64 (ISO timestamp -> Unix epoch)

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root
#include "Json.h" // transcript line parsing
#include "Persistence.h" // NormDirKey — filesystem-aware cwd comparison (Rule #8)
#include "ProcessInspect.h" // EncodeCwdToProjectDir — cwd -> project-dir leaf

namespace
{
    using namespace Agentmaster;

    // A corrupt/binary run with no newline must not wedge the scan cursor forever (the same
    // guard the SessionScanner's tail uses): past this many carried bytes, skip the run.
    constexpr size_t kForceConsumeBytes = 8u << 20;
    // Chunk size for the streaming scan (bounded memory even on multi-GB corpora).
    constexpr size_t kScanChunkBytes = 1u << 20;
    // Head/tail window ladder for the quick-facts reads. Single lines exceed 1 MiB in the real
    // corpus, and the literal tail is usually untimestamped state lines — grow until a
    // timestamped message is found or the cap is hit.
    constexpr int64_t kWindowLadder[] = { 64 << 10, 256 << 10, 1 << 20, 4 << 20, 16 << 20 };

    // FILETIME (100ns ticks since 1601) -> Unix epoch ms. 0 for a pre-epoch value.
    int64_t FileTimeToUnixMs(const FILETIME& ft)
    {
        ULARGE_INTEGER u{};
        u.HighPart = ft.dwHighDateTime;
        u.LowPart = ft.dwLowDateTime;
        if (u.QuadPart < 116444736000000000ULL)
        {
            return 0;
        }
        return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }

    // UTF-8 bytes -> wide. Claude transcripts are UTF-8 .jsonl. Empty on empty/failure.
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

    // Collapse a (possibly multi-line) text to a single trimmed display line (the title shape).
    std::wstring FirstLineTrim(const std::wstring& s)
    {
        size_t i = 0;
        while (i < s.size() && (s[i] == L'\n' || s[i] == L'\r' || s[i] == L' ' || s[i] == L'\t'))
        {
            ++i;
        }
        size_t j = i;
        while (j < s.size() && s[j] != L'\n' && s[j] != L'\r')
        {
            ++j;
        }
        while (j > i && (s[j - 1] == L' ' || s[j - 1] == L'\t'))
        {
            --j;
        }
        return s.substr(i, j - i);
    }

    // Append `src` onto `dst` within a total-character budget (0 budget == extraction disabled).
    void AppendCapped(std::wstring& dst, std::wstring_view src, size_t maxChars)
    {
        if (maxChars == 0 || dst.size() >= maxChars || src.empty())
        {
            return;
        }
        if (!dst.empty())
        {
            dst.push_back(L' '); // block separator (keeps tokens from gluing across blocks)
        }
        const size_t room = maxChars - std::min(dst.size(), maxChars);
        dst.append(src.substr(0, std::min(src.size(), room)));
    }

    // A numeric `timestamp` from the oldest strata: >= 1e11 == epoch ms, >= 1e9 == epoch seconds.
    int64_t EpochNumToMs(double n)
    {
        if (n >= 1e11)
        {
            return static_cast<int64_t>(n);
        }
        if (n >= 1e9)
        {
            return static_cast<int64_t>(n) * 1000;
        }
        return 0;
    }

    // Open a transcript for shared reading (claude appends while we read — never block it).
    HANDLE OpenShared(const std::wstring& path)
    {
        return ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }

    // Read a whole (small) file, capped. Empty on failure / oversize. Shared like OpenShared.
    std::string ReadFileWhole(const std::wstring& path, size_t maxBytes)
    {
        const HANDLE h = OpenShared(path);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        if (sz.QuadPart < 0 || static_cast<uint64_t>(sz.QuadPart) > maxBytes)
        {
            ::CloseHandle(h);
            return {};
        }
        std::string bytes(static_cast<size_t>(sz.QuadPart), '\0');
        size_t off = 0;
        while (off < bytes.size())
        {
            DWORD got = 0;
            if (!::ReadFile(h, bytes.data() + off, static_cast<DWORD>(std::min<size_t>(bytes.size() - off, kScanChunkBytes)), &got, nullptr) || got == 0)
            {
                break;
            }
            off += got;
        }
        ::CloseHandle(h);
        bytes.resize(off);
        return bytes;
    }

    // Write text as UTF-8 via a temp file + atomic replace (a torn sidecar must never survive).
    bool WriteFileUtf8(const std::wstring& path, const std::wstring& text)
    {
        int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
        }
        const std::wstring tmp = path + L".tmp";
        const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD w = 0;
        const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr);
        ::CloseHandle(h);
        if (!ok || w != bytes.size())
        {
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    // Read `want` bytes at `offset`. Returns the bytes actually read (short on EOF/error).
    std::string ReadAt(HANDLE h, int64_t offset, size_t want)
    {
        LARGE_INTEGER li{};
        li.QuadPart = offset;
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        {
            return {};
        }
        std::string bytes(want, '\0');
        size_t off = 0;
        while (off < bytes.size())
        {
            DWORD got = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - off, kScanChunkBytes));
            if (!::ReadFile(h, bytes.data() + off, chunk, &got, nullptr) || got == 0)
            {
                break;
            }
            off += got;
        }
        bytes.resize(off);
        return bytes;
    }

    // Iterate the COMPLETE lines of a byte window: cb(lineBytes) per newline-terminated line
    // (CR trimmed, blanks skipped). When `includeFinalPartial`, the segment after the last '\n'
    // is also delivered (used when the window provably reaches EOF). Returns the byte count up
    // to and including the last consumed '\n'.
    template<typename CB>
    size_t ForEachLine(std::string_view bytes, bool includeFinalPartial, CB&& cb)
    {
        size_t consumed = 0;
        size_t start = 0;
        for (size_t i = 0; i <= bytes.size(); ++i)
        {
            const bool atEnd = (i == bytes.size());
            if (!atEnd && bytes[i] != '\n')
            {
                continue;
            }
            if (atEnd && !includeFinalPartial)
            {
                break;
            }
            std::string_view line = bytes.substr(start, i - start);
            if (!atEnd)
            {
                consumed = i + 1;
            }
            start = i + 1;
            while (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            bool blank = true;
            for (const char c : line)
            {
                if (c != ' ' && c != '\t')
                {
                    blank = false;
                    break;
                }
            }
            if (!blank)
            {
                cb(line);
            }
            if (atEnd)
            {
                break;
            }
        }
        return consumed;
    }
}

namespace Agentmaster
{
    // ===== enumeration =======================================================================

    bool IsSessionIdStem(std::wstring_view stem)
    {
        if (stem.size() != 36)
        {
            return false;
        }
        for (size_t i = 0; i < 36; ++i)
        {
            const wchar_t c = stem[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != L'-')
                {
                    return false;
                }
            }
            else
            {
                const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                if (!hex)
                {
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<TranscriptRef> EnumerateTranscriptsIn(std::wstring_view projectsDir, int64_t sinceUnixMs)
    {
        std::vector<TranscriptRef> out;
        if (projectsDir.empty())
        {
            return out;
        }
        const std::wstring root{ projectsDir };
        WIN32_FIND_DATAW fd{};
        const HANDLE hd = ::FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (hd == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        static const std::wstring ext = L".jsonl";
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                continue;
            }
            const std::wstring leaf = fd.cFileName;
            if (leaf == L"." || leaf == L"..")
            {
                continue;
            }
            const std::wstring dir = root + L"\\" + leaf;
            WIN32_FIND_DATAW ff{};
            const HANDLE hf = ::FindFirstFileW((dir + L"\\*.jsonl").c_str(), &ff);
            if (hf == INVALID_HANDLE_VALUE)
            {
                continue; // a project dir with no transcripts (e.g. only memory/) — or swept mid-walk
            }
            do
            {
                if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                const std::wstring name = ff.cFileName;
                if (name.size() <= ext.size())
                {
                    continue;
                }
                const std::wstring stem = name.substr(0, name.size() - ext.size());
                if (!IsSessionIdStem(stem))
                {
                    continue; // legacy agent-*.jsonl / anything that is not a session transcript
                }
                const int64_t mtime = FileTimeToUnixMs(ff.ftLastWriteTime);
                if (sinceUnixMs > 0 && mtime < sinceUnixMs)
                {
                    continue; // mtime >= last-activity always, so this never drops an in-window session
                }
                TranscriptRef r;
                r.sessionId = stem;
                r.path = dir + L"\\" + name;
                r.projectDirLeaf = leaf;
                r.sizeBytes = (static_cast<int64_t>(ff.nFileSizeHigh) << 32) | static_cast<int64_t>(ff.nFileSizeLow);
                r.mtimeMs = mtime;
                r.birthMs = FileTimeToUnixMs(ff.ftCreationTime);
                out.push_back(std::move(r));
            } while (::FindNextFileW(hf, &ff));
            ::FindClose(hf);
        } while (::FindNextFileW(hd, &fd));
        ::FindClose(hd);

        std::sort(out.begin(), out.end(), [](const TranscriptRef& a, const TranscriptRef& b) {
            return a.mtimeMs > b.mtimeMs; // newest-activity first (superset order; refine per-row)
        });
        return out;
    }

    std::vector<TranscriptRef> EnumerateTranscripts(int64_t sinceUnixMs)
    {
        return EnumerateTranscriptsIn(ClaudeProjectsDir(), sinceUnixMs);
    }

    // ===== pure line-level primitives ========================================================

    int64_t ParseTranscriptTimestamp(std::wstring_view ts)
    {
        if (ts.empty())
        {
            return 0;
        }
        // Pure-digit epoch (oldest strata): >= 12 digits == ms, 9-11 == seconds.
        bool digits = true;
        for (const wchar_t c : ts)
        {
            if (c < L'0' || c > L'9')
            {
                digits = false;
                break;
            }
        }
        if (digits)
        {
            if (ts.size() >= 12 && ts.size() <= 17)
            {
                return static_cast<int64_t>(::wcstoll(std::wstring{ ts }.c_str(), nullptr, 10));
            }
            if (ts.size() >= 9 && ts.size() <= 11)
            {
                return static_cast<int64_t>(::wcstoll(std::wstring{ ts }.c_str(), nullptr, 10)) * 1000;
            }
            return 0;
        }
        // ISO-8601: YYYY-MM-DDTHH:MM:SS[.fff...][Z] (every recent transcript line is ISO-Z).
        if (ts.size() < 19 || ts[4] != L'-' || ts[7] != L'-' || (ts[10] != L'T' && ts[10] != L' ') || ts[13] != L':' || ts[16] != L':')
        {
            return 0;
        }
        const auto num = [&ts](size_t off, size_t len) -> int {
            int v = 0;
            for (size_t i = off; i < off + len; ++i)
            {
                if (ts[i] < L'0' || ts[i] > L'9')
                {
                    return -1;
                }
                v = v * 10 + (ts[i] - L'0');
            }
            return v;
        };
        const int Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), m = num(14, 2), s = num(17, 2);
        if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60)
        {
            return 0;
        }
        struct tm tmv
        {
        };
        tmv.tm_year = Y - 1900;
        tmv.tm_mon = M - 1;
        tmv.tm_mday = D;
        tmv.tm_hour = h;
        tmv.tm_min = m;
        tmv.tm_sec = s;
        const __time64_t secs = ::_mkgmtime64(&tmv);
        if (secs < 0)
        {
            return 0;
        }
        int64_t ms = 0;
        if (ts.size() > 19 && ts[19] == L'.')
        {
            int frac = 0, n = 0;
            for (size_t i = 20; i < ts.size() && n < 3 && ts[i] >= L'0' && ts[i] <= L'9'; ++i, ++n)
            {
                frac = frac * 10 + (ts[i] - L'0');
            }
            while (n > 0 && n < 3)
            {
                frac *= 10;
                ++n;
            }
            ms = frac;
        }
        return static_cast<int64_t>(secs) * 1000 + ms;
    }

    bool IsNoiseUserPrompt(std::wstring_view prompt)
    {
        size_t i = 0;
        while (i < prompt.size() && (prompt[i] == L' ' || prompt[i] == L'\t' || prompt[i] == L'\n' || prompt[i] == L'\r'))
        {
            ++i;
        }
        const std::wstring_view p = prompt.substr(i);
        static constexpr std::wstring_view kNoise[] = {
            L"<command-name>", // slash-command echo
            L"<command-message>", // slash-command echo (message form)
            L"<local-command", // local command stdout/caveat wrappers
            L"<bash-input>", // ! bash passthrough echo
            L"<bash-stdout>",
            L"<bash-stderr>",
            L"<task-notification>", // background-task completion notification (post-2.1 schema)
            L"<system-reminder", // injected reminder context
            L"<teammate-message", // agent-team traffic (the bare wrapper)
            L"Another Claude session sent a message:", // ...and the wrapper's REAL on-disk shape: Claude
            // Code prefixes the <teammate-message> block with this line, so the bare-wrapper prefix above
            // never fires on actual traffic. A title / prompt-list is the USER's framing, so NO teammate
            // message belongs there (idle_notification protocol AND a delivered report alike) — none is a
            // human prompt. (The SUMMARY filter SeIsCommandNoise is narrower: it keeps a real teammate
            // report and drops only the JSON protocol envelope.)
            L"Caveat:", // the injected caveat preamble
            L"[Request interrupted", // Esc-interrupt control markers (both variants)
        };
        for (const auto n : kNoise)
        {
            if (p.size() >= n.size() && p.substr(0, n.size()) == n)
            {
                return true;
            }
        }
        return false;
    }

    std::wstring PickDisplayTitle(std::wstring_view customTitle, std::wstring_view aiTitle, std::wstring_view summary, std::wstring_view firstPrompt)
    {
        if (!customTitle.empty())
        {
            return std::wstring{ customTitle };
        }
        if (!aiTitle.empty())
        {
            return std::wstring{ aiTitle };
        }
        if (!summary.empty())
        {
            return std::wstring{ summary };
        }
        return std::wstring{ firstPrompt };
    }

    TranscriptLineFacts ClassifyTranscriptLine(std::wstring_view line, size_t maxUserTextChars, size_t maxAgentTextChars)
    {
        TranscriptLineFacts f;
        const auto parsed = json::Parse(line);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return f; // not a JSON object line — tolerate anything (Other)
        }
        const auto& obj = *parsed;

        // Envelope facts present on most line shapes.
        if (const auto* ts = obj.Find(L"timestamp"))
        {
            if (ts->type == json::Value::Type::Str)
            {
                f.timestampMs = ParseTranscriptTimestamp(ts->str);
            }
            else if (ts->type == json::Value::Type::Num)
            {
                f.timestampMs = EpochNumToMs(ts->num); // oldest strata carried epoch numbers
            }
        }
        f.uuid = obj.StrAt(L"uuid"); // the tree-node id (empty on state/marker lines) — feeds onActiveBranch
        f.sidechain = obj.BoolAt(L"isSidechain");
        f.cwd = obj.StrAt(L"cwd");
        f.gitBranch = obj.StrAt(L"gitBranch");
        if (const auto* fk = obj.Find(L"forkedFrom"); fk && fk->type == json::Value::Type::Obj)
        {
            f.forkedFromId = fk->StrAt(L"sessionId");
        }

        const std::wstring type = obj.StrAt(L"type");
        if (type == L"user")
        {
            f.meta = obj.BoolAt(L"isMeta") || obj.BoolAt(L"isCompactSummary");
            const auto* msg = obj.Find(L"message");
            const auto* content = (msg && msg->type == json::Value::Type::Obj) ? msg->Find(L"content") : nullptr;
            bool toolResult = false;
            std::wstring prompt;
            if (content && content->type == json::Value::Type::Str)
            {
                prompt = content->str;
            }
            else if (content && content->type == json::Value::Type::Arr)
            {
                for (const auto& blk : content->arr)
                {
                    if (blk.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    const std::wstring bt = blk.StrAt(L"type");
                    if (bt == L"tool_result")
                    {
                        toolResult = true;
                        // The tool's output is AGENT-side searchable text (the 🤖 scope).
                        const auto* rc = blk.Find(L"content");
                        if (rc && rc->type == json::Value::Type::Str)
                        {
                            AppendCapped(f.agentText, rc->str, maxAgentTextChars);
                        }
                        else if (rc && rc->type == json::Value::Type::Arr)
                        {
                            for (const auto& rb : rc->arr)
                            {
                                if (rb.type == json::Value::Type::Obj && rb.StrAt(L"type") == L"text")
                                {
                                    AppendCapped(f.agentText, rb.StrAt(L"text"), maxAgentTextChars);
                                }
                            }
                        }
                    }
                    else if (bt == L"text")
                    {
                        prompt += blk.StrAt(L"text");
                    }
                }
            }
            if (toolResult)
            {
                f.kind = TranscriptLineKind::UserToolResult;
                // The structured per-tool mirror (stdout/stderr/content) rides the same line.
                if (const auto* tur = obj.Find(L"toolUseResult"))
                {
                    if (tur->type == json::Value::Type::Str)
                    {
                        AppendCapped(f.agentText, tur->str, maxAgentTextChars);
                    }
                    else if (tur->type == json::Value::Type::Obj)
                    {
                        for (const auto key : { L"stdout", L"stderr", L"content" })
                        {
                            if (const auto* v = tur->Find(key); v && v->type == json::Value::Type::Str)
                            {
                                AppendCapped(f.agentText, v->str, maxAgentTextChars);
                            }
                        }
                    }
                }
            }
            else
            {
                f.kind = TranscriptLineKind::UserPrompt;
                if (!f.meta && IsNoiseUserPrompt(prompt))
                {
                    f.meta = true; // a control marker, not a human message
                }
                if (!f.meta && !f.sidechain && !prompt.empty())
                {
                    AppendCapped(f.userText, prompt, maxUserTextChars);
                }
            }
        }
        else if (type == L"assistant")
        {
            f.kind = TranscriptLineKind::Assistant;
            const auto* msg = obj.Find(L"message");
            // Context occupancy from message.usage (input + cache_creation + cache_read + output) ≈
            // the size of the request that produced this line; cache_read carries the whole
            // conversation forward, so one assistant block reflects the current context size. The
            // NEWEST assistant line wins downstream (AccumulateTranscriptStats). Mirrors the scanner's
            // ParseTranscriptDelta (SessionScanner.cpp) so the board card + this on-disk index agree.
            if (msg && msg->type == json::Value::Type::Obj)
            {
                if (const auto* usage = msg->Find(L"usage"); usage && usage->type == json::Value::Type::Obj)
                {
                    f.contextTokens = usage->I64At(L"input_tokens") +
                                      usage->I64At(L"cache_creation_input_tokens") +
                                      usage->I64At(L"cache_read_input_tokens") +
                                      usage->I64At(L"output_tokens");
                }
            }
            const auto* content = (msg && msg->type == json::Value::Type::Obj) ? msg->Find(L"content") : nullptr;
            if (content && content->type == json::Value::Type::Arr)
            {
                for (const auto& blk : content->arr)
                {
                    if (blk.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    const std::wstring bt = blk.StrAt(L"type");
                    if (bt == L"text")
                    {
                        AppendCapped(f.agentText, blk.StrAt(L"text"), maxAgentTextChars);
                    }
                    else if (bt == L"thinking")
                    {
                        AppendCapped(f.agentText, blk.StrAt(L"thinking"), maxAgentTextChars);
                    }
                    else if (bt == L"tool_use")
                    {
                        ++f.toolUses;
                        AppendCapped(f.agentText, blk.StrAt(L"name"), maxAgentTextChars);
                        if (const auto* in = blk.Find(L"input"); in && in->type == json::Value::Type::Obj)
                        {
                            if (maxAgentTextChars != 0 && f.agentText.size() < maxAgentTextChars)
                            {
                                AppendCapped(f.agentText, json::Dump(*in), maxAgentTextChars);
                            }
                            // Tool-touched paths (the 📁/📄 search scopes): the canonical path
                            // keys across Read/Edit/Write/NotebookEdit/Grep/Glob inputs.
                            for (const auto key : { L"file_path", L"notebook_path", L"path" })
                            {
                                if (f.toolPaths.size() >= 8)
                                {
                                    break;
                                }
                                const std::wstring p = in->StrAt(key);
                                if (!p.empty() && p.size() <= 512)
                                {
                                    f.toolPaths.push_back(p);
                                }
                            }
                        }
                    }
                }
            }
            else if (content && content->type == json::Value::Type::Str)
            {
                AppendCapped(f.agentText, content->str, maxAgentTextChars); // oldest strata: plain string content
            }
        }
        else if (type == L"system")
        {
            f.kind = TranscriptLineKind::System;
            AppendCapped(f.agentText, obj.StrAt(L"content"), maxAgentTextChars); // away_summary / api_error / local_command bodies
        }
        else if (type == L"attachment")
        {
            f.kind = TranscriptLineKind::Attachment; // payload deliberately NOT extracted (injected context)
        }
        else if (type == L"custom-title")
        {
            f.kind = TranscriptLineKind::CustomTitle;
            f.title = FirstLineTrim(obj.StrAt(L"customTitle"));
        }
        else if (type == L"ai-title")
        {
            f.kind = TranscriptLineKind::AiTitle;
            f.title = FirstLineTrim(obj.StrAt(L"aiTitle"));
        }
        else if (type == L"summary")
        {
            f.kind = TranscriptLineKind::Summary;
            f.title = FirstLineTrim(obj.StrAt(L"summary"));
        }
        else if (type == L"queue-operation")
        {
            f.kind = TranscriptLineKind::QueueOperation;
        }
        else if (type == L"file-history-snapshot")
        {
            f.kind = TranscriptLineKind::FileHistorySnapshot;
        }
        return f;
    }

    // ===== active-branch (revert-aware) reconstruction =======================================

    std::unordered_set<std::wstring> ActiveBranchUuids(std::wstring_view transcriptText)
    {
        // Pass 1: index every tree node's uuid -> parentUuid edge, and track the LAST `leafUuid`
        // marker (file order => the authoritative current leaf). EVERY uuid-bearing line is a node,
        // INCLUDING attachment lines (skill_listing / file snippets injected mid-turn) — they are
        // pass-through hops in the chain, so omitting them would break the walk at the first one.
        std::unordered_map<std::wstring, std::wstring> parentOf;
        std::wstring leaf;
        size_t start = 0;
        for (size_t i = 0; i <= transcriptText.size(); ++i)
        {
            if (i < transcriptText.size() && transcriptText[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line = transcriptText.substr(start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto& obj = *parsed;
            if (const std::wstring lu = obj.StrAt(L"leafUuid"); !lu.empty())
            {
                leaf = lu; // a `last-prompt` (or bare leafUuid) line advances the current leaf
            }
            if (const std::wstring u = obj.StrAt(L"uuid"); !u.empty())
            {
                // parentUuid "" (StrAt returns "" for a JSON null/absent) == a conversation ROOT.
                parentOf[u] = obj.StrAt(L"parentUuid");
            }
        }

        std::unordered_set<std::wstring> active;
        // No leaf marker, OR the leaf names a node that isn't present (a truncated read, or a
        // stratum that never wrote markers) => return EMPTY so the caller keeps every line. NEVER
        // return a partial set built from an unknown leaf — that would wrongly orphan the whole file.
        if (leaf.empty() || parentOf.find(leaf) == parentOf.end())
        {
            return active;
        }
        // Pass 2: walk leaf -> root via parentUuid. The `active.insert().second` test is the cycle
        // guard (a malformed self/loop reference stops instead of spinning); a parent that isn't a
        // known node terminates the walk (the chain reached the root, whose parentUuid is "").
        std::wstring cur = leaf;
        while (!cur.empty() && active.insert(cur).second)
        {
            const auto it = parentOf.find(cur);
            if (it == parentOf.end())
            {
                break;
            }
            cur = it->second;
        }
        return active;
    }

    std::vector<TranscriptLineFacts> ClassifyTranscriptLines(std::wstring_view transcriptText, size_t maxUserTextChars, size_t maxAgentTextChars, bool markActiveBranch)
    {
        // The active-branch set comes from the WHOLE text (the trailing leaf marker + the full
        // uuid->parent map). markActiveBranch=false (a partial / HEAD read, whose tail marker is absent
        // and whose in-window markers would be STALE) => empty set => every message stays active.
        const std::unordered_set<std::wstring> active = markActiveBranch ? ActiveBranchUuids(transcriptText) : std::unordered_set<std::wstring>{};
        std::vector<TranscriptLineFacts> out;
        size_t start = 0;
        for (size_t i = 0; i <= transcriptText.size(); ++i)
        {
            if (i < transcriptText.size() && transcriptText[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line = transcriptText.substr(start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            TranscriptLineFacts f = ClassifyTranscriptLine(line, maxUserTextChars, maxAgentTextChars);
            // A uuid-bearing node absent from the active set sits on a rewound-away branch. uuid-less
            // state/marker lines, and the keep-all case (empty set), stay onActiveBranch==true.
            if (!active.empty() && !f.uuid.empty() && active.count(f.uuid) == 0)
            {
                f.onActiveBranch = false;
            }
            out.push_back(std::move(f));
        }
        return out;
    }

    // ===== streaming scan + stats ============================================================

    int64_t ScanTranscript(const std::wstring& path, int64_t fromOffset, size_t maxUserTextChars, size_t maxAgentTextChars, const TranscriptLineSink& sink)
    {
        const HANDLE h = OpenShared(path);
        if (h == INVALID_HANDLE_VALUE)
        {
            return -1;
        }
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        const int64_t size = sz.QuadPart;
        int64_t offset = std::max<int64_t>(0, fromOffset);
        if (offset >= size)
        {
            ::CloseHandle(h);
            return size; // nothing new (or the caller's cursor is past a replaced file — they re-key on size)
        }
        LARGE_INTEGER li{};
        li.QuadPart = offset;
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        {
            ::CloseHandle(h);
            return -1;
        }

        std::string carry;
        for (;;)
        {
            std::string chunk(kScanChunkBytes, '\0');
            DWORD got = 0;
            if (!::ReadFile(h, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0)
            {
                break;
            }
            chunk.resize(got);
            carry += chunk;

            const size_t consumed = ForEachLine(carry, false, [&](std::string_view lineBytes) {
                const std::wstring wide = Utf8ToWide(lineBytes.data(), lineBytes.size());
                if (!wide.empty() && sink)
                {
                    sink(ClassifyTranscriptLine(wide, maxUserTextChars, maxAgentTextChars));
                }
            });
            if (consumed > 0)
            {
                offset += static_cast<int64_t>(consumed);
                carry.erase(0, consumed);
            }
            else if (carry.size() > kForceConsumeBytes)
            {
                // A pathological unterminated run: skip it rather than carrying it forever.
                offset += static_cast<int64_t>(carry.size());
                carry.clear();
            }
        }
        ::CloseHandle(h);
        return offset; // a trailing partial line (an append in flight) stays unconsumed
    }

    bool AccumulateTranscriptStats(const std::wstring& path, TranscriptStats& stats)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            stats.found = false; // swept/vanished mid-listing — the caller drops the row
            return false;
        }
        const int64_t size = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | static_cast<int64_t>(fad.nFileSizeLow);
        if (size < stats.parsedBytes)
        {
            stats = {}; // shrank => replaced wholesale => rebuild from 0 (append-only never shrinks)
        }
        if (size == stats.parsedBytes)
        {
            stats.found = true;
            return true; // up to date — the (size,mtime)-keyed fast path
        }

        const int64_t newOffset = ScanTranscript(path, stats.parsedBytes, 2048 /*first-prompt budget*/, 0 /*no agent text for stats*/, [&stats](const TranscriptLineFacts& f) {
            if (f.timestampMs != 0 && stats.firstTimestampMs == 0)
            {
                stats.firstTimestampMs = f.timestampMs;
            }
            if (!f.sidechain && f.timestampMs != 0 &&
                (f.kind == TranscriptLineKind::UserPrompt || f.kind == TranscriptLineKind::UserToolResult || f.kind == TranscriptLineKind::Assistant))
            {
                stats.lastTimestampMs = std::max(stats.lastTimestampMs, f.timestampMs); // max, not last-seen: a fork's copied tail carries OLD stamps
            }
            if (f.kind == TranscriptLineKind::UserPrompt && !f.meta && !f.sidechain && !f.userText.empty())
            {
                ++stats.userPrompts;
                if (stats.firstUserPrompt.empty())
                {
                    stats.firstUserPrompt = FirstLineTrim(f.userText);
                }
            }
            if (f.kind == TranscriptLineKind::Assistant && !f.sidechain)
            {
                ++stats.assistantLines;
                stats.toolUses += f.toolUses;
                // Context occupancy: the NEWEST assistant usage wins. The forward scan means the
                // last assistant line with a usage block survives — i.e. the current context size.
                // A usage-less line (oldest strata) leaves the prior value intact. (Subagent lines
                // are sidechain-filtered above, so this never jumps to a subagent's context.)
                if (f.contextTokens > 0)
                {
                    stats.contextTokens = f.contextTokens;
                }
            }
            // Deduped union of tool-touched paths (case-insensitive — Windows paths), capped.
            for (const auto& p : f.toolPaths)
            {
                if (stats.pathsAccessed.size() >= kMaxPathsAccessed)
                {
                    break;
                }
                bool seen = false;
                for (const auto& have : stats.pathsAccessed)
                {
                    if (have.size() == p.size() && ::_wcsicmp(have.c_str(), p.c_str()) == 0)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    stats.pathsAccessed.push_back(p);
                }
            }
            if (!f.title.empty())
            {
                switch (f.kind)
                {
                case TranscriptLineKind::CustomTitle:
                    stats.customTitle = f.title; // the LAST one wins (a retitle appends)
                    break;
                case TranscriptLineKind::AiTitle:
                    stats.aiTitle = f.title;
                    break;
                case TranscriptLineKind::Summary:
                    stats.summary = f.title;
                    break;
                default:
                    break;
                }
            }
            if (stats.forkedFromId.empty() && !f.forkedFromId.empty())
            {
                stats.forkedFromId = f.forkedFromId;
            }
            if (stats.cwd.empty() && !f.cwd.empty())
            {
                stats.cwd = f.cwd;
            }
            if (stats.gitBranch.empty() && !f.gitBranch.empty())
            {
                stats.gitBranch = f.gitBranch;
            }
        });
        if (newOffset < 0)
        {
            stats.found = false;
            return false;
        }
        stats.parsedBytes = newOffset;
        stats.found = true;
        return true;
    }

    // ===== cheap row facts (head + growing tail) =============================================

    TranscriptQuickFacts ReadTranscriptQuickFacts(const std::wstring& path, int64_t fileBirthMs)
    {
        TranscriptQuickFacts q;
        const HANDLE h = OpenShared(path);
        if (h == INVALID_HANDLE_VALUE)
        {
            return q;
        }
        q.found = true;
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        const int64_t size = sz.QuadPart;

        // HEAD: first timestamped line (created) + cwd + fork stamp. The prefix is tiny state
        // lines, but a giant first file-history-snapshot can overflow a window — grow the ladder
        // until a timestamp lands (or the cap).
        int64_t firstTs = 0;
        for (const int64_t w : kWindowLadder)
        {
            const int64_t want = std::min<int64_t>(w, size);
            const std::string bytes = ReadAt(h, 0, static_cast<size_t>(want));
            const bool wholeFile = want >= size;
            firstTs = 0; // re-derive over the (re-read) wider window
            ForEachLine(bytes, wholeFile, [&](std::string_view lineBytes) {
                if (firstTs != 0 && q.fork && !q.cwd.empty())
                {
                    return; // everything harvested
                }
                const std::wstring wide = Utf8ToWide(lineBytes.data(), lineBytes.size());
                const auto f = ClassifyTranscriptLine(wide, 0, 0);
                if (f.timestampMs != 0 && firstTs == 0)
                {
                    firstTs = f.timestampMs;
                }
                if (!f.forkedFromId.empty() && !q.fork)
                {
                    q.fork = true;
                    q.forkedFromId = f.forkedFromId;
                }
                if (q.cwd.empty() && !f.cwd.empty())
                {
                    q.cwd = f.cwd;
                }
            });
            if (firstTs != 0 || wholeFile)
            {
                break;
            }
        }

        // TAIL: the last non-sidechain user/assistant timestamp == the real last activity. The
        // literal tail is usually untimestamped state lines (and single lines exceed 1 MiB), so
        // grow the window until a message lands or the cap is hit.
        for (const int64_t w : kWindowLadder)
        {
            const int64_t want = std::min<int64_t>(w, size);
            const int64_t at = size - want;
            std::string bytes = ReadAt(h, at, static_cast<size_t>(want));
            size_t begin = 0;
            if (at > 0)
            {
                const size_t nl = bytes.find('\n');
                if (nl == std::string::npos)
                {
                    continue; // one giant partial line — widen
                }
                begin = nl + 1; // drop the partial first segment
            }
            int64_t last = 0;
            ForEachLine(std::string_view{ bytes }.substr(begin), true, [&](std::string_view lineBytes) {
                const std::wstring wide = Utf8ToWide(lineBytes.data(), lineBytes.size());
                const auto f = ClassifyTranscriptLine(wide, 0, 0);
                if (!f.sidechain && f.timestampMs != 0 &&
                    (f.kind == TranscriptLineKind::UserPrompt || f.kind == TranscriptLineKind::UserToolResult || f.kind == TranscriptLineKind::Assistant))
                {
                    last = std::max(last, f.timestampMs);
                }
            });
            if (last != 0 || want >= size)
            {
                q.lastActivityMs = last;
                break;
            }
        }

        // CREATION TIME (resume-hardened). Claude REWRITES the whole transcript on `--resume`, which
        // resets the file's creation time (ctime/birth) to "now" — verified live: a session whose
        // messages are all from day 1 reports a birth that marches forward on every resume. So
        // fileBirthMs is unreliable: it can land AFTER the session's own newest message. A session
        // cannot be born after its last activity, so clamp a "future" birth back to the last real
        // activity. This keeps a resumed session's created time STABLE instead of drifting forward each
        // resume — which otherwise corrupts created-time sort/display and can spoof a continuation edge
        // (an old session made to look freshly created lands inside a later session's gap window).
        // Healthy transcripts (birth <= lastActivity) are unaffected; this is a no-op for them.
        int64_t birthMs = fileBirthMs;
        if (q.lastActivityMs > 0 && birthMs > q.lastActivityMs)
        {
            birthMs = q.lastActivityMs;
        }
        // A fork's copied line timestamps predate the file (they're the parent's verbatim), so its true
        // creation is the file birth (clamped above). A normal session's first timestamped line IS its
        // creation; a prefix-only (never-prompted) file keeps firstTs==0 so the CALLER falls back to
        // birth/mtime (the established contract — do not fill it in here).
        q.createdMs = q.fork ? birthMs : firstTs;

        ::CloseHandle(h);
        return q;
    }

    // ===== live-session presence =============================================================

    std::vector<SessionPresenceRow> ReadSessionPresenceIn(std::wstring_view sessionsDir)
    {
        std::vector<SessionPresenceRow> out;
        if (sessionsDir.empty())
        {
            return out;
        }
        const std::wstring root{ sessionsDir };
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW((root + L"\\*.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            const std::string bytes = ReadFileWhole(root + L"\\" + fd.cFileName, 64 << 10);
            if (bytes.empty())
            {
                continue;
            }
            const std::wstring wide = Utf8ToWide(bytes.data(), bytes.size());
            const auto parsed = json::Parse(wide);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue;
            }
            SessionPresenceRow r;
            r.pid = parsed->U32At(L"pid");
            r.sessionId = parsed->StrAt(L"sessionId");
            r.cwd = parsed->StrAt(L"cwd");
            r.status = parsed->StrAt(L"status");
            r.version = parsed->StrAt(L"version");
            r.startedAtMs = parsed->I64At(L"startedAt");
            r.updatedAtMs = parsed->I64At(L"updatedAt");
            if (r.pid != 0 && !r.sessionId.empty())
            {
                out.push_back(std::move(r));
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return out;
    }

    std::vector<SessionPresenceRow> ReadSessionPresence()
    {
        // The presence dir is `<claude home>\sessions`, the sibling of `<claude home>\projects`.
        std::wstring projects = ClaudeProjectsDir();
        while (!projects.empty() && (projects.back() == L'\\' || projects.back() == L'/'))
        {
            projects.pop_back();
        }
        const size_t cut = projects.find_last_of(L"\\/");
        if (cut == std::wstring::npos)
        {
            return {};
        }
        return ReadSessionPresenceIn(projects.substr(0, cut) + L"\\sessions");
    }

    // ===== the per-session sidecar index =====================================================

    SessionIndexEntry LoadOrRefreshSessionIndexIn(const std::wstring& indexDir, const TranscriptRef& ref)
    {
        SessionIndexEntry e;
        e.sessionId = ref.sessionId;
        e.path = ref.path;
        e.birthMs = ref.birthMs;
        if (indexDir.empty() || ref.sessionId.empty())
        {
            return e;
        }
        const std::wstring sidecar = indexDir + L"\\" + ref.sessionId + L".json";

        // Load the prior sidecar (if any) — its (size, mtime) is the invalidation key, its
        // stats.parsedBytes the resume cursor.
        int64_t cachedSize = -1, cachedMtime = -1;
        int64_t cachedCtx = -1; // ctxTokens read from the sidecar; -1 == the key was ABSENT (a pre-contextTokens sidecar — see the backfill below)
        {
            const std::string bytes = ReadFileWhole(sidecar, 4 << 20);
            if (!bytes.empty())
            {
                const std::wstring wide = Utf8ToWide(bytes.data(), bytes.size());
                const auto parsed = json::Parse(wide);
                if (parsed && parsed->type == json::Value::Type::Obj && parsed->StrAt(L"sid") == ref.sessionId)
                {
                    const auto& o = *parsed;
                    cachedSize = o.I64At(L"size", -1);
                    cachedMtime = o.I64At(L"mtime", -1);
                    e.stats.parsedBytes = o.I64At(L"parsedBytes");
                    e.stats.firstTimestampMs = o.I64At(L"firstTs");
                    e.stats.lastTimestampMs = o.I64At(L"lastTs");
                    e.stats.userPrompts = static_cast<int>(o.I64At(L"userPrompts"));
                    e.stats.assistantLines = static_cast<int>(o.I64At(L"assistantLines"));
                    e.stats.toolUses = static_cast<int>(o.I64At(L"toolUses"));
                    e.stats.customTitle = o.StrAt(L"customTitle");
                    e.stats.aiTitle = o.StrAt(L"aiTitle");
                    e.stats.summary = o.StrAt(L"summary");
                    e.stats.firstUserPrompt = o.StrAt(L"firstUserPrompt");
                    e.stats.forkedFromId = o.StrAt(L"forkedFromId");
                    e.stats.cwd = o.StrAt(L"cwd");
                    e.stats.gitBranch = o.StrAt(L"gitBranch");
                    cachedCtx = o.I64At(L"ctxTokens", -1); // -1 == absent (an old sidecar): triggers the one-time backfill below
                    e.stats.contextTokens = cachedCtx < 0 ? 0 : cachedCtx;
                    if (const auto* paths = o.Find(L"paths"); paths && paths->type == json::Value::Type::Arr)
                    {
                        for (const auto& p : paths->arr)
                        {
                            if (p.type == json::Value::Type::Str && e.stats.pathsAccessed.size() < kMaxPathsAccessed)
                            {
                                e.stats.pathsAccessed.push_back(p.str);
                            }
                        }
                    }
                    e.stats.found = true;
                }
            }
        }
        // One-time migration: a sidecar written BEFORE contextTokens existed lacks the `ctxTokens`
        // key (cachedCtx == -1). If the session has assistant turns there's a real context value to
        // recover, so force a single rebuild-from-0 to fill it; the rewritten sidecar then carries
        // the key (>= 0) and never re-triggers. A genuinely usage-less session writes ctxTokens=0 and
        // is a normal cache hit next time. (Same one-time full read the index already paid when first
        // built — paid once more, only for in-window sessions, only on the first open after this ships.)
        const bool needsCtxBackfill = cachedCtx < 0 && e.stats.assistantLines > 0;
        if (cachedSize == ref.sizeBytes && cachedMtime == ref.mtimeMs && e.stats.found && !needsCtxBackfill)
        {
            e.sizeBytes = cachedSize;
            e.mtimeMs = cachedMtime;
            e.valid = true;
            return e; // cache hit: the sidecar IS current — zero transcript IO
        }
        if (needsCtxBackfill)
        {
            e.stats = {}; // rebuild from offset 0 so contextTokens is filled (every other field recomputes identically)
        }

        // Stale / absent: resume the accumulate (only the appended suffix is read; a shrink
        // auto-rebuilds inside AccumulateTranscriptStats) and rewrite the sidecar.
        if (!AccumulateTranscriptStats(ref.path, e.stats))
        {
            e.valid = false; // transcript swept mid-listing
            return e;
        }
        e.sizeBytes = ref.sizeBytes;
        e.mtimeMs = ref.mtimeMs;
        e.valid = true;

        json::Value o = json::Value::MkObj();
        o.Set(L"sid", json::Value::MkStr(ref.sessionId));
        o.Set(L"path", json::Value::MkStr(ref.path));
        o.Set(L"size", json::Value::MkNum(static_cast<double>(ref.sizeBytes)));
        o.Set(L"mtime", json::Value::MkNum(static_cast<double>(ref.mtimeMs)));
        o.Set(L"birth", json::Value::MkNum(static_cast<double>(ref.birthMs)));
        o.Set(L"parsedBytes", json::Value::MkNum(static_cast<double>(e.stats.parsedBytes)));
        o.Set(L"firstTs", json::Value::MkNum(static_cast<double>(e.stats.firstTimestampMs)));
        o.Set(L"lastTs", json::Value::MkNum(static_cast<double>(e.stats.lastTimestampMs)));
        o.Set(L"userPrompts", json::Value::MkNum(e.stats.userPrompts));
        o.Set(L"assistantLines", json::Value::MkNum(e.stats.assistantLines));
        o.Set(L"toolUses", json::Value::MkNum(e.stats.toolUses));
        o.Set(L"ctxTokens", json::Value::MkNum(static_cast<double>(e.stats.contextTokens))); // context occupancy (≤ a few M — exact in a double)
        o.Set(L"customTitle", json::Value::MkStr(e.stats.customTitle));
        o.Set(L"aiTitle", json::Value::MkStr(e.stats.aiTitle));
        o.Set(L"summary", json::Value::MkStr(e.stats.summary));
        o.Set(L"firstUserPrompt", json::Value::MkStr(e.stats.firstUserPrompt));
        o.Set(L"forkedFromId", json::Value::MkStr(e.stats.forkedFromId));
        o.Set(L"cwd", json::Value::MkStr(e.stats.cwd));
        o.Set(L"gitBranch", json::Value::MkStr(e.stats.gitBranch));
        json::Value paths = json::Value::MkArr();
        for (const auto& p : e.stats.pathsAccessed)
        {
            paths.Push(json::Value::MkStr(p));
        }
        o.Set(L"paths", std::move(paths));
        WriteFileUtf8(sidecar, json::Dump(o));
        return e;
    }

    SessionIndexEntry LoadOrRefreshSessionIndex(const TranscriptRef& ref)
    {
        const std::wstring dir = AgentmasterStateDir() + L"\\sessions-index";
        ::CreateDirectoryW(dir.c_str(), nullptr); // idempotent; parent exists (the state dir)
        return LoadOrRefreshSessionIndexIn(dir, ref);
    }

    // ===== continuation-chain lineage ========================================================

    // Pure single-hop continuation edge: the DIRECT successor of `cur` among `nodes` (the B in
    // cur->B), or nullptr. `visited` excludes ids already on a walked chain (pass an empty set for a
    // standalone query). Edge: B same cwd (NormDirKey), !B.fork, B created in [cur.lastActivity - skew,
    // cur.lastActivity + gap], B the EARLIEST such; BAILS (nullptr) on AMBIGUITY — a 2nd candidate
    // whose lifetime overlaps B (parallel same-dir sessions), Rule #14 spirit. Factored out of
    // ResolveContinuationChainTail so the REVERSE resolver (ResolveContinuationPredecessor) is the
    // EXACT inverse of the forward edge and the two can never drift. [Agentmaster]
    static const SessionChainNode* ContinuationNext(const std::vector<SessionChainNode>& nodes,
                                                    const SessionChainNode* cur,
                                                    const std::unordered_set<std::wstring>& visited,
                                                    int64_t gapMaxMs,
                                                    int64_t skewMs)
    {
        const std::wstring curKey = NormDirKey(cur->cwd);
        // Candidates that could continue `cur`: same cwd, not a fork, unvisited, and created at/after
        // cur ended (within skew). The chain successor is the EARLIEST of these.
        std::vector<const SessionChainNode*> cands;
        for (const auto& n : nodes)
        {
            if (n.sessionId == cur->sessionId || n.fork || visited.count(n.sessionId))
            {
                continue;
            }
            if (n.createdMs < cur->lastActivityMs - skewMs)
            {
                continue; // started before cur finished — a parallel/earlier session, not a continuation
            }
            if (NormDirKey(n.cwd) != curKey)
            {
                continue;
            }
            cands.push_back(&n);
        }
        if (cands.empty())
        {
            return nullptr;
        }
        std::sort(cands.begin(), cands.end(), [](const SessionChainNode* a, const SessionChainNode* b) {
            if (a->createdMs != b->createdMs)
            {
                return a->createdMs < b->createdMs;
            }
            return a->sessionId < b->sessionId; // deterministic tiebreak
        });
        const SessionChainNode* next = cands.front();
        if (next->createdMs - cur->lastActivityMs > gapMaxMs)
        {
            return nullptr; // the next session starts too long after — a NEW conversation, not a continuation
        }
        // Ambiguity guard: if a SECOND candidate's lifetime overlaps `next` (it started at/before
        // `next`'s last activity), two same-dir sessions ran in parallel — we can't say which one
        // continues `cur`, so stop here rather than silently pick one (Rule #14 spirit).
        if (cands.size() >= 2 && cands[1]->createdMs <= next->lastActivityMs)
        {
            return nullptr;
        }
        return next;
    }

    std::wstring ResolveContinuationPredecessor(const std::vector<SessionChainNode>& nodes,
                                                const std::wstring& targetId,
                                                int64_t gapMaxMs,
                                                int64_t skewMs)
    {
        if (targetId.empty())
        {
            return {};
        }
        static const std::unordered_set<std::wstring> kNoVisited;
        std::wstring pred;
        int count = 0;
        for (const auto& a : nodes)
        {
            if (a.sessionId == targetId)
            {
                continue;
            }
            const SessionChainNode* nx = ContinuationNext(nodes, &a, kNoVisited, gapMaxMs, skewMs);
            if (nx && nx->sessionId == targetId)
            {
                if (++count > 1)
                {
                    return {}; // two sessions continue into the target — ambiguous, don't guess
                }
                pred = a.sessionId;
            }
        }
        return pred;
    }

    SessionChainResult ResolveContinuationChainTail(const std::vector<SessionChainNode>& nodes,
                                                    const std::wstring& startId,
                                                    int64_t gapMaxMs,
                                                    int64_t skewMs)
    {
        SessionChainResult res{ startId, 0 };
        if (startId.empty() || nodes.empty())
        {
            return res;
        }
        const auto findNode = [&nodes](const std::wstring& id) -> const SessionChainNode* {
            for (const auto& n : nodes)
            {
                if (n.sessionId == id)
                {
                    return &n;
                }
            }
            return nullptr;
        };
        const SessionChainNode* cur = findNode(startId);
        if (!cur)
        {
            return res; // the clicked id isn't in this dir — don't redirect
        }

        std::unordered_set<std::wstring> visited;
        visited.insert(cur->sessionId);
        constexpr int kMaxHops = 64; // a chain this long never happens; a hard backstop vs. a pathological loop
        while (res.hops < kMaxHops)
        {
            const SessionChainNode* next = ContinuationNext(nodes, cur, visited, gapMaxMs, skewMs);
            if (!next)
            {
                break; // no further continuation edge (none, too-late, or ambiguous — see ContinuationNext)
            }
            cur = next;
            visited.insert(cur->sessionId);
            res.tailId = cur->sessionId;
            ++res.hops;
        }
        return res;
    }

    // Filesystem: the <uuid>.jsonl refs DIRECTLY in `cwd`'s project dir (<claude>/projects/<encoded>).
    // The shared enumeration behind both continuation resolvers (tail + predecessor). Scans the files
    // at projDir's TOP (EnumerateTranscriptsIn walks the SUBDIRS of a root — wrong level here). Empty
    // when the projects root / dir is unreadable or has no transcripts. [Agentmaster]
    static std::vector<TranscriptRef> EnumerateProjectDirRefs(const std::wstring& cwd)
    {
        std::vector<TranscriptRef> refs;
        std::wstring projRoot = ClaudeProjectsDir();
        while (!projRoot.empty() && (projRoot.back() == L'\\' || projRoot.back() == L'/'))
        {
            projRoot.pop_back();
        }
        if (projRoot.empty())
        {
            return refs;
        }
        const std::wstring leaf = EncodeCwdToProjectDir(cwd);
        const std::wstring projDir = projRoot + L"\\" + leaf;
        static const std::wstring ext = L".jsonl";
        WIN32_FIND_DATAW ff{};
        const HANDLE hf = ::FindFirstFileW((projDir + L"\\*.jsonl").c_str(), &ff);
        if (hf == INVALID_HANDLE_VALUE)
        {
            return refs; // unknown dir / no transcripts
        }
        do
        {
            if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            const std::wstring name = ff.cFileName;
            if (name.size() <= ext.size())
            {
                continue;
            }
            const std::wstring stem = name.substr(0, name.size() - ext.size());
            if (!IsSessionIdStem(stem))
            {
                continue;
            }
            TranscriptRef r;
            r.sessionId = stem;
            r.path = projDir + L"\\" + name;
            r.projectDirLeaf = leaf;
            r.sizeBytes = (static_cast<int64_t>(ff.nFileSizeHigh) << 32) | static_cast<int64_t>(ff.nFileSizeLow);
            r.mtimeMs = FileTimeToUnixMs(ff.ftLastWriteTime);
            r.birthMs = FileTimeToUnixMs(ff.ftCreationTime);
            refs.push_back(std::move(r));
        } while (::FindNextFileW(hf, &ff));
        ::FindClose(hf);
        return refs;
    }

    // Filesystem: the chain nodes for `refs` — ReadTranscriptQuickFacts each (timing / fork / cwd).
    // `cwd` is the fallback for a node whose own cwd can't be read. Skips unreadable files. The shared
    // node-build behind both continuation resolvers. [Agentmaster]
    static std::vector<SessionChainNode> ChainNodesFromRefs(const std::vector<TranscriptRef>& refs,
                                                            const std::wstring& cwd)
    {
        std::vector<SessionChainNode> nodes;
        nodes.reserve(refs.size());
        for (const auto& r : refs)
        {
            const auto qf = ReadTranscriptQuickFacts(r.path, r.birthMs);
            if (!qf.found)
            {
                continue;
            }
            SessionChainNode n;
            n.sessionId = r.sessionId;
            n.cwd = !qf.cwd.empty() ? qf.cwd : cwd;
            n.createdMs = qf.createdMs;
            n.lastActivityMs = qf.lastActivityMs > 0 ? qf.lastActivityMs : qf.createdMs;
            n.fork = qf.fork;
            nodes.push_back(std::move(n));
        }
        return nodes;
    }

    ContinuationPredecessor ResolveContinuationPredecessorOnDisk(const std::wstring& sessionId, const std::wstring& cwd)
    {
        ContinuationPredecessor out;
        if (sessionId.empty() || cwd.empty())
        {
            return out;
        }
        const std::vector<TranscriptRef> refs = EnumerateProjectDirRefs(cwd);
        if (refs.size() < 2)
        {
            return out; // a lone session (or unknown dir) has no predecessor
        }
        const std::vector<SessionChainNode> nodes = ChainNodesFromRefs(refs, cwd);
        out.predId = ResolveContinuationPredecessor(nodes, sessionId);
        if (!out.predId.empty())
        {
            for (const auto& n : nodes)
            {
                if (n.sessionId == out.predId)
                {
                    out.predCwd = n.cwd;
                    break;
                }
            }
        }
        return out;
    }

    ContinuationTail ResolveContinuationTailOnDisk(const std::wstring& sessionId, const std::wstring& cwd)
    {
        ContinuationTail out{ sessionId, cwd, L"", 0 };
        if (sessionId.empty() || cwd.empty())
        {
            return out;
        }
        const std::vector<TranscriptRef> refs = EnumerateProjectDirRefs(cwd);
        if (refs.size() < 2)
        {
            return out; // a lone session (or unknown dir) can't have a continuation
        }
        const std::vector<SessionChainNode> nodes = ChainNodesFromRefs(refs, cwd);

        const auto chain = ResolveContinuationChainTail(nodes, sessionId);
        out.tailId = chain.tailId;
        out.hops = chain.hops;
        for (const auto& n : nodes)
        {
            if (n.sessionId == chain.tailId)
            {
                if (!n.cwd.empty())
                {
                    out.tailCwd = n.cwd;
                }
                break;
            }
        }
        if (chain.tailId != sessionId)
        {
            for (const auto& r : refs)
            {
                if (r.sessionId == chain.tailId)
                {
                    const auto idx = LoadOrRefreshSessionIndex(r);
                    if (idx.valid)
                    {
                        out.tailTitle = PickDisplayTitle(idx.stats.customTitle, idx.stats.aiTitle, idx.stats.summary, idx.stats.firstUserPrompt);
                    }
                    break;
                }
            }
        }
        return out;
    }
}
