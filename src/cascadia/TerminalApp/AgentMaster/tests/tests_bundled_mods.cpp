// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 harness — the BUNDLED MODS (doc/agentmaster/BUNDLED_MODS.md, AgentMaster/BundledMods.{h,cpp}).
//
// Agentmaster ships Claude Code mods inside its binary (BundledModsData.h, generated from AgentMaster/mods/) and loads
// them into every Claude tab it launches with --plugin-dir. What is pinned here:
//   * the embedded catalog against its own bytes: each mod's plugin.json names it and its version, its hooks.json names
//     modules it carries, no test file and no CR byte rides along, and the recorded content hash (the folder's name) is
//     the hash of the embedded files — so a hand-edited or half-regenerated header fails here;
//   * the version gate's text reader and comparison;
//   * the user's-own-copy rule: enabledPlugins parsed per scope, merged key by key with the higher scope winning, names
//     compared without case; CLAUDE_CODE_PLUGIN_DIRS split and read from a settings `env` block;
//   * the --plugin-dir arguments' spelling and their place on the command line (after the base form, before --model, the
//     preset's arguments and the positional prompt; inside a `cmd /c` wrap; nothing at all when empty);
//   * the folders on disk: written whole, checked again without a rewrite, a changed or deleted file put back, no
//     temporary folder left behind;
//   * every launch decision end to end on a scratch Claude config (CLAUDE_CONFIG_DIR, ProgramFiles and
//     CLAUDE_CODE_PLUGIN_DIRS pointed at temp folders for the duration, then restored), with a real versioned
//     executable standing in for claude.exe;
//   * the spawn builders through the provider seam: no provider (every other program) or an unreadable claude version
//     -> byte-identical command lines; the app's provider -> both folders on the restart's command line and a [mods]
//     line in hooks.log.

#include "m5_tests.h"

#include "../BundledMods.h"

#include <fstream>

namespace
{
    /// <summary>Writes a small text file (UTF-8, no BOM), creating its folder.</summary>
    /// <param name="path">The file.</param>
    /// <param name="text">The content (ASCII in these tests).</param>
    /// <returns>True when written.</returns>
    bool WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out{ path, std::ios::binary | std::ios::trunc };
        out << text;
        out.close();
        return !out.fail();
    }

    /// <summary>Reads a whole file's bytes.</summary>
    /// <param name="path">The file.</param>
    /// <returns>Its bytes; empty when it cannot be read.</returns>
    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream in{ path, std::ios::binary };
        return std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
    }

    /// <summary>Decodes UTF-8 bytes (an embedded plugin.json, whose description may hold non-ASCII text).</summary>
    /// <param name="bytes">The bytes.</param>
    /// <returns>The text; empty for empty or undecodable input.</returns>
    std::wstring FromUtf8(const std::string& bytes)
    {
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0)
        {
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
        }
        return out;
    }

    /// <summary>An environment variable's value, to restore it after a test changed it.</summary>
    /// <param name="name">The variable.</param>
    /// <returns>Its value; nullopt when it is unset.</returns>
    std::optional<std::wstring> GetEnv(const wchar_t* name)
    {
        const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (need == 0)
        {
            return std::nullopt;
        }
        std::wstring buf(need, L'\0');
        const DWORD got = ::GetEnvironmentVariableW(name, buf.data(), need);
        buf.resize(got);
        return buf;
    }

    /// <summary>Sets or clears an environment variable of this process.</summary>
    /// <param name="name">The variable.</param>
    /// <param name="value">Its new value; nullopt clears it.</param>
    void SetEnv(const wchar_t* name, const std::optional<std::wstring>& value)
    {
        ::SetEnvironmentVariableW(name, value ? value->c_str() : nullptr);
    }

    /// <summary>The choice for one mod in a decision list.</summary>
    /// <param name="choices">ChooseBundledMods' answer.</param>
    /// <param name="name">The mod.</param>
    /// <returns>Its choice; a default (Off, empty name) when the list has none for it.</returns>
    BundledModChoice ChoiceFor(const std::vector<BundledModChoice>& choices, const std::wstring& name)
    {
        for (const auto& c : choices)
        {
            if (c.name == name)
            {
                return c;
            }
        }
        return {};
    }

    /// <summary>This test executable's own path: a real file with NO version resource, so it reads as a claude.exe
    /// whose version is unknown.</summary>
    /// <returns>The path.</returns>
    std::wstring OwnExePath()
    {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::wstring{ buf, n };
    }

    /// <summary>A real executable WITH a version resource that is newer than any mod's minimum (Windows' own cmd.exe,
    /// 10.0.x), standing in for a claude.exe new enough for every bundled mod. The version reader maps the image as data
    /// and never runs it.</summary>
    /// <returns>The path; empty when the system folder cannot be read.</returns>
    std::wstring VersionedExePath()
    {
        wchar_t sys[MAX_PATH]{};
        const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
        return n == 0 ? std::wstring{} : (std::wstring{ sys, n } + L"\\cmd.exe");
    }

    /// <summary>Counts the entries of a folder whose names start with a prefix.</summary>
    /// <param name="folder">The folder.</param>
    /// <param name="prefix">The name prefix.</param>
    /// <returns>How many; 0 for a missing folder.</returns>
    size_t CountNamed(const std::filesystem::path& folder, const std::wstring& prefix)
    {
        size_t n = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{ folder, ec })
        {
            if (entry.path().filename().wstring().rfind(prefix, 0) == 0)
            {
                ++n;
            }
        }
        return n;
    }
}

