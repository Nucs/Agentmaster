// Agentmaster (doc/agentmaster/BUNDLED_MODS.md): the Claude Code mods Agentmaster ships and loads into its Claude tabs.
//
// A mod is a Claude Code plugin whose hooks are a TypeScript module (Claude Code 2.1.287+). Agentmaster carries a few
// inside TerminalApp.dll (BundledModsData.h, generated from AgentMaster/mods/ by tools/bundled-mods/bundled_mods.py),
// writes each one once into the profile (<profile>\mods\<name>\<version>-<hash8>\, one immutable folder per content),
// and adds `--plugin-dir "<that folder>"` to the command line of every Claude tab it launches: a new tab, a resume, a
// fork, a restore, a restart, a handover successor (ClaudeSpawn's BuildClaudeSpawn / BuildClaudeRestartSpec, through
// the provider seam SetBundledModArgsProvider, which TerminalPage.AgentEngine.cpp sets once per process).
//
// Each launch decides per mod (ChooseBundledMods):
//   * off in the cog (AppSettings::bundledModsOff)                  -> not loaded;
//   * the claude.exe is older than the mod's minimum, or its version unreadable -> not loaded (an older claude cannot
//     run a hooks module, and one before --plugin-dir would refuse the whole command line);
//   * the user has a copy of their own: a plugin of that name enabled in the merged enabledPlugins of Claude Code's
//     user / project / local / managed settings, or a CLAUDE_CODE_PLUGIN_DIRS folder holding it -> not loaded, theirs
//     runs. Claude Code lets a --plugin-dir plugin REPLACE an installed one of the same name (2.1.310's debug log:
//     `Plugin "file-links" from --plugin-dir overrides installed version`), so ours would silently take the place of
//     the user's own, often newer copy (a working copy they edit in ~/.claude/mods);
//   * else loaded, from its folder.
// Measured live on 2.1.310 (BUNDLED_MODS.md §6): two --plugin-dir folders under a profile start to the input box with no
// dialog, both load as `<name>@inline` (tier user), and Claude Code watches them and writes .claude-plugin\types\ and a
// tsconfig.json into each. A folder plugin sits ABOVE every installed plugin in Claude Code's hook chain (the mods' own
// READMEs, "seat order"), which is why better-verbose-bash 0.1.1 hands a row another plugin drew straight on.
//
// Plain C++ (no WinRT): the pure helpers are unit-tested in the m5 harness (tests_bundled_mods.cpp).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ClaudeSpawn.h" // ClaudeFileVersion
#include "SessionModels.h" // AppSettings

namespace Agentmaster
{
    // One mod Agentmaster ships, as the generated table records it.
    struct BundledModInfo
    {
        std::wstring name; // its plugin.json name, also its folder name under <profile>\mods: L"file-links"
        std::wstring version; // its plugin.json version: L"0.4.1"
        std::wstring minClaudeCode; // the oldest Claude Code that runs it, "major.minor.patch": L"2.1.287"
        std::wstring summary; // the one line the cog's Claude tab shows for it
        std::wstring contentHash; // SHA-256 (64 hex) over its runtime files (BundledModsData.h's framing)
    };

    // The mods this build ships, in --plugin-dir order. Built once from the generated table (a function-local static,
    // so the first call from any thread builds it safely).
    // Returns the catalog; empty only in a build whose manifest lists none.
    // Throws std::bad_alloc only (the first call copies the table's strings).
    const std::vector<BundledModInfo>& BundledModCatalog();

    // One embedded file of a bundled mod.
    struct BundledModFile
    {
        std::wstring path; // under the mod's folder, '/'-separated: L"hooks/register.ts"
        std::string bytes; // its exact bytes (UTF-8, LF line ends)
    };

    // The embedded runtime files of one bundled mod, joined from the generated pieces: what its folder must hold.
    //   name - the mod's name (exact, as BundledModCatalog spells it).
    // Returns its files sorted by path; empty for a name this build does not ship. Throws std::bad_alloc only.
    std::vector<BundledModFile> BundledModFiles(std::wstring_view name);

    // PURE: the content hash of a mod's files in the generator's framing (tools/bundled-mods/bundled_mods.py
    // content_hash): for each file in the given order its UTF-8 path, '\n', its byte count in decimal, '\n', its bytes,
    // all through one SHA-256. The unit tests recompute every catalog mod's hash with it, so an edited header whose bytes
    // no longer match its recorded hash (and so its folder name) fails the suite.
    //   files - the files, sorted by path (BundledModFiles' order).
    // Returns the SHA-256 as 64 lowercase hex digits. Throws std::bad_alloc only.
    std::wstring BundledModContentHashOf(const std::vector<BundledModFile>& files);

