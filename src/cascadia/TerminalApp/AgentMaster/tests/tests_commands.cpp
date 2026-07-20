// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — COMMANDS.md tests: the slash-command binding/await infrastructure.
//   * ParseCommandEcho / IsMarkdownPath / IsAbsolutePathForWatch / PickMarkdownWritePath (pure)
//   * ParseTranscriptDelta's Command events + assistant fileWritePaths (both echo strata, real
//     corpus shapes) — and that a command echo stays a NON-event for the state machine
//   * the CommandWatch state machine (arm / MULTI-FILE collect + seal-at-turn-end + settle
//     fallback / disk-await / FIFO / turn-end + deadline expiry / freshness replay guard /
//     per-session cap / same-echo idempotence / DropSession), injected file probe
//   * durable per-session progress (COMMANDS.md §3a): Encode/DecodeCommandProgress, the fired
//     watermark (no re-fire across a restart/replay), armed-marker revival (a mid-await command
//     survives a restart, deadline-anchored at its original stamp), past-deadline marker pruning
//   * DeriveSuffixedTitle (the generalized fork-title derivation the /handover successor shares)
//   * BuildClaudeCommandline's initial-prompt positional arg + PsDoubleQuote
//   * EnsureHandoverCommandFileIn (create-if-absent under a temp config dir — never the real one)
//   * safeguard belts (COMMANDS.md §7): IsSaneWatchPath + insane-match rejection, a throwing
//     handler swallowed per fire, a throwing probe reading as absent then recovering
//   * TestCommandHandoverE2E — the FABRICATED expected-behavior /handover session (real ISO
//     timestamps) through the REAL parser + the _readDelta feed mapping + the DEFAULT disk probe:
//     happy path / clarification round / no-md expiry / two handovers in one conversation /
//     stale restart replay / chunked scanner-style parse equivalence
//   * TestCommandEchoRealCorpus — REAL ~/.claude transcripts replayed (guarded, [info]-skips):
//     every echo -> a Command event, zero turn-event leaks, Write lines yield file_path

#include "m5_tests.h"

#include "../CommandWatch.h"
#include "../SessionScanner.h" // ParseTranscriptDelta / TranscriptEvent

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>

using namespace Agentmaster;

namespace
{
    // A minimal registry of fired handovers the handler under test records into. A fire carries a
    // PATH SET (multi-file, COMMANDS.md §5); mdPath keeps the first for the single-file common
    // case's assertions, mdPaths holds the whole set for the multi-file ones.
    struct FiredHandover
    {
        std::wstring sessionId;
        std::wstring mdPath;
        std::wstring args;
        std::vector<std::wstring> mdPaths;
    };

    // The vector-handler capture shared by the tests: record (sid, first path, args, all paths).
    FiredHandover MakeFired(const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args)
    {
        return FiredHandover{ sid, mds.empty() ? std::wstring{} : mds.front(), args, mds };
    }
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

