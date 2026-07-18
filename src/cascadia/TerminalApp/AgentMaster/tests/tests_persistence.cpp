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
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration / updater version+prefs
// ★ tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color / engine window lifecycle
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: persistence tests. Shared CHECK/fixtures/decls
// live in m5_tests.h; the runner (m5_tests.cpp) calls each entry point. See run-m5-tests.bat.
#include "m5_tests.h"

#include "../Engine.h" // the window-lifecycle `...In` seams (TestEngineWindowLifecycle)

void TestPersistence()
{
    std::wprintf(L"Persistence (JSON + sessions + templates):\n");

    // JSON round-trip of a small document.
    {
        auto o = json::Value::MkObj();
        o.Set(L"a", json::Value::MkStr(L"he\"llo\n"));
        o.Set(L"n", json::Value::MkNum(42));
        o.Set(L"b", json::Value::MkBool(true));
        auto arr = json::Value::MkArr();
        arr.Push(json::Value::MkNum(1));
        arr.Push(json::Value::MkStr(L"two"));
        o.Set(L"arr", std::move(arr));
        const auto text = json::Dump(o);
        const auto rt = json::Parse(text);
        CHECK(rt.has_value(), "json parses its own dump");
        CHECK(rt && rt->StrAt(L"a") == L"he\"llo\n", "json string escape round-trip");
        CHECK(rt && rt->I64At(L"n") == 42, "json number round-trip");
        CHECK(rt && rt->BoolAt(L"b") == true, "json bool round-trip");
        CHECK(rt && rt->Find(L"arr") && rt->Find(L"arr")->arr.size() == 2, "json array round-trip");
    }

    // enum round-trips.
    CHECK(SessionStateFromString(ToString(SessionState::NeedsApproval)) == SessionState::NeedsApproval, "state enum round-trip");
    CHECK(AutorunnerModeFromString(ToString(AutorunnerMode::SemiAuto)) == AutorunnerMode::SemiAuto, "mode enum round-trip");
    CHECK(PromptStatusFromString(ToString(PromptStatus::Held)) == PromptStatus::Held, "status enum round-trip");
    CHECK(PromptGateFromString(ToString(PromptGate::Manual)) == PromptGate::Manual, "gate enum round-trip");

    // Session round-trip (queue + autorunner preserved, incl. Sent status — no replay).
    {
        SessionInfo s;
        s.id = L"sid-1";
        s.title = L"My Task";
        s.workingDir = L"K:/api";
        s.state = SessionState::WaitingForInput;
        s.lastActivityUnixMs = 123456789;
        s.lastMessageWasQuestion = true; // PERSISTED (Rule #16): the question-guard must survive a crash so a queued prompt can't auto-answer it on reopen
        s.external = true; // adopted session: must survive the round-trip
        s.forkParentId = L"src-conv-7"; // a never-messaged fork remembers its source across restart (PERSISTED)
        s.tabColorHex = L"#61AFEF"; // tab color modes (Individual): the session's own color survives close/restore (PERSISTED)
        s.inferredWorkingDir = L"K:/api/src/deep"; // tab color modes (Inferred): the inferred-workdir cache survives restart (PERSISTED)
        QueuedPrompt a;
        a.id = L"p1";
        a.label = L"add tests";
        a.text = L"please add unit tests";
        a.status = PromptStatus::Sent;
        a.sentAtUnixMs = 999;
        a.origin = PromptOrigin::Typed; // a directly-typed message must survive the round-trip
        QueuedPrompt b;
        b.id = L"p2";
        b.label = L"commit";
        b.text = L"commit it";
        b.gate = PromptGate::Manual;
        b.guardPattern = L"answers-a-question:ok";
        s.queue = { a, b };
        s.autorunner.mode = AutorunnerMode::Full;
        s.autorunner.throttleMs = 750;
        s.autorunner.stopOnError = false;
        s.autorunner.maxAutoSends = 7;
        s.autorunner.approval.pauseForHuman = false;
        s.autorunner.approval.autoApproveTools = { L"Read", L"Bash(git *)" };

        const auto text = SerializeSessions({ s });
        const auto back = DeserializeSessions(text);
        CHECK(back.size() == 1, "sessions round-trip count");
        if (back.size() == 1)
        {
            const auto& r = back[0];
            CHECK(r.id == L"sid-1" && r.title == L"My Task" && r.workingDir == L"K:/api", "session metadata");
            CHECK(r.state == SessionState::WaitingForInput, "session state");
            CHECK(r.lastMessageWasQuestion, "question-guard flag preserved (PERSISTED: a crash mustn't drop the guard and auto-answer a pending question)");
            CHECK(r.external, "external flag preserved");
            CHECK(r.forkParentId == L"src-conv-7", "forkParentId preserved (PERSISTED: restores a never-messaged fork)");
            CHECK(r.tabColorHex == L"#61AFEF", "tabColorHex preserved (PERSISTED: an Individual-mode session keeps ITS color across restore)");
            CHECK(r.inferredWorkingDir == L"K:/api/src/deep", "inferredWorkingDir preserved (PERSISTED: a reopened session wears its inferred color immediately)");
            CHECK(r.queue.size() == 2, "queue size");
            CHECK(r.queue.size() == 2 && r.queue[0].status == PromptStatus::Sent && r.queue[0].sentAtUnixMs == 999, "Sent status preserved (no replay)");
            CHECK(r.queue.size() == 2 && r.queue[0].origin == PromptOrigin::Typed && r.queue[1].origin == PromptOrigin::Autorun, "prompt origin preserved (Typed vs Flight)");
            CHECK(r.queue.size() == 2 && r.queue[1].gate == PromptGate::Manual && r.queue[1].guardPattern == L"answers-a-question:ok", "prompt gate+guard preserved");
            CHECK(r.autorunner.mode == AutorunnerMode::Full && r.autorunner.throttleMs == 750 && !r.autorunner.stopOnError && r.autorunner.maxAutoSends == 7, "autorunner preserved");
            CHECK(!r.autorunner.approval.pauseForHuman && r.autorunner.approval.autoApproveTools.size() == 2, "approval policy preserved");
        }
    }

    // Codex managed session: the agent kind + the rollout resume target (codexSessionId) round-trip,
    // and a default (Claude) session carries NEITHER key, so an all-Claude sessions.json is unchanged.
    {
        SessionInfo cx;
        cx.id = L"codex-handle-1"; // OUR durable handle (the registry / persistence / tab-map key)
        cx.title = L"proxmox homelab";
        cx.workingDir = L"K:/source/proxmox";
        cx.kind = AgentKind::Codex;
        cx.codexSessionId = L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a"; // the codex resume uuid
        SessionInfo claudeDefault;
        claudeDefault.id = L"claude-1";
        const auto text = SerializeSessions({ cx, claudeDefault });
        const auto back = DeserializeSessions(text);
        CHECK(back.size() == 2, "codex+claude sessions round-trip count");
        if (back.size() == 2)
        {
            CHECK(back[0].kind == AgentKind::Codex, "codex session kind preserved");
            CHECK(back[0].codexSessionId == L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex resume uuid (codexSessionId) preserved");
            CHECK(back[1].kind == AgentKind::Claude && back[1].codexSessionId.empty(), "default session is Claude with no codex uuid");
            CHECK(back[0].forkParentId.empty() && back[1].forkParentId.empty(), "a non-fork session carries no forkParentId (key omitted when empty)");
            CHECK(back[0].tabColorHex.empty() && back[1].tabColorHex.empty() && back[0].inferredWorkingDir.empty() && back[1].inferredWorkingDir.empty(),
                  "default sessions carry no tab-color-mode fields");
        }
        // Tab color modes: both new keys are OMITTED when empty, so a pre-feature sessions.json is
        // byte-unchanged (the forkParentId/codexSessionId omission contract).
        CHECK(text.find(L"tabColorHex") == std::wstring::npos && text.find(L"inferredWorkingDir") == std::wstring::npos,
              "empty tabColorHex/inferredWorkingDir keys omitted from the serialized document");
    }

    // Templates: capture-from-queue resets ids/status; apply assigns fresh ids + Pending.
    {
        std::vector<QueuedPrompt> queue;
        QueuedPrompt a;
        a.id = L"x";
        a.label = L"impl";
        a.text = L"implement";
        a.status = PromptStatus::Sent; // should NOT carry into the template/apply
        queue.push_back(a);

        const auto tmpl = MakeTemplateFromQueue(L"impl-test-fix", queue);
        CHECK(tmpl.name == L"impl-test-fix" && tmpl.prompts.size() == 1, "template captured");
        CHECK(tmpl.prompts.size() == 1 && tmpl.prompts[0].id.empty(), "template prompt id reset");

        const auto ttext = SerializeTemplates({ tmpl });
        const auto tback = DeserializeTemplates(ttext);
        CHECK(tback.size() == 1 && tback[0].name == L"impl-test-fix", "templates round-trip");

        std::vector<QueuedPrompt> target;
        AppendTemplateToQueue(target, tmpl);
        CHECK(target.size() == 1, "apply appends");
        CHECK(target.size() == 1 && !target[0].id.empty() && target[0].status == PromptStatus::Pending, "apply: fresh id + Pending");
        AppendTemplateToQueue(target, tmpl);
        CHECK(target.size() == 2 && target[0].id != target[1].id, "apply twice: distinct ids");
    }
}

void TestManagerLayout()
{
    std::wprintf(L"Manager layout (splitter geometry persistence):\n");
    auto approx = [](double a, double b) { return (a > b ? a - b : b - a) < 1e-9; };

    // Round-trip of in-band fractions.
    {
        ManagerLayout in;
        in.boardFraction = 0.55;
        in.treeFraction = 0.62;
        const auto out = DeserializeLayout(SerializeLayout(in));
        CHECK(approx(out.boardFraction, 0.55), "layout boardFraction round-trip");
        CHECK(approx(out.treeFraction, 0.62), "layout treeFraction round-trip");
    }

    // Empty / unparseable text -> defaults (no throw, no zero that would vanish a pane).
    {
        const auto out = DeserializeLayout(L"");
        CHECK(approx(out.boardFraction, 0.4) && approx(out.treeFraction, 0.4), "layout defaults on empty");
        const auto out2 = DeserializeLayout(L"not json at all");
        CHECK(approx(out2.boardFraction, 0.4) && approx(out2.treeFraction, 0.4), "layout defaults on garbage");
    }

    // Out-of-band values are rejected back to the default (a corrupt file can't collapse a pane).
    {
        const auto out = DeserializeLayout(L"{\"boardFraction\":0.99,\"treeFraction\":0.0}");
        CHECK(approx(out.boardFraction, 0.4), "layout rejects too-large board -> default");
        CHECK(approx(out.treeFraction, 0.4), "layout rejects zero tree -> default");
        const auto out2 = DeserializeLayout(L"{\"boardFraction\":-3,\"treeFraction\":0.5}");
        CHECK(approx(out2.boardFraction, 0.4), "layout rejects negative board -> default");
        CHECK(approx(out2.treeFraction, 0.5), "layout keeps in-band tree alongside a bad board");
    }

    // A missing field keeps its default; a present in-band field is honored.
    {
        const auto out = DeserializeLayout(L"{\"treeFraction\":0.3}");
        CHECK(approx(out.boardFraction, 0.4), "layout missing board -> default");
        CHECK(approx(out.treeFraction, 0.3), "layout present tree honored");
    }
}

// Agentmaster M10: the per-window WindowRecord — per-window UI state (geometry + lens + ORDERED
// tab refs) layered OVER the session-archive model (sessions.json stays the session source of
// truth, so a Claude tab here is just a sessionId reference, never a copy). Verify the whole
// tree round-trips: geometry (incl. a negative coordinate from a 2nd monitor), ordered Claude
// (ref) + Other (opaque actionsJson) tabs, and the Manager lens. Plus the one-file-per-window
// disk path (Save/Load/Delete), self-cleaning.
void TestWindowRecord()
{
    std::wprintf(L"Window record (per-window workspace persistence, M10):\n");
    auto approx = [](double a, double b) { return (a > b ? a - b : b - a) < 1e-9; };

    WindowRecord in;
    in.windowId = L"win-guid-1";
    in.geometry.hasPosition = true;
    in.geometry.x = 120;
    in.geometry.y = -40; // a real negative coordinate (2nd monitor) must survive
    in.geometry.hasSize = true;
    in.geometry.width = 1280;
    in.geometry.height = 800;
    in.geometry.launchMode = L"maximized";

    // A Claude tab is a REFERENCE — just the session id (its full record lives in sessions.json).
    TabEntry claude;
    claude.kind = TabKind::Claude;
    claude.tabColor = L"#FFD700";
    claude.sessionId = L"conv-abc";
    in.tabs.push_back(claude);

    // An Other tab is opaque: actionsJson is WT ActionAndArgs — itself JSON, so this also tests
    // nested-JSON-as-a-string escaping survives the round-trip.
    TabEntry other;
    other.kind = TabKind::Other;
    other.actionsJson = L"[{\"action\":\"newTab\",\"profile\":\"pwsh\"}]";
    in.tabs.push_back(other);

    // A Codex tab is a REFERENCE like Claude (the resume uuid lives on the referenced SessionInfo).
    TabEntry codex;
    codex.kind = TabKind::Codex;
    codex.sessionId = L"codex-handle-1";
    in.tabs.push_back(codex);

    // Selected tab persisted by stable identity (Claude conversation id preferred; index is the shell
    // fallback). Both set to non-default values so a dropped field FAILS the round-trip (a -1 default
    // would otherwise mask a missing index).
    in.selectedSessionId = L"conv-abc";
    in.selectedTabIndex = 1;
    in.managerTabColor = L"#3A6EA5"; // the pinned Manager tab's per-window color (non-default so a dropped field fails the round-trip)

    in.manager.selectedId = L"conv-abc";
    in.manager.scopeDir = L"K:/api";
    in.manager.selectedPromptId = L"q1";
    in.manager.collapsedDirs.push_back(L"K:/old");
    in.manager.layout.boardFraction = 0.5;
    in.manager.layout.treeFraction = 0.45;
    in.manager.treeScope = 2; // EXTERNAL — non-default so a dropped field fails the round-trip

    const auto out = DeserializeWindowRecord(SerializeWindowRecord(in));

    CHECK(out.windowId == L"win-guid-1", "record windowId round-trip");
    CHECK(out.geometry.hasPosition && approx(out.geometry.x, 120) && approx(out.geometry.y, -40), "geometry position (negative y) round-trip");
    CHECK(out.geometry.hasSize && approx(out.geometry.width, 1280) && approx(out.geometry.height, 800), "geometry size round-trip");
    CHECK(out.geometry.launchMode == L"maximized", "geometry launchMode round-trip");

    CHECK(out.tabs.size() == 3, "tab count + order preserved");
    if (out.tabs.size() == 3)
    {
        CHECK(out.tabs[0].kind == TabKind::Claude, "tab[0] is Claude");
        CHECK(out.tabs[0].tabColor == L"#FFD700", "Claude tab color round-trip");
        CHECK(out.tabs[0].sessionId == L"conv-abc", "Claude tab sessionId reference round-trip");
        CHECK(out.tabs[1].kind == TabKind::Other, "tab[1] is Other");
        CHECK(out.tabs[1].actionsJson == L"[{\"action\":\"newTab\",\"profile\":\"pwsh\"}]", "Other tab actionsJson (nested JSON) round-trip");
        CHECK(out.tabs[2].kind == TabKind::Codex, "tab[2] is Codex");
        CHECK(out.tabs[2].sessionId == L"codex-handle-1", "Codex tab sessionId reference round-trip");
    }

    CHECK(out.selectedSessionId == L"conv-abc", "selected tab persisted by stable Claude id (round-trip)");
    CHECK(out.selectedTabIndex == 1, "selectedTabIndex shell fallback (round-trip)");
    CHECK(out.managerTabColor == L"#3A6EA5", "managerTabColor (Manager tab's per-window color) round-trip");

    CHECK(out.manager.selectedId == L"conv-abc", "lens selectedId round-trip");
    CHECK(out.manager.scopeDir == L"K:/api", "lens scopeDir round-trip");
    CHECK(out.manager.selectedPromptId == L"q1", "lens selectedPromptId round-trip");
    CHECK(out.manager.collapsedDirs.size() == 1 && out.manager.collapsedDirs[0] == L"K:/old", "lens collapsedDirs round-trip");
    CHECK(approx(out.manager.layout.boardFraction, 0.5) && approx(out.manager.layout.treeFraction, 0.45), "lens splitter fractions round-trip");
    CHECK(out.manager.treeScope == 2, "lens treeScope (shared tree/board scope) round-trip");

    // Tolerant of a missing / corrupt document.
    {
        const auto empty = DeserializeWindowRecord(L"");
        CHECK(empty.windowId.empty() && empty.tabs.empty(), "empty text -> empty record (no throw)");
        const auto garbage = DeserializeWindowRecord(L"}{not json");
        CHECK(garbage.windowId.empty(), "garbage text -> empty record (no throw)");
        // An older record (no treeScope key) and an out-of-range value both land on LOCAL (0).
        const auto legacy = DeserializeWindowRecord(L"{\"windowId\":\"w\",\"manager\":{\"selectedId\":\"s\"}}");
        CHECK(legacy.manager.treeScope == 0, "missing treeScope -> LOCAL (older record)");
        CHECK(legacy.managerTabColor.empty(), "missing managerTabColor -> empty (older record, no Manager-tab color)");
        const auto outOfRange = DeserializeWindowRecord(L"{\"windowId\":\"w\",\"manager\":{\"treeScope\":7}}");
        CHECK(outOfRange.manager.treeScope == 0, "out-of-range treeScope clamps to LOCAL");
    }

    // Disk round-trip (one file per window under .agentmaster\windows\). Uses a sentinel id and
    // cleans up, so it neither collides with nor leaves residue among real window records.
    {
        WindowRecord rec;
        rec.windowId = L"__m5test_window__";
        rec.geometry.hasSize = true;
        rec.geometry.width = 640;
        rec.tabs.push_back(TabEntry{}); // one default (Claude) tab
        SaveWindowRecord(rec);
        bool found = false;
        for (const auto& w : LoadWindowRecords())
        {
            if (w.windowId == L"__m5test_window__")
            {
                found = true;
                CHECK(w.geometry.hasSize && approx(w.geometry.width, 640), "disk record geometry survives Save/Load");
                CHECK(w.tabs.size() == 1, "disk record tabs survive Save/Load");
            }
        }
        CHECK(found, "SaveWindowRecord then LoadWindowRecords finds it");
        DeleteWindowRecord(L"__m5test_window__");
        bool stillThere = false;
        for (const auto& w : LoadWindowRecords())
        {
            stillThere = stillThere || (w.windowId == L"__m5test_window__");
        }
        CHECK(!stillThere, "DeleteWindowRecord prunes the file");
    }
}

void TestAppSettings()
{
    std::wprintf(L"App settings (the Settings cog):\n");

    // Round-trip every field with non-default values.
    {
        AppSettings in;
        in.skipPermissions = false;
        in.model = L"opus";
        in.launchModels = L"Opus 4.6 | claude-opus-4-6\nMine | my-model"; // non-default (default = the shipped 3-model kDefaultLaunchModels list) — launch-model picker
        in.includeCoAuthoredBy = false;
        in.defaultAutorunnerMode = AutorunnerMode::Full;
        in.maxAutoSends = 7;
        in.stopOnError = false;
        in.pauseOnHumanInput = false;
        in.confirmBeforeKill = false;
        in.tabRenameCommitMode = TabRenameCommitMode::ClickAwayOrEnter; // non-default (default is ClickAwayOrShiftEnter)
        in.favoriteIcon = FavoriteIcon::Star; // non-default (default is Crown)
        in.tabColorMode = TabColorMode::Individual; // non-default (default is WorkingDirectory) — tab color modes
        in.inferGitRoot = false; // non-default (default true) — "Use .git folder to infer" OFF
        in.flashRingColor = L"#8000FF00"; // non-default (default #CCFF0000) — 50%-opaque green flash ring (alpha byte = opacity)
        in.pendingDotsLightColor = L"#FF112233"; // non-default (default #FFE0A92B) — pending "3 dots" shown on a DARK tab/card bg
        in.pendingDotsDarkColor = L"#FF445566"; // non-default (default #FF5A3E00) — pending "3 dots" shown on a LIGHT tab/card bg
        in.defaultLaunchDir = L"K:/work";
        in.env = L"FOO=bar;BAZ=qux";
        in.archiveSplitFraction = 0.33;
        in.summaryPanelWidthFraction = 0.4; // in-band (0.08..0.5)
        in.summaryPanelHeightFraction = 0.6; // in-band (0.06..0.75)
        in.summaryPanelWrapNewlines = true; // non-default (default false = the literal-\n look)
        in.summaryPanelTruncate = false; // non-default (default true = truncate long messages)
        in.showTabCloseButton = false; // non-default (default true = show the X / theme-driven)
        in.closeTabOnMiddleClick = false; // non-default (default true = middle-click closes a tab)
        in.showTabIcon = true; // non-default (default false = tab icons HIDDEN)
        in.waitingForYouTimeoutMinutes = 0; // 0 = never decay — MUST round-trip as 0, not fall back to the default
        in.serverCacheMinutes = 17; // non-default (default 5) — the ⚡ "still cached" window
        in.treeSort = ExplorerSort::ByPid; // non-default (default Newest) — Explorer Tree sort
        in.boardSort = ExplorerSort::Newest; // non-default (default MostActive) — Triage Board sort
        in.autoTestingShowsSummary = false; // non-default (default true = Summary) — Manager Auto-Testing pane tab
        // System notifications (the cog's "Notifications" tab): every switch defaults ON, so store OFF.
        in.notificationsEnabled = false;
        in.notifyOnWaiting = false;
        in.notifyOnNeedsApproval = false;
        in.notifyOnIdle = false;
        in.notifyOnDone = false;
        in.notifyOnError = false;
        in.notifySuppressFocused = false;
        in.notifySound = false;
        in.hiddenSessionIds = { L"11111111-1111-1111-1111-111111111111", L"22222222-2222-2222-2222-222222222222" };
        const auto out = DeserializeAppSettings(SerializeAppSettings(in));
        CHECK(out.skipPermissions == false, "settings skipPermissions round-trip");
        CHECK(out.env == L"FOO=bar;BAZ=qux", "settings env round-trip");
        CHECK(out.model == L"opus", "settings model round-trip");
        CHECK(out.launchModels == L"Opus 4.6 | claude-opus-4-6\nMine | my-model", "settings launchModels round-trip (verbatim)");
        CHECK(out.includeCoAuthoredBy == false, "settings includeCoAuthoredBy round-trip");
        CHECK(out.defaultAutorunnerMode == AutorunnerMode::Full, "settings defaultAutorunnerMode round-trip");
        CHECK(out.maxAutoSends == 7u, "settings maxAutoSends round-trip");
        CHECK(out.stopOnError == false, "settings stopOnError round-trip");
        CHECK(out.pauseOnHumanInput == false, "settings pauseOnHumanInput round-trip");
        CHECK(out.confirmBeforeKill == false, "settings confirmBeforeKill round-trip");
        CHECK(out.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter, "settings tabRenameCommitMode round-trip");
        CHECK(out.favoriteIcon == FavoriteIcon::Star, "settings favoriteIcon round-trip");
        CHECK(out.tabColorMode == TabColorMode::Individual, "settings tabColorMode round-trip");
        CHECK(out.inferGitRoot == false, "settings inferGitRoot round-trip (stored OFF)");
        CHECK(out.flashRingColor == L"#8000FF00", "settings flashRingColor round-trip");
        CHECK(out.pendingDotsLightColor == L"#FF112233", "settings pendingDotsLightColor round-trip");
        CHECK(out.pendingDotsDarkColor == L"#FF445566", "settings pendingDotsDarkColor round-trip");
        CHECK(out.defaultLaunchDir == L"K:/work", "settings defaultLaunchDir round-trip");
        CHECK(out.archiveSplitFraction > 0.329 && out.archiveSplitFraction < 0.331, "settings archiveSplitFraction round-trip");
        CHECK(out.summaryPanelWidthFraction > 0.399 && out.summaryPanelWidthFraction < 0.401, "settings summaryPanelWidthFraction round-trip");
        CHECK(out.summaryPanelHeightFraction > 0.599 && out.summaryPanelHeightFraction < 0.601, "settings summaryPanelHeightFraction round-trip");
        CHECK(out.summaryPanelWrapNewlines == true, "settings summaryPanelWrapNewlines round-trip");
        CHECK(out.summaryPanelTruncate == false, "settings summaryPanelTruncate round-trip");
        CHECK(out.showTabCloseButton == false, "settings showTabCloseButton round-trip");
        CHECK(out.closeTabOnMiddleClick == false, "settings closeTabOnMiddleClick round-trip");
        CHECK(out.showTabIcon == true, "settings showTabIcon round-trip");
        CHECK(out.waitingForYouTimeoutMinutes == 0u, "settings waitingForYouTimeoutMinutes stored 0 (= never) round-trips as 0");
        CHECK(out.serverCacheMinutes == 17u, "settings serverCacheMinutes round-trip");
        CHECK(out.treeSort == ExplorerSort::ByPid, "settings treeSort round-trip");
        CHECK(out.boardSort == ExplorerSort::Newest, "settings boardSort round-trip");
        CHECK(out.autoTestingShowsSummary == false, "settings autoTestingShowsSummary round-trip");
        CHECK(out.notificationsEnabled == false, "settings notificationsEnabled round-trip (stored OFF)");
        CHECK(out.notifyOnWaiting == false && out.notifyOnNeedsApproval == false && out.notifyOnIdle == false &&
                  out.notifyOnDone == false && out.notifyOnError == false,
              "settings notifyOn* target-state switches round-trip (all stored OFF)");
        CHECK(out.notifySuppressFocused == false, "settings notifySuppressFocused round-trip (stored OFF)");
        CHECK(out.notifySound == false, "settings notifySound round-trip (stored OFF)");
        CHECK(out.hiddenSessionIds.size() == 2 &&
                  out.hiddenSessionIds[0] == L"11111111-1111-1111-1111-111111111111" &&
                  out.hiddenSessionIds[1] == L"22222222-2222-2222-2222-222222222222",
              "settings hiddenSessionIds round-trip (order preserved)");
    }

    // Agentmaster (launch-model picker): launchModels gates its default on key PRESENCE — an
    // ABSENT key (a pre-picker settings.json) seeds the shipped kDefaultLaunchModels, while a
    // PRESENT empty string is a deliberate "no models" (the user cleared the cog box) and stays "".
    // (The on-disk shape is the {"version":1,"settings":{…}} wrapper SerializeAppSettings writes.)
    {
        const auto absent = DeserializeAppSettings(L"{\"version\":1,\"settings\":{\"model\":\"opus\"}}");
        CHECK(absent.model == L"opus" && absent.launchModels == std::wstring{ kDefaultLaunchModels }, "settings launchModels ABSENT key -> the shipped defaults");
        const auto cleared = DeserializeAppSettings(L"{\"version\":1,\"settings\":{\"launchModels\":\"\"}}");
        CHECK(cleared.launchModels.empty(), "settings launchModels PRESENT-but-empty stays empty (deliberate 'just Default')");
    }

    // Empty / garbage -> all defaults (a missing settings.json must change nothing).
    {
        const auto out = DeserializeAppSettings(L"");
        CHECK(out.skipPermissions == true && out.includeCoAuthoredBy == true, "settings defaults on empty");
        CHECK(out.launchModels == std::wstring{ kDefaultLaunchModels }, "settings launchModels defaults (Fable 5 / Opus 4.8 / Sonnet 5) on empty");
        CHECK(out.defaultAutorunnerMode == AutorunnerMode::Full && out.maxAutoSends == 100u, "settings autorunner default Full on empty");
        CHECK(out.archiveSplitFraction > 0.499 && out.archiveSplitFraction < 0.501, "settings archiveSplitFraction default 0.5 on empty");
        CHECK(out.summaryPanelWidthFraction == 0.0 && out.summaryPanelHeightFraction == 0.0, "settings summaryPanel size fractions default 0 (auto) on empty");
        CHECK(out.summaryPanelWrapNewlines == false, "settings summaryPanelWrapNewlines default false (literal-\\n look) on empty");
        CHECK(out.summaryPanelTruncate == true, "settings summaryPanelTruncate default true (truncate) on empty");
        CHECK(out.showTabCloseButton == true, "settings showTabCloseButton default true (show X) on empty");
        CHECK(out.closeTabOnMiddleClick == true, "settings closeTabOnMiddleClick default true (middle-click closes) on empty");
        CHECK(out.showTabIcon == false, "settings showTabIcon default false (tab icons HIDDEN) on empty");
        CHECK(out.waitingForYouTimeoutMinutes == 4320u, "settings waitingForYouTimeoutMinutes default 4320 (3d Waiting-for-you timeout) on empty");
        CHECK(out.serverCacheMinutes == 5u, "settings serverCacheMinutes default 5 (server cache lifetime) on empty");
        CHECK(out.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode default (Shift+Enter) on empty");
        CHECK(out.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon default (Crown) on empty");
        CHECK(out.tabColorMode == TabColorMode::WorkingDirectory, "settings tabColorMode default (shared per working dir) on empty");
        CHECK(out.inferGitRoot == true, "settings inferGitRoot default ON (\"Use .git folder to infer\") on empty");
        CHECK(out.flashRingColor == L"#CCFF0000", "settings flashRingColor default (80% red) on empty");
        CHECK(out.pendingDotsLightColor == L"#FFE0A92B", "settings pendingDotsLightColor default (gold, on dark) on empty");
        CHECK(out.pendingDotsDarkColor == L"#FF5A3E00", "settings pendingDotsDarkColor default (amber, on light) on empty");
        CHECK(out.treeSort == ExplorerSort::Newest, "settings treeSort default (Newest) on empty");
        CHECK(out.boardSort == ExplorerSort::MostActive, "settings boardSort default (MostActive) on empty");
        CHECK(out.autoTestingShowsSummary == true, "settings autoTestingShowsSummary default (Summary) on empty");
        // System notifications: a pre-feature settings.json gets the default rule — a toast on
        // Running -> ANYTHING else (master + every target state + skip-focused + sound all ON).
        CHECK(out.notificationsEnabled == true, "settings notificationsEnabled default ON on empty");
        CHECK(out.notifyOnWaiting == true && out.notifyOnNeedsApproval == true && out.notifyOnIdle == true &&
                  out.notifyOnDone == true && out.notifyOnError == true,
              "settings notifyOn* target-state switches default ON on empty (the 'Running to anything else' rule)");
        CHECK(out.notifySuppressFocused == true, "settings notifySuppressFocused default ON on empty");
        CHECK(out.notifySound == true, "settings notifySound default ON on empty");
        CHECK(out.hiddenSessionIds.empty(), "settings hiddenSessionIds empty on empty");
        const auto out2 = DeserializeAppSettings(L"not json");
        CHECK(out2.skipPermissions == true && out2.confirmBeforeKill == true, "settings defaults on garbage");
    }

    // Agentmaster: the legacy "waitingDecayMinutes" key was RENAMED to "waitingForYouTimeoutMinutes"
    // because the Waiting-for-you behavior changed (a 5-minute cache window -> a read-gated unread
    // timeout). A pre-existing settings.json carries the OLD key with a value tuned for the old
    // behavior (often 5); it must be INVALIDATED — ignored, falling back to the new 4320 (3d) default,
    // NOT carried over as a 5-minute unread timeout. The new key, when present, reads normally.
    {
        const auto legacy = DeserializeAppSettings(L"{\"settings\":{\"waitingDecayMinutes\":5}}");
        CHECK(legacy.waitingForYouTimeoutMinutes == 4320u, "settings legacy waitingDecayMinutes key is IGNORED -> 4320 (3d) default (rename invalidates the stale value)");
        const auto fresh = DeserializeAppSettings(L"{\"settings\":{\"waitingForYouTimeoutMinutes\":120}}");
        CHECK(fresh.waitingForYouTimeoutMinutes == 120u, "settings new waitingForYouTimeoutMinutes key is read");
    }

    // ExplorerSort token parsing for treeSort/boardSort: each token -> its mode; an unknown token falls
    // back to Newest. boardSort additionally defaults to MostActive when the KEY is absent (above).
    {
        const auto a = DeserializeAppSettings(L"{\"settings\":{\"boardSort\":\"alpha\",\"treeSort\":\"active\"}}");
        CHECK(a.boardSort == ExplorerSort::Alpha, "settings boardSort 'alpha' honored");
        CHECK(a.treeSort == ExplorerSort::MostActive, "settings treeSort 'active' honored");
        const auto act = DeserializeAppSettings(L"{\"settings\":{\"boardSort\":\"active\"}}");
        CHECK(act.boardSort == ExplorerSort::MostActive, "settings boardSort 'active' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"boardSort\":\"bogus\"}}");
        CHECK(bad.boardSort == ExplorerSort::Newest, "settings boardSort unknown token -> Newest");
    }

    // tabRenameCommitMode: each token parses to its mode; an unknown token falls back to the default.
    {
        const auto none = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"clickAway\"}}");
        CHECK(none.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOnly, "settings tabRenameCommitMode 'clickAway' honored");
        const auto ent = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"enter\"}}");
        CHECK(ent.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter, "settings tabRenameCommitMode 'enter' honored");
        const auto se = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"shiftEnter\"}}");
        CHECK(se.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode 'shiftEnter' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"tabRenameCommitMode\":\"bogus\"}}");
        CHECK(bad.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode unknown -> default (Shift+Enter)");
    }

    // favoriteIcon (FAVORITES.md §5a): each token parses to its glyph; an unknown/absent token falls
    // back to Crown (the prior behavior).
    {
        const auto crown = DeserializeAppSettings(L"{\"settings\":{\"favoriteIcon\":\"crown\"}}");
        CHECK(crown.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon 'crown' honored");
        const auto star = DeserializeAppSettings(L"{\"settings\":{\"favoriteIcon\":\"star\"}}");
        CHECK(star.favoriteIcon == FavoriteIcon::Star, "settings favoriteIcon 'star' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"favoriteIcon\":\"bogus\"}}");
        CHECK(bad.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon unknown -> default (Crown)");
    }

    // tabColorMode (tab color modes): each token parses to its mode; an unknown/absent token falls
    // back to WorkingDirectory (the prior shared-per-dir behavior).
    {
        const auto wd = DeserializeAppSettings(L"{\"settings\":{\"tabColorMode\":\"workingDirectory\"}}");
        CHECK(wd.tabColorMode == TabColorMode::WorkingDirectory, "settings tabColorMode 'workingDirectory' honored");
        const auto ind = DeserializeAppSettings(L"{\"settings\":{\"tabColorMode\":\"individual\"}}");
        CHECK(ind.tabColorMode == TabColorMode::Individual, "settings tabColorMode 'individual' honored");
        const auto inf = DeserializeAppSettings(L"{\"settings\":{\"tabColorMode\":\"inferredWorkingDirectory\"}}");
        CHECK(inf.tabColorMode == TabColorMode::InferredWorkingDirectory, "settings tabColorMode 'inferredWorkingDirectory' honored");
        const auto bad = DeserializeAppSettings(L"{\"settings\":{\"tabColorMode\":\"bogus\"}}");
        CHECK(bad.tabColorMode == TabColorMode::WorkingDirectory, "settings tabColorMode unknown -> default (shared per working dir)");
    }

    // A present subset is honored; the rest keep defaults.
    {
        const auto out = DeserializeAppSettings(L"{\"settings\":{\"model\":\"sonnet\",\"maxAutoSends\":3}}");
        CHECK(out.model == L"sonnet", "settings present model honored");
        CHECK(out.maxAutoSends == 3u, "settings present maxAutoSends honored");
        CHECK(out.skipPermissions == true, "settings missing skipPermissions -> default");
    }

    // Tab title naming trio: present values honored (incl. the branch tokens); absent keys land
    // the defaults (LastWord / Default case / no underscores).
    {
        const auto out = DeserializeAppSettings(L"{\"settings\":{\"tabTitleNaming\":\"capitals\",\"tabTitleCase\":\"upper\",\"tabTitleSpacesToUnderscores\":true}}");
        CHECK(out.tabTitleNaming == TabTitleNaming::Capitals, "settings tabTitleNaming honored");
        CHECK(out.tabTitleCase == TabTitleCase::Upper, "settings tabTitleCase honored");
        CHECK(out.tabTitleSpacesToUnderscores == true, "settings tabTitleSpacesToUnderscores honored");
        const auto br = DeserializeAppSettings(L"{\"settings\":{\"tabTitleNaming\":\"branchTwoFolders\"}}");
        CHECK(br.tabTitleNaming == TabTitleNaming::BranchTwoFolders, "settings tabTitleNaming branch token honored");
        const auto def = DeserializeAppSettings(L"{\"settings\":{}}");
        CHECK(def.tabTitleNaming == TabTitleNaming::LastWord, "settings tabTitleNaming absent -> LastWord");
        CHECK(def.tabTitleCase == TabTitleCase::Default, "settings tabTitleCase absent -> Default");
        CHECK(def.tabTitleSpacesToUnderscores == false, "settings tabTitleSpacesToUnderscores absent -> off");
    }

    // archiveSplitFraction: a sane value is honored; an extreme/corrupt one falls back to 0.5
    // (the same sane-band rule as the Manager layout fractions — a pane must never collapse).
    {
        const auto ok = DeserializeAppSettings(L"{\"settings\":{\"archiveSplitFraction\":0.7}}");
        CHECK(ok.archiveSplitFraction > 0.699 && ok.archiveSplitFraction < 0.701, "settings archiveSplitFraction honored in-band");
        const auto lo = DeserializeAppSettings(L"{\"settings\":{\"archiveSplitFraction\":0.001}}");
        CHECK(lo.archiveSplitFraction > 0.499 && lo.archiveSplitFraction < 0.501, "settings archiveSplitFraction clamped (too small)");
        const auto hi = DeserializeAppSettings(L"{\"settings\":{\"archiveSplitFraction\":1.5}}");
        CHECK(hi.archiveSplitFraction > 0.499 && hi.archiveSplitFraction < 0.501, "settings archiveSplitFraction clamped (too large)");
    }

    // summaryPanel size fractions (TAB_OVERLAY.md resize): an in-band value is honored; 0 (auto) or an
    // out-of-band/corrupt value falls back to 0 (auto), so a bad value can't wedge the panel at a
    // degenerate size. Bands: width (0.08, 0.5], height (0.06, 0.75].
    {
        const auto ok = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0.45,\"summaryPanelHeightFraction\":0.7}}");
        CHECK(ok.summaryPanelWidthFraction > 0.449 && ok.summaryPanelWidthFraction < 0.451, "settings summaryPanelWidthFraction honored in-band");
        CHECK(ok.summaryPanelHeightFraction > 0.699 && ok.summaryPanelHeightFraction < 0.701, "settings summaryPanelHeightFraction honored in-band");
        const auto zero = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0,\"summaryPanelHeightFraction\":0}}");
        CHECK(zero.summaryPanelWidthFraction == 0.0 && zero.summaryPanelHeightFraction == 0.0, "settings summaryPanel size 0 (auto) preserved");
        const auto wide = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0.9}}");
        CHECK(wide.summaryPanelWidthFraction == 0.0, "settings summaryPanelWidthFraction out-of-band (too wide) -> 0 (auto)");
        const auto tall = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelHeightFraction\":0.95}}");
        CHECK(tall.summaryPanelHeightFraction == 0.0, "settings summaryPanelHeightFraction out-of-band (too tall) -> 0 (auto)");
        const auto tiny = DeserializeAppSettings(L"{\"settings\":{\"summaryPanelWidthFraction\":0.02}}");
        CHECK(tiny.summaryPanelWidthFraction == 0.0, "settings summaryPanelWidthFraction out-of-band (too thin) -> 0 (auto)");
    }
}

