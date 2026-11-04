// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 harness — SEVERAL WINDOWS AT START (doc/agentmaster/PERSISTENCE.md §18; AgentMaster/ReopenWindowsAtStart.h,
// AgentMaster/ReopenWindowsPrompt.h).
//
// The startup question "Reopen your N previous Agentmaster windows?" gained a "Remember my choice" check box, and the cog's
// Behavior tab a setting that answers it for good. What is pinned here, without a dialog ever on screen:
//   * the three choices' settings.json words, both ways, and that anything else reads Ask (fail toward asking);
//   * the startup rule (DecideReopenAtStart): none -> the single default window, a lone window -> always back without a question,
//     several -> the remembered choice, else the question;
//   * the exe's settings format guard constant == the engine's settings document version;
//   * the question's words through DialogEcho's description (what the console echo prints and what the e2e driver, the
//     build server's install watch and tools\Launch-AgentmasterDev.ps1 rely on: the title, the instruction's start, Yes /
//     No with Yes the default, the unticked check box, no Cancel);
//   * the exe's read of the choice (nested key only, unknown words, a file that cannot be read reported as such);
//   * the exe's write of the choice: a missing file becomes the engine's envelope, every other byte's meaning kept (the
//     writer stamp first, other settings, a newer build's members), the key replaced in place, and every refusal (a newer
//     format, text that is not a settings object, a file that cannot be read) leaving the file byte for byte;
//   * the start's whole decision for the cases that never ask (none, one, a remembered Yes / No) and its "[reopen]" lines
//     in the profile's hooks.log. The asking path itself shows a modal dialog, so it is a manual / e2e check (flows 07,
//     TAB-11), never run here: every DecideReopenAtStartup call below is guarded so that it cannot reach the question.

#include "m5_tests.h"

#include <sddl.h> // ConvertStringSecurityDescriptorToSecurityDescriptorW: the settings.json that can be replaced but not read

#include "../ReopenWindowsPrompt.h"

namespace
{
    /// <summary>A fresh, empty-named scratch folder under %TEMP% for one block of this test (never the live profile).</summary>
    /// <param name="what">A short tag naming the block, for a reader of %TEMP%.</param>
    /// <returns>The folder's path; it does not exist yet (each block creates it, or lets the code under test do so).</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    std::wstring ReopenScratchDir(const wchar_t* what)
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        return std::wstring{ tmp } + L"am-reopen-test-" + what + L"-" + NewSessionId();
    }

    /// <summary>How many times a text holds a needle (a key written twice would mean an append, not a replace).</summary>
    /// <param name="text">The haystack.</param>
    /// <param name="needle">The text to count (non-empty).</param>
    /// <returns>The number of occurrences, overlapping ones included.</returns>
    /// <remarks>Never throws.</remarks>
    size_t CountOf(const std::wstring& text, std::wstring_view needle) noexcept
    {
        size_t n = 0;
        for (size_t at = text.find(needle); at != std::wstring::npos; at = text.find(needle, at + 1))
        {
            ++n;
        }
        return n;
    }

    /// <summary>Removes a scratch folder and everything in it; a failure only leaves litter in %TEMP%.</summary>
    /// <param name="dir">The folder.</param>
    /// <remarks>Never throws (the error_code overload).</remarks>
    void RemoveScratch(const std::wstring& dir) noexcept
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }
}

