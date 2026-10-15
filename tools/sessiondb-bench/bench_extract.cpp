// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's C++ line extractor, timed over the whole corpus on N threads
// (SESSIONS_DB.md section 17: can the database be built from scratch in under 30 s?).
//
// Every top-level and subagent transcript under ~/.claude/projects is read whole, one file per task, biggest file
// first (so no thread is left alone with a 130 MB file at the end), and each complete line goes through
// SessionDbExtract.h's ExtractLine: the one JSON parse and every fact the database derives from a line today. Nothing
// is written. Prints the files, lines, bytes, blocks and block text seen, the wall time and the throughput.
//   usage: bench_extract.exe [threads=16] [maxFiles=0 (all)]
// Measured 2026-10-16 (section 17.2): 16 threads, the whole corpus (6,867 files, 10.74 GB, 3.37 M lines) in 16.4 s.
// Build: build_bench_extract.bat.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../../src/cascadia/TerminalApp/AgentMaster/sessiondb/SessionDbExtract.h"

namespace fs = std::filesystem;

namespace
{
    // One transcript to extract.
    struct Item
    {
        std::wstring path; // the file
        uint64_t size; // its size when listed (the read takes what is there then)
        bool subagent; // a subagent's own transcript: ExtractLine flags its blocks as a subagent's
    };

    // Read a whole file into `out` (reused per thread, so a big file does not re-allocate every time).
    //   path — the file; opened with every share mode, because claude may be appending to it.
    //   out  — receives the bytes read (may be fewer than the size if the file shrank while read).
    // Returns false when the file cannot be opened (gone, or locked against reading); never throws.
    bool ReadWhole(const std::wstring& path, std::string& out)
    {
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        out.resize(static_cast<size_t>(sz.QuadPart));
        size_t got = 0;
        while (got < out.size())
        {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - got, 64u << 20));
            if (!::ReadFile(h, out.data() + got, want, &n, nullptr) || n == 0)
            {
                break;
            }
            got += n;
        }
        out.resize(got);
        ::CloseHandle(h);
        return true;
    }

    // List every top-level transcript (<project>\<sid>.jsonl) and subagent transcript
    // (<project>\<sid>\subagents\*.jsonl) under `root`, biggest first.
    //   root — ~/.claude/projects.
    // Returns the list; a folder that cannot be read is skipped (the error code absorbs it), so this never throws
    // for a missing or locked folder.
    std::vector<Item> ListTranscripts(const fs::path& root)
    {
        std::vector<Item> files;
        std::error_code ec;
        for (const auto& proj : fs::directory_iterator(root, ec))
        {
            if (!proj.is_directory(ec))
            {
                continue;
            }
            for (const auto& e : fs::directory_iterator(proj.path(), ec))
            {
                if (e.is_regular_file(ec) && e.path().extension() == L".jsonl")
                {
                    files.push_back({ e.path().wstring(), e.file_size(ec), false });
                }
                else if (e.is_directory(ec))
                {
                    const fs::path sub = e.path() / L"subagents";
                    if (fs::is_directory(sub, ec))
                    {
                        for (const auto& s : fs::directory_iterator(sub, ec))
                        {
                            if (s.is_regular_file(ec) && s.path().extension() == L".jsonl")
                            {
                                files.push_back({ s.path().wstring(), s.file_size(ec), true });
                            }
                        }
                    }
                }
            }
        }
        std::sort(files.begin(), files.end(), [](const Item& a, const Item& b) { return a.size > b.size; });
        return files;
    }
}

// The benchmark.
//   argv[1] — worker threads (default 16);
//   argv[2] — only the N biggest files (default 0 = every file).
// Returns 0. A file that cannot be read is skipped; a torn last line (claude still writing it) is not extracted.
// ExtractLine does not throw on bad input (it returns empty facts), so no exception is expected; one would end
// the process, which is acceptable for a benchmark.
int wmain(int argc, wchar_t** argv)
{
    const int threads = argc > 1 ? _wtoi(argv[1]) : 16;
    const size_t maxFiles = argc > 2 ? static_cast<size_t>(_wtoi(argv[2])) : 0;
    wchar_t profile[MAX_PATH]{};
    ::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Item> files = ListTranscripts(fs::path(profile) / L".claude" / L"projects");
    if (maxFiles && files.size() > maxFiles)
    {
        files.resize(maxFiles);
    }
    const auto t1 = std::chrono::steady_clock::now();

    std::atomic<size_t> next{ 0 };
    std::atomic<uint64_t> bytes{ 0 }, lines{ 0 }, blocks{ 0 }, textChars{ 0 }, readMs{ 0 };
    // One worker: take the next file, read it, extract every complete line, add up what it saw.
    const auto work = [&]() {
        std::string buf;
        for (;;)
        {
            const size_t i = next.fetch_add(1);
            if (i >= files.size())
            {
                return;
            }
            const auto r0 = std::chrono::steady_clock::now();
            if (!ReadWhole(files[i].path, buf))
            {
                continue;
            }
            readMs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - r0).count());
            bytes += buf.size();
            uint64_t l = 0, b = 0, t = 0;
            size_t at = 0;
            while (at < buf.size())
            {
                const size_t nl = buf.find('\n', at);
                if (nl == std::string::npos)
                {
                    break; // a torn last line: claude is still writing it
                }
                const std::string_view line{ buf.data() + at, nl - at };
                at = nl + 1;
                if (line.empty())
                {
                    continue;
                }
                const auto facts = Agentmaster::SessionDb::ExtractLine(line, files[i].subagent);
                ++l;
                b += facts.blocks.size();
                for (const auto& blk : facts.blocks)
                {
                    t += blk.text.size();
                }
            }
            lines += l;
            blocks += b;
            textChars += t;
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i)
    {
        pool.emplace_back(work);
    }
    for (auto& th : pool)
    {
        th.join();
    }
    const auto t2 = std::chrono::steady_clock::now();
    const double listS = std::chrono::duration<double>(t1 - t0).count();
    const double runS = std::chrono::duration<double>(t2 - t1).count();
    std::printf("%zu files (listed in %.2f s); %d threads\n", files.size(), listS, threads);
    std::printf("%llu lines, %.2f GB read, %llu blocks, %.2f G characters of block text\n", static_cast<unsigned long long>(lines.load()),
                bytes.load() / 1e9, static_cast<unsigned long long>(blocks.load()), textChars.load() / 1e9);
    std::printf("extract: %.1f s wall (%.2f GB/s); reading took %.1f s of thread time\n", runS, bytes.load() / 1e9 / runS,
                readMs.load() / 1000.0);
    return 0;
}
