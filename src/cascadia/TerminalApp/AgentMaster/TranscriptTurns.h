// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TranscriptTurns: a Claude Code session rendered TURN BY TURN as Markdown, at one of
// five detail levels. It backs the session copy menus' "Copy ▸ Transcript ▸ Default / Max / Full /
// Compact / Minimal" (AgentCopyActions.h codes 9-13), which replaced the flat "Transcript" and
// "Transcript Followup" items.
//
// It is a C++ PORT of transcript_turns.py, the "turns view" of the am-agentmaster-cli skill
// (~/.claude/skills/am-agentmaster-cli/scripts/transcript_turns.py, version v9). That script is the
// reference: for every transcript the port is meant to print exactly what the script prints with
// `--preset v9 --utc` (or local time) and the matching level flag. The harness pins it byte for
// byte on the skill's own synthetic fixtures (tests/turns/, goldens written by
// tests/turns/make_goldens.py from the script itself — TestTranscriptTurns). A change to the
// script's v9 output must be regenerated there and ported here; a NEW script version is a
// deliberate move of both (the goldens pin --preset v9).
//
// What a render shows, per turn (the levels pick how much of it):
//     ### Turn N · <start> → last reply <time>
//     **❯ User**            the message that started the turn (✉ <name> when a teammate or another
//                           session sent it), then anything typed while it ran
//     **● Claude**          what the assistant did — tool calls on one line each (how they ended),
//                           its remarks between calls, background completions, compactions, the
//                           user's answers to its questions — then the LAST text it wrote
//     **● <agent>**         one entry per subagent / teammate the turn spawned, read from that
//                           agent's own transcript
// plus, at the top, the chain of sessions this one was forked or plan-restarted from (their turns
// are rendered too, numbered as one conversation) and the slash commands run outside a turn.
//
// Divergences from the reference, all on input Claude Code never writes:
//   - A model-written tool argument with the WRONG JSON TYPE (a number where a path string is
//     expected, ...) renders as its Python str() instead of the script's "(unreadable arguments:
//     TypeError: ...)" line; the port has no per-call failure mode to report.
//   - JSON numbers are doubles here, so an integral float literal ("5.0") prints like an int ("5").
//   - A transcript string that is ENTIRELY one unbroken base64 run of 4 KiB or more (a pasted
//     image's payload, which no text field ever holds) is replaced by "<base64 N chars elided>"
//     before parsing (ScrubLargeBase64Payloads) — it keeps huge image-bearing transcripts cheap.
//   - A timestamp without a time zone (Claude Code always writes "Z") is compared as UTC and
//     printed as its own wall-clock time in both clocks (the reference converts it from local time
//     when printing UTC). Only the "YYYY-MM-DD" date form is read (Python also takes ISO week
//     dates and the compact "YYYYMMDD"), and a stamp CPython 3.12 accepts by accident — a stray
//     character just before its zone, a fraction after the hour or the minutes — reads as unknown.
//   - \d / \w in the reference's regexes match ASCII digits / iswalnum here, not every Unicode digit.
//   - The repr() of a JSON list or object (shown only where a malformed argument holds one where a
//     string belongs) escapes the ASCII control characters as Python does, but not the non-ASCII
//     characters Python's repr also escapes (U+00A0, U+200D, ...): those are printed as they are.
//   - With the chain feature OFF, the reference reads a transcript's lines split at "\n" alone (a
//     stray "\r" inside a line keeps it whole); the port splits at "\r" too, as both do with the
//     chain feature on — which every level is.
//
// Plain C++ + Win32 (file reads, file times, the CRT's local time zone), no WinRT and no PCH, so it
// links into the standalone test harness unchanged. Every entry point is NEVER-THROW.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"

