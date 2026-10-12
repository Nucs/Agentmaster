// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (SESSIONS.md section 10 - the Sessions page's FOLDER FILTER): the pure half of the
// "Folders" dropdown in the Sessions browser's filters row.
//
// WHAT the user asked for (2026-10-12): a folder filter dropdown in the Sessions search. Its list is
// split 50% / 50%: the first half holds the folders of the RECENT sessions, the second half the MOST
// COMMONLY USED folders over all the listed sessions. A folder is where a session WORKED - its launch
// folder (the transcript's cwd) or its INFERRED folder (where its tool calls concentrate,
// InferWorkingDirectory). The user adds one or more folders; the picked folders combine with OR (a
// session in ANY of them passes), and the folder filter as a whole ANDs with every other filter.
//
// WHAT lives here (no WinRT, no file IO except through an injected probe):
//   - SessionFolderFacts / SessionFolderUse - one session's folders in, one folder's usage out.
//   - CollectSessionFolders - the folder universe of a session list: every distinct folder (keyed
//     by NormDirKey, Rule #8) with how many sessions worked there, how many were launched there,
//     and the newest activity among them.
//   - SortFoldersByRecency / SortFoldersByUse - the two halves' orders.
//   - SplitFolderFindTerms / FolderMatchesFindTerms - the dropdown's find box (AND of substrings).
//   - IsAbsoluteFolderPath / TrimFolderPath - what a typed path must look like to be added as is.
//   - FolderKeyCovers / SessionInPickedFolders - the filter predicate (exact folder, or the folder
//     and everything under it when "Include subfolders" is on).
//   - GitRootMemo - FindGitRootForDir's walk with a memo per ANCESTOR, the git arm of the inference
//     pass that gives every listed session its inferred folder (a month of sessions is ~4,600
//     distinct parent folders; measured 2026-10-12: 6,442 probes, 6.6 s with a cold disk cache).
//
// Header-only and pure-ASCII source (the PointerCardPlacement.h idiom), so the m5 harness tests it
// without a build of the app: tests_transcript.cpp, TestSessionFolderFilter.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Persistence.h" // NormDirKey - the ONE filesystem-aware directory key (Rule #8)

namespace Agentmaster
{
    // One listed session's folder facts - the input of CollectSessionFolders.
    struct SessionFolderFacts
    {
        // The folder the session was launched in (the transcript's cwd). A value that is not an
        // absolute folder path (the page's fallback to the encoded project-folder name when no line
        // carried a cwd) contributes nothing: it names no real folder a user could pick.
        std::wstring workingDir;
        // Where the session's tool calls concentrate (InferWorkingDirectory). EMPTY when it is not
        // known yet or when it is the same folder as workingDir; the same folder is never counted
        // twice for one session either way.
        std::wstring inferredDir;
        // The session's last real activity (Unix ms) - what orders the RECENT half.
        int64_t lastActivityMs{};
    };

    // One folder of the dropdown: its spelling, its key and how the listed sessions used it.
    struct SessionFolderUse
    {
        std::wstring path; // the display spelling: the first one seen, separators as '\', trailing separators trimmed
        std::wstring key; // NormDirKey(path) - the identity every compare uses (case / slash variants collapse)
        int sessions{}; // distinct sessions that worked here (launched here, or inferred here)
        int launched{}; // ...of which were LAUNCHED here (their workingDir is this folder)
        int inferredOnly{}; // ...of which only their INFERRED folder is this one (launched somewhere else)
        int64_t lastActivityMs{}; // the newest activity among those sessions (Unix ms; 0 = unknown)
    };