void TestTabNamingAndColor()
{
    std::wprintf(L"[tab naming + per-dir color]\n");

    // --- DeriveSessionTitle: walk up past generic segments, then the configured naming technique
    // + output transforms. The 2-arg form is PURE (options passed in); TitleNamingOptions{} == the
    // defaults (LastWord / Default case / no underscores) — the 1-arg form reads the cog settings
    // from disk, so tests always pass options explicitly. ---
    const TitleNamingOptions optDefault{};
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp", optDefault) == L"NumSharp", "one-word leaf stays whole (LastWord default)");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\bin\\Debug", optDefault) == L"NumSharp", "walk up past bin/Debug");
    CHECK(DeriveSessionTitle(L"K:\\proj\\obj\\x64\\Release", optDefault) == L"proj", "walk up past obj/x64/Release");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\", optDefault) == L"NumSharp", "trailing slash tolerated");
    CHECK(DeriveSessionTitle(L"K:/source/NumSharp", optDefault) == L"NumSharp", "forward slashes tolerated");
    CHECK(DeriveSessionTitle(L"C:\\bin", optDefault) == L"bin", "all-generic falls back to the leaf");
    CHECK(DeriveSessionTitle(L"", optDefault) == L"claude", "empty dir -> claude");

    // LastWord (the default technique): the last '.'/' '/'-'/'_'-separated word; whole when no separator.
    CHECK(DeriveSessionTitle(L"C:\\repos\\Potato.Tomato.SlangGang", optDefault) == L"SlangGang", "LastWord: dotted name -> last segment");
    CHECK(DeriveSessionTitle(L"C:\\x\\PotatoTomato", optDefault) == L"PotatoTomato", "LastWord: no separators -> whole name");
    CHECK(DeriveSessionTitle(L"C:\\x\\Potato Tomato Slang", optDefault) == L"Slang", "LastWord: spaces separate words");
    CHECK(DeriveSessionTitle(L"C:\\x\\Potato.Tomato", optDefault) == L"Tomato", "LastWord: two segments -> the last");
    CHECK(DeriveSessionTitle(L"C:\\x\\potato-tomato_gang", optDefault) == L"gang", "LastWord: '-' and '_' separate too");
    CHECK(DeriveSessionTitle(L"C:\\x\\Potato.Tomato.", optDefault) == L"Tomato", "LastWord: trailing separator ignored");
    CHECK(DeriveSessionTitle(L"C:\\x\\...", optDefault) == L"...", "LastWord: nothing but separators -> whole (never empty)");

    // FolderName: the meaningful folder name as-is (the generic walk still applies).
    TitleNamingOptions optAsIs{};
    optAsIs.naming = TabTitleNaming::FolderName;
    CHECK(DeriveSessionTitle(L"C:\\repos\\Potato.Tomato.SlangGang", optAsIs) == L"Potato.Tomato.SlangGang", "FolderName: name kept verbatim");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\bin\\Debug", optAsIs) == L"NumSharp", "FolderName: generic walk still applies");
    {
        // The trim is a 255-char SAFETY NET only — real (even long) names stay whole; past 255 the
        // title becomes its first 252 chars + "..." (== 255 total).
        const std::wstring leaf(100, L'a');
        CHECK(DeriveSessionTitle(L"C:\\x\\" + leaf, optAsIs) == leaf, "FolderName: a 100-char name is NOT trimmed (the cap is 255)");
        const std::wstring exact(255, L'a');
        CHECK(DeriveSessionTitle(L"C:\\x\\" + exact, optAsIs) == exact, "FolderName: exactly 255 chars stays whole");
        const std::wstring huge(300, L'a');
        CHECK(DeriveSessionTitle(L"C:\\x\\" + huge, optAsIs) == std::wstring(252, L'a') + L"...", "FolderName: >255 chars trims to 252 + ... (255 total)");
    }

    // TwoFolders: "<parent>/<folder>"; a folder directly under the drive root has no parent folder.
    TitleNamingOptions optTwo{};
    optTwo.naming = TabTitleNaming::TwoFolders;
    CHECK(DeriveSessionTitle(L"C:\\repos\\Potato.Tomato.SlangGang", optTwo) == L"repos/Potato.Tomato.SlangGang", "TwoFolders: parent/folder");
    CHECK(DeriveSessionTitle(L"C:\\OnlyFolder", optTwo) == L"OnlyFolder", "TwoFolders: no parent below the drive root -> folder alone");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\bin\\Debug", optTwo) == L"source/NumSharp", "TwoFolders: parent of the MEANINGFUL folder (generic walk first)");

    // Capitals: the capitals only; a no-capitals name falls back to word initials (>=2 words) else as-is.
    TitleNamingOptions optCaps{};
    optCaps.naming = TabTitleNaming::Capitals;
    CHECK(DeriveSessionTitle(L"C:\\x\\PotaTo.Tomato.Slang", optCaps) == L"PTTS", "Capitals: every capital collected");
    CHECK(DeriveSessionTitle(L"C:\\x\\PotatoTomato", optCaps) == L"PT", "Capitals: camel-case capitals");
    CHECK(DeriveSessionTitle(L"C:\\x\\Potato Tomato Slang", optCaps) == L"PTS", "Capitals: spaced words");
    CHECK(DeriveSessionTitle(L"C:\\x\\MyVeryLongProjectName", optCaps) == L"MVLPN", "Capitals: the legacy capitals-only rule");
    CHECK(DeriveSessionTitle(L"C:\\x\\potato tomato", optCaps) == L"PT", "Capitals: no capitals -> word initials uppercased");
    CHECK(DeriveSessionTitle(L"C:\\x\\agentmaster", optCaps) == L"agentmaster", "Capitals: single lowercase word -> as-is");

    // Branch / BranchFolder / BranchTwoFolders: the branch is an INPUT (opts.branch — the 2-arg
    // form stays pure; the 1-arg configured form resolves it via ReadGitBranchForDir). An empty
    // branch drops the component + its separator; the bare Branch technique then falls back to the
    // folder name (never empty).
    {
        TitleNamingOptions o{};
        o.naming = TabTitleNaming::Branch;
        o.branch = L"feature/issue123";
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"feature/issue123", "Branch: the branch name alone");
        o.branch.clear();
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"Agentmaster", "Branch: no branch -> the folder name");

        o.naming = TabTitleNaming::BranchFolder;
        o.branch = L"feature/ui";
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"feature/ui/Agentmaster", "BranchFolder: <branch>/<folder>");
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster\\bin\\Debug", o) == L"feature/ui/Agentmaster", "BranchFolder: generic walk still applies");
        o.branch.clear();
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"Agentmaster", "BranchFolder: no branch -> folder alone (no dangling '/')");

        o.naming = TabTitleNaming::BranchTwoFolders;
        o.branch = L"main";
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"main/source/Agentmaster", "BranchTwoFolders: <branch>/<parent>/<folder>");
        CHECK(DeriveSessionTitle(L"C:\\OnlyFolder", o) == L"main/OnlyFolder", "BranchTwoFolders: no parent below the drive root -> branch/folder");
        o.branch.clear();
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"source/Agentmaster", "BranchTwoFolders: no branch -> the TwoFolders output");
        o.branch = L"feature/a-very-long-branch-name";
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"feature/a-very-long-branch-name/source/Agentmaster", "BranchTwoFolders: a long (but <255) combo stays whole");
        o.branch = std::wstring(300, L'b');
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == std::wstring(252, L'b') + L"...", "BranchTwoFolders: past 255 trims to 252 + ... (255 total)");
    }

    // '\' -> '/' normalization is UNCONDITIONAL (no setting) and title-wide — a backslash in any
    // component (only a caller-supplied branch can realistically carry one) reads as '/'.
    {
        TitleNamingOptions o{};
        o.naming = TabTitleNaming::BranchFolder;
        o.branch = L"feature\\ui";
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"feature/ui/Agentmaster", "normalization: '\\' in the branch reads as '/'");
        o.naming = TabTitleNaming::Branch;
        o.branch = L"a\\b\\c";
        CHECK(DeriveSessionTitle(L"K:\\source\\Agentmaster", o) == L"a/b/c", "normalization: every '\\' normalized");
    }

    // Case + whitespace transforms compose over any technique.
    {
        TitleNamingOptions o{};
        o.naming = TabTitleNaming::FolderName;
        o.caseMode = TabTitleCase::Upper;
        CHECK(DeriveSessionTitle(L"C:\\x\\Potato Tomato", o) == L"POTATO TOMATO", "case: Uppercase");
        o.caseMode = TabTitleCase::Lower;
        CHECK(DeriveSessionTitle(L"C:\\x\\Potato Tomato", o) == L"potato tomato", "case: Lowercase");
        o.caseMode = TabTitleCase::Default;
        o.spacesToUnderscores = true;
        CHECK(DeriveSessionTitle(L"C:\\x\\Potato Tomato Slang", o) == L"Potato_Tomato_Slang", "whitespace -> underscores");
        o.caseMode = TabTitleCase::Upper;
        CHECK(DeriveSessionTitle(L"C:\\x\\Potato Tomato", o) == L"POTATO_TOMATO", "case + underscores compose");
        o = TitleNamingOptions{};
        o.naming = TabTitleNaming::TwoFolders;
        o.caseMode = TabTitleCase::Lower;
        CHECK(DeriveSessionTitle(L"C:\\Repos\\NumSharp", o) == L"repos/numsharp", "TwoFolders lowercased");
    }

    // --- DeriveForkTitle: first fork appends " (fork)", forking a fork BUMPS the counter ---
    CHECK(DeriveForkTitle(L"NumSharp") == L"NumSharp (fork)", "first fork appends (fork)");
    CHECK(DeriveForkTitle(L"NumSharp (fork)") == L"NumSharp (fork 2)", "fork of a fork -> (fork 2), not (fork) (fork)");
    CHECK(DeriveForkTitle(L"NumSharp (fork 2)") == L"NumSharp (fork 3)", "(fork 2) -> (fork 3)");
    CHECK(DeriveForkTitle(L"NumSharp (fork 9)") == L"NumSharp (fork 10)", "multi-digit counter increments");
    CHECK(DeriveForkTitle(L"Foo (bar)") == L"Foo (bar) (fork)", "non-fork trailing paren is preserved, gets (fork)");
    CHECK(DeriveForkTitle(L"Foo (1.0)") == L"Foo (1.0) (fork)", "non-numeric-after-fork trailer untouched");
    CHECK(DeriveForkTitle(L"My App (beta) (fork)") == L"My App (beta) (fork 2)", "only the trailing fork group is bumped (nested paren safe)");
    CHECK(DeriveForkTitle(L"") == L" (fork)", "empty source -> ' (fork)' (degenerate; caller derives a base first)");
    CHECK(DeriveForkTitle(L"(fork)") == L"(fork) (fork)", "no prefix before (fork) -> append (not bump; degenerate)");
    CHECK(DeriveForkTitle(L"x (fork )") == L"x (fork ) (fork)", "trailing space inside the group is not a counter -> append");

    // --- IsGenericDirName (case-folded) ---
    CHECK(IsGenericDirName(L"bin") && IsGenericDirName(L"BIN") && IsGenericDirName(L"Obj"), "generic names case-insensitive");
    CHECK(!IsGenericDirName(L"NumSharp"), "project name not generic");

    // --- NormDirKey: slash + trailing + (windows) case fold collapse to one key ---
    CHECK(NormDirKey(L"K:\\A\\B\\") == NormDirKey(L"K:/a/b"), "case/slash/trailing variants share a key");

    // --- AutoDirColorHex (preview): deterministic, palette form, stable across path spelling ---
    SeedDirColors(0xA11CE5EEull); // fix the seed so probe orders are deterministic in this test
    const auto c1 = AutoDirColorHex(L"K:\\source\\NumSharp");
    CHECK(c1.size() == 7 && c1[0] == L'#', "auto color is #RRGGBB");
    CHECK(c1 == AutoDirColorHex(L"K:\\source\\NumSharp"), "auto color deterministic");
    CHECK(c1 == AutoDirColorHex(L"k:/source/numsharp"), "auto color stable across spelling");

    // --- ChooseDirColor (PURE permanent allocator; no disk): permanence, uniqueness, exhaustion reset ---
    {
        const uint64_t seed = 0xA11CE5EEull;
        using Map = std::vector<std::pair<std::wstring, std::wstring>>;

        // (0) An already-assigned folder keeps its color — permanence (independent of seed/open set).
        const Map one = { { L"k:\\a", L"#123456" } };
        CHECK(ChooseDirColor(L"k:\\a", one, {}, seed) == L"#123456", "assigned folder keeps its color (permanent)");
        CHECK(ChooseDirColor(L"k:\\a", one, {}, 999ull) == L"#123456", "permanence is seed-independent");

        // (1) Assigning palette-many folders in turn (each appended to the map) yields all-DISTINCT
        // colors — the "two folders, same color" fix: a new folder avoids every color already assigned.
        const size_t n = 14; // == kAutoPalette size in Persistence.cpp
        Map existing;
        std::unordered_set<std::wstring> seen;
        bool distinct = true;
        for (size_t i = 0; i < n; ++i)
        {
            const std::wstring key = NormDirKey(L"k:\\fleet\\dir" + std::to_wstring(i));
            const auto c = ChooseDirColor(key, existing, {}, seed);
            existing.emplace_back(key, c);
            if (!seen.insert(c).second)
            {
                distinct = false;
            }
        }
        CHECK(distinct && seen.size() == n, "palette-many folders get distinct colors (collision-free)");

        // (2) Palette exhausted (all 14 assigned): a new folder RESETS the collection and reuses, but
        // avoids the colors open tabs are actively showing.
        const std::wstring activeHex = existing.front().second;
        const auto reused = ChooseDirColor(L"k:\\fleet\\extra", existing, { activeHex }, seed);
        CHECK(reused.size() == 7 && reused[0] == L'#', "exhausted -> still a palette color");
        CHECK(reused != activeHex, "exhausted reuse avoids an actively-shown color");

        // (3) Every palette color is active (more open dirs than colors): reuse is unavoidable, but the
        // result is still a valid palette color.
        std::unordered_set<std::wstring> allActive;
        for (const auto& [k, h] : existing)
        {
            allActive.insert(h);
        }
        const auto forced = ChooseDirColor(L"k:\\fleet\\extra2", existing, allActive, seed);
        CHECK(forced.size() == 7 && forced[0] == L'#', "all-active -> fallback is still a palette color");
    }

    // --- DeCollideDirColors (v1->v2 migration core): fix duplicate palette colors, keep user picks ---
    {
        const std::vector<std::pair<std::wstring, std::wstring>> in = {
            { L"k:\\desktop", L"#E06C75" }, // palette
            { L"k:\\eli", L"#E06C75" }, // dup of desktop -> reassign
            { L"k:\\numsharp", L"#2BBAC5" }, // palette
            { L"k:\\grimes", L"#2BBAC5" }, // dup of numsharp -> reassign
            { L"k:\\agentmaster", L"#9E696A" }, // off-palette user pick -> keep verbatim
        };
        const auto out = DeCollideDirColors(in, 0xD3C0DEull);
        CHECK(out.size() == in.size(), "de-collide keeps every folder");
        const auto colorOf = [&](const std::wstring& key) -> std::wstring {
            for (const auto& [k, h] : out)
            {
                if (k == key)
                {
                    return h;
                }
            }
            return L"";
        };
        CHECK(colorOf(L"k:\\desktop") == L"#E06C75", "first occurrence keeps its palette color");
        CHECK(colorOf(L"k:\\numsharp") == L"#2BBAC5", "first occurrence keeps its palette color (2)");
        CHECK(colorOf(L"k:\\agentmaster") == L"#9E696A", "off-palette user pick preserved verbatim");
        CHECK(colorOf(L"k:\\eli") != L"#E06C75", "duplicate reassigned away from desktop");
        CHECK(colorOf(L"k:\\grimes") != L"#2BBAC5", "duplicate reassigned away from numsharp");
        std::unordered_set<std::wstring> palSeen;
        bool allUnique = true;
        for (const auto& [k, h] : out)
        {
            if (h == L"#9E696A")
            {
                continue; // off-palette user pick — not a palette slot
            }
            if (!palSeen.insert(h).second)
            {
                allUnique = false;
            }
        }
        CHECK(allUnique, "de-collide leaves no two folders sharing a palette color");
    }
    CHECK(SerializeDirColors({}).find(L"\"version\":2") != std::wstring::npos, "dir-colors serializes at version 2");

    // --- DirColors serialize round-trip (pure; no disk) ---
    {
        std::vector<std::pair<std::wstring, std::wstring>> in = {
            { L"k:\\a", L"#112233" }, { L"k:\\b", L"#AABBCC" }
        };
        const auto out = DeserializeDirColors(SerializeDirColors(in));
        CHECK(out.size() == 2, "dir-colors round-trip count");
        CHECK(out.size() == 2 && out[0].first == L"k:\\a" && out[0].second == L"#112233", "dir-colors round-trip [0]");
        CHECK(out.size() == 2 && out[1].first == L"k:\\b" && out[1].second == L"#AABBCC", "dir-colors round-trip [1]");
    }
    CHECK(DeserializeDirColors(L"").empty(), "empty dir-colors -> empty");
    CHECK(DeserializeDirColors(L"not json").empty(), "garbage dir-colors -> empty");
}


