// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the NEW-THREAD WIZARD (MESSAGING.md §21.25): what a thread the user opens in the bot's chat asks, one step at a
// time and in ONE message that is edited in place (the §21.17 picker), before its session starts. The user's request
// (2026-10-18):
//   "If path picker was set manual path and it does not exist (checking case-insensitively as we on win) then we will ask user
//    to confirm creation or back to pick again. After path choosing, add model chooser which shows basically same options like
//    right-click menu context Fork/Resume Session->. This is an optional pick and skip == default. User can specify model
//    manually like "claude-opus-5-5[1m]". After model picking, Allow window picking by number, each window has stats like
//    number of tabs, last tab created, last 10 tab titles. Defaults to finding all tabs with same (inferred) working directory
//    and picking the tab with newest tab with that (inferred) working directory. Make sure the lifecycle is complete and qa
//    through tests."
//
// THE STEPS (NewThreadStep):
//   Folder        the §21.17 picker: Recent / Most used, or a typed folder path. A typed folder that does not exist goes to
//   CreateFolder  "create it?": Create, or Back to the folder lists. The check is CASE-INSENSITIVE (ResolveNewThreadFolder), so
//                 "k:\SOURCE\agentmaster" is the existing "K:\source\Agentmaster" and is never offered as a new folder.
//   Model         optional, Default when skipped: the configured launch models and the presets (the right-click menu's "Fork
//                 session ▸" list, LaunchPresets.h LaunchMenuEntries), the model last typed in a "Specify a model" prompt, and
//                 ✏ Specify…, which leads to
//   ModelSpecify  a model id the user TYPES ("claude-opus-5-5[1m]"), checked by ParseNewThreadModelId. "/model <id>" typed at
//                 the Model step does the same in one message.
//   Window        by number, each window with its stats (tabs, the newest tab, the ten newest titles). The DEFAULT is the window
//                 of the NEWEST tab working in the chosen folder (its launch or inferred folder), and the new tab opens right
//                 after that tab. One window, or none answering, skips the step.
//   Then the launch (§21.17.4 / §21.23). A launch the PC refuses comes back to the step that can fix it
//   (NewThreadStepAfterRefusal). ✖ Cancel at any step deletes the thread — the user's rule, "If the creation process aborts in
//   any way, the new topic needs to be deleted" — and so does a wizard nobody touched for a day (kNewThreadPickerIdleMs).
//
// This header is the PURE half: the records (persisted in state.json, so a restart resumes a wizard where it stood), the
// decisions (which button a step takes, what a typed message means, where a refusal goes back to), the folder resolution over
// an INJECTED file system, the model-id check, the window choices and their default, and every message and keyboard. No I/O,
// no Win32, no WinRT: the m5 suite drives whole lifecycles through it (tests_messaging_newthread.cpp, TestMessagingNewThreadWizard).
// The service half — the bounded folder checks and the creation, the window census, the launch, the thread's deletion — is
// MessagingService's (Messaging.Inbound.cpp, "the new-thread wizard").

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype> // std::towupper (the drive letter of a root)
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Json.h" // json::Value (the held messages, the persisted records)
#include "MessagingNewThread.h" // NewThreadLists, NewThreadAction, the picker's builders, kNewThread* limits
#include "RemoteLaunch.h" // RemoteLaunchResult (where a refused launch goes back to)
#include "WindowTabTools.h" // WindowCensus (the window step's input)

namespace Agentmaster::Msg
{
    // ===== the limits ================================================================================================

    // The newest tab titles a window's stats list — the user's "last 10 tab titles".
    inline constexpr size_t kNewThreadWindowTitles = 10;
    // The most characters of ONE title in a window's stats (an ellipsis past it): ten titles for several windows must fit in one
    // Telegram message (4,096 characters), and the title is there to be recognized, not read in full.
    inline constexpr size_t kNewThreadWindowTitleChars = 40;
    // The most windows the step lists (one button each). A session is rarely opened into the 21st window; past it the step lists
    // the first ones in creation order and says how many it left out.
    inline constexpr size_t kNewThreadWindowsMax = 20;
    // The window step's text budget (UTF-16 units): past it each window lists fewer titles (10, then 6, then 3, then none), so the
    // message stays one Telegram message however many tabs the windows hold.
    inline constexpr size_t kNewThreadWindowTextBudget = 3600;
    // The most model choices the model step shows: the PC's menus cap the configured models at 32, and presets come on top; a
    // keyboard of more buttons scrolls off a phone's screen.
    inline constexpr size_t kNewThreadModelChoicesMax = 40;
    // The longest model id a user may type (a real id is under 60 characters; this bounds the text that reaches the launch).
    inline constexpr size_t kNewThreadModelIdMaxChars = 120;
    // The most characters of a button's label (an ellipsis past it): two buttons share a row on a phone.
    inline constexpr size_t kNewThreadButtonLabelChars = 28;
    // The longest folder path the wizard creates. CreateDirectoryW without the long-path prefix stops at 248 characters, and
    // the tools a session runs (pwsh, git, claude's own) are not all long-path aware: a path past this is refused with a reason
    // instead of a folder nothing can work in.
    inline constexpr size_t kNewThreadCreatePathMaxChars = 240;
    // How long the window step waits for the windows' census (Engine.h CollectWindowCensus — every window answers from its UI
    // thread). The AI search's tab tools wait the same; a window busier than this is left out of the list.
    inline constexpr uint32_t kNewThreadCensusTimeoutMs = 4000;
    // How long the poll thread waits for a folder's creation (CreateDirectoryW, on a worker of its own): a network share can
    // take a while; past it the user is told, and the creation may still land.
    inline constexpr uint32_t kNewThreadCreateWaitMs = 5000;

    // ===== the steps =================================================================================================

    /// <summary>
    /// Where a new thread's wizard stands (MESSAGING.md §21.25). Persisted as its number (NewThreadWizardToJson), so the values
    /// never change meaning: a new step gets a new number.
    /// </summary>
    enum class NewThreadStep : int
    {
        Folder = 0, // the §21.17 folder picker (Recent / Most used, a typed path)
        CreateFolder = 1, // a typed folder that does not exist: Create it, or Back to the lists
        Model = 2, // the optional model choice: Default, the menu's models and presets, ✏ Specify…
        ModelSpecify = 3, // waiting for a TYPED model id (after ✏ Specify…)
        Window = 4, // the window choice by number, with each window's stats and the default
    };

    /// <summary>The step's word for the logs ("folder", "create-folder", "model", "model-id", "window").</summary>
    /// <param name="step">The step.</param>
    /// <returns>A static string; "folder" for a value the switch does not know.</returns>
    inline const wchar_t* NewThreadStepName(NewThreadStep step) noexcept
    {
        switch (step)
        {
        case NewThreadStep::CreateFolder:
            return L"create-folder";
        case NewThreadStep::Model:
            return L"model";
        case NewThreadStep::ModelSpecify:
            return L"model-id";
        case NewThreadStep::Window:
            return L"window";
        case NewThreadStep::Folder:
        default:
            return L"folder";
        }
    }

    /// <summary>
    /// A persisted step number back to its step. A number this build does not know (a state file of a newer build) reads as
    /// Folder: the wizard then starts over at the folder choice, which loses a choice but never acts on one it cannot read.
    /// </summary>
    /// <param name="value">The persisted number.</param>
    /// <returns>The step.</returns>
    inline NewThreadStep NewThreadStepFromNumber(int64_t value) noexcept
    {
        switch (value)
        {
        case 1:
            return NewThreadStep::CreateFolder;
        case 2:
            return NewThreadStep::Model;
        case 3:
            return NewThreadStep::ModelSpecify;
        case 4:
            return NewThreadStep::Window;
        default:
            return NewThreadStep::Folder;
        }
    }

    // ===== the folder: case-insensitive resolution and the "create it?" check ========================================

    /// <summary>One entry of a folder, as the injected file system (FolderFs) reports it.</summary>
    struct FolderStat
    {
        bool exists{ false }; // something by that name is there — a folder or a file
        bool directory{ false }; // it is a folder
        std::wstring name; // its name as the DISK spells it (the real casing); "" = the name as it was asked
    };

    /// <summary>
    /// The file system ResolveNewThreadFolder walks. Injected, so the rule is tested without a disk; the service passes the real
    /// one (FindFirstFileExW, run on a worker of its own and waited for with a bound — an offline share holds a call for the
    /// network's timeout).
    /// </summary>
    struct FolderFs
    {
        // Whether a ROOT exists: a drive ("K:\") or a share ("\\server\share\"). Empty = no root exists.
        std::function<bool(const std::wstring& root)> rootExists;
        // One path's entry, looked up by its name: on Windows the lookup is case-insensitive, so it finds "K:\SOURCE" as
        // "source" and reports that spelling. Empty = nothing exists.
        std::function<FolderStat(const std::wstring& path)> stat;
        // Every entry of a folder (nullopt = it cannot be listed): what the walk looks through when a lookup missed, for a
        // folder whose lookups are case-SENSITIVE (WSL can mark a folder so). Empty = never listed.
        std::function<std::optional<std::vector<FolderStat>>(const std::wstring& folder)> list;
    };

    /// <summary>What ResolveNewThreadFolder found.</summary>
    enum class FolderResolveKind
    {
        Exists, // the folder exists: `path` is its disk spelling
        Missing, // the folder does not exist and CAN be made: `existingPrefix` exists, `missing` are the folders to make
        IsFile, // a part of the path is a FILE: `path` names it
        NoRoot, // the drive or the share is not there
        Invalid, // not a folder path the wizard takes, or one Windows cannot make (`reason` says which)
        NoAnswer, // the check did not answer within its wait (set by the service: the pure walk never answers this)
    };

    /// <summary>The answer of ResolveNewThreadFolder (and of the service's bounded check around it).</summary>
    struct FolderResolution
    {
        FolderResolveKind kind{ FolderResolveKind::Invalid };
        std::wstring path; // Exists: the folder in its disk spelling; Missing: the folder that would be made; IsFile: the file
        std::wstring existingPrefix; // Missing: the deepest part that exists, in its disk spelling
        std::vector<std::wstring> missing; // Missing: the folders to make, the outermost first
        std::wstring reason; // IsFile / NoRoot / Invalid / NoAnswer: why, in plain words (one sentence, ending in a period)
    };

    /// <summary>A folder path split into its root and its parts.</summary>
    struct FolderPathParts
    {
        std::wstring root; // "K:\" (the letter upper case) or "\\server\share\"
        std::vector<std::wstring> components; // the names under the root, outermost first ("." left out)
        bool unc{ false }; // the root is a share
        bool parentRef{ false }; // a ".." part was given — the wizard refuses such a path (it would name a folder the user never wrote)
    };

    /// <summary>
    /// Joins a folder and one name with exactly one separator ("K:\" + "x" = "K:\x"; "K:\a" + "x" = "K:\a\x").
    /// </summary>
    /// <param name="folder">The folder (a root ends with its separator).</param>
    /// <param name="name">The name to append.</param>
    /// <returns>The joined path.</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    inline std::wstring JoinFolderPath(std::wstring_view folder, std::wstring_view name)
    {
        std::wstring out{ folder };
        if (!out.empty() && out.back() != L'\\')
        {
            out += L'\\';
        }
        out += name;
        return out;
    }

    /// <summary>
    /// A typed folder path split into its root and parts (TrimFolderPath first: quotes and blanks off, '/' as '\'). Doubled
    /// separators are one; "." parts are dropped; a ".." part is FLAGGED (FolderPathParts::parentRef) for the caller to refuse.
    /// The parts are not checked here: a name Windows cannot have is the caller's (NewThreadFolderNameIllegal).
    /// </summary>
    /// <param name="display">The path as typed.</param>
    /// <returns>The parts; nullopt when the text is not an absolute folder path (IsAbsoluteFolderPath).</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be allocated.</exception>
    inline std::optional<FolderPathParts> SplitFolderPath(std::wstring_view display)
    {
        const std::wstring path = TrimFolderPath(display);
        if (!IsAbsoluteFolderPath(path))
        {
            return std::nullopt;
        }
        FolderPathParts parts;
        size_t pos = 0;
        if (path[0] == L'\\')
        {
            // "\\server\share[\...]": IsAbsoluteFolderPath guarantees a server, a separator and a share name.
            parts.unc = true;
            const size_t serverEnd = path.find(L'\\', 2);
            size_t shareEnd = path.find(L'\\', serverEnd + 1);
            if (shareEnd == std::wstring::npos)
            {
                shareEnd = path.size();
            }
            parts.root = path.substr(0, shareEnd) + L"\\";
            pos = shareEnd;
        }
        else
        {
            // "K:\...": the drive letter upper case, so "k:\x" and "K:\x" name one root in every message.
            parts.root = std::wstring(1, static_cast<wchar_t>(std::towupper(static_cast<wint_t>(path[0])))) + L":\\";
            pos = 3;
        }
        while (pos < path.size())
        {
            while (pos < path.size() && path[pos] == L'\\')
            {
                ++pos;
            }
            if (pos >= path.size())
            {
                break;
            }
            size_t end = path.find(L'\\', pos);
            if (end == std::wstring::npos)
            {
                end = path.size();
            }
            std::wstring comp = path.substr(pos, end - pos);
            pos = end;
            if (comp == L".")
            {
                continue;
            }
            if (comp == L"..")
            {
                parts.parentRef = true;
                continue;
            }
            parts.components.push_back(std::move(comp));
        }
        return parts;
    }