    // True for a path a user can pick as a folder on its own: a drive path with a separator ("K:\x",
    // "K:/x", the drive root "K:\") or a UNC path with a server and a share ("\\server\share[\...]").
    // False for a drive-relative "K:", a relative path, a POSIX-absolute "/k/x" (no drive or share to
    // anchor on - the same rule ExtractPathsFromText applies) and the device namespaces "\\?\" and
    // "\\.\", which no session cwd carries.
    //   path    - the candidate, as typed or as stored; surrounding whitespace is NOT trimmed here.
    //   returns - true when the path is anchored to a drive or a share.
    //   throws  - nothing.
    inline bool IsAbsoluteFolderPath(std::wstring_view path) noexcept
    {
        const auto isSep = [](wchar_t c) noexcept { return c == L'\\' || c == L'/'; };
        if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && isSep(path[2]))
        {
            return true;
        }
        if (path.size() < 5 || !isSep(path[0]) || !isSep(path[1]))
        {
            return false;
        }
        // "\\server\share": a non-empty server, a separator, a non-empty share.
        size_t serverEnd = 2;
        while (serverEnd < path.size() && !isSep(path[serverEnd]))
        {
            ++serverEnd;
        }
        const std::wstring_view server = path.substr(2, serverEnd - 2);
        if (server.empty() || server == L"?" || server == L"." || serverEnd + 1 >= path.size())
        {
            return false; // no server, a device namespace, or nothing after the server's separator
        }
        return !isSep(path[serverEnd + 1]); // the share name starts right after the separator
    }

    // The display form of a folder path: surrounding whitespace and ONE pair of matching quotes removed
    // (a path pasted from a shell often carries them), every '/' turned into '\', and trailing
    // separators trimmed - except the one that makes a drive root ("K:\" stays "K:\"; NormDirKey keeps
    // it the same way, so the display form and the key agree on what a root is).
    //   raw     - the text as typed or stored.
    //   returns - the trimmed spelling; empty when nothing but whitespace / quotes was given.
    //   throws  - std::bad_alloc.
    inline std::wstring TrimFolderPath(std::wstring_view raw)
    {
        const auto isSpace = [](wchar_t c) noexcept { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        const auto trimSpaces = [&](std::wstring_view v) {
            while (!v.empty() && isSpace(v.front()))
            {
                v.remove_prefix(1);
            }
            while (!v.empty() && isSpace(v.back()))
            {
                v.remove_suffix(1);
            }
            return v;
        };
        std::wstring_view v = trimSpaces(raw);
        if (v.size() >= 2 && (v.front() == L'"' || v.front() == L'\'') && v.back() == v.front())
        {
            v = trimSpaces(v.substr(1, v.size() - 2)); // "C:\Program Files\x" pasted with its quotes
        }
        std::wstring s{ v };
        for (auto& c : s)
        {
            if (c == L'/')
            {
                c = L'\\';
            }
        }
        // Keep the separator of a drive root: "K:\" is a folder, "K:" (drive-relative) is not.
        while (s.size() > 3 && s.back() == L'\\')
        {
            s.pop_back();
        }
        return s;
    }

    // The folder universe of a session list - every distinct folder any session worked in, with its
    // usage. A session counts for its launch folder AND, when it differs, for its inferred folder, so a
    // session launched in the home folder that worked in a repo appears under both (it worked in both
    // places). Each session counts at most ONCE per folder. Folders that are not absolute paths are
    // skipped (IsAbsoluteFolderPath). The result is in first-seen order; sort it with
    // SortFoldersByRecency / SortFoldersByUse.
    //   sessions - the listed sessions' folder facts (the caller leaves out what the list does not
    //              show, e.g. hidden sessions).
    //   returns  - one SessionFolderUse per distinct NormDirKey.
    //   throws   - std::bad_alloc.
    inline std::vector<SessionFolderUse> CollectSessionFolders(const std::vector<SessionFolderFacts>& sessions)
    {
        std::vector<SessionFolderUse> out;
        std::unordered_map<std::wstring, size_t> at; // key -> index into out
        const auto count = [&](const std::wstring& raw, bool launched, int64_t lastActivityMs) -> std::wstring {
            const std::wstring path = TrimFolderPath(raw);
            std::wstring key = NormDirKey(path);
            auto [it, isNew] = at.try_emplace(key, out.size());
            if (isNew)
            {
                SessionFolderUse f;
                f.path = path;
                f.key = key;
                out.push_back(std::move(f));
            }
            auto& f = out[it->second];
            ++f.sessions;
            if (launched)
            {
                ++f.launched;
            }
            else
            {
                ++f.inferredOnly;
            }
            f.lastActivityMs = (std::max)(f.lastActivityMs, lastActivityMs); // parenthesized: <windows.h> without NOMINMAX (the m5 harness) defines a max() macro
            return key;
        };
        for (const auto& s : sessions)
        {
            std::wstring launchKey;
            if (IsAbsoluteFolderPath(s.workingDir))
            {
                launchKey = count(s.workingDir, true, s.lastActivityMs);
            }
            if (IsAbsoluteFolderPath(s.inferredDir) && NormDirKey(TrimFolderPath(s.inferredDir)) != launchKey)
            {
                count(s.inferredDir, false, s.lastActivityMs);
            }
        }
        return out;
    }

    // The RECENT half's order: the folder whose newest session was active most recently first. Ties
    // (equal activity, e.g. 0 = unknown) fall back to more sessions first, then the key, so the order is
    // stable from one render to the next.
    //   folders - sorted in place.
    //   throws  - nothing beyond what std::sort's swaps of std::wstring can throw (none in practice).
    inline void SortFoldersByRecency(std::vector<SessionFolderUse>& folders)
    {
        std::sort(folders.begin(), folders.end(), [](const SessionFolderUse& a, const SessionFolderUse& b) {
            if (a.lastActivityMs != b.lastActivityMs)
            {
                return a.lastActivityMs > b.lastActivityMs;
            }
            if (a.sessions != b.sessions)
            {
                return a.sessions > b.sessions;
            }
            return a.key < b.key;
        });
    }

    // The MOST USED half's order: the folder more sessions worked in first. Ties fall back to the more
    // recent folder, then the key (stable from one render to the next).
    //   folders - sorted in place.
    //   throws  - nothing beyond what std::sort's swaps of std::wstring can throw (none in practice).
    inline void SortFoldersByUse(std::vector<SessionFolderUse>& folders)
    {
        std::sort(folders.begin(), folders.end(), [](const SessionFolderUse& a, const SessionFolderUse& b) {
            if (a.sessions != b.sessions)
            {
                return a.sessions > b.sessions;
            }
            if (a.lastActivityMs != b.lastActivityMs)
            {
                return a.lastActivityMs > b.lastActivityMs;
            }
            return a.key < b.key;
        });
    }

    // The dropdown find box's terms: the text split at whitespace, each term folded exactly like a folder
    // key (NormDirKey: lower case, '/' -> '\'), so "source/Agent" finds "K:\source\Agentmaster". Quotes
    // are not special - a path with a space splits into two terms that both still match its key.
    //   text    - the find box's text.
    //   returns - the folded terms; empty for an empty / whitespace-only text (matches every folder).
    //   throws  - std::bad_alloc.
    inline std::vector<std::wstring> SplitFolderFindTerms(std::wstring_view text)
    {
        std::vector<std::wstring> terms;
        size_t i = 0;
        while (i < text.size())
        {
            while (i < text.size() && (text[i] == L' ' || text[i] == L'\t' || text[i] == L'\r' || text[i] == L'\n'))
            {
                ++i;
            }
            const size_t start = i;
            while (i < text.size() && !(text[i] == L' ' || text[i] == L'\t' || text[i] == L'\r' || text[i] == L'\n'))
            {
                ++i;
            }
            if (i > start)
            {
                std::wstring term = NormDirKey(std::wstring{ text.substr(start, i - start) });
                if (!term.empty())
                {
                    terms.push_back(std::move(term));
                }
            }
        }
        return terms;
    }

    // Whether a folder passes the find box: EVERY term is a substring of its key (AND, like the page's
    // own search box). No terms = every folder passes.
    //   folder  - the folder to test (its key is already folded).
    //   terms   - SplitFolderFindTerms' output.
    //   returns - true when the folder stays in the lists.
    //   throws  - nothing.
    inline bool FolderMatchesFindTerms(const SessionFolderUse& folder, const std::vector<std::wstring>& terms) noexcept
    {
        for (const auto& t : terms)
        {
            if (folder.key.find(t) == std::wstring::npos)
            {
                return false;
            }
        }
        return true;
    }

    // Whether a picked folder covers a session's folder. Exact: the two keys are equal. With
    // `includeSubfolders`, also any folder BELOW the picked one, matched at a SEGMENT boundary -
    // "K:\src" covers "K:\src\app" but never "K:\srcfoo" (the ExcludePathsUnderRoots rule). A picked
    // drive root ("k:\", whose key keeps its separator) covers the whole drive.
    //   folderKey         - NormDirKey of the picked folder.
    //   dirKey            - NormDirKey of the session's folder (launch or inferred); empty never matches.
    //   includeSubfolders - the dropdown's "Include subfolders" check box.
    //   returns           - true when the session's folder is the picked folder (or under it).
    //   throws            - nothing.
    inline bool FolderKeyCovers(const std::wstring& folderKey, const std::wstring& dirKey, bool includeSubfolders) noexcept
    {
        if (folderKey.empty() || dirKey.empty())
        {
            return false;
        }
        if (dirKey == folderKey)
        {
            return true;
        }
        if (!includeSubfolders || dirKey.size() <= folderKey.size() || dirKey.compare(0, folderKey.size(), folderKey) != 0)
        {
            return false;
        }
        const auto isSep = [](wchar_t c) noexcept { return c == L'\\' || c == L'/'; };
        return isSep(folderKey.back()) || isSep(dirKey[folderKey.size()]);
    }

    // The folder filter's predicate for one session: true when ANY picked folder covers the session's
    // launch folder OR its inferred folder (OR between the picked folders - the user's rule). No picked
    // folder = no constraint (true). Keys are computed here, per call: the page calls this once per row
    // and render with a handful of picks, a few hundred NormDirKey calls in all.
    //   pickedFolders     - the picked folders (display spellings; folded here).
    //   includeSubfolders - the dropdown's "Include subfolders" check box.
    //   workingDir        - the session's launch folder.
    //   inferredDir       - the session's inferred folder; empty = not known yet or the same folder.
    //   returns           - true when the session passes the folder filter.
    //   throws            - std::bad_alloc.
    inline bool SessionInPickedFolders(const std::vector<std::wstring>& pickedFolders,
                                       bool includeSubfolders,
                                       const std::wstring& workingDir,
                                       const std::wstring& inferredDir)
    {
        if (pickedFolders.empty())
        {
            return true;
        }
        const std::wstring launchKey = workingDir.empty() ? std::wstring{} : NormDirKey(workingDir);
        const std::wstring inferredKey = inferredDir.empty() ? std::wstring{} : NormDirKey(inferredDir);
        for (const auto& picked : pickedFolders)
        {
            const std::wstring pickedKey = NormDirKey(picked);
            if (FolderKeyCovers(pickedKey, launchKey, includeSubfolders) || FolderKeyCovers(pickedKey, inferredKey, includeSubfolders))
            {
                return true;
            }
        }
        return false;
    }

    // FindGitRootForDir's answer - the nearest ancestor (the folder itself included) that holds a ".git"
    // entry - with every probed ANCESTOR memoized, for a pass that resolves thousands of folders sharing
    // ancestor chains (the Sessions page's inference pass: every listed session's tool paths). The walk
    // and its bounds are FindGitRootForDir's (TranscriptStore.cpp): separators normalized, trailing ones
    // trimmed, never probing the drive / UNC-share root itself, relative or rootless input = "" with no
    // probe. Only the probe is injected, so the memo logic is testable without a file system.
    // Not thread-safe: one instance per pass, used from one thread.
    class GitRootMemo
    {
    public:
        // The injected test for ONE folder: true when `dir` holds a ".git" entry (a directory for a
        // normal checkout, a file for a worktree / submodule). The real one is one GetFileAttributesW.
        using Probe = std::function<bool(const std::wstring& dir)>;

        // probe - the ".git" test; an empty function answers "no repo" everywhere (never called).
        explicit GitRootMemo(Probe probe) :
            _probe(std::move(probe))
        {
        }

        // The git root that encloses `dir`, FindGitRootForDir's contract (see the class comment).
        //   dir     - any folder spelling (case / slash variants share one memo entry).
        //   returns - the root's spelling as first resolved; "" when no ancestor below the drive / share
        //             root holds a ".git" entry, or when `dir` is relative / rootless.
        //   throws  - std::bad_alloc, and whatever the injected probe throws (the real one never does).
        std::wstring RootOf(const std::wstring& dir)
        {
            std::wstring p = dir;
            for (auto& c : p)
            {
                if (c == L'/')
                {
                    c = L'\\';
                }
            }
            while (!p.empty() && p.back() == L'\\')
            {
                p.pop_back();
            }
            size_t rootEnd = 0; // end of the root token ("K:" or "\\server\share") - never probed
            if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
            {
                const size_t serverSep = p.find(L'\\', 2);
                const size_t shareSep = serverSep == std::wstring::npos ? std::wstring::npos : p.find(L'\\', serverSep + 1);
                if (shareSep == std::wstring::npos)
                {
                    return {}; // just "\\server" / "\\server\share": nothing below a root to probe
                }
                rootEnd = shareSep;
            }
            else if (p.size() >= 2 && p[1] == L':')
            {
                rootEnd = 2;
            }
            else
            {
                return {}; // relative / rootless: no anchored ancestor chain
            }
            // Walk up, stopping at the first memo hit or ".git" hit. Every folder visited below the
            // answer shares that answer (none of them holds a ".git" of its own, or the walk would
            // have stopped there), so all of them are memoized with it - the next path under any of
            // them costs no probe at all.
            std::vector<std::wstring> visited;
            std::wstring answer;
            while (p.size() > rootEnd + 1)
            {
                std::wstring key = NormDirKey(p);
                if (const auto it = _memo.find(key); it != _memo.end())
                {
                    answer = it->second;
                    break;
                }
                ++_probes;
                const bool isRoot = _probe && _probe(p);
                visited.push_back(std::move(key));
                if (isRoot)
                {
                    answer = p;
                    break;
                }
                const size_t lastSep = p.find_last_of(L'\\');
                if (lastSep == std::wstring::npos || lastSep <= rootEnd)
                {
                    break; // the next cut would be the root token itself
                }
                p.resize(lastSep);
            }
            for (auto& key : visited)
            {
                _memo.emplace(std::move(key), answer);
            }
            return answer;
        }

        // How many folders were probed so far (each memo miss costs exactly one). Read for the pass's
        // log line, and by the tests to prove the memo works.
        //   returns - the probe count since construction.
        //   throws  - nothing.
        size_t Probes() const noexcept
        {
            return _probes;
        }

    private:
        Probe _probe; // the injected ".git" test
        std::unordered_map<std::wstring, std::wstring> _memo; // NormDirKey(folder) -> its git root ("" = none)
        size_t _probes{ 0 }; // memo misses so far
    };
}
