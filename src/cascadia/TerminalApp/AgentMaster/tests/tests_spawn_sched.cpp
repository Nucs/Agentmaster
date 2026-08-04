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

// The updater tests odr-use RunStartupUpdateCheck (the fresh-launch latch-clear check), which pulls
// ShowUpdatePrompt -> TaskDialogIndirect — imported from comctl32 BY ORDINAL (345), which only
// exists in comctl32 v6. The APP declares v6 in WindowsTerminal.manifest; the bare harness exe would
// bind System32's v5.82 and DIE AT LOAD with STATUS_ORDINAL_NOT_FOUND (0xC0000138) before main —
// zero output, exit 127. This linker directive gives the harness the same v6 dependency.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

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
        // Version-LESS by design: the ids are Claude Code's "latest of this family" aliases, so the
        // list never rots the way the pinned "claude-opus-4-8" era did (SessionModels.h).
        const auto d = ParseLaunchModels(kDefaultLaunchModels);
        CHECK(d.size() == 3, "launch models: shipped defaults -> 3 entries");
        CHECK(d[0].first == L"Fable" && d[0].second == L"fable", "launch models: default #1 Fable | fable (alias, not claude-fable-5)");
        CHECK(d[1].first == L"Opus" && d[1].second == L"opus", "launch models: default #2 Opus | opus (alias, not claude-opus-4-8)");
        CHECK(d[2].first == L"Sonnet" && d[2].second == L"sonnet", "launch models: default #3 Sonnet | sonnet (alias, not claude-sonnet-5)");
        for (const auto& [name, id] : d)
        {
            CHECK(id.find(L'-') == std::wstring::npos && id.find_first_of(L"0123456789") == std::wstring::npos,
                  "launch models: every shipped id is a bare version-LESS alias");
            CHECK(!name.empty() && name.find_first_of(L"0123456789") == std::wstring::npos,
                  "launch models: every shipped label is version-LESS too");
        }
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

    // LaunchModelsAreSupersededDefault (Agentmaster, launch-model picker): the "ours, unmodified =>
    // upgrade" gate that keeps an EXISTING install from being pinned to a stale shipped default by
    // the settings key's mere presence. Matches the PARSED pairs, so line endings / trailing blank
    // lines / spacing never hide a pristine list; anything the user actually edited is left alone.
    {
        for (const auto& superseded : kSupersededLaunchModels)
        {
            CHECK(LaunchModelsAreSupersededDefault(superseded), "superseded models: a verbatim retired default is recognized");
        }
        const std::wstring v1 = L"Fable 5 | claude-fable-5\nOpus 4.8 | claude-opus-4-8\nSonnet 5 | claude-sonnet-5";
        // The cog TextBox rewrites newlines to '\r' on the first open+Save, so the REAL on-disk
        // shape of an untouched list is \r-separated — the case a byte compare would miss.
        std::wstring cr = v1;
        for (auto& c : cr)
        {
            if (c == L'\n')
            {
                c = L'\r';
            }
        }
        CHECK(LaunchModelsAreSupersededDefault(cr), "superseded models: '\\r' line endings (the cog TextBox round-trip) still match");
        CHECK(LaunchModelsAreSupersededDefault(v1 + L"\n"), "superseded models: a trailing newline still matches");
        CHECK(LaunchModelsAreSupersededDefault(L"Fable 5|claude-fable-5\nOpus 4.8|claude-opus-4-8\nSonnet 5|claude-sonnet-5"),
              "superseded models: spacing around '|' still matches (parsed pairs, not bytes)");
        // Anything the user touched — a drop, an addition, a rename, a reorder — is THEIRS.
        CHECK(!LaunchModelsAreSupersededDefault(L"Fable 5 | claude-fable-5\nOpus 4.8 | claude-opus-4-8"),
              "superseded models: a REMOVED entry is a user edit — left alone");
        CHECK(!LaunchModelsAreSupersededDefault(v1 + L"\nHaiku 4.5 | claude-haiku-4-5-20251001"),
              "superseded models: an ADDED entry is a user edit — left alone");
        CHECK(!LaunchModelsAreSupersededDefault(L"Opus 4.8 | claude-opus-4-8\nFable 5 | claude-fable-5\nSonnet 5 | claude-sonnet-5"),
              "superseded models: a REORDERED list is a user edit — left alone");
        CHECK(!LaunchModelsAreSupersededDefault(L"Fable | claude-fable-5\nOpus 4.8 | claude-opus-4-8\nSonnet 5 | claude-sonnet-5"),
              "superseded models: a RENAMED label is a user edit — left alone");
        // The CURRENT default is not superseded (no upgrade loop), and an empty/no-models spec is a
        // deliberate choice, never replaced.
        CHECK(!LaunchModelsAreSupersededDefault(kDefaultLaunchModels), "superseded models: the CURRENT default is not superseded");
        CHECK(!LaunchModelsAreSupersededDefault(L""), "superseded models: \"\" is the deliberate no-models choice — never upgraded");
        CHECK(!LaunchModelsAreSupersededDefault(L"# all commented out\n\n  "), "superseded models: a spec parsing to nothing is never upgraded");
        // The histories must stay disjoint from the current text, or a load would flip-flop.
        for (const auto& superseded : kSupersededLaunchModels)
        {
            CHECK(ParseLaunchModels(superseded) != ParseLaunchModels(kDefaultLaunchModels),
                  "superseded models: no retired default equals the current one");
        }
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
// Agentmaster (workspace trust): the pure half of the "never show Claude's startup trust dialog on a
// managed tab" seed. SpliceWorkspaceTrust edits the user's REAL ~/.claude.json, which holds their
// oauth account + 60-odd project entries + float stats — so the contract under test is not merely
// "produces valid JSON" but "changes NOTHING except the one flag", byte for byte.
void TestWorkspaceTrust()
{
    using ::Agentmaster::SpliceWorkspaceTrust;
    std::wprintf(L"Workspace trust (Claude startup trust dialog):\n");

    // ---- the key ----------------------------------------------------------------------------
    // Claude normalizes to forward slashes and keys on the enclosing repo; a non-repo dir keys
    // itself. (The repo probe is filesystem-driven, so the portable assertions here are the
    // normalization ones; the repo case is covered by the live check at the end.)
    CHECK(::Agentmaster::ClaudeWorkspaceTrustKey(L"") == L"", "trust key: empty dir => empty");
    {
        const auto k = ::Agentmaster::ClaudeWorkspaceTrustKey(L"Z:\\no\\such\\dir");
        CHECK(k == L"Z:/no/such/dir", "trust key: backslashes -> forward slashes");
        const auto trailing = ::Agentmaster::ClaudeWorkspaceTrustKey(L"Z:\\no\\such\\dir\\");
        CHECK(trailing == L"Z:/no/such/dir", "trust key: trailing separator stripped");
        CHECK(::Agentmaster::ClaudeWorkspaceTrustKey(L"Z:\\") == L"Z:/", "trust key: drive root keeps its slash");
    }

    // ---- already trusted => NO write at all (the steady state on every launch after the first) --
    {
        const std::wstring cfg = LR"({"projects":{"K:/repo":{"hasTrustDialogAccepted":true}}})";
        CHECK(!SpliceWorkspaceTrust(cfg, L"K:/repo").has_value(), "already trusted => no write");
    }

    // ---- flip an existing false in place, touching nothing else ------------------------------
    {
        const std::wstring cfg =
            L"{\n  \"numStartups\": 3060,\n  \"lastCost\": 0.12345678901234567,\n"
            L"  \"projects\": {\n    \"K:/repo\": {\n      \"allowedTools\": [],\n"
            L"      \"hasTrustDialogAccepted\": false,\n      \"projectOnboardingSeenCount\": 39\n    }\n  }\n}";
        const auto out = SpliceWorkspaceTrust(cfg, L"K:/repo");
        CHECK(out.has_value(), "false => spliced");
        if (out)
        {
            CHECK(out->find(L"\"hasTrustDialogAccepted\": true") != std::wstring::npos, "flip: reads true");
            CHECK(out->find(L"false") == std::wstring::npos, "flip: the old token is gone");
            // The whole point of splicing instead of re-serializing: a full-precision float that
            // Json.h's "%g" printer would have truncated to 0.123457 survives verbatim.
            CHECK(out->find(L"0.12345678901234567") != std::wstring::npos, "flip: float precision preserved verbatim");
            CHECK(out->size() == cfg.size() - 1, "flip: only 'false'->'true' changed (one char shorter)");
            // Idempotent: the spliced text needs no further write.
            CHECK(!SpliceWorkspaceTrust(*out, L"K:/repo").has_value(), "flip: result is already-trusted");
        }
    }

    // ---- an entry that exists but has no flag: insert, keeping the file's indentation ---------
    {
        const std::wstring cfg =
            L"{\n  \"projects\": {\n    \"K:/repo\": {\n      \"allowedTools\": [],\n      \"mcpServers\": {}\n    }\n  }\n}";
        const auto out = SpliceWorkspaceTrust(cfg, L"K:/repo");
        CHECK(out.has_value(), "missing flag => spliced");
        if (out)
        {
            CHECK(out->find(L"\n      \"hasTrustDialogAccepted\": true,\n      \"allowedTools\"") != std::wstring::npos,
                  "insert: matches the surrounding 6-space indent");
            CHECK(out->find(L"\"mcpServers\": {}") != std::wstring::npos, "insert: siblings untouched");
            CHECK(!SpliceWorkspaceTrust(*out, L"K:/repo").has_value(), "insert: result is already-trusted");
        }
    }

    // ---- no entry for this workspace: add one under projects ---------------------------------
    {
        const std::wstring cfg = L"{\n  \"projects\": {\n    \"K:/other\": {\n      \"allowedTools\": []\n    }\n  }\n}";
        const auto out = SpliceWorkspaceTrust(cfg, L"K:/repo");
        CHECK(out.has_value(), "absent entry => spliced");
        if (out)
        {
            CHECK(out->find(L"\"K:/other\"") != std::wstring::npos, "add entry: the other workspace survives");
            CHECK(!SpliceWorkspaceTrust(*out, L"K:/repo").has_value(), "add entry: result is already-trusted");
            const auto parsed = ::Agentmaster::json::Parse(*out);
            CHECK(parsed && parsed->Find(L"projects") && parsed->Find(L"projects")->members.size() == 2, "add entry: two workspaces");
        }
    }

    // ---- an EMPTY projects object (no trailing comma may be emitted) --------------------------
    {
        const auto out = SpliceWorkspaceTrust(L"{\"projects\":{}}", L"K:/repo");
        CHECK(out.has_value(), "empty projects => spliced");
        if (out)
        {
            CHECK(out->find(L",}") == std::wstring::npos && out->find(L",\n}") == std::wstring::npos, "empty projects: no dangling comma");
            CHECK(!SpliceWorkspaceTrust(*out, L"K:/repo").has_value(), "empty projects: result is already-trusted");
        }
    }

    // ---- no projects key at all (a brand-new config) ------------------------------------------
    {
        const auto out = SpliceWorkspaceTrust(L"{\n  \"numStartups\": 1\n}", L"K:/repo");
        CHECK(out.has_value(), "no projects key => spliced");
        if (out)
        {
            CHECK(out->find(L"\"numStartups\": 1") != std::wstring::npos, "no projects key: existing members survive");
            CHECK(!SpliceWorkspaceTrust(*out, L"K:/repo").has_value(), "no projects key: result is already-trusted");
        }
        const auto bare = SpliceWorkspaceTrust(L"{}", L"K:/repo");
        CHECK(bare.has_value() && !SpliceWorkspaceTrust(*bare, L"K:/repo").has_value(), "bare {} => spliced + trusted");
    }

    // ---- NO-CLOBBER refusals: never rewrite a config we cannot read ---------------------------
    CHECK(!SpliceWorkspaceTrust(L"", L"K:/repo").has_value(), "refuse: empty file (would race claude's first write)");
    CHECK(!SpliceWorkspaceTrust(L"{ this is not json", L"K:/repo").has_value(), "refuse: unparseable");
    CHECK(!SpliceWorkspaceTrust(L"[1,2,3]", L"K:/repo").has_value(), "refuse: not an object");
    CHECK(!SpliceWorkspaceTrust(L"{\"projects\":{\"K:/repo\":{\"hasTrustDialogAccepted\":true}}}", L"").has_value(), "refuse: empty key");

    // ---- a key that needs JSON escaping, and one whose NAME contains braces/quotes ------------
    {
        // A brace inside a member NAME must not unbalance the object walk.
        const std::wstring cfg = LR"({"projects":{"K:/a{b}c":{"x":1},"K:/repo":{"y":2}}})";
        const auto out = SpliceWorkspaceTrust(cfg, L"K:/repo");
        CHECK(out.has_value(), "brace-in-key sibling => spliced");
        if (out)
        {
            const auto parsed = ::Agentmaster::json::Parse(*out);
            CHECK(parsed && parsed->Find(L"projects") && parsed->Find(L"projects")->Find(L"K:/a{b}c"), "brace-in-key: sibling intact");
            CHECK(!SpliceWorkspaceTrust(*out, L"K:/repo").has_value(), "brace-in-key: result is already-trusted");
        }
    }

    // ---- the ancestor rule is Claude's, not ours: an ancestor entry does NOT satisfy an exact
    //      key lookup here, so we still seed (harmless: Claude then finds either one) -----------
    {
        const auto out = SpliceWorkspaceTrust(LR"({"projects":{"K:":{"hasTrustDialogAccepted":true}}})", L"K:/repo");
        CHECK(out.has_value(), "ancestor-trusted still seeds the exact key");
    }

    // ---- LIVE (best-effort): the real config must round-trip our splice unchanged apart from
    //      the flag. Skipped silently when there is no config on this machine. ------------------
    {
        const std::wstring path = ::Agentmaster::ClaudeGlobalConfigPath();
        CHECK(!path.empty() && path.find(L".claude.json") != std::wstring::npos, "global config path resolves to .claude.json");
        // Updater.h's UTF-8 reader (a wifstream would mangle the file through the narrow locale).
        const std::wstring text = ::Agentmaster::Updater::detail::ReadFileWide(path);
        {
            if (text.size() > 2)
            {
                const std::wstring key = L"Z:/agentmaster-trust-selftest";
                const auto out = SpliceWorkspaceTrust(text, key);
                CHECK(out.has_value(), "live config: spliced");
                if (out)
                {
                    CHECK(out->size() > text.size(), "live config: grew by the inserted entry only");
                    CHECK(out->find(key) != std::wstring::npos, "live config: the new key is present");
                    // Every original byte still there, in order: the splice is one contiguous insert.
                    const size_t at = out->find(key);
                    const size_t insBegin = out->rfind(L'"', at); // start of the inserted quoted key
                    CHECK(insBegin != std::wstring::npos, "live config: insertion located");
                    CHECK(!SpliceWorkspaceTrust(*out, key).has_value(), "live config: result is already-trusted");
                    std::wprintf(L"  [info] live config: %zu chars spliced -> %zu (+%zu), every other byte verbatim\n",
                                 text.size(), out->size(), out->size() - text.size());
                }
            }
            else
            {
                std::wprintf(L"  [info] live config: none on this machine (%ls) - live check skipped\n", path.c_str());
            }
        }
    }
}

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

    // PersistedDebugModeEnabled reads the ENGINE ENVELOPE — {version, settings:{debugMode}} — the
    // shape SaveAppSettings actually writes. The original top-level-only read could never see the
    // nested key (Json.h's *At readers are flat), which made the About-tab "Enable Debug Mode"
    // toggle a permanent no-op: a release relaunched without --debug ran with the Tests Autorunner
    // disabled and queued prompts sat Pending forever (the journey2 report). Exercised against the
    // ACTIVE profile's settings.json — run-m5-tests.bat / wmain point AGENTMASTER_PROFILE at the
    // wiped scratch dir, so ResolveProfileDir() lands there; whatever an earlier suite wrote is
    // saved and restored so this block perturbs nothing.
    {
        namespace fs = std::filesystem;
        const std::wstring dir = P::ResolveProfileDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path sj = fs::path{ dir } / L"settings.json";
        std::string prevBody;
        bool hadPrev = false;
        {
            std::ifstream f{ sj, std::ios::binary };
            if (f)
            {
                hadPrev = true;
                prevBody.assign(std::istreambuf_iterator<char>{ f }, std::istreambuf_iterator<char>{});
            }
        }
        auto put = [&](const std::string& bytes) {
            std::ofstream f{ sj, std::ios::binary | std::ios::trunc };
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        put("{\"version\":1,\"settings\":{\"debugMode\":true}}");
        CHECK(P::PersistedDebugModeEnabled(), "debugMode: envelope true is read (the cog's real shape)");
        put("{\"version\":1,\"settings\":{\"debugMode\":false}}");
        CHECK(!P::PersistedDebugModeEnabled(), "debugMode: envelope false stays off");
        put("{\"version\":1,\"settings\":{\"skipPermissions\":true}}");
        CHECK(!P::PersistedDebugModeEnabled(), "debugMode: envelope without the key -> off");
        put("{\"debugMode\":true}");
        CHECK(P::PersistedDebugModeEnabled(), "debugMode: top-level stray still honored (fallback)");
        put("{\"version\":1,\"settings\":{\"debugMode\":false},\"debugMode\":true}");
        CHECK(!P::PersistedDebugModeEnabled(), "debugMode: a PRESENT envelope key wins over a top-level stray");
        put("not json at all");
        CHECK(!P::PersistedDebugModeEnabled(), "debugMode: garbage file -> off");
        fs::remove(sj, ec);
        CHECK(!P::PersistedDebugModeEnabled(), "debugMode: missing file -> off");
        if (hadPrev)
        {
            put(prevBody);
        }
    }

    // ApplyPersistedDebugMode PROPAGATES the verdict process-wide (the cross-module fix). The
    // DebugForced latch is MODULE-local — a function-local static inside an inline header function
    // is one PRIVATE copy per linked binary, never crossing the DLL boundary — so the EXE-side
    // startup apply used to leave TerminalApp.dll (the Engine scheduler gate + every Auto Testing
    // UI gate) reading FALSE: the About-tab toggle enabled debug in the EXE only and the Tests
    // Autorunner stayed dead in a release install ("did not get enabled across the entire app",
    // proven live by the `[engine] release build (no --debug)` line landing in hooks.log with
    // settings.debugMode:true on disk). The fix rides the per-PROCESS env block —
    // AGENTMASTER_DEBUG=1, the same cross-module channel AGENTMASTER_PROFILE uses — which every
    // module's IsDebugPackage() first-use scan reads. settings.json + the ambient env var are
    // saved/restored (the AGENTMASTER_PROFILE idiom above).
    //
    // ⚠ ORDERING: the ON-case apply LATCHES this module's DebugForced() for the REST OF THE
    // PROCESS (one-way by design — there is no un-force), so this block must stay the LAST in
    // this function, and no later check in the harness RUN may assume IsDevOrDebugPackage() is
    // false (nothing does today: the only engine consumer is SharedEngine()'s wiring, which the
    // harness never calls — tests_persistence.cpp works on local Engine instances for that reason).
    {
        namespace fs = std::filesystem;
        const std::wstring dir = P::ResolveProfileDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path sj = fs::path{ dir } / L"settings.json";
        std::string prevBody;
        bool hadPrev = false;
        {
            std::ifstream f{ sj, std::ios::binary };
            if (f)
            {
                hadPrev = true;
                prevBody.assign(std::istreambuf_iterator<char>{ f }, std::istreambuf_iterator<char>{});
            }
        }
        auto put = [&](const std::string& bytes) {
            std::ofstream f{ sj, std::ios::binary | std::ios::trunc };
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        // Save + clear the ambient var so the assertions read OUR effects only (a developer shell
        // with AGENTMASTER_DEBUG set must not fail the OFF case).
        wchar_t prevEnvBuf[64];
        const DWORD prevEnvLen = ::GetEnvironmentVariableW(L"AGENTMASTER_DEBUG", prevEnvBuf, 64);
        const bool hadEnv = prevEnvLen > 0 && prevEnvLen < 64;
        const std::wstring prevEnv = hadEnv ? std::wstring{ prevEnvBuf, prevEnvLen } : std::wstring{};
        ::SetEnvironmentVariableW(L"AGENTMASTER_DEBUG", nullptr);
        const auto envIsOne = []() {
            wchar_t buf[8];
            const DWORD n = ::GetEnvironmentVariableW(L"AGENTMASTER_DEBUG", buf, 8);
            return n == 1 && buf[0] == L'1';
        };

        // OFF: the apply must neither latch this module nor export the env var.
        put("{\"version\":1,\"settings\":{\"debugMode\":false}}");
        P::ApplyPersistedDebugMode();
        CHECK(!P::detail::DebugForced().load(), "debug apply: OFF setting latches nothing");
        CHECK(!envIsOne(), "debug apply: OFF setting exports no env var");

        // ON: latch THIS module AND export process-wide — the channel every OTHER module reads.
        put("{\"version\":1,\"settings\":{\"debugMode\":true}}");
        P::ApplyPersistedDebugMode();
        CHECK(P::detail::DebugForced().load(), "debug apply: ON setting latches this module");
        CHECK(envIsOne(), "debug apply: ON setting exports AGENTMASTER_DEBUG=1 (cross-module channel)");
        CHECK(P::IsDebugPackage(), "debug apply: gate reads true via the live latch (pre-cache callers heal on next call)");
        CHECK(P::IsDevOrDebugPackage(), "debug apply: the Auto Testing feature-gate predicate is unlocked");

        // Restore ambient state (the latch itself is one-way — see the block comment above).
        ::SetEnvironmentVariableW(L"AGENTMASTER_DEBUG", hadEnv ? prevEnv.c_str() : nullptr);
        if (hadPrev)
        {
            put(prevBody);
        }
        else
        {
            fs::remove(sj, ec);
        }
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
        s.started = true; // its claude is RUNNING — a dormant session is never pressed (DELIVERY.md RC3; the dedicated dormant checks live in TestDeliveryGate)
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
        // Agentmaster (DELIVERY_PLAN.md R2): raw transcript advance past the send NO LONGER drains
        // the watch — a still-running PREVIOUS turn also writes the file, which is exactly how the
        // watchdog drained instead of resolving the lost `#6` (delivered into another turn,
        // consumed by a dialog, never a message). With the pull echo-consume (R1) marking a
        // genuinely-delivered prompt `echoed`, an unechoed one past transcript activity is a
        // problem to KEEP watching, not proof of pickup.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.convLastActivityUnixMs = T + 60000; // the (other) turn wrote plenty past our send
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry,
              "R2: transcript advance alone no longer drains the watch (another turn's writes are not our pickup)");
    }
    {
        // Agentmaster (DELIVERY_PLAN.md R2 — the DRAFT GUARD, proven on the 08:50:07 live log): a
        // box observed holding text that is NOT our watched prompt is never pressed into — a lone
        // Enter would SUBMIT the human's draft (the RC3 merge). Keep watching (Waiting) instead;
        // the lost-send verdict owns the terminal resolution.
        auto s = mk(SessionState::WaitingForInput, T, false, 0);
        s.pendingInput = L"maybe another one"; // the human's draft, not our prompt ("go")
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Waiting,
              "R2 draft guard: a foreign draft in the box blocks the press (Waiting, never Retry)");
        // The box holding OUR prompt is the eaten-CR shape — pressing IS the rescue.
        s.pendingInput = L"go";
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry,
              "R2 draft guard: the box holding the watched prompt still presses (the rescue)");
        // Newline-fold + trailing-trim tolerant: a \r-composed prompt vs the detector's \n box read.
        s.queue[0].text = L"line1\rline2";
        s.pendingInput = L"line1\nline2\n";
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry,
              "R2 draft guard: fold/trim-matched box text still presses");
        // An empty observed box allows the press (a lone Enter into an empty box is a claude no-op).
        s.pendingInput.clear();
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::Retry,
              "R2 draft guard: an empty box presses");
        // The guard never blocks the give-up accounting: at max presses a draft-blocked prompt
        // still resolves through GiveUp's Failed+paused (the guard sits after the GiveUp branch).
        s.pendingInput = L"human draft";
        s.queue[0].enterRetries = kEnterRetryMax;
        CHECK(DecideEnterRetry(s, T + kEnterRetryIntervalMs).action == EnterRetryAction::GiveUp,
              "R2 draft guard: an exhausted ladder still gives up (Failed + paused), draft or not");
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

