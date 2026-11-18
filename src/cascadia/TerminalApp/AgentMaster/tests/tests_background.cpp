// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - Claude Code BACKGROUND SESSIONS (OBSERVER.md section 21; AgentMaster/BackgroundSessions.h).
//
// Claude Code 2.1.322 runs conversations in its own daemon: `/fork <task>` (fleet on) runs the task in a NEW background
// session AND moves the tab's conversation into one (a copy under a new id, which the window then shows - its presence row
// keeps the old id and names the job in `parkedJobId`); `/bg` moves it; `claude attach <id>` shows one. A background
// session's hooks carry no WT_SESSION, so before this the tab stopped updating and the moved conversation showed as a
// tab-less card titled with its GUID (the user's report of 2026-11-18). These tests pin:
//   TestBackgroundSessionRules   - the pure header: the command-line split, the verbs that run no conversation, which job a
//                                  tab shows (a park, an attach target by id / session id / prefix / name), a job's parent
//                                  from its launch, the title that follows Claude Code's name, and the sticky card order.
//   TestBackgroundPresenceFacts  - the presence row's background fields (kind, jobId, parkedJobId, name, spare) read from
//                                  files shaped like the incident's, and the process facts the parent comes from.
//   TestBackgroundAttachSpawn    - `claude attach <job>` (the launcher rendering, a hostile job id) and the spawn builders'
//                                  lookup seam: a RESUME of a conversation a live job holds attaches, nothing else does.
//   TestBackgroundRegistry       - the registry: a detached job's record (title from Claude Code's name, followed across its
//                                  renames until somebody renames it; a stale token cleared; no adoption fan-out), a tab that
//                                  shows its own conversation's continuation keeps its title, the parent is never erased,
//                                  a spare's placeholder record is forgotten (and nothing else is), the end of a tab-less
//                                  record (removed or archived after its grace), a token-less SessionStart that fans out to
//                                  no window, and `/branch` keeping the tab's name as a fork title in either arrival order.

#include "m5_tests.h"

#include "../BackgroundSessions.h"

#include <fstream>

namespace
{
    // Write `text` to `path` as UTF-8 bytes (the presence files are UTF-8 JSON). Returns false when the file cannot be written.
    bool WriteUtf8File(const std::wstring& path, const std::string& text)
    {
        std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(f);
    }

    // One live background job, shaped like a survey's (BackgroundJob).
    BackgroundJob Job(const std::wstring& jobId, const std::wstring& sessionId, const std::wstring& name, const std::wstring& parentId = {})
    {
        BackgroundJob job;
        job.jobId = jobId;
        job.sessionId = sessionId;
        job.pid = 4242;
        job.status = L"busy";
        job.name = name;
        job.cwd = L"K:\\source\\TraderLab-options";
        job.parentId = parentId;
        return job;
    }

    // The ids of the incident (2026-11-18): the tab's conversation, its parked continuation, the forked task, the spare.
    const std::wstring kTabConversation = L"6ab8b1af-e712-4f73-abe6-f57997134fc1";
    const std::wstring kParkedJobSession = L"0fac38d6-fd50-4d17-b002-e2ec2e5442ce";
    const std::wstring kForkTaskSession = L"7c488ed3-2484-4e18-b762-a6422dfe2af4";
    const std::wstring kSpareSession = L"0d42712a-3f43-4556-820a-4235557b86f6";

    // The lookup the attach tests install: only the incident's parked job is live.
    std::wstring FakeLiveJobLookup(std::wstring_view sessionId) noexcept
    {
        return BackgroundIdEquals(sessionId, L"0fac38d6-fd50-4d17-b002-e2ec2e5442ce") ? std::wstring{ L"0fac38d6" } : std::wstring{};
    }
}

