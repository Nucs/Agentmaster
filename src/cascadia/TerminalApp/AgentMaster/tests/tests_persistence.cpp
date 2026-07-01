// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The 38 tests + 2 benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all 40 test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration
// ★ tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: persistence tests. Shared CHECK/fixtures/decls
// live in m5_tests.h; the runner (m5_tests.cpp) calls each entry point. See run-m5-tests.bat.
#include "m5_tests.h"

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
        const auto back = DeserializeSessions(SerializeSessions({ cx, claudeDefault }));
        CHECK(back.size() == 2, "codex+claude sessions round-trip count");
        if (back.size() == 2)
        {
            CHECK(back[0].kind == AgentKind::Codex, "codex session kind preserved");
            CHECK(back[0].codexSessionId == L"019ec0c7-a4e3-7c73-8c57-9f83ecb1903a", "codex resume uuid (codexSessionId) preserved");
            CHECK(back[1].kind == AgentKind::Claude && back[1].codexSessionId.empty(), "default session is Claude with no codex uuid");
            CHECK(back[0].forkParentId.empty() && back[1].forkParentId.empty(), "a non-fork session carries no forkParentId (key omitted when empty)");
        }
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
        in.includeCoAuthoredBy = false;
        in.defaultAutorunnerMode = AutorunnerMode::Full;
        in.maxAutoSends = 7;
        in.stopOnError = false;
        in.pauseOnHumanInput = false;
        in.confirmBeforeKill = false;
        in.tabRenameCommitMode = TabRenameCommitMode::ClickAwayOrEnter; // non-default (default is ClickAwayOrShiftEnter)
        in.favoriteIcon = FavoriteIcon::Star; // non-default (default is Crown)
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
        in.waitingForYouTimeoutMinutes = 0; // 0 = never decay — MUST round-trip as 0, not fall back to the default
        in.serverCacheMinutes = 17; // non-default (default 5) — the ⚡ "still cached" window
        in.treeSort = ExplorerSort::ByPid; // non-default (default Newest) — Explorer Tree sort
        in.boardSort = ExplorerSort::Newest; // non-default (default MostActive) — Triage Board sort
        in.autoTestingShowsSummary = false; // non-default (default true = Summary) — Manager Auto-Testing pane tab
        in.hiddenSessionIds = { L"11111111-1111-1111-1111-111111111111", L"22222222-2222-2222-2222-222222222222" };
        const auto out = DeserializeAppSettings(SerializeAppSettings(in));
        CHECK(out.skipPermissions == false, "settings skipPermissions round-trip");
        CHECK(out.env == L"FOO=bar;BAZ=qux", "settings env round-trip");
        CHECK(out.model == L"opus", "settings model round-trip");
        CHECK(out.includeCoAuthoredBy == false, "settings includeCoAuthoredBy round-trip");
        CHECK(out.defaultAutorunnerMode == AutorunnerMode::Full, "settings defaultAutorunnerMode round-trip");
        CHECK(out.maxAutoSends == 7u, "settings maxAutoSends round-trip");
        CHECK(out.stopOnError == false, "settings stopOnError round-trip");
        CHECK(out.pauseOnHumanInput == false, "settings pauseOnHumanInput round-trip");
        CHECK(out.confirmBeforeKill == false, "settings confirmBeforeKill round-trip");
        CHECK(out.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrEnter, "settings tabRenameCommitMode round-trip");
        CHECK(out.favoriteIcon == FavoriteIcon::Star, "settings favoriteIcon round-trip");
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
        CHECK(out.waitingForYouTimeoutMinutes == 0u, "settings waitingForYouTimeoutMinutes stored 0 (= never) round-trips as 0");
        CHECK(out.serverCacheMinutes == 17u, "settings serverCacheMinutes round-trip");
        CHECK(out.treeSort == ExplorerSort::ByPid, "settings treeSort round-trip");
        CHECK(out.boardSort == ExplorerSort::Newest, "settings boardSort round-trip");
        CHECK(out.autoTestingShowsSummary == false, "settings autoTestingShowsSummary round-trip");
        CHECK(out.hiddenSessionIds.size() == 2 &&
                  out.hiddenSessionIds[0] == L"11111111-1111-1111-1111-111111111111" &&
                  out.hiddenSessionIds[1] == L"22222222-2222-2222-2222-222222222222",
              "settings hiddenSessionIds round-trip (order preserved)");
    }

    // Empty / garbage -> all defaults (a missing settings.json must change nothing).
    {
        const auto out = DeserializeAppSettings(L"");
        CHECK(out.skipPermissions == true && out.includeCoAuthoredBy == true, "settings defaults on empty");
        CHECK(out.defaultAutorunnerMode == AutorunnerMode::Full && out.maxAutoSends == 100u, "settings autorunner default Full on empty");
        CHECK(out.archiveSplitFraction > 0.499 && out.archiveSplitFraction < 0.501, "settings archiveSplitFraction default 0.5 on empty");
        CHECK(out.summaryPanelWidthFraction == 0.0 && out.summaryPanelHeightFraction == 0.0, "settings summaryPanel size fractions default 0 (auto) on empty");
        CHECK(out.summaryPanelWrapNewlines == false, "settings summaryPanelWrapNewlines default false (literal-\\n look) on empty");
        CHECK(out.summaryPanelTruncate == true, "settings summaryPanelTruncate default true (truncate) on empty");
        CHECK(out.showTabCloseButton == true, "settings showTabCloseButton default true (show X) on empty");
        CHECK(out.closeTabOnMiddleClick == true, "settings closeTabOnMiddleClick default true (middle-click closes) on empty");
        CHECK(out.waitingForYouTimeoutMinutes == 4320u, "settings waitingForYouTimeoutMinutes default 4320 (3d Waiting-for-you timeout) on empty");
        CHECK(out.serverCacheMinutes == 5u, "settings serverCacheMinutes default 5 (server cache lifetime) on empty");
        CHECK(out.tabRenameCommitMode == TabRenameCommitMode::ClickAwayOrShiftEnter, "settings tabRenameCommitMode default (Shift+Enter) on empty");
        CHECK(out.favoriteIcon == FavoriteIcon::Crown, "settings favoriteIcon default (Crown) on empty");
        CHECK(out.flashRingColor == L"#CCFF0000", "settings flashRingColor default (80% red) on empty");
        CHECK(out.pendingDotsLightColor == L"#FFE0A92B", "settings pendingDotsLightColor default (gold, on dark) on empty");
        CHECK(out.pendingDotsDarkColor == L"#FF5A3E00", "settings pendingDotsDarkColor default (amber, on light) on empty");
        CHECK(out.treeSort == ExplorerSort::Newest, "settings treeSort default (Newest) on empty");
        CHECK(out.boardSort == ExplorerSort::MostActive, "settings boardSort default (MostActive) on empty");
        CHECK(out.autoTestingShowsSummary == true, "settings autoTestingShowsSummary default (Summary) on empty");
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

    // A present subset is honored; the rest keep defaults.
    {
        const auto out = DeserializeAppSettings(L"{\"settings\":{\"model\":\"sonnet\",\"maxAutoSends\":3}}");
        CHECK(out.model == L"sonnet", "settings present model honored");
        CHECK(out.maxAutoSends == 3u, "settings present maxAutoSends honored");
        CHECK(out.skipPermissions == true, "settings missing skipPermissions -> default");
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

    // --- DeriveSessionTitle: walk up past generic segments, then length/case rules ---
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp") == L"NumSharp", "short non-generic leaf as-is");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\bin\\Debug") == L"NumSharp", "walk up past bin/Debug");
    CHECK(DeriveSessionTitle(L"K:\\proj\\obj\\x64\\Release") == L"proj", "walk up past obj/x64/Release");
    CHECK(DeriveSessionTitle(L"K:\\source\\NumSharp\\") == L"NumSharp", "trailing slash tolerated");
    CHECK(DeriveSessionTitle(L"K:/source/NumSharp") == L"NumSharp", "forward slashes tolerated");
    CHECK(DeriveSessionTitle(L"C:\\bin") == L"bin", "all-generic falls back to the leaf");
    CHECK(DeriveSessionTitle(L"") == L"claude", "empty dir -> claude");

    // length rules
    CHECK(DeriveSessionTitle(L"C:\\x\\MyProject") == L"MyProject", "<=16 used as-is (mixed case)");
    CHECK(DeriveSessionTitle(L"C:\\x\\abcdefghijklmnop") == L"abcdefghijklmnop", "16 chars used as-is");
    CHECK(DeriveSessionTitle(L"C:\\x\\MyVeryLongProjectName") == L"MVLPN", ">16 mixed-case -> capitals only");
    CHECK(DeriveSessionTitle(L"C:\\x\\myreasonablylongname") == L"myreasonablylongname", ">16 all-lower <=30 as-is");
    {
        const std::wstring leaf(35, L'a');
        CHECK(DeriveSessionTitle(L"C:\\x\\" + leaf) == std::wstring(30, L'a') + L"...", ">30 all-lower truncated with ...");
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