void TestBundledMods()
{
    std::wprintf(L"TestBundledMods\n");

    // ---- the embedded catalog against its own bytes ----
    const auto& catalog = BundledModCatalog();
    CHECK(catalog.size() == 2, "bundled: two mods ship");
    CHECK(catalog.size() == 2 && catalog[0].name == L"file-links" && catalog[1].name == L"better-verbose-bash", "bundled: file-links, then better-verbose-bash (the --plugin-dir order)");
    for (const auto& mod : catalog)
    {
        const auto files = BundledModFiles(mod.name);
        CHECK(!files.empty(), "bundled: every mod carries files");
        bool hasPlugin = false;
        bool hasHooks = false;
        bool clean = true;
        std::vector<std::wstring> modules;
        for (const auto& f : files)
        {
            clean = clean && f.bytes.find('\r') == std::string::npos && f.path.find(L".test.") == std::wstring::npos;
            if (f.path == L".claude-plugin/plugin.json")
            {
                hasPlugin = true;
                const auto root = json::Parse(FromUtf8(f.bytes));
                CHECK(root && root->StrAt(L"name") == mod.name, "bundled: plugin.json names the mod");
                CHECK(root && root->StrAt(L"version") == mod.version, "bundled: plugin.json's version is the catalog's");
            }
            if (f.path == L"hooks/hooks.json")
            {
                hasHooks = true;
                const auto root = json::Parse(FromUtf8(f.bytes));
                const auto* list = root ? root->Find(L"modules") : nullptr;
                CHECK(list && list->type == json::Value::Type::Arr && !list->arr.empty(), "bundled: hooks.json names its modules");
                if (list)
                {
                    for (const auto& m : list->arr)
                    {
                        modules.push_back(m.str);
                    }
                }
            }
        }
        CHECK(hasPlugin && hasHooks, "bundled: plugin.json and hooks/hooks.json are embedded");
        CHECK(clean, "bundled: no CR byte and no test file is embedded");
        for (const auto& m : modules)
        {
            // "./register.ts" under hooks/ -> "hooks/register.ts".
            const std::wstring want = L"hooks/" + (m.rfind(L"./", 0) == 0 ? m.substr(2) : m);
            bool found = false;
            for (const auto& f : files)
            {
                found = found || f.path == want;
            }
            CHECK(found, "bundled: every module hooks.json names is embedded");
        }
        CHECK(BundledModContentHashOf(files) == mod.contentHash, "bundled: the recorded hash is the hash of the embedded files (regenerate BundledModsData.h)");
        CHECK(mod.contentHash.size() == 64, "bundled: the hash is a full SHA-256");
        CHECK(BundledModFolderLeaf(mod) == mod.version + L"-" + mod.contentHash.substr(0, 8), "bundled: the folder leaf is <version>-<hash8>");
        CHECK(ParseClaudeVersionText(mod.minClaudeCode).has_value(), "bundled: the minimum Claude Code version reads");
        CHECK(!mod.summary.empty(), "bundled: the cog has a summary line");
    }
    CHECK(BundledModFiles(L"no-such-mod").empty(), "bundled: an unknown name carries no files");
    CHECK(BundledModContentHashOf({}) == L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "bundled: no files hash as the empty input (the framing adds nothing)");

    // ---- the version gate ----
    {
        const auto v = ParseClaudeVersionText(L"2.1.310");
        CHECK(v && v->major == 2 && v->minor == 1 && v->build == 310, "version: 2.1.310 reads");
        CHECK(ParseClaudeVersionText(L"2.1.310.0") && ParseClaudeVersionText(L"2.1.310 (Claude Code)") && ParseClaudeVersionText(L"2.1.310-beta"), "version: a fourth part and a suffix are ignored");
        CHECK(!ParseClaudeVersionText(L"2.1") && !ParseClaudeVersionText(L"") && !ParseClaudeVersionText(L"x.1.2") && !ParseClaudeVersionText(L"2..3") && !ParseClaudeVersionText(L"2.1.99999"), "version: junk does not read");
        ClaudeFileVersion a;
        a.major = 2;
        a.minor = 1;
        a.build = 308;
        ClaudeFileVersion b = a;
        CHECK(ClaudeVersionAtLeast(a, b), "version: equal is at least");
        b.build = 309;
        CHECK(!ClaudeVersionAtLeast(a, b), "version: an older patch is not");
        b.build = 1;
        b.minor = 2;
        CHECK(!ClaudeVersionAtLeast(a, b), "version: an older minor is not, whatever the patch");
        b.major = 1;
        b.minor = 9;
        b.build = 999;
        CHECK(ClaudeVersionAtLeast(a, b), "version: a newer major wins");
        a.revision = 0;
        b = a;
        b.revision = 5;
        CHECK(ClaudeVersionAtLeast(a, b), "version: the revision is ignored");
    }

    // ---- enabledPlugins, per scope and merged ----
    {
        const auto e = ParseEnabledPlugins(LR"({ "model": "opus", "enabledPlugins": { "file-links@claude-mods": true, "csharp-lsp": true, "x@y@z": false, "odd": "yes", "": true } })");
        CHECK(e.size() == 3, "plugins: boolean entries only (a string value and an empty key are skipped)");
        CHECK(e.size() == 3 && e[0].key == L"file-links@claude-mods" && e[0].name == L"file-links" && e[0].enabled, "plugins: name@marketplace");
        CHECK(e.size() == 3 && e[1].name == L"csharp-lsp", "plugins: a bare key is the name");
        CHECK(e.size() == 3 && e[2].name == L"x@y" && !e[2].enabled, "plugins: the name ends at the LAST '@'");
        CHECK(ParseEnabledPlugins(L"").empty() && ParseEnabledPlugins(L"[1,2]").empty() && ParseEnabledPlugins(LR"({"enabledPlugins": [1]})").empty() && ParseEnabledPlugins(L"{torn").empty(), "plugins: no object, no entries");

        const auto user = ParseEnabledPlugins(LR"({"enabledPlugins": {"file-links@claude-mods": true}})");
        const auto offLocal = ParseEnabledPlugins(LR"({"enabledPlugins": {"FILE-LINKS@claude-mods": false}})");
        const auto onProject = ParseEnabledPlugins(LR"({"enabledPlugins": {"file-links@icskills": true}})");
        CHECK(EffectiveEnabledPluginKey(L"file-links", { user }) == L"file-links@claude-mods", "merge: a user-scope true loads the user's copy");
        CHECK(EffectiveEnabledPluginKey(L"FILE-LINKS", { user }) == L"file-links@claude-mods", "merge: names compare without case");
        CHECK(EffectiveEnabledPluginKey(L"file-links", { user, {}, offLocal }).empty(), "merge: a higher scope's false (another case) overrides the user's true");
        CHECK(EffectiveEnabledPluginKey(L"file-links", { offLocal, onProject }) == L"file-links@icskills", "merge: another marketplace's copy enabled higher up still loads");
        CHECK(EffectiveEnabledPluginKey(L"better-verbose-bash", { user, onProject }).empty(), "merge: other plugins' keys do not count");
        CHECK(EffectiveEnabledPluginKey(L"file-links", {}).empty(), "merge: no scopes, no copy");
    }

    // ---- CLAUDE_CODE_PLUGIN_DIRS ----
    {
        const auto dirs = SplitPluginDirsValue(L" C:\\a ; ;\"C:\\b c\";C:\\d;");
        CHECK(dirs.size() == 3 && dirs[0] == L"C:\\a" && dirs[1] == L"C:\\b c" && dirs[2] == L"C:\\d", "dirs: ';'-separated, trimmed, quotes and empties dropped");
        CHECK(SplitPluginDirsValue(L"").empty(), "dirs: an empty value names none");
        CHECK(PluginDirsFromSettingsEnv(LR"({"env": {"CLAUDE_CODE_PLUGIN_DIRS": "C:\\m"}})") == L"C:\\m", "dirs: read from a settings env block");
        CHECK(PluginDirsFromSettingsEnv(LR"({"env": {"OTHER": "1"}})").empty() && PluginDirsFromSettingsEnv(L"nope").empty(), "dirs: no such block, nothing");
    }

    // ---- the arguments and their place on the command line ----
    {
        const auto args = RenderPluginDirArgs({ L"C:\\p\\mods\\a\\1", L"C:\\with space\\b" });
        CHECK(args == L" --plugin-dir \"C:/p/mods/a/1\" --plugin-dir \"C:/with space/b\"", "args: one quoted --plugin-dir per folder, forward slashes, leading space");
        CHECK(RenderPluginDirArgs({}).empty(), "args: no folders, nothing");
        const auto plain = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\bin\\claude.exe", L"claude-x", L"", L"--effort high");
        CHECK(BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\bin\\claude.exe", L"claude-x", L"", L"--effort high", L"") == plain, "args: empty plugin args keep the command line byte-identical");
        const auto withMods = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\bin\\claude.exe", L"claude-x", L"", L"--effort high", args);
        CHECK(withMods == L"\"C:\\bin\\claude.exe\" --dangerously-skip-permissions --resume abc-123 --settings \"C:/x/s.json\" --plugin-dir \"C:/p/mods/a/1\" --plugin-dir \"C:/with space/b\" --model claude-x --effort high",
              "args: after the base form, before --model and the preset's arguments");
        const auto fresh = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", false, true, L"", L"C:\\bin\\claude.exe", L"", L"first words", L"", args);
        const auto plugAt = fresh.find(L" --plugin-dir");
        const auto idAt = fresh.find(L"--session-id abc-123");
        CHECK(plugAt != std::wstring::npos && idAt != std::wstring::npos && plugAt > idAt && fresh.rfind(L"first words") > plugAt, "args: a fresh launch carries them after --session-id and before the positional prompt");
        const auto batch = BuildClaudeCommandline(L"C:/x/s.json", L"abc-123", true, true, L"", L"C:\\npm\\claude.cmd", L"", L"", L"", args);
        CHECK(batch.rfind(L"cmd /c \"", 0) == 0 && batch.back() == L'"' && batch.find(args) != std::wstring::npos, "args: they ride inside the cmd /c outer-quote wrap");
    }

    // ---- the folders on disk ----
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / (L"agentmaster-m5-bundled-mods-" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    {
        const std::wstring stateDir = (scratch / L"profile").wstring();
        const auto first = MaterializeBundledModsIn(stateDir);
        CHECK(first.size() == catalog.size(), "disk: one entry per mod");
        for (size_t i = 0; i < first.size() && i < catalog.size(); ++i)
        {
            const auto& mod = catalog[i];
            const std::filesystem::path folder = first[i].folder;
            CHECK(first[i].name == mod.name && folder == std::filesystem::path{ stateDir } / L"mods" / mod.name / BundledModFolderLeaf(mod), "disk: <profile>\\mods\\<name>\\<version>-<hash8>");
            bool exact = true;
            for (const auto& f : BundledModFiles(mod.name))
            {
                exact = exact && ReadText(folder / std::filesystem::path{ f.path }.make_preferred()) == f.bytes;
            }
            CHECK(exact, "disk: every file holds the embedded bytes");
            CHECK(CountNamed(std::filesystem::path{ stateDir } / L"mods" / mod.name, L".tmp-") == 0, "disk: no temporary folder is left behind");
        }
        if (first.size() == 2)
        {
            const std::filesystem::path fileLinks = first[0].folder;
            const auto registerTs = fileLinks / L"hooks" / L"register.ts";
            const auto before = std::filesystem::last_write_time(registerTs, ec);
            // Claude Code writes its types into a folder it loads: a file of its own must not count as damage.
            CHECK(WriteText(fileLinks / L".claude-plugin" / L"types" / L"claude-code" / L"index.d.ts", "declare module 'claude-code' {}"), "disk: (setup) a types file of Claude Code's");
            const auto again = MaterializeBundledModsIn(stateDir);
            CHECK(again.size() == 2 && again[0].folder == first[0].folder && again[1].folder == first[1].folder, "disk: a second run answers the same folders");
            CHECK(std::filesystem::last_write_time(registerTs, ec) == before, "disk: an intact folder is not rewritten");
            CHECK(std::filesystem::exists(fileLinks / L".claude-plugin" / L"types" / L"claude-code" / L"index.d.ts"), "disk: Claude Code's own files are left alone");
            // Damage: one file changed, one deleted.
            CHECK(WriteText(registerTs, "tampered"), "disk: (setup) tamper with a module");
            std::filesystem::remove(fileLinks / L"hooks" / L"links.ts", ec);
            const auto repaired = MaterializeBundledModsIn(stateDir);
            const auto files = BundledModFiles(L"file-links");
            bool restored = true;
            for (const auto& f : files)
            {
                restored = restored && ReadText(fileLinks / std::filesystem::path{ f.path }.make_preferred()) == f.bytes;
            }
            CHECK(repaired.size() == 2 && repaired[0].folder == first[0].folder && restored, "disk: a changed file and a deleted one are written back");
        }
        // A profile whose mods folder cannot be made (a FILE sits where it would go) leaves every folder "".
        const std::wstring blocked = (scratch / L"blocked").wstring();
        CHECK(WriteText(std::filesystem::path{ blocked } / L"mods", "not a folder"), "disk: (setup) a file where the mods folder goes");
        const auto none = MaterializeBundledModsIn(blocked);
        bool allEmpty = none.size() == catalog.size();
        for (const auto& m : none)
        {
            allEmpty = allEmpty && m.folder.empty();
        }
        CHECK(allEmpty, "disk: a mods folder that cannot be made gives no folder (and no throw)");
    }

    // ---- every launch decision, on a scratch Claude config ----
    {
        const auto savedConfig = GetEnv(L"CLAUDE_CONFIG_DIR");
        const auto savedProgramFiles = GetEnv(L"ProgramFiles");
        const auto savedDirs = GetEnv(L"CLAUDE_CODE_PLUGIN_DIRS");
        const std::wstring stateDir = (scratch / L"profile").wstring();
        const std::wstring claude = VersionedExePath();
        SetEnv(L"ProgramFiles", (scratch / L"programfiles").wstring()); // no managed settings
        SetEnv(L"CLAUDE_CODE_PLUGIN_DIRS", std::nullopt);
        AppSettings settings;

        /// <summary>Points CLAUDE_CONFIG_DIR at a fresh config folder of this case, with its user settings.json. Each
        /// case gets its own config folder and project folder: the text cache keys on the path and the file's stamp, and
        /// two rewrites of one file inside one clock tick could otherwise share a stamp.</summary>
        /// <param name="tag">The case's name (the folder's suffix; unique per case).</param>
        /// <param name="userSettings">The user settings.json's text; empty writes none.</param>
        auto useConfig = [&](const wchar_t* tag, const std::string& userSettings) {
            const auto dir = scratch / (std::wstring{ L"config-" } + tag);
            if (!userSettings.empty())
            {
                WriteText(dir / L"settings.json", userSettings);
            }
            SetEnv(L"CLAUDE_CONFIG_DIR", dir.wstring());
        };

        useConfig(L"none", "");
        auto c = ChooseBundledMods(stateDir, settings, claude, (scratch / L"project-none").wstring(), false);
        CHECK(c.size() == 2 && ChoiceFor(c, L"file-links").verdict == BundledModVerdict::Loads && ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::Loads, "choose: nothing of the user's -> both load");
        CHECK(ChoiceFor(c, L"file-links").folder.empty(), "choose: a preview names no folder and writes none");
        CHECK(DescribeBundledModChoice(ChoiceFor(c, L"better-verbose-bash")) == L"better-verbose-bash " + catalog[1].version + L" loads", "choose: the loaded sentence");

        useConfig(L"user", R"({"enabledPlugins": {"file-links@claude-mods": true}})");
        c = ChooseBundledMods(stateDir, settings, claude, (scratch / L"project-user").wstring(), false);
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::UsersOwnCopy && ChoiceFor(c, L"file-links").detail == L"file-links@claude-mods (user settings)", "choose: the user's installed copy -> ours stands aside, named with its scope");
        CHECK(ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::Loads, "choose: the other mod still loads");
        CHECK(DescribeBundledModChoice(ChoiceFor(c, L"file-links")).find(L"stands aside: Claude Code loads your own file-links@claude-mods (user settings)") != std::wstring::npos, "choose: the stand-aside sentence");

        useConfig(L"user2", R"({"enabledPlugins": {"file-links@claude-mods": true}})");
        const auto localOff = scratch / L"project-localoff";
        WriteText(localOff / L".claude" / L"settings.local.json", R"({"enabledPlugins": {"file-links@claude-mods": false}})");
        c = ChooseBundledMods(stateDir, settings, claude, localOff.wstring(), false);
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::Loads, "choose: the project's local false overrides the user's true -> ours loads");

        useConfig(L"user3", R"({"enabledPlugins": {"better-verbose-bash@icskills": false}})");
        const auto projOn = scratch / L"project-on";
        WriteText(projOn / L".claude" / L"settings.json", R"({"enabledPlugins": {"better-verbose-bash@icskills": true}})");
        c = ChooseBundledMods(stateDir, settings, claude, (projOn.wstring() + L"\\"), false);
        CHECK(ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::UsersOwnCopy && ChoiceFor(c, L"better-verbose-bash").detail == L"better-verbose-bash@icskills (project settings)", "choose: a project-scope true over a user false -> the project's copy (a trailing slash on the folder is fine)");

        const auto dirPlugin = scratch / L"dirplugin";
        WriteText(dirPlugin / L".claude-plugin" / L"plugin.json", R"({"name": "better-verbose-bash", "version": "9.9.9"})");
        useConfig(L"none2", "");
        SetEnv(L"CLAUDE_CODE_PLUGIN_DIRS", (scratch / L"missing").wstring() + L";" + dirPlugin.wstring());
        c = ChooseBundledMods(stateDir, settings, claude, (scratch / L"project-dirs").wstring(), false);
        CHECK(ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::UsersOwnCopy && ChoiceFor(c, L"better-verbose-bash").detail.find(L"CLAUDE_CODE_PLUGIN_DIRS") != std::wstring::npos, "choose: a CLAUDE_CODE_PLUGIN_DIRS folder holding the mod -> ours stands aside");
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::Loads, "choose: a folder that holds no plugin names nothing");
        SetEnv(L"CLAUDE_CODE_PLUGIN_DIRS", std::nullopt);

        {
            // The folder as a JSON string: its backslashes doubled (the scratch path is ASCII).
            std::string escaped;
            for (const char ch : dirPlugin.string())
            {
                escaped += ch == '\\' ? std::string{ "\\\\" } : std::string(1, ch);
            }
            useConfig(L"env", "{\"env\": {\"CLAUDE_CODE_PLUGIN_DIRS\": \"" + escaped + "\"}}");
        }
        c = ChooseBundledMods(stateDir, settings, claude, (scratch / L"project-env").wstring(), false);
        CHECK(ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::UsersOwnCopy, "choose: CLAUDE_CODE_PLUGIN_DIRS set in a settings env block counts too");

        useConfig(L"none3", "");
        AppSettings cogEnv;
        cogEnv.env = L"CLAUDE_CODE_PLUGIN_DIRS=" + dirPlugin.wstring();
        c = ChooseBundledMods(stateDir, cogEnv, claude, (scratch / L"project-cogenv").wstring(), false);
        CHECK(ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::UsersOwnCopy, "choose: CLAUDE_CODE_PLUGIN_DIRS from the cog's own env reaches the tab too");

        AppSettings off;
        off.bundledModsOff = { L"FILE-LINKS" };
        c = ChooseBundledMods(stateDir, off, claude, (scratch / L"project-off").wstring(), false);
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::Off && ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::Loads, "choose: turned off in the cog (any case) -> off; the other loads");
        CHECK(DescribeBundledModChoice(ChoiceFor(c, L"file-links")).find(L"is off") != std::wstring::npos, "choose: the off sentence");

        c = ChooseBundledMods(stateDir, settings, L"", (scratch / L"project-x").wstring(), false);
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::ClaudeUnknown && ChoiceFor(c, L"better-verbose-bash").verdict == BundledModVerdict::ClaudeUnknown, "choose: no claude.exe -> no mod");
        c = ChooseBundledMods(stateDir, settings, OwnExePath(), (scratch / L"project-x").wstring(), false);
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::ClaudeUnknown && ChoiceFor(c, L"file-links").detail.find(L"no version could be read") != std::wstring::npos, "choose: a claude.exe without a version -> no mod (never a flag it may not know)");
        c = ChooseBundledMods(stateDir, settings, L"C:\\no\\such\\claude.exe", (scratch / L"project-x").wstring(), false);
        CHECK(ChoiceFor(c, L"file-links").verdict == BundledModVerdict::ClaudeUnknown, "choose: a missing claude.exe -> no mod");

        // ---- the spawn builders through the provider seam ----
        useConfig(L"builders", "");
        AppSettings rst;
        const std::wstring rid = L"5b0c3d1e-0000-4000-8000-00000000b0dd";
        const auto noProvider = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", rid, rst, claude);
        CHECK(noProvider.commandline.find(L"--plugin-dir") == std::wstring::npos, "seam: no provider (every program but the app) -> no --plugin-dir");
        SetBundledModArgsProvider(&BundledModPluginArgs);
        const auto fakeClaude = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", rid, rst, L"C:\\bin\\claude.exe");
        CHECK(fakeClaude.commandline.find(L"--plugin-dir") == std::wstring::npos, "seam: the provider with an unreadable claude version -> no --plugin-dir (the pinned command lines elsewhere stay byte-identical)");
        const auto withMods = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", rid, rst, claude);
        const auto& folders = EnsureBundledModsMaterialized(AgentmasterStateDir());
        bool both = folders.size() == 2;
        for (const auto& m : folders)
        {
            both = both && !m.folder.empty() && withMods.commandline.find(L"--plugin-dir \"" + ToForwardSlashes(m.folder) + L"\"") != std::wstring::npos;
        }
        CHECK(both, "seam: the app's provider puts both folders on a restart's command line");
        const auto settingsAt = withMods.commandline.find(L"--settings \"");
        CHECK(settingsAt != std::wstring::npos && withMods.commandline.find(L"--plugin-dir") > settingsAt, "seam: after the base form");
        const std::string log = ReadText(std::filesystem::path{ AgentmasterStateDir() } / L"hooks.log");
        CHECK(log.find("[mods] 5b0c3d1e") != std::string::npos && log.find("file-links") != std::string::npos, "seam: each launch logs a [mods] line naming what loads");
        SetBundledModArgsProvider(nullptr);
        const auto cleared = BuildClaudeRestartSpec(L"K:/work/api", L"api", L"\\\\.\\pipe\\agentmaster.42", rid, rst, claude);
        CHECK(cleared.commandline.find(L"--plugin-dir") == std::wstring::npos, "seam: a cleared provider loads nothing again");

        SetEnv(L"CLAUDE_CONFIG_DIR", savedConfig);
        SetEnv(L"ProgramFiles", savedProgramFiles);
        SetEnv(L"CLAUDE_CODE_PLUGIN_DIRS", savedDirs);
    }
    std::filesystem::remove_all(scratch, ec);
}
