// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the LAUNCHER's pure rules (TAB_STRIP.md "The + button: the launcher", SIDE_PANEL.md §2e, AGENT_MANAGER.md
// "The launch row"). Header-only and std-only, so the m5 harness tests it (TestLauncherRules, tests_sidepanel.cpp) and the
// plain headers that describe a launcher (AgentLaunchBar.h, AgentManagerContent.h, TerminalPage.h) can include it without
// pulling WinRT or Windows in.
//
// WHAT a launcher is since the user's picks of 2026-11-14: ONE launch surface for Claude, Codex AND the Windows Terminal
// shells (PowerShell, Command Prompt, WSL, SSH, ...). The kind is chosen on the launch row's KIND CHIP, whose click opens a
// menu — Claude, Codex, then every profile of Windows Terminal's new-tab menu — "on all menus of this kind": the tab strip's
// + (which opens the launcher card, the ▾ beside it is gone), the side panel's + (the same card) and the Agent Manager's
// launch row. This header holds the decisions those surfaces share and that need no XAML:
//   - the KIND a launch starts, and the persisted TOKEN of a pick (AppSettings::launcherKind — the card remembers the last
//     kind picked; "claude" / "codex" / "profile:{guid}");
//   - the SHELL CHOICES tree the host builds from Windows Terminal's new-tab menu, and the lookups over it (a remembered
//     profile that is gone falls back to Claude);
//   - which of the card's rows show (the MODEL / PRESET pills only for a NEW Claude session);
//   - the folder a + opens the launcher on (the focused tab's: the user's pick "Focused tab's folder");
//   - the Launch button's words per kind.
// Nothing here throws but std::bad_alloc (string building); every function documents it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cwctype> // towlower — GUIDs compare case-insensitively
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // What a launch starts. The launch row's KIND CHIP picks it (AgentLaunchBarHost::kindMenu hosts); a host without the
    // chip menu (the Agent Manager's old Claude <-> Codex toggle, a Claude-only chip, the folder-pick card) only ever uses
    // the first two.
    enum class LaunchKind : uint8_t
    {
        Claude, // a managed Claude Code session — the default of every launcher
        Codex, // a managed Codex session: directory only (no session id to resume or fork from the box)
        Shell, // a Windows Terminal PROFILE in the box's folder (PowerShell, Command Prompt, WSL, SSH, ...): a plain shell tab
    };

    // One entry of the kind chip's shell section: the host flattens Windows Terminal's new-tab menu (the user's newTabMenu:
    // remaining profiles, matched profiles, folders such as the "SSH" one, separators) into this tree each time the menu
    // opens, so a profile added in Windows Terminal's settings shows at once. Actions of the new-tab menu are left out: they
    // start no shell.
    struct LaunchShellChoice
    {
        enum class Type : uint8_t
        {
            Profile, // a profile: name, profileGuid, icon, isDefault
            Separator, // a line between groups (nothing else is read)
            Folder, // a submenu: name, icon, children
        };
        Type type{ Type::Profile };
        // The profile's or folder's name as Windows Terminal shows it ("PowerShell", "SSH").
        std::wstring name;
        // A profile's GUID in its braced string form ("{574e775e-4f2a-5b96-ac1e-a2962a402336}") — what Windows Terminal's
        // NewTerminalArgs::Profile takes, and the persisted token's payload. Empty for a separator or a folder.
        std::wstring profileGuid;
        // The resolved icon source (a path, a URI or a glyph) for the host's icon factory; "" = no icon.
        std::wstring icon;
        // Windows Terminal's DEFAULT profile (the one a plain new tab opens). Drawn bold in the menu, as Windows Terminal's own
        // menu draws it, and the one a remembered profile falls back to never — a gone profile falls back to Claude.
        bool isDefault{ false };
        // A folder's entries (never empty: the host drops an empty folder, as Windows Terminal's menu does).
        std::vector<LaunchShellChoice> children;
    };

    // A launcher's kind pick: the kind, plus the profile for a shell.
    struct LaunchKindPick
    {
        LaunchKind kind{ LaunchKind::Claude };
        std::wstring profileGuid; // Shell only: the profile's braced GUID; empty otherwise
    };

    // The persisted spellings (AppSettings::launcherKind). A word, never a number, so a reordered enum can never remap a
    // saved pick.
    inline constexpr std::wstring_view kLaunchKindTokenClaude = L"claude";
    inline constexpr std::wstring_view kLaunchKindTokenCodex = L"codex";
    inline constexpr std::wstring_view kLaunchKindTokenProfilePrefix = L"profile:";

    // A pick as its persisted token: "claude", "codex", or "profile:" + the GUID. A shell pick with no GUID (never made by
    // the chip) is written as "claude", so a token never names an empty profile.
    //   pick    - the pick.
    //   returns - the token.
    //   throws  - std::bad_alloc.
    inline std::wstring FormatLaunchKindToken(const LaunchKindPick& pick)
    {
        switch (pick.kind)
        {
        case LaunchKind::Codex:
            return std::wstring{ kLaunchKindTokenCodex };
        case LaunchKind::Shell:
            if (!pick.profileGuid.empty())
            {
                return std::wstring{ kLaunchKindTokenProfilePrefix } + pick.profileGuid;
            }
            return std::wstring{ kLaunchKindTokenClaude };
        case LaunchKind::Claude:
        default:
            return std::wstring{ kLaunchKindTokenClaude };
        }
    }

    // A token back into a pick. Case-insensitive for the two words and the prefix, surrounding blanks ignored; anything
    // else — empty, unknown, "profile:" with nothing after it — is Claude, the launcher's default, so a damaged or future
    // value can never leave a launcher without a kind.
    //   token   - the persisted text.
    //   returns - the pick (the GUID kept as written, trimmed).
    //   throws  - std::bad_alloc.
    inline LaunchKindPick ParseLaunchKindToken(std::wstring_view token)
    {
        const auto isBlank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (!token.empty() && isBlank(token.front()))
        {
            token.remove_prefix(1);
        }
        while (!token.empty() && isBlank(token.back()))
        {
            token.remove_suffix(1);
        }
        const auto startsWithFolded = [](std::wstring_view text, std::wstring_view prefix) {
            if (text.size() < prefix.size())
            {
                return false;
            }
            for (size_t i = 0; i < prefix.size(); ++i)
            {
                if (::towlower(text[i]) != ::towlower(prefix[i]))
                {
                    return false;
                }
            }
            return true;
        };
        LaunchKindPick pick;
        if (token.size() == kLaunchKindTokenCodex.size() && startsWithFolded(token, kLaunchKindTokenCodex))
        {
            pick.kind = LaunchKind::Codex;
        }
        else if (startsWithFolded(token, kLaunchKindTokenProfilePrefix))
        {
            auto guid = token.substr(kLaunchKindTokenProfilePrefix.size());
            while (!guid.empty() && isBlank(guid.front()))
            {
                guid.remove_prefix(1);
            }
            if (!guid.empty())
            {
                pick.kind = LaunchKind::Shell;
                pick.profileGuid = std::wstring{ guid };
            }
        }
        return pick; // "claude", empty, unknown: Claude
    }

    // Are two profile GUIDs the same profile? Case-insensitive, and the braces are optional on either side (Windows
    // Terminal writes "{...}", a hand-edited settings file may not). Two empty strings are NOT a match: an empty GUID names
    // no profile.
    //   a, b    - the GUID strings.
    //   returns - true for the same GUID.
    //   throws  - nothing.
    inline bool LaunchGuidEquals(std::wstring_view a, std::wstring_view b) noexcept
    {
        const auto strip = [](std::wstring_view g) {
            if (g.size() >= 2 && g.front() == L'{' && g.back() == L'}')
            {
                g = g.substr(1, g.size() - 2);
            }
            return g;
        };
        a = strip(a);
        b = strip(b);
        if (a.empty() || a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (::towlower(a[i]) != ::towlower(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    // The profile entry with that GUID anywhere in the tree (folders searched depth first, in menu order).
    //   choices - the tree.
    //   guid    - the GUID (braces optional).
    //   returns - the entry, or nullptr when no profile has it (removed, hidden, or never there). The pointer lives as long as
    //             `choices` is unchanged.
    //   throws  - nothing.
    inline const LaunchShellChoice* FindLaunchShellProfile(const std::vector<LaunchShellChoice>& choices, std::wstring_view guid) noexcept
    {
        for (const auto& choice : choices)
        {
            if (choice.type == LaunchShellChoice::Type::Profile && LaunchGuidEquals(choice.profileGuid, guid))
            {
                return &choice;
            }
            if (choice.type == LaunchShellChoice::Type::Folder)
            {
                if (const auto* const inner = FindLaunchShellProfile(choice.children, guid))
                {
                    return inner;
                }
            }
        }
        return nullptr;
    }

    // The tree's default profile (isDefault), else its first profile in menu order — what a SHELL launch with no profile of
    // its own would open. Not used for a remembered pick whose profile is gone (that one falls back to Claude).
    //   choices - the tree.
    //   returns - the entry, or nullptr for a tree without any profile.
    //   throws  - nothing.
    inline const LaunchShellChoice* DefaultLaunchShellProfile(const std::vector<LaunchShellChoice>& choices) noexcept
    {
        const LaunchShellChoice* first = nullptr;
        const auto walk = [&first](const auto& self, const std::vector<LaunchShellChoice>& level) -> const LaunchShellChoice* {
            for (const auto& choice : level)
            {
                if (choice.type == LaunchShellChoice::Type::Profile)
                {
                    if (choice.isDefault)
                    {
                        return &choice;
                    }
                    if (!first)
                    {
                        first = &choice;
                    }
                }
                else if (choice.type == LaunchShellChoice::Type::Folder)
                {
                    if (const auto* const found = self(self, choice.children))
                    {
                        return found;
                    }
                }
            }
            return nullptr;
        };
        if (const auto* const isDefault = walk(walk, choices))
        {
            return isDefault;
        }
        return first;
    }

    // How many profiles the tree offers (folders counted through, separators and folders themselves not).
    //   choices - the tree.
    //   returns - the count.
    //   throws  - nothing.
    inline size_t CountLaunchShellProfiles(const std::vector<LaunchShellChoice>& choices) noexcept
    {
        size_t n = 0;
        for (const auto& choice : choices)
        {
            if (choice.type == LaunchShellChoice::Type::Profile)
            {
                ++n;
            }
            else if (choice.type == LaunchShellChoice::Type::Folder)
            {
                n += CountLaunchShellProfiles(choice.children);
            }
        }
        return n;
    }

    // A remembered pick checked against what the host offers NOW: a shell pick whose profile is no longer in the tree (the
    // profile was deleted or hidden in Windows Terminal) becomes Claude — never another profile, which would open a shell
    // the user did not pick — and a Codex pick on a host that cannot start Codex becomes Claude too.
    //   wanted     - the remembered pick (ParseLaunchKindToken).
    //   choices    - the shells the host offers now.
    //   codexReady - the host can start Codex (AgentLaunchBarHost::spawnCodex is set).
    //   shellReady - the host can start a shell (AgentLaunchBarHost::spawnShell is set).
    //   returns    - the pick to apply (the GUID respelled as the tree has it, so a token written next time matches).
    //   throws     - std::bad_alloc.
    inline LaunchKindPick ResolveLaunchKindPick(const LaunchKindPick& wanted, const std::vector<LaunchShellChoice>& choices, bool codexReady, bool shellReady)
    {
        LaunchKindPick out;
        if (wanted.kind == LaunchKind::Codex && codexReady)
        {
            out.kind = LaunchKind::Codex;
        }
        else if (wanted.kind == LaunchKind::Shell && shellReady)
        {
            if (const auto* const profile = FindLaunchShellProfile(choices, wanted.profileGuid))
            {
                out.kind = LaunchKind::Shell;
                out.profileGuid = profile->profileGuid;
            }
        }
        return out;
    }

    // Does the launcher card show its MODEL and PRESET rows? Only while its Launch starts a NEW Claude session: a model or a
    // preset means nothing to Codex or a shell here, and a session id in the box resumes (or forks) that session on its own
    // launch snapshot — a pick would be ignored, so the rows step aside instead of lying.
    //   kind              - the launcher's kind.
    //   boxHoldsSessionId - the box's text is a session id (AgentLaunchBar::BoxHoldsSessionId).
    //   returns           - true = show the rows.
    //   throws            - nothing.
    inline bool LauncherShowsModelRows(LaunchKind kind, bool boxHoldsSessionId) noexcept
    {
        return kind == LaunchKind::Claude && !boxHoldsSessionId;
    }

    // The folder a + opens the launcher on — the user's pick of 2026-11-14, "Focused tab's folder": the folder of the tab you
    // are on. In order: a managed session's EFFECTIVE working folder (EffectiveWorkingDir: the inferred folder while the
    // session infers, else its cwd); a shell's folder as its shell integration reports it (OSC 9;9); a shell's folder as the
    // Fleet Observer read it out of band (the window restore's source too); and when the focused tab names none (the Agent
    // Manager page is up, a fresh shell with neither), the shared launch box — what the side panel's + opened on before.
    // Each candidate is taken as given (the caller checked it is a folder); surrounding blanks make a candidate empty.
    //   sessionFolder    - the focused tab's managed session's effective folder, or "".
    //   shellOscCwd      - the focused shell's reported folder, or "".
    //   shellObservedCwd - the focused shell's observed folder, or "".
    //   sharedBoxText    - the Agent Manager's launch box text (the fallback), or "".
    //   returns          - the first non-blank candidate, unchanged; "" when all are blank.
    //   throws           - std::bad_alloc.
    inline std::wstring PickLauncherFolder(std::wstring_view sessionFolder, std::wstring_view shellOscCwd, std::wstring_view shellObservedCwd, std::wstring_view sharedBoxText)
    {
        const auto blank = [](std::wstring_view s) {
            for (const auto c : s)
            {
                if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
                {
                    return false;
                }
            }
            return true;
        };
        for (const auto candidate : { sessionFolder, shellOscCwd, shellObservedCwd, sharedBoxText })
        {
            if (!blank(candidate))
            {
                return std::wstring{ candidate };
            }
        }
        return {};
    }

    // The Launch button's words for a NEW launch of `kind` (a session id in the box reads "Resume session" instead — the
    // bar's own rule). Claude and Codex keep the words they always had; a shell OPENS (it is not a session that runs):
    // "Open PowerShell", "Create & Open Command Prompt".
    //   kind      - the launcher's kind.
    //   shellName - the picked profile's name (Shell only); an empty name reads "Open shell".
    //   create    - the box names a folder that does not exist yet and the launch will create it.
    //   returns   - the label.
    //   throws    - std::bad_alloc.
    inline std::wstring LaunchButtonLabel(LaunchKind kind, std::wstring_view shellName, bool create)
    {
        switch (kind)
        {
        case LaunchKind::Codex:
            return create ? L"Create & Launch Codex" : L"Launch Codex";
        case LaunchKind::Shell:
        {
            std::wstring label = create ? L"Create & Open " : L"Open ";
            label += shellName.empty() ? std::wstring_view{ L"shell" } : shellName;
            return label;
        }
        case LaunchKind::Claude:
        default:
            return create ? L"Create & Launch Claude" : L"Launch Claude";
        }
    }
}
