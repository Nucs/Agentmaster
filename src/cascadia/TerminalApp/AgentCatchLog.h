// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster (exception forensics — the "never lose a swallowed exception" policy).
//
// Every catch(...) that swallows must log WHAT was thrown (type + HRESULT + message) and WHERE
// FROM (the throw-site callstack). The catch site runs after the unwind — the throw stack is
// gone by then — so the engine's Vectored Exception Handler (ClaudeSpawn: InstallThrowStackCapture)
// snapshots the raw stack AT RAISE TIME into a per-thread ring, and the loggers here dump it as
// `Module.dll+0xRVA` frames: ASLR-stable and offline-symbolizable against the matching build's
// PDBs with the debug-dumps `diasym` tool, so one hooks.log block pins the exact source line later.
//
// Three pieces (all best-effort + noexcept — a logger must never become a second crash):
//   - AgentLogCaughtException(context): call INSIDE a catch block. Classifies the in-flight
//     exception (winrt::hresult_error / wil::ResultException / std::system_error / std::exception /
//     unknown), then logs "[exc] <context>: swallowed <detail> ..." + the throw stacks, throttled
//     per context (~2s) so a hot-path catch can't flood hooks.log ("[suppressed N ...]" keeps the
//     count honest). The engine-side twin for plain-C++ TUs is Agentmaster::LogSwallowedException.
//   - AgentWilFailureLogger: the wil::SetResultLoggingCallback sink for THIS module
//     (TerminalApp.dll — wil state is per-module), so every existing CATCH_LOG / LOG_* site
//     (the destructor guards, upstream WT code in this DLL) reports into hooks.log as "[wil] ..."
//     instead of vanishing into ETW-only tracing. Includes the newest captured throw stack with an
//     age label (a CATCH_LOG fires mid-catch, so a fresh entry is that exception's raise; a stale
//     age marks it unrelated — e.g. a plain LOG_IF_FAILED with no exception in flight).
//   - InstallAgentExceptionTrace(): process-once install of both (VEH + wil callback); called at
//     engine init (TerminalPage.AgentEngine.cpp, beside the toast-activator registration).
//
// Relies on the TerminalApp pch for winrt/base.h + wil (the AgentTipHelpers.h idiom); the pure
// Win32 core (ring / formatting / throttle / log chokepoint) lives in AgentMaster/ClaudeSpawn.
#pragma once

#include <cstdio> // swprintf_s
#include <mutex> // std::once_flag (the process-once install)

#include "AgentMaster/ClaudeSpawn.h"

namespace Agentmaster
{
    // Call ONLY from inside a catch block (it rethrows to classify — `throw;` with no in-flight
    // exception is std::terminate). Never throws; throttled per `context`.
    inline void AgentLogCaughtException(const wchar_t* context) noexcept
    {
        try
        {
            // Snapshot the throw stacks BEFORE the classification rethrow below — the rethrow is
            // itself captured by the VEH and would displace the entry this call came to read.
            const std::wstring stacks = CaptureRecentThrowStacksText();
            const auto widen = [](const char* s) noexcept {
                std::wstring w;
                for (; s && *s; ++s)
                {
                    const auto c = static_cast<unsigned char>(*s);
                    w.push_back((c >= 0x20 && c < 0x7f) ? static_cast<wchar_t>(c) : L'?');
                }
                return w;
            };
            std::wstring detail;
            wchar_t hr[16]{};
            try
            {
                throw;
            }
            catch (const winrt::hresult_error& e)
            {
                swprintf_s(hr, L"0x%08X", static_cast<unsigned>(e.code().value));
                detail = std::wstring{ L"winrt::hresult_error hr=" } + hr + L" msg=\"" + std::wstring{ e.message() } + L"\"";
            }
            catch (const wil::ResultException& e)
            {
                swprintf_s(hr, L"0x%08X", static_cast<unsigned>(e.GetErrorCode()));
                detail = std::wstring{ L"wil::ResultException hr=" } + hr + L" msg=\"" + widen(e.what()) + L"\"";
            }
            catch (const std::system_error& e)
            {
                detail = L"std::system_error code=" + std::to_wstring(e.code().value()) + L" msg=\"" + widen(e.what()) + L"\"";
            }
            catch (const std::exception& e)
            {
                detail = L"std::exception msg=\"" + widen(e.what()) + L"\"";
            }
            catch (...)
            {
                detail = L"unknown exception";
            }
            LogSwallowedExceptionCore(context, detail, stacks);
        }
        catch (...)
        {
        }
    }