void TestBackgroundSessionRules()
{
    std::wprintf(L"Background sessions: the pure rules (BackgroundSessions.h, OBSERVER.md section 21):\n");

    // --- the command-line split (CommandLineToArgvW's rules) ---
    {
        const auto a = SplitClaudeCommandLine(L"\"C:\\Program Files\\Claude\\claude.exe\" attach  \"two words\" x\\\"y \"a\\\\\"b");
        CHECK(a.size() == 5, "split: five arguments (quotes group, runs of blanks separate)");
        CHECK(a.size() == 5 && a[0] == L"C:\\Program Files\\Claude\\claude.exe", "split: a quoted program path keeps its spaces");
        CHECK(a.size() == 5 && a[2] == L"two words", "split: a quoted argument keeps its space and loses its quotes");
        CHECK(a.size() == 5 && a[3] == L"x\"y", "split: an odd backslash run before a quote is a literal quote");
        CHECK(a.size() == 5 && a[4] == L"a\\b", "split: an even backslash run before a quote halves and toggles the quoting");
        CHECK(SplitClaudeCommandLine(L"   ").empty() && SplitClaudeCommandLine(L"").empty(), "split: an empty or blank line has no arguments");
        const auto empties = SplitClaudeCommandLine(L"claude \"\" x");
        CHECK(empties.size() == 3 && empties[1].empty(), "split: an empty quoted pair is an empty argument");
    }

    // --- which verb a claude command line runs ---
    {
        const auto attach = ParseClaudeCliInvocation(L"\"C:\\Users\\ELI\\.local\\bin\\claude.exe\" attach 7c488ed3");
        CHECK(attach.verb == ClaudeCliVerb::Attach && attach.target == L"7c488ed3", "verbs: `claude attach <job>` is Attach with its target");
        const auto byName = ParseClaudeCliInvocation(L"claude.exe attach \"\x2442 Can text be fixed size\"");
        CHECK(byName.verb == ClaudeCliVerb::Attach && byName.target == L"\x2442 Can text be fixed size", "verbs: attach by a quoted name keeps the whole name");
        CHECK(ParseClaudeCliInvocation(L"claude.exe agents").verb == ClaudeCliVerb::Agents, "verbs: `claude agents` is the agent view");
        CHECK(ParseClaudeCliInvocation(L"claude.exe logs 7c488ed3").verb == ClaudeCliVerb::Logs, "verbs: logs");
        CHECK(ParseClaudeCliInvocation(L"claude.exe stop 7c488ed3").verb == ClaudeCliVerb::Stop, "verbs: stop");
        CHECK(ParseClaudeCliInvocation(L"claude.exe kill 7c488ed3").verb == ClaudeCliVerb::Kill, "verbs: kill");
        CHECK(ParseClaudeCliInvocation(L"claude.exe respawn 7c488ed3").verb == ClaudeCliVerb::Respawn, "verbs: respawn");
        CHECK(ParseClaudeCliInvocation(L"claude.exe rm 7c488ed3").verb == ClaudeCliVerb::Rm, "verbs: rm");
        const auto daemon = ParseClaudeCliInvocation(L"C:\\Users\\ELI\\.local\\bin\\claude.exe daemon run --origin transient --spawned-by \"{\\\"label\\\":\\\"claude\\\",\\\"pid\\\":131124}\"");
        CHECK(daemon.verb == ClaudeCliVerb::Daemon && daemon.target == L"run", "verbs: the daemon itself (the incident's `daemon run` line)");
        // The incident's pty host line: the hosted command after `--` carries --fork-session / a prompt, never this process's verb.
        const auto host = ParseClaudeCliInvocation(L"C:\\Users\\ELI\\.local\\bin\\claude.exe --bg-pty-host \\\\.\\pipe\\cc-daemon-967c29a8db69aeab-pty-7c488ed3 180 51 -- "
                                                   L"C:\\Users\\ELI\\.local\\bin\\claude.exe --session-id 7c488ed3-2484-4e18-b762-a6422dfe2af4 --fork-session --resume x attach");
        CHECK(host.verb == ClaudeCliVerb::PtyHost && host.target == L"\\\\.\\pipe\\cc-daemon-967c29a8db69aeab-pty-7c488ed3", "verbs: the daemon's pty host, with its pipe");
        // The incident's job line and the parked window's own line: conversations, whatever their arguments say.
        CHECK(ParseClaudeCliInvocation(L"C:\\Users\\ELI\\.local\\bin\\claude.exe --session-id 0fac38d6-fd50-4d17-b002-e2ec2e5442ce --fork-session --resume "
                                       L"C:\\Users\\ELI\\.claude\\projects\\K--source-TraderLab-options\\6ab8b1af-e712-4f73-abe6-f57997134fc1.jsonl --settings x -- \"please attach the logs\"")
                      .verb == ClaudeCliVerb::None,
              "verbs: a background job's own line is a conversation (its prompt mentioning `attach` changes nothing)");
        CHECK(ParseClaudeCliInvocation(L"\"C:\\Users\\ELI\\.local\\bin\\claude.exe\" --dangerously-skip-permissions --resume 6ab8b1af-e712-4f73-abe6-f57997134fc1 --settings C:/x.json").verb == ClaudeCliVerb::None,
              "verbs: the parked window's own line is a conversation");
        CHECK(ParseClaudeCliInvocation(L"claude.exe Attach x").verb == ClaudeCliVerb::None, "verbs: Claude Code's verbs are lower case (\"Attach\" is a prompt)");
        CHECK(ParseClaudeCliInvocation(L"claude.exe").verb == ClaudeCliVerb::None, "verbs: the program alone is an interactive conversation");
        CHECK(ParseClaudeCliInvocation(L"claude.exe attach").target.empty(), "verbs: an attach without a target has an empty target");
        CHECK(!ClaudeCliVerbRunsNoConversation(ClaudeCliVerb::None) && ClaudeCliVerbRunsNoConversation(ClaudeCliVerb::Attach) &&
                  ClaudeCliVerbRunsNoConversation(ClaudeCliVerb::Agents) && ClaudeCliVerbRunsNoConversation(ClaudeCliVerb::PtyHost),
              "verbs: only None runs a conversation of its own");
    }

    // --- ids ---
    CHECK(LooksLikeConversationId(kTabConversation) && LooksLikeConversationId(L"6AB8B1AF-E712-4F73-ABE6-F57997134FC1"), "ids: a GUID in either case is a conversation id");
    CHECK(!LooksLikeConversationId(L"6ab8b1af") && !LooksLikeConversationId(L"6ab8b1af-e712-4f73-abe6-f57997134fcz") && !LooksLikeConversationId(L"6ab8b1af_e712-4f73-abe6-f57997134fc1"),
          "ids: a short id, a non-hex digit or a wrong separator is not");
    CHECK(BackgroundIdEquals(L"0FAC38D6", L"0fac38d6") && !BackgroundIdEquals(L"0fac38d6", L"0fac38d7") && !BackgroundIdEquals(L"0fac38d", L"0fac38d6"), "ids: equality ignores ASCII case only");
    CHECK(BackgroundIdLower(L"0FAC38D6-X") == L"0fac38d6-x", "ids: lower-casing folds A-Z only");

    // --- which job a tab shows ---
    {
        std::vector<BackgroundJob> jobs;
        jobs.push_back(Job(L"0fac38d6", kParkedJobSession, L"gap direction colors spec"));
        jobs.push_back(Job(L"7c488ed3", kForkTaskSession, L"\x2442 Can text be fixed size and not resize with scroll? make text", kTabConversation));
        const ClaudeCliInvocation conversation{};
        const auto* parked = ResolveTabShownBackgroundJob(jobs, L"0fac38d6", conversation);
        CHECK(parked && parked->sessionId == kParkedJobSession, "shown: a window's parkedJobId names the job it shows (the incident's tab)");
        CHECK(ResolveTabShownBackgroundJob(jobs, L"0FAC38D6", conversation) == parked, "shown: the parked id matches ignoring case");
        CHECK(ResolveTabShownBackgroundJob(jobs, L"", conversation) == nullptr, "shown: no park and no attach -> the tab shows its own conversation");
        CHECK(ResolveTabShownBackgroundJob(jobs, L"deadbeef", conversation) == nullptr, "shown: a parked job that is not live yet shows nothing (the window keeps its own id)");
        ClaudeCliInvocation attach;
        attach.verb = ClaudeCliVerb::Attach;
        attach.target = L"7c488ed3";
        const auto* byJob = ResolveTabShownBackgroundJob(jobs, L"", attach);
        CHECK(byJob && byJob->sessionId == kForkTaskSession, "shown: `claude attach <job id>`");
        attach.target = kForkTaskSession;
        CHECK(ResolveTabShownBackgroundJob(jobs, L"", attach) == byJob, "shown: `claude attach <session id>`");
        attach.target = L"7c488ed3-2484";
        CHECK(ResolveTabShownBackgroundJob(jobs, L"", attach) == byJob, "shown: `claude attach <a unique session-id prefix>`");
        attach.target = L"\x2442 Can text be fixed size and not resize with scroll? make text";
        CHECK(ResolveTabShownBackgroundJob(jobs, L"", attach) == byJob, "shown: `claude attach <name>`, exactly");
        attach.target = L"GAP DIRECTION COLORS SPEC";
        const auto* folded = ResolveTabShownBackgroundJob(jobs, L"", attach);
        CHECK(folded && folded->sessionId == kParkedJobSession, "shown: a name ignoring ASCII case when exactly one job carries it");
        attach.target = L"nothing-like-it";
        CHECK(ResolveTabShownBackgroundJob(jobs, L"", attach) == nullptr, "shown: an attach target no live job matches shows nothing");
        CHECK(ResolveTabShownBackgroundJob(jobs, L"0fac38d6", attach) == parked, "shown: a park wins over the command line");
        ClaudeCliInvocation agents;
        agents.verb = ClaudeCliVerb::Agents;
        agents.target = L"7c488ed3";
        CHECK(ResolveTabShownBackgroundJob(jobs, L"", agents) == nullptr, "shown: only an attach opens a job by its argument");

        // Ambiguity opens nothing rather than guess.
        std::vector<BackgroundJob> twins;
        twins.push_back(Job(L"aaaaaaaa", L"aaaaaaaa-1111-4000-8000-000000000001", L"same"));
        twins.push_back(Job(L"aaaaaaab", L"aaaaaaaa-1111-4000-8000-000000000002", L"same"));
        CHECK(FindBackgroundJobForAttach(twins, L"aaaaaaaa-1111") == nullptr, "attach: a prefix two sessions share opens nothing");
        CHECK(FindBackgroundJobForAttach(twins, L"same") == nullptr, "attach: a name two sessions share opens nothing");
        CHECK(FindBackgroundJobForAttach(twins, L"aaaaaaaa") == &twins[0], "attach: a job id is exact, never a prefix question");
        CHECK(FindBackgroundJobForAttach(twins, L"aaaa") == nullptr, "attach: a prefix shorter than a job id never picks a session");
        CHECK(FindBackgroundJobForAttach(twins, L"") == nullptr && FindBackgroundJobById(twins, L"") == nullptr, "attach: an empty target opens nothing");
    }

    // --- a job's parent, from its launch ---
    {
        CHECK(BackgroundParentFromLaunch(L"7c488ed3-2484-4e18-b762-a6422dfe2af4|2026-11-18T04:24:21.511Z|6AB8B1AF-E712-4F73-ABE6-F57997134FC1", L"C:\\Users\\ELI\\.claude\\jobs\\7c488ed3\\parent-transcript.jsonl", true, kForkTaskSession) == kTabConversation,
              "parent: a /fork task's CLAUDE_CODE_RESUME_SOURCE_ALIVE third field (lower-cased)");
        CHECK(BackgroundParentFromLaunch(L"", L"C:\\Users\\ELI\\.claude\\projects\\K--source-TraderLab-options\\6ab8b1af-e712-4f73-abe6-f57997134fc1.jsonl", true, kParkedJobSession) == kTabConversation,
              "parent: a parked continuation's `--fork-session --resume <the tab's transcript>`");
        CHECK(BackgroundParentFromLaunch(L"", L"C:/x/6ab8b1af-e712-4f73-abe6-f57997134fc1.JSONL", true, kParkedJobSession) == kTabConversation, "parent: forward slashes and an upper-case extension");
        CHECK(BackgroundParentFromLaunch(L"", kTabConversation, true, kParkedJobSession) == kTabConversation, "parent: a `--fork-session --resume <id>`");
        CHECK(BackgroundParentFromLaunch(L"", L"C:\\Users\\ELI\\.claude\\jobs\\7c488ed3\\parent-transcript.jsonl", true, kForkTaskSession).empty(), "parent: a private parent-transcript copy names nobody");
        CHECK(BackgroundParentFromLaunch(L"", L"C:\\x\\6ab8b1af-e712-4f73-abe6-f57997134fc1.jsonl", false, kParkedJobSession).empty(), "parent: a plain --resume (no copy) continues the same conversation: no parent");
        CHECK(BackgroundParentFromLaunch(L"a|b|" + kForkTaskSession, L"", false, kForkTaskSession).empty(), "parent: a session is never its own parent");
        CHECK(BackgroundParentFromLaunch(L"only-two|fields", L"", false, kForkTaskSession).empty() && BackgroundParentFromLaunch(L"a|b|not-a-guid", L"", false, kForkTaskSession).empty(),
              "parent: a malformed environment value names nobody");
        CHECK(BackgroundParentFromLaunch(L"", L"", true, kForkTaskSession).empty(), "parent: nothing to read -> no parent");
    }

    // --- the title that follows Claude Code's name ---
    {
        CHECK(FollowBackgroundSessionName(L"", L"", L"Can a short gap be reddish") == L"Can a short gap be reddish", "name: an untitled record takes Claude Code's name");
        CHECK(FollowBackgroundSessionName(L"Can a short gap be reddish", L"Can a short gap be reddish", L"gap direction colors spec") == L"gap direction colors spec",
              "name: a record still titled by the last name follows Claude Code's rename (the incident's 21 s rename)");
        CHECK(FollowBackgroundSessionName(L"my gap work", L"Can a short gap be reddish", L"gap direction colors spec") == L"my gap work", "name: a title somebody gave wins");
        CHECK(FollowBackgroundSessionName(L"keep", L"keep", L"") == L"keep", "name: no name keeps the title");
    }

    // --- the sticky order ---
    {
        const std::vector<std::wstring> ids{ L"other", L"tab", L"other2", L"task" };
        const auto order = StickyChildOrder(ids, { L"", L"", L"", L"tab" });
        CHECK((order == std::vector<size_t>{ 0, 1, 3, 2 }), "sticky: the forked task's card sits right below its tab's card (the user's rule)");
        CHECK((StickyChildOrder(ids, { L"", L"", L"", L"" }) == std::vector<size_t>{ 0, 1, 2, 3 }), "sticky: no child -> the sorted order");
        CHECK((StickyChildOrder({ L"a", L"t1", L"p", L"t2" }, { L"", L"p", L"", L"p" }) == std::vector<size_t>{ 0, 2, 1, 3 }), "sticky: a parent's children keep their own order below it");
        CHECK((StickyChildOrder({ L"g", L"p", L"c" }, { L"c", L"", L"p" }) == std::vector<size_t>{ 1, 2, 0 }), "sticky: a grandchild sits below its parent, below the root");
        CHECK((StickyChildOrder({ L"a", L"c" }, { L"", L"gone" }) == std::vector<size_t>{ 0, 1 }), "sticky: a child whose parent is not listed keeps its place");
        CHECK((StickyChildOrder({ L"a", L"b", L"x" }, { L"b", L"a", L"" }) == std::vector<size_t>{ 0, 1, 2 }), "sticky: a parent loop is broken at its first row, nothing lost");
        CHECK((StickyChildOrder({ L"a", L"b" }, { L"" }) == std::vector<size_t>{ 0, 1 }), "sticky: mismatched inputs -> the given order");
        CHECK((StickyChildOrder({ L"Tab", L"task" }, { L"", L"TAB" }) == std::vector<size_t>{ 0, 1 }), "sticky: ids match ignoring ASCII case");
        CHECK(StickyChildOrder({}, {}).empty(), "sticky: no rows");
        // Every permutation of a small list with a chain and a loop stays a permutation (no row lost or doubled).
        const std::vector<std::wstring> sweepIds{ L"a", L"b", L"c", L"d", L"e" };
        const std::vector<std::vector<std::wstring>> parentSets{ { L"", L"a", L"b", L"", L"d" }, { L"b", L"c", L"a", L"", L"" }, { L"e", L"", L"", L"c", L"d" } };
        bool allPermutations = true;
        for (const auto& parents : parentSets)
        {
            auto o = StickyChildOrder(sweepIds, parents);
            std::sort(o.begin(), o.end());
            allPermutations = allPermutations && (o == std::vector<size_t>{ 0, 1, 2, 3, 4 });
        }
        CHECK(allPermutations, "sticky: every answer is a permutation of the rows (chains and loops included)");
    }
}

