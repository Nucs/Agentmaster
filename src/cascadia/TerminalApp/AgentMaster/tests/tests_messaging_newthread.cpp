// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.17: the user's three requests of 2026-10-18, their pure halves —
//   1. every title the bot prints LINKS to its session's topic (TelegramFormat.h: the token's THREAD part, TgTopicUrl, the linked
//      classic render, the URL chip; the card, the change line and the Search cards name their thread), and a "👤 You" line has the
//      session's title as its SECOND ROW (UserLineTitleRow, the builders, RenderHistory / PlanHistoryPosts);
//   1.5 a turn's prompt line replies to the previous turn's — the history replay's chain survives a restart on the topic record
//      (TopicRecord::historyPromptMessageId), and the card's link is a FORM like its rich-ness (TopicRecord::cardLinked);
//   2. a thread the user opens starts a NEW SESSION: the folder picker (MessagingNewThread.h — the two lists from the registry's
//      facts, the pages, the text, the keyboard, the buttons' data, a typed folder, a topic name without its decoration), the
//      registry's lean read behind it (SessionRegistry::PlaceSamples);
//   3. §21.17.4's side fixes: the BOUNDED folder check (FolderProbe.h — a hung probe never holds the caller past its wait, one
//      probe per path, the cap, the kept answers), its "did not answer" text, and when a held message counts as deleted
//      (HeldMessageGone).
//   4. §21.25, the NEW-THREAD WIZARD (MessagingNewThreadWizard.h, TestMessagingNewThreadWizard): the steps' buttons, the
//      case-insensitive folder resolution over a fake file system (a case-sensitive folder included), "create it?", the model
//      choices and a typed id, the window choices and their default (the window of the newest tab in the folder), whole
//      lifecycles through the reducers — forward, back, stale, refused (never retried by itself) and cancelled at every step —
//      the views, the persisted records (a restart resumes them), the abort line and the session's creation stamp.
// The service's sends, the binding of the thread and the launch with a chosen id need Telegram and a window: the live check is the
// dev bot (BuildClaudeSpawn's chosen id is pinned in tests_spawn_sched.cpp; the wizard's window routing in tests_tabtools.cpp).

#include "m5_tests.h"

#include <algorithm> // std::find / find_if / any_of / none_of
#include <chrono> // the bounded probe's waits (section 3)
#include <condition_variable> // the hanging probe's gate (section 3)
#include <cstdio> // swprintf (the test folders' names)
#include <memory> // the gate, shared with the probe threads (section 3)
#include <mutex>
#include <stdexcept> // a probe that throws (section 3)
#include <vector>
#include <cwctype> // std::towlower (the wizard tests' fake file system)
#include <map> // the wizard records (NewThreadsToJson)
#include <optional>
#include <unordered_map> // the window step's facts

#include "../MessagingDirectory.h" // DirectoryTopicUrl (one link builder with TgTopicUrl)
#include "../MessagingModels.h"
#include "../MessagingNewThread.h"
#include "../SessionRegistry.h" // PlaceSamples
#include "../TelegramFormat.h"
#include "../MessagingNewThreadWizard.h" // §21.25: the wizard (TestMessagingNewThreadWizard)
// LAST: it brings <windows.h>, whose min / max macros must not reach the headers above.
#include "../FolderProbe.h" // FolderProber, FolderProbeAnswer (section 3)

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    // How often `needle` occurs in `hay` (non-overlapping).
    size_t NtCount(const std::wstring& hay, const std::wstring& needle)
    {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::wstring::npos; p = hay.find(needle, p + needle.size()))
        {
            ++n;
        }
        return n;
    }

    // A transcript item of the history replay.
    TopicItem NtItem(TopicItemKind kind, const wchar_t* text, int64_t tsMs = 0, bool turnEnds = false)
    {
        TopicItem it;
        it.kind = kind;
        it.text = text;
        it.tsMs = tsMs;
        it.turnEnds = turnEnds;
        return it;
    }

    // A folder fact: a session launched in `dir` (and inferred in `inferred`), last active at `ms`.
    SessionFolderFacts NtFacts(const std::wstring& dir, const std::wstring& inferred, int64_t ms)
    {
        SessionFolderFacts f;
        f.workingDir = dir;
        f.inferredDir = inferred;
        f.lastActivityMs = ms;
        return f;
    }
}

namespace
{
    // ===== §21.25: the new-thread wizard's fixtures ===========================================================================

    // One entry of the fake file system: its parent folder (no trailing separator: "K:" for a root's child), its name as the
    // disk spells it, and whether it is a folder.
    struct WzEntry
    {
        std::wstring parent;
        std::wstring name;
        bool dir{ true };
    };

    // Lower case, for the fake's case-insensitive folders.
    std::wstring WzLower(std::wstring s)
    {
        for (auto& c : s)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return s;
    }

    // A folder's key in the fake: lower case, no trailing separator ("K:\" -> "k:").
    std::wstring WzKey(std::wstring folder)
    {
        while (!folder.empty() && folder.back() == L'\\')
        {
            folder.pop_back();
        }
        return WzLower(std::move(folder));
    }

    // The fake file system ResolveNewThreadFolder walks: K:\ and \\nas\share\ exist; K:\source holds Agentmaster (a folder),
    // notes.txt (a file) and CaseSens, a CASE-SENSITIVE folder that holds "Foo" (a folder) and "foo" (a file) — a lookup there
    // answers only the exact spelling, as a folder WSL marked case-sensitive does. `listed` counts the listing fallback's calls.
    FolderFs WzFs(const std::shared_ptr<std::vector<WzEntry>>& entries, const std::shared_ptr<int>& listed)
    {
        const std::wstring caseSensitive = WzKey(L"K:\\source\\CaseSens");
        FolderFs fs;
        fs.rootExists = [](const std::wstring& root) { return root == L"K:\\" || root == L"\\\\nas\\share\\"; };
        fs.stat = [entries, caseSensitive](const std::wstring& path) {
            FolderStat st;
            const size_t sep = path.rfind(L'\\');
            if (sep == std::wstring::npos)
            {
                return st;
            }
            const std::wstring parentKey = WzKey(path.substr(0, sep));
            const std::wstring name = path.substr(sep + 1);
            for (const auto& e : *entries)
            {
                if (WzKey(e.parent) != parentKey)
                {
                    continue;
                }
                const bool match = parentKey == caseSensitive ? e.name == name : WzLower(e.name) == WzLower(name);
                if (match)
                {
                    st.exists = true;
                    st.directory = e.dir;
                    st.name = e.name;
                    return st;
                }
            }
            return st;
        };
        fs.list = [entries, listed](const std::wstring& folder) -> std::optional<std::vector<FolderStat>> {
            ++*listed;
            std::vector<FolderStat> out;
            for (const auto& e : *entries)
            {
                if (WzKey(e.parent) == WzKey(folder))
                {
                    out.push_back(FolderStat{ true, e.dir, e.name });
                }
            }
            return out;
        };
        return fs;
    }

    // A window census tab: its strip index, title and session ("" = a shell tab).
    WindowTabInfo WzTab(uint32_t index, const std::wstring& title, const std::wstring& sessionId)
    {
        WindowTabInfo t;
        t.index = index;
        t.title = title;
        t.sessionId = sessionId;
        return t;
    }

    // A session's facts for the window step: its launch folder, its inferred folder ("" = none), its creation stamp and its
    // MOST RECENT key.
    NewThreadTabFacts WzFacts(const std::wstring& dir, const std::wstring& worksIn, int64_t created, int64_t recent)
    {
        NewThreadTabFacts f;
        f.dirKey = NormDirKey(dir);
        f.worksInKey = worksIn.empty() ? std::wstring{} : NormDirKey(worksIn);
        f.createdMs = created;
        f.recentMs = recent;
        return f;
    }

    // The two windows of the window-step checks: window 1 holds a shell, "alpha" (in K:\proj, created at 100), "beta" (launched
    // in K:\other but WORKING in K:\proj, created at 300) and "gamma" (no stamp, used recently); window 2 — the one used last —
    // holds "delta" (K:\proj, 200) and "eps" (K:\y, 500).
    std::vector<WindowCensus> WzCensus()
    {
        WindowCensus w1;
        w1.windowId = L"{W1}";
        w1.tabs = { WzTab(0, L"pwsh", L""), WzTab(1, L"alpha", L"a"), WzTab(2, L"beta", L"b"), WzTab(3, L"gamma", L"c") };
        WindowCensus w2;
        w2.windowId = L"{W2}";
        w2.lastActive = true;
        w2.tabs = { WzTab(0, L"delta", L"d"), WzTab(1, L"eps", L"e") };
        return { w1, w2 };
    }

    std::unordered_map<std::wstring, NewThreadTabFacts> WzTabFacts()
    {
        return {
            { L"a", WzFacts(L"K:\\proj", L"", 100, 10) },
            { L"b", WzFacts(L"K:\\other", L"K:\\proj", 300, 20) },
            { L"c", WzFacts(L"K:\\x", L"", 0, 900) },
            { L"d", WzFacts(L"K:\\proj", L"", 200, 30) },
            { L"e", WzFacts(L"K:\\y", L"", 500, 40) },
        };
    }

    // The action a callback data string parses to (the wizard's checks press buttons through their data, as Telegram does).
    NewThreadAction WzPress(const std::vector<std::vector<NewThreadButton>>& rows, const std::wstring& label)
    {
        for (const auto& row : rows)
        {
            for (const auto& b : row)
            {
                if (b.text.find(label) != std::wstring::npos)
                {
                    return ParseNewThreadCallbackData(b.data);
                }
            }
        }
        return NewThreadAction{};
    }

    // A held message as Telegram sends it (a Message object with an id and a text).
    json::Value WzMessage(int64_t id, const std::wstring& text)
    {
        auto m = json::Value::MkObj();
        m.Set(L"message_id", json::Value::MkNum(static_cast<double>(id)));
        m.Set(L"text", json::Value::MkStr(text));
        return m;
    }