    // wil failure sink for TerminalApp.dll (CATCH_LOG / LOG_* / THROW_* / fail-fasts in this
    // module). Throttled per call site; reentrancy-guarded (a failure raised while logging one
    // must not recurse).
    inline void __stdcall AgentWilFailureLogger(const wil::FailureInfo& f) noexcept
    {
        try
        {
            static thread_local bool t_inLogger = false;
            if (t_inLogger)
            {
                return;
            }
            struct Reset
            {
                bool* p;
                ~Reset() { *p = false; }
            } reset{ &t_inLogger };
            t_inLogger = true;

            const auto widen = [](const char* s) noexcept {
                std::wstring w;
                for (; s && *s; ++s)
                {
                    const auto c = static_cast<unsigned char>(*s);
                    w.push_back((c >= 0x20 && c < 0x7f) ? static_cast<wchar_t>(c) : L'?');
                }
                return w;
            };
            std::wstring file = widen(f.pszFile); // present in release (pszCode/pszFunction are debug-only)
            if (const auto slash = file.find_last_of(L"\\/"); slash != std::wstring::npos)
            {
                file.erase(0, slash + 1);
            }
            const wchar_t* kind = L"failure";
            switch (f.type)
            {
            case wil::FailureType::Exception:
                kind = L"exception";
                break;
            case wil::FailureType::Return:
                kind = L"return";
                break;
            case wil::FailureType::Log:
                kind = L"log"; // CATCH_LOG / LOG_* land here
                break;
            case wil::FailureType::FailFast:
                kind = L"failfast";
                break;
            }

            unsigned suppressed = 0;
            if (!ExcLogThrottleAllow(L"wil:" + file + L":" + std::to_wstring(f.uLineNumber), suppressed))
            {
                return;
            }

            wchar_t hr[16]{};
            swprintf_s(hr, L"0x%08X", static_cast<unsigned>(f.hr));
            std::wstring block = std::wstring{ L"[wil] " } + kind + L" hr=" + hr + L" at " + file + L":" + std::to_wstring(f.uLineNumber);
            if (f.pszFunction && *f.pszFunction) // debug builds only
            {
                block += L" fn=" + widen(f.pszFunction);
            }
            if (f.pszMessage && *f.pszMessage)
            {
                block += L" msg=\"" + std::wstring{ f.pszMessage } + L"\"";
            }
            block += L" ret=" + FormatAddressModuleRva(f.returnAddress);
            if (suppressed != 0)
            {
                block += L" [suppressed " + std::to_wstring(suppressed) + L" earlier repeat(s)]";
            }
            block += L"\n";
            // The newest throw stack: for a CATCH_LOG this call runs mid-catch, so a fresh (small
            // age) entry IS this exception's raise; a stale age marks it unrelated (no exception
            // was in flight — e.g. a plain LOG_IF_FAILED).
            block += CaptureRecentThrowStacksText(1);
            AppendStateLog(L"hooks.log", block);
        }
        catch (...)
        {
        }
    }

    // Process-once (per module): the VEH throw-stack ring + the wil failure sink. Cheap when idle.
    inline void InstallAgentExceptionTrace() noexcept
    {
        try
        {
            static std::once_flag s_once;
            std::call_once(s_once, []() noexcept {
                InstallThrowStackCapture();
                wil::SetResultLoggingCallback(&AgentWilFailureLogger);
                AppendStateLog(L"hooks.log", L"[exc] exception forensics installed (VEH throw-stack ring + wil failure logger)\n");
            });
        }
        catch (...)
        {
        }
    }
}
