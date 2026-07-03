// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster: a robust Win32 clipboard TEXT writer, shared by every Agentmaster copy site (the
// per-tab overlay's copy menu + the Triage Board / Explorer-tree Copy submenu — both routed through
// the two anonymous-namespace CopyTextToClipboard helpers in AgentTabOverlay.Internal.h /
// AgentManagerContent.Internal.h).
//
// WHY THIS EXISTS ("Copy ... does not work well in a focused tab"): those helpers used the WinRT
// Windows.ApplicationModel.DataTransfer.Clipboard (SetContent + Flush). That path opens the OLE
// clipboard exactly ONCE with NO retry; when another active clipboard user momentarily holds it —
// most notably the ConPTY TermControl of the FOCUSED tab (copy-on-select and its own OLE clipboard
// rendering) — SetContent/Flush throws, the caller swallowed it via CATCH_LOG(), and the copy
// silently did NOTHING (no chime, empty clipboard). It reproduces exactly when the tab you copy from
// is the focused, actively-used terminal — hence the report.
//
// This is the SAME recipe Windows Terminal's own terminal-copy path uses to dodge that class of
// failure (TerminalPage.cpp, namespace `clipboard`): the raw Win32 clipboard with an OpenClipboard
// RETRY loop (10ms -> ... -> ~10s), guarded by an SRW lock. Its comment is the whole story:
//   "OpenClipboard/CloseClipboard are not thread-safe whatsoever ... WinUI also uses OpenClipboard
//    (through WinRT which uses OLE), and so even with this mutex we can still crash randomly ...
//    Makes you wonder how many Windows apps are subtly broken, huh."
// So instead of losing the race, we WAIT the busy clipboard out. Because this is an `inline` function,
// its function-local `static` SRWLOCK is ONE instance across every TU that includes this header, so
// all Agentmaster copies serialize through a single lock (they already all run on the UI thread, but
// this keeps them safe if that ever changes).
//
// Returns true iff the text actually reached the clipboard, so callers gate their "copied" chime on a
// real success. Text-only (CF_UNICODETEXT) is all any Agentmaster copy needs — the terminal's own path
// additionally writes HTML/RTF. Real data (not delayed rendering) is placed on the clipboard, so it
// persists after the app closes WITHOUT needing WinRT's Flush(). MUST be included after pch.h (relies
// on the Win32 + wil facilities it brings in); call on the UI thread.

#pragma once

#include <string_view>
#include <wil/resource.h> // wil::unique_hglobal + wil::scope_exit (also reachable via pch; explicit here to be self-contained)

namespace winrt::TerminalApp::implementation
{
    inline bool RobustCopyTextToClipboard(std::wstring_view text)
    {
        // ONE shared lock across all Agentmaster copy sites (inline => a single static instance).
        static SRWLOCK s_clipboardLock = SRWLOCK_INIT;
        AcquireSRWLockExclusive(&s_clipboardLock);
        const auto releaseLock = wil::scope_exit([&]() noexcept { ReleaseSRWLockExclusive(&s_clipboardLock); });

        // OpenClipboard fails while another window (e.g. the focused terminal control) holds the OS
        // clipboard lock — retry with exponential backoff instead of giving up on the first failure
        // the way WinRT SetContent does. Same backoff schedule as WT's own clipboard::open (~10s total).
        bool opened = false;
        for (DWORD sleep = 10;; sleep *= 2)
        {
            if (OpenClipboard(nullptr))
            {
                opened = true;
                break;
            }
            if (sleep > 10000)
            {
                break; // the clipboard is genuinely wedged — give up (best-effort, like the terminal path)
            }
            Sleep(sleep);
        }
        if (!opened)
        {
            LOG_LAST_ERROR();
            return false;
        }
        // Hold the SRW lock ACROSS CloseClipboard (declared after the open, so it destructs first): the
        // comment's "on CloseClipboard the handle may get freed" race then can't overlap our next open.
        const auto closeClip = wil::scope_exit([]() noexcept { CloseClipboard(); });

        if (!EmptyClipboard())
        {
            LOG_LAST_ERROR();
            return false;
        }

        // CF_UNICODETEXT wants a null-terminated buffer -> +1 wchar for the terminator (a string_view is
        // NOT guaranteed null-terminated, so we write the terminator ourselves).
        const size_t chars = text.size();
        const size_t bytes = (chars + 1) * sizeof(wchar_t);
        wil::unique_hglobal handle{ GlobalAlloc(GMEM_MOVEABLE, bytes) };
        if (!handle)
        {
            LOG_LAST_ERROR();
            return false;
        }
        {
            const auto locked = static_cast<wchar_t*>(GlobalLock(handle.get()));
            if (!locked)
            {
                LOG_LAST_ERROR();
                return false;
            }
            if (chars)
            {
                memcpy(locked, text.data(), chars * sizeof(wchar_t));
            }
            locked[chars] = L'\0';
            GlobalUnlock(handle.get());
        }
        if (!SetClipboardData(CF_UNICODETEXT, handle.get()))
        {
            LOG_LAST_ERROR();
            return false;
        }
        handle.release(); // the clipboard now owns the global block (survives app close)
        return true;
    }
}
