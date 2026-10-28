// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — STATE BACKUPS AND SHADOWS across versions (doc/agentmaster/PERSISTENCE.md §17; the owner's task of
// 2026-10-28: "making sure that any case will always migrate successfully without hurting the existing files and their
// data"). The Win32 half of StateCompat.h, header-only so the EXE's startup (WindowEmperor) and the engine share it:
//
//   * WHO WRITES — CurrentWriter(): the running build's version and install kind, for the writer stamp every state
//     document carries (StateCompat.h WriterStamp) and for the version-change check below.
//   * THE VERSION-CHANGE SNAPSHOT — SnapshotOnVersionChange(): at the first start of a build that differs from the one
//     that ran last (<profile>\last-run.json), BEFORE anything of the run writes a state file, every state file is
//     copied into <profile>\backups\<yyyyMMdd-HHmmss> <from> to <to>\ with a README. Any migration — an upgrade's, a
//     downgrade's, a bug's — can then be undone by copying the files back. The newest kKeepSnapshots stay.
//   * THE SHADOW — ShadowPathFor(): <profile>\shadow\<relative path>, the copy of each state document as a build that
//     keeps unknown fields last wrote it (Persistence.cpp writes it). An older build drops our fields when it saves; the
//     shadow is where the next start takes them back from (StateCompat.h RestoreKeysDroppedByOlderWriter).
//
// Everything here copies or writes OUR files (the backups, the shadows, the marker); it never deletes or replaces a state
// file. The one delete is the pruning of OUR OWN older snapshot folders, each recognized by its README's first line.

#pragma once

#include <windows.h>
#include <appmodel.h> // GetCurrentPackageId

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread> // SnapshotProfileState: the session store's background copy
#include <vector>

#include "Json.h"
#include "ProfileBootstrap.h" // Profiles::IsDevPackage / IsPortableInstall / detail::ExeDirPath / detail::Utf8ToWide / WideToUtf8

namespace Agentmaster::StateBackups
{
    /// <summary>The profile subfolder that holds the version-change snapshots.</summary>
    inline constexpr const wchar_t* kBackupsFolder = L"backups";
    /// <summary>The profile subfolder that holds the shadows of the state documents.</summary>
    inline constexpr const wchar_t* kShadowFolder = L"shadow";
    /// <summary>The profile file that records which build ran last (the version-change check).</summary>
    inline constexpr const wchar_t* kLastRunFile = L"last-run.json";
    /// <summary>How many snapshot folders pruning keeps (the newest). ~16 MB each on a 2,700-session profile.</summary>
    inline constexpr size_t kKeepSnapshots = 12;
    /// <summary>The first line of every snapshot's README: pruning removes only a folder whose README starts so.</summary>
    inline constexpr std::wstring_view kSnapshotMarker = L"Agentmaster state snapshot";

    /// <summary>
    /// The profile's state files, relative to the profile folder: every file a migration could rewrite. Terminal's own
    /// settings are included (Windows Terminal rewrites them when it migrates or adds generated profiles).
    /// </summary>
    inline constexpr const wchar_t* kStateFiles[] = {
        L"sessions.json",
        L"settings.json",
        L"layout.json",
        L"open-windows.json",
        L"templates.json",
        L"recent-dirs.json",
        L"model-list.json",
        L"dir-colors.json",
        L"dir-env.json",
        L"launch-presets.json",
        L"tags.json",
        L"tag-colors.json",
        L"messaging\\telegram\\state.json",
        L"messaging\\telegram\\state.json.prev",
        L"messaging\\telegram\\filelinks.json",
        L"terminal\\settings.json",
        L"terminal\\state.json",
        L"terminal\\elevated-state.json",
    };
    /// <summary>The profile's state FOLDERS, relative: each one's *.json files are state (window records, session store).</summary>
    inline constexpr const wchar_t* kStateFolders[] = { L"windows", L"session-store" };

