// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What today's WHOLE-FILE transcript readers cost, measured with the engine's own code — the baseline
// of SESSIONS_DB.md section 8.5 (the summary from the database) and the ground truth its prototype is
// verified against. Built by build_bench_summary.bat (an optimized build of the engine TUs the m5
// harness links; outputs go to the benchmarks' work folder, never the repository).
//
// Two modes:
//   bench_summary.exe <scratch dir> <transcript.jsonl>...
//       For each transcript: AnalyzeSessionTranscript (the summary box: per-tab panel, side panel, tab
//       tooltip, Sessions detail, Manager Summary tab) three times on fresh COPIES, so every call misses
//       the analyzer's (path, size, mtime) cache, as a transcript's growth does; CollectConversationUpstream
//       (the summary's whole-conversation chain) on the ORIGINAL path, whose project folder holds the
//       relatives; RenderTranscriptTurnsFile at the Default level (the transcript overlay and
//       Copy ▸ Transcript ▸ Default) on a copy. Copies go to the scratch dir and are deleted.
//   bench_summary.exe --facts <out.tsv> <project folder>
//       One AnalyzeSessionTranscript per top-level transcript of a ~/.claude/projects folder, written as
//       a TSV row of the summary's facts: what summary_from_db.py --verify diffs its SQL against.
//
// The originals are only read. Run it with AGENTMASTER_PROFILE pointing at a scratch folder: the engine
// code logs through the active profile, and a release-hosted shell carries the LIVE profile's path.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "ProcessInspect.h"
#include "TranscriptTurns.h"

namespace fs = std::filesystem;

