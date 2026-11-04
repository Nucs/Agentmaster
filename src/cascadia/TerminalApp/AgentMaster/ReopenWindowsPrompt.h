// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the startup question "Reopen your N previous Agentmaster windows?" with its "Remember my choice" check box,
// and the read and the write of the remembered choice in settings.json (PERSISTENCE.md §18; the rule and the words:
// ReopenWindowsAtStart.h). Header-only, plain Win32.
//
// WHERE IT RUNS: in the app exe's prelude (WindowEmperor::HandleCommandlineArgs), after the profile is resolved and the
// open-at-exit scan found the windows to reopen, BEFORE any window, the engine or the splash exists. The exe links
// TerminalApp.dll, not the engine library, so Persistence.cpp's LoadAppSettings / SaveAppSettings are out of its reach:
// like Updater.h (the update prompt's Skip / Postpone) and ProfileBootstrap.h (debugMode), this header reads and writes its
// one settings.json key itself, inside the engine's {version, settings:{...}} envelope, where AppSettings round-trips it.
// It reuses Updater.h's settings.json file helpers: the lock-aware read that tells an unreadable file from a missing one
// and the atomic write (both log their own low-level failures under the "[update]" tag, before this header's "[reopen]"
// line names the outcome).
//
// NO RACE WITH THE ENGINE: the question is answered, and a remembered choice written, before this process creates its
// engine, whose first LoadAppSettings therefore already sees it; a second Agentmaster process on the same profile hands
// its command line to this one and exits before it reaches the question (single instance).

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstddef>
#include <filesystem>
#include <optional> // DecideReopenAtStartup: the remembered choice's write result, empty when nothing was ticked
#include <string>
#include <string_view>
#include <system_error>

#include "DialogEcho.h" // every dialog is echoed to the console of the command that started the process (CLI.md §2b)
#include "Json.h"
#include "ReopenWindowsAtStart.h" // the three choices, their settings.json words, the startup rule
#include "StartupTiming.h" // Startup::Note: the exe-safe tagged line into the profile's hooks.log
#include "Updater.h" // Updater::SettingsPath + the settings.json helpers (detail::ReadFileWideForUpdate, detail::WriteFileUtf8, SetMember, FindMember)

namespace Agentmaster::ReopenPrompt
{
    /// <summary>
    /// The settings.json format this build writes: the root <c>version</c> of the engine's settings document
    /// (Persistence.cpp AppSettingsDocument). A file with a GREATER root version was written by a newer Agentmaster in a
    /// format this build does not know, and the remembered choice is not written into it (PERSISTENCE.md §17, the format
    /// guard): the newer build must find the file as it left it.
    /// </summary>
    /// <remarks>Mirrors a literal in Persistence.cpp; the m5 test TestReopenWindowsAtStart pins the two equal, so a
    /// format bump there fails the suite until this follows.</remarks>
    inline constexpr double kSettingsDocumentVersion = 1;

    /// <summary>What ReadReopenWindowsAtStart found in settings.json.</summary>
    struct ReopenSettingRead
    {
        /// <summary>The remembered choice; ReopenWindowsAtStart::Ask whenever none could be read.</summary>
        ReopenWindowsAtStart choice{ ReopenWindowsAtStart::Ask };
        /// <summary>
        /// True when settings.json EXISTS but could not be read (another program holds it past the read's retries, an ACL,
        /// an internal failure). The choice then reads Ask, so the user is asked although a choice may be remembered:
        /// the log says why.
        /// </summary>
        bool unreadable{ false };
    };

