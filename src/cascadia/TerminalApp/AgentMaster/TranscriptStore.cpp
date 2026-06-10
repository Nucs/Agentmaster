// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See TranscriptStore.h / SESSIONS.md §6.
#define NOMINMAX
#include "TranscriptStore.h"

#include <windows.h>

#include <time.h> // _mkgmtime64 (ISO timestamp -> Unix epoch)

#include <algorithm>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root
#include "Json.h" // transcript line parsing

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
            L"<teammate-message", // agent-team traffic
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
                        if (maxAgentTextChars != 0 && f.agentText.size() < maxAgentTextChars)
                        {
                            if (const auto* in = blk.Find(L"input"); in && in->type == json::Value::Type::Obj)
                            {
                                AppendCapped(f.agentText, json::Dump(*in), maxAgentTextChars);
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
        // A fork's copied line timestamps predate the file — its true creation is the file birth.
        q.createdMs = q.fork ? fileBirthMs : firstTs;

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
        ::CloseHandle(h);
        return q;
    }
}