    /// <summary>
    /// MESSAGING.md §21.25: the NEW-THREAD WIZARD's pure half (MessagingNewThreadWizard.h) — the steps' buttons, the
    /// case-insensitive folder resolution, the model choices and a typed id, the window choices and their default, whole
    /// lifecycles through the reducers (every way forward, back, stale and aborted), the views, the persisted records and the
    /// abort line.
    /// </summary>
    void TestMessagingNewThreadWizard()
    {
        wprintf(L"== TestMessagingNewThreadWizard (MESSAGING.md 21.25: create a folder, a model, a window; the lifecycle; persistence) ==\n");
        using K = NewThreadAction::Kind;
        using O = NewThreadOutcome::Kind;

        // ===== W1. the wizard's buttons (callback data) ======================================================================
        {
            CHECK((NewThreadCallbackData(K::CreateFolder, 7) == L"nt:cf" && NewThreadCallbackData(K::BackToFolders, 0) == L"nt:bf" &&
                      NewThreadCallbackData(K::ModelDefault, 0) == L"nt:md" && NewThreadCallbackData(K::ModelPick, 4) == L"nt:mo:4" &&
                      NewThreadCallbackData(K::ModelSpecify, 0) == L"nt:ms" && NewThreadCallbackData(K::BackToModels, 0) == L"nt:bm" &&
                      NewThreadCallbackData(K::WindowPick, 1) == L"nt:w:1" && NewThreadCallbackData(K::WindowDefault, 0) == L"nt:wd" &&
                      NewThreadCallbackData(K::WindowRefresh, 0) == L"nt:wr"),
                  "W1: each wizard button's data (a valueless kind ignores its value)");
            const K all[] = { K::CreateFolder, K::BackToFolders, K::ModelDefault, K::ModelPick, K::ModelSpecify, K::BackToModels, K::WindowPick, K::WindowDefault, K::WindowRefresh, K::Cancel };
            bool roundTrip = true;
            for (const K k : all)
            {
                const NewThreadAction back = ParseNewThreadCallbackData(NewThreadCallbackData(k, 12));
                const bool valued = k == K::ModelPick || k == K::WindowPick;
                roundTrip = roundTrip && back.kind == k && back.value == (valued ? 12u : 0u) && NewThreadCallbackData(k, 999999999).size() <= 64;
            }
            CHECK((roundTrip), "W1: every wizard button parses back to its kind (and value), within 64 bytes");
            CHECK((ParseNewThreadCallbackData(L"nt:cf:1").kind == K::None && ParseNewThreadCallbackData(L"nt:mo:").kind == K::None &&
                      ParseNewThreadCallbackData(L"nt:w:x").kind == K::None && ParseNewThreadCallbackData(L"nt:mo:1234567890").kind == K::None &&
                      ParseNewThreadCallbackData(L"nt:md:").kind == K::None && ParseNewThreadCallbackData(L"nt:wdx").kind == K::None),
                  "W1: a value on a valueless kind, a valued kind without its value, a bad number: none");
        }

        // ===== W2. folder names and paths ====================================================================================
        {
            CHECK((!NewThreadFolderNameIllegal(L"a<b").empty() && !NewThreadFolderNameIllegal(L"a|b").empty() && !NewThreadFolderNameIllegal(L"q?").empty() &&
                      !NewThreadFolderNameIllegal(std::wstring(1, L'\x01')).empty()),
                  "W2: a character no Windows name takes is illegal (a wildcard to the lookup, too)");
            CHECK((!NewThreadFolderNameIllegal(L"CON").empty() && !NewThreadFolderNameIllegal(L"con.txt").empty() && !NewThreadFolderNameIllegal(L"Com1").empty() &&
                      !NewThreadFolderNameIllegal(L"LPT9.log").empty() && !NewThreadFolderNameIllegal(L"nul ").empty()),
                  "W2: the device names, any case, with an extension or blanks: illegal");
            CHECK((NewThreadFolderNameIllegal(L"COM10").empty() && NewThreadFolderNameIllegal(L"console").empty() && NewThreadFolderNameIllegal(L"a.").empty() &&
                      NewThreadFolderNameIllegal(L"my project").empty()),
                  "W2: COM10, a longer word, a trailing dot (a lookup drops it), a space inside: possible names");
            CHECK((!NewThreadFolderNameIllegal(std::wstring(256, L'x')).empty() && NewThreadFolderNameIllegal(std::wstring(255, L'x')).empty()), "W2: at most 255 characters");
            CHECK((!NewThreadFolderNameProblem(L"a.").empty() && !NewThreadFolderNameProblem(L"a ").empty() && NewThreadFolderNameProblem(L"a.b").empty() &&
                      !NewThreadFolderNameProblem(L"CON").empty()),
                  "W2: a creation also refuses a trailing dot or space (Windows would drop it)");
            const auto parts = SplitFolderPath(L"  \"k:/Source//x/./y\\\"  ");
            CHECK((parts && parts->root == L"K:\\" && !parts->unc && parts->components == std::vector<std::wstring>{ L"Source", L"x", L"y" } && !parts->parentRef),
                  "W2: a path split: the drive upper case, doubled and trailing separators and \".\" dropped, quotes off");
            const auto unc = SplitFolderPath(L"\\\\nas\\share\\proj\\sub");
            CHECK((unc && unc->unc && unc->root == L"\\\\nas\\share\\" && unc->components == std::vector<std::wstring>{ L"proj", L"sub" }), "W2: a share's root and parts");
            CHECK((SplitFolderPath(L"K:\\a\\..\\b") && SplitFolderPath(L"K:\\a\\..\\b")->parentRef && !SplitFolderPath(L"src\\x") && !SplitFolderPath(L"")),
                  "W2: \"..\" is flagged; a relative path or nothing is no path");
            CHECK((JoinFolderPath(L"K:\\", L"x") == L"K:\\x" && JoinFolderPath(L"K:\\a", L"x") == L"K:\\a\\x" && JoinFolderPath(L"\\\\nas\\share\\", L"p") == L"\\\\nas\\share\\p"),
                  "W2: one separator between a folder and a name");
            CHECK((NewThreadFolderLeaf(L"K:\\source\\Agentmaster") == L"Agentmaster" && NewThreadFolderLeaf(L"K:\\") == L"K:" && NewThreadFolderLeaf(L"K:\\a\\") == L"a"),
                  "W2: a folder's last name; a drive root's letter");
        }

        // ===== W3. the case-insensitive resolution of a typed folder =========================================================
        {
            const auto entries = std::make_shared<std::vector<WzEntry>>(std::vector<WzEntry>{
                { L"K:", L"source", true },
                { L"K:\\source", L"Agentmaster", true },
                { L"K:\\source", L"notes.txt", false },
                { L"K:\\source", L"CaseSens", true },
                { L"K:\\source\\CaseSens", L"Foo", true },
                { L"K:\\source\\CaseSens", L"foo", false },
            });
            const auto listed = std::make_shared<int>(0);
            const FolderFs fs = WzFs(entries, listed);

            auto r = ResolveNewThreadFolder(L"k:\\SOURCE\\agentmaster", fs);
            CHECK((r.kind == FolderResolveKind::Exists && r.path == L"K:\\source\\Agentmaster"), "W3: a typed folder in another case IS the existing one, in the disk's spelling");
            r = ResolveNewThreadFolder(L"K:/source/agentmaster/new/deeper", fs);
            CHECK((r.kind == FolderResolveKind::Missing && r.existingPrefix == L"K:\\source\\Agentmaster" && r.missing == std::vector<std::wstring>{ L"new", L"deeper" } &&
                      r.path == L"K:\\source\\Agentmaster\\new\\deeper"),
                  "W3: a missing folder: what exists (disk spelling), what a Create makes (the user's spelling), outermost first");
            r = ResolveNewThreadFolder(L"K:\\source\\notes.txt\\x", fs);
            CHECK((r.kind == FolderResolveKind::IsFile && r.path == L"K:\\source\\notes.txt" && r.reason.find(L"file") != std::wstring::npos), "W3: a file in the way");
            *listed = 0;
            r = ResolveNewThreadFolder(L"K:\\source\\casesens\\FOO", fs);
            CHECK((r.kind == FolderResolveKind::Exists && r.path == L"K:\\source\\CaseSens\\Foo" && *listed == 1),
                  "W3: in a case-sensitive folder the lookup misses, the listing finds the folder in another case (never a second folder)");
            r = ResolveNewThreadFolder(L"K:\\source\\CaseSens\\foo", fs);
            CHECK((r.kind == FolderResolveKind::IsFile && r.path == L"K:\\source\\CaseSens\\foo"), "W3: the EXACT spelling wins over a case variant (here a file)");
            r = ResolveNewThreadFolder(L"Q:\\x", fs);
            CHECK((r.kind == FolderResolveKind::NoRoot && r.reason.find(L"Q:") != std::wstring::npos), "W3: a drive that is not there");
            r = ResolveNewThreadFolder(L"\\\\down\\share\\x", fs);
            CHECK((r.kind == FolderResolveKind::NoRoot && r.reason.find(L"\\\\down\\share") != std::wstring::npos), "W3: a share that is not reachable");
            r = ResolveNewThreadFolder(L"\\\\nas\\share\\proj", fs);
            CHECK((r.kind == FolderResolveKind::Missing && r.existingPrefix == L"\\\\nas\\share\\" && r.path == L"\\\\nas\\share\\proj"), "W3: a missing folder on a share");
            CHECK((ResolveNewThreadFolder(L"K:\\source\\..\\x", fs).kind == FolderResolveKind::Invalid), "W3: \"..\" is refused");
            CHECK((ResolveNewThreadFolder(L"K:\\a*b", fs).kind == FolderResolveKind::Invalid && ResolveNewThreadFolder(L"K:\\source\\a<b", fs).kind == FolderResolveKind::Invalid),
                  "W3: a wildcard or a character Windows refuses: refused before any lookup");
            CHECK((ResolveNewThreadFolder(L"K:\\source\\CON\\x", fs).kind == FolderResolveKind::Invalid), "W3: a device name is refused");
            r = ResolveNewThreadFolder(L"K:\\source\\new.", fs);
            CHECK((r.kind == FolderResolveKind::Invalid && r.reason.find(L"cannot be created") != std::wstring::npos), "W3: a missing name Windows cannot make as written");
            CHECK((ResolveNewThreadFolder(L"K:\\source\\" + std::wstring(240, L'z'), fs).kind == FolderResolveKind::Invalid), "W3: a creation past the path cap is refused");
            CHECK((ResolveNewThreadFolder(L"src\\x", fs).kind == FolderResolveKind::Invalid && ResolveNewThreadFolder(L"K:\\x", FolderFs{}).kind == FolderResolveKind::NoRoot),
                  "W3: a relative path; an empty file system has no root");
        }

        // ===== W4. the model step's choices, a typed id, /model ==============================================================
        {
            const std::vector<std::pair<std::wstring, std::wstring>> menu = { { L"Opus", L"opus" }, { L"Sonnet", L"sonnet" }, { L"OPUS again", L"OPUS" },
                                                                                { L"", L"haiku" }, { L"broken", L"" }, { L"MiMo Pro", L"@mimo-pro" } };
            auto choices = BuildNewThreadModelChoices(menu, L"claude-opus-5-5[1m]");
            CHECK((choices.size() == 5 && choices[0].last && choices[0].target == L"claude-opus-5-5[1m]" && choices[1].label == L"Opus" && choices[2].target == L"sonnet" &&
                      choices[3].label == L"haiku" && choices[4].preset && choices[4].target == L"@mimo-pro"),
                  "W4: the last typed model first, then the menu's order; a twin in another case and an empty target left out; no label = the id");
            choices = BuildNewThreadModelChoices(menu, L"SONNET");
            CHECK((choices.size() == 4 && !choices[0].last && choices[0].target == L"opus"), "W4: a last typed model the menu lists is not shown twice");
            CHECK((BuildNewThreadModelChoices({}, L"").empty()), "W4: nothing configured, nothing typed: no choice (Default and Specify remain)");
            std::vector<std::pair<std::wstring, std::wstring>> many;
            for (int i = 0; i < 60; ++i)
            {
                many.emplace_back(L"m" + std::to_wstring(i), L"model-" + std::to_wstring(i));
            }
            CHECK((BuildNewThreadModelChoices(many, L"").size() == kNewThreadModelChoicesMax), "W4: at most kNewThreadModelChoicesMax choices");
            std::wstring why;
            CHECK((ParseNewThreadModelId(L"  claude-opus-5-5[1m] ", &why) == std::optional<std::wstring>{ L"claude-opus-5-5[1m]" } &&
                      ParseNewThreadModelId(L"anthropic/claude-sonnet-5.5", nullptr) && ParseNewThreadModelId(L"qwen3:32b", nullptr) && ParseNewThreadModelId(L"sonnet[1m]", nullptr)),
                  "W4: model ids as the user types them (the [1m] window, a provider path, a tag)");
            CHECK((!ParseNewThreadModelId(L"two words", &why) && why.find(L"one word") != std::wstring::npos), "W4: two words are no id, and the reason says so");
            CHECK((!ParseNewThreadModelId(L"x;calc", nullptr) && !ParseNewThreadModelId(L"$(evil)", nullptr) && !ParseNewThreadModelId(L"a`b", nullptr) &&
                      !ParseNewThreadModelId(L"\"q\"", nullptr) && !ParseNewThreadModelId(L"-flag", nullptr) && !ParseNewThreadModelId(L"@preset", nullptr)),
                  "W4: nothing that could leave the pwsh command line's word: ; $ ` quotes, a leading - or @");
            CHECK((!ParseNewThreadModelId(L"", &why) && !ParseNewThreadModelId(std::wstring(121, L'a'), nullptr) && ParseNewThreadModelId(std::wstring(120, L'a'), nullptr)),
                  "W4: empty, and the length cap");
            CHECK((NewThreadModelCommandArg(L"/model claude-opus-5-5[1m]") == std::optional<std::wstring>{ L"claude-opus-5-5[1m]" } &&
                      NewThreadModelCommandArg(L"  /MODEL@SomeBot   opus  ") == std::optional<std::wstring>{ L"opus" } &&
                      NewThreadModelCommandArg(L"/model") == std::optional<std::wstring>{ L"" }),
                  "W4: /model <id>, any case, addressed to the bot or not; a bare /model");
            CHECK((!NewThreadModelCommandArg(L"/models x") && !NewThreadModelCommandArg(L"use /model x") && !NewThreadModelCommandArg(L"")), "W4: /models, a mention, nothing: not /model");
        }

        // ===== W5. the window step's choices and their default ===============================================================
        {
            const auto facts = WzTabFacts();
            auto plan = BuildNewThreadWindowPlan(WzCensus(), facts, NormDirKey(L"K:\\proj"), L"proj");
            CHECK((plan.windows.size() == 2 && plan.windows[0].number == 1 && plan.windows[1].number == 2 && plan.windows[0].windowId == L"{W1}" && plan.leftOut == 0),
                  "W5: the windows in census order, numbered from 1");
            const auto& w1 = plan.windows[0];
            CHECK((w1.tabCount == 4 && w1.newestTitle == L"beta" && w1.newestCreatedMs == 300), "W5: a window's tab count and its newest tab (the user's \"last tab created\")");
            CHECK((w1.titles == std::vector<std::wstring>{ L"beta", L"alpha", L"gamma", L"pwsh" }), "W5: the titles newest first: a stamp beats none, then the MOST RECENT key, then the strip place");
            CHECK((w1.folderTabs == 2 && w1.anchorSessionId == L"b" && w1.anchorTitle == L"beta"),
                  "W5: a tab counts for the folder by its launch folder OR its inferred one; the newest such tab is the anchor");
            CHECK((plan.windows[1].folderTabs == 1 && plan.windows[1].anchorSessionId == L"d" && plan.windows[1].lastActive), "W5: the second window's folder tab");
            CHECK((plan.defaultIndex == 0 && plan.defaultReason.find(L"beta") != std::wstring::npos && plan.defaultReason.find(L"proj") != std::wstring::npos),
                  "W5: the default is the window of the NEWEST folder tab over every window (beta, 300, over delta, 200), though window 2 was used last");
            plan = BuildNewThreadWindowPlan(WzCensus(), facts, NormDirKey(L"K:\\none"), L"none");
            CHECK((plan.defaultIndex == 1 && plan.defaultReason.find(L"used last") != std::wstring::npos && plan.windows[0].anchorSessionId.empty()),
                  "W5: no tab in the folder: the window used last");
            auto census = WzCensus();
            census[1].lastActive = false;
            plan = BuildNewThreadWindowPlan(census, facts, NormDirKey(L"K:\\none"), L"none");
            CHECK((plan.defaultIndex == 0), "W5: no folder tab and no window used last: the first window");
            CHECK((BuildNewThreadWindowPlan({}, facts, NormDirKey(L"K:\\proj"), L"proj").defaultIndex == -1), "W5: no window: no default");
            auto tie = WzTabFacts();
            tie[L"d"].createdMs = 300; // delta as new as beta
            tie[L"d"].recentMs = 20;
            plan = BuildNewThreadWindowPlan(WzCensus(), tie, NormDirKey(L"K:\\proj"), L"proj");
            CHECK((plan.defaultIndex == 1), "W5: a tie on both clocks goes to the window used last");
            std::vector<WindowCensus> crowd;
            for (int i = 0; i < 23; ++i)
            {
                WindowCensus w;
                w.windowId = L"{X" + std::to_wstring(i) + L"}";
                for (uint32_t t = 0; t < 12; ++t)
                {
                    w.tabs.push_back(WzTab(t, L"tab " + std::to_wstring(t) + L" " + std::wstring(50, L'x'), L""));
                }
                crowd.push_back(std::move(w));
            }
            plan = BuildNewThreadWindowPlan(crowd, {}, NormDirKey(L"K:\\proj"), L"proj");
            CHECK((plan.windows.size() == kNewThreadWindowsMax && plan.leftOut == 3 && plan.windows[0].titles.size() == kNewThreadWindowTitles),
                  "W5: at most kNewThreadWindowsMax windows (the rest counted), at most ten titles each");
            CHECK((plan.windows[0].titles[0].size() <= kNewThreadWindowTitleChars && plan.windows[0].titles[0].back() == L'\x2026'), "W5: a long title is cut, with an ellipsis");
            CHECK((ParseNewThreadWindowNumber(L"2", 3) == std::optional<size_t>{ 1 } && ParseNewThreadWindowNumber(L" #2 ", 3) == std::optional<size_t>{ 1 } &&
                      ParseNewThreadWindowNumber(L"w2", 3) == std::optional<size_t>{ 1 } && ParseNewThreadWindowNumber(L"Window 3", 3) == std::optional<size_t>{ 2 } &&
                      ParseNewThreadWindowNumber(L"W 1", 3) == std::optional<size_t>{ 0 }),
                  "W5: a window number as typed: 2, #2, w2, Window 3, W 1");
            CHECK((!ParseNewThreadWindowNumber(L"0", 3) && !ParseNewThreadWindowNumber(L"4", 3) && !ParseNewThreadWindowNumber(L"2x", 3) && !ParseNewThreadWindowNumber(L"window", 3) &&
                      !ParseNewThreadWindowNumber(L"", 3) && !ParseNewThreadWindowNumber(L"fix the tests", 3)),
                  "W5: 0, out of range, a number with more, no number, words: none");
        }

        // ===== W6. a whole lifecycle: a list pick, a preset, the default window ==============================================
        NewThreadRecord rec;
        rec.listsBuilt = true;
        rec.lists.recent = { NewThreadFolder{ L"K:\\proj", 3, 1000 } };
        rec.lists.mostUsed = { NewThreadFolder{ L"K:\\proj", 3, 1000 }, NewThreadFolder{ L"K:\\other", 1, 900 } };
        rec.topicName = L"my work";
        {
            NewThreadView v = BuildNewThreadView(rec, 2000);
            CHECK((v.html.find(L"pick the folder") != std::wstring::npos && v.html.find(L"Cancel deletes this thread") != std::wstring::npos && v.rows.back().back().data == L"nt:x"),
                  "W6: the folder step is the 21.17 picker, saying what follows and what Cancel does");
            NewThreadOutcome out = ApplyNewThreadButton(rec, WzPress(v.rows, L"1 \x00B7 proj"));
            CHECK((out.kind == O::CheckFolder && out.path == L"K:\\proj" && !out.typed), "W6: a list folder's button: check it (not typed: it is never offered for creation)");
            FolderResolution exists;
            exists.kind = FolderResolveKind::Exists;
            exists.path = L"K:\\proj";
            out = ApplyNewThreadFolderResolved(rec, exists, false);
            CHECK((out.kind == O::EnterModel && rec.wizard.folder == L"K:\\proj"), "W6: an existing folder is chosen: the model step comes next");
            EnterNewThreadModelStep(rec.wizard, BuildNewThreadModelChoices({ { L"Opus", L"opus" }, { L"MiMo Pro", L"@mimo-pro" } }, L""), false, L"opus");
            v = BuildNewThreadView(rec, 2000);
            CHECK((rec.wizard.step == NewThreadStep::Model && v.html.find(L"<code>opus</code>") != std::wstring::npos && v.html.find(L"claude-opus-5-5[1m]") != std::wstring::npos),
                  "W6: the model step names the settings model Default stands for, and how to type an id");
            CHECK((v.rows.size() >= 3 && ParseNewThreadCallbackData(v.rows[0][0].data).kind == K::ModelDefault && ParseNewThreadCallbackData(v.rows[0][1].data).kind == K::ModelSpecify &&
                      ParseNewThreadCallbackData(v.rows.back()[0].data).kind == K::BackToFolders && ParseNewThreadCallbackData(v.rows.back()[1].data).kind == K::Cancel),
                  "W6: the PC menus' order: Default and Specify first; Back and Cancel last");
            CHECK((ApplyNewThreadButton(rec, NewThreadAction{ K::PickRecent, 0 }).kind == O::Stale && rec.wizard.step == NewThreadStep::Model),
                  "W6: a folder button tapped at the model step is stale and changes nothing");
            out = ApplyNewThreadButton(rec, WzPress(v.rows, L"MiMo Pro"));
            CHECK((out.kind == O::CollectWindows && rec.wizard.step == NewThreadStep::Window && rec.wizard.modelTarget == L"@mimo-pro" && rec.wizard.modelLabel == L"MiMo Pro" &&
                      out.rememberModel.empty()),
                  "W6: a preset's button: the window step comes next (a preset is not remembered as a typed id)");
            v = BuildNewThreadView(rec, 2000);
            CHECK((v.html.find(L"Looking at the open windows") != std::wstring::npos && ApplyNewThreadButton(rec, NewThreadAction{ K::WindowDefault, 0 }).kind == O::Stale &&
                      ApplyNewThreadButton(rec, NewThreadAction{ K::WindowPick, 0 }).kind == O::Stale),
                  "W6: while the windows are looked at, the window buttons are stale");
            out = ApplyNewThreadWindows(rec.wizard, BuildNewThreadWindowPlan(WzCensus(), WzTabFacts(), NormDirKey(L"K:\\proj"), L"proj"));
            CHECK((out.kind == O::Render && rec.wizard.windowsCollected && rec.wizard.defaultWindow == 0), "W6: two windows: the step shows them");
            v = BuildNewThreadView(rec, 2000);
            CHECK((v.html.find(L"newest: \x201C" L"beta\x201D") != std::wstring::npos && v.html.find(L"2 in proj") != std::wstring::npos &&
                      v.html.find(L"used last") != std::wstring::npos && v.html.find(L"MiMo Pro") != std::wstring::npos && v.html.find(L"Default: <b>window 1</b>") != std::wstring::npos),
                  "W6: the window step: each window's tabs, newest tab, folder tabs; the default and why; the chosen model");
            CHECK((WzPress(v.rows, L"Default \x2014 window 1").kind == K::WindowDefault && WzPress(v.rows, L"\U0001FA9F 2").kind == K::WindowPick &&
                      WzPress(v.rows, L"Refresh").kind == K::WindowRefresh && WzPress(v.rows, L"Back").kind == K::BackToModels),
                  "W6: the window step's buttons");
            out = ApplyNewThreadButton(rec, WzPress(v.rows, L"Default \x2014 window 1"));
            CHECK((out.kind == O::Launch && rec.wizard.windowId == L"{W1}" && rec.wizard.windowNumber == 1 && rec.wizard.anchorSessionId == L"b"),
                  "W6: the default: window 1, right after the newest tab of the folder");
            const NewThreadLaunchPlan plan = BuildNewThreadLaunchPlan(rec);
            CHECK((plan.dir == L"K:\\proj" && plan.title == L"my work" && plan.model == L"@mimo-pro" && plan.windowId == L"{W1}" && plan.afterSessionId == L"b"),
                  "W6: the launch: folder, the thread's name as the title, the model target, the window, the anchor");
            CHECK((plan.summaryText == L"Opening a new session in proj \x00B7 MiMo Pro \x00B7 window 1, next to \x201C" L"beta\x201D\x2026" &&
                      plan.summaryHtml.find(L"<b>proj</b>") != std::wstring::npos),
                  "W6: the opening line says what opens where");
        }

        // ===== W7. a launch that did not go through comes back, never retried by itself ======================================
        {
            NewThreadWizard w = rec.wizard;
            CHECK((NewThreadStepAfterRefusal(w, RemoteLaunchResult::PresetRefused) == NewThreadStep::Model), "W7: a refused preset: back to the model step");
            CHECK((NewThreadStepAfterRefusal(w, RemoteLaunchResult::WindowClosed) == NewThreadStep::Window && NewThreadStepAfterRefusal(w, RemoteLaunchResult::Failed) == NewThreadStep::Window),
                  "W7: a closed window, a failed tab: back to the window step");
            w.windowAuto = true;
            CHECK((NewThreadStepAfterRefusal(w, RemoteLaunchResult::WindowClosed) == NewThreadStep::Model), "W7: a skipped window step goes back to the model step (it would skip again)");
            NewThreadRecord back = rec;
            back.wizard.windowsSerial = 41; // the census that made the list it launched with
            ReturnNewThreadToStep(back.wizard, NewThreadStep::Window, L"\x26A0 That window is not open any more.");
            CHECK((back.wizard.step == NewThreadStep::Window && back.wizard.askWindow && !back.wizard.windowsCollected && !back.wizard.windowChosen),
                  "W7: back at the window step: its list is read again, and it MUST be shown");
            CHECK((back.wizard.windowsSerial == 0), "W7: ...with no census in flight (0), so the service's render starts one instead of waiting for an old one");
            WindowCensus only;
            only.windowId = L"{W2}";
            only.tabs = { WzTab(0, L"delta", L"d") };
            NewThreadOutcome out = ApplyNewThreadWindows(back.wizard, BuildNewThreadWindowPlan({ only }, WzTabFacts(), NormDirKey(L"K:\\proj"), L"proj"));
            CHECK((out.kind == O::Render && back.wizard.windows.size() == 1), "W7: after a refusal ONE window is shown, not launched into (a refusal is never retried without a tap)");
            CHECK((BuildNewThreadView(back, 2000).html.find(L"That window is not open any more") != std::wstring::npos), "W7: the step says why it came back");
            out = ApplyNewThreadWindows(back.wizard, NewThreadWindowPlan{});
            NewThreadView v = BuildNewThreadView(back, 2000);
            CHECK((out.kind == O::Render && v.html.find(L"No window answered") != std::wstring::npos && WzPress(v.rows, L"Refresh").kind == K::WindowRefresh),
                  "W7: after a refusal NO window answering is said, with Refresh");
            out = ApplyNewThreadButton(back, WzPress(v.rows, L"Refresh"));
            CHECK((out.kind == O::CollectWindows && !back.wizard.windowsCollected && back.wizard.notice.empty() && back.wizard.askWindow),
                  "W7: Refresh asks the windows again (the notice goes with the user's next choice; the step still must ask)");
            out = ApplyNewThreadButton(back, NewThreadAction{ K::BackToModels, 0 });
            CHECK((out.kind == O::Render && back.wizard.step == NewThreadStep::Model && !back.wizard.modelChosen), "W7: Back from the window step: the model step");
            out = ApplyNewThreadButton(back, NewThreadAction{ K::ModelDefault, 0 });
            CHECK((out.kind == O::CollectWindows && !back.wizard.askWindow && back.wizard.modelTarget.empty() && back.wizard.modelLabel == L"Default"),
                  "W7: a new model choice (Default) is a fresh pass: the window step may skip again");
            out = ApplyNewThreadWindows(back.wizard, BuildNewThreadWindowPlan({ only }, WzTabFacts(), NormDirKey(L"K:\\proj"), L"proj"));
            CHECK((out.kind == O::Launch && back.wizard.windowAuto && back.wizard.windowId == L"{W2}" && back.wizard.anchorSessionId == L"d"),
                  "W7: a fresh pass with one window skips the step: that window, after its folder tab");
            CHECK((BuildNewThreadLaunchPlan(back).summaryText.find(L"window") == std::wstring::npos), "W7: a skipped window step is not named in the opening line");
            NewThreadWizard none;
            none.step = NewThreadStep::Window;
            out = ApplyNewThreadWindows(none, NewThreadWindowPlan{});
            CHECK((out.kind == O::Launch && none.windowAuto && none.windowChosen && none.windowId.empty()), "W7: a fresh pass with no window answering: the engine's choice");
        }

        // ===== W8. a typed folder that does not exist: create it, or go back =================================================
        {
            NewThreadRecord r;
            NewThreadOutcome out = ApplyNewThreadText(r, L"K:/brand/new");
            CHECK((out.kind == O::CheckFolder && out.typed && out.path == L"K:\\brand\\new"), "W8: a typed path is a folder choice (typed: it may be created)");
            FolderResolution missing;
            missing.kind = FolderResolveKind::Missing;
            missing.path = L"K:\\brand\\new";
            missing.existingPrefix = L"K:\\";
            missing.missing = { L"brand", L"new" };
            out = ApplyNewThreadFolderResolved(r, missing, true);
            NewThreadView v = BuildNewThreadView(r, 0);
            CHECK((out.kind == O::Render && r.wizard.step == NewThreadStep::CreateFolder && r.wizard.createPath == L"K:\\brand\\new" &&
                      v.html.find(L"does not exist yet") != std::wstring::npos && v.html.find(L"brand\\new") != std::wstring::npos && v.html.find(L"2 new folders") != std::wstring::npos),
                  "W8: a missing typed folder: \"create it?\" with what exists and what a Create makes");
            CHECK((WzPress(v.rows, L"Create").kind == K::CreateFolder && WzPress(v.rows, L"Back to the folders").kind == K::BackToFolders && WzPress(v.rows, L"Cancel").kind == K::Cancel),
                  "W8: Create, Back, Cancel");
            out = ApplyNewThreadButton(r, WzPress(v.rows, L"Create"));
            CHECK((out.kind == O::CreateFolder && out.path == L"K:\\brand\\new"), "W8: Create makes the checked folder");
            out = ApplyNewThreadFolderCreated(r, false, L"Access is denied. (error 5)");
            CHECK((out.kind == O::Render && r.wizard.step == NewThreadStep::CreateFolder && r.wizard.notice.find(L"Access is denied") != std::wstring::npos),
                  "W8: a refused creation stays at the step with Windows' reason");
            out = ApplyNewThreadFolderCreated(r, true, L"");
            CHECK((out.kind == O::EnterModel && r.wizard.folder == L"K:\\brand\\new" && r.wizard.folderCreated && r.wizard.createPath.empty()),
                  "W8: made: the folder is chosen (and marked created); the model step comes next");
            NewThreadRecord r2;
            ApplyNewThreadFolderResolved(r2, missing, true);
            out = ApplyNewThreadButton(r2, NewThreadAction{ K::BackToFolders, 0 });
            CHECK((out.kind == O::Render && r2.wizard.step == NewThreadStep::Folder && r2.wizard.createPath.empty() && r2.wizard.createMissing.empty()),
                  "W8: Back from \"create it?\": the folder lists, nothing to create");
            NewThreadRecord gone;
            gone.listsBuilt = true;
            ApplyNewThreadFolderResolved(gone, missing, false);
            CHECK((gone.wizard.step == NewThreadStep::Folder && !gone.listsBuilt && gone.wizard.notice.find(L"gone") != std::wstring::npos),
                  "W8: a LIST pick that is gone is never offered for creation: the lists are read again");
            FolderResolution bad;
            bad.kind = FolderResolveKind::NoRoot;
            bad.reason = L"The drive Q: is not available on the PC.";
            ApplyNewThreadFolderResolved(gone, bad, true);
            CHECK((gone.wizard.step == NewThreadStep::Folder && gone.wizard.notice.find(L"Q:") != std::wstring::npos), "W8: a folder that cannot be: the reason above the lists");
            FolderResolution slow;
            slow.kind = FolderResolveKind::NoAnswer;
            slow.path = L"\\\\nas\\x";
            ApplyNewThreadFolderResolved(gone, slow, true);
            CHECK((gone.wizard.notice.find(L"did not answer") != std::wstring::npos), "W8: a folder that did not answer: said so");
            out = ApplyNewThreadButton(gone, NewThreadAction{ K::PageLabelUsed, 0 });
            CHECK((out.kind == O::Toast && !gone.wizard.notice.empty()), "W8: a page label is a toast and keeps the notice");
            out = ApplyNewThreadButton(gone, NewThreadAction{ K::PageUsed, 0 });
            CHECK((out.kind == O::Render && gone.wizard.notice.empty()), "W8: the next choice clears the notice");
        }

        // ===== W9. typed choices: a model id, /model, a preset, a window number; the user's own words ========================
        {
            NewThreadRecord r;
            r.wizard.folder = L"K:\\proj";
            EnterNewThreadModelStep(r.wizard, BuildNewThreadModelChoices({ { L"Opus", L"opus" }, { L"MiMo Pro", L"@mimo-pro" } }, L""), true, L"");
            NewThreadView v = BuildNewThreadView(r, 0);
            CHECK((v.html.find(L"No presets yet") != std::wstring::npos && v.html.find(L"Claude Code\x2019s own default model") != std::wstring::npos),
                  "W9: no preset: the step says where to add one; no settings model: Default is Claude Code's own");
            NewThreadOutcome out = ApplyNewThreadText(r, L"fix the build please");
            CHECK((out.kind == O::Hold && r.wizard.step == NewThreadStep::Model), "W9: words at the model step are the user's message, held");
            out = ApplyNewThreadText(r, L"/model claude-opus-5-5[1m]");
            CHECK((out.kind == O::CollectWindows && r.wizard.modelTarget == L"claude-opus-5-5[1m]" && out.rememberModel == L"claude-opus-5-5[1m]"),
                  "W9: /model <id> at the model step: that model (remembered for the next time)");
            ApplyNewThreadButton(r, NewThreadAction{ K::BackToModels, 0 });
            out = ApplyNewThreadText(r, L"/model");
            CHECK((out.kind == O::Render && r.wizard.step == NewThreadStep::ModelSpecify), "W9: a bare /model asks for the id");
            v = BuildNewThreadView(r, 0);
            CHECK((v.html.find(L"Send the model id") != std::wstring::npos && WzPress(v.rows, L"Use Default").kind == K::ModelDefault &&
                      WzPress(v.rows, L"Back to the list").kind == K::BackToModels && WzPress(v.rows, L"Cancel").kind == K::Cancel),
                  "W9: the model-id prompt: Default, back to the list, Cancel");
            out = ApplyNewThreadText(r, L"bad;id");
            CHECK((out.kind == O::Reply && out.text.find(L"letters, digits") != std::wstring::npos && r.wizard.step == NewThreadStep::ModelSpecify), "W9: a bad id is answered; the step stays");
            out = ApplyNewThreadText(r, L"please use opus for this");
            CHECK((out.kind == O::Hold && r.wizard.notice.find(L"Kept as a message") != std::wstring::npos), "W9: words at the id prompt are held, and the prompt says what it still waits for");
            out = ApplyNewThreadText(r, L"opus");
            CHECK((out.kind == O::CollectWindows && r.wizard.modelLabel == L"Opus" && r.wizard.notice.empty()), "W9: a listed model typed by its id is that choice (its name)");
            ApplyNewThreadButton(r, NewThreadAction{ K::BackToModels, 0 });
            ApplyNewThreadButton(r, NewThreadAction{ K::ModelSpecify, 0 });
            out = ApplyNewThreadText(r, L"@mimo-pro");
            CHECK((out.kind == O::CollectWindows && r.wizard.modelTarget == L"@mimo-pro" && out.rememberModel.empty()), "W9: a listed preset typed as @id");
            ApplyNewThreadButton(r, NewThreadAction{ K::BackToModels, 0 });
            ApplyNewThreadButton(r, NewThreadAction{ K::ModelSpecify, 0 });
            out = ApplyNewThreadText(r, L"@nope");
            CHECK((out.kind == O::Reply && out.text.find(L"No preset") != std::wstring::npos), "W9: an unknown preset is answered");
            out = ApplyNewThreadText(r, L"K:\\elsewhere");
            CHECK((out.kind == O::CheckFolder && out.typed), "W9: a typed folder at a later step changes the folder");
            ApplyNewThreadButton(r, NewThreadAction{ K::ModelDefault, 0 });
            ApplyNewThreadWindows(r.wizard, BuildNewThreadWindowPlan(WzCensus(), WzTabFacts(), NormDirKey(L"K:\\proj"), L"proj"));
            out = ApplyNewThreadText(r, L"2");
            CHECK((out.kind == O::Launch && r.wizard.windowId == L"{W2}" && r.wizard.anchorSessionId == L"d"), "W9: a window number typed at the window step: that window");
            NewThreadRecord w;
            w.wizard.step = NewThreadStep::Window;
            ApplyNewThreadWindows(w.wizard, BuildNewThreadWindowPlan(WzCensus(), WzTabFacts(), NormDirKey(L"K:\\proj"), L"proj"));
            CHECK((ApplyNewThreadText(w, L"5").kind == O::Hold && ApplyNewThreadText(w, L"what about window three").kind == O::Hold),
                  "W9: a number out of range or words at the window step: held");
            NewThreadRecord atFolder;
            CHECK((ApplyNewThreadText(atFolder, L"2").kind == O::Hold && atFolder.wizard.step == NewThreadStep::Folder),
                  "W9: a lone number at the folder step is the user's message (only the window step reads numbers)");
        }

        // ===== W10. Cancel at every step, the stale guard ====================================================================
        {
            const NewThreadStep steps[] = { NewThreadStep::Folder, NewThreadStep::CreateFolder, NewThreadStep::Model, NewThreadStep::ModelSpecify, NewThreadStep::Window };
            bool aborts = true;
            for (const NewThreadStep s : steps)
            {
                NewThreadRecord r;
                r.wizard.step = s;
                aborts = aborts && ApplyNewThreadButton(r, NewThreadAction{ K::Cancel, 0 }).kind == O::Abort;
                aborts = aborts && BuildNewThreadView(r, 0).rows.back().back().data == L"nt:x";
            }
            CHECK((aborts), "W10: every step has Cancel, and Cancel aborts (the service deletes the thread)");
            CHECK((!NewThreadStepTakes(NewThreadStep::Folder, K::ModelPick) && !NewThreadStepTakes(NewThreadStep::Model, K::CreateFolder) &&
                      !NewThreadStepTakes(NewThreadStep::Window, K::PickRecent) && !NewThreadStepTakes(NewThreadStep::ModelSpecify, K::ModelPick) &&
                      NewThreadStepTakes(NewThreadStep::ModelSpecify, K::ModelDefault) && NewThreadStepTakes(NewThreadStep::Window, K::BackToModels)),
                  "W10: which step takes which button");
            NewThreadRecord r;
            r.wizard.step = NewThreadStep::Model;
            r.wizard.modelChoices = { NewThreadModelChoice{ L"Opus", L"opus", false, false } };
            CHECK((ApplyNewThreadButton(r, NewThreadAction{ K::ModelPick, 3 }).kind == O::Stale && r.wizard.step == NewThreadStep::Model), "W10: a model index past the shown list is stale");
            CHECK((ApplyNewThreadButton(r, NewThreadAction{ K::None, 0 }).kind == O::Stale), "W10: a malformed button is stale");
        }

        // ===== W11. the window step's text stays one message =================================================================
        {
            NewThreadRecord r;
            r.wizard.folder = L"K:\\proj";
            r.wizard.step = NewThreadStep::Window;
            std::vector<WindowCensus> crowd;
            for (int i = 0; i < 20; ++i)
            {
                WindowCensus w;
                w.windowId = L"{C" + std::to_wstring(i) + L"}";
                for (uint32_t t = 0; t < 12; ++t)
                {
                    w.tabs.push_back(WzTab(t, L"a rather long tab title number " + std::to_wstring(t) + L" <x>", L""));
                }
                crowd.push_back(std::move(w));
            }
            ApplyNewThreadWindows(r.wizard, BuildNewThreadWindowPlan(crowd, {}, NormDirKey(L"K:\\proj"), L"proj"));
            const NewThreadView v = BuildNewThreadView(r, 0);
            CHECK((v.html.size() <= kNewThreadWindowTextBudget && v.html.find(L"<b>20</b>") != std::wstring::npos),
                  "W11: twenty windows of twelve long titles fit the budget (the titles shrink; every window is still listed)");
            NewThreadRecord few;
            few.wizard.folder = L"K:\\proj";
            few.wizard.step = NewThreadStep::Window;
            ApplyNewThreadWindows(few.wizard, BuildNewThreadWindowPlan({ crowd[0], crowd[1] }, {}, NormDirKey(L"K:\\proj"), L"proj"));
            const NewThreadView fv = BuildNewThreadView(few, 0);
            CHECK((fv.html.find(L"&lt;x&gt;") != std::wstring::npos && fv.html.find(L"number 9 ") != std::wstring::npos), "W11: two windows show their ten titles, escaped");
        }

        // ===== W12. persistence: the wizards and launches survive a restart ===================================================
        {
            std::map<int64_t, NewThreadRecord> records;
            NewThreadRecord a = rec; // the W6 record: every step answered
            a.pickerMessageId = 4321;
            a.held = { WzMessage(11, L"first prompt"), WzMessage(12, L"second <one>") };
            a.lastUseMs = 1000;
            a.wizard.notice = L"\x26A0 a notice";
            a.wizard.askWindow = true;
            records[777] = a;
            NewThreadRecord b;
            b.pickerMessageId = -1; // a post in flight
            b.lastUseMs = 0; // unknown: the day's grace starts at the restore
            records[778] = b;
            std::vector<NewThreadBind> binds(1);
            binds[0].launchToken = 9;
            binds[0].answered = true;
            binds[0].sessionId = L"sess-1";
            binds[0].threadId = 779;
            binds[0].dir = L"K:\\proj";
            binds[0].title = L"t";
            binds[0].held = { WzMessage(21, L"held in the bind") };
            binds[0].requestedMs = 5;
            binds[0].replyTo = 21;
            binds[0].wizard = a.wizard;
            const json::Value doc = NewThreadsToJson(records, binds);
            std::map<int64_t, NewThreadRecord> rr;
            std::vector<NewThreadBind> rb;
            NewThreadsFromJson(doc, rr, rb, 50000);
            CHECK((rr.size() == 2 && rb.size() == 1), "W12: every record and bind comes back");
            const auto& ra = rr[777];
            CHECK((ra.topicName == L"my work" && ra.listsBuilt && ra.lists.mostUsed.size() == 2 && ra.lists.mostUsed[1].path == L"K:\\other" && ra.pickerMessageId == 4321 &&
                      ra.held.size() == 2 && ra.held[1].StrAt(L"text") == L"second <one>" && ra.lastUseMs == 1000),
                  "W12: a record's lists, picker, held messages and clock");
            const auto& wz = ra.wizard;
            CHECK((wz.step == NewThreadStep::Window && wz.folder == L"K:\\proj" && wz.modelTarget == L"@mimo-pro" && wz.modelChoices.size() == 2 && wz.modelChoices[1].preset &&
                      wz.windowsCollected && wz.windows.size() == 2 && wz.windows[0].anchorSessionId == L"b" && wz.windows[0].titles.size() == 4 && wz.defaultWindow == 0 &&
                      wz.windowId == L"{W1}" && wz.anchorSessionId == L"b" && wz.notice == L"\x26A0 a notice" && wz.askWindow && wz.windowsSerial == 0),
                  "W12: the wizard's step, answers and the choices it showed (its census serial is not persisted)");
            CHECK((rr[778].pickerMessageId == 0 && rr[778].lastUseMs == 50000), "W12: a post in flight comes back as no picker; an unknown clock starts at the restore");
            CHECK((rb[0].sessionId == L"sess-1" && rb[0].threadId == 779 && rb[0].held.size() == 1 && rb[0].launchToken == 0 && !rb[0].answered && rb[0].requestedMs == 50000 &&
                      rb[0].replyTo == 21 && rb[0].wizard.modelTarget == L"@mimo-pro"),
                  "W12: a bind comes back with its wizard, but no token and no answer, its clock started at the restore");
            // Malformed entries: skipped, or read as their defaults; a choice index can never point outside its list.
            auto broken = json::Value::MkObj();
            auto threads = json::Value::MkArr();
            threads.Push(json::Value::MkStr(L"not an object"));
            auto noId = json::Value::MkObj();
            noId.Set(L"topicName", json::Value::MkStr(L"x"));
            threads.Push(noId);
            auto odd = json::Value::MkObj();
            odd.Set(L"threadId", json::Value::MkNum(5));
            auto oddWizard = json::Value::MkObj();
            oddWizard.Set(L"step", json::Value::MkNum(99));
            oddWizard.Set(L"defaultWindow", json::Value::MkNum(3));
            odd.Set(L"wizard", oddWizard);
            threads.Push(odd);
            auto win = json::Value::MkObj();
            win.Set(L"threadId", json::Value::MkNum(6));
            auto winWizard = json::Value::MkObj();
            winWizard.Set(L"step", json::Value::MkNum(4));
            winWizard.Set(L"windowsCollected", json::Value::MkBool(true));
            win.Set(L"wizard", winWizard);
            threads.Push(win);
            broken.Set(L"threads", threads);
            auto badBinds = json::Value::MkArr();
            auto noSession = json::Value::MkObj();
            noSession.Set(L"threadId", json::Value::MkNum(9));
            badBinds.Push(noSession);
            broken.Set(L"binds", badBinds);
            NewThreadsFromJson(broken, rr, rb, 1);
            CHECK((rr.size() == 2 && rb.empty()), "W12: a record without an object or a thread id, a bind without a session: skipped");
            CHECK((rr[5].wizard.step == NewThreadStep::Folder && rr[5].wizard.defaultWindow == -1), "W12: an unknown step reads as the folder step; a default outside the list as none");
            CHECK((rr[6].wizard.step == NewThreadStep::Window && !rr[6].wizard.windowsCollected), "W12: a window step whose list did not survive is read again");
            NewThreadsFromJson(json::Value{}, rr, rb, 1);
            CHECK((rr.empty() && rb.empty()), "W12: no document: nothing");
            // Through the state file (MessagingState::newThreads, opaque there).
            MessagingState st;
            st.newThreads = doc;
            const auto back = StateFromJson(StateToJson(st));
            std::map<int64_t, NewThreadRecord> sr;
            std::vector<NewThreadBind> sb;
            CHECK((back.has_value() && back->newThreads.type == json::Value::Type::Obj), "W12: the state file carries the wizards");
            if (back)
            {
                NewThreadsFromJson(back->newThreads, sr, sb, 7);
            }
            CHECK((sr.size() == 2 && sb.size() == 1 && sr[777].wizard.windows.size() == 2 && sb[0].held[0].StrAt(L"text") == L"held in the bind"), "W12: ...and gives them back whole");
            const auto plain = StateToJson(MessagingState{});
            CHECK((plain.Find(L"newThreads") == nullptr && StateFromJson(plain)->newThreads.type == json::Value::Type::Null),
                  "W12: a state without wizards writes no key (a file as before 21.25), and reads none");
        }

        // ===== W13. the abort line in the main view ==========================================================================
        {
            std::wstring html = BuildNewThreadAbortHtml(L"my <work>", { L"first <b>", L"", L"second" }, true, L"");
            CHECK((html.find(L"Cancelled") != std::wstring::npos && html.find(L"\x201Cmy &lt;work&gt;\x201D") != std::wstring::npos && html.find(L"2 messages") != std::wstring::npos &&
                      html.find(L"<blockquote expandable>first &lt;b&gt;\n\nsecond</blockquote>") != std::wstring::npos),
                  "W13: a cancelled thread: its name, every text it held (escaped, the empty one skipped) in one quote");
            html = BuildNewThreadAbortHtml(L"", {}, false, L"nothing was chosen in it for 24 hours");
            CHECK((html.find(L"a new thread") != std::wstring::npos && html.find(L"24 hours") != std::wstring::npos && html.find(L"blockquote") == std::wstring::npos &&
                      html.find(L"Cancelled") == std::wstring::npos),
                  "W13: an abandoned thread with nothing held: why, no quote");
            std::vector<std::wstring> lots(6, std::wstring(1500, L'q'));
            html = BuildNewThreadAbortHtml(L"t", lots, false, L"x");
            CHECK((html.find(L"6 messages") != std::wstring::npos && html.find(L"more not shown") != std::wstring::npos && html.size() < kNewThreadAbortTotalChars + 400),
                  "W13: long texts are cut, and past the total the rest is counted");
        }

        // ===== W14. the session's creation stamp is persisted =================================================================
        {
            SessionInfo s = MakeSession(L"created-1");
            s.createdUnixMs = 1234567;
            const SessionInfo back = SessionFromJson(ToJson(s));
            CHECK((back.createdUnixMs == 1234567), "W14: SessionInfo::createdUnixMs round-trips through sessions.json");
            const json::Value none = ToJson(MakeSession(L"created-0"));
            CHECK((none.Find(L"createdUnixMs") == nullptr && SessionFromJson(none).createdUnixMs == 0), "W14: an unstamped session writes no key (sessions.json byte-unchanged)");
        }
    }
}

