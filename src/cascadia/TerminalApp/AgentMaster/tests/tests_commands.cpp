// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — COMMANDS.md tests: the slash-command binding/await infrastructure.
//   * ParseCommandEcho / IsMarkdownPath / IsAbsolutePathForWatch / PickMarkdownWritePath (pure)
//   * ParseTranscriptDelta's Command events + assistant fileWritePaths (both echo strata, real
//     corpus shapes) — and that a command echo stays a NON-event for the state machine
//   * the CommandWatch state machine (arm / match / disk-await / FIFO / turn-end + deadline
//     expiry / freshness replay guard / per-session cap / DropSession), injected file probe
//   * DeriveSuffixedTitle (the generalized fork-title derivation the /handover successor shares)
//   * BuildClaudeCommandline's initial-prompt positional arg + PsDoubleQuote
//   * EnsureHandoverCommandFileIn (create-if-absent under a temp config dir — never the real one)

#include "m5_tests.h"

#include "../CommandWatch.h"
#include "../SessionScanner.h" // ParseTranscriptDelta / TranscriptEvent

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace Agentmaster;

namespace
{
    // A minimal registry of fired handovers the handler under test records into.
    struct FiredHandover
    {
        std::wstring sessionId;
        std::wstring mdPath;
        std::wstring args;
    };
}

