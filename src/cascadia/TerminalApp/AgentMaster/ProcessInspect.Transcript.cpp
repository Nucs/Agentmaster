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
// ★ ProcessInspect.Transcript.cpp  - Claude transcript resolution + timing/title/prompts + git plumbing + Codex (C1) facts/rollout
//   ProcessInspect.Content.cpp     - conversation-text read + the session-end.js summary analyzer + Codex (C2) rollout-tail state
//   ProcessInspect.Window.cpp      - Bring Window To Front (its own UIA-helper anon ns + the public window/tab-pick API)
//   ProcessInspect.Summary.cpp     - the shared summary-box renderers (the per-tab overlay + the Sessions page)
// ======================================================================================
//
// Agentmaster engine TU. ProcessInspect TRANSCRIPT: Claude transcript resolution + timing/title/prompts + git plumbing + Codex (Phase C1) facts/rollout resolution. Partial TU of ProcessInspect.cpp.
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
    // ===== transcript resolution ===========================================================

    std::wstring EncodeCwdToProjectDir(std::wstring_view cwd)
    {
        std::wstring out;
        out.reserve(cwd.size());
        for (const wchar_t c : cwd)
        {
            const bool alnum = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
            out.push_back(alnum ? c : L'-');
        }
        return out;
    }

    std::wstring ExtractCwdFromTranscriptHead(std::wstring_view headText)
    {
        size_t pos = 0;
        while ((pos = headText.find(L"\"cwd\"", pos)) != std::wstring_view::npos)
        {
            size_t i = pos + 5; // past the key
            while (i < headText.size() && (headText[i] == L' ' || headText[i] == L'\t'))
            {
                ++i;
            }
            if (i < headText.size() && headText[i] == L':')
            {
                ++i;
            }
            while (i < headText.size() && (headText[i] == L' ' || headText[i] == L'\t'))
            {
                ++i;
            }
            if (i < headText.size() && headText[i] == L'"')
            {
                ++i;
                std::wstring val;
                while (i < headText.size() && headText[i] != L'"')
                {
                    if (headText[i] == L'\\' && i + 1 < headText.size())
                    {
                        ++i;
                        const wchar_t e = headText[i];
                        val.push_back(e == L'n' ? L'\n' : (e == L't' ? L'\t' : e)); // \\ -> \, \" -> ", etc.
                    }
                    else
                    {
                        val.push_back(headText[i]);
                    }
                    ++i;
                }
                if (!val.empty())
                {
                    return val;
                }
            }
            pos += 5;
        }
        return {};
    }

    std::wstring PickNewestTranscript(const std::vector<TranscriptCandidate>& candidates, int64_t startUnixMs, int64_t tieWindowMs)
    {
        if (candidates.empty())
        {
            return {};
        }
        if (startUnixMs > 0)
        {
            // IDENTITY by creation time: a claude's OWN transcript is created when it first writes —
            // at/after its process start. Pick the candidate whose CREATION time is closest to (and
            // not significantly before) the claude's start, and REJECT transcripts created well
            // before it started (those belong to OTHER claudes / are stale). This is what keeps two
            // claudes sharing one cwd bound to their OWN conversation, and resolves a never-written-
            // yet claude to "" rather than collapsing it onto a stale transcript (§11d / §13). mtime
            // (activity) does NOT determine identity, so it is deliberately not used on this path.
            const TranscriptCandidate* best = nullptr;
            int64_t bestDelta = 0;
            for (const auto& c : candidates)
            {
                if (c.ctimeMs < startUnixMs - kTranscriptStartSkewMs)
                {
                    continue; // created before this claude started -> not its own
                }
                const int64_t d = c.ctimeMs >= startUnixMs ? c.ctimeMs - startUnixMs : startUnixMs - c.ctimeMs;
                if (best == nullptr || d < bestDelta)
                {
                    best = &c;
                    bestDelta = d;
                }
            }
            return best != nullptr ? best->stem : std::wstring{};
        }
        // No start hint (startUnixMs == 0): fall back to the newest by mtime (tie-broken by ctime).
        (void)tieWindowMs; // retained for API compat; the start>0 path keys on ctime, not an mtime band
        const TranscriptCandidate* best = nullptr;
        for (const auto& c : candidates)
        {
            if (best == nullptr || c.mtimeMs > best->mtimeMs || (c.mtimeMs == best->mtimeMs && c.ctimeMs > best->ctimeMs))
            {
                best = &c;
            }
        }
        return best != nullptr ? best->stem : std::wstring{};
    }

    std::wstring ResolveSessionIdIn(std::wstring_view projectsDir, std::wstring_view cwd, int64_t startUnixMs)
    {
        if (projectsDir.empty() || cwd.empty())
        {
            return {};
        }
        const std::wstring dir = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd);
        const auto candidates = GlobTranscripts(dir);
        return PickNewestTranscript(candidates, startUnixMs);
    }

    std::wstring ResolveSessionId(std::wstring_view cwd, int64_t startUnixMs)
    {
        return ResolveSessionIdIn(ClaudeProjectsDir(), cwd, startUnixMs);
    }

    // ===== transcript content: timing + title + human prompts ================================

    int64_t SubagentActivityUnixMs(std::wstring_view transcriptPath)
    {
        // The side files live in a sibling directory named after the session id: strip the
        // ".jsonl" off "<...>/<id>.jsonl" to get "<...>/<id>", then scan its "subagents" and
        // "tool-results" children for the newest FILE write time. (We enumerate files, not the
        // dir, because Windows does NOT bump a directory's mtime when a file inside it is appended
        // to — only on add/remove — and Claude APPENDS to agent-<id>.jsonl as a subagent works.)
        if (transcriptPath.size() < 7) // shorter than "x.jsonl"
        {
            return 0;
        }
        std::wstring base{ transcriptPath };
        constexpr std::wstring_view kExt = L".jsonl";
        if (base.size() >= kExt.size())
        {
            const size_t off = base.size() - kExt.size();
            bool isJsonl = true;
            for (size_t i = 0; i < kExt.size(); ++i)
            {
                if (towlower(base[off + i]) != kExt[i]) // kExt is lowercase; the path's ext is too, but fold to be safe
                {
                    isJsonl = false;
                    break;
                }
            }
            if (isJsonl)
            {
                base.resize(off);
            }
        }
        int64_t newest = 0;
        for (const wchar_t* sub : { L"\\subagents\\*", L"\\tool-results\\*" })
        {
            const std::wstring pattern = base + sub;
            WIN32_FIND_DATAW fd{};
            const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE)
            {
                continue; // no such side dir (the common case) — instant miss
            }
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue; // ".", "..", or a nested-subagent dir (presence-"busy" covers those)
                }
                const int64_t m = FileTimeToUnixMs(fd.ftLastWriteTime);
                if (m > newest)
                {
                    newest = m;
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        return newest;
    }

    bool TranscriptTimesIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, int64_t& createdUnixMs, int64_t& lastActivityUnixMs)
    {
        createdUnixMs = 0;
        lastActivityUnixMs = 0;
        if (projectsDir.empty() || cwd.empty() || sessionId.empty())
        {
            return false;
        }
        const std::wstring path = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd) + L"\\" + std::wstring{ sessionId } + L".jsonl";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return false;
        }
        createdUnixMs = FileTimeToUnixMs(fad.ftCreationTime);
        lastActivityUnixMs = FileTimeToUnixMs(fad.ftLastWriteTime);
        // Agentmaster (subagent activity): while a Task/Agent subagent runs, the side files
        // (<id>/subagents/*.jsonl, <id>/tool-results/*) grow but THIS transcript stays quiescent —
        // so its mtime alone reads "stale". Fold the newest side-file write into "last activity" so
        // the per-session timing adornment (and the scanner's "transcript advanced" Enter-retry
        // check) reflect work happening inside a subagent. createdUnixMs (the conversation start) is
        // left as the parent's — a subagent never predates its parent.
        const int64_t subMs = SubagentActivityUnixMs(path);
        if (subMs > lastActivityUnixMs)
        {
            lastActivityUnixMs = subMs;
        }
        return true;
    }

    bool TranscriptTimes(std::wstring_view cwd, std::wstring_view sessionId, int64_t& createdUnixMs, int64_t& lastActivityUnixMs)
    {
        return TranscriptTimesIn(ClaudeProjectsDir(), cwd, sessionId, createdUnixMs, lastActivityUnixMs);
    }

    static TranscriptInfo ReadTranscriptInfoInImpl(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts);
    // Agentmaster (extra-safe): this whole-file reader materializes per-line facts for EVERY line and
    // is called from detached background threads (the Manager's external-plan loader, the observer's
    // title enrichment) with no frame to catch a throw — the same uncontained-helper class behind the
    // v0.6.x resume crash-loop (a std::bad_alloc on a huge transcript == process death). Contain +
    // return the empty "not found" result, the intended degradation.
    TranscriptInfo ReadTranscriptInfoIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts)
    {
        try
        {
            return ReadTranscriptInfoInImpl(projectsDir, cwd, sessionId, maxBytes, maxPrompts);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] ReadTranscriptInfoIn: swallowed exception (no crash)\n");
            return {};
        }
    }

    static TranscriptInfo ReadTranscriptInfoInImpl(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts)
    {
        TranscriptInfo info;
        if (projectsDir.empty() || cwd.empty() || sessionId.empty())
        {
            return info;
        }
        const std::wstring path = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd) + L"\\" + std::wstring{ sessionId } + L".jsonl";

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return info; // not found
        }
        info.found = true;
        info.createdUnixMs = FileTimeToUnixMs(fad.ftCreationTime);
        info.lastActivityUnixMs = FileTimeToUnixMs(fad.ftLastWriteTime);

        // Scrub pasted-image base64 blobs BEFORE widening (analyze-footprint; titles/prompts never
        // live inside an image payload).
        const std::string bytes = ScrubLargeBase64Payloads(ReadFileHead(path, maxBytes));
        if (bytes.empty())
        {
            return info; // times only
        }
        const bool truncated = (maxBytes != 0); // a head read may end mid-line
        const std::wstring wide = Utf8ToWide(bytes);

        // Agentmaster (revert-aware DISPLAY): build the per-message facts WITH the "IsActiveLeaf"
        // property (onActiveBranch) and honor it, so the title + prompt list reflect ONLY the LIVE
        // conversation — the chain from the current leaf to root. A double-ESC rewind orphans the
        // abandoned branch (its lines stay in the file, interleaved with the live ones). Marking is
        // whole-file, so on a truncated HEAD read (the tail leaf marker is absent, and an in-window
        // marker would name a STALE leaf) we DON'T mark — the title degrades to the legacy first-in-file
        // prompt. SEARCH never filters on this (TranscriptStore keeps indexing every line).
        const auto lines = ClassifyTranscriptLines(wide, /*maxUserTextChars*/ static_cast<size_t>(-1), /*maxAgentTextChars*/ 0, /*markActiveBranch*/ !truncated);

        std::wstring firstPrompt;
        for (const auto& f : lines)
        {
            // Title lines win by precedence (custom > ai > legacy summary); the LAST of each kind wins
            // (a retitle appends a newer line). They carry no uuid, so a rewind never filters them — a
            // user's chosen title persists across one.
            if (f.kind == TranscriptLineKind::CustomTitle)
            {
                if (!f.title.empty())
                {
                    info.customTitle = f.title;
                }
                continue;
            }
            if (f.kind == TranscriptLineKind::AiTitle)
            {
                if (!f.title.empty())
                {
                    info.aiTitle = f.title;
                }
                continue;
            }
            if (f.kind == TranscriptLineKind::Summary)
            {
                if (!f.title.empty())
                {
                    info.summary = f.title;
                }
                continue;
            }
            // A rewound-away line (onActiveBranch==false) contributes no prompt. f.userText is non-empty
            // ONLY for a REAL human prompt — ClassifyTranscriptLine already drops meta / noise / sidechain
            // / tool-result turns — so this one test stands in for the whole old user-line filter chain.
            if (!f.onActiveBranch || f.kind != TranscriptLineKind::UserPrompt || f.userText.empty())
            {
                continue;
            }
            if (info.gitBranch.empty() && !f.gitBranch.empty())
            {
                info.gitBranch = f.gitBranch;
            }
            if (firstPrompt.empty())
            {
                firstPrompt = f.userText;
            }
            if (info.userPrompts.size() < maxPrompts)
            {
                info.userPrompts.push_back(f.userText);
            }
        }
        info.title = FirstLineTrim(firstPrompt);
        return info;
    }

    TranscriptInfo ReadTranscriptInfo(std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts)
    {
        return ReadTranscriptInfoIn(ClaudeProjectsDir(), cwd, sessionId, maxBytes, maxPrompts);
    }

    // Agentmaster: extract the idle RECAP (away_summary) from a chunk of transcript JSONL — the LAST
    // {"type":"system","subtype":"away_summary"} line's content, normalized (NormalizeRecapText, the
    // one-true normalizer that strips the "(disable recaps in /config)" UI hint). PURE + total, so it
    // is unit-testable without file IO (m5_tests). The detection is the SAME trivial check three
    // readers share — ParseTranscriptDelta (SessionScanner, the managed delta) + AnalyzeSessionTranscript
    // (the summary box, a full head pass) + this one — and they all share NormalizeRecapText, so the
    // normalization can never drift; only the REGION each reads differs. The chunk may begin with a
    // PARTIAL line (a tail read can start mid-line): a partial JSON line fails json::Parse and is
    // skipped. Returns "" when no away_summary is present, so an empty/garbled tail NEVER clears a
    // stored recap (the "empty never clears" rule, identical to the other two readers).
    std::wstring RecapFromTranscriptChunk(std::wstring_view chunk)
    {
        return TailFactsFromTranscriptChunk(chunk).recap;
    }

    // Agentmaster (current-model adornment): the shared TAIL-FACTS extractor — one pass over the
    // chunk collects BOTH the idle recap (as before, RecapFromTranscriptChunk above is now a thin
    // view over this) and the CURRENT MODEL: the LAST real assistant line's message.model. "Real"
    // excludes the synthetic API-error line (isApiErrorMessage:true, whose pseudo-model is
    // "<synthetic>" — belt-and-braces: both signals are checked) and isSidechain lines (an inlined
    // subagent's reply runs on ITS model, not the session's — the same defensiveness
    // ParseTranscriptDelta applies). "Last wins" for both fields; "" == not present in the window,
    // so the caller's "empty never clears" rule holds per field.
    TranscriptTailFacts TailFactsFromTranscriptChunk(std::wstring_view chunk)
    {
        TranscriptTailFacts facts;
        size_t start = 0;
        for (size_t i = 0; i <= chunk.size(); ++i)
        {
            if (i < chunk.size() && chunk[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line(chunk.data() + start, i - start);
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
                continue; // garbage / a partial leading line — skip
            }
            const auto& obj = *parsed;
            const std::wstring type = obj.StrAt(L"type");
            if (type == L"system" && obj.StrAt(L"subtype") == L"away_summary")
            {
                if (std::wstring r = NormalizeRecapText(obj.StrAt(L"content")); !r.empty())
                {
                    facts.recap = std::move(r); // LAST one in the chunk wins (newer recaps supersede)
                }
            }
            else if (type == L"assistant" && !obj.BoolAt(L"isApiErrorMessage") && !obj.BoolAt(L"isSidechain"))
            {
                if (const auto* msg = obj.Find(L"message"); msg && msg->type == json::Value::Type::Obj)
                {
                    if (std::wstring m = msg->StrAt(L"model"); !m.empty() && m != L"<synthetic>")
                    {
                        facts.model = std::move(m); // LAST real assistant line wins (== the current model)
                    }
                }
            }
        }
        return facts;
    }

    // Agentmaster: read JUST the idle RECAP out-of-band, from the transcript TAIL. WHY the tail and
    // not ReadTranscriptInfo's head: the recap is an IDLE summary Claude appends near the END of the
    // file (>5-min idle), so it lives in the TAIL — the OPPOSITE end from the first-prompt title. This
    // pulls the recap from the SAME REGION the SessionScanner's byte-cursor delta pulls it from for
    // MANAGED sessions (SessionScanner.cpp), and the SAME region + window the agentmaster-cli `show`
    // reader uses (cli/agentcli.cpp: ReadFileTail(kTailBytes) -> ParseTranscriptDelta.recap). That is
    // what lets the Fleet Observer be the recap provider for EXTERNAL sessions — which have NO scanner
    // cursor — without a whole-file read: one bounded tail read, only when the transcript grew (the
    // observer mtime-gates the call). `maxTailBytes` 0 == the whole file. Empty if no recap is in the
    // tail window / unreadable. Filesystem only.
    std::wstring ReadTranscriptRecapTailIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxTailBytes)
    {
        return ReadTranscriptTailFactsIn(projectsDir, cwd, sessionId, maxTailBytes).recap;
    }

    std::wstring ReadTranscriptRecapTail(std::wstring_view cwd, std::wstring_view sessionId, size_t maxTailBytes)
    {
        return ReadTranscriptRecapTailIn(ClaudeProjectsDir(), cwd, sessionId, maxTailBytes);
    }

    TranscriptTailFacts ReadTranscriptTailFactsIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxTailBytes)
    {
        if (projectsDir.empty() || cwd.empty() || sessionId.empty())
        {
            return {};
        }
        const std::wstring path = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd) + L"\\" + std::wstring{ sessionId } + L".jsonl";
        const std::string bytes = ReadFileTail(path, maxTailBytes);
        if (bytes.empty())
        {
            return {};
        }
        return TailFactsFromTranscriptChunk(Utf8ToWide(bytes));
    }

    TranscriptTailFacts ReadTranscriptTailFacts(std::wstring_view cwd, std::wstring_view sessionId, size_t maxTailBytes)
    {
        return ReadTranscriptTailFactsIn(ClaudeProjectsDir(), cwd, sessionId, maxTailBytes);
    }

    std::wstring TranscriptDisplayTitle(const TranscriptInfo& info)
    {
        return PickDisplayTitle(info.customTitle, info.aiTitle, info.summary, info.title);
    }

    // ---- git plumbing shared by ReadGitBranchForDir + ListGitWorktrees (Agentmaster) ----
    // All filesystem-only, factored out of the original ReadGitBranchForDir so the worktree
    // enumeration reuses the EXACT same .git-file / HEAD parsing. Internal linkage (anon namespace);
    // they see the file's earlier ReadFileHead / Utf8ToWide primitives.
    namespace
    {
        // Trim surrounding whitespace/newlines (the .git admin files are single short lines).
        std::wstring GitTrim(const std::wstring& s)
        {
            const auto b = s.find_first_not_of(L" \t\r\n");
            if (b == std::wstring::npos)
            {
                return {};
            }
            const auto e = s.find_last_not_of(L" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        // Flip '/'->'\' (git writes forward slashes in .git admin files on Windows).
        void GitFlipSeps(std::wstring& s)
        {
            for (auto& ch : s)
            {
                if (ch == L'/')
                {
                    ch = L'\\';
                }
            }
        }

        bool GitIsAbsolute(const std::wstring& s)
        {
            return (s.size() >= 2 && s[1] == L':') || (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\');
        }

        // Lexically resolve '.'/'..' (NO filesystem touch) so a "commondir" like "../.." canonicalizes.
        std::wstring GitFullPath(const std::wstring& p)
        {
            wchar_t buf[1024];
            const DWORD n = ::GetFullPathNameW(p.c_str(), ARRAYSIZE(buf), buf, nullptr);
            return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring{ buf, n } : p;
        }

        // The last path component, trailing separators stripped ("" for a bare root).
        std::wstring GitLeaf(std::wstring s)
        {
            while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/'))
            {
                s.pop_back();
            }
            const auto pos = s.find_last_of(L"\\/");
            return (pos == std::wstring::npos) ? s : s.substr(pos + 1);
        }

        // The parent directory (one component up), trailing separators stripped ("" if at a root).
        std::wstring GitParent(std::wstring s)
        {
            while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/'))
            {
                s.pop_back();
            }
            const auto pos = s.find_last_of(L"\\/");
            return (pos == std::wstring::npos || pos < 2) ? std::wstring{} : s.substr(0, pos);
        }

        bool GitLeafEq(const std::wstring& p, const wchar_t* leaf)
        {
            const std::wstring l = GitLeaf(p);
            return ::CompareStringOrdinal(l.c_str(), -1, leaf, -1, TRUE) == CSTR_EQUAL;
        }

        bool GitPathEq(const std::wstring& a, const std::wstring& b)
        {
            return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
        }

        // Walk UP from `dir` to the nearest .git, returning the per-worktree git directory: a normal
        // repo root's ".git" DIR, or — for a worktree/submodule ".git" FILE ("gitdir: <path>") — the
        // path it names (a linked worktree's <common>\worktrees\<id>). "" when not under a repo.
        std::wstring FindGitDirForPath(const std::wstring& dir)
        {
            if (dir.empty())
            {
                return {};
            }
            std::wstring cur = dir;
            while (cur.size() > 1 && (cur.back() == L'\\' || cur.back() == L'/'))
            {
                cur.pop_back();
            }
            for (;;)
            {
                const std::wstring dot = cur + L"\\.git";
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(dot.c_str(), GetFileExInfoStandard, &fad))
                {
                    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        return dot; // normal repo root
                    }
                    std::wstring text = Utf8ToWide(ReadFileHead(dot, 4096)); // ".git" FILE: "gitdir: <path>"
                    const std::wstring key = L"gitdir:";
                    if (const auto k = text.find(key); k != std::wstring::npos)
                    {
                        std::wstring g = GitTrim(text.substr(k + key.size()));
                        if (!g.empty())
                        {
                            GitFlipSeps(g);
                            return GitIsAbsolute(g) ? g : GitFullPath(cur + L"\\" + g); // relative gitdir is relative to .git's dir
                        }
                    }
                    return {};
                }
                const auto slash = cur.find_last_of(L"\\/");
                if (slash == std::wstring::npos || slash < 2)
                {
                    return {}; // reached the drive root (e.g. "C:") -> not under a repo
                }
                cur = cur.substr(0, slash);
            }
        }

        // Parse a .git/HEAD body -> branch name (slashes kept: feature/issue123), a non-branch ref's
        // leaf, or a short SHA for a detached HEAD. "" only for empty input.
        std::wstring ParseGitHead(const std::wstring& raw)
        {
            const std::wstring head = GitTrim(raw);
            const std::wstring branchPrefix = L"ref: refs/heads/";
            if (head.rfind(branchPrefix, 0) == 0)
            {
                return head.substr(branchPrefix.size());
            }
            if (head.rfind(L"ref: ", 0) == 0)
            {
                const auto s = head.find_last_of(L'/'); // a non-branch ref (tag / note) -> its leaf
                return (s != std::wstring::npos) ? head.substr(s + 1) : head.substr(5);
            }
            return head.size() >= 7 ? head.substr(0, 7) : head; // detached HEAD: a short SHA, not "HEAD"
        }
    }

    // Agentmaster: the CURRENT git branch of a working dir (see ProcessInspect.h). Out-of-band,
    // filesystem only — reuses the shared .git walk (FindGitDirForPath) + HEAD parse (ParseGitHead).
    std::wstring ReadGitBranchForDir(const std::wstring& dir)
    {
        const std::wstring gitDir = FindGitDirForPath(dir);
        if (gitDir.empty())
        {
            return {};
        }
        return ParseGitHead(Utf8ToWide(ReadFileHead(gitDir + L"\\HEAD", 4096)));
    }

    // Agentmaster: every worktree of the repo containing `dir` (see ProcessInspect.h). The main
    // worktree (the repo root) first, then each linked worktree under <common>\worktrees\<id>,
    // alphabetized. Pure filesystem — mirrors ReadGitBranchForDir's reads.
    std::vector<GitWorktreeInfo> ListGitWorktrees(const std::wstring& dir)
    {
        std::vector<GitWorktreeInfo> out;
        const std::wstring gitDir = FindGitDirForPath(dir);
        if (gitDir.empty())
        {
            return out;
        }

        // Resolve the COMMON git dir (the main repo's .git). A linked worktree's gitDir is
        // <common>\worktrees\<id> and carries a "commondir" file (usually "../.."); the main
        // worktree's gitDir already IS the common dir (it has no commondir file).
        std::wstring commonDir = gitDir;
        if (std::wstring cd = GitTrim(Utf8ToWide(ReadFileHead(gitDir + L"\\commondir", 4096))); !cd.empty())
        {
            GitFlipSeps(cd);
            commonDir = GitIsAbsolute(cd) ? cd : GitFullPath(gitDir + L"\\" + cd);
        }

        // Which worktree CONTAINS the queried dir (to flag isCurrent): the main worktree when we
        // resolved a real ".git" DIR, else — when we walked into a LINKED worktree — that worktree's
        // own path, read from its gitDir's "gitdir" breadcrumb (<common>\worktrees\<id>\gitdir ->
        // <worktree>\.git). Without this second arm, a query from INSIDE a linked worktree left
        // currentWtPath empty, so nothing was ever flagged current there (only from the main worktree).
        std::wstring currentWtPath;
        if (GitLeafEq(gitDir, L".git"))
        {
            currentWtPath = GitParent(gitDir); // the typed path is in the MAIN worktree
        }
        else if (std::wstring gd = GitTrim(Utf8ToWide(ReadFileHead(gitDir + L"\\gitdir", 4096))); !gd.empty())
        {
            GitFlipSeps(gd);
            currentWtPath = GitLeafEq(gd, L".git") ? GitParent(gd) : gd; // strip the trailing \.git
        }

        // MAIN worktree = the parent of the common ".git" dir (skipped for a bare repo). HEAD is there.
        if (GitLeafEq(commonDir, L".git"))
        {
            if (std::wstring root = GitParent(commonDir); !root.empty())
            {
                GitWorktreeInfo w;
                w.path = root;
                w.name = GitLeaf(root);
                w.branch = ParseGitHead(Utf8ToWide(ReadFileHead(commonDir + L"\\HEAD", 4096)));
                w.isMain = true;
                out.push_back(std::move(w));
            }
        }

        // LINKED worktrees: each <commonDir>\worktrees\<id>\ names its working tree via a "gitdir"
        // file (-> <worktree>\.git) and its checked-out ref via HEAD.
        const std::wstring wtRoot = commonDir + L"\\worktrees";
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW((wtRoot + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    continue;
                }
                const std::wstring id = fd.cFileName;
                if (id == L"." || id == L"..")
                {
                    continue;
                }
                const std::wstring admin = wtRoot + L"\\" + id;
                std::wstring gd = GitTrim(Utf8ToWide(ReadFileHead(admin + L"\\gitdir", 4096)));
                if (gd.empty())
                {
                    continue;
                }
                GitFlipSeps(gd);
                const std::wstring wtPath = GitLeafEq(gd, L".git") ? GitParent(gd) : gd; // strip the trailing \.git
                if (wtPath.empty())
                {
                    continue;
                }
                GitWorktreeInfo w;
                w.path = wtPath;
                w.name = GitLeaf(wtPath);
                if (w.name.empty())
                {
                    w.name = id;
                }
                w.branch = ParseGitHead(Utf8ToWide(ReadFileHead(admin + L"\\HEAD", 4096)));
                out.push_back(std::move(w));
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }

        // Main worktree pinned first; linked worktrees alphabetized by name (case-insensitive).
        std::sort(out.begin(), out.end(), [](const GitWorktreeInfo& a, const GitWorktreeInfo& b) {
            if (a.isMain != b.isMain)
            {
                return a.isMain;
            }
            return ::CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        for (auto& w : out)
        {
            w.isCurrent = !currentWtPath.empty() && GitPathEq(w.path, currentWtPath);
        }
        return out;
    }

    // ===== Codex (OpenAI Codex CLI) — observe-only enrichment (OBSERVER.md §19-Q3, Phase C1) ====
    // File-local helpers (internal linkage). They reuse the anon-namespace primitives above
    // (GlobTranscripts / ReadFileHead / Utf8ToWide / FileTimeToUnixMs / FirstLineTrim) and the
    // public TU functions (ExtractCwdFromTranscriptHead / PickNewestTranscript / EnvLookup /
    // ExtractCmdlineArg / TokenizeCmdline).

    // Our OWN process environment variable (CODEX_HOME / USERPROFILE). Empty if unset.
    static std::wstring GetOwnEnvW(const wchar_t* name)
    {
        const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0); // includes NUL; 0 == absent
        if (need == 0)
        {
            return {};
        }
        std::wstring v(need, L'\0');
        const DWORD got = ::GetEnvironmentVariableW(name, v.data(), need); // got == chars w/o NUL
        v.resize(got);
        return v;
    }

    static std::wstring TrimTrailingSlashes(std::wstring s)
    {
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
        {
            s.pop_back();
        }
        return s;
    }

    // A 36-char hyphenated UUID (8-4-4-4-12). Codex uses time-ordered UUIDv7, still this shape.
    static bool LooksLikeGuidStr(std::wstring_view s)
    {
        if (s.size() != 36)
        {
            return false;
        }
        for (size_t i = 0; i < 36; ++i)
        {
            const wchar_t c = s[i];
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

    static int64_t NowUnixMsSrc() // current unix ms (the startUnixMs == 0 day-dir anchor)
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        return FileTimeToUnixMs(ft);
    }

    static std::wstring PadNum(unsigned v, size_t width)
    {
        std::wstring s = std::to_wstring(v);
        while (s.size() < width)
        {
            s.insert(s.begin(), L'0');
        }
        return s;
    }

    // "YYYY\\MM\\DD" in LOCAL time for a unix-ms instant ("" on failure). Codex shards rollouts by
    // the LOCAL date (the filename uses local wall-clock; session_meta.timestamp is UTC), so the
    // day dir is derived in local time.
    static std::wstring CodexDayDirLocal(int64_t unixMs)
    {
        if (unixMs <= 0)
        {
            return {};
        }
        ULARGE_INTEGER u;
        u.QuadPart = static_cast<uint64_t>(unixMs) * 10000ull + 116444736000000000ull; // unix ms -> FILETIME (100ns since 1601)
        FILETIME ut;
        ut.dwLowDateTime = u.LowPart;
        ut.dwHighDateTime = u.HighPart;
        FILETIME lt{};
        if (!::FileTimeToLocalFileTime(&ut, &lt))
        {
            return {};
        }
        SYSTEMTIME st{};
        if (!::FileTimeToSystemTime(&lt, &st))
        {
            return {};
        }
        return PadNum(st.wYear, 4) + L"\\" + PadNum(st.wMonth, 2) + L"\\" + PadNum(st.wDay, 2);
    }

    // Filesystem-aware cwd compare (Windows): normalize '/'->'\\', trim trailing separators, then
    // ordinal case-insensitive. The codex PEB cwd ("K:\\source\\proxmox") vs session_meta.cwd.
    static bool CodexPathEq(std::wstring_view a, std::wstring_view b)
    {
        const auto norm = [](std::wstring_view s) {
            std::wstring r;
            r.reserve(s.size());
            for (const wchar_t c : s)
            {
                r.push_back(c == L'/' ? L'\\' : c);
            }
            while (!r.empty() && r.back() == L'\\')
            {
                r.pop_back();
            }
            return r;
        };
        const std::wstring na = norm(a), nb = norm(b);
        return ::CompareStringOrdinal(na.c_str(), static_cast<int>(na.size()), nb.c_str(), static_cast<int>(nb.size()), TRUE) == CSTR_EQUAL;
    }

    // The immediate subdirectory NAMES of `dir` (no "." / ".."). Used to walk the year/month/day
    // shards when resolving a rollout by a known uuid.
    static std::vector<std::wstring> ListSubdirs(const std::wstring& dir)
    {
        std::vector<std::wstring> out;
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            out.push_back(name);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return out;
    }

    std::wstring CodexDefaultHome()
    {
        std::wstring base = GetOwnEnvW(L"CODEX_HOME");
        if (base.empty())
        {
            const std::wstring home = GetOwnEnvW(L"USERPROFILE");
            if (home.empty())
            {
                return {};
            }
            base = home + L"\\.codex";
        }
        return TrimTrailingSlashes(std::move(base));
    }

    void ParseCodexFacts(std::wstring_view commandline, const std::unordered_map<std::wstring, std::wstring>& env, CodexProcessFacts& facts)
    {
        facts.wtSession = EnvLookup(env, L"WT_SESSION");
        facts.amSession = EnvLookup(env, L"AM_SESSION");
        facts.codexHome = EnvLookup(env, L"CODEX_HOME");

        // model: --model X / -m X (often absent — config.toml carries it; read from the rollout then)
        if (const auto v = ExtractCmdlineArg(commandline, L"--model"))
        {
            facts.model = *v;
        }
        if (facts.model.empty())
        {
            if (const auto v = ExtractCmdlineArg(commandline, L"-m"))
            {
                facts.model = *v;
            }
        }
        // sandbox: --sandbox X / -s X
        if (const auto v = ExtractCmdlineArg(commandline, L"--sandbox"))
        {
            facts.sandbox = *v;
        }
        if (facts.sandbox.empty())
        {
            if (const auto v = ExtractCmdlineArg(commandline, L"-s"))
            {
                facts.sandbox = *v;
            }
        }
        // approval: --ask-for-approval X / -a X
        if (const auto v = ExtractCmdlineArg(commandline, L"--ask-for-approval"))
        {
            facts.approvalMode = *v;
        }
        if (facts.approvalMode.empty())
        {
            if (const auto v = ExtractCmdlineArg(commandline, L"-a"))
            {
                facts.approvalMode = *v;
            }
        }
        // explicit `codex resume <guid>` (authoritative id — beats cwd->rollout discovery): the
        // first non-flag token after a `resume` token, when it is a guid.
        const auto toks = TokenizeCmdline(commandline);
        for (size_t i = 0; i + 1 < toks.size(); ++i)
        {
            if (toks[i] == L"resume")
            {
                for (size_t j = i + 1; j < toks.size(); ++j)
                {
                    if (!toks[j].empty() && toks[j][0] == L'-')
                    {
                        continue; // skip flags (--last / --all / -C ...)
                    }
                    if (LooksLikeGuidStr(toks[j]))
                    {
                        facts.resumeTarget = toks[j];
                    }
                    break;
                }
                break;
            }
        }
    }

    CodexProcessFacts ReadCodexFacts(uint32_t pid)
    {
        CodexProcessFacts f;
        f.pid = pid;
        f.startUnixMs = ProcessStartUnixMs(pid);
        f.cwd = ReadProcessCwd(pid);
        f.commandline = ReadProcessCommandLine(pid);
        f.subsystem = ReadProcessImageSubsystem(pid);
        const auto env = ReadProcessEnv(pid);
        ParseCodexFacts(f.commandline, env, f);
        f.alive = true;
        return f;
    }

    std::wstring CodexRolloutUuid(std::wstring_view rolloutStem)
    {
        if (rolloutStem.size() < 36)
        {
            return {};
        }
        const std::wstring_view tail = rolloutStem.substr(rolloutStem.size() - 36);
        return LooksLikeGuidStr(tail) ? std::wstring{ tail } : std::wstring{};
    }

    CodexSession ResolveCodexSessionIn(std::wstring_view codexHome, std::wstring_view cwd, int64_t startUnixMs)
    {
        CodexSession out;
        if (codexHome.empty() || cwd.empty())
        {
            return out;
        }
        const std::wstring sessions = std::wstring{ codexHome } + L"\\sessions";
        const int64_t anchor = startUnixMs > 0 ? startUnixMs : NowUnixMsSrc();

        // Candidate day dirs: the start's local day ± 1 (midnight boundary / small clock skew).
        std::vector<std::wstring> days;
        for (const int delta : { -1, 0, 1 })
        {
            const std::wstring d = CodexDayDirLocal(anchor + static_cast<int64_t>(delta) * 86400000ll);
            if (!d.empty() && std::find(days.begin(), days.end(), d) == days.end())
            {
                days.push_back(d);
            }
        }

        // Gather rollout candidates (uuid + ctime/mtime + path) across those day dirs. The uuid is in
        // the filename, so no read is needed for the id — only the cwd confirmation below reads.
        struct Cand
        {
            std::wstring uuid;
            std::wstring path;
            int64_t ctime{};
            int64_t mtime{};
        };
        std::vector<Cand> cands;
        for (const auto& day : days)
        {
            const std::wstring dir = sessions + L"\\" + day;
            for (const auto& tc : GlobTranscripts(dir))
            {
                const std::wstring uuid = CodexRolloutUuid(tc.stem);
                if (uuid.empty())
                {
                    continue;
                }
                cands.push_back({ uuid, dir + L"\\" + tc.stem + L".jsonl", tc.ctimeMs, tc.mtimeMs });
            }
        }
        if (cands.empty())
        {
            return out;
        }

        // Newest-mtime first, then bound the cwd-confirm head reads (the date dir mixes all cwds, so
        // each candidate must be confirmed against the target cwd before the start-time pick).
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.mtime > b.mtime; });
        constexpr size_t kMaxConfirm = 64;
        if (cands.size() > kMaxConfirm)
        {
            cands.resize(kMaxConfirm);
        }

        std::vector<TranscriptCandidate> cwdCands;
        std::unordered_map<std::wstring, Cand> byUuid;
        for (const auto& c : cands)
        {
            const std::string head = ReadFileHead(c.path, 4096); // session_meta is line 1; cwd precedes the bulky base_instructions
            if (head.empty())
            {
                continue;
            }
            const std::wstring rcwd = ExtractCwdFromTranscriptHead(Utf8ToWide(head));
            if (rcwd.empty() || !CodexPathEq(rcwd, cwd))
            {
                continue;
            }
            TranscriptCandidate t;
            t.stem = c.uuid;
            t.ctimeMs = c.ctime;
            t.mtimeMs = c.mtime;
            cwdCands.push_back(t);
            byUuid[c.uuid] = c;
        }
        if (cwdCands.empty())
        {
            return out;
        }

        // ctime ≈ start IDENTITY for a FRESH session (rejects rollouts created before this process);
        // newest-mtime-in-cwd FALLBACK for a RESUMED one (its rollout predates the process, so the
        // identity path rejects it). Mirrors PickNewestTranscript's two modes.
        std::wstring pick = PickNewestTranscript(cwdCands, startUnixMs);
        if (pick.empty())
        {
            pick = PickNewestTranscript(cwdCands, 0);
        }
        if (pick.empty())
        {
            return out;
        }
        const auto it = byUuid.find(pick);
        if (it == byUuid.end())
        {
            return out;
        }
        out.sessionId = it->second.uuid;
        out.rolloutPath = it->second.path;
        out.createdUnixMs = it->second.ctime;
        out.lastActivityUnixMs = it->second.mtime;
        return out;
    }

    CodexSession ResolveCodexSession(std::wstring_view cwd, int64_t startUnixMs)
    {
        return ResolveCodexSessionIn(CodexDefaultHome(), cwd, startUnixMs);
    }

    std::wstring ResolveCodexRolloutPathIn(std::wstring_view codexHome, std::wstring_view sessionId)
    {
        if (codexHome.empty() || sessionId.empty() || !LooksLikeGuidStr(sessionId))
        {
            return {};
        }
        const std::wstring sessions = std::wstring{ codexHome } + L"\\sessions";
        const std::wstring needle = L"*" + std::wstring{ sessionId } + L".jsonl";
        // Walk year\month\day. The uuid is unique, so the first match is THE rollout. (The explicit
        // `codex resume <guid>` path; the cwd-discovery path above carries the path directly.)
        for (const auto& year : ListSubdirs(sessions))
        {
            const std::wstring ydir = sessions + L"\\" + year;
            for (const auto& month : ListSubdirs(ydir))
            {
                const std::wstring mdir = ydir + L"\\" + month;
                for (const auto& day : ListSubdirs(mdir))
                {
                    const std::wstring ddir = mdir + L"\\" + day;
                    WIN32_FIND_DATAW fd{};
                    const HANDLE h = ::FindFirstFileW((ddir + L"\\" + needle).c_str(), &fd);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        std::wstring found;
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                        {
                            found = ddir + L"\\" + fd.cFileName;
                        }
                        ::FindClose(h);
                        if (!found.empty())
                        {
                            return found;
                        }
                    }
                }
            }
        }
        return {};
    }

    void ParseCodexRolloutText(std::wstring_view text, bool truncated, size_t maxPrompts, CodexRolloutInfo& out)
    {
        std::wstring firstPrompt;
        bool haveTurnCtx = false;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            if (i < text.size() && text[i] != L'\n')
            {
                continue;
            }
            if (i == text.size() && truncated)
            {
                break; // a head read may end mid-line — leave the partial segment for a fuller read
            }
            std::wstring_view line = text.substr(start, i - start);
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
            const std::wstring lineType = obj.StrAt(L"type");
            const auto* pl = obj.Find(L"payload");
            if (!pl || pl->type != json::Value::Type::Obj)
            {
                continue;
            }
            if (lineType == L"session_meta")
            {
                if (out.cwd.empty())
                {
                    out.cwd = pl->StrAt(L"cwd");
                }
                if (out.gitBranch.empty())
                {
                    if (const auto* git = pl->Find(L"git"); git && git->type == json::Value::Type::Obj)
                    {
                        out.gitBranch = git->StrAt(L"branch"); // best-effort (codex may record git info)
                    }
                }
                continue;
            }
            if (lineType == L"turn_context")
            {
                if (!haveTurnCtx) // the FIRST turn_context carries the session's model/effort/sandbox/approval
                {
                    haveTurnCtx = true;
                    out.model = pl->StrAt(L"model");
                    out.approvalMode = pl->StrAt(L"approval_policy");
                    if (const auto* sp = pl->Find(L"sandbox_policy"); sp && sp->type == json::Value::Type::Obj)
                    {
                        out.sandbox = sp->StrAt(L"type");
                    }
                    else
                    {
                        out.sandbox = pl->StrAt(L"sandbox_mode"); // flat fallback
                    }
                    std::wstring eff;
                    if (const auto* cm = pl->Find(L"collaboration_mode"); cm && cm->type == json::Value::Type::Obj)
                    {
                        if (const auto* se = cm->Find(L"settings"); se && se->type == json::Value::Type::Obj)
                        {
                            eff = se->StrAt(L"reasoning_effort");
                        }
                    }
                    if (eff.empty())
                    {
                        eff = pl->StrAt(L"reasoning_effort");
                    }
                    if (eff.empty())
                    {
                        eff = pl->StrAt(L"model_reasoning_effort");
                    }
                    out.effort = eff;
                }
                continue;
            }
            if (lineType == L"event_msg" && pl->StrAt(L"type") == L"user_message")
            {
                // event_msg/user_message is the CLEAN human prompt (the AGENTS.md / context blobs are
                // response_item user messages, skipped). Noise-filter via the shared rule.
                std::wstring msg = pl->StrAt(L"message");
                if (msg.empty() || IsNoiseUserPrompt(msg))
                {
                    continue;
                }
                if (firstPrompt.empty())
                {
                    firstPrompt = msg;
                }
                if (out.userPrompts.size() < maxPrompts)
                {
                    out.userPrompts.push_back(std::move(msg));
                }
            }
        }
        out.title = FirstLineTrim(firstPrompt);
    }

    static CodexRolloutInfo ReadCodexRolloutInfoImpl(std::wstring_view rolloutPath, size_t maxBytes, size_t maxPrompts);
    // Agentmaster (extra-safe): never let a rollout-parse throw escape into a background coroutine (would
    // std::terminate the app) -- contain it and return an empty result. See AnalyzeSessionTranscript.
    CodexRolloutInfo ReadCodexRolloutInfo(std::wstring_view rolloutPath, size_t maxBytes, size_t maxPrompts)
    {
        try
        {
            return ReadCodexRolloutInfoImpl(rolloutPath, maxBytes, maxPrompts);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] ReadCodexRolloutInfo: swallowed parse exception (no crash)\n");
            return {};
        }
    }
    static CodexRolloutInfo ReadCodexRolloutInfoImpl(std::wstring_view rolloutPath, size_t maxBytes, size_t maxPrompts)
    {
        CodexRolloutInfo info;
        if (rolloutPath.empty())
        {
            return info;
        }
        const std::wstring path{ rolloutPath };
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return info; // not found
        }
        info.found = true;
        info.createdUnixMs = FileTimeToUnixMs(fad.ftCreationTime);
        info.lastActivityUnixMs = FileTimeToUnixMs(fad.ftLastWriteTime);
        const std::string bytes = ReadFileHead(path, maxBytes);
        if (bytes.empty())
        {
            return info; // times only
        }
        ParseCodexRolloutText(Utf8ToWide(bytes), maxBytes != 0, maxPrompts, info);
        return info;
    }

}
