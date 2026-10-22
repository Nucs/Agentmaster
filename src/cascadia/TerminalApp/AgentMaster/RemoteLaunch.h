// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — RemoteLaunch: the one request shape a REMOTE surface (the Telegram bot, MESSAGING.md
// §5.4 / §7.3) uses to open a session into a tab — resume a closed one, fork one, start a new one in
// a folder — and the one ANSWER it gets back (MESSAGING.md §21.23). The engine routes the request to ONE
// window (Engine.h RequestLaunchInWindow — the most recently active window), whose sink runs the same
// seams the Sessions page's buttons run and answers with a RemoteLaunchOutcome: the session the tab hosts,
// or why nothing opened. A tiny header of its own so Engine.h and Messaging.h share it without either
// including the other.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "InterruptedTurns.h" // ForkTurnChoice (HOOKS.md "Interrupted turns")

namespace Agentmaster
{
    struct RemoteLaunchRequest
    {
        enum class Kind
        {
            NewSession, // a fresh session in `dir` (TerminalPage::_SpawnClaudeSession)
            Resume, // `sessionId` resumed into a tab (TerminalPage::_ResumeSessionFromDisk)
            Fork, // a fork of `sessionId` (TerminalPage::_ForkSessionFromDisk)
        };
        Kind kind{ Kind::NewSession };
        std::wstring sessionId; // Resume / Fork: the session on disk
        std::wstring dir; // the working directory (NewSession; Resume / Fork: the session's)
        std::wstring title; // the tab title to pin ("" = derived)
        std::wstring model; // a per-launch `--model <id>` ("" = the settings model)
        bool pin{ false }; // MESSAGING.md §5.3: pin the opened session (never idle) once it is live
        // MESSAGING.md §21.17 (NewSession only): the session id the new session takes, minted by the requester with
        // NewSessionId — the Telegram bot binds the user's thread to it BEFORE the tab exists, so the thread becomes the
        // session's topic as soon as the session is live (no topic of its own is made). "" = the launch mints one.
        std::wstring newSessionId;
        // MESSAGING.md §21.25 (the new-thread wizard's window step): the window the tab opens in — WindowCensus::windowId, the
        // window the user picked by its number. When set, Engine.h RequestLaunchInWindow asks THAT window only: a window that
        // closed since the census answers nothing (the request returns false), and the wizard asks again, rather than the tab
        // landing in a window the user did not pick. "" = the engine's choice (the window activated last, then the others).
        std::wstring windowId;
        // MESSAGING.md §21.25 (NewSession only): the new tab opens RIGHT AFTER this session's tab when the window that takes the
        // request hosts it — the wizard's default places a new session beside the newest tab working in the same folder. A
        // session the window does not host (it moved or closed since the census) leaves the tab at the strip's end, as "" does.
        std::wstring afterSessionId;
        // HOOKS.md "Interrupted turns" (Fork only): what the fork does about the source's unfinished turn, which Claude Code's
        // resume would continue in the fork by itself. Interrupted arms the window's fork stop watch (the fork's tab locked, ONE
        // Esc the moment it runs); Uninterrupted lets it run. Ask makes the BOT ask in the topic first (its _requestLaunch
        // resolves it before the request reaches a window; a window that still sees Ask forks Uninterrupted).
        ForkTurnChoice forkTurn{ ForkTurnChoice::Uninterrupted };
    };

    /// <summary>
    /// MESSAGING.md §21.23: what a window's launch came to — the one fact its answer carries. A launch asked from the phone has
    /// nobody at the PC to read a dialog, so every way it can end is a value here that goes back to the requester; none is shown
    /// as a modal on the PC (TerminalPage::_RemoteLaunch records what the seams would have shown).
    /// </summary>
    /// <remarks>
    /// Opened and AlreadyOpen are the two successes (RemoteLaunchOpened); every other value means NOTHING opened. Keep the
    /// list in step with RemoteLaunchResultName and RemoteLaunchRefusalText, which name each value for the logs and the phone.
    /// </remarks>
    enum class RemoteLaunchResult
    {
        Opened, // a NEW background tab hosts the session (RemoteLaunchOutcome::sessionId); its claude starts when the tab is started (the bot's first prompt, or its "start" action)
        AlreadyOpen, // the session was open already (a resume of a live session): its tab stands, nothing new opened
        WindowClosed, // the window that took the request closed, or dropped it unrun, before it acted
        ClaudeMissing, // no native claude.exe on the PC (EnsureClaudeAvailable): nothing was started
        PresetRefused, // the launch's preset cannot start (no key, no Base URL, a deleted preset — `detail` says which)
        NoFolder, // a resume or a fork whose working folder is not known
        Failed, // the tab could not be made, or an error was caught — `detail` says what, when the PC knows
    };