    /// <summary>
    /// Reads the remembered choice (<c>settings.reopenWindowsAtStart</c>) from the profile's settings.json.
    /// </summary>
    /// <param name="stateDir">The active profile folder (Profiles::ResolveProfileDir in the exe; a scratch folder in the
    /// tests).</param>
    /// <returns>
    /// The choice and whether the file was unreadable. A missing file, an unparseable one, a missing or non-object
    /// <c>settings</c>, a missing key and an unknown word all read ReopenWindowsAtStart::Ask (fail toward asking:
    /// ReopenWindowsAtStartFromToken). Only the nested key counts: the setting is newer than the envelope, so no build
    /// ever wrote it at the top level (the Updater::ReadPrefs nightly precedent).
    /// </returns>
    /// <remarks>
    /// Uses the updater's lock-aware read, which retries a sharing violation for ~400 ms: an antivirus or sync client that
    /// holds the file for a moment at logon must not make a remembered choice look absent and bring the question back.
    /// Never throws: an internal failure (an allocation) reads Ask, marked unreadable, and is logged ([reopen] in
    /// hooks.log) by DecideReopenAtStartup, which owns the log line.
    /// </remarks>
    inline ReopenSettingRead ReadReopenWindowsAtStart(const std::wstring& stateDir) noexcept
    {
        ReopenSettingRead result;
        try
        {
            bool unreadable = false;
            const std::wstring raw = Updater::detail::ReadFileWideForUpdate(Updater::SettingsPath(stateDir), unreadable);
            if (unreadable)
            {
                result.unreadable = true;
                return result;
            }
            const auto parsed = json::Parse(raw);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                return result; // missing (raw "") or not a settings document: nothing remembered
            }
            const json::Value* settings = parsed->Find(L"settings");
            if (!settings || settings->type != json::Value::Type::Obj)
            {
                return result;
            }
            result.choice = ReopenWindowsAtStartFromToken(settings->StrAt(kReopenWindowsAtStartKey));
        }
        catch (...)
        {
            // std::bad_alloc only (the path, the text, the parse). No evidence of what the file holds: ask, and say why.
            result = ReopenSettingRead{};
            result.unreadable = true;
        }
        return result;
    }

    /// <summary>What WriteReopenWindowsAtStart did with a remembered choice.</summary>
    enum class RememberResult
    {
        /// <summary>The choice is in settings.json; the next start does not ask.</summary>
        Saved,
        /// <summary>
        /// settings.json exists but could not be read (a lock past the retries, an ACL): NOT written, because a write
        /// built from an unread file would replace every other setting with this one key (PERSISTENCE.md §15 / §17, the
        /// updater's lesson).
        /// </summary>
        RefusedUnreadable,
        /// <summary>
        /// settings.json holds text that is not a JSON object (unparseable, or an array / a number): not Agentmaster's
        /// settings document, so it is not rebuilt around one key; left byte for byte.
        /// </summary>
        RefusedNotSettings,
        /// <summary>settings.json's root <c>version</c> is newer than kSettingsDocumentVersion (a newer Agentmaster's
        /// format): left as it is (the format guard).</summary>
        RefusedNewerFormat,
        /// <summary>The atomic write failed, or an internal failure stopped it; the old file stays whole.</summary>
        WriteFailed,
    };

    /// <summary>
    /// Writes a remembered choice into the profile's settings.json: a freshest-disk read-modify-write that sets ONLY
    /// <c>settings.reopenWindowsAtStart</c> and keeps every other byte's meaning — every other setting, the writer stamp
    /// and its place as the first member, members this build does not know (PERSISTENCE.md §17's residue is the whole
    /// document here).
    /// </summary>
    /// <param name="stateDir">The active profile folder.</param>
    /// <param name="choice">The choice to remember (ReopenWindowsAtStart::Ask is legal and stores "ask", which is how
    /// "forget my choice" would be written; the startup question itself only ever stores Yes or No).</param>
    /// <returns>RememberResult::Saved, or the reason nothing was written (each refusal leaves the file untouched).</returns>
    /// <remarks>
    /// <para>A MISSING settings.json is created as the engine's envelope (<c>{"version":1,"settings":{...}}</c>), which the
    /// engine's first load reads with defaults for every other field. A <c>settings</c> member that is not an object is
    /// replaced by one in place (it holds no setting the engine could read; Updater::WriteUpdateState does the same).</para>
    /// <para>The writer stamp is left as it was, like the updater's write does: the stamp tells the next start whether an
    /// older build last saved the file (PERSISTENCE.md §17, the older-writer repair), and stamping this one-key change
    /// would hide such a save from the repair.</para>
    /// <para>Never throws: an internal failure is logged ("[reopen] ... CRASHED") and answers WriteFailed.</para>
    /// </remarks>
    inline RememberResult WriteReopenWindowsAtStart(const std::wstring& stateDir, ReopenWindowsAtStart choice) noexcept
    {
        try
        {
            const std::wstring path = Updater::SettingsPath(stateDir);
            bool unreadable = false;
            const std::wstring raw = Updater::detail::ReadFileWideForUpdate(path, unreadable);
            if (unreadable)
            {
                return RememberResult::RefusedUnreadable;
            }
            const auto parsed = json::Parse(raw);
            const bool haveObject = parsed && parsed->type == json::Value::Type::Obj;
            if (!raw.empty() && !haveObject)
            {
                return RememberResult::RefusedNotSettings;
            }
            json::Value root = haveObject ? *parsed : json::Value::MkObj();
            if (root.NumAt(L"version", 0) > kSettingsDocumentVersion)
            {
                return RememberResult::RefusedNewerFormat;
            }
            // Root-level structure first, the nested pointer last: every append to `root.members` can reallocate it, so
            // `settings` is taken only after the last root change and root is not touched after that.
            if (!Updater::FindMember(root, L"version"))
            {
                Updater::SetMember(root, L"version", json::Value::MkNum(kSettingsDocumentVersion));
            }
            json::Value* settings = Updater::FindMember(root, L"settings");
            if (!settings)
            {
                Updater::SetMember(root, L"settings", json::Value::MkObj());
                settings = Updater::FindMember(root, L"settings");
            }
            else if (settings->type != json::Value::Type::Obj)
            {
                *settings = json::Value::MkObj();
            }
            // SetMember REPLACES an existing key in place (Json.h's Value::Set would append a duplicate).
            Updater::SetMember(*settings, std::wstring{ kReopenWindowsAtStartKey }, json::Value::MkStr(std::wstring{ ReopenWindowsAtStartToken(choice) }));
            // The profile folder exists at every real call (the open-at-exit scan read it); a scratch test folder may not.
            // The error_code overload: a failure here surfaces as the write's failure below, never as an exception.
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path{ stateDir }, ec);
            return Updater::detail::WriteFileUtf8(path, json::Dump(root)) ? RememberResult::Saved : RememberResult::WriteFailed;
        }
        catch (...)
        {
            // std::bad_alloc only (the text, the parse, the dump). Nothing was renamed over the file (the atomic write
            // either replaced it whole or not at all), so the old settings stand. Rule #18: say so in hooks.log.
            try
            {
                Startup::Note(L"[reopen] remembering the choice CRASHED (exception): settings.json was not changed");
            }
            catch (...)
            {
                // logger-failed: an allocation failure while building the line; nothing left to report through
            }
            return RememberResult::WriteFailed;
        }
    }

    /// <summary>The log words for a RememberResult (the tail of the "[reopen] asked ..." line).</summary>
    /// <param name="result">What the write did.</param>
    /// <returns>A static, never-null text.</returns>
    /// <remarks>Never throws.</remarks>
    inline const wchar_t* RememberResultText(RememberResult result) noexcept
    {
        switch (result)
        {
        case RememberResult::Saved:
            return L"saved: the next starts do not ask (Settings, Behavior, Startup changes it)";
        case RememberResult::RefusedUnreadable:
            return L"NOT saved: settings.json exists but could not be read (it keeps every setting); the next start asks again";
        case RememberResult::RefusedNotSettings:
            return L"NOT saved: settings.json holds text that is not Agentmaster's settings (left as it is); the next start asks again";
        case RememberResult::RefusedNewerFormat:
            return L"NOT saved: a newer Agentmaster wrote settings.json in a newer format (left as it is); the next start asks again";
        case RememberResult::WriteFailed:
        default:
            return L"NOT saved: the write failed (the old file is whole); the next start asks again";
        }
    }

    /// <summary>
    /// Keeps the question on top and in front when it appears: the TaskDialog twin of the MessageBox flags
    /// MB_TOPMOST | MB_SETFOREGROUND the question carried before it had a check box.
    /// </summary>
    /// <param name="hwnd">The task dialog's window.</param>
    /// <param name="msg">The notification; only TDN_CREATED acts.</param>
    /// <param name="wParam">Unused (the notification's argument).</param>
    /// <param name="lParam">Unused (the notification's argument).</param>
    /// <param name="refData">Unused (TASKDIALOGCONFIG::lpCallbackData).</param>
    /// <returns>S_OK, so the dialog's default handling continues.</returns>
    /// <remarks>
    /// LOAD-BEARING, not cosmetic: this question is the ONLY surface the process has at this point — no terminal window
    /// exists yet, and the splash deliberately starts AFTER it (a TOPMOST splash would cover it) — while a window from a
    /// freshly launched process (one with no foreground rights when started from a shell, an alias or automation) opens
    /// BEHIND the current foreground window. The launch then sat blocked on an invisible modal with no taskbar presence:
    /// the 2026-07-03 "dev is not launching" report, where a 3-window reopen stalled 74 s until the box was found and
    /// clicked (hooks.log: reopen-scan +566ms -> prelude-done +74573ms). TOPMOST keeps it visible even when Windows
    /// refuses the foreground (it then shows inactive, on top, one click away). Never throws (plain Win32 calls).
    /// </remarks>
    inline HRESULT CALLBACK ReopenQuestionCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR refData) noexcept
    {
        (void)wParam;
        (void)lParam;
        (void)refData;
        if (msg == TDN_CREATED)
        {
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ::SetForegroundWindow(hwnd);
        }
        return S_OK;
    }

    /// <summary>
    /// The question as a task dialog: "Reopen your N previous Agentmaster windows?", Yes (the default) / No, and the
    /// "Remember my choice and don't ask again" check box. Built apart from showing it, so the m5 harness reads every word
    /// (through DialogEcho::DescribeTaskDialog) without a dialog on screen.
    /// </summary>
    /// <remarks>
    /// <para>The configuration POINTS INTO this object's strings, so the object can be neither copied nor moved: a copy
    /// would carry pointers into the original. Build it where the dialog is shown and keep it alive until the call
    /// returns.</para>
    /// <para>Kept on purpose, for the e2e driver (e2e/amdriver: MessageBox.find / press), the build server's install watch
    /// and tools/Launch-AgentmasterDev.ps1, which find and answer this question: the window class (#32770, every task
    /// dialog's), the title "Agentmaster", the main instruction starting "Reopen your N previous Agentmaster windows?"
    /// and the button words Yes / No. Like MB_YESNO, it offers no Cancel, Esc or close button: the only answers are Yes
    /// and No.</para>
    /// </remarks>
    class ReopenQuestion
    {
    public:
        /// <summary>Builds the question for a number of windows.</summary>
        /// <param name="windowCount">How many windows would be reopened (2 or more in practice; the number is shown).</param>
        /// <exception cref="std::bad_alloc">If the texts cannot be allocated.</exception>
        explicit ReopenQuestion(std::size_t windowCount) :
            _instruction{ L"Reopen your " + std::to_wstring(windowCount) + L" previous Agentmaster windows?" },
            _content{ L"Yes reopens every one of them with its tabs. No starts with a single window; the Manager's \x201CReopen Windows\x201D button can bring the rest back later." },
            _verification{ L"Remember my choice and don't ask again" },
            _footer{ L"Change it any time from the Agent Manager: \x2699 Settings \x2192 Behavior \x2192 Startup." }
        {
            _config.cbSize = sizeof(_config);
            _config.hwndParent = nullptr; // no window exists yet: the dialog owns itself (and is kept in front by the callback)
            _config.pszWindowTitle = L"Agentmaster";
            _config.pszMainIcon = TD_INFORMATION_ICON;
            _config.pszMainInstruction = _instruction.c_str();
            _config.pszContent = _content.c_str();
            _config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
            _config.nDefaultButton = IDYES; // Enter reopens, as the MessageBox's first (default) button did
            _config.pszVerificationText = _verification.c_str(); // unticked by default: remembering is the user's act
            _config.pszFooter = _footer.c_str();
            _config.pfCallback = ReopenQuestionCallback;
            // No TDF_ALLOW_DIALOG_CANCELLATION: without a Cancel button that leaves no Esc and no close button, so the
            // answer is always a real Yes or No (MB_YESNO's behavior) and a remembered choice is never made by a dismissal.
        }

        ReopenQuestion(const ReopenQuestion&) = delete;
        ReopenQuestion& operator=(const ReopenQuestion&) = delete;
        ReopenQuestion(ReopenQuestion&&) = delete;
        ReopenQuestion& operator=(ReopenQuestion&&) = delete;
        ~ReopenQuestion() = default;

        /// <summary>The dialog's configuration, for DialogEcho::ShowTaskDialogIndirect (or DescribeTaskDialog in a test).</summary>
        /// <returns>A reference valid while this object lives.</returns>
        /// <remarks>Never throws.</remarks>
        const TASKDIALOGCONFIG& Config() const noexcept
        {
            return _config;
        }

    private:
        std::wstring _instruction; // "Reopen your N previous Agentmaster windows?" (the e2e driver matches its start)
        std::wstring _content; // what Yes and No do
        std::wstring _verification; // the "Remember my choice" check box
        std::wstring _footer; // where the remembered choice is changed later
        TASKDIALOGCONFIG _config{}; // points into the four strings above (hence no copy, no move)
    };

    /// <summary>The user's answer to the question.</summary>
    struct ReopenAnswer
    {
        /// <summary>True for Yes (reopen the windows); false for No, and when nothing could be asked.</summary>
        bool reopen{ false };
        /// <summary>True when "Remember my choice" was ticked on a real Yes / No answer.</summary>
        bool remember{ false };
        /// <summary>
        /// True when the task dialog asked. False when it could not be shown (comctl32 v6 unavailable) and the plain Yes /
        /// No message box asked instead — that box has no check box, so nothing can be remembered — or when nothing could
        /// be asked at all.
        /// </summary>
        bool taskDialog{ false };
    };

    /// <summary>
    /// Asks "Reopen your N previous Agentmaster windows?" and waits for the answer (a modal dialog on the calling thread),
    /// echoed to the console of the command that started the process (DialogEcho.h).
    /// </summary>
    /// <param name="windowCount">How many windows would be reopened.</param>
    /// <returns>The answer.</returns>
    /// <remarks>
    /// <para>The task dialog first; when TaskDialogIndirect fails (the exe's manifest declares Common Controls v6, so in
    /// practice never) the original Yes / No message box asks with its original words and flags, so the start is never
    /// left without the question — it only loses the check box.</para>
    /// <para>Never throws: an allocation failure while building the texts answers "No, nothing remembered", the least
    /// destructive answer — the windows stay one click away behind Reopen Windows (N), exactly what a message box that
    /// failed to show gave before — and is logged by DecideReopenAtStartup's line ("could not be asked").</para>
    /// </remarks>
    inline ReopenAnswer AskToReopenWindows(std::size_t windowCount) noexcept
    {
        ReopenAnswer answer;
        try
        {
            const ReopenQuestion question{ windowCount };
            int pressed = 0;
            BOOL ticked = FALSE;
            if (SUCCEEDED(DialogEcho::ShowTaskDialogIndirect(&question.Config(), &pressed, nullptr, &ticked)))
            {
                answer.taskDialog = true;
                answer.reopen = pressed == IDYES;
                // Only a real Yes or No is remembered: the dialog offers no other way out, but an id Windows reports for a
                // dialog that was destroyed under it (0) must never be stored as "No, remembered".
                answer.remember = ticked != FALSE && (pressed == IDYES || pressed == IDNO);
                return answer;
            }
            const std::wstring fallback = L"Reopen your " + std::to_wstring(windowCount) +
                                          L" previous Agentmaster windows?\r\n\r\n(Choose No to start with a single window; the Manager's \x201CReopen Windows\x201D button can bring the rest back later.)";
            answer.reopen = DialogEcho::ShowMessageBox(nullptr, fallback.c_str(), L"Agentmaster", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
        }
        catch (...)
        {
            // std::bad_alloc only (the texts). Nothing was asked: not reopened, nothing remembered (see remarks).
            answer = ReopenAnswer{};
        }
        return answer;
    }

    namespace detail
    {
        /// <summary>
        /// Writes one "[reopen] ..." line into the profile's hooks.log through the exe-safe writer, building it only here,
        /// so a failure to build the line can never change the decision it reports.
        /// </summary>
        /// <typeparam name="Build">A callable returning the line (std::wstring), tag included.</typeparam>
        /// <param name="build">Builds the line.</param>
        /// <remarks>Never throws: an allocation failure loses this line only (the logger has nowhere left to report it).
        /// Call only after the profile is resolved (Startup::Note's Rule #15 contract).</remarks>
        template<typename Build>
        inline void NoteReopen(Build&& build) noexcept
        {
            try
            {
                Startup::Note(build());
            }
            catch (...)
            {
                // logger-failed: an allocation failure while building or copying the line; nothing left to report through
            }
        }
    }

    /// <summary>
    /// The start's whole decision about the windows that were open at the last exit: whether to dispatch them. Reads the
    /// remembered choice, asks when it must, remembers a ticked answer, and logs one "[reopen]" line per decision that
    /// involved several windows.
    /// </summary>
    /// <param name="stateDir">The active profile folder (already resolved: the open-at-exit scan read it).</param>
    /// <param name="windowCount">How many open-at-exit windows the scan found.</param>
    /// <returns>
    /// True to reopen every one of them; false to reopen none (the caller then dispatches nothing, and its
    /// <c>_windows.empty()</c> fallback opens the single default window, which takes over the first saved record no other
    /// window claimed: PERSISTENCE.md §13.5). One window → true and none → false without reading anything
    /// (DecideReopenAtStart's lone-window rule); several → the remembered choice, else the user's answer.
    /// </returns>
    /// <remarks>
    /// <para>A remembered Yes or No answers without a dialog, and the "[reopen]" line says so, naming the setting — the
    /// one trace a user who forgot they ticked the box will find (and the cog's Behavior tab shows the choice).</para>
    /// <para>A ticked answer is honored for THIS start even when it cannot be remembered (an unreadable or foreign
    /// settings.json): the line then says the next start asks again.</para>
    /// <para>Blocks the calling thread while the question is up (it is modal). Never throws: every step answers in a
    /// defined way when it fails (an unreadable setting asks; a question that cannot be shown reopens nothing).</para>
    /// </remarks>
    inline bool DecideReopenAtStartup(const std::wstring& stateDir, std::size_t windowCount) noexcept
    {
        if (windowCount < 2)
        {
            // Nothing to ask about: the rule needs no setting, so settings.json is not even read on this path (almost
            // every start takes it).
            return DecideReopenAtStart(windowCount, ReopenWindowsAtStart::Ask) == ReopenStartAction::ReopenAll;
        }
        const ReopenSettingRead setting = ReadReopenWindowsAtStart(stateDir);
        switch (DecideReopenAtStart(windowCount, setting.choice))
        {
        case ReopenStartAction::ReopenAll:
            detail::NoteReopen([&] {
                return L"[reopen] " + std::to_wstring(windowCount) + L" windows reopened without asking (remembered: reopenWindowsAtStart=" +
                       std::wstring{ ReopenWindowsAtStartToken(setting.choice) } + L"; Settings, Behavior, Startup changes it)";
            });
            return true;
        case ReopenStartAction::OpenSingleWindow:
            detail::NoteReopen([&] {
                return L"[reopen] " + std::to_wstring(windowCount) +
                       L" windows were open at the last exit: started with a single window without asking, the others wait behind Reopen Windows (remembered: reopenWindowsAtStart=" +
                       std::wstring{ ReopenWindowsAtStartToken(setting.choice) } + L"; Settings, Behavior, Startup changes it)";
            });
            return false;
        case ReopenStartAction::Ask:
        default:
            break;
        }

        const ReopenAnswer answer = AskToReopenWindows(windowCount);
        // Remember BEFORE the log line, so the line can say whether it worked; the write happens before this process
        // creates its engine (see the file header), so the engine's first settings load already sees the choice. Empty
        // when the box was not ticked: nothing was tried, so there is no result to report.
        const std::optional<RememberResult> remembered =
            answer.remember ? std::optional<RememberResult>{ WriteReopenWindowsAtStart(stateDir, RememberedReopenChoice(answer.reopen)) } : std::nullopt;
        detail::NoteReopen([&] {
            std::wstring line = L"[reopen] asked about " + std::to_wstring(windowCount) + L" windows";
            if (setting.unreadable)
            {
                line += L" (settings.json could not be read, so a remembered choice could not be seen)";
            }
            line += answer.reopen ? L": Yes, reopened them" : L": No, a single window";
            if (remembered)
            {
                line += L"; remember reopenWindowsAtStart=" + std::wstring{ ReopenWindowsAtStartToken(RememberedReopenChoice(answer.reopen)) } + L": " +
                        RememberResultText(*remembered);
            }
            else if (!answer.taskDialog)
            {
                line += L" (the task dialog could not be shown or nothing could be asked: no Remember my choice)";
            }
            return line;
        });
        return answer.reopen;
    }
}
