// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the BUNDLED MODS (doc/agentmaster/BUNDLED_MODS.md): the Claude Code mods Agentmaster ships, written
// once into the profile and loaded into every Claude tab it launches with --plugin-dir. See BundledMods.h for the
// rules each launch applies. Plain C++ + Win32 (no WinRT, no PCH): TerminalAppLib and the m5 harness compile this file;
// the other link sets of ClaudeSpawn.cpp (the CLI, the session database, the probes) never do, because the spawn
// builders reach it only through the provider TerminalPage.AgentEngine.cpp sets (SetBundledModArgsProvider).

#include "BundledMods.h"

#include "BundledModsData.h" // the embedded mods (generated; this is the one unit that includes it)
#include "ClaudeSpawn.h" // ReadClaudeFileVersion, ResolveSessionEnv, ToForwardSlashes, AppendStateLog, ShortId, LogSwallowedException
#include "Json.h"
#include "Persistence.h" // ClaudeUserSettingsPath
#include "Sha256.h"
#include "StateFileGuard.h" // ReadStateFileUtf8

#include <windows.h>

#include <climits> // INT_MAX (the Win32 UTF conversion takes int lengths)
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator> // std::size
#include <mutex>
#include <unordered_map>

namespace Agentmaster
{
    namespace
    {
        namespace fs = std::filesystem;

        // How long a launch waits on a Claude Code settings file that is locked (Claude Code saving /config holds it for
        // an instant) before it reads the file as absent. Short, because the launch runs on a UI thread; a file read as
        // absent can at worst load a bundled mod beside the user's own copy for that one launch.
        constexpr unsigned kSettingsReadBudgetMs = 100;

        // At most this many settings / plugin.json texts stay cached; past it the cache starts over (each launch reads
        // at most four settings files and the CLAUDE_CODE_PLUGIN_DIRS folders, so this only bounds a long run's growth).
        constexpr size_t kTextCacheLimit = 64;

        // The scopes of Claude Code's settings, lowest precedence first, as ChooseBundledMods reads them.
        constexpr const wchar_t* kScopeNames[] = { L"user settings", L"project settings", L"local project settings", L"managed settings" };

        // An ASCII-lowercased copy, for comparing plugin names (ASCII kebab-case) and as a path cache key (a non-ASCII
        // case variant of a path then just misses the cache, never matches a wrong entry).
        //   s - the text.
        // Returns the copy. Throws std::bad_alloc only.
        std::wstring Lower(std::wstring_view s)
        {
            std::wstring out{ s };
            for (auto& c : out)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return out;
        }

        // Whether two plugin names are the same plugin: Claude Code's names are ASCII, compared here without case so a
        // hand-typed key in another case still counts as the user's copy.
        //   a, b - the names.
        // Returns true when they match. Throws std::bad_alloc only.
        bool SameName(std::wstring_view a, std::wstring_view b)
        {
            return a.size() == b.size() && Lower(a) == Lower(b);
        }

        // The text without the given characters at either end.
        //   s     - the text.
        //   chars - the characters to strip.
        // Returns the trimmed view (into s). Never throws.
        std::wstring_view Trim(std::wstring_view s, std::wstring_view chars) noexcept
        {
            const size_t a = s.find_first_not_of(chars);
            if (a == std::wstring_view::npos)
            {
                return {};
            }
            const size_t b = s.find_last_not_of(chars);
            return s.substr(a, b - a + 1);
        }

        // A file's size and last write time: the key every cache here is invalidated by (an installer replacing
        // claude.exe in place, a hand edit of settings.json, Claude Code saving /config).
        struct FileStamp
        {
            uint64_t size{};
            uint64_t write{};
            bool operator==(const FileStamp& o) const noexcept { return size == o.size && write == o.write; }
        };

