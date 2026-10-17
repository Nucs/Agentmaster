// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the START NOTE (MESSAGING.md §4.3, §21.18): the one message the bot posts in the 🔎 Search topic every time it
// starts. The user's request (2026-10-18): "When agentmaster launches, it should send a message on telegram to Search topic
// mentioning that. This will help us notice restarts or regain accessability". An APP LAUNCH rings (a notification with sound:
// it is how the user notices a crash, an update or a reboot, and learns that the fleet answers from the phone again); a start
// of the bot inside an app that keeps running (messaging switched on later, a Settings save that restarted the bot) is a silent
// line that says so — a ring there would announce a restart that never happened.
//
// What the note says comes from two small files under <profile>\messaging\ that Messaging.cpp writes:
//   run.marker   rewritten every minute while the bot runs: "<unix ms> <version word>" (FormatRunMarker). The FIRST write of a
//                run keeps the previous run's copy as run.marker.last: when that run was last seen alive, and which version it
//                was — so the note can say "was dev 0.0.1.27" after an update and how long the bot was gone.
//   run.closed   "<unix ms>", written when the app's LAST window closes (Engine.cpp, UnregisterLiveWindowIn →
//                MessagingService::NoteAppClosing). The app ends with TerminateProcess (WindowEmperor) and the engine is never
//                destroyed, so MessagingService::Stop — the only other place that knows an end was clean — never runs at a
//                quit; without this stamp EVERY start read as a crash (the note of 2026-10-16..18 said "ended unexpectedly"
//                after every quit, with an empty time). A stamp OLDER than the run's last marker write belongs to an earlier
//                run, or to a moment the window set was briefly empty while the app went on (a tear-out of the last tab), and
//                proves nothing (StartNoteRunClosedCleanly).
// …and the app's version word (ServiceHooks::versionWord → FormatAppVersionWord: "0.6.8.21", "dev 0.0.1.28").
//
// This header is the PURE half — the marker's text both ways, the version word, the note's kind, its sound and its HTML — so
// the m5 suite pins every form (tests_messaging.cpp). Header-only, no I/O, no Win32, no WinRT. The service reads and removes
// the files, sends the note, logs whether it arrived and sends it again after a failure (MessagingService::_ensureSearchTopic,
// _pumpStartNote).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "TelegramFormat.h" // TgEscapeHtml, TgClockText, TgShortSpan, TgDayKey

namespace Agentmaster::Msg
{
    // ===== the timings ================================================================================================

    /// <summary>
    /// How long after the PROCESS started the bot's first start still counts as the app's launch. Past it, the bot was switched
    /// on in an app that was already running (the cog's Messaging tab), so the note says the BOT started, without a sound — the
    /// user is at the PC then, and a ring would claim a restart that did not happen. Generous on purpose: a heavy window restore
    /// delays the engine's init by tens of seconds, never by minutes.
    /// </summary>
    inline constexpr int64_t kStartNoteLaunchWindowMs = 5 * 60 * 1000;

    /// <summary>
    /// How late the note may go out before it says so. The note waits for Telegram (getMe with topics on, the Search topic, the
    /// send itself), so a note sent this long after the bot's start — no network at boot, a VPN not up yet, Threaded Mode off —
    /// names the start time and the wait: the message's own time in Telegram is then the moment access came BACK, which is the
    /// other thing the user wants to know ("regain accessibility"), not the moment of the launch.
    /// </summary>
    inline constexpr int64_t kStartNoteLateMs = 2 * 60 * 1000;

    // ===== the run marker =============================================================================================

    /// <summary>
    /// What the previous run's marker says (run.marker.last): the last minute that run was seen alive, and the version it ran.
    /// </summary>
    struct RunMarker
    {
        int64_t aliveMs{ 0 }; // Unix ms of the marker's last write; 0 = no marker, or one that does not start with a number
        std::wstring version; // the version word the run wrote; "" = a marker of a build before §21.18 (a bare number)
    };