    /// <summary>
    /// MESSAGING.md §21.23: a window's answer to one RemoteLaunchRequest — the value RequestLaunchInWindow's reply carries,
    /// exactly once per accepted request.
    /// </summary>
    struct RemoteLaunchOutcome
    {
        RemoteLaunchResult result{ RemoteLaunchResult::Failed }; // the default reads "failed": an answer nobody filled in never passes for a success
        // Opened / AlreadyOpen: the session the tab hosts — a fork's NEW id, a new session's id (the requester's newSessionId
        // when it chose one), a resume's own id or the FRESH one it fell back to. "" for every refusal.
        std::wstring sessionId;
        // Opened: a NEW conversation opened in place of the one asked for — a resume whose transcript was gone, or a fork of a
        // session with no transcript (both fall back to a fresh session in the same folder, Correctness Rule #6).
        bool fresh{ false };
        std::wstring detail; // PresetRefused / Failed: the PC's own words for the refusal (plain text, may be ""); "" otherwise
        std::wstring windowId; // the window that answered; "" when the engine answered for a window that dropped the request
    };

    /// <summary>
    /// MESSAGING.md §21.23: the callback RequestLaunchInWindow hands the request's answer to. It is called EXACTLY ONCE for a
    /// request the engine accepted, from ANY thread — the window's UI thread when it acted, or whichever thread drops the last
    /// copy of the reply when the window never ran the request (then with WindowClosed) — and possibly before
    /// RequestLaunchInWindow returned. So it must be quick, must not need a lock its requester holds while asking, and must
    /// not throw (the engine contains a throw, Rule #18, but the answer is then lost).
    /// </summary>
    using RemoteLaunchReply = std::function<void(const RemoteLaunchOutcome& outcome)>;

    /// <summary>
    /// MESSAGING.md §21.23: how long a remote surface waits for a launch's answer and for the session it names to be live before
    /// it tells the user that the PC never answered. Three minutes covers a window whose UI thread is busy (a large restore, a
    /// stall); the answer itself normally comes within a second.
    /// </summary>
    inline constexpr int64_t kRemoteLaunchTimeoutMs = 3 * 60 * 1000;

    /// <summary>MESSAGING.md §21.23: whether a launch's answer says a tab hosts the session (Opened or AlreadyOpen).</summary>
    /// <param name="outcome">A window's answer.</param>
    /// <returns>True for Opened and AlreadyOpen; false for every refusal.</returns>
    [[nodiscard]] inline bool RemoteLaunchOpened(const RemoteLaunchOutcome& outcome) noexcept
    {
        return outcome.result == RemoteLaunchResult::Opened || outcome.result == RemoteLaunchResult::AlreadyOpen;
    }

    /// <summary>
    /// MESSAGING.md §21.23: the result's word for the logs ("opened", "already-open", "window-closed", "claude-missing",
    /// "preset-refused", "no-folder", "failed") — one stable token per value, so a log line can be searched for it.
    /// </summary>
    /// <param name="result">The result to name.</param>
    /// <returns>A static string; "failed" for a value the switch does not know (a value added without a name).</returns>
    [[nodiscard]] inline const wchar_t* RemoteLaunchResultName(RemoteLaunchResult result) noexcept
    {
        switch (result)
        {
        case RemoteLaunchResult::Opened:
            return L"opened";
        case RemoteLaunchResult::AlreadyOpen:
            return L"already-open";
        case RemoteLaunchResult::WindowClosed:
            return L"window-closed";
        case RemoteLaunchResult::ClaudeMissing:
            return L"claude-missing";
        case RemoteLaunchResult::PresetRefused:
            return L"preset-refused";
        case RemoteLaunchResult::NoFolder:
            return L"no-folder";
        case RemoteLaunchResult::Failed:
        default:
            return L"failed";
        }
    }

