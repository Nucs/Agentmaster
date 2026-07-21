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
//   * Sha256 (NIST vectors + the 55/56/63/64/65-byte padding edges + the 1,000,000-byte case) —
//     the identity primitive the shipped command definitions' version history is built on
//   * EnsureShippedCommandFileIn (COMMANDS.md §6): the whole create / digest-matched UPGRADE /
//     never-overwrite policy over a SYNTHETIC command (the real histories keep digests only, so a
//     prior version's bytes no longer exist to lay on disk), plus EnsureHandoverCommandFileIn +
//     EnsureHandoverHereCommandFileIn against a temp config dir — never the real ~/.claude — and
//     the "history's last digest == sha256(current text)" gate that catches a text edit which
//     forgot to append its digest
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

    // The UTF-8 bytes of a wide string EXACTLY as WriteFileUtf8 lays them on disk — what the
    // shipped command definitions are hashed and compared as (ClaudeSpawn's own Utf16ToUtf8 is
    // file-static, so the tests carry the same two lines).
    std::string Utf8Of(std::wstring_view w)
    {
        std::string out;
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (need > 0)
        {
            out.resize(static_cast<size_t>(need));
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
        }
        return out;
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
    // §6b FILE-MATCH REGEX: a configured VALID pattern REPLACES the contains-hint as the leaf
    // qualifier (case-insensitive regex search over the file NAME), so a user can rename the
    // briefing family entirely; an INVALID pattern falls back to the shipped hint (a broken user
    // regex must never silently kill handovers).
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(sid, mds, args));
        }, LR"(^BRIEF-.*\.md$)");
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        // The regex is AUTHORITATIVE: a BRIEF-*.md qualifies though its leaf lacks "handover", and
        // a HANDOVER-*.md does NOT (the pattern, not the hint, decides) — but the nothing-collected
        // fallback still can't fire alone, so assert against the collected set at the turn end.
        w.OnFileToolWrite(L"s", { L"K:\\r\\BRIEF-alpha.md", L"K:\\r\\HANDOVER-old.md" }, L"K:\\r", now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\brief-beta.MD" }, L"K:\\r", now); // case-insensitive match
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 1, "regex leaf match: one fire for the command");
        CHECK(!fired.empty() && (fired[0].mdPaths == std::vector<std::wstring>{ L"K:\\r\\BRIEF-alpha.md", L"K:\\r\\brief-beta.MD" }),
              "regex leaf match: the PATTERN decides (BRIEF-* collected case-insensitively; the hint-named HANDOVER-old.md excluded)");
    }
    // …and a VALID pattern is authoritative ALL the way: it also SUPPRESSES the legacy
    // nothing-collected-yet fallback (the batch's first markdown), which exists only as tolerance
    // for a mis-named single file under the loose shipped hint. With it live, a first batch
    // writing an unrelated notes.md would be collected as the briefing and spawn a successor from
    // an incidental doc edit — exactly what the hint preference exists to prevent.
    // The first-markdown tolerance is now a CALLER-OWNED policy (the default pattern is a real
    // setting value, so the watch can't infer "default vs customized" itself): Engine passes
    // allowFirstMarkdownFallback = (pattern is the shipped default or empty/invalid).
    {
        CommandWatch w;
        int fired = 0;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>&, const std::wstring&) { ++fired; }, LR"(^BRIEF-.*\.md$)", /*allowFirstMarkdownFallback*/ false);
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\notes.md" }, L"K:\\r", now); // NOTHING collected yet + no qualifying file
        w.OnTurnEnd(L"s");
        CHECK(fired == 0, "a CUSTOMIZED leaf regex (fallback off) never adopts an unrelated notes.md as the briefing");
    }
    {
        // The DEFAULT rule keeps the tolerance (unchanged behavior — a mis-named single file still
        // hands over). Driven with the REAL shipped default pattern + fallback-on, exactly what
        // Engine.cpp passes for an untouched install.
        CommandWatch w;
        std::wstring firedPath;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring&, const std::vector<std::wstring>& mds, const std::wstring&) { firedPath = mds.empty() ? std::wstring{} : mds.front(); }, std::wstring{ kDefaultCommandFileMatchRegex }, /*allowFirstMarkdownFallback*/ true);
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\notes.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(firedPath == L"K:\\r\\notes.md", "the SHIPPED DEFAULT pattern keeps the legacy first-markdown tolerance");
    }
    {
        // …and that same default pattern matches a normal HANDOVER-*.md by NAME (it is the regex
        // spelling of the definitions' own `HANDOVER-<topic>.md` contract, case-insensitive).
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) { fired.push_back(MakeFired(sid, mds, args)); }, std::wstring{ kDefaultCommandFileMatchRegex }, true);
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\notes.md", L"K:\\r\\HANDOVER-a.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 1 && !fired.empty() && (fired[0].mdPaths == std::vector<std::wstring>{ L"K:\\r\\HANDOVER-a.md" }),
              "the default pattern collects the HANDOVER-*.md and excludes the incidental notes.md");
    }
    {
        // The default is TIGHTER than the contains-"handover" leaf hint it replaced: a doc whose
        // name merely MENTIONS handover (no `HANDOVER-` separator) is NOT a briefing. It rides the
        // batch's-first-markdown tolerance here (the default keeps that on, so a lone mis-named
        // file still hands over) — but it must never be collected ALONGSIDE a real HANDOVER-*.md.
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) { fired.push_back(MakeFired(sid, mds, args)); }, std::wstring{ kDefaultCommandFileMatchRegex }, true);
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-a.md", L"K:\\r\\old-handover.md", L"K:\\r\\handover-b.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 1 && !fired.empty() && (fired[0].mdPaths == std::vector<std::wstring>{ L"K:\\r\\HANDOVER-a.md", L"K:\\r\\handover-b.md" }),
              "the default pattern requires the HANDOVER- separator: a bare mention (old-handover.md) is not collected, a lowercase handover-b.md is");
    }
    {
        CommandWatch w;
        std::vector<FiredHandover> fired;
        w.BindMarkdownAwait(L"handover", L"handover", [&](const std::wstring& sid, const std::vector<std::wstring>& mds, const std::wstring& args) {
            fired.push_back(MakeFired(sid, mds, args));
        }, L"([unclosed"); // invalid pattern — RegexIsValid false
        w.SetFileProbe([](const std::wstring&) { return true; });
        w.OnCommandSighting(L"s", SlashCommand{ L"handover", L"" }, freshTs, now);
        w.OnFileToolWrite(L"s", { L"K:\\r\\HANDOVER-a.md" }, L"K:\\r", now);
        w.OnTurnEnd(L"s");
        CHECK(fired.size() == 1 && !fired.empty() && fired[0].mdPath == L"K:\\r\\HANDOVER-a.md",
              "an INVALID leaf regex falls back to the shipped 'handover' hint (a broken pattern never kills the await)");
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

    // ---- Sha256 (the shipped-definition history's identity primitive, COMMANDS.md §6) ----
    {
        // FIPS 180-4 / NIST vectors plus the padding edges: 55 = the last length whose 0x80 + the
        // 64-bit length still fit one block, 56 = the first that SPILLS into a second block, and
        // 63/64/65 straddle the block boundary. A drift in any of these would silently break the
        // command-definition upgrade rule for every install (a mismatching digest reads as
        // "user-owned": no upgrade, no error, no log).
        CHECK(Sha256Hex(std::string_view{}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(empty message) — the NIST vector");
        CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\") — the NIST vector");
        CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256(448-bit NIST vector)");
        CHECK(Sha256Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "sha256(55 bytes — the last length that fits its own final block)");
        CHECK(Sha256Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "sha256(56 bytes — the length spills into a second block)");
        CHECK(Sha256Hex(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34", "sha256(63 bytes — one short of a full block)");
        CHECK(Sha256Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "sha256(64 bytes — exactly one block, all padding in the second)");
        CHECK(Sha256Hex(std::string(65, 'a')) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0", "sha256(65 bytes — one past a full block)");
        CHECK(Sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256(1,000,000 x 'a') — the multi-block NIST vector");
        CHECK(Sha256Hex("handover") != Sha256Hex("handovef"), "a one-byte difference changes the digest (the whole point of the upgrade gate)");
    }

    // ---- EnsureShippedCommandFileIn: the create / upgrade / never-overwrite POLICY ----
    // Driven over a SYNTHETIC command, because the real histories carry digests ONLY — a prior
    // version's bytes no longer exist in the binary to lay on disk. This is the one place the whole
    // policy (the shared core both real definitions route through) is exercised end to end.
    {
        wchar_t tmp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring cfg = std::wstring{ tmp } + L"am-shipped-cmd-policy";
        const std::wstring leaf = L"am-test-cmd.md";
        const std::wstring path = cfg + L"\\commands\\" + leaf;
        ::DeleteFileW(path.c_str());
        ::RemoveDirectoryW((cfg + L"\\commands").c_str());
        ::RemoveDirectoryW(cfg.c_str());

        static constexpr std::wstring_view kV1 = L"# pretend shipped v1\nUse the Write tool.\n";
        static constexpr std::wstring_view kV2 = L"# pretend shipped v2\nUse the Write tool, and say more.\n";
        static constexpr std::wstring_view kV3 = L"# pretend shipped v3 (current)\nUse the Write tool, and say the most.\n";
        const std::string h1 = Sha256Hex(Utf8Of(kV1));
        const std::string h2 = Sha256Hex(Utf8Of(kV2));
        const std::string h3 = Sha256Hex(Utf8Of(kV3));
        const std::vector<std::string_view> history{ h1, h2, h3 }; // oldest first, current last

        const auto readBack = [&path] {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        const auto layDown = [&path](std::string_view body) {
            std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
        };
        const auto ensure = [&] { return EnsureShippedCommandFileIn(cfg, leaf, history, kV3, L"am-test"); };

        CHECK(ensure() == path && readBack() == Utf8Of(kV3), "absent -> the CURRENT text is created (dirs made)");
        layDown(Utf8Of(kV1));
        CHECK(ensure() == path && readBack() == Utf8Of(kV3), "a pristine OLDEST shipped version (v1) upgrades to the current text");
        layDown(Utf8Of(kV2));
        CHECK(ensure() == path && readBack() == Utf8Of(kV3), "the version right before current (v2) upgrades too");
        CHECK(ensure() == path && readBack() == Utf8Of(kV3), "an ALREADY-CURRENT file is left alone (its digest is the history's last entry, never a prior one)");
        layDown("# my own /handover\n");
        CHECK(ensure() == path && readBack() == "# my own /handover\n", "a user-owned file is NEVER overwritten");
        layDown(Utf8Of(kV1) + " ");
        CHECK(ensure() == path && readBack() == Utf8Of(kV1) + " ", "ONE byte off a shipped version reads as user-EDITED (no upgrade — the edit sticks forever)");
        layDown("");
        CHECK(ensure() == path && readBack().empty(), "an EMPTY file is left alone too (never rewritten blind)");
        // A history that doesn't KNOW the on-disk bytes never rewrites them — the guarantee that
        // protects a user's own same-named command from a sibling command's history.
        layDown(Utf8Of(kV1));
        const std::vector<std::string_view> foreign{ h2, h3 };
        CHECK(EnsureShippedCommandFileIn(cfg, leaf, foreign, kV3, L"am-test") == path && readBack() == Utf8Of(kV1), "bytes absent from THIS command's history are user-owned (no cross-command upgrade)");
        // Degenerate inputs are refused outright (no write, no path).
        CHECK(EnsureShippedCommandFileIn(L"", leaf, history, kV3, L"am-test").empty(), "no config dir -> no write");
        CHECK(EnsureShippedCommandFileIn(cfg, leaf, {}, kV3, L"am-test").empty(), "an EMPTY history -> no write (a command with no shipped version is a bug, not a create)");
        CHECK(EnsureShippedCommandFileIn(cfg, leaf, history, L"", L"am-test").empty(), "an EMPTY current text -> no write (never truncate the user's file to nothing)");
        CHECK(readBack() == Utf8Of(kV1), "the refused calls left the file exactly as it was");
        // The definition is written ATOMICALLY (temp sibling + MoveFileExW) so a torn write can
        // never leave bytes that match no shipped digest — which would read as user-owned and
        // never be repaired. The temp must not survive a successful write (and never as a .md,
        // which Claude Code would offer as a command).
        CHECK(::GetFileAttributesW((path + L".am-tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "the atomic write leaves no temp file behind");
        CHECK(ensure() == path && readBack() == Utf8Of(kV3) && ::GetFileAttributesW((path + L".am-tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "an upgrade over an existing file replaces it atomically, leaving no temp");

        ::DeleteFileW(path.c_str());
        ::RemoveDirectoryW((cfg + L"\\commands").c_str());
        ::RemoveDirectoryW(cfg.c_str());
    }

    // ---- customizable command names (COMMANDS.md §6a): normalize / pair-heal / render / identity ----
    {
        // NormalizeCommandName: the strict ASCII slug every downstream layer agrees on.
        CHECK(NormalizeCommandName(L"/Handover ") == L"handover", "names: leading '/' stripped, ASCII lowered, junk dropped");
        CHECK(NormalizeCommandName(L"  /ho-2_x") == L"ho-2_x", "names: leading whitespace + '/' tolerated; -/_ and digits kept");
        CHECK(NormalizeCommandName(L"My Cmd!") == L"mycmd", "names: spaces + punctuation dropped (a clean slug remains)");
        CHECK(NormalizeCommandName(L"..\\evil/name") == L"evilname", "names: path separators + dots can never survive (the file-leaf safety)");
        CHECK(NormalizeCommandName(L"").empty() && NormalizeCommandName(L"/ ").empty(), "names: blank/degenerate -> empty (the caller's fallback decides)");
        CHECK(NormalizeCommandName(std::wstring(100, L'a')).size() == 64, "names: capped at 64 chars");
        // ResolveCommandNamePair: normalize + default-fallback + collision-heal (deterministic).
        {
            std::wstring a, b;
            ResolveCommandNamePair(a, b);
            CHECK(a == L"handover" && b == L"handover-here", "pair: blanks -> the shipped defaults");
            a = L"/HO ";
            b = L"x y";
            ResolveCommandNamePair(a, b);
            CHECK(a == L"ho" && b == L"xy", "pair: each side normalizes independently");
            a = L"same";
            b = L"same";
            ResolveCommandNamePair(a, b);
            CHECK(a == L"same" && b == L"handover-here", "pair: a collision heals the HERE side to its default");
            a = L"handover-here";
            b = L"";
            ResolveCommandNamePair(a, b);
            CHECK(a == L"handover" && b == L"handover-here", "pair: /handover named 'handover-here' would still collide with the healed default -> BOTH fall back");
        }
        // RenderShippedCommandText / NormalizeCommandBytesForIdentity: exact inverses, word-boundary
        // bounded (a "/am-cmd" substitution must never corrupt an "/am-cmd-here" mention).
        {
            static constexpr std::wstring_view kText = L"Type /am-cmd now (never /am-cmd-here); type `/am-cmd <x>`.\n";
            CHECK(RenderShippedCommandText(kText, L"am-cmd", L"am-cmd") == kText, "render: the default name renders verbatim");
            const std::wstring r = RenderShippedCommandText(kText, L"am-cmd", L"zz");
            CHECK(r == L"Type /zz now (never /am-cmd-here); type `/zz <x>`.\n", "render: every bounded /am-cmd token renamed; the -here mention untouched (word boundary)");
            CHECK(NormalizeCommandBytesForIdentity(Utf8Of(r), L"am-cmd", L"zz") == Utf8Of(kText), "identity: the byte normalization inverts the render exactly");
            CHECK(NormalizeCommandBytesForIdentity(Utf8Of(kText), L"am-cmd", L"am-cmd") == Utf8Of(kText), "identity: default name -> bytes verbatim");
        }
        // The REAL definitions render clean: every "/<default>" mention takes the custom name and
        // none survives (the self-invocation guard then tells the user to TYPE the right thing).
        {
            const std::wstring r = RenderShippedCommandText(ShippedHandoverCommandText(), kDefaultHandoverCommandName, L"ho");
            CHECK(r.find(L"/ho") != std::wstring::npos && r.find(L"/handover") == std::wstring::npos, "render: the real /handover text carries only the custom name");
            CHECK(r.find(L"HANDOVER-") != std::wstring::npos && r.find(L"Write tool") != std::wstring::npos, "render: the await's load-bearing signals survive a rename (leaf hint + Write tool)");
            CHECK(Utf8Of(RenderShippedCommandText(ShippedHandoverCommandText(), kDefaultHandoverCommandName, kDefaultHandoverCommandName)) == Utf8Of(ShippedHandoverCommandText()), "render: default-name render is byte-identical (digest history stays valid)");
            const std::wstring rh = RenderShippedCommandText(ShippedHandoverHereCommandText(), kDefaultHandoverHereCommandName, L"swap");
            CHECK(rh.find(L"/swap") != std::wstring::npos && rh.find(L"/handover") == std::wstring::npos, "render: the real /handover-here text carries only the custom name");
            CHECK(Sha256Hex(NormalizeCommandBytesForIdentity(Utf8Of(rh), kDefaultHandoverHereCommandName, L"swap")) == ShippedHandoverHereCommandHashes().back(), "identity: a custom-named CURRENT render digests back onto the history's last entry");
        }
    }

    // ---- RegexUtil (COMMANDS.md §6b): the ONE guarded regex component ----
    // Everything user-typed goes through here, so the contract is: NEVER throw, invalid reads as
    // no-match / unchanged, bounded input+pattern, ECMAScript search semantics + $1 backrefs.
    {
        CHECK(RegexIsValid(LR"(^HANDOVER-.*\.md$)") && !RegexIsValid(L"([unclosed") && !RegexIsValid(L""),
              "regex: valid / invalid / empty classified (the cog's live-validation probe)");
        CHECK(!RegexIsValid(std::wstring(kRegexMaxPatternChars + 1, L'a')), "regex: an over-cap PATTERN is refused (unbounded-cost guard)");
        CHECK(RegexSearch(L"HANDOVER-docs.md", LR"(^HANDOVER-.*\.md$)") && !RegexSearch(L"notes.md", LR"(^HANDOVER-.*\.md$)"),
              "regex: search matches/rejects by pattern");
        CHECK(RegexSearch(L"brief-A.MD", LR"(^brief-.*\.md$)", /*ci*/ true) && !RegexSearch(L"brief-A.MD", LR"(^brief-.*\.md$)", /*ci*/ false),
              "regex: the case-insensitive flag is honored (the leaf matcher's mode)");
        CHECK(!RegexSearch(L"anything", L"([unclosed"), "regex: an INVALID pattern reads as no-match (never throws)");
        CHECK(!RegexSearch(std::wstring(kRegexMaxInputChars + 1, L'x'), L"x"), "regex: an over-cap INPUT is refused");
        CHECK(RegexSearch(L"a handover file", L"handover"), "regex: unanchored patterns match anywhere (regex_search semantics)");
        bool applied = false;
        CHECK(RegexReplace(L"Fix the parser", L"Fix", L"Ship", false, &applied) == L"Ship the parser" && applied, "regex: replace + the applied flag");
        CHECK(RegexReplace(L"a-a-a", L"a", L"b") == L"b-b-b", "regex: EVERY occurrence is replaced");
        CHECK(RegexReplace(L"Fix parser", LR"(^(\w+) (\w+)$)", L"$2 $1") == L"parser Fix", "regex: $1/$2 backrefs work");
        applied = true;
        CHECK(RegexReplace(L"untouched", L"([unclosed", L"x", false, &applied) == L"untouched" && !applied, "regex: an INVALID pattern leaves the text UNCHANGED + applied=false");
        applied = true;
        CHECK(RegexReplace(L"untouched", L"zzz", L"x", false, &applied) == L"untouched" && !applied, "regex: NO match leaves the text unchanged + applied=false (the title fallback's signal)");
    }

    // ---- DeriveHandoverSuccessorTitle (§6b): the successor-title regex rewrite ----
    // Contract: "" whenever the rewrite does not apply (unset / invalid / no-match / blank result),
    // so the caller falls back to the classic "(handover)" naming — the rewrite can only IMPROVE a
    // title, never lose one (Rule #11's never-empty invariant).
    {
        CHECK(DeriveHandoverSuccessorTitle(L"Parser work", L"", L"x").empty(), "title rewrite: unset find regex -> \"\" (default naming)");
        CHECK(DeriveHandoverSuccessorTitle(L"Parser work", L"([unclosed", L"x").empty(), "title rewrite: an INVALID pattern -> \"\" (default naming)");
        CHECK(DeriveHandoverSuccessorTitle(L"Parser work", L"zzz", L"x").empty(), "title rewrite: a pattern matching nowhere -> \"\" (default naming)");
        CHECK(DeriveHandoverSuccessorTitle(L"Parser work", L"^.*$", L"   ").empty(), "title rewrite: a blank RESULT -> \"\" (a title never goes empty)");
        CHECK(DeriveHandoverSuccessorTitle(L"Parser work", L"work", L"next") == L"Parser next", "title rewrite: plain find/replace");
        CHECK(DeriveHandoverSuccessorTitle(L"Parser (v2)", LR"(^(.*)$)", L"$1 - continued") == L"Parser (v2) - continued", "title rewrite: $1 backref (the append idiom)");
        CHECK(DeriveHandoverSuccessorTitle(L"  Parser work  ", L"work", L"next") == L"Parser next", "title rewrite: the result is trimmed");
        CHECK(DeriveHandoverSuccessorTitle(L"Parser 3", LR"(\d+)", L"4") == L"Parser 4", "title rewrite: a numeric bump pattern");
        {
            const std::wstring longTitle(400, L'x');
            const auto capped = DeriveHandoverSuccessorTitle(longTitle, L"^", L"pre-");
            CHECK(capped.size() == 255 && capped.rfind(L"...") == 252, "title rewrite: a degenerate >255-char result caps at 252 + \"...\" (DeriveSessionTitle's net)");
        }
        // The SHIPPED DEFAULT pair (now a real, visible setting rather than a hidden code path)
        // must reproduce the classic naming EXACTLY — including the chain-bump, which is the
        // reason it is not the naive ^(.*)$: that would STACK "(handover) (handover)", the very
        // bug DeriveForkTitle exists to prevent. The pattern eats an existing suffix and re-adds
        // it, so a repeat resolves to the SAME title as its origin — which the caller's
        // uniqueness bump (DeriveSuffixedTitle, asserted here as the second step) then walks.
        {
            const auto def = [](std::wstring_view t) { return DeriveHandoverSuccessorTitle(t, kDefaultCommandTitleFindRegex, kDefaultCommandTitleReplace); };
            CHECK(RegexIsValid(kDefaultCommandTitleFindRegex), "default title pattern is a VALID regex (a broken shipped default would silently disable the rewrite)");
            CHECK(def(L"Parser work") == L"Parser work (handover)", "default title pair: a plain title gets \" (handover)\" — identical to DeriveSuffixedTitle");
            CHECK(def(L"Parser work") == DeriveSuffixedTitle(L"Parser work", L"handover"), "default title pair: byte-identical to the built-in suffixer on a plain title");
            CHECK(def(L"Parser work (handover)") == L"Parser work (handover)", "default title pair: a CHAINED handover resolves to the origin's own title (never stacked) …");
            CHECK(DeriveSuffixedTitle(L"Parser work (handover)", L"handover") == L"Parser work (handover 2)", "… and the caller's uniqueness bump then walks it to (handover 2)");
            CHECK(def(L"Parser work (handover 7)") == L"Parser work (handover)", "default title pair: a numbered chain also collapses to the base (the bump re-walks it)");
            CHECK(def(L"Fix (handover) notes") == L"Fix (handover) notes (handover)", "default title pair: an INTERIOR \"(handover)\" is not a suffix — left alone");
            CHECK(!def(L"x").empty(), "default title pair: always applies (the pattern matches any title, so the rewrite is never a silent no-op)");
        }
    }

    // ---- name-aware Ensure / Remove / Reconcile: the rename + disable migration POLICY ----
    // Same synthetic-command discipline as the block above (real histories are digests only), now
    // with texts that CARRY the command token so the render/identity path is exercised end to end.
    {
        wchar_t tmp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring cfg = std::wstring{ tmp } + L"am-shipped-cmd-rename";
        const auto pathOf = [&cfg](std::wstring_view name) { return cfg + L"\\commands\\" + std::wstring{ name } + L".md"; };
        const auto wipe = [&] {
            for (const auto n : { L"am-cmd", L"zz", L"qq" })
            {
                ::DeleteFileW(pathOf(n).c_str());
            }
            ::RemoveDirectoryW((cfg + L"\\commands").c_str());
            ::RemoveDirectoryW(cfg.c_str());
        };
        wipe();

        static constexpr std::wstring_view kN1 = L"# shipped n1\nType /am-cmd to run.\nUse the Write tool.\n";
        static constexpr std::wstring_view kN2 = L"# shipped n2 (current)\nType /am-cmd (see also /am-cmd-here).\nUse the Write tool.\n";
        const std::string nh1 = Sha256Hex(Utf8Of(kN1));
        const std::string nh2 = Sha256Hex(Utf8Of(kN2));
        const std::vector<std::string_view> history{ nh1, nh2 };
        const auto readAt = [](const std::wstring& p) {
            std::ifstream f(std::filesystem::path{ p }, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        const auto layAt = [](const std::wstring& p, std::string_view body) {
            std::ofstream f(std::filesystem::path{ p }, std::ios::binary | std::ios::trunc);
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
        };
        const auto renderedCurrent = [&](std::wstring_view name) { return Utf8Of(RenderShippedCommandText(kN2, L"am-cmd", name)); };

        // Named Ensure: create under a custom name (rendered), upgrade a custom-named PRIOR
        // version, never overwrite a user edit — the §6 policy through the identity seam.
        CHECK(EnsureShippedCommandFileNamedIn(cfg, L"am-cmd", history, kN2, L"am-test", L"zz") == pathOf(L"zz") && readAt(pathOf(L"zz")) == renderedCurrent(L"zz"),
              "named ensure: absent -> the custom-named RENDERED current text is created");
        layAt(pathOf(L"zz"), Utf8Of(RenderShippedCommandText(kN1, L"am-cmd", L"zz")));
        CHECK(EnsureShippedCommandFileNamedIn(cfg, L"am-cmd", history, kN2, L"am-test", L"zz") == pathOf(L"zz") && readAt(pathOf(L"zz")) == renderedCurrent(L"zz"),
              "named ensure: a custom-named PRISTINE PRIOR version upgrades (identity through the name normalization)");
        layAt(pathOf(L"zz"), "# my own zz command\n");
        CHECK(EnsureShippedCommandFileNamedIn(cfg, L"am-cmd", history, kN2, L"am-test", L"zz") == pathOf(L"zz") && readAt(pathOf(L"zz")) == "# my own zz command\n",
              "named ensure: a user-owned custom-named file is NEVER overwritten");
        CHECK(EnsureShippedCommandFileNamedIn(cfg, L"am-cmd", history, kN2, L"am-test", L"").empty(), "named ensure: a blank name refuses (no leaf to build)");

        // Ours-only Remove: pristine (ANY shipped version, incl. current) deletes; a user edit or
        // an absent file does not.
        CHECK(!RemoveShippedCommandFileNamedIn(cfg, L"am-cmd", history, L"am-test", L"zz") && readAt(pathOf(L"zz")) == "# my own zz command\n",
              "remove: a user-owned file is LEFT IN PLACE (and reported not-deleted)");
        layAt(pathOf(L"zz"), renderedCurrent(L"zz"));
        CHECK(RemoveShippedCommandFileNamedIn(cfg, L"am-cmd", history, L"am-test", L"zz") && ::GetFileAttributesW(pathOf(L"zz").c_str()) == INVALID_FILE_ATTRIBUTES,
              "remove: a pristine CURRENT custom-named file deletes (rename/disable migration)");
        layAt(pathOf(L"zz"), Utf8Of(RenderShippedCommandText(kN1, L"am-cmd", L"zz")));
        CHECK(RemoveShippedCommandFileNamedIn(cfg, L"am-cmd", history, L"am-test", L"zz"), "remove: a pristine PRIOR version deletes too (any shipped version is ours)");
        CHECK(!RemoveShippedCommandFileNamedIn(cfg, L"am-cmd", history, L"am-test", L"zz"), "remove: an absent file is a no-op");

        // Reconcile: the full engine-init story — materialize, rename-migrate, respect a user
        // edit, disable, re-enable, and the enabled-but-blank fallback.
        CHECK(ReconcileShippedCommandFileIn(cfg, L"am-cmd", history, kN2, L"am-test", L"", L"am-cmd", true) == L"am-cmd" && readAt(pathOf(L"am-cmd")) == Utf8Of(kN2),
              "reconcile: first materialize under the default name (marker was empty)");
        CHECK(ReconcileShippedCommandFileIn(cfg, L"am-cmd", history, kN2, L"am-test", L"am-cmd", L"zz", true) == L"zz",
              "reconcile: rename returns the new marker");
        CHECK(::GetFileAttributesW(pathOf(L"am-cmd").c_str()) == INVALID_FILE_ATTRIBUTES && readAt(pathOf(L"zz")) == renderedCurrent(L"zz"),
              "reconcile: rename DELETED the pristine default file and materialized the custom one");
        layAt(pathOf(L"zz"), "# my edited zz\n");
        CHECK(ReconcileShippedCommandFileIn(cfg, L"am-cmd", history, kN2, L"am-test", L"zz", L"qq", true) == L"qq" &&
                  readAt(pathOf(L"zz")) == "# my edited zz\n" && readAt(pathOf(L"qq")) == renderedCurrent(L"qq"),
              "reconcile: a rename away from a USER-EDITED file leaves it in place (their command) and still materializes the new name");
        CHECK(ReconcileShippedCommandFileIn(cfg, L"am-cmd", history, kN2, L"am-test", L"qq", L"qq", false).empty() &&
                  ::GetFileAttributesW(pathOf(L"qq").c_str()) == INVALID_FILE_ATTRIBUTES,
              "reconcile: DISABLE deletes the pristine file and returns an empty marker");
        CHECK(ReconcileShippedCommandFileIn(cfg, L"am-cmd", history, kN2, L"am-test", L"", L"am-cmd", true) == L"am-cmd" && readAt(pathOf(L"am-cmd")) == Utf8Of(kN2),
              "reconcile: RE-ENABLE from an empty marker just materializes again");
        CHECK(ReconcileShippedCommandFileIn(cfg, L"am-cmd", history, kN2, L"am-test", L"am-cmd", L"", true) == L"am-cmd",
              "reconcile: enabled-but-blank configured name falls back to the default (never 'enabled but nameless')");
        wipe(); // (also clears the user-edited zz.md the rename-away test deliberately left behind)
    }

    // ---- AppSettings round-trip: the /handover-family customization fields (COMMANDS.md §6a) ----
    {
        AppSettings as;
        as.commandHandoverName = L"ho";
        as.commandHandoverEnabled = false;
        as.commandHandoverHereName = L"swap";
        as.commandHandoverHereEnabled = true;
        as.commandHandoverMaterializedName = L""; // disabled last run — nothing materialized
        as.commandHandoverHereMaterializedName = L"swap";
        const auto back = AppSettingsFromJson(ToJson(as));
        CHECK(back.commandHandoverName == L"ho" && !back.commandHandoverEnabled, "cmd settings: the /handover name + enable round-trip");
        CHECK(back.commandHandoverHereName == L"swap" && back.commandHandoverHereEnabled, "cmd settings: the /handover-here name + enable round-trip");
        CHECK(back.commandHandoverMaterializedName.empty(), "cmd settings: a PRESENT empty marker round-trips as empty (the disabled state, not the absent-key default)");
        CHECK(back.commandHandoverHereMaterializedName == L"swap", "cmd settings: a custom marker round-trips");
        const auto fresh = AppSettingsFromJson(json::Value::MkObj());
        CHECK(fresh.commandHandoverName == L"handover" && fresh.commandHandoverEnabled &&
                  fresh.commandHandoverHereName == L"handover-here" && fresh.commandHandoverHereEnabled,
              "cmd settings: absent keys reproduce the shipped commands exactly");
        CHECK(fresh.commandHandoverMaterializedName == L"handover" && fresh.commandHandoverHereMaterializedName == L"handover-here",
              "cmd settings: absent markers read as the DEFAULT names (a pre-feature install has those files on disk to migrate)");
        AppSettings col;
        col.commandHandoverName = L"x";
        col.commandHandoverHereName = L"x";
        const auto healed = AppSettingsFromJson(ToJson(col));
        CHECK(healed.commandHandoverName == L"x" && healed.commandHandoverHereName == L"handover-here", "cmd settings: a stored collision heals on load (HERE falls back)");
        auto o = json::Value::MkObj(); // fresh object: json::Value::Set APPENDS and Find returns the FIRST hit, so a re-Set on a ToJson output would be shadowed
        o.Set(L"commandHandoverMaterializedName", json::Value::MkStr(L"..\\evil"));
        CHECK(AppSettingsFromJson(o).commandHandoverMaterializedName == L"evil", "cmd settings: a hand-edited marker normalizes (path chars can never reach the commands-dir delete)");
    }

    // ---- AppSettings round-trip: the §6b SUCCESSOR SHAPING fields ----
    {
        AppSettings as;
        as.commandHandoverSuccessorModel = L"claude-opus-4-8";
        as.commandHandoverHereSuccessorModel = L""; // Default
        as.commandHandoverTitleFindRegex = LR"(^(.*)$)";
        as.commandHandoverTitleReplace = L"$1 - next";
        as.commandHandoverFileMatchRegex = LR"(^BRIEF-.*\.md$)";
        as.commandHandoverDeleteFileAfterLaunch = true;
        const auto back = AppSettingsFromJson(ToJson(as));
        CHECK(back.commandHandoverSuccessorModel == L"claude-opus-4-8" && back.commandHandoverHereSuccessorModel.empty(),
              "shaping: the per-command successor models round-trip (\"\" == Default)");
        CHECK(back.commandHandoverTitleFindRegex == LR"(^(.*)$)" && back.commandHandoverTitleReplace == L"$1 - next",
              "shaping: the title find/replace pair round-trips VERBATIM (regex chars + $ backrefs unmangled)");
        CHECK(back.commandHandoverFileMatchRegex == LR"(^BRIEF-.*\.md$)", "shaping: the file-match regex round-trips verbatim");
        CHECK(back.commandHandoverDeleteFileAfterLaunch, "shaping: the delete-after toggle round-trips");
        // The three REGEX fields are PRESENCE-GATED (the launchModels idiom): an ABSENT key seeds
        // the SHIPPED DEFAULT — the boxes show the real rule instead of hiding a code fallback —
        // while a PRESENT empty string is a deliberate "fall back to the built-in behavior".
        const auto fresh = AppSettingsFromJson(json::Value::MkObj());
        CHECK(fresh.commandHandoverSuccessorModel.empty() && fresh.commandHandoverHereSuccessorModel.empty(),
              "shaping: absent model keys reproduce the shipped behavior (Default model)");
        // §6c pairing: the shipped write location is the session SCRATCHPAD, so a FRESH install
        // also gets delete-after ON (a temp briefing has no reason to linger once its successor
        // holds the content). An install that already stored `false` keeps it — see below.
        CHECK(fresh.commandHandoverDeleteFileAfterLaunch && CommandWritePathIsScratchpad(fresh.commandHandoverWritePath),
              "shaping: an absent write-path/delete pair reads as the shipped scratchpad + delete-after ON");
        {
            auto kept = json::Value::MkObj();
            kept.Set(L"commandHandoverDeleteFileAfterLaunch", json::Value::MkBool(false));
            CHECK(!AppSettingsFromJson(kept).commandHandoverDeleteFileAfterLaunch,
                  "shaping: an install that stored delete-after OFF keeps it (the new default only seeds an ABSENT key)");
        }
        CHECK(fresh.commandHandoverTitleFindRegex == kDefaultCommandTitleFindRegex &&
                  fresh.commandHandoverTitleReplace == kDefaultCommandTitleReplace &&
                  fresh.commandHandoverFileMatchRegex == kDefaultCommandFileMatchRegex,
              "shaping: an ABSENT regex key SEEDS the shipped default (a pre-6b settings.json gets the visible rule, not an empty box)");
        {
            auto cleared = json::Value::MkObj();
            cleared.Set(L"commandHandoverTitleFindRegex", json::Value::MkStr(L""));
            cleared.Set(L"commandHandoverFileMatchRegex", json::Value::MkStr(L""));
            const auto c = AppSettingsFromJson(cleared);
            CHECK(c.commandHandoverTitleFindRegex.empty() && c.commandHandoverFileMatchRegex.empty(),
                  "shaping: a PRESENT empty regex is kept (the user cleared the box == use the built-in behavior)");
        }
        // An INVALID stored pattern round-trips as typed (it is validated at USE, not at load —
        // the consumers fall back and the cog warns; normalizing here would corrupt patterns).
        AppSettings bad;
        bad.commandHandoverTitleFindRegex = L"([unclosed";
        CHECK(AppSettingsFromJson(ToJson(bad)).commandHandoverTitleFindRegex == L"([unclosed", "shaping: an invalid pattern is stored verbatim (validated at use, not at load)");
        CHECK(DeriveHandoverSuccessorTitle(L"any title", AppSettingsFromJson(ToJson(bad)).commandHandoverTitleFindRegex, L"x").empty(),
              "shaping: … and that invalid pattern degrades to the default naming at USE time");
    }

    // ---- customizable WRITE LOCATION (COMMANDS.md §6c): normalize / phrase / render / identity ----
    {
        // NormalizeCommandWritePath: the value is inlined into ONE line of a markdown instruction,
        // so anything that could break that line out is dropped, not escaped.
        CHECK(NormalizeCommandWritePath(L"  ./docs  ") == L"./docs", "write path: surrounding whitespace trimmed");
        CHECK(NormalizeCommandWritePath(L"./docs\\") == L"./docs" && NormalizeCommandWritePath(L"./docs/") == L"./docs", "write path: a trailing separator is noise");
        CHECK(NormalizeCommandWritePath(L"./") == L"./" && NormalizeCommandWritePath(L"C:\\") == L"C:\\", "write path: the degenerate roots ARE the value (never stripped to something else)");
        CHECK(NormalizeCommandWritePath(L"a\nb\rc`d\"e|f") == L"abcdef", "write path: newlines/backticks/quotes/pipe dropped (the rendered line can never break out)");
        CHECK(NormalizeCommandWritePath(std::wstring(400, L'a')).size() == 240, "write path: length capped");
        CHECK(CommandWritePathIsScratchpad(L"") && CommandWritePathIsScratchpad(L"scratchpad") && CommandWritePathIsScratchpad(L" Scratchpad "),
              "write path: blank == the token == the shipped default (case-insensitively)");
        CHECK(!CommandWritePathIsScratchpad(L"./") && !CommandWritePathIsScratchpad(L"./scratchpad"), "write path: a real folder is not the scratchpad token");

        // The rendered phrase: the default renders verbatim, "./" is the working directory, and a
        // folder is quoted + told to be created (absolute vs relative worded differently).
        CHECK(CommandWritePathPhrase(L"") == CommandWritePathPhrase(L"scratchpad"), "phrase: blank and the token render identically");
        CHECK(CommandWritePathPhrase(L"./") == L"the current working directory", "phrase: ./ is the pre-6c behavior, spelled out");
        CHECK(CommandWritePathPhrase(L"./docs").find(L"relative to the current working directory") != std::wstring::npos, "phrase: a relative folder says what it is relative to");
        CHECK(CommandWritePathPhrase(L"D:\\briefings").find(L"relative") == std::wstring::npos &&
                  CommandWritePathPhrase(L"D:\\briefings").find(L"create the folder") != std::wstring::npos,
              "phrase: an absolute folder is not called relative, and is still created if missing");

        // Render <-> identity are EXACT inverses over the real shipped texts — the property the
        // whole upgrade rule rests on (a location-rendered file must digest back onto the history).
        for (const bool here : { false, true })
        {
            const std::wstring_view current = here ? ShippedHandoverHereCommandText() : ShippedHandoverCommandText();
            const auto& hashes = here ? ShippedHandoverHereCommandHashes() : ShippedHandoverCommandHashes();
            CHECK(current.find(L"WRITE IT IN: ") != std::wstring_view::npos, "shipped text carries the write-location MARKER (the render/identity span)");
            CHECK(Utf8Of(RenderShippedCommandWritePath(current, L"")) == Utf8Of(current) &&
                      Utf8Of(RenderShippedCommandWritePath(current, L"scratchpad")) == Utf8Of(current),
                  "render: the shipped default renders BYTE-IDENTICALLY (the digest history stays valid)");
            const std::wstring rendered = RenderShippedCommandWritePath(current, L"./docs/handovers");
            CHECK(rendered != current && rendered.find(L"`./docs/handovers`") != std::wstring::npos, "render: a configured folder lands on the marker line");
            CHECK(rendered.find(L"HANDOVER-<short-topic>.md") != std::wstring::npos, "render: the FILE-NAME contract is untouched (folder-only setting)");
            CHECK(std::count(rendered.begin(), rendered.end(), L'\n') == std::count(current.begin(), current.end(), L'\n'),
                  "render: the phrase stays ONE line (line count unchanged)");
            CHECK(NormalizeCommandWritePathBytesForIdentity(Utf8Of(rendered)) == Utf8Of(current), "identity: the byte normalization inverts the render exactly");
            CHECK(Sha256Hex(NormalizeCommandWritePathBytesForIdentity(Utf8Of(rendered))) == hashes.back(),
                  "identity: a location-rendered CURRENT text digests back onto the history's last entry");
            // Both customizations at once (a renamed command with a custom location) must still
            // resolve to the shipped digest — the two spans are independent.
            const std::wstring_view defName = here ? kDefaultHandoverHereCommandName : kDefaultHandoverCommandName;
            const std::wstring both = RenderShippedCommandWritePath(RenderShippedCommandText(current, defName, L"zz"), L"D:\\briefs");
            CHECK(Sha256Hex(NormalizeCommandBytesForIdentity(NormalizeCommandWritePathBytesForIdentity(Utf8Of(both)), defName, L"zz")) == hashes.back(),
                  "identity: a RENAMED command with a CUSTOM location still digests onto the shipped history (both spans invert)");
        }
        // A text WITHOUT the marker (every pre-6c version) is returned verbatim — exactly what
        // keeps those versions matching their own historical digests instead of reading user-owned.
        CHECK(NormalizeCommandWritePathBytesForIdentity("# no marker here\n") == "# no marker here\n", "identity: a marker-less text is untouched (pre-6c versions still upgrade)");

        // The write policy through the location seam: a pristine file written under location A is
        // RE-RENDERED when the configured location becomes B (same shipped version — the old
        // "already current" short-circuit would have frozen it), while a user edit is still never
        // touched, and ForceReinstall is the only thing that overwrites one.
        {
            wchar_t tmp[MAX_PATH];
            ::GetTempPathW(MAX_PATH, tmp);
            const std::wstring cfg = std::wstring{ tmp } + L"am-writepath-policy";
            const std::wstring path = cfg + L"\\commands\\hb.md";
            const auto readAt = [&path] {
                std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            };
            const auto wipe = [&] {
                ::DeleteFileW(path.c_str());
                ::RemoveDirectoryW((cfg + L"\\commands").c_str());
                ::RemoveDirectoryW(cfg.c_str());
            };
            wipe();
            static constexpr std::wstring_view kOld = L"# v1\nUse the Write tool.\nWRITE IT IN: your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)\ndone\n";
            static constexpr std::wstring_view kNew = L"# v2 (current)\nUse the Write tool.\nWRITE IT IN: your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)\ndone\n";
            const std::string h1 = Sha256Hex(Utf8Of(kOld));
            const std::string h2 = Sha256Hex(Utf8Of(kNew));
            const std::vector<std::string_view> history{ h1, h2 };
            const auto ensure = [&](std::wstring_view loc) { return EnsureShippedCommandFileNamedIn(cfg, L"hb", history, kNew, L"am-test", L"hb", loc); };

            CHECK(ensure(L"") == path && readAt() == Utf8Of(kNew), "policy: absent -> the current text at the shipped location");
            CHECK(ensure(L"./docs") == path && readAt() == Utf8Of(RenderShippedCommandWritePath(kNew, L"./docs")),
                  "policy: a CHANGED location RE-RENDERS the same shipped version (the file follows the setting)");
            CHECK(ensure(L"./docs") == path && readAt() == Utf8Of(RenderShippedCommandWritePath(kNew, L"./docs")), "policy: re-running with the same location is a no-op");
            CHECK(ensure(L"") == path && readAt() == Utf8Of(kNew), "policy: changing back re-renders back");
            {
                std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
                f << Utf8Of(RenderShippedCommandWritePath(kOld, L"./docs"));
            }
            CHECK(ensure(L"./docs") == path && readAt() == Utf8Of(RenderShippedCommandWritePath(kNew, L"./docs")),
                  "policy: a PRIOR version written under a custom location is still recognized and upgraded");
            CHECK(InspectShippedCommandFileNamedIn(cfg, L"hb", history, kNew, L"hb", L"./docs") == ShippedCommandFileState::UpToDate, "inspect: ours + current == UpToDate");
            CHECK(InspectShippedCommandFileNamedIn(cfg, L"hb", history, kNew, L"hb", L"./elsewhere") == ShippedCommandFileState::OursStale, "inspect: ours but written for another location == OursStale");
            {
                std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
                f << "# my own command\n";
            }
            CHECK(InspectShippedCommandFileNamedIn(cfg, L"hb", history, kNew, L"hb", L"./docs") == ShippedCommandFileState::UserOwned, "inspect: an edited file == UserOwned");
            CHECK(ensure(L"./docs") == path && readAt() == "# my own command\n", "policy: a user-edited file is STILL never overwritten by the location change");
            CHECK(ForceReinstallShippedCommandFileNamedIn(cfg, L"hb", kNew, L"am-test", L"hb", L"./docs") == path &&
                      readAt() == Utf8Of(RenderShippedCommandWritePath(kNew, L"./docs")),
                  "reinstall: the confirmed escape hatch overwrites a user-owned file, rendered with the configured location");
            ::DeleteFileW(path.c_str());
            CHECK(InspectShippedCommandFileNamedIn(cfg, L"hb", history, kNew, L"hb", L"") == ShippedCommandFileState::Missing, "inspect: no file == Missing");
            wipe();
        }

        // AppSettings round-trip + the presence gate (an ABSENT key seeds the scratchpad default,
        // a PRESENT value — including a hand-edited one with junk — is honored, normalized).
        {
            AppSettings as;
            as.commandHandoverWritePath = L"./docs/handovers";
            CHECK(AppSettingsFromJson(ToJson(as)).commandHandoverWritePath == L"./docs/handovers", "write path: round-trips verbatim");
            CHECK(CommandWritePathIsScratchpad(AppSettingsFromJson(json::Value::MkObj()).commandHandoverWritePath), "write path: an absent key reads as the shipped scratchpad default");
            auto o = json::Value::MkObj();
            o.Set(L"commandHandoverWritePath", json::Value::MkStr(L"  ./x`y  "));
            CHECK(AppSettingsFromJson(o).commandHandoverWritePath == L"./xy", "write path: a hand-edited value normalizes on load (no line-breaking chars reach the definition)");
        }
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
        // The shipped-version HISTORY is a list of SHA-256 digests (COMMANDS.md §6): the upgrade
        // rule recognizes a pristine older OURS by digest, so the superseded texts no longer live
        // in the binary. Two things must hold — the current text still carries every load-bearing
        // sentinel, and the history's LAST entry is that text's digest.
        {
            const auto& hashes = ShippedHandoverCommandHashes();
            const std::wstring_view current = ShippedHandoverCommandText();
            CHECK(hashes.size() >= 6 && current.find(L"injected VERBATIM") != std::wstring_view::npos, "shipped history: >= 6 versions, current is the content-injection text");
            CHECK(current.find(L"whatever its size") != std::wstring_view::npos &&
                      current.find(L"truncated") == std::wstring_view::npos,
                  "current definition promises FULL delivery (never-truncate) and carries no truncation caution");
            CHECK(current.find(L"MORE THAN ONE") != std::wstring_view::npos, "current definition permits writing several HANDOVER files in one turn");
            CHECK(current.find(L"YOU invoked the skill yourself") != std::wstring_view::npos, "current definition carries the SELF-INVOCATION guard (V5 — a model-invoked skill writes no command echo, so nothing watches; redirect the user to TYPE the command)");
            CHECK(current.find(L"its OWN successor") != std::wstring_view::npos, "current definition briefs the FAN-OUT semantics (V6 — each file starts its OWN successor tab; files must be self-contained)");
            // Well-formed + unique digests: a typo'd entry silently disables that version's upgrade
            // path forever (its installs would read as user-owned), a duplicated one hides a version.
            bool wellFormed = true;
            bool unique = true;
            for (size_t i = 0; i < hashes.size(); ++i)
            {
                wellFormed = wellFormed && hashes[i].size() == 64 && hashes[i].find_first_not_of("0123456789abcdef") == std::string_view::npos;
                for (size_t j = i + 1; j < hashes.size(); ++j)
                {
                    unique = unique && hashes[i] != hashes[j];
                }
            }
            CHECK(wellFormed, "every shipped-history entry is a 64-char lowercase-hex SHA-256");
            CHECK(unique, "the shipped history holds no duplicate digest");
            // THE version gate: edit the definition text without appending its digest and this
            // fails — printing the digest to append (see ClaudeSpawn.cpp's history comment).
            const std::string digest = Sha256Hex(Utf8Of(current));
            const std::string msg = "shipped history's LAST entry == sha256(current /handover text) — after editing the text, APPEND this digest: " + digest;
            CHECK(!hashes.empty() && digest == hashes.back(), msg.c_str());
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
            const auto& hashes = ShippedHandoverHereCommandHashes();
            const std::wstring_view current = ShippedHandoverHereCommandText();
            CHECK(hashes.size() >= 4 && current.find(L"RESTARTS THIS TAB") != std::wstring_view::npos, "shipped handover-here history: >= 4 versions; the current text names the in-place restart");
            CHECK(current.find(L"MORE THAN ONE") != std::wstring_view::npos, "current handover-here definition permits writing several HANDOVER files in one turn");
            CHECK(current.find(L"YOU invoked the skill yourself") != std::wstring_view::npos, "current handover-here definition carries the SELF-INVOCATION guard (V3)");
            CHECK(current.find(L"its OWN successor") != std::wstring_view::npos && current.find(L"FIRST file's successor REPLACES this tab") != std::wstring_view::npos, "current handover-here definition briefs the FAN-OUT semantics (V4 — first file replaces this tab, additional files open beside it)");
            bool wellFormed = true;
            for (const auto& h : hashes)
            {
                wellFormed = wellFormed && h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string_view::npos;
            }
            CHECK(wellFormed, "every shipped handover-here history entry is a 64-char lowercase-hex SHA-256");
            const std::string digest = Sha256Hex(Utf8Of(current));
            const std::string msg = "shipped handover-here history's LAST entry == sha256(current text) — after editing the text, APPEND this digest: " + digest;
            CHECK(!hashes.empty() && digest == hashes.back(), msg.c_str());
            // The two commands share the markdown-await leaf hint but are DISTINCT documents, so
            // their histories can never collide (a shared digest would cross-upgrade the files).
            bool disjoint = true;
            for (const auto& a : ShippedHandoverCommandHashes())
            {
                for (const auto& b : hashes)
                {
                    disjoint = disjoint && a != b;
                }
            }
            CHECK(disjoint, "the /handover and /handover-here histories share no digest (no cross-upgrade)");
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
