// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The tests + benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
// ★ tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration / updater version+prefs
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color / engine window lifecycle
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: spawn_sched tests. Shared CHECK/fixtures/decls
// live in m5_tests.h; the runner (m5_tests.cpp) calls each entry point. See run-m5-tests.bat.
#include "m5_tests.h"

#include "../Updater.h" // version parse/compare + the settings.json skip/postpone RMW (TestUpdaterVersionLogic)

void TestSpawnBuilders()
{
    std::wprintf(L"ClaudeSpawn builders:\n");
    CHECK(ToForwardSlashes(L"C:\\a\\b") == L"C:/a/b", "to forward slashes");
    CHECK(JsonEscape(L"a\"b\\c") == L"a\\\"b\\\\c", "json escape quote+backslash");

    const auto cmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true);
    CHECK(cmd == L"claude --dangerously-skip-permissions --settings \"C:/x/s.json\" --session-id abc-123", "claude commandline (fresh, bypass)");
    const auto rcmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true);
    CHECK(rcmd == L"claude --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\"", "claude commandline (resume, bypass)");
    // skipPermissions OFF: no flag (the settings file pins permissions.defaultMode instead).
    const auto cmdNB = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, false);
    CHECK(cmdNB == L"claude --settings \"C:/x/s.json\" --session-id abc-123", "claude commandline (fresh, no bypass)");
    const auto rcmdNB = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, false);
    CHECK(rcmdNB == L"claude --resume abc-123 --settings \"C:/x/s.json\"", "claude commandline (resume, no bypass)");

    // Launcher resolution (Agentmaster — the "my friend couldn't run it" 0x80070002 fix). ConPTY's
    // CreateProcessW appends only ".exe" and ignores PATHEXT, so a bare `claude` token misses the npm
    // `claude.cmd`. Given the REAL launcher full path we emit it directly: a .exe as the quoted leading
    // token; a .cmd/.bat wrapped in `cmd /c` (CreateProcessW cannot exec a batch file directly).
    const auto exeCmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\Users\\me\\.local\\bin\\claude.exe");
    CHECK(exeCmd == L"\"C:\\Users\\me\\.local\\bin\\claude.exe\" --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\"", "claude commandline (resume, resolved .exe by full path)");
    const auto cmdResume = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\npm\\claude.cmd");
    CHECK(cmdResume == L"cmd /c \"\"C:\\npm\\claude.cmd\" --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\"\"", "claude commandline (resume, npm .cmd via cmd /c)");
    const auto cmdFresh = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true, L"", L"C:\\npm\\claude.cmd");
    CHECK(cmdFresh == L"cmd /c \"\"C:\\npm\\claude.cmd\" --dangerously-skip-permissions --settings \"C:/x/s.json\" --session-id abc-123\"", "claude commandline (fresh, npm .cmd via cmd /c, keeps --session-id)");
    // .bat resolves like .cmd, extension match is case-insensitive; no-bypass omits the flag.
    const auto batCmd = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, false, L"", L"C:\\tools\\Claude.BAT");
    CHECK(batCmd == L"cmd /c \"\"C:\\tools\\Claude.BAT\" --resume abc-123 --settings \"C:/x/s.json\"\"", "claude commandline (resume, .BAT case-insensitive, no bypass)");
    // Fork via a resolved .exe: full path leads, source conversation forked into the new id.
    const auto forkExe = BuildClaudeCommandline(L"C:/x/s.json", L"new-id", false, true, L"src-id", L"C:\\bin\\claude.exe");
    CHECK(forkExe == L"\"C:\\bin\\claude.exe\" --dangerously-skip-permissions --resume src-id --fork-session --session-id new-id --settings \"C:/x/s.json\"", "claude commandline (fork, resolved .exe)");
    // Empty launcher => the bare-token fallback (unchanged from the original behavior).
    CHECK(BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"") == rcmd, "empty launcher falls back to the bare `claude` token");

    // BuildClaudeRestartSpec (Agentmaster — the "Restart session" fix). Relaunch an EXISTING conversation
    // IN PLACE instead of replaying the launch commandline. The invariants vs the old behavior: it KEEPS
    // the conversation id (never re-keys), it NEVER forks, and it RESUMES when a transcript exists else
    // starts FRESH reusing the id. A freshly-minted random id has no transcript, so this is the
    // deterministic FRESH form — `--session-id <id>`, NOT `--resume`, NOT `--fork-session`, SAME id. (Like
    // BuildClaudeSpawn it materializes the hook files in the active profile dir; we assert only the
    // profile-dir-independent parts, so the embedded --settings path is irrelevant to the checks.)
    {
        AppSettings rst;
        rst.skipPermissions = true;
        const std::wstring rid = NewSessionId(); // random => guaranteed no transcript => the fresh form
        const auto rspec = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", rid, rst, L"C:\\bin\\claude.exe");
        CHECK(rspec.sessionId == rid, "restart spec KEEPS the conversation id (never re-keys)");
        CHECK(rspec.commandline.find(L"--session-id " + rid) != std::wstring::npos, "restart of a transcript-less id uses the FRESH form (--session-id <id>)");
        CHECK(rspec.commandline.find(L"--resume") == std::wstring::npos, "restart (no transcript) does NOT --resume");
        CHECK(rspec.commandline.find(L"--fork-session") == std::wstring::npos, "restart NEVER forks");
        CHECK(rspec.commandline.rfind(L"\"C:\\bin\\claude.exe\"", 0) == 0, "restart launches the resolved claude.exe by full path");
        bool hasSid = false, hasPipe = false;
        for (const auto& [k, v] : rspec.env)
        {
            if (k == L"CCMGR_SESSION_ID" && v == rid)
            {
                hasSid = true;
            }
            if (k == L"CCMGR_HOOK_PIPE" && v == L"\\\\.\\pipe\\agentmaster.42")
            {
                hasPipe = true;
            }
        }
        CHECK(hasSid, "restart spec env carries CCMGR_SESSION_ID = the kept id");
        CHECK(hasPipe, "restart spec env carries CCMGR_HOOK_PIPE = the bridge pipe");
    }

    // BuildClaudeRestartSpec RE-FORK (Agentmaster — "restart a fork loads a NEW claude session" fix).
    // A never-messaged fork writes NO transcript until its first turn, so the old restart's fresh
    // `--session-id <id>` form silently swapped the forked branch for an EMPTY conversation. With the
    // session's persisted forkParentId threaded in, the spec re-forks from the source INTO the same id
    // (the [restore->refork] recipe) — the own transcript, when it exists, still wins (resume, never
    // re-fork off a divergent source), and a vanished source falls back to the fresh form. Stages a
    // throwaway CLAUDE_CONFIG_DIR so ClaudeConversationExists sees exactly the fixtures.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        AppSettings rst;
        rst.skipPermissions = true;
        const std::wstring cfg = (fs::temp_directory_path(ec) / (L"am-restart-refork-" + NewSessionId())).wstring();
        wchar_t prevBuf[2048]{};
        const DWORD prevN = ::GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", prevBuf, 2048);
        const std::wstring prevCfg{ prevBuf, prevN };
        ::SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", cfg.c_str());
        const std::wstring proj = cfg + L"\\projects\\K--work-api";
        auto touch = [](const std::wstring& p) {
            std::error_code e2;
            fs::create_directories(fs::path{ p }.parent_path(), e2);
            std::ofstream f{ fs::path{ p }, std::ios::binary };
            f << "{}\n";
        };

        const std::wstring parent = NewSessionId();
        const std::wstring forkId = NewSessionId();
        touch(proj + L"\\" + parent + L".jsonl"); // the SOURCE conversation exists on disk

        // (a) fork with NO own transcript + a live source -> RE-FORK from the source into the SAME id.
        const auto a = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", forkId, rst, L"C:\\bin\\claude.exe", parent);
        CHECK(a.sessionId == forkId, "restart re-fork KEEPS the fork's id (never re-keys)");
        CHECK(a.commandline.find(L"--resume " + parent + L" --fork-session --session-id " + forkId) != std::wstring::npos, "restart of a never-messaged fork RE-FORKS from its source into the same id");

        // (b) the fork's OWN transcript exists -> plain resume wins (never re-fork a grown fork).
        touch(proj + L"\\" + forkId + L".jsonl");
        const auto b = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", forkId, rst, L"C:\\bin\\claude.exe", parent);
        CHECK(b.commandline.find(L"--resume " + forkId) != std::wstring::npos, "restart of a fork WITH its own transcript resumes it");
        CHECK(b.commandline.find(L"--fork-session") == std::wstring::npos, "own transcript wins over the fork link (no re-fork)");

        // (c) fork link set but the SOURCE transcript is gone -> the fresh form (best possible).
        const std::wstring ghostParent = NewSessionId();
        const std::wstring fork2 = NewSessionId();
        const auto c = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", fork2, rst, L"C:\\bin\\claude.exe", ghostParent);
        CHECK(c.commandline.find(L"--session-id " + fork2) != std::wstring::npos, "restart with a VANISHED fork source falls back to the fresh form (same id)");
        CHECK(c.commandline.find(L"--fork-session") == std::wstring::npos, "no re-fork off a vanished source");

        ::SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", prevCfg.empty() ? nullptr : prevCfg.c_str());
        fs::remove_all(fs::path{ cfg }, ec);
    }

    // BuildClaudeSpawn fork id selection (Agentmaster — restoring a never-messaged fork). A GENUINE
    // fork mints a FRESH target id (!= the source); a RESTORE re-fork passes forkIntoSessionId so the
    // fork branches back into its EXISTING id (preserving identity across a restart instead of churning
    // a new id every reopen). Both emit the same `--resume <src> --fork-session --session-id <target>`
    // shape — only the target id differs. (Materializes hook files in the active profile dir; we assert
    // only the profile-independent parts.)
    {
        AppSettings fst;
        fst.skipPermissions = true;
        const std::wstring src = NewSessionId();
        // Genuine fork: forkIntoSessionId omitted => a fresh, distinct id is minted as the target.
        const auto genuine = BuildClaudeSpawn(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", L"", fst, src, L"C:\\bin\\claude.exe");
        CHECK(genuine.sessionId != src && !genuine.sessionId.empty(), "genuine fork mints a fresh target id (!= source)");
        CHECK(genuine.commandline.find(L"--resume " + src + L" --fork-session --session-id " + genuine.sessionId) != std::wstring::npos, "genuine fork commandline forks the source into the minted id");
        // Re-fork: forkIntoSessionId == the fork's existing id => spec targets THAT id (no churn).
        const std::wstring keep = NewSessionId();
        const auto refork = BuildClaudeSpawn(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", L"", fst, src, L"C:\\bin\\claude.exe", keep);
        CHECK(refork.sessionId == keep, "re-fork targets the fork's existing id (forkIntoSessionId), preserving identity");
        CHECK(refork.commandline.find(L"--resume " + src + L" --fork-session --session-id " + keep) != std::wstring::npos, "re-fork commandline forks the source into the SAME id");
        // forkIntoSessionId is honored only when forking: a plain resume ignores it (reuses the resume id).
        const auto resumeIgnores = BuildClaudeSpawn(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", keep, fst, L"", L"C:\\bin\\claude.exe", L"some-other-id");
        CHECK(resumeIgnores.sessionId == keep, "forkIntoSessionId is ignored on a non-fork (resume reuses its id)");
    }

    // ResolveClaudeExeIn (native-exe-only policy): resolve a real claude.exe; a .cmd is only a
    // breadcrumb to its npm binary; a pure-Node .cmd (no binary) resolves to empty (gated). Temp fixture.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::wstring root = (fs::temp_directory_path(ec) / (L"am-claude-exe-" + NewSessionId())).wstring();
        const std::wstring binExe = root + L"\\binexe";
        const std::wstring binCmd = root + L"\\bincmd";
        const std::wstring binNode = root + L"\\binnode";
        const std::wstring home = root + L"\\home";
        const std::wstring exeDirect = binExe + L"\\claude.exe";
        const std::wstring npmExe = binCmd + L"\\node_modules\\@anthropic-ai\\claude-code-win32-x64\\claude.exe";
        const std::wstring homeExe = home + L"\\.local\\bin\\claude.exe";
        auto touch = [](const std::wstring& p) {
            std::error_code e2;
            fs::create_directories(fs::path{ p }.parent_path(), e2);
            std::ofstream f{ fs::path{ p }, std::ios::binary };
            f << "x";
        };
        touch(exeDirect);
        touch(binCmd + L"\\claude.cmd");
        touch(npmExe);
        touch(binNode + L"\\claude.cmd"); // a pure-Node install: a .cmd but NO native binary anywhere
        touch(homeExe);

        // 1. A valid override (an existing .exe) wins outright.
        CHECK(ResolveClaudeExeIn(exeDirect, {}, L"") == exeDirect, "ResolveClaudeExe: valid .exe override");
        // 2. A .cmd override is rejected (exe-only) -> empty.
        CHECK(ResolveClaudeExeIn(binCmd + L"\\claude.cmd", {}, L"").empty(), "ResolveClaudeExe: .cmd override rejected");
        // 3. A nonexistent .exe override -> empty.
        CHECK(ResolveClaudeExeIn(root + L"\\nope.exe", {}, L"").empty(), "ResolveClaudeExe: missing .exe override -> empty");
        // 4. A direct claude.exe on PATH beats an earlier dir's .cmd (step 2 runs before the .cmd follow).
        CHECK(ResolveClaudeExeIn(L"", { binCmd, binExe }, L"") == exeDirect, "ResolveClaudeExe: PATH claude.exe preferred over a .cmd");
        // 5. Follow a claude.cmd to its npm native binary (home empty so the ~/.local/bin step is skipped).
        CHECK(ResolveClaudeExeIn(L"", { binCmd }, L"") == npmExe, "ResolveClaudeExe: follow .cmd -> npm node_modules claude.exe");
        // 6. A pure-Node .cmd (no native binary anywhere) -> empty (gated, by design).
        CHECK(ResolveClaudeExeIn(L"", { binNode }, L"").empty(), "ResolveClaudeExe: pure-node .cmd -> not detected");
        // 7. The ~/.local/bin native install (step 3) beats chasing a .cmd's node_modules (step 4).
        CHECK(ResolveClaudeExeIn(L"", { binNode }, home) == homeExe, "ResolveClaudeExe: ~/.local/bin claude.exe fallback");

        fs::remove_all(fs::path{ root }, ec);
    }
    // Codex (managed-session support): with no resolved launcher, the bare token (back-compat +
    // the not-found path): bare `codex` fresh; `codex resume <uuid>` to continue a rollout; and
    // `codex fork <uuid>` to branch one (the adopt-a-live-external two-writers fix). Signature is
    // (resumeUuid, forkUuid, launcher) — fork WINS over resume when both are set.
    CHECK(BuildCodexCommandline(L"") == L"codex", "codex commandline (fresh, no launcher)");
    CHECK(BuildCodexCommandline(L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a") == L"codex resume 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (resume by rollout uuid, no launcher)");
    CHECK(BuildCodexCommandline(L"", L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a") == L"codex fork 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (fork by rollout uuid, no launcher)");
    CHECK(BuildCodexCommandline(L"aaaa", L"bbbb") == L"codex fork bbbb", "codex commandline (fork wins over resume)");
    // With a resolved launcher, codex is invoked BY FULL PATH — ConPTY's CreateProcessW ignores
    // PATHEXT, so a bare `codex` misses an npm codex.cmd and dies 0x80070002 (ERROR_FILE_NOT_FOUND).
    // A .exe runs directly (quoted, so a space in the path is safe).
    CHECK(BuildCodexCommandline(L"", L"", L"C:\\bin\\codex.exe") == L"\"C:\\bin\\codex.exe\"", "codex commandline (fresh, .exe full path)");
    CHECK(BuildCodexCommandline(L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"", L"C:\\bin\\codex.exe") == L"\"C:\\bin\\codex.exe\" resume 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (resume, .exe full path)");
    CHECK(BuildCodexCommandline(L"", L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"C:\\bin\\codex.exe") == L"\"C:\\bin\\codex.exe\" fork 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex commandline (fork, .exe full path)");
    // A .cmd/.bat is a batch script CreateProcessW cannot execute directly, so it goes via `cmd /c`,
    // wrapped in ONE outer quote pair so the inner launcher quote survives (cmd strips only the
    // first+last quote of the remainder — the >2-quotes case; mirrors BuildClaudeCommandline).
    CHECK(BuildCodexCommandline(L"", L"", L"C:\\npm\\codex.cmd") == L"cmd /c \"\"C:\\npm\\codex.cmd\"\"", "codex commandline (fresh, .cmd via cmd /c)");
    CHECK(BuildCodexCommandline(L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"", L"C:\\npm\\codex.cmd") == L"cmd /c \"\"C:\\npm\\codex.cmd\" resume 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a\"", "codex commandline (resume, .cmd via cmd /c)");
    CHECK(BuildCodexCommandline(L"", L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", L"C:\\npm\\codex.cmd") == L"cmd /c \"\"C:\\npm\\codex.cmd\" fork 019ec0c7-a4e3-7c73-8c57-9f83ecb1903a\"", "codex commandline (fork, .cmd via cmd /c)");
    CHECK(BuildCodexCommandline(L"", L"", L"C:\\path with space\\codex.bat") == L"cmd /c \"\"C:\\path with space\\codex.bat\"\"", "codex commandline (.bat with spaces via cmd /c)");

    const auto json = BuildHooksSettingsJson(L"C:/x/agentmaster-hook.ps1", L"", true, true);
    CHECK(json.find(L"\"hooks\"") != std::wstring::npos, "settings has hooks");
    CHECK(json.find(L"SessionStart") != std::wstring::npos, "settings has SessionStart");
    CHECK(json.find(L"UserPromptSubmit") != std::wstring::npos, "settings has UserPromptSubmit");
    CHECK(json.find(L"Notification") != std::wstring::npos, "settings has Notification");
    CHECK(json.find(L"Stop") != std::wstring::npos, "settings has Stop");
    CHECK(json.find(L"SubagentStop") != std::wstring::npos, "settings has SubagentStop");
    CHECK(json.find(L"SessionEnd") != std::wstring::npos, "settings has SessionEnd");
    CHECK(json.find(L"-Event Stop") != std::wstring::npos, "settings wires -Event Stop");
    CHECK(json.find(L"C:/x/agentmaster-hook.ps1") != std::wstring::npos, "settings references forwarder");
    // All-defaults (bypass on, model empty, co-authored on) => hooks-only, no extra keys.
    CHECK(json.find(L"\"model\"") == std::wstring::npos, "default settings omit model");
    CHECK(json.find(L"includeCoAuthoredBy") == std::wstring::npos, "default settings omit includeCoAuthoredBy");
    CHECK(json.find(L"permissions") == std::wstring::npos, "bypass settings omit permissions.defaultMode");
    // Non-default Claude fields are emitted; no-bypass pins permissions.defaultMode ("other variation").
    const auto json2 = BuildHooksSettingsJson(L"C:/x/agentmaster-hook.ps1", L"sonnet", false, false);
    CHECK(json2.find(L"\"model\": \"sonnet\"") != std::wstring::npos, "settings emit model when set");
    CHECK(json2.find(L"\"includeCoAuthoredBy\": false") != std::wstring::npos, "settings emit includeCoAuthoredBy:false");
    CHECK(json2.find(L"\"defaultMode\": \"default\"") != std::wstring::npos, "no-bypass pins permissions.defaultMode");

    // ParseEnvAssignments: ';'-delimited NAME=VALUE -> pairs (for AppSettings.env).
    {
        const auto e = ParseEnvAssignments(L" FOO=bar ; HTTPS_PROXY=http://h:8080 ; PATH=a=b=c ");
        CHECK(e.size() == 3, "env parse: three entries");
        CHECK(e[0].first == L"FOO" && e[0].second == L"bar", "env parse: trims + splits FOO=bar");
        CHECK(e[1].first == L"HTTPS_PROXY" && e[1].second == L"http://h:8080", "env parse: value keeps :// ");
        CHECK(e[2].first == L"PATH" && e[2].second == L"a=b=c", "env parse: value keeps later '='");
        const auto empty = ParseEnvAssignments(L"");
        CHECK(empty.empty(), "env parse: empty -> none");
        const auto bad = ParseEnvAssignments(L"NOEQUALS;=noname;;GOOD=1");
        CHECK(bad.size() == 1 && bad[0].first == L"GOOD", "env parse: skips malformed/empty entries");
    }

    // ParseEnvAssignments: newline-delimited (the multi-line editor) + '#' comments + mixed separators.
    {
        const auto e = ParseEnvAssignments(L"FOO=bar\nHTTPS_PROXY=http://h:8080\n# a comment\n\nBAZ=1");
        CHECK(e.size() == 3, "env parse: newline split, blank + '#' comment skipped");
        CHECK(e[0].first == L"FOO" && e[0].second == L"bar", "env parse: newline FOO=bar");
        CHECK(e[1].first == L"HTTPS_PROXY" && e[1].second == L"http://h:8080", "env parse: newline keeps ://");
        CHECK(e[2].first == L"BAZ" && e[2].second == L"1", "env parse: entry after comment + blank");
        const auto crlf = ParseEnvAssignments(L"A=1\r\nB=2\r\n");
        CHECK(crlf.size() == 2 && crlf[0].first == L"A" && crlf[1].first == L"B", "env parse: CRLF lines");
        const auto mixed = ParseEnvAssignments(L"A=1;B=2\nC=3");
        CHECK(mixed.size() == 3, "env parse: ';' and newline both separate (back-compat)");
        const auto comment = ParseEnvAssignments(L"#FOO=bar");
        CHECK(comment.empty(), "env parse: a '#'-led line is a comment even with '='");
    }

    // ParseLaunchModels (Agentmaster, launch-model picker): "Display name | model-id" lines ->
    // ordered {name, id} pairs feeding every "Open New Session Here" submenu.
    {
        // The SHIPPED defaults (AppSettings.launchModels seed) parse to the three advertised models.
        const auto d = ParseLaunchModels(kDefaultLaunchModels);
        CHECK(d.size() == 3, "launch models: shipped defaults -> 3 entries");
        CHECK(d[0].first == L"Fable 5" && d[0].second == L"claude-fable-5", "launch models: default #1 Fable 5 | claude-fable-5");
        CHECK(d[1].first == L"Opus 4.8" && d[1].second == L"claude-opus-4-8", "launch models: default #2 Opus 4.8 | claude-opus-4-8");
        CHECK(d[2].first == L"Sonnet 5" && d[2].second == L"claude-sonnet-5", "launch models: default #3 Sonnet 5 | claude-sonnet-5");
        // '|' splits on the FIRST bar, both sides trimmed; entries split on newline OR ';'; a
        // TextBox's \r-normalized text (the cog editor round-trip) parses identically.
        const auto e = ParseLaunchModels(L"  Opus 4.6 | claude-opus-4-6  ;Custom|my|model\rHaiku 4.5 | claude-haiku-4-5-20251001");
        CHECK(e.size() == 3, "launch models: ';' / '\\r' both separate");
        CHECK(e[0].first == L"Opus 4.6" && e[0].second == L"claude-opus-4-6", "launch models: trims around name and id");
        CHECK(e[1].first == L"Custom" && e[1].second == L"my|model", "launch models: FIRST '|' splits (id keeps later bars)");
        CHECK(e[2].first == L"Haiku 4.5" && e[2].second == L"claude-haiku-4-5-20251001", "launch models: '\\r'-separated entry parses");
        // A bare token (no '|') is BOTH name and id; blanks / '#' comments / empty-side entries skip.
        const auto b = ParseLaunchModels(L"claude-sonnet-5\n# a comment\n\n | missing-name\nmissing-id | \nOK | ok-id");
        CHECK(b.size() == 2, "launch models: bare token kept; comment/blank/empty-side skipped");
        CHECK(b[0].first == L"claude-sonnet-5" && b[0].second == L"claude-sonnet-5", "launch models: bare token is its own label");
        CHECK(b[1].first == L"OK" && b[1].second == L"ok-id", "launch models: entry after skips parses");
        CHECK(ParseLaunchModels(L"").empty(), "launch models: empty spec -> none (submenus offer just Default)");
        // The kMaxLaunchModels cap bounds a runaway settings edit.
        std::wstring big;
        for (int i = 0; i < 50; ++i)
        {
            big += L"M" + std::to_wstring(i) + L" | id-" + std::to_wstring(i) + L"\n";
        }
        CHECK(ParseLaunchModels(big).size() == kMaxLaunchModels, "launch models: capped at kMaxLaunchModels");
    }

    // BuildClaudeCommandline modelOverride (Agentmaster, launch-model picker): the per-LAUNCH
    // `--model <id>` pick — appended LAST on every form, quoted only when it carries whitespace,
    // absent when empty (the pre-picker commandline, byte-identical).
    {
        const auto m = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true, L"", L"", L"claude-opus-4-8");
        CHECK(m == L"claude --dangerously-skip-permissions --settings \"C:/x/s.json\" --session-id abc-123 --model claude-opus-4-8", "model override: fresh form appends --model last");
        const auto mFork = BuildClaudeCommandline(L"C:/x/s.json", L"new-id", false, true, L"src-id", L"C:\\bin\\claude.exe", L"claude-fable-5");
        CHECK(mFork == L"\"C:\\bin\\claude.exe\" --dangerously-skip-permissions --resume src-id --fork-session --session-id new-id --settings \"C:/x/s.json\" --model claude-fable-5", "model override: fork form appends --model last");
        const auto mBatch = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\npm\\claude.cmd", L"claude-sonnet-5");
        CHECK(mBatch == L"cmd /c \"\"C:\\npm\\claude.cmd\" --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\" --model claude-sonnet-5\"", "model override: rides INSIDE the cmd /c outer-quote wrap");
        const auto mWs = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true, L"", L"", L"my model");
        CHECK(mWs.find(L"--model \"my model\"") != std::wstring::npos, "model override: whitespace-carrying value is quoted");
        CHECK(BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true, L"", L"", L"") ==
                  BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true),
              "model override: empty == no flag (byte-identical to the pre-picker form)");
    }

    // MergeSessionEnv: per-dir overrides global (case-insensitive name match), CCMGR_* dropped, last-wins.
    {
        const auto m = MergeSessionEnv(L"FOO=global\nBAR=keep\nCCMGR_X=nope", L"foo=perdir\nNEW=1");
        CHECK(m.size() == 3, "merge: FOO/BAR/NEW (CCMGR_ dropped)");
        bool fooOk = false, barOk = false, newOk = false, ccmgr = false;
        for (const auto& [k, v] : m)
        {
            if (k == L"FOO")
            {
                fooOk = (v == L"perdir"); // per-dir wins; first-seen NAME spelling "FOO" kept
            }
            if (k == L"BAR")
            {
                barOk = (v == L"keep");
            }
            if (k == L"NEW")
            {
                newOk = (v == L"1");
            }
            if (k.rfind(L"CCMGR_", 0) == 0)
            {
                ccmgr = true;
            }
        }
        CHECK(fooOk, "merge: per-dir 'foo' overrides global 'FOO' (case-insensitive)");
        CHECK(barOk, "merge: global-only BAR kept");
        CHECK(newOk, "merge: per-dir-only NEW added");
        CHECK(!ccmgr, "merge: CCMGR_* dropped");
        const auto dup = MergeSessionEnv(L"A=1\nA=2", L"");
        CHECK(dup.size() == 1 && dup[0].second == L"2", "merge: duplicate name -> last value wins");
        CHECK(MergeSessionEnv(L"", L"").empty(), "merge: empty -> none");
    }

    // LexEnvText: per-line verdicts + worst level + first-issue (drives the cog border + status line).
    {
        const auto okr = LexEnvText(L"FOO=bar\nBAZ=1");
        CHECK(okr.ok == 2 && okr.warn == 0 && okr.error == 0, "lex: two valid vars");
        CHECK(okr.worst == EnvLineKind::Ok, "lex: all-ok worst == Ok");

        const auto err = LexEnvText(L"FOO=bar\nNOEQUALS\n=noname\n1BAD=x");
        CHECK(err.error == 3, "lex: missing '=' + empty name + invalid name => 3 errors");
        CHECK(err.worst == EnvLineKind::Error, "lex: worst == Error");
        CHECK(err.firstIssueLine == 2, "lex: first issue on line 2 (NOEQUALS)");

        const auto warn = LexEnvText(L"FOO=1\nCCMGR_X=2\nAM_SESSION=3\nFOO=4");
        CHECK(warn.error == 0 && warn.warn == 3, "lex: reserved + owned + duplicate => 3 warns");
        CHECK(warn.worst == EnvLineKind::Warn, "lex: worst == Warn (no errors)");
        CHECK(warn.ok == 1, "lex: only the first FOO counts as a valid var");

        const auto ign = LexEnvText(L"\n# comment\n   ");
        CHECK(ign.ok == 0 && ign.warn == 0 && ign.error == 0, "lex: blanks + comment => nothing");
    }

    // LexLaunchModelsText (launch-model picker): per-entry verdicts + worst level + first-issue,
    // driving the "Launch models" editor's live border + status line — the LexEnvText twin. `ok`
    // is the OFFERED model count (Ok + listed Warns), i.e. exactly the pickers' submenu size.
    {
        // The shipped defaults lex clean: 3 offered, all-Ok, green.
        const auto d = LexLaunchModelsText(kDefaultLaunchModels);
        CHECK(d.ok == 3 && d.warn == 0 && d.error == 0, "models lex: shipped defaults => 3 offered, clean");
        CHECK(d.worst == EnvLineKind::Ok, "models lex: defaults worst == Ok");

        // Errors — an empty side is SKIPPED by the parser: missing name / missing id / bare '|'.
        const auto err = LexLaunchModelsText(L"Good | good-id\n | no-name\nno-id | \n|");
        CHECK(err.error == 3, "models lex: missing-name + missing-id + bare '|' => 3 errors");
        CHECK(err.ok == 1, "models lex: only the good entry is offered");
        CHECK(err.worst == EnvLineKind::Error, "models lex: worst == Error");
        CHECK(err.firstIssueLine == 2, "models lex: first issue on line 2 (missing name)");
        CHECK(ParseLaunchModels(L"Good | good-id\n | no-name\nno-id | \n|").size() == 1, "models lex: parser agrees — 1 accepted");

        // Warns are LISTED (they count into ok): a duplicate display name (ASCII-case-insensitive)
        // and a whitespace-carrying model id; a bare token is plain Ok (its own label).
        const auto warn = LexLaunchModelsText(L"Opus | claude-opus-4-8\nopus | claude-opus-4-6\nX | my model\nclaude-sonnet-5");
        CHECK(warn.error == 0 && warn.warn == 2, "models lex: duplicate name + spaced id => 2 warns");
        CHECK(warn.ok == 4, "models lex: all four entries are still OFFERED (ok counts listed warns)");
        CHECK(warn.worst == EnvLineKind::Warn, "models lex: worst == Warn (no errors)");
        CHECK(warn.firstIssueLine == 2, "models lex: first issue on line 2 (duplicate name)");
        CHECK(ParseLaunchModels(L"Opus | claude-opus-4-8\nopus | claude-opus-4-6\nX | my model\nclaude-sonnet-5").size() == 4, "models lex: parser agrees — 4 accepted");

        // ';' splits entries WITHIN a line like the parser; both carry that line's number.
        const auto semi = LexLaunchModelsText(L"A | a-1; B | b-1");
        CHECK(semi.ok == 2 && semi.error == 0 && semi.warn == 0, "models lex: ';' splits two entries on one line");
        CHECK(semi.lines.size() == 2 && semi.lines[0].line == 1 && semi.lines[1].line == 1, "models lex: both ';' entries report line 1");

        // The kMaxLaunchModels cap: entry #33+ is Warn "not offered"; ok stays exactly the cap —
        // and the parser drops the same tail, so status count == submenu size.
        std::wstring big;
        for (int i = 0; i < 40; ++i)
        {
            big += L"M" + std::to_wstring(i) + L" | id-" + std::to_wstring(i) + L"\n";
        }
        const auto capped = LexLaunchModelsText(big);
        CHECK(capped.ok == kMaxLaunchModels, "models lex: ok capped at kMaxLaunchModels (the true offered count)");
        CHECK(capped.warn == 40 - kMaxLaunchModels, "models lex: every past-cap entry warns (dropped)");
        CHECK(capped.firstIssueLine == static_cast<uint32_t>(kMaxLaunchModels + 1), "models lex: first issue on the first past-cap line");
        CHECK(ParseLaunchModels(big).size() == capped.ok, "models lex: parser agrees — offered == ok");

        // Blanks + comments are Ignored (no counts, neutral).
        const auto ign = LexLaunchModelsText(L"\n# Fable 5 | claude-fable-5\n   ");
        CHECK(ign.ok == 0 && ign.warn == 0 && ign.error == 0, "models lex: blanks + comment => nothing");
    }

    // dir-env.json round-trip (Serialize/Deserialize; pure — no disk).
    {
        std::vector<std::pair<std::wstring, std::wstring>> entries{
            { L"c:\\work\\proj", L"AWS_PROFILE=dev\nFOO=bar" },
            { L"c:\\work\\other", L"X=1" },
        };
        const auto back = DeserializeDirEnv(SerializeDirEnv(entries));
        CHECK(back.size() == 2, "dir-env: round-trip two entries");
        CHECK(back[0].first == L"c:\\work\\proj" && back[0].second == L"AWS_PROFILE=dev\nFOO=bar", "dir-env: multi-line env survives");
        std::vector<std::pair<std::wstring, std::wstring>> withBlank{ { L"c:\\d", L"   " }, { L"c:\\e", L"Y=2" } };
        const auto back2 = DeserializeDirEnv(SerializeDirEnv(withBlank));
        CHECK(back2.size() == 1 && back2[0].first == L"c:\\e", "dir-env: blank-env entry dropped on load");
    }

    // ApplyEnvDefaults (ENV_VARS.md §8): one-time seed of the shipped GLOBAL env defaults, version-gated.
    {
        // Fresh install (version 0, empty env): CLAUDE_CODE_MAX_RETRIES=50000 appended; version -> current.
        auto [env1, ver1] = ApplyEnvDefaults(L"", 0);
        CHECK(ver1 == kEnvDefaultsVersion, "env-defaults: version bumped to current");
        CHECK(env1.find(L"CLAUDE_CODE_MAX_RETRIES=50000") != std::wstring::npos, "env-defaults: seeds CLAUDE_CODE_MAX_RETRIES=50000");

        // Already current: nothing re-added (a user who deleted it keeps it gone).
        auto [env2, ver2] = ApplyEnvDefaults(L"", kEnvDefaultsVersion);
        CHECK(env2.empty() && ver2 == kEnvDefaultsVersion, "env-defaults: no re-seed once current version reached");

        // User already has the NAME (any case): never duplicated; version still advances.
        auto [env3, ver3] = ApplyEnvDefaults(L"claude_code_max_retries=7", 0);
        CHECK(env3 == L"claude_code_max_retries=7" && ver3 == kEnvDefaultsVersion, "env-defaults: existing NAME (case-insensitive) not duplicated");

        // Non-empty env without the default: appended on its OWN line (no run-together).
        auto [env4, ver4] = ApplyEnvDefaults(L"FOO=bar", 0);
        CHECK(env4 == L"FOO=bar\nCLAUDE_CODE_MAX_RETRIES=50000", "env-defaults: appended on a fresh line");
    }

    // UpsertJsonNumberKey (ENV_VARS.md §8): the PURE RMW core of the Claude user settings.json repository.
    {
        // Empty input + set => a fresh object carrying the key.
        const auto fresh = UpsertJsonNumberKey(L"", L"cleanupPeriodDays", 36500.0);
        const auto p1 = fresh ? json::Parse(*fresh) : std::nullopt;
        CHECK(p1 && p1->I64At(L"cleanupPeriodDays", 0) == 36500, "upsert: sets the number on an empty doc");

        // Preserve other keys; upsert (no duplicate) when the key already exists.
        const auto merged = UpsertJsonNumberKey(L"{\"a\":1,\"b\":\"x\",\"cleanupPeriodDays\":30}", L"cleanupPeriodDays", 36500.0);
        const auto p2 = merged ? json::Parse(*merged) : std::nullopt;
        CHECK(p2 && p2->I64At(L"a", 0) == 1 && p2->StrAt(L"b") == L"x", "upsert: preserves other keys");
        CHECK(p2 && p2->I64At(L"cleanupPeriodDays", 0) == 36500, "upsert: updates the existing key value");
        int dupCount = 0;
        if (p2)
        {
            for (const auto& kv : p2->members)
            {
                if (kv.first == L"cleanupPeriodDays")
                {
                    ++dupCount;
                }
            }
        }
        CHECK(dupCount == 1, "upsert: exactly one cleanupPeriodDays member (no duplicate appended)");

        // nullopt removes the key, keeping the rest.
        const auto removed = UpsertJsonNumberKey(L"{\"a\":1,\"cleanupPeriodDays\":30}", L"cleanupPeriodDays", std::nullopt);
        const auto p3 = removed ? json::Parse(*removed) : std::nullopt;
        CHECK(p3 && p3->Find(L"cleanupPeriodDays") == nullptr && p3->I64At(L"a", 0) == 1, "upsert: nullopt removes the key, keeps the rest");

        // A non-empty, non-object file is NEVER clobbered (caller must not overwrite).
        CHECK(!UpsertJsonNumberKey(L"this is not json", L"cleanupPeriodDays", 36500.0).has_value(), "upsert: refuses an unparseable non-empty file");
        CHECK(!UpsertJsonNumberKey(L"[1,2,3]", L"cleanupPeriodDays", 36500.0).has_value(), "upsert: refuses a non-object JSON (array)");
    }

    // PsSingleQuote: PowerShell single-quoted literal (only escape = doubled quote).
    CHECK(PsSingleQuote(L"C:\\Users\\x\\.agentmaster") == L"'C:\\Users\\x\\.agentmaster'", "ps quote: plain path verbatim");
    CHECK(PsSingleQuote(L"C:\\Users\\o'brien") == L"'C:\\Users\\o''brien'", "ps quote: embedded quote doubled");
    CHECK(PsSingleQuote(L"$env:FOO") == L"'$env:FOO'", "ps quote: $ stays inert");

    const auto fwd = BuildForwarderScript(L"C:\\Users\\x\\.agentmaster-dev");
    CHECK(fwd.find(L"NamedPipeClientStream") != std::wstring::npos, "forwarder uses NamedPipeClientStream");
    CHECK(fwd.find(L"CCMGR_SESSION_ID") != std::wstring::npos, "forwarder reads CCMGR_SESSION_ID (launch-id fallback)");
    CHECK(fwd.find(L"CCMGR_HOOK_PIPE") != std::wstring::npos, "forwarder reads CCMGR_HOOK_PIPE");
    CHECK(fwd.find(L"session_id") != std::wstring::npos, "forwarder uses payload session_id");
    // Payload-FIRST precedence: the hook's session_id (Claude's CURRENT conversation, which follows
    // /resume,/clear,/compact) must be chosen BEFORE the launch-time env fallback. Env-first
    // stranded a diverged managed session as a phantom record stuck in the last mis-attributed
    // state (no transcript under the dead launch id => the scanner could never reconcile it).
    {
        const auto payloadAt = fwd.find(L"$sid = [string]$j.session_id");
        const auto envAt = fwd.find(L"$sid = $env:CCMGR_SESSION_ID");
        CHECK(payloadAt != std::wstring::npos && envAt != std::wstring::npos && payloadAt < envAt,
              "forwarder prefers payload session_id over the launch env (id follows /resume,/clear,/compact)");
    }
    CHECK(fwd.find(L"WT_SESSION") != std::wstring::npos, "forwarder emits WT_SESSION tabToken");
    // The bridge-discovery fallback is PER-PROFILE: the stateDir is baked in (PS-single-quoted);
    // no profile-blind $env:USERPROFILE\.agentmaster path and no unexpanded placeholder remain.
    CHECK(fwd.find(L"$disc = 'C:\\Users\\x\\.agentmaster-dev\\bridge.json'") != std::wstring::npos, "forwarder discovery is the per-profile bridge.json");
    CHECK(fwd.find(L"Join-Path $env:USERPROFILE") == std::wstring::npos, "forwarder discovery is not profile-blind");
    CHECK(fwd.find(L"{{AM_BRIDGE_JSON}}") == std::wstring::npos, "forwarder placeholder fully substituted");
    // #5 (forwarder silent failures): every dropped delivery leaves a local trace — the bridge's
    // hooks.log only sees lines that ARRIVED, so a dead pipe / stale bridge.json was invisible.
    CHECK(fwd.find(L"$AmErrLog = 'C:\\Users\\x\\.agentmaster-dev\\forwarder-errors.log'") != std::wstring::npos, "forwarder error log is per-profile");
    CHECK(fwd.find(L"{{AM_FWD_ERRLOG}}") == std::wstring::npos, "error-log placeholder fully substituted");
    CHECK(fwd.find(L"NoteFwdDrop \"drop: no session id") != std::wstring::npos, "no-sid drop is noted");
    CHECK(fwd.find(L"NoteFwdDrop \"drop: no pipe") != std::wstring::npos, "no-pipe drop is noted");
    CHECK(fwd.find(L"NoteFwdDrop (\"drop: \" + $_.Exception.Message)") != std::wstring::npos, "pipe connect/write failure is noted");

    const auto id = NewSessionId();
    CHECK(id.size() == 36, "uuid length 36");
    CHECK(id[8] == L'-' && id[13] == L'-' && id[18] == L'-' && id[23] == L'-', "uuid hyphens");

    // BuildPwshHostedCommandline (Agentmaster — "claude run from a pwsh terminal"). Wrap an inner agent
    // command line so the ConPTY root is an interactive pwsh that -NoExit's to a live prompt at the cwd
    // when the agent quits, instead of the connection dying into a dead "press Enter to restart" pane.
    // -EncodedCommand carries the script as base64 UTF-16LE, side-stepping ALL nested-quote escaping —
    // so we decode the payload here and assert it is exactly the call-operator (&) invocation of inner.
    {
        const std::wstring inner = L"\"C:\\bin\\claude.exe\" --resume abc-123 --settings \"C:/x/s.json\"";
        const auto hosted = BuildPwshHostedCommandline(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe", inner);
        CHECK(hosted.rfind(L"\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo -NoExit -EncodedCommand ", 0) == 0,
              "pwsh host: quoted full path + -NoLogo -NoExit -EncodedCommand");
        // Decode the base64 payload (the last space-delimited token; base64 has no spaces) back to a
        // wstring (its bytes are UTF-16LE) and confirm it round-trips to '& <inner>'.
        auto b64decodeToWide = [](const std::wstring& b64) -> std::wstring {
            auto val = [](wchar_t c) -> int {
                if (c >= L'A' && c <= L'Z')
                    return c - L'A';
                if (c >= L'a' && c <= L'z')
                    return c - L'a' + 26;
                if (c >= L'0' && c <= L'9')
                    return c - L'0' + 52;
                if (c == L'+')
                    return 62;
                if (c == L'/')
                    return 63;
                return -1; // '=' padding / anything else
            };
            std::vector<unsigned char> bytes;
            int acc = 0, accBits = 0;
            for (const wchar_t c : b64)
            {
                const int v = val(c);
                if (v < 0)
                    continue;
                acc = (acc << 6) | v;
                accBits += 6;
                if (accBits >= 8)
                {
                    accBits -= 8;
                    bytes.push_back(static_cast<unsigned char>((acc >> accBits) & 0xFF));
                }
            }
            std::wstring out;
            for (size_t i = 0; i + 1 < bytes.size(); i += 2)
            {
                out.push_back(static_cast<wchar_t>(bytes[i] | (static_cast<unsigned>(bytes[i + 1]) << 8)));
            }
            return out;
        };
        const auto enc = hosted.substr(hosted.rfind(L' ') + 1);
        CHECK(!enc.empty(), "pwsh host: non-empty -EncodedCommand payload");
        CHECK(b64decodeToWide(enc) == L"& " + inner, "pwsh host: payload decodes to '& <inner>' (UTF-16LE, & call operator)");
        // Empty launcher => the bare `pwsh.exe` token (the not-found fallback).
        const auto hostedBare = BuildPwshHostedCommandline(L"", inner);
        CHECK(hostedBare.rfind(L"\"pwsh.exe\" -NoLogo -NoExit -EncodedCommand ", 0) == 0, "pwsh host: empty launcher -> quoted bare pwsh.exe token");
        // A bare inner token (codex fresh, or claude's empty-launcher fallback) is still invoked via &.
        const auto hostedCodex = BuildPwshHostedCommandline(L"C:\\bin\\pwsh.exe", L"codex resume 019ec0c7");
        CHECK(b64decodeToWide(hostedCodex.substr(hostedCodex.rfind(L' ') + 1)) == L"& codex resume 019ec0c7", "pwsh host: bare inner token invoked via &");
    }
}

// Agentmaster: the per-install state PROFILE (ProfileBootstrap.h) — the pure pieces: the choice
// file round-trip, the resolution precedence's env override, and the per-identity defaults.
// (The picker itself is UI; the packaged-PFN branches need a package context — both untestable
// headless. Tests run unpackaged, so PackageKey() must be "Unpackaged" and the silent DEFAULT
// must be the HISTORICAL ~/.agentmaster — the invariant that keeps old tooling/headless hosts
// stable. The harness itself no longer RUNS there: run-m5-tests.bat / wmain point
// AGENTMASTER_PROFILE at a %TEMP% scratch profile so engine traces + test window records never
// land in the LIVE release install's state dir.)
void TestProfileBootstrap()
{
    namespace P = ::Agentmaster::Profiles;
    std::wprintf(L"ProfileBootstrap (profiles):\n");

    CHECK(P::PackageFamilyName().empty(), "unpackaged: no package family");
    CHECK(!P::IsDevPackage(), "unpackaged: not the dev package");
    CHECK(P::PackageKey() == L"Unpackaged", "unpackaged: choice key");

    const auto rel = P::DefaultReleaseProfileDir();
    const auto dev = P::DefaultDevProfileDir();
    CHECK(rel.size() > 12 && rel.compare(rel.size() - 12, 12, L".agentmaster") == 0, "release default ends .agentmaster");
    CHECK(dev.size() > 16 && dev.compare(dev.size() - 16, 16, L".agentmaster-dev") == 0, "dev default ends .agentmaster-dev");
    CHECK(P::DefaultProfileDir() == rel, "unpackaged silent default == the historical ~/.agentmaster");

    // Choice-file round-trip on a temp path (never the real %USERPROFILE% map).
    wchar_t tmpDir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring file = std::wstring{ tmpDir } + L"am-profiles-test-" + NewSessionId() + L".txt";
    {
        CHECK(P::ReadChoiceFile(file).empty(), "choice file: missing -> empty map");
        std::map<std::wstring, std::wstring> m;
        m[L"Agentmaster_56k4f06dsfp9r"] = L"C:\\Users\\x\\.agentmaster";
        m[L"AgentmasterDev_56k4f06dsfp9r"] = L"C:\\Users\\x\\.agentmaster-dev";
        m[L"Unpackaged"] = L"Q:\\profiles\\portable one"; // space survives (no quoting needed)
        CHECK(P::WriteChoiceFile(file, m), "choice file: write");
        const auto r = P::ReadChoiceFile(file);
        CHECK(r.size() == 3, "choice file: three entries back");
        CHECK(r.at(L"Agentmaster_56k4f06dsfp9r") == L"C:\\Users\\x\\.agentmaster", "choice file: release slot");
        CHECK(r.at(L"AgentmasterDev_56k4f06dsfp9r") == L"C:\\Users\\x\\.agentmaster-dev", "choice file: dev slot");
        CHECK(r.at(L"Unpackaged") == L"Q:\\profiles\\portable one", "choice file: path with space");
        // Read-modify-write keeps other installs' slots (the SaveChoice contract).
        auto r2 = r;
        r2[L"Unpackaged"] = L"D:\\elsewhere";
        CHECK(P::WriteChoiceFile(file, r2), "choice file: rewrite");
        const auto r3 = P::ReadChoiceFile(file);
        CHECK(r3.size() == 3 && r3.at(L"Unpackaged") == L"D:\\elsewhere" &&
                  r3.at(L"Agentmaster_56k4f06dsfp9r") == L"C:\\Users\\x\\.agentmaster",
              "choice file: update preserves other slots");
        ::DeleteFileW(file.c_str());
        ::DeleteFileW((file + L".tmp").c_str());
    }

    // Resolution precedence: the env override beats everything (and is what the WindowEmperor
    // bootstrap exports, so dll-side resolution always agrees with the exe). Save + RESTORE the
    // ambient override — run-m5-tests.bat / wmain point AGENTMASTER_PROFILE at the scratch
    // profile, and the old clear-to-nullptr would re-route any later first-resolution (or
    // Uncached path) back at the LIVE ~/.agentmaster.
    {
        wchar_t prevBuf[1024];
        const DWORD prevLen = ::GetEnvironmentVariableW(L"AGENTMASTER_PROFILE", prevBuf, 1024);
        const bool hadPrev = prevLen > 0 && prevLen < 1024;
        const std::wstring prev = hadPrev ? std::wstring{ prevBuf, prevLen } : std::wstring{};
        const std::wstring fake = std::wstring{ tmpDir } + L"am-profile-env-" + NewSessionId();
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", fake.c_str());
        CHECK(P::ResolveProfileDirUncached() == fake, "resolve: env override wins");
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", nullptr);
        const auto silent = P::ResolveProfileDirUncached();
        CHECK(!silent.empty(), "resolve: silent resolution non-empty");
        ::RemoveDirectoryW(fake.c_str());
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", hadPrev ? prev.c_str() : nullptr);
    }

    // SamePath: the filesystem-aware-enough comparison the migrate guard uses.
    CHECK(P::detail::SamePath(L"C:\\A\\b\\", L"c:/a/B"), "SamePath: case/slash/trailing-insensitive");
    CHECK(!P::detail::SamePath(L"C:\\A\\b", L"C:\\A\\b2"), "SamePath: distinct dirs differ");

    // MigrateProfileData: copies content, skips locks/shim/bridge.json, never clobbers.
    {
        const std::wstring src = std::wstring{ tmpDir } + L"am-mig-src-" + NewSessionId();
        const std::wstring dst = std::wstring{ tmpDir } + L"am-mig-dst-" + NewSessionId();
        std::filesystem::create_directories(std::filesystem::path{ src } / L"windows");
        std::filesystem::create_directories(std::filesystem::path{ src } / L"locks");
        std::filesystem::create_directories(std::filesystem::path{ src } / L"shim");
        auto put = [](const std::filesystem::path& p, const char* bytes) {
            std::ofstream f{ p, std::ios::binary };
            f << bytes;
        };
        put(std::filesystem::path{ src } / L"sessions.json", "{\"v\":1}");
        put(std::filesystem::path{ src } / L"bridge.json", "{\"pipe\":\"stale\"}");
        put(std::filesystem::path{ src } / L"windows" / L"w1.json", "{\"id\":\"w1\"}");
        put(std::filesystem::path{ src } / L"locks" / L"x", "lock");
        put(std::filesystem::path{ src } / L"shim" / L"claude.cmd", "rem old");
        std::filesystem::create_directories(std::filesystem::path{ dst });
        put(std::filesystem::path{ dst } / L"settings.json", "{\"keep\":true}");
        put(std::filesystem::path{ src } / L"settings.json", "{\"keep\":false}");

        P::MigrateProfileData(src, dst);
        namespace fs = std::filesystem;
        CHECK(fs::exists(fs::path{ dst } / L"sessions.json"), "migrate: copies sessions.json");
        CHECK(fs::exists(fs::path{ dst } / L"windows" / L"w1.json"), "migrate: copies windows/ recursively");
        CHECK(!fs::exists(fs::path{ dst } / L"bridge.json"), "migrate: skips bridge.json (stale pipe)");
        CHECK(!fs::exists(fs::path{ dst } / L"locks"), "migrate: skips locks/");
        CHECK(!fs::exists(fs::path{ dst } / L"shim"), "migrate: skips shim/ (regenerated)");
        {
            std::ifstream f{ fs::path{ dst } / L"settings.json", std::ios::binary };
            std::string body{ std::istreambuf_iterator<char>{ f }, std::istreambuf_iterator<char>{} };
            CHECK(body == "{\"keep\":true}", "migrate: never clobbers existing target files");
        }
        // Same-path call is a no-op (the guard the picker's checkbox relies on).
        P::MigrateProfileData(src, src + L"\\");
        CHECK(fs::exists(fs::path{ src } / L"bridge.json"), "migrate: same-path no-op leaves source intact");

        std::error_code ec;
        fs::remove_all(src, ec);
        fs::remove_all(dst, ec);
    }
}

static bool WriteLineToPipe(const std::wstring& pipeName, const std::string& utf8Line)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        const HANDLE h = ::CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, utf8Line.data(), static_cast<DWORD>(utf8Line.size()), &written, nullptr);
            ::CloseHandle(h);
            return ok && written == utf8Line.size();
        }
        if (::GetLastError() == ERROR_PIPE_BUSY)
        {
            ::WaitNamedPipeW(pipeName.c_str(), 1000);
            continue;
        }
        ::Sleep(20);
    }
    return false;
}