    // arm -> match -> SEAL at turn end -> file present -> fires once, with args + the path.
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"sess-1", SlashCommand{ L"handover", L"finish the docs" }, freshTs, now);
        CHECK(w.PendingCount() == 1, "fresh sighting arms one pending");
        w.OnFileToolWrite(L"sess-1", { L"K:\\r\\HANDOVER-docs.md" }, L"K:\\r", now);
        CHECK(fired.empty() && w.PendingCount() == 1, "a matched write alone does NOT fire yet (the collection stays open until the turn ends)");
        w.OnTurnEnd(L"sess-1");
        CHECK(fired.size() == 1, "the turn end SEALS the collection and fires (file already on disk)");
        CHECK(!fired.empty() && fired[0].sessionId == L"sess-1" && fired[0].mdPath == L"K:\\r\\HANDOVER-docs.md", "fired with the session + path");
        CHECK(!fired.empty() && fired[0].mdPaths.size() == 1, "single-file command fires with a one-path set");
        CHECK(!fired.empty() && fired[0].args == L"finish the docs", "fired with the command's args");
        CHECK(w.PendingCount() == 0, "fired pending consumed");
        w.OnFileToolWrite(L"sess-1", { L"K:\\r\\HANDOVER-docs.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"sess-1");
        CHECK(fired.size() == 1, "a later write with no pending fires nothing (never twice)");
    }
    // MULTI-FILE (COMMANDS.md §5): one command writes SEVERAL HANDOVER-*.md files — every
    // hint-matching markdown after the command is COLLECTED (across batches, deduped, an
    // incidental non-hint md never rides along) and the turn end fires ONE handover with all of
    // them, in write order.
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"split briefing" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-part1.md" }, L"K:\\r", now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\notes.md", L"K:\\r\\HANDOVER-part2.md" }, L"K:\\r", now); // the incidental notes.md must NOT ride along
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-part1.md" }, L"K:\\r", now); // an Edit of part1 again — dedup, not a third file
        CHECK(fired.empty() && w.PendingCount() == 1, "collection stays open (no fire) until the turn ends");
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 1, "ONE fire for the whole multi-file command");
        CHECK(!fired.empty() && (fired[0].mdPaths == std::vector<std::wstring>{ L"K:\\r\\HANDOVER-part1.md", L"K:\\r\\HANDOVER-part2.md" }), "all HANDOVER files collected in write order, deduped, incidental md excluded");
        CHECK(!fired.empty() && fired[0].args == L"split briefing", "args ride the multi-file fire");
    }
    // file NOT yet on disk -> stays pending; a later Tick with the file present fires.
    {
        CommandWatch w;
        int firedCount = 0;
        bool filePresent = false;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++firedCount; });
        w.SetFileProbe([&](const std::wstring&) { return filePresent; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-a.md" }, L"K:\\r", now);
        CHECK(firedCount == 0 && w.PendingCount() == 1, "matched but absent file -> still pending (approval-gated write)");
        w.OnTurnEnd(L"s"); // seals the collection (file still absent -> no fire yet)
        w.OnTurnEnd(L"s");
        CHECK(w.PendingCount() == 1, "a MATCHED (sealed) pending never expires by turn-ends (deadline-bounded only)");
        filePresent = true;
        w.Tick(now + 5'000);
        CHECK(firedCount == 1 && w.PendingCount() == 0, "Tick fires the moment the file lands");
    }
    // freshness replay guard: an old line timestamp / a missing one never arms.
    {
        CommandWatch w;
        w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {});
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
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>& mds, const std::wstring&) { firedPath = mds.empty() ? std::wstring{} : mds.front(); });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"HANDOVER-rel.md" }, L"K:\\repo", now);
        w.OnTurnEnd(L"s");
        CHECK(firedPath == L"K:\\repo\\HANDOVER-rel.md", "relative write path resolved against the session cwd");
    }
    // FIFO across commands: each command's writes belong to ITS turn — the turn end seals the
    // older pending before the next command's writes arrive, so pairs can never bleed.
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"first" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-1.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s"); // seals + fires the FIRST command with its own file
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"second" }, freshTs + 1, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-2.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 2, "both commands fired, one per turn");
        CHECK(fired.size() == 2 && fired[0].args == L"first" && fired[0].mdPath == L"K:\\r\\HANDOVER-1.md", "FIFO: the first command's turn owns the first write");
        CHECK(fired.size() == 2 && fired[1].args == L"second" && fired[1].mdPath == L"K:\\r\\HANDOVER-2.md", "FIFO: the second command's turn owns the second write");
    }
    // /handover-here (COMMANDS.md — the in-place twin): the hyphenated echo parses whole, and the
    // binding lookup is name-EXACT — a /handover-here sighting fires ONLY its own binding, never
    // prefix-aliasing onto /handover (both share the "handover" leaf preference by design: one
    // HANDOVER-<topic>.md definition contract).
    {
        SlashCommand c;
        CHECK(ParseCommandEcho(L"<command-name>/handover-here</command-name>\n            <command-message>handover-here</command-message>\n            <command-args>ship the tests</command-args>", c), "hyphenated command echo parses");
        CHECK(c.name == L"handover-here" && c.args == L"ship the tests", "the hyphen survives extraction (bindings key on the exact name)");
    }
    {
        CommandWatch w;
        int firedHandover = 0;
        std::vector<FiredHandover> firedHere;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++firedHandover; });
        w.BindMarkdownAwait(L"handover-here", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            firedHere.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover-here", L"replace me" }, freshTs, now);
        CHECK(w.PendingCount() == 1, "/handover-here arms its own pending beside the /handover binding");
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-swap.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(firedHere.size() == 1 && firedHandover == 0, "the md write fires ONLY the /handover-here binding (name-exact lookup, no prefix aliasing)");
        CHECK(!firedHere.empty() && firedHere[0].mdPath == L"K:\\r\\HANDOVER-swap.md" && firedHere[0].args == L"replace me", "fired with the resolved path + the command's args");
    }

    // ---- SAME-FAMILY SUPERSEDE (the /handover vs /handover-here race guard): both await the
    // SAME HANDOVER-* family, so they are ONE logical operation with different handling paths —
    // a new family sighting RE-AIMS a still-unsatisfied older await instead of queueing behind
    // it (FIFO would hand the NEW command's write to the OLD pending and fire the WRONG path). ----
    {
        // THE reported race: /handover left unmatched (a clarification round), THEN
        // /handover-here, THEN Claude writes. The write must fire the /handover-here path — the
        // user's latest intent — never the stale /handover.
        CommandWatch w;
        int firedHandover = 0;
        std::vector<FiredHandover> firedHere;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++firedHandover; });
        w.BindMarkdownAwait(L"handover-here", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            firedHere.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"first intent" }, freshTs, now);
        w.OnTurnEnd(L"s"); // the clarification round ends turn 1 — /handover still unmatched
        w.OnCommandSighting(L"s", SlashCommand{ L"handover-here", L"replace instead" }, freshTs + 1, now);
        CHECK(w.PendingCount() == 1, "the unmatched /handover is SUPERSEDED — one family, the newest handling path owns the await");
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-x.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(firedHandover == 0 && firedHere.size() == 1, "the write fires ONLY the newest family command (/handover-here) — never the stale /handover");
        CHECK(!firedHere.empty() && firedHere[0].mdPath == L"K:\\r\\HANDOVER-x.md" && firedHere[0].args == L"replace instead", "the newer command got its own file + args");
        CHECK(w.PendingCount() == 0, "nothing pending (the superseded await left no residue)");
    }
    {
        // The reverse order guards identically: /handover-here superseded by a later /handover.
        CommandWatch w;
        std::vector<FiredHandover> firedHandover;
        int firedHere = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            firedHandover.push_back(MakeFired(sid, mds, args));
        });
        w.BindMarkdownAwait(L"handover-here", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++firedHere; });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover-here", L"" }, freshTs, now);
        w.OnTurnEnd(L"s");
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs + 1, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-y.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(firedHere == 0 && firedHandover.size() == 1 && firedHandover[0].mdPath == L"K:\\r\\HANDOVER-y.md", "supersede is direction-agnostic (the newest family command wins either way)");
    }
    {
        // A SEALED older family pending is a COMPLETE distinct operation — never superseded: it
        // fires with its OWN file (even while awaiting an approval-held disk write), and the new
        // command's write goes to the new pending. Repeatability upheld.
        CommandWatch w;
        std::vector<FiredHandover> firedHandover;
        std::vector<FiredHandover> firedHere;
        bool filePresent = false;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            firedHandover.push_back(MakeFired(sid, mds, args));
        });
        w.BindMarkdownAwait(L"handover-here", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            firedHere.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([&](const std::wstring&) { return filePresent; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"keep me" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-a.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s"); // /handover SEALED with its own file, awaiting the (approval-held) disk
        w.OnCommandSighting(L"s", SlashCommand{ L"handover-here", L"and me" }, freshTs + 1, now);
        CHECK(w.PendingCount() == 2, "a SEALED family pending is untouched by a newer sighting (a distinct completed collection)");
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-b.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        filePresent = true;
        w.Tick(now + 1'000);
        CHECK(firedHandover.size() == 1 && firedHandover[0].mdPath == L"K:\\r\\HANDOVER-a.md", "the sealed /handover fired with ITS OWN file");
        CHECK(firedHere.size() == 1 && firedHere[0].mdPath == L"K:\\r\\HANDOVER-b.md", "the /handover-here fired with ITS OWN file (no cross-steal in either direction)");
    }
    {
        // A same-command re-run supersedes too (a /handover retry mid-clarification is ONE
        // operation, not two successors) — and a matched-but-UNSEALED predecessor (no turn end
        // seen yet) is defensively SEALED instead of killed: its collected file is its own.
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(sid, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"take one" }, freshTs, now);
        w.OnTurnEnd(L"s");
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"take two" }, freshTs + 1, now);
        CHECK(w.PendingCount() == 1, "a re-run /handover supersedes its own unmatched predecessor (one retry == one successor)");
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-retry.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 1 && fired[0].args == L"take two", "the retry's fire carries the NEWEST args");
        // Defensive seal: matched-but-unsealed predecessor at the moment a family echo arrives.
        w.OnCommandSighting(L"s2", SlashCommand{ L"handover", L"one" }, freshTs, now);
        w.OnFileToolWrite(L"s2", { L"K:\\r\\HANDOVER-one.md" }, L"K:\\r", now); // matched, turn NOT ended
        w.OnCommandSighting(L"s2", SlashCommand{ L"handover", L"two" }, freshTs + 1, now); // family echo -> predecessor defensively sealed
        w.OnFileToolWrite(L"s2", { L"K:\\r\\HANDOVER-two.md" }, L"K:\\r", now); // must go to the NEW pending
        w.OnTurnEnd(L"s2");
        CHECK(fired.size() == 3, "both s2 operations fired (the defensively-sealed one + the new one)");
        CHECK(fired.size() == 3 && fired[1].args == L"one" && fired[1].mdPath == L"K:\\r\\HANDOVER-one.md", "the defensively-sealed predecessor fired with ITS OWN file");
        CHECK(fired.size() == 3 && fired[2].args == L"two" && fired[2].mdPath == L"K:\\r\\HANDOVER-two.md", "the new pending collected the LATER write (no steal)");
    }

    // turn-end expiry for UNMATCHED pendings (the command turn + one clarification round).
    {
        CommandWatch w;
        w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {});
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnTurnEnd(L"s");
        CHECK(w.PendingCount() == 1, "one turn-end: still armed (a clarification round is allowed)");
        w.OnTurnEnd(L"s");
        CHECK(w.PendingCount() == 0, "second turn-end with no md: expired (never grabs a later unrelated md)");
    }
    // deadline expiry + DropSession + the per-session cap + same-echo idempotence.
    {
        CommandWatch w;
        w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {});
        w.SetFileProbe([](const std::wstring&) { return false; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.Tick(now + kCommandAwaitDeadlineMs);
        CHECK(w.PendingCount() == 0, "deadline sweeps an unresolved pending");
        w.OnCommandSighting(L"s2", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.DropSession(L"s2");
        CHECK(w.PendingCount() == 0, "DropSession clears the session's pendings");
        w.OnCommandSighting(L"s2b", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnCommandSighting(L"s2b", SlashCommand{ L"handover", L"" }, freshTs, now);
        CHECK(w.PendingCount() == 1, "re-feeding the SAME echo (identical session/command/timestamp) never double-arms (replay idempotence)");
        w.DropSession(L"s2b");
        // Six SAME-FAMILY sightings collapse to ONE pending (the supersede: each re-run re-aims
        // the await) — they can never queue toward the cap.
        for (int i = 0; i < 6; ++i)
        {
            // Distinct line timestamps — six REAL commands (identical stamps would be the same
            // echo re-fed, dropped by the idempotence guard above).
            w.OnCommandSighting(L"s3", SlashCommand{ L"handover", L"" }, freshTs + i, now);
        }
        CHECK(w.PendingCount() == 1, "same-family sightings collapse to ONE pending (supersede), never queue toward the cap");
        w.DropSession(L"s3");
        // The cap needs DISTINCT families (distinct leaf hints — no supersede between them).
        for (int i = 0; i < 6; ++i)
        {
            const std::wstring name = L"cmd" + std::to_wstring(i);
            w.BindMarkdownAwait(name, L"hint" + std::to_wstring(i), [](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {});
            w.OnCommandSighting(L"s3", SlashCommand{ name, L"" }, freshTs + i, now);
        }
        CHECK(w.PendingCount() == kCommandMaxPendingPerSession, "per-session cap bounds pendings across distinct families (oldest evicted)");
    }

    // ---- safeguards: sane-path gate, throwing handler, throwing probe ----
    {
        CHECK(IsSaneWatchPath(L"K:\\r\\HANDOVER-x.md"), "sane path accepted");
        CHECK(!IsSaneWatchPath(L""), "empty path rejected");
        CHECK(!IsSaneWatchPath(L"K:\\r\\bad\npath.md"), "embedded newline rejected (never a real Windows path)");
        CHECK(!IsSaneWatchPath(L"K:\\r\\bad\"quote.md"), "embedded quote rejected");
        CHECK(!IsSaneWatchPath(L"K:\\r\\bad|pipe.md"), "embedded pipe rejected (illegal in a path AND the fan-out separator)");
        CHECK(!IsSaneWatchPath(std::wstring(kWatchMaxPathChars + 1, L'a')), "oversize path rejected");
        // Join/Split — the multi-path fan-out payload round-trip.
        const std::vector<std::wstring> two{ L"K:\\r\\HANDOVER-a.md", L"K:\\r\\HANDOVER-b.md" };
        CHECK(JoinWatchPaths(two) == L"K:\\r\\HANDOVER-a.md|K:\\r\\HANDOVER-b.md", "JoinWatchPaths '|'-joins");
        CHECK(SplitWatchPaths(JoinWatchPaths(two)) == two, "SplitWatchPaths round-trips the set");
        CHECK(SplitWatchPaths(L"|a||b|") == (std::vector<std::wstring>{ L"a", L"b" }), "empty segments dropped (no phantom paths)");
        CHECK(SplitWatchPaths(L"").empty() && JoinWatchPaths({}).empty(), "empty payload <-> empty set");
    }
    {
        // An INSANE matched path (control char smuggled through a tool input) is rejected AT THE
        // MATCH — the pending stays collecting and a later sane write still satisfies it.
        CommandWatch w;
        std::wstring firedPath;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>& mds, const std::wstring&) { firedPath = mds.empty() ? std::wstring{} : mds.front(); });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\evil\nHANDOVER-a.md" }, L"K:\\r", now);
        CHECK(firedPath.empty() && w.PendingCount() == 1, "insane path rejected at match; pending stays unmatched");
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-b.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(firedPath == L"K:\\r\\HANDOVER-b.md", "a later sane write still satisfies the pending");
    }
    {
        // A THROWING bound handler is caught per fire (LogSwallowedException) — the watch stays
        // fully functional for the next sighting/fire.
        CommandWatch w;
        int calls = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {
            if (++calls == 1)
            {
                throw std::runtime_error("handler boom");
            }
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-1.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(calls == 1 && w.PendingCount() == 0, "throwing handler swallowed; fire consumed");
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs + 1, now); // a distinct second command (a same-stamp re-feed would be the idempotence guard)
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-2.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(calls == 2, "the watch keeps firing after a handler throw (self-contained)");
    }
    {
        // A THROWING injected probe reads as "file absent" — the pending survives and fires once
        // the probe stops throwing (the retried-next-Tick contract).
        CommandWatch w;
        int fired = 0;
        bool probeThrows = true;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
        w.SetFileProbe([&](const std::wstring&) -> bool {
            if (probeThrows)
            {
                throw std::runtime_error("probe boom");
            }
            return true;
        });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-x.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s"); // sealed — awaiting the disk
        CHECK(fired == 0 && w.PendingCount() == 1, "throwing probe reads absent; pending kept");
        probeThrows = false;
        w.Tick(now + 1'000);
        CHECK(fired == 1 && w.PendingCount() == 0, "pending fires once the probe recovers");
    }

    // ---- durable progress (COMMANDS.md §3a): encode/decode, the fired watermark, marker revival ----
    {
        // Encode/decode round-trip + tolerance.
        CommandProgress p;
        CHECK(EncodeCommandProgress(p).empty(), "empty progress encodes to \"\" (the store removes the key)");
        p.processedMs = 1'700'000'000'123;
        p.armed.emplace_back(L"handover", 1'700'000'000'456);
        p.armed.emplace_back(L"handover-here", 1'700'000'000'789);
        const auto enc = EncodeCommandProgress(p);
        const auto back = DecodeCommandProgress(enc);
        CHECK(back.processedMs == p.processedMs && back.armed == p.armed, "progress round-trips through the encoding");
        CHECK(DecodeCommandProgress(L"").processedMs == 0 && DecodeCommandProgress(L"").armed.empty(), "empty decodes to the default");
        CHECK(DecodeCommandProgress(L"garbage!!").processedMs == 0, "garbage decodes to the default (tolerant)");
        CHECK(DecodeCommandProgress(L"v1;p=42;a=x@nonsense,ok@7").armed == (std::vector<std::pair<std::wstring, int64_t>>{ { L"ok", 7 } }), "malformed armed entries skipped, sane ones kept");
    }
    {
        // The FIRED WATERMARK: a fire persists processedMs; a SECOND watch instance over the SAME
        // store (== the app restarted) replaying the SAME still-fresh echo never re-arms — the
        // restart double-processing guard (resume-a-handed-over-session / crash-after-fire).
        std::map<std::wstring, std::wstring> store; // the injected in-memory "SessionStore"
        const auto load = [&](const std::wstring& sid) { const auto it = store.find(sid); return it == store.end() ? std::wstring{} : it->second; };
        const auto save = [&](const std::wstring& sid, const std::wstring& v) { if (v.empty()) { store.erase(sid); } else { store[sid] = v; } };
        int fired = 0;
        {
            CommandWatch w;
            w.SetProgressStore(load, save);
            w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
            w.SetFileProbe([](const std::wstring&) { return true; });
            w.OnCommandSighting(L"sp", SlashCommand{ L"handover", L"" }, freshTs, now);
            CHECK(!load(L"sp").empty(), "arming persists an armed marker (durable mid-await state)");
            w.OnFileToolWrite(L"sp", { L"K:\\r\\HANDOVER-p.md" }, L"K:\\r", now);
            w.OnTurnEnd(L"sp");
            CHECK(fired == 1, "first run fires");
            const auto prog = DecodeCommandProgress(load(L"sp"));
            CHECK(prog.processedMs == freshTs && prog.armed.empty(), "the fire advanced the watermark and retired the armed marker");
        }
        {
            CommandWatch w2; // "the app restarted" — a fresh instance over the same store
            w2.SetProgressStore(load, save);
            w2.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
            w2.SetFileProbe([](const std::wstring&) { return true; });
            w2.OnCommandSighting(L"sp", SlashCommand{ L"handover", L"" }, freshTs, now); // the history replay: same echo, still fresh
            CHECK(w2.PendingCount() == 0, "a replayed already-FIRED echo never re-arms (the watermark remembers across the restart)");
            w2.OnFileToolWrite(L"sp", { L"K:\\r\\HANDOVER-p.md" }, L"K:\\r", now);
            w2.OnTurnEnd(L"sp");
            CHECK(fired == 1, "no double-processing: the successor is never spawned twice");
        }
    }
    {
        // MARKER REVIVAL: a watch armed an await and the app died before the md landed. The next
        // run's history replay carries the echo with a stamp PAST the freshness window — the
        // persisted armed marker revives it (deadline anchored at the ORIGINAL typing time), and
        // the md write later in history / live still fires it. A marker past the deadline prunes
        // instead (never revives).
        std::map<std::wstring, std::wstring> store;
        const auto load = [&](const std::wstring& sid) { const auto it = store.find(sid); return it == store.end() ? std::wstring{} : it->second; };
        const auto save = [&](const std::wstring& sid, const std::wstring& v) { if (v.empty()) { store.erase(sid); } else { store[sid] = v; } };
        const int64_t typedAt = now - 5 * 60'000; // typed 5 min ago (fresh THEN, stale NOW)
        {
            CommandWatch w;
            w.SetProgressStore(load, save);
            w.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {});
            w.OnCommandSighting(L"sr", SlashCommand{ L"handover", L"revive me" }, typedAt, typedAt + 1'000); // armed while fresh
            CHECK(w.PendingCount() == 1 && !load(L"sr").empty(), "armed + marker persisted, then the app 'dies'");
        }
        {
            CommandWatch w2;
            int fired = 0;
            std::wstring firedArgs;
            w2.SetProgressStore(load, save);
            w2.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring& args) { ++fired; firedArgs = args; });
            w2.SetFileProbe([](const std::wstring&) { return true; });
            w2.OnCommandSighting(L"sr", SlashCommand{ L"handover", L"revive me" }, typedAt, now); // 5 min old — stale for the freshness gate, but MARKED
            CHECK(w2.PendingCount() == 1, "a marked mid-await echo REVIVES past the freshness window (restart resilience)");
            w2.OnFileToolWrite(L"sr", { L"K:\\r\\HANDOVER-revived.md" }, L"K:\\r", now);
            w2.OnTurnEnd(L"sr");
            CHECK(fired == 1 && firedArgs == L"revive me", "the revived await completes normally (fired with its original args)");
            CHECK(DecodeCommandProgress(load(L"sr")).armed.empty(), "the revived fire retired its marker");
        }
        {
            // Past-deadline marker: prunes at load, never revives.
            store.clear();
            CommandProgress stale;
            stale.armed.emplace_back(L"handover", now - kCommandAwaitDeadlineMs - 60'000);
            store[L"sx"] = EncodeCommandProgress(stale);
            CommandWatch w3;
            w3.SetProgressStore(load, save);
            w3.BindMarkdownAwait(L"handover", L"handover", [](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) {});
            w3.OnCommandSighting(L"sx", SlashCommand{ L"handover", L"" }, now - kCommandAwaitDeadlineMs - 60'000, now);
            CHECK(w3.PendingCount() == 0, "a marker older than the await deadline never revives (pruned at load)");
            CHECK(DecodeCommandProgress(load(L"sx")).armed.empty(), "the stale marker was garbage-collected from the store");
        }
    }
    {
        // The settle-seal fallback: a matched pending whose turn end never arrives (session died
        // mid-turn) seals after kCommandMatchSettleMs of write-silence and still fires.
        CommandWatch w;
        int fired = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-alone.md" }, L"K:\\r", now);
        w.Tick(now + kCommandMatchSettleMs - 1);
        CHECK(fired == 0 && w.PendingCount() == 1, "before the settle window: still collecting (no turn end seen)");
        w.Tick(now + kCommandMatchSettleMs);
        CHECK(fired == 1 && w.PendingCount() == 0, "the settle fallback seals + fires without a turn end");
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

    // ---- PsEscapedCost (the commandline-vs-paste TIER check; never a truncation bound) ----
    {
        CHECK(PsEscapedCost(L"plain") == 5, "plain chars cost 1 each");
        CHECK(PsEscapedCost(L"a$b`c\"d") == 10, "` \" $ cost 2 each (4 plain + 3 doubled)");
        // The definitive drift-proof assertion: cost == the ACTUAL PsDoubleQuote output size
        // minus the two wrapping quotes, for a mixed string.
        const std::wstring mixed = L"line one\n$var and `tick` plus \"quote\" end";
        CHECK(PsEscapedCost(mixed) == PsDoubleQuote(mixed).size() - 2, "cost model mirrors PsDoubleQuote exactly (no drift)");
        CHECK(PsEscapedCost(std::wstring(kHandoverPromptEscapedBudget, L'a')) <= kHandoverPromptEscapedBudget, "budget-sized plain text fits the commandline tier");
        CHECK(PsEscapedCost(std::wstring(kHandoverPromptEscapedBudget, L'$')) > kHandoverPromptEscapedBudget, "escape-heavy text of the same raw length overflows to the paste tier");
    }

    // ---- ReadHandoverDocumentPrompt (the injected first-user-message shaping) ----
    {
        wchar_t tmp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am-handover-read";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring md = dir + L"\\HANDOVER-read.md";
        {
            std::ofstream f(std::filesystem::path{ md }, std::ios::binary | std::ios::trunc);
            f << "\xEF\xBB\xBF" // UTF-8 BOM (a user-edited md may carry one)
              << "# Handover\r\n\r\nGoal: finish it.\x07\r\n"; // CRLF + a stray control char (BEL)
        }
        const auto p = ReadHandoverDocumentPrompt(md);
        CHECK(p == L"# Handover\n\nGoal: finish it.", "content read VERBATIM: BOM stripped, CRLF -> LF, stray control dropped, outer ws trimmed");
        // Oversized document -> returned IN FULL, never truncated (the caller tiers the channel:
        // PsEscapedCost over the budget routes it to the bracketed-paste stdin injection).
        {
            std::ofstream f(std::filesystem::path{ md }, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 2000; ++i)
            {
                f << "line with some $vars and `ticks` in it to cost extra escapes\n";
            }
        }
        const auto big = ReadHandoverDocumentPrompt(md);
        CHECK(big.size() > 100'000, "oversized document comes back IN FULL (never truncated)");
        CHECK(big.find(L"[handover truncated") == std::wstring::npos, "no truncation tail — the message is whole");
        CHECK(PsEscapedCost(big) > kHandoverPromptEscapedBudget, "an over-budget document reads as the PASTE tier (the caller's channel decision)");
        CHECK(big.rfind(L"extra escapes") == big.size() - 13, "the document's LAST line survives verbatim (trimmed only)");
        // Whitespace-only + missing -> "" (the caller's pointer fallback).
        {
            std::ofstream f(std::filesystem::path{ md }, std::ios::binary | std::ios::trunc);
            f << "  \r\n\t\n";
        }
        CHECK(ReadHandoverDocumentPrompt(md).empty(), "whitespace-only document -> empty (pointer fallback beats an empty first message)");
        ::DeleteFileW(md.c_str());
        CHECK(ReadHandoverDocumentPrompt(md).empty(), "missing file -> empty (pointer fallback)");
        ::RemoveDirectoryW(dir.c_str());
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
            CHECK(body.find("injected VERBATIM") != std::string::npos, "current definition briefs Claude on content injection (write AS the successor's message)");
        }
        // Version-aware UPGRADE: a file byte-identical to a PRIOR shipped version is ours and
        // untouched — it silently upgrades to the current text on the next ensure.
        {
            const auto& history = ShippedHandoverCommandHistory();
            CHECK(history.size() >= 6 && history.back().find(L"injected VERBATIM") != std::wstring_view::npos, "shipped history: >= 6 versions, current is the content-injection text");
            CHECK(history.back().find(L"whatever its size") != std::wstring_view::npos &&
                      history.back().find(L"truncated") == std::wstring_view::npos,
                  "current definition promises FULL delivery (never-truncate) and carries no truncation caution");
            CHECK(history.back().find(L"MORE THAN ONE") != std::wstring_view::npos, "current definition permits writing several HANDOVER files in one turn");
            CHECK(history.back().find(L"YOU invoked the skill yourself") != std::wstring_view::npos, "current definition carries the SELF-INVOCATION guard (V5 — a model-invoked skill writes no command echo, so nothing watches; redirect the user to TYPE the command)");
            CHECK(history.back().find(L"its OWN successor") != std::wstring_view::npos, "current definition briefs the FAN-OUT semantics (V6 — each file starts its OWN successor tab; files must be self-contained)");
            const auto utf8Of = [](std::wstring_view w) {
                std::string out;
                const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
                if (need > 0)
                {
                    out.resize(static_cast<size_t>(need));
                    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
                }
                return out;
            };
            {
                std::ofstream f(std::filesystem::path{ p1 }, std::ios::binary | std::ios::trunc);
                const std::string v1 = utf8Of(history.front());
                f.write(v1.data(), static_cast<std::streamsize>(v1.size())); // pretend this install still carries shipped v1
            }
            const auto pUp = EnsureHandoverCommandFileIn(cfg);
            CHECK(pUp == p1, "upgrade path returns the same file");
            std::ifstream f(std::filesystem::path{ p1 }, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(body == utf8Of(history.back()), "a pristine PRIOR shipped version silently upgrades to the current text");
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

    // ---- EnsureHandoverHereCommandFileIn: the in-place twin's OWN definition file ----
    {
        wchar_t tmp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring cfg = std::wstring{ tmp } + L"am-cmdwatch-test-cfg2";
        // wipe from a previous run
        ::DeleteFileW((cfg + L"\\commands\\handover-here.md").c_str());
        ::RemoveDirectoryW((cfg + L"\\commands").c_str());
        ::RemoveDirectoryW(cfg.c_str());
        const auto p1 = EnsureHandoverHereCommandFileIn(cfg);
        CHECK(!p1.empty() && p1.find(L"handover-here.md") != std::wstring::npos && ::GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES, "absent -> handover-here definition created (its own file, beside handover.md)");
        {
            std::ifstream f(std::filesystem::path{ p1 }, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(body.find("HANDOVER-") != std::string::npos && body.find("Write tool") != std::string::npos, "handover-here definition keeps the await's load-bearing signal (Write tool + HANDOVER-*.md)");
            CHECK(body.find("injected VERBATIM") != std::string::npos && body.find("whatever its size") != std::string::npos, "handover-here definition carries the content-injection + never-truncate briefing (the /handover contract)");
            CHECK(body.find("REPLACES") != std::string::npos && body.find("RESTARTS THIS TAB") != std::string::npos, "handover-here definition briefs the IN-PLACE semantics (the tab is replaced, not a new one opened)");
        }
        {
            const auto& history = ShippedHandoverHereCommandHistory();
            CHECK(history.size() >= 4 && history.back().find(L"RESTARTS THIS TAB") != std::wstring_view::npos, "shipped handover-here history: >= 4 versions; the current text names the in-place restart");
            CHECK(history.back().find(L"MORE THAN ONE") != std::wstring_view::npos, "current handover-here definition permits writing several HANDOVER files in one turn");
            CHECK(history.back().find(L"YOU invoked the skill yourself") != std::wstring_view::npos, "current handover-here definition carries the SELF-INVOCATION guard (V3)");
            CHECK(history.back().find(L"its OWN successor") != std::wstring_view::npos && history.back().find(L"FIRST file's successor REPLACES this tab") != std::wstring_view::npos, "current handover-here definition briefs the FAN-OUT semantics (V4 — first file replaces this tab, additional files open beside it)");
        }
        // A user edit is NEVER overwritten (the shared create-if-absent + upgrade discipline —
        // EnsureShippedCommandFileIn is the one core both wrappers share).
        {
            std::ofstream f(std::filesystem::path{ p1 }, std::ios::binary | std::ios::trunc);
            f << "user-owned-here";
        }
        const auto p2 = EnsureHandoverHereCommandFileIn(cfg);
        CHECK(p2 == p1, "present -> same path returned");
        {
            std::ifstream f(std::filesystem::path{ p1 }, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(body == "user-owned-here", "an existing (user-edited) handover-here definition is never overwritten");
        }
        ::DeleteFileW(p1.c_str());
        ::RemoveDirectoryW((cfg + L"\\commands").c_str());
        ::RemoveDirectoryW(cfg.c_str());
    }
}

namespace
{
    // Unix ms -> the transcript's ISO-Z stamp ("2026-07-20T10:00:00.000Z"). The fabricated
    // session writes REAL timestamps so the watch's freshness gate exercises for real.
    std::wstring IsoZ(int64_t unixMs)
    {
        const __time64_t secs = static_cast<__time64_t>(unixMs / 1000);
        tm g{};
        _gmtime64_s(&g, &secs);
        wchar_t buf[40];
        swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                   g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec,
                   static_cast<int>(unixMs % 1000));
        return buf;
    }

    int64_t WallNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Escape a wstring for embedding inside a JSON string literal within a fabricated line.
    std::wstring JsonEsc(std::wstring_view s)
    {
        std::wstring out;
        for (const wchar_t c : s)
        {
            if (c == L'\\' || c == L'"')
            {
                out.push_back(L'\\');
                out.push_back(c);
            }
            else if (c == L'\n')
            {
                out += L"\\n";
            }
            else if (c == L'\r')
            {
                out += L"\\r";
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // --- the fabricated transcript lines (the EXPECTED /handover session shape) ---
    std::wstring FabUserEchoNamed(const std::wstring& sid, const std::wstring& cmdName, const std::wstring& args, int64_t tsMs)
    {
        // The CURRENT (2026-07) echo shape verbatim: name tag first, indented message/args tags.
        return L"{\"parentUuid\":\"p0\",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<command-name>/" + cmdName + L"</command-name>\\n            <command-message>" + cmdName + L"</command-message>\\n            <command-args>" +
               JsonEsc(args) + L"</command-args>\"},\"uuid\":\"u-echo\",\"timestamp\":\"" + IsoZ(tsMs) + L"\",\"sessionId\":\"" + sid + L"\",\"cwd\":\"K:\\\\repo\",\"version\":\"2.1.190\",\"gitBranch\":\"agentmaster\"}\n";
    }
    std::wstring FabUserEcho(const std::wstring& sid, const std::wstring& args, int64_t tsMs)
    {
        return FabUserEchoNamed(sid, L"handover", args, tsMs);
    }
    std::wstring FabAssistantWrite(const std::wstring& mdPath, int64_t tsMs, const std::wstring& uuid)
    {
        // Mid-turn assistant message: a text block + the Write tool_use (stop_reason tool_use).
        return L"{\"parentUuid\":\"u-echo\",\"isSidechain\":false,\"type\":\"assistant\",\"message\":{\"model\":\"claude-fable-5\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"Writing the handover document now.\"},{\"type\":\"tool_use\",\"id\":\"toolu_fab01\",\"name\":\"Write\",\"input\":{\"file_path\":\"" +
               JsonEsc(mdPath) + L"\",\"content\":\"# Handover\\n\\nGoal, state, next steps...\"}}]},\"uuid\":\"" + uuid + L"\",\"timestamp\":\"" + IsoZ(tsMs) + L"\"}\n";
    }
    std::wstring FabToolResult(int64_t tsMs)
    {
        return L"{\"parentUuid\":\"a-write\",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"tool_use_id\":\"toolu_fab01\",\"type\":\"tool_result\",\"content\":\"File created successfully.\"}]},\"uuid\":\"u-result\",\"timestamp\":\"" + IsoZ(tsMs) + L"\"}\n";
    }
    std::wstring FabAssistantEnd(const std::wstring& text, int64_t tsMs, const std::wstring& uuid)
    {
        return L"{\"parentUuid\":\"u-result\",\"isSidechain\":false,\"type\":\"assistant\",\"message\":{\"model\":\"claude-fable-5\",\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"" +
               JsonEsc(text) + L"\"}]},\"uuid\":\"" + uuid + L"\",\"timestamp\":\"" + IsoZ(tsMs) + L"\"}\n";
    }
    std::wstring FabUserPrompt(const std::wstring& text, int64_t tsMs)
    {
        return L"{\"parentUuid\":\"a-q\",\"isSidechain\":false,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"" +
               JsonEsc(text) + L"\"},\"uuid\":\"u-answer\",\"timestamp\":\"" + IsoZ(tsMs) + L"\"}\n";
    }

    // Mirror SessionScanner::_readDelta's PRIMED feed mapping exactly (the one glue this harness
    // replicates, kept in lockstep with the scanner's event loop; COMMANDS.md testing section):
    // Command -> OnCommandSighting; assistant -> OnFileToolWrite then (terminal) OnTurnEnd;
    // a user interrupt marker -> OnTurnEnd. Nothing else feeds.
    void FeedParsedEvents(CommandWatch& w, const std::wstring& sid, const std::wstring& cwd, const TranscriptParse& parsed, int64_t nowMs)
    {
        for (const auto& ev : parsed.events)
        {
            if (ev.kind == TranscriptEvent::Kind::Command)
            {
                w.OnCommandSighting(sid, SlashCommand{ ev.commandName, ev.commandArgs }, ev.lineTsMs, nowMs);
            }
            else if (ev.kind == TranscriptEvent::Kind::Assistant)
            {
                if (!ev.fileWritePaths.empty())
                {
                    w.OnFileToolWrite(sid, ev.fileWritePaths, cwd, nowMs);
                }
                if (IsTerminalStopReason(ev.stopReason))
                {
                    w.OnTurnEnd(sid);
                }
            }
            else if (ev.kind == TranscriptEvent::Kind::UserPrompt && IsUserInterruptMarker(ev.text))
            {
                w.OnTurnEnd(sid);
            }
        }
    }
}

void TestCommandHandoverE2E()
{
    std::wprintf(L"/handover fabricated session (end-to-end: parse -> feed -> fire, real disk probe):\n");
    const int64_t now = WallNowMs();
    const std::wstring sid = L"fab-handover-session";

    // A REAL md on disk in a temp dir, so scenario A runs the DEFAULT GetFileAttributesExW probe
    // (no injection) — the same code the deployed app runs.
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring{ tmp } + L"am-handover-e2e";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring mdPath = dir + L"\\HANDOVER-commandwatch.md";
    {
        std::ofstream f(std::filesystem::path{ mdPath }, std::ios::binary | std::ios::trunc);
        f << "# Handover\n\nGoal, state, decisions, next steps.\n";
    }

    // ---- scenario A (the happy path, exactly the designed flow): /handover echo -> assistant
    //      Write(HANDOVER-*.md) -> tool_result -> end_turn. ONE fire, real disk probe. ----
    {
        const std::wstring content =
            FabUserEcho(sid, L"wrap up the CommandWatch work; see COMMANDS.md", now - 3000) +
            FabAssistantWrite(mdPath, now - 2000, L"a-write") +
            FabToolResult(now - 1500) +
            FabAssistantEnd(L"Handover written. Ending the turn here.", now - 1000, L"a-end");
        const auto parsed = ParseTranscriptDelta(content);
        CHECK(parsed.consumed == content.size(), "fabricated session parses whole (every line newline-terminated)");
        // The expected event shape of the designed flow:
        size_t cCmd = 0, cAsst = 0, cToolRes = 0, cUser = 0;
        for (const auto& ev : parsed.events)
        {
            cCmd += ev.kind == TranscriptEvent::Kind::Command;
            cAsst += ev.kind == TranscriptEvent::Kind::Assistant;
            cToolRes += ev.kind == TranscriptEvent::Kind::ToolResult;
            cUser += ev.kind == TranscriptEvent::Kind::UserPrompt;
        }
        CHECK(cCmd == 1 && cAsst == 2 && cToolRes == 1 && cUser == 0, "expected event shape: 1 Command + 2 Assistant + 1 ToolResult, 0 UserPrompt (the echo is not a prompt)");

        CommandWatch w; // DEFAULT probe — the real GetFileAttributesExW path
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& s, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(s, mds, args));
        });
        FeedParsedEvents(w, sid, dir, parsed, now);
        CHECK(fired.size() == 1, "scenario A: exactly one fire");
        CHECK(!fired.empty() && fired[0].sessionId == sid && fired[0].mdPath == mdPath, "scenario A: fired with the real on-disk md path (default probe verified it)");
        CHECK(!fired.empty() && fired[0].args == L"wrap up the CommandWatch work; see COMMANDS.md", "scenario A: the command's args ride the fire");
        CHECK(w.PendingCount() == 0, "scenario A: nothing pending after the fire");
        // Content injection (the fired path -> the successor's first user message): the REAL file's
        // content comes back verbatim (trimmed), exactly what _HandleCommandHandover injects.
        if (!fired.empty())
        {
            CHECK(ReadHandoverDocumentPrompt(fired[0].mdPath) == L"# Handover\n\nGoal, state, decisions, next steps.",
                  "scenario A: the fired md's CONTENT reads back verbatim for injection (the successor's first user message)");
        }

        // ---- chunked-delta equivalence: the SAME session parsed the way the scanner actually
        //      reads it (bounded windows; only complete lines consumed, a partial trailing line
        //      re-read whole next pass) yields the IDENTICAL event sequence. ----
        std::vector<TranscriptEvent::Kind> whole;
        for (const auto& ev : parsed.events)
        {
            whole.push_back(ev.kind);
        }
        std::vector<TranscriptEvent::Kind> chunked;
        size_t pos = 0;
        size_t window = 217; // deliberately awkward: splits mid-JSON, mid-tag, mid-escape
        while (pos < content.size())
        {
            const std::wstring_view chunk = std::wstring_view{ content }.substr(pos, (std::min)(window, content.size() - pos)); // (std::min) — windows.h min macro guard
            const auto part = ParseTranscriptDelta(chunk);
            for (const auto& ev : part.events)
            {
                chunked.push_back(ev.kind);
            }
            if (part.consumed == 0)
            {
                window += 217; // no complete line fit the window — the scanner re-reads the partial whole with more bytes next pass
                continue;
            }
            pos += part.consumed;
            window = 217;
        }
        CHECK(chunked == whole, "chunked (scanner-style, partial-line-safe) parse == whole parse (same event sequence)");
    }

    // ---- scenario B (clarification round): echo -> assistant QUESTION (end_turn, boundary 1)
    //      -> user answer -> assistant Write + end_turn. Fires — the 2-turn window covers it. ----
    {
        CommandWatch w;
        int fired = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
        w.SetFileProbe([](const std::wstring&) { return true; });
        const std::wstring content =
            FabUserEcho(sid, L"hand this over", now - 5000) +
            FabAssistantEnd(L"Before I write it: should the successor continue the tests too?", now - 4000, L"a-q") +
            FabUserPrompt(L"yes, tests too", now - 3000) +
            FabAssistantWrite(mdPath, now - 2000, L"a-write2") +
            FabAssistantEnd(L"Written.", now - 1000, L"a-end2");
        FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
        CHECK(fired == 1, "scenario B: one clarification round still fires (the 2-turn window)");
    }

    // ---- scenario C (no md ever): echo -> two end_turns -> expired, never fires. ----
    {
        CommandWatch w;
        int fired = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
        w.SetFileProbe([](const std::wstring&) { return true; });
        const std::wstring content =
            FabUserEcho(sid, L"hand this over", now - 5000) +
            FabAssistantEnd(L"I need more context than that; what should the successor do?", now - 4000, L"a-q1") +
            FabUserPrompt(L"never mind", now - 3000) +
            FabAssistantEnd(L"Okay, not writing a handover.", now - 2000, L"a-q2");
        FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
        CHECK(fired == 0 && w.PendingCount() == 0, "scenario C: no md within 2 turns -> expired, no fire, no leak");
    }

    // ---- scenario D (repeatable — the explicit design requirement): two /handovers in ONE
    //      conversation, each fires its own successor with its own md + args. ----
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& s, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(s, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        const std::wstring md2 = dir + L"\\HANDOVER-round-two.md";
        const std::wstring content =
            FabUserEcho(sid, L"first handover", now - 9000) +
            FabAssistantWrite(mdPath, now - 8000, L"a-w1") +
            FabAssistantEnd(L"Written.", now - 7000, L"a-e1") +
            FabUserEcho(sid, L"second handover", now - 4000) +
            FabAssistantWrite(md2, now - 3000, L"a-w2") +
            FabAssistantEnd(L"Written again.", now - 2000, L"a-e2");
        FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
        CHECK(fired.size() == 2, "scenario D: two /handovers in one conversation -> two fires");
        CHECK(fired.size() == 2 && fired[0].mdPath == mdPath && fired[0].args == L"first handover", "scenario D: first fire carries the first md + args");
        CHECK(fired.size() == 2 && fired[1].mdPath == md2 && fired[1].args == L"second handover", "scenario D: second fire carries the second md + args");
    }

    // ---- scenario E (restart replay): the SAME session with OLD timestamps (a restored/adopted
    //      session's history read) never arms — no ghost successor tabs on reopen. ----
    {
        CommandWatch w;
        int fired = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
        w.SetFileProbe([](const std::wstring&) { return true; });
        const int64_t old = now - 2 * 60 * 60 * 1000; // two hours ago
        const std::wstring content =
            FabUserEcho(sid, L"stale handover", old) +
            FabAssistantWrite(mdPath, old + 1000, L"a-wold") +
            FabAssistantEnd(L"Written.", old + 2000, L"a-eold");
        FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
        CHECK(fired == 0 && w.PendingCount() == 0, "scenario E: a replayed old /handover (stale line timestamps) never arms nor fires");
    }

    // ---- scenario F (MULTI-FILE, real disk): one /handover writes TWO HANDOVER files in its
    //      turn -> ONE fire carrying both, contents joined for the successor's first message. ----
    const std::wstring mdPathB = dir + L"\\HANDOVER-commandwatch-part2.md";
    {
        std::ofstream f(std::filesystem::path{ mdPathB }, std::ios::binary | std::ios::trunc);
        f << "# Handover part 2\n\nAppendix: the gotchas.\n";
    }
    {
        CommandWatch w; // DEFAULT probe — both files really on disk
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& s, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(s, mds, args));
        });
        const std::wstring content =
            FabUserEcho(sid, L"split handover", now - 4000) +
            FabAssistantWrite(mdPath, now - 3000, L"a-wf1") +
            FabAssistantWrite(mdPathB, now - 2000, L"a-wf2") +
            FabAssistantEnd(L"Both parts written.", now - 1000, L"a-ef");
        FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
        CHECK(fired.size() == 1, "scenario F: one fire for the whole multi-file command");
        CHECK(!fired.empty() && (fired[0].mdPaths == std::vector<std::wstring>{ mdPath, mdPathB }), "scenario F: both HANDOVER files ride the fire, in write order");
        // FAN-OUT delivery: each fired path reads back as ITS OWN successor's first user message
        // (_HandleCommandHandover spawns one successor per file, in this order).
        if (!fired.empty() && fired[0].mdPaths.size() == 2)
        {
            CHECK(ReadHandoverDocumentPrompt(fired[0].mdPaths[0]).find(L"# Handover") == 0, "scenario F: file A's content is successor 1's first message");
            CHECK(ReadHandoverDocumentPrompt(fired[0].mdPaths[1]).rfind(L"Appendix: the gotchas.") != std::wstring::npos, "scenario F: file B's content is successor 2's first message");
        }
    }

    // ---- scenario G (RESTART PERSISTENCE, the full fabricated session over a durable store):
    //      run 1 fires; "the app restarts"; run 2 replays the SAME history (stamps now stale but
    //      inside the raw freshness window is irrelevant — the WATERMARK blocks it) -> no second
    //      fire. Then a run that armed-but-never-resolved revives after the restart and fires. ----
    {
        std::map<std::wstring, std::wstring> store;
        const auto load = [&](const std::wstring& s) { const auto it = store.find(s); return it == store.end() ? std::wstring{} : it->second; };
        const auto save = [&](const std::wstring& s, const std::wstring& v) { if (v.empty()) { store.erase(s); } else { store[s] = v; } };
        const std::wstring content =
            FabUserEcho(sid, L"persisted handover", now - 3000) +
            FabAssistantWrite(mdPath, now - 2000, L"a-wg") +
            FabAssistantEnd(L"Written.", now - 1000, L"a-eg");
        int fired = 0;
        {
            CommandWatch w;
            w.SetProgressStore(load, save);
            w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
            FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
            CHECK(fired == 1, "scenario G: run 1 fires once");
        }
        {
            CommandWatch w2; // the restarted app: fresh instance, same durable store
            w2.SetProgressStore(load, save);
            w2.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; });
            FeedParsedEvents(w2, sid, dir, ParseTranscriptDelta(content), now + 10'000); // the reopen's history replay
            CHECK(fired == 1 && w2.PendingCount() == 0, "scenario G: the replay after the restart never re-fires (the durable watermark)");
        }
        {
            // Armed-but-unresolved at "shutdown": echo only in run 1; the write + end_turn land
            // in run 2's replay (typed moments before the app closed) -> revives + fires once.
            store.clear();
            const std::wstring sid2 = L"fab-handover-revive";
            int fired2 = 0;
            {
                CommandWatch w;
                w.SetProgressStore(load, save);
                w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired2; });
                FeedParsedEvents(w, sid2, dir, ParseTranscriptDelta(FabUserEcho(sid2, L"mid-await", now - 3000)), now);
                CHECK(fired2 == 0 && w.PendingCount() == 1 && !load(sid2).empty(), "scenario G: armed + marker persisted, app 'dies' mid-await");
            }
            {
                CommandWatch w2;
                w2.SetProgressStore(load, save);
                w2.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired2; });
                const int64_t later = now + 3 * 60'000; // reopened 3 min later — the echo is stale for the freshness gate
                const std::wstring replay =
                    FabUserEcho(sid2, L"mid-await", now - 3000) +
                    FabAssistantWrite(mdPath, now + 2 * 60'000, L"a-wr") +
                    FabAssistantEnd(L"Written after the reopen.", now + 2 * 60'000 + 500, L"a-er");
                FeedParsedEvents(w2, sid2, dir, ParseTranscriptDelta(replay), later);
                CHECK(fired2 == 1, "scenario G: a mid-await command REVIVES across the restart and completes (fired once, never lost)");
            }
        }
    }

    // ---- scenario H (the FAMILY race, end-to-end): /handover answered with a clarifying
    //      question (turn ends unmatched), the user pivots to /handover-here, Claude writes.
    //      With BOTH bindings registered, the write must fire ONLY the /handover-here handling
    //      path — the stale /handover await was superseded, never handed the newer command's
    //      file (the FIFO race guard). ----
    {
        CommandWatch w;
        int firedHandover = 0;
        std::vector<FiredHandover> firedHere;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++firedHandover; });
        w.BindMarkdownAwait(L"handover-here", L"handover", [&](const std::wstring& s, const std::vector<std::wstring>& mds, const std::wstring& args) {
            firedHere.push_back(MakeFired(s, mds, args));
        });
        w.SetFileProbe([](const std::wstring&) { return true; });
        const std::wstring content =
            FabUserEcho(sid, L"hand this over", now - 6000) +
            FabAssistantEnd(L"New tab or replace this one?", now - 5000, L"a-hq") +
            FabUserEchoNamed(sid, L"handover-here", L"replace this tab", now - 4000) +
            FabAssistantWrite(mdPath, now - 3000, L"a-hw") +
            FabAssistantEnd(L"Written - replacing.", now - 2000, L"a-he");
        FeedParsedEvents(w, sid, dir, ParseTranscriptDelta(content), now);
        CHECK(firedHandover == 0 && firedHere.size() == 1, "scenario H: the pivot fires ONLY /handover-here (the stale /handover was superseded, not fed the file)");
        CHECK(!firedHere.empty() && firedHere[0].mdPath == mdPath && firedHere[0].args == L"replace this tab", "scenario H: the newest family command owns the write");
        CHECK(w.PendingCount() == 0, "scenario H: no residue");
    }

    ::DeleteFileW(mdPath.c_str());
    ::DeleteFileW(mdPathB.c_str());
    ::RemoveDirectoryW(dir.c_str());
}

