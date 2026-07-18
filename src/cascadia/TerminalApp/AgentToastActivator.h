// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (System notifications; NOTIFICATIONS.md §4a) — the TOAST COM ACTIVATOR.
//
// WHY THIS EXISTS (the "click opens a stray window" bug):
// A packaged app's toast click is dispatched by the shell. With NO ToastActivatorCLSID registered,
// the shell falls back to a plain **AUMID activation** of the package — which for our
// FullTrustApplication means LAUNCHING WindowsTerminal.exe with no arguments. That launch hits the
// single-instance handoff and the running Emperor obligingly opens a brand-new window with a default
// tab. So a click produced BOTH the in-process jump (our ToastNotification.Activated handler) AND a
// stray window. There is no way to tell that launch apart from a user typing `agentmasterdev` — the
// activation reason simply isn't carried on the commandline.
//
// The FIX is the documented mechanism: declare a ToastActivatorCLSID in the manifest and register a
// COM class object for it while we run. The shell then CoCreateInstances that CLSID INSTEAD of
// activating the AUMID; COM's SCM finds our already-registered class object and calls
// INotificationActivationCallback::Activate IN THE RUNNING PROCESS — **no second process is ever
// launched, so the stray window cannot exist by construction** (not "is suppressed afterwards" — the
// launch never happens). `invokedArgs` carries the toast's `launch` string, which we set to the
// session id, so the callback knows exactly which session to surface.
//
// COLD START (no instance running): the SCM launches our ExeServer registration —
// `WindowsTerminal.exe -ToastActivated -Embedding` (the manifest's Arguments + COM's own -Embedding).
// WindowEmperor treats -ToastActivated as a NORMAL startup (it strips the pair and restores the
// workspace, rather than taking the defterm -Embedding path); this registration then happens at engine
// init and the SCM's pending activation completes into Activate() below, which jumps. If the SCM times
// out first the user still just gets their app back — no worse than before, and a fresh window IS the
// right answer when nothing was open.
//
// Header-only + included by exactly ONE TU (TerminalPage.AgentEngine.cpp, from the process-once engine
// init) — the PromptAnchor.h / PendingInput.h idiom, so it needs no TerminalAppLib.vcxproj entry. It
// lives in TerminalApp.dll rather than the EXE because the jump routes through the engine's activate
// fan-out (Engine.h), which the WindowsTerminal EXE deliberately does not link (it only takes the
// header-only engine bits: ProfileBootstrap.h / Updater.h / Splash.h).
#pragma once

#include <NotificationActivationCallback.h> // INotificationActivationCallback (the shell's toast-activation contract)
#include <objbase.h>
#include <string>

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / LogNav / ShortId
#include "AgentMaster/Engine.h" // ActivateSessionInOtherWindows — the cross-window "surface this session" fan-out
#include "AgentMaster/ProfileBootstrap.h" // Profiles::IsDevPackage — pick this identity's CLSID

namespace Agentmaster::ToastActivator
{
    // The per-IDENTITY activator CLSIDs. Release and dev install SIDE BY SIDE (PROFILES.md), and a
    // CLSID is a machine-global COM key — sharing one would let whichever instance registered last
    // steal the other's toast clicks. So each identity declares its OWN, in its own manifest
    // (Package-Rel / Package-Dev), and we pick by package family at runtime. (Deliberately NOT more
    // of the shared-CLSID debt PROFILES.md §5 tracks for the defterm/shellext GUIDs.)
    // Keep these in lockstep with the manifests' <desktop:ToastNotificationActivation
    // ToastActivatorCLSID> + <com:Class Id> — a mismatch silently reverts to the stray-window
    // fallback (the shell can't resolve the CLSID, so it activates the AUMID instead).
    // {7608CBBC-E01D-4807-852F-0B630A1DDC51} — Agentmaster (release)
    inline constexpr CLSID kReleaseClsid{ 0x7608cbbc, 0xe01d, 0x4807, { 0x85, 0x2f, 0x0b, 0x63, 0x0a, 0x1d, 0xdc, 0x51 } };
    // {6CB0FAE1-2D27-4103-B55A-9E40C885BBBF} — AgentmasterDev
    inline constexpr CLSID kDevClsid{ 0x6cb0fae1, 0x2d27, 0x4103, { 0xb5, 0x5a, 0x9e, 0x40, 0xc8, 0x85, 0xbb, 0xbf } };

    inline const CLSID& ClsidForThisPackage()
    {
        return ::Agentmaster::Profiles::IsDevPackage() ? kDevClsid : kReleaseClsid;
    }