    // The leaf name of one mod version's folder under <profile>\mods\<name>\: "<version>-<the hash's first 8 hex>".
    // A changed file gives a changed hash and so a NEW folder: a folder never changes under a running claude that loaded
    // it, and an older Agentmaster's sessions keep theirs.
    //   mod - the catalog entry.
    // Returns the leaf (L"0.4.1-8d36d97c"). Throws std::bad_alloc only.
    std::wstring BundledModFolderLeaf(const BundledModInfo& mod);

    // PURE: read "major.minor.patch" (a Claude Code version as a manifest or `claude --version` spells it; a fourth part
    // and anything after a space or '-' are ignored) as a ClaudeFileVersion.
    //   text - the version text.
    // Returns the version; nullopt when the first three parts are not all decimal numbers. Never throws.
    std::optional<ClaudeFileVersion> ParseClaudeVersionText(std::wstring_view text) noexcept;

    // PURE: is `have` at least `need`? Compares major, minor, then build (Claude Code's patch); revision is ignored, as
    // every Claude Code release stamps it 0.
    //   have - the claude.exe's version.
    //   need - the minimum.
    // Returns true when have >= need. Never throws.
    bool ClaudeVersionAtLeast(const ClaudeFileVersion& have, const ClaudeFileVersion& need) noexcept;

    // One entry of a Claude Code settings file's `enabledPlugins` object.
    struct PluginEnableEntry
    {
        std::wstring key; // as written: L"file-links@claude-mods" (or a bare L"csharp-lsp")
        std::wstring name; // the plugin's name: the key up to its last '@' (the whole key when it has none)
        bool enabled{ false }; // its value
    };

    // PURE: the `enabledPlugins` entries of one Claude Code settings file, in file order.
    //   settingsJsonText - the file's text (a UTF-8 BOM already dropped or not: json::Parse skips it).
    // Returns the entries whose value is a boolean (any other value is not Claude Code's on/off and is skipped); empty
    // for text that does not parse, is not an object, or has no `enabledPlugins` object.
    // Throws std::bad_alloc only.
    std::vector<PluginEnableEntry> ParseEnabledPlugins(std::wstring_view settingsJsonText);

    // PURE: does Claude Code itself load a plugin named `modName`, by the merged enabledPlugins of its settings scopes?
    // Claude Code merges the scopes key by key, a higher scope's value winning (local over project over user, managed
    // over all), so a user-scope `true` that a project's `false` overrides loads nothing. Names compare without case.
    //   modName         - the bundled mod's name.
    //   scopesLowToHigh - each scope's entries, lowest precedence first (user, project, local, managed).
    // Returns the key that loads it (L"file-links@claude-mods"); "" when no key with that name ends up true. A key enabled
    // for a plugin that is not installed counts too (this reads settings, not the install records).
    // Throws std::bad_alloc only.
    std::wstring EffectiveEnabledPluginKey(std::wstring_view modName, const std::vector<std::vector<PluginEnableEntry>>& scopesLowToHigh);

    // PURE: the folders a CLAUDE_CODE_PLUGIN_DIRS value names: ';'-separated (the Windows path list), each trimmed of
    // spaces and quotes, empties dropped.
    //   value - the variable's value.
    // Returns the folders in order. Throws std::bad_alloc only.
    std::vector<std::wstring> SplitPluginDirsValue(std::wstring_view value);

    // PURE: the CLAUDE_CODE_PLUGIN_DIRS a Claude Code settings file sets in its `env` block (Claude Code applies that
    // block to its own process, so folders listed there load like the variable's).
    //   settingsJsonText - the file's text.
    // Returns the value; "" when the file sets none or does not parse. Throws std::bad_alloc only.
    std::wstring PluginDirsFromSettingsEnv(std::wstring_view settingsJsonText);

    // PURE: the command-line arguments that load `folders`: ` --plugin-dir "<folder>"` per folder, forward slashes (the
    // --settings path's spelling), each quoted, with a leading space; the flag repeats, it never takes a list.
    //   folders - the mod folders, in load order.
    // Returns the arguments with a leading space, or "" for none (the command line is then byte-identical).
    // Throws std::bad_alloc only.
    std::wstring RenderPluginDirArgs(const std::vector<std::wstring>& folders);

    // A bundled mod's folder on disk.
    struct MaterializedBundledMod
    {
        std::wstring name; // the mod's name
        std::wstring folder; // its folder (backslashes); "" when it could not be written (logged)
    };

