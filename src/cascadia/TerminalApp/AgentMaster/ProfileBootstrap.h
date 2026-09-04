// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the state PROFILE bootstrap (header-only, plain Win32; no WinRT, no engine deps).
//
// A "profile" is ONE folder that holds EVERYTHING an Agentmaster install persists: the engine
// state (sessions.json, windows/<id>.json, open-windows.json, settings.json, hooks files, the
// claude shim, sessions-index/, dir-colors.json, ...) AND — via the AGENTMASTER_PROFILE redirect
// in TerminalSettingsModel's GetBaseSettingsPath() — Windows Terminal's own settings.json /
// state.json (under <profile>\terminal\). The app "merely loads a profile folder".
//
// WHY: the RELEASE package (Agentmaster_...) and the DEV package (AgentmasterDev_...) install
// side by side; without per-install profiles they would fight over ONE ~/.agentmaster (two
// SharedEngines clobbering sessions.json / open-windows.json / bridge.json). Each install
// remembers its OWN profile choice. An INSTALLED copy's first launch AUTO-SELECTS the per-identity
// default WITHOUT prompting — release → Default (~/.agentmaster; the picker option formerly
// labeled "Production"), dev → Development (~/.agentmaster-dev) — and persists it; a PORTABLE
// copy's first launch instead ASKS (Portable / Default / Development / Browse…) and remembers the
// answer NEXT TO THE EXE. Everyone can switch later from the cog's "Change profile folder…".
//
// RESOLUTION ORDER (ResolveProfileDir):
//   1. env  AGENTMASTER_PROFILE        — explicit override (also exported by the bootstrap so
//                                        every module in the process resolves identically).
//   2. <exedir>\profile.path           — the EXE-SIDE POINTER: one line naming the profile folder
//                                        (a RELATIVE line resolves against the exe dir, so a moved
//                                        unzip keeps working). Written by a PORTABLE copy's
//                                        first-launch picker — its per-unzip memory (the home
//                                        map's one "Unpackaged" slot can't tell two unzips apart);
//                                        honored for INSTALLED copies too when someone plants one
//                                        next to the exe (creating it there needs a writable exe
//                                        dir, which an MSIX install doesn't have — power users).
//   3. <exedir>\profile                — when <exedir>\.portable exists and no pointer was written
//                                        yet: the self-contained default. The EXE prelude PROMPTS
//                                        on a portable's first interactive launch; headless/library
//                                        resolution never asks (Rule #15) and lands here. A
//                                        portable NEVER consults the home map below (steps 4–5) —
//                                        self-containment, and the shared "Unpackaged" slot would
//                                        cross-contaminate unzips and legacy tools.
//   4. the saved per-install choice    — %USERPROFILE%\.agentmaster.profiles, a tiny text map
//                                        keyed by package family name (or "Unpackaged"). Lives
//                                        OUTSIDE any profile (chicken-and-egg) and OUTSIDE the
//                                        MSIX-virtualized AppData, so it is shared, honest and
//                                        debuggable.
//   5. the per-identity default        — ~/.agentmaster (release) / ~/.agentmaster-dev (the
//                                        AgentmasterDev package). Headless/test resolution never
//                                        shows UI and lands here, which preserves the historical
//                                        ~/.agentmaster for unpackaged tools and the test harness.
//
// The PICKER (TaskDialogIndirect command links + an IFileDialog folder browse, ShowProfilePicker)
// is shown on a PORTABLE copy's first launch (with the exe-local "Portable profile" option leading
// and defaulted) and by the Settings cog's "Change profile folder…" — an INSTALLED first launch
// auto-selects by identity (above), no prompt. It is pure Win32 — in XAML Islands a Win32 modal
// gets its keyboard input directly (no ContentDialog input trap).
//
// SAFETY: EnsureProfileResolvedAtStartup also takes a kernel mutex named after the resolved
// profile dir. Two live instances (e.g. release + dev pointed at one folder via Browse…) would
// corrupt the profile; the second instance gets a warn-and-confirm. The mutex dies with the
// process, so it can never go stale.

#pragma once

#include <windows.h>
#include <appmodel.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h" // Agentmaster::json — read the persisted "Enable Debug Mode" setting (PersistedDebugModeEnabled); tiny + header-only

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

namespace Agentmaster::Profiles
{
    namespace detail
    {
        inline std::wstring GetEnvVar(const wchar_t* name)
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

        inline bool StartsWith(std::wstring_view s, std::wstring_view prefix)
        {
            return s.size() >= prefix.size() && ::wcsncmp(s.data(), prefix.data(), prefix.size()) == 0;
        }

        // True when the process commandline carries `flag` as a WHOLE token in either the `--flag`
        // or `-flag` spelling, case-insensitively (flag=L"debug" matches `--debug`, `-Debug`). A real
        // token walk (whitespace-split, honoring "quoted" runs) so `flag` appearing INSIDE a path or
        // another argument never false-matches. Pure Win32 — no shell32/CommandLineToArgvW dependency,
        // keeping this header lean and linkable everywhere it is already included.
        inline bool CommandLineHasFlag(std::wstring_view flag)
        {
            const std::wstring_view cmd{ ::GetCommandLineW() };
            const size_t n = cmd.size();
            size_t i = 0;
            while (i < n)
            {
                while (i < n && (cmd[i] == L' ' || cmd[i] == L'\t'))
                {
                    ++i;
                }
                bool quoted = false;
                std::wstring token;
                while (i < n && (quoted || (cmd[i] != L' ' && cmd[i] != L'\t')))
                {
                    const wchar_t c = cmd[i++];
                    if (c == L'"')
                    {
                        quoted = !quoted; // drop the quote character, keep the run together
                        continue;
                    }
                    token.push_back(c);
                }
                std::wstring_view body{ token };
                if (StartsWith(body, L"--"))
                {
                    body.remove_prefix(2);
                }
                else if (StartsWith(body, L"-"))
                {
                    body.remove_prefix(1);
                }
                else
                {
                    continue; // not a flag-shaped token
                }
                if (body.size() != flag.size())
                {
                    continue;
                }
                bool equal = true;
                for (size_t k = 0; k < flag.size(); ++k)
                {
                    wchar_t a = body[k];
                    wchar_t b = flag[k];
                    if (a >= L'A' && a <= L'Z')
                    {
                        a = static_cast<wchar_t>(a - L'A' + L'a');
                    }
                    if (b >= L'A' && b <= L'Z')
                    {
                        b = static_cast<wchar_t>(b - L'A' + L'a');
                    }
                    if (a != b)
                    {
                        equal = false;
                        break;
                    }
                }
                if (equal)
                {
                    return true;
                }
            }
            return false;
        }