void TestBridgeRoundTrip()
{
    std::wprintf(L"HooksBridge pipe round-trip (end-to-end):\n");
    SessionRegistry reg;
    reg.Upsert(MakeSession(L"pipe-1"));

    const auto pipeName = HookPipeName(::GetCurrentProcessId());
    HooksBridge bridge(pipeName, [&](const HookMessage& m) { reg.OnHookEvent(m); }, 2);
    CHECK(bridge.Start(), "bridge started");

    // Give the listener threads a moment to create their pipe instances.
    ::Sleep(100);

    const bool wrote = WriteLineToPipe(pipeName, std::string{ "Stop\tpipe-1\tK:/api\t1\t0\t\n" });
    CHECK(wrote, "client wrote wire line to pipe");

    // Poll for the state change (the dispatch happens on a bridge thread).
    bool became = false;
    for (int i = 0; i < 200; ++i)
    {
        const auto s = reg.Get(L"pipe-1");
        if (s && s->state == SessionState::WaitingForInput)
        {
            became = true;
            break;
        }
        ::Sleep(10);
    }
    CHECK(became, "registry reached WaitingForInput via pipe");
    CHECK(reg.Get(L"pipe-1") && reg.Get(L"pipe-1")->lastMessageWasQuestion, "question flag carried over the pipe");
    CHECK(reg.Get(L"pipe-1") && reg.Get(L"pipe-1")->workingDir == L"K:/x", "existing workingDir preserved");

    bridge.Stop();
    CHECK(!bridge.Running(), "bridge stopped");
}