    // The shell's callback. Runs on an RPC/COM thread — it must NOT touch UI state directly, and it
    // must be fast (the shell waits on it). ActivateSessionInOtherWindows is thread-safe (it copies
    // the sink table under the engine lock) and every window's sink hops to its OWN UI thread before
    // touching tabs, so handing off here is both correct and immediate.
    struct NotificationActivator : winrt::implements<NotificationActivator, INotificationActivationCallback>
    {
        HRESULT STDMETHODCALLTYPE Activate(LPCWSTR /*appUserModelId*/,
                                           LPCWSTR invokedArgs,
                                           const NOTIFICATION_USER_INPUT_DATA* /*data*/,
                                           ULONG /*count*/) noexcept override
        try
        {
            // invokedArgs == the toast's `launch` string == the session id we stamped in
            // _ShowAgentSessionToast. Empty (a toast from an older build, or a body-less activation)
            // is a no-op rather than a guess — surfacing the WRONG session is worse than none.
            const std::wstring sessionId{ invokedArgs ? invokedArgs : L"" };
            if (sessionId.empty())
            {
                return S_OK;
            }
            // Nav audit: the user clicked a session's completion toast — the shell-side jump into the
            // fleet. Logged HERE (not at the window) so the trail records the click even if no window
            // ends up hosting the session (closed since the toast was shown).
            ::Agentmaster::LogNav(L"notify-click " + ::Agentmaster::ShortId(sessionId) + L" (toast -> foreground window + jump to tab)");
            // Fan out with an EMPTY source window id, so NO window is excluded: we are not "a window
            // asking the others", we are the shell asking the fleet — whichever window hosts the tab
            // selects it and foregrounds itself (_FocusClaudeSessionTab(id, bringWindowToFront=true)).
            // A session whose tab was closed since the toast was shown simply finds no host: every
            // window misses and nothing happens (no stray window — the whole point of this class).
            ::Agentmaster::ActivateSessionInOtherWindows(sessionId, L"");
            return S_OK;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"toast activate (click jump)");
            return S_OK; // never fail back into the shell over a jump we couldn't make
        }
    };

    struct ActivatorFactory : winrt::implements<ActivatorFactory, IClassFactory>
    {
        HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) noexcept override
        {
            *result = nullptr;
            if (outer)
            {
                return CLASS_E_NOAGGREGATION;
            }
            return winrt::make<NotificationActivator>()->QueryInterface(iid, result);
        }

        HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
        {
            return S_OK; // our lifetime is the process's (the Emperor owns it), not COM's
        }
    };

    namespace detail
    {
        inline bool g_registered{ false }; // set by Register(); read by IsRegistered() (UI thread / engine init only)
    }

    // Did the COM activator take? TRUE => a toast click is delivered to Activate() above and NO second
    // process is launched, so the toast must NOT also wire the legacy in-process handler (it would jump
    // twice and log two [nav] lines). FALSE => this build can't be COM-activated (unpackaged, or a
    // package registered before the manifests carried the CLSID — i.e. the loose layout wasn't
    // re-registered after this change), so the toast keeps its in-process ToastNotification.Activated
    // fallback: the jump still works exactly as it did, stray window and all. Never a regression.
    inline bool IsRegistered() noexcept
    {
        return detail::g_registered;
    }

    // Register the class object process-wide. Call ONCE, from the engine's process-once init.
    // REGCLS_MULTIPLEUSE: one class object serves every activation for the process lifetime — which is
    // exactly what makes the shell reuse THIS process instead of launching the ExeServer.
    // Best-effort: an UNPACKAGED build has no manifest CLSID (and no AUMID, so it can raise no toasts
    // either) — the registration is harmless there, and a failure only means we fall back to the
    // pre-fix behavior, never a crash.
    inline void Register()
    {
        static DWORD cookie = 0;
        if (cookie != 0)
        {
            return; // already registered (the once-init guard's belt)
        }
        try
        {
            const auto factory = winrt::make<ActivatorFactory>();
            const auto hr = ::CoRegisterClassObject(ClsidForThisPackage(),
                                                    factory.get(),
                                                    CLSCTX_LOCAL_SERVER,
                                                    REGCLS_MULTIPLEUSE,
                                                    &cookie);
            if (FAILED(hr))
            {
                cookie = 0;
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[notify] toast activator registration failed (hr=0x" +
                                                  [&] { wchar_t b[16]{}; swprintf_s(b, L"%08X", static_cast<unsigned>(hr)); return std::wstring{ b }; }() +
                                                  L") - toast clicks fall back to the in-process handler (+ the shell's own app activation)\n");
                return;
            }
            detail::g_registered = true;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[notify] toast activator registered (clicks activate this instance in-process; no stray window)\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"toast activator register");
            cookie = 0;
        }
    }
}