    // Write (or check) every catalog mod's folder under <stateDir>\mods. A folder that exists keeps its files when they
    // match the embedded bytes, and a file that differs or is missing is written again (a repair, logged). A new folder
    // is written whole into a temporary sibling first and renamed into place, so a crash or a second process never
    // leaves a half folder under the final name. Files Claude Code adds to a folder it loads (.claude-plugin/types/)
    // are not ours and are left alone. Never deletes anything else; older versions' folders stay.
    //   stateDir - the profile folder (AgentmasterStateDir(); a scratch folder in the tests).
    // Returns one entry per catalog mod, in catalog order. Writes [mods] lines to hooks.log for what it wrote or could
    // not write. Never throws: a failure is logged (Rule #18) and leaves that mod's folder "".
    std::vector<MaterializedBundledMod> MaterializeBundledModsIn(const std::wstring& stateDir) noexcept;

    // MaterializeBundledModsIn once per process: the first call writes (or checks) the folders, every later call
    // returns that result, whatever stateDir it passes (the profile never changes mid-run, Rule #15). Concurrent first
    // callers wait for the one writer (std::call_once).
    //   stateDir - the profile folder.
    // Returns the folders. Never throws.
    const std::vector<MaterializedBundledMod>& EnsureBundledModsMaterialized(const std::wstring& stateDir) noexcept;

    // Why a bundled mod does or does not load into one launch.
    enum class BundledModVerdict
    {
        Loads, // passed every check: --plugin-dir carries its folder
        Off, // the user turned it off in the cog's Claude tab
        ClaudeUnknown, // no claude.exe path, or its PE version could not be read: never pass a flag it may not know
        ClaudeTooOld, // the claude.exe is older than the mod's minimum
        UsersOwnCopy, // the user has their own copy (settings or CLAUDE_CODE_PLUGIN_DIRS): ours would replace it, so theirs runs
        NoFolder, // its folder could not be written into the profile
    };

    // One mod's decision for one launch.
    struct BundledModChoice
    {
        std::wstring name; // the mod
        std::wstring version; // its version
        BundledModVerdict verdict{ BundledModVerdict::Off };
        std::wstring detail; // what decided it: the user's key and its scope, the claude version, ...; "" for Loads
        std::wstring folder; // the folder to load (only for Loads, and only when materialize was true)
    };

    // Decide every catalog mod for one launch (the checks in the file comment above, in that order).
    //   stateDir       - the profile folder (where the folders are written when materialize is true).
    //   settings       - the cog settings (read fresh by the launch seam: a Save applies to the next launch).
    //   claudeLauncher - the claude.exe the launch runs (Engine::claudeExePath); "" -> ClaudeUnknown.
    //   workingDir     - the session's working folder: its .claude\settings.json and .claude\settings.local.json are
    //                    the project and local scopes; "" (the cog's preview) reads the user and managed scopes only.
    //   materialize    - true at a launch (the folders are made, EnsureBundledModsMaterialized); false for a preview,
    //                    which writes nothing and leaves `folder` empty.
    // Returns one choice per catalog mod, in catalog order. Reads the claude.exe version and the settings files through
    // caches keyed by each file's size and write time, so a launch burst costs a few attribute reads each.
    // Throws std::bad_alloc only (file and parse failures read as "nothing there").
    std::vector<BundledModChoice> ChooseBundledMods(const std::wstring& stateDir, const AppSettings& settings, std::wstring_view claudeLauncher, std::wstring_view workingDir, bool materialize);

    // One choice in words, for the cog and hooks.log: "better-verbose-bash 0.1.1 loads", "file-links 0.4.1 stands
    // aside: Claude Code loads your own file-links@claude-mods (user settings)", "... needs Claude Code 2.1.308 (this
    // claude.exe is 2.1.300)", "... is off (Settings > Claude)", ...
    //   choice - the decision.
    // Returns the sentence. Throws std::bad_alloc only.
    std::wstring DescribeBundledModChoice(const BundledModChoice& choice);

    // THE PROVIDER (ClaudeSpawn's BundledModArgsProvider): the --plugin-dir arguments for one launch, with one
    // `[mods] <session> ...` line in hooks.log saying what loads and why the rest does not.
    //   stateDir       - the profile folder.
    //   settings       - the cog settings of this launch.
    //   claudeLauncher - the claude.exe it runs.
    //   workingDir     - its working folder.
    //   sessionId      - its session id (only for the log line).
    // Returns RenderPluginDirArgs of the folders that load ("" when none do). Never throws: any failure is logged
    // (Rule #18) and the launch goes on without bundled mods.
    std::wstring BundledModPluginArgs(const std::wstring& stateDir, const AppSettings& settings, std::wstring_view claudeLauncher, std::wstring_view workingDir, std::wstring_view sessionId) noexcept;
}