void TestScheduler()
{
    std::wprintf(L"Autorunner DecideAdvance (Correctness Rule #1 + backstops):\n");
    auto mk = [](AutorunnerMode m, SessionState st) {
        SessionInfo s;
        s.id = L"a";
        s.state = st;
        s.autorunner.mode = m;
        QueuedPrompt p;
        p.id = L"p1";
        p.text = L"do it";
        s.queue.push_back(p);
        return s;
    };
    const int64_t now = 100000;

    {
        auto s = mk(AutorunnerMode::Off, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "off -> none");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::Running);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "not turn-complete -> none");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && p.promptIndex == 0, "full+waiting -> send #0");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, 0, true).action == AdvanceAction::None, "global pause -> none");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        s.autorunner.maxAutoSends = 2;
        s.autorunner.autoSendsThisRun = 2;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "maxAutoSends -> none");
    }
    {
        // A pending question / "needs you" state is treated like a mid-turn Running state: the
        // prompt stays Pending and waits for the next (non-question) turn-complete — it is NOT
        // parked in Held, and autorunner is NOT paused (the "reacted to needs-approval" bug).
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        s.lastMessageWasQuestion = true;
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::None, "question -> none (stay queued, wait like running)");
        CHECK(s.queue[0].status == PromptStatus::Pending, "question: prompt is left Pending (never moved to Held)");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        s.lastMessageWasQuestion = true;
        s.queue[0].guardPattern = std::wstring{ kAnswersQuestionOk };
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::Send, "question + override -> send");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        s.queue[0].gate = PromptGate::Manual;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "manual gate -> none");
    }
    {
        auto s = mk(AutorunnerMode::SemiAuto, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::AwaitConfirm, "semi-auto -> await confirm");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        s.queue[0].status = PromptStatus::Sent;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::PlanDone, "no pending -> plan done");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, now - 500, false).action == AdvanceAction::None, "human typing -> none");
    }
    {
        auto s = mk(AutorunnerMode::Full, SessionState::WaitingForInput);
        CHECK(DecideAdvance(s, now, now - 5000, false).action == AdvanceAction::Send, "human idle -> send");
    }

    // --- Idle bootstrap: a just-resumed / freshly-launched session must START its plan, not
    //     wait for a Stop it will never emit (the "ddd is Idle + auto but won't fire" bug). ---
    {
        auto s = mk(AutorunnerMode::Full, SessionState::Idle);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && p.promptIndex == 0, "full + idle -> send (bootstrap)");
    }
    {
        auto s = mk(AutorunnerMode::SemiAuto, SessionState::Idle);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::AwaitConfirm, "semi-auto + idle -> await confirm");
    }
    {
        auto s = mk(AutorunnerMode::Off, SessionState::Idle);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "off + idle -> none");
    }
    {
        // Not-ready states stay rejected (mid-turn / awaiting you).
        auto s = mk(AutorunnerMode::Full, SessionState::NeedsApproval);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "needs-approval -> none");
        s.state = SessionState::Done;
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "done -> none");
    }

    // --- Pickup guard: one prompt per turn even though advances can now be change-driven. ---
    auto withLead = [&](PromptStatus st, bool echoed, int64_t sentAt, SessionState state) {
        auto s = mk(AutorunnerMode::Full, state);
        QueuedPrompt lead; // a prior Flight prompt at the FRONT; p1 (Pending) follows
        lead.id = L"p0";
        lead.text = L"already sent";
        lead.status = st;
        lead.origin = PromptOrigin::Autorun;
        lead.echoed = echoed;
        lead.sentAtUnixMs = sentAt;
        s.queue.insert(s.queue.begin(), lead);
        return s;
    };
    {
        // Just injected (Sent, recent, not echoed) -> hold the next prompt until Claude picks it up.
        auto s = withLead(PromptStatus::Sent, false, now - 500, SessionState::Idle);
        CHECK(DecideAdvance(s, now, 0, false).action == AdvanceAction::None, "pickup guard: awaiting injection -> none");
    }
    {
        // Echoed (Claude moved to Running and back) -> the next Pending proceeds.
        auto s = withLead(PromptStatus::Sent, true, now - 500, SessionState::WaitingForInput);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && s.queue[p.promptIndex].id == L"p1", "echoed lead -> next pending sends");
    }
    {
        // Stale un-echoed (echo lost long ago) -> window expired, don't stall the plan forever.
        auto s = withLead(PromptStatus::Sent, false, now - 60000, SessionState::WaitingForInput);
        const auto p = DecideAdvance(s, now, 0, false);
        CHECK(p.action == AdvanceAction::Send && s.queue[p.promptIndex].id == L"p1", "stale un-echoed -> window expired, proceed");
    }
}