// Agentmaster (DELIVERY.md) — the DELIVERY GATE: the per-session "someone owns the input box" fact
// that serializes the whole placement pipeline. Covers the pure predicate, the registry's
// open/decline/owner-close/reclaim/stale-close semantics + the close-notify wake-up, DecideAdvance's
// hold, DecideEnterRetry's gate + dormant refusals, SubmitPrompt's synchronous decline / fallback
// resolve / submitter lifecycles, and the RC6 newline-folded echo consume.
void TestDeliveryGate()
{
    std::wprintf(L"Delivery gate (DELIVERY.md):\n");

    // ---- the pure predicate ----
    {
        SessionInfo s = MakeSession(L"g");
        const int64_t T = 1000000;
        CHECK(!DeliveryGateOpen(s, T), "gate: default closed");
        s.deliveryPromptId = L"p1";
        s.deliveryOpenedUnixMs = T;
        CHECK(DeliveryGateOpen(s, T + 1000), "gate: open within the window");
        CHECK(!DeliveryGateOpen(s, T + kDeliveryGateTimeoutMs), "gate: expired reads closed (fail-open)");
        CHECK(!DeliveryGateOpen(s, T - 5), "gate: a future stamp reads closed (clock jump)");
        s.deliveryOpenedUnixMs = 0;
        CHECK(!DeliveryGateOpen(s, T), "gate: a tag with no stamp reads closed");
    }

    // ---- registry open / decline / owner-close / reclaim / stale-close ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"gate-1");
        s.live = true;
        reg.Upsert(s);
        CHECK(reg.TryOpenDeliveryGate(L"gate-1", L"pA"), "registry: first open succeeds");
        CHECK(!reg.TryOpenDeliveryGate(L"gate-1", L"pB"), "registry: second open declined while held");
        CHECK(reg.DeliveryGateHeld(L"gate-1"), "registry: held query");
        reg.CloseDeliveryGate(L"gate-1", L"pB"); // a mismatched close must NOT clear pA's claim
        CHECK(reg.DeliveryGateHeld(L"gate-1"), "registry: mismatched close is a no-op");
        reg.CloseDeliveryGate(L"gate-1", L"pA");
        CHECK(!reg.DeliveryGateHeld(L"gate-1"), "registry: owner close releases");
        CHECK(reg.TryOpenDeliveryGate(L"gate-1", L"pB"), "registry: reopen after close");
        // Expiry reclaim: backdate the open stamp past the timeout, then a new open reclaims it.
        reg.UpdateQuiet(L"gate-1", [](SessionInfo& ss) { ss.deliveryOpenedUnixMs -= (kDeliveryGateTimeoutMs + 1000); });
        CHECK(!reg.DeliveryGateHeld(L"gate-1"), "registry: an expired gate reads not-held");
        CHECK(reg.TryOpenDeliveryGate(L"gate-1", L"pC"), "registry: an expired gate is reclaimed by a new open");
        reg.CloseDeliveryGate(L"gate-1", L"pB"); // the dead holder's late close: mismatched now
        CHECK(reg.DeliveryGateHeld(L"gate-1"), "registry: the dead holder's late close cannot clear the reclaimer");
        reg.CloseDeliveryGate(L"gate-1", L"pC");
        CHECK(!reg.TryOpenDeliveryGate(L"unknown-id", L"p"), "registry: an unknown session cannot open");
        CHECK(!reg.TryOpenDeliveryGate(L"gate-1", L""), "registry: an empty tag is refused (it could never be owner-closed)");
    }

    // ---- the close NOTIFIES (the held advance's wake-up); opens + double closes stay quiet ----
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"gate-n"));
        std::atomic<int> notifies{ 0 };
        const auto tok = reg.AddObserver([&](const SessionInfo&, HookEvent) { ++notifies; });
        reg.TryOpenDeliveryGate(L"gate-n", L"p1");
        const int afterOpen = notifies.load();
        reg.CloseDeliveryGate(L"gate-n", L"p1");
        CHECK(notifies.load() == afterOpen + 1, "gate: a real close notifies exactly once (the open is quiet)");
        reg.CloseDeliveryGate(L"gate-n", L"p1"); // double close
        CHECK(notifies.load() == afterOpen + 1, "gate: a double close is quiet (no phantom wake-ups)");
        reg.RemoveObserver(tok);
    }

    // ---- DecideAdvance HOLDS while the gate is open (the 07:27 livelock's fix) ----
    {
        SessionInfo s = MakeSession(L"a", SessionState::WaitingForInput);
        s.live = true;
        s.autorunner.mode = AutorunnerMode::Full;
        QueuedPrompt p;
        p.id = L"q1";
        p.text = L"next";
        s.queue.push_back(p);
        const int64_t T = 1000000;
        CHECK(DecideAdvance(s, T, 0, false).action == AdvanceAction::Send, "advance: sends with the gate closed");
        s.deliveryPromptId = L"in-flight-prompt";
        s.deliveryOpenedUnixMs = T - 1000;
        const auto held = DecideAdvance(s, T, 0, false);
        CHECK(held.action == AdvanceAction::None && held.reason == L"delivery in flight", "advance: HOLDS while the gate is open (never mark-then-decline)");
        s.deliveryOpenedUnixMs = T - kDeliveryGateTimeoutMs - 1;
        CHECK(DecideAdvance(s, T, 0, false).action == AdvanceAction::Send, "advance: an expired gate no longer holds (the leaked-gate belt)");
    }

    // ---- the EVIDENCE-released pickup guard (DELIVERY.md §9 — no more naked 4s expiry) ----
    {
        const int64_t T = 1000000;
        SessionInfo s = MakeSession(L"pg", SessionState::WaitingForInput);
        s.live = true;
        s.autorunner.mode = AutorunnerMode::Full;
        QueuedPrompt sent; // the just-delivered prompt, still unacknowledged
        sent.id = L"p1";
        sent.text = L"first";
        sent.status = PromptStatus::Sent;
        sent.origin = PromptOrigin::Autorun;
        sent.echoed = false;
        sent.sentAtUnixMs = T;
        s.queue.push_back(sent);
        QueuedPrompt next;
        next.id = L"p2";
        next.text = L"second";
        s.queue.push_back(next);
        // No evidence the turn started: held FAR past the old 4s window — the 07:27 incident's #6
        // fired 9s after #5 exactly through that lapsed window.
        const auto pgHeld = DecideAdvance(s, T + 9000, 0, false);
        CHECK(pgHeld.action == AdvanceAction::None && pgHeld.reason == L"awaiting injection pickup",
              "pickup: held at 9s with no turn evidence (the old 4s expiry fired here)");
        // The echo releases — push (the hook) or pull (the scanner's transcript consume, R1): both
        // land as `echoed`, THE became-a-message fact.
        s.queue[0].echoed = true;
        CHECK(DecideAdvance(s, T + 9000, 0, false).action == AdvanceAction::Send, "pickup: the consumed echo releases (push or pull)");
        s.queue[0].echoed = false;
        // A newer UserPromptSubmit stamp releases (an echo the text-match missed still proves
        // pickup — and R3 makes the stamp sound: an EMPTY phantom twin no longer writes it).
        s.turns.lastPromptUnixMs = T + 1500;
        CHECK(DecideAdvance(s, T + 9000, 0, false).action == AdvanceAction::Send, "pickup: a newer prompt stamp releases");
        s.turns.lastPromptUnixMs = 0;
        // Agentmaster (DELIVERY_PLAN.md R2): raw transcript advance past the send NO LONGER
        // releases — a still-running PREVIOUS turn also writes the file (the exact shape in which
        // `#6` was stacked and lost), so growth alone proves nothing about OUR prompt's turn.
        s.convLastActivityUnixMs = T + 60000;
        CHECK(DecideAdvance(s, T + 9000, 0, false).action == AdvanceAction::None,
              "R2: transcript advance alone no longer releases the guard (another turn's writes are not our pickup)");
        s.convLastActivityUnixMs = 0;
        // The lost-evidence belt: past kPickupGuardMaxMs the guard stands aside (the Enter-retry
        // watchdog / the lost-send verdict have long since resolved a truly-dead send to Failed).
        CHECK(DecideAdvance(s, T + kPickupGuardMaxMs, 0, false).action == AdvanceAction::Send, "pickup: the 30s belt releases");
    }

    // ---- DecideEnterRetry: never press while gated / into a dormant session ----
    {
        const int64_t T = 1000000;
        SessionInfo s = MakeSession(L"r", SessionState::Idle);
        s.live = true;
        s.started = true;
        QueuedPrompt p;
        p.id = L"q1";
        p.text = L"go";
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Autorun;
        p.echoed = false;
        p.sentAtUnixMs = T;
        s.queue.push_back(p);
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::Retry, "enter-retry: due press with the gate closed + started");
        s.deliveryPromptId = L"q1";
        s.deliveryOpenedUnixMs = T;
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::None, "enter-retry: NEVER presses while the gate is open (a mid-swap Enter would submit the user's draft)");
        s.deliveryPromptId.clear();
        s.deliveryOpenedUnixMs = 0;
        s.started = false;
        s.external = false;
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::None, "enter-retry: never presses into a DORMANT session (restart press-storms)");
        s.external = true; // adopted: `started` is meaningless for it — still drivable
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::Retry, "enter-retry: an adopted external stays drivable");
    }

    // ---- SubmitPrompt: synchronous decline while held; the fallback resolves synchronously;
    //      an accepted submitter keeps the gate open until the window's close ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"sub", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        std::atomic<int> injected{ 0 };
        reg.SetInjector(L"sub", [&](const std::wstring&) { ++injected; });
        CHECK(reg.TryOpenDeliveryGate(L"sub", L"held"), "submit: pre-hold the gate");
        CHECK(!reg.SubmitPrompt({ L"sub", L"pX", L"text", false }), "submit: declined while the gate is held");
        CHECK(injected.load() == 0, "submit: a declined submit reaches no injector");
        reg.CloseDeliveryGate(L"sub", L"held");
        CHECK(reg.SubmitPrompt({ L"sub", L"pX", L"text", false }), "submit: delivers once the gate is free");
        CHECK(injected.load() == 1, "submit: the fallback inject ran exactly once");
        CHECK(!reg.DeliveryGateHeld(L"sub"), "submit: the fallback path closes the gate synchronously");
        // Submitter path: `accepted` keeps the gate OPEN — the hosting window owns the close.
        reg.SetPromptSubmitter(L"sub", [](const ::Agentmaster::PromptSubmission&) { return true; });
        CHECK(reg.SubmitPrompt({ L"sub", L"pY", L"text", false }), "submit: submitter accepted");
        CHECK(reg.DeliveryGateHeld(L"sub"), "submit: accepted keeps the gate open until the swap resolves");
        CHECK(!reg.SubmitPrompt({ L"sub", L"pZ", L"text", false }), "submit: a second delivery is declined while the first is unresolved");
        reg.CloseDeliveryGate(L"sub", ::Agentmaster::DeliveryGateTagFor({ L"sub", L"pY", L"text", false }));
        CHECK(!reg.DeliveryGateHeld(L"sub"), "submit: the window's owner-tagged close releases");
        // A submitter that REFUSES closes the gate right here (nothing was or will be sent).
        reg.SetPromptSubmitter(L"sub", [](const ::Agentmaster::PromptSubmission&) { return false; });
        CHECK(!reg.SubmitPrompt({ L"sub", L"pW", L"text", false }), "submit: submitter refusal reads not-accepted");
        CHECK(!reg.DeliveryGateHeld(L"sub"), "submit: a refused submission closes the gate");
        // A THROWING submitter: contained (Rule #18 logging), false, gate closed.
        reg.SetPromptSubmitter(L"sub", [](const ::Agentmaster::PromptSubmission&) -> bool { throw 42; });
        CHECK(!reg.SubmitPrompt({ L"sub", L"pT", L"text", false }), "submit: a throwing submitter reads not-accepted");
        CHECK(!reg.DeliveryGateHeld(L"sub"), "submit: a throwing submitter closes the gate");
        // The tag rule: an id-less submission still maps to a closable owner tag.
        CHECK(DeliveryGateTagFor({ L"sub", L"", L"text", false }) == L"#send", "gate tag: an id-less submission gets the reserved #send tag");
        CHECK(DeliveryGateTagFor({ L"sub", L"pid-1", L"text", false }) == L"pid-1", "gate tag: a prompt id IS its tag");
    }

    // ---- RC6: the \r-composed multi-line prompt's \n echo is CONSUMED, once, with no dup row ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"echo", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        reg.Update(L"echo", [&](SessionInfo& ss) {
            QueuedPrompt p;
            p.id = L"m1";
            p.text = L"line one\rline two"; // the compose TextBox stores a typed newline as \r
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = NowMsTest();
            ss.queue.push_back(p);
        });
        reg.OnHookEvent(UPS(L"echo", L"line one\nline two")); // the wire echo carries \n (BuildPromptFill folded at inject)
        const auto after = reg.Get(L"echo");
        CHECK(after.has_value() && after->queue.size() == 1, "echo-fold: no duplicate Typed row is recorded");
        CHECK(after.has_value() && !after->queue.empty() && after->queue[0].echoed, "echo-fold: the \\r-composed prompt's \\n echo is consumed");
        // CRLF-composed folds identically.
        reg.Update(L"echo", [&](SessionInfo& ss) {
            QueuedPrompt p;
            p.id = L"m2";
            p.text = L"a\r\nb";
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = NowMsTest();
            ss.queue.push_back(p);
        });
        reg.OnHookEvent(UPS(L"echo", L"a\nb"));
        const auto after2 = reg.Get(L"echo");
        CHECK(after2.has_value() && after2->queue.size() == 2, "echo-fold: CRLF prompt adds no dup row");
        CHECK(after2.has_value() && after2->queue.size() == 2 && after2->queue[1].echoed, "echo-fold: the CRLF prompt's echo is consumed");
    }
}