        // Read a UTF-8 file (our settings.json is written UTF-8 by the engine AND the updater's RMW) to a
        // wide string, tolerating a leading UTF-8 BOM. Empty on any failure/missing file. Kept tiny + local
        // so ProfileBootstrap can read the persisted "Enable Debug Mode" flag WITHOUT the engine link (the
        // EXE includes this header) — the same "header-only reads settings.json" pattern Updater::ReadPrefs uses.
        inline std::wstring ReadUtf8File(const std::wstring& path)
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (!f)
            {
                return {};
            }
            std::string bytes{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(0, 3); // strip the UTF-8 BOM
            }
            if (bytes.empty())
            {
                return {};
            }
            const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (need <= 0)
            {
                return {};
            }
            std::wstring w(static_cast<size_t>(need), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), need);
            return w;
        }

        // ⚠ MODULE-local, one-way DEBUG override — NOT process-wide. A function-local static inside an
        // inline HEADER function is one PRIVATE copy per linked binary (Agentmaster.exe,
        // TerminalApp.dll, the engine harness, the CLI — nothing exports the symbol across the DLL
        // boundary; the same per-module trap AgentCatchLog.h documents for wil's logging state). Cross-
        // module agreement therefore rides the per-PROCESS env block instead: ApplyPersistedDebugMode
        // latches the module it runs in AND exports AGENTMASTER_DEBUG=1, which every OTHER module's
        // IsDebugPackage() first-use scan reads (the AGENTMASTER_PROFILE propagation idiom). Set once at
        // startup from the persisted "Enable Debug Mode" setting, BEFORE any gate reads IsDebugPackage().
        // Additive — it can only turn debug ON, so it never clobbers an ad-hoc --debug / AGENTMASTER_DEBUG
        // enable. Atomic because the startup write is later read from the engine/UI threads (write
        // happens-before all reads).
        inline std::atomic<bool>& DebugForced()
        {
            static std::atomic<bool> forced{ false };
            return forced;
        }

        inline std::string WideToUtf8(std::wstring_view s)
        {
            if (s.empty())
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

        inline std::wstring Utf8ToWide(std::string_view s)
        {
            if (s.empty())
            {
                return {};
            }
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        // Filesystem-aware-enough equality for two profile dirs (Windows: case-insensitive,
        // slash-agnostic, trailing-separator-agnostic). Mirrors the engine's PathEq spirit
        // without depending on it (this header must stay engine-free).
        inline std::wstring NormPathKey(std::wstring_view p)
        {
            std::wstring out{ p };
            for (auto& c : out)
            {
                if (c == L'/')
                {
                    c = L'\\';
                }
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            while (!out.empty() && out.back() == L'\\')
            {
                out.pop_back();
            }
            return out;
        }

        inline bool SamePath(std::wstring_view a, std::wstring_view b)
        {
            return NormPathKey(a) == NormPathKey(b);
        }

        inline void EnsureDirExists(const std::wstring& dir)
        {
            try
            {
                std::filesystem::create_directories(std::filesystem::path{ dir });
            }
            catch (...)
            {
            }
        }

        inline uint64_t Fnv1a64(std::wstring_view s)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (const wchar_t c : s)
            {
                h ^= static_cast<uint64_t>(c & 0xFF);
                h *= 0x100000001b3ull;
                h ^= static_cast<uint64_t>((c >> 8) & 0xFF);
                h *= 0x100000001b3ull;
            }
            return h;
        }
    }

    // ---------------------------------------------------------------- identity / defaults ----

    // The package family name, or "" when running unpackaged (portable zip, tests, tools).
    inline std::wstring PackageFamilyName()
    {
        // Agentmaster (perf): a process's package family name is immutable for its lifetime, so the
        // two GetCurrentPackageFamilyName syscalls run ONCE per module and every later call is a
        // string copy. IsDevPackage / IsDevOrDebugPackage sit on hot paths — once per Triage-Board
        // card per rebuild (the ⚙ queue badge gate), per launch-model submenu, per settings seed —
        // and used to pay the syscall pair each time.
        static const std::wstring cached = []() -> std::wstring {
            UINT32 len = 0;
            if (::GetCurrentPackageFamilyName(&len, nullptr) != ERROR_INSUFFICIENT_BUFFER || len <= 1)
            {
                return {};
            }
            std::wstring pfn(len, L'\0');
            if (::GetCurrentPackageFamilyName(&len, pfn.data()) != ERROR_SUCCESS)
            {
                return {};
            }
            pfn.resize(len > 0 ? len - 1 : 0); // drop the trailing NUL
            return pfn;
        }();
        return cached;
    }

    // Forward declaration — the cached profile resolver is defined further down (it depends on the
    // choice-file/mutex machinery below), but PersistedDebugModeEnabled needs to name it above that point.
    inline const std::wstring& ResolveProfileDir();

    // True for the AgentmasterDev_* package (the local dev loose layout). The release package is
    // Agentmaster_* — order matters everywhere this prefix pair is tested (Dev first).
    inline bool IsDevPackage()
    {
        return detail::StartsWith(PackageFamilyName(), L"AgentmasterDev");
    }

    // True when the DEBUG escape hatch is active, from ANY of three sources: the persisted "Enable Debug
    // Mode" setting (Settings cog -> About, applied at startup by ApplyPersistedDebugMode) OR the
    // AGENTMASTER_DEBUG env var set (any non-empty value) OR a `--debug` / `-debug` token on the process
    // commandline. This unlocks the DEV-only Auto Testing / Tests Autorunner surfaces in a RELEASE (or
    // unpackaged) build — the same "force a package-gated feature on for QA" escape hatch
    // Updater::IsUpdaterChannel exposes via AGENTMASTER_UPDATE_STARTUP. The persisted setting is the
    // durable, discoverable twin of the launch flag (no env/commandline needed).
    // WindowEmperor::_dispatchCommandlineCommon strips the `--debug` token
    // from the WT commandline (it is NOT a Windows Terminal option) BEFORE the args are parsed, but
    // GetCommandLineW() still returns the original string, so this scan stays valid regardless.
    //
    // Effective for the FRESH launch whose OWN commandline/env carries it. A `--debug` handed off to an
    // ALREADY-running instance does not retro-enable it there (that process predates the flag, and the
    // UI gates cache their verdict) — relaunch, or set AGENTMASTER_DEBUG in the environment, to persist.
    inline bool IsDebugPackage()
    {
        // The persisted "Enable Debug Mode" setting (Settings cog -> About) forces this on via a
        // process-local, one-way override applied at startup (ApplyPersistedDebugMode). Checked LIVE
        // (before the cache) so it composes with the env/commandline sources without disturbing their
        // one-time scan — and so a caller that ran before the startup apply still sees the right verdict
        // on its next call.
        if (detail::DebugForced().load(std::memory_order_relaxed))
        {
            return true;
        }
        // Process-lifetime constant (env + commandline never change mid-run) — cache so the hot gate
        // sites (per-card board rebuilds, per-settings-push) don't rescan the commandline every call.
        // The cache is per-MODULE too (an inline function's static — see detail::DebugForced), but env
        // + commandline are per-PROCESS facts, so every module's first-use scan converges on the same
        // verdict as long as the AGENTMASTER_DEBUG export (ApplyPersistedDebugMode, EXE prelude) runs
        // before that module's first read — which startup ordering guarantees.
        static const bool debug = []() {
            if (!detail::GetEnvVar(L"AGENTMASTER_DEBUG").empty())
            {
                return true;
            }
            return detail::CommandLineHasFlag(L"debug");
        }();
        return debug;
    }

    // True when the persisted "Enable Debug Mode" setting is ON in the ACTIVE profile's settings.json
    // (the AppSettings `debugMode` field the Settings cog's About tab writes). A header-only read — the
    // EXE cannot link the engine's LoadAppSettings, so this parses the one key directly, mirroring how
    // Updater::ReadPrefs reads its own settings.json keys. Any missing file / parse failure => false.
    // Agentmaster (schema fix — the toggle used to be a permanent NO-OP): the cog persists
    // AppSettings INSIDE the engine's {version, settings:{...}} ENVELOPE (SaveAppSettings ->
    // root.Set(L"settings", ToJson(settings))), so the key lives at settings.debugMode — the
    // original TOP-LEVEL BoolAt could never see it (Json.h's *At readers are flat, non-traversing),
    // exactly the envelope-vs-top-level mismatch the updater keys once had. Consequence in the
    // field: a release instance relaunched without --debug came up with the Tests Autorunner
    // DISABLED no matter what the About-tab toggle said, and every queued prompt sat Pending
    // forever (the "queued while Running, never sent" report — journey2). Read the envelope first;
    // keep a top-level fallback for a hand-edited/stray file (the Updater::ReadPrefs
    // migration-tolerant idiom: never drop a value the user expressed, wherever it landed).
    inline bool PersistedDebugModeEnabled()
    {
        const std::wstring path = ResolveProfileDir() + L"\\settings.json";
        const auto parsed = json::Parse(detail::ReadUtf8File(path));
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return false;
        }
        // The envelope's settings.debugMode is authoritative when the key is PRESENT there — a
        // nested false must not fall through to (and be overridden by) a stale top-level stray.
        if (const auto* nested = parsed->Find(L"settings");
            nested && nested->type == json::Value::Type::Obj && nested->Find(L"debugMode"))
        {
            return nested->BoolAt(L"debugMode", false);
        }
        return parsed->BoolAt(L"debugMode", false);
    }

    // Turn the DEBUG escape hatch ON for this process (idempotent, one-way — never disables). Feeds
    // IsDebugPackage()/IsDevOrDebugPackage() through the process-local override, so it is the programmatic
    // equivalent of passing --debug. Used by ApplyPersistedDebugMode; not otherwise called.
    inline void ForceDebugMode()
    {
        detail::DebugForced().store(true, std::memory_order_relaxed);
    }

    // Apply the persisted "Enable Debug Mode" setting: if settings.json turns it on, force the escape
    // hatch for THIS module AND export AGENTMASTER_DEBUG=1 into the process env block so EVERY module
    // resolves identically. The latch alone is NOT enough: detail::DebugForced() is one PRIVATE copy per
    // linked binary (see its comment), and the startup caller runs in WindowsTerminal.EXE — which left
    // TerminalApp.DLL (the Engine's scheduler gate + every Auto Testing UI gate) reading FALSE: the cog
    // toggle enabled debug in the EXE only, and the Tests Autorunner stayed dead in a release install
    // ("enabled debug mode and restarted, the test runner did not get enabled across the entire app").
    // The env block IS per-process — the same cross-module channel AGENTMASTER_PROFILE already rides
    // (ResolveProfileDir exports it so every module resolves identically) — and IsDebugPackage()'s
    // first-use scan picks it up in any module whose first gate read happens after this runs. Child-
    // process note: a child CAN inherit the exported var (exactly as if the user had set the documented
    // AGENTMASTER_DEBUG knob themselves) — benign in practice: WT regenerates a tab child's env from the
    // registry (dropping it) unless reloadEnvironmentVariables is off, and a same-profile relaunch
    // re-derives the verdict from settings.json anyway (toggle OFF ⇒ no export ⇒ debug off).
    // Call at startup — AFTER the profile is resolved and BEFORE engine init or any UI reads
    // IsDebugPackage() (WindowEmperor::HandleCommandlineArgs), and idempotently again from the DLL's own
    // process-once engine wiring (Engine.cpp — the cross-module belt). One-way + idempotent, so the
    // double application is harmless. This is what makes the cog toggle take effect on the NEXT start,
    // exactly like relaunching with --debug.
    inline void ApplyPersistedDebugMode()
    {
        if (PersistedDebugModeEnabled())
        {
            ForceDebugMode();
            ::SetEnvironmentVariableW(L"AGENTMASTER_DEBUG", L"1");
        }
    }

    // Dev package OR the debug escape hatch — the predicate every Auto Testing / Tests Autorunner
    // FEATURE gate uses, so those surfaces appear under the AgentmasterDev build AND a `--debug` /
    // AGENTMASTER_DEBUG release. IDENTITY decisions (profile dir, toast CLSID, reopen alias, updater
    // channel, the "Agent Manager Dev" title/branding) deliberately stay on IsDevPackage() — they are
    // about WHICH install this is, not whether the dev feature set is unlocked.
    inline bool IsDevOrDebugPackage()
    {
        return IsDevPackage() || IsDebugPackage();
    }

    // The key an install's profile choice is stored under: the PFN (per-package — release and
    // dev each remember their own), or "Unpackaged" for portable/loose runs.
    inline std::wstring PackageKey()
    {
        auto pfn = PackageFamilyName();
        return pfn.empty() ? std::wstring{ L"Unpackaged" } : pfn;
    }

    // %USERPROFILE%, with the same fallback chain the historical AgentmasterStateDir used.
    // Returns { baseDir, isRealHome }: when isRealHome is false the caller must use the
    // visible "Agentmaster" leaf instead of a dotfile (matches the legacy behavior).
    inline std::pair<std::wstring, bool> HomeBase()
    {
        auto home = detail::GetEnvVar(L"USERPROFILE");
        if (!home.empty())
        {
            return { home, true };
        }
        auto fallback = detail::GetEnvVar(L"LOCALAPPDATA");
        if (fallback.empty())
        {
            fallback = detail::GetEnvVar(L"TEMP");
        }
        if (fallback.empty())
        {
            fallback = L".";
        }
        return { fallback, false };
    }

    inline std::wstring DefaultReleaseProfileDir()
    {
        const auto [base, real] = HomeBase();
        return base + (real ? L"\\.agentmaster" : L"\\Agentmaster");
    }

    inline std::wstring DefaultDevProfileDir()
    {
        const auto [base, real] = HomeBase();
        return base + (real ? L"\\.agentmaster-dev" : L"\\Agentmaster-dev");
    }

    // The silent default for THIS binary: keyed off the runtime package identity, so one
    // TerminalApp.dll serves both installs. Unpackaged (tests/tools/portable-without-marker)
    // lands on the release default — the historical ~/.agentmaster, unchanged behavior.
    inline std::wstring DefaultProfileDir()
    {
        return IsDevPackage() ? DefaultDevProfileDir() : DefaultReleaseProfileDir();
    }

    // ------------------------------------------- portable install + the exe-side pointer ----

    namespace detail
    {
        // The folder holding the running PROCESS image ("" on failure), no trailing separator.
        // Deliberately the process exe (GetModuleFileNameW(nullptr)) and not the current module:
        // every module in one process (Agentmaster.exe, TerminalApp.dll, the Model DLL) must
        // agree on the SAME exe-side files, and every shipped layout (package, loose, portable
        // unzip) keeps all binaries — the app exe AND agentmaster-cli.exe — in one folder.
        inline std::wstring ExeDirPath()
        {
            wchar_t buf[MAX_PATH * 2];
            const DWORD n = ::GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
            if (n == 0 || n >= ARRAYSIZE(buf))
            {
                return {};
            }
            const std::wstring exe{ buf, n };
            const size_t cut = exe.find_last_of(L"\\/");
            if (cut == std::wstring::npos || cut == 0)
            {
                return {};
            }
            return exe.substr(0, cut);
        }
    }

    // True when a `.portable` marker sits in exeDir — the TRUE-PORTABLE layout (the release zip
    // ships one; upstream WT keys its own portable settings mode off the same file).
    inline bool IsPortableInstallIn(const std::wstring& exeDir)
    {
        if (exeDir.empty())
        {
            return false;
        }
        try
        {
            return std::filesystem::exists(std::filesystem::path{ exeDir } / L".portable");
        }
        catch (...)
        {
            return false;
        }
    }

    inline bool IsPortableInstall()
    {
        return IsPortableInstallIn(detail::ExeDirPath());
    }

    // The portable-local default profile — <exedir>\profile: the self-contained choice the picker
    // offers first, and the silent fallback when a portable's first-launch prompt is cancelled or
    // headless. "" when the exe dir is unknown.
    inline std::wstring PortableDefaultProfileDirIn(const std::wstring& exeDir)
    {
        return exeDir.empty() ? std::wstring{} : exeDir + L"\\profile";
    }

    inline std::wstring PortableDefaultProfileDir()
    {
        return PortableDefaultProfileDirIn(detail::ExeDirPath());
    }

    // <exedir>\profile.path — the EXE-SIDE PROFILE POINTER: one value line naming the profile
    // folder this exe location uses. A PORTABLE copy's first-launch picker writes it (the
    // per-unzip memory the shared home map cannot provide); an installed copy honors a manually
    // planted one too. Plain UTF-8 text (`#` comments + blank lines skipped, first value line
    // wins) so the EXE parses it without JSON helpers — the choice-file idiom.
    inline constexpr std::wstring_view LocalProfilePointerLeaf{ L"profile.path" };

    inline std::wstring LocalProfilePointerPathIn(const std::wstring& exeDir)
    {
        return exeDir.empty() ? std::wstring{} : exeDir + L"\\" + std::wstring{ LocalProfilePointerLeaf };
    }

    inline std::wstring LocalProfilePointerPath()
    {
        return LocalProfilePointerPathIn(detail::ExeDirPath());
    }

    inline bool LocalProfilePointerExistsIn(const std::wstring& exeDir)
    {
        const auto path = LocalProfilePointerPathIn(exeDir);
        if (path.empty())
        {
            return false;
        }
        try
        {
            return std::filesystem::exists(std::filesystem::path{ path });
        }
        catch (...)
        {
            return false;
        }
    }

    // PURE: the line the pointer file stores for a chosen profile dir. PREFER RELATIVE — but only
    // when the dir sits INSIDE the exe dir (the `profile` case and any subfolder), so the pointer
    // survives the user moving the whole unzip. Anything else — another tree, another drive, the
    // home-dir Default/Development defaults — is stored ABSOLUTE verbatim: a `..`-relative
    // spelling would silently re-anchor to a WRONG (and then auto-created) folder if the unzip
    // moved, which is worse than an absolute path that at least stays honest.
    inline std::wstring EncodeLocalProfilePointer(std::wstring_view exeDir, std::wstring_view profileDir)
    {
        std::wstring exe{ exeDir };
        while (!exe.empty() && (exe.back() == L'\\' || exe.back() == L'/'))
        {
            exe.pop_back();
        }
        const std::wstring dir{ profileDir };
        if (!exe.empty() && dir.size() > exe.size() + 1 &&
            (dir[exe.size()] == L'\\' || dir[exe.size()] == L'/') &&
            detail::SamePath(dir.substr(0, exe.size()), exe))
        {
            return dir.substr(exe.size() + 1); // inside the exe dir -> the relative tail
        }
        return dir;
    }

    // PURE: pointer-file text -> the profile dir it names ("" when it names none). First
    // non-empty, non-`#` line wins; a RELATIVE value resolves against the exe dir; the result is
    // lexically normalized (`./profile`, mixed slashes and `.` segments all collapse).
    inline std::wstring DecodeLocalProfilePointer(std::wstring_view exeDir, std::wstring_view text)
    {
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find(L'\n', pos);
            if (nl == std::wstring_view::npos)
            {
                nl = text.size();
            }
            std::wstring line{ text.substr(pos, nl - pos) };
            pos = nl + 1;
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t'))
            {
                line.pop_back();
            }
            size_t b = 0;
            while (b < line.size() && (line[b] == L' ' || line[b] == L'\t'))
            {
                ++b;
            }
            line.erase(0, b);
            if (line.empty() || line.front() == L'#')
            {
                continue;
            }
            const bool absolute = (line.size() >= 2 && line[1] == L':') || line.front() == L'\\' || line.front() == L'/';
            if (!absolute && exeDir.empty())
            {
                return {}; // a relative pointer with no exe-dir anchor is unusable
            }
            try
            {
                const std::filesystem::path p = absolute ? std::filesystem::path{ line } :
                                                           (std::filesystem::path{ std::wstring{ exeDir } } / line);
                return p.lexically_normal().wstring();
            }
            catch (...)
            {
                return {};
            }
        }
        return {};
    }

    // The pointer's resolved profile dir ("" when the file is absent / empty / comments-only).
    inline std::wstring ReadLocalProfilePointerIn(const std::wstring& exeDir)
    {
        const auto path = LocalProfilePointerPathIn(exeDir);
        if (path.empty())
        {
            return {};
        }
        return DecodeLocalProfilePointer(exeDir, detail::ReadUtf8File(path));
    }

    inline std::wstring ReadLocalProfilePointer()
    {
        return ReadLocalProfilePointerIn(detail::ExeDirPath());
    }

    // Persist the pointer (atomic tmp + rename, the choice-file idiom). False when the exe dir
    // refuses the write — a portable on read-only media simply asks again next launch.
    inline bool SaveLocalProfilePointerIn(const std::wstring& exeDir, const std::wstring& profileDir)
    {
        const auto path = LocalProfilePointerPathIn(exeDir);
        if (path.empty() || profileDir.empty())
        {
            return false;
        }
        try
        {
            std::string bytes = "# Agentmaster profile pointer: the profile folder THIS exe location uses.\n"
                                "# A relative line resolves against the exe's folder (a moved portable keeps working).\n";
            bytes += detail::WideToUtf8(EncodeLocalProfilePointer(exeDir, profileDir));
            bytes += '\n';
            const std::wstring tmp = path + L".tmp";
            {
                std::ofstream f{ std::filesystem::path{ tmp }, std::ios::binary | std::ios::trunc };
                if (!f)
                {
                    return false;
                }
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!f.good())
                {
                    return false;
                }
            }
            return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }
        catch (...)
        {
            return false;
        }
    }

    inline bool SaveLocalProfilePointer(const std::wstring& profileDir)
    {
        return SaveLocalProfilePointerIn(detail::ExeDirPath(), profileDir);
    }

    // ------------------------------------------------------------------- the choice file ----

    // %USERPROFILE%\.agentmaster.profiles — one line per install: <packageKey>=<absolute dir>.
    // Plain UTF-8 text (not JSON) so the WindowsTerminal EXE — which links TerminalApp.dll, not
    // the TerminalAppLib static lib — can parse it without any JSON helper.
    inline std::wstring ChoiceFilePath()
    {
        const auto [base, real] = HomeBase();
        return base + (real ? L"\\.agentmaster.profiles" : L"\\Agentmaster.profiles");
    }

    inline std::map<std::wstring, std::wstring> ReadChoiceFile(const std::wstring& path)
    {
        std::map<std::wstring, std::wstring> out;
        try
        {
            std::ifstream in{ std::filesystem::path{ path }, std::ios::binary };
            if (!in)
            {
                return out;
            }
            const std::string bytes{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
            size_t pos = 0;
            while (pos <= bytes.size())
            {
                size_t nl = bytes.find('\n', pos);
                if (nl == std::string::npos)
                {
                    nl = bytes.size();
                }
                std::string line = bytes.substr(pos, nl - pos);
                pos = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                {
                    line.pop_back();
                }
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string::npos || eq == 0)
                {
                    continue;
                }
                const auto key = detail::Utf8ToWide(std::string_view{ line }.substr(0, eq));
                const auto val = detail::Utf8ToWide(std::string_view{ line }.substr(eq + 1));
                if (!key.empty() && !val.empty())
                {
                    out[key] = val;
                }
            }
        }
        catch (...)
        {
        }
        return out;
    }

    inline bool WriteChoiceFile(const std::wstring& path, const std::map<std::wstring, std::wstring>& entries)
    {
        try
        {
            std::string bytes = "# Agentmaster profile map: <package family name|Unpackaged>=<profile folder>\n";
            for (const auto& [k, v] : entries)
            {
                bytes += detail::WideToUtf8(k);
                bytes += '=';
                bytes += detail::WideToUtf8(v);
                bytes += '\n';
            }
            const std::wstring tmp = path + L".tmp";
            {
                std::ofstream f{ std::filesystem::path{ tmp }, std::ios::binary | std::ios::trunc };
                if (!f)
                {
                    return false;
                }
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!f.good())
                {
                    return false;
                }
            }
            return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }
        catch (...)
        {
            return false;
        }
    }

    // The saved choice for THIS install ("" when none — first launch).
    inline std::wstring ReadSavedChoice()
    {
        const auto entries = ReadChoiceFile(ChoiceFilePath());
        const auto it = entries.find(PackageKey());
        return it == entries.end() ? std::wstring{} : it->second;
    }

    // Persist the choice for THIS install (read-modify-write keeps the other installs' slots).
    inline bool SaveChoice(const std::wstring& profileDir)
    {
        const auto path = ChoiceFilePath();
        auto entries = ReadChoiceFile(path);
        entries[PackageKey()] = profileDir;
        return WriteChoiceFile(path, entries);
    }

    // Persist a profile choice WHERE THIS INSTALL'S RESOLUTION READS IT BACK — the one rule that
    // keeps the cog's "Change profile folder…" honest for every layout. A PORTABLE copy (and any
    // copy already steered by an exe-side pointer file, which outranks the home map) writes the
    // pointer NEXT TO THE EXE; everything else writes its slot in ~/.agentmaster.profiles. False
    // == the choice could not be stored anywhere resolution would actually read it (a portable
    // whose exe dir refuses the write — its resolution never consults the home map, so a map
    // write would be a DEAD choice; same for a pointer-steered install whose pointer can't be
    // rewritten, since the stale pointer would keep outranking the map). The caller should say
    // so instead of pretending the change took.
    inline bool PersistProfileChoiceIn(const std::wstring& exeDir, const std::wstring& profileDir)
    {
        if (IsPortableInstallIn(exeDir) || LocalProfilePointerExistsIn(exeDir))
        {
            return SaveLocalProfilePointerIn(exeDir, profileDir);
        }
        return SaveChoice(profileDir);
    }

    inline bool PersistProfileChoice(const std::wstring& profileDir)
    {
        return PersistProfileChoiceIn(detail::ExeDirPath(), profileDir);
    }

    // WHERE PersistProfileChoice will write for this install — for error messages + display.
    inline std::wstring ProfileChoiceStorePath()
    {
        const auto exeDir = detail::ExeDirPath();
        if (IsPortableInstallIn(exeDir) || LocalProfilePointerExistsIn(exeDir))
        {
            return LocalProfilePointerPathIn(exeDir);
        }
        return ChoiceFilePath();
    }

    // ----------------------------------------------------------------------- resolution ----

    // The full precedence WITHOUT UI and WITHOUT caching (tests use this directly).
    inline std::wstring ResolveProfileDirUncached()
    {
        if (auto env = detail::GetEnvVar(L"AGENTMASTER_PROFILE"); !env.empty())
        {
            return env;
        }
        const auto exeDir = detail::ExeDirPath();
        if (auto pinned = ReadLocalProfilePointerIn(exeDir); !pinned.empty())
        {
            return pinned; // the exe-side pointer: a portable's remembered choice (honored for installed copies too)
        }
        if (IsPortableInstallIn(exeDir))
        {
            // Portable with nothing chosen yet: the classic self-contained default. The EXE
            // prelude prompts here (EnsureProfileResolvedAtStartup); library/headless resolution
            // never does (Rule #15). NEVER fall through to the home map: its one "Unpackaged"
            // slot is shared by every unzip AND legacy unpackaged tools — reading it would
            // cross-contaminate copies that must stay self-contained.
            return PortableDefaultProfileDirIn(exeDir);
        }
        if (auto saved = ReadSavedChoice(); !saved.empty())
        {
            return saved;
        }
        return DefaultProfileDir();
    }

    // The process-wide resolved profile dir. Cached: a profile cannot change mid-run (the cog's
    // "Change profile folder…" saves the pointer and applies on the next launch). In the real
    // app the WindowEmperor bootstrap exported AGENTMASTER_PROFILE long before any engine code
    // first calls this, so every module resolves the same dir.
    inline const std::wstring& ResolveProfileDir()
    {
        static const std::wstring dir = []() {
            auto d = ResolveProfileDirUncached();
            detail::EnsureDirExists(d);
            return d;
        }();
        return dir;
    }

    // ------------------------------------------------------------- migration / seeding ----

    // Copy an existing state folder into a (typically fresh) profile. skip_existing — never
    // clobber data already in the target. Excludes the things that must NOT travel: locks/
    // (the build-launch mutex, machine-global dev tooling), shim/ + bridge.json (both embed
    // absolute paths / a live pipe name; the engine regenerates them at init), bin/ (the
    // portable INSTALL folder PortableInstall.h places INSIDE the Default profile —
    // ~/.agentmaster\bin — binaries, not state: copying it would drag ~200 MB of exe/dll into
    // every migrated profile), and *.tmp.
    inline void MigrateProfileData(const std::wstring& fromDir, const std::wstring& toDir)
    {
        try
        {
            if (fromDir.empty() || toDir.empty() || detail::SamePath(fromDir, toDir))
            {
                return;
            }
            const std::filesystem::path src{ fromDir };
            const std::filesystem::path dst{ toDir };
            if (!std::filesystem::exists(src))
            {
                return;
            }
            std::filesystem::create_directories(dst);
            for (const auto& entry : std::filesystem::directory_iterator{ src })
            {
                const auto leaf = entry.path().filename().wstring();
                const auto leafKey = detail::NormPathKey(leaf);
                if (leafKey == L"locks" || leafKey == L"shim" || leafKey == L"bridge.json" || leafKey == L"bin" ||
                    (leafKey.size() > 4 && leafKey.compare(leafKey.size() - 4, 4, L".tmp") == 0))
                {
                    continue;
                }
                std::error_code ec; // best-effort per entry; a locked file must not abort the rest
                std::filesystem::copy(entry.path(), dst / entry.path().filename(),
                                      std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
                                      ec);
            }
        }
        catch (...)
        {
        }
    }

    // Seed <profile>\terminal from an EXPLICIT source folder holding Terminal's settings.json /
    // state.json. Copy-if-target-absent per file, so it is idempotent and never clobbers.
    inline void SeedTerminalSettingsFromDir(const std::wstring& fromDir, const std::wstring& profileDir)
    {
        try
        {
            if (fromDir.empty() || profileDir.empty())
            {
                return;
            }
            const std::filesystem::path stock{ fromDir };
            std::error_code probe;
            if (!std::filesystem::exists(stock, probe))
            {
                return;
            }
            const std::filesystem::path target = std::filesystem::path{ profileDir } / L"terminal";
            std::filesystem::create_directories(target);
            for (const auto* leaf : { L"settings.json", L"state.json", L"elevated-state.json" })
            {
                const auto from = stock / leaf;
                const auto to = target / leaf;
                std::error_code ec;
                if (std::filesystem::exists(from, ec) && !std::filesystem::exists(to, ec))
                {
                    std::filesystem::copy_file(from, to, ec);
                }
            }
        }
        catch (...)
        {
        }
    }

    // Seed <profile>\terminal with the Terminal settings this install currently uses, so the
    // first profile-redirected launch looks identical instead of resetting to defaults. The
    // source is where this install kept them BEFORE the profile redirect: a PORTABLE copy →
    // <exedir>\settings (upstream WT portable mode, which the AGENTMASTER_PROFILE redirect now
    // outranks); packaged → %LOCALAPPDATA%\Packages\<PFN>\LocalState; unpackaged →
    // %LOCALAPPDATA%\Microsoft\Windows Terminal. Copy-if-target-absent only.
    inline void SeedTerminalSettings(const std::wstring& profileDir)
    {
        try
        {
            if (profileDir.empty())
            {
                return;
            }
            if (const auto exeDir = detail::ExeDirPath(); IsPortableInstallIn(exeDir))
            {
                SeedTerminalSettingsFromDir(exeDir + L"\\settings", profileDir);
                return;
            }
            const auto localAppData = detail::GetEnvVar(L"LOCALAPPDATA");
            if (localAppData.empty())
            {
                return;
            }
            std::filesystem::path stock{ localAppData };
            const auto pfn = PackageFamilyName();
            if (!pfn.empty())
            {
                stock = stock / L"Packages" / pfn / L"LocalState";
            }
            else
            {
                stock = stock / L"Microsoft" / L"Windows Terminal";
            }
            SeedTerminalSettingsFromDir(stock.wstring(), profileDir);
        }
        catch (...)
        {
        }
    }

    // ------------------------------------------------------------------------ the picker ----

    struct PickerResult
    {
        std::wstring dir; // chosen profile folder (empty when !chosen)
        bool chosen = false; // false == the user cancelled (caller keeps the silent default and asks again next launch)
        bool migrate = false; // the "copy existing data" verification was ticked (and applies)
    };

    namespace detail
    {
        // IFileDialog in FOS_PICKFOLDERS mode (the modern folder browser). Returns "" on cancel.
        // `title` names the dialog: the profile picker's default, or the portable installer's
        // "Choose where to install Agentmaster" (PortableInstall.h).
        inline std::wstring BrowseForFolder(HWND owner, const wchar_t* title = L"Choose the Agentmaster profile folder")
        {
            std::wstring picked;
            const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            {
                IFileOpenDialog* dlg = nullptr;
                // __uuidof(FileOpenDialog): the coclass GUID via the compiler intrinsic — no
                // CLSID_FileOpenDialog extern, so consumers need no uuid.lib at link time.
                if (SUCCEEDED(::CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg)
                {
                    DWORD opts = 0;
                    dlg->GetOptions(&opts);
                    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                    dlg->SetTitle(title ? title : L"Choose the Agentmaster profile folder");
                    if (SUCCEEDED(dlg->Show(owner)))
                    {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(dlg->GetResult(&item)) && item)
                        {
                            PWSTR psz = nullptr;
                            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
                            {
                                picked = psz;
                                ::CoTaskMemFree(psz);
                            }
                            item->Release();
                        }
                    }
                    dlg->Release();
                }
            }
            if (coInit == S_OK || coInit == S_FALSE)
            {
                ::CoUninitialize();
            }
            return picked;
        }
    }

    // True when `dir` exists AND holds at least one entry — the picker marks such options with
    // "existing data" so the choice between "fresh start" and "adopt what's already there" is
    // informed (a Default profile another install populated, an older portable's ./profile). An
    // auto-created EMPTY folder deliberately reads as no data.
    inline bool ProfileDirHasData(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return false;
        }
        try
        {
            const std::filesystem::path p{ dir };
            if (!std::filesystem::is_directory(p))
            {
                return false;
            }
            return std::filesystem::directory_iterator{ p } != std::filesystem::directory_iterator{};
        }
        catch (...)
        {
            return false;
        }
    }

    // The profile picker. `migrateSource` is the folder offered by the "copy existing data"
    // checkbox (a portable's first launch: the pre-existing <exedir>\profile, when one exists;
    // the cog's Change…: the active profile). Pure Win32 (TaskDialogIndirect needs the
    // Common-Controls v6 manifest dependency, which WindowsTerminal.manifest declares). Loops
    // back from a cancelled Browse…. On a PORTABLE copy (the `.portable` marker) a leading
    // "Portable profile" command link offers the self-contained <exedir>\profile and is the
    // default — for everyone else the picker is byte-identical to before. Every fixed option's
    // path line appends "— existing data" when that folder already holds something
    // (ProfileDirHasData), so a profile that EXISTS is visibly different from a fresh one.
    inline PickerResult ShowProfilePicker(HWND owner, bool firstLaunch, const std::wstring& migrateSource)
    {
        PickerResult result;

        const std::wstring releaseDir = DefaultReleaseProfileDir();
        const std::wstring devDir = DefaultDevProfileDir();
        const bool dev = IsDevPackage();
        const bool packaged = !PackageFamilyName().empty();
        const std::wstring portableDir = PortableDefaultProfileDir();
        const bool offerPortable = IsPortableInstall() && !portableDir.empty();

        bool offerMigrate = false;
        try
        {
            offerMigrate = !migrateSource.empty() && std::filesystem::exists(std::filesystem::path{ migrateSource });
        }
        catch (...)
        {
        }

        const std::wstring title = L"Agentmaster";
        const std::wstring instruction = firstLaunch ? L"Choose a profile for this installation" :
                                                       L"Switch the profile folder";
        std::wstring content =
            L"A profile folder holds everything Agentmaster stores: sessions, test queues, window "
            L"layouts, settings and hooks. Each installation remembers its own choice, so the "
            L"release and development installs never touch each other's data.\n\nAsking: ";
        content += offerPortable ? L"a PORTABLE copy \x2014 its choice is remembered next to the exe (profile.path; kept relative when the profile lives inside this folder, so the unzip stays movable)." :
                   !packaged     ? L"a portable/unpackaged copy." :
                   dev           ? L"the DEVELOPMENT install (AgentmasterDev)." :
                                   L"the RELEASE install (Agentmaster).";
        if (!firstLaunch)
        {
            content += L"\n\nThe new profile takes effect the next time Agentmaster starts.";
        }

        const auto dirLine = [](const std::wstring& dir) {
            return ProfileDirHasData(dir) ? dir + L"  \x2014  existing data" : dir;
        };
        const std::wstring portableLabel = L"Portable profile (self-contained)\n" + dirLine(portableDir);
        const std::wstring prodLabel = L"Default profile\n" + dirLine(releaseDir);
        const std::wstring devLabel = L"Development profile\n" + dirLine(devDir);
        const std::wstring browseLabel = L"Browse for a profile folder…\nUse any folder (a synced drive, a per-project location, …)";
        const std::wstring verification = L"Copy existing data from " + migrateSource + L" into the chosen profile";
        const std::wstring footer = L"Change this later from the Manager tab \x2192 \x2699 Settings \x2192 Profile.";

        constexpr int idProd = 1001;
        constexpr int idDev = 1002;
        constexpr int idBrowse = 1003;
        constexpr int idPortable = 1004;
        std::vector<TASKDIALOG_BUTTON> buttons;
        if (offerPortable)
        {
            buttons.push_back({ idPortable, portableLabel.c_str() });
        }
        buttons.push_back({ idProd, prodLabel.c_str() });
        buttons.push_back({ idDev, devLabel.c_str() });
        buttons.push_back({ idBrowse, browseLabel.c_str() });

        for (;;)
        {
            TASKDIALOGCONFIG cfg{};
            cfg.cbSize = sizeof(cfg);
            cfg.hwndParent = owner;
            cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
            cfg.pszWindowTitle = title.c_str();
            cfg.pszMainIcon = TD_INFORMATION_ICON;
            cfg.pszMainInstruction = instruction.c_str();
            cfg.pszContent = content.c_str();
            cfg.cButtons = static_cast<UINT>(buttons.size());
            cfg.pButtons = buttons.data();
            cfg.nDefaultButton = offerPortable ? idPortable : (dev ? idDev : idProd);
            cfg.pszFooter = footer.c_str();
            cfg.pszFooterIcon = TD_INFORMATION_ICON;
            if (offerMigrate)
            {
                cfg.pszVerificationText = verification.c_str();
                cfg.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
            }

            int pressed = 0;
            BOOL verified = FALSE;
            if (FAILED(::TaskDialogIndirect(&cfg, &pressed, nullptr, &verified)))
            {
                return result; // comctl v6 unavailable / unexpected failure -> behave like cancel
            }

            std::wstring dir;
            switch (pressed)
            {
            case idPortable:
                dir = portableDir;
                break;
            case idProd:
                dir = releaseDir;
                break;
            case idDev:
                dir = devDir;
                break;
            case idBrowse:
                dir = detail::BrowseForFolder(owner);
                if (dir.empty())
                {
                    continue; // browse cancelled -> back to the picker
                }
                break;
            default:
                return result; // Cancel / X -> not chosen
            }

            result.dir = std::move(dir);
            result.chosen = true;
            result.migrate = offerMigrate && verified && !detail::SamePath(result.dir, migrateSource);
            return result;
        }
    }

    // ----------------------------------------------------------- the startup entry point ----

    namespace detail
    {
        // One live instance per profile: a kernel mutex named after the normalized dir. The
        // handle is deliberately leaked (process lifetime); the OS releases it on ANY exit, so
        // it can never go stale. Returns false when another process already holds the profile.
        inline bool AcquireProfileMutex(const std::wstring& profileDir)
        {
            static HANDLE held = nullptr;
            if (held)
            {
                return true;
            }
            wchar_t name[64];
            ::swprintf_s(name, L"Local\\Agentmaster.profile.%016llx",
                         static_cast<unsigned long long>(Fnv1a64(NormPathKey(profileDir))));
            const HANDLE h = ::CreateMutexW(nullptr, TRUE, name);
            if (!h)
            {
                return true; // can't tell -> don't block startup
            }
            if (::GetLastError() == ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(h);
                return false;
            }
            held = h;
            return true;
        }
    }

    // The WindowEmperor calls this ONCE, right after winning the single-instance handoff and
    // BEFORE anything reads persisted state (Terminal settings via the GetBaseSettingsPath
    // redirect, ApplicationState, the windows/<id>.json reopen scan, and — later — the engine's
    // AgentmasterStateDir). An INSTALLED copy's first launch (no env / pointer / saved choice)
    // AUTO-SELECTS the per-identity default profile (release → Default, dev → Development) and
    // persists it — no prompt. A PORTABLE copy's first launch (marker, no pointer yet) instead
    // PROMPTS (Portable <exedir>\profile / Default / Development / Browse…) and remembers the
    // answer in the exe-side pointer file; Cancel — and `allowUi == false` (a `-Embedding` COM
    // activation / defterm handoff must never block on a dialog) — lands on the self-contained
    // <exedir>\profile WITHOUT persisting, so the next interactive launch asks again. `allowUi`
    // therefore gates that portable prompt AND the "profile already in use by another instance"
    // warning below; the installed auto-pick needs no UI either way. `deferPortablePicker` (the
    // portable INSTALL question's "Ask me later" — PortableInstall.h) skips the portable picker
    // for THIS launch only: the copy runs on the self-contained <exedir>\profile without
    // persisting a choice (exactly the Cancel path), so one "later" defers every first-launch
    // question at once instead of trading one dialog for another; the in-use warning still shows.
    // Returns false ONLY when the profile is held by another live instance and the user chose not
    // to continue — caller exits.
    inline bool EnsureProfileResolvedAtStartup(bool allowUi, bool deferPortablePicker = false)
    {
        std::wstring dir = detail::GetEnvVar(L"AGENTMASTER_PROFILE");
        if (dir.empty())
        {
            const auto exeDir = detail::ExeDirPath();
            dir = ReadLocalProfilePointerIn(exeDir); // a portable's remembered choice (installed copies honor a planted one too)
            if (IsPortableInstallIn(exeDir))
            {
                if (dir.empty() && allowUi && !deferPortablePicker)
                {
                    // FIRST INTERACTIVE LAUNCH of a PORTABLE copy: ask. The migrate checkbox
                    // offers the pre-existing <exedir>\profile when one holds data already (an
                    // older portable build used it unconditionally), so upgrading users can carry
                    // it into a non-portable pick; picking Portable itself keeps that data in
                    // place (SamePath makes the migrate a no-op).
                    std::wstring migrateSource = PortableDefaultProfileDirIn(exeDir);
                    try
                    {
                        if (!std::filesystem::exists(std::filesystem::path{ migrateSource }))
                        {
                            migrateSource.clear();
                        }
                    }
                    catch (...)
                    {
                        migrateSource.clear();
                    }
                    const auto pick = ShowProfilePicker(nullptr, true, migrateSource);
                    if (pick.chosen)
                    {
                        dir = pick.dir;
                        detail::EnsureDirExists(dir);
                        // Best-effort: an unwritable exe dir (read-only media) simply asks again
                        // next launch — the session still runs on the chosen dir.
                        SaveLocalProfilePointerIn(exeDir, dir);
                        if (pick.migrate)
                        {
                            MigrateProfileData(migrateSource, dir);
                        }
                    }
                }
                if (dir.empty())
                {
                    dir = PortableDefaultProfileDirIn(exeDir); // cancelled / headless: self-contained, un-persisted
                }
                // The pre-choice portable kept Terminal's own settings in <exedir>\settings
                // (upstream WT portable mode, which the AGENTMASTER_PROFILE redirect now
                // outranks) — seed <profile>\terminal from there so the first profile-homed
                // launch keeps the user's terminal look. Copy-if-absent ⇒ a no-op every launch
                // after the first, and it never clobbers a profile that already has settings.
                SeedTerminalSettings(dir);
            }
        }
        if (dir.empty())
        {
            dir = ReadSavedChoice();
        }
        if (dir.empty())
        {
            // First launch of an INSTALLED copy: AUTO-SELECT the per-identity default profile and
            // remember it, WITHOUT prompting — the RELEASE install picks the Default profile
            // (~/.agentmaster), the DEV install picks the Development profile (~/.agentmaster-dev).
            // (The Default / Development / Browse… TaskDialog picker — ShowProfilePicker — is
            // shown by a PORTABLE first launch above and by the cog's "Change profile folder…".)
            // The choice is keyed off the package identity (DefaultProfileDir == IsDevPackage() ?
            // dev : release), so it needs no UI and runs for headless -Embedding/defterm
            // activations too. It stays changeable later from the cog's "Change profile folder…".
            dir = DefaultProfileDir();
            detail::EnsureDirExists(dir);
            SaveChoice(dir);
            SeedTerminalSettings(dir);
        }

        detail::EnsureDirExists(dir);
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", dir.c_str());

        if (!detail::AcquireProfileMutex(dir))
        {
            if (!allowUi)
            {
                return true; // headless: proceed (the defterm window is better than a hang)
            }
            const std::wstring warn =
                L"The profile\n\n    " + dir +
                L"\n\nis already in use by another running Agentmaster instance. Running two instances "
                L"on one profile can corrupt its data (sessions, window records, settings).\n\nContinue anyway?";
            if (::MessageBoxW(nullptr, warn.c_str(), L"Agentmaster \x2014 profile in use", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            {
                return false;
            }
        }
        return true;
    }
}