// Agentmaster — the Enter-retry decision (DecideEnterRetry): re-press Enter when the TUI ate the
// submit Enter of a just-injected Flight prompt (the prompt is typed but never sent, so the turn
// never starts). Pure + deterministic, like DecideAdvance.
void TestEnterRetry()
{
    std::wprintf(L"Autorunner DecideEnterRetry (the TUI-ate-my-Enter backstop):\n");
    const int64_t T = 1000000;
    // A live, managed session with one Sent (Flight, not echoed) prompt at sentAt.
    auto mk = [](SessionState st, int64_t sentAt, bool echoed, uint32_t retries) {
        SessionInfo s;
        s.id = L"r";
        s.live = true;
        s.external = false;
        s.state = st;
        QueuedPrompt p;
        p.id = L"rp";
        p.text = L"go";
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Autorun;
        p.sentAtUnixMs = sentAt;
        p.echoed = echoed;
        p.enterRetries = retries;
        s.queue.push_back(p);
        return s;
    };

    {
        // Just sent -> watching, not yet due.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        const auto r = DecideEnterRetry(s, T);
        CHECK(r.action == EnterRetryAction::Waiting && r.promptId == L"rp", "fresh send -> waiting");
    }
    {
        // One ms before the FIRST-retry delay -> still waiting. A fresh send (enterRetries==0) is due
        // after the SHORT kEnterRetryFirstMs (the snappy first re-press), not the longer interval.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs - 1).action == EnterRetryAction::Waiting, "just under first delay -> waiting");
    }
    {
        // First-retry delay elapsed, turn never started -> re-press Enter (attempt 0).
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        const auto r = DecideEnterRetry(s, T + kEnterRetryFirstMs);
        CHECK(r.action == EnterRetryAction::Retry && r.promptId == L"rp" && r.attempt == 0, "first delay elapsed -> retry");
    }
    {
        // Escalating: AFTER the first press (enterRetries==1) the next press waits the LONGER
        // kEnterRetryIntervalMs, not the short first delay. (The worker refreshes sentAtUnixMs to the
        // last press, so the comparison is "since the last press".)
        auto s = mk(SessionState::WaitingForInput, T, false, 1);
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::Waiting, "2nd press waits longer than the first");
        const auto r2 = DecideEnterRetry(s, T + kEnterRetryIntervalMs);
        CHECK(r2.action == EnterRetryAction::Retry && r2.attempt == 1, "2nd press due at the longer interval");
    }
    {
        // Same, from a freshly-resumed Idle session (it never emits a Stop).
        auto s = mk(SessionState::Idle, T, false, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry, "idle + due -> retry");
    }
    {
        // The UserPromptSubmit echo arrived (managed/hook path) -> the turn started, stop watching.
        auto s = mk(SessionState::WaitingForInput, T, true, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "echoed -> none");
    }
    {
        // State left the ready set (Running) -> the turn started (covers a hook-less adopted session
        // whose state is driven by the transcript tail), stop watching.
        auto s = mk(SessionState::Running, T, false, 0);
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "running -> none");
        s.state = SessionState::NeedsApproval;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "needs-approval -> none");
    }
    {
        // No-hook fast-turn fallback: echo never set + state back to Waiting, but the transcript
        // advanced past the send -> it was picked up, stop watching.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.convLastActivityUnixMs = T + kEnterRetryActivityMarginMs + 1;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "transcript advanced -> none");
    }
    {
        // Transcript moved only WITHIN the margin (a send fired right after the prior turn's tail) ->
        // NOT counted as started; still retry.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.convLastActivityUnixMs = T + kEnterRetryActivityMarginMs - 1;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry, "transcript within margin -> retry");
    }
    {
        // Exhausted the retry budget -> give up (and the caller stops watching + logs).
        auto s = mk(SessionState::WaitingForInput, T, false, kEnterRetryMax);
        const auto r = DecideEnterRetry(s, T + kEnterRetryIntervalMs);
        CHECK(r.action == EnterRetryAction::GiveUp && r.attempt == kEnterRetryMax, "max retries -> give up");
    }
    {
        // Not controllable (no bound injector) -> nothing to re-press Enter on, never retry. This is
        // the observe-only case: an external census claude we hold no stdin for.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.external = true;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs, /*controllable*/ false).action == EnterRetryAction::None, "uncontrollable -> none");
    }
    {
        // Agentmaster (autorunner-on-adopted): an ADOPTED external (external=true) that IS controllable
        // (an injector was bound on adoption) MUST be driven — provenance != controllability. The pure
        // decider keys on `controllable`, not s.external, so an adopted session retries like a launched one.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.external = true;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs, /*controllable*/ true).action == EnterRetryAction::Retry, "adopted + controllable -> retry");
    }
    {
        // Archived (!live) session -> no live claude, never retry.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.live = false;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "archived -> none");
    }
    {
        // A Typed prompt (the human's own keystroke, captured) is never our send to re-press.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.queue[0].origin = PromptOrigin::Typed;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "typed origin -> none");
    }
    {
        // A Pending (not-yet-sent) prompt isn't awaiting pickup.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.queue[0].status = PromptStatus::Pending;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::None, "pending -> none");
    }
    {
        // The LATEST un-acknowledged send is the one watched (a later send supersedes an earlier).
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        QueuedPrompt p2;
        p2.id = L"rp2";
        p2.text = L"go2";
        p2.status = PromptStatus::Sent;
        p2.origin = PromptOrigin::Autorun;
        p2.sentAtUnixMs = T + 2000; // sent later
        p2.echoed = false;
        s.queue.push_back(p2);
        const auto r = DecideEnterRetry(s, T + 2000 + kEnterRetryIntervalMs);
        CHECK(r.action == EnterRetryAction::Retry && r.promptId == L"rp2", "latest send is the watched one");
    }
}