    /// <summary>
    /// Why a NAME cannot be a folder on Windows at all, in plain words; "" when it can be one. Checked for every part of a typed
    /// path BEFORE any lookup: none of &lt; &gt; : " / \ | ? * and no control character (and &lt; &gt; " are wildcards to
    /// FindFirstFileExW, so a lookup of such a name could find ANOTHER file), not a reserved device name (CON, PRN, AUX, NUL,
    /// COM0–COM9, LPT0–LPT9, with or without an extension: a lookup would reach the device), at most 255 characters. A trailing
    /// dot or space is NOT refused here — a lookup drops it (K:\x. finds K:\x); only a creation cannot keep it
    /// (NewThreadFolderNameProblem).
    /// </summary>
    /// <param name="name">One folder name (no separator).</param>
    /// <returns>The reason (a clause, no period), or "" for a possible name.</returns>
    /// <exception cref="std::bad_alloc">If the reason cannot be allocated.</exception>
    inline std::wstring NewThreadFolderNameIllegal(std::wstring_view name)
    {
        if (name.empty())
        {
            return L"a folder name is empty";
        }
        if (name.size() > 255)
        {
            return L"a folder name is longer than 255 characters";
        }
        for (const wchar_t c : name)
        {
            if (c < 0x20 || std::wstring_view{ L"<>:\"/\\|?*" }.find(c) != std::wstring_view::npos)
            {
                return L"\x201C" + std::wstring{ name } + L"\x201D holds a character Windows does not allow in a folder name (< > : \" / \\ | ? * or a control character)";
            }
        }
        // The device names: the part before the first dot, trailing blanks off, compared upper case ("con.txt" is still CON).
        std::wstring stem{ name.substr(0, name.find(L'.')) };
        while (!stem.empty() && stem.back() == L' ')
        {
            stem.pop_back();
        }
        for (auto& c : stem)
        {
            c = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
        }
        const bool numbered = stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'0' && stem[3] <= L'9';
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || numbered)
        {
            return L"\x201C" + std::wstring{ name } + L"\x201D is a device name Windows reserves";
        }
        return {};
    }

    /// <summary>
    /// Why Windows would refuse to MAKE a folder of this name, in plain words; "" when it can: every rule of
    /// NewThreadFolderNameIllegal, and no trailing dot or space (Windows drops them, so the folder made would not be the one
    /// named — a later lookup of the typed spelling would still find it, the session's own path would not match it).
    /// </summary>
    /// <param name="name">One folder name (no separator).</param>
    /// <returns>The reason (a clause, no period), or "" for a name Windows makes as written.</returns>
    /// <exception cref="std::bad_alloc">If the reason cannot be allocated.</exception>
    inline std::wstring NewThreadFolderNameProblem(std::wstring_view name)
    {
        if (std::wstring illegal = NewThreadFolderNameIllegal(name); !illegal.empty())
        {
            return illegal;
        }
        if (name.back() == L'.' || name.back() == L' ')
        {
            return L"\x201C" + std::wstring{ name } + L"\x201D ends with a dot or a space, which Windows drops from a folder name";
        }
        return {};
    }

    /// <summary>
    /// Whether two folder NAMES are one name on Windows (case-insensitive, the app's own folder identity: NormDirKey).
    /// </summary>
    /// <param name="a">A name.</param>
    /// <param name="b">Another name.</param>
    /// <returns>True when they differ at most in case.</returns>
    /// <exception cref="std::bad_alloc">If the keys cannot be allocated.</exception>
    inline bool NewThreadFolderNamesEqual(std::wstring_view a, std::wstring_view b)
    {
        return NormDirKey(std::wstring{ a }) == NormDirKey(std::wstring{ b });
    }

    /// <summary>
    /// What a TYPED folder is on this PC, checked CASE-INSENSITIVELY (the user's "checking case-insensitively as we on win"):
    /// the walk goes down the path one part at a time; each part is looked up (a lookup on Windows ignores case and reports the
    /// disk's spelling), and when a lookup misses, the parent is LISTED and searched case-insensitively — a folder WSL marked
    /// case-sensitive answers only its exact spelling, and "k:\Source\agentmaster" must still find "K:\source\Agentmaster"
    /// instead of offering to make a second folder that differs only in case. The first part that is not there, and everything
    /// under it, is what a Create would make — each name checked for what Windows refuses (NewThreadFolderNameProblem).
    /// </summary>
    /// <param name="display">The folder as the user typed it.</param>
    /// <param name="fs">The file system to walk.</param>
    /// <returns>Exists (the disk's spelling), Missing (what exists and what a Create makes), IsFile, NoRoot or Invalid — never
    /// NoAnswer (that is the service's, for a walk that did not finish in time).</returns>
    /// <exception cref="std::bad_alloc">If the strings cannot be allocated.</exception>
    /// <remarks>Every result's path is built from the disk's spellings for the parts that exist and the user's for the parts a
    /// Create makes, so the folder a session opens in carries the casing the disk already has.</remarks>
    inline FolderResolution ResolveNewThreadFolder(std::wstring_view display, const FolderFs& fs)
    {
        FolderResolution res;
        const auto parts = SplitFolderPath(display);
        if (!parts)
        {
            res.kind = FolderResolveKind::Invalid;
            res.reason = L"That is not a folder\x2019s full path (like K:\\source\\project, or \\\\server\\share\\folder).";
            return res;
        }
        if (parts->parentRef)
        {
            res.kind = FolderResolveKind::Invalid;
            res.reason = L"A path with \x201C..\x201D is not taken: send the folder\x2019s full path.";
            return res;
        }
        // Every part checked BEFORE any lookup: a character no Windows name takes would be a wildcard to the lookup (< > " * ?)
        // and could find another entry, and a device name would reach the device.
        for (const auto& comp : parts->components)
        {
            if (const std::wstring illegal = NewThreadFolderNameIllegal(comp); !illegal.empty())
            {
                res.kind = FolderResolveKind::Invalid;
                res.reason = L"That is not a folder path Windows takes: " + illegal + L".";
                return res;
            }
        }
        if (!fs.rootExists || !fs.rootExists(parts->root))
        {
            res.kind = FolderResolveKind::NoRoot;
            res.path = parts->root;
            res.reason = parts->unc ? L"The share " + parts->root.substr(0, parts->root.size() - 1) + L" is not reachable from the PC." :
                                      L"The drive " + parts->root.substr(0, 2) + L" is not available on the PC (no such drive, or no disk in it).";
            return res;
        }
        std::wstring current = parts->root;
        for (size_t i = 0; i < parts->components.size(); ++i)
        {
            const std::wstring& comp = parts->components[i];
            FolderStat st = fs.stat ? fs.stat(JoinFolderPath(current, comp)) : FolderStat{};
            if (!st.exists && fs.list)
            {
                // The lookup missed: a case-sensitive folder answers only its exact spelling. Look through the parent: the exact
                // spelling wins over every case variant (in a case-sensitive folder "Foo" and "foo" can both be there); among
                // the case variants a folder wins over a file (which would block the path).
                if (const auto entries = fs.list(current))
                {
                    const FolderStat* exact = nullptr;
                    const FolderStat* folderMatch = nullptr;
                    const FolderStat* fileMatch = nullptr;
                    for (const auto& e : *entries)
                    {
                        if (e.name == comp)
                        {
                            exact = &e;
                            break;
                        }
                        if (NewThreadFolderNamesEqual(e.name, comp))
                        {
                            if (e.directory && !folderMatch)
                            {
                                folderMatch = &e;
                            }
                            else if (!e.directory && !fileMatch)
                            {
                                fileMatch = &e;
                            }
                        }
                    }
                    if (const FolderStat* found = exact ? exact : (folderMatch ? folderMatch : fileMatch))
                    {
                        st = *found;
                        st.exists = true;
                    }
                }
            }
            if (st.exists)
            {
                const std::wstring here = JoinFolderPath(current, st.name.empty() ? comp : st.name);
                if (!st.directory)
                {
                    res.kind = FolderResolveKind::IsFile;
                    res.path = here;
                    res.reason = L"That is a file, not a folder: " + here + L".";
                    return res;
                }
                current = here;
                continue;
            }
            // Missing from here on: this part and everything under it is what a Create makes — if Windows can make every name.
            res.existingPrefix = current;
            std::wstring made = current;
            for (size_t j = i; j < parts->components.size(); ++j)
            {
                if (const std::wstring problem = NewThreadFolderNameProblem(parts->components[j]); !problem.empty())
                {
                    res.kind = FolderResolveKind::Invalid;
                    res.missing.clear();
                    res.reason = L"The folder cannot be created: " + problem + L".";
                    return res;
                }
                made = JoinFolderPath(made, parts->components[j]);
                res.missing.push_back(parts->components[j]);
            }
            if (made.size() > kNewThreadCreatePathMaxChars)
            {
                res.kind = FolderResolveKind::Invalid;
                res.missing.clear();
                res.reason = L"The folder cannot be created: its path is longer than " + std::to_wstring(kNewThreadCreatePathMaxChars) +
                             L" characters, which the tools a session runs do not all take.";
                return res;
            }
            res.kind = FolderResolveKind::Missing;
            res.path = made;
            return res;
        }
        res.kind = FolderResolveKind::Exists;
        res.path = current;
        return res;
    }

    // ===== the model =================================================================================================

    /// <summary>One choice of the model step: what its button shows and what the launch is given.</summary>
    struct NewThreadModelChoice
    {
        std::wstring label; // the button's text: the menu's display name, or the id itself for the last typed model
        std::wstring target; // the launch target: a model id, or "@<presetId>" for a preset (LaunchPresets.h PresetToken)
        bool preset{ false }; // a preset (an "@" target): shown after the models, with its own glyph
        bool last{ false }; // the model last typed in a "Specify a model" prompt (model-list.json `last`), offered first
    };

    /// <summary>Case-insensitive equality of two launch targets (a model id is matched by its spelling, not its case).</summary>
    /// <param name="a">A target.</param>
    /// <param name="b">Another target.</param>
    /// <returns>True when they differ at most in case.</returns>
    inline bool NewThreadTargetsEqual(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::towupper(static_cast<wint_t>(a[i])) != std::towupper(static_cast<wint_t>(b[i])))
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// The model step's choices, the right-click menu's own list (LaunchMenuEntries: the configured launch models, then every
    /// visible preset as "@id") with the model last typed in a "Specify a model" prompt FIRST when the list does not have it —
    /// the PC's prompt opens pre-filled with that model, and here it is one tap. A target listed twice is shown once (the first
    /// spelling); an entry with an empty target is left out; at most kNewThreadModelChoicesMax.
    /// </summary>
    /// <param name="entries">{display name, launch target} pairs, in the menu's order.</param>
    /// <param name="lastSpecified">The model last committed in a "Specify a model" prompt ("" = none).</param>
    /// <returns>The choices, in the order the step shows them.</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
    inline std::vector<NewThreadModelChoice> BuildNewThreadModelChoices(const std::vector<std::pair<std::wstring, std::wstring>>& entries, std::wstring_view lastSpecified)
    {
        std::vector<NewThreadModelChoice> out;
        const auto listed = [&](std::wstring_view target) {
            return std::any_of(out.begin(), out.end(), [&](const NewThreadModelChoice& c) { return NewThreadTargetsEqual(c.target, target); });
        };
        const bool lastInMenu = !lastSpecified.empty() &&
                                std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return NewThreadTargetsEqual(e.second, lastSpecified); });
        if (!lastSpecified.empty() && !lastInMenu)
        {
            NewThreadModelChoice last;
            last.label = std::wstring{ lastSpecified };
            last.target = std::wstring{ lastSpecified };
            last.last = true;
            out.push_back(std::move(last));
        }
        for (const auto& [label, target] : entries)
        {
            if (out.size() >= kNewThreadModelChoicesMax)
            {
                break;
            }
            if (target.empty() || listed(target))
            {
                continue;
            }
            NewThreadModelChoice c;
            c.label = label.empty() ? target : label;
            c.target = target;
            c.preset = target.front() == L'@';
            out.push_back(std::move(c));
        }
        return out;
    }

    /// <summary>
    /// A model id the user TYPED, checked before it can reach a launch: the id goes into claude's command line, which the pwsh
    /// host runs as PowerShell source, so only the characters a model id uses are taken — letters, digits and - _ . : / [ ] —
    /// starting with a letter or a digit, one word, at most kNewThreadModelIdMaxChars. "claude-opus-5-5[1m]", "opus",
    /// "sonnet[1m]", "anthropic/claude-sonnet-5.5" and "qwen3:32b" pass; a space, a quote, ';', '$', '`', '@', a leading '-'
    /// (claude would read it as a flag) do not.
    /// </summary>
    /// <param name="text">The text as typed (surrounding blanks are trimmed).</param>
    /// <param name="reason">Receives why the text is not a model id (a sentence, ending in a period); untouched on success.
    /// May be null.</param>
    /// <returns>The id, trimmed; nullopt when the text is not one.</returns>
    /// <exception cref="std::bad_alloc">If the id or the reason cannot be allocated.</exception>
    inline std::optional<std::wstring> ParseNewThreadModelId(std::wstring_view text, std::wstring* reason)
    {
        const auto fail = [&](std::wstring why) -> std::optional<std::wstring> {
            if (reason)
            {
                *reason = std::move(why);
            }
            return std::nullopt;
        };
        const size_t a = text.find_first_not_of(L" \t\r\n");
        if (a == std::wstring_view::npos)
        {
            return fail(L"Send a model id, like claude-opus-5-5[1m].");
        }
        const size_t b = text.find_last_not_of(L" \t\r\n");
        const std::wstring_view id = text.substr(a, b - a + 1);
        if (id.find_first_of(L" \t\r\n") != std::wstring_view::npos)
        {
            return fail(L"A model id is one word, with no spaces (like claude-opus-5-5[1m]).");
        }
        if (id.size() > kNewThreadModelIdMaxChars)
        {
            return fail(L"That is longer than " + std::to_wstring(kNewThreadModelIdMaxChars) + L" characters; a model id is shorter.");
        }
        const auto isAlnum = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'); };
        if (!isAlnum(id.front()))
        {
            return fail(L"A model id starts with a letter or a digit (like claude-opus-5-5[1m]).");
        }
        for (const wchar_t c : id)
        {
            if (!isAlnum(c) && std::wstring_view{ L"-_.:/[]" }.find(c) == std::wstring_view::npos)
            {
                return fail(L"A model id holds letters, digits and - _ . : / [ ] only (like claude-opus-5-5[1m]).");
            }
        }
        return std::wstring{ id };
    }

    /// <summary>
    /// The argument of a "/model …" message typed at the model step ("/model claude-opus-5-5[1m]", "/model@bot opus") — the
    /// one-message form of ✏ Specify… for a Claude Code user. Matched case-insensitively; "/models" or "/modelx" is not it.
    /// </summary>
    /// <param name="text">The message's text.</param>
    /// <returns>What follows "/model", trimmed ("" for a bare "/model"); nullopt when the text is not a /model command.</returns>
    /// <exception cref="std::bad_alloc">If the argument cannot be allocated.</exception>
    inline std::optional<std::wstring> NewThreadModelCommandArg(std::wstring_view text)
    {
        const size_t a = text.find_first_not_of(L" \t\r\n");
        if (a == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        std::wstring_view t = text.substr(a);
        constexpr std::wstring_view kCmd = L"/model";
        if (t.size() < kCmd.size() || !NewThreadTargetsEqual(t.substr(0, kCmd.size()), kCmd))
        {
            return std::nullopt;
        }
        t.remove_prefix(kCmd.size());
        if (!t.empty() && t.front() == L'@')
        {
            // "/model@SomeBot …": a command addressed to this bot; the bot's name runs to the first blank.
            const size_t end = t.find_first_of(L" \t\r\n");
            t = end == std::wstring_view::npos ? std::wstring_view{} : t.substr(end);
        }
        else if (!t.empty() && t.front() != L' ' && t.front() != L'\t' && t.front() != L'\r' && t.front() != L'\n')
        {
            return std::nullopt; // "/models", "/modelfoo": another word
        }
        const size_t s = t.find_first_not_of(L" \t\r\n");
        if (s == std::wstring_view::npos)
        {
            return std::wstring{};
        }
        const size_t e = t.find_last_not_of(L" \t\r\n");
        return std::wstring{ t.substr(s, e - s + 1) };
    }

    // ===== the window =================================================================================================

    /// <summary>
    /// What the window step needs to know about one tab's session, read from the registry by the service (the census reports
    /// only each window's strip): the folders it works in and how new it is.
    /// </summary>
    struct NewThreadTabFacts
    {
        std::wstring dirKey; // NormDirKey of the session's launch folder
        std::wstring worksInKey; // NormDirKey of its INFERRED folder ("" = not inferred, or the launch folder)
        int64_t createdMs{ 0 }; // SessionInfo::createdUnixMs — when the session was created (0 = before the stamp existed)
        int64_t recentMs{ 0 }; // MostRecentSortKeyMs — its newest turn, else its last activity (the side panel's MOST RECENT key)
    };

    /// <summary>One window as the step shows it, numbered in creation order (the AI search's tab tools number them alike).</summary>
    struct NewThreadWindowChoice
    {
        std::wstring windowId; // the window's record id — what the launch targets (RemoteLaunchRequest::windowId)
        uint32_t number{ 0 }; // 1-based, the census order
        bool lastActive{ false }; // the window the user activated last
        uint32_t tabCount{ 0 }; // every tab of its strip, shells included
        std::wstring newestTitle; // the user's "last tab created": the newest tab's title ("" = no tab)
        int64_t newestCreatedMs{ 0 }; // when it was created (0 = not known: a shell tab, a session from before the stamp)
        std::vector<std::wstring> titles; // the newest titles first, at most kNewThreadWindowTitles, each on one line and cut
        uint32_t folderTabs{ 0 }; // how many of its tabs work in the chosen folder
        std::wstring anchorSessionId; // the NEWEST tab of the chosen folder in this window: the new tab opens right after it
        std::wstring anchorTitle;
        int64_t anchorCreatedMs{ 0 };
        int64_t anchorRecentMs{ 0 };
    };

    /// <summary>The window step's list and its default.</summary>
    struct NewThreadWindowPlan
    {
        std::vector<NewThreadWindowChoice> windows; // census order, at most kNewThreadWindowsMax
        size_t leftOut{ 0 }; // windows past kNewThreadWindowsMax
        int defaultIndex{ -1 }; // into `windows`; -1 = none (no window)
        std::wstring defaultReason; // why that one (plain text, one clause)
    };

    namespace detail
    {
        /// <summary>
        /// Whether tab A is NEWER than tab B: a later creation stamp first (a stamp beats no stamp), then a later MOST RECENT key
        /// (a session from before the stamp is ranked by its newest turn, which a just-used tab has), then the later strip place.
        /// </summary>
        inline bool NewThreadTabNewer(int64_t aCreated, int64_t aRecent, uint32_t aIndex, int64_t bCreated, int64_t bRecent, uint32_t bIndex) noexcept
        {
            if (aCreated != bCreated)
            {
                return aCreated > bCreated;
            }
            if (aRecent != bRecent)
            {
                return aRecent > bRecent;
            }
            return aIndex > bIndex;
        }

        /// <summary>
        /// A title as the window stats print it: on one line, cut at kNewThreadWindowTitleChars ("(untitled)" when blank) — by
        /// TgTitleLine, the rule of every title the bot prints (a cut never splits a surrogate pair, which Telegram refuses).
        /// </summary>
        inline std::wstring NewThreadStatTitle(std::wstring_view title)
        {
            std::wstring one = TgTitleLine(title, kNewThreadWindowTitleChars);
            return one.empty() ? std::wstring{ L"(untitled)" } : one;
        }
    }

    /// <summary>
    /// The window step's choices from the windows' census and the registry's facts: per window its tab count, its newest tab
    /// (the user's "last tab created"), its ten newest titles, and its newest tab working in the chosen folder. The DEFAULT is
    /// the window holding the newest such tab over all windows — "finding all tabs with same (inferred) working directory and
    /// picking the tab with newest tab with that (inferred) working directory" — a tab counting when its launch folder OR its
    /// inferred folder is the chosen one. With no such tab the default is the window the user activated last, else the first.
    /// </summary>
    /// <param name="census">The windows' strips, in creation order (Engine.h CollectWindowCensus).</param>
    /// <param name="facts">Session id -> its facts (the registry's live sessions); a tab with no entry (a shell, a session mid-bind)
    /// counts as a tab with no folder and no creation time.</param>
    /// <param name="folderKey">NormDirKey of the chosen folder.</param>
    /// <param name="folderLeaf">The chosen folder's last name, for the reason text ("Agentmaster").</param>
    /// <returns>The list and its default.</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
    inline NewThreadWindowPlan BuildNewThreadWindowPlan(const std::vector<WindowCensus>& census, const std::unordered_map<std::wstring, NewThreadTabFacts>& facts, std::wstring_view folderKey, std::wstring_view folderLeaf)
    {
        NewThreadWindowPlan plan;
        for (const auto& w : census)
        {
            if (plan.windows.size() >= kNewThreadWindowsMax)
            {
                ++plan.leftOut;
                continue;
            }
            NewThreadWindowChoice choice;
            choice.windowId = w.windowId;
            choice.number = static_cast<uint32_t>(plan.windows.size() + 1);
            choice.lastActive = w.lastActive;
            choice.tabCount = static_cast<uint32_t>(w.tabs.size());
            // Every tab with its newness, sorted newest first: the newest one and the ten titles come from the same order.
            struct Ranked
            {
                const WindowTabInfo* tab;
                int64_t created;
                int64_t recent;
                bool folder;
            };
            std::vector<Ranked> ranked;
            ranked.reserve(w.tabs.size());
            for (const auto& t : w.tabs)
            {
                Ranked r{ &t, 0, 0, false };
                if (!t.sessionId.empty())
                {
                    if (const auto it = facts.find(t.sessionId); it != facts.end())
                    {
                        r.created = it->second.createdMs;
                        r.recent = it->second.recentMs;
                        r.folder = !folderKey.empty() && (it->second.dirKey == folderKey || (!it->second.worksInKey.empty() && it->second.worksInKey == folderKey));
                    }
                }
                ranked.push_back(r);
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
                return detail::NewThreadTabNewer(a.created, a.recent, a.tab->index, b.created, b.recent, b.tab->index);
            });
            if (!ranked.empty())
            {
                choice.newestTitle = detail::NewThreadStatTitle(ranked.front().tab->title);
                choice.newestCreatedMs = ranked.front().created;
            }
            for (const auto& r : ranked)
            {
                if (choice.titles.size() < kNewThreadWindowTitles)
                {
                    choice.titles.push_back(detail::NewThreadStatTitle(r.tab->title));
                }
                if (r.folder)
                {
                    ++choice.folderTabs;
                    if (choice.anchorSessionId.empty())
                    {
                        // The list is newest first, so the first tab of the folder is its newest one in this window.
                        choice.anchorSessionId = r.tab->sessionId;
                        choice.anchorTitle = detail::NewThreadStatTitle(r.tab->title);
                        choice.anchorCreatedMs = r.created;
                        choice.anchorRecentMs = r.recent;
                    }
                }
            }
            plan.windows.push_back(std::move(choice));
        }
        // The default: the newest folder tab over every window (a tie goes to the window used last, then the earlier window).
        int best = -1;
        for (size_t i = 0; i < plan.windows.size(); ++i)
        {
            const auto& c = plan.windows[i];
            if (c.anchorSessionId.empty())
            {
                continue;
            }
            if (best < 0)
            {
                best = static_cast<int>(i);
                continue;
            }
            const auto& b = plan.windows[static_cast<size_t>(best)];
            const bool newer = c.anchorCreatedMs != b.anchorCreatedMs ? c.anchorCreatedMs > b.anchorCreatedMs :
                               c.anchorRecentMs != b.anchorRecentMs   ? c.anchorRecentMs > b.anchorRecentMs :
                                                                        (c.lastActive && !b.lastActive);
            if (newer)
            {
                best = static_cast<int>(i);
            }
        }
        const std::wstring leaf = folderLeaf.empty() ? std::wstring{ L"the folder" } : std::wstring{ folderLeaf };
        if (best >= 0)
        {
            plan.defaultIndex = best;
            plan.defaultReason = L"next to \x201C" + plan.windows[static_cast<size_t>(best)].anchorTitle + L"\x201D, the newest tab in " + leaf;
            return plan;
        }
        for (size_t i = 0; i < plan.windows.size(); ++i)
        {
            if (plan.windows[i].lastActive)
            {
                plan.defaultIndex = static_cast<int>(i);
                plan.defaultReason = L"the window you used last \x2014 no tab works in " + leaf + L" yet";
                return plan;
            }
        }
        if (!plan.windows.empty())
        {
            plan.defaultIndex = 0;
            plan.defaultReason = L"no tab works in " + leaf + L" yet";
        }
        return plan;
    }

    /// <summary>
    /// A window NUMBER the user typed at the window step: "2", "#2", "w2", "window 2" (any case), within 1..count.
    /// </summary>
    /// <param name="text">The message's text.</param>
    /// <param name="count">How many windows the step lists.</param>
    /// <returns>The window's 0-based index; nullopt for any other text or a number out of range.</returns>
    inline std::optional<size_t> ParseNewThreadWindowNumber(std::wstring_view text, size_t count) noexcept
    {
        const size_t a = text.find_first_not_of(L" \t\r\n");
        if (a == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        const size_t b = text.find_last_not_of(L" \t\r\n");
        std::wstring_view t = text.substr(a, b - a + 1);
        const auto stripPrefix = [&](std::wstring_view prefix) {
            if (t.size() > prefix.size() && NewThreadTargetsEqual(t.substr(0, prefix.size()), prefix))
            {
                t.remove_prefix(prefix.size());
                while (!t.empty() && t.front() == L' ')
                {
                    t.remove_prefix(1);
                }
                return true;
            }
            return false;
        };
        if (!stripPrefix(L"window") && !stripPrefix(L"#"))
        {
            stripPrefix(L"w");
        }
        if (t.empty() || t.size() > 3)
        {
            return std::nullopt;
        }
        size_t n = 0;
        for (const wchar_t c : t)
        {
            if (c < L'0' || c > L'9')
            {
                return std::nullopt;
            }
            n = n * 10 + static_cast<size_t>(c - L'0');
        }
        if (n < 1 || n > count)
        {
            return std::nullopt;
        }
        return n - 1;
    }

    // ===== the records ================================================================================================

    /// <summary>
    /// The wizard's answers so far and what each step showed (MESSAGING.md §21.25). Persisted with its thread's record
    /// (NewThreadWizardToJson), so a restart resumes the wizard where it stood and a tap on a button that was on the screen before
    /// the restart still maps to what that button showed.
    /// </summary>
    struct NewThreadWizard
    {
        NewThreadStep step{ NewThreadStep::Folder };
        // One line at the top of the step (a folder that is gone, a creation Windows refused, a launch that did not go through).
        // Kept until the user's next choice clears it (every reducer below that accepts a button or a typed choice does): a
        // render alone never clears it, since a second render can replace the first in the outbox before it is sent (the
        // picker's edits share one key), and the notice would then never be seen.
        std::wstring notice;
        // The folder, chosen at the Folder step or made at the CreateFolder step: its disk spelling.
        std::wstring folder;
        bool folderCreated{ false }; // the wizard made it (the launch line says so)
        // CreateFolder: what a Create makes.
        std::wstring createPath;
        std::wstring createExistingPrefix;
        std::vector<std::wstring> createMissing;
        // Model: the choices the step SHOWED (a tap maps to them, not to a menu that changed since), Default's description, the
        // choice made ("" target = Default).
        std::vector<NewThreadModelChoice> modelChoices;
        bool offerAddPresets{ false }; // the user has no preset yet: the step says where to add one, as the PC's menus do
        std::wstring settingsModel; // the settings model Default stands for ("" = Claude Code's own default)
        bool modelChosen{ false };
        std::wstring modelTarget;
        std::wstring modelLabel;
        // Window: the census as the step showed it, its default and the choice made.
        bool windowsCollected{ false }; // the list below is the step's (false = "looking at the windows…")
        uint64_t windowsSerial{ 0 }; // NOT persisted: the census in flight — an answer of an older ask is stale
        std::vector<NewThreadWindowChoice> windows;
        size_t windowsLeftOut{ 0 };
        int defaultWindow{ -1 };
        std::wstring defaultReason;
        bool windowChosen{ false };
        bool windowAuto{ false }; // the step was skipped: one window, or none answered
        // The step must SHOW its list even with one window (or say that none answered) instead of skipping it: set when a launch
        // that did not go through brings the wizard back here (ReturnNewThreadToStep), so a refused launch is never retried
        // without a tap — with the step skipped, the census of one window would launch again at once, and a refusal that holds
        // (the window keeps refusing) would loop. A new model choice clears it (a fresh pass through the steps may skip again).
        bool askWindow{ false };
        std::wstring windowId; // the window the tab opens in ("" = the engine's own choice: the window used last)
        uint32_t windowNumber{ 0 }; // as the step showed it (0 = none)
        std::wstring anchorSessionId; // the tab the new one opens right after ("" = the strip's end)
        std::wstring anchorTitle;
    };

    /// <summary>
    /// A thread the user opened that no session owns yet, and its PICKER — the one message the wizard edits (§21.17, §21.25).
    /// Moved here from MessagingService (where `NewThread` names it) so its persisted form is pure and tested.
    /// </summary>
    struct NewThreadRecord
    {
        std::wstring topicName; // the name the user gave the thread ("" = Telegram named it, or it is not known)
        NewThreadLists lists; // the folder lists, built on first show (listsBuilt); persisted, so a tap maps to what it showed
        bool listsBuilt{ false };
        size_t recentPage{ 0 };
        size_t usedPage{ 0 };
        int64_t pickerMessageId{ 0 }; // the picker (0 = not posted, or its post was refused; -1 = the post is in flight)
        bool pickerStale{ false }; // NOT persisted: a change came while the post flew; its answer renders again
        std::vector<json::Value> held; // the user's messages in the thread, oldest first (each a copy of the Message object)
        int64_t lastUseMs{ 0 }; // the last message or tap (WallMs): a wizard idle past kNewThreadPickerIdleMs is deleted with its thread
        NewThreadWizard wizard; // §21.25: the steps
    };

    /// <summary>
    /// A wizard's launch, waiting for its session to be live — the session id the launch was given, the thread it becomes the
    /// topic of, and the user's messages to hand over (§21.17.4). The tick binds it and keeps it as the thread's hand-over queue
    /// until the topic is recorded and every held message is handed over. Moved here from MessagingService (`ThreadBind` there).
    /// </summary>
    struct NewThreadBind
    {
        uint64_t launchToken{ 0 }; // NOT persisted: the key the launch's answer comes back with (§21.23); 0 after a restart
        bool answered{ false }; // NOT persisted: the window opened the tab
        int64_t replyTo{ 0 }; // the user's message that finished the wizard by typing (0 = a button): a refusal's reply quotes it
        std::wstring sessionId;
        int64_t threadId{ 0 };
        std::wstring dir;
        std::wstring title;
        std::vector<json::Value> held;
        int64_t requestedMs{ 0 }; // when the launch was asked (WallMs; a restart restarts it): kNewThreadBindTimeoutMs counts from it
        NewThreadWizard wizard; // §21.25: the choices it launched with — a refused launch goes back to them
    };

    // ===== the decisions ==============================================================================================

    /// <summary>
    /// What the service does next after a button, a typed message or the answer of a check (§21.25). The reducers below change
    /// the record and return one of these; the service carries it out (the I/O is the service's).
    /// </summary>
    struct NewThreadOutcome
    {
        enum class Kind
        {
            Stale, // the button belongs to another step: nothing changed; `text` is the toast
            Toast, // nothing to render; `text` is the toast (a pager label)
            Render, // the wizard changed: render its current step (edit the picker in place)
            CheckFolder, // check `path` (a list pick must exist; a typed folder may be created), then ApplyNewThreadFolderResolved
            CreateFolder, // make `path` (the CreateFolder step's Create), then ApplyNewThreadFolderCreated
            EnterModel, // the folder is chosen: load the model choices (EnterNewThreadModelStep), then render
            CollectWindows, // the model is chosen: collect the windows (asynchronous), then ApplyNewThreadWindows
            Launch, // every choice is made: launch (BuildNewThreadLaunchPlan)
            Abort, // ✖ Cancel: delete the thread (the user's rule: an aborted creation deletes its new topic)
            Hold, // the text is the user's message: hold it for the session (the picker counts it)
            Reply, // tell the user `text` and change nothing (an invalid model id)
        };
        Kind kind{ Kind::Stale };
        std::wstring path; // CheckFolder / CreateFolder: the folder
        bool typed{ false }; // CheckFolder: the folder was TYPED (a missing one may be created)
        std::wstring text; // Stale / Toast / Reply: what the user is told
        std::wstring rememberModel; // CollectWindows: a TYPED model id the service remembers (model-list.json), "" = none
    };

    /// <summary>Whether a button of this kind belongs to the step (a mismatch is a stale tap on an older keyboard).</summary>
    /// <param name="step">The wizard's step now.</param>
    /// <param name="kind">The button's action.</param>
    /// <returns>True when the step shows such a button.</returns>
    inline bool NewThreadStepTakes(NewThreadStep step, NewThreadAction::Kind kind) noexcept
    {
        using K = NewThreadAction::Kind;
        switch (kind)
        {
        case K::Cancel:
            return true; // every step has ✖ Cancel
        case K::PickRecent:
        case K::PickUsed:
        case K::PageRecent:
        case K::PageUsed:
        case K::PageLabelRecent:
        case K::PageLabelUsed:
            return step == NewThreadStep::Folder;
        case K::CreateFolder:
            return step == NewThreadStep::CreateFolder;
        case K::BackToFolders:
            return step == NewThreadStep::CreateFolder || step == NewThreadStep::Model || step == NewThreadStep::ModelSpecify;
        case K::ModelDefault:
            return step == NewThreadStep::Model || step == NewThreadStep::ModelSpecify;
        case K::ModelPick:
        case K::ModelSpecify:
            return step == NewThreadStep::Model;
        case K::BackToModels:
            return step == NewThreadStep::ModelSpecify || step == NewThreadStep::Window;
        case K::WindowPick:
        case K::WindowDefault:
        case K::WindowRefresh:
            return step == NewThreadStep::Window;
        case K::None:
        default:
            return false;
        }
    }

    /// <summary>The toast of a stale tap (a button of an earlier step, tapped before the edit reached the phone).</summary>
    inline constexpr const wchar_t* kNewThreadStaleTap = L"That button belongs to an earlier step \x2014 use the picker\x2019s current buttons.";

    /// <summary>Clears the model choice and everything after it (the folder changed, or the user went back to it).</summary>
    /// <param name="w">The wizard.</param>
    inline void ResetNewThreadAfterFolder(NewThreadWizard& w)
    {
        w.modelChosen = false;
        w.modelTarget.clear();
        w.modelLabel.clear();
        w.windowsCollected = false;
        w.windows.clear();
        w.windowsLeftOut = 0;
        w.defaultWindow = -1;
        w.defaultReason.clear();
        w.windowChosen = false;
        w.windowAuto = false;
        w.windowId.clear();
        w.windowNumber = 0;
        w.anchorSessionId.clear();
        w.anchorTitle.clear();
    }

    /// <summary>
    /// Records a model choice and moves to the window step (whose list the service collects next). A new model choice is a fresh
    /// pass through the window step: it may be skipped again (one window), so `askWindow` is cleared, and so is the notice.
    /// </summary>
    /// <param name="w">The wizard.</param>
    /// <param name="target">The launch target ("" = Default).</param>
    /// <param name="label">What the launch line shows for it.</param>
    inline void ChooseNewThreadModel(NewThreadWizard& w, std::wstring target, std::wstring label)
    {
        w.modelChosen = true;
        w.modelTarget = std::move(target);
        w.modelLabel = std::move(label);
        w.step = NewThreadStep::Window;
        w.windowsCollected = false;
        w.windowChosen = false;
        w.windowAuto = false;
        w.askWindow = false;
        w.notice.clear();
    }

    /// <summary>
    /// Records a window choice (an index of the step's list): its id, its number, and the tab to open right after. The choice ends
    /// the step's "must ask" (askWindow) and clears the notice: the launch that follows is the user's own.
    /// </summary>
    /// <param name="w">The wizard.</param>
    /// <param name="index">The window's index in w.windows (must be in range; the callers check it).</param>
    inline void ChooseNewThreadWindow(NewThreadWizard& w, size_t index)
    {
        const auto& c = w.windows[index];
        w.windowChosen = true;
        w.windowId = c.windowId;
        w.windowNumber = c.number;
        w.anchorSessionId = c.anchorSessionId;
        w.anchorTitle = c.anchorTitle;
        w.askWindow = false;
        w.notice.clear();
    }

    /// <summary>
    /// A BUTTON of the wizard's picker (§21.25): what it changes and what the service does next. A button that does not belong to
    /// the step changes nothing (Stale); ✖ Cancel aborts from every step.
    /// </summary>
    /// <param name="r">The thread's record (changed in place).</param>
    /// <param name="a">The button's action (ParseNewThreadCallbackData).</param>
    /// <returns>The next thing to do.</returns>
    /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
    inline NewThreadOutcome ApplyNewThreadButton(NewThreadRecord& r, const NewThreadAction& a)
    {
        using K = NewThreadAction::Kind;
        NewThreadWizard& w = r.wizard;
        NewThreadOutcome out;
        if (!NewThreadStepTakes(w.step, a.kind))
        {
            out.kind = NewThreadOutcome::Kind::Stale;
            out.text = kNewThreadStaleTap;
            return out;
        }
        // Every accepted button is the user's next choice, so the line of the step before goes — but a page label (a toast that
        // changes nothing) and Cancel (the whole record goes) leave it. A choice below that fails sets its own line again.
        if (a.kind != K::Cancel && a.kind != K::PageLabelRecent && a.kind != K::PageLabelUsed)
        {
            w.notice.clear();
        }
        switch (a.kind)
        {
        case K::Cancel:
            out.kind = NewThreadOutcome::Kind::Abort;
            return out;
        case K::PageLabelRecent:
        case K::PageLabelUsed:
        {
            const bool recent = a.kind == K::PageLabelRecent;
            out.kind = NewThreadOutcome::Kind::Toast;
            out.text = NewThreadPageToast(recent ? L"Recent" : L"Most used", a.value, recent ? r.lists.recent.size() : r.lists.mostUsed.size());
            return out;
        }
        case K::PageRecent:
            r.recentPage = ClampNewThreadPage(a.value, r.lists.recent.size());
            out.kind = NewThreadOutcome::Kind::Render;
            return out;
        case K::PageUsed:
            r.usedPage = ClampNewThreadPage(a.value, r.lists.mostUsed.size());
            out.kind = NewThreadOutcome::Kind::Render;
            return out;
        case K::PickRecent:
        case K::PickUsed:
        {
            const auto& list = a.kind == K::PickRecent ? r.lists.recent : r.lists.mostUsed;
            if (a.value >= list.size())
            {
                out.kind = NewThreadOutcome::Kind::Stale;
                out.text = L"That folder is no longer in the list.";
                return out;
            }
            out.kind = NewThreadOutcome::Kind::CheckFolder;
            out.path = list[a.value].path;
            out.typed = false;
            return out;
        }
        case K::CreateFolder:
            out.kind = NewThreadOutcome::Kind::CreateFolder;
            out.path = w.createPath;
            return out;
        case K::BackToFolders:
            w.step = NewThreadStep::Folder;
            w.createPath.clear();
            w.createExistingPrefix.clear();
            w.createMissing.clear();
            ResetNewThreadAfterFolder(w);
            out.kind = NewThreadOutcome::Kind::Render;
            return out;
        case K::ModelDefault:
            ChooseNewThreadModel(w, L"", L"Default");
            out.kind = NewThreadOutcome::Kind::CollectWindows;
            return out;
        case K::ModelPick:
            if (a.value >= w.modelChoices.size())
            {
                out.kind = NewThreadOutcome::Kind::Stale;
                out.text = L"That model is no longer in the list.";
                return out;
            }
            ChooseNewThreadModel(w, w.modelChoices[a.value].target, w.modelChoices[a.value].label);
            out.kind = NewThreadOutcome::Kind::CollectWindows;
            return out;
        case K::ModelSpecify:
            w.step = NewThreadStep::ModelSpecify;
            out.kind = NewThreadOutcome::Kind::Render;
            return out;
        case K::BackToModels:
            w.step = NewThreadStep::Model;
            w.modelChosen = false;
            w.windowsCollected = false;
            w.windowChosen = false;
            out.kind = NewThreadOutcome::Kind::Render;
            return out;
        case K::WindowDefault:
            if (!w.windowsCollected || w.defaultWindow < 0 || static_cast<size_t>(w.defaultWindow) >= w.windows.size())
            {
                out.kind = NewThreadOutcome::Kind::Stale;
                out.text = L"The windows are still being looked at \x2014 a moment.";
                return out;
            }
            ChooseNewThreadWindow(w, static_cast<size_t>(w.defaultWindow));
            out.kind = NewThreadOutcome::Kind::Launch;
            return out;
        case K::WindowPick:
            if (!w.windowsCollected || a.value >= w.windows.size())
            {
                out.kind = NewThreadOutcome::Kind::Stale;
                out.text = L"That window is no longer in the list.";
                return out;
            }
            ChooseNewThreadWindow(w, a.value);
            out.kind = NewThreadOutcome::Kind::Launch;
            return out;
        case K::WindowRefresh:
            w.windowsCollected = false;
            out.kind = NewThreadOutcome::Kind::CollectWindows;
            return out;
        case K::None:
        default:
            out.kind = NewThreadOutcome::Kind::Stale;
            out.text = kNewThreadStaleTap;
            return out;
        }
    }

    /// <summary>
    /// A MESSAGE typed in the wizard's thread (§21.25): a lone folder path is a folder choice at every step (the user changed
    /// their mind about the folder); "/model &lt;id&gt;" at the model step and one word at the model-id step are a model; a
    /// window number at the window step is a window; anything else is the user's message, HELD for the session.
    /// </summary>
    /// <param name="r">The thread's record (changed in place).</param>
    /// <param name="text">The message's text.</param>
    /// <returns>The next thing to do.</returns>
    /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
    inline NewThreadOutcome ApplyNewThreadText(NewThreadRecord& r, std::wstring_view text)
    {
        NewThreadWizard& w = r.wizard;
        NewThreadOutcome out;
        if (const auto folder = NewThreadTypedFolder(text))
        {
            w.notice.clear(); // a choice: the line of the step before goes (the check sets its own when it fails)
            out.kind = NewThreadOutcome::Kind::CheckFolder;
            out.path = *folder;
            out.typed = true;
            return out;
        }
        // A model id typed at the model step ("/model x") or the model-id step (one word), or "@id" naming a listed preset.
        const auto takeModel = [&](const std::wstring& raw) -> NewThreadOutcome {
            NewThreadOutcome o;
            if (!raw.empty() && raw.front() == L'@')
            {
                for (const auto& c : w.modelChoices)
                {
                    if (c.preset && NewThreadTargetsEqual(c.target, raw))
                    {
                        ChooseNewThreadModel(w, c.target, c.label);
                        o.kind = NewThreadOutcome::Kind::CollectWindows;
                        return o;
                    }
                }
                o.kind = NewThreadOutcome::Kind::Reply;
                o.text = L"No preset is called \x201C" + raw + L"\x201D \x2014 pick one with its button, or send a model id.";
                return o;
            }
            std::wstring why;
            if (const auto id = ParseNewThreadModelId(raw, &why))
            {
                // A listed model typed by its id is that choice (its label); any other id is a model of its own.
                std::wstring label = *id;
                for (const auto& c : w.modelChoices)
                {
                    if (!c.preset && NewThreadTargetsEqual(c.target, *id))
                    {
                        label = c.label;
                        break;
                    }
                }
                ChooseNewThreadModel(w, *id, std::move(label));
                o.kind = NewThreadOutcome::Kind::CollectWindows;
                o.rememberModel = *id;
                return o;
            }
            o.kind = NewThreadOutcome::Kind::Reply;
            o.text = why;
            return o;
        };
        switch (w.step)
        {
        case NewThreadStep::Model:
            if (const auto arg = NewThreadModelCommandArg(text))
            {
                if (arg->empty())
                {
                    w.step = NewThreadStep::ModelSpecify; // a bare "/model": the id comes next
                    w.notice.clear();
                    out.kind = NewThreadOutcome::Kind::Render;
                    return out;
                }
                return takeModel(*arg);
            }
            break;
        case NewThreadStep::ModelSpecify:
        {
            if (const auto arg = NewThreadModelCommandArg(text); arg && !arg->empty())
            {
                return takeModel(*arg);
            }
            const size_t a = text.find_first_not_of(L" \t\r\n");
            const size_t b = text.find_last_not_of(L" \t\r\n");
            const std::wstring_view trimmed = a == std::wstring_view::npos ? std::wstring_view{} : text.substr(a, b - a + 1);
            if (!trimmed.empty() && trimmed.find_first_of(L" \t\r\n") == std::wstring_view::npos)
            {
                return takeModel(std::wstring{ trimmed });
            }
            // Words: the user's first prompt, typed before the model id. It is held, and the picker reminds what it waits for.
            w.notice = L"\U0001F4AC Kept as a message for the session. Send just the model id, or tap \u2705 Default.";
            break;
        }
        case NewThreadStep::Window:
            if (w.windowsCollected)
            {
                if (const auto index = ParseNewThreadWindowNumber(text, w.windows.size()))
                {
                    ChooseNewThreadWindow(w, *index);
                    out.kind = NewThreadOutcome::Kind::Launch;
                    return out;
                }
            }
            break;
        case NewThreadStep::Folder:
        case NewThreadStep::CreateFolder:
        default:
            break;
        }
        out.kind = NewThreadOutcome::Kind::Hold;
        return out;
    }

    /// <summary>
    /// The answer of a folder check (§21.25): an existing folder is chosen (the model step comes next); a TYPED folder that is
    /// missing but can be made goes to the CreateFolder step; anything else leaves the user at the folder lists with the reason.
    /// A LIST pick that is gone is never offered for creation (the folder was deleted since it was listed) — the lists are read
    /// again instead.
    /// </summary>
    /// <param name="r">The thread's record (changed in place).</param>
    /// <param name="res">The check's answer.</param>
    /// <param name="typed">The folder was typed (true) or picked from a list (false).</param>
    /// <returns>EnterModel for a chosen folder, Render otherwise.</returns>
    /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
    inline NewThreadOutcome ApplyNewThreadFolderResolved(NewThreadRecord& r, const FolderResolution& res, bool typed)
    {
        NewThreadWizard& w = r.wizard;
        NewThreadOutcome out;
        out.kind = NewThreadOutcome::Kind::Render;
        switch (res.kind)
        {
        case FolderResolveKind::Exists:
            w.folder = res.path;
            w.folderCreated = false;
            w.createPath.clear();
            w.createExistingPrefix.clear();
            w.createMissing.clear();
            ResetNewThreadAfterFolder(w);
            out.kind = NewThreadOutcome::Kind::EnterModel;
            return out;
        case FolderResolveKind::Missing:
            if (!typed)
            {
                w.step = NewThreadStep::Folder;
                w.notice = L"\u26A0 That folder is gone: " + res.path + L". The lists are read again.";
                r.listsBuilt = false; // the next render reads them again, without the deleted folder
                return out;
            }
            w.step = NewThreadStep::CreateFolder;
            w.createPath = res.path;
            w.createExistingPrefix = res.existingPrefix;
            w.createMissing = res.missing;
            return out;
        case FolderResolveKind::NoAnswer:
            w.step = NewThreadStep::Folder;
            w.notice = L"\u26A0 " + (res.reason.empty() ? NewThreadFolderNoAnswerText(res.path) : res.reason);
            return out;
        case FolderResolveKind::IsFile:
        case FolderResolveKind::NoRoot:
        case FolderResolveKind::Invalid:
        default:
            w.step = NewThreadStep::Folder;
            w.notice = L"\u26A0 " + res.reason;
            return out;
        }
    }

    /// <summary>
    /// The answer of the CreateFolder step's creation: made — the folder is chosen and the model step comes next; refused — the
    /// step stays, with Windows' reason.
    /// </summary>
    /// <param name="r">The thread's record (changed in place).</param>
    /// <param name="ok">The folder exists now.</param>
    /// <param name="error">Why not (Windows' words, plain text; "" when ok).</param>
    /// <returns>EnterModel when made, Render otherwise.</returns>
    /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
    inline NewThreadOutcome ApplyNewThreadFolderCreated(NewThreadRecord& r, bool ok, std::wstring_view error)
    {
        NewThreadWizard& w = r.wizard;
        NewThreadOutcome out;
        if (ok)
        {
            w.folder = w.createPath;
            w.folderCreated = true;
            w.createPath.clear();
            w.createExistingPrefix.clear();
            w.createMissing.clear();
            ResetNewThreadAfterFolder(w);
            out.kind = NewThreadOutcome::Kind::EnterModel;
            return out;
        }
        w.step = NewThreadStep::CreateFolder;
        w.notice = L"\u26A0 The folder could not be created: " + (error.empty() ? std::wstring{ L"Windows refused it." } : std::wstring{ error });
        out.kind = NewThreadOutcome::Kind::Render;
        return out;
    }

    /// <summary>Enters the model step with the choices the step will show (kept, so a tap maps to what it showed).</summary>
    /// <param name="w">The wizard.</param>
    /// <param name="choices">BuildNewThreadModelChoices.</param>
    /// <param name="offerAddPresets">The user has no preset at all (LaunchPresets.h ShouldOfferAddPresets).</param>
    /// <param name="settingsModel">The settings model Default stands for ("" = Claude Code's own default).</param>
    inline void EnterNewThreadModelStep(NewThreadWizard& w, std::vector<NewThreadModelChoice> choices, bool offerAddPresets, std::wstring settingsModel)
    {
        w.step = NewThreadStep::Model;
        w.modelChoices = std::move(choices);
        w.offerAddPresets = offerAddPresets;
        w.settingsModel = std::move(settingsModel);
        w.modelChosen = false;
    }

    /// <summary>
    /// The windows' census, as a plan (BuildNewThreadWindowPlan), reaches the window step: with two windows or more the step
    /// shows them (Render); with ONE the step is skipped and that window is chosen, its newest folder tab as the anchor; with NONE
    /// (no window answered) the step is skipped too and the engine picks the window (the one used last). After a launch that did
    /// not go through (askWindow) the step is NEVER skipped — it shows its one window, or says that none answered — so a refusal
    /// is never retried without the user's tap.
    /// </summary>
    /// <param name="w">The wizard.</param>
    /// <param name="plan">The windows and their default.</param>
    /// <returns>Render (choose a window) or Launch (the step was skipped).</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be copied.</exception>
    inline NewThreadOutcome ApplyNewThreadWindows(NewThreadWizard& w, NewThreadWindowPlan plan)
    {
        NewThreadOutcome out;
        w.step = NewThreadStep::Window;
        w.windows = std::move(plan.windows);
        w.windowsLeftOut = plan.leftOut;
        w.defaultWindow = plan.defaultIndex;
        w.defaultReason = std::move(plan.defaultReason);
        w.windowsCollected = true;
        w.windowChosen = false;
        w.windowAuto = false;
        if (w.windows.size() >= 2 || w.askWindow)
        {
            out.kind = NewThreadOutcome::Kind::Render;
            return out;
        }
        w.windowAuto = true;
        if (w.windows.size() == 1)
        {
            ChooseNewThreadWindow(w, 0);
        }
        else
        {
            w.windowChosen = true;
            w.windowId.clear();
            w.windowNumber = 0;
            w.anchorSessionId.clear();
            w.anchorTitle.clear();
        }
        out.kind = NewThreadOutcome::Kind::Launch;
        return out;
    }

    /// <summary>
    /// The step a launch that did not go through goes back to (§21.25), so one tap can retry or fix it: a preset that cannot
    /// start → the model step (pick another model); a launch whose window step was SKIPPED (one window) → the model step too,
    /// since the window step would skip itself again and launch at once — a tap on a model (or Default) is the retry; anything
    /// else (a window that closed, no answer, a failed tab) → the window step, its list read again. The choices stay as they were.
    /// </summary>
    /// <param name="w">The wizard the launch was made with.</param>
    /// <param name="result">The window's answer (WindowClosed for a launch the PC never answered).</param>
    /// <returns>The step to show again.</returns>
    inline NewThreadStep NewThreadStepAfterRefusal(const NewThreadWizard& w, RemoteLaunchResult result) noexcept
    {
        if (result == RemoteLaunchResult::PresetRefused || w.windowAuto)
        {
            return NewThreadStep::Model;
        }
        return NewThreadStep::Window;
    }

    /// <summary>
    /// Puts a wizard back at a step after a launch that did not go through (a refusal, a timeout, a window that was not open any
    /// more): the step, its notice, and — for the window step — a list to read again (the windows may have changed) that is shown
    /// even with one window (askWindow), so the retry is always the user's tap.
    /// </summary>
    /// <param name="w">The wizard.</param>
    /// <param name="step">The step to show.</param>
    /// <param name="notice">The line above it ("" = none).</param>
    inline void ReturnNewThreadToStep(NewThreadWizard& w, NewThreadStep step, std::wstring notice)
    {
        w.step = step;
        w.notice = std::move(notice);
        w.windowChosen = false;
        // No census is in flight for a wizard that comes back (the one that made its list ended long ago): 0 lets the service's
        // render start a new one for the window step — with the old serial kept, the step would wait for an answer that never
        // comes ("looking at the open windows…" for good).
        w.windowsSerial = 0;
        if (step == NewThreadStep::Window)
        {
            w.windowsCollected = false;
            w.askWindow = true;
        }
        if (step == NewThreadStep::Model || step == NewThreadStep::ModelSpecify)
        {
            w.modelChosen = false;
            w.windowsCollected = false;
        }
    }

    /// <summary>The folder's last name, for a label ("Agentmaster" for "K:\source\Agentmaster"; "K:" for "K:\").</summary>
    /// <param name="folder">The folder.</param>
    /// <returns>The name.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring NewThreadFolderLeaf(std::wstring_view folder)
    {
        std::wstring_view f = folder;
        while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/'))
        {
            f.remove_suffix(1);
        }
        if (f.size() <= 3 && f.size() >= 2 && f[1] == L':')
        {
            return std::wstring{ f.substr(0, 2) };
        }
        const size_t sep = f.find_last_of(L"\\/");
        return std::wstring{ sep == std::wstring_view::npos ? f : f.substr(sep + 1) };
    }

    /// <summary>What a launch made by the wizard asks for, and the line that says it (§21.25).</summary>
    struct NewThreadLaunchPlan
    {
        std::wstring dir; // the folder
        std::wstring title; // the thread's name when the user gave one ("" = the folder-derived default)
        std::wstring model; // the launch target ("" = Default; a model id; "@<presetId>")
        std::wstring windowId; // "" = the engine's choice
        std::wstring afterSessionId; // the tab to open right after ("" = the strip's end)
        std::wstring summaryHtml; // "🚀 Opening a new session in <b>Agentmaster</b> · Opus · window 2, next to “docs 2”…"
        std::wstring summaryText; // the same, plain (a button's toast, cut at 190 characters by the service)
    };

    /// <summary>
    /// The launch the wizard's choices ask for, and the line that tells the user what is opening: the folder (and "created" when
    /// the wizard made it), the model (when not Default), the window (when the step was shown) and the tab it opens next to.
    /// </summary>
    /// <param name="r">The thread's record, every choice made.</param>
    /// <returns>The plan.</returns>
    /// <exception cref="std::bad_alloc">If a string cannot be allocated.</exception>
    inline NewThreadLaunchPlan BuildNewThreadLaunchPlan(const NewThreadRecord& r)
    {
        const NewThreadWizard& w = r.wizard;
        NewThreadLaunchPlan p;
        p.dir = w.folder;
        p.title = r.topicName;
        p.model = w.modelTarget;
        p.windowId = w.windowId;
        p.afterSessionId = w.anchorSessionId;
        const std::wstring leaf = NewThreadFolderLeaf(w.folder);
        std::wstring textTail;
        std::wstring htmlTail;
        if (w.folderCreated)
        {
            textTail += L" (created)";
            htmlTail += L" (created)";
        }
        if (!w.modelTarget.empty())
        {
            const std::wstring label = w.modelLabel.empty() ? w.modelTarget : w.modelLabel;
            textTail += L" \x00B7 " + label;
            htmlTail += L" \x00B7 " + TgEscapeHtml(label);
        }
        if (w.windowNumber > 0 && !w.windowAuto)
        {
            textTail += L" \x00B7 window " + std::to_wstring(w.windowNumber);
            htmlTail += L" \x00B7 window " + std::to_wstring(w.windowNumber);
        }
        if (!w.anchorTitle.empty())
        {
            textTail += L", next to \x201C" + w.anchorTitle + L"\x201D";
            htmlTail += L", next to \x201C" + TgEscapeHtml(w.anchorTitle) + L"\x201D";
        }
        p.summaryText = L"Opening a new session in " + leaf + textTail + L"\x2026";
        p.summaryHtml = L"\U0001F680 Opening a new session in <b>" + TgEscapeHtml(leaf) + L"</b>" + htmlTail + L"\x2026";
        return p;
    }

    // ===== the abort ==================================================================================================

    // The most characters of ONE held text the abort line keeps (an ellipsis past it), and of all of them together: the line is
    // one Telegram message, and it is a safety copy of what the user wrote, not a transcript.
    inline constexpr size_t kNewThreadAbortTextChars = 1000;
    inline constexpr size_t kNewThreadAbortTotalChars = 3200;

    /// <summary>
    /// The silent line the chat's MAIN VIEW gets when an aborted wizard's thread is deleted WITH messages in it (§21.25): which
    /// thread, why, and what the user wrote there — the messages go with the thread, so this is their only copy — in an
    /// expandable quote. Classic Telegram HTML; every text escaped.
    /// </summary>
    /// <param name="topicName">The thread's name ("" = Telegram named it: "a new thread").</param>
    /// <param name="texts">The held messages' texts, oldest first (empty ones are skipped).</param>
    /// <param name="cancelled">True for the user's ✖ Cancel; false for an abandoned wizard (`why` says why).</param>
    /// <param name="why">Why an abandoned wizard was aborted ("nothing was chosen in it for 24 hours"); unused when cancelled.</param>
    /// <returns>The line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildNewThreadAbortHtml(std::wstring_view topicName, const std::vector<std::wstring>& texts, bool cancelled, std::wstring_view why)
    {
        const std::wstring name = topicName.empty() ? std::wstring{ L"a new thread" } : L"\x201C" + TgEscapeHtml(TgOneLineTitle(topicName)) + L"\x201D";
        std::wstring html = L"\U0001F5D1 " + std::wstring{ cancelled ? L"Cancelled: " : L"" } + name + L" was deleted" +
                            (cancelled ? std::wstring{} : (why.empty() ? std::wstring{} : L" \x2014 " + TgEscapeHtml(why))) + L".";
        std::wstring quoted;
        size_t total = 0;
        size_t kept = 0;
        size_t shown = 0;
        for (const auto& t : texts)
        {
            if (t.empty())
            {
                continue;
            }
            ++kept;
            if (total >= kNewThreadAbortTotalChars)
            {
                continue; // counted, not shown
            }
            std::wstring one = t.size() > kNewThreadAbortTextChars ? t.substr(0, kNewThreadAbortTextChars) + L"\x2026" : t;
            if (total + one.size() > kNewThreadAbortTotalChars)
            {
                one = one.substr(0, kNewThreadAbortTotalChars - total) + L"\x2026";
            }
            total += one.size();
            quoted += (quoted.empty() ? L"" : L"\n\n") + TgEscapeHtml(one);
            ++shown;
        }
        if (kept == 0)
        {
            return html;
        }
        html += L"\n" + std::wstring{ kept == 1 ? L"What you wrote there:" : L"What you wrote there (" + std::to_wstring(kept) + L" messages):" };
        html += L"\n<blockquote expandable>" + quoted + L"</blockquote>";
        if (shown < kept)
        {
            html += L"\n<i>" + std::to_wstring(kept - shown) + L" more not shown.</i>";
        }
        return html;
    }

    // ===== the views ==================================================================================================

    /// <summary>A rendered step: the picker's text (classic Telegram HTML) and its keyboard.</summary>
    struct NewThreadView
    {
        std::wstring html;
        std::vector<std::vector<NewThreadButton>> rows;
    };

    namespace detail
    {
        /// <summary>A button label cut at kNewThreadButtonLabelChars (one line, an ellipsis past it; TgTitleLine's surrogate-safe cut).</summary>
        inline std::wstring NewThreadButtonLabel(std::wstring_view text)
        {
            return TgTitleLine(text, kNewThreadButtonLabelChars);
        }

        /// <summary>The line every step after the folder starts with: the chosen folder (its name, then its path).</summary>
        inline std::wstring NewThreadFolderLine(const NewThreadWizard& w)
        {
            return L"\U0001F4C2 <b>" + TgEscapeHtml(NewThreadFolderLeaf(w.folder)) + L"</b> \x00B7 <code>" + TgEscapeHtml(w.folder) + L"</code>" +
                   (w.folderCreated ? std::wstring{ L" (created)" } : std::wstring{});
        }

        /// <summary>The held-messages line of a step (empty when nothing is held yet — the step then says what a message does).</summary>
        inline std::wstring NewThreadHeldLine(size_t heldCount)
        {
            if (heldCount == 0)
            {
                return L"\n<i>A message you send here waits for the session and becomes its first prompt.</i>";
            }
            if (heldCount == 1)
            {
                return L"\n<i>Your message waits here and becomes the session\x2019s first prompt.</i>";
            }
            return L"\n<i>Your " + std::to_wstring(heldCount) + L" messages wait here and go to the session in order.</i>";
        }

        /// <summary>The notice line ("" when there is none), shown at the top of a step.</summary>
        inline std::wstring NewThreadNoticeLine(const NewThreadWizard& w)
        {
            return w.notice.empty() ? std::wstring{} : L"<i>" + TgEscapeHtml(w.notice) + L"</i>\n\n";
        }

        /// <summary>The window step's text with at most `titles` titles per window (the budget loop of BuildNewThreadView shrinks it).</summary>
        inline std::wstring NewThreadWindowHtml(const NewThreadRecord& r, size_t titles, int64_t nowMs)
        {
            const NewThreadWizard& w = r.wizard;
            std::wstring html = NewThreadNoticeLine(w) + NewThreadFolderLine(w);
            if (w.modelChosen)
            {
                html += L"\n\U0001F9E0 " + TgEscapeHtml(w.modelTarget.empty() ? std::wstring{ L"Default" } : (w.modelLabel.empty() ? w.modelTarget : w.modelLabel));
            }
            html += L"\n\n\U0001FA9F <b>Window</b> \x2014 the new tab opens there, in the background.";
            if (!w.windowsCollected)
            {
                html += L"\n<i>Looking at the open windows\x2026</i>";
                return html + NewThreadHeldLine(r.held.size());
            }
            if (w.windows.empty())
            {
                // Only after a launch that did not go through (askWindow): a fresh pass with no window answering skips the step.
                html += L"\n<i>No window answered \x2014 is Agentmaster open on the PC? Tap \U0001F504 Refresh to look again, or \x25C0 Back.</i>";
                return html + NewThreadHeldLine(r.held.size());
            }
            if (w.defaultWindow >= 0 && static_cast<size_t>(w.defaultWindow) < w.windows.size())
            {
                html += L"\n\u2B50 Default: <b>window " + std::to_wstring(w.windows[static_cast<size_t>(w.defaultWindow)].number) + L"</b> \x2014 " + TgEscapeHtml(w.defaultReason) + L".";
            }
            for (size_t i = 0; i < w.windows.size(); ++i)
            {
                const auto& c = w.windows[i];
                html += L"\n\n<b>" + std::to_wstring(c.number) + L"</b> \x00B7 " + std::to_wstring(c.tabCount) + (c.tabCount == 1 ? L" tab" : L" tabs");
                if (static_cast<int>(i) == w.defaultWindow)
                {
                    html += L" \u2B50";
                }
                if (c.lastActive)
                {
                    html += L" \x00B7 used last";
                }
                if (c.folderTabs > 0)
                {
                    html += L" \x00B7 " + std::to_wstring(c.folderTabs) + L" in " + TgEscapeHtml(NewThreadFolderLeaf(w.folder));
                }
                if (!c.newestTitle.empty())
                {
                    html += L"\n   newest: \x201C" + TgEscapeHtml(c.newestTitle) + L"\x201D";
                    if (c.newestCreatedMs > 0 && nowMs >= c.newestCreatedMs)
                    {
                        html += L" \x00B7 opened " + TgShortSpan(nowMs - c.newestCreatedMs) + L" ago";
                    }
                }
                if (titles > 0 && !c.titles.empty())
                {
                    std::wstring list;
                    for (size_t k = 0; k < c.titles.size() && k < titles; ++k)
                    {
                        list += (k == 0 ? L"" : L" \x00B7 ") + TgEscapeHtml(c.titles[k]);
                    }
                    if (c.titles.size() > titles || c.tabCount > c.titles.size())
                    {
                        list += L" \x00B7 \x2026";
                    }
                    html += L"\n   <i>" + list + L"</i>";
                }
            }
            if (w.windowsLeftOut > 0)
            {
                html += L"\n\n<i>" + std::to_wstring(w.windowsLeftOut) + L" more window(s) not listed.</i>";
            }
            html += L"\n\n<i>Or send the window\x2019s number.</i>";
            return html + NewThreadHeldLine(r.held.size());
        }
    }

    /// <summary>
    /// The picker message of the wizard's current step (§21.25): its text and its keyboard. The folder step is the §21.17 picker
    /// (BuildNewThreadPickerHtml / BuildNewThreadPickerKeyboard) under the notice line; the other steps are this header's.
    /// </summary>
    /// <param name="r">The thread's record.</param>
    /// <param name="nowMs">The clock the "ago" texts count against (Unix ms).</param>
    /// <returns>The view.</returns>
    /// <exception cref="std::bad_alloc">If the view cannot be allocated.</exception>
    inline NewThreadView BuildNewThreadView(const NewThreadRecord& r, int64_t nowMs)
    {
        using K = NewThreadAction::Kind;
        const NewThreadWizard& w = r.wizard;
        NewThreadView v;
        const auto cancelRow = [](std::vector<NewThreadButton> row) {
            row.push_back(NewThreadButton{ L"\x2716 Cancel", NewThreadCallbackData(K::Cancel, 0) });
            return row;
        };
        switch (w.step)
        {
        case NewThreadStep::CreateFolder:
        {
            std::wstring made;
            for (size_t i = 0; i < w.createMissing.size(); ++i)
            {
                made += (i == 0 ? L"" : L"\\") + w.createMissing[i];
            }
            v.html = detail::NewThreadNoticeLine(w) + L"\U0001F4C1 <b>" + TgEscapeHtml(NewThreadFolderLeaf(w.createPath)) + L"</b> does not exist yet:\n<code>" +
                     TgEscapeHtml(w.createPath) + L"</code>\n\n<code>" + TgEscapeHtml(w.createExistingPrefix) + L"</code> exists; Create makes <b>" + TgEscapeHtml(made) + L"</b>" +
                     (w.createMissing.size() > 1 ? L" (" + std::to_wstring(w.createMissing.size()) + L" new folders)" : std::wstring{}) +
                     L" and starts the session there.\nOr go back and pick another folder." + detail::NewThreadHeldLine(r.held.size());
            v.rows.push_back({ NewThreadButton{ L"\U0001F4C1 Create the folder", NewThreadCallbackData(K::CreateFolder, 0) } });
            v.rows.push_back(cancelRow({ NewThreadButton{ L"\u25C0 Back to the folders", NewThreadCallbackData(K::BackToFolders, 0) } }));
            return v;
        }
        case NewThreadStep::Model:
        {
            v.html = detail::NewThreadNoticeLine(w) + detail::NewThreadFolderLine(w) + L"\n\n\U0001F9E0 <b>Model</b> \x2014 optional.\n<i>Default is " +
                     (w.settingsModel.empty() ? std::wstring{ L"Claude Code\x2019s own default model" } : L"the settings model, <code>" + TgEscapeHtml(w.settingsModel) + L"</code>") +
                     L". Or tap \u270F Specify\x2026 and send a model id (like <code>claude-opus-5-5[1m]</code>), or send <code>/model &lt;id&gt;</code>.</i>";
            if (w.offerAddPresets)
            {
                v.html += L"\n<i>No presets yet \x2014 add one at the PC: Settings \u2699 \u2192 Presets \u2192 TEMPLATES.</i>";
            }
            v.html += detail::NewThreadHeldLine(r.held.size());
            // The PC menus' order (AgentModelMenu.h): Default, then Specify right under it, then the models, then the presets.
            v.rows.push_back({ NewThreadButton{ L"\u2705 Default", NewThreadCallbackData(K::ModelDefault, 0) },
                               NewThreadButton{ L"\u270f Specify\x2026", NewThreadCallbackData(K::ModelSpecify, 0) } });
            std::vector<NewThreadButton> row;
            for (size_t i = 0; i < w.modelChoices.size(); ++i)
            {
                const auto& c = w.modelChoices[i];
                const wchar_t* glyph = c.last ? L"\u21BA " : (c.preset ? L"\U0001F50C " : L"\U0001F9E0 ");
                row.push_back(NewThreadButton{ glyph + detail::NewThreadButtonLabel(c.label), NewThreadCallbackData(K::ModelPick, i) });
                if (row.size() == 2)
                {
                    v.rows.push_back(std::move(row));
                    row.clear();
                }
            }
            if (!row.empty())
            {
                v.rows.push_back(std::move(row));
            }
            v.rows.push_back(cancelRow({ NewThreadButton{ L"\u25C0 Back", NewThreadCallbackData(K::BackToFolders, 0) } }));
            return v;
        }
        case NewThreadStep::ModelSpecify:
            v.html = detail::NewThreadNoticeLine(w) + detail::NewThreadFolderLine(w) +
                     L"\n\n\u270F <b>Send the model id</b> as your next message \x2014 like <code>claude-opus-5-5[1m]</code>.\n<i>Letters, digits and - _ . : / [ ] only, no spaces.</i>" +
                     detail::NewThreadHeldLine(r.held.size());
            v.rows.push_back({ NewThreadButton{ L"\u2705 Use Default", NewThreadCallbackData(K::ModelDefault, 0) },
                               NewThreadButton{ L"\u25C0 Back to the list", NewThreadCallbackData(K::BackToModels, 0) } });
            v.rows.push_back(cancelRow({}));
            return v;
        case NewThreadStep::Window:
        {
            // The text within the budget: each window's title list shrinks until it fits (10, 6, 3, none).
            static constexpr size_t kTitleSteps[] = { kNewThreadWindowTitles, 6, 3, 0 };
            for (const size_t titles : kTitleSteps)
            {
                v.html = detail::NewThreadWindowHtml(r, titles, nowMs);
                if (v.html.size() <= kNewThreadWindowTextBudget)
                {
                    break;
                }
            }
            if (w.windowsCollected)
            {
                if (w.defaultWindow >= 0 && static_cast<size_t>(w.defaultWindow) < w.windows.size())
                {
                    v.rows.push_back({ NewThreadButton{ L"\u2B50 Default \x2014 window " + std::to_wstring(w.windows[static_cast<size_t>(w.defaultWindow)].number),
                                                        NewThreadCallbackData(K::WindowDefault, 0) } });
                }
                std::vector<NewThreadButton> row;
                for (size_t i = 0; i < w.windows.size(); ++i)
                {
                    row.push_back(NewThreadButton{ L"\U0001FA9F " + std::to_wstring(w.windows[i].number), NewThreadCallbackData(K::WindowPick, i) });
                    if (row.size() == 4)
                    {
                        v.rows.push_back(std::move(row));
                        row.clear();
                    }
                }
                if (!row.empty())
                {
                    v.rows.push_back(std::move(row));
                }
                v.rows.push_back({ NewThreadButton{ L"\U0001F504 Refresh", NewThreadCallbackData(K::WindowRefresh, 0) } });
            }
            v.rows.push_back(cancelRow({ NewThreadButton{ L"\u25C0 Back", NewThreadCallbackData(K::BackToModels, 0) } }));
            return v;
        }
        case NewThreadStep::Folder:
        default:
            v.html = detail::NewThreadNoticeLine(w) + BuildNewThreadPickerHtml(r.lists, r.recentPage, r.usedPage, r.held.size(), r.topicName, nowMs);
            v.rows = BuildNewThreadPickerKeyboard(r.lists, r.recentPage, r.usedPage);
            return v;
        }
    }

    // ===== persistence ================================================================================================

    namespace detail
    {
        /// <summary>A string array as JSON.</summary>
        inline json::Value NewThreadStringsToJson(const std::vector<std::wstring>& items)
        {
            auto a = json::Value::MkArr();
            for (const auto& s : items)
            {
                a.Push(json::Value::MkStr(s));
            }
            return a;
        }

        /// <summary>A JSON string array back (members of another type are skipped).</summary>
        inline std::vector<std::wstring> NewThreadStringsFromJson(const json::Value* v)
        {
            std::vector<std::wstring> out;
            if (v && v->type == json::Value::Type::Arr)
            {
                for (const auto& e : v->arr)
                {
                    if (e.type == json::Value::Type::Str)
                    {
                        out.push_back(e.str);
                    }
                }
            }
            return out;
        }

        /// <summary>A folder list as JSON ({path, sessions, last} per folder).</summary>
        inline json::Value NewThreadFoldersToJson(const std::vector<NewThreadFolder>& folders)
        {
            auto a = json::Value::MkArr();
            for (const auto& f : folders)
            {
                auto o = json::Value::MkObj();
                o.Set(L"path", json::Value::MkStr(f.path));
                o.Set(L"sessions", json::Value::MkNum(static_cast<double>(f.sessions)));
                o.Set(L"last", json::Value::MkNum(static_cast<double>(f.lastActivityMs)));
                a.Push(std::move(o));
            }
            return a;
        }

        /// <summary>A folder list back (an entry without a path is skipped).</summary>
        inline std::vector<NewThreadFolder> NewThreadFoldersFromJson(const json::Value* v)
        {
            std::vector<NewThreadFolder> out;
            if (v && v->type == json::Value::Type::Arr)
            {
                for (const auto& e : v->arr)
                {
                    if (e.type != json::Value::Type::Obj || e.StrAt(L"path").empty())
                    {
                        continue;
                    }
                    out.push_back(NewThreadFolder{ e.StrAt(L"path"), static_cast<int>(e.I64At(L"sessions", 0)), e.I64At(L"last", 0) });
                }
            }
            return out;
        }

        /// <summary>The held messages as JSON (each Message object as it came).</summary>
        inline json::Value NewThreadHeldToJson(const std::vector<json::Value>& held)
        {
            auto a = json::Value::MkArr();
            for (const auto& m : held)
            {
                a.Push(m);
            }
            return a;
        }

        /// <summary>The held messages back (only objects: a message is one).</summary>
        inline std::vector<json::Value> NewThreadHeldFromJson(const json::Value* v)
        {
            std::vector<json::Value> out;
            if (v && v->type == json::Value::Type::Arr)
            {
                for (const auto& e : v->arr)
                {
                    if (e.type == json::Value::Type::Obj)
                    {
                        out.push_back(e);
                    }
                }
            }
            return out;
        }
    }

    /// <summary>The wizard as JSON (every field but the transient windowsSerial).</summary>
    /// <param name="w">The wizard.</param>
    /// <returns>The JSON object.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value NewThreadWizardToJson(const NewThreadWizard& w)
    {
        auto o = json::Value::MkObj();
        o.Set(L"step", json::Value::MkNum(static_cast<double>(static_cast<int>(w.step))));
        o.Set(L"notice", json::Value::MkStr(w.notice));
        o.Set(L"folder", json::Value::MkStr(w.folder));
        o.Set(L"folderCreated", json::Value::MkBool(w.folderCreated));
        o.Set(L"createPath", json::Value::MkStr(w.createPath));
        o.Set(L"createExistingPrefix", json::Value::MkStr(w.createExistingPrefix));
        o.Set(L"createMissing", detail::NewThreadStringsToJson(w.createMissing));
        auto models = json::Value::MkArr();
        for (const auto& c : w.modelChoices)
        {
            auto m = json::Value::MkObj();
            m.Set(L"label", json::Value::MkStr(c.label));
            m.Set(L"target", json::Value::MkStr(c.target));
            m.Set(L"preset", json::Value::MkBool(c.preset));
            m.Set(L"last", json::Value::MkBool(c.last));
            models.Push(std::move(m));
        }
        o.Set(L"modelChoices", std::move(models));
        o.Set(L"offerAddPresets", json::Value::MkBool(w.offerAddPresets));
        o.Set(L"settingsModel", json::Value::MkStr(w.settingsModel));
        o.Set(L"modelChosen", json::Value::MkBool(w.modelChosen));
        o.Set(L"modelTarget", json::Value::MkStr(w.modelTarget));
        o.Set(L"modelLabel", json::Value::MkStr(w.modelLabel));
        o.Set(L"windowsCollected", json::Value::MkBool(w.windowsCollected));
        auto windows = json::Value::MkArr();
        for (const auto& c : w.windows)
        {
            auto m = json::Value::MkObj();
            m.Set(L"windowId", json::Value::MkStr(c.windowId));
            m.Set(L"number", json::Value::MkNum(static_cast<double>(c.number)));
            m.Set(L"lastActive", json::Value::MkBool(c.lastActive));
            m.Set(L"tabCount", json::Value::MkNum(static_cast<double>(c.tabCount)));
            m.Set(L"newestTitle", json::Value::MkStr(c.newestTitle));
            m.Set(L"newestCreatedMs", json::Value::MkNum(static_cast<double>(c.newestCreatedMs)));
            m.Set(L"titles", detail::NewThreadStringsToJson(c.titles));
            m.Set(L"folderTabs", json::Value::MkNum(static_cast<double>(c.folderTabs)));
            m.Set(L"anchorSessionId", json::Value::MkStr(c.anchorSessionId));
            m.Set(L"anchorTitle", json::Value::MkStr(c.anchorTitle));
            m.Set(L"anchorCreatedMs", json::Value::MkNum(static_cast<double>(c.anchorCreatedMs)));
            m.Set(L"anchorRecentMs", json::Value::MkNum(static_cast<double>(c.anchorRecentMs)));
            windows.Push(std::move(m));
        }
        o.Set(L"windows", std::move(windows));
        o.Set(L"windowsLeftOut", json::Value::MkNum(static_cast<double>(w.windowsLeftOut)));
        o.Set(L"defaultWindow", json::Value::MkNum(static_cast<double>(w.defaultWindow)));
        o.Set(L"defaultReason", json::Value::MkStr(w.defaultReason));
        o.Set(L"windowChosen", json::Value::MkBool(w.windowChosen));
        o.Set(L"windowAuto", json::Value::MkBool(w.windowAuto));
        o.Set(L"askWindow", json::Value::MkBool(w.askWindow));
        o.Set(L"windowId", json::Value::MkStr(w.windowId));
        o.Set(L"windowNumber", json::Value::MkNum(static_cast<double>(w.windowNumber)));
        o.Set(L"anchorSessionId", json::Value::MkStr(w.anchorSessionId));
        o.Set(L"anchorTitle", json::Value::MkStr(w.anchorTitle));
        return o;
    }

    /// <summary>
    /// The wizard back from JSON. A missing or malformed member reads as its default, and a default-window index outside the
    /// list reads as none: a file of another build can lose a choice, never point a tap at a window that is not in the list.
    /// </summary>
    /// <param name="v">The JSON object (anything else reads as a fresh wizard at the folder step).</param>
    /// <returns>The wizard.</returns>
    /// <exception cref="std::bad_alloc">If the wizard cannot be allocated.</exception>
    inline NewThreadWizard NewThreadWizardFromJson(const json::Value& v)
    {
        NewThreadWizard w;
        if (v.type != json::Value::Type::Obj)
        {
            return w;
        }
        w.step = NewThreadStepFromNumber(v.I64At(L"step", 0));
        w.notice = v.StrAt(L"notice");
        w.folder = v.StrAt(L"folder");
        w.folderCreated = v.BoolAt(L"folderCreated", false);
        w.createPath = v.StrAt(L"createPath");
        w.createExistingPrefix = v.StrAt(L"createExistingPrefix");
        w.createMissing = detail::NewThreadStringsFromJson(v.Find(L"createMissing"));
        if (const auto* models = v.Find(L"modelChoices"); models && models->type == json::Value::Type::Arr)
        {
            for (const auto& m : models->arr)
            {
                if (m.type != json::Value::Type::Obj || m.StrAt(L"target").empty())
                {
                    continue;
                }
                NewThreadModelChoice c;
                c.label = m.StrAt(L"label");
                c.target = m.StrAt(L"target");
                c.preset = m.BoolAt(L"preset", false);
                c.last = m.BoolAt(L"last", false);
                w.modelChoices.push_back(std::move(c));
            }
        }
        w.offerAddPresets = v.BoolAt(L"offerAddPresets", false);
        w.settingsModel = v.StrAt(L"settingsModel");
        w.modelChosen = v.BoolAt(L"modelChosen", false);
        w.modelTarget = v.StrAt(L"modelTarget");
        w.modelLabel = v.StrAt(L"modelLabel");
        w.windowsCollected = v.BoolAt(L"windowsCollected", false);
        if (const auto* windows = v.Find(L"windows"); windows && windows->type == json::Value::Type::Arr)
        {
            for (const auto& m : windows->arr)
            {
                if (m.type != json::Value::Type::Obj)
                {
                    continue;
                }
                NewThreadWindowChoice c;
                c.windowId = m.StrAt(L"windowId");
                c.number = static_cast<uint32_t>(m.I64At(L"number", 0));
                c.lastActive = m.BoolAt(L"lastActive", false);
                c.tabCount = static_cast<uint32_t>(m.I64At(L"tabCount", 0));
                c.newestTitle = m.StrAt(L"newestTitle");
                c.newestCreatedMs = m.I64At(L"newestCreatedMs", 0);
                c.titles = detail::NewThreadStringsFromJson(m.Find(L"titles"));
                c.folderTabs = static_cast<uint32_t>(m.I64At(L"folderTabs", 0));
                c.anchorSessionId = m.StrAt(L"anchorSessionId");
                c.anchorTitle = m.StrAt(L"anchorTitle");
                c.anchorCreatedMs = m.I64At(L"anchorCreatedMs", 0);
                c.anchorRecentMs = m.I64At(L"anchorRecentMs", 0);
                w.windows.push_back(std::move(c));
            }
        }
        w.windowsLeftOut = static_cast<size_t>((std::max)(int64_t{ 0 }, v.I64At(L"windowsLeftOut", 0))); // (std::max): the <windows.h> macro
        const int64_t def = v.I64At(L"defaultWindow", -1);
        w.defaultWindow = def >= 0 && static_cast<size_t>(def) < w.windows.size() ? static_cast<int>(def) : -1;
        w.defaultReason = v.StrAt(L"defaultReason");
        w.windowChosen = v.BoolAt(L"windowChosen", false);
        w.windowAuto = v.BoolAt(L"windowAuto", false);
        w.askWindow = v.BoolAt(L"askWindow", false);
        w.windowId = v.StrAt(L"windowId");
        w.windowNumber = static_cast<uint32_t>(v.I64At(L"windowNumber", 0));
        w.anchorSessionId = v.StrAt(L"anchorSessionId");
        w.anchorTitle = v.StrAt(L"anchorTitle");
        // A window step whose list did not survive is read again (the step shows "looking at the windows…").
        if (w.step == NewThreadStep::Window && w.windows.empty())
        {
            w.windowsCollected = false;
        }
        return w;
    }

    /// <summary>A thread's record as JSON (the transient pickerStale left out).</summary>
    /// <param name="threadId">The thread.</param>
    /// <param name="r">Its record.</param>
    /// <returns>The JSON object.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value NewThreadRecordToJson(int64_t threadId, const NewThreadRecord& r)
    {
        auto o = json::Value::MkObj();
        o.Set(L"threadId", json::Value::MkNum(static_cast<double>(threadId)));
        o.Set(L"topicName", json::Value::MkStr(r.topicName));
        o.Set(L"listsBuilt", json::Value::MkBool(r.listsBuilt));
        o.Set(L"recent", detail::NewThreadFoldersToJson(r.lists.recent));
        o.Set(L"mostUsed", detail::NewThreadFoldersToJson(r.lists.mostUsed));
        o.Set(L"recentPage", json::Value::MkNum(static_cast<double>(r.recentPage)));
        o.Set(L"usedPage", json::Value::MkNum(static_cast<double>(r.usedPage)));
        // A post still in flight (-1) has no message yet: it is persisted as "no picker", and the next render posts one.
        o.Set(L"pickerMessageId", json::Value::MkNum(static_cast<double>(r.pickerMessageId > 0 ? r.pickerMessageId : 0)));
        o.Set(L"held", detail::NewThreadHeldToJson(r.held));
        o.Set(L"lastUseMs", json::Value::MkNum(static_cast<double>(r.lastUseMs)));
        o.Set(L"wizard", NewThreadWizardToJson(r.wizard));
        return o;
    }

    /// <summary>A thread's record back from JSON.</summary>
    /// <param name="v">The JSON object.</param>
    /// <returns>The thread and its record; nullopt for anything without a thread id.</returns>
    /// <exception cref="std::bad_alloc">If the record cannot be allocated.</exception>
    inline std::optional<std::pair<int64_t, NewThreadRecord>> NewThreadRecordFromJson(const json::Value& v)
    {
        if (v.type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        const int64_t threadId = v.I64At(L"threadId", 0);
        if (threadId <= 0)
        {
            return std::nullopt;
        }
        NewThreadRecord r;
        r.topicName = v.StrAt(L"topicName");
        r.listsBuilt = v.BoolAt(L"listsBuilt", false);
        r.lists.recent = detail::NewThreadFoldersFromJson(v.Find(L"recent"));
        r.lists.mostUsed = detail::NewThreadFoldersFromJson(v.Find(L"mostUsed"));
        r.recentPage = ClampNewThreadPage(static_cast<size_t>((std::max)(int64_t{ 0 }, v.I64At(L"recentPage", 0))), r.lists.recent.size()); // (std::max): the <windows.h> macro
        r.usedPage = ClampNewThreadPage(static_cast<size_t>((std::max)(int64_t{ 0 }, v.I64At(L"usedPage", 0))), r.lists.mostUsed.size());
        r.pickerMessageId = (std::max)(int64_t{ 0 }, v.I64At(L"pickerMessageId", 0));
        r.held = detail::NewThreadHeldFromJson(v.Find(L"held"));
        r.lastUseMs = v.I64At(L"lastUseMs", 0);
        if (const auto* w = v.Find(L"wizard"))
        {
            r.wizard = NewThreadWizardFromJson(*w);
        }
        return std::make_pair(threadId, std::move(r));
    }

    /// <summary>A bind as JSON (its launch token and answered flag left out: no answer survives a restart).</summary>
    /// <param name="b">The bind.</param>
    /// <returns>The JSON object.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value NewThreadBindToJson(const NewThreadBind& b)
    {
        auto o = json::Value::MkObj();
        o.Set(L"sessionId", json::Value::MkStr(b.sessionId));
        o.Set(L"threadId", json::Value::MkNum(static_cast<double>(b.threadId)));
        o.Set(L"replyTo", json::Value::MkNum(static_cast<double>(b.replyTo)));
        o.Set(L"dir", json::Value::MkStr(b.dir));
        o.Set(L"title", json::Value::MkStr(b.title));
        o.Set(L"held", detail::NewThreadHeldToJson(b.held));
        o.Set(L"requestedMs", json::Value::MkNum(static_cast<double>(b.requestedMs)));
        o.Set(L"wizard", NewThreadWizardToJson(b.wizard));
        return o;
    }

    /// <summary>A bind back from JSON.</summary>
    /// <param name="v">The JSON object.</param>
    /// <returns>The bind; nullopt for anything without a session id and a thread id.</returns>
    /// <exception cref="std::bad_alloc">If the bind cannot be allocated.</exception>
    inline std::optional<NewThreadBind> NewThreadBindFromJson(const json::Value& v)
    {
        if (v.type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        NewThreadBind b;
        b.sessionId = v.StrAt(L"sessionId");
        b.threadId = v.I64At(L"threadId", 0);
        if (b.sessionId.empty() || b.threadId <= 0)
        {
            return std::nullopt;
        }
        b.replyTo = v.I64At(L"replyTo", 0);
        b.dir = v.StrAt(L"dir");
        b.title = v.StrAt(L"title");
        b.held = detail::NewThreadHeldFromJson(v.Find(L"held"));
        b.requestedMs = v.I64At(L"requestedMs", 0);
        if (const auto* w = v.Find(L"wizard"))
        {
            b.wizard = NewThreadWizardFromJson(*w);
        }
        return b;
    }

    /// <summary>
    /// Every wizard and bind of the bot as ONE JSON object — MessagingState::newThreads, written with the state file (§21.25), so
    /// a restart resumes them.
    /// </summary>
    /// <param name="records">Thread id -> its record.</param>
    /// <param name="binds">The launches waiting for their sessions.</param>
    /// <returns>{"threads": [...], "binds": [...]}.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value NewThreadsToJson(const std::map<int64_t, NewThreadRecord>& records, const std::vector<NewThreadBind>& binds)
    {
        auto o = json::Value::MkObj();
        auto threads = json::Value::MkArr();
        for (const auto& [threadId, r] : records)
        {
            threads.Push(NewThreadRecordToJson(threadId, r));
        }
        o.Set(L"threads", std::move(threads));
        auto bindsJson = json::Value::MkArr();
        for (const auto& b : binds)
        {
            bindsJson.Push(NewThreadBindToJson(b));
        }
        o.Set(L"binds", std::move(bindsJson));
        return o;
    }

    /// <summary>
    /// The wizards and binds back from MessagingState::newThreads (replacing what `records` and `binds` held). A malformed entry is
    /// skipped; a bind comes back with no launch token and as not answered (its answer died with the last run), and with its
    /// clock started now (`nowMs`), so the PC gets a whole timeout to bring its session back.
    /// </summary>
    /// <param name="v">The JSON object (anything else clears both).</param>
    /// <param name="records">Receives the records.</param>
    /// <param name="binds">Receives the binds.</param>
    /// <param name="nowMs">The clock a restored bind's timeout counts from.</param>
    /// <exception cref="std::bad_alloc">If the records cannot be allocated.</exception>
    inline void NewThreadsFromJson(const json::Value& v, std::map<int64_t, NewThreadRecord>& records, std::vector<NewThreadBind>& binds, int64_t nowMs)
    {
        records.clear();
        binds.clear();
        if (v.type != json::Value::Type::Obj)
        {
            return;
        }
        if (const auto* threads = v.Find(L"threads"); threads && threads->type == json::Value::Type::Arr)
        {
            for (const auto& t : threads->arr)
            {
                if (auto r = NewThreadRecordFromJson(t))
                {
                    if (r->second.lastUseMs <= 0)
                    {
                        r->second.lastUseMs = nowMs; // an unknown last use: the day's grace starts now, never "long ago"
                    }
                    records[r->first] = std::move(r->second);
                }
            }
        }
        if (const auto* bindsJson = v.Find(L"binds"); bindsJson && bindsJson->type == json::Value::Type::Arr)
        {
            for (const auto& b : bindsJson->arr)
            {
                if (auto bind = NewThreadBindFromJson(b))
                {
                    bind->launchToken = 0;
                    bind->answered = false;
                    bind->requestedMs = nowMs;
                    binds.push_back(std::move(*bind));
                }
            }
        }
    }
}