void TestBackgroundPresenceFacts()
{
    std::wprintf(L"Background sessions: the presence rows and the process facts (OBSERVER.md section 21):\n");
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring{ tmp } + L"am_presence_bg_" + std::to_wstring(::GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
    // The incident's three rows, trimmed to what the readers use (2026-11-18 ~/.claude/sessions).
    WriteUtf8File(dir + L"\\131124.json",
                  "{\"pid\":131124,\"sessionId\":\"6ab8b1af-e712-4f73-abe6-f57997134fc1\",\"cwd\":\"K:\\\\source\\\\TraderLab-options\",\"startedAt\":1794935615865,"
                  "\"procStart\":\"134394092150081760\",\"kind\":\"interactive\",\"name\":\"traderlab-options-13\",\"status\":\"idle\",\"parkedJobId\":\"0fac38d6\"}");
    WriteUtf8File(dir + L"\\12616.json",
                  "{\"pid\":12616,\"sessionId\":\"0fac38d6-fd50-4d17-b002-e2ec2e5442ce\",\"cwd\":\"K:\\\\source\\\\TraderLab-options\",\"startedAt\":1794975968274,"
                  "\"kind\":\"bg\",\"name\":\"gap direction colors spec\",\"jobId\":\"0fac38d6\",\"status\":\"busy\"}");
    WriteUtf8File(dir + L"\\777.json",
                  "{\"pid\":777,\"sessionId\":\"0d42712a-3f43-4556-820a-4235557b86f6\",\"cwd\":\"K:\\\\source\\\\TraderLab-options\",\"kind\":\"bg\",\"jobId\":\"0d42712a\",\"spare\":true,\"status\":\"idle\"}");
    WriteUtf8File(dir + L"\\888.json", "{\"pid\":888,\"sessionId\":\"11111111-2222-4333-8444-555555555555\",\"status\":\"busy\"}");
    const auto rows = ReadSessionPresenceIn(dir);
    const SessionPresenceRow* window = nullptr;
    const SessionPresenceRow* job = nullptr;
    const SessionPresenceRow* spare = nullptr;
    const SessionPresenceRow* old = nullptr;
    for (const auto& r : rows)
    {
        window = (r.pid == 131124) ? &r : window;
        job = (r.pid == 12616) ? &r : job;
        spare = (r.pid == 777) ? &r : spare;
        old = (r.pid == 888) ? &r : old;
    }
    CHECK(window && window->kind == L"interactive" && window->parkedJobId == L"0fac38d6" && window->sessionId == kTabConversation,
          "presence: the parked window's row keeps its old id and names the job it shows");
    CHECK(job && job->kind == kPresenceKindBackground && job->jobId == L"0fac38d6" && job->name == L"gap direction colors spec" && !job->spare,
          "presence: a background job's row (kind bg, job id, Claude Code's name)");
    CHECK(spare && spare->spare && spare->kind == L"bg", "presence: a daemon spare is marked");
    CHECK(old && old->kind.empty() && old->jobId.empty() && old->parkedJobId.empty() && old->name.empty() && !old->spare, "presence: an older claude's row reads as an ordinary session");
    std::filesystem::remove_all(std::filesystem::path{ dir }, ec);

    // The process facts the parent comes from: the daemon's env for a fork task, the copy flag on the command line.
    {
        std::unordered_map<std::wstring, std::wstring> env{ { L"CLAUDE_CODE_RESUME_SOURCE_ALIVE", L"7c488ed3-2484-4e18-b762-a6422dfe2af4|2026-11-18T04:24:21.511Z|6ab8b1af-e712-4f73-abe6-f57997134fc1" },
                                                            { L"CLAUDE_CODE_SESSION_KIND", L"bg" } };
        ClaudeProcessFacts f;
        ParseClaudeFacts(L"C:\\x\\claude.exe --session-id 7c488ed3-2484-4e18-b762-a6422dfe2af4 --fork-session --resume C:\\Users\\ELI\\.claude\\jobs\\7c488ed3\\parent-transcript.jsonl", env, f);
        CHECK(f.forkSession && f.background, "facts: a fork task's line is a copy (--fork-session) and a background session");
        CHECK(f.resumeSourceAlive.find(L"|6ab8b1af-") != std::wstring::npos, "facts: CLAUDE_CODE_RESUME_SOURCE_ALIVE is kept for the parent");
        CHECK(BackgroundParentFromLaunch(f.resumeSourceAlive, f.resumeTarget, f.forkSession, L"7c488ed3-2484-4e18-b762-a6422dfe2af4") == kTabConversation, "facts: the fork task's parent is the tab's conversation");
        ClaudeProcessFacts host;
        ParseClaudeFacts(L"claude.exe --bg-pty-host p 180 51 -- claude.exe --session-id x --fork-session", {}, host);
        CHECK(!host.forkSession, "facts: a --fork-session after `--` is the hosted line's, not this process's");
        ClaudeProcessFacts prompt;
        ParseClaudeFacts(L"claude.exe \"tell me about --fork-session\"", {}, prompt);
        CHECK(!prompt.forkSession, "facts: a prompt that mentions the flag is not the flag");
    }

    // The live lookup reads the real presence folder read-only: a made-up conversation is never held by a job.
    CHECK(FindLiveBackgroundJobId(L"00000000-0000-4000-8000-0000000000bb").empty() && FindLiveBackgroundJobId(L"").empty(), "lookup: no live job holds a made-up or empty id");
}

void TestBackgroundAttachSpawn()
{
    std::wprintf(L"Background sessions: `claude attach` and the spawn builders' lookup (OBSERVER.md section 21):\n");
    CHECK(BuildClaudeAttachCommandline(L"C:\\Users\\ELI\\.local\\bin\\claude.exe", L"0fac38d6") == L"\"C:\\Users\\ELI\\.local\\bin\\claude.exe\" attach 0fac38d6", "attach: the native launcher, quoted, then attach <job>");
    CHECK(BuildClaudeAttachCommandline(L"C:\\npm\\claude.cmd", L"0fac38d6") == L"cmd /c \"\"C:\\npm\\claude.cmd\" attach 0fac38d6\"", "attach: a batch launcher runs through cmd /c with one outer quote pair");
    CHECK(BuildClaudeAttachCommandline(L"", L"0fac38d6") == L"claude attach 0fac38d6", "attach: no launcher -> the bare token");
    CHECK(BuildClaudeAttachCommandline(L"claude.exe", L"0fac38d6; Remove-Item x \"y\" $(z)") == L"\"claude.exe\" attach 0fac38d6Remove-Itemxyz", "attach: a hostile job id is reduced to [A-Za-z0-9_-] (one bare argument)");

    // The lookup seam: unset (every program but the app) -> a resume stays a resume.
    AppSettings settings;
    SetBackgroundJobLookup(nullptr);
    const auto plain = BuildClaudeSpawn(L"K:/source/TraderLab-options", L"gaprush", L"\\\\.\\pipe\\agentmaster.42", kParkedJobSession, settings, {}, L"C:\\bin\\claude.exe");
    CHECK(plain.attachJobId.empty() && plain.commandline.find(L"--resume " + kParkedJobSession) != std::wstring::npos, "seam: no lookup -> `--resume`, as before");
    SetBackgroundJobLookup(&FakeLiveJobLookup);
    const auto attached = BuildClaudeSpawn(L"K:/source/TraderLab-options", L"gaprush", L"\\\\.\\pipe\\agentmaster.42", kParkedJobSession, settings, {}, L"C:\\bin\\claude.exe");
    CHECK(attached.attachJobId == L"0fac38d6", "seam: a resume of a conversation a live job holds ATTACHES");
    CHECK(attached.commandline == L"\"C:\\bin\\claude.exe\" attach 0fac38d6", "seam: ...with `claude attach <job>` (no --resume, no --settings: the job keeps its own)");
    CHECK(attached.sessionId == kParkedJobSession, "seam: ...and keeps the session id (the registry key and the tab's ref)");
    const auto otherResume = BuildClaudeSpawn(L"K:/x", L"x", L"\\\\.\\pipe\\agentmaster.42", kTabConversation, settings, {}, L"C:\\bin\\claude.exe");
    CHECK(otherResume.attachJobId.empty() && otherResume.commandline.find(L"--resume ") != std::wstring::npos, "seam: a conversation no job holds resumes");
    const auto fork = BuildClaudeSpawn(L"K:/x", L"x", L"\\\\.\\pipe\\agentmaster.42", {}, settings, kParkedJobSession, L"C:\\bin\\claude.exe");
    CHECK(fork.attachJobId.empty() && fork.commandline.find(L"--fork-session") != std::wstring::npos, "seam: a FORK of a job's conversation copies it (never an attach)");
    const auto fresh = BuildClaudeSpawn(L"K:/x", L"x", L"\\\\.\\pipe\\agentmaster.42", {}, settings, {}, L"C:\\bin\\claude.exe", kParkedJobSession);
    CHECK(fresh.attachJobId.empty() && fresh.commandline.find(L"attach") == std::wstring::npos, "seam: a fresh launch never attaches");
    // A restart resumes only an id whose transcript exists; a made-up one restarts fresh, and the lookup never makes it attach.
    const auto restartFresh = BuildClaudeRestartSpec(L"K:/x", L"x", L"\\\\.\\pipe\\agentmaster.42", L"00000000-0000-4000-8000-0000000000cc", settings, L"C:\\bin\\claude.exe");
    CHECK(restartFresh.attachJobId.empty() && restartFresh.commandline.find(L"--session-id") != std::wstring::npos, "seam: a restart that is no resume never attaches");
    SetBackgroundJobLookup(nullptr);
    const auto cleared = BuildClaudeSpawn(L"K:/source/TraderLab-options", L"gaprush", L"\\\\.\\pipe\\agentmaster.42", kParkedJobSession, settings, {}, L"C:\\bin\\claude.exe");
    CHECK(cleared.attachJobId.empty(), "seam: a cleared lookup resumes again");
}

void TestBackgroundRegistry()
{
    std::wprintf(L"Background sessions: the registry's rules (OBSERVER.md section 21):\n");

    // --- a DETACHED job: titled by Claude Code's name, followed until renamed; a stale token cleared; no adoption ---
    {
        SessionRegistry reg;
        int adopted = 0;
        reg.AddAdoptionHandler([&](const std::wstring&, const std::wstring&, const std::wstring&) { ++adopted; });
        // The job's own SessionStart: no WT_SESSION (the daemon dropped it) -> a record, no adoption.
        HookMessage start = Msg(kForkTaskSession, HookEvent::SessionStart);
        start.cwd = L"K:\\source\\TraderLab";
        start.source = L"fork";
        reg.OnHookEvent(start);
        auto s = reg.Get(kForkTaskSession);
        CHECK(s && s->live && s->external && s->title.empty() && adopted == 0, "detached: the job's token-less SessionStart makes an untitled record and adopts nothing");
        ObservedClaude o;
        o.sessionId = kForkTaskSession;
        o.cwd = L"K:\\source\\TraderLab\\.claude\\worktrees\\gap-label-zoom";
        o.pid = 76352;
        o.runningApp = RunningApp::Agentmaster;
        o.background = true;
        o.sessionName = L"\x2442 Can text be fixed size and not resize with scroll? make text";
        o.presenceStatus = L"busy";
        o.observedUnixMs = 1000;
        o.bgJobId = L"7c488ed3";
        o.bgParentId = kTabConversation;
        o.bgDetached = true;
        reg.ObserveClaude(o);
        s = reg.Get(kForkTaskSession);
        CHECK(s && s->title == o.sessionName, "detached: the record takes Claude Code's name (never a GUID)");
        CHECK(s && s->bgDetached && s->bgJobId == L"7c488ed3" && s->bgParentId == kTabConversation && s->background, "detached: the job, its parent and the detached mark are on the record");
        CHECK(adopted == 0, "detached: an observation without a token fans out to no window");
        o.sessionName = L"GapRush label zoom";
        reg.ObserveClaude(o);
        CHECK(reg.Get(kForkTaskSession)->title == L"GapRush label zoom", "detached: the title follows Claude Code's rename");
        reg.Update(kForkTaskSession, [](SessionInfo& r) { r.title = L"my zoom fix"; });
        o.sessionName = L"another auto name";
        reg.ObserveClaude(o);
        CHECK(reg.Get(kForkTaskSession)->title == L"my zoom fix", "detached: a rename in Agentmaster wins over Claude Code's later names");
        // A stale token (the attach tab that showed it closed) is cleared by a detached observation.
        reg.Update(kForkTaskSession, [](SessionInfo& r) { r.tabToken = L"wt-closed-attach-tab"; });
        reg.ObserveClaude(o);
        CHECK(reg.Get(kForkTaskSession)->tabToken.empty(), "detached: a stale token is cleared (no tab of ours shows the job)");
        // A tab's later observation (an attach) makes it ordinary again; an observation without a parent never erases it.
        ObservedClaude tabSees = o;
        tabSees.tabToken = L"wt-attach";
        tabSees.bgDetached = false;
        tabSees.bgParentId.clear();
        tabSees.pid = 5555;
        reg.ObserveClaude(tabSees);
        s = reg.Get(kForkTaskSession);
        CHECK(s && !s->bgDetached && s->tabToken == L"wt-attach" && s->bgParentId == kTabConversation, "detached: a tab's observation ends the detached mark, keeps the parent");
    }

    // --- a tab that shows its OWN conversation's continuation keeps its title (the incident's tab) ---
    {
        SessionRegistry reg;
        SessionInfo tab = MakeSession(kTabConversation, SessionState::WaitingForInput);
        tab.title = L"gaprush";
        tab.live = true;
        tab.tabToken = L"a8e97b09-e455-40c7-b8fc-c919d189b66d";
        tab.pid = 131124;
        reg.Upsert(tab);
        HookMessage jobStart = Msg(kParkedJobSession, HookEvent::SessionStart);
        jobStart.cwd = L"K:\\source\\TraderLab-options";
        reg.OnHookEvent(jobStart); // the continuation's own SessionStart: no token
        ObservedClaude o;
        o.sessionId = kParkedJobSession;
        o.tabToken = L"a8e97b09-e455-40c7-b8fc-c919d189b66d";
        o.cwd = L"K:\\source\\TraderLab-options";
        o.pid = 131124;
        o.runningApp = RunningApp::Agentmaster;
        o.background = true;
        o.sessionName = L"gap direction colors spec";
        o.observedUnixMs = 2000;
        o.bgJobId = L"0fac38d6";
        o.bgParentId = kTabConversation;
        o.carryTitleFrom = kTabConversation;
        reg.ObserveClaude(o);
        const auto s = reg.Get(kParkedJobSession);
        CHECK(s && s->title == L"gaprush", "continuation: the job's record takes the tab's title (the tab keeps its name)");
        CHECK(s && s->tabToken == L"a8e97b09-e455-40c7-b8fc-c919d189b66d" && !s->bgDetached, "continuation: the tab's token is on the job's record (the reconcile re-homes the tab onto it)");
        // A record that already has a title keeps it.
        reg.Update(kParkedJobSession, [](SessionInfo& r) { r.title = L"renamed"; });
        reg.ObserveClaude(o);
        CHECK(reg.Get(kParkedJobSession)->title == L"renamed", "continuation: an existing title is never replaced by the carry");
    }

    // --- a daemon spare's placeholder record is forgotten; nothing else is ---
    {
        SessionRegistry reg;
        reg.OnHookEvent(Msg(kSpareSession, HookEvent::SessionStart)); // the spare's own SessionStart: no token
        CHECK(reg.Get(kSpareSession).has_value(), "spare: its SessionStart made a record");
        CHECK(reg.RetireBackgroundSpare(kSpareSession) && !reg.Get(kSpareSession).has_value(), "spare: the untouched placeholder record is forgotten");
        CHECK(!reg.RetireBackgroundSpare(kSpareSession) && !reg.RetireBackgroundSpare(L""), "spare: an unknown or empty id is a no-op");
        reg.OnHookEvent(Msg(L"spare-titled", HookEvent::SessionStart));
        reg.Update(L"spare-titled", [](SessionInfo& r) { r.title = L"somebody named it"; });
        CHECK(!reg.RetireBackgroundSpare(L"spare-titled") && reg.Get(L"spare-titled"), "spare: a record somebody titled is left alone");
        reg.OnHookEvent(Msg(L"spare-bound", HookEvent::SessionStart));
        reg.SetInjector(L"spare-bound", [](const std::wstring&) {});
        CHECK(!reg.RetireBackgroundSpare(L"spare-bound") && reg.Get(L"spare-bound"), "spare: a record a tab bound (an injector) is left alone");
        SessionInfo managed = MakeSession(L"spare-managed");
        managed.live = true;
        reg.Upsert(managed);
        CHECK(!reg.RetireBackgroundSpare(L"spare-managed"), "spare: a managed (non-external) record is left alone");
    }

    // --- the END of a tab-less record: forgotten without a transcript, archived with one, kept within its grace ---
    {
        SessionRegistry reg;
        HookMessage placeholder = Msg(L"tabless-placeholder", HookEvent::SessionStart);
        placeholder.ts = 1000;
        reg.OnHookEvent(placeholder);
        HookMessage ended = Msg(L"tabless-ended", HookEvent::SessionStart);
        ended.ts = 1000;
        reg.OnHookEvent(ended);
        reg.Update(L"tabless-ended", [](SessionInfo& r) {
            r.title = L"\x2442 the forked task";
            r.bgDetached = true;
        });
        SessionInfo bound = MakeSession(L"tabless-bound");
        bound.live = true;
        bound.tabToken = L"wt-b";
        reg.Upsert(bound);
        const auto candidates = reg.TablessLiveSessionIds();
        const auto has = [&](const std::wstring& id) { return std::find(candidates.begin(), candidates.end(), id) != candidates.end(); };
        CHECK(has(L"tabless-placeholder") && has(L"tabless-ended") && !has(L"tabless-bound"), "tabless: the candidates are the live records no tab binds");
        CHECK(!reg.RetireTablessSession(L"tabless-placeholder", false, 1000 + SessionRegistry::kTablessRetireGraceMs - 1) && reg.Get(L"tabless-placeholder"),
              "tabless: within its grace a record is kept (a fresh claude's presence row may not be written yet)");
        CHECK(reg.RetireTablessSession(L"tabless-placeholder", false, 1000 + SessionRegistry::kTablessRetireGraceMs) && !reg.Get(L"tabless-placeholder"),
              "tabless: a quiet placeholder that never got a conversation is forgotten");
        CHECK(reg.RetireTablessSession(L"tabless-ended", true, 1000 + SessionRegistry::kTablessRetireGraceMs + 5), "tabless: a quiet record WITH a transcript is retired...");
        const auto archived = reg.Get(L"tabless-ended");
        CHECK(archived && !archived->live && !archived->bgDetached && archived->title == L"\x2442 the forked task", "tabless: ...archived, not dropped (Rule #16: resumable)");
        CHECK(!reg.RetireTablessSession(L"tabless-bound", false, 1000000) && reg.Get(L"tabless-bound")->live, "tabless: a record with a tab token is never retired");
        reg.Update(L"tabless-bound", [](SessionInfo& r) { r.tabToken.clear(); });
        reg.SetInjector(L"tabless-bound", [](const std::wstring&) {});
        CHECK(!reg.RetireTablessSession(L"tabless-bound", false, 1000000) && reg.Get(L"tabless-bound")->live, "tabless: a record with an injector (a tab) is never retired");
    }

    // --- `/branch`: the branch keeps the tab's name as a fork title, whichever of the hook and the observer comes first ---
    {
        SessionRegistry reg;
        SessionInfo origin = MakeSession(L"branch-origin");
        origin.title = L"gaprush";
        origin.live = true;
        origin.tabToken = L"wt-branch";
        reg.Upsert(origin);
        HookMessage branch = Msg(L"branch-new", HookEvent::SessionStart);
        branch.tabToken = L"wt-branch";
        branch.source = std::wstring{ kHookSourceFork };
        reg.OnHookEvent(branch);
        CHECK(reg.Get(L"branch-new") && reg.Get(L"branch-new")->title == L"gaprush (fork)", "branch: the hook first -> the new record is titled as a fork of the tab's conversation");
        // The observer first: the bind derived the folder's name, then the hook arrives.
        SessionInfo observed = MakeSession(L"branch-late");
        observed.title = L"TraderLab-options";
        observed.titleDerivedAtBind = L"TraderLab-options";
        observed.live = true;
        observed.tabToken = L"wt-branch";
        observed.external = true;
        reg.Upsert(observed);
        HookMessage late = Msg(L"branch-late", HookEvent::SessionStart);
        late.tabToken = L"wt-branch";
        late.source = std::wstring{ kHookSourceFork };
        reg.OnHookEvent(late);
        const auto lateRec = reg.Get(L"branch-late");
        CHECK(lateRec && lateRec->title.find(L"(fork") != std::wstring::npos && lateRec->titleDerivedAtBind.empty(), "branch: the observer first -> the derived folder name is replaced by the fork title");
        // A renamed branch keeps its name; a /clear start and a token-less start take no fork title.
        SessionInfo renamed = MakeSession(L"branch-renamed");
        renamed.title = L"my branch";
        renamed.titleDerivedAtBind = L"TraderLab-options";
        renamed.live = true;
        renamed.tabToken = L"wt-branch";
        reg.Upsert(renamed);
        HookMessage again = Msg(L"branch-renamed", HookEvent::SessionStart);
        again.tabToken = L"wt-branch";
        again.source = std::wstring{ kHookSourceFork };
        reg.OnHookEvent(again);
        CHECK(reg.Get(L"branch-renamed")->title == L"my branch", "branch: a title somebody chose is kept");
        HookMessage clear = Msg(L"after-clear", HookEvent::SessionStart);
        clear.tabToken = L"wt-branch";
        clear.source = std::wstring{ kHookSourceClear };
        reg.OnHookEvent(clear);
        CHECK(reg.Get(L"after-clear") && reg.Get(L"after-clear")->title.empty(), "branch: a /clear takes no fork title (it continues nothing)");
        HookMessage daemonJob = Msg(L"daemon-fork", HookEvent::SessionStart);
        daemonJob.source = std::wstring{ kHookSourceFork };
        reg.OnHookEvent(daemonJob);
        CHECK(reg.Get(L"daemon-fork") && reg.Get(L"daemon-fork")->title.empty(), "branch: a daemon job's fork start (no token) takes no fork title");
    }
}