// Agentmaster (#6) — BuildPromptSubmission wraps a prompt body in a bracketed paste + ONE trailing CR
// so a multi-line body submits as a single message (not line-by-line, the "submit on first CR" bug).
// Pure + deterministic.
void TestBuildPromptSubmission()
{
    std::wprintf(L"BuildPromptSubmission (bracketed-paste submit, #6):\n");
    const std::wstring B = L"\x1b[200~"; // bracketed-paste begin
    const std::wstring E = L"\x1b[201~"; // bracketed-paste end

    CHECK(BuildPromptSubmission(L"test 1") == B + L"test 1" + E + L"\r", "single line wrapped + one CR");
    CHECK(BuildPromptSubmission(L"line1\rline2") == B + L"line1\nline2" + E + L"\r", "CR line breaks -> LF inside the paste");
    CHECK(BuildPromptSubmission(L"a\r\nb") == B + L"a\nb" + E + L"\r", "CRLF -> a single LF");
    CHECK(BuildPromptSubmission(L"a\nb") == B + L"a\nb" + E + L"\r", "a bare LF body is preserved");
    CHECK(BuildPromptSubmission(L"") == B + E + L"\r", "empty body is still well-formed");
    {
        // The whole point of #6: exactly ONE submit CR (the trailing one) regardless of line count.
        const auto out = BuildPromptSubmission(L"x\ry\rz");
        size_t crs = 0;
        for (const wchar_t c : out)
        {
            if (c == L'\r')
            {
                ++crs;
            }
        }
        CHECK(crs == 1, "a multi-line body yields exactly one submit CR");
    }
}

