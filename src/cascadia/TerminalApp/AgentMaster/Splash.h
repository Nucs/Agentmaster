// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — a lightweight launch SPLASH (header-only, pure Win32; no WinRT, no engine deps).
//
// WHY: launch spends ~7s in session/window restore (see StartupTiming.h), and that restore runs
// SYNCHRONOUSLY on the XAML UI thread — so a loading panel in the same visual tree would be FROZEN
// for exactly the window you want it to animate. The fix is a tiny window on its OWN thread with its
// OWN message pump: it keeps painting an indeterminate progress animation while the UI thread is
// blocked, then dismisses itself the instant the first real window finishes laying out.
//
// SHAPE (mirrors ProfileBootstrap.h / Updater.h / StartupTiming.h — header-only so it works on BOTH
// sides of the exe/dll split):
//   * Splash::Show()        — EXE (WindowEmperor): spins up the splash thread. Returns immediately.
//   * Splash::SetStatus(s)  — EXE: updates the status sub-line ("Restoring your sessions…").
//   * Splash::SignalReady() — DLL (TerminalPage, end of _OnFirstLayout): dismiss it.
//
// CROSS-BOUNDARY DISMISS: the exe and dll get SEPARATE copies of these header-inline statics, so a
// shared flag can't carry "ready" across the boundary. We use a PID-named kernel EVENT instead
// (Local\Agentmaster.Splash.Ready.<pid>): the splash thread (exe) waits on it; SignalReady (dll)
// CreateEventW's the same name (returns the existing handle) and SetEvents it. Same process ⇒ same
// PID ⇒ same event. A ~30s safety timeout + process-exit teardown guarantee it can never get stuck.
//
// ANTI-FLASH: the thread waits kPreShowDelayMs BEFORE creating the window — a fast/empty launch
// signals ready within that window and the splash is NEVER shown (no flicker); it only appears for a
// genuinely slow launch.
//
// WINDOW: a NORMAL top-level window — dark caption, the app icon, a TASKBAR button, and MINIMIZE +
// CLOSE buttons (WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX + WS_EX_APPWINDOW). It is NOT topmost: it is
// shown ACTIVATED (SW_SHOWNORMAL) so Windows brings it to the foreground ONCE when it appears, but the
// still-building main window can come forward and cover it as soon as it's ready (a topmost splash used
// to sit stubbornly above every window, including ones the user switched to). The user can minimize it,
// or CLOSE it early to ABORT the launch: closing the loading window (its X / Alt+F4 / the taskbar or
// system-menu Close) shuts the whole starting-up instance DOWN instead of letting it keep initializing
// invisibly in the background (WM_CLOSE => g_userClosed => TerminateProcess in ThreadMain; the main/UI
// thread is blocked in the synchronous restore this splash covers, so a posted quit wouldn't be seen
// until that finished — a decisive terminate is the only thing that actually stops the launch). The
// settle-watch still dismisses it automatically (WITHOUT terminating) once the window is ready.

#pragma once