namespace Agentmaster
{
    // How much of each turn a render shows — the reference's v8 "detail levels". Cumulative: each
    // level shows everything the one before it shows. The numeric values are stable: the copy menus
    // encode a level as CopySessionField code 9 + the value (AgentCopyActions.h).
    enum class TurnsLevel : int
    {
        Minimal = 0, // the user's messages and each turn's final reply, nothing else
        Compact = 1, // + compaction markers, the user's answers, slash-command notes, each agent's brief + report
        Default = 2, // + every tool call on one line (how it ended) and the remarks between calls — the
                     //   reference's "steps" level, what it prints with no level flag
        Full = 3, // + the verbatim command under each Bash call, the first line each call returned, and
                  //   how long each call took
        Max = 4, // + the full content of each Write, and a stats line per turn
    };

    // How many TurnsLevel values exist (the copy menus' item count; codes 9..9+count-1).
    inline constexpr int kTurnsLevelCount = 5;

    // Every output feature and correctness rule of the reference, one flag each (the script's
    // --<feature> / --no-<feature>). The member initializers are the reference's LATEST preset (v9)
    // at its default level — exactly what `transcript_turns.py <session>` prints. Use
    // TurnsOptionsForLevel to get a level; flip a flag afterwards to inspect one feature (the
    // harness does). Features that are on in every v9 level (times, shortPaths, opMeta, bgEvents,
    // nestedBlocks, compactedHistory, chain, peerMessages) still have both code paths, ported as
    // the script has them.
    struct TurnsOptions
    {
        // --- features ---
        bool notes = true; // a note for slash commands the user ran outside a turn (e.g. /model before the first prompt)
        bool times = true; // timestamps in turn headers, on typed-while-it-worked labels, notes and background events
        bool ops = true; // the tool-call list under ● Claude: each call with the arguments that say what it did
        bool shortPaths = true; // paths inside the session's temp folder shown relative to it (with one legend line)
        bool fullBash = false; // the full, verbatim command in a code block under each Bash call
        bool stats = false; // a stats line per turn: tool calls, duration, API calls, model, context size, output tokens
        bool opMeta = true; // per call: how it ended (ok, exit code, background id, created/updated, lines read)
        bool opDuration = false; // per call: how long it took, before how it ended — only shown while opMeta is on
        bool opOutput = false; // per call: the first line of what it returned
        bool bgEvents = true; // per background call: when its task finished and with what exit code
        bool narration = true; // the assistant's remarks between tool calls, in order
        bool compactions = true; // a line where the context was compacted: trigger, tokens before -> after, duration
        bool answers = true; // the user's answers to AskUserQuestion (or how an unanswered one ended), and their words when rejecting a call
        bool nestedBlocks = true; // a full-bash / full-write code block is a '-' sub-item under its call (off: a bare block)
        bool compactedHistory = true; // the live-branch walk crosses EVERY compaction into the history it summarized
        bool chain = true; // the sessions this one was forked or plan-restarted from, back to the first, rendered as one history
        bool agents = true; // an entry per subagent / teammate a call spawned, read from its own transcript
        bool peerMessages = true; // a message from a teammate or another session is shown as theirs (✉ <name>), not the user's
        bool fullWrite = false; // the full content written by each Write call, in a code block
        // --- correctness rules (on in every preset; off only to inspect raw behavior) ---
        bool liveBranch = true; // follow only the live branch (a rewound prompt and what it produced are dropped)
        bool skipTaskNotices = true; // Claude Code's background-task notices are not user input
        bool skipSidechains = true; // skip subagent (sidechain) lines stored inline by older Claude Code versions
        bool skipSynthetic = true; // skip Claude Code's synthetic filler lines ("No response requested."); API errors are kept
        bool keepPrePromptWork = true; // show work done before the file's first prompt instead of dropping it
    };

    // The options a detail level stands for, on top of the latest preset (v9). A level sets the
    // reference's eleven LADDER features absolutely (notes, ops, fullBash, stats, opDuration, opOutput,
    // narration, compactions, answers, agents, fullWrite) and leaves every other feature at the
    // preset's value, exactly like `transcript_turns.py --minimal|--compact|(none)|--full|--max`.
    //   level — the level; an out-of-range value is treated as Default.
    // Returns the options. Never throws.
    TurnsOptions TurnsOptionsForLevel(TurnsLevel level) noexcept;