// Agentmaster — Scheduler INTEGRATION (the threaded change-driven advance, wired exactly like
// Engine.cpp). The pure DecideAdvance above never exercised the OnObserved -> RequestAdvance ->
// worker -> Inject chain, which is precisely where the "toggle Autorunner Off and back -> pending
// not sent" bug lived: OnObserved gated the change-driven advance on !s.external (provenance)
// instead of HasInjector (controllability), so an ADOPTED session (external=true but injector-bound)
// was never driven. It ALSO covers the Finding-B dormant gate: a re-homed BACKGROUND tab (live +
// injector-bound but not yet STARTED) must NOT be driven until its ConPTY launches (else the send is
// lost -> Enter-retry -> Failed + plan paused). This test drives the real Scheduler thread and polls
// for the injected result.
void TestSchedulerIntegration()
{
    std::wprintf(L"Autorunner Scheduler integration (toggle Off->Full; controllability != provenance):\n");

    // Wire a registry + scheduler like Engine.cpp (advance handler + OnObserved observer), seed a
    // live Waiting session with one Pending Flight prompt (throttleMs 0 so the worker injects at
    // once), optionally bind an injector, then toggle Autorunner Off->Full via the registry exactly
    // as AgentManagerContent::_OnAutorunnerChanged does. Returns true iff the prompt reached Sent
    // within the poll budget. Each case gets its OWN registry+scheduler so they can't cross-talk.
    auto runToggleCase = [](bool external, bool bindInjector, bool started) -> bool {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });

        const std::wstring id = L"sess";
        std::atomic<int> injected{ 0 };
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput; // turn already complete; no future Stop hook fires
            s.live = true;
            s.external = external;
            s.started = started; // Agentmaster (Finding B): a launched session must be STARTED to be driven; external is carved out (started is meaningless for it)
            s.autorunner.mode = AutorunnerMode::Off;
            s.autorunner.throttleMs = 0; // inject immediately (no 500ms throttle) -> a short poll suffices
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the pending prompt";
            p.status = PromptStatus::Pending;
            p.origin = PromptOrigin::Autorun;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        if (bindInjector)
        {
            reg->SetInjector(id, [&injected](const std::wstring&) { injected.fetch_add(1); });
        }

        // Toggle Off->Full (the user re-enabling Autorunner) — the exact _OnAutorunnerChanged write.
        reg->Update(id, [](SessionInfo& s) {
            s.autorunner.mode = AutorunnerMode::Full;
            s.autorunner.autoSendsThisRun = 0;
            s.pendingConfirmPromptId.clear();
        });

        // Poll up to ~2s for the worker to pick up + inject (deterministic outcome, bounded wait).
        bool sent = false;
        for (int i = 0; i < 200 && !sent; ++i)
        {
            const auto snap = reg->Get(id);
            sent = snap && !snap->queue.empty() && snap->queue[0].status == PromptStatus::Sent;
            if (!sent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        reg->RemoveObserver(obsTok);
        sched.Stop();
        // A real send marks Sent AND delivers to the injector (when one is bound).
        return sent && (!bindInjector || injected.load() > 0);
    };

    // Manager-Launched (external=false, injector bound, STARTED): always worked — the regression baseline.
    CHECK(runToggleCase(/*external*/ false, /*injector*/ true, /*started*/ true), "launched+started: toggle Off->Full sends pending");
    // Adopted (external=true, injector bound): THE fix — provenance != controllability, must send. Its
    // control is untracked so `started` stays false; the `|| s.external` carve-out keeps it drivable.
    CHECK(runToggleCase(/*external*/ true, /*injector*/ true, /*started*/ false), "adopted: toggle Off->Full sends pending (external carve-out, started=false)");
    // Observe-only external (external=true, NO injector): must NOT send (nothing to drive, no churn).
    CHECK(!runToggleCase(/*external*/ true, /*injector*/ false, /*started*/ false), "observe-only: toggle does not send (no injector)");
    // Agentmaster (Finding B): a DORMANT re-homed tab — launched (external=false) + injector bound but
    // its ConPTY has NOT started yet (WT lazy-starts a background tab) — must NOT be driven: injecting
    // into a not-yet-launched claude loses the submit -> the Enter-retry watchdog gives up -> Failed +
    // plan paused. The send must DEFER until SetStarted(true) fires on the tab's first layout/activation.
    CHECK(!runToggleCase(/*external*/ false, /*injector*/ true, /*started*/ false), "dormant launched: a not-yet-started re-homed tab does NOT auto-send (Finding B)");

    // --- Question-guard treats a pending question / "needs you" status like a Running mid-turn: the
    //     queued prompt STAYS Pending (never parked in Held, autorunner never paused) and fires only
    //     when the question clears (a later non-question turn-complete). The "reacted to
    //     needs-approval" report (session 2de51dd0): two Flight prompts were Held + stranded. ---
    {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });

        const std::wstring id = L"q-sess";
        std::atomic<int> injected{ 0 };
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput; // turn complete, but...
            s.live = true;
            s.started = true; // a live, driven session IS started (Finding B gate); the question-guard is what holds it here
            s.lastMessageWasQuestion = true; // ...the agent ended it asking the user something
            s.autorunner.mode = AutorunnerMode::Full;
            s.autorunner.throttleMs = 0;
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the queued prompt";
            p.status = PromptStatus::Pending;
            p.origin = PromptOrigin::Autorun;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        reg->SetInjector(id, [&injected](const std::wstring&) { injected.fetch_add(1); });

        // An observed change while the question stands must NOT send — the prompt stays Pending.
        reg->Update(id, [](SessionInfo&) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            const auto snap = reg->Get(id);
            CHECK(snap && snap->queue[0].status == PromptStatus::Pending, "question pending: prompt stays Pending (not Held), not sent");
            CHECK(injected.load() == 0, "question pending: nothing injected while the question stands");
        }
        // The question clears (a non-question turn-complete) -> the queued prompt now fires.
        reg->Update(id, [](SessionInfo& s) { s.lastMessageWasQuestion = false; });
        bool sent = false;
        for (int i = 0; i < 200 && !sent; ++i)
        {
            const auto snap = reg->Get(id);
            sent = snap && snap->queue[0].status == PromptStatus::Sent;
            if (!sent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(sent && injected.load() > 0, "question cleared: the queued prompt fires (was waiting like running)");
        reg->RemoveObserver(obsTok);
        sched.Stop();
    }

    // --- A Held prompt persisted by an OLDER build must rehabilitate, not strand: an observed change
    //     wakes the advance (OnObserved counts Held as work), _process un-holds it to Pending (the
    //     question is gone), and it sends. Without the OnObserved Held-as-work fix it sat dead. ---
    {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });

        const std::wstring id = L"held-sess";
        std::atomic<int> injected{ 0 };
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput;
            s.live = true;
            s.started = true; // a live, driven session IS started (Finding B gate)
            s.lastMessageWasQuestion = false; // the question is gone — the Held prompt should recover
            s.autorunner.mode = AutorunnerMode::Full;
            s.autorunner.throttleMs = 0;
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the held prompt";
            p.status = PromptStatus::Held; // a legacy hold loaded from disk
            p.origin = PromptOrigin::Autorun;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        reg->SetInjector(id, [&injected](const std::wstring&) { injected.fetch_add(1); });

        reg->Update(id, [](SessionInfo&) {}); // an enrich/observe tick wakes the advance
        bool sent = false;
        for (int i = 0; i < 200 && !sent; ++i)
        {
            const auto snap = reg->Get(id);
            sent = snap && snap->queue[0].status == PromptStatus::Sent;
            if (!sent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(sent && injected.load() > 0, "legacy Held prompt rehabilitated to Pending and sent (not stranded)");
        reg->RemoveObserver(obsTok);
        sched.Stop();
    }
}