void TestCommandEchoRealCorpus()
{
    std::wprintf(L"Command echoes, REAL corpus (~/.claude transcripts replayed through the parser):\n");
    const std::wstring proj = ClaudeProjectsDir();
    if (proj.empty() || ::GetFileAttributesW(proj.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::wprintf(L"  [info] no Claude projects dir on this machine - corpus replay skipped\n");
        return;
    }

    // Gather the NEWEST transcripts (mtime-desc — biases to the CURRENT echo stratum, which is
    // what /handover will produce; older strata lines are still counted when the window reaches
    // them). Bounded sweep: a stat pass over everything, then head-read the newest kMaxFiles.
    struct Cand
    {
        std::wstring path;
        int64_t mtime;
    };
    std::vector<Cand> files;
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator projIt{ std::filesystem::path{ proj }, ec }, projEnd; !ec && projIt != projEnd; projIt.increment(ec))
        {
            std::error_code ed;
            if (!projIt->is_directory(ed))
            {
                continue;
            }
            std::error_code es;
            for (std::filesystem::directory_iterator f{ projIt->path(), es }, fEnd; !es && f != fEnd; f.increment(es))
            {
                std::error_code ef;
                if (!f->is_regular_file(ef) || f->path().extension() != L".jsonl")
                {
                    continue;
                }
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(f->path().c_str(), GetFileExInfoStandard, &fad))
                {
                    ULARGE_INTEGER u{};
                    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                    u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                    files.push_back({ f->path().wstring(), static_cast<int64_t>(u.QuadPart) });
                }
            }
        }
    }
    if (files.empty())
    {
        std::wprintf(L"  [info] no transcripts found - corpus replay skipped\n");
        return;
    }
    std::sort(files.begin(), files.end(), [](const Cand& a, const Cand& b) { return a.mtime > b.mtime; });

    constexpr size_t kMaxFiles = 120;
    constexpr size_t kHeadBytes = 2u << 20; // 2 MB head per file — echoes are sprinkled, a sample is the point
    constexpr size_t kMaxEchoes = 400;
    constexpr size_t kMaxWriteLines = 60;

    size_t userEchoes = 0, sysEchoes = 0, echoParsed = 0, echoWithTs = 0, turnEventLeaks = 0;
    size_t writeLines = 0, writePathsOk = 0;
    size_t filesScanned = 0;
    std::wstring firstLeak;

    for (const auto& cand : files)
    {
        if (filesScanned >= kMaxFiles || userEchoes + sysEchoes >= kMaxEchoes)
        {
            break;
        }
        ++filesScanned;
        std::ifstream f(std::filesystem::path{ cand.path }, std::ios::binary);
        if (!f)
        {
            continue;
        }
        std::string head(kHeadBytes, '\0');
        f.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(f.gcount()));
        // Only whole lines (drop a truncated tail — the head cap can split a line).
        const size_t lastNl = head.rfind('\n');
        if (lastNl == std::string::npos)
        {
            continue;
        }
        head.resize(lastNl + 1);

        size_t lineStart = 0;
        while (lineStart < head.size())
        {
            size_t nl = head.find('\n', lineStart);
            if (nl == std::string::npos)
            {
                break;
            }
            const std::string_view line{ head.data() + lineStart, nl - lineStart };
            lineStart = nl + 1;

            const bool isUserEcho = line.find("\"message\":{\"role\":\"user\",\"content\":\"<command-") != std::string_view::npos &&
                                    line.find("\"isMeta\":true") == std::string_view::npos &&
                                    line.find("\"isCompactSummary\":true") == std::string_view::npos &&
                                    line.find("\"isSidechain\":true") == std::string_view::npos;
            const bool isSysEcho = !isUserEcho &&
                                   line.find("\"subtype\":\"local_command\"") != std::string_view::npos &&
                                   line.find("<command-name>") != std::string_view::npos &&
                                   line.find("\"isSidechain\":true") == std::string_view::npos;
            const bool isWriteLine = !isUserEcho && !isSysEcho && writeLines < kMaxWriteLines &&
                                     line.find("\"type\":\"assistant\"") != std::string_view::npos &&
                                     line.find("\"type\":\"tool_use\"") != std::string_view::npos &&
                                     line.find("\"name\":\"Write\"") != std::string_view::npos &&
                                     line.find("\"file_path\"") != std::string_view::npos;
            if (!isUserEcho && !isSysEcho && !isWriteLine)
            {
                continue;
            }
            // Convert just this line and replay it through the REAL parser.
            const int need = ::MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
            if (need <= 0)
            {
                continue;
            }
            std::wstring wide(static_cast<size_t>(need), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), need);
            wide += L"\n";
            const auto r = ParseTranscriptDelta(wide);

            if (isUserEcho || isSysEcho)
            {
                (isUserEcho ? userEchoes : sysEchoes)++;
                bool sawCommand = false, sawTurn = false;
                for (const auto& ev : r.events)
                {
                    if (ev.kind == TranscriptEvent::Kind::Command && !ev.commandName.empty())
                    {
                        sawCommand = true;
                        echoWithTs += ev.lineTsMs > 0;
                    }
                    sawTurn = sawTurn || ev.kind == TranscriptEvent::Kind::UserPrompt || ev.kind == TranscriptEvent::Kind::Assistant || ev.kind == TranscriptEvent::Kind::ToolResult;
                }
                echoParsed += sawCommand;
                if (sawTurn)
                {
                    ++turnEventLeaks;
                    if (firstLeak.empty())
                    {
                        firstLeak = cand.path;
                    }
                }
            }
            else // isWriteLine
            {
                ++writeLines;
                for (const auto& ev : r.events)
                {
                    if (ev.kind == TranscriptEvent::Kind::Assistant && !ev.fileWritePaths.empty())
                    {
                        ++writePathsOk;
                        break;
                    }
                }
            }
        }
    }

    std::wprintf(L"  [info] corpus: %zu files scanned, %zu user-echo + %zu system-echo lines, %zu Write-tool lines\n",
                 filesScanned, userEchoes, sysEchoes, writeLines);
    if (userEchoes + sysEchoes == 0)
    {
        std::wprintf(L"  [info] no command echoes in the sampled window - echo assertions skipped\n");
    }
    else
    {
        CHECK(echoParsed == userEchoes + sysEchoes, "every real command echo parses to a Command event (name extracted)");
        CHECK(turnEventLeaks == 0, "no real command echo EVER leaks a turn event (the /model false-Running invariant, corpus-wide)");
        if (turnEventLeaks != 0)
        {
            std::wprintf(L"  [FAIL-detail] first leaking file: %s\n", firstLeak.c_str());
        }
        CHECK(echoWithTs > 0, "real echoes carry parseable line timestamps (the freshness guard's input)");
    }
    if (writeLines > 0)
    {
        CHECK(writePathsOk == writeLines, "every real Write tool_use line yields its file_path in fileWritePaths");
    }
}