#include <windows.h>
#include <appmodel.h> // GetCurrentPackageFamilyName — dev-vs-release card title (AgentmasterDev => "Agentmaster Dev")
#include <dwmapi.h> // DwmSetWindowAttribute — dark title bar to match the dark card
#include <shellapi.h> // ExtractIconExW — the app icon for the taskbar button + caption

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace Agentmaster::Splash
{
    namespace detail
    {
        // ---- tuning ---------------------------------------------------------------------------
        constexpr int kPreShowDelayMs = 350; // don't show unless launch is still going after this
        constexpr int kMaxLifetimeMs = 60000; // hard cap so a dll hang can't strand the splash (must exceed
                                              // TerminalPage's ~45s settle-watch timeout, which is the
                                              // intended dismiss path; this is only the last-resort backstop)
        constexpr int kBaseCardW = 460;
        constexpr int kBaseCardH = 150;
        constexpr UINT_PTR kAnimTimerId = 1;
        constexpr UINT kAnimIntervalMs = 16; // ~60fps
        constexpr int kAnimStepPx = 5; // progress-segment speed (px per tick)
        constexpr UINT WM_AM_SPLASH_REPAINT = WM_APP + 11;

        // ---- splash-thread + cross-thread state (EXE side) ------------------------------------
        inline std::atomic<bool> g_started{ false };
        inline std::atomic<HWND> g_hwnd{ nullptr };
        inline std::atomic<int> g_anim{ 0 }; // progress-segment position counter
        inline std::atomic<bool> g_dismissing{ false };
        inline std::atomic<bool> g_userClosed{ false }; // the user closed the splash early (X/Alt+F4) => abort the whole launch
        inline std::mutex g_statusMtx;
        inline std::wstring g_status{ L"Launching Agentmaster..." }; // ASCII dots — keep this header codepage-clean
        inline HFONT g_titleFont{ nullptr }; // splash-thread-only after Show()
        inline HFONT g_bodyFont{ nullptr };
        inline UINT g_dpi{ 96 };

        inline std::wstring ReadyEventName()
        {
            return L"Local\\Agentmaster.Splash.Ready." + std::to_wstring(::GetCurrentProcessId());
        }

        inline int Scaled(int base) { return ::MulDiv(base, static_cast<int>(g_dpi), 96); }

        inline std::wstring CurrentStatus()
        {
            std::lock_guard<std::mutex> lk{ g_statusMtx };
            return g_status;
        }

        // The identity name shown as the loading card's bold title: "Agentmaster Dev" for the DEV
        // package (family AgentmasterDev_*, the local Debug loose layout), plain "Agentmaster" for the
        // RELEASE package (Agentmaster_*) and for unpackaged/portable runs. This is the SAME
        // dev-vs-release signal the rest of the app keys on (Profiles::IsDevPackage — the package family
        // name prefix), re-derived LOCALLY here so this splash header stays self-contained (pure Win32,
        // no engine deps) across the exe/dll split, exactly as it re-derives the ready-event name from
        // the PID. Package identity is fixed for the process lifetime, so compute it ONCE in a magic
        // static. NB "Agentmaster" is a PREFIX of "AgentmasterDev", so test Dev FIRST — the same ordering
        // rule used everywhere this pair is compared (GetWtExePath, windowClassName, _AgentmasterReopenTarget).
        inline const wchar_t* IdentityTitle()
        {
            static const wchar_t* const kTitle = []() -> const wchar_t* {
                UINT32 len = 0;
                if (::GetCurrentPackageFamilyName(&len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 1)
                {
                    std::wstring pfn(len, L'\0');
                    if (::GetCurrentPackageFamilyName(&len, pfn.data()) == ERROR_SUCCESS)
                    {
                        pfn.resize(len > 0 ? len - 1 : 0); // drop the trailing NUL
                        if (pfn.rfind(L"AgentmasterDev", 0) == 0)
                        {
                            return L"Agentmaster Dev";
                        }
                    }
                }
                return L"Agentmaster";
            }();
            return kTitle;
        }

        inline void PaintCard(HWND hwnd, HDC hdc)
        {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const int W = rc.right - rc.left;
            const int H = rc.bottom - rc.top;
            if (W <= 0 || H <= 0)
            {
                return;
            }

            // Double-buffer (the animation timer would otherwise flicker).
            HDC mem = ::CreateCompatibleDC(hdc);
            HBITMAP bmp = ::CreateCompatibleBitmap(hdc, W, H);
            HBITMAP oldBmp = static_cast<HBITMAP>(::SelectObject(mem, bmp));

            // Dark card fill (#2e2e2e — the always-dark Agentmaster surface) + a 1px hairline border.
            HBRUSH bg = ::CreateSolidBrush(RGB(0x2E, 0x2E, 0x2E));
            ::FillRect(mem, &rc, bg);
            ::DeleteObject(bg);
            HBRUSH border = ::CreateSolidBrush(RGB(0x46, 0x46, 0x46));
            ::FrameRect(mem, &rc, border);
            ::DeleteObject(border);

            const int pad = Scaled(20);
            ::SetBkMode(mem, TRANSPARENT);

            // Title — "Agentmaster Dev" on the dev package, plain "Agentmaster" on release/unpackaged.
            HFONT oldFont = static_cast<HFONT>(::SelectObject(mem, g_titleFont));
            ::SetTextColor(mem, RGB(0xEC, 0xEC, 0xEC));
            RECT tr{ rc.left + pad, rc.top + pad, rc.right - pad, rc.top + pad + Scaled(30) };
            ::DrawTextW(mem, IdentityTitle(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

            // Status sub-line (live).
            ::SelectObject(mem, g_bodyFont);
            ::SetTextColor(mem, RGB(0xA8, 0xA8, 0xA8));
            const std::wstring status = CurrentStatus();
            RECT sr{ rc.left + pad, tr.bottom + Scaled(6), rc.right - pad, tr.bottom + Scaled(6) + Scaled(22) };
            ::DrawTextW(mem, status.c_str(), -1, &sr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_WORD_ELLIPSIS | DT_NOPREFIX);

            // Indeterminate progress bar: a moving accent segment ping-ponging across a dim track.
            RECT track{ rc.left + pad, rc.bottom - pad - Scaled(6), rc.right - pad, rc.bottom - pad };
            HBRUSH trackBr = ::CreateSolidBrush(RGB(0x3A, 0x3A, 0x3A));
            ::FillRect(mem, &track, trackBr);
            ::DeleteObject(trackBr);

            const int trackW = track.right - track.left;
            const int segW = trackW / 3;
            const int range = (trackW - segW) > 0 ? (trackW - segW) : 1;
            const int p = g_anim.load() % (2 * range);
            const int x = (p <= range) ? p : (2 * range - p); // ping-pong
            RECT seg{ track.left + x, track.top, track.left + x + segW, track.bottom };
            HBRUSH segBr = ::CreateSolidBrush(RGB(0x4C, 0x8B, 0xF5)); // accent blue
            ::FillRect(mem, &seg, segBr);
            ::DeleteObject(segBr);

            ::SelectObject(mem, oldFont);
            ::BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);

            ::SelectObject(mem, oldBmp);
            ::DeleteObject(bmp);
            ::DeleteDC(mem);
        }

        inline LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_PAINT:
            {
                PAINTSTRUCT ps{};
                HDC hdc = ::BeginPaint(hwnd, &ps);
                PaintCard(hwnd, hdc);
                ::EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1; // fully painted in WM_PAINT (double-buffered) — skip the flicker erase
            case WM_AM_SPLASH_REPAINT:
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_TIMER:
                if (wp == kAnimTimerId)
                {
                    g_anim.fetch_add(kAnimStepPx);
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_CLOSE:
                // The user dismissed the loading window early — the X button, Alt+F4, or the taskbar /
                // system-menu Close — BEFORE launch finished. Treat that as "abort the launch": record
                // the user-initiated close so ThreadMain shuts the whole instance down (below) instead of
                // letting it keep starting up invisibly in the background. The automatic dismiss path
                // (ready/expired) calls DestroyWindow DIRECTLY with g_dismissing set, so it never routes
                // through WM_CLOSE — hence a WM_CLOSE outside an in-progress dismiss is always the user.
                if (!g_dismissing.load())
                {
                    g_userClosed.store(true);
                }
                ::DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
            default:
                return ::DefWindowProcW(hwnd, msg, wp, lp);
            }
        }

        inline const wchar_t* EnsureClass()
        {
            static const wchar_t* const kClass = L"AgentmasterSplashWindow";
            static std::once_flag once;
            std::call_once(once, []() {
                WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = ::GetModuleHandleW(nullptr);
                wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
                wc.hbrBackground = nullptr; // we paint everything (double-buffered in WM_PAINT)
                wc.lpszClassName = kClass;
                // The app's own icon, for the taskbar button + the caption. Extract it from this exe
                // (Agentmaster.exe / the alias) so the loading window matches the installed app;
                // a null result just falls back to the default icon.
                wchar_t exePath[MAX_PATH]{};
                if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH))
                {
                    // NB: don't name a local `small` — <rpcndr.h> (pulled into WinRT TUs like
                    // TerminalPage.cpp via the PCH) does `#define small char`, so `small` would
                    // expand to `char` and break the whole header in those TUs (macro collision).
                    HICON bigIcon = nullptr, smallIcon = nullptr;
                    ::ExtractIconExW(exePath, 0, &bigIcon, &smallIcon, 1);
                    wc.hIcon = bigIcon; // may be null => default
                    wc.hIconSm = smallIcon;
                }
                ::RegisterClassExW(&wc);
            });
            return kClass;
        }

        inline void ThreadMain()
        {
            HANDLE ready = ::CreateEventW(nullptr, TRUE /*manual-reset*/, FALSE, ReadyEventName().c_str());

            // Anti-flash: if launch finishes within the pre-show window, never show the splash at all.
            if (ready && ::WaitForSingleObject(ready, kPreShowDelayMs) == WAIT_OBJECT_0)
            {
                ::CloseHandle(ready);
                return;
            }

            g_dpi = ::GetDpiForSystem() ? ::GetDpiForSystem() : 96;
            g_titleFont = ::CreateFontW(-::MulDiv(15, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_bodyFont = ::CreateFontW(-::MulDiv(10, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            // A NORMAL top-level window (not a tool-window overlay): a real caption with the app title +
            // a MINIMIZE and CLOSE button (WS_SYSMENU | WS_MINIMIZEBOX), and a TASKBAR button
            // (WS_EX_APPWINDOW, and crucially NO WS_EX_TOOLWINDOW). NOT topmost — it is brought to the
            // foreground once at show time (SW_SHOWNORMAL below) instead of being pinned above every
            // window, so the main window (and anything else the user switches to) can cover it; no
            // WS_MAXIMIZEBOX / WS_THICKFRAME (a fixed-size loading window). Sized so the CLIENT area equals
            // the card — AdjustWindowRectExForDpi grows the rect by the DPI-scaled caption + borders so the
            // card content isn't squeezed under the title bar.
            const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            const DWORD exStyle = WS_EX_APPWINDOW;
            RECT wr{ 0, 0, Scaled(kBaseCardW), Scaled(kBaseCardH) };
            ::AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, g_dpi);
            const int W = wr.right - wr.left;
            const int H = wr.bottom - wr.top;
            RECT wa{};
            ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            const int x = wa.left + ((wa.right - wa.left) - W) / 2;
            const int y = wa.top + ((wa.bottom - wa.top) - H) / 2;

            HWND hwnd = ::CreateWindowExW(
                exStyle,
                EnsureClass(),
                L"", // no caption title text — the card body already shows the bolded app name
                     // (IdentityTitle: "Agentmaster" / "Agentmaster Dev"); the title bar keeps just the
                     // app icon + the minimize/close buttons
                style,
                x, y, W, H,
                nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            if (!hwnd)
            {
                if (ready)
                {
                    ::CloseHandle(ready);
                }
                if (g_titleFont)
                {
                    ::DeleteObject(g_titleFont);
                    g_titleFont = nullptr;
                }
                if (g_bodyFont)
                {
                    ::DeleteObject(g_bodyFont);
                    g_bodyFont = nullptr;
                }
                return;
            }

            // Dark title bar to match the dark card (best-effort; a harmless no-op / error on Windows
            // builds that predate the attribute — the caption just stays light there).
            {
                const BOOL dark = TRUE;
                ::DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
            }
            g_hwnd.store(hwnd);
            // Show ACTIVATED so Windows brings the loading window to the foreground ONCE when it appears —
            // we're a freshly-launched foreground process, so normal activation succeeds here and no
            // explicit SetForegroundWindow is needed. It is NOT topmost, so the (blank, still-building)
            // main window can come forward and cover it as soon as it's ready; the user can also click its
            // taskbar button or minimize/close it as usual.
            ::ShowWindow(hwnd, SW_SHOWNORMAL);
            ::UpdateWindow(hwnd); // paint the card synchronously now (no longer layered) so there's no blank flash on show
            ::SetTimer(hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);

            const ULONGLONG start = ::GetTickCount64();
            for (;;)
            {
                const DWORD r = ::MsgWaitForMultipleObjects(ready ? 1 : 0, ready ? &ready : nullptr, FALSE, 100, QS_ALLINPUT);

                if (!g_dismissing.load())
                {
                    const bool readySignaled = (ready && r == WAIT_OBJECT_0);
                    const bool expired = (::GetTickCount64() - start) > static_cast<ULONGLONG>(kMaxLifetimeMs);
                    if (readySignaled || expired)
                    {
                        g_dismissing.store(true);
                        ::DestroyWindow(hwnd); // -> WM_DESTROY -> PostQuitMessage -> WM_QUIT below
                    }
                }

                MSG m{};
                bool quit = false;
                while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                {
                    if (m.message == WM_QUIT)
                    {
                        quit = true;
                        break;
                    }
                    ::TranslateMessage(&m);
                    ::DispatchMessageW(&m);
                }
                if (quit)
                {
                    break;
                }
            }

            g_hwnd.store(nullptr);
            if (ready)
            {
                ::CloseHandle(ready);
            }
            if (g_titleFont)
            {
                ::DeleteObject(g_titleFont);
                g_titleFont = nullptr;
            }
            if (g_bodyFont)
            {
                ::DeleteObject(g_bodyFont);
                g_bodyFont = nullptr;
            }

            // Agentmaster: the user CLOSED the loading window before launch finished => abort. Shut the
            // whole instance down rather than let it keep starting up invisibly in the background (the
            // half-built main window would otherwise still appear seconds later). TerminateProcess — not
            // a posted quit — because the main/UI thread is busy with the SYNCHRONOUS session/window
            // restore this splash exists to cover, so a WM_QUIT posted to it wouldn't be processed until
            // that ~7s of work drained, defeating the point. This mirrors the app's other decisive exits
            // (the single-instance handoff + the updater relaunch both TerminateProcess from a background
            // thread). Safe here: the splash only ever runs in the genuine emperor process (a handoff /
            // update / profile-decline all exit long BEFORE Splash::Show()), and startup only READS
            // persisted state, so there is nothing half-written to lose by terminating now.
            if (g_userClosed.load())
            {
                ::TerminateProcess(::GetCurrentProcess(), 0);
            }
        }
    }

    // EXE: start the splash (idempotent). Spawns a detached thread and returns immediately.
    inline void Show()
    {
        bool expected = false;
        if (!detail::g_started.compare_exchange_strong(expected, true))
        {
            return;
        }
        try
        {
            std::thread{ detail::ThreadMain }.detach();
        }
        catch (...)
        {
        }
    }

    // EXE: update the status sub-line. Safe before the window exists (it just stores the text).
    inline void SetStatus(std::wstring_view text)
    {
        {
            std::lock_guard<std::mutex> lk{ detail::g_statusMtx };
            detail::g_status.assign(text);
        }
        if (HWND h = detail::g_hwnd.load())
        {
            ::PostMessageW(h, detail::WM_AM_SPLASH_REPAINT, 0, 0);
        }
    }

    // DLL (or EXE): dismiss the splash. Crosses the exe/dll boundary via the PID-named event, so it
    // works even though the dll has its own copy of these statics. A no-op if no splash is running.
    inline void SignalReady()
    {
        HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, detail::ReadyEventName().c_str());
        if (ev)
        {
            ::SetEvent(ev);
            ::CloseHandle(ev);
        }
    }
}