    /// <summary>
    /// Reads a run marker's text: "1792273131650" (a build before §21.18) or "1792273131650 dev 0.0.1.28". Tolerant on purpose —
    /// the file can be torn by a power loss mid-write (WriteFileAtomic makes that unlikely, not impossible), and a marker that
    /// cannot be read must cost the note its history line, never the note.
    /// </summary>
    /// <param name="text">The file's bytes (ASCII by construction; anything else is dropped from the version).</param>
    /// <returns>The moment and the version; aliveMs 0 when the text does not start with a number (leading spaces allowed).</returns>
    /// <exception cref="std::bad_alloc">If the version text cannot be allocated.</exception>
    inline RunMarker ParseRunMarker(std::string_view text)
    {
        RunMarker m;
        size_t i = 0;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        {
            ++i;
        }
        int64_t ms = 0;
        size_t digits = 0;
        // 18 digits at most: a Unix-ms value has 13 today; the cap keeps a corrupt run of digits from overflowing int64.
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 18)
        {
            ms = ms * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0)
        {
            return m;
        }
        m.aliveMs = ms;
        // The version: everything after ONE separating space, printable ASCII only (the version word is ASCII by
        // construction — FormatRunMarker writes nothing else), trailing spaces and line breaks dropped, 64 characters at most.
        if (i < text.size() && text[i] == ' ')
        {
            ++i;
            for (; i < text.size() && m.version.size() < 64; ++i)
            {
                const char c = text[i];
                if (c >= 0x20 && c <= 0x7E)
                {
                    m.version.push_back(static_cast<wchar_t>(c));
                }
            }
            while (!m.version.empty() && m.version.back() == L' ')
            {
                m.version.pop_back();
            }
        }
        return m;
    }

    /// <summary>
    /// The text the tick loop writes to run.marker: the moment, then the version word when there is one. ASCII only, so a build
    /// before §21.18 that reads the file with std::stoll still finds its number first — a rollback reads the marker as before.
    /// </summary>
    /// <param name="aliveMs">The moment of this write (Unix ms).</param>
    /// <param name="version">The app's version word (FormatAppVersionWord; "" = none). Non-ASCII characters are dropped.</param>
    /// <returns>"1792273131650" or "1792273131650 dev 0.0.1.28".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::string FormatRunMarker(int64_t aliveMs, std::wstring_view version)
    {
        std::string out = std::to_string(static_cast<long long>(aliveMs));
        std::string word;
        for (const wchar_t c : version)
        {
            if (c >= 0x20 && c <= 0x7E && word.size() < 64)
            {
                word.push_back(static_cast<char>(c));
            }
        }
        // A version made only of spaces carries nothing: no separator for it either (the reader would keep an empty word).
        if (word.find_first_not_of(' ') != std::string::npos)
        {
            out += ' ';
            out += word;
        }
        return out;
    }

    /// <summary>
    /// Reads run.closed: the moment the app's last window closed. A torn or foreign file reads as "no clean close", which makes
    /// the note say the run did not close normally — the safe side (a false "it closed" would hide a crash).
    /// </summary>
    /// <param name="text">The file's bytes.</param>
    /// <returns>Unix ms; 0 when the text does not start with a number (leading spaces allowed).</returns>
    inline int64_t ParseRunClosed(std::string_view text) noexcept
    {
        size_t i = 0;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        {
            ++i;
        }
        int64_t ms = 0;
        size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 18) // 18 digits: no int64 overflow
        {
            ms = ms * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        return digits == 0 ? 0 : ms;
    }

    // ===== the version word ===========================================================================================

    /// <summary>
    /// The app's version word as the note and the marker show it: the release package's version as it is ("0.6.8.21"), the dev
    /// package's with "dev " before it (both installs post into their own bot, but a word that says which build came up is what
    /// tells a hot swap from a crash), a portable copy's .am-version stamp with "portable " before it. An empty version — an
    /// unpackaged local build, the test harness — gives "": the note then shows no version rather than a misleading "0.0.0".
    /// </summary>
    /// <param name="devPackage">The process runs as the AgentmasterDev package (Profiles::IsDevPackage).</param>
    /// <param name="portable">The process runs unpackaged from a portable copy (a `.portable` marker beside the exe).</param>
    /// <param name="version">"Major.Minor.Build.Revision" of the package, or the portable stamp; "" = unknown.</param>
    /// <returns>The word, or "".</returns>
    /// <exception cref="std::bad_alloc">If the word cannot be allocated.</exception>
    inline std::wstring FormatAppVersionWord(bool devPackage, bool portable, std::wstring_view version)
    {
        if (version.empty())
        {
            return {};
        }
        if (devPackage)
        {
            return L"dev " + std::wstring{ version };
        }
        if (portable)
        {
            return L"portable " + std::wstring{ version };
        }
        return std::wstring{ version };
    }

    // ===== the note ===================================================================================================

    /// <summary>
    /// Which start the note announces — the heading, the sound and the lines depend on it.
    /// </summary>
    enum class StartNoteKind : int
    {
        AppLaunch = 0, // the bot's first start, at the app's launch: "Agentmaster started", with sound
        BotStarted = 1, // the bot's first start, but well after the launch (messaging switched on later): silent
        BotRestarted = 2, // a later start of the bot in the same process (a Settings save restarted it): silent
    };

    /// <summary>
    /// Tells an app launch from a start of the bot inside a running app. MessagingService::_start runs once at the engine's
    /// init (the launch) and again whenever a Settings save stops and starts the bot (a new secretary token, messaging off and
    /// on); only the first, made within kStartNoteLaunchWindowMs of the process start, is the launch the user wants to hear.
    /// </summary>
    /// <param name="botStarts">How many times the bot has started in this process, this start included (1 = the first).</param>
    /// <param name="processStartedMs">When the process started (Unix ms; 0 = unknown, which reads as a launch).</param>
    /// <param name="botStartedMs">When this start of the bot ran (Unix ms).</param>
    /// <returns>The kind.</returns>
    inline StartNoteKind ClassifyStartNote(size_t botStarts, int64_t processStartedMs, int64_t botStartedMs) noexcept
    {
        if (botStarts > 1)
        {
            return StartNoteKind::BotRestarted;
        }
        if (processStartedMs > 0 && botStartedMs - processStartedMs > kStartNoteLaunchWindowMs)
        {
            return StartNoteKind::BotStarted;
        }
        return StartNoteKind::AppLaunch;
    }

    /// <summary>
    /// Everything the note says, gathered once per start of the bot (the files are read and removed then).
    /// </summary>
    struct StartNoteFacts
    {
        StartNoteKind kind{ StartNoteKind::AppLaunch };
        std::wstring version; // this run's version word ("" = not shown)
        RunMarker previous; // the previous run's marker (run.marker.last); aliveMs 0 = no previous run known
        int64_t closedMs{ 0 }; // run.closed (0 = absent): the moment the app's last window closed
        int64_t processStartedMs{ 0 }; // when the process started (0 = unknown)
        int64_t botStartedMs{ 0 }; // when this start of the bot ran (MessagingService::_startedMs)
        size_t topics{ 0 }; // the topics the state file kept from the last run
    };

    /// <summary>
    /// Whether the previous run ENDED BY A CLOSE (its last window closed) rather than by a crash, a forced stop (a hot swap's
    /// Stop-Process, the Task Manager), a Windows shutdown or a power loss. Only a close stamp at or after the run's last marker
    /// write counts: the marker is rewritten every minute, so a stamp older than it was followed by a minute of life — an
    /// earlier run's stamp, or a window set that was empty for a moment while the app went on.
    /// </summary>
    /// <param name="f">The facts.</param>
    /// <returns>True when the stamp proves a close.</returns>
    inline bool StartNoteRunClosedCleanly(const StartNoteFacts& f) noexcept
    {
        return f.previous.aliveMs > 0 && f.closedMs > 0 && f.closedMs >= f.previous.aliveMs;
    }

    /// <summary>
    /// Whether the note rings: only an app launch. The note is the user's one signal that Agentmaster came back (a crash, an
    /// update, a reboot), so it must notify with sound; a start of the bot inside a running app happens while the user is at
    /// the PC, and a sound would announce a restart of Agentmaster that never happened.
    /// </summary>
    /// <param name="f">The facts.</param>
    /// <returns>True = send with sound.</returns>
    inline bool StartNoteRings(const StartNoteFacts& f) noexcept
    {
        return f.kind == StartNoteKind::AppLaunch;
    }

    namespace detail
    {
        /// <summary>
        /// A moment for the note: a date-time entity Telegram renders in the READER's time zone and format — the time only
        /// ("t") on the note's own day, the short date and time ("dt") on another (formatting.md: `r|w?[dD]?[tT]?`) —, with the
        /// PC's clock time inside it (TgClockText), which an app that does not know the entity shows as it is. Never an empty
        /// entity: the note of v1 sent `&lt;tg-time …&gt;&lt;/tg-time&gt;` with nothing inside, and its time read as nothing.
        /// </summary>
        /// <param name="ms">The moment (Unix ms; &gt; 0).</param>
        /// <param name="nowMs">The note's clock (the "same day" test).</param>
        /// <returns>Telegram HTML.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring StartNoteTime(int64_t ms, int64_t nowMs)
        {
            const int64_t day = TgDayKey(ms);
            const bool sameDay = day != 0 && day == TgDayKey(nowMs);
            return L"<tg-time unix=\"" + std::to_wstring(ms / 1000) + L"\" format=\"" + (sameDay ? L"t" : L"dt") + L"\">" + TgEscapeHtml(TgClockText(ms, nowMs)) +
                   L"</tg-time>";
        }
    }

    /// <summary>
    /// The note as Telegram HTML, one fact per line:
    ///   "🟢 Agentmaster started · dev 0.0.1.29 (was dev 0.0.1.28)"  — the heading (bold), the version, a version change;
    ///   "The last run closed at 23:58 · offline 9m." or "⚠ The last run did not close normally (…): last seen running at 00:06 ·
    ///   offline about 1m." — how the previous run ended and how long the bot was gone (the marker is a minute's resolution,
    ///   hence "about" when only the marker knows);
    ///   "Agentmaster has been running since 10:02." / "Agentmaster kept running; a Settings change restarted the bot." — the
    ///   two silent kinds;
    ///   "⏳ Sent late: the bot started at 08:01, 14m ago, and could not post until now (…)." — a note that waited;
    ///   "48 topics kept from the last run."
    /// Built at each send attempt with the attempt's clock, so a note that goes out after a failed send still says how late
    /// it is. Plain words, no relative span that a re-render would change: the note is sent once, never edited.
    /// </summary>
    /// <param name="f">The facts (StartNoteFacts).</param>
    /// <param name="nowMs">The clock of this send (Unix ms).</param>
    /// <returns>The HTML text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildStartNoteText(const StartNoteFacts& f, int64_t nowMs)
    {
        std::wstring out;
        switch (f.kind)
        {
        case StartNoteKind::BotStarted:
            out = L"\U0001F7E2 <b>The Telegram bot started</b>";
            break;
        case StartNoteKind::BotRestarted:
            out = L"\U0001F504 <b>The Telegram bot restarted</b>";
            break;
        case StartNoteKind::AppLaunch:
        default:
            out = L"\U0001F7E2 <b>Agentmaster started</b>";
            break;
        }
        if (!f.version.empty())
        {
            out += L" \x00B7 " + TgEscapeHtml(f.version);
            // A version change is the one sign the note can give that the restart was an UPDATE (a hot swap stops the app
            // like a crash does). Not for a restart of the bot: the app — and so the version — did not change.
            if (f.kind != StartNoteKind::BotRestarted && !f.previous.version.empty() && f.previous.version != f.version)
            {
                out += L" (was " + TgEscapeHtml(f.previous.version) + L")";
            }
        }
        // How the previous run ended. A restart of the bot has none to tell: its Stop just removed the marker.
        if (f.kind != StartNoteKind::BotRestarted && f.previous.aliveMs > 0)
        {
            if (StartNoteRunClosedCleanly(f))
            {
                out += L"\nThe last run closed at " + detail::StartNoteTime(f.closedMs, nowMs) + L" \x00B7 offline " + TgShortSpan(nowMs - f.closedMs) + L".";
            }
            else
            {
                out += L"\n\x26A0 The last run did not close normally (a crash, a forced stop or a Windows shutdown): last seen running at " +
                       detail::StartNoteTime(f.previous.aliveMs, nowMs) + L" \x00B7 offline about " + TgShortSpan(nowMs - f.previous.aliveMs) + L".";
            }
        }
        if (f.kind == StartNoteKind::BotStarted && f.processStartedMs > 0)
        {
            out += L"\nAgentmaster has been running since " + detail::StartNoteTime(f.processStartedMs, nowMs) + L".";
        }
        else if (f.kind == StartNoteKind::BotRestarted)
        {
            out += L"\nAgentmaster kept running; a Settings change restarted the bot.";
        }
        // A note that waited: its Telegram time is when access came back, so the launch time goes into the text. The two
        // causes the service cannot tell apart from here are named both (the getMe gate waits for either).
        if (f.botStartedMs > 0 && nowMs - f.botStartedMs >= kStartNoteLateMs)
        {
            out += L"\n\x23F3 Sent late: the bot started at " + detail::StartNoteTime(f.botStartedMs, nowMs) + L", " + TgShortSpan(nowMs - f.botStartedMs) +
                   L" ago, and could not post until now (no connection to Telegram, or its topics were off).";
        }
        if (f.topics > 0)
        {
            out += L"\n" + std::to_wstring(f.topics) + (f.topics == 1 ? L" topic" : L" topics") + L" kept from the last run.";
        }
        return out;
    }
}