    /// <summary>
    /// MESSAGING.md §21.23: why a launch opened nothing, in plain words for the person who asked from the phone — one or two
    /// whole sentences, ending in a period, that a surface puts after its own lead ("The new session did not start.").
    /// The PC's own words (`detail`) are used as they are when the PC gave some; otherwise the result's generic sentence.
    /// </summary>
    /// <param name="outcome">A window's answer.</param>
    /// <returns>The reason's sentences (plain text: a surface that renders HTML escapes it); "" for a success (Opened,
    /// AlreadyOpen), which needs no reason.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    [[nodiscard]] inline std::wstring RemoteLaunchRefusalText(const RemoteLaunchOutcome& outcome)
    {
        // The PC's own words, edge-trimmed; a sentence the PC left without its period gets one, so the phone's message reads as
        // whole sentences whatever the seam wrote.
        std::wstring detail = outcome.detail;
        const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (!detail.empty() && isSpace(detail.back()))
        {
            detail.pop_back();
        }
        size_t lead = 0;
        while (lead < detail.size() && isSpace(detail[lead]))
        {
            ++lead;
        }
        detail.erase(0, lead);
        if (!detail.empty() && detail.back() != L'.' && detail.back() != L'!' && detail.back() != L'?')
        {
            detail += L'.';
        }
        switch (outcome.result)
        {
        case RemoteLaunchResult::Opened:
        case RemoteLaunchResult::AlreadyOpen:
            return {};
        case RemoteLaunchResult::WindowClosed:
            return L"The window that took the request closed before it opened the tab.";
        case RemoteLaunchResult::ClaudeMissing:
            // The PC's dialog says the same at more length (TerminalPage::_PromptClaudeMissing): the two fixes the user can make.
            return L"Claude Code (the native claude.exe) was not found on the PC. Install it there, or set its path in Settings \x2192 Claude binary.";
        case RemoteLaunchResult::PresetRefused:
            // The preset's refusal names the preset and what it lacks (PresetLaunchRefusal / CompileLaunchForSpawn); the tail
            // is the PC dialog's own rule (TerminalPage::_PromptPresetLaunchError).
            return (detail.empty() ? std::wstring{ L"Its launch preset cannot start." } : detail) +
                   L" Nothing was started: a preset session never falls back to your Claude login.";
        case RemoteLaunchResult::NoFolder:
            return L"The session's working folder is not known.";
        case RemoteLaunchResult::Failed:
        default:
            return detail.empty() ? std::wstring{ L"The PC could not open it; its hooks.log says why." } : detail;
        }
    }

    /// <summary>
    /// MESSAGING.md §21.23: what a launch request was, in plain words for a line that tells its fate — "the resume of “T”",
    /// "the fork of “T”" (T = the request's title, else the session's short id), "the new session in F" (F = the folder's
    /// last name, else the whole path). The title is printed on ONE line: each line break, with the spaces and tabs around it,
    /// becomes one space (the rule of every title the bot prints, TgOneLineTitle — a tab title can hold the lone CR of the
    /// tab's rename box).
    /// </summary>
    /// <param name="req">The request.</param>
    /// <param name="capital">True for the start of a sentence ("The fork of …"), false for its middle ("the fork of …").</param>
    /// <returns>The phrase, plain text (a surface that renders HTML escapes it).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    [[nodiscard]] inline std::wstring RemoteLaunchSubject(const RemoteLaunchRequest& req, bool capital)
    {
        // One line: a run of line breaks — with the blanks on both sides — is one space; blanks elsewhere stay as they are.
        const auto oneLine = [](const std::wstring& text) {
            std::wstring out;
            out.reserve(text.size());
            size_t i = 0;
            while (i < text.size())
            {
                const wchar_t c = text[i];
                if (c != L'\r' && c != L'\n')
                {
                    out += c;
                    ++i;
                    continue;
                }
                while (!out.empty() && (out.back() == L' ' || out.back() == L'\t'))
                {
                    out.pop_back();
                }
                while (i < text.size() && (text[i] == L'\r' || text[i] == L'\n' || text[i] == L' ' || text[i] == L'\t'))
                {
                    ++i;
                }
                if (!out.empty() && i < text.size())
                {
                    out += L' '; // a break at the start or the end leaves nothing
                }
            }
            return out;
        };
        if (req.kind == RemoteLaunchRequest::Kind::NewSession)
        {
            // The folder's last name: the separators at the end dropped first ("K:\src\app\" -> "app"); a drive root keeps its path.
            std::wstring dir = req.dir;
            while (dir.size() > 1 && (dir.back() == L'\\' || dir.back() == L'/'))
            {
                dir.pop_back();
            }
            const size_t cut = dir.find_last_of(L"\\/");
            std::wstring leaf = cut == std::wstring::npos ? dir : dir.substr(cut + 1);
            if (leaf.empty() || (leaf.size() == 2 && leaf[1] == L':'))
            {
                leaf = req.dir; // "C:" or nothing left: the path as given
            }
            return std::wstring{ capital ? L"The" : L"the" } + L" new session in " + (leaf.empty() ? std::wstring{ L"its folder" } : leaf);
        }
        std::wstring name = oneLine(req.title);
        if (name.empty())
        {
            name = req.sessionId.substr(0, 8); // the short id, as the logs and the cards print one
        }
        const wchar_t* what = req.kind == RemoteLaunchRequest::Kind::Fork ? L" fork of \x201C" : L" resume of \x201C";
        return std::wstring{ capital ? L"The" : L"the" } + what + name + L"\x201D";
    }
}