void TestTabColorModes()
{
    std::wprintf(L"[tab color modes]\n");

    // --- enum <-> string round-trip + unknown-token fallback ---
    CHECK(TabColorModeFromString(ToString(TabColorMode::WorkingDirectory)) == TabColorMode::WorkingDirectory, "tabColorMode workingDirectory round-trip");
    CHECK(TabColorModeFromString(ToString(TabColorMode::Individual)) == TabColorMode::Individual, "tabColorMode individual round-trip");
    CHECK(TabColorModeFromString(ToString(TabColorMode::InferredWorkingDirectory)) == TabColorMode::InferredWorkingDirectory, "tabColorMode inferredWorkingDirectory round-trip");
    CHECK(TabColorModeFromString(ToString(TabColorMode::NoColor)) == TabColorMode::NoColor, "tabColorMode noColor round-trip");
    CHECK(ToString(TabColorMode::NoColor) == L"noColor", "tabColorMode NoColor serializes as the 'noColor' token");
    CHECK(TabColorModeFromString(L"nonsense") == TabColorMode::WorkingDirectory, "tabColorMode unknown token -> WorkingDirectory (the prior behavior)");

    // --- tab title naming enums: <-> string round-trip + unknown-token fallback ---
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::LastWord)) == TabTitleNaming::LastWord, "tabTitleNaming lastWord round-trip");
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::FolderName)) == TabTitleNaming::FolderName, "tabTitleNaming folderName round-trip");
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::TwoFolders)) == TabTitleNaming::TwoFolders, "tabTitleNaming twoFolders round-trip");
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::Capitals)) == TabTitleNaming::Capitals, "tabTitleNaming capitals round-trip");
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::Branch)) == TabTitleNaming::Branch, "tabTitleNaming branch round-trip");
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::BranchFolder)) == TabTitleNaming::BranchFolder, "tabTitleNaming branchFolder round-trip");
    CHECK(TabTitleNamingFromString(ToString(TabTitleNaming::BranchTwoFolders)) == TabTitleNaming::BranchTwoFolders, "tabTitleNaming branchTwoFolders round-trip");
    CHECK(TabTitleNamingFromString(L"nonsense") == TabTitleNaming::LastWord, "tabTitleNaming unknown token -> LastWord (the default technique)");
    CHECK(TitleNamingUsesBranch(TabTitleNaming::Branch) && TitleNamingUsesBranch(TabTitleNaming::BranchFolder) && TitleNamingUsesBranch(TabTitleNaming::BranchTwoFolders), "TitleNamingUsesBranch: the Branch* trio");
    CHECK(!TitleNamingUsesBranch(TabTitleNaming::LastWord) && !TitleNamingUsesBranch(TabTitleNaming::FolderName) && !TitleNamingUsesBranch(TabTitleNaming::TwoFolders) && !TitleNamingUsesBranch(TabTitleNaming::Capitals), "TitleNamingUsesBranch: folder-only techniques don't read the branch");
    CHECK(TabTitleCaseFromString(ToString(TabTitleCase::Default)) == TabTitleCase::Default, "tabTitleCase default round-trip");
    CHECK(TabTitleCaseFromString(ToString(TabTitleCase::Lower)) == TabTitleCase::Lower, "tabTitleCase lower round-trip");
    CHECK(TabTitleCaseFromString(ToString(TabTitleCase::Upper)) == TabTitleCase::Upper, "tabTitleCase upper round-trip");
    CHECK(TabTitleCaseFromString(L"nonsense") == TabTitleCase::Default, "tabTitleCase unknown token -> Default");

    // --- SessionColorKeyDir: which dir KEYS a session's color under each mode ---
    {
        SessionInfo s;
        s.id = L"sid-key";
        s.workingDir = L"K:\\repo";
        CHECK(SessionColorKeyDir(TabColorMode::WorkingDirectory, s) == L"K:\\repo", "key dir: default mode -> the working dir");
        CHECK(SessionColorKeyDir(TabColorMode::InferredWorkingDirectory, s) == L"K:\\repo", "key dir: inferred mode with NO inference yet -> the working dir (no interim color flip)");
        s.inferredWorkingDir = L"K:\\repo\\src\\area";
        CHECK(SessionColorKeyDir(TabColorMode::InferredWorkingDirectory, s) == L"K:\\repo\\src\\area", "key dir: inferred mode with an inference -> the inferred dir");
        CHECK(SessionColorKeyDir(TabColorMode::WorkingDirectory, s) == L"K:\\repo", "key dir: default mode IGNORES a stored inference (mode switch keeps dir semantics)");
        CHECK(SessionColorKeyDir(TabColorMode::Individual, s) == L"K:\\repo", "key dir: individual mode returns the working dir (callers branch on the mode before grouping)");
        CHECK(SessionColorKeyDir(TabColorMode::NoColor, s) == L"K:\\repo", "key dir: NoColor mode returns the working dir too (grouping keeps classic dir semantics)");
    }

    // --- EffectiveWorkingDir: the ONE "which directory does this session WORK in" answer, shared
    // by every semantic surface (tree grouping / dir scope / board card / launch pre-aim /
    // Open-New-Here / overlay subline+Open Path+Copy Path). SessionColorKeyDir builds on it, adding
    // ONLY a git-worktree -> main-repo canonicalization for COLOR (see the worktree test below), so
    // for every NON-worktree dir the color key == the effective dir — a card/row can't sit in one
    // directory group while its tab wears another group's color. ---
    {
        SessionInfo s;
        s.id = L"sid-eff";
        s.workingDir = L"K:\\launchcwd";
        CHECK(EffectiveWorkingDir(TabColorMode::WorkingDirectory, s) == L"K:\\launchcwd", "effective dir: default mode -> the launch cwd");
        CHECK(EffectiveWorkingDir(TabColorMode::InferredWorkingDirectory, s) == L"K:\\launchcwd", "effective dir: inferred mode, no inference yet -> the launch cwd (fresh session behaves classic)");
        s.inferredWorkingDir = L"Q:\\other\\repo";
        CHECK(EffectiveWorkingDir(TabColorMode::InferredWorkingDirectory, s) == L"Q:\\other\\repo", "effective dir: inferred mode + an inference -> the INFERRED dir (where the session actually works)");
        CHECK(EffectiveWorkingDir(TabColorMode::WorkingDirectory, s) == L"K:\\launchcwd", "effective dir: default mode ignores a dormant inference (mode switch restores cwd semantics)");
        CHECK(EffectiveWorkingDir(TabColorMode::Individual, s) == L"K:\\launchcwd", "effective dir: Individual mode ignores the inference too (deliberate cwd — the dormant inference never leaks)");
        CHECK(EffectiveWorkingDir(TabColorMode::NoColor, s) == L"K:\\launchcwd", "effective dir: NoColor mode ignores the inference too (colors off, deliberate-cwd semantics classic)");
        // The never-drift contract for NON-worktree dirs (these paths aren't linked worktrees): the
        // color key IS the effective dir, in every mode x inference state. (The one deliberate
        // divergence — a git worktree keying its main repo's color — is exercised below.)
        for (const auto mode : { TabColorMode::WorkingDirectory, TabColorMode::Individual, TabColorMode::InferredWorkingDirectory, TabColorMode::NoColor })
        {
            CHECK(SessionColorKeyDir(mode, s) == EffectiveWorkingDir(mode, s), "never-drift: SessionColorKeyDir == EffectiveWorkingDir (with an inference)");
            SessionInfo bare = s;
            bare.inferredWorkingDir.clear();
            CHECK(SessionColorKeyDir(mode, bare) == EffectiveWorkingDir(mode, bare), "never-drift: SessionColorKeyDir == EffectiveWorkingDir (no inference)");
        }
    }

    // --- SessionColorKeyDir worktree sharing (the tab-color change): a git WORKTREE cwd/inferred dir
    // keys the MAIN repo's COLOR, while EffectiveWorkingDir (grouping + mechanics) stays the worktree
    // — the SINGLE deliberate divergence. So a repo and all its worktrees land on ONE color key and
    // wear ONE color. Builds the minimal git-worktree admin layout under %TEMP% (the shape `git
    // worktree add` writes) so the .git-file + commondir resolution runs on the real filesystem. ---
    {
        wchar_t tmp[MAX_PATH]{};
        const DWORD tn = ::GetTempPathW(MAX_PATH, tmp);
        if (tn > 0 && tn < MAX_PATH)
        {
            const std::wstring base = std::wstring(tmp, tn) + L"am-wt-keytest";
            const std::wstring mainRoot = base + L"\\main";
            const std::wstring wtAdmin = mainRoot + L"\\.git\\worktrees\\wt";
            const std::wstring wtRoot = base + L"\\wt";

            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(wtAdmin), ec);
            std::filesystem::create_directories(std::filesystem::path(wtRoot), ec);
            const auto writeFile = [](const std::wstring& path, const std::string& content) {
                const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    DWORD w = 0;
                    ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
                    ::CloseHandle(h);
                }
            };
            const auto narrow = [](const std::wstring& w) { return std::string(w.begin(), w.end()); }; // ASCII temp paths
            writeFile(wtAdmin + L"\\commondir", "../..\n"); // -> main\.git
            writeFile(wtRoot + L"\\.git", "gitdir: " + narrow(wtAdmin) + "\n");

            // A session whose cwd IS the worktree: grouping stays the worktree, color remaps to main.
            SessionInfo wt;
            wt.id = L"sid-wt";
            wt.workingDir = wtRoot;
            CHECK(NormDirKey(EffectiveWorkingDir(TabColorMode::WorkingDirectory, wt)) == NormDirKey(wtRoot), "worktree color: EffectiveWorkingDir stays the WORKTREE (grouping/mechanics unchanged)");
            CHECK(NormDirKey(SessionColorKeyDir(TabColorMode::WorkingDirectory, wt)) == NormDirKey(mainRoot), "worktree color: SessionColorKeyDir remaps the worktree cwd -> the MAIN repo root");
            CHECK(NormDirKey(SessionColorKeyDir(TabColorMode::WorkingDirectory, wt)) != NormDirKey(EffectiveWorkingDir(TabColorMode::WorkingDirectory, wt)), "worktree color: the color key DIVERGES from the effective dir (the deliberate exception)");

            // A session in the MAIN checkout keeps its own cwd as the key -> it MATCHES the worktree's
            // color key, so the repo and its worktree wear ONE color.
            SessionInfo mn;
            mn.id = L"sid-main";
            mn.workingDir = mainRoot;
            CHECK(NormDirKey(SessionColorKeyDir(TabColorMode::WorkingDirectory, mn)) == NormDirKey(mainRoot), "worktree color: the MAIN checkout keeps its own cwd as the color key");
            CHECK(NormDirKey(SessionColorKeyDir(TabColorMode::WorkingDirectory, wt)) == NormDirKey(SessionColorKeyDir(TabColorMode::WorkingDirectory, mn)), "worktree color: repo + worktree share ONE color key");

            // Inferred mode: an INFERRED worktree dir remaps the same way (cwd is elsewhere).
            SessionInfo inf;
            inf.id = L"sid-inf";
            inf.workingDir = L"K:\\elsewhere";
            inf.inferredWorkingDir = wtRoot;
            CHECK(NormDirKey(SessionColorKeyDir(TabColorMode::InferredWorkingDirectory, inf)) == NormDirKey(mainRoot), "worktree color: an INFERRED worktree dir also remaps to the main repo root");

            std::filesystem::remove_all(std::filesystem::path(base), ec); // best-effort temp cleanup
        }
    }

    // --- SessionInfersWorkingDir + the HOME-DIR forcing: a session LAUNCHED in the user's home
    // directory (%USERPROFILE% — the Launch box's empty-`defaultLaunchDir` fallback) INFERS in
    // EVERY mode (its cwd is meaningless — the user just opened a tab and ran claude), while a
    // deliberately-chosen cwd infers only under InferredWorkingDirectory. Uses the REAL
    // %USERPROFILE% (the predicate's cached env read), so this exercises the live resolution. ---
    {
        wchar_t homeBuf[2048]{};
        const DWORD homeLen = ::GetEnvironmentVariableW(L"USERPROFILE", homeBuf, 2048);
        CHECK(homeLen > 0 && homeLen < 2048, "home-dir forcing: %USERPROFILE% resolvable on this machine (the predicate's input)");
        if (homeLen > 0 && homeLen < 2048)
        {
            const std::wstring home{ homeBuf };
            const auto allModes = { TabColorMode::WorkingDirectory, TabColorMode::Individual, TabColorMode::InferredWorkingDirectory, TabColorMode::NoColor };

            SessionInfo athome;
            athome.id = L"sid-home";
            athome.workingDir = home;
            for (const auto mode : allModes)
            {
                CHECK(SessionInfersWorkingDir(mode, athome), "home-dir forcing: a %USERPROFILE% launch infers in EVERY mode");
            }
            // Consumers, no inference yet: the launch cwd answers (a fresh home session behaves classic).
            CHECK(EffectiveWorkingDir(TabColorMode::WorkingDirectory, athome) == home, "home-dir forcing: no inference yet -> the home cwd answers (fresh session classic)");
            // Consumers, inference known: the INFERRED dir answers in EVERY mode — grouping, color
            // key, and every semantic surface follow where the session actually works.
            athome.inferredWorkingDir = L"K:\\source\\Agentmaster";
            for (const auto mode : allModes)
            {
                CHECK(EffectiveWorkingDir(mode, athome) == L"K:\\source\\Agentmaster", "home-dir forcing: effective dir == the INFERRED dir in every mode");
                CHECK(SessionColorKeyDir(mode, athome) == EffectiveWorkingDir(mode, athome), "home-dir forcing: never-drift holds (color key == effective dir)");
            }
            // NormDirKey folding: case / slash / trailing variants of the home dir still force.
            SessionInfo variant = athome;
            std::wstring shouty = home;
            for (auto& c : shouty)
            {
                if (c >= L'a' && c <= L'z')
                {
                    c = static_cast<wchar_t>(c - L'a' + L'A');
                }
            }
            std::wstring slashy = shouty + L"/";
            for (auto& c : slashy)
            {
                if (c == L'\\')
                {
                    c = L'/';
                }
            }
            variant.workingDir = slashy;
            CHECK(SessionInfersWorkingDir(TabColorMode::WorkingDirectory, variant), "home-dir forcing: case/slash/trailing variants of %USERPROFILE% still force (NormDirKey folding)");
            CHECK(EffectiveWorkingDir(TabColorMode::WorkingDirectory, variant) == L"K:\\source\\Agentmaster", "home-dir forcing: the variant-cwd session answers its inferred dir too");
            // A deliberate launch — a SUBfolder of home, or any other dir — never forces: exact-dir
            // match only, so ~/Desktop keeps classic per-cwd semantics outside the Inferred mode.
            SessionInfo sub = athome;
            sub.workingDir = home + L"\\Desktop";
            CHECK(!SessionInfersWorkingDir(TabColorMode::WorkingDirectory, sub), "home-dir forcing: a SUBfolder of home is a deliberate cwd -> no forcing");
            CHECK(EffectiveWorkingDir(TabColorMode::WorkingDirectory, sub) == home + L"\\Desktop", "home-dir forcing: the subfolder session keeps its cwd (dormant inference stays dormant)");
            SessionInfo elsewhere = athome;
            elsewhere.workingDir = L"K:\\repo";
            CHECK(!SessionInfersWorkingDir(TabColorMode::WorkingDirectory, elsewhere), "home-dir forcing: a deliberately-chosen cwd does not force outside the Inferred mode");
            CHECK(SessionInfersWorkingDir(TabColorMode::InferredWorkingDirectory, elsewhere), "home-dir forcing: the Inferred mode still admits every session (the global opt-in)");
        }
    }

    // --- ResolveSessionColorHex: the one read-side resolution every display surface shares ---
    {
        SeedDirColors(0xA11CE5EEull); // deterministic probe orders (no disk write — the seed is in-memory)
        SessionInfo s;
        s.id = L"sid-resolve";
        s.workingDir = L"K:\\repo";
        s.tabColorHex = L"#ABCDEF";
        CHECK(ResolveSessionColorHex(TabColorMode::Individual, s) == L"#ABCDEF", "resolve: Individual -> the session's own persisted color");
        // Dir modes always resolve SOME "#RRGGBB" (persisted dir color, else the AutoDirColorHex
        // preview) — the exact hue depends on this machine's dir-colors.json, so shape-check only.
        const auto wd = ResolveSessionColorHex(TabColorMode::WorkingDirectory, s);
        CHECK(wd.size() == 7 && wd[0] == L'#', "resolve: WorkingDirectory -> a #RRGGBB (persisted or auto preview)");
        SessionInfo bare = s;
        bare.tabColorHex.clear();
        const auto fallback = ResolveSessionColorHex(TabColorMode::Individual, bare);
        CHECK(fallback.size() == 7 && fallback[0] == L'#', "resolve: Individual with NO dealt color falls back to the dir-keyed precedence");
        CHECK(fallback == wd, "resolve: the Individual fallback IS the dir-keyed color (matches the tab until the first deal)");
        // NoColor ("Remove colors"): EMPTY in every state — even with a dealt tabColorHex and a
        // resolvable dir color on record, nothing is read (and, being read-only, nothing dropped):
        // every display surface renders its neutral fallback, matching the uncolored tab.
        CHECK(ResolveSessionColorHex(TabColorMode::NoColor, s).empty(), "resolve: NoColor -> empty even with a persisted session color (kept, not loaded)");
        CHECK(ResolveSessionColorHex(TabColorMode::NoColor, bare).empty(), "resolve: NoColor -> empty for a bare session too (no dir-keyed fallback)");
    }

    // --- ChooseSessionAutoColor: the per-SESSION deal (Individual mode) ---
    {
        SeedDirColors(0xA11CE5EEull); // the session deal shares the dir-color seed
        using Pairs = std::vector<std::pair<std::wstring, std::wstring>>;

        // Deterministic: the same session id + the same live set -> the same color.
        const auto c1 = ChooseSessionAutoColor(L"11111111-aaaa-bbbb-cccc-000000000001", {}, {});
        CHECK(c1.size() == 7 && c1[0] == L'#', "session deal yields a #RRGGBB palette color");
        CHECK(c1 == ChooseSessionAutoColor(L"11111111-aaaa-bbbb-cccc-000000000001", {}, {}), "session deal deterministic");

        // Collision avoidance: dealing palette-many sessions in turn (each fed back as live) yields
        // all-DISTINCT colors — no two open tabs share a color while palette colors remain.
        const size_t n = 14; // == kAutoPalette size in Persistence.cpp
        Pairs live;
        std::unordered_set<std::wstring> active;
        std::unordered_set<std::wstring> seen;
        bool distinct = true;
        for (size_t i = 0; i < n; ++i)
        {
            const std::wstring sid = L"22222222-aaaa-bbbb-cccc-0000000000" + std::to_wstring(10 + i);
            const auto c = ChooseSessionAutoColor(sid, live, active);
            live.emplace_back(sid, c);
            active.insert(c);
            if (!seen.insert(c).second)
            {
                distinct = false;
            }
        }
        CHECK(distinct && seen.size() == n, "palette-many OPEN sessions get distinct colors (collision-free)");

        // Palette exhausted: the deal resets + reuses but still avoids actively-shown colors when any
        // slack remains; with EVERY color active, reuse is unavoidable but stays a palette color.
        const auto forced = ChooseSessionAutoColor(L"33333333-aaaa-bbbb-cccc-000000000001", live, active);
        CHECK(forced.size() == 7 && forced[0] == L'#', "session deal all-active fallback is still a palette color");
        std::unordered_set<std::wstring> nearlyAll = active;
        nearlyAll.erase(c1); // free exactly one color
        const auto slack = ChooseSessionAutoColor(L"44444444-aaaa-bbbb-cccc-000000000001", live, nearlyAll);
        CHECK(slack == c1, "session deal exhausted-reset picks the one color no open tab is showing");
    }
}