namespace
{
    // Milliseconds elapsed since `start`.
    //   start — a steady_clock time point taken before the work.
    // Returns the elapsed milliseconds. Never throws.
    double MsSince(std::chrono::steady_clock::time_point start) noexcept
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // Reads a whole file once and discards it, so the timed call that follows reads from the OS file
    // cache (the summary surfaces re-read a transcript the scanner just tailed: a warm cache is the
    // common case, and the comparison with the database must not be a disk benchmark).
    //   path — the file.
    // Returns the bytes read (0 when it cannot be opened). Never throws.
    size_t WarmFileCache(const fs::path& path) noexcept
    {
        size_t total = 0;
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return 0;
        }
        static char buffer[1 << 20];
        DWORD got = 0;
        while (::ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got != 0)
        {
            total += got;
        }
        ::CloseHandle(h);
        return total;
    }

    // UTF-16 -> UTF-8 for the TSV writer.
    //   w — the text.
    // Returns the UTF-8 bytes ("" for empty input or a failed conversion). Throws only std::bad_alloc.
    std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        }
        return out;
    }

    // Every prompt the summary numbers: the earlier (compacted-away) segments' plus the current one's.
    //   s — an analyzer result.
    // Returns the count. Never throws.
    size_t AllPrompts(const ::Agentmaster::SessionSummary& s) noexcept
    {
        size_t all = s.userMsgs.size();
        for (const auto& seg : s.previousSegments)
        {
            all += seg.userMsgs.size();
        }
        return all;
    }

    // The --facts mode (see the file comment).
    //   outPath — the TSV to write (overwritten).
    //   folder  — a ~/.claude/projects/<encoded cwd> folder; only its *.jsonl files are read (subagent
    //             transcripts live in subfolders and are skipped).
    // Returns 0, or 1 when the TSV cannot be opened. The analyzer contains its own failures (an
    // unreadable transcript is a row of zeros); a failed directory listing ends the listing early.
    int WriteFactsTsv(const fs::path& outPath, const fs::path& folder)
    {
        FILE* out = nullptr;
        if (_wfopen_s(&out, outPath.c_str(), L"wb") != 0 || !out)
        {
            std::fwprintf(stderr, L"cannot open %ls\n", outPath.c_str());
            return 1;
        }
        std::fputs("stem\tsize\tms\tprompts_current\tprompts_all\tearlier_segments\tread\tcreated\tedited\tskills\ttasks_done\ttasks_open\t"
                   "compacted\trecap_chars\tfirst_ts\tlast_ts\tlast_user_ts\tbranch\tparent\texit_plan\tplan_content\tplan_file\tplan_reads\n",
                   out);
        std::error_code ec;
        size_t files = 0;
        double totalMs = 0;
        for (const auto& entry : fs::directory_iterator(folder, ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().extension() != L".jsonl")
            {
                continue;
            }
            const auto size = entry.file_size(ec);
            const auto start = std::chrono::steady_clock::now();
            const auto s = ::Agentmaster::AnalyzeSessionTranscript(entry.path().wstring(), 0);
            const double ms = MsSince(start);
            totalMs += ms;
            ++files;
            std::fprintf(out, "%s\t%llu\t%.1f\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%d\t%d\t%d\t%zu\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%zu\n",
                         Utf8(entry.path().stem().wstring()).c_str(), static_cast<unsigned long long>(size), ms,
                         s.userMsgs.size(), AllPrompts(s), s.previousSegments.size(), s.filesRead.size(), s.filesCreated.size(),
                         s.filesEdited.size(), s.skillsLoaded.size(), s.tasksCompleted, s.tasksPending, s.compacted ? 1 : 0,
                         s.awaySummary.size(), Utf8(s.firstTs).c_str(), Utf8(s.lastTs).c_str(), Utf8(s.lastUserTs).c_str(),
                         Utf8(s.branch).c_str(), Utf8(s.parentSessionId).c_str(), s.hasExitPlanMode ? 1 : 0, s.hasPlanContent ? 1 : 0,
                         Utf8(s.planFilePath).c_str(), s.planFilesRead.size());
        }
        std::fclose(out);
        std::wprintf(L"%zu transcripts analyzed in %.1f s\n", files, totalMs / 1000.0);
        return 0;
    }

    // The timing mode for one transcript (see the file comment).
    //   scratch  — where the copies go.
    //   original — the transcript.
    // Never throws (a failed copy skips that measurement and says so).
    void TimeOne(const fs::path& scratch, const fs::path& original)
    {
        std::error_code ec;
        const auto size = fs::file_size(original, ec);
        if (ec)
        {
            std::wprintf(L"%ls: cannot stat (%hs)\n", original.c_str(), ec.message().c_str());
            return;
        }
        std::wprintf(L"== %ls (%.1f MB)\n", original.filename().c_str(), size / 1e6);

        // The analyzer, three runs on three fresh copies (each a cache miss: a new path).
        for (int run = 0; run < 3; ++run)
        {
            const fs::path copy = scratch / (original.stem().wstring() + L"-run" + std::to_wstring(run) + L".jsonl");
            fs::copy_file(original, copy, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                std::wprintf(L"   copy failed (%hs)\n", ec.message().c_str());
                break;
            }
            WarmFileCache(copy);
            const auto start = std::chrono::steady_clock::now();
            const auto s = ::Agentmaster::AnalyzeSessionTranscript(copy.wstring(), 0);
            const double ms = MsSince(start);
            std::wprintf(L"   AnalyzeSessionTranscript run %d: %.0f ms  (prompts %zu current, %zu all, %zu earlier segments; read %zu, "
                         L"created %zu, edited %zu; skills %zu; tasks %d done / %d open; recap %zu chars; first %ls last %ls; branch %ls)\n",
                         run, ms, s.userMsgs.size(), AllPrompts(s), s.previousSegments.size(), s.filesRead.size(), s.filesCreated.size(),
                         s.filesEdited.size(), s.skillsLoaded.size(), s.tasksCompleted, s.tasksPending, s.awaySummary.size(),
                         s.firstTs.c_str(), s.lastTs.c_str(), s.branch.c_str());
            fs::remove(copy, ec);
        }

        // The conversation chain, on the original (its project folder is where the relatives are).
        WarmFileCache(original);
        {
            const auto start = std::chrono::steady_clock::now();
            const auto up = ::Agentmaster::CollectConversationUpstream(original, {});
            std::wprintf(L"   CollectConversationUpstream: %.0f ms  (%zu upstream segments, %zu upstream uuids)\n",
                         MsSince(start), up.segments.size(), up.uuids.size());
        }

        // The transcript overlay's Default level, on a copy (no relatives beside it: the render alone).
        const fs::path copy = scratch / (original.stem().wstring() + L"-render.jsonl");
        fs::copy_file(original, copy, fs::copy_options::overwrite_existing, ec);
        if (!ec)
        {
            WarmFileCache(copy);
            const auto start = std::chrono::steady_clock::now();
            const auto doc = ::Agentmaster::RenderTranscriptTurnsFile(copy, ::Agentmaster::TurnsOptionsForLevel(::Agentmaster::TurnsLevel::Default), false);
            std::wprintf(L"   RenderTranscriptTurnsFile(Default): %.0f ms  (%zu chars)\n", MsSince(start), doc ? doc->size() : 0);
            fs::remove(copy, ec);
        }
    }
}

// Entry point: see the file comment.
//   argc, argv — <scratch dir> <transcript.jsonl>..., or --facts <out.tsv> <project folder>.
// Returns 0, 1 when the TSV cannot be written, or 2 on a usage error.
int wmain(int argc, wchar_t** argv)
{
    if (argc == 4 && std::wstring_view{ argv[1] } == L"--facts")
    {
        return WriteFactsTsv(argv[2], argv[3]);
    }
    if (argc < 3)
    {
        std::fwprintf(stderr, L"usage: bench_summary.exe <scratch dir> <transcript.jsonl>...\n"
                              L"       bench_summary.exe --facts <out.tsv> <project folder>\n");
        return 2;
    }
    const fs::path scratch{ argv[1] };
    std::error_code ec;
    fs::create_directories(scratch, ec);
    for (int a = 2; a < argc; ++a)
    {
        TimeOne(scratch, argv[a]);
    }
    return 0;
}