    /// <summary>The build that is running: what its writer stamp says and what the version-change check compares.</summary>
    struct WriterIdentity
    {
        /// <summary>The version: the package's (packaged), the portable copy's .am-version, else "0.0.0.0".</summary>
        std::wstring version;
        /// <summary>The install kind: "release", "dev", "portable" or "unpackaged".</summary>
        std::wstring install;
    };

    namespace detail
    {
        /// <summary>The digits-and-dots version at the start of a text ("0.8.0.0" from "\xFEFF0.8.0.0\r\n"); "" when none.</summary>
        /// <param name="text">The text (a .am-version stamp).</param>
        /// <returns>The version text.</returns>
        /// <exception cref="std::bad_alloc">The result could not be built.</exception>
        inline std::wstring LeadingVersion(std::wstring_view text)
        {
            size_t i = 0;
            while (i < text.size() && !(text[i] >= L'0' && text[i] <= L'9'))
            {
                ++i; // a BOM, spaces, a "v"
            }
            std::wstring out;
            while (i < text.size() && ((text[i] >= L'0' && text[i] <= L'9') || text[i] == L'.'))
            {
                out.push_back(text[i++]);
            }
            return out;
        }

        /// <summary>Reads a small file whole as UTF-8 text; "" when it is missing, cannot be read or is over 1 MiB.</summary>
        /// <param name="path">The file.</param>
        /// <returns>Its text.</returns>
        /// <remarks>Never throws (a failure reads as "").</remarks>
        inline std::wstring ReadSmallFile(const std::wstring& path) noexcept
        {
            try
            {
                const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    return {};
                }
                LARGE_INTEGER size{};
                std::string bytes;
                if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (1 << 20))
                {
                    bytes.resize(static_cast<size_t>(size.QuadPart));
                    DWORD got = 0;
                    if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr))
                    {
                        got = 0;
                    }
                    bytes.resize(got);
                }
                ::CloseHandle(h);
                return Profiles::detail::Utf8ToWide(bytes);
            }
            catch (...)
            {
                return {}; // allocation failure: read as "nothing" — every caller treats that as "not recorded"
            }
        }

        /// <summary>Writes a small file whole as UTF-8 through a temp sibling and a rename (a reader sees the old or the new
        /// file, never a torn one). For OUR files only (the marker, the README) — state files go through the state-file guard.</summary>
        /// <param name="path">The file.</param>
        /// <param name="text">Its new content.</param>
        /// <returns>True when the file now holds the text.</returns>
        /// <remarks>Never throws.</remarks>
        inline bool WriteSmallFile(const std::wstring& path, std::wstring_view text) noexcept
        {
            try
            {
                const std::string bytes = Profiles::detail::WideToUtf8(std::wstring{ text });
                const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
                const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    return false;
                }
                DWORD wrote = 0;
                const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
                ::CloseHandle(h);
                if (!ok || wrote != bytes.size() || !::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                {
                    ::DeleteFileW(tmp.c_str());
                    return false;
                }
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        /// <summary>A folder-name-safe form of a text: letters, digits, spaces, dots, dashes and underscores kept, the rest
        /// turned into '_'.</summary>
        /// <param name="text">The text.</param>
        /// <returns>The safe text.</returns>
        /// <exception cref="std::bad_alloc">The result could not be built.</exception>
        inline std::wstring SafeName(std::wstring_view text)
        {
            std::wstring out;
            for (const wchar_t c : text)
            {
                out.push_back((std::iswalnum(c) || c == L' ' || c == L'.' || c == L'-' || c == L'_') ? c : L'_');
            }
            return out;
        }

        /// <summary>The local time as yyyyMMdd-HHmmss (the snapshot folders' sortable prefix).</summary>
        /// <returns>The stamp.</returns>
        /// <exception cref="std::bad_alloc">The result could not be built.</exception>
        inline std::wstring LocalStamp()
        {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t buf[32]{};
            ::swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            return buf;
        }

        /// <summary>
        /// Copies one file into a snapshot. The source is opened sharing read, write AND delete, so a writer's atomic replace
        /// of it (a temp file renamed over it) succeeds while the copy reads — the background copy of the session store runs
        /// beside the app's own writes — and the copy keeps the bytes it opened. The copy keeps the source's last-write time:
        /// a file put back from a snapshot must look older than its shadow, which is how the older-writer repair leaves a
        /// user's restore alone (Persistence.cpp RepairStateFilesWrittenByOlderBuilds). A missing source is simply not part
        /// of this profile.
        /// </summary>
        /// <param name="from">The state file.</param>
        /// <param name="to">Its place in the snapshot (its folder is created; an existing file there is never overwritten).</param>
        /// <param name="copied">Counts a copy.</param>
        /// <param name="failed">Receives "&lt;relative path&gt; (error N)" for a copy that failed for another reason.</param>
        /// <param name="rel">The file's relative path, for <paramref name="failed"/>.</param>
        /// <remarks>Never throws (a failure is recorded in <paramref name="failed"/>, or dropped when even that cannot grow).</remarks>
        inline void CopyIntoSnapshot(const std::wstring& from, const std::wstring& to, size_t& copied, std::wstring& failed, std::wstring_view rel) noexcept
        {
            HANDLE src = INVALID_HANDLE_VALUE;
            HANDLE dst = INVALID_HANDLE_VALUE;
            try
            {
                const auto fail = [&](DWORD error) {
                    failed += (failed.empty() ? L"" : L", ") + std::wstring{ rel } + L" (error " + std::to_wstring(error) + L")";
                };
                src = ::CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (src == INVALID_HANDLE_VALUE)
                {
                    const DWORD e = ::GetLastError();
                    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
                    {
                        fail(e);
                    }
                    return;
                }
                FILETIME lastWrite{};
                const bool haveTime = ::GetFileTime(src, nullptr, nullptr, &lastWrite) != FALSE;
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path{ to }.parent_path(), ec);
                dst = ::CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW /* a fresh folder: never overwrite */, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (dst == INVALID_HANDLE_VALUE)
                {
                    fail(::GetLastError());
                    ::CloseHandle(src);
                    return;
                }
                std::vector<char> buffer(64 * 1024);
                DWORD error = 0;
                for (;;)
                {
                    DWORD got = 0;
                    if (!::ReadFile(src, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
                    {
                        error = ::GetLastError();
                        break;
                    }
                    if (got == 0)
                    {
                        break; // the whole file
                    }
                    DWORD put = 0;
                    if (!::WriteFile(dst, buffer.data(), got, &put, nullptr) || put != got)
                    {
                        error = put != got && ::GetLastError() == 0 ? ERROR_WRITE_FAULT : ::GetLastError();
                        break;
                    }
                }
                if (error == 0 && haveTime)
                {
                    ::SetFileTime(dst, nullptr, nullptr, &lastWrite); // the source's time, not the copy's (see the summary)
                }
                ::CloseHandle(dst);
                dst = INVALID_HANDLE_VALUE;
                ::CloseHandle(src);
                src = INVALID_HANDLE_VALUE;
                if (error != 0)
                {
                    ::DeleteFileW(to.c_str()); // never a partial copy in a snapshot
                    fail(error);
                    return;
                }
                ++copied;
            }
            catch (...)
            {
                // The buffer or the report could not grow (bad_alloc): close what is open; the copy counts as not made.
                if (dst != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(dst);
                    ::DeleteFileW(to.c_str());
                }
                if (src != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(src);
                }
            }
        }

        /// <summary>Copies every *.json directly in one profile folder into the same folder of a snapshot.</summary>
        /// <param name="profileDir">The profile folder.</param>
        /// <param name="snapshotDir">The snapshot folder.</param>
        /// <param name="folder">The state folder, relative ("windows", "session-store").</param>
        /// <param name="copied">Counts the copies.</param>
        /// <param name="failed">Receives the failed copies (CopyIntoSnapshot).</param>
        /// <remarks>Never throws (a folder that cannot be listed is not part of this profile).</remarks>
        inline void CopyFolderIntoSnapshot(const std::wstring& profileDir, const std::wstring& snapshotDir, std::wstring_view folder, size_t& copied, std::wstring& failed) noexcept
        {
            try
            {
                WIN32_FIND_DATAW fd{};
                const HANDLE find = ::FindFirstFileExW((profileDir + L"\\" + std::wstring{ folder } + L"\\*.json").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                                       FIND_FIRST_EX_LARGE_FETCH);
                if (find == INVALID_HANDLE_VALUE)
                {
                    return; // no such folder in this profile
                }
                do
                {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    {
                        const std::wstring rel = std::wstring{ folder } + L"\\" + fd.cFileName;
                        CopyIntoSnapshot(profileDir + L"\\" + rel, snapshotDir + L"\\" + rel, copied, failed, rel);
                    }
                } while (::FindNextFileW(find, &fd));
                ::FindClose(find);
            }
            catch (...)
            {
                // A name could not be built (bad_alloc): the files not reached are not in this snapshot (its README says how many are).
            }
        }

        /// <summary>The text of a snapshot's README.txt (its first line is kSnapshotMarker, which pruning looks for).</summary>
        /// <param name="profileDir">The profile folder the files belong in.</param>
        /// <param name="reason">Why the snapshot was taken.</param>
        /// <param name="copied">How many files the snapshot holds.</param>
        /// <param name="failed">The copies that failed ("" for none).</param>
        /// <param name="storePending">True while the session store is still being copied in the background.</param>
        /// <returns>The README text.</returns>
        /// <exception cref="std::bad_alloc">The text could not be built.</exception>
        inline std::wstring SnapshotReadme(const std::wstring& profileDir, std::wstring_view reason, size_t copied, const std::wstring& failed, bool storePending)
        {
            return std::wstring{ kSnapshotMarker } + L" \x2014 taken " + LocalStamp() + L"\r\n\r\n" + std::wstring{ reason } +
                   L"\r\n\r\nThese are copies of Agentmaster's state files as they were BEFORE that start wrote anything: the sessions (sessions.json), "
                   L"the windows and their tabs (windows\\), the titles, tags and favorites (session-store\\), the settings, the folder colors, "
                   L"the Telegram bot's topic map (messaging\\telegram\\) and Windows Terminal's own settings (terminal\\).\r\n\r\n"
                   L"To go back to them: close Agentmaster, copy the files from this folder back into\r\n" +
                   profileDir + L"\r\n(replace the files there when asked), and start Agentmaster again.\r\n\r\nCopied: " + std::to_wstring(copied) + L" file(s)." +
                   (storePending ? std::wstring{ L"\r\nThe session store (session-store\\) is being copied in the background; this line changes when it is done. "
                                                 L"If it never does, Agentmaster closed first, and the copies there are incomplete." } :
                                   std::wstring{}) +
                   (failed.empty() ? std::wstring{} : (L"\r\nCould not be copied: " + failed)) + L"\r\n\r\nAgentmaster keeps the newest " +
                   std::to_wstring(kKeepSnapshots) + L" such folders and removes older ones.\r\n";
        }
    }

    // Declared ahead: SnapshotProfileState prunes after it copies (the definition and its documentation are below).
    inline size_t PruneSnapshots(const std::wstring& profileDir, size_t keep) noexcept;

    /// <summary>
    /// The running build: the package version and "release" / "dev" for a packaged run; the exe-side .am-version and
    /// "portable" for a portable copy; "0.0.0.0" and "unpackaged" for any other run (a local build).
    /// </summary>
    /// <returns>The identity.</returns>
    /// <remarks>Never throws (an allocation failure yields an empty identity, which every caller treats as unknown).</remarks>
    inline WriterIdentity CurrentWriter() noexcept
    {
        WriterIdentity w;
        try
        {
            UINT32 len = 0;
            if (::GetCurrentPackageId(&len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 0)
            {
                std::vector<BYTE> buf(len);
                if (::GetCurrentPackageId(&len, buf.data()) == ERROR_SUCCESS)
                {
                    const auto* id = reinterpret_cast<const PACKAGE_ID*>(buf.data());
                    w.version = std::to_wstring(id->version.Major) + L"." + std::to_wstring(id->version.Minor) + L"." + std::to_wstring(id->version.Build) + L"." +
                                std::to_wstring(id->version.Revision);
                    w.install = Profiles::IsDevPackage() ? L"dev" : L"release";
                    return w;
                }
            }
            if (Profiles::IsPortableInstall())
            {
                w.version = detail::LeadingVersion(detail::ReadSmallFile(Profiles::detail::ExeDirPath() + L"\\.am-version"));
                w.install = L"portable";
                if (w.version.empty())
                {
                    w.version = L"0.0.0.0";
                }
                return w;
            }
            w.version = L"0.0.0.0";
            w.install = L"unpackaged";
        }
        catch (...)
        {
            w = WriterIdentity{};
        }
        return w;
    }

    /// <summary>A writer identity as one line ("0.8.0.1 release"), for the marker comparison and the logs.</summary>
    /// <param name="w">The identity.</param>
    /// <returns>The line; "" for an empty identity.</returns>
    /// <exception cref="std::bad_alloc">The line could not be built.</exception>
    inline std::wstring WriterText(const WriterIdentity& w)
    {
        if (w.version.empty() && w.install.empty())
        {
            return {};
        }
        return w.version + L" " + w.install;
    }

    /// <summary>
    /// The shadow path of a state file: &lt;profile&gt;\shadow\&lt;its path relative to the profile&gt;. The shadow keeps the state
    /// document as a build that keeps unknown fields last wrote it, so the next start can take back what an older build
    /// dropped (StateCompat.h). Only files inside the profile have one.
    /// </summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <param name="stateFile">The state file's full path.</param>
    /// <returns>The shadow path; "" when the file is not inside the profile (or is already a shadow or a backup).</returns>
    /// <exception cref="std::bad_alloc">The path could not be built.</exception>
    inline std::wstring ShadowPathFor(const std::wstring& profileDir, const std::wstring& stateFile)
    {
        if (profileDir.empty() || stateFile.size() <= profileDir.size() + 1)
        {
            return {};
        }
        // Case-insensitive prefix: Windows paths ("C:\Users\…" vs "c:\users\…") name the same folder either way.
        if (::CompareStringOrdinal(stateFile.c_str(), static_cast<int>(profileDir.size()), profileDir.c_str(), static_cast<int>(profileDir.size()), TRUE) != CSTR_EQUAL ||
            (stateFile[profileDir.size()] != L'\\' && stateFile[profileDir.size()] != L'/'))
        {
            return {};
        }
        const std::wstring rel = stateFile.substr(profileDir.size() + 1);
        const auto startsWith = [&rel](std::wstring_view folder) {
            return rel.size() > folder.size() && ::CompareStringOrdinal(rel.c_str(), static_cast<int>(folder.size()), folder.data(), static_cast<int>(folder.size()), TRUE) == CSTR_EQUAL &&
                   (rel[folder.size()] == L'\\' || rel[folder.size()] == L'/');
        };
        if (startsWith(kShadowFolder) || startsWith(kBackupsFolder))
        {
            return {}; // never a shadow of a shadow or of a backup
        }
        return profileDir + L"\\" + kShadowFolder + L"\\" + rel;
    }

    /// <summary>
    /// Copies every state file of a profile (kStateFiles and the *.json of kStateFolders) into a new folder
    /// &lt;profile&gt;\backups\&lt;yyyyMMdd-HHmmss&gt; &lt;label&gt;\ with a README that says when, why and how to put the files back.
    /// Nothing in the profile is changed. Then prunes the snapshots beyond kKeepSnapshots (PruneSnapshots).
    /// </summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <param name="reason">Why the snapshot is taken (the README's second paragraph and the log line).</param>
    /// <param name="label">The folder name's label after the stamp ("0.8.0.1 release to 0.8.1.0 release").</param>
    /// <param name="summary">Receives "N file(s)" or the failures, for the caller's log line.</param>
    /// <param name="storeInBackground">
    /// True (the default): the session store (session-store\, one small file per session — ~2,700 files and ~2.5 s on a
    /// long-lived profile, measured 2026-10-28, and growing) is copied by a background thread at background priority, after
    /// this returns; the README says so until the copy ends. A record the app writes meanwhile may then be in the snapshot as
    /// written (its writes replace a record atomically, and the copy reads with full sharing, so neither side fails). False:
    /// copied before this returns (the harness).
    /// </param>
    /// <returns>The snapshot folder; "" when none could be created.</returns>
    /// <remarks>Never throws: a failure is reported through the return value and <paramref name="summary"/>.</remarks>
    inline std::wstring SnapshotProfileState(const std::wstring& profileDir, std::wstring_view reason, std::wstring_view label, std::wstring& summary,
                                             bool storeInBackground = true) noexcept
    {
        std::wstring dir;
        try
        {
            const std::wstring root = profileDir + L"\\" + kBackupsFolder;
            ::CreateDirectoryW(root.c_str(), nullptr);
            const std::wstring base = root + L"\\" + detail::LocalStamp() + L" " + detail::SafeName(label);
            for (int n = 1; n <= 99 && dir.empty(); ++n)
            {
                const std::wstring candidate = base + (n > 1 ? L" (" + std::to_wstring(n) + L")" : std::wstring{});
                if (::CreateDirectoryW(candidate.c_str(), nullptr))
                {
                    dir = candidate;
                }
                else if (::GetLastError() != ERROR_ALREADY_EXISTS)
                {
                    break;
                }
            }
            if (dir.empty())
            {
                summary = L"no snapshot folder could be created under " + root + L" (error " + std::to_wstring(::GetLastError()) + L")";
                return {};
            }
            size_t copied = 0;
            std::wstring failed;
            for (const auto* rel : kStateFiles)
            {
                detail::CopyIntoSnapshot(profileDir + L"\\" + rel, dir + L"\\" + rel, copied, failed, rel);
            }
            detail::CopyFolderIntoSnapshot(profileDir, dir, L"windows", copied, failed);
            bool storePending = false;
            if (storeInBackground)
            {
                try
                {
                    // Detached: it must never hold up the start, and a process that ends first just leaves the README saying
                    // the store copy did not finish. It owns copies of everything it reads.
                    std::thread{ [profileDir, dir, reasonText = std::wstring{ reason }, copiedBefore = copied, failedBefore = failed]() noexcept {
                        try
                        {
                            ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // low CPU and I/O priority
                            size_t total = copiedBefore;
                            std::wstring failures = failedBefore;
                            detail::CopyFolderIntoSnapshot(profileDir, dir, L"session-store", total, failures);
                            detail::WriteSmallFile(dir + L"\\README.txt", detail::SnapshotReadme(profileDir, reasonText, total, failures, false));
                        }
                        catch (...)
                        {
                            // An allocation failed: the README keeps saying the store copy did not finish, which is the truth.
                        }
                    } }.detach();
                    storePending = true;
                }
                catch (...)
                {
                    storePending = false; // no thread (system_error): copy it here instead
                }
            }
            if (!storePending)
            {
                detail::CopyFolderIntoSnapshot(profileDir, dir, L"session-store", copied, failed);
            }
            detail::WriteSmallFile(dir + L"\\README.txt", detail::SnapshotReadme(profileDir, reason, copied, failed, storePending));
            summary = std::to_wstring(copied) + L" file(s)" + (storePending ? L", the session store copying in the background" : L"") +
                      (failed.empty() ? std::wstring{} : (L"; NOT copied: " + failed));
            PruneSnapshots(profileDir, kKeepSnapshots);
        }
        catch (...)
        {
            try
            {
                summary = L"the snapshot stopped on an exception; what was copied stays in " + dir;
            }
            catch (...)
            {
                // the summary could not be built either: the caller logs without it
            }
        }
        return dir;
    }

    /// <summary>
    /// Removes the oldest snapshot folders beyond <paramref name="keep"/>. Only OUR snapshots count and go: a folder
    /// directly under &lt;profile&gt;\backups\ whose README.txt starts with kSnapshotMarker. Folder names start with a sortable
    /// stamp, so name order is creation order.
    /// </summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <param name="keep">How many of the newest snapshots stay.</param>
    /// <returns>The number of snapshot folders removed.</returns>
    /// <remarks>Never throws: a folder that cannot be removed simply stays.</remarks>
    inline size_t PruneSnapshots(const std::wstring& profileDir, size_t keep) noexcept
    {
        size_t removed = 0;
        try
        {
            const std::wstring root = profileDir + L"\\" + kBackupsFolder;
            std::vector<std::wstring> ours;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator{ std::filesystem::path{ root }, ec })
            {
                if (!entry.is_directory(ec))
                {
                    continue;
                }
                const std::wstring readme = detail::ReadSmallFile(entry.path().wstring() + L"\\README.txt");
                if (readme.rfind(kSnapshotMarker, 0) == 0)
                {
                    ours.push_back(entry.path().wstring());
                }
            }
            if (ours.size() <= keep)
            {
                return 0;
            }
            std::sort(ours.begin(), ours.end());
            for (size_t i = 0; i + keep < ours.size(); ++i)
            {
                std::error_code rmEc;
                std::filesystem::remove_all(std::filesystem::path{ ours[i] }, rmEc);
                if (!rmEc)
                {
                    ++removed;
                }
            }
        }
        catch (...)
        {
            // An allocation failure mid-listing: the snapshots that were not judged stay (pruning is housekeeping only).
        }
        return removed;
    }

    /// <summary>
    /// The version-change check, run once per start right after the profile is resolved and BEFORE anything writes a
    /// state file: when the build that ran last (&lt;profile&gt;\last-run.json) is not this one — or none is recorded (the
    /// first start of a build with this check) — every state file is copied into a snapshot (SnapshotProfileState), then
    /// the marker records this build. A failed snapshot still records the marker (the log line says so), so a start that
    /// cannot copy does not retry at every start.
    /// </summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <param name="storeInBackground">As SnapshotProfileState's (true at startup; the harness passes false).</param>
    /// <returns>A log line for the startup log ("" when the same build ran last).</returns>
    /// <remarks>Never throws.</remarks>
    inline std::wstring SnapshotOnVersionChange(const std::wstring& profileDir, bool storeInBackground = true) noexcept
    {
        try
        {
            if (profileDir.empty())
            {
                return {};
            }
            const std::wstring current = WriterText(CurrentWriter());
            const std::wstring markerPath = profileDir + L"\\" + kLastRunFile;
            std::wstring previous;
            if (const auto doc = json::Parse(detail::ReadSmallFile(markerPath)); doc && doc->type == json::Value::Type::Obj)
            {
                previous = doc->StrAt(L"writer");
            }
            if (!current.empty() && previous == current)
            {
                return {};
            }
            const std::wstring reason = previous.empty() ?
                                            L"This was the first start of an Agentmaster build that keeps state backups (no earlier run is recorded); it is " + current + L"." :
                                            L"Agentmaster changed from " + previous + L" to " + current + L" at this start.";
            const std::wstring label = previous.empty() ? (L"first run of " + current) : (previous + L" to " + current);
            std::wstring summary;
            const std::wstring folder = SnapshotProfileState(profileDir, reason, label, summary, storeInBackground);
            auto marker = json::Value::MkObj();
            marker.Set(L"writer", json::Value::MkStr(current));
            marker.Set(L"note", json::Value::MkStr(L"The Agentmaster build that ran last on this profile (a different one takes a state snapshot into backups\\ first)."));
            detail::WriteSmallFile(markerPath, json::Dump(marker));
            return L"[migrate] " + reason + L" " +
                   (folder.empty() ? (L"The state snapshot FAILED: " + summary) : (L"The state files were copied into " + folder + L" first (" + summary + L")"));
        }
        catch (...)
        {
            return {}; // an allocation failure: no snapshot this start, and no line about it (the next start tries again)
        }
    }
}