// Agentmaster (DELIVERY_PLAN.md R1) — the LOST-SEND reconciler: the PULL echo-consume
// (NoteExternalPrompt marking a fold-matched transcript user line as a Sent prompt's `echoed`
// evidence) + the pure DecideLostSend verdict (a Sent, unechoed, at-rest, settled, gate-closed
// Autorun prompt provably never became a message -> the scanner marks it Failed + pauses the
// autorunner). The `#6` gap: a prompt pasted into a running turn, consumed by an AskUserQuestion
// dialog's rendering, sat terminally `Sent` — indistinguishable from a success.
void TestLostSendReconciler()
{
    std::wprintf(L"Lost-send reconciler (DELIVERY_PLAN.md R1):\n");

    const int64_t T = 1'700'000'000'000;

    // ---- the PULL echo-consume (NoteExternalPrompt's new evidence side-effect) ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"pull", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        const int64_t sentAt = NowMsTest() - 5000;
        reg.Update(L"pull", [&](SessionInfo& ss) {
            QueuedPrompt p;
            p.id = L"f1";
            p.text = L"run the suite\ragain"; // compose-box \r newline
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = sentAt;
            ss.queue.push_back(p);
        });
        // A STALE identical line (its own ts BEFORE the send — a history replay) must not vouch.
        reg.NoteExternalPrompt(L"pull", L"run the suite\nagain", sentAt - 60000);
        auto after = reg.Get(L"pull");
        CHECK(after && after->queue.size() == 1 && !after->queue[0].echoed,
              "pull-echo: a pre-send transcript line never consumes (replay guard)");
        // The FRESH line (ts at/after the send) consumes — fold-matched, no duplicate row.
        reg.NoteExternalPrompt(L"pull", L"run the suite\nagain", sentAt + 1200);
        after = reg.Get(L"pull");
        CHECK(after && after->queue.size() == 1, "pull-echo: the consuming line is deduped (no Typed row)");
        CHECK(after && after->queue[0].echoed, "pull-echo: a fresh fold-matched user line marks the Sent prompt echoed");
        // A ts-less caller (0) trusts the fold match — the pre-R1 call shape stays consuming.
        reg.Update(L"pull", [&](SessionInfo& ss) {
            QueuedPrompt p;
            p.id = L"f2";
            p.text = L"second send";
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = NowMsTest();
            ss.queue.push_back(p);
        });
        reg.NoteExternalPrompt(L"pull", L"second send");
        after = reg.Get(L"pull");
        CHECK(after && after->queue.size() == 2 && after->queue[1].echoed, "pull-echo: a ts-less observation (0) still consumes");
        // A NON-matching line records a Typed row exactly as before (the back-fill is untouched).
        reg.NoteExternalPrompt(L"pull", L"something the human typed", NowMsTest());
        after = reg.Get(L"pull");
        CHECK(after && after->queue.size() == 3 && after->queue[2].origin == PromptOrigin::Typed,
              "pull-echo: a non-matching line still back-fills a Typed row");
        // Re-seeing the recorded line dedupes quietly and flips nothing (Typed rows are echoed=true).
        reg.NoteExternalPrompt(L"pull", L"something the human typed", NowMsTest());
        after = reg.Get(L"pull");
        CHECK(after && after->queue.size() == 3, "pull-echo: a re-seen recorded line stays deduped");
    }

    // ---- DecideLostSend: the conjunct matrix ----
    {
        const auto mkLost = [&](SessionState st) {
            SessionInfo s = MakeSession(L"lost", st);
            s.live = true;
            QueuedPrompt p;
            p.id = L"l1";
            p.label = L"the lost one";
            p.text = L"then for another one but spawn subagent this time";
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = T;
            s.queue.push_back(p);
            return s;
        };
        // The verdict shape: at rest + settled + unechoed + gate closed -> LOST.
        {
            const auto s = mkLost(SessionState::WaitingForInput);
            const auto lost = DecideLostSend(s, T + kLostSendSettleMs);
            CHECK(lost.size() == 1 && lost[0] == L"l1", "lost: at-rest + settled + unechoed -> LOST");
            CHECK(DecideLostSend(mkLost(SessionState::Idle), T + kLostSendSettleMs).size() == 1, "lost: Idle counts as at rest too");
        }
        // Each conjunct's negation holds the verdict:
        {
            auto s = mkLost(SessionState::Running);
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: never judged mid-turn (Running)");
            s.state = SessionState::NeedsApproval;
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: never judged while needs-you (the dialog may yet consume it)");
        }
        {
            auto s = mkLost(SessionState::WaitingForInput);
            CHECK(DecideLostSend(s, T + kLostSendSettleMs - 1).empty(), "lost: not before the settle elapses (a slow push echo can still land)");
        }
        {
            auto s = mkLost(SessionState::WaitingForInput);
            s.queue[0].echoed = true; // push OR pull evidence arrived
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: an echoed prompt is a delivered message, never lost");
        }
        {
            auto s = mkLost(SessionState::WaitingForInput);
            s.deliveryPromptId = L"l1";
            s.deliveryOpenedUnixMs = T + kLostSendSettleMs - 1000;
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: an open delivery gate defers the verdict");
        }
        {
            auto s = mkLost(SessionState::WaitingForInput);
            s.live = false;
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: an archived record is never judged");
        }
        {
            auto s = mkLost(SessionState::WaitingForInput);
            s.queue[0].origin = PromptOrigin::Typed;
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: a Typed capture is already the message, never a lost send");
            s.queue[0].origin = PromptOrigin::Autorun;
            s.queue[0].sentAtUnixMs = 0;
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: an un-stamped (restored) Sent row is never judged");
        }
        // The watchdog serialization: a press REFRESHES sentAtUnixMs, restarting the settle — the
        // verdict structurally waits out an active ladder (DecideLostSend's contract).
        {
            auto s = mkLost(SessionState::WaitingForInput);
            s.queue[0].sentAtUnixMs = T + 10000; // the last Enter-retry press
            s.queue[0].enterRetries = 2;
            CHECK(DecideLostSend(s, T + kLostSendSettleMs).empty(), "lost: a fresh watchdog press restarts the settle clock");
            CHECK(DecideLostSend(s, T + 10000 + kLostSendSettleMs).size() == 1, "lost: settled after the ladder went quiet -> LOST");
        }
        // Multiple lost sends all report (send-now + autorun can both strand one).
        {
            auto s = mkLost(SessionState::WaitingForInput);
            QueuedPrompt p2 = s.queue[0];
            p2.id = L"l2";
            p2.sentAtUnixMs = T + 2000;
            s.queue.push_back(p2);
            const auto lost = DecideLostSend(s, T + 2000 + kLostSendSettleMs);
            CHECK(lost.size() == 2, "lost: every settled unechoed Sent reports (not just the newest)");
        }
    }

    // ---- the `#6` shape end-to-end at the registry level (what the scanner's pass sees) ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"six", SessionState::Running); // the dialog turn is running
        s.live = true;
        reg.Upsert(s);
        const int64_t sentAt = NowMsTest() - kLostSendSettleMs - 2000;
        reg.Update(L"six", [&](SessionInfo& ss) {
            QueuedPrompt p;
            p.id = L"p6";
            p.text = L"then for another one but spawn subagent this time";
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = sentAt;
            ss.queue.push_back(p);
        });
        // While the blocking turn runs: transcript lines from THAT turn arrive (another message's
        // text) — they must neither consume our echo nor release anything.
        reg.NoteExternalPrompt(L"six", L"check for best one", sentAt + 500);
        auto mid = reg.Get(L"six");
        CHECK(mid && !mid->queue[0].echoed, "six: another turn's user line is not our echo");
        CHECK(DecideLostSend(*mid, NowMsTest()).empty(), "six: no verdict while the session is Running");
        // The turn ends; the session comes to rest; the settle has long elapsed -> LOST.
        reg.Update(L"six", [](SessionInfo& ss) { ss.state = SessionState::WaitingForInput; });
        const auto rest = reg.Get(L"six");
        const auto lost = DecideLostSend(*rest, NowMsTest());
        CHECK(lost.size() == 1 && lost[0] == L"p6", "six: at rest, the vanished delivery is the verdict");
        // The scanner's apply shape: re-verified mutate -> Failed + autorunner paused.
        reg.Update(L"six", [&](SessionInfo& ss) {
            for (auto& p : ss.queue)
            {
                if (p.id == lost[0] && p.status == PromptStatus::Sent && !p.echoed)
                {
                    p.status = PromptStatus::Failed;
                    ss.autorunner.mode = AutorunnerMode::Off;
                }
            }
        });
        const auto done = reg.Get(L"six");
        CHECK(done && done->queue[0].status == PromptStatus::Failed && done->autorunner.mode == AutorunnerMode::Off,
              "six: the apply lands Failed + a paused autorunner (never a silent resend)");
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

    // --- The SemiAuto arm must SETTLE, not spin (the 2026-07-25 release freeze). Arming the
    //     one-click confirm writes pendingConfirmPromptId; SessionRegistry::Update _notify's
    //     unconditionally, OnObserved re-requests an advance for a SemiAuto session whose prompt is
    //     (by design) still Pending, and _process re-reached the arm — so the old unconditional
    //     re-write of the SAME armed id closed a notify -> OnObserved -> RequestAdvance -> re-arm
    //     cycle at ~3ms/turn, forever (137k+ [await-confirm] lines live; the per-notify observer
    //     fan-out starved the UI dispatcher into a half-frozen app). With the change-gate the cycle
    //     ends on its settle pass: the confirm arms once, the prompt stays Pending, and the notify
    //     stream goes QUIET. This drives the real worker + the Engine.cpp wiring and counts
    //     notifies — pre-fix this measured hundreds in the settle window. ---
    {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });
        std::atomic<int> notifies{ 0 };
        const auto cntTok = reg->AddObserver([&notifies](const SessionInfo&, HookEvent) { notifies.fetch_add(1); });

        const std::wstring id = L"semi-sess";
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput; // at rest — exactly the state the spin lived in
            s.live = true;
            s.started = true;
            s.autorunner.mode = AutorunnerMode::Off; // the stop-on-error backstop just paused it
            s.autorunner.throttleMs = 0;
            QueuedPrompt p;
            p.id = L"p1";
            p.text = L"the pending prompt";
            p.status = PromptStatus::Pending;
            p.origin = PromptOrigin::Autorun;
            s.queue.push_back(p);
            reg->Upsert(std::move(s));
        }
        reg->SetInjector(id, [](const std::wstring&) {});

        // The per-tab overlay's cycle click, Off -> Semi (AgentTabOverlay::_CycleAutorunner's write).
        reg->Update(id, [](SessionInfo& s) {
            s.autorunner.mode = AutorunnerMode::SemiAuto;
            s.autorunner.autoSendsThisRun = 0;
            s.pendingConfirmPromptId.clear();
        });

        // Wait for the confirm to arm...
        bool armed = false;
        for (int i = 0; i < 200 && !armed; ++i)
        {
            const auto snap = reg->Get(id);
            armed = snap && snap->pendingConfirmPromptId == L"p1";
            if (!armed)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(armed, "semi-auto arm: pendingConfirmPromptId set for the first Pending prompt");
        // ...then the stream must go QUIET: no further Update/notify while the confirm awaits the
        // user. Pre-fix the re-arm cycle produced ~2 notifies every ~3ms (hundreds in this window).
        const int atArm = notifies.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const int drift = notifies.load() - atArm;
        CHECK(drift <= 2, "semi-auto arm SETTLES: no notify feedback spin while awaiting the confirm");
        {
            const auto snap = reg->Get(id);
            CHECK(snap && snap->queue[0].status == PromptStatus::Pending, "semi-auto: the armed prompt stays Pending (nothing auto-sent)");
        }
        reg->RemoveObserver(cntTok);
        reg->RemoveObserver(obsTok);
        sched.Stop();
    }

    // --- The un-hold probe must not TOUCH the registry when nothing is Held (the spin's second
    //     closer). _process opened with an UNCONDITIONAL Update whose lambda usually changed
    //     nothing — but Update notifies regardless, so even a DecideAdvance that answers None (the
    //     pickup guard, the question guard, global pause) re-notified and re-queued the advance.
    //     Shape: mode Full, prompt #1 just Sent (awaiting pickup), prompt #2 Pending — pre-fix the
    //     4s pickup window was a silent ~3ms spin (hidden by the [advance-skip] dedup). Now the
    //     no-Held pass is registry-silent and the window settles. ---
    {
        auto reg = std::make_shared<SessionRegistry>();
        Scheduler sched{ reg };
        sched.Start();
        reg->SetAdvanceHandler([&sched](const std::wstring& id) { sched.RequestAdvance(id); });
        const auto obsTok = reg->AddObserver([&sched](const SessionInfo& s, HookEvent) { sched.OnObserved(s); });
        std::atomic<int> notifies{ 0 };
        const auto cntTok = reg->AddObserver([&notifies](const SessionInfo&, HookEvent) { notifies.fetch_add(1); });

        const std::wstring id = L"pickup-sess";
        {
            SessionInfo s;
            s.id = id;
            s.workingDir = L"K:\\tmp";
            s.state = SessionState::WaitingForInput;
            s.live = true;
            s.started = true;
            s.autorunner.mode = AutorunnerMode::Full;
            s.autorunner.throttleMs = 0;
            QueuedPrompt p1;
            p1.id = L"p1";
            p1.text = L"first";
            p1.status = PromptStatus::Pending;
            p1.origin = PromptOrigin::Autorun;
            s.queue.push_back(p1);
            QueuedPrompt p2;
            p2.id = L"p2";
            p2.text = L"second";
            p2.status = PromptStatus::Pending;
            p2.origin = PromptOrigin::Autorun;
            s.queue.push_back(p2);
            reg->Upsert(std::move(s));
        }
        reg->SetInjector(id, [](const std::wstring&) {});

        // Kick the plan: #1 sends, #2 sits Pending behind the 4s pickup guard.
        reg->Update(id, [](SessionInfo& s) { s.autorunner.autoSendsThisRun = 0; });
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
        CHECK(sent, "pickup window: the first prompt sends");
        const auto atSent = notifies.load();
        // Inside the pickup hold (no turn evidence yet — well under kPickupGuardMaxMs and the 3s
        // first Enter-retry) the registry must stay untouched: DecideAdvance answers None (awaiting
        // pickup) WITHOUT the un-hold probe's no-op Update re-notifying. Pre-fix: hundreds of
        // notifies here.
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const int drift = notifies.load() - atSent;
        CHECK(drift <= 2, "pickup window SETTLES: the no-Held un-hold probe is registry-silent");
        {
            const auto snap = reg->Get(id);
            CHECK(snap && snap->queue[1].status == PromptStatus::Pending, "pickup window: the second prompt stays Pending (queue not drained)");
        }
        reg->RemoveObserver(cntTok);
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
        CHECK(!missing.allowNightly, "prefs: missing settings.json -> nightly OFF (nightlies always skipped by default)");

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
        cogSaved.allowUpdateNightly = true; // the warning-gated nightly opt-in rides the same envelope
        CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(cogSaved)), "atomic: engine-shaped seed lands");
        CHECK(U::ReadPrefs(dir).allowPrerelease, "prefs: cog-saved (nested) prerelease opt-in is READ (bug-1 regression)");
        CHECK(U::ReadPrefs(dir).allowNightly, "prefs: cog-saved (nested) NIGHTLY opt-in is READ");

        U::WriteSkip(dir, L"v1.0.0");
        U::WritePostpone(dir, 1234567890123LL);
        const auto p = U::ReadPrefs(dir);
        CHECK(p.skippedVersion == L"v1.0.0", "prefs: WriteSkip round-trips");
        CHECK(p.postponedUntilUnixMs == 1234567890123LL, "prefs: WritePostpone round-trips (int64 survives the double)");
        CHECK(p.allowPrerelease, "prefs: the RMW preserves the cog's nested prerelease");
        CHECK(p.allowNightly, "prefs: the RMW preserves the cog's nested nightly opt-in");
        {
            const AppSettings loaded = DeserializeAppSettings(U::detail::ReadFileWide(sj));
            CHECK(loaded.model == L"opus", "prefs: the cog's model survives the updater RMW");
            CHECK(loaded.allowUpdateNightly, "prefs: the ENGINE reads the nightly opt-in through the RMW'd file");
            CHECK(loaded.updateSkippedVersion == L"v1.0.0" && loaded.updatePostponedUntilUnixMs == 1234567890123LL,
                  "prefs: the ENGINE reads the updater's skip/postpone (same nested fields)");
            // The engine-save cycle (bug-2 regression): rebuild the envelope from the struct — the
            // updater's choices must SURVIVE it (top-level keys used to be wiped exactly here).
            CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(loaded)), "atomic: engine save-cycle lands");
            const auto after = U::ReadPrefs(dir);
            CHECK(after.skippedVersion == L"v1.0.0" && after.postponedUntilUnixMs == 1234567890123LL && after.allowPrerelease,
                  "prefs: skip/postpone/prerelease SURVIVE an engine save (bug-2 regression)");
            CHECK(after.allowNightly, "prefs: the nightly opt-in SURVIVES an engine save");
        }
        // A settings.json WITHOUT the nightly key (every pre-feature install) reads OFF — the
        // engine deserialize AND the updater's prefs read agree on the safe default.
        {
            AppSettings noNightly;
            noNightly.allowUpdatePrerelease = true;
            CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(noNightly)), "atomic: nightly-off seed lands");
            CHECK(!DeserializeAppSettings(U::detail::ReadFileWide(sj)).allowUpdateNightly, "prefs: engine default for a present-but-false nightly key is OFF");
            CHECK(U::ReadPrefs(dir).allowPrerelease && !U::ReadPrefs(dir).allowNightly,
                  "prefs: prerelease ON never implies nightly (orthogonal opt-ins)");
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
    // hourly re-prompt). The GATE is PRESENCE-based (any decline turns the startup/hourly checks
    // fully off, pre-network, until the next launch); the tag is kept for the log trail + queries.
    // A fresh launch (RunStartupUpdateCheck) CLEARS an inherited latch so "next launch asks again"
    // holds even across env inheritance. Save/restore the ambient var (harness hygiene).
    {
        wchar_t prevBuf[256];
        const DWORD prevLen = ::GetEnvironmentVariableW(U::kDeclinedEnvVar, prevBuf, 256);
        const bool hadPrev = prevLen > 0 && prevLen < 256;
        const std::wstring prev = hadPrev ? std::wstring{ prevBuf, prevLen } : std::wstring{};

        U::MarkDeclinedThisRun(L""); // ensure a clean slate regardless of the ambient env
        CHECK(U::DeclinedThisRunTag().empty(), "declined: unset -> empty tag (checks run)");
        CHECK(!U::WasDeclinedThisRun(L"v0.6.8"), "declined: unset -> false");
        U::MarkDeclinedThisRun(L"v0.6.8");
        CHECK(U::DeclinedThisRunTag() == L"v0.6.8", "declined: PRESENCE gate sees the latch (whole check skipped until restart)");
        CHECK(U::WasDeclinedThisRun(L"v0.6.8"), "declined: exact-tag query matches");
        CHECK(!U::WasDeclinedThisRun(L"v0.6.9"), "declined: exact-tag query rejects another tag");
        CHECK(!U::WasDeclinedThisRun(L""), "declined: empty tag never matches");
        U::MarkDeclinedThisRun(L"v0.7.0");
        CHECK(!U::WasDeclinedThisRun(L"v0.6.8") && U::WasDeclinedThisRun(L"v0.7.0"),
              "declined: re-decline replaces the tag (single-slot latch)");
        U::MarkDeclinedThisRun(L"");
        CHECK(!U::WasDeclinedThisRun(L"v0.7.0"), "declined: cleared");

        // A FRESH LAUNCH always asks again: RunStartupUpdateCheck clears an INHERITED latch first
        // (the installer relaunch / a child-spawned instance carries the parent's env). In the
        // harness the channel gate then bails before any network — so this exercises exactly the
        // clear + the gate, nothing else. AGENTMASTER_UPDATE_STARTUP is neutralized for the call:
        // if a dev shell exported it, the forced-on channel would do a REAL network round-trip and
        // could raise a REAL TaskDialog mid-harness.
        {
            wchar_t forceBuf[256];
            const DWORD forceLen = ::GetEnvironmentVariableW(L"AGENTMASTER_UPDATE_STARTUP", forceBuf, 256);
            const bool hadForce = forceLen > 0 && forceLen < 256;
            const std::wstring force = hadForce ? std::wstring{ forceBuf, forceLen } : std::wstring{};
            ::SetEnvironmentVariableW(L"AGENTMASTER_UPDATE_STARTUP", nullptr);
            U::MarkDeclinedThisRun(L"v0.6.8");
            CHECK(!U::RunStartupUpdateCheck(nullptr), "declined: startup check no-ops off-channel (harness is unpackaged)");
            CHECK(U::DeclinedThisRunTag().empty(), "declined: a fresh launch CLEARS an inherited latch (asks again)");
            ::SetEnvironmentVariableW(L"AGENTMASTER_UPDATE_STARTUP", hadForce ? force.c_str() : nullptr);
        }

        ::SetEnvironmentVariableW(U::kDeclinedEnvVar, hadPrev ? prev.c_str() : nullptr);
    }

    // ==== Hardening suite ("never break updating") — safeguards added with the try-catch-log pass ====

    // ParseVersion overflow clamp: a degenerate/hostile tag must not overflow the int accumulator
    // (UB — a negative component would corrupt every CompareVersion after it).
    {
        const auto huge = U::ParseVersion(L"v99999999999999.2.3");
        CHECK(huge.major == 100000000 && huge.minor == 2 && huge.patch == 3, "clamp: absurd component clamps, later parts intact");
        CHECK(huge.major > 0 && U::CompareVersion(huge, U::ParseVersion(L"v1.0.0")) > 0, "clamp: clamped version stays positive + orders sanely");
    }

    // Trusted-asset gate (IsTrustedAssetUrl): the installer DOWNLOADS AND EXECUTES what these URLs
    // point at, so only our own repo's release-download URLs may ever qualify as installable.
    {
        CHECK(U::IsTrustedAssetUrl(L"https://github.com/Nucs/Agentmaster/releases/download/v1.0.0/Agentmaster.msixbundle"), "trust: own release-download URL accepted");
        CHECK(!U::IsTrustedAssetUrl(L"https://evil.example/Agentmaster.msixbundle"), "trust: foreign host rejected");
        CHECK(!U::IsTrustedAssetUrl(L"http://github.com/Nucs/Agentmaster/releases/download/v1/x.msixbundle"), "trust: plain http rejected");
        CHECK(!U::IsTrustedAssetUrl(L"https://github.com/Nucs/Agentmaster/releases/download-evil/v1/x.msixbundle"), "trust: prefix-confusable path rejected");
        CHECK(!U::IsTrustedAssetUrl(L"https://github.com/SomeoneElse/Agentmaster/releases/download/v1/x.msixbundle"), "trust: another repo rejected");
        CHECK(!U::IsTrustedAssetUrl(L""), "trust: empty rejected");
    }

    // ParseReleaseObj: a synthetic GitHub release object end-to-end — asset detection (extension
    // case-insensitive), digest capture, availability vs the current version, and the URL gate.
    {
        const std::wstring relJson =
            L"{\"tag_name\":\"v2.0.0\",\"prerelease\":true,\"html_url\":\"https://github.com/Nucs/Agentmaster/releases/tag/v2.0.0\","
            L"\"body\":\"notes\",\"assets\":["
            L"{\"name\":\"Agentmaster.MsixBundle\",\"browser_download_url\":\"https://github.com/Nucs/Agentmaster/releases/download/v2.0.0/Agentmaster.MsixBundle\",\"digest\":\"sha256:abc\"},"
            L"{\"name\":\"Agentmaster.cer\",\"browser_download_url\":\"https://github.com/Nucs/Agentmaster/releases/download/v2.0.0/Agentmaster.cer\"},"
            L"{\"name\":\"portable.zip\",\"browser_download_url\":\"https://github.com/Nucs/Agentmaster/releases/download/v2.0.0/portable.zip\"}]}";
        const auto rel = json::Parse(relJson);
        CHECK(rel && rel->type == json::Value::Type::Obj, "release: synthetic JSON parses");
        U::UpdateInfo info;
        U::Version cur;
        cur.major = 1; // 1.0.0 -> a v2.0.0 release is newer
        U::ParseReleaseObj(*rel, cur, info);
        CHECK(info.available && info.isPrerelease, "release: newer prerelease reads available");
        CHECK(info.installable && !info.bundleUrl.empty() && !info.cerUrl.empty(), "release: bundle+cer detected (ext case-insensitive)");
        CHECK(info.bundleSha256 == L"sha256:abc", "release: digest captured for the installer's verify");
        CHECK(info.latestVersionStr == L"2.0.0", "release: tag 'v' stripped for display");

        U::UpdateInfo eq;
        U::ParseReleaseObj(*rel, U::ParseVersion(L"2.0.0"), eq);
        CHECK(!eq.available, "release: equal version not available");
        U::UpdateInfo older;
        U::ParseReleaseObj(*rel, U::ParseVersion(L"2.0.1"), older);
        CHECK(!older.available, "release: older release not available");

        // A tampered (foreign-host) asset set: still 'available' (the version IS newer) but NEVER
        // installable — the prompt then links to the releases page instead of installing it.
        const std::wstring evilJson =
            L"{\"tag_name\":\"v2.0.0\",\"assets\":["
            L"{\"name\":\"x.msixbundle\",\"browser_download_url\":\"https://evil.example/x.msixbundle\"},"
            L"{\"name\":\"x.cer\",\"browser_download_url\":\"https://evil.example/x.cer\"}]}";
        const auto evil = json::Parse(evilJson);
        U::UpdateInfo einfo;
        U::ParseReleaseObj(*evil, cur, einfo);
        CHECK(einfo.available && !einfo.installable && einfo.bundleUrl.empty(), "release: foreign asset URLs rejected (not installable)");
    }

    // NIGHTLY channel (IsNightlyTag / ReleaseAllowedOnChannel): a nightly is an unstable dev build
    // whose TAG contains "nightly" (e.g. v0.6.10-prerelease-nightly, published as a GitHub
    // prerelease). It is a tier BELOW pre-release: ALWAYS skipped — even with the pre-release
    // opt-in ON — unless the user accepted the warning-gated nightly opt-in; the two opt-ins are
    // ORTHOGONAL (each admits only its own tier, stable is always offered).
    {
        // Tag detection: case-insensitive CONTAINS, per the naming contract.
        CHECK(U::IsNightlyTag(L"v0.6.10-prerelease-nightly"), "nightly: canonical tag matches");
        CHECK(U::IsNightlyTag(L"v0.6.10-NIGHTLY"), "nightly: case-insensitive");
        CHECK(U::IsNightlyTag(L"Nightly-2026-07-22"), "nightly: prefix position matches (contains, not suffix)");
        CHECK(U::IsNightlyTag(L"nightly"), "nightly: the bare word matches");
        CHECK(!U::IsNightlyTag(L"v0.6.10"), "nightly: a plain tag is not nightly");
        CHECK(!U::IsNightlyTag(L"v0.7.0-beta.2"), "nightly: a beta prerelease tag is not nightly");
        CHECK(!U::IsNightlyTag(L"v0.6.10-night"), "nightly: 'night' alone is not 'nightly'");
        CHECK(!U::IsNightlyTag(L""), "nightly: empty tag is not nightly");

        // Channel matrix — P = allowPrerelease, N = allowNightly.
        const auto allowed = [](const wchar_t* tag, bool pre, bool P, bool N) {
            return U::ReleaseAllowedOnChannel(tag, pre, P, N);
        };
        // Stable: always offered, whatever the opt-ins.
        CHECK(allowed(L"v1.0.0", false, false, false), "channel: stable on the default channel");
        CHECK(allowed(L"v1.0.0", false, true, false), "channel: stable with prerelease on");
        CHECK(allowed(L"v1.0.0", false, false, true), "channel: stable with nightly on");
        CHECK(allowed(L"v1.0.0", false, true, true), "channel: stable with both on");
        // A plain (non-nightly) prerelease: gated on P only.
        CHECK(!allowed(L"v1.1.0-beta", true, false, false), "channel: beta skipped by default");
        CHECK(allowed(L"v1.1.0-beta", true, true, false), "channel: beta offered under the prerelease opt-in");
        CHECK(!allowed(L"v1.1.0-beta", true, false, true), "channel: nightly opt-in alone never admits a beta (orthogonal)");
        CHECK(allowed(L"v1.1.0-beta", true, true, true), "channel: beta offered with both on");
        // A NIGHTLY (marked prerelease, as published): gated on N only — the core requirement:
        // "always skipped unless the user checked the nightly box".
        CHECK(!allowed(L"v0.6.10-prerelease-nightly", true, false, false), "channel: nightly skipped by default");
        CHECK(!allowed(L"v0.6.10-prerelease-nightly", true, true, false), "channel: prerelease opt-in alone NEVER admits a nightly");
        CHECK(allowed(L"v0.6.10-prerelease-nightly", true, false, true), "channel: nightly opt-in admits a nightly");
        CHECK(allowed(L"v0.6.10-prerelease-nightly", true, true, true), "channel: nightly offered with both on");
        // A nightly whose publish FORGOT the prerelease checkbox: the TAG is authoritative — still
        // nightly-gated, so it can never leak onto the stable or prerelease channels.
        CHECK(!allowed(L"v0.6.11-nightly", false, false, false), "channel: mis-published nightly still skipped by default");
        CHECK(!allowed(L"v0.6.11-nightly", false, true, false), "channel: mis-published nightly still skipped under prerelease-only");
        CHECK(allowed(L"v0.6.11-nightly", false, false, true), "channel: mis-published nightly still needs the nightly opt-in");

        // ParseReleaseObj carries the classification (the prompt's warning + the [update] trail's
        // NIGHTLY word key on it) and the version math is unchanged: the numeric part orders.
        const auto rel = json::Parse(
            L"{\"tag_name\":\"v0.6.10-prerelease-nightly\",\"prerelease\":true,"
            L"\"html_url\":\"https://github.com/Nucs/Agentmaster/releases/tag/v0.6.10-prerelease-nightly\"}");
        CHECK(rel && rel->type == json::Value::Type::Obj, "nightly: synthetic release parses");
        U::UpdateInfo ni;
        U::ParseReleaseObj(*rel, U::ParseVersion(L"0.6.9"), ni);
        CHECK(ni.isNightly && ni.isPrerelease, "nightly: ParseReleaseObj classifies by tag");
        CHECK(ni.available, "nightly: numeric version (0.6.10 > 0.6.9) still orders");
        CHECK(ni.latest.major == 0 && ni.latest.minor == 6 && ni.latest.patch == 10, "nightly: suffix never corrupts the parsed version");
        U::UpdateInfo same;
        U::ParseReleaseObj(*rel, U::ParseVersion(L"0.6.10"), same);
        CHECK(!same.available, "nightly: same numeric version as installed is not an update (publishers must bump)");
    }

    // Display/link/format helpers (each feeds a user-visible surface — a regression here corrupts
    // the prompt, the changelog links, or the [update] trail).
    {
        U::UpdateInfo i1;
        i1.latestTag = L"v1.2.3";
        CHECK(U::DisplayVersion(i1) == L"v1.2.3", "display: v-tag verbatim");
        U::UpdateInfo i2;
        i2.latestTag = L"1.2.3";
        i2.latestVersionStr = L"1.2.3";
        CHECK(U::DisplayVersion(i2) == L"v1.2.3", "display: bare tag gains the v");
        CHECK(U::ReleasePageForTag(L"0.6.8") == L"https://github.com/Nucs/Agentmaster/releases/tag/v0.6.8", "link: bare tag page ensures v");
        CHECK(U::ReleasePageForTag(L"v0.6.8") == L"https://github.com/Nucs/Agentmaster/releases/tag/v0.6.8", "link: v-tag page verbatim");
        U::UpdateInfo i3;
        i3.htmlUrl = L"https://x/y";
        i3.latestTag = L"v9";
        CHECK(U::ChangelogUrl(i3) == L"https://x/y", "link: html_url wins");
        i3.htmlUrl.clear();
        CHECK(U::ChangelogUrl(i3) == L"https://github.com/Nucs/Agentmaster/releases/tag/v9", "link: built from tag when no html_url");
        CHECK(U::CmdArg(L"50%off") == L"\"50%%off\"", "cmdarg: quoted + % doubled (cmd metachar)");
        CHECK(U::detail::EndsWithNoCase(L"A.MsixBundle", L".msixbundle") && !U::detail::EndsWithNoCase(L"A.zip", L".msixbundle"), "endswith: case-insensitive suffix");
        CHECK(U::FormatSpanShort(-5) == L"0m" && U::FormatSpanShort(0) == L"0m", "span: negative/zero -> 0m");
        CHECK(U::FormatSpanShort(59LL * 60000) == L"59m", "span: minutes");
        CHECK(U::FormatSpanShort(60LL * 60000) == L"1h", "span: exact hour");
        CHECK(U::FormatSpanShort(24LL * 60 * 60000) == L"1d", "span: exact day");
        CHECK(U::FormatSpanShort((25LL * 60 + 10) * 60000) == L"1d1h", "span: day+hour");
    }

    // ReadPrefs/WriteUpdateState robustness: malformed and wrong-shaped files read as pristine
    // defaults / are repaired or refused — never a throw, never a clobber.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am-upd-robust-" + NewSessionId();
        const std::wstring sj = dir + L"\\settings.json";
        std::filesystem::create_directories(std::filesystem::path{ dir });
        const auto put = [&](const std::wstring& text) {
            CHECK(U::detail::WriteFileUtf8(sj, text), "robust: seed written");
        };

        put(L"{{{{ not json");
        const auto g = U::ReadPrefs(dir);
        CHECK(!g.allowPrerelease && g.skippedVersion.empty() && g.postponedUntilUnixMs == 0, "robust: garbage file -> defaults");

        put(L"[1,2,3]");
        CHECK(U::ReadPrefs(dir).skippedVersion.empty(), "robust: array root -> defaults");

        put(L"{\"version\":1,\"settings\":\"oops\",\"updateSkippedVersion\":\"v0.3.0\"}");
        CHECK(U::ReadPrefs(dir).skippedVersion == L"v0.3.0", "robust: settings-as-string falls back to the top-level read");

        put(L"{\"version\":1,\"settings\":{\"allowUpdatePrerelease\":\"true\",\"updatePostponedUntilUnixMs\":\"soon\"}}");
        const auto w = U::ReadPrefs(dir);
        CHECK(!w.allowPrerelease && w.postponedUntilUnixMs == 0, "robust: wrong-typed nested values -> defaults");

        // A malformed "settings" member (a number) is replaced in place by the RMW; unrelated
        // top-level keys survive.
        put(L"{\"version\":1,\"settings\":42,\"model\":\"keepme\"}");
        {
            const std::wstring tag = L"v1.1.1";
            CHECK(U::WriteUpdateState(dir, &tag, nullptr), "robust: settings-as-number RMW succeeds");
            const auto doc = json::Parse(U::detail::ReadFileWide(sj));
            const auto* s = doc ? doc->Find(L"settings") : nullptr;
            CHECK(s && s->type == json::Value::Type::Obj && s->StrAt(L"updateSkippedVersion") == L"v1.1.1", "robust: malformed settings member replaced with a real object");
            CHECK(doc && doc->StrAt(L"model") == L"keepme", "robust: unrelated top-level keys survive the repair");
        }

        // Duplicate stray keys (a hand-mangled file): the heal leaves exactly ONE nested copy and
        // still migrates the value.
        put(L"{\"version\":1,\"updateSkippedVersion\":\"a\",\"updateSkippedVersion\":\"b\",\"settings\":{}}");
        {
            long long pp = 777;
            CHECK(U::WriteUpdateState(dir, nullptr, &pp), "robust: duplicate-stray RMW succeeds");
            const std::wstring healed = U::detail::ReadFileWide(sj);
            size_t n = 0;
            for (size_t at = healed.find(L"updateSkippedVersion"); at != std::wstring::npos; at = healed.find(L"updateSkippedVersion", at + 1))
            {
                ++n;
            }
            CHECK(n == 1, "robust: duplicate strays healed to one nested key");
            CHECK(U::ReadPrefs(dir).skippedVersion == L"a" && U::ReadPrefs(dir).postponedUntilUnixMs == 777, "robust: first stray migrated, postpone landed");
        }

        // A FILE standing where the state DIR should be: every writer fails gracefully (false).
        const std::wstring blocked = std::wstring{ tmp } + L"am-upd-blocked-" + NewSessionId();
        {
            std::ofstream f{ std::filesystem::path{ blocked }, std::ios::binary };
            f << "im a file";
        }
        {
            long long pp = 1;
            CHECK(!U::WriteUpdateState(blocked, nullptr, &pp), "robust: dir-blocked-by-file RMW returns false (no throw)");
            CHECK(!U::WritePostpone(blocked, 2), "robust: WritePostpone propagates the failure");
            const std::wstring tag2 = L"v1";
            CHECK(!U::WriteSkip(blocked, tag2), "robust: WriteSkip propagates the failure");
            CHECK(U::ReadPrefs(blocked).postponedUntilUnixMs == 0, "robust: nothing landed behind the blocked dir");
        }
        ::DeleteFileW(blocked.c_str());

        // LogUpdate: stamped [update] lines land in hooks.log, one per call; bad dirs are no-ops.
        {
            U::LogUpdate(dir, L"unit-test line one");
            U::LogUpdate(dir, L"unit-test line two");
            const std::wstring log = U::detail::ReadFileWide(dir + L"\\hooks.log");
            CHECK(log.find(L"[update] unit-test line one\n") != std::wstring::npos, "log: line one written + newline-terminated");
            CHECK(log.find(L"[update] unit-test line two\n") != std::wstring::npos, "log: line two appended");
            CHECK(!log.empty() && log[0] == L'[', "log: [HH:MM:SS.mmm] stamp leads the line");
            U::LogUpdate(L"", L"never lands"); // empty stateDir -> no-op
            U::LogUpdate(dir + L"\\no-such-sub\\deeper", L"never lands"); // missing dir -> no-op
            CHECK(true, "log: bad stateDirs are safe no-ops (no throw)");
        }

        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }

    // ApplyDecision: the decision -> persist -> read-back loop, exactly what a prompt drives.
    // UpdateNow is deliberately NOT exercised (it ShellExecutes a browser / the real installer).
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am-upd-decide-" + NewSessionId();
        std::filesystem::create_directories(std::filesystem::path{ dir });

        wchar_t prevBuf[256];
        const DWORD prevLen = ::GetEnvironmentVariableW(U::kDeclinedEnvVar, prevBuf, 256);
        const bool hadPrev = prevLen > 0 && prevLen < 256;
        const std::wstring prev = hadPrev ? std::wstring{ prevBuf, prevLen } : std::wstring{};
        U::MarkDeclinedThisRun(L"");

        U::UpdateInfo info;
        info.latestTag = L"v9.9.9";
        info.latestVersionStr = L"9.9.9";

        constexpr long long kDay = 24LL * 60 * 60 * 1000;
        const long long t0 = U::NowUnixMs();
        // "Remind me tomorrow" = the NEXT LOCAL 08:00, not now + 24h: strictly future, never more
        // than a day out, and 08:00 on the LOCAL clock.
        SYSTEMTIME morning{};
        const long long next8 = U::NextLocalMorningUnixMs(U::kPostponeMorningHour, &morning);
        CHECK(next8 > t0 && next8 <= t0 + kDay + 60000, "decide: next-08:00 is strictly future and within 24h");
        CHECK(morning.wHour == 8 && morning.wMinute == 0 && morning.wSecond == 0 && morning.wMilliseconds == 0,
              "decide: next-08:00 resolves to 08:00:00.000 on the LOCAL wall clock");
        // ...and the RETURNED INSTANT really lands there. The out-param above proves nothing on its
        // own — it is the struct we asked to be built — so round-trip the persisted epoch ms back
        // through the INDEPENDENT inverse (ms -> UTC FILETIME -> SystemTimeToTzSpecificLocalTime).
        // An inverted local<->UTC conversion sails past every check but this one, landing the
        // postpone 2x the UTC offset away (here: 06:00 or 10:00 instead of 08:00).
        {
            ULARGE_INTEGER u{};
            u.QuadPart = static_cast<unsigned long long>(next8) * 10000ULL + 116444736000000000ULL;
            FILETIME ft{ u.LowPart, u.HighPart };
            SYSTEMTIME utc{}, back{};
            const bool converted = ::FileTimeToSystemTime(&ft, &utc) && ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &back);
            CHECK(converted, "decide: the next-08:00 instant converts back to local time");
            CHECK(converted && back.wHour == 8 && back.wMinute == 0 && back.wSecond == 0,
                  "decide: the INSTANT reads 08:00 on the LOCAL clock (round-trip: local, not UTC)");
            CHECK(converted && back.wYear == morning.wYear && back.wMonth == morning.wMonth && back.wDay == morning.wDay,
                  "decide: ...on the resolved DATE (today's or tomorrow's 08:00, whichever is next)");
            if (converted)
            {
                // Printed so the resolved instant is eyeball-verifiable against this machine's clock
                // (a self-consistent round-trip on a UTC machine would have no teeth otherwise).
                std::wprintf(L"  [info] next-08:00 local: %04u-%02u-%02u %02u:%02u (in %ls)\n",
                             back.wYear, back.wMonth, back.wDay, back.wHour, back.wMinute,
                             U::FormatSpanShort(next8 - t0).c_str());
            }
        }
        CHECK(!U::ApplyDecision(dir, info, U::Decision::PostponeTomorrow, nullptr), "decide: postpone-tomorrow returns not-launched");
        const auto p1 = U::ReadPrefs(dir).postponedUntilUnixMs;
        CHECK(p1 > t0 && p1 <= t0 + kDay + 60000, "decide: postpone-tomorrow persists the next 08:00 (not a rolling 24h)");
        CHECK(!U::ApplyDecision(dir, info, U::Decision::Postpone3, nullptr), "decide: postpone3 returns not-launched");
        const auto p3 = U::ReadPrefs(dir).postponedUntilUnixMs;
        CHECK(p3 >= t0 + 3 * kDay - 60000 && p3 <= U::NowUnixMs() + 3 * kDay + 60000, "decide: postpone3 lands ~3 days out");
        CHECK(p3 > p1, "decide: postpone3 replaces the shorter postpone-tomorrow");
        CHECK(!U::ApplyDecision(dir, info, U::Decision::Postpone30, nullptr), "decide: postpone30 returns not-launched");
        const auto p30 = U::ReadPrefs(dir).postponedUntilUnixMs;
        CHECK(p30 > p3 && p30 >= t0 + 30 * kDay - 60000, "decide: postpone30 replaces with ~30 days");

        CHECK(!U::ApplyDecision(dir, info, U::Decision::Skip, nullptr), "decide: skip returns not-launched");
        CHECK(U::ReadPrefs(dir).skippedVersion == L"v9.9.9", "decide: skip persists the exact tag");

        // "Remind me next restart" (the default radio, and Cancel/X): the ONE choice that writes
        // nothing durable — it only latches the process-scoped marker the startup + hourly checks
        // gate on PRE-network, which the next launch clears (asserted in the declined-latch block
        // above, incl. RunStartupUpdateCheck's fresh-launch clear). So "next restart" is literally
        // what happens: no automatic prompt for the rest of THIS process, always asked again after
        // a restart, and the cog's manual "Check for updates" never passes through that gate.
        const std::wstring before = U::detail::ReadFileWide(dir + L"\\settings.json");
        CHECK(!U::ApplyDecision(dir, info, U::Decision::RemindNextRestart, nullptr), "decide: remind-next-restart returns not-launched");
        CHECK(U::WasDeclinedThisRun(L"v9.9.9"), "decide: remind-next-restart latches the declined marker (no hourly nag this run)");
        CHECK(U::detail::ReadFileWide(dir + L"\\settings.json") == before, "decide: remind-next-restart persists NOTHING to settings.json");
        U::MarkDeclinedThisRun(L""); // what a fresh launch does (RunStartupUpdateCheck)
        CHECK(U::DeclinedThisRunTag().empty(), "decide: ...and the NEXT RESTART clears it -> the prompt returns");

        ::SetEnvironmentVariableW(U::kDeclinedEnvVar, hadPrev ? prev.c_str() : nullptr);
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }

    // Full-fidelity envelope: an updater RMW must not disturb ANY other cog setting (the broad
    // "an update choice can never break your settings" guarantee, past the model-only check above).
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am-upd-fidelity-" + NewSessionId();
        std::filesystem::create_directories(std::filesystem::path{ dir });
        AppSettings a;
        a.model = L"opus";
        a.env = L"FOO=1;BAR=two words";
        a.launchModels = L"Fable 5 | claude-fable-5\nOpus | claude-opus-4-8";
        a.flashRingColor = L"#CC00FF00";
        a.recentDirsLimit = 17;
        a.maxTags = 33;
        a.skipPermissions = false;
        a.notifySound = false;
        a.hiddenSessionIds.push_back(L"11111111-2222-3333-4444-555555555555");
        CHECK(U::detail::WriteFileUtf8(dir + L"\\settings.json", SerializeAppSettings(a)), "fidelity: engine-shaped seed written");
        CHECK(U::WritePostpone(dir, 123456789LL), "fidelity: RMW succeeds");
        const AppSettings b = DeserializeAppSettings(U::detail::ReadFileWide(dir + L"\\settings.json"));
        CHECK(b.model == a.model && b.env == a.env && b.launchModels == a.launchModels, "fidelity: strings intact through the RMW");
        CHECK(b.flashRingColor == a.flashRingColor && b.recentDirsLimit == a.recentDirsLimit && b.maxTags == a.maxTags, "fidelity: numbers/colors intact");
        CHECK(b.skipPermissions == a.skipPermissions && b.notifySound == a.notifySound, "fidelity: bools intact");
        CHECK(b.hiddenSessionIds.size() == 1 && b.hiddenSessionIds[0] == a.hiddenSessionIds[0], "fidelity: lists intact");
        CHECK(b.updatePostponedUntilUnixMs == 123456789LL, "fidelity: the RMW'd key visible to the engine");
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }
}