// PERSISTENCE.md §18: several windows at start — the words, the rule, the question, the read, the write, the decision.
void TestReopenWindowsAtStart()
{
    std::wprintf(L"Several windows at start (PERSISTENCE.md 18: the reopen question's Remember my choice + the Behavior setting):\n");
    namespace RP = ::Agentmaster::ReopenPrompt;
    namespace U = ::Agentmaster::Updater;
    using RA = ReopenStartAction;

    // --- the words: one spelling per choice, read back exactly; anything else is Ask ---
    CHECK(ReopenWindowsAtStartToken(ReopenWindowsAtStart::Ask) == L"ask" && ReopenWindowsAtStartToken(ReopenWindowsAtStart::ReopenAll) == L"reopen" &&
              ReopenWindowsAtStartToken(ReopenWindowsAtStart::OneWindow) == L"oneWindow",
          "words: ask / reopen / oneWindow");
    for (const auto choice : { ReopenWindowsAtStart::Ask, ReopenWindowsAtStart::ReopenAll, ReopenWindowsAtStart::OneWindow })
    {
        CHECK(ReopenWindowsAtStartFromToken(ReopenWindowsAtStartToken(choice)) == choice, "words: every choice's word reads back as that choice");
    }
    CHECK(ReopenWindowsAtStartFromToken(L"") == ReopenWindowsAtStart::Ask, "words: no word (every settings.json from before the setting) reads Ask");
    CHECK(ReopenWindowsAtStartFromToken(L"Reopen") == ReopenWindowsAtStart::Ask, "words: the words are case-sensitive, so another spelling reads Ask");
    CHECK(ReopenWindowsAtStartFromToken(L"alwaysAsk2") == ReopenWindowsAtStart::Ask, "words: a newer build's word reads Ask");
    CHECK(ReopenWindowsAtStartToken(static_cast<ReopenWindowsAtStart>(7)) == L"ask", "words: an out-of-range value is written as ask");

    // --- the rule ---
    for (const auto setting : { ReopenWindowsAtStart::Ask, ReopenWindowsAtStart::ReopenAll, ReopenWindowsAtStart::OneWindow })
    {
        CHECK(DecideReopenAtStart(0, setting) == RA::OpenSingleWindow, "rule: no window to reopen opens the single default window, whatever the setting");
        CHECK(DecideReopenAtStart(1, setting) == RA::ReopenAll, "rule: a lone window always comes back without a question, whatever the setting");
    }
    CHECK(DecideReopenAtStart(2, ReopenWindowsAtStart::Ask) == RA::Ask && DecideReopenAtStart(12, ReopenWindowsAtStart::Ask) == RA::Ask,
          "rule: several windows and Ask: the question");
    CHECK(DecideReopenAtStart(2, ReopenWindowsAtStart::ReopenAll) == RA::ReopenAll, "rule: several windows and a remembered Yes: reopened, no question");
    CHECK(DecideReopenAtStart(5, ReopenWindowsAtStart::OneWindow) == RA::OpenSingleWindow, "rule: several windows and a remembered No: a single window, no question");
    CHECK(DecideReopenAtStart(3, static_cast<ReopenWindowsAtStart>(7)) == RA::Ask, "rule: an out-of-range setting asks");
    CHECK(RememberedReopenChoice(true) == ReopenWindowsAtStart::ReopenAll && RememberedReopenChoice(false) == ReopenWindowsAtStart::OneWindow,
          "rule: a remembered Yes stores reopen, a remembered No stores oneWindow");

    // --- the format the exe guards is the one the engine writes ---
    {
        const auto doc = json::Parse(SerializeAppSettings(AppSettings{}));
        CHECK(doc && doc->NumAt(L"version", 0) == RP::kSettingsDocumentVersion,
              "format: the exe's settings format version equals the engine's settings document version (bump both together)");
    }

    // --- the question: its words, as the console echo prints them (no dialog is shown) ---
    {
        const RP::ReopenQuestion question{ 3 };
        const TASKDIALOGCONFIG& cfg = question.Config();
        const DialogEcho::Description d = DialogEcho::DescribeTaskDialog(cfg);
        CHECK(d.title == L"Agentmaster", "question: titled Agentmaster (the e2e driver, the install watch and the dev launcher find it by it)");
        CHECK(!d.lines.empty() && d.lines.front() == L"Reopen your 3 previous Agentmaster windows?",
              "question: the first text is the instruction with the count (the e2e driver's REOPEN_PROMPT)");
        bool buttons = false;
        bool box = false;
        bool footer = false;
        for (const auto& line : d.lines)
        {
            buttons = buttons || line == L"buttons: Yes (default) | No";
            box = box || line == L"[ ] Remember my choice and don't ask again";
            footer = footer || (line.find(L"Settings") != std::wstring::npos && line.find(L"Behavior") != std::wstring::npos && line.find(L"Startup") != std::wstring::npos);
        }
        CHECK(buttons, "question: Yes (the default) and No, the words the e2e driver presses");
        CHECK(box, "question: the Remember my choice check box, unticked until the user ticks it");
        CHECK(footer, "question: the footer says where a remembered choice is changed (Settings, Behavior, Startup)");
        CHECK((cfg.dwFlags & TDF_ALLOW_DIALOG_CANCELLATION) == 0 && cfg.cButtons == 0 && cfg.cRadioButtons == 0,
              "question: no Cancel, Esc or close button, no other button: the only answers are Yes and No");
        CHECK(cfg.pfCallback != nullptr && cfg.hwndParent == nullptr, "question: owned by no window, with the callback that keeps it on top and in front");
        CHECK(DialogEcho::TaskDialogAnswer(cfg, S_OK, IDYES, 0, TRUE) == L"Yes; check box: ticked", "question: a ticked Yes is echoed with its check box");
        CHECK(DialogEcho::TaskDialogAnswer(cfg, S_OK, IDNO, 0, FALSE) == L"No; check box: not ticked", "question: an unticked No is echoed with its check box");
        CHECK(RP::ReopenQuestionCallback(nullptr, TDN_BUTTON_CLICKED, IDYES, 0, 0) == S_OK, "question: the callback lets every notification through");
    }
    {
        const RP::ReopenQuestion many{ 12 };
        const DialogEcho::Description d = DialogEcho::DescribeTaskDialog(many.Config());
        CHECK(!d.lines.empty() && d.lines.front() == L"Reopen your 12 previous Agentmaster windows?", "question: the count is the number of windows to reopen");
    }

    // --- the exe's read of the remembered choice ---
    {
        const std::wstring dir = ReopenScratchDir(L"read");
        const std::wstring sj = dir + L"\\settings.json";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);

        RP::ReopenSettingRead read = RP::ReadReopenWindowsAtStart(dir);
        CHECK(read.choice == ReopenWindowsAtStart::Ask && !read.unreadable, "read: no settings.json reads Ask, and is not unreadable (a first run)");

        AppSettings saved;
        saved.model = L"opus";
        saved.reopenWindowsAtStart = ReopenWindowsAtStart::ReopenAll;
        CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(saved)), "read: (setup) a settings.json the engine saved");
        read = RP::ReadReopenWindowsAtStart(dir);
        CHECK(read.choice == ReopenWindowsAtStart::ReopenAll && !read.unreadable, "read: the exe reads the choice the engine saved (the cog's pick)");

        CHECK(U::detail::WriteFileUtf8(sj, L"{\"version\":1,\"reopenWindowsAtStart\":\"reopen\",\"settings\":{}}"), "read: (setup) a top-level stray");
        CHECK(RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::Ask, "read: only the nested key counts (no build ever wrote it at the top level)");
        CHECK(U::detail::WriteFileUtf8(sj, L"{\"version\":1,\"settings\":{\"reopenWindowsAtStart\":\"sometimes\"}}"), "read: (setup) an unknown word");
        CHECK(RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::Ask, "read: an unknown word reads Ask");
        CHECK(U::detail::WriteFileUtf8(sj, L"{\"version\":1,\"settings\":5}"), "read: (setup) a settings member that is no object");
        CHECK(RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::Ask, "read: a settings member that is no object reads Ask");
        CHECK(U::detail::WriteFileUtf8(sj, L"{not json"), "read: (setup) text that does not parse");
        read = RP::ReadReopenWindowsAtStart(dir);
        CHECK(read.choice == ReopenWindowsAtStart::Ask && !read.unreadable, "read: text that does not parse reads Ask (it was read; it is just not settings)");

        // Held by another program that denies readers: reported, so the log can say why the question came back.
        CHECK(U::detail::WriteFileUtf8(sj, SerializeAppSettings(saved)), "read: (setup) the saved settings again");
        const HANDLE lock = ::CreateFileW(sj.c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE, "read: (setup) settings.json is held, denying readers");
        read = RP::ReadReopenWindowsAtStart(dir);
        CHECK(read.choice == ReopenWindowsAtStart::Ask && read.unreadable, "read: a settings.json that cannot be read is reported unreadable, and reads Ask");
        if (lock != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(lock);
        }
        CHECK(RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::ReopenAll, "read: ...and once released, the remembered choice reads again");
        RemoveScratch(dir);
    }

    // --- the exe's write of a remembered choice ---
    {
        const std::wstring dir = ReopenScratchDir(L"write");
        const std::wstring sj = dir + L"\\settings.json";

        CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::ReopenAll) == RP::RememberResult::Saved, "write: a missing settings.json (and its folder) is created");
        {
            const std::wstring text = U::detail::ReadFileWide(sj);
            const auto doc = json::Parse(text);
            const json::Value* settings = doc ? doc->Find(L"settings") : nullptr;
            CHECK(doc && doc->NumAt(L"version", 0) == 1 && settings && settings->type == json::Value::Type::Obj && settings->StrAt(L"reopenWindowsAtStart") == L"reopen",
                  "write: ...as the engine's envelope, the word nested under settings");
            CHECK(DeserializeAppSettings(text).reopenWindowsAtStart == ReopenWindowsAtStart::ReopenAll, "write: the engine's next load reads the remembered choice");
        }

        // A file the engine stamped, with other settings and members only a NEWER build knows: every one survives.
        const std::wstring stamped =
            L"{\"writer\":{\"app\":\"Agentmaster\",\"version\":\"0.8.0.2\",\"install\":\"release\"},\"version\":1,\"futureRoot\":[1,2],"
            L"\"settings\":{\"model\":\"opus\",\"confirmBeforeKill\":false,\"reopenWindowsAtStart\":\"reopen\",\"futureSetting\":{\"a\":1}}}";
        CHECK(U::detail::WriteFileUtf8(sj, stamped), "write: (setup) a stamped settings.json holding a newer build's members");
        CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::OneWindow) == RP::RememberResult::Saved, "write: the choice is saved into it");
        {
            const std::wstring text = U::detail::ReadFileWide(sj);
            const auto doc = json::Parse(text);
            const json::Value* settings = doc ? doc->Find(L"settings") : nullptr;
            CHECK(text.rfind(L"{\"writer\":{\"app\":\"Agentmaster\",\"version\":\"0.8.0.2\"", 0) == 0,
                  "write: the writer stamp is kept, unchanged, as the FIRST member (the older-writer repair reads it there)");
            CHECK(doc && doc->Find(L"futureRoot") != nullptr, "write: a newer build's root member is kept");
            CHECK(settings && settings->StrAt(L"model") == L"opus" && settings->Find(L"futureSetting") != nullptr,
                  "write: the other settings and a newer build's setting are kept");
            CHECK(settings && settings->StrAt(L"reopenWindowsAtStart") == L"oneWindow", "write: the word is the new choice's");
            CHECK(CountOf(text, L"reopenWindowsAtStart") == 1, "write: the key is replaced in place, never appended a second time");
            const AppSettings loaded = DeserializeAppSettings(text);
            CHECK(loaded.reopenWindowsAtStart == ReopenWindowsAtStart::OneWindow && loaded.model == L"opus" && !loaded.confirmBeforeKill,
                  "write: the engine reads the choice and the user's other settings unchanged");
        }

        CHECK(U::detail::WriteFileUtf8(sj, L"{\"version\":1,\"settings\":\"broken\"}"), "write: (setup) a settings member that is no object");
        CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::ReopenAll) == RP::RememberResult::Saved &&
                  RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::ReopenAll,
              "write: a settings member that is no object is replaced by one holding the choice");

        // Every refusal leaves the file byte for byte.
        const auto refused = [&](const std::wstring& seed, RP::RememberResult expected, const char* outcome, const char* untouched) {
            CHECK(U::detail::WriteFileUtf8(sj, seed), "write: (setup) the seed of a refusal");
            CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::ReopenAll) == expected, outcome);
            CHECK(U::detail::ReadFileWide(sj) == seed, untouched);
        };
        refused(L"{\"version\":2,\"settings\":{\"model\":\"opus\"}}", RP::RememberResult::RefusedNewerFormat,
                "write: a newer format (root version 2) is refused (the format guard)", "write: ...and the newer build's file is left byte for byte");
        refused(L"{not json!!", RP::RememberResult::RefusedNotSettings, "write: text that does not parse is refused",
                "write: ...and left byte for byte (never rebuilt around one key)");
        refused(L"[1,2,3]", RP::RememberResult::RefusedNotSettings, "write: JSON that is no object is refused", "write: ...and left byte for byte");

        // The case that loses data when a read-modify-write takes "unreadable" for "missing": the file can still be REPLACED
        // (a rename) but its data cannot be read. The PERSISTENCE.md §17 updater control: an ACL that denies reading the
        // data to Everyone and allows everything else (an open handle would stop the rename too, and prove nothing).
        {
            AppSettings before;
            before.model = L"sonnet";
            const std::wstring seed = SerializeAppSettings(before);
            CHECK(U::detail::WriteFileUtf8(sj, seed), "unreadable: (setup) the user's settings");
            PSECURITY_DESCRIPTOR denyRead = nullptr;
            PSECURITY_DESCRIPTOR allowAll = nullptr;
            // D:P = a protected DACL (nothing inherited): deny FILE_READ_DATA (0x1) to Everyone, allow every right to Everyone.
            const bool built = ::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &denyRead, nullptr) &&
                               ::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;FA;;;WD)", SDDL_REVISION_1, &allowAll, nullptr);
            const bool applied = built && ::SetFileSecurityW(sj.c_str(), DACL_SECURITY_INFORMATION, denyRead);
            CHECK(applied && U::detail::ReadFileWide(sj).empty(), "unreadable: (setup) settings.json's data can no longer be read, though the file can be replaced");
            CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::ReopenAll) == RP::RememberResult::RefusedUnreadable,
                  "unreadable: the choice is refused - nothing is written over the settings");
            if (built)
            {
                ::SetFileSecurityW(sj.c_str(), DACL_SECURITY_INFORMATION, allowAll); // readable again (and removable below)
            }
            CHECK(U::detail::ReadFileWide(sj) == seed, "unreadable: ...and settings.json still holds every setting, byte for byte");
            if (denyRead)
            {
                ::LocalFree(denyRead);
            }
            if (allowAll)
            {
                ::LocalFree(allowAll);
            }
        }
        CHECK(RP::RememberResultText(RP::RememberResult::Saved) != nullptr && RP::RememberResultText(static_cast<RP::RememberResult>(99)) != nullptr,
              "write: every result, an unknown one too, has log words");
        RemoveScratch(dir);
    }

    // --- the start's whole decision, for the cases that never ask ---
    {
        const std::wstring dir = ReopenScratchDir(L"decide");
        // None and one need no setting: not even a settings.json exists here.
        CHECK(!RP::DecideReopenAtStartup(dir, 0), "decide: nothing to reopen reopens nothing");
        CHECK(RP::DecideReopenAtStartup(dir, 1), "decide: a lone window comes back without a question (nothing read)");

        // Each call with several windows is guarded by a read of the remembered choice: with Ask the call would show the
        // question and stop the suite, so a failed setup skips it (its own CHECK already reports the failure).
        CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::ReopenAll) == RP::RememberResult::Saved, "decide: (setup) a remembered Yes");
        if (RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::ReopenAll)
        {
            CHECK(RP::DecideReopenAtStartup(dir, 3), "decide: a remembered Yes reopens 3 windows without a question");
        }
        CHECK(RP::WriteReopenWindowsAtStart(dir, ReopenWindowsAtStart::OneWindow) == RP::RememberResult::Saved, "decide: (setup) a remembered No");
        if (RP::ReadReopenWindowsAtStart(dir).choice == ReopenWindowsAtStart::OneWindow)
        {
            CHECK(!RP::DecideReopenAtStartup(dir, 4), "decide: a remembered No dispatches none of 4 windows (a single window), without a question");
        }
        CHECK(RP::DecideReopenAtStartup(dir, 1), "decide: a remembered No still brings a lone window back");

        // The trace a user who forgot the ticked box finds: one "[reopen]" line per decision, naming the setting.
        const std::wstring log = U::detail::ReadFileWide(Profiles::ResolveProfileDir() + L"\\hooks.log");
        CHECK(log.find(L"[reopen] 3 windows reopened without asking (remembered: reopenWindowsAtStart=reopen") != std::wstring::npos,
              "decide: the remembered Yes is logged with its setting (hooks.log)");
        CHECK(log.find(L"[reopen] 4 windows were open at the last exit: started with a single window without asking, the others wait behind Reopen Windows (remembered: reopenWindowsAtStart=oneWindow") !=
                  std::wstring::npos,
              "decide: the remembered No is logged with its setting (hooks.log)");
        RemoveScratch(dir);
    }
}