    // A level's lowercase name — "minimal", "compact", "default", "full", "max" — for log lines and
    // test names (an out-of-range value reads "default"). Never null, never throws.
    const wchar_t* TurnsLevelName(TurnsLevel level) noexcept;

    // When a transcript FILE was created, in seconds since the Unix epoch. Creation time is what
    // orders a fork after its parent (both start from the same copied lines, so content can't). The
    // chain walk asks through this so the harness can pin times a checkout scrambles.
    //   Return nullopt when the file can't be stat'ed (the walk then treats it as unreadable).
    using TurnsBirthFn = std::function<std::optional<double>(const std::filesystem::path&)>;

    // The default TurnsBirthFn: the file's NTFS creation time (GetFileAttributesExW).
    //   path — the transcript.
    // Returns the creation time in epoch seconds, or nullopt when the file can't be stat'ed. Never throws.
    std::optional<double> TranscriptFileBirth(const std::filesystem::path& path) noexcept;

    // Reads a transcript timestamp ("2026-09-26T19:34:04.478Z", any ISO-8601 offset) the way the
    // reference reads it for comparisons (aware_ts): a stamp without a zone counts as UTC.
    //   ts — the timestamp text.
    // Returns microseconds since the Unix epoch (UTC), or nullopt when empty or malformed. Never throws.
    std::optional<int64_t> TranscriptTimestampToUnixMicros(std::wstring_view ts) noexcept;

    // Parses JSONL bytes the way the reference does (parse_jsonl): one JSON value per line (split on
    // \n, \r\n and \r), keeping only the lines that parse to a JSON OBJECT. A line that is not
    // UTF-8, not JSON, carries anything after its value, or is not an object (a live file's torn
    // last line, a run of NULs from a torn write) is skipped, never fatal. A leading UTF-8 BOM on a
    // line is ignored. Pasted-image base64 payloads are elided first (see the file comment).
    //   bytes — the file's bytes.
    // Returns the objects in file order. Throws std::bad_alloc only (the caller decides; the render
    // entry points below contain it).
    std::vector<json::Value> ParseTranscriptJsonl(std::string bytes);

    // Renders one transcript's already-parsed lines — the reference's render(): no file is known,
    // so there is no chain to follow and no agent transcript to find (those features find nothing).
    //   lines — every line of the transcript, in file order (ParseTranscriptJsonl).
    //   opts  — the features and rules (TurnsOptionsForLevel).
    //   utc   — print times in UTC (the goldens) instead of this machine's local time.
    // Returns the Markdown document — at least "\n" for a transcript with no turns. An EMPTY string
    // means the render failed (out of memory); the failure is logged to hooks.log (Rule #18).
    // Never throws.
    std::wstring RenderTranscriptTurns(const std::vector<json::Value>& lines, const TurnsOptions& opts, bool utc) noexcept;

    // Reads and renders a transcript FILE — the reference's load_history + render_history. With
    // opts.chain it walks back through the sessions this one was forked (same project folder, the
    // copied uuids) or plan-restarted ("read the full transcript at: <parent>.jsonl") from, and with
    // opts.agents it reads each spawned agent's transcript from `<session>\subagents\` (or beside
    // the session's own, Claude Code 2.0.x). Reads only; never writes anything.
    // COST: the chain walk reads every older .jsonl in the transcript's project folder whose
    // creation time precedes it (a cheap byte search rejects non-relatives before parsing) — call
    // it OFF the UI thread.
    //   transcript — the session's .jsonl (e.g. ResolveClaudeTranscriptPath).
    //   opts       — the features and rules (TurnsOptionsForLevel).
    //   utc        — print times in UTC instead of local time.
    //   born       — file creation times; empty = TranscriptFileBirth. The harness pins them.
    // Returns the Markdown document, or nullopt when the transcript can't be read (gone, locked, its
    // creation time unreadable) or the render failed — a failure other than an unreadable file is
    // logged to hooks.log (Rule #18). Never throws.
    std::optional<std::wstring> RenderTranscriptTurnsFile(const std::filesystem::path& transcript,
                                                          const TurnsOptions& opts,
                                                          bool utc,
                                                          const TurnsBirthFn& born = {}) noexcept;
}