void TestCommandWatch()
{
    std::wprintf(L"CommandWatch (slash-command bindings, COMMANDS.md):\n");

    // ---- ParseCommandEcho: the CURRENT user-line shape (2026-07 corpus — name tag FIRST) ----
    {
        SlashCommand sc;
        const bool ok = ParseCommandEcho(L"<command-name>/handover</command-name>\n            <command-message>handover</command-message>\n            <command-args>wrap up the observer work</command-args>", sc);
        CHECK(ok, "current (name-first) echo parses");
        CHECK(sc.name == L"handover", "leading slash stripped, name extracted");
        CHECK(sc.args == L"wrap up the observer work", "args extracted verbatim");
    }
    // The OLDER custom-command shape (June-2026 corpus — message tag first) parses identically.
    {
        SlashCommand sc;
        const bool ok = ParseCommandEcho(L"<command-message>compress</command-message>\n<command-name>/compress</command-name>\n<command-args>We will go over the design we made</command-args>", sc);
        CHECK(ok && sc.name == L"compress", "older (message-first) echo parses (order-agnostic)");
        CHECK(sc.args == L"We will go over the design we made", "older shape args extracted");
    }
    // Uppercase + no-args + surrounding whitespace: name lowercased, args empty.
    {
        SlashCommand sc;
        CHECK(ParseCommandEcho(L"<command-name> /Handover </command-name>", sc), "no-args echo parses");
        CHECK(sc.name == L"handover", "name trimmed + ASCII-lowercased");
        CHECK(sc.args.empty(), "absent <command-args> -> empty args");
    }
    // Multi-line args survive verbatim (outer-trimmed) — the /compact corpus shape carries \r\n.
    {
        SlashCommand sc;
        CHECK(ParseCommandEcho(L"<command-name>/compact</command-name>\n<command-args>line one\r\nline two</command-args>", sc), "multi-line args parse");
        CHECK(sc.args == L"line one\r\nline two", "args keep interior newlines (outer-trimmed only)");
    }
    // Not a command echo -> false; an empty name -> false.
    {
        SlashCommand sc;
        CHECK(!ParseCommandEcho(L"just a normal prompt mentioning <command-args>", sc), "no command-name tag -> false");
        CHECK(!ParseCommandEcho(L"<command-name>/</command-name>", sc), "empty (slash-only) name -> false");
        CHECK(!ParseCommandEcho(L"", sc), "empty text -> false");
    }

    // ---- IsMarkdownPath / IsAbsolutePathForWatch / PickMarkdownWritePath ----
    {
        CHECK(IsMarkdownPath(L"K:\\repo\\HANDOVER-obs.md"), "md path recognized");
        CHECK(IsMarkdownPath(L"notes.MD"), "extension case-insensitive");
        CHECK(!IsMarkdownPath(L"K:\\repo\\file.txt"), "non-md rejected");
        CHECK(!IsMarkdownPath(L"md"), "bare 'md' (no dot) rejected");
        CHECK(IsAbsolutePathForWatch(L"K:\\x\\y.md"), "drive-rooted is absolute");
        CHECK(IsAbsolutePathForWatch(L"\\\\srv\\share\\y.md"), "UNC is absolute");
        CHECK(!IsAbsolutePathForWatch(L"docs\\y.md"), "relative is not absolute");
        const std::vector<std::wstring> paths{ L"K:\\r\\notes.md", L"K:\\r\\code.cpp", L"K:\\r\\HANDOVER-topic.md" };
        CHECK(PickMarkdownWritePath(paths, L"handover") == L"K:\\r\\HANDOVER-topic.md", "leaf preference outranks an earlier incidental md");
        CHECK(PickMarkdownWritePath(paths, L"") == L"K:\\r\\notes.md", "no preference -> first md");
        CHECK(PickMarkdownWritePath({ L"K:\\r\\code.cpp" }, L"handover").empty(), "no md in batch -> empty");
    }

    // ---- ParseTranscriptDelta: Command events (both strata) + assistant fileWritePaths ----
    {
        // The CURRENT user-line echo (real corpus shape, 2026-07): -> ONE Command event, NOT a
        // UserPrompt (the state machine must keep seeing nothing — the /model false-Running fix).
        const std::wstring line = LR"j({"type":"user","message":{"role":"user","content":"<command-name>/handover</command-name>\n            <command-message>handover</command-message>\n            <command-args>ship the watch</command-args>"},"timestamp":"2026-07-20T10:00:00.000Z","uuid":"u1","sessionId":"s"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1, "command echo -> exactly 1 event");
        CHECK(!r.events.empty() && r.events[0].kind == TranscriptEvent::Kind::Command, "command echo -> Kind::Command (never UserPrompt)");
        CHECK(!r.events.empty() && r.events[0].commandName == L"handover", "Command event carries the bare name");
        CHECK(!r.events.empty() && r.events[0].commandArgs == L"ship the watch", "Command event carries the args");
        CHECK(!r.events.empty() && r.events[0].lineTsMs > 0, "Command event carries the line's own timestamp (the replay guard)");
    }
    {
        // The OLDER system/local_command stratum -> the same Command event.
        const std::wstring line = LR"j({"type":"system","subtype":"local_command","content":"<command-name>/fork</command-name>\n            <command-message>fork</command-message>\n            <command-args>search online</command-args>","timestamp":"2026-06-13T15:15:38.295Z","uuid":"u2"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        bool sawCommand = false;
        for (const auto& ev : r.events)
        {
            if (ev.kind == TranscriptEvent::Kind::Command)
            {
                sawCommand = ev.commandName == L"fork" && ev.commandArgs == L"search online" && ev.lineTsMs > 0;
            }
        }
        CHECK(sawCommand, "system/local_command stratum -> Command event (name+args+ts)");
    }
    {
        // A NON-command noise line (injected reminder) still emits nothing — unchanged behavior.
        const std::wstring line = LR"j({"type":"user","message":{"role":"user","content":"<system-reminder>ignore me</system-reminder>"},"timestamp":"2026-07-20T10:00:00.000Z"})j" L"\n";
        CHECK(ParseTranscriptDelta(line).events.empty(), "non-command noise user line still emits no events");
    }
    {
        // Assistant Write/Edit tool_use -> fileWritePaths (block order); Read never collects.
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read","input":{"file_path":"K:\\r\\a.md"}},{"type":"tool_use","name":"Write","input":{"file_path":"K:\\r\\HANDOVER-x.md","content":"..."}},{"type":"tool_use","name":"Edit","input":{"file_path":"K:\\r\\b.md"}}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::Assistant, "tool_use message -> 1 assistant event");
        CHECK(!r.events.empty() && r.events[0].fileWritePaths.size() == 2, "Write+Edit collected, Read not");
        CHECK(!r.events.empty() && r.events[0].fileWritePaths.size() == 2 && r.events[0].fileWritePaths[0] == L"K:\\r\\HANDOVER-x.md" && r.events[0].fileWritePaths[1] == L"K:\\r\\b.md", "paths in block order");
    }

    // ---- the CommandWatch state machine (injected probe; fabricated clock) ----
    const int64_t now = 1'700'000'000'000;
    const auto freshTs = now - 2'000; // well inside kCommandSightingFreshMs

    // arm -> match -> file present -> fires once, with args + the resolved path.
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::wstring& md, const std::wstring& args) {
            fired.push_back({ sid, md, args });
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"sess-1", SlashCommand{ L"handover", L"finish the docs" }, freshTs, now);
        CHECK(w.PendingCount() == 1, "fresh sighting arms one pending");
        w.OnFileToolWrite(L"sess-1", { L"K:\\r\\HANDOVER-docs.md" }, L"K:\\r", now);
        CHECK(fired.size() == 1, "matched write with the file on disk fires exactly once");
        CHECK(!fired.empty() && fired[0].sessionId == L"sess-1" && fired[0].mdPath == L"K:\\r\\HANDOVER-docs.md", "fired with the session + path");
        CHECK(!fired.empty() && fired[0].args == L"finish the docs", "fired with the command's args");
        CHECK(w.PendingCount() == 0, "fired pending consumed");
        w.OnFileToolWrite(L"sess-1", { L"K:\\r\\HANDOVER-docs.md" }, L"K:\\r", now);
        CHECK(fired.size() == 1, "a later write with no pending fires nothing (never twice)");
    }
    // file NOT yet on disk -> stays pending; a later Tick with the file present fires.
    {
        CommandWatch w;
        int firedCount = 0;
        bool filePresent = false;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::wstring&, const std::wstring&) { ++firedCount; });
        w.SetFileProbe([&](const std::wstring&) { return filePresent; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-a.md" }, L"K:\\r", now);
        CHECK(firedCount == 0 && w.PendingCount() == 1, "matched but absent file -> still pending (approval-gated write)");
        w.OnTurnEnd(L"s");
        w.OnTurnEnd(L"s");
        CHECK(w.PendingCount() == 1, "a MATCHED pending never expires by turn-ends (deadline-bounded only)");
        filePresent = true;
        w.Tick(now + 5'000);
        CHECK(firedCount == 1 && w.PendingCount() == 0, "Tick fires the moment the file lands");
    }
    // freshness replay guard: an old line timestamp / a missing one never arms.
    {
        CommandWatch w;
        w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::wstring&, const std::wstring&) {});
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, now - kCommandSightingFreshMs - 1, now);
        CHECK(w.PendingCount() == 0, "stale line timestamp never arms (history replay guard)");
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, 0, now);
        CHECK(w.PendingCount() == 0, "absent line timestamp never arms");
        w.OnCommandSighting(L"s", SlashCommand{ L"model", L"" }, freshTs, now);
        CHECK(w.PendingCount() == 0, "an unbound command (/model) never arms");
    }
    // relative tool path resolves against the session cwd.
    {
        CommandWatch w;
        std::wstring firedPath;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::wstring& md, const std::wstring&) { firedPath = md; });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"HANDOVER-rel.md" }, L"K:\\repo", now);
        CHECK(firedPath == L"K:\\repo\\HANDOVER-rel.md", "relative write path resolved against the session cwd");
    }
    // FIFO: two sightings, two writes — the first write satisfies the OLDER pending.
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::wstring& md, const std::wstring& args) {
            fired.push_back({ sid, md, args });
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"first" }, freshTs, now);
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"second" }, freshTs, now);
        CHECK(w.PendingCount() == 2, "two sightings arm two pendings");
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-1.md" }, L"K:\\r", now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-2.md" }, L"K:\\r", now);
        CHECK(fired.size() == 2, "both pendings satisfied");
        CHECK(fired.size() == 2 && fired[0].args == L"first" && fired[0].mdPath == L"K:\\r\\HANDOVER-1.md", "FIFO: first write -> the older sighting");
        CHECK(fired.size() == 2 && fired[1].args == L"second" && fired[1].mdPath == L"K:\\r\\HANDOVER-2.md", "FIFO: second write -> the newer sighting");
    }
    // turn-end expiry for UNMATCHED pendings (the command turn + one clarification round).
    {
        CommandWatch w;
        w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::wstring&, const std::wstring&) {});
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnTurnEnd(L"s");
        CHECK(w.PendingCount() == 1, "one turn-end: still armed (a clarification round is allowed)");
        w.OnTurnEnd(L"s");
        CHECK(w.PendingCount() == 0, "second turn-end with no md: expired (never grabs a later unrelated md)");
    }
    // deadline expiry + DropSession + the per-session cap.
    {
        CommandWatch w;
        w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::wstring&, const std::wstring&) {});
        w.SetFileProbe([](const std::wstring&) { return false; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.Tick(now + kCommandAwaitDeadlineMs);
        CHECK(w.PendingCount() == 0, "deadline sweeps an unresolved pending");
        w.OnCommandSighting(L"s2", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.DropSession(L"s2");
        CHECK(w.PendingCount() == 0, "DropSession clears the session's pendings");
        for (int i = 0; i < 6; ++i)
        {
            w.OnCommandSighting(L"s3", SlashCommand{ L"handover", L"" }, freshTs, now);
        }
        CHECK(w.PendingCount() == kCommandMaxPendingPerSession, "per-session cap bounds pendings (oldest evicted)");
    }

    // ---- DeriveSuffixedTitle (the generalized fork-title derivation) ----
    {
        CHECK(DeriveSuffixedTitle(L"Obs", L"handover") == L"Obs (handover)", "first handover appends the group");
        CHECK(DeriveSuffixedTitle(L"Obs (handover)", L"handover") == L"Obs (handover 2)", "handover of a handover bumps");
        CHECK(DeriveSuffixedTitle(L"Obs (handover 2)", L"handover") == L"Obs (handover 3)", "numbered group keeps bumping");
        CHECK(DeriveSuffixedTitle(L"Obs (fork)", L"handover") == L"Obs (fork) (handover)", "a DIFFERENT word's trailing group is left intact");
        CHECK(DeriveSuffixedTitle(L"Obs (1.0)", L"handover") == L"Obs (1.0) (handover)", "a non-matching trailer is left intact");
        CHECK(DeriveForkTitle(L"Obs") == L"Obs (fork)", "DeriveForkTitle delegates (fork parity)");
        CHECK(DeriveForkTitle(L"Obs (fork)") == L"Obs (fork 2)", "DeriveForkTitle bump parity");
    }

    // ---- BuildClaudeCommandline initial prompt + PsDoubleQuote ----
    {
        CHECK(PsDoubleQuote(L"plain text") == L"\"plain text\"", "PsDoubleQuote wraps");
        CHECK(PsDoubleQuote(L"a$b`c\"d") == L"\"a`$b``c`\"d\"", "PsDoubleQuote backtick-escapes $, `, and \"");
        const std::wstring prompt = L"Read the handover document at K:\\r\\HANDOVER-x.md and continue the work it describes.";
        const auto base = BuildClaudeCommandline(L"C:/p/settings.json", L"sid-1", false, true, {}, L"C:\\bin\\claude.exe", {});
        const auto with = BuildClaudeCommandline(L"C:/p/settings.json", L"sid-1", false, true, {}, L"C:\\bin\\claude.exe", {}, prompt);
        CHECK(with == base + L" \"" + prompt + L"\"", "initial prompt strictly appends LAST as a quoted positional (base untouched)");
        CHECK(BuildClaudeCommandline(L"C:/p/s.json", L"sid", false, true) == BuildClaudeCommandline(L"C:/p/s.json", L"sid", false, true, {}, {}, {}, {}), "empty initial prompt is byte-identical to the pre-parameter form");
    }

    // ---- EnsureHandoverCommandFileIn: create-if-absent under a TEMP config dir ----
    {
        wchar_t tmp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring cfg = std::wstring{ tmp } + L"am-cmdwatch-test-cfg";
        // wipe from a previous run
        ::DeleteFileW((cfg + L"\\commands\\handover.md").c_str());
        ::RemoveDirectoryW((cfg + L"\\commands").c_str());
        ::RemoveDirectoryW(cfg.c_str());
        const auto p1 = EnsureHandoverCommandFileIn(cfg);
        CHECK(!p1.empty() && ::GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES, "absent -> command definition created");
        {
            std::ifstream f(std::filesystem::path{ p1 }, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(body.find("HANDOVER-") != std::string::npos && body.find("Write tool") != std::string::npos, "definition instructs a Write-tool HANDOVER-*.md (the await's signal)");
        }
        // A user edit is NEVER overwritten (create-if-absent).
        {
            std::ofstream f(std::filesystem::path{ p1 }, std::ios::binary | std::ios::trunc);
            f << "user-owned";
        }
        const auto p2 = EnsureHandoverCommandFileIn(cfg);
        CHECK(p2 == p1, "present -> same path returned");
        {
            std::ifstream f(std::filesystem::path{ p1 }, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(body == "user-owned", "an existing (user-edited) definition is never overwritten");
        }
        ::DeleteFileW(p1.c_str());
        ::RemoveDirectoryW((cfg + L"\\commands").c_str());
        ::RemoveDirectoryW(cfg.c_str());
    }
}