// ---------------------------------------------------------------------------------------------
// Updater version logic (Updater.h — header-only, pure Win32). ParseVersion's tolerances, the
// CompareVersion contract (the 4th part is BUILD METADATA — it never participates in "is a newer
// release available?"), the unpackaged process facts that gate the startup auto-check, and the
// settings.json skip/postpone RMW the EXE performs WITHOUT linking the engine — including the
// preserve-other-keys contract the cog's Save relies on (and the SetMember replace-not-append
// rule, since Json.h's Value::Set always APPENDS).
// ---------------------------------------------------------------------------------------------
void TestUpdaterVersionLogic()
{
    namespace U = ::Agentmaster::Updater;
    std::wprintf(L"Updater version logic (parse / compare / skip-postpone RMW):\n");

    // ParseVersion: tags, 4-part manifest versions, prerelease suffixes, junk, gaps.
    {
        const auto v = U::ParseVersion(L"v0.6.6");
        CHECK(v.major == 0 && v.minor == 6 && v.patch == 6 && v.build == 0, "parse: v-prefixed tag");
        const auto w = U::ParseVersion(L"1.2.3.4");
        CHECK(w.major == 1 && w.minor == 2 && w.patch == 3 && w.build == 4, "parse: 4-part manifest version");
        const auto b = U::ParseVersion(L"0.7.0-beta.2");
        CHECK(b.major == 0 && b.minor == 7 && b.patch == 0 && b.build == 0, "parse: prerelease suffix stops cleanly");
        const auto cap = U::ParseVersion(L"V2.10.3");
        CHECK(cap.major == 2 && cap.minor == 10 && cap.patch == 3, "parse: capital V + multi-digit minor");
        const auto e = U::ParseVersion(L"");
        CHECK(e.major == 0 && e.minor == 0 && e.patch == 0 && e.build == 0, "parse: empty -> zeros");
        const auto g = U::ParseVersion(L"garbage");
        CHECK(g.major == 0 && g.minor == 0 && g.patch == 0, "parse: no digits -> zeros");
        const auto m = U::ParseVersion(L"1..2");
        CHECK(m.major == 1 && m.minor == 0 && m.patch == 2, "parse: a missing middle part defaults to 0");
    }

    // CompareVersion: strict major.minor.patch ordering; the 4th part NEVER participates.
    {
        auto mk = [](int a, int b2, int c, int d = 0) {
            U::Version ver;
            ver.major = a;
            ver.minor = b2;
            ver.patch = c;
            ver.build = d;
            return ver;
        };
        CHECK(U::CompareVersion(mk(0, 6, 6), mk(0, 6, 7)) < 0, "compare: patch orders");
        CHECK(U::CompareVersion(mk(0, 7, 0), mk(0, 6, 9)) > 0, "compare: minor beats patch");
        CHECK(U::CompareVersion(mk(1, 0, 0), mk(0, 99, 99)) > 0, "compare: major beats all");
        CHECK(U::CompareVersion(mk(1, 2, 3, 9), mk(1, 2, 3, 0)) == 0, "compare: the 4th part is build metadata (ignored)");
        CHECK(U::VersionToString(mk(0, 6, 6, 4)) == L"0.6.6", "to-string: three parts only");
    }

    // Unpackaged process facts (the harness IS unpackaged): version zeros + not packaged — the
    // pair that gates the startup auto-check off dev/unpackaged runs.
    {
        const auto cur = U::CurrentPackageVersion();
        CHECK(cur.major == 0 && cur.minor == 0 && cur.patch == 0 && cur.build == 0, "unpackaged: package version is zeros");
        CHECK(!U::IsPackaged(), "unpackaged: IsPackaged false");
    }

    // Skip/Postpone/prerelease persistence on settings.json (explicit stateDir -> a temp profile).
    // settings.json is the engine's ENVELOPE ({version:1, settings:{...}} — Persistence::
    // SerializeAppSettings), so the update keys must live NESTED under "settings", where AppSettings
    // round-trips them. Reading/writing the ROOT was the original updater bug: the startup/hourly
    // checks never saw the cog-saved prerelease opt-in (they checked stable-only forever), and a
    // top-level Skip/Postpone was silently WIPED by the next engine save (which rebuilds the whole
    // envelope from the struct).
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am-updater-test-" + NewSessionId();
        const std::wstring sj = dir + L"\\settings.json";
        const auto countKey = [](const std::wstring& text, const wchar_t* key) {
            size_t n = 0;
            for (size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + 1))
            {
                ++n;
            }
            return n;
        };

        const auto missing = U::ReadPrefs(dir);
        CHECK(!missing.allowPrerelease && missing.skippedVersion.empty() && missing.postponedUntilUnixMs == 0,
              "prefs: missing settings.json -> defaults");

        // An RMW on a MISSING file mints the engine envelope (version + nested settings), never a
        // flat doc a later engine save would half-ignore.
        U::WriteSkip(dir, L"v0.9.9");
        {
            const auto doc = json::Parse(U::detail::ReadFileWide(sj));
            CHECK(doc && doc->type == json::Value::Type::Obj, "prefs: fresh RMW parses");
            const auto* s = doc ? doc->Find(L"settings") : nullptr;
            CHECK(s && s->type == json::Value::Type::Obj && s->StrAt(L"updateSkippedVersion") == L"v0.9.9",
                  "prefs: fresh RMW mints the ENGINE ENVELOPE (key nested under settings)");
            CHECK(doc->NumAt(L"version", 0) == 1, "prefs: fresh RMW stamps version 1");
            CHECK(!doc->Find(L"updateSkippedVersion"), "prefs: no top-level stray minted");
        }

        // ENGINE round-trip — the bug-1 + bug-2 regression pair. Seed an engine-shaped file exactly
        // as the cog's Save writes it (model set, prerelease opted in), then verify both directions.
        AppSettings cogSaved;
        cogSaved.model = L"opus";
        cogSaved.allowUpdatePrerelease = true;
        CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(cogSaved)), "atomic: engine-shaped seed lands");
        CHECK(U::ReadPrefs(dir).allowPrerelease, "prefs: cog-saved (nested) prerelease opt-in is READ (bug-1 regression)");

        U::WriteSkip(dir, L"v1.0.0");
        U::WritePostpone(dir, 1234567890123LL);
        const auto p = U::ReadPrefs(dir);
        CHECK(p.skippedVersion == L"v1.0.0", "prefs: WriteSkip round-trips");
        CHECK(p.postponedUntilUnixMs == 1234567890123LL, "prefs: WritePostpone round-trips (int64 survives the double)");
        CHECK(p.allowPrerelease, "prefs: the RMW preserves the cog's nested prerelease");
        {
            const AppSettings loaded = DeserializeAppSettings(U::detail::ReadFileWide(sj));
            CHECK(loaded.model == L"opus", "prefs: the cog's model survives the updater RMW");
            CHECK(loaded.updateSkippedVersion == L"v1.0.0" && loaded.updatePostponedUntilUnixMs == 1234567890123LL,
                  "prefs: the ENGINE reads the updater's skip/postpone (same nested fields)");
            // The engine-save cycle (bug-2 regression): rebuild the envelope from the struct — the
            // updater's choices must SURVIVE it (top-level keys used to be wiped exactly here).
            CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(loaded)), "atomic: engine save-cycle lands");
            const auto after = U::ReadPrefs(dir);
            CHECK(after.skippedVersion == L"v1.0.0" && after.postponedUntilUnixMs == 1234567890123LL && after.allowPrerelease,
                  "prefs: skip/postpone/prerelease SURVIVE an engine save (bug-2 regression)");
        }

        // SetMember must REPLACE, not append: a second skip leaves exactly ONE (nested) key.
        U::WriteSkip(dir, L"v1.0.1");
        {
            CHECK(U::ReadPrefs(dir).skippedVersion == L"v1.0.1", "prefs: a second skip replaces the tag");
            CHECK(countKey(U::detail::ReadFileWide(sj), L"updateSkippedVersion") == 1,
                  "prefs: SetMember replaces in place (one nested key, no stray)");
        }

        // Legacy strays (a file the PRE-FIX updater touched): top-level skip/postpone beside the
        // envelope are honored on read (fallback), the NESTED value wins when both are set, and the
        // next write MIGRATES the stray into the envelope (healing must never forget a choice).
        {
            const std::wstring legacy =
                L"{\"version\": 1, \"updateSkippedVersion\": \"v0.5.0\", \"updatePostponedUntilUnixMs\": 777, "
                L"\"settings\": {\"model\": \"opus\", \"updateSkippedVersion\": \"\", \"updatePostponedUntilUnixMs\": 0}}";
            CHECK(U::detail::WriteFileUtf8(sj, legacy), "legacy: seed written");
            const auto lp = U::ReadPrefs(dir);
            CHECK(lp.skippedVersion == L"v0.5.0" && lp.postponedUntilUnixMs == 777,
                  "legacy: top-level strays honored while nested unset");
            U::WritePostpone(dir, 999);
            const std::wstring healed = U::detail::ReadFileWide(sj);
            CHECK(countKey(healed, L"updateSkippedVersion") == 1, "legacy: heal leaves ONE nested key");
            const auto hp = U::ReadPrefs(dir);
            CHECK(hp.postponedUntilUnixMs == 999 && hp.skippedVersion == L"v0.5.0",
                  "legacy: heal MIGRATES the stray skip into the envelope (not dropped)");
            const AppSettings engineView = DeserializeAppSettings(healed);
            CHECK(engineView.updateSkippedVersion == L"v0.5.0" && engineView.model == L"opus",
                  "legacy: after the heal the ENGINE sees the migrated skip + keeps its own keys");

            const std::wstring both =
                L"{\"version\":1,\"updateSkippedVersion\":\"v0.1.0\",\"settings\":{\"updateSkippedVersion\":\"v0.2.0\"}}";
            CHECK(U::detail::WriteFileUtf8(sj, both), "legacy: both-set seed written");
            CHECK(U::ReadPrefs(dir).skippedVersion == L"v0.2.0", "legacy: the NESTED value wins over a stray");
        }

        // A non-empty file that doesn't parse is REFUSED — never clobbered by an update-keys-only
        // skeleton (the user's whole settings live in this file).
        {
            CHECK(U::detail::WriteFileUtf8(sj, L"{not json!!"), "garbage: seeded");
            const std::wstring tag = L"v9.9.9";
            CHECK(!U::WriteUpdateState(dir, &tag, nullptr), "garbage: RMW refuses an unparseable file");
            CHECK(U::detail::ReadFileWide(sj) == L"{not json!!", "garbage: file bytes untouched");
        }

        // The atomic writer: full replace of an existing (longer) file + no temp leftovers.
        {
            CHECK(U::detail::WriteFileUtf8(sj, L"short"), "atomic: replace long with short");
            CHECK(U::detail::ReadFileWide(sj) == L"short", "atomic: no tail of the old content");
            bool leftover = false;
            for (const auto& e : std::filesystem::directory_iterator{ std::filesystem::path{ dir } })
            {
                if (e.path().filename().wstring().find(L".tmp.") != std::wstring::npos)
                {
                    leftover = true;
                }
            }
            CHECK(!leftover, "atomic: no .tmp.* leftover");
        }

        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }

    // "Not now" declined-this-run latch (Updater.h): PROCESS-scoped via an env var (one env block
    // per process, unlike a per-module inline/static — the cog's DLL prompt must silence the EXE's
    // hourly re-prompt), keyed by the exact declined tag so a NEWER release still prompts, cleared
    // by process exit ("ask again next launch"). Save/restore the ambient var (harness hygiene).
    {
        wchar_t prevBuf[256];
        const DWORD prevLen = ::GetEnvironmentVariableW(U::kDeclinedEnvVar, prevBuf, 256);
        const bool hadPrev = prevLen > 0 && prevLen < 256;
        const std::wstring prev = hadPrev ? std::wstring{ prevBuf, prevLen } : std::wstring{};

        U::MarkDeclinedThisRun(L""); // ensure a clean slate regardless of the ambient env
        CHECK(!U::WasDeclinedThisRun(L"v0.6.8"), "declined: unset -> false");
        U::MarkDeclinedThisRun(L"v0.6.8");
        CHECK(U::WasDeclinedThisRun(L"v0.6.8"), "declined: same tag latched (no hourly re-nag)");
        CHECK(!U::WasDeclinedThisRun(L"v0.6.9"), "declined: a NEWER tag still prompts");
        CHECK(!U::WasDeclinedThisRun(L""), "declined: empty tag never matches");
        U::MarkDeclinedThisRun(L"v0.7.0");
        CHECK(!U::WasDeclinedThisRun(L"v0.6.8") && U::WasDeclinedThisRun(L"v0.7.0"),
              "declined: re-decline replaces the tag (single-slot latch)");
        U::MarkDeclinedThisRun(L"");
        CHECK(!U::WasDeclinedThisRun(L"v0.7.0"), "declined: cleared");

        ::SetEnvironmentVariableW(U::kDeclinedEnvVar, hadPrev ? prev.c_str() : nullptr);
    }
}