void TestMessagingNewThread()
{
    wprintf(L"== TestMessagingNewThread (MESSAGING.md 21.17: title links, the You lines' title row, the turn chain fields, the new-thread picker) ==\n");
    const std::wstring blue = L"\U0001F7E6";
    const std::wstring green = L"\U0001F7E9";
    const std::wstring url42 = L"https://t.me/bot/42?topic=42";

    // ===== 1. the topic link ====================================================================================================
    {
        CHECK(TgTopicUrl(L"@bot", 9) == L"https://t.me/bot/9?topic=9" && TgTopicUrl(L"bot", 9) == L"https://t.me/bot/9?topic=9", "TgTopicUrl drops the @ and builds the topic link");
        CHECK(TgTopicUrl(L"", 9).empty() && TgTopicUrl(L"@", 9).empty() && TgTopicUrl(L"bot", 0).empty() && TgTopicUrl(L"bot", -3).empty(), "no link without a username or a thread");
        CHECK(DirectoryTopicUrl(L"@bot", 9) == TgTopicUrl(L"@bot", 9), "the directory's chips and the title links use one builder");
    }

    // ===== 1. a token that names its thread ===================================================================================
    {
        const std::wstring plain = TgTitleToken(TgTitleForm::Plain, blue, L"a <b> & c", 42);
        // Without the username: the pre-21.17 render byte for byte (every hash is taken this way).
        CHECK(TgRenderTitleTokensClassic(plain) == blue + L" a &lt;b&gt; &amp; c", "a threaded token renders like before without the username");
        CHECK(TgRenderTitleTokensClassic(plain) == TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, blue, L"a <b> & c")), "the thread does not change the hashed render");
        CHECK(TgRenderTitleTokensClassic(plain, L"bot") == blue + L" <a href=\"" + url42 + L"\">a &lt;b&gt; &amp; c</a>", "classic + username: the title is a link, the square outside it");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Bold, blue, L"t", 7), L"@Bot") == blue + L" <b><a href=\"https://t.me/Bot/7?topic=7\">t</a></b>",
              "the Bold form links inside the bold");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Quoted, blue, L"t", 42), L"bot") == L"\x201C" + blue + L" <a href=\"" + url42 + L"\">t</a>\x201D",
              "the Quoted form links inside the quotes");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Square, green, L"t", 42), L"bot") == green, "the bare square names no title and links nowhere");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, L"", L"t", 42), L"bot") == L"<a href=\"" + url42 + L"\">t</a>", "no square: the link alone");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, blue, L"t"), L"bot") == blue + L" t", "a token without a thread never links");
        // A stray THREAD marker in a title is dropped like the other framing characters; the token still parses.
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, L"", std::wstring{ L"a" } + kTgTitleTokenThread + L"b", 5), L"bot") ==
                  L"<a href=\"https://t.me/bot/5?topic=5\">ab</a>",
              "a THREAD marker inside a title is dropped");
        // A malformed thread part (not digits): no link, the title as before.
        const std::wstring bad = std::wstring{ kTgTitleTokenOpen } + L"p" + blue + kTgTitleTokenThread + L"4x2" + kTgTitleTokenMid + L"t" + kTgTitleTokenClose;
        CHECK(TgRenderTitleTokensClassic(bad, L"bot") == blue + L" t", "a thread part that is not a number links nowhere");
        const std::wstring tooLong = std::wstring{ kTgTitleTokenOpen } + L"p" + kTgTitleTokenThread + L"1234567890123456789" + kTgTitleTokenMid + L"t" + kTgTitleTokenClose;
        CHECK(TgRenderTitleTokensClassic(tooLong, L"bot") == L"t", "a thread of 19 digits (an overflow) links nowhere");
        // Rich: a URL chip with the look and the label of the copy chip; without the username the copy chip of 21.15.
        CHECK(TgRichHtmlFromClassic(L"x " + plain + L" y", L"bot") ==
                  L"<p>x <tg-button type=\"url\" style=\"primary\" url=\"" + url42 + L"\">" + blue + L" a &lt;b&gt; &amp; c</tg-button> y</p>",
              "rich + username: the title is a URL chip to its topic");
        CHECK(TgRichHtmlFromClassic(L"x " + plain + L" y") == L"<p>x <tg-button type=\"copy_text\" style=\"primary\" text=\"a &lt;b&gt; &amp; c\">" + blue + L" a &lt;b&gt; &amp; c</tg-button> y</p>",
              "rich without the username: the 21.15 copy chip");
        CHECK(TgRichHtmlFromClassic(TgTitleToken(TgTitleForm::Plain, blue, L"t"), L"bot").find(L"type=\"copy_text\"") != std::wstring::npos, "rich: a token without a thread stays a copy chip");
        CHECK(TgRichHtmlFromClassic(L"<blockquote>" + plain + L"</blockquote>", L"bot").find(L"type=\"url\"") != std::wstring::npos, "a title inside a quote links too");
        CHECK(TgRichHtmlFromClassic(L"<pre>" + plain + L"</pre>", L"bot").find(L"<a href") == std::wstring::npos, "a title inside code stays text");
        CHECK(TgTitleChipHtml(blue, L"t", url42) == L"<tg-button type=\"url\" style=\"primary\" url=\"" + url42 + L"\">" + blue + L" t</tg-button>", "TgTitleChipHtml with a url is a URL button");
        CHECK(TgTitleChipHtml(L"", L"t", L"https://t.me/b/1?topic=1&x=\"y\"").find(L"url=\"https://t.me/b/1?topic=1&amp;x=&quot;y&quot;\"") != std::wstring::npos, "the url attribute is escaped");
    }

    // ===== 1. the card, the change line, the Search cards ======================================================================
    {
        SessionInfo s = MakeSession(L"abcdef12-3456-7890-abcd-ef1234567890", SessionState::WaitingForInput);
        s.title = L"tab";
        CardExtras ex;
        ex.colorSquare = blue;
        ex.threadId = 42;
        const std::wstring card = BuildCardText(s, 1'800'000'000'000, 0, false, &ex);
        CardExtras noThread = ex;
        noThread.threadId = 0;
        const std::wstring cardBefore = BuildCardText(s, 1'800'000'000'000, 0, false, &noThread);
        CHECK(TgRenderTitleTokensClassic(card) == TgRenderTitleTokensClassic(cardBefore), "the card's hashed render does not move with its thread");
        CHECK(TgRenderTitleTokensClassic(card, L"bot").find(L"<b><a href=\"" + url42 + L"\">tab</a></b>") != std::wstring::npos, "the card's title links to its topic");
        const std::wstring notice = BuildTopicChangeNoticeHtml(L"old", blue, L"new", blue, 42);
        CHECK(TgRenderTitleTokensClassic(notice) == TgRenderTitleTokensClassic(BuildTopicChangeNoticeHtml(L"old", blue, L"new", blue)), "the change line renders like before without the username");
        CHECK(NtCount(TgRenderTitleTokensClassic(notice, L"bot"), L"<a href=\"" + url42 + L"\">") == 2, "both titles of the change line link to the topic");
        SearchCard sc;
        sc.number = 3;
        sc.sessionId = s.id;
        sc.title = L"found";
        sc.colorSquare = blue;
        sc.threadId = 42;
        sc.state = SearchPickState::Listed;
        CHECK(TgRenderTitleTokensClassic(BuildSearchPickMenuHtml(sc, 1'800'000'000'000), L"bot").find(L"<a href=\"" + url42 + L"\">found</a>") != std::wstring::npos, "the pick menu's title links to the topic");
        CHECK(TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"one", { sc }, 0, 1'800'000'000'000), L"bot").find(L"<a href=\"" + url42 + L"\">found</a>") != std::wstring::npos, "a Search card's title links to the topic");
        sc.threadId = 0;
        CHECK(TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"one", { sc }, 0, 1'800'000'000'000), L"bot").find(L"<a href") == std::wstring::npos, "a session with no topic: no link");
    }

    // ===== 1. the "You" lines' title row ======================================================================================
    {
        const std::wstring row = UserLineTitleRow(L"my tab", L"12345678-aaaa-bbbb-cccc-dddddddddddd", blue, 42);
        CHECK(row == TgTitleToken(TgTitleForm::Plain, blue, L"my tab", 42), "UserLineTitleRow: the title as a Plain token naming the topic");
        CHECK(UserLineTitleRow(L"", L"12345678-aaaa", L"", 0) == TgTitleToken(TgTitleForm::Plain, L"", L"12345678", 0), "no title: the short id, no link");
        // 2026-10-18: a title is printed on ONE line (TgTitleLine), no longer cut to its first line.
        CHECK(UserLineTitleRow(L"line one\nline two", L"x", L"", 1) == TgTitleToken(TgTitleForm::Plain, L"", L"line one line two", 1) &&
                  UserLineTitleRow(L"line one\rline two", L"x", L"", 1) == TgTitleToken(TgTitleForm::Plain, L"", L"line one line two", 1),
              "a two-line title: both lines, joined by a space (an LF, or the lone CR the tab's rename box writes)");
        const std::wstring without = BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false);
        const std::wstring with = BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false, row);
        const size_t nl = without.find(L'\n');
        CHECK(nl != std::wstring::npos && with == without.substr(0, nl) + L"\n" + row + without.substr(nl), "the prompt line: header, the title row, the prompt");
        CHECK(BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false, L"") == without, "no row: the 21.12 line byte for byte");
        CHECK(TgRenderTitleTokensClassic(with, L"bot").find(L"\n" + blue + L" <a href=\"" + url42 + L"\">my tab</a>\nhello") != std::wstring::npos, "the row renders as a linked title on line 2");
        const std::wstring rich = TgRichHtmlFromClassic(with, L"bot");
        CHECK(rich.find(L"<br><tg-button type=\"url\"") != std::wstring::npos && rich.find(L"</tg-button><br>hello") != std::wstring::npos, "rich: the row is a URL chip on its own line");
        const std::wstring answer = BuildAnswerLineHtml(0, L"yes", row);
        CHECK(answer.find(L"\n" + row + L"\n") != std::wstring::npos && BuildAnswerLineHtml(0, L"yes") == BuildAnswerLineHtml(0, L"yes", L""), "the answer line has the row; none without it");
        const std::wstring command = BuildCommandLineHtml(0, L"/compact", row);
        CHECK(command.find(L"\n" + row + L"\n<code>/compact</code>") != std::wstring::npos, "the command line has the row above the command");
        // The history replay: every "You" line of the bot carries the row; the assistant's posts do not.
        // (A first turn gets a day line before its prompt, so the prompt is looked up, not taken as the first post.)
        int64_t day = 0;
        const auto posts = RenderHistory({ NtItem(TopicItemKind::Prompt, L"go", 1'800'000'000'000), NtItem(TopicItemKind::Text, L"Done.", 0, true) }, false, day, {}, false, row);
        const auto prompt = std::find_if(posts.begin(), posts.end(), [](const HistoryPost& p) { return p.turnPrompt; });
        CHECK(prompt != posts.end() && prompt->fromUser && prompt->html.find(row) != std::wstring::npos, "the replayed prompt line carries the row");
        CHECK(!posts.empty() && posts.back().turnReply && posts.back().html.find(row) == std::wstring::npos, "the reply carries no row");
        int64_t day2 = 0;
        const auto plainPosts = RenderHistory({ NtItem(TopicItemKind::Prompt, L"go", 1'800'000'000'000), NtItem(TopicItemKind::Text, L"Done.", 0, true) }, false, day2, {}, false);
        CHECK(!plainPosts.empty() && std::none_of(plainPosts.begin(), plainPosts.end(), [](const HistoryPost& p) { return TgHasTitleTokens(p.html); }),
              "no row: the replay's lines carry no token");
        HistoryWindow w;
        w.items = { NtItem(TopicItemKind::Prompt, L"go", 1'800'000'000'000), NtItem(TopicItemKind::Text, L"Done.", 0, true) };
        w.itemAt = { 0, 10 };
        w.consumedTo = 20;
        int64_t day3 = 0;
        const auto planned = PlanHistoryPosts(w, day3, {}, false, row);
        CHECK(std::any_of(planned.begin(), planned.end(), [&](const HistoryPlanPost& p) { return p.post.turnPrompt && p.post.html.find(row) != std::wstring::npos; }),
              "PlanHistoryPosts passes the row to the replay");
    }

    // ===== 1.5 the topic record's new fields ==================================================================================
    {
        MessagingState st;
        TopicRecord t;
        t.sessionId = L"s1";
        t.threadId = 5;
        t.cardLinked = true;
        t.historyPromptMessageId = 77;
        st.topics[L"s1"] = t;
        TopicRecord bare;
        bare.sessionId = L"s2";
        bare.threadId = 6;
        st.topics[L"s2"] = bare;
        const auto parsed = json::Parse(json::Dump(StateToJson(st)));
        const auto back = parsed ? StateFromJson(*parsed) : std::nullopt;
        CHECK(back && back->topics.count(L"s1") && back->topics.at(L"s1").cardLinked && back->topics.at(L"s1").historyPromptMessageId == 77, "cardLinked and historyPromptMessageId round-trip");
        CHECK(back && back->topics.count(L"s2") && !back->topics.at(L"s2").cardLinked && back->topics.at(L"s2").historyPromptMessageId == 0, "absent: not linked, no chain yet");
        CHECK(json::Dump(StateToJson(st)).find(L"historyPromptMessageId\":0") == std::wstring::npos, "an empty chain is not written");
    }

    // ===== 2. the picker's lists ==============================================================================================
    const int64_t now = 1'800'000'000'000;
    std::vector<SessionFolderFacts> facts;
    for (int i = 1; i <= 23; ++i)
    {
        wchar_t name[32];
        swprintf(name, 32, L"K:\\w\\f%02d", i);
        facts.push_back(NtFacts(name, L"", now - i * 60'000)); // fNN active NN minutes ago
    }
    facts.push_back(NtFacts(L"K:\\w\\f05", L"", now - 100 * 60'000)); // f05: 3 sessions, the most used
    facts.push_back(NtFacts(L"K:\\w\\f05", L"", now - 200 * 60'000));
    facts.push_back(NtFacts(L"K:\\w\\f09", L"", now - 300 * 60'000)); // f09: 2 sessions
    facts.push_back(NtFacts(L"C:\\Temp\\probe", L"", now)); // the newest, under a scratch root: never offered
    facts.push_back(NtFacts(L"K:\\w\\gone", L"", now - 30'000)); // the probe says it is gone
    facts.push_back(NtFacts(L"K:\\w\\f01", L"K:\\w\\deep", now - 45'000)); // f01: 2 sessions; deep: inferred only
    size_t probes = 0;
    std::vector<std::wstring> probed;
    const auto probe = [&](const std::wstring& path) {
        ++probes;
        probed.push_back(path);
        return path != L"K:\\w\\gone";
    };
    const NewThreadLists lists = BuildNewThreadLists(facts, { NormDirKey(L"C:\\Temp") }, probe);
    {
        CHECK(lists.recent.size() == 24 && lists.mostUsed.size() == 24, "24 folders in each list (23 + the inferred one; temp and gone left out)");
        CHECK(!lists.recent.empty() && lists.recent[0].path == L"K:\\w\\f01" && lists.recent[1].path == L"K:\\w\\deep" && lists.recent[2].path == L"K:\\w\\f02",
              "Recent: the newest activity first, a tie by sessions");
        CHECK(!lists.mostUsed.empty() && lists.mostUsed[0].path == L"K:\\w\\f05" && lists.mostUsed[0].sessions == 3 && lists.mostUsed[1].path == L"K:\\w\\f01" &&
                  lists.mostUsed[2].path == L"K:\\w\\f09" && lists.mostUsed[3].path == L"K:\\w\\deep",
              "Most used: the most sessions first, a tie by recency");
        bool noTemp = true;
        for (const auto& f : lists.recent)
        {
            noTemp = noTemp && f.path.find(L"Temp") == std::wstring::npos && f.path != L"K:\\w\\gone";
        }
        CHECK(noTemp, "no scratch folder and no gone folder in Recent");
        CHECK(probes == 25 && std::find(probed.begin(), probed.end(), L"C:\\Temp\\probe") == probed.end(), "one probe per folder (25), none for a scratch root");
        const NewThreadLists capped = BuildNewThreadLists(facts, {}, nullptr, 5);
        CHECK(capped.recent.size() == 5 && capped.mostUsed.size() == 5 && capped.recent[0].path == L"C:\\Temp\\probe", "the cap per list; no probe and no roots keep everything");
        CHECK(BuildNewThreadLists({}, {}, probe).recent.empty(), "no sessions: empty lists");
    }

    // ===== 2. pages and the buttons' data ======================================================================================
    {
        CHECK(NewThreadPageCount(0) == 1 && NewThreadPageCount(10) == 1 && NewThreadPageCount(11) == 2 && NewThreadPageCount(24) == 3, "page counts (10 a page, at least 1)");
        CHECK(ClampNewThreadPage(5, 24) == 2 && ClampNewThreadPage(1, 0) == 0 && ClampNewThreadPage(1, 24) == 1, "a page is kept inside the list");
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PickRecent, 3) == L"nt:r:3" && NewThreadCallbackData(NewThreadAction::Kind::PickUsed, 0) == L"nt:m:0" &&
                  NewThreadCallbackData(NewThreadAction::Kind::PageRecent, 1) == L"nt:rp:1" && NewThreadCallbackData(NewThreadAction::Kind::PageUsed, 2) == L"nt:mp:2" &&
                  NewThreadCallbackData(NewThreadAction::Kind::Cancel, 9) == L"nt:x" && NewThreadCallbackData(NewThreadAction::Kind::None, 1).empty(),
              "the buttons' callback data");
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PageUsed, 999999999).size() <= 64, "within the Bot API's 64 bytes");
        const auto back = ParseNewThreadCallbackData(L"nt:m:17");
        CHECK(back.kind == NewThreadAction::Kind::PickUsed && back.value == 17, "parse: a pick");
        CHECK(ParseNewThreadCallbackData(L"nt:rp:2").kind == NewThreadAction::Kind::PageRecent && ParseNewThreadCallbackData(L"nt:x").kind == NewThreadAction::Kind::Cancel, "parse: a page, cancel");
        CHECK(ParseNewThreadCallbackData(L"nt:r:").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:r:3a").kind == NewThreadAction::Kind::None &&
                  ParseNewThreadCallbackData(L"nt:q:1").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:r:1234567890").kind == NewThreadAction::Kind::None &&
                  ParseNewThreadCallbackData(L"c:r:1").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:").kind == NewThreadAction::Kind::None,
              "parse: malformed data is no action");
        CHECK(ParseNewThreadCallbackData(L"d:front:5").kind == NewThreadAction::Kind::None, "a directory dot is not a picker button");
        // The pager's page label (2026-10-18): its own data, so a tap only answers which page it is.
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PageLabelRecent, 1) == L"nt:rl:1" && NewThreadCallbackData(NewThreadAction::Kind::PageLabelUsed, 0) == L"nt:ml:0",
              "the pager labels' callback data");
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PageLabelUsed, static_cast<size_t>(-1)).size() <= 26, "the longest data: 26 characters at most");
        const auto label = ParseNewThreadCallbackData(L"nt:ml:3");
        CHECK(label.kind == NewThreadAction::Kind::PageLabelUsed && label.value == 3 && ParseNewThreadCallbackData(L"nt:rl:0").kind == NewThreadAction::Kind::PageLabelRecent,
              "parse: a pager label");
        CHECK(ParseNewThreadCallbackData(L"nt:rl:").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:ml:2x").kind == NewThreadAction::Kind::None &&
                  ParseNewThreadCallbackData(L"nt:l:1").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:rlx:1").kind == NewThreadAction::Kind::None,
              "parse: a malformed label is no action");
        CHECK(NewThreadPageLabel(0, 4) == L"1 / 4" && NewThreadPageLabel(3, 4) == L"4 / 4" && NewThreadPageLabel(1, 2) == L"2 / 2", "the page label: the 1-based page / the pages");
        CHECK(NewThreadPageToast(L"Recent", 1, 34) == L"Recent: page 2 of 4 (folders 11\x2013" L"20 of 34)", "the label's toast: the page, the pages, the folders shown");
        CHECK(NewThreadPageToast(L"Most used", 9, 34) == L"Most used: page 4 of 4 (folders 31\x2013" L"34 of 34)", "a label past the end tells the last page");
        CHECK(NewThreadPageToast(L"Recent", 0, 0) == L"Recent: no folders known yet." && NewThreadPageToast(L"Recent", 0, 7) == L"Recent: page 1 of 1 (folders 1\x2013" L"7 of 7)",
              "an empty list says so; a short list is one page");
    }

    // ===== 2. the pager rows (2026-10-18) ======================================================================================
    // The user's layout: "We start with one button to the right and after moving one page there are 2 buttons and one label in
    // middle for the page number. reaching end shows only to the left".
    {
        using K = NewThreadAction::Kind;
        CHECK(BuildNewThreadPagingRow(L"Recent", 0, 0, K::PageRecent, K::PageLabelRecent).empty() && BuildNewThreadPagingRow(L"Recent", 10, 0, K::PageRecent, K::PageLabelRecent).empty(),
              "one page, or none: no pager");
        const auto firstPage = BuildNewThreadPagingRow(L"Recent", 34, 0, K::PageRecent, K::PageLabelRecent);
        CHECK(firstPage.size() == 2 && firstPage[0].text == L"1 / 4" && firstPage[0].data == L"nt:rl:0" && firstPage[1].text == L"Recent \x25B6" && firstPage[1].data == L"nt:rp:1",
              "the first page: the label, then the one arrow, on the right");
        const auto middlePage = BuildNewThreadPagingRow(L"Recent", 34, 1, K::PageRecent, K::PageLabelRecent);
        CHECK(middlePage.size() == 3 && middlePage[0].text == L"\x25C0 Recent" && middlePage[0].data == L"nt:rp:0" && middlePage[1].text == L"2 / 4" &&
                  middlePage[1].data == L"nt:rl:1" && middlePage[2].text == L"Recent \x25B6" && middlePage[2].data == L"nt:rp:2",
              "a middle page: the two arrows with the label between them");
        const auto lastPage = BuildNewThreadPagingRow(L"Most used", 34, 3, K::PageUsed, K::PageLabelUsed);
        CHECK(lastPage.size() == 2 && lastPage[0].text == L"\x25C0 Most used" && lastPage[0].data == L"nt:mp:2" && lastPage[1].text == L"4 / 4" && lastPage[1].data == L"nt:ml:3",
              "the last page: the one arrow, on the left, then the label");
        const auto pastEnd = BuildNewThreadPagingRow(L"Recent", 34, 99, K::PageRecent, K::PageLabelRecent);
        CHECK(pastEnd.size() == 2 && pastEnd[0].data == L"nt:rp:2" && pastEnd[1].text == L"4 / 4" && pastEnd[1].data == L"nt:rl:3", "a page past the end: the last page's pager");
        const auto twoPages = BuildNewThreadPagingRow(L"Recent", 20, 1, K::PageRecent, K::PageLabelRecent);
        CHECK(twoPages.size() == 2 && twoPages[0].data == L"nt:rp:0" && twoPages[1].text == L"2 / 2", "two pages (dev's 20 folders): the second page is the last");
    }

    // ===== 2. the labels ======================================================================================================
    {
        CHECK(NewThreadFolderLabel(L"K:\\a\\proj", { L"K:\\a\\proj", L"K:\\b\\other" }) == L"proj", "a label is the folder's name");
        CHECK(NewThreadFolderLabel(L"K:\\a\\proj", { L"K:\\a\\proj", L"Q:\\b\\Proj" }) == L"a\\proj", "two shown folders with one name: the parent too");
        CHECK(NewThreadFolderLabel(L"K:\\a\\proj", { L"K:\\a\\proj", L"k:/a/proj/" }) == L"proj", "the same folder spelled twice is no clash");
        CHECK(NewThreadFolderLabel(L"K:\\", {}) == L"K:", "a drive root");
        const std::wstring longLabel = NewThreadFolderLabel(L"K:\\x\\" + std::wstring(40, L'n'), {});
        CHECK(longLabel.size() == kNewThreadLabelMaxChars && longLabel.back() == L'\x2026', "a long name is cut with an ellipsis");
    }

    // ===== 2. the picker's text ===============================================================================================
    {
        const std::wstring html = BuildNewThreadPickerHtml(lists, 1, 0, 0, L"", now);
        CHECK(html.find(L"\U0001F4C2 <b>New session</b>") == 0, "the picker starts with what it is for");
        CHECK(html.find(L"\U0001F558 <b>Recent</b> \x00B7 11\x2013" L"20 of 24") != std::wstring::npos, "Recent's page 2 shows 11-20 of 24");
        CHECK(html.find(L"\n<b>11</b> K:\\w\\f10 \x00B7 10m ago") != std::wstring::npos, "a Recent line: number, path, how long ago");
        CHECK(html.find(L"\U0001F3C6 <b>Most used</b> \x00B7 1\x2013" L"10 of 24") != std::wstring::npos, "Most used's first page");
        CHECK(html.find(L"\n<b>1</b> K:\\w\\f05 \x00B7 3 sessions") != std::wstring::npos && html.find(L"K:\\w\\f02 \x00B7 1 session\n") != std::wstring::npos,
              "a Most used line: number, path, sessions (singular for one)");
        CHECK(html.find(L"A message you send here waits") != std::wstring::npos && html.find(L"the folder's name \x2014 rename this thread") != std::wstring::npos,
              "no message held, no name: what happens and how the title is chosen");
        const std::wstring two = BuildNewThreadPickerHtml(lists, 0, 0, 2, L"my <tab>", now);
        CHECK(two.find(L"Your 2 messages wait here") != std::wstring::npos && two.find(L"Its title: <b>my &lt;tab&gt;</b>") != std::wstring::npos, "held messages counted; the name escaped");
        CHECK(BuildNewThreadPickerHtml(lists, 0, 0, 1, L"", now).find(L"Your message waits here") != std::wstring::npos, "one held message");
        const std::wstring empty = BuildNewThreadPickerHtml(NewThreadLists{}, 0, 0, 0, L"", now);
        CHECK(NtCount(empty, L"No folders known yet.") == 2 && empty.find(L"send a folder's full path") != std::wstring::npos, "empty lists say so; a path still works");
        CHECK(BuildNewThreadPickerHtml(lists, 99, 0, 0, L"", now).find(L"21\x2013" L"24 of 24") != std::wstring::npos, "a page past the end shows the last one");
    }

    // ===== 2. the keyboard ====================================================================================================
    {
        const auto rows = BuildNewThreadPickerKeyboard(lists, 1, 0);
        CHECK(rows.size() == 13, "5 + pager + 5 + pager + cancel rows");
        if (rows.size() == 13)
        {
            CHECK(rows[0].size() == 2 && rows[0][0].text == L"\U0001F558 11 \x00B7 f10" && rows[0][0].data == L"nt:r:10" && rows[0][1].data == L"nt:r:11", "Recent's folders two to a row, by global index");
            CHECK(rows[5].size() == 3 && rows[5][0].data == L"nt:rp:0" && rows[5][0].text == L"\x25C0 Recent" && rows[5][1].text == L"2 / 3" && rows[5][1].data == L"nt:rl:1" &&
                      rows[5][2].data == L"nt:rp:2" && rows[5][2].text == L"Recent \x25B6",
                  "Recent's page 2 of 3: both arrows around its page label");
            CHECK(rows[6][0].text == L"\U0001F3C6 1 \x00B7 f05" && rows[6][0].data == L"nt:m:0", "Most used's first folder");
            CHECK(rows[11].size() == 2 && rows[11][0].text == L"1 / 3" && rows[11][0].data == L"nt:ml:0" && rows[11][1].data == L"nt:mp:1" && rows[11][1].text == L"Most used \x25B6",
                  "Most used's first page: its label, then the arrow forward");
            CHECK(rows[12].size() == 1 && rows[12][0].data == L"nt:x", "Cancel last");
        }
        const auto lastRows = BuildNewThreadPickerKeyboard(lists, 2, 2);
        CHECK(lastRows.size() == 7, "both last pages (folders 21-24): 2 + pager + 2 + pager + cancel rows");
        if (lastRows.size() == 7)
        {
            CHECK(lastRows[2].size() == 2 && lastRows[2][0].text == L"\x25C0 Recent" && lastRows[2][0].data == L"nt:rp:1" && lastRows[2][1].text == L"3 / 3" && lastRows[2][1].data == L"nt:rl:2",
                  "Recent's last page: the arrow back, then its label");
            CHECK(lastRows[5].size() == 2 && lastRows[5][0].text == L"\x25C0 Most used" && lastRows[5][1].text == L"3 / 3" && lastRows[5][1].data == L"nt:ml:2",
                  "Most used's last page: the arrow back, then its label");
        }
        const NewThreadLists few = BuildNewThreadLists({ NtFacts(L"K:\\a\\one", L"", now), NtFacts(L"K:\\b\\one", L"", now - 1), NtFacts(L"K:\\c\\two", L"", now - 2) }, {}, nullptr);
        const auto fewRows = BuildNewThreadPickerKeyboard(few, 0, 0); // (never "small": a <windows.h> macro)
        CHECK(fewRows.size() == 5, "3 folders: 2 rows + 2 rows + cancel, no paging rows");
        if (fewRows.size() == 5)
        {
            CHECK(fewRows[0][0].text == L"\U0001F558 1 \x00B7 a\\one" && fewRows[0][1].text == L"\U0001F558 2 \x00B7 b\\one" && fewRows[1][0].text == L"\U0001F558 3 \x00B7 two",
                  "shown folders with one name get their parent; the others their name");
        }
        CHECK(BuildNewThreadPickerKeyboard(NewThreadLists{}, 0, 0).size() == 1, "no folders: Cancel only");
    }

    // ===== 2. the user's own words ============================================================================================
    {
        CHECK(NewThreadTypedFolder(L"  \"K:/src/x\" ") == std::optional<std::wstring>{ L"K:\\src\\x" }, "a quoted forward-slash path is a folder");
        CHECK(NewThreadTypedFolder(L"\\\\server\\share\\x") == std::optional<std::wstring>{ L"\\\\server\\share\\x" }, "a UNC path is a folder");
        CHECK(!NewThreadTypedFolder(L"fix the build") && !NewThreadTypedFolder(L"K:\\a\nmore") && !NewThreadTypedFolder(L"src\\x") && !NewThreadTypedFolder(L""),
              "a prompt, two lines, a relative path, nothing: no folder");
        CHECK(StripTopicNameDecoration(L"\U0001F7E1 \U0001F7E6 my title") == L"my title" && StripTopicNameDecoration(L"my title") == L"my title", "glyph and square go; a bare name stays");
        CHECK(StripTopicNameDecoration(L"\U0001F535 x") == L"x" && StripTopicNameDecoration(L"\U0001F7E6 x") == L"x" && StripTopicNameDecoration(L"\x26AA ") == L"", "each part alone; nothing left");
        CHECK(StripTopicNameDecoration(L"\U0001F7E6\U0001F7E6 x") == L"\U0001F7E6 x" && StripTopicNameDecoration(L"  plain  ") == L"plain", "one square only; edges trimmed");
    }

    // ===== 2. the registry's lean read ========================================================================================
    {
        SessionRegistry reg;
        SessionInfo a = MakeSession(L"a1");
        a.workingDir = L"K:\\x";
        a.inferredWorkingDir = L"K:\\y";
        a.lastActivityUnixMs = 5;
        a.convLastActivityUnixMs = 9;
        reg.Upsert(a);
        SessionInfo b = MakeSession(L"b2");
        b.workingDir = L"K:\\z";
        b.lastActivityUnixMs = 7;
        b.live = false; // archived records count ("top 10 used overall")
        reg.Upsert(b);
        const auto samples = reg.PlaceSamples();
        CHECK(samples.size() == 2 && samples[0].workingDir == L"K:\\x" && samples[0].inferredWorkingDir == L"K:\\y" && samples[0].lastActivityUnixMs == 9 &&
                  samples[1].workingDir == L"K:\\z" && samples[1].lastActivityUnixMs == 7,
              "PlaceSamples: every record in insertion order, the newer activity stamp");
    }

    // ===== 3. the bounded folder check's text, and a deleted held message (§21.17.4) ==========================================
    {
        const std::wstring share = L"\\\\nas\\x";
        const std::wstring noAnswer = NewThreadFolderNoAnswerText(share);
        CHECK(noAnswer.rfind(L"The folder did not answer within 2 s (an offline network share?)", 0) == 0 && noAnswer.size() > share.size() &&
                  noAnswer.compare(noAnswer.size() - share.size(), share.size(), share) == 0,
              "the no-answer text: the reason first, the folder last (a button's toast is cut at 190 characters)");
        CHECK(HeldMessageGone(false, 400, L"Bad Request: message to react not found"), "a deleted held message: the Bot API's words for it");
        CHECK(HeldMessageGone(false, 400, L"Bad Request: MESSAGE_ID_INVALID") && HeldMessageGone(false, 400, L"Bad Request: message not found"),
              "...and the raw forms of the same answer");
        CHECK(!HeldMessageGone(true, 200, L"") && !HeldMessageGone(false, 429, L"Too Many Requests: retry after 5") && !HeldMessageGone(false, 0, L"transport failure 12007"),
              "a success, a flood refusal, a lost connection: no proof, so the message is handed over");
        CHECK(!HeldMessageGone(false, 400, L"Bad Request: message thread not found") && !HeldMessageGone(false, 400, L"Bad Request: chat not found") &&
                  !HeldMessageGone(false, 400, L"Bad Request: REACTION_INVALID"),
              "a missing topic, a missing chat, a refused reaction: none says the message was deleted");
    }

    // ===== 3. the bounded folder probe (FolderProbe.h) ========================================================================
    {
        // A probe that answers at once for local paths and HANGS for the share "\\nas\…" until the test opens the gate — the
        // offline share of the report. Its own wait is bounded too, so a failed check cannot hang the suite. Every path it is
        // run for is recorded (under the gate's lock), so the checks can count probes per path.
        struct Gate
        {
            std::mutex m;
            std::condition_variable cv;
            bool open{ false };
            std::vector<std::wstring> asked;
        };
        const auto gate = std::make_shared<Gate>();
        const auto askedFor = [&gate](const std::wstring& path) {
            std::lock_guard lk{ gate->m };
            return static_cast<int>(std::count(gate->asked.begin(), gate->asked.end(), path));
        };
        // Polls `done` for up to 10 s: the probe threads start and end on their own clock.
        const auto waitFor = [](const auto& done) {
            const uint64_t until = ::GetTickCount64() + 10000;
            while (!done() && ::GetTickCount64() < until)
            {
                ::Sleep(5);
            }
            return done();
        };
        FolderProber prober{ [gate](const std::wstring& path) {
                                {
                                    std::lock_guard lk{ gate->m };
                                    gate->asked.push_back(path);
                                }
                                if (path.rfind(L"\\\\nas\\", 0) == 0)
                                {
                                    std::unique_lock lk{ gate->m };
                                    gate->cv.wait_for(lk, std::chrono::seconds{ 20 }, [&gate] { return gate->open; });
                                    return true;
                                }
                                return path == L"K:\\here";
                            },
                             2 };
        CHECK(prober.Probe(L"K:\\here", 2000) == FolderProbeAnswer::Folder && prober.Probe(L"K:\\gone", 2000) == FolderProbeAnswer::NotFolder,
              "a probe that answers: Folder, NotFolder");
        CHECK(prober.Probe(L"", 2000) == FolderProbeAnswer::NotFolder && askedFor(L"") == 0, "an empty path is no folder, and nothing is asked");
        const uint64_t t0 = ::GetTickCount64();
        CHECK(prober.Probe(L"\\\\nas\\off", 100) == FolderProbeAnswer::NoAnswer, "a share that hangs: NoAnswer once the caller's wait is up");
        CHECK(::GetTickCount64() - t0 < 5000, "...the CALLER's wait, not the share's timeout");
        CHECK(waitFor([&] { return askedFor(L"\\\\nas\\off") == 1; }), "the hung probe runs on its own thread");
        CHECK(prober.Probe(L"\\\\nas\\off", 50) == FolderProbeAnswer::NoAnswer && askedFor(L"\\\\nas\\off") == 1 && prober.InFlight() == 1,
              "a second ask joins the probe in flight: never a second thread for one path");
        CHECK(prober.Probe(L"\\\\nas\\other", 0) == FolderProbeAnswer::NoAnswer && prober.InFlight() == 2, "a 0 wait starts a probe and returns at once");
        const uint64_t t1 = ::GetTickCount64();
        CHECK(prober.Probe(L"\\\\nas\\third", 2000) == FolderProbeAnswer::NoAnswer && ::GetTickCount64() - t1 < 1000 && prober.InFlight() == 2,
              "past the cap (2 hung probes): NoAnswer at once, and no third thread");
        CHECK(waitFor([&] { return askedFor(L"\\\\nas\\other") == 1; }) && askedFor(L"\\\\nas\\third") == 0, "the capped path was never probed");
        {
            std::lock_guard lk{ gate->m };
            gate->open = true; // the share comes back: both hung probes answer
        }
        gate->cv.notify_all();
        CHECK(waitFor([&] { return prober.InFlight() == 0; }), "both probes end once the share answers");
        CHECK(prober.Probe(L"\\\\nas\\off", 0, 60000) == FolderProbeAnswer::Folder && askedFor(L"\\\\nas\\off") == 1,
              "the late answer was kept: an ask that accepts it (a picker's list) takes it without a probe");
        CHECK(prober.Probe(L"\\\\nas\\off", 2000, 0) == FolderProbeAnswer::Folder && askedFor(L"\\\\nas\\off") == 2, "maxAgeMs 0 (a pick) asks the folder again");

        // A probe that THROWS (only an injected one can; GetFileAttributesW does not): NoAnswer, and the next ask probes again.
        const auto throws = std::make_shared<int>(0);
        FolderProber thrower{ [throws](const std::wstring&) -> bool {
            ++*throws;
            throw std::runtime_error{ "probe failed" };
        } };
        CHECK(thrower.Probe(L"K:\\x", 2000, 60000) == FolderProbeAnswer::NoAnswer, "a probe that throws reads NoAnswer");
        CHECK(waitFor([&] { return thrower.InFlight() == 0; }) && thrower.Probe(L"K:\\x", 2000, 60000) == FolderProbeAnswer::NoAnswer &&
                  waitFor([&] { return thrower.InFlight() == 0; }) && *throws == 2,
              "...its failure is not kept as an answer: the next ask probes again");
    }
    TestMessagingNewThreadWizard(); // §21.25: the new-thread wizard (its own header line)
}