        // Read a regular file's stamp.
        //   path - the file.
        // Returns the stamp; nullopt when the path does not exist, is a folder, or cannot be queried. Never throws.
        std::optional<FileStamp> StampOf(const std::wstring& path) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return std::nullopt;
            }
            FileStamp s;
            s.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            s.write = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
            return s;
        }

        // The text of a small file (a Claude Code settings file, a plugin.json), cached by its stamp, so a burst of
        // launches (the restore wake) parses each file once. Thread-safe: every UI thread may launch.
        //   path - the file.
        // Returns its text (UTF-16, a BOM dropped); nullopt when it is missing, a folder, or unreadable within
        // kSettingsReadBudgetMs (ReadStateFileUtf8 logs an unreadable one).
        // Throws std::bad_alloc only.
        std::optional<std::wstring> CachedFileText(const std::wstring& path)
        {
            const auto stamp = StampOf(path);
            if (!stamp)
            {
                return std::nullopt;
            }
            static std::mutex s_mtx;
            static std::unordered_map<std::wstring, std::pair<FileStamp, std::wstring>> s_cache;
            const std::wstring key = Lower(path);
            {
                std::lock_guard lk{ s_mtx };
                if (const auto it = s_cache.find(key); it != s_cache.end() && it->second.first == *stamp)
                {
                    return it->second.second;
                }
            }
            // The read itself is outside the lock: a locked file's retries must not hold up another thread's launch.
            const auto fileRead = ReadStateFileUtf8(path, kSettingsReadBudgetMs);
            if (fileRead.status != StateFileStatus::Read)
            {
                return std::nullopt;
            }
            std::lock_guard lk{ s_mtx };
            if (s_cache.size() >= kTextCacheLimit)
            {
                s_cache.clear();
            }
            s_cache[key] = { *stamp, fileRead.text };
            return fileRead.text;
        }

        // The claude.exe's PE version, cached by the file's stamp (ReadClaudeFileVersion maps the ~300 MB image; a
        // launch burst then pays one attribute read per launch). The native installer replaces claude.exe in place on an
        // update, and the new write time reads the new version.
        //   exe - the claude.exe path.
        // Returns the version; nullopt when the file is missing or carries no VERSIONINFO. Thread-safe.
        // Throws std::bad_alloc only.
        std::optional<ClaudeFileVersion> CachedClaudeVersion(std::wstring_view exe)
        {
            const std::wstring path{ exe };
            const auto stamp = StampOf(path);
            if (!stamp)
            {
                return std::nullopt;
            }
            static std::mutex s_mtx;
            static std::wstring s_path;
            static FileStamp s_stamp{};
            static std::optional<ClaudeFileVersion> s_version;
            static bool s_valid = false;
            std::lock_guard lk{ s_mtx };
            // Filesystem-aware (Rule #8): case-variant spellings of one path are one file.
            if (s_valid && s_stamp == *stamp && ::CompareStringOrdinal(s_path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                return s_version;
            }
            s_version = ReadClaudeFileVersion(path);
            s_path = path;
            s_stamp = *stamp;
            s_valid = true;
            return s_version;
        }

        // A version as "major.minor.patch".
        //   v - the version.
        // Returns the text. Throws std::bad_alloc only.
        std::wstring VersionText(const ClaudeFileVersion& v)
        {
            return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.build);
        }

        // An environment variable of THIS process (what a tab inherits before its spec's own variables).
        //   name - the variable.
        // Returns its value; "" when it is unset or empty. Throws std::bad_alloc only.
        std::wstring ProcessEnv(const wchar_t* name)
        {
            const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
            if (need == 0)
            {
                return {};
            }
            std::wstring buf(need, L'\0');
            const DWORD got = ::GetEnvironmentVariableW(name, buf.data(), need);
            if (got == 0 || got >= need)
            {
                return {};
            }
            buf.resize(got);
            return buf;
        }

        // Claude Code's MANAGED settings file on Windows: %ProgramFiles%\ClaudeCode\managed-settings.json (the path the
        // 2.1.310 binary names; managed settings may also come from a registry key, which is not read here).
        // Returns the path; "" when %ProgramFiles% is unset. Throws std::bad_alloc only.
        std::wstring ManagedSettingsPath()
        {
            const std::wstring pf = ProcessEnv(L"ProgramFiles");
            return pf.empty() ? std::wstring{} : (pf + L"\\ClaudeCode\\managed-settings.json");
        }

        // The name a plugin folder's own plugin.json declares.
        //   folder - the plugin's folder (as CLAUDE_CODE_PLUGIN_DIRS names it).
        // Returns the name; "" when the folder holds no readable .claude-plugin\plugin.json with a string name.
        // Throws std::bad_alloc only.
        std::wstring PluginNameInFolder(const std::wstring& folder)
        {
            std::wstring base = folder;
            while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
            {
                base.pop_back();
            }
            const auto text = CachedFileText(base + L"\\.claude-plugin\\plugin.json");
            if (!text)
            {
                return {};
            }
            const auto root = json::Parse(*text);
            return (root && root->type == json::Value::Type::Obj) ? root->StrAt(L"name") : std::wstring{};
        }

        // UTF-16 to UTF-8 (the hash framing's paths).
        //   s - the text.
        // Returns the UTF-8 bytes; "" for empty or unconvertible text. Throws std::bad_alloc only.
        std::string ToUtf8(std::wstring_view s)
        {
            if (s.empty() || s.size() > static_cast<size_t>(INT_MAX))
            {
                return {};
            }
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            if (n <= 0)
            {
                return {};
            }
            std::string out(static_cast<size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        // Find a mod's generated record.
        //   name - the mod's name (exact).
        // Returns the record; nullptr for a name this build does not ship. Never throws.
        const BundledModsData::Mod* FindModData(std::wstring_view name) noexcept
        {
            for (size_t i = 0; i < BundledModsData::kModCount; ++i)
            {
                if (BundledModsData::kMods[i].name == name)
                {
                    return &BundledModsData::kMods[i];
                }
            }
            return nullptr;
        }

        // Read a whole file's bytes.
        //   path - the file.
        // Returns its bytes; nullopt when it cannot be opened or read. Never throws (an allocation failure reads as
        // unreadable, which makes the caller rewrite the file).
        std::optional<std::string> ReadBytes(const fs::path& path) noexcept
        {
            try
            {
                std::ifstream in{ path, std::ios::binary };
                if (!in)
                {
                    return std::nullopt;
                }
                std::string bytes{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
                if (in.bad())
                {
                    return std::nullopt;
                }
                return bytes;
            }
            catch (...)
            {
                LogSwallowedException(L"BundledMods ReadBytes");
                return std::nullopt;
            }
        }

        // Write a whole file (created, or truncated and rewritten).
        //   path  - the file; its folder must exist.
        //   bytes - the content.
        // Returns true when every byte was written and the file closed cleanly. Never throws.
        bool WriteBytes(const fs::path& path, std::string_view bytes) noexcept
        {
            try
            {
                std::ofstream out{ path, std::ios::binary | std::ios::trunc };
                if (!out)
                {
                    return false;
                }
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.close();
                return !out.fail();
            }
            catch (...)
            {
                LogSwallowedException(L"BundledMods WriteBytes");
                return false;
            }
        }

        // Does a folder hold every file of a mod with the exact bytes?
        //   folder - the mod's folder.
        //   files  - its embedded files.
        // Returns true when each one reads back equal (extra files, such as the .claude-plugin\types\ Claude Code writes
        // into a folder it loads, do not matter). Never throws.
        bool FolderMatches(const fs::path& folder, const std::vector<BundledModFile>& files) noexcept
        {
            try
            {
                for (const auto& f : files)
                {
                    const auto have = ReadBytes(folder / fs::path{ f.path }.make_preferred());
                    if (!have || *have != f.bytes)
                    {
                        return false;
                    }
                }
                return true;
            }
            catch (...)
            {
                LogSwallowedException(L"BundledMods FolderMatches");
                return false;
            }
        }

        // Remove what WriteModFolder wrote into a temporary folder: the files it wrote, then the folders it created,
        // deepest first. Never a recursive delete: a folder that is not empty (something else wrote into it) stays.
        //   files   - the files written, in order.
        //   folders - the folders created, in creation order (parents first).
        // Never throws.
        void RemoveWritten(const std::vector<fs::path>& files, const std::vector<fs::path>& folders) noexcept
        {
            std::error_code ec;
            for (const auto& f : files)
            {
                fs::remove(f, ec);
            }
            for (auto it = folders.rbegin(); it != folders.rend(); ++it)
            {
                fs::remove(*it, ec); // removes a folder only when it is empty
            }
        }

        // Write one mod's files into a new folder (which must not exist yet).
        //   folder  - the folder to create and fill.
        //   files   - the mod's embedded files.
        //   written - receives every file written, in order (for RemoveWritten).
        //   created - receives every folder created, parents first (for RemoveWritten).
        // Returns true when every folder and file was written. Never throws.
        bool WriteModFolder(const fs::path& folder, const std::vector<BundledModFile>& files, std::vector<fs::path>& written, std::vector<fs::path>& created) noexcept
        {
            try
            {
                std::error_code ec;
                if (!fs::create_directory(folder, ec))
                {
                    return false; // exists already (a stale temp name) or cannot be made: the caller gives up on this run
                }
                created.push_back(folder);
                for (const auto& f : files)
                {
                    const fs::path target = folder / fs::path{ f.path }.make_preferred();
                    // Create the file's folders one level at a time, so each one we made is known and removable alone.
                    std::vector<fs::path> chain;
                    for (fs::path p = target.parent_path(); p != folder && !p.empty(); p = p.parent_path())
                    {
                        chain.push_back(p);
                    }
                    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                    {
                        if (fs::create_directory(*it, ec))
                        {
                            created.push_back(*it);
                        }
                        else if (ec)
                        {
                            return false;
                        }
                    }
                    if (!WriteBytes(target, f.bytes))
                    {
                        return false;
                    }
                    written.push_back(target);
                }
                return true;
            }
            catch (...)
            {
                LogSwallowedException(L"BundledMods WriteModFolder");
                return false;
            }
        }

        // Write (or check and repair) one mod's folder under <stateDir>\mods (MaterializeBundledModsIn's per-mod step).
        //   stateDir - the profile folder.
        //   mod      - the catalog entry.
        // Returns the folder; "" when it could not be written or repaired (logged). Never throws.
        std::wstring MaterializeOne(const std::wstring& stateDir, const BundledModInfo& mod) noexcept
        {
            try
            {
                const auto files = BundledModFiles(mod.name);
                const fs::path parent = fs::path{ stateDir } / L"mods" / mod.name;
                const fs::path finalDir = parent / BundledModFolderLeaf(mod);
                std::error_code ec;
                if (fs::is_directory(finalDir, ec))
                {
                    // The folder of this exact content exists (an earlier run wrote it). Check each file and write back any
                    // one that differs or is gone (an edit, a cleanup tool): a broken mod must never load.
                    size_t repaired = 0;
                    for (const auto& f : files)
                    {
                        const fs::path target = finalDir / fs::path{ f.path }.make_preferred();
                        const auto have = ReadBytes(target);
                        if (have && *have == f.bytes)
                        {
                            continue;
                        }
                        fs::create_directories(target.parent_path(), ec);
                        if (!WriteBytes(target, f.bytes))
                        {
                            AppendStateLog(L"hooks.log", L"[mods] " + mod.name + L" " + mod.version + L": could not repair " + target.wstring() + L" - not loaded this run\n");
                            return {};
                        }
                        ++repaired;
                    }
                    if (repaired > 0)
                    {
                        AppendStateLog(L"hooks.log", L"[mods] " + mod.name + L" " + mod.version + L": rewrote " + std::to_wstring(repaired) + L" changed or missing file(s) in " + finalDir.wstring() + L"\n");
                    }
                    return finalDir.wstring();
                }

                // A new folder: written whole under a temporary name, then renamed into place, so the final name never
                // holds half a mod (a crash mid-write, a second Agentmaster process on this profile writing too).
                fs::create_directories(parent, ec);
                if (ec)
                {
                    AppendStateLog(L"hooks.log", L"[mods] " + mod.name + L" " + mod.version + L": could not create " + parent.wstring() + L" (" + std::to_wstring(ec.value()) + L") - not loaded this run\n");
                    return {};
                }
                const fs::path temp = parent / (L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64()));
                std::vector<fs::path> written;
                std::vector<fs::path> created;
                if (!WriteModFolder(temp, files, written, created))
                {
                    RemoveWritten(written, created);
                    AppendStateLog(L"hooks.log", L"[mods] " + mod.name + L" " + mod.version + L": could not write its files under " + temp.wstring() + L" - not loaded this run\n");
                    return {};
                }
                fs::rename(temp, finalDir, ec);
                if (ec)
                {
                    // The rename fails when the final folder appeared meanwhile: another process won the race, and its
                    // folder holds the same content (the name is the content's hash), so use it once it checks out.
                    RemoveWritten(written, created);
                    if (FolderMatches(finalDir, files))
                    {
                        return finalDir.wstring();
                    }
                    AppendStateLog(L"hooks.log", L"[mods] " + mod.name + L" " + mod.version + L": could not move its folder into place at " + finalDir.wstring() + L" (" + std::to_wstring(ec.value()) + L") - not loaded this run\n");
                    return {};
                }
                AppendStateLog(L"hooks.log", L"[mods] wrote " + mod.name + L" " + mod.version + L" (" + std::to_wstring(files.size()) + L" files) into " + finalDir.wstring() + L"\n");
                return finalDir.wstring();
            }
            catch (...)
            {
                LogSwallowedException(L"BundledMods MaterializeOne");
                return {};
            }
        }

        // Whether the cog turned a mod off.
        //   settings - the cog settings.
        //   name     - the mod's name.
        // Returns true when AppSettings::bundledModsOff names it (without case). Throws std::bad_alloc only.
        bool IsTurnedOff(const AppSettings& settings, std::wstring_view name)
        {
            for (const auto& off : settings.bundledModsOff)
            {
                if (SameName(off, name))
                {
                    return true;
                }
            }
            return false;
        }

        // What Claude Code reads about the user's own plugins for one launch: each settings scope's enabledPlugins and
        // the plugins named by the CLAUDE_CODE_PLUGIN_DIRS the claude will see. Read once per launch, and only when a
        // mod reaches that check.
        struct UsersPlugins
        {
            std::vector<std::vector<PluginEnableEntry>> scopes; // kScopeNames order (lowest precedence first)
            std::vector<std::pair<std::wstring, std::wstring>> dirPlugins; // (plugin name, its folder)
        };

        // Read the user's own plugins as the launch's claude will.
        //   settings   - the cog settings (their env and the folder's own env reach the tab's process env).
        //   workingDir - the session's folder: its .claude\ holds the project and local scopes; "" skips both.
        // Returns what was found; a missing or unreadable file reads as empty. Throws std::bad_alloc only.
        UsersPlugins ReadUsersPlugins(const AppSettings& settings, std::wstring_view workingDir)
        {
            UsersPlugins u;
            std::vector<std::wstring> paths;
            paths.push_back(ClaudeUserSettingsPath());
            std::wstring wd{ workingDir };
            while (!wd.empty() && (wd.back() == L'\\' || wd.back() == L'/'))
            {
                wd.pop_back();
            }
            paths.push_back(wd.empty() ? std::wstring{} : wd + L"\\.claude\\settings.json");
            paths.push_back(wd.empty() ? std::wstring{} : wd + L"\\.claude\\settings.local.json");
            paths.push_back(ManagedSettingsPath());

            // The CLAUDE_CODE_PLUGIN_DIRS the claude ends up with: the tab's environment (this process's, then the
            // launch spec's own variables on top: the cog's global env and the folder's per-directory env), then each
            // settings scope's `env` block, a higher scope winning, as Claude Code applies those blocks to its process.
            std::wstring dirsValue = ProcessEnv(L"CLAUDE_CODE_PLUGIN_DIRS");
            for (const auto& [name, value] : ResolveSessionEnv(settings, workingDir))
            {
                if (SameName(name, L"CLAUDE_CODE_PLUGIN_DIRS"))
                {
                    dirsValue = value;
                }
            }
            for (const auto& path : paths)
            {
                const auto text = path.empty() ? std::nullopt : CachedFileText(path);
                u.scopes.push_back(text ? ParseEnabledPlugins(*text) : std::vector<PluginEnableEntry>{});
                if (text)
                {
                    if (auto v = PluginDirsFromSettingsEnv(*text); !v.empty())
                    {
                        dirsValue = std::move(v);
                    }
                }
            }
            for (const auto& folder : SplitPluginDirsValue(dirsValue))
            {
                if (auto name = PluginNameInFolder(folder); !name.empty())
                {
                    u.dirPlugins.emplace_back(std::move(name), folder);
                }
            }
            return u;
        }
    }

    const std::vector<BundledModInfo>& BundledModCatalog()
    {
        // Built on first use (a function-local static: thread-safe initialization), from the generated table.
        static const std::vector<BundledModInfo> s_catalog = [] {
            std::vector<BundledModInfo> v;
            for (size_t i = 0; i < BundledModsData::kModCount; ++i)
            {
                const auto& m = BundledModsData::kMods[i];
                v.push_back(BundledModInfo{ std::wstring{ m.name }, std::wstring{ m.version }, std::wstring{ m.minClaudeCode }, std::wstring{ m.summary }, std::wstring{ m.contentHash } });
            }
            return v;
        }();
        return s_catalog;
    }

    std::vector<BundledModFile> BundledModFiles(std::wstring_view name)
    {
        std::vector<BundledModFile> out;
        const auto* mod = FindModData(name);
        if (!mod)
        {
            return out;
        }
        for (size_t i = 0; i < mod->fileCount; ++i)
        {
            const auto& f = mod->files[i];
            BundledModFile file;
            file.path = std::wstring{ f.path };
            file.bytes.reserve(f.size);
            for (size_t p = 0; p < f.pieceCount; ++p)
            {
                file.bytes.append(f.pieces[p]);
            }
            out.push_back(std::move(file));
        }
        return out;
    }

    std::wstring BundledModContentHashOf(const std::vector<BundledModFile>& files)
    {
        // The generator's framing, byte for byte (bundled_mods.py content_hash): path, LF, decimal size, LF, bytes.
        std::string framed;
        for (const auto& f : files)
        {
            framed += ToUtf8(f.path);
            framed += '\n';
            framed += std::to_string(f.bytes.size());
            framed += '\n';
            framed += f.bytes;
        }
        const std::string hex = Sha256Hex(framed);
        return std::wstring{ hex.begin(), hex.end() };
    }

    std::wstring BundledModFolderLeaf(const BundledModInfo& mod)
    {
        return mod.version + L"-" + mod.contentHash.substr(0, 8);
    }

    std::optional<ClaudeFileVersion> ParseClaudeVersionText(std::wstring_view text) noexcept
    {
        // "2.1.310", "2.1.310.0", "2.1.310 (Claude Code)": the first three dot-separated decimal parts.
        uint32_t parts[3]{};
        size_t i = 0;
        for (int part = 0; part < 3; ++part)
        {
            if (part > 0)
            {
                if (i >= text.size() || text[i] != L'.')
                {
                    return std::nullopt;
                }
                ++i;
            }
            const size_t start = i;
            uint64_t value = 0;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'9')
            {
                value = value * 10 + static_cast<uint64_t>(text[i] - L'0');
                if (value > 0xFFFF)
                {
                    return std::nullopt; // a PE version part is 16 bits: anything bigger is not a version
                }
                ++i;
            }
            if (i == start)
            {
                return std::nullopt;
            }
            parts[part] = static_cast<uint32_t>(value);
        }
        ClaudeFileVersion v;
        v.major = parts[0];
        v.minor = parts[1];
        v.build = parts[2];
        return v;
    }

    bool ClaudeVersionAtLeast(const ClaudeFileVersion& have, const ClaudeFileVersion& need) noexcept
    {
        if (have.major != need.major)
        {
            return have.major > need.major;
        }
        if (have.minor != need.minor)
        {
            return have.minor > need.minor;
        }
        return have.build >= need.build;
    }

    std::vector<PluginEnableEntry> ParseEnabledPlugins(std::wstring_view settingsJsonText)
    {
        std::vector<PluginEnableEntry> out;
        const auto root = json::Parse(settingsJsonText);
        if (!root || root->type != json::Value::Type::Obj)
        {
            return out;
        }
        const auto* plugins = root->Find(L"enabledPlugins");
        if (!plugins || plugins->type != json::Value::Type::Obj)
        {
            return out;
        }
        for (const auto& [key, value] : plugins->members)
        {
            if (value.type != json::Value::Type::Bool || key.empty())
            {
                continue; // not Claude Code's on/off (a hand-edited value): Claude Code would not load on it either
            }
            PluginEnableEntry e;
            e.key = key;
            // "<name>@<marketplace>": the name is everything before the LAST '@'. A key with no '@' (or one at the very
            // start) is a bare name.
            const size_t at = key.rfind(L'@');
            e.name = (at == std::wstring::npos || at == 0) ? key : key.substr(0, at);
            e.enabled = value.boolean;
            out.push_back(std::move(e));
        }
        return out;
    }

    std::wstring EffectiveEnabledPluginKey(std::wstring_view modName, const std::vector<std::vector<PluginEnableEntry>>& scopesLowToHigh)
    {
        // Merge the scopes key by key, lowest first, so each key ends with the value of the highest scope that sets it
        // (Claude Code's settings merge). Keys compare without case; the first spelling seen is the one reported.
        std::vector<std::pair<std::wstring, PluginEnableEntry>> merged; // (lowercased key, entry)
        for (const auto& scope : scopesLowToHigh)
        {
            for (const auto& e : scope)
            {
                const std::wstring k = Lower(e.key);
                bool found = false;
                for (auto& [mk, me] : merged)
                {
                    if (mk == k)
                    {
                        me.enabled = e.enabled;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    merged.emplace_back(k, e);
                }
            }
        }
        for (const auto& [mk, me] : merged)
        {
            if (me.enabled && SameName(me.name, modName))
            {
                return me.key;
            }
        }
        return {};
    }

    std::vector<std::wstring> SplitPluginDirsValue(std::wstring_view value)
    {
        std::vector<std::wstring> out;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t sep = value.find(L';', start);
            const size_t end = sep == std::wstring_view::npos ? value.size() : sep;
            const auto folder = Trim(value.substr(start, end - start), L" \t\"");
            if (!folder.empty())
            {
                out.emplace_back(folder);
            }
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            start = sep + 1;
        }
        return out;
    }

    std::wstring PluginDirsFromSettingsEnv(std::wstring_view settingsJsonText)
    {
        const auto root = json::Parse(settingsJsonText);
        if (!root || root->type != json::Value::Type::Obj)
        {
            return {};
        }
        const auto* env = root->Find(L"env");
        return (env && env->type == json::Value::Type::Obj) ? env->StrAt(L"CLAUDE_CODE_PLUGIN_DIRS") : std::wstring{};
    }

    std::wstring RenderPluginDirArgs(const std::vector<std::wstring>& folders)
    {
        std::wstring out;
        for (const auto& folder : folders)
        {
            // Quoted, forward slashes: the spelling the --settings path already uses on this command line, which the pwsh
            // host's `&` operator passes to claude.exe as one argument (a profile path may hold spaces).
            out += L" --plugin-dir \"" + ToForwardSlashes(folder) + L"\"";
        }
        return out;
    }

    std::vector<MaterializedBundledMod> MaterializeBundledModsIn(const std::wstring& stateDir) noexcept
    {
        std::vector<MaterializedBundledMod> out;
        try
        {
            for (const auto& mod : BundledModCatalog())
            {
                out.push_back(MaterializedBundledMod{ mod.name, MaterializeOne(stateDir, mod) });
            }
        }
        catch (...)
        {
            // Only an allocation can throw here (MaterializeOne catches its own): the mods not reached load nothing.
            LogSwallowedException(L"MaterializeBundledModsIn");
        }
        return out;
    }

    const std::vector<MaterializedBundledMod>& EnsureBundledModsMaterialized(const std::wstring& stateDir) noexcept
    {
        static std::once_flag s_once;
        static std::vector<MaterializedBundledMod> s_result;
        try
        {
            std::call_once(s_once, [&stateDir] { s_result = MaterializeBundledModsIn(stateDir); });
        }
        catch (...)
        {
            // The assignment's allocation failed: call_once stays unset, so the next launch tries again.
            LogSwallowedException(L"EnsureBundledModsMaterialized");
        }
        return s_result;
    }

    std::vector<BundledModChoice> ChooseBundledMods(const std::wstring& stateDir, const AppSettings& settings, std::wstring_view claudeLauncher, std::wstring_view workingDir, bool materialize)
    {
        std::vector<BundledModChoice> out;
        const auto& catalog = BundledModCatalog();
        if (catalog.empty())
        {
            return out;
        }
        const auto have = claudeLauncher.empty() ? std::nullopt : CachedClaudeVersion(claudeLauncher);
        std::optional<UsersPlugins> users; // read at the first mod that gets that far
        for (const auto& mod : catalog)
        {
            BundledModChoice c;
            c.name = mod.name;
            c.version = mod.version;
            const auto need = ParseClaudeVersionText(mod.minClaudeCode);
            if (IsTurnedOff(settings, mod.name))
            {
                c.verdict = BundledModVerdict::Off;
            }
            else if (!have)
            {
                // Never pass a flag to a claude whose version is unknown: one from before --plugin-dir refuses the
                // whole command line, and the tab would die at start.
                c.verdict = BundledModVerdict::ClaudeUnknown;
                c.detail = claudeLauncher.empty() ? std::wstring{ L"no claude.exe was found" } : (L"no version could be read from " + std::wstring{ claudeLauncher });
            }
            else if (!need || !ClaudeVersionAtLeast(*have, *need))
            {
                c.verdict = BundledModVerdict::ClaudeTooOld;
                c.detail = L"needs Claude Code " + mod.minClaudeCode + L" (this claude.exe is " + VersionText(*have) + L")";
            }
            else
            {
                if (!users)
                {
                    users = ReadUsersPlugins(settings, workingDir);
                }
                std::wstring ownKey = EffectiveEnabledPluginKey(mod.name, users->scopes);
                if (!ownKey.empty())
                {
                    // Name the scope whose value decided it: the highest one that sets that key.
                    std::wstring scope = L"settings";
                    for (size_t s = users->scopes.size(); s-- > 0;)
                    {
                        bool sets = false;
                        for (const auto& e : users->scopes[s])
                        {
                            sets = sets || SameName(e.key, ownKey);
                        }
                        if (sets)
                        {
                            scope = s < std::size(kScopeNames) ? kScopeNames[s] : L"settings";
                            break;
                        }
                    }
                    c.verdict = BundledModVerdict::UsersOwnCopy;
                    c.detail = ownKey + L" (" + scope + L")";
                }
                else
                {
                    for (const auto& [pluginName, folder] : users->dirPlugins)
                    {
                        if (SameName(pluginName, mod.name))
                        {
                            c.verdict = BundledModVerdict::UsersOwnCopy;
                            c.detail = L"the folder " + folder + L" (CLAUDE_CODE_PLUGIN_DIRS)";
                            break;
                        }
                    }
                    if (c.detail.empty())
                    {
                        c.verdict = BundledModVerdict::Loads;
                        if (materialize)
                        {
                            for (const auto& m : EnsureBundledModsMaterialized(stateDir))
                            {
                                if (m.name == mod.name)
                                {
                                    c.folder = m.folder;
                                }
                            }
                            if (c.folder.empty())
                            {
                                c.verdict = BundledModVerdict::NoFolder;
                            }
                        }
                    }
                }
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    std::wstring DescribeBundledModChoice(const BundledModChoice& choice)
    {
        const std::wstring who = choice.name + L" " + choice.version;
        switch (choice.verdict)
        {
        case BundledModVerdict::Loads:
            return who + L" loads";
        case BundledModVerdict::Off:
            return who + L" is off (Settings > Claude)";
        case BundledModVerdict::ClaudeUnknown:
            return who + L" waits for a Claude Code whose version can be read: " + choice.detail;
        case BundledModVerdict::ClaudeTooOld:
            return who + L" " + choice.detail;
        case BundledModVerdict::UsersOwnCopy:
            return who + L" stands aside: Claude Code loads your own " + choice.detail;
        case BundledModVerdict::NoFolder:
            return who + L" could not be written into the profile (see the [mods] lines before this one)";
        }
        return who;
    }

    std::wstring BundledModPluginArgs(const std::wstring& stateDir, const AppSettings& settings, std::wstring_view claudeLauncher, std::wstring_view workingDir, std::wstring_view sessionId) noexcept
    {
        try
        {
            const auto choices = ChooseBundledMods(stateDir, settings, claudeLauncher, workingDir, true);
            if (choices.empty())
            {
                return {};
            }
            std::vector<std::wstring> folders;
            std::wstring line = L"[mods] " + ShortId(std::wstring{ sessionId });
            for (size_t i = 0; i < choices.size(); ++i)
            {
                const auto& c = choices[i];
                if (c.verdict == BundledModVerdict::Loads && !c.folder.empty())
                {
                    folders.push_back(c.folder);
                }
                line += (i == 0 ? L" " : L"; ") + DescribeBundledModChoice(c);
            }
            AppendStateLog(L"hooks.log", line + L"\n");
            return RenderPluginDirArgs(folders);
        }
        catch (...)
        {
            // The launch goes on without bundled mods; the tab is still a normal Claude tab.
            LogSwallowedException(L"BundledModPluginArgs");
            return {};
        }
    }
}