// ---------------------------------------------------------------------------------------------
// Engine window lifecycle (PERSISTENCE.md 13.5 / Correctness Rule #16): the open-at-exit
// manifest's SKIP-EMPTY rule, claim-by-id vs the no-arg front-pop, the reclaimable pool, the
// Manager-only record deletion, and the race-safe Manager-only close reservation. Runs on LOCAL
// Engine instances through the `...In` seams (Engine.h) — NEVER SharedEngine(): its first access
// wires + STARTS the bridge (which would collide with TestBridgeRoundTrip's pipe on the same
// `agentmaster.<pid>` name), the observer, and the scheduler. All disk I/O (windows/<id>.json +
// open-windows.json) lands in the harness's scratch profile (AGENTMASTER_PROFILE — exported by
// run-m5-tests.bat; wmain self-defaults it for a direct exe run).
// ---------------------------------------------------------------------------------------------
void TestEngineWindowLifecycle()
{
    std::wprintf(L"Engine window lifecycle (claim / manifest skip-empty / reclaim / reserve):\n");

    const std::wstring idA = L"__m5eng_a__";
    const std::wstring idB = L"__m5eng_b__";
    const std::wstring idC = L"__m5eng_c__";
    auto wipe = [&] {
        DeleteWindowRecord(idA);
        DeleteWindowRecord(idB);
        DeleteWindowRecord(idC);
    };
    wipe(); // stale residue from a crashed earlier run

    auto mkRec = [](const std::wstring& id, bool withTab) {
        WindowRecord r;
        r.windowId = id;
        r.geometry.hasSize = true;
        r.geometry.width = 800;
        r.geometry.height = 600;
        if (withTab)
        {
            TabEntry t;
            t.kind = TabKind::Claude;
            t.sessionId = L"conv-" + id;
            r.tabs.push_back(t);
        }
        return r;
    };
    auto has = [](const std::vector<std::wstring>& v, const std::wstring& id) {
        for (const auto& x : v)
        {
            if (x == id)
            {
                return true;
            }
        }
        return false;
    };

    // A + B carry a tab ref (content-full); C held only the pinned Manager tab (no tab refs).
    SaveWindowRecord(mkRec(idA, true));
    SaveWindowRecord(mkRec(idB, true));
    SaveWindowRecord(mkRec(idC, false));

    // --- claim-by-id draws from the startup pool exactly once; unknown/empty ids miss ---
    {
        Engine e;
        auto a = ClaimWindowRecordIn(e, idA);
        CHECK(a && a->windowId == idA && a->tabs.size() == 1, "claim by id returns the saved record");
        CHECK(!ClaimWindowRecordIn(e, idA).has_value(), "second claim of the same id misses (single owner)");
        CHECK(!ClaimWindowRecordIn(e, L"__m5eng_none__").has_value(), "unknown id misses (window mints fresh)");
        CHECK(!ClaimWindowRecordIn(e, L"").has_value(), "empty id misses");
    }

    // --- the open-at-exit manifest: register writes the live set, unregister prunes a closed
    //     window, and SKIP-EMPTY preserves the final snapshot (Rule #16's "closed slowly" rule) ---
    {
        Engine e;
        RegisterLiveWindowIn(e, idA);
        RegisterLiveWindowIn(e, idB);
        auto m = LoadOpenWindows();
        CHECK(has(m, idA) && has(m, idB), "manifest carries both live windows");
        CHECK(LiveWindowIdsIn(e).size() == 2, "live-id snapshot has both");
        UnregisterLiveWindowIn(e, idA);
        m = LoadOpenWindows();
        CHECK(!has(m, idA) && has(m, idB), "a closed window is pruned from the manifest (not re-offered)");
        UnregisterLiveWindowIn(e, idB); // would EMPTY the manifest
        m = LoadOpenWindows();
        CHECK(has(m, idB), "skip-empty: the LAST unregister keeps the final open-at-exit snapshot");
    }

    // --- the reclaimable pool: a claimed-then-closed record re-claims BY ID (real id + lens);
    //     the no-arg front-pop NEVER draws from it (a plain "+ new window" must not adopt a
    //     closed window's layout); a double teardown pools exactly ONE copy ---
    {
        Engine e;
        auto a = ClaimWindowRecordIn(e, idA); // startup-pool claim (loads windows/*.json once)
        CHECK(a.has_value(), "reclaim scenario: startup claim");
        RegisterLiveWindowIn(e, idA);
        RegisterLiveWindowIn(e, idB);
        UnregisterLiveWindowIn(e, idA); // A closes mid-session -> its record returns to the pool
        auto a2 = ClaimWindowRecordIn(e, idA);
        CHECK(a2 && a2->windowId == idA, "closed-this-session record re-claims by id (recover-button path)");
        UnregisterLiveWindowIn(e, idA); // back to the pool...
        UnregisterLiveWindowIn(e, idA); // ...and a double teardown must not stack a second copy
        while (ClaimWindowRecordIn(e).has_value())
        {
            // drain the startup pool (front-pop) so only the reclaimable pool remains
        }
        CHECK(!ClaimWindowRecordIn(e).has_value(), "front-pop never draws from the reclaimable pool");
        CHECK(ClaimWindowRecordIn(e, idA).has_value(), "...but the by-id claim finds the closed record");
        CHECK(!ClaimWindowRecordIn(e, idA).has_value(), "dedup: the double teardown pooled exactly one copy");
    }

    // --- Manager-only record deletion: a MID-SESSION-closed window whose record has NO tab refs
    //     is deleted from disk (reopening it reconstructs "+ new window" = noise); the LAST window
    //     out KEEPS its record — the open-at-exit geometry snapshot the next launch claims ---
    {
        Engine e;
        RegisterLiveWindowIn(e, idB);
        RegisterLiveWindowIn(e, idC);
        UnregisterLiveWindowIn(e, idC); // C: no tabs + another window remains -> record deleted
        CHECK(!LoadWindowRecord(idC).has_value(), "mid-session-closed Manager-only record is deleted from disk");
        UnregisterLiveWindowIn(e, idB); // B: last one out (live set empties) -> record kept
        CHECK(LoadWindowRecord(idB).has_value(), "the LAST window out keeps its record (open-at-exit snapshot)");
    }

    // --- RecoverableWindows = on-disk records − live − content-less, each indexed by its
    //     CANONICAL LoadWindowRecords position (the `-s <idx>` the reopen dispatch uses) ---
    {
        Engine e;
        SaveWindowRecord(mkRec(idC, false)); // re-seed the Manager-only record
        RegisterLiveWindowIn(e, idB); // B is open
        const auto rec = RecoverableWindowsIn(e);
        bool offersA = false, offersB = false, offersC = false;
        int idxA = -1;
        for (const auto& r : rec)
        {
            if (r.record.windowId == idA)
            {
                offersA = true;
                idxA = r.index;
            }
            offersB = offersB || r.record.windowId == idB;
            offersC = offersC || r.record.windowId == idC;
        }
        CHECK(offersA, "recoverable: a not-open, content-full record is offered");
        CHECK(!offersB, "recoverable: a live window is never offered");
        CHECK(!offersC, "recoverable: a Manager-only (no-tabs) record is never offered");
        const auto all = LoadWindowRecords();
        int want = -1;
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
        {
            if (all[i].windowId == idA)
            {
                want = i;
            }
        }
        CHECK(want >= 0 && idxA == want, "recoverable index == canonical LoadWindowRecords position (-s <idx>)");
    }

    // --- ReserveManagerOnlyClose: N windows emptying at once can never ALL close — the last
    //     effective window stays (remaining==1), and a completed close clears its reservation ---
    {
        Engine e;
        RegisterLiveWindowIn(e, idA);
        RegisterLiveWindowIn(e, idB);
        RegisterLiveWindowIn(e, idC);
        CHECK(ReserveManagerOnlyCloseIn(e, idA), "reserve: 1st of 3 may close");
        CHECK(ReserveManagerOnlyCloseIn(e, idB), "reserve: 2nd of 3 may close (one would remain)");
        CHECK(!ReserveManagerOnlyCloseIn(e, idC), "reserve: the last effective window must STAY");
        UnregisterLiveWindowIn(e, idA); // the close completes -> its reservation clears with it
        CHECK(!ReserveManagerOnlyCloseIn(e, idC), "reserve: still last-effective after A finished closing");
        UnregisterLiveWindowIn(e, idB);
        CHECK(!ReserveManagerOnlyCloseIn(e, idC), "reserve: a lone window can never reserve");
        CHECK(!ReserveManagerOnlyCloseIn(e, L""), "reserve: empty id refused");
    }

    wipe(); // leave no records behind (the scratch profile is wiped per bat run anyway)
}
