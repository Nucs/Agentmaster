// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Claude Code BACKGROUND SESSIONS (OBSERVER.md §21): the pure rules the Fleet Observer, the spawn builders and
// the side panel share about the conversations Claude Code runs in its own daemon.
//
// Claude Code 2.1.322 can run a conversation in a BACKGROUND SESSION: a `claude.exe` that its daemon (`claude daemon run`)
// hosts on a pseudo console of its own (`claude --bg-pty-host <pipe> <cols> <rows> -- claude ...`), with no terminal of the
// user's around it. Such a process writes a presence row (`~/.claude/sessions/<pid>.json`) of `kind` "bg" with a short
// `jobId` (the first 8 hex digits of its session id), a `name`, and its own `status` heartbeat. It is how three built-in
// commands work, and each of them moves a tab's conversation where the tab-keyed binding cannot follow it:
//   * `/fork <task>` (fleet on) — runs <task> in a NEW background session forked from the tab's conversation, AND moves the
//     tab's own conversation into a background session too (a `--fork-session` copy under a NEW id, the original's
//     transcript ending in a `continued-in` line), whose job the window then shows: its presence row keeps the OLD id and
//     names the job in `parkedJobId` ("session X, moved to the background from this window").
//   * `/background` (`/bg`) — moves the tab's conversation into a background session the same way.
//   * `claude attach <id|name>` — opens a background session in the terminal it runs in.
// Every hook of a background session arrives WITHOUT the tab's WT_SESSION (Claude Code drops that variable for the
// daemon's children and keeps every other one, AM_SESSION included), so the hook adoption, which binds by WT_SESSION, can
// never bind it: before this header, the tab kept a conversation that no longer moved, and the moved one showed as a
// tab-less card titled with its GUID. The observer now asks these rules which background job a tab shows, and which
// background sessions no tab shows, so those get a card of their own (titled by Claude Code's name, sticky below the card
// of the session they came from, and opened in a new tab by `claude attach`).
//
// Header-only and pure (no Win32, no WinRT, no SessionModels.h): the m5 harness pins every rule (tests_background.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // The presence `kind` of a session the Claude Code daemon hosts (the other kinds: "interactive" — a terminal's own claude —
    // "daemon" and "daemon-worker", which run no conversation). Compared exactly, as Claude Code writes it.
    inline constexpr std::wstring_view kPresenceKindBackground = L"bg";

    // What the Fleet Observer knows about ONE live background session of ours, from its validated presence row and the
    // process facts behind it. Built fresh every survey; nothing here is persisted.
    struct BackgroundJob
    {
        // Claude Code's short job id (the presence row's `jobId`, e.g. "7c488ed3"): the handle `claude attach <jobId>` takes
        // and the value a parked window names in its own row's `parkedJobId`.
        std::wstring jobId;
        // The conversation the job runs NOW (the row's `sessionId`); the registry key its record lives under.
        std::wstring sessionId;
        // The job's claude.exe (the process its hooks run in; its tool children hang below it, not below any tab).
        uint32_t pid{};
        // The job's own heartbeat (busy | idle | waiting | shell) — a display FACT, never SessionState (Rule #13).
        std::wstring status;
        // Why the job waits on the user, while status == "waiting" (e.g. "input needed"); "" otherwise.
        std::wstring waitingFor;
        // Claude Code's name for the session (the row's `name`: the first words of its task, a "⑂ " mark on a fork's task,
        // later an auto name Claude Code gives it, or one the user set with /rename). The title of a card no tab shows.
        std::wstring name;
        // The job's working folder (the row's `cwd`; a fork's task may run in a git worktree of the tab's folder).
        std::wstring cwd;
        // The conversation the job was made from (BackgroundParentFromLaunch): the tab conversation a `/fork` forked, or the
        // one a `/fork` / `/bg` moved into this job. "" when its launch names none. The side panel's sticky parent.
        std::wstring parentId;
    };

    // Claude Code's command-line verbs that run NO conversation of their own (the `claude <verb> ...` forms), plus the
    // daemon's pseudo-console host. A claude.exe running one of them sits in a tab like any other claude, so the observer
    // must not mistake it for a conversation: its transcript-age guess (Rule #14's last resort) would pick whichever
    // transcript appears next in its folder.
    enum class ClaudeCliVerb
    {
        None, // a conversation: `claude [flags] [prompt]`, `--resume`, `--continue`, `--session-id`, ...
        Attach, // `claude attach <id|name>` — shows a background session in this terminal
        Agents, // `claude agents` — the agent view: the list of background sessions
        Logs, // `claude logs <id>` — prints a background session's output
        Stop, // `claude stop <id>`
        Kill, // `claude kill <id>`
        Respawn, // `claude respawn <id>`
        Rm, // `claude rm <id>`
        Daemon, // `claude daemon ...` — the daemon itself (`daemon run`) or its management verbs
        PtyHost, // `claude --bg-pty-host <pipe> <cols> <rows> -- claude ...` — the daemon's pseudo console for one job
    };

    // A parsed claude command line: which verb it runs and the verb's first argument.
    struct ClaudeCliInvocation
    {
        ClaudeCliVerb verb{ ClaudeCliVerb::None };
        // The verb's first argument as Windows parses it (quotes removed): attach's <id|name>, a job verb's <id>, the pty
        // host's pipe name. "" when the command line has none.
        std::wstring target;
    };

    // ASCII case-insensitive equality of two ids (session ids, job ids, WT_SESSION tokens). Claude Code writes them in lower
    // case, but a user-typed `claude attach 7C488ED3` or a hand-edited file must still match.
    //   a, b    — the ids.
    //   returns — true when they are equal ignoring ASCII case. Never throws.
    inline bool BackgroundIdEquals(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            wchar_t x = a[i];
            wchar_t y = b[i];
            if (x >= L'A' && x <= L'Z')
            {
                x = static_cast<wchar_t>(x - L'A' + L'a');
            }
            if (y >= L'A' && y <= L'Z')
            {
                y = static_cast<wchar_t>(y - L'A' + L'a');
            }
            if (x != y)
            {
                return false;
            }
        }
        return true;
    }

    // ASCII lower case of an id (the registry keys conversations by Claude Code's lower-case GUID).
    //   s       — the id.
    //   returns — the id with A-Z folded to a-z; every other character as it was.
    //   throws  — std::bad_alloc only.
    inline std::wstring BackgroundIdLower(std::wstring_view s)
    {
        std::wstring out{ s };
        for (auto& c : out)
        {
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        return out;
    }

    // Is `s` a conversation id — the 36-character hyphenated GUID (8-4-4-4-12 hex digits) Claude Code names a session by?
    // Used to accept an id read from an environment variable, a file stem or a command line, where anything else can appear.
    //   s       — the candidate.
    //   returns — true for a well-formed GUID in either case. Never throws.
    inline bool LooksLikeConversationId(std::wstring_view s) noexcept
    {
        if (s.size() != 36)
        {
            return false;
        }
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
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

    // Split a Windows command line into its arguments the way the C runtime (CommandLineToArgvW) does: whitespace separates
    // arguments outside double quotes; a double quote toggles quoting and is dropped; 2n backslashes before a quote become
    // n backslashes and the quote toggles, 2n+1 become n backslashes and a literal quote; other backslashes are literal.
    // The FIRST argument (the program) follows the same rules here, which only differs from the runtime's own rule for a
    // program path containing a backslash right before a quote — never the case for a claude.exe path.
    //   commandline — the process's command line (the PEB's CommandLine).
    //   returns     — the arguments, the program first; empty for an empty or all-blank line.
    //   throws      — std::bad_alloc only.
    inline std::vector<std::wstring> SplitClaudeCommandLine(std::wstring_view commandline)
    {
        std::vector<std::wstring> out;
        std::wstring cur;
        bool inToken = false; // something (even an empty quoted pair) started the current argument
        bool quoted = false;
        size_t i = 0;
        while (i < commandline.size())
        {
            const wchar_t c = commandline[i];
            if (c == L'\\')
            {
                // Count the run of backslashes: their meaning depends on whether a quote follows them.
                size_t n = 0;
                while (i < commandline.size() && commandline[i] == L'\\')
                {
                    ++n;
                    ++i;
                }
                inToken = true;
                if (i < commandline.size() && commandline[i] == L'"')
                {
                    cur.append(n / 2, L'\\');
                    if (n % 2 == 1)
                    {
                        cur.push_back(L'"'); // an escaped quote is a literal character
                        ++i;
                    }
                    // An even run leaves the quote to the loop's next turn, which toggles the quoting.
                }
                else
                {
                    cur.append(n, L'\\'); // backslashes not before a quote are literal
                }
                continue;
            }
            if (c == L'"')
            {
                inToken = true;
                quoted = !quoted;
                ++i;
                continue;
            }
            if ((c == L' ' || c == L'\t') && !quoted)
            {
                if (inToken)
                {
                    out.push_back(std::move(cur));
                    cur.clear();
                    inToken = false;
                }
                ++i;
                continue;
            }
            cur.push_back(c);
            inToken = true;
            ++i;
        }
        if (inToken)
        {
            out.push_back(std::move(cur));
        }
        return out;
    }

    // Which Claude Code verb a claude.exe command line runs (ClaudeCliVerb), and the verb's first argument. A verb counts only
    // as the FIRST argument after the program — where Claude Code's own parser and every launch Agentmaster makes put it —
    // so a conversation whose prompt or resume path happens to contain "attach" is never read as one. The daemon's pty host
    // is recognized by its `--bg-pty-host` flag anywhere before the `--` that starts the hosted command line (the hosted
    // command's own arguments, after it, are not this process's).
    //   commandline — the claude.exe's command line (the PEB's CommandLine).
    //   returns     — the verb (None for a conversation) and its first argument ("" when there is none).
    //   throws      — std::bad_alloc only.
    inline ClaudeCliInvocation ParseClaudeCliInvocation(std::wstring_view commandline)
    {
        ClaudeCliInvocation out;
        const auto args = SplitClaudeCommandLine(commandline);
        if (args.size() < 2)
        {
            return out; // the program alone: a plain interactive conversation
        }
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == L"--")
            {
                break; // the hosted command line starts here: its flags are not this process's
            }
            if (args[i] == L"--bg-pty-host")
            {
                out.verb = ClaudeCliVerb::PtyHost;
                out.target = (i + 1 < args.size()) ? args[i + 1] : std::wstring{};
                return out;
            }
        }
        // Claude Code's verbs are lower case and exact (its own parser compares them so): "Attach" is a prompt, not a verb.
        struct VerbName
        {
            std::wstring_view name;
            ClaudeCliVerb verb;
        };
        static constexpr VerbName kVerbs[] = {
            { L"attach", ClaudeCliVerb::Attach },
            { L"agents", ClaudeCliVerb::Agents },
            { L"logs", ClaudeCliVerb::Logs },
            { L"stop", ClaudeCliVerb::Stop },
            { L"kill", ClaudeCliVerb::Kill },
            { L"respawn", ClaudeCliVerb::Respawn },
            { L"rm", ClaudeCliVerb::Rm },
            { L"daemon", ClaudeCliVerb::Daemon },
        };
        for (const auto& v : kVerbs)
        {
            if (args[1] == v.name)
            {
                out.verb = v.verb;
                out.target = (args.size() > 2) ? args[2] : std::wstring{};
                return out;
            }
        }
        return out;
    }

    // Does this verb run a conversation of its own? Only ClaudeCliVerb::None does: every other process is a viewer (attach,
    // agents), a one-shot command (logs, stop, kill, respawn, rm) or the daemon's own plumbing (daemon, the pty host).
    //   verb    — the parsed verb.
    //   returns — true when the process runs no conversation, so its conversation must never be GUESSED from its folder's
    //             transcripts (Rule #14's transcript-age fallback). Never throws.
    constexpr bool ClaudeCliVerbRunsNoConversation(ClaudeCliVerb verb) noexcept
    {
        return verb != ClaudeCliVerb::None;
    }

    // The live background job with this job id, if any.
    //   jobs    — this survey's live background jobs.
    //   jobId   — the short job id (a presence row's `parkedJobId`, an attach target); compared ignoring ASCII case.
    //   returns — the job, or nullptr when no live job has that id (or `jobId` is empty). Never throws.
    inline const BackgroundJob* FindBackgroundJobById(const std::vector<BackgroundJob>& jobs, std::wstring_view jobId) noexcept
    {
        if (jobId.empty())
        {
            return nullptr;
        }
        for (const auto& job : jobs)
        {
            if (BackgroundIdEquals(job.jobId, jobId))
            {
                return &job;
            }
        }
        return nullptr;
    }

    // The live background job `claude attach <target>` opens, resolved the ways Claude Code accepts a target, in this order:
    //   1. a job id ("7c488ed3");
    //   2. a whole session id ("7c488ed3-2484-4e18-b762-a6422dfe2af4");
    //   3. a session-id prefix of at least 8 characters that exactly one live job's session id starts with;
    //   4. a name (a session's `name`), exactly, else ignoring ASCII case — when exactly one live job carries it.
    // An ambiguous prefix or name opens nothing (Claude Code asks the user to be precise in that case too).
    //   jobs    — this survey's live background jobs.
    //   target  — the attach command's argument.
    //   returns — the job, or nullptr when none or several match. Never throws.
    inline const BackgroundJob* FindBackgroundJobForAttach(const std::vector<BackgroundJob>& jobs, std::wstring_view target) noexcept
    {
        if (target.empty())
        {
            return nullptr;
        }
        if (const auto* byJob = FindBackgroundJobById(jobs, target))
        {
            return byJob;
        }
        for (const auto& job : jobs)
        {
            if (BackgroundIdEquals(job.sessionId, target))
            {
                return &job;
            }
        }
        // A prefix: only from 8 characters on (the job id's own length), so a short word can never pick a session.
        if (target.size() >= 8 && target.size() < 36)
        {
            const BackgroundJob* found = nullptr;
            size_t matches = 0;
            for (const auto& job : jobs)
            {
                if (job.sessionId.size() >= target.size() && BackgroundIdEquals(std::wstring_view{ job.sessionId }.substr(0, target.size()), target))
                {
                    found = &job;
                    ++matches;
                }
            }
            if (matches == 1)
            {
                return found;
            }
            if (matches > 1)
            {
                return nullptr; // ambiguous: open nothing rather than guess
            }
        }
        // A name: exactly first, then ignoring ASCII case; either must be unique.
        const BackgroundJob* exact = nullptr;
        size_t exactCount = 0;
        const BackgroundJob* folded = nullptr;
        size_t foldedCount = 0;
        for (const auto& job : jobs)
        {
            if (job.name.empty())
            {
                continue;
            }
            if (job.name == target)
            {
                exact = &job;
                ++exactCount;
            }
            if (BackgroundIdEquals(job.name, target))
            {
                folded = &job;
                ++foldedCount;
            }
        }
        if (exactCount == 1)
        {
            return exact;
        }
        if (exactCount == 0 && foldedCount == 1)
        {
            return folded;
        }
        return nullptr;
    }

    // Which live background job a TAB's claude shows, if any — the one question the observer asks of every claude it finds
    // under one of our tabs:
    //   1. a window that moved its conversation to the background (`/fork` with the fleet on, `/bg`) names the job in its own
    //      presence row's `parkedJobId`, and Claude Code opens that job in the window: the tab shows the job;
    //   2. else a `claude attach <id|name>` shows the job its target names.
    // ⚠ After a park, the window shows Claude Code's agent view, where the user can open ANOTHER job; nothing on disk says
    // which, so the tab keeps following the parked one (OBSERVER.md §21 "Limits").
    //   jobs        — this survey's live background jobs.
    //   parkedJobId — the tab claude's own presence row's `parkedJobId` ("" when it parked nothing or wrote no row).
    //   invocation  — its parsed command line (ParseClaudeCliInvocation).
    //   returns     — the job the tab shows, or nullptr when it shows its own conversation (or a parked job that is not
    //                 live yet: the daemon starts the job's process a few seconds after the park). Never throws.
    inline const BackgroundJob* ResolveTabShownBackgroundJob(const std::vector<BackgroundJob>& jobs, std::wstring_view parkedJobId, const ClaudeCliInvocation& invocation) noexcept
    {
        if (const auto* parked = FindBackgroundJobById(jobs, parkedJobId))
        {
            return parked;
        }
        if (invocation.verb == ClaudeCliVerb::Attach)
        {
            return FindBackgroundJobForAttach(jobs, invocation.target);
        }
        return nullptr;
    }

    // The conversation a background session was made FROM, as its launch records it — its sticky parent (the side panel puts
    // its card right below the parent's) and the proof that a tab now shows its own conversation's continuation. In order:
    //   1. CLAUDE_CODE_RESUME_SOURCE_ALIVE = "<self id>|<fork boundary>|<source id>" — what the daemon gives a `/fork` task
    //      (measured on 2.1.322; the same id as the job's state.json `forkParentSessionId`);
    //   2. a `--fork-session` copy's `--resume` target: an id, or a transcript FILE whose stem is the id — a `/fork` or `/bg`
    //      that moved a window's conversation into the daemon resumes `...\projects\<project>\<source id>.jsonl` (a fork
    //      task resumes a private copy, `...\jobs\<job>\parent-transcript.jsonl`, whose stem names nothing).
    // An id equal to the session's own is never its parent.
    //   resumeSourceAlive — the process's CLAUDE_CODE_RESUME_SOURCE_ALIVE ("" when absent).
    //   resumeTarget      — its `--resume` argument ("" when none).
    //   forkSession       — its command line carries `--fork-session` (a copy, not the conversation itself).
    //   selfSessionId     — the session the process runs now.
    //   returns           — the parent's id in lower case, or "" when the launch names none.
    //   throws            — std::bad_alloc only.
    inline std::wstring BackgroundParentFromLaunch(std::wstring_view resumeSourceAlive, std::wstring_view resumeTarget, bool forkSession, std::wstring_view selfSessionId)
    {
        // 1. "<self>|<boundary>|<source>": the THIRD field (the first two are the copy's own id and the fork's instant).
        if (!resumeSourceAlive.empty())
        {
            std::vector<std::wstring_view> fields;
            size_t start = 0;
            while (start <= resumeSourceAlive.size())
            {
                const size_t bar = resumeSourceAlive.find(L'|', start);
                if (bar == std::wstring_view::npos)
                {
                    fields.push_back(resumeSourceAlive.substr(start));
                    break;
                }
                fields.push_back(resumeSourceAlive.substr(start, bar - start));
                start = bar + 1;
            }
            if (fields.size() >= 3 && LooksLikeConversationId(fields[2]) && !BackgroundIdEquals(fields[2], selfSessionId))
            {
                return BackgroundIdLower(fields[2]);
            }
        }
        // 2. A `--fork-session` copy names its source by its `--resume` target. A plain `--resume` (no copy) continues the SAME
        //    conversation, so it names no parent.
        if (forkSession && !resumeTarget.empty())
        {
            if (LooksLikeConversationId(resumeTarget))
            {
                return BackgroundIdEquals(resumeTarget, selfSessionId) ? std::wstring{} : BackgroundIdLower(resumeTarget);
            }
            std::wstring_view stem = resumeTarget;
            if (const size_t slash = stem.find_last_of(L"\\/"); slash != std::wstring_view::npos)
            {
                stem = stem.substr(slash + 1);
            }
            constexpr std::wstring_view kExt = L".jsonl";
            if (stem.size() > kExt.size() && BackgroundIdEquals(stem.substr(stem.size() - kExt.size()), kExt))
            {
                stem = stem.substr(0, stem.size() - kExt.size());
            }
            if (LooksLikeConversationId(stem) && !BackgroundIdEquals(stem, selfSessionId))
            {
                return BackgroundIdLower(stem);
            }
        }
        return {};
    }

    // The title a DETACHED background session's record takes from Claude Code's own name for it, applied at every observation:
    // Claude Code names a background session by the first words of its task and renames it once it has worked out a better
    // name (measured: "Can a short gap (downwards) be reddish…" became "gap direction colors spec" 21 s later), and the user
    // can /rename it. The record FOLLOWS that name while its title is still the name it last took (or it has none), and stops
    // following once somebody gave it another title — a rename in Agentmaster, or the tab text of a tab it was opened in.
    //   currentTitle — the record's title now ("" when none).
    //   previousName — the name the record took from Claude Code last time ("" when never).
    //   newName      — the name the job reports now ("" when it has none).
    //   returns      — the title the record should carry (the current one when nothing changes).
    //   throws       — std::bad_alloc only.
    inline std::wstring FollowBackgroundSessionName(std::wstring_view currentTitle, std::wstring_view previousName, std::wstring_view newName)
    {
        if (newName.empty())
        {
            return std::wstring{ currentTitle };
        }
        if (currentTitle.empty() || currentTitle == previousName)
        {
            return std::wstring{ newName };
        }
        return std::wstring{ currentTitle }; // a title somebody gave wins over Claude Code's name
    }

    // The order that puts every CHILD row right below its PARENT row (the side panel's sticky background cards: a card no tab
    // shows sits under the card of the session it came from — the user's "the card should be sticky below the parent card").
    // The rows keep their given order otherwise: a parent's children follow it in their own given order (each child's own
    // children right below it), and a child whose parent is not among the rows — filtered out, in another window's scope, or
    // unknown — keeps its own place. A loop in the parent links (never written by Claude Code, but read from files) is broken
    // at its first member in row order, which then keeps its place.
    //   ids       — the rows' ids, in their sorted order (unique; a repeated id is treated as its first occurrence's parent).
    //   parentIds — parallel to `ids`: each row's parent id, "" for a row that is no child.
    //   returns   — the permutation: out[k] is the index into `ids` of the row shown k-th. The identity when the two vectors
    //               differ in length.
    //   throws    — std::bad_alloc only.
    inline std::vector<size_t> StickyChildOrder(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& parentIds)
    {
        const size_t n = ids.size();
        std::vector<size_t> out;
        out.reserve(n);
        if (parentIds.size() != n)
        {
            for (size_t i = 0; i < n; ++i)
            {
                out.push_back(i);
            }
            return out;
        }
        constexpr size_t kNone = static_cast<size_t>(-1);
        // Each row's parent ROW (kNone when it has no parent among the rows). A quadratic lookup is fine here: the callers pass
        // one list update's rows only when at least one of them is a child (the side panel's background cards are few).
        std::vector<size_t> parentRow(n, kNone);
        for (size_t i = 0; i < n; ++i)
        {
            if (parentIds[i].empty())
            {
                continue;
            }
            for (size_t j = 0; j < n; ++j)
            {
                if (j != i && BackgroundIdEquals(ids[j], parentIds[i]))
                {
                    parentRow[i] = j;
                    break;
                }
            }
        }
        // Break every loop: walking up from row i, reaching i again means i sits on a loop; it becomes a root (keeps its place),
        // which turns the rest of that loop into an ordinary chain below it. At most n steps per walk.
        for (size_t i = 0; i < n; ++i)
        {
            size_t at = parentRow[i];
            for (size_t steps = 0; at != kNone && steps < n; ++steps)
            {
                if (at == i)
                {
                    parentRow[i] = kNone;
                    break;
                }
                at = parentRow[at];
            }
        }
        std::vector<std::vector<size_t>> children(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (parentRow[i] != kNone)
            {
                children[parentRow[i]].push_back(i);
            }
        }
        // Depth first from every root in row order, an explicit stack (a deep chain must not overflow the thread's stack).
        std::vector<size_t> stack;
        for (size_t root = 0; root < n; ++root)
        {
            if (parentRow[root] != kNone)
            {
                continue;
            }
            stack.push_back(root);
            while (!stack.empty())
            {
                const size_t row = stack.back();
                stack.pop_back();
                out.push_back(row);
                // Pushed in reverse, so the first child is visited first and the children keep their row order.
                for (auto it = children[row].rbegin(); it != children[row].rend(); ++it)
                {
                    stack.push_back(*it);
                }
            }
        }
        if (out.size() != n)
        {
            // Belt: every row has exactly one way down from a root once the loops are broken, so this is unreachable; if it
            // ever were reached, the given order is the safe answer (no row lost, none twice).
            out.clear();
            for (size_t i = 0; i < n; ++i)
            {
                out.push_back(i);
            }
        }
        return out;
    }
}
