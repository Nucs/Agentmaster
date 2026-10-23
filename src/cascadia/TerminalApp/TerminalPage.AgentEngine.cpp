// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation (7 partial files)
// The TerminalPage-side Agentmaster glue -- engine wiring + session lifecycle + observer +
// window-record + the Sessions browser -- connecting the Manager engine to WT's TerminalPage.
// Same class (TerminalPage, declared in TerminalPage.h), split across same-class partial TUs (the
// upstream TabManagement.cpp pattern) so TerminalPage.cpp stays close to upstream.
//
// Partial files in this group (★ marks THIS file):
// ★ TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Agent Manager page (+ its Home button), _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster — TerminalPage's engine wiring + Agent Manager page hosting (M9 / M6; see
// doc/agentmaster/DESIGN.md and IMPLEMENTATION.md): consume the ONE process-wide
// SharedEngine (registry / bridge / scheduler / scanner / observer), claim this
// window's WindowRecord, build the Agent Manager PAGE (shown by the tab strip's Home
// button — it is no longer a tab), wire the AgentManagerContent callbacks, and detach
// everything again in ~TerminalPage.
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // GuidToString (mint a fresh windowId)

#include "AgentManagerContent.h" // the Agent Manager page's content (C1 UI) — created, wired and shown here
#include "AgentTabOverlay.h" // ~TerminalPage destroys the com_ptr<AgentTabOverlay> maps — needs the complete type
#include "AgentTranscriptOverlay.h" // ~TerminalPage destroys the com_ptr<AgentTranscriptOverlay> — needs the complete type
#include "AgentLaunchBar.h" // ~TerminalPage shuts the side panel "+" launcher's bar down (AgentLaunchBar::Shutdown) — needs the complete type
#include "AgentCatchLog.h" // exception forensics: InstallAgentExceptionTrace (VEH throw-stack ring + wil failure logger, engine init below)
#include "AgentTipHelpers.h" // AgentSetTip — the Home button's tooltip, whose text follows its face (Home / jump back)
#include <winrt/Windows.UI.Xaml.Automation.h> // AutomationProperties::SetName — the Home button's accessible name follows its face too
#include "AgentToastActivator.h" // System notifications: the toast COM activator (registered once, process-wide, from the engine init below)
#include "TabHeaderControl.h" // Agentmaster: SetTabRenameCommitMode (push the GLOBAL rename-commit mode to tab headers)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog / AgentmasterStateDir / MaterializeSharedHookFiles
#include "AgentMaster/Engine.h" // SharedEngine / ClaimWindowRecord / Register-UnregisterLiveWindow
#include "AgentMaster/SessionDbRefresher.h" // SESSIONS_DB.md §7.6 — the cog Save re-applies the session database settings
#include "AgentMaster/Messaging.h" // MESSAGING.md — the Telegram bot: the cog Save re-applies its settings; its Search factory + version word are set here
#include "MessagingSearchBridge.h" // MESSAGING.md §5.4 — the Search topic's agent (a SessionSearchAgent of its own)
#include "AgentMaster/LaunchPresetsOs.h" // MESSAGING.md §21.25 — LaunchMenuEntries / ShouldOfferAddPresets: the bot wizard's model step
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings
#include "AgentMaster/ProcessObserver.h" // UnpublishWindow (teardown; Rule #10)
#include "AgentMaster/Scheduler.h" // SetGlobalPause / GlobalPaused + the global-pause observer (the Manager's Pause button, DELIVERY.md §17)
#include "AgentMaster/SessionRegistry.h" // adoption-handler + registry-observer tokens
#include "AgentMaster/SessionScanner.h" // liveness-probe token
#include "AgentMaster/TabDragMath.h" // the pointer-owned tab reorder gesture's pure decision math (bands / slots / release / auto-scroll)

#include <WtExeUtils.h> // IsPackaged — the ui-stall restart tier relaunches by AUMID when packaged
#include <appmodel.h> // GetCurrentPackageFamilyName (the packaged relaunch target)

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

// Agentmaster: TabHeaderControl mirrors the rename-commit mode as raw ints (it doesn't include the
// engine model). SetTabRenameCommitMode below casts the enum straight to int, so the enum's
// underlying values MUST be these — lock it here, where both the enum and that contract are visible.
static_assert(static_cast<int32_t>(::Agentmaster::TabRenameCommitMode::ClickAwayOnly) == 0 &&
                  static_cast<int32_t>(::Agentmaster::TabRenameCommitMode::ClickAwayOrShiftEnter) == 1 &&
                  static_cast<int32_t>(::Agentmaster::TabRenameCommitMode::ClickAwayOrEnter) == 2,
              "TabRenameCommitMode values must match the raw-int constants in TabHeaderControl.cpp");

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

// Agentmaster: depth-first walk for the TabView's internal horizontal ScrollViewer — the template
// part named "ScrollViewer" inside its TabViewListView (see Microsoft.UI.Xaml 2.8 Generic.xaml: the
// TabScrollViewerStyle hosts the `<`/`>` ScrollDecrease/IncreaseButtons around it). That ScrollViewer
// is what scrolls the tab strip, so it is the instrument the strip reveal (_RevealTabInStrip) and the
// drag gesture's edge auto-scroll move. Each TabViewItem's content is just an empty Border (Tab.cpp), so
// the strip subtree contains exactly one ScrollViewer and the first found in document order is correct.
static winrt::WUX::Controls::ScrollViewer _FindTabStripScrollViewer(const winrt::WUX::DependencyObject& root)
{
    if (!root)
    {
        return nullptr;
    }
    const auto count = winrt::WUX::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int32_t i = 0; i < count; ++i)
    {
        const auto child = winrt::WUX::Media::VisualTreeHelper::GetChild(root, i);
        if (const auto sv = child.try_as<winrt::WUX::Controls::ScrollViewer>())
        {
            if (const auto fe = child.try_as<winrt::WUX::FrameworkElement>(); fe && fe.Name() == L"ScrollViewer")
            {
                return sv;
            }
        }
        if (auto found = _FindTabStripScrollViewer(child))
        {
            return found;
        }
    }
    return nullptr;
}

// Agentmaster (tab sizing): depth-first walk for the TabView's inner TabViewListView — the template part
// named "TabListView" (Microsoft.UI.Xaml 2.8 Generic.xaml: the list inside the TabView template's
// TabContainerGrid). MUX's SizeToContent / Compact width pass caps that list's MaxWidth at the space then
// available, and its Equal pass never resets it, so after a mode change the stale cap would keep the strip
// from growing with the window (TerminalPage::_ClearTabStripListMaxWidth drops it). The walk returns at
// the first match, which document order reaches before the selected tab's (far larger) content subtree,
// so it stays cheap.
// Parameters: root - where to start (the TabView); null is tolerated.
// Returns: the list, or nullptr before the TabView's template is applied.
static winrt::WUX::FrameworkElement _FindTabStripListView(const winrt::WUX::DependencyObject& root)
{
    if (!root)
    {
        return nullptr;
    }
    const auto count = winrt::WUX::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int32_t i = 0; i < count; ++i)
    {
        const auto child = winrt::WUX::Media::VisualTreeHelper::GetChild(root, i);
        if (const auto fe = child.try_as<winrt::WUX::FrameworkElement>(); fe && fe.Name() == L"TabListView")
        {
            return fe;
        }
        if (auto found = _FindTabStripListView(child))
        {
            return found;
        }
    }
    return nullptr;
}

// Agentmaster (tab sizing): give `element` Agentmaster's LOCAL value for the double property `dp`, or —
// with `clear` — drop that local value so the XAML style's own value governs again (how a setting left at
// its default stays byte-identical to the strip before the setting existed). Writes only on a real change:
// every write re-measures the strip, and the tab-sizing apply runs this for every tab on each settings
// change. A style setter is not a LOCAL value, so a never-touched tab reads as "unset" here.
// Parameters:
//   element - the tab (TabViewItem) to size.
//   dp      - a double-typed FrameworkElement property (MinHeight / MinWidth / MaxWidth).
//   value   - the px value to hold (ignored with `clear`).
//   clear   - true: remove the local value instead of setting one.
// Throws: whatever the XAML property system throws (a winrt::hresult_error); callers catch + log.
static void _SetOrClearLocalDouble(const winrt::WUX::FrameworkElement& element, const winrt::WUX::DependencyProperty& dp, const double value, const bool clear)
{
    const auto local = element.ReadLocalValue(dp);
    const bool hasLocal = local != winrt::WUX::DependencyProperty::UnsetValue();
    if (clear)
    {
        if (hasLocal)
        {
            element.ClearValue(dp);
        }
        return;
    }
    // -1 can never equal a real (positive) size, so an unboxing surprise just means "write it".
    if (hasLocal && winrt::unbox_value_or<double>(local, -1.0) == value)
    {
        return;
    }
    element.SetValue(dp, winrt::box_value(value));
}

// Agentmaster: flip a tab-strip nav button's visibility, but only when it actually changes — the
// updater runs on every tab add/remove/switch (hundreds of times during a big window restore), so a
// no-op write would needlessly invalidate the strip's layout each time.
static void _SetTabStripButtonVisible(const winrt::WUX::Controls::Button& btn, bool visible)
{
    if (!btn)
    {
        return;
    }
    const auto desired = visible ? winrt::WUX::Visibility::Visible : winrt::WUX::Visibility::Collapsed;
    if (btn.Visibility() != desired)
    {
        btn.Visibility(desired);
    }
}

// Agentmaster (the Agent Manager PAGE): paint the Home button as the strip's ACTIVE entry while the
// Manager page is on screen — the Home button is where the Manager lives in the strip now that it is no
// longer a tab, and with the tabs deselected under the page nothing else would say "you are here". The
// fill is the system accent at ~40% alpha, the same "selected" tint the Linked-Lenses pill paints on a
// tab (_SetTabSelectionPill), so the two read as one visual language. Idle restores the XAML's flat
// Transparent. Only Background is written: the glyph keeps inheriting the button's {ThemeResource}
// foreground, so a theme switch still re-colors it (overwriting Foreground would freeze the old theme's
// brush). Change-gated through the button's Tag (nothing else uses it), because this runs from every tab
// add/remove/switch. Parameters: btn — the Home button (null is a no-op); active — the page is on screen.
// Never throws: a failed write is logged (Rule #18) and the button keeps its previous look.
static void _PaintManagerHomeButton(const winrt::WUX::Controls::Button& btn, const bool active)
{
    if (!btn)
    {
        return;
    }
    try
    {
        // An unset Tag means "never painted" (first call) — always paint then, whatever `active` is.
        if (const auto tag = btn.Tag(); tag && winrt::unbox_value_or<bool>(tag, !active) == active)
        {
            return;
        }
        auto fill = winrt::Windows::UI::ColorHelper::FromArgb(0x00, 0x00, 0x00, 0x00); // idle: the XAML's flat Transparent
        if (active)
        {
            // Resolved like the pill / the active-pane border accent: HasKey-guarded, WT-blue fallback.
            fill = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x00, 0x78, 0xD4);
            const auto res = winrt::WUX::Application::Current().Resources();
            if (const auto accentKey = winrt::box_value(L"SystemAccentColor"); res.HasKey(accentKey))
            {
                fill = winrt::unbox_value_or<winrt::Windows::UI::Color>(res.Lookup(accentKey), fill);
            }
            fill.A = 0x66; // ~40% — reads "selected" without drowning the glyph
        }
        btn.Background(winrt::WUX::Media::SolidColorBrush{ fill });
        btn.Tag(winrt::box_value(active));
    }
    catch (...)
    {
        ::Agentmaster::AgentLogCaughtException(L"_PaintManagerHomeButton");
    }
}

// Agentmaster: how long the pointer must rest on the Home button before its tooltip opens — the delay its
// XAML tooltip had before the tooltip moved to AgentSetTip (2026-10-12, when the text started following
// the button's face). XAML's ToolTipService opens a mouse-hovered tip after TWICE the system mouse-hover
// time (WinUI ToolTipService::GetInitialShowDelay: Mouse = 2x SPI_GETMOUSEHOVERTIME, 400 ms by default),
// while AgentSetTip's default is a third of it (~133 ms). The button sits at the strip's left edge, where
// the pointer passes on its way to the tabs, so the faster default would pop the tip up far more often;
// keeping the old delay keeps the old feel.
// Returns: 2 x the system mouse-hover time (800 ms when the setting is unreadable or zero).
// Never throws: one SystemParametersInfoW read, with a fallback.
static std::chrono::milliseconds _ManagerHomeButtonTipDelay() noexcept
{
    UINT hoverMs = 0;
    if (!::SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hoverMs, 0) || hoverMs == 0)
    {
        hoverMs = 400; // ToolTipService's own fallback (DEFAULT_SPI_GETMOUSEHOVERTIME)
    }
    return std::chrono::milliseconds{ 2LL * hoverMs };
}

// ---- Agentmaster: UI-stall RESCUE window + self-RESTART (Engine.h's escalation block) -------------
// The 2026-08-22 release freeze, dump-proven: XAML's CoreMessaging dispatch delivery latched dead
// under machine-wide commit exhaustion — the UI thread idled in the Emperor's GetMessageW and
// answered SENT messages, while CoreDispatcher.RunAsync work was accepted-but-never-delivered for
// 18+ minutes and every window's XAML input (same delivery) died with it. A SENT message is the one
// channel that still executes code ON the UI thread in that state, so each window owns a message-only
// rescue HWND: the engine watchdog SENDS into it to (a) PROVE the pump is alive and (b) attempt a
// synchronous CoreDispatcher.ProcessEvents drain of the backed-up queue. If drains can't clear the
// stall, the watchdog's restart tier calls _AgentUiStallRestart below.
namespace
{
    extern "C" IMAGE_DOS_HEADER __ImageBase; // this DLL's module handle (for RegisterClassExW)

    constexpr UINT kAgentUiRescueMsg = WM_APP + 0x5A; // probe + drain (sent by the watchdog thread)
    constexpr UINT kAgentUiRescueDestroyMsg = WM_APP + 0x5B; // teardown: DestroyWindow ON the owning (UI) thread

    struct AgentUiRescueCtx
    {
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        std::wstring windowId;
    };

    LRESULT CALLBACK _AgentUiRescueWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == kAgentUiRescueMsg)
        {
            // Executing here — via a SENT message — is itself the proof the Win32 pump is alive
            // (posted/dispatcher work may still be jammed; the drain below is the attempt to fix
            // that). Return contract with the rescuer: 1 = pump alive, 2 = pump alive + drain ran.
            auto* ctx = reinterpret_cast<AgentUiRescueCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            LRESULT result = 1;
            if (ctx && ctx->dispatcher)
            {
                try
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[ui-stall] rescue window " + ctx->windowId + L": ON the UI thread — draining the dispatcher queue (ProcessAllIfPresent)\n");
                    // Synchronously deliver everything the jammed dispatcher has been sitting on.
                    // This PULLS from the queue, so it works even when the wake/delivery side is
                    // latched: the queued beats land (clearing the stall), queued UI work executes,
                    // and the queue's empty->non-empty transition on the NEXT enqueue is a fresh
                    // chance for the delivery machinery to re-arm. A nested pump here is the same
                    // shape XAML's own modal loops use.
                    ctx->dispatcher.ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
                    result = 2;
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentUiRescue ProcessEvents");
                }
            }
            return result;
        }
        if (msg == kAgentUiRescueDestroyMsg)
        {
            ::DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_NCDESTROY)
        {
            auto* ctx = reinterpret_cast<AgentUiRescueCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete ctx;
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND _CreateAgentUiRescueWindow(const std::wstring& windowId, const winrt::Windows::UI::Core::CoreDispatcher& dispatcher)
    {
        static const ATOM atom = []() {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = _AgentUiRescueWndProc;
            wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            wc.lpszClassName = L"AgentmasterUiRescue";
            return ::RegisterClassExW(&wc);
        }();
        if (!atom)
        {
            return nullptr;
        }
        const HWND hwnd = ::CreateWindowExW(0, L"AgentmasterUiRescue", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
        if (!hwnd)
        {
            return nullptr;
        }
        auto ctx = std::make_unique<AgentUiRescueCtx>();
        ctx->dispatcher = dispatcher;
        ctx->windowId = windowId;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx.release()));
        return hwnd;
    }

    // The watchdog's restart tier (runs on the WATCHDOG thread — the UI is unusable by definition
    // here). Toast (best-effort) -> spawn a waiter that relaunches us after we exit -> terminate.
    // A hard exit is the durability-designed path: open-windows.json holds the full window set and
    // sessions/records/drafts are autosaved, so the relaunch restores the workspace (same contract
    // as the updater's install-relaunch). If the relauncher can't even spawn, we deliberately STAY
    // UP (a frozen app the user can kill beats a dead app that never comes back).
    void _AgentUiStallRestart(const std::wstring& reason)
    {
        // The watchdog thread never touched COM — init MTA for the toast. S_FALSE/RPC_E_CHANGED_MODE
        // are both fine (already initialized); the toast try/catch below covers a hostile state.
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try
        {
            namespace notif = winrt::Windows::UI::Notifications;
            const auto xml = notif::ToastNotificationManager::GetTemplateContent(notif::ToastTemplateType::ToastText02);
            const auto texts = xml.GetElementsByTagName(L"text");
            texts.Item(0).InnerText(L"Agentmaster restarted itself");
            texts.Item(1).InnerText(L"The UI froze (its dispatcher wedged), so the app restarted to recover. Your sessions and windows are preserved.");
            xml.DocumentElement().SetAttribute(L"launch", L"--from-toast"); // the recognized no-op sentinel — a click just foregrounds/starts the app
            notif::ToastNotification toast{ xml };
            notif::ToastNotifier notifier{ nullptr };
            if (IsPackaged())
            {
                notifier = notif::ToastNotificationManager::CreateToastNotifier();
            }
            else
            {
                wil::unique_cotaskmem_string aumid;
                if (SUCCEEDED(::GetCurrentProcessExplicitAppUserModelID(&aumid)))
                {
                    notifier = notif::ToastNotificationManager::CreateToastNotifier(aumid.get());
                }
            }
            if (notifier)
            {
                notifier.Show(toast);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"UiStallRestart toast");
        }

        // What to relaunch: packaged -> the package AUMID (a raw exe launch of a packaged app fails,
        // WT #926/#4043); unpackaged -> this exe's path.
        std::wstring relaunch;
        if (IsPackaged())
        {
            wchar_t pfn[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1]{};
            UINT32 len = ARRAYSIZE(pfn);
            if (::GetCurrentPackageFamilyName(&len, pfn) == ERROR_SUCCESS)
            {
                relaunch = std::wstring(L"shell:AppsFolder\\") + pfn + L"!App";
            }
        }
        else
        {
            wchar_t exe[MAX_PATH * 2]{};
            if (::GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe)) > 0)
            {
                relaunch = exe;
            }
        }

        bool launched = false;
        if (!relaunch.empty())
        {
            // Windows PowerShell by full path (always present; pwsh may not be): wait for OUR pid to
            // die, then start the app. The waiter outlives us, so the relaunch can never race the
            // single-instance handoff into the very process being killed.
            wchar_t sysdir[MAX_PATH]{};
            ::GetSystemDirectoryW(sysdir, ARRAYSIZE(sysdir));
            std::wstring cmd = L"\"";
            cmd += sysdir;
            cmd += L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -WindowStyle Hidden -Command \"Wait-Process -Id ";
            cmd += std::to_wstring(::GetCurrentProcessId());
            cmd += L" -ErrorAction SilentlyContinue; Start-Process '";
            cmd += relaunch;
            cmd += L"'\"";
            STARTUPINFOW si{ sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                ::CloseHandle(pi.hThread);
                ::CloseHandle(pi.hProcess);
                launched = true;
            }
        }

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      launched ?
                                          L"[ui-stall] restart: relauncher spawned — terminating now (" + reason + L")\n" :
                                          L"[ui-stall] restart: relauncher FAILED to spawn — staying up, NOT terminating (" + reason + L")\n");
        if (launched)
        {
            ::Sleep(1000); // let the toast + log write settle
            ::TerminateProcess(::GetCurrentProcess(), 0x51A11u); // 'stall' marker exit code (like the updater's handoff, durability reopens the workspace)
        }
    }
}

namespace winrt::TerminalApp::implementation
{
    // Agentmaster (M9): the engine is a process singleton shared by every window. When this
    // window is torn down, drop its adoption handler from the shared registry so it doesn't
    // linger (the handler captures get_weak(), so a stray call is already a safe no-op — this
    // just keeps the registry's handler list bounded across many window open/close cycles). The
    // registry itself outlives every window (held by SharedEngine), so this call stays valid.
    TerminalPage::~TerminalPage()
    {
        // Agentmaster (quit-all window-record loss): persist this window's record BEFORE the
        // teardown-archive below clears _claudeTabs — capturing after it would degrade every Claude tab
        // to an anonymous Other ref (the hazard CloseWindow's flush note describes). This is the
        // catch-all for the NON-initiating windows on a quit-all (QuitRequested), which are torn down
        // straight through this destructor with no per-window CloseWindow/RequestQuit, so without this
        // their final geometry / tab order / focus / lens never reaches disk past the 750ms autosave.
        // Gated on the latch so it does NOT re-capture (and clobber) a good close/quit-seam flush after
        // _claudeTabs was already cleared; the geometry fallback in _CaptureWindowRecord keeps the
        // last-known position/size if the window is already too far torn down to read it live.
        // Agentmaster (terminate-net — teardown can run on a BACKGROUND thread): ~TerminalPage assumes
        // UI-thread execution (it stops DispatcherTimers + reads XAML in the flush/archive here — all
        // UI-affine), but a fire_and_forget observer/sweep coroutine can hold the last get_strong() ref
        // and, when the window closes mid-tick, its resume_foreground() can't reach the dying dispatcher
        // and completes INLINE on the thread-pool thread — so releasing that last ref runs THIS
        // destructor THERE. A UI-affine call then throws RPC_E_WRONG_THREAD, and a throw escaping a
        // destructor => std::terminate (0xC0000409 FATAL_APP_EXIT — hit live after a tab drag-drop tore a
        // window down while _ObserverProbe was mid-tick). So EVERY UI-affine step below is individually
        // guarded (CATCH_LOG), while the thread-safe engine detaches (Rule #10) stay bare so a guarded
        // throw can never skip them. On the normal UI-thread teardown nothing throws, so this is inert.
        if (!_windowRecordTeardownFlushed)
        {
            try
            {
                _FlushWindowRecord();
            }
            CATCH_LOG();
            _windowRecordTeardownFlushed = true;
        }

        // Agentmaster (lifecycle gap #1): archive any of this window's still-live sessions before we
        // detach. Normally CloseWindow already did this (deterministically, before raising
        // CloseWindowRequested); this is the catch-all for quit-all / any teardown path that bypassed
        // it. Idempotent — a no-op if CloseWindow already cleared _claudeTabs. Guarded (bg-thread net).
        try
        {
            _ArchiveWindowSessionsOnTeardown();
        }
        CATCH_LOG();

        if (_sessionRegistry && _adoptionToken)
        {
            _sessionRegistry->RemoveAdoptionHandler(_adoptionToken);
        }
        // Agentmaster (tab status dot): same Rule-#10 detach for the dot's registry observer.
        if (_sessionRegistry && _agentDotObserverToken)
        {
            _sessionRegistry->RemoveObserver(_agentDotObserverToken);
        }
        // Symmetric to the adoption handler: drop this window's liveness probe from the shared
        // scanner so a closed window's probe (it captures get_weak()) doesn't linger on the
        // process-wide scanner. The scanner outlives every window (held by SharedEngine).
        if (_scanner && _livenessToken)
        {
            _scanner->RemoveLivenessProbe(_livenessToken);
        }
        // Agentmaster (alt+up/down prompt nav): stop the 30 s focused-refresh timer. DispatcherTimer is
        // UI-thread-affine, so Stop() throws RPC_E_WRONG_THREAD on a background-thread teardown (see the
        // terminate-net note above) — guard it. Unstopped is harmless: the timer dies with the page and
        // its Tick captures get_weak().
        if (_promptNavRefreshTimer)
        {
            try
            {
                _promptNavRefreshTimer.Stop();
            }
            CATCH_LOG();
        }
        // Agentmaster (eager-init "Activate All Tabs" pacing): stop a mid-flight activate-all drip — its
        // queue dies with the page; the weak Tick would no-op anyway. Same UI-affine guard as above.
        if (_activateAllTimer)
        {
            try
            {
                _activateAllTimer.Stop();
            }
            CATCH_LOG();
        }
        // Agentmaster (the RESTORE WAKE — PERSISTENCE.md §14): stop this window's wait (UI-affine — the same guard as the
        // timers above), then LEAVE the process-wide wake barrier, so a window that closes while it waits never holds the
        // other windows' busy tabs back (until the barrier's cap). _LeaveRestoreWake is thread-safe, touches no XAML and
        // never throws, so it stays bare like the engine detaches below; a no-op for a window that already left.
        if (_restoreWakeTimer)
        {
            try
            {
                _restoreWakeTimer.Stop();
            }
            CATCH_LOG();
        }
        _LeaveRestoreWake(L"the window closed");
        // Agentmaster (HOOKS.md "Interrupted turns"): stop the fork stop watch's timer (UI-affine — the same guard as the timers
        // above). Its watches die with the page: a fork tab that closes with its window keeps its persisted stop intent
        // (SessionInfo::stopForkedTurn), so the window that reopens it watches it again.
        if (_forkTurnStopTimer)
        {
            try
            {
                _forkTurnStopTimer.Stop();
            }
            CATCH_LOG();
        }
        // Agentmaster (side panel "+" launcher, SIDE_PANEL.md §2e): stop its launch bar's PRESETS hover timer and close
        // an open hover menu and drop-down. Shutdown is noexcept and guards each UI-affine call itself (a pool-thread
        // teardown throws RPC_E_WRONG_THREAD there — caught and logged inside), so it needs no guard here.
        if (_sidePanelLaunchBar)
        {
            _sidePanelLaunchBar->Shutdown();
        }
        // Agentmaster (the Sessions page's AI search, SESSIONS.md §9): remove this page's listener from
        // the process-wide search agent and leave its in-use tally — the last page out lets the agent
        // free its session index and park its idle process (Rule #10). Thread-safe and noexcept, no
        // XAML inside, so it needs no guard and is never skipped; a no-op for a page that never
        // turned the AI search on.
        _SessionsAiDetach();
        // Agentmaster (cross-window activate): drop this window's activate sink from the shared
        // engine — a stray fan-out after teardown is already a safe no-op (the sink captures
        // get_weak() + an agile dispatcher), this keeps the engine's sink list bounded (Rule #10).
        if (_windowActivateToken)
        {
            ::Agentmaster::UnregisterWindowActivateHandler(_windowActivateToken);
        }
        // Agentmaster (cross-window restart): drop this window's restart sink too (Rule #10).
        if (_commandActionToken)
        {
            ::Agentmaster::UnregisterCommandActionHandler(_commandActionToken);
            _commandActionToken = 0;
        }
        if (_windowRestartToken)
        {
            ::Agentmaster::UnregisterWindowRestartHandler(_windowRestartToken);
        }
        // Agentmaster (cross-window "Close ▸ Of Same Folder"): drop this window's close-session sink too (Rule #10).
        if (_windowCloseSessionToken)
        {
            ::Agentmaster::UnregisterWindowCloseSessionHandler(_windowCloseSessionToken);
        }
        // Agentmaster (cross-window "Deactivate ▸ Of Same Folder"): drop this window's park-session sink too
        // (Rule #10). Bare on purpose, like its neighbors: the detach is thread-safe engine work, and it must
        // never be skipped by a guarded throw (the destructor-cascade rule in Gotchas).
        if (_windowDeactivateSessionToken)
        {
            ::Agentmaster::UnregisterWindowDeactivateSessionHandler(_windowDeactivateSessionToken);
        }
        // Agentmaster (MESSAGING.md §8.2): drop this window's two remote sinks too (Rule #10). Bare like their
        // neighbors: thread-safe engine work that must never be skipped by a guarded throw.
        if (_windowSessionActionToken)
        {
            ::Agentmaster::UnregisterWindowSessionActionHandler(_windowSessionActionToken);
        }
        if (_windowLaunchToken)
        {
            ::Agentmaster::UnregisterWindowLaunchHandler(_windowLaunchToken);
        }
        // Agentmaster (SESSIONS.md §9.14): drop this window's agent-tool sink too (Rule #10) — a closed window must not
        // be counted in the AI search's census or asked to open a tab. Bare like its neighbors: thread-safe engine work
        // that must never be skipped by a guarded throw.
        if (_windowAgentToolsToken)
        {
            ::Agentmaster::UnregisterWindowAgentToolHandler(_windowAgentToolsToken);
        }
        // Agentmaster (cross-window tab DOCKING): drop this window's dock-target sink too (Rule #10) —
        // a torn-down window must never be offered as a drop target.
        if (_windowDockTargetToken)
        {
            ::Agentmaster::UnregisterWindowDockTarget(_windowDockTargetToken);
        }
        // Agentmaster (eager-init "Activate All Tabs"): drop this window's activate-all sink too (Rule #10).
        if (_windowActivateAllToken)
        {
            ::Agentmaster::UnregisterActivateAllDormantHandler(_windowActivateAllToken);
        }
        // Agentmaster (cross-window settings broadcast): drop this window's settings sink too (Rule #10).
        if (_settingsChangedToken)
        {
            ::Agentmaster::UnregisterSettingsChangedHandler(_settingsChangedToken);
        }
        // Agentmaster (taskbar unread badge; NOTIFICATIONS.md section 8): drop this window's count sink (Rule #10), THEN
        // withdraw its unread sessions from the census, so the windows that remain stop counting them on their taskbar
        // buttons — the close's own archive notifications may never reach this dying page's flash bookkeeping, so the
        // census can't rely on them. The order matters: the withdrawal fans the new count out to every REMAINING sink,
        // and ours must already be gone (its dispatcher dies with this page). Gated on the token, so a window whose
        // engine init never ran can't construct the shared engine from its destructor. Both calls are thread-safe and
        // noexcept (this destructor may run on a pool thread), so they stay BARE like the other engine detaches.
        if (_unreadCountToken)
        {
            ::Agentmaster::UnregisterUnreadCountHandler(_unreadCountToken);
            _unreadCountToken = 0;
            ::Agentmaster::PublishWindowUnreadSessions(_windowId, {});
        }
        // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): drop this window's listener from the shared
        // scheduler (Rule #10). The scheduler outlives every window, and its listener list would otherwise
        // grow by one dead entry per closed window (each entry holds only a weak page reference and the
        // agile dispatcher, so a late call is harmless — the list growth is the cost). The remove takes the
        // scheduler's own mutex and touches no XAML, so it is safe on the pool thread this destructor can
        // run on, and it stays BARE like the other engine detaches so a guarded throw can never skip it.
        if (_scheduler && _globalPauseObserverToken)
        {
            _scheduler->RemoveGlobalPauseObserver(_globalPauseObserverToken);
            _globalPauseObserverToken = 0;
        }
        // Agentmaster (ui-stall escalation): drop this window's rescuer from the engine watchdog
        // (Rule #10 — a thread-safe engine detach, stays BARE so it can never be skipped), then
        // destroy the message-only rescue window ON ITS OWNING (UI) THREAD via a sent message —
        // ~TerminalPage can run on a pool thread (the fire_and_forget teardown class), where a
        // direct DestroyWindow would fail. A timed-out send leaks one message-only window, which is
        // harmless (it dies with the process; its wndproc guards a null context).
        ::Agentmaster::UnregisterUiStallRescuer(_windowId);
        if (_uiRescueWnd)
        {
            DWORD_PTR ignored = 0;
            ::SendMessageTimeoutW(_uiRescueWnd, kAgentUiRescueDestroyMsg, 0, 0, SMTO_ABORTIFHUNG, 1000, &ignored);
            _uiRescueWnd = nullptr;
        }
        // Fleet Observer (OBSERVER.md §10/§12): drop THIS window's tab roster from the process-wide
        // observer so a closed window's tabs aren't surveyed/correlated after teardown (Rule #10).
        if (_observer && !_windowId.empty())
        {
            _observer->UnpublishWindow(_windowId);
        }
        // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5): this window is gone — drop it
        // from the process-wide live set, which rewrites open-windows.json (skip-empty: the last window
        // out leaves the final snapshot intact). Symmetric to the RegisterLiveWindow in engine init.
        if (!_windowId.empty())
        {
            ::Agentmaster::UnregisterLiveWindow(_windowId);
        }
    }

    // Agentmaster (the Agent Manager PAGE): build this window's one Manager content — the fleet view that
    // used to live in the pinned, leftmost, non-closable Manager TAB — wire it to the engine, and keep it
    // OFF screen until the tab strip's Home button shows it (_ShowManagerPage). It is owned by _managerPage
    // for the window's whole life and is NEVER inserted into _tabs, so no tab loop, drag, move, close,
    // split or rename can ever reach it (every pinned-tab guard the old tab needed is gone with it). Its
    // lens starts HIDDEN: every refresh until the first show only marks it dirty (the hidden-lens rebuild
    // tax fix), and the first show runs the single catch-up rebuild.
    // Called once from _OnFirstLayout, after _InitAgentmasterEngine (the registry the wiring hands over
    // must exist) and before _InitAgentSidePanel (the side panel delegates sort/menu/selection to this
    // content). Idempotent: a second call is a no-op. Never throws: a failed build is logged (Rule #18)
    // and leaves the window page-less — Home stays hidden, and a window emptied of tabs then closes the
    // upstream way instead of showing a page it doesn't have.
    void TerminalPage::_CreateAgentManagerPage()
    {
        if (_managerPage)
        {
            return;
        }
        try
        {
            auto page = winrt::make_self<AgentManagerContent>();
            // Route keys the content didn't handle back to the page's keybindings (as other content does).
            // Weak: XAML can keep the root alive a beat past the page during teardown.
            page->GetRoot().KeyDown({ get_weak(), &TerminalPage::_KeyDownHandler });
            // ...but tab-switching chords (ctrl+tab, ctrl+alt+<n>) must beat a focused Manager box, which
            // would eat them before the bubbling KeyDown above. Tunnel them in (Root carries a twin wiring).
            page->GetRoot().PreviewKeyDown({ get_weak(), &TerminalPage::_ManagerPaneNavPreviewKeyDown });
            // Off screen until the first show — set BEFORE the wiring, whose SetRegistry / SetManagerState
            // already run refreshes (those now just mark the lens dirty for that first show).
            page->SetLensVisible(false);
            // Wire the Manager UI to the engine (registry + spawn/activate/close/... callbacks).
            _WireAgentManagerContent(page);
            _managerPage = std::move(page);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_CreateAgentManagerPage");
            _managerPage = nullptr;
        }
        // The tab row must stay reachable to host Home (see _UpdateTabView), and Home appears the moment
        // there is a page to open.
        _UpdateTabView();
        _UpdateManagerNavButtons();
    }

    // Agentmaster (the Agent Manager PAGE): put the Manager on screen in place of the current tab — the one
    // thing the tab strip's Home button does, and the fallback wherever the strip has nothing else to show
    // (the last tab closed, a close whose every remaining tab is dormant, a Deactivate of the focused tab,
    // a window reopened while it was on the Manager). It mirrors, step for step, what selecting the old
    // Manager tab did: unfocus the tabs; swap the page's root into the tab-content host (_tabContent — the
    // exact spot the tab's pane occupied, so the layout, the side panel beside it and the full-window pages
    // over it are unchanged); DESELECT the strip, so no tab reads as focused (every tab-targeting action —
    // close, split, rename, find — is then a no-op) and the Home button paints as the active entry; re-open
    // a full-window page (Sessions) that was left open over the Manager; focus the page's invisible key
    // sink (unless the command palette owns focus — GH#7409, the _UpdatedSelectedTab rule); refresh the
    // title / theme / process priority; run the shared Agentmaster selection effects with a NULL tab (the
    // lens goes on screen with its catch-up rebuild, no overlay is focused, the pill, the bring-into-view);
    // and persist it (the window now reopens onto the Manager — its record names no focused tab).
    // Parameters:
    //   reason - which entry point asked; rides the [nav] line ("home", "last tab closed", ...).
    // Idempotent: while the page is already up this does nothing — a Sessions page open over the Manager
    //   keeps its keyboard focus. (A double-clicked Home never reaches here twice: the button's own
    //   double-click guard drops the second click, which would otherwise land on its jump-back face —
    //   _OnManagerHomeButtonClick.) No page (a failed build) is a no-op too. UI thread only.
    // Never throws: every step is contained and logged (Rule #18); a failed step degrades only its effect.
    void TerminalPage::_ShowManagerPage(const std::wstring_view reason)
    {
        if (!_managerPage || _managerPageVisible)
        {
            return;
        }
        // What we are leaving, read BEFORE the flag flips (_GetFocusedTabIndex answers nullopt once it is set)
        // — only for the [nav] line. Not during a removal (_RemoveTab's fallbacks): the tab being left is
        // the one just closed, and the strip's selection then is merely the neighbor MUX auto-picked (never
        // shown), which would read as "from <neighbor>" and mislead a log reader.
        const auto leaving = _removing ? winrt::TerminalApp::Tab{ nullptr } : _GetFocusedTab();
        _managerPageVisible = true;
        try
        {
            // The outgoing tab loses focus exactly as a tab switch unfocuses it (_UpdatedSelectedTab).
            for (const auto& tab : _tabs)
            {
                tab.Focus(FocusState::Unfocused);
            }
            _tabContent.Children().Clear();
            _tabContent.Children().Append(_managerPage->GetRoot());
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ShowManagerPage content swap");
        }
        _DeselectTabStripForManagerPage();
        try
        {
            // A full-window page (Sessions) the user left open over the Manager comes back as it was left —
            // the counterpart of the dismiss every tab switch runs (_UpdatedSelectedTab).
            _RestoreAgentPageOverlays();
            const auto palette = CommandPaletteElement();
            if (!palette || palette.Visibility() != Visibility::Visible)
            {
                _managerPage->Focus(FocusState::Programmatic); // the invisible sink: the page's keys work, nothing looks focused
            }
            TitleChanged.raise(*this, nullptr);
            _updateThemeColors();
            _adjustProcessPriorityThrottled->Run(); // the tab we left drops its foreground boost, like any switch away
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ShowManagerPage restore/focus/title/theme");
        }
        try
        {
            _ApplyAgentTabSelection(nullptr, nullptr);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ShowManagerPage selection effects");
        }
        _ScheduleWindowRecordSave();
        _UpdateManagerNavButtons();
        ::Agentmaster::LogNav(L"manager-page show (" + std::wstring{ reason } + L")" + (leaving ? (L" from " + _DescribeTabForLog(leaving)) : std::wstring{}));
    }

    // Agentmaster (the Agent Manager PAGE): take the page off screen because a TAB is about to take its
    // place. Callers (the tab-switch funnel, _LeaveManagerPageOntoSelectedTab) MUST follow with
    // _UpdatedSelectedTab(toTab): that content swap is what removes the page's root from _tabContent and
    // collapses the full-window pages opened over it, so this only flips the page's own state — the flag
    // (tab-targeting actions see the focused tab again), the lens (hidden: refreshes go back to only
    // marking it dirty), and the Home paint.
    // Parameters:
    //   toTab - the tab being shown instead (null tolerated); only named in the [nav] line.
    // No-op when the page is not up. UI thread only. Never throws (contained + logged — Rule #18).
    void TerminalPage::_HideManagerPage(const TerminalApp::Tab& toTab)
    {
        if (!_managerPageVisible)
        {
            return;
        }
        _managerPageVisible = false;
        try
        {
            if (_managerPage)
            {
                _managerPage->SetLensVisible(false);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_HideManagerPage lens");
        }
        _UpdateManagerNavButtons();
        ::Agentmaster::LogNav(L"manager-page hide -> " + (toTab ? _DescribeTabForLog(toTab) : std::wstring{ L"(no tab)" }));
    }

    // Agentmaster (the Agent Manager PAGE): clear the tab strip's selection so that, while the page is on
    // screen, no tab is drawn as the active one and _GetFocusedTab has nothing to answer. The re-entrant
    // SelectionChanged this raises is marked as ours (_managerPageDeselecting) so the funnel ignores it.
    // Called by _ShowManagerPage, and again after any strip mutation the page survives: MUX TabView
    // re-selects the nearest REALIZED neighbor when an item is removed from a strip with no selection
    // (its OnItemsChanged "SelectedIndex might also already be -1" branch), which would leave a tab
    // highlighted under the page. No-op when nothing is selected. UI thread only. Never throws: a failure
    // is logged (Rule #18) and harmless — _GetFocusedTabIndex already answers nullopt while the page is up.
    void TerminalPage::_DeselectTabStripForManagerPage()
    {
        if (!_tabView)
        {
            return;
        }
        try
        {
            if (!_tabView.SelectedItem())
            {
                return; // already nothing selected
            }
            _managerPageDeselecting = true;
            auto clearDeselecting = wil::scope_exit([this]() noexcept { _managerPageDeselecting = false; });
            _tabView.SelectedItem(nullptr);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_DeselectTabStripForManagerPage");
        }
    }

    // Agentmaster (the Agent Manager PAGE): the belt for the one case the tab-switch funnel can't see. A
    // caller is about to make `tab` the selected tab while the page is up; normally that raises
    // SelectionChanged and _OnTabSelectionChanged leaves the page. But when `tab` is ALREADY the strip's
    // selected item — MUX kept or re-acquired the selection under the page (see
    // _DeselectTabStripForManagerPage) — selecting it again raises nothing, and the page would stay up over
    // a tab the user just asked for (the Home button's jump back to the tab you came from, a click on that
    // tab's header, a toast click). Then this runs the switch the funnel would have run: hide the page,
    // swap the tab in, the shared Agentmaster selection effects, the record save and the nav buttons.
    // Parameters:
    //   tab - the tab about to be selected (null tolerated).
    // Returns: true when it switched to `tab` itself; false when the page is not up, `tab` is null, or the
    //   caller's own selection change will do it.
    // UI thread only. Never throws: a failure is logged (Rule #18) and answers false, so the caller carries
    //   on with its normal selection path.
    bool TerminalPage::_LeaveManagerPageOntoSelectedTab(const TerminalApp::Tab& tab)
    {
        if (!_managerPageVisible || !tab || !_tabView)
        {
            return false;
        }
        try
        {
            const auto tvi = tab.TabViewItem();
            if (!tvi || _tabView.SelectedItem() != tvi)
            {
                return false; // the caller's selection change raises SelectionChanged -> the funnel leaves the page
            }
            _HideManagerPage(tab);
            _UpdatedSelectedTab(tab);
            _ApplyAgentTabSelection(tab, nullptr);
            _ScheduleWindowRecordSave();
            _UpdateManagerNavButtons();
            return true;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_LeaveManagerPageOntoSelectedTab");
        }
        return false;
    }

    // Agentmaster: lazily bind to the TabView's internal horizontal ScrollViewer. It isn't realized
    // until the strip's template is applied (and at least one tab exists), so the first attempts
    // (from _OnFirstLayout) may find nothing — later callers (_OnTabItemsChanged, every
    // _UpdateManagerNavButtons, the strip reveal, the drag gesture) retry until it's there. The strip
    // reveal and the drag gesture's edge auto-scroll are its users now (the Home button no longer tracks
    // the scroll offset — it is always shown). The guard makes repeat calls cheap no-ops.
    //
    // Binding also turns the strip's BRING-INTO-VIEW off (BringIntoViewOnFocusChange = false), so that
    // Agentmaster's own reveal (_RevealTabInStrip) is the only thing that scrolls the strip to a tab.
    //   Why: MUX scrolls its SELECTED tab into view every time the strip's content changes size
    //   (TabView::OnItemsPresenterSizeChanged -> BringSelectedTabIntoView), and in a big window that happens
    //   WHILE THE USER SCROLLS. The strip lays out only the tabs near the view and estimates the width of the
    //   rest from the width it recorded for each tab it ever laid out (StackingLayoutStrategyImpl::
    //   RegisterSize / EstimateNonGroupedExtent). A recorded width can be stale (a cog Save that changed the
    //   tab width; a tab first measured before MUX gave it its equal width), so the first layout of such a
    //   tab during a scroll moves the estimate, the content changes size, and MUX pulls the strip back to the
    //   selected tab. That works as long as the selected tab is still laid out — about two strip widths on
    //   each side of the view — so the strip could never be scrolled away from it. 2026-10-05, release
    //   0.6.8.16, a 476-tab window, an hour after two tab-width cog Saves: "The scrolling now does not allow
    //   me to move away from the selected tab's view". UI Automation (read-only) measured that window's
    //   content at 67,800 px for about 66,650 px of tabs; a 31-tab window, every tab laid out, was exact.
    //   How: XAML's ScrollViewer::OnBringIntoViewRequested acts on a request from inside it only when the
    //   request is FORCED or BringIntoViewOnFocusChange is true, and UIElement::StartBringIntoView — what
    //   MUX's TabViewItem::StartBringTabIntoView calls — never forces. ChangeView (the reveal's precise
    //   pass, the Home button, the drag auto-scroll, MUX's < / > buttons) and ListView::ScrollIntoView (the
    //   panel changes the offset itself) are not bring-into-view requests, so they still work.
    //   What else stops, and who covers it: MUX's reveal on a SELECTION (every selection reveals through
    //   _UpdatedSelectedTab, the close successor included — _RemoveTab calls it — and _TryMoveTab reveals
    //   itself); MUX's reveal after a native drag (native drag is off); the scroll to an element that gets
    //   FOCUS inside the strip — the one that matters, a tab's rename box, is revealed by the page
    //   (_TrackRenamingTabInStrip), and ListView keyboard navigation scrolls through ScrollIntoView. The
    //   Shift+Click revert (_OnTabSelectionChanged) now leaves the strip where you clicked instead of
    //   pulling it to the restored tab.
    // A failure to set it is logged (Rule #18) and the strip keeps MUX's reveals — the behavior before this.
    void TerminalPage::_EnsureTabStripScrollViewer()
    {
        if (_tabStripScrollViewer || !_tabView)
        {
            return;
        }

        auto sv = _FindTabStripScrollViewer(_tabView.as<winrt::WUX::DependencyObject>());
        if (!sv)
        {
            return; // template not realized yet — a later call will retry
        }

        _tabStripScrollViewer = sv;
        try
        {
            // A LOCAL value: it outranks whatever the template or its style sets on this ScrollViewer.
            sv.BringIntoViewOnFocusChange(false);
            // Once per window (the bind runs once): the line that shows a running build carries this fix.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[strip] bring-into-view off: only _RevealTabInStrip scrolls the tab strip to a tab\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_EnsureTabStripScrollViewer BringIntoViewOnFocusChange");
        }
    }

    // Agentmaster: the Manager lens's currently-selected managed session (the highlighted board card /
    // tree row — the same one the Linked-Lenses sync auto-selects when you switch to the Manager from a
    // session's tab). Empty when nothing — or an external (observe-only) — is selected. Read fresh each
    // time so it always reflects the live selection.
    std::wstring TerminalPage::_ManagerSelectedSessionId() const
    {
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                return std::wstring{ mgr->SelectedSessionId() };
            }
        }
        return {};
    }

    // Agentmaster: the session the Home button's JUMP-BACK face returns to — the Manager lens's selected
    // managed session (the board card / tree row highlighted on the page; the Linked-Lenses sync selects
    // the session of the tab you switched from, so it is usually where you came from), but only while the
    // Agent Manager page is on screen and only when THIS window hosts that session as a live tab. A
    // session hosted by another window (the GLOBAL board shows the whole fleet) is no jump BACK — a click
    // would carry the user into a different window — so the button keeps its Home face then; the same rule
    // the separate Jump Back button had before it merged into Home (2026-10-12).
    // Returns: the session id; empty when there is nothing to jump back to (off the page, nothing or an
    //   external selected, the session's tab closed or hosted elsewhere).
    // UI thread only (reads the page flag and _claudeTabs). Never throws: a flag read, the lens's own
    //   selection read and a map lookup with a weak-reference resolve.
    std::wstring TerminalPage::_ManagerJumpBackSessionId() const
    {
        if (!_managerPageVisible)
        {
            return {};
        }
        auto sel = _ManagerSelectedSessionId();
        if (sel.empty())
        {
            return {};
        }
        const auto it = _claudeTabs.find(sel);
        if (it == _claudeTabs.end() || !it->second.get())
        {
            return {}; // its tab is gone, or it lives in another window
        }
        return sel;
    }

    // Agentmaster: put the Home button's FACE on it — its glyph, tooltip and accessible name, which always
    // change together (the click decides from the live state, the same way — _OnManagerHomeButtonClick):
    //  - off the page: HOME (E80F). A click opens the Agent Manager page.
    //  - on the page, nothing to jump back to: still HOME (painted active by _PaintManagerHomeButton). A
    //    click does nothing — the page is already up — so the tooltip says how the arrow comes back.
    //  - on the page, with a session to jump back to: the JUMP-BACK arrow (E7A7, the Undo glyph the separate
    //    Jump Back button wore until 2026-10-12). A click returns to that session's tab. The tooltip and the
    //    accessible name say which session, by the title its tab shows (a managed tab's pinned session
    //    title, Rule #11) — the selection is not always the tab you came from (a card clicked on the page,
    //    or a page opened from a shell tab, which leaves the earlier selection in place).
    // Change-gated on the tooltip text (_managerHomeButtonTip), which encodes the whole face, because the
    // updater runs on every tab add / remove / switch — hundreds of times in a big window restore — and each
    // glyph write would re-measure the strip. The tooltip goes through AgentSetTip, the shared islands-safe
    // tip: a re-call only swaps the recorded text, and a tip open on the button shows the new text at once.
    // Parameters:
    //   backSessionId - the session a click would return to (_ManagerJumpBackSessionId); empty = a Home face.
    // UI thread only. Never throws: a failed write is logged (Rule #18) and leaves the gate unchanged, so the
    //   next update retries; a click still does the right thing meanwhile, since it reads the live state.
    void TerminalPage::_ApplyManagerHomeButtonFace(const std::wstring& backSessionId)
    {
        if (!_managerHomeButton)
        {
            return;
        }
        try
        {
            const bool back = !backSessionId.empty();
            // The session the arrow names: its tab's title. Never empty for a managed tab in practice (the
            // title is pinned at launch); an empty one falls back to a generic phrase, still a full sentence.
            std::wstring backTitle;
            if (back)
            {
                if (const auto it = _claudeTabs.find(backSessionId); it != _claudeTabs.end())
                {
                    if (const auto tab = it->second.get())
                    {
                        backTitle = std::wstring{ tab.Title() };
                    }
                }
            }
            std::wstring tip;
            std::wstring name;
            if (back)
            {
                const auto quoted = backTitle.empty() ? std::wstring{ L"the selected session" } : (L"\x201C" + backTitle + L"\x201D");
                tip = L"Jump back to " + quoted + L" \x2014 leave the Agent Manager for the tab of the session selected in it.";
                name = L"Jump back to " + (backTitle.empty() ? std::wstring{ L"the selected session" } : backTitle);
            }
            else if (_managerPageVisible)
            {
                tip = L"Agent Manager \x2014 open now. Select a session of this window (a board card or a tree row) and this button turns into a back arrow that returns to its tab. Or click any tab.";
                name = L"Agent Manager";
            }
            else
            {
                tip = L"Agent Manager \x2014 open the fleet page (Triage Board, Explorer Tree, Prompt Queue). While it is open, this button turns into a back arrow that returns to the selected session's tab.";
                name = L"Agent Manager";
            }
            if (_managerHomeButtonTip == tip)
            {
                return; // this face is already on the button
            }
            // The glyph: the FontIcon is the button's DIRECT Content (TabRowControl.xaml keeps it so). A
            // different Content would only leave the glyph unchanged; the tooltip and the name still flip.
            if (const auto glyph = _managerHomeButton.Content().try_as<winrt::WUX::Controls::FontIcon>())
            {
                glyph.Glyph(back ? L"\xE7A7" : L"\xE80F");
            }
            winrt::WUX::Automation::AutomationProperties::SetName(_managerHomeButton, winrt::hstring{ name });
            // With the open delay the XAML tooltip had (_ManagerHomeButtonTipDelay), not AgentSetTip's faster default.
            AgentSetTip(_managerHomeButton, winrt::hstring{ tip }, _ManagerHomeButtonTipDelay());
            // Last, so a throw above leaves the gate on the old face and the next update writes again.
            _managerHomeButtonTip = std::move(tip);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ApplyManagerHomeButtonFace");
        }
    }

    // Agentmaster: recompute the tab-strip Home button — the Agent Manager's whole presence in the strip now
    // that it is a page, not a tab:
    //  - shown whenever this window HAS a Manager page (always, once _CreateAgentManagerPage built it) — it
    //    is the ONLY way to open the page, so it can never hide the way the old scroll-triggered "the
    //    Manager tab scrolled off" Home did;
    //  - painted as the strip's active entry while the page is on screen (the tabs are deselected then —
    //    nothing else says "you are here");
    //  - its face: Home, or — on the page, with a selected managed session this window hosts as a live tab
    //    (typically the session you came from, which the Linked-Lenses sync auto-selected) — the jump-back
    //    arrow that returns to that tab (the separate Jump Back button until 2026-10-12).
    // Called from tab add/remove, tab switch, page show/hide, the lens-changed push (selection), a hover on
    // the button, and once at first layout. Cheap and idempotent (every write is change-gated). UI thread only.
    void TerminalPage::_UpdateManagerNavButtons()
    {
        _EnsureTabStripScrollViewer();

        // --- Home: always present with a page; the active paint follows the page. ---
        _SetTabStripButtonVisible(_managerHomeButton, static_cast<bool>(_managerPage));
        _PaintManagerHomeButton(_managerHomeButton, _managerPageVisible);

        // --- Its face: the jump-back arrow on the page with a selected managed session this window hosts
        //     live; Home otherwise. ---
        _ApplyManagerHomeButtonFace(_ManagerJumpBackSessionId());
    }

    // Agentmaster: the Home button's action, by the face it shows (both read from the live state here, not
    // from the face, so a click can never act on a stale face):
    //  - off the page: open the Agent Manager page (the one and only user-facing way to reach it, now that
    //    the pinned Manager tab is gone);
    //  - on the page, with a session to jump back to (_ManagerJumpBackSessionId): return to that session's
    //    tab. _ActivateClaudeSession selects it in this window, and that selection takes the page off screen
    //    (the tab-switch funnel, or _LeaveManagerPageOntoSelectedTab when the strip still had that tab
    //    selected under the page) — whereupon the button turns back into Home;
    //  - on the page, with nothing to jump back to: nothing (the page is already up; a Sessions page open
    //    over the Manager keeps its focus).
    // A DOUBLE-CLICK GUARD: a click within the system double-click time of a click that ACTED (opened the
    // page, or jumped back) is ignored. Without it a double-click on Home would open the page and jump
    // straight back (its second click lands on the arrow), and a double-click on the arrow would jump back
    // and reopen the page. The guard is armed BEFORE the action, so it holds however the tab switch is
    // delivered; a click that does nothing (the Home face on the page) does not arm it. Logged as a [nav]
    // line so a "my click did nothing" report shows why.
    // Parameters: sender / args — the Click event's (unused: the button is the one _managerHomeButton).
    // UI thread only. Never throws: an event handler must not (an exception escaping into XAML's dispatch
    // can end the process), so a failure — the jump back's tab selection on a TabView torn down under it —
    // is logged (Rule #18) and the click simply does nothing more.
    void TerminalPage::_OnManagerHomeButtonClick(const IInspectable& /*sender*/, const winrt::WUX::RoutedEventArgs& /*args*/)
    {
        try
        {
            const auto now = ::GetTickCount64();
            if (_managerHomeButtonFlipTick != 0 && now - _managerHomeButtonFlipTick <= ::GetDoubleClickTime())
            {
                ::Agentmaster::LogNav(L"home-button click ignored (the second click of a double-click)");
                return;
            }
            if (_managerPageVisible)
            {
                const auto backId = _ManagerJumpBackSessionId();
                if (backId.empty())
                {
                    return; // the Home face on the page: nothing to jump back to, and the page is already up
                }
                _managerHomeButtonFlipTick = now;
                ::Agentmaster::LogNav(L"home-button jump-back -> " + ::Agentmaster::ShortId(backId));
                _ActivateClaudeSession(winrt::hstring{ backId });
                return;
            }
            _managerHomeButtonFlipTick = now;
            _ShowManagerPage(L"home");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_OnManagerHomeButtonClick");
        }
    }

    // Agentmaster: the pointer entered the Home button — bring its face up to date before its tooltip opens
    // (the shared tip reads the text when it opens, ~1/3 of the hover time later). The jump-back tooltip
    // names the session by its tab's title, and a rename while the page is up (F2 in the tree) raises
    // nothing that reaches the button, so without this the arrow could name the session's old title.
    // Parameters: sender / args — the PointerEntered event's (unused; never marked Handled, so the button's
    // own hover visuals and the tooltip's handlers run as before).
    // UI thread only. Never throws: an event handler must not, so a failure is logged (Rule #18) and the
    //   button keeps the face it had.
    void TerminalPage::_OnManagerHomeButtonPointerEntered(const IInspectable& /*sender*/, const winrt::WUX::Input::PointerRoutedEventArgs& /*args*/)
    {
        try
        {
            _UpdateManagerNavButtons();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_OnManagerHomeButtonPointerEntered");
        }
    }

    // Agentmaster: stand up the session-management engine — the SessionRegistry (single
    // source of truth) and the HooksBridge (the local named-pipe listener that turns Claude
    // Code hook events into authoritative session state). Called once, before the Manager
    // tab opens, so the pipe is live and the registry exists when sessions are spawned.
    void TerminalPage::_InitAgentmasterEngine()
    {
        if (_sessionRegistry)
        {
            return;
        }

        _appSettings = ::Agentmaster::LoadAppSettings(); // the Settings cog (per-field defaults if absent)
        // Agentmaster: publish the GLOBAL tab-rename commit mode to the (process-wide) tab headers.
        // Cross-window, so it lives in one process-static the headers read live, not per-tab state.
        SetTabRenameCommitMode(static_cast<int32_t>(_appSettings.tabRenameCommitMode));
        // Agentmaster (RENDERING.md): publish the GLOBAL GPU/CPU rendering choice to the Control DLL
        // BEFORE this window restores or creates any tab (_OnFirstLayout runs this first), so every
        // terminal initializes on the chosen renderer — a CPU-rendering fleet never builds a single
        // per-tab GPU device, which is the whole point. Process-wide and idempotent: every window sets
        // the same value from the same settings.json.
        try
        {
            winrt::Microsoft::Terminal::Control::TermControl::SetSoftwareRenderingOverride(_appSettings.softwareRendering);
            if (_appSettings.softwareRendering)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[render] software rendering ON (settings.json) - terminals draw on the CPU (WARP)\n");
            }
        }
        catch (...)
        {
            // Only the activation-factory hop can fail: terminals then start on the GPU, the historical
            // renderer — degraded, never broken (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"_InitAgentmasterEngine(software rendering)");
        }
        // Agentmaster (RENDERING.md §4c, "Hidden tabs"): publish the GLOBAL off-screen policy before any
        // tab exists, so every terminal this window creates or restores follows it from its first
        // visibility change on. No walk is needed: there are no terminals yet. Logged so a report of
        // "tabs flash blank when I switch back" can be matched to the active policy.
        try
        {
            winrt::Microsoft::Terminal::Control::TermControl::ConfigureHiddenRenderingPolicy(_appSettings.pauseHiddenTerminals,
                                                                                             ::Agentmaster::HiddenTerminalReleaseDelayMs(_appSettings));
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[render] hidden tabs (settings.json): " + _HiddenRenderingPolicyText() + L"\n");
        }
        catch (...)
        {
            // Only the activation-factory hop can fail: terminals then keep the Control DLL's built-in
            // defaults (pause on, release after 60 s) — the same as the shipped settings (Rule #18).
            ::Agentmaster::AgentLogCaughtException(L"_InitAgentmasterEngine(hidden rendering)");
        }
        // Agentmaster: seed the tab-strip close affordances (show-X / middle-click close) from the
        // loaded settings now (the initial SetSettings pass ran with default _appSettings). No tabs
        // exist yet, so this just sets the hook flag + overlay mode; per-tab visibility is then applied
        // as each tab is created/selected (_UpdatedSelectedTab -> _updateAllTabCloseButtons).
        _updateAllTabCloseButtons();
        // Agentmaster (tab sizing): publish the cog's TAB SIZE settings (width mode, width bounds, height,
        // title font) BEFORE this window builds its first tab, so the strip's first layout already uses
        // them. No forced relayout — no tab exists yet; every tab later gets its share in _InitializeTab.
        _ApplyTabSizingSettings(false);

        // M9: consume the ONE process-wide engine. v1.24 WT is a WindowEmperor — every window
        // lives in a single process — so the SessionRegistry (single source of truth), the
        // HooksBridge (the `\\.\pipe\agentmaster.<pid>` listener — the PID is unambiguous only
        // because there is exactly one bridge), and the Scheduler must be a process singleton,
        // NOT a per-window object. The first window to reach here constructs + wires + starts
        // it (the logging / scheduler / persistence observers, the pipe, bridge discovery + the
        // shared hook files + the claude PATH shim — see AgentMaster/Engine.{h,cpp}); every
        // later window receives the same instance. This page just copies the shared_ptrs.
        auto& engine = ::Agentmaster::SharedEngine();
        _sessionRegistry = engine.registry;
        _hooksBridge = engine.bridge;
        _scheduler = engine.scheduler;
        _scanner = engine.scanner;
        _observer = engine.observer; // Fleet Observer S-lane (PULL census/correlation; OBSERVER.md §10)

        // Agentmaster (MESSAGING.md §5.4): give the Telegram bot the two things only the app layer has — the Search
        // topic's agent (a SessionSearchAgent needs the SDK unity file, which Engine.cpp's plain-C++ contract and the
        // test harness never build) and the version word for its start note. Once per process: the service is the
        // engine's one instance, and a second window must not replace a factory a running Search already used.
        {
            static std::once_flag messagingWiredOnce;
            std::call_once(messagingWiredOnce, [&engine] {
                try
                {
                    if (engine.messaging)
                    {
                        engine.messaging->SetSearchFactory([](std::function<void(const ::Agentmaster::Msg::SearchBridgeEvent&)> sink, std::wstring thinkingWord) {
                            return ::Agentmaster::Msg::MakeTelegramSearchBridge(std::move(sink), std::move(thinkingWord));
                        });
                        // MESSAGING.md §21.25: the new-thread wizard's MODEL step lists the right-click menu's own models and
                        // presets. Those live in LaunchPresetsOs.h (the presets file, Credential Manager), which only this
                        // layer links; LaunchMenuEntries and ShouldOfferAddPresets read a stamp-cached config under its own
                        // lock, so the service may call this from its poll thread.
                        engine.messaging->SetLaunchMenuSource([](const std::wstring& launchModelsSpec) {
                            ::Agentmaster::Msg::LaunchMenuInfo info;
                            info.entries = ::Agentmaster::LaunchMenuEntries(launchModelsSpec);
                            info.offerAddPresets = ::Agentmaster::ShouldOfferAddPresets();
                            return info;
                        });
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"messaging: set the Search factory");
                }
            });
        }

        // Agentmaster (System notifications; NOTIFICATIONS.md §4a): register the TOAST COM ACTIVATOR
        // process-wide, so the shell delivers a toast click to THIS running instance
        // (INotificationActivationCallback::Activate -> the activate fan-out -> foreground + jump)
        // instead of falling back to an AUMID activation, which launches a second process and opens a
        // stray new window. Once per process (std::once_flag) — it registers a machine-global COM class
        // object, not per-window state. It lives here rather than in Engine.cpp because that TU is plain
        // C++/no-WinRT by contract, and this needs COM/WinRT; and it must be in TerminalApp.dll rather
        // than the EXE because the jump routes through the engine's fan-out, which the EXE doesn't link.
        {
            static std::once_flag toastActivatorOnce;
            std::call_once(toastActivatorOnce, [] { ::Agentmaster::ToastActivator::Register(); });
        }

        // Agentmaster (exception forensics): install the VEH throw-stack ring + this module's wil
        // failure sink, so every swallowed catch(...) / CATCH_LOG logs WHAT threw and the THROW-SITE
        // stack (module+RVA, offline-symbolizable) to hooks.log. Self-onced; cheap when idle.
        ::Agentmaster::InstallAgentExceptionTrace();

        // M10 (PERSISTENCE.md §13): claim this window's persisted record — an existing
        // windows/<id>.json (geometry + Manager lens + ordered tab refs), or a fresh GUID if none
        // remains. Stashed in _windowRecord; its lens is seeded into the Manager page in
        // _WireAgentManagerContent, and changes are debounced-autosaved back to the same file.
        // Increment 3: when the Emperor assigned this window a specific record (multi-window
        // reopen), claim THAT id so geometry (TerminalWindow) and lens (here) come from the same
        // record; otherwise claim the front record (single-window) or mint a fresh id.
        // Agentmaster (tear-out content loss): a window created from MOVED content — a tab torn out
        // into a new window, or moved cross-window — must NEVER front-pop a leftover on-disk record.
        // Its content comes from the moved-content startup actions (_OnFirstLayout ->
        // ProcessStartupActions) and its geometry from the drop point. Front-popping an unclaimed
        // record here sets _windowRecordClaimed, which makes _OnFirstLayout SKIP the moved content
        // (the `&& !_windowRecordClaimed` gate) AND makes _RestoreWindowTabs replay the stale record's
        // tabs instead — silently DROPPING the dragged session (the live content orphans in the
        // process-wide ContentManager: claude.exe keeps running but no tab hosts it). This mirrors
        // upstream TerminalWindow::Initialize, where moved content wins over a persisted layout. An
        // explicit Emperor-assigned id (multi-window reopen) still claims THAT record by id.
        std::optional<::Agentmaster::WindowRecord> claimed;
        if (!_assignedWindowId.empty())
        {
            claimed = ::Agentmaster::ClaimWindowRecord(_assignedWindowId);
        }
        else if (!_isContentWindow)
        {
            claimed = ::Agentmaster::ClaimWindowRecord();
        }
        if (claimed)
        {
            _windowRecord = std::move(*claimed);
            _windowRecordClaimed = true;
        }
        else
        {
            GUID fresh{};
            ::CoCreateGuid(&fresh);
            _windowRecord = ::Agentmaster::WindowRecord{};
            _windowRecord.windowId = ::Microsoft::Console::Utils::GuidToString(fresh);
        }
        _windowId = _windowRecord.windowId;

        // Restore-story trace (line 1): did this window CLAIM a saved record (geometry + lens + ordered tab
        // refs) or start fresh? When claimed, count the refs by kind + name the focused session, so the
        // [rehome-begin]/[rehome] lines that follow (in _RestoreWindowTabs) read as one coherent window
        // restore story for THIS windowId. Once per window creation — never per-tick.
        if (_windowRecordClaimed)
        {
            size_t cl = 0, cx = 0, sh = 0;
            for (const auto& t : _windowRecord.tabs)
            {
                if (t.kind == ::Agentmaster::TabKind::Claude)
                {
                    ++cl;
                }
                else if (t.kind == ::Agentmaster::TabKind::Codex)
                {
                    ++cx;
                }
                else
                {
                    ++sh;
                }
            }
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[window-claim] " + _windowId + L" refs=" + std::to_wstring(_windowRecord.tabs.size()) +
                                              L" (claude=" + std::to_wstring(cl) + L" codex=" + std::to_wstring(cx) + L" shell=" + std::to_wstring(sh) +
                                              L") selected=" + ::Agentmaster::ShortId(_windowRecord.selectedSessionId) + L"\n");
        }
        else
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[window-fresh] " + _windowId + (_isContentWindow ? std::wstring{ L" (dragged-out content window)" } : std::wstring{ L" (new window \x2014 no saved record)" }) + L"\n");
        }

        // M10 Increment 3 (open-at-exit manifest; PERSISTENCE.md §13.5): mark this window LIVE in the
        // process-wide set, which rewrites open-windows.json. Done here (not at WindowEmperor create
        // time) because _windowId is only resolved now — register early + correct so even a one-window
        // session lands in the manifest and reopens next run. Unregistered in ~TerminalPage.
        ::Agentmaster::RegisterLiveWindow(_windowId);

        // Agentmaster (ui-stall escalation; Engine.h block comment + the TU-local helpers above):
        // create this window's message-only RESCUE window on the UI thread and register the rescuer
        // the engine watchdog SENDS into when the UI lane stops beating — a sent message executes on
        // the UI thread even while posted/dispatcher work is jammed (the 2026-08-22 wedge), so it
        // both proves the pump alive and drains the backed-up dispatcher queue. The restart handler
        // is process-wide, registered once by whichever window initializes first (it is
        // window-agnostic — by the time it fires, EVERY window's lane is dead). Detached in
        // ~TerminalPage (Rule #10).
        {
            _uiRescueWnd = _CreateAgentUiRescueWindow(_windowId, Dispatcher());
            if (_uiRescueWnd)
            {
                const HWND rescue = _uiRescueWnd;
                ::Agentmaster::RegisterUiStallRescuer(_windowId, [rescue]() {
                    ::Agentmaster::UiRescueOutcome o;
                    DWORD_PTR res = 0;
                    if (::SendMessageTimeoutW(rescue, kAgentUiRescueMsg, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 3000, &res))
                    {
                        o.pumpAlive = true; // the handler ran on the UI thread within the timeout
                        o.drainRan = (res == 2);
                    }
                    return o;
                });
            }
            else
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[ui-stall] rescue window creation FAILED for " + _windowId + L" — stalls will be logged but not rescued\n");
            }
            static std::once_flag s_uiStallRestartOnce;
            std::call_once(s_uiStallRestartOnce, []() {
                ::Agentmaster::SetUiStallRestartHandler([](const std::wstring& reason) {
                    _AgentUiStallRestart(reason);
                });
            });
        }

        // Cross-window activate sink (Linked Lenses): the Manager board/tree show the WHOLE fleet,
        // but a session's tab lives in exactly one window — when ANOTHER window's Activate
        // (board/tree double-click, tree Enter, the Auto Testing's eye) targets a session hosted
        // HERE, this sink hops to this window's UI thread, re-checks _claudeTabs there (the host
        // can change while the hop is in flight), and on a hit selects the tab + brings this
        // window to the foreground. A miss is a no-op — the engine fans out to every window, and
        // only the (single) host acts. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowActivateToken = ::Agentmaster::RegisterWindowActivateHandler(_windowId, [weakThis, dispatcher](const std::wstring& id) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                    if (auto self = weakThis.get())
                    {
                        self->_FocusClaudeSessionTab(id, /*bringWindowToFront*/ true);
                    }
                });
            });
        }

        // Cross-window restart sink: the twin of the activate sink for "Restart session" (Triage Board
        // card / Explorer-tree row). When ANOTHER window's restart targets a session hosted HERE, this
        // sink hops to this window's UI thread and rebuilds its connection in place (no foreground — a
        // restart shouldn't yank the user away from the board). A miss is a no-op. Detached in
        // ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            // HOOKS.md "Interrupted turns": `turnChoice` is the answer the caller already got to "continue the turn you
            // interrupted?" (the window the user clicked in, or the Telegram topic) — it restarts with it, so the hosting
            // window never asks a second time. Ask (nobody asked yet) lets this window ask.
            _windowRestartToken = ::Agentmaster::RegisterWindowRestartHandler(_windowId, [weakThis, dispatcher](const std::wstring& id, ::Agentmaster::RestartTurnChoice turnChoice) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id, turnChoice]() {
                    if (auto self = weakThis.get())
                    {
                        self->_RestartClaudeSessionLocal(id, turnChoice);
                    }
                });
            });
        }

        // Cross-window close-session sink ("Close ▸ Of Same Folder"): the board/tree session menu closes
        // every live managed session sharing a folder; the initiating window closes the tabs it hosts and
        // fans the rest out here. When ANOTHER window's folder-close targets a session hosted HERE, this
        // sink hops to this window's UI thread and closes its tab (skip-confirm — the initiating window's
        // one "Close N sessions in this folder?" dialog already covered the whole batch). A miss is a
        // no-op. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowCloseSessionToken = ::Agentmaster::RegisterWindowCloseSessionHandler(_windowId, [weakThis, dispatcher](const std::wstring& id) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                    if (auto self = weakThis.get())
                    {
                        self->_CloseClaudeSessionLocal(id);
                    }
                });
            });
        }

        // Cross-window park-session sink ("Deactivate ▸ Of Same Folder"): the tab menu's folder scopes park
        // every live managed session sharing a folder; the initiating window parks the tabs it hosts and
        // fans the rest out here. When ANOTHER window's folder-deactivate names a session hosted HERE, this
        // sink hops to this window's UI thread and parks its tab — a park swaps the pane's terminal control,
        // which only the hosting window's UI thread may touch. No confirmation (no Deactivate scope asks),
        // and this window's own focused-tab rule applies: a session that is on screen here sends this window
        // to its Agent Manager page first. A miss is a no-op. Each session arrives as its own dispatcher
        // item, so a batch never holds this window's UI thread for more than one park at a time. Detached
        // in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowDeactivateSessionToken = ::Agentmaster::RegisterWindowDeactivateSessionHandler(_windowId, [weakThis, dispatcher](const std::wstring& id) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id]() {
                    // The page may be gone by the time the item runs (the window closed meanwhile): a dead
                    // weak reference simply ends it.
                    if (auto self = weakThis.get())
                    {
                        // An exception escaping a dispatcher item is fatal to the process, and a park
                        // touches XAML and WinRT throughout — contain it (Rule #18) and leave the tab running.
                        try
                        {
                            self->_DeactivateSessionLocal(id);
                        }
                        catch (...)
                        {
                            ::Agentmaster::AgentLogCaughtException(L"park-session sink (_DeactivateSessionLocal)");
                        }
                    }
                });
            });
        }

        // Agentmaster (MESSAGING.md §8.2): the Telegram bot's two window sinks. The SESSION-ACTION sink receives
        // (session, action, arg, reply) from the engine's fan-out on a bot thread, hops to this window's UI thread
        // and acts only when this window hosts the session's tab (_RemoteSessionAction); the LAUNCH sink receives a
        // resume / fork / new-session request the engine aimed at THIS window (the most recently activated one), opens it
        // as a background tab (_RemoteLaunch) and answers what came of it (MESSAGING.md §21.23). Both detach in
        // ~TerminalPage (Rule #10). Each item is contained (Rule #18): an exception escaping a dispatcher item ends the
        // process.
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowSessionActionToken = ::Agentmaster::RegisterWindowSessionActionHandler(_windowId, [weakThis, dispatcher](const std::wstring& id, const std::wstring& action, const std::wstring& arg, std::function<void(std::wstring)> reply) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, id, action, arg, reply]() {
                    if (auto self = weakThis.get())
                    {
                        try
                        {
                            self->_RemoteSessionAction(id, action, arg, reply);
                        }
                        catch (...)
                        {
                            ::Agentmaster::AgentLogCaughtException(L"remote session-action sink (_RemoteSessionAction)");
                        }
                    }
                });
            });
            // MESSAGING.md §21.23: ANSWERED. The work item carries the engine's reply and calls it with _RemoteLaunch's answer —
            // or with WindowClosed when the page is gone by the time the item runs. An item the dispatcher drops unrun (a window
            // closing) takes the reply's copy with it, and the engine answers WindowClosed for it (LaunchAnswer). A dispatcher
            // that refuses the item answers FALSE here, never through the reply: the engine then asks the next window.
            _windowLaunchToken = ::Agentmaster::RegisterWindowLaunchHandler(_windowId, [weakThis, dispatcher](const ::Agentmaster::RemoteLaunchRequest& req, ::Agentmaster::RemoteLaunchReply reply) -> bool {
                try
                {
                    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, req, reply]() {
                        ::Agentmaster::RemoteLaunchOutcome outcome;
                        outcome.result = ::Agentmaster::RemoteLaunchResult::WindowClosed; // the window closed between the hand-over and now
                        if (auto self = weakThis.get())
                        {
                            outcome = self->_RemoteLaunch(req); // noexcept: every failure comes back as an answer
                        }
                        if (reply)
                        {
                            reply(outcome); // the engine's LaunchAnswer: delivered once, and it contains a throwing requester itself
                        }
                    });
                    return true;
                }
                catch (...)
                {
                    // Rule #18. Recovery: a dispatcher that refused the item (a window being torn down) — this window cannot take
                    // the launch, and says so at once: the engine asks the next window instead of waiting for an answer.
                    ::Agentmaster::AgentLogCaughtException(L"remote launch sink: RunAsync");
                    return false;
                }
            });
        }

        // Agentmaster (SESSIONS.md §9.14): the AI search's AGENT-TOOL sink — its window and tab tools (list this window's
        // strip, open a resumed / forked session at a chosen place here, park a tab this window hosts). The engine waits
        // for the REPLY on the SDK's reader thread, so every path must reply exactly once: a page that is gone, a
        // dispatcher that refused the item, or a throw all reply with an outcome (handled == false: "not this window")
        // instead of leaving the caller to wait out its timeout. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowAgentToolsToken = ::Agentmaster::RegisterWindowAgentToolHandler(_windowId, [weakThis, dispatcher](const ::Agentmaster::WindowToolRequest& request, std::function<void(::Agentmaster::WindowToolOutcome)> reply) {
                // The reply may run once only (the engine ignores a second one); a shared flag keeps the dispatcher item
                // and the RunAsync failure path below from both answering.
                auto answered = std::make_shared<std::atomic<bool>>(false);
                auto answer = [reply, answered](::Agentmaster::WindowToolOutcome outcome) {
                    if (!answered->exchange(true) && reply)
                    {
                        reply(std::move(outcome));
                    }
                };
                try
                {
                    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, request, answer]() {
                        ::Agentmaster::WindowToolOutcome outcome; // handled == false: the window closed meanwhile
                        if (auto self = weakThis.get())
                        {
                            outcome = self->_RunAgentWindowTool(request); // noexcept: failures come back as outcomes
                        }
                        answer(std::move(outcome));
                    });
                }
                catch (...)
                {
                    // Rule #18. Recovery: a dispatcher that refused the item (a window being torn down) answers "not this
                    // window" at once, so the engine does not wait out its timeout for it.
                    ::Agentmaster::AgentLogCaughtException(L"agent-tool sink: RunAsync");
                    answer(::Agentmaster::WindowToolOutcome{});
                }
            });
        }

        // Cross-window tab-DOCKING target (the pointer-owned gesture's OLE-docking replacement): register
        // THIS window's top-level HWND + a probe so another window's in-flight drag can ask "is this
        // screen point over YOUR strip row, and at which insertion slot?" (Preview also lights this
        // window's own insertion caret). Invoked SYNCHRONOUSLY by the dragging window — every window
        // lives on the Emperor's ONE UI thread (WindowEmperor::CreateNewWindow), the same fact the
        // native OLE receive path relied on (AppHost::_handleMoveContent calls the target's
        // AttachContent directly) — and self-guarded: a torn-down page (or any threading surprise —
        // the guarded XAML reads would throw) answers a MISS, which the source degrades to the classic
        // new-window tear-out. Detached in ~TerminalPage (Rule #10).
        if (_hostingHwnd)
        {
            const auto weakThis = get_weak();
            _windowDockTargetToken = ::Agentmaster::RegisterWindowDockTarget(
                _windowId,
                static_cast<void*>(*_hostingHwnd),
                [weakThis](long screenX, long screenY, ::Agentmaster::DockProbeMode mode) -> ::Agentmaster::DockProbeResult {
                    try
                    {
                        if (auto self = weakThis.get())
                        {
                            return self->_DockProbeHit(screenX, screenY, mode);
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"dock-target probe");
                    }
                    return {}; // miss — the source falls back to the classic tear-out
                });
        }

        // Cross-window "Activate All Tabs" sink (eager-init): the Manager's fleet-wide "Activate All Tabs"
        // in ANOTHER window fans out here; this sink hops to this window's UI thread and eager-inits all of
        // its OWN dormant tabs (a tab can only be started in the window that hosts its control). Detached in
        // ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _windowActivateAllToken = ::Agentmaster::RegisterActivateAllDormantHandler(_windowId, [weakThis, dispatcher]() {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    if (auto self = weakThis.get())
                    {
                        self->_ActivateAllDormantTabsLocal();
                    }
                });
            });
        }

        // Command-action sink (COMMANDS.md): a CommandWatch binding's await resolved — v1 the
        // /handover markdown landed on disk. Fired on the engine's scanner thread and fanned to
        // EVERY window (no source window exists); this sink hops to this window's UI thread, and
        // the handler acts only when THIS window hosts the origin session's tab (spawn the
        // "(handover)" successor beside it). A miss is a no-op. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _commandActionToken = ::Agentmaster::RegisterCommandActionHandler(_windowId, [weakThis, dispatcher](const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, sessionId, command, payload, args]() {
                    // Safeguard: a background-originated action landing on the UI thread must
                    // never unwind unhandled (an uncaught throw here terminates the app). The
                    // handler is internally guarded too; this is the dispatch-lambda belt.
                    // `args` = the typed command's <command-args> verbatim — the §6b per-message
                    // model hint parses out of their leading words inside the handler.
                    try
                    {
                        if (auto self = weakThis.get())
                        {
                            if (command == L"handover")
                            {
                                self->_HandleCommandHandover(sessionId, payload, args);
                            }
                            else if (command == L"handover-here")
                            {
                                // COMMANDS.md — the in-place twin: REPLACE the origin tab (the
                                // Restart-session swap into a fresh successor) instead of
                                // spawning a new tab beside it.
                                self->_HandleCommandHandover(sessionId, payload, args, /*inPlace*/ true);
                            }
                            else if (command == L"handover-standby")
                            {
                                // COMMANDS.md §5b — the FILL-NOT-SEND member: new successor
                                // tab(s) like /handover, but each briefing is TYPED into its
                                // session's input box WITHOUT submitting (one Enter away).
                                self->_HandleCommandHandover(sessionId, payload, args, /*inPlace*/ false, /*standby*/ true);
                            }
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"command-action sink dispatch");
                    }
                });
            });
        }

        // Cross-window settings broadcast: a GLOBAL settings change in ANOTHER window (the cog Save, or
        // the Explorer-Tree / Triage-Board sort toggle) reaches here; this sink hops to this window's UI
        // thread and re-applies it live (this window's _appSettings + the sort toggles + a board/tree
        // re-sort). The SOURCE window is excluded by BroadcastSettingsChanged (it already applied it), so
        // this never echoes back. Detached in ~TerminalPage (Rule #10).
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _settingsChangedToken = ::Agentmaster::RegisterSettingsChangedHandler(_windowId, [weakThis, dispatcher](const ::Agentmaster::AppSettings& s) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, s]() {
                    if (auto self = weakThis.get())
                    {
                        self->_ApplyBroadcastSettings(s);
                    }
                });
            });
        }

        // Taskbar unread-count sink (NOTIFICATIONS.md section 8): the engine's unread census calls this whenever the
        // process-wide count changes — whichever window's flash / Mark-Unread change moved it, this one included — and
        // this window repaints its own taskbar button with the SAME number (the taskbar combines our windows into one
        // button whose overlay is "the last one received"). The hop carries no value on purpose: _ApplyTaskbarUnreadCount
        // re-reads the census, so hops landing out of order can't regress the badge. The seed right after covers a
        // window opened while others already hold unread sessions. Detached in ~TerminalPage (Rule #10).
        // The SAME hop re-rings the side panel's cards (SIDE_PANEL.md §2g): a card's status icon shows the red flash ring
        // whenever its session's tab rings, in ANY window (a GLOBAL list shows other windows' sessions, whose flash state
        // only their own pages know) — and this sink is the one signal every window hears for every window's change. It
        // re-reads the census's SET (not the count), so out-of-order hops converge too; a change that keeps the count,
        // which no sink hears, is caught by the side panel's periodic tick.
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _unreadCountToken = ::Agentmaster::RegisterUnreadCountHandler(_windowId, [weakThis, dispatcher](uint32_t /*count*/) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    if (auto self = weakThis.get())
                    {
                        self->_ApplyTaskbarUnreadCount();
                        self->_SidePanelNoteUnreadChanged(); // never throws; a no-op until the panel is built
                    }
                });
            });
            _ApplyTaskbarUnreadCount();
        }

        // Global-pause listener (DELIVERY.md §17): the "Pause Prompt Queue Runner" state is ONE
        // process-wide value owned by the shared scheduler, but every window draws its own button and
        // its own per-tab badges ("<current> (Off globally)"). The
        // scheduler calls this listener on each real change — whichever window's click made it, this one
        // included — on the CLICKING thread, so the listener only hops to this window's UI thread; the
        // hop carries no value on purpose (_ApplyGlobalPauseShown re-reads the scheduler, so hops that
        // land out of order cannot leave a stale label). No seed here: the Manager page is not built yet
        // at engine init, and _WireAgentManagerContent seeds its button when it is; no tab badge exists
        // yet either, and each one seeds itself when it is attached (_AttachClaudeOverlay). Detached in
        // ~TerminalPage (Rule #10).
        if (_scheduler)
        {
            const auto weakThis = get_weak();
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            _globalPauseObserverToken = _scheduler->AddGlobalPauseObserver([weakThis, dispatcher](bool /*paused*/) {
                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis]() {
                    // The page may be gone by the time the item runs (the window closed meanwhile): a dead
                    // weak reference simply ends it.
                    if (auto self = weakThis.get())
                    {
                        self->_ApplyGlobalPauseShown();
                    }
                });
            });
        }

        // Debounced autosave of the window record (750ms trailing): structural/lens churn (drag a
        // splitter, reorder tabs, resize the window) collapses to one write; never per keystroke.
        _saveWindowRecordThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 750 },
                .debounce = true,
                .trailing = true,
            },
            [weakThis = get_weak()]() {
                if (auto self = weakThis.get())
                {
                    self->_FlushWindowRecord();
                }
            });

        // Agentmaster (discard Manager-only windows): a debounced self-close check, run after every tab
        // add/remove + at the end of startup. Debounced (500ms < the 750ms record save) so a window
        // settling into Manager-only — a user closing/tearing-out its last terminal tab, or an
        // empty/failed reopen — is evaluated only once its tab set SETTLES (an async shell re-home is
        // never momentarily mistaken for empty), and so a burst of tab churn collapses to one decision.
        _managerOnlyCheckThrottled = std::make_shared<ThrottledFunc<>>(
            DispatcherQueue::GetForCurrentThread(),
            til::throttled_func_options{
                .delay = std::chrono::milliseconds{ 500 },
                .debounce = true,
                .trailing = true,
            },
            [weakThis = get_weak()]() {
                if (auto self = weakThis.get())
                {
                    self->_CloseWindowIfManagerOnly();
                }
            });

        // Adoption seam, PER WINDOW: a hook for a session we didn't Launch -> try to bind it to
        // its hosting ConPTY so it becomes fully managed (observe + control). The shared
        // registry fans the event out to EVERY window's handler; whichever window hosts the `+`
        // tab binds it, the rest no-op. Detached in ~TerminalPage so a closed window's handler
        // doesn't linger on the process-wide registry.
        {
            const auto weakThis = get_weak();
            _adoptionToken = _sessionRegistry->AddAdoptionHandler([weakThis](const std::wstring& id, const std::wstring& cwd, const std::wstring& tabToken) {
                if (auto self = weakThis.get())
                {
                    self->_AdoptExternalSession(winrt::hstring{ id }, winrt::hstring{ cwd }, winrt::hstring{ tabToken });
                }
            });

            // Agentmaster (tab status dot): live-update the tab strip's "[icon] ● <title>" dot on
            // registry changes — the same push that redraws the Manager board recolors the hosting
            // tab's dot (the Waiting->Idle cache decay rides this too, so the dot fades with the
            // card). Observers fire on arbitrary threads (bridge/scanner) -> bounce to this window's
            // dispatcher; the UI-thread reaction is one _claudeTabs lookup + a brush write, and
            // _SetTabAgentDot is idempotent on an unchanged color. The SAME hop also re-pins the
            // hosting tab's TITLE when the registry title changed (cross-window rename, Rule #11:
            // an Explorer-tree/board rename in ANOTHER window writes the shared registry; only the
            // window holding the tab can retitle it — _SyncClaudeTabTitleFromRegistry reads the
            // title FRESH and pins it through the latch, so the two sync directions converge and
            // never ping-pong). Token detached in ~TerminalPage (Rule #10 — a closed window's
            // observer must not linger on the registry).
            //
            // ⚠ COALESCED (the AgentManagerContent lens-observer discipline; 2026-07-25 release
            // freeze). The old shape enqueued ONE RunAsync PER NOTIFY, so a notify storm — a hook
            // burst across a big fleet, or a misbehaving engine loop (the SemiAuto arm spin: ~300
            // notifies/s sustained for minutes) — grew this window's dispatcher queue faster than
            // the UI thread drained it, and input starved for good ("half-frozen app"; the engine
            // threads kept running, which is exactly what the logs showed). Now the observer only
            // FOLDS the changed id into a shared pending set; a single Low-priority hop drains the
            // WHOLE set, reading each session FRESH from the registry on the UI thread (the same
            // no-stale-capture rule the title re-read has always followed — a batched dot can only
            // be MORE current than the snapshot the notify carried). A notify landing mid-drain
            // re-queues the next hop, so the trailing state is never lost; sub-hop state blips are
            // deliberately foldable (the flash/toast edge detectors already suppress exactly those
            // via ShouldSuppressAnswerBlipFlash / the toast HOLD + dedupe guards).
            const auto dispatcher = Dispatcher(); // agile — safe to call into from any thread
            struct AgentDotBatch
            {
                std::mutex mtx;
                std::unordered_set<std::wstring> ids; // sessions with an un-drained change
                bool hopQueued{ false }; // one drain hop in flight at a time
            };
            auto batch = std::make_shared<AgentDotBatch>();
            const auto registry = _sessionRegistry; // the drain re-reads FRESH state (never the notify snapshot)
            _agentDotObserverToken = _sessionRegistry->AddObserver([weakThis, dispatcher, batch, registry](const ::Agentmaster::SessionInfo& s, ::Agentmaster::HookEvent) {
                {
                    std::lock_guard lk{ batch->mtx };
                    batch->ids.insert(s.id);
                    if (batch->hopQueued)
                    {
                        return; // a drain hop is already queued — this change folds into it
                    }
                    batch->hopQueued = true;
                }
                try
                {
                    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low, [weakThis, batch, registry]() {
                        std::unordered_set<std::wstring> ids;
                        {
                            std::lock_guard lk{ batch->mtx };
                            ids.swap(batch->ids);
                            batch->hopQueued = false; // cleared WITH the swap: a notify after this queues the next hop
                        }
                        const auto self = weakThis.get();
                        if (!self)
                        {
                            return;
                        }
                        for (const auto& id : ids)
                        {
                            if (const auto cur = registry->Get(id))
                            {
                                // Agentmaster (eager-init): a live MANAGED session whose ConPTY hasn't
                                // started yet (a window-restored / re-homed tab the user never clicked)
                                // shows the half-hollow dot. An external (observe-only) session has no
                                // control of ours, so it is never "dormant".
                                const bool dormant = cur->live && !cur->started && !cur->external;
                                self->_UpdateTabAgentDot(id, cur->state, cur->live, dormant);
                            }
                            else
                            {
                                // Removed from the registry between the notify and this drain (the
                                // resume-fresh stale-drop). The per-notify shape delivered that final
                                // snapshot; a fresh read can only miss — so synthesize the !live update
                                // the flash/toast trackers key their per-session cleanup on.
                                self->_UpdateTabAgentDot(id, ::Agentmaster::SessionState::Idle, /*live*/ false, /*dormant*/ false);
                            }
                            self->_SyncClaudeTabTitleFromRegistry(id); // reads the CURRENT registry title (no stale capture)
                        }
                        // Side panel: the fleet changed — schedule its coalesced card rebuild. ONCE per
                        // drained batch (not per id): the rebuild reads the whole live snapshot anyway,
                        // and the panel throttles further inside (min-gap + collapsed-defers).
                        self->_SidePanelNoteRegistryChange();
                    });
                }
                catch (...)
                {
                    // Dispatcher gone (window tearing down) — never leave the drain latched off.
                    {
                        std::lock_guard lk{ batch->mtx };
                        batch->hopQueued = false;
                    }
                    ::Agentmaster::AgentLogCaughtException(L"agent tab-dot observer dispatch");
                }
            });
        }

        // Liveness probe, PER WINDOW: the shared scanner ticks this on its slow cadence; it
        // marshals to OUR UI thread and archives any of this window's claude tabs whose ConPTY
        // has Closed (a crash / `/exit` that fired no SessionEnd, or a clean SessionEnd that only
        // set Done). Fans out to every window like adoption; each window sweeps only its own tabs.
        if (_scanner)
        {
            const auto weakThis = get_weak();
            _livenessToken = _scanner->AddLivenessProbe([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ReconcileClaudeTabs(); // bind/attach + re-home hooked sessions (tabToken)
                    self->_ObserverProbe(); // Fleet Observer: publish this window's roster + bind via the correlation table (PULL; no hooks needed)
                    self->_SweepClaudeLiveness(); // then archive dead tabs (all self-marshal to the UI thread)
                    self->_ScanPendingInput(); // PENDING_INPUT.md: record each live Claude tab's unsent input-box draft
                    self->_ScanCacheWarmTabs(); // SPARK CROWN: re-evaluate ServerCacheStillWarm per hosted session -> the tab-strip glow + rising embers (polled: warmth ENDS on a clock, nothing pushes it)
                    self->_PumpHandoverInjections(); // COMMANDS.md §5: paste-inject a spawned successor's FULL over-budget handover document once its claude starts
                    self->_SweepHandoverDeletes(); // COMMANDS.md §6b: delete a handed-over md once its successor actually STARTED (opt-in; keeps the file if the successor never comes up)
                    self->_PumpDraftRestores(); // PENDING_INPUT.md §10: type a reopened session's remembered unsent draft back into its input box once its claude starts (fill, never submit)
                    self->_SweepAgentPendingToasts(); // System notifications: fire/drop the HELD completion toasts (spurious-toast suppression — SessionScanner's DecideHeldToast)
                    self->_ScanInferredTabColors(); // tab color modes: re-infer each Claude tab's ACTUAL workdir + recolor (every session under InferredWorkingDirectory; ONLY home-dir launches — forced inference, SessionInfersWorkingDir — in the other modes; throttled + mtime-gated inside)
                    self->_SidePanelPeriodicTick(); // SIDE PANEL: deferred-rebuild catch-up + the summary's mtime-gated re-check + the ~30s in-place timing refresh (self-marshals)
                }
            });
        }

        // M10 autosave triggers (PERSISTENCE.md §13): structural changes (tab add/remove/reorder)
        // and window resize debounce a window-record save. Lens changes come through the content's
        // push (SetLensChangedHandler in _WireAgentManagerContent); tab recolor through
        // _OnClaudeTabColorChanged. _ScheduleWindowRecordSave no-ops until startup completes, so
        // the tabs added during _OnFirstLayout don't thrash saves.
        _tabs.VectorChanged([weakThis = get_weak()](auto&&, auto&&) {
            if (auto self = weakThis.get())
            {
                self->_ScheduleWindowRecordSave();
                // Agentmaster (discard Manager-only windows): a tab add/remove may have left this window
                // holding no tab at all, just the Manager page (its last tab closed or torn out) — schedule the
                // debounced check that self-closes / un-persists such a window once its tab set settles.
                self->_ScheduleManagerOnlyCheck();
            }
        });
        if (_tabContent)
        {
            _tabContent.SizeChanged([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->_ScheduleWindowRecordSave();
                }
            });
        }

        // Agentmaster (alt+up/down prompt nav, SUMMARY_JUMP.md §7): a free-running 30 s refresh that keeps
        // the FOCUSED Claude session's jump data in sync with the live buffer between keypresses — re-reads
        // its sent prompts (mtime-gated) into _promptNavCache + re-resolves the summary panel's jump-icon
        // eligibility. Each tick no-ops unless a managed Claude tab is focused. Self-stops if the page is
        // gone (weak); also stopped in ~TerminalPage. (_InitAgentmasterEngine runs once per window — the
        // `_sessionRegistry` guard at the top — so this builds + starts exactly one timer.)
        _promptNavRefreshTimer = DispatcherTimer{};
        _promptNavRefreshTimer.Interval(std::chrono::seconds{ 30 });
        _promptNavRefreshTimer.Tick([weakThis = get_weak()](const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::IInspectable&) {
            if (auto self = weakThis.get())
            {
                if (const auto sid = self->_FocusedPromptNavSession(); !sid.empty())
                {
                    self->_RefreshPromptNavCache(sid);
                }
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop(); // page destroyed — stop ticking (UI thread, safe)
            }
        });
        _promptNavRefreshTimer.Start();
    }

    // Agentmaster: wire a freshly-created Manager content to the engine. Idempotently
    // ensures the engine exists, then hands the content the registry + the spawn / activate
    // / kill callbacks (all routed back through the page on the UI thread).
    void TerminalPage::_WireAgentManagerContent(const winrt::com_ptr<AgentManagerContent>& content)
    {
        if (!content)
        {
            return;
        }
        _InitAgentmasterEngine();
        content->SetRegistry(_sessionRegistry);
        // Agentmaster (O6): remember the content (weak, as its projected IPaneContent) so
        // _ObserverProbe can push the observer's External (WindowsTerminal) census to it each tick.
        _agentManagerContent = winrt::make_weak(content.as<winrt::TerminalApp::IPaneContent>());

        const auto weakThis = get_weak();
        content->SetSpawnHandler([weakThis](winrt::hstring dir, winrt::hstring title, winrt::hstring model) {
            if (auto self = weakThis.get())
            {
                // model rides the launch-model picker ("Open New Session Here ▸ <model>"); "" = Default.
                self->_SpawnClaudeSession(dir, title, static_cast<uint32_t>(-1), model);
            }
        });
        content->SetActivateHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ActivateClaudeSession(id);
            }
        });
        // Agentmaster (eager-init): "Activate Tab" — start a dormant session's claude IN PLACE (no focus
        // change). LOCAL only: the content offers the item solely for a session this window hosts (a
        // single-session start can only target the window owning that control). A no-op if it isn't ours
        // (e.g. it started since the menu was built); a remote dormant session is woken via "Jump to Tab"
        // or "Activate All Tabs".
        content->SetActivateDormantHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ActivateDormantSession(std::wstring{ id });
            }
        });
        // Agentmaster (eager-init): "Activate All Tabs" — wake this window's dormant tabs; if allWindows,
        // fan out to every other window too (each eager-inits its own). The content decides the scope
        // (prompting only when other windows also have dormant tabs).
        content->SetActivateAllHandler([weakThis](bool allWindows) {
            if (auto self = weakThis.get())
            {
                self->_ActivateAllDormantTabsLocal();
                if (allWindows)
                {
                    ::Agentmaster::ActivateAllDormantInOtherWindows(self->_windowId);
                }
            }
        });
        // Agentmaster (Linked Lenses): the Manager reports a pointer enter/leave on a managed
        // card/row (id, entering). Track the effective hovered session here — a leave only clears
        // when it's still the hovered id, which absorbs the enter-B-before-leave-A ordering when the
        // pointer slides between rows — then re-evaluate the per-tab "selected/active" pill (a live
        // preview that follows the mouse while the Manager page is on screen).
        content->SetHoverSessionHandler([weakThis](winrt::hstring id, bool entering) {
            if (auto self = weakThis.get())
            {
                const std::wstring sid{ id };
                if (entering)
                {
                    self->_managerHoverSessionId = sid;
                }
                else if (self->_managerHoverSessionId == sid)
                {
                    self->_managerHoverSessionId.clear();
                }
                self->_UpdateManagerSelectionHighlight();
            }
        });
        // Agentmaster (tab-strip reveal): a board card / tree row single-click -> scroll that session's
        // tab into view, even when the click did not change the selection (the re-click the lens push
        // can't express). _RevealClickedSessionTab applies the Manager-active + hosted-here conditions.
        content->SetRevealSessionTabHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_RevealClickedSessionTab(std::wstring{ id });
            }
        });
        // Agentmaster (FAVORITES.md): the board/tree "Close" verb (formerly Archive) — shut the session
        // down but KEEP the record (always archived), so it stays resumable in the Sessions browser.
        content->SetArchiveHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_ArchiveClaudeSession(id);
            }
        });
        // Agentmaster (board/tree session menu — "Close ▸ Of Same Folder" / "Other of Same Folder"): close
        // every live managed session whose effective work dir matches the folder, skipping excludeId when
        // non-empty ("Other …" keeps the clicked session). Routes to the SHARED confirm+close entry
        // _ConfirmAndCloseClaudeSessionsInFolder (the same one the WT tab-strip "Close ▸" submenu uses), so
        // the page shows the ONE confirm listing the titles, then does the cross-window batch close.
        content->SetCloseFolderHandler([weakThis](winrt::hstring folder, winrt::hstring excludeId) {
            if (auto self = weakThis.get())
            {
                self->_ConfirmAndCloseClaudeSessionsInFolder(folder, excludeId);
            }
        });
        // Agentmaster (session menu "Deactivate ▸" / "Close ▸" — the board card / tree row / side-panel card twins of the WT
        // tab menu's two submenus): every item but Close's two folder ones (SetCloseFolderHandler above) runs as the
        // session's own tab menu would, in the window that hosts its tab (_RunSessionMenuScope: here, or handed to that
        // window). The content's scope values ARE the tab menu's codes — asserted here, so the two menus can never name
        // different tabs for one item — and FavoriteAllTabs is the page's one extra code.
        using SessionMenuScope = implementation::AgentManagerContent::SessionMenuScope;
        using SessionMenuVerb = implementation::AgentManagerContent::SessionMenuVerb;
        static_assert(static_cast<int32_t>(SessionMenuScope::ThisTab) == implementation::Tab::kDeactivateScopeThisTab, "the session menu's ThisTab must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::OtherTabs) == implementation::Tab::kDeactivateScopeOtherTabs, "the session menu's OtherTabs must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::AllTabs) == implementation::Tab::kDeactivateScopeAllTabs, "the session menu's AllTabs must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::TabsBefore) == implementation::Tab::kDeactivateScopeTabsBefore, "the session menu's TabsBefore must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::TabsAfter) == implementation::Tab::kDeactivateScopeTabsAfter, "the session menu's TabsAfter must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::SameFolder) == implementation::Tab::kDeactivateScopeSameFolder, "the session menu's SameFolder must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::OtherSameFolder) == implementation::Tab::kDeactivateScopeOtherSameFolder, "the session menu's OtherSameFolder must be the tab menu's code");
        static_assert(static_cast<int32_t>(SessionMenuScope::FavoriteAllTabs) == kSessionMenuScopeFavoriteAllTabs, "the session menu's FavoriteAllTabs must be the page's code");
        content->SetSessionMenuScopeHandler([weakThis](winrt::hstring id, SessionMenuVerb verb, SessionMenuScope scope) {
            if (auto self = weakThis.get())
            {
                self->_RunSessionMenuScope(std::wstring{ id }, verb == SessionMenuVerb::Close, static_cast<int32_t>(scope));
            }
        });
        // Where a session's tab sits in THIS window's strip (nullopt: another window hosts it, or no tab yet) — the content
        // grays the menu's strip items from it exactly as the tab menu grays its own (Tab::_EnableMenuItems). Read on the UI
        // thread each time a session menu opens; _claudeTabs + the strip are this window's own. A dead page answers nullopt.
        content->SetSessionTabSpotProvider([weakThis](const std::wstring& id) -> std::optional<implementation::AgentManagerContent::SessionTabSpot> {
            const auto self = weakThis.get();
            if (!self)
            {
                return std::nullopt;
            }
            const auto it = self->_claudeTabs.find(id);
            const auto tab = (it != self->_claudeTabs.end()) ? it->second.get() : nullptr;
            uint32_t index = 0;
            if (!tab || !self->_tabs.IndexOf(tab, index))
            {
                return std::nullopt; // not hosted here, or the tab is mid-move and out of the strip this instant
            }
            return implementation::AgentManagerContent::SessionTabSpot{ index, self->_tabs.Size() };
        });
        content->SetRestoreHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_RestoreArchivedSession(id);
            }
        });
        // Agentmaster: the Launch box accepted a FOUND session id. The content already resolved its
        // (dir, title), so reuse the same on-disk seams the Sessions page uses — resume (live -> Jump,
        // archived/unknown -> minimal record + claude --resume) and fork (claude --resume --fork-session).
        content->SetResumeSessionHandler([weakThis](winrt::hstring id, winrt::hstring dir, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_ResumeSessionFromDisk(std::wstring{ id }, std::wstring{ dir }, std::wstring{ title });
            }
        });
        content->SetForkSessionHandler([weakThis](winrt::hstring id, winrt::hstring dir, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                // HOOKS.md "Interrupted turns": asked first when the session is in the middle of a turn.
                ForkLaunchRequest request;
                request.sourceId = std::wstring{ id };
                request.dir = std::wstring{ dir };
                request.title = std::wstring{ title };
                request.origin = L"launch-bar";
                self->_ForkAskingAboutRunningTurn(std::move(request));
            }
        });
        // Agentmaster: the Triage Board / Explorer-tree session menu's "Restart session" — restart a
        // managed session's live connection in place (resume the current conversation; never replay the
        // launch commandline). Local-first, then fan out to the hosting window (the board GLOBAL scope
        // shows the whole fleet; the live pane lives in exactly one window).
        content->SetRestartSessionHandler([weakThis](winrt::hstring id) {
            if (auto self = weakThis.get())
            {
                self->_RestartClaudeSession(id);
            }
        });
        // Agentmaster: the Triage Board / Explorer-tree session menu's "Fork session" — kind-aware fork
        // (Claude --fork-session / Codex `codex fork`), the same path the WT tab's "Fork session" uses,
        // opening the fork tab in THIS window (it reads the shared registry; no live tab needed).
        // model rides the launch-model picker's "Fork session ▸ <model>" submenu ("" = Default).
        content->SetForkManagedSessionHandler([weakThis](winrt::hstring id, winrt::hstring model) {
            if (auto self = weakThis.get())
            {
                // HOOKS.md "Interrupted turns": asked first when the session is in the middle of a turn.
                ForkLaunchRequest request;
                request.sourceId = std::wstring{ id };
                request.managed = true;
                request.model = std::wstring{ model };
                request.origin = L"board";
                self->_ForkAskingAboutRunningTurn(std::move(request));
            }
        });
        content->SetRenameHandler([weakThis](winrt::hstring id, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_RenameClaudeSession(id, title);
            }
        });
        // Agentmaster (bookmark tags): the board-card / tree-row "Tags" item — open the page's tag
        // editor panel for that session, anchored under the clicked card/row (the Manager content
        // lives inside this page's visual tree, so the anchor transforms into Root() space).
        content->SetTagsHandler([weakThis](winrt::hstring id, winrt::Windows::UI::Xaml::FrameworkElement anchor) {
            if (auto self = weakThis.get())
            {
                self->_OpenTagEditorForElement(std::wstring{ id }, anchor);
            }
        });
        // Agentmaster (transcript overlay): the session menu's "Show Transcript" / "Copy ▸ Transcript ▸ Show"
        // (board card, tree row, side-panel card) and the EXTERNAL menu's "Show Transcript" — open that
        // conversation in this window's reading view, over the Agent Manager page the click came from.
        content->SetShowTranscriptHandler([weakThis](winrt::hstring id, bool codex, winrt::hstring codexRolloutId, winrt::hstring title) {
            if (auto self = weakThis.get())
            {
                self->_ShowTranscriptOverlay(std::wstring{ id }, codex, std::wstring{ codexRolloutId }, std::wstring{ title }, L"session menu");
            }
        });
        // Agentmaster (bookmark tags): a Triage-Board card ribbon's pointer enter/leave — route to
        // the SAME rich tag hover panel the tab badges + the Sessions Tags column use (every session
        // carrying the tag + its status dot; clicking a live row jumps to its tab). The Manager
        // content lives inside this page's visual tree, so the ribbon anchors into Root() space
        // like any main-tree badge.
        content->SetTagHoverHandlers(
            [weakThis](winrt::hstring tag, winrt::Windows::UI::Xaml::UIElement anchor) {
                if (auto self = weakThis.get())
                {
                    self->_OnTagBadgeHoverBegin(tag, anchor);
                }
            },
            [weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_OnTagBadgeHoverEnd();
                }
            });
        // Agentmaster: adopt an EXTERNAL (observe-only) claude from the Explorer Tree's EXTERNAL
        // scope — bring its conversation under management. `fork` (chosen in the Manager's Adopt
        // dialog) selects the two-writers-safe branch (--fork-session into a NEW transcript) vs. a
        // straight --resume of the same conversation (true take-over; the user stops the original).
        content->SetAdoptExternalHandler([weakThis](uint32_t pid, winrt::hstring cwd, bool fork) {
            if (auto self = weakThis.get())
            {
                self->_AdoptExternalClaude(pid, cwd, fork);
            }
        });
        // Agentmaster (Codex-launch): the EXTERNAL-codex menu — Adopt (bring its rollout under
        // management: `fork` => `codex fork` into a NEW rollout [safe on a live codex], else
        // `codex resume` the same) or Open New Codex Session Here (a fresh managed codex in the cwd,
        // adopt==false). Lifecycle + state only.
        content->SetCodexLaunchHandler([weakThis](uint32_t pid, winrt::hstring cwd, bool adopt, bool fork) {
            if (auto self = weakThis.get())
            {
                if (adopt)
                {
                    self->_AdoptExternalCodex(pid, cwd, fork);
                }
                else
                {
                    self->_SpawnCodexSession(cwd, winrt::hstring{});
                }
            }
        });
        // Agentmaster: the Explorer Tree's refresh button — force the Fleet Observer to re-survey NOW
        // (re-enrich the registry + recompute the External census) and redraw, instead of waiting for
        // the next observer/scanner tick. Covers every scope (LOCAL/GLOBAL re-pull + EXTERNAL census).
        content->SetRefreshHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_RefreshObserverData();
            }
        });
        // Agentmaster (PENDING_INPUT.md §8): the session menu's "Copy Current Prompt" reads the UNSENT
        // draft LIVE off the session's buffer. Only the hosting window can do that, and the board/tree
        // list the WHOLE fleet — _ReadLiveDraftForSession answers "" for anything not readable HERE
        // (another window's tab, a dormant one, a torn-down control), which makes the copy fall back to
        // the observer's recorded draft. Read-only, on-demand (per click), UI thread.
        content->SetLiveDraftProvider([weakThis](const std::wstring& sessionId) -> std::wstring {
            auto self = weakThis.get();
            return self ? self->_ReadLiveDraftForSession(sessionId) : std::wstring{};
        });
        // Agentmaster: surface THIS window's hosted session ids for the Explorer Tree's LOCAL
        // scope. The shared (process-wide) registry holds every window's sessions; _claudeTabs is
        // the per-window subset. The set comes from _HostedSessionIds — the ONE "hosted here"
        // predicate (expired weak tabs skipped, so it is live) that the SIDE PANEL's LOCAL card list
        // reads too, so the tree's LOCAL and the panel's LOCAL can never list different sessions.
        content->SetLocalScopeProvider([weakThis]() -> std::unordered_set<std::wstring> {
            if (auto self = weakThis.get())
            {
                return self->_HostedSessionIds();
            }
            return {}; // page gone — an empty set reads "nothing hosted here", the safe answer
        });
        // Agentmaster (focus-steal fix): tell the content whether THIS window is the OS foreground
        // window. The Manager's _Refresh restores keyboard focus to a board card after each rebuild,
        // and in XAML Islands Control.Focus() escalates to Win32 activation of the island's host
        // window — so a refresh firing on a BACKGROUND Manager window (the Triage Board sitting behind
        // a Claude tab you're working in, in ANOTHER window) would steal the OS foreground on every
        // ~2s observer/registry tick. Comparing GetForegroundWindow() to our HWND (the same real-time
        // test _FocusClaudeSessionTab uses) gates the restore to the foreground case only.
        content->SetWindowForegroundProvider([weakThis]() -> bool {
            auto self = weakThis.get();
            return self && self->_hostingHwnd && ::GetForegroundWindow() == *self->_hostingHwnd;
        });
        // The toolbar's "Pause / Resume Prompt Queue Runner" button (the GLOBAL PAUSE — DELIVERY.md §17).
        // The click only REQUESTS the new state: the shared scheduler owns it, and the button of every
        // window — this one too — is repainted by the global-pause listener that engine init registered
        // (_ApplyGlobalPauseShown), never by the click itself. That is what keeps two windows from showing
        // different states. `paused` is the state the user asked for (true == hold every automatic send).
        // The lambda runs inside a XAML Click handler, where an escaping exception is fatal to the process,
        // and SetGlobalPause can throw (std::bad_alloc while it copies the live sessions on a resume), so
        // the call is contained (Rule #18): the pause state then simply stays what it was.
        content->SetPauseHandler([weakThis](bool paused) {
            if (auto self = weakThis.get())
            {
                if (self->_scheduler)
                {
                    try
                    {
                        self->_scheduler->SetGlobalPause(paused);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"pause handler (Scheduler::SetGlobalPause)");
                    }
                }
            }
        });
        // Seed the button from the scheduler: a window opened while the runner is already paused (the
        // pause was set from another window) must not read "Pause". The listener covers every later change.
        content->SetGlobalPauseShown(_scheduler && _scheduler->GlobalPaused());
        // Settings cog: seed the dialog with the loaded settings, and persist + apply on Save.
        content->SetSettings(_appSettings);
        content->SetSettingsHandler([weakThis](::Agentmaster::AppSettings s) {
            if (auto self = weakThis.get())
            {
                // hiddenSessionIds (the Sessions browser's "Hide from list" set) AND showSummaryPanel
                // (the per-tab summary-panel pencil) are owned by UI actions OUTSIDE the cog form, each a
                // freshest-disk RMW. The cog FORM never edits them, so preserve the on-disk values here so
                // a form Save can't regress a hide/reset/pencil-toggle done since the modal was seeded
                // (incl. by another window).
                {
                    const auto disk = ::Agentmaster::LoadAppSettings();
                    s.hiddenSessionIds = disk.hiddenSessionIds;
                    s.showSummaryPanel = disk.showSummaryPanel;
                    s.summaryPanelWrapNewlines = disk.summaryPanelWrapNewlines; // wrap-line toggle (panel times bar), out-of-cog UI action
                    s.summaryPanelTruncate = disk.summaryPanelTruncate; // truncate toggle (panel times bar), out-of-cog UI action
                    s.summaryPanelShowHistory = disk.summaryPanelShowHistory; // ⏮ whole-conversation toggle (panel times bar), out-of-cog UI action
                    // Tab color picker: the Custom / advanced (More) expansion states are written by
                    // the ColorPickupFlyout itself as the user toggles them (freshest-disk RMW), never
                    // by this form — preserve them so a cog Save can't reset how the picker opens.
                    s.tabColorPickerCustomOpen = disk.tabColorPickerCustomOpen;
                    s.tabColorPickerAdvancedOpen = disk.tabColorPickerAdvancedOpen;
                    // The transcript overlay's remembered level and its ↵ line-wrap toggle: written by the
                    // overlay itself on every tab pick / toggle click (freshest-disk RMW), never by this
                    // form — a Save must not reset them.
                    s.transcriptOverlayLevel = disk.transcriptOverlayLevel;
                    s.transcriptOverlayWrapLines = disk.transcriptOverlayWrapLines;
                    // The Sessions AI search's thinking level: written by the page's strip toggle
                    // (freshest-disk RMW), never by this form — a Save must not reset it.
                    s.sessionsAiThinkingLevel = disk.sessionsAiThinkingLevel;
                    // Updater (Updater.h): ALL update state is written outside the cog form —
                    // skip/postpone by the prompt's JSON RMW (possibly from another window or the
                    // startup check), the prerelease + nightly opt-ins by each switch's own
                    // INSTANT-APPLY RMW — so preserve all four from disk on Save (a stale form copy
                    // must never regress a flip/choice made since the modal was seeded).
                    s.allowUpdatePrerelease = disk.allowUpdatePrerelease;
                    s.allowUpdateNightly = disk.allowUpdateNightly;
                    s.updateSkippedVersion = disk.updateSkippedVersion;
                    s.updatePostponedUntilUnixMs = disk.updatePostponedUntilUnixMs;
                    // Slash commands (COMMANDS.md §6a): the MATERIALIZED-name markers are engine
                    // reality (which name's definition file the last init wrote), RMW'd at engine
                    // init — never form state. Preserve them like the seed markers so a cog Save
                    // can't rewrite what is actually on disk (the next init's migration relies
                    // on them to know which old file to clean up).
                    s.commandHandoverMaterializedName = disk.commandHandoverMaterializedName;
                    s.commandHandoverHereMaterializedName = disk.commandHandoverHereMaterializedName;
                    s.commandHandoverStandbyMaterializedName = disk.commandHandoverStandbyMaterializedName; // the §5b standby member's marker
                    // Side panel: all five are written OUTSIDE the cog form — the splitters' /
                    // collapse's / "Sum" toggle's / dock flip's own freshest-disk RMWs ("last one to
                    // adjust sets the new-window default") — so a form Save must never regress an
                    // adjustment made since the modal was seeded (incl. from another window).
                    s.sidePanelOnLeft = disk.sidePanelOnLeft;
                    s.sidePanelWidthFraction = disk.sidePanelWidthFraction;
                    s.sidePanelSplitFraction = disk.sidePanelSplitFraction;
                    s.sidePanelCollapsed = disk.sidePanelCollapsed;
                    s.sidePanelSummaryHidden = disk.sidePanelSummaryHidden; // the header "Sum" toggle — written by its own RMW, never by this form
                    // ...and the header's ⌖ scroll-to-tab toggle, written by its own RMW. It matters most
                    // HERE: the board-sort cycle reaches this sink too, carrying the Manager content's
                    // _appSettings, which never learns of THIS window's own flip (the toggle lives on the
                    // page; the content only hears other windows' broadcasts) — without this line, the next
                    // sort click in this window would silently turn the toggle back.
                    s.sidePanelScrollToSelectedTab = disk.sidePanelScrollToSelectedTab;

                }
                // Inferred-workdir inputs (mode / "Use .git folder to infer") — capture the change
                // BEFORE adopting `s`, for the scan-state reset below.
                const bool inferInputsChanged = self->_appSettings.tabColorMode != s.tabColorMode ||
                                                self->_appSettings.inferGitRoot != s.inferGitRoot;
                // RENDERING.md: likewise for the software-rendering switch — re-point the terminals only on
                // a real flip (this sink also carries the sort toggles' unrelated saves).
                const bool softwareRenderingChanged = self->_appSettings.softwareRendering != s.softwareRendering;
                // RENDERING.md §4c: the hidden-tab policy is compared the same way (and through the ONE
                // combiner, so "release off" vs "pause off" changes are both seen).
                const bool hiddenRenderingChanged = self->_appSettings.pauseHiddenTerminals != s.pauseHiddenTerminals ||
                                                    ::Agentmaster::HiddenTerminalReleaseDelayMs(self->_appSettings) != ::Agentmaster::HiddenTerminalReleaseDelayMs(s);
                self->_appSettings = s;
                ::Agentmaster::SaveAppSettings(s);
                // Adopt/re-home autorunner default: the registry stamps this MODE onto records it
                // creates ITSELF (the hook-adopt + observer first-sight creates — the two open-seams
                // outside TerminalPage's launch/restore stamping). Engine init seeds it from disk;
                // keep it live with the cog here. ONE shared engine per process, so no broadcast leg
                // is needed. In EVERY build since 2026-10-09: the Prompt Queue Runner left the
                // dev-or-debug gate, so an ordinary release mints its records with the cog's default
                // mode too (until then it kept minting Off records for a scheduler that never ran).
                if (self->_sessionRegistry)
                {
                    self->_sessionRegistry->SetDefaultAutorunnerMode(s.defaultAutorunnerMode);
                }
                // Apply the (possibly changed) GLOBAL rename-commit mode to every window's tab
                // headers immediately (process-wide static), not just next launch.
                SetTabRenameCommitMode(static_cast<int32_t>(s.tabRenameCommitMode));
                // Apply the (possibly changed) tab-strip close affordances (show-X / middle-click
                // close) to THIS window's tabs immediately; other windows get them via the broadcast.
                self->_updateAllTabCloseButtons();
                // Apply the (possibly changed) "Show icons on tabs" setting to THIS window's tabs now
                // (show/hide the profile icon on every tab); other windows get it via the broadcast.
                self->_UpdateAllTabIcons();
                // Agentmaster (tab sizing): apply the (possibly changed) TAB SIZE settings — width mode, width
                // bounds, height, title font — to THIS window's strip now; other windows via the broadcast.
                self->_ApplyTabSizingSettings(true);
                // (The tab-strip Home button no longer has a setting: it is the Agent Manager's only entry
                // point, so it is always shown — the retired "Always display Home button" cog toggle's key
                // still round-trips in settings.json but nothing reads it.)
                // FAVORITES.md §5a: the favorite marker (Crown <-> Star) is GLOBAL — re-assert it on every
                // hosted favorited tab so a glyph change takes effect now; other windows get it via the broadcast.
                self->_RefreshAllFavoriteIcons();
                // Status-dot flash-ring color is GLOBAL — re-point this window's shared flash-ring brush at the
                // (possibly changed) color/opacity so any currently-flashing tab recolors live; other windows
                // pick it up via the broadcast below.
                self->_RefreshFlashRingBrush();
                // Taskbar unread count (NOTIFICATIONS.md section 8): the switch is GLOBAL — turning it off clears this
                // window's overlay now (on repaints the census's current count); other windows follow via the broadcast.
                self->_ApplyTaskbarUnreadCount();
                // Taskbar preview frame (NOTIFICATIONS.md section 9): its switch and its color (the flash-ring color,
                // re-read just above) are GLOBAL — unframe / reframe / recolor this window's preview now; other windows
                // follow via the broadcast.
                self->_ApplyTaskbarUnreadFrame();
                // Per-tab overlay rest/hover opacities are GLOBAL (TAB_OVERLAY.md) — push the (possibly
                // changed) pair to every hosted overlay so the badge/summary dim<->bright updates live; other
                // windows pick it up via the broadcast below.
                self->_RefreshOverlayOpacities();
                // Tab color mode is GLOBAL — repaint THIS window's managed tabs per the (possibly
                // changed) mode now (per-dir / individual / inferred); other windows repaint via the
                // broadcast below, and the board/chips re-resolve on their next rebuild.
                self->_ReapplyManagedTabColors();
                // Inferred-workdir inputs changed (the mode, or "Use .git folder to infer"): drop the
                // per-session scan gates (lastMtime/throttle) so every hosted session RE-INFERS under
                // the new rule now — a quiet transcript would otherwise keep its old inference until
                // its next write — and kick one scan pass immediately (it self-gates per session:
                // SessionInfersWorkingDir admits the whole fleet under the Inferred mode, home-dir
                // launches in every mode).
                if (inferInputsChanged)
                {
                    self->_inferredColorScan.clear();
                    self->_ScanInferredTabColors();
                }
                // RENDERING.md: GPU <-> CPU drawing is GLOBAL — set the process-wide override and switch
                // THIS window's terminals now; other windows switch theirs via the broadcast below.
                if (softwareRenderingChanged)
                {
                    self->_ApplySoftwareRenderingSetting(s.softwareRendering, L"cog");
                }
                // RENDERING.md §4c: the hidden-tab policy is GLOBAL too — publish it and re-apply it to
                // THIS window's terminals now (an already-hidden terminal adopts a new delay at once);
                // other windows do theirs via the broadcast below.
                if (hiddenRenderingChanged)
                {
                    self->_ApplyHiddenRenderingSetting(L"cog");
                }
                // Waiting-for-you "unread" model: push the (possibly changed) WaitingForInput -> Idle
                // timeout to the process-wide scanner so it applies immediately, not next launch.
                if (self->_scanner)
                {
                    self->_scanner->SetWaitingDecayMinutes(s.waitingForYouTimeoutMinutes);
                }
                // OBSERVER.md §13a: push the (possibly changed) orphan-reaper switch to the process-wide
                // Fleet Observer so it applies on its next survey, not next launch (one engine per process).
                if (self->_observer)
                {
                    self->_observer->SetReapOrphanedSessions(s.reapOrphanedSessions);
                }
                // SESSIONS_DB.md §7.6: the session database's mode and refresh interval apply at once (Off stops
                // the refresh tick; Shadow / On start it; the service itself exits 10 minutes after its last client).
                if (const auto& db = ::Agentmaster::SharedEngine().sessionDb)
                {
                    db->Apply(s);
                }
                // MESSAGING.md: the Telegram bot's switch, paired owner, idle grace and Search thinking level apply at
                // once (the token was stored in Credential Manager by the cog before it called this handler).
                if (const auto& msg = ::Agentmaster::SharedEngine().messaging)
                {
                    msg->Apply(s);
                }
                // Side panel: adopt the (possibly changed) globals — the shared boardSort label,
                // the dock side, the summary content flags (its fill-hosted overlay sits outside
                // the _claudeOverlays broadcast loops) — and rebuild the cards under the new rules.
                self->_SidePanelApplySettings();
                // Re-materialize the shared --settings file so model / co-authored-by /
                // permission-mode changes also reach an adopted hand-typed `claude` (the PATH
                // shim points at this file). Fresh spawns rebuild it from settings regardless.
                // The resolved claude.exe keeps the hook invocation right (HOOKS.md "Hook invocation"):
                // without it this write would drop the file back to the slow shell form (Git Bash +
                // BASH_ENV per hook) until the next spawn. UI thread: claudeExePath's owning thread.
                try
                {
                    ::Agentmaster::MaterializeSharedHookFiles(::Agentmaster::AgentmasterStateDir(), s, ::Agentmaster::SharedEngine().claudeExePath);
                }
                CATCH_LOG();
                // Cross-window settings broadcast: live-propagate the merged settings to every OTHER open
                // window, so the Explorer-Tree / Triage-Board sort (and the cog's globals) sync immediately
                // instead of only on each window's next launch. The source window already applied it (its
                // own toggle/cog re-rendered locally + self->_appSettings above), so the broadcast excludes
                // _windowId; each other window's sink marshals onto its UI thread and calls _ApplyBroadcastSettings.
                ::Agentmaster::BroadcastSettingsChanged(s, self->_windowId);
            }
        });

        // M10 Increment 3 (PERSISTENCE.md §13.5): the Manager's "Reopen Windows (N)" recover button —
        // reopen saved windows that aren't currently open, the runtime analog of the Emperor's startup
        // reopen loop. Dispatched on the page (it owns the wt-exe new-window path).
        content->SetReopenWindowsHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ReopenSavedWindows();
            }
        });
        // Agentmaster (Sessions page; SESSIONS.md): the Manager's "Sessions" button opens the
        // full-window browser over EVERY on-disk Claude Code session — the sole history view now that
        // the separate Archive page is gone (FAVORITES.md).
        content->SetOpenSessionsHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ShowSessionsPage();
            }
        });
        // Agentmaster (Sessions page; SESSIONS.md): the Settings cog's "Reset hidden sessions"
        // clears the user's "Hide from list" set (owned by the page, not the cog form) and re-shows
        // every hidden session in the Sessions browser.
        content->SetResetHiddenSessionsHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_ResetHiddenSessions();
            }
        });
        // Agentmaster (updater; Updater.h): the cog's "Update now" launches the embedded am-update
        // installer detached, then closes the app (no-confirm) so the package isn't in use while the
        // installer Add-AppxPackages the new build and relaunches it. NOT RequestQuit — that pops WT's
        // generic "close all tabs?" confirmation, which is redundant after the update dialog and whose
        // Cancel would strand the app for the installer to force-kill 20s later.
        content->SetQuitForUpdateHandler([weakThis]() {
            if (auto self = weakThis.get())
            {
                self->_QuitForUpdate();
            }
        });

        // M10 (PERSISTENCE.md §13): seed this window's Manager lens from its claimed record
        // (selection / scope / collapsed dirs / splitter sizes survive close/reopen), and have the
        // content PUSH lens changes back so the page caches the current lens and debounce-saves the
        // window record. The push carries the lens payload, so _CaptureWindowRecord never has to
        // reach back into the tab to pull it. Only a CLAIMED record seeds — a fresh window keeps the
        // content's ctor-loaded global splitter sizes (no reset-to-default on every new window).
        if (_windowRecordClaimed)
        {
            content->SetManagerState(_windowRecord.manager); // content now matches the cache
        }
        else
        {
            // Fresh window: prime the cache FROM the content's actual lens (incl. its ctor-loaded
            // global splitter sizes) so a save triggered before the first lens push (e.g. a resize)
            // persists the real layout, not a stale default — which would reset splitters on reopen.
            _windowRecord.manager = content->GetManagerState();
        }
        content->SetLensChangedHandler([weakThis](::Agentmaster::ManagerState st) {
            if (auto self = weakThis.get())
            {
                self->_windowRecord.manager = std::move(st);
                self->_ScheduleWindowRecordSave();
                // Linked Lenses: a selection change is one of the lens mutations — re-pill the
                // selected session's tab (when nothing is hovered, the pill tracks the selection).
                self->_UpdateManagerSelectionHighlight();
                // Agentmaster: the Home button's jump-back face targets the selected session, so a
                // selection change while the Manager page is up flips whether it has somewhere to jump
                // back to (arrow vs Home) and which session its tooltip names.
                self->_UpdateManagerNavButtons();
                // Side panel: follow the lens selection (a board-card/tree click selects the same
                // session's card + summary there). driveManagerLens=false — the lens IS the
                // source; the panel's same-id early-out absorbs the echo of its own card clicks.
                self->_SidePanelSelectSession(self->_windowRecord.manager.selectedId, /*driveManagerLens*/ false);
            }
        });
    }

    // Agentmaster (updater; Updater.h): quit the app for an in-app update — the post-confirm half of
    // RequestQuit (flush this window's record + raise QuitRequested) WITHOUT the "close all tabs?"
    // confirmation. Fired by the cog's "Update now" after the embedded installer is launched detached:
    // the user already confirmed in the update dialog, sessions archive on teardown, and the installer
    // would force-close us regardless — so a second generic close-confirm (whose Cancel would only
    // strand the app for the installer to force-kill seconds later) is wrong here.
    void TerminalPage::_QuitForUpdate()
    {
        _FlushWindowRecord();
        _windowRecordTeardownFlushed = true; // mirror RequestQuit: don't let ~TerminalPage re-capture post-teardown
        QuitRequested.raise(nullptr, nullptr);
    }

    // Agentmaster (Linked Lenses — the per-tab -> Manager half of the selection sync): when the user
    // switches to a managed session's terminal tab, drive the Manager lens to select that session, so
    // opening the Agent Manager page (Home) shows the session you were just in highlighted (board card +
    // tree row + its Auto Testing). Equivalent to a single-click on the session's board card. Called from
    // the one post-startup tab-switch funnel (_OnTabSelectionChanged), so a user click, Ctrl+Tab, or a
    // switchToTab action all follow through here. The SIDE PANEL follows too: its selected card + summary
    // move to this session, and — with its ⌖ scroll-to-tab toggle ON — its card list scrolls to the card
    // (a shell tab, which has no card, instead cancels a scroll still owed to the previous tab).
    void TerminalPage::_SyncManagerSelectionToTab(const TerminalApp::Tab& tab)
    {
        // Only once startup/restore is done: during _RestoreWindowTabs the focused tab is re-selected,
        // and we must NOT clobber the per-window lens selection that SetManagerState restored from the
        // record (the same gate _ScheduleWindowRecordSave uses).
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        // A null tab (the Agent Manager page came on screen — returning to it must SHOW the last
        // selection, not change it) and non-session tabs (pwsh / cmd / external) leave the current
        // Manager selection untouched.
        if (!tab)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            // Side panel (⌖ scroll-to-tab): this tab has no card — cancel a scroll still owed to the tab
            // just LEFT (its card may not have rendered yet), so it can't fire later and scroll the list to
            // a tab you are no longer on.
            _SidePanelRevealTabCard(std::wstring{});
            return; // not a managed Claude/Codex session tab -> nothing to select
        }
        // Nav audit: the user switched FOCUS to this managed session's tab (a tab click, Ctrl+Tab, a
        // switchToTab action, or the landing of an Activate/jump). The core "where is the user now"
        // navigation signal; gated on Initialized so a restore's focus-restore can't spam it.
        ::Agentmaster::LogNav(L"tab-focus " + ::Agentmaster::ShortId(id));
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SelectSession(winrt::hstring{ id });
            }
        }
        // Side panel: the selection follows the focused tab here too (highlight the card + swing
        // the summary below). driveManagerLens=false — the lens was just driven above, and the
        // side panel's same-id early-out breaks any echo.
        _SidePanelSelectSession(id, /*driveManagerLens*/ false);
        // Side panel (⌖ scroll-to-tab, default ON): SELECTING a tab scrolls the card list to its card.
        // Called even when the selection above was a same-id no-op (you peeked this very card, scrolled
        // away, then picked its tab) — a tab selection is always an explicit "show me this one". Deferred
        // + coalesced inside; a card already fully on screen never moves; a no-op with the toggle OFF.
        _SidePanelRevealTabCard(id);
    }

    // Agentmaster: EVERY Agentmaster reaction to what just came on screen, in the order the tab-switch
    // funnel always ran them (moved here verbatim from TabManagement.cpp's _OnTabSelectionChanged). One
    // body, three callers:
    //   - the funnel (_OnTabSelectionChanged), right after _UpdatedSelectedTab swapped the content in;
    //   - _RemoveTab, after it hand-picks the tab that takes focus when the FOCUSED tab closes. Upstream
    //     suppresses the funnel for that selection (its _removing guard), so before this was shared a
    //     close successor got none of it: its overlay never learned it was focused, and the
    //     Linked-Lenses selection kept pointing at the closed session;
    //   - _ShowManagerPage, with a NULL tab: the Agent Manager page is what is on screen now. Every
    //     tab-keyed step below (visit, wake, summary, Linked-Lenses sync) no-ops on null, the overlay gate
    //     focuses nothing, and the page-only steps (the lens going on screen, the pill, the bring-into-
    //     view) run — exactly what selecting the old Manager tab ran.
    // Parameters:
    //   tab  - the tab that just became the selected one, or null for "the Agent Manager page".
    //   laps - nullable; when given, receives the stopwatch's lap instants (the funnel's
    //          [tab-switch-slow] breakdown). _RemoveTab and _ShowManagerPage pass null.
    // UI thread only. Exceptions: whatever a step lets escape propagates — the same exposure these
    // steps had inline in the funnel (most catch + log their own failures); _RemoveTab and
    // _ShowManagerPage wrap their calls.
    void TerminalPage::_ApplyAgentTabSelection(const winrt::TerminalApp::Tab& tab, AgentTabSelectionLaps* laps)
    {
        // Agentmaster (perf — the hidden-lens rebuild tax): tell the Manager lens whether it is
        // now on screen. Hidden, it stops rebuilding its board/tree/plan on every registry
        // notify / timing tick / selection sync (all of which land on THIS one UI thread, in
        // the way of the terminal tab being used); shown again, it runs one catch-up rebuild
        // — BEFORE the steps below that read its cards (selection highlight, bring-into-view).
        // _managerPageVisible is the page's own authority, set/cleared before this runs by
        // _ShowManagerPage / _HideManagerPage.
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SetLensVisible(_managerPageVisible);
            }
        }
        // Agentmaster (bookmark tags): a tab switch dismisses the Tags panel — it is anchored
        // under the tab it was opened for, which the switch just left behind. The hover panel
        // likewise (its badge anchor belongs to the strip layout that is about to change).
        _CloseTagEditorPopup();
        _CloseTagHoverPopup();
        // Agentmaster (side panel "+" launcher, SIDE_PANEL.md §2e): a tab switch closes the launcher card too — the user
        // moved away from it (a tab, a close successor, the Agent Manager page). It runs BEFORE _SyncManagerSelectionToTab
        // below: the card's last text goes into the Agent Manager's box first, and the selection pre-aim then sets that
        // box to the newly selected session's folder — exactly what a tab switch does when no card is open. A no-op
        // while the card is closed (a launch from the card already closed it before its new tab was selected).
        _SidePanelCloseLaunchCard(L"tab switch");
        // Agentmaster (transcript overlay): "moving a tab closes the overlay" — any change of what is on
        // screen (a tab switch, a close successor, the Agent Manager page shown / left) closes it. A no-op
        // when it isn't open. It also hands focus back to the tab now on screen: the overlay's focus guard
        // refused the focus move _UpdatedSelectedTab made just before this ran.
        _CloseTranscriptOverlay(tab ? std::wstring_view{ L"tab switch" } : std::wstring_view{ L"agent manager page" });
        // Agentmaster (tab status-dot red flash): switching TO a tab is a "visit" — the current
        // tab is always considered visited, so stop any red flash on the now-focused tab.
        _VisitTabClearFlash(tab);
        // Agentmaster (deactivate): a DEACTIVATED tab — like a window-restored one — holds a
        // never-initialized control, so laying it out (its content went into _tabContent in
        // _UpdatedSelectedTab, just before this runs) is what starts its resume. This call is the
        // belt for an INITIALIZED control parked on a never-started connection (its one-shot layout
        // init is spent); a structural no-op for every other tab, incl. restored and deactivated ones
        // and plain shells.
        _WakeDeactivatedTab(tab);
        // Agentmaster (TAB_OVERLAY.md summary panel): a "here-and-now lens" should be current the
        // instant you look at it — kick a cheap, mtime-gated content re-read of the now-focused
        // tab's summary panel so it isn't up to ~5 s stale (its own timer-backstop cadence).
        _RefreshFocusedTabSummary(tab);
        // Agentmaster (SUMMARY_JUMP.md §4, perf): mirror the new selection onto every linked
        // overlay, so only the focused panel runs the expensive periodic jump-eligibility resolve
        // (and the one we just switched TO resolves once, immediately).
        _SyncOverlayFocusToTab(tab);
        if (laps)
        {
            laps->afterOverlayFocus = std::chrono::steady_clock::now(); // lap 2: lens visibility + popups + flash + wake + summary + overlay focus
        }
        // Agentmaster (PENDING_INPUT.md): a colored tab's effective background shifts on
        // selection (WT draws a deselected tab at 30% over the tab row, much darker), so the
        // now-deselected and now-selected pending tabs must re-pick their "3 dots" light/dark
        // color immediately instead of waiting for the next ~2s scan tick. Cheap (no buffer read).
        _RefreshPendingDotsContrast();
        // Agentmaster (Linked Lenses): follow the switch into the Manager lens — select this
        // tab's managed session so opening the Manager page shows the session you were just
        // in. No-op for the page itself (null), a non-session tab, or before startup completes.
        _SyncManagerSelectionToTab(tab);
        if (laps)
        {
            laps->afterManagerSync = std::chrono::steady_clock::now(); // lap 3: pending-dots contrast + the Manager selection sync (a full lens rebuild when the Manager is on screen)
        }
        // Agentmaster (Linked Lenses): re-evaluate the per-tab "selected/active" pill — it
        // shows only while the Manager page is up, so leaving the page clears it and
        // returning re-applies it for the current hover/selection.
        _UpdateManagerSelectionHighlight();
        // Agentmaster (Linked Lenses): the Manager page coming on screen REVEALS the current
        // selection — scroll the selected card/row into view, since it may have scrolled off
        // while the selection followed tab switches with the page hidden. Page show only.
        if (!tab && _managerPageVisible)
        {
            _BringManagerSelectionIntoView();
        }
        if (laps)
        {
            laps->afterManagerReveal = std::chrono::steady_clock::now(); // lap 4: selection highlight + bring-into-view
        }
    }

    // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): repaint everything THIS window shows of the shared
    // scheduler's current pause state: its "Pause / Resume Prompt Queue Runner" toolbar button, and the
    // mode button of every per-tab badge, which reads "<current> (Off globally)" while the pause is on
    // (AgentTabOverlay::SetGlobalPauseShown). Called on this window's UI thread by the global-pause
    // listener's hop (engine init), for a change made in ANY window.
    // The state is RE-READ here, never passed in: the listener runs on the clicking thread and its hops
    // can reach this thread in any order, so a carried value could leave the button and the badges on
    // the older state.
    // A window without a scheduler does nothing. A window without a Manager page (not built yet, or
    // already released at teardown) skips only the button — the page seeds it when it is wired — and
    // still repaints its badges. A badge that is attached later seeds itself (_AttachClaudeOverlay).
    // Each target is contained on its own: a badge whose re-render throws must not cost the other
    // badges, or the button, their repaint.
    // Cost: one re-render per linked badge of this window (a registry read and a rebuild of the badge's
    // rows), once per real pause change — a user's click, never a timer. Every badge is told at once and
    // not when its tab is next entered, because nothing else would repaint a badge of a quiet session.
    //   returns — nothing.
    //   throws  — nothing: this runs as a dispatcher item, where an escaping exception ends the
    //             process, and it sets XAML content (which can throw). A failure is logged (Rule #18)
    //             and that one button or badge keeps its previous text until the next change repaints it.
    void TerminalPage::_ApplyGlobalPauseShown() noexcept
    {
        try
        {
            if (!_scheduler)
            {
                return;
            }
            const bool paused = _scheduler->GlobalPaused();
            if (_managerPage)
            {
                try
                {
                    _managerPage->SetGlobalPauseShown(paused);
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"_ApplyGlobalPauseShown (Manager button)");
                }
            }
            // The linked badges only: an observe badge (_pendingOverlays) has no mode button, and the side
            // panel's fill-hosted overlay shows its summary panel alone, never the badge rows.
            for (const auto& [id, overlay] : _claudeOverlays)
            {
                if (!overlay)
                {
                    continue;
                }
                try
                {
                    overlay->SetGlobalPauseShown(paused); // change-gated: a badge already on this state does nothing
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"_ApplyGlobalPauseShown (tab badge)");
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ApplyGlobalPauseShown");
        }
    }

    // Agentmaster (cross-window settings broadcast): a GLOBAL settings change in ANOTHER window reached
    // this window's engine sink, which marshaled us onto this UI thread. Adopt the merged settings for
    // this window's page copy (so future spawns / the cog's next open use the latest globals) and hand
    // them to the Manager content, which repaints its Explorer-Tree + Triage-Board sort toggles and
    // re-sorts. A no-op for the content if the Manager page isn't built yet (the page copy still updates).
    void TerminalPage::_ApplyBroadcastSettings(const ::Agentmaster::AppSettings& settings)
    {
        // Inferred-workdir inputs (mode / "Use .git folder to infer") — capture the change BEFORE
        // adopting the broadcast, for the scan-state reset below.
        const bool inferInputsChanged = _appSettings.tabColorMode != settings.tabColorMode ||
                                        _appSettings.inferGitRoot != settings.inferGitRoot;
        // RENDERING.md: capture before adopting too — re-pointing every terminal is only worth doing (and
        // only safe to do cheaply) on a real flip, not on every unrelated broadcast (a sort click).
        const bool softwareRenderingChanged = _appSettings.softwareRendering != settings.softwareRendering;
        // RENDERING.md §4c: same for the hidden-tab policy (through the ONE combiner).
        const bool hiddenRenderingChanged = _appSettings.pauseHiddenTerminals != settings.pauseHiddenTerminals ||
                                            ::Agentmaster::HiddenTerminalReleaseDelayMs(_appSettings) != ::Agentmaster::HiddenTerminalReleaseDelayMs(settings);
        _appSettings = settings;
        // Agentmaster: the tab-strip close affordances (show-X / middle-click close) are GLOBAL, so a
        // change made in another window must re-apply to THIS window's tabs live (the source window
        // already did so in its Save handler).
        _updateAllTabCloseButtons();
        // Agentmaster: "Show icons on tabs" is GLOBAL too — re-apply a change made in another window to
        // THIS window's tabs live (show/hide the profile icon on every tab).
        _UpdateAllTabIcons();
        // Agentmaster (tab sizing): the TAB SIZE settings are GLOBAL — a change saved in another window
        // resizes THIS window's strip live too (the width bounds are process-wide app resources, already in
        // force; the mode + per-tab height / font are per window and applied here).
        _ApplyTabSizingSettings(true);
        // FAVORITES.md §5a: the favorite marker (Crown <-> Star) is GLOBAL — re-assert it on every hosted
        // favorited tab so a change made in another window switches the glyph live here too.
        _RefreshAllFavoriteIcons();
        // Status-dot flash-ring color is GLOBAL — re-point this window's shared flash-ring brush so a change
        // made in another window recolors this window's live flash too.
        _RefreshFlashRingBrush();
        // Taskbar unread count (NOTIFICATIONS.md section 8): the switch is GLOBAL — a change made in another window
        // clears (or repaints) THIS window's taskbar overlay too; every combined button must agree.
        _ApplyTaskbarUnreadCount();
        // Taskbar preview frame (NOTIFICATIONS.md section 9): the switch and the flash-ring color are GLOBAL — a change
        // made in another window unframes / recolors THIS window's preview too.
        _ApplyTaskbarUnreadFrame();
        // Per-tab overlay rest/hover opacities are GLOBAL (TAB_OVERLAY.md) — apply a change made in another
        // window to this window's overlays live too.
        _RefreshOverlayOpacities();
        // Tab color mode is GLOBAL — a mode change made in another window repaints THIS window's
        // managed tabs live too (the source window already repainted its own in the Save handler).
        _ReapplyManagedTabColors();
        // Inferred-workdir inputs changed in another window: re-infer THIS window's hosted sessions
        // under the new rule too (the Save-handler twin — drop the scan gates + kick one pass).
        if (inferInputsChanged)
        {
            _inferredColorScan.clear();
            _ScanInferredTabColors();
        }
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->ApplyExternalSettings(settings);
            }
        }
        // Side panel: adopt the broadcast globals here too (shared sort label, dock side, the
        // summary flags for its out-of-map overlay) + rebuild the cards under the new rules.
        _SidePanelApplySettings();
        // RENDERING.md: a software-rendering flip saved in another window. The process-wide override is
        // already set (one Control DLL per process), but only THIS window can re-point its own
        // terminals, on its own UI thread.
        if (softwareRenderingChanged)
        {
            _ApplySoftwareRenderingSetting(settings.softwareRendering, L"broadcast");
        }
        // RENDERING.md §4c: a hidden-tab policy change saved in another window. The policy statics are
        // process-wide (already set there), but only THIS window can re-apply them to its own terminals.
        if (hiddenRenderingChanged)
        {
            _ApplyHiddenRenderingSetting(L"broadcast");
        }
    }

    // Agentmaster (RENDERING.md §4c): see TerminalPage.h.
    std::wstring TerminalPage::_HiddenRenderingPolicyText() const
    {
        if (!_appSettings.pauseHiddenTerminals)
        {
            return L"pause OFF (hidden terminals keep drawing and keep their GPU device)";
        }
        if (!_appSettings.releaseHiddenTerminalDevices)
        {
            return L"pause ON, GPU release OFF (hidden terminals draw nothing but keep their device)";
        }
        return L"pause ON, GPU release after " + std::to_wstring(::Agentmaster::ClampHiddenTerminalReleaseSeconds(_appSettings.releaseHiddenTerminalDevicesAfterSeconds)) + L" s hidden";
    }

    // Agentmaster (RENDERING.md §4c): see TerminalPage.h. Runs on this window's UI thread (the cog's Save
    // sink or the broadcast sink, both already marshaled here): ReapplyHiddenRenderingPolicy reads each
    // control's IsLoaded() and may take its terminal lock, so it must not run on a background thread.
    void TerminalPage::_ApplyHiddenRenderingSetting(const std::wstring_view reason) noexcept
    {
        // 1. The process-wide policy FIRST, so a terminal that changes visibility between here and the
        //    walk (a tab created this very tick) already reads the new values.
        try
        {
            winrt::Microsoft::Terminal::Control::TermControl::ConfigureHiddenRenderingPolicy(_appSettings.pauseHiddenTerminals,
                                                                                             ::Agentmaster::HiddenTerminalReleaseDelayMs(_appSettings));
        }
        catch (...)
        {
            // The projected static can only fail to reach the Control DLL's activation factory; the walk
            // below still pushes the old policy, which is harmless (Rule #18: never swallowed silently).
            ::Agentmaster::AgentLogCaughtException(L"_ApplyHiddenRenderingSetting(policy)");
        }
        // 2. Every terminal of this window: every pane of every tab (WindowVisibilityChanged's walk).
        //    A hidden one adopts the new delay (or resumes, if the pause was switched off); the visible
        //    one is unaffected. A dormant tab's control is visited too; with no render thread yet the
        //    push only records the state.
        uint32_t visited = 0;
        uint32_t failed = 0;
        try
        {
            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto control = pane->GetTerminalControl())
                        {
                            ++visited;
                            try
                            {
                                control.ReapplyHiddenRenderingPolicy();
                            }
                            catch (...)
                            {
                                // One terminal failed: it keeps its previous policy until its next
                                // visibility change; the rest still adopt the new one. Logged.
                                ++failed;
                                ::Agentmaster::AgentLogCaughtException(L"_ApplyHiddenRenderingSetting(terminal)");
                            }
                        }
                    });
                }
            }
        }
        catch (...)
        {
            // The tab collection changed under the walk (a close racing the Save tick): terminals not
            // reached adopt the policy at their next visibility change, which reads the statics above.
            ::Agentmaster::AgentLogCaughtException(L"_ApplyHiddenRenderingSetting(walk)");
        }
        // 3. One hooks.log line per window per change (grep '\[render\]').
        try
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[render] hidden tabs (" + std::wstring{ reason } + L") window " + ::Agentmaster::ShortId(_windowId) +
                                              L": " + _HiddenRenderingPolicyText() + L"; re-applied " + std::to_wstring(visited - failed) +
                                              L" of " + std::to_wstring(visited) + L" terminal(s)\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ApplyHiddenRenderingSetting(log)");
        }
    }

    // ---- Agentmaster (tab sizing — the cog's Tabs & Overlay "TAB SIZE" section) ---------------------------
    // Four XAML levers, because MUX's TabView exposes no single "tab size" knob:
    //   * WIDTH MODE — TabView.TabWidthMode, the same property Terminal's own tabWidthMode setting drives.
    //   * WIDTH BOUNDS in Equal mode — MUX computes every tab's width itself (std::clamp(available / count,
    //     min, max)) and reads min/max ONLY from the TabViewItemMinWidth / TabViewItemMaxWidth APPLICATION
    //     resources, fresh on each of its width passes; per-tab MinWidth/MaxWidth would fight its explicit
    //     Width and break its overflow (scroll-button) math. So Equal mode = the resources, nothing per tab.
    //   * WIDTH BOUNDS in Fit to title / Compact — MUX leaves each tab content-sized (Width = NaN), so the
    //     bounds go on every TabViewItem as MinWidth / MaxWidth (Compact: max only — a min would stop
    //     unselected tabs from shrinking to their icon).
    //   * HEIGHT + TITLE FONT — per tab: TabViewItem.MinHeight (overrides the style's ThemeResource floor)
    //     and the header control's title TextBlocks (TabHeaderControl::ApplyTitleFont).
    // Every default CLEARS Agentmaster's local value instead of writing the default, so an untouched
    // install keeps exactly the values the XAML styles give (App.xaml's 130 px floor, MUX's 240 / 12 / 32).

    // See TerminalPage.h. Reads _appSettings (default-constructed before engine init == "follow Terminal")
    // and Terminal's own settings. Returns: the TabViewWidthMode the strip should be in. Never throws in
    // practice (projected getters on a loaded settings object).
    winrt::MUX::Controls::TabViewWidthMode TerminalPage::_EffectiveTabWidthMode() const
    {
        switch (_appSettings.tabWidthMode)
        {
        case ::Agentmaster::TabStripWidthMode::Equal:
            return winrt::MUX::Controls::TabViewWidthMode::Equal;
        case ::Agentmaster::TabStripWidthMode::TitleLength:
            return winrt::MUX::Controls::TabViewWidthMode::SizeToContent;
        case ::Agentmaster::TabStripWidthMode::Compact:
            return winrt::MUX::Controls::TabViewWidthMode::Compact;
        case ::Agentmaster::TabStripWidthMode::Terminal:
        default:
            break;
        }
        // "Windows Terminal setting": upstream's own source. Guarded only for a page torn down mid-call —
        // _settings is set in Create(), before any caller can run.
        return _settings ? _settings.GlobalSettings().TabWidthMode() : winrt::MUX::Controls::TabViewWidthMode::Equal;
    }

    // See TerminalPage.h. Called at engine init (relayoutStrip=false — no tab exists yet, and the strip's
    // first width pass reads the bounds published here), from the cog's Save sink and from the cross-window
    // broadcast (relayoutStrip=true). The application resources are PROCESS-wide (one XAML app hosts every
    // window), so each window re-publishing the same values is a no-op after the first; the mode and the
    // per-tab values are per window and re-applied here for this window only. UI thread. Never throws: a
    // XAML failure is logged (Rule #18) and leaves the strip as it was — a cosmetic loss, never a crash.
    void TerminalPage::_ApplyTabSizingSettings(bool relayoutStrip)
    {
        try
        {
            uint32_t minWidth = _appSettings.tabMinWidth;
            uint32_t maxWidth = _appSettings.tabMaxWidth;
            ::Agentmaster::NormalizeTabWidthRange(minWidth, maxWidth); // never hand MUX's std::clamp a crossed range

            // (1) Tab::_UpdateHeaderControlMaxWidth sizes each rename box by the width mode but can only read
            //     Terminal's settings — publish the override where it looks (-1 == follow Terminal).
            SetTabWidthModeOverride(_appSettings.tabWidthMode == ::Agentmaster::TabStripWidthMode::Terminal ?
                                        -1 :
                                        static_cast<int32_t>(_EffectiveTabWidthMode()));

            // (2) The Equal-mode bounds, where MUX's width pass reads them. The resources are PROCESS-wide, so
            //     whether THIS window's strip still needs a width pass can't be read off them: in a second
            //     window (the broadcast) they are already current — the saving window wrote them — yet its
            //     tabs were sized with the old ones. Hence the per-window record of what this strip last got.
            _PublishTabWidthBounds(static_cast<double>(minWidth), static_cast<double>(maxWidth));
            const bool boundsChanged = minWidth != _tabSizingAppliedMinWidth || maxWidth != _tabSizingAppliedMaxWidth;
            _tabSizingAppliedMinWidth = minWidth;
            _tabSizingAppliedMaxWidth = maxWidth;

            if (!_tabView)
            {
                return; // not built yet — Create() hasn't run; nothing on screen to size
            }

            // (3) The mode. A REAL change makes MUX re-run its width pass (with the bounds just published) and
            //     _UpdateTabWidthMode re-applies every tab's mode-dependent bounds itself.
            const auto modeBefore = _tabView.TabWidthMode();
            _UpdateTabWidthMode();
            const auto mode = _tabView.TabWidthMode();
            if (mode == modeBefore)
            {
                // (4) Same mode: the per-tab parts (height, title font, Fit-to-title bounds) still need
                //     re-applying — each write is change-gated, so an untouched setting costs only reads.
                _ApplyTabSizingToAllTabs();
                // (5) ...and in Equal, new bounds only reach the tabs on MUX's NEXT width pass (a resize, a
                //     tab add/remove) — force one now so a Save is visibly live.
                if (relayoutStrip && boundsChanged && mode == winrt::MUX::Controls::TabViewWidthMode::Equal)
                {
                    _ForceTabStripWidthPass();
                }
            }

            const auto modeName = mode == winrt::MUX::Controls::TabViewWidthMode::SizeToContent ? L"titleLength" :
                                  mode == winrt::MUX::Controls::TabViewWidthMode::Compact       ? L"compact" :
                                                                                                 L"equal";
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[tab-size] mode=" + std::wstring{ modeName } +
                                              L" (setting " + ::Agentmaster::ToString(_appSettings.tabWidthMode) + L")" +
                                              L" width=" + std::to_wstring(minWidth) + L".." + std::to_wstring(maxWidth) +
                                              L" font=" + std::to_wstring(::Agentmaster::ClampTabTitleFontSize(_appSettings.tabTitleFontSize)) +
                                              L"/" + ::Agentmaster::ToString(_appSettings.tabTitleFontWeight) +
                                              L" minHeight=" + std::to_wstring(::Agentmaster::ClampTabMinHeight(_appSettings.tabMinHeight)) +
                                              L" tabs=" + std::to_wstring(_tabs.Size()) +
                                              (relayoutStrip ? L"" : L" (init)") + L"\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ApplyTabSizingSettings");
        }
    }

    // See TerminalPage.h. Per tab: the height / title font / bounds, then the rename box's width ceiling
    // (it follows the width mode — Tab::RefreshHeaderRenamerMaxWidth reads the override published above).
    // UI thread. Never throws: _ApplyTabSizingToTab catches per tab, and the ceiling refresh CATCH_LOGs.
    void TerminalPage::_ApplyTabSizingToAllTabs()
    {
        for (const auto& tab : _tabs)
        {
            if (!tab)
            {
                continue;
            }
            _ApplyTabSizingToTab(tab.TabViewItem());
            if (const auto tabImpl{ _GetTabImpl(tab) })
            {
                tabImpl->RefreshHeaderRenamerMaxWidth();
            }
        }
    }

    // See TerminalPage.h.
    // Parameters: tabViewItem - the tab to size; null is tolerated (a tab mid-teardown).
    // Returns: the width in px this call holds the tab at for an open rename box (Equal width, selected tab —
    //          RenamingTabWidth), or 0 when no rename hold is in force (the usual case; also after a failure).
    // UI thread. Never throws: one tab's XAML failure is logged (Rule #18) and the loop moves on.
    uint32_t TerminalPage::_ApplyTabSizingToTab(const winrt::MUX::Controls::TabViewItem& tabViewItem)
    {
        if (!tabViewItem)
        {
            return 0;
        }
        try
        {
            uint32_t minWidth = _appSettings.tabMinWidth;
            uint32_t maxWidth = _appSettings.tabMaxWidth;
            ::Agentmaster::NormalizeTabWidthRange(minWidth, maxWidth);
            const auto minHeight = ::Agentmaster::ClampTabMinHeight(_appSettings.tabMinHeight);
            const auto fontSize = ::Agentmaster::ClampTabTitleFontSize(_appSettings.tabTitleFontSize);

            // HEIGHT — the style's own floor (TabViewItemMinHeight, 32) is the default, so 32 clears ours.
            _SetOrClearLocalDouble(tabViewItem, winrt::WUX::FrameworkElement::MinHeightProperty(), static_cast<double>(minHeight), minHeight == ::Agentmaster::kDefaultTabMinHeight);

            // The header control: the title font below, and whether its rename box is open (the hold below).
            winrt::com_ptr<implementation::TabHeaderControl> headerImpl{ nullptr };
            if (const auto header = tabViewItem.Header().try_as<winrt::TerminalApp::TabHeaderControl>())
            {
                headerImpl.copy_from(winrt::get_self<implementation::TabHeaderControl>(header));
            }

            // WIDTH — by the mode the strip is ACTUALLY in (read off the TabView, so this stays right while
            // a mode flip is mid-flight): Equal hands widths to MUX entirely (both cleared — the bounds ride
            // the app resources); Compact keeps only the max (the selected tab's title); Fit to title gets both.
            const auto mode = _tabView ? _tabView.TabWidthMode() : winrt::MUX::Controls::TabViewWidthMode::Equal;
            const bool equal = mode == winrt::MUX::Controls::TabViewWidthMode::Equal;
            const bool compact = mode == winrt::MUX::Controls::TabViewWidthMode::Compact;

            // ...except the RENAME HOLD (tab title fit). The rename box is bounded by its tab (the title row's
            // star column, TabHeaderControl.xaml), and an Equal tab has the same fixed share as every other —
            // with many tabs, the minimum — so a rename there would show only a few typed characters. While the
            // box is open the SELECTED tab gets MinWidth = RenamingTabWidth (the maximum tab width, 200 px
            // floor): a MinWidth larger than the Width MUX writes wins in XAML's min/max resolution, so the tab
            // widens without fighting MUX's own width pass. The widened tab is kept on screen by the page
            // (_TrackRenamingTabInStrip, run by the same RenamerOpenChanged right after this): the strip
            // ignores bring-into-view requests since _EnsureTabStripScrollViewer, so MUX's own reaction to the
            // grown items presenter (TabView::OnItemsPresenterSizeChanged -> BringSelectedTabIntoView) no
            // longer scrolls anything. The hold is limited to the selected tab as designed (an unselected tab
            // renamed from its context menu keeps its narrow tab, the box bounded by it and its text
            // scrolling). Its original reason, that the same MUX call would scroll the strip to the selected
            // tab and carry the box out of view, no longer applies. Fit to title and Compact need no hold:
            // there the tab already grows with the box up to its MaxWidth. The close (InRename() false again)
            // falls through to the normal Equal branch, which clears the MinWidth.
            const bool renameHold = equal && headerImpl && headerImpl->InRename() && tabViewItem.IsSelected();
            const uint32_t heldWidth = renameHold ? ::Agentmaster::RenamingTabWidth(maxWidth) : 0;
            if (renameHold)
            {
                _SetOrClearLocalDouble(tabViewItem, winrt::WUX::FrameworkElement::MinWidthProperty(), static_cast<double>(heldWidth), false);
            }
            else
            {
                _SetOrClearLocalDouble(tabViewItem, winrt::WUX::FrameworkElement::MinWidthProperty(), static_cast<double>(minWidth), equal || compact);
            }
            _SetOrClearLocalDouble(tabViewItem, winrt::WUX::FrameworkElement::MaxWidthProperty(), static_cast<double>(maxWidth), equal);

            // TITLE FONT — the header control applies it to the title TextBlocks; 0 == inherit (the default).
            if (headerImpl)
            {
                const uint16_t weight = _appSettings.tabTitleFontWeight == ::Agentmaster::TabTitleFontWeight::Bold     ? uint16_t{ 700 } :
                                        _appSettings.tabTitleFontWeight == ::Agentmaster::TabTitleFontWeight::SemiBold ? uint16_t{ 600 } :
                                                                                                                      uint16_t{ 0 };
                headerImpl->ApplyTitleFont(fontSize == ::Agentmaster::kDefaultTabTitleFontSize ? 0.0 : static_cast<double>(fontSize), weight);
            }
            return heldWidth;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ApplyTabSizingToTab");
        }
        return 0;
    }

    // See TerminalPage.h.
    // Parameters: minWidth / maxWidth - the normalized Equal-mode bounds in px (max >= min).
    // Throws: a winrt::hresult_error from the resource dictionary; the caller catches + logs.
    void TerminalPage::_PublishTabWidthBounds(double minWidth, double maxWidth)
    {
        // Application.Resources, not the TabView's own Resources: MUX's TabView::UpdateTabWidths looks the two
        // keys up with SharedHelpers::FindInApplicationResources, so nothing else is consulted. Inserting at
        // the dictionary's ROOT overrides App.xaml's merged TabViewItemMinWidth and MUX's merged
        // TabViewItemMaxWidth alike, since a root key wins over merged dictionaries.
        const auto res = winrt::WUX::Application::Current().Resources();
        const auto publish = [&res](const wchar_t* name, const double value) {
            const auto key = winrt::box_value(winrt::hstring{ name });
            if (res.HasKey(key) && winrt::unbox_value_or<double>(res.Lookup(key), -1.0) == value)
            {
                return; // already in force — App.xaml's / MUX's own value, or this process's last write
            }
            res.Insert(key, winrt::box_value(value));
        };
        publish(L"TabViewItemMinWidth", minWidth);
        publish(L"TabViewItemMaxWidth", maxWidth);
    }

    // See TerminalPage.h. A flip to SizeToContent and straight back runs MUX's private UpdateTabWidths twice
    // within this one UI tick; no layout pass runs in between, so the intermediate mode never paints, and the
    // second (Equal) pass sizes every tab with the bounds just published. It is the same runtime path
    // Terminal itself takes when its tabWidthMode setting changes. UI thread. Throws: a winrt::hresult_error
    // from the TabView; the caller (_ApplyTabSizingSettings) catches + logs.
    void TerminalPage::_ForceTabStripWidthPass()
    {
        const auto equal = winrt::MUX::Controls::TabViewWidthMode::Equal;
        if (!_tabView || _tabView.TabWidthMode() != equal)
        {
            return; // only Equal consumes the resource bounds; the other modes need no width pass
        }
        _tabView.TabWidthMode(winrt::MUX::Controls::TabViewWidthMode::SizeToContent);
        _tabView.TabWidthMode(equal);
        _ClearTabStripListMaxWidth(); // the intermediate SizeToContent pass capped the list; Equal never resets it
    }

    // See TerminalPage.h. UI thread. Never throws: a failed walk/clear is logged (Rule #18) and leaves the cap
    // (a strip that can't grow past its old width until the next mode change — cosmetic).
    void TerminalPage::_ClearTabStripListMaxWidth()
    {
        try
        {
            if (!_tabView)
            {
                return;
            }
            // ClearValue on a property with no local value is a no-op, so this is safe to repeat.
            if (const auto list = _FindTabStripListView(_tabView.as<winrt::WUX::DependencyObject>()))
            {
                list.ClearValue(winrt::WUX::FrameworkElement::MaxWidthProperty());
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ClearTabStripListMaxWidth");
        }
    }

    // Agentmaster (RENDERING.md): see TerminalPage.h. Runs on this window's UI thread (the cog's Save
    // sink or the broadcast sink, both already marshaled here). ReapplySoftwareRendering takes each
    // terminal's write lock, so it must not run on a background thread.
    void TerminalPage::_ApplySoftwareRenderingSetting(const bool enabled, const std::wstring_view reason) noexcept
    {
        // 1. The process-wide flag FIRST, so a terminal that initializes between here and the walk
        //    (a tab being created this very tick) already builds the right engine.
        try
        {
            winrt::Microsoft::Terminal::Control::TermControl::SetSoftwareRenderingOverride(enabled);
        }
        catch (...)
        {
            // The projected static can only fail to reach the Control DLL's activation factory; keep going,
            // the walk below still re-points what it can (Rule #18: never swallow it silently).
            ::Agentmaster::AgentLogCaughtException(L"_ApplySoftwareRenderingSetting(override)");
        }
        // 2. Every terminal this window ALREADY initialized: every pane of every tab — the same walk
        //    WindowVisibilityChanged uses. A dormant tab's control is visited too; its core ignores the
        //    call (nothing built yet) and reads the override when it first initializes.
        uint32_t visited = 0;
        uint32_t failed = 0;
        try
        {
            for (const auto& tab : _tabs)
            {
                if (auto tabImpl{ _GetTabImpl(tab) })
                {
                    tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                        if (const auto control = pane->GetTerminalControl())
                        {
                            ++visited;
                            try
                            {
                                control.ReapplySoftwareRendering();
                            }
                            catch (...)
                            {
                                // One terminal's projected settings call failed: that terminal keeps its
                                // current renderer, the rest still switch (no all-or-nothing). Logged.
                                ++failed;
                                ::Agentmaster::AgentLogCaughtException(L"_ApplySoftwareRenderingSetting(terminal)");
                            }
                        }
                    });
                }
            }
        }
        catch (...)
        {
            // The tab collection changed under the walk (a close racing the Save tick) or a pane walk
            // threw: the terminals not reached keep their renderer until the next flip or restart, and
            // every NEW terminal still follows the override set above.
            ::Agentmaster::AgentLogCaughtException(L"_ApplySoftwareRenderingSetting(walk)");
        }
        // 3. One hooks.log line per window per flip — the evidence trail when a user reports "tabs went
        //    black" / "the app got slow" after toggling it (grep '\[render\]').
        try
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          std::wstring{ L"[render] software rendering " } + (enabled ? L"ON (CPU / WARP)" : L"OFF (GPU)") +
                                              L" (" + std::wstring{ reason } + L") window " + ::Agentmaster::ShortId(_windowId) +
                                              L": re-pointed " + std::to_wstring(visited - failed) + L" of " + std::to_wstring(visited) +
                                              L" terminal(s)\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ApplySoftwareRenderingSetting(log)");
        }
    }

    // Agentmaster (Linked Lenses — the reveal half of the per-tab -> Manager sync): when the Agent
    // Manager page comes on screen, scroll the currently-selected board card / tree row into view. The
    // lens selection follows tab switches (_SyncManagerSelectionToTab) while the page is hidden, so on
    // return the highlighted card can be scrolled off-screen in a tall column (or after a state change
    // moved it to another column); this synchronizes its visibility with its highlight. Called from
    // _ApplyAgentTabSelection only for the page show (a null tab while _managerPageVisible).
    void TerminalPage::_BringManagerSelectionIntoView()
    {
        // Pre-startup the focused-tab restore is in flight (the lens selection is being seeded from the
        // WindowRecord); don't yank the view — the same gate _SyncManagerSelectionToTab uses.
        if (_startupState != StartupState::Initialized)
        {
            return;
        }
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->BringSelectedIntoView();
            }
        }
    }

    // Agentmaster: one-line log identity for tab-strip forensics — `<sid8> "<title>"` for a managed
    // tab, `"<title>"` for a shell tab. Log-path safe: never throws.
    std::wstring TerminalPage::_DescribeTabForLog(const winrt::TerminalApp::Tab& tab)
    {
        try
        {
            if (!tab)
            {
                return L"?";
            }
            std::wstring ident;
            if (const auto sid = _ClaudeSessionForTab(tab); !sid.empty())
            {
                ident = ::Agentmaster::ShortId(sid) + L" ";
            }
            ident += L"\"" + std::wstring{ tab.Title() } + L"\"";
            return ident;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_DescribeTabForLog");
            return L"?";
        }
    }

    // Agentmaster: MUX TabView drag AV — the root cause, the disproven candidates, and the RESOLUTION.
    // Dragging ANY tab has WUX report the dragged ITEM as the pressed container's **Content**, not the
    // TabViewItem itself (ListViewBase::GetDraggedItems, ListViewBase_Partial_Reorder.cpp: for an
    // items-are-their-own-containers list — exactly WT's TabItems of TabViewItems — it appends
    // get_Content() to DragItemsStarting.Items). For WT that Content is the BODGY unique empty Border
    // every Tab plants at construction (Tab.cpp `TabViewItem().Content(Border{})` — upstream's
    // disambiguator for EXACTLY this MUX lookup). So MUX's TabView::FindTabViewItemFromDragItem can
    // NEVER resolve the item up front: ContainerFromItem(border) misses (a Border is not an item of
    // TabItems) and the SINGLE-level VisualTreeHelper::GetParent(border) is null (the Content is never
    // rendered), which sends EVERY drag into the fallback loop `ContainerFromIndex(i).Content() ==
    // item` from index 0 — and that loop null-derefs on the FIRST virtualized-out container
    // (0xC0000005 at Microsoft.UI.Xaml.dll+0xB605C, uncatchable under /EHsc). The lookup runs at
    // drag-START (OnListViewDragItemsStarting) AND at the DROP (OnListViewDragItemsCompleted), and
    // both are crash sites. Dump-proven FOUR times (3x 0.6.7.x on 2026-07-16; 0.6.8.1 on 2026-07-30
    // with Rsi=0xA7 == 167 TabItems, Rdi=0 == died at loop index 0 — the DROP-side variant, whose AV
    // was SWALLOWED at the callback boundary and left a live-but-wedged app). Every fix candidate is
    // DISPROVEN — see the CLAUDE.md "MUX TabView drag-start null-deref" gotcha for the full verdicts:
    //   - panel swap to StackPanel (0.6.7.2): ILLEGAL for a ListView — deterministic startup render
    //     crash; removed. Do NOT retry.
    //   - CacheLength boost (the former _BoostTabStripCacheForDrag, commit c4d366460): ACTIVE in the
    //     crashed 0.6.8.1 instance and index 0 was STILL unrealized at the drop — a large cache is a
    //     realization HINT the panel may not honor. Removed 2026-07-30.
    //   - check-then-allow gates: the 2026-07-30 crash PASSED the start-side lookup and died at the
    //     drop 2.5 s later — mid-drag derealization defeats any gate that can only veto the start.
    //   - Content tricks (self/template-root/parented-Border): the stock TabView template's
    //     UpdateTabContent re-parents tvi.Content() on every selection change — guaranteed
    //     "already child of another element" crash.
    // RESOLUTION: the lookup is made UNREACHABLE — CanReorderTabs(false) + CanDragTabs(false)
    // (TerminalPage::Create + TabRowControl.xaml; the exact configuration WT ships for elevated
    // windows), with reorder + tear-out re-implemented as the POINTER-OWNED gesture below
    // (_WireTabReorderGesture / _TabReorder*): raw pointer events + the existing _TryMoveTab /
    // _sendDraggedTabToWindow — no WUX/MUX drag pipeline, no OLE, nothing that consults an
    // unrealized container. The settle/CanDrag belts below stay: cheap, and they keep the
    // item->container map sane for OUR OWN ContainerFromIndex/ContainerFromItem probes.

    // Agentmaster: MUX TabView drag-start AV guard, layer 1 (a BELT — see the drag-AV note above) —
    // settle the strip's item->container mapping BEFORE
    // returning to the message pump. XAML dispatches queued INPUT ahead of the pending LAYOUT pass, so
    // a press+move queued behind a TabItems() insert/remove/reinsert can cross the drag threshold
    // while the mapping is still uncommitted, and MUX's drag paths consult that mapping
    // (ContainerFromItem/IndexFromContainer). UpdateLayout() runs the pass NOW — the layout was going
    // to run next frame anyway, so this is re-ordered work, not added work. Called after EVERY
    // TabItems() mutation (_InitializeTab / _RemoveTab / _TryMoveTab / _TabDragCompleted).
    // ⚠ The re-ordering is visible in one place: the pass raises the strip's SizeChanged NOW, and MUX
    // answers a strip that GREW by scrolling its SELECTED tab into view (TabView::OnItemsPresenterSizeChanged
    // -> BringSelectedTabIntoView; MUX skips it after a removal until the pointer leaves the strip). That
    // request is routed asynchronously, so it lands after everything else this call stack does and nothing
    // here can undo it. So settle with the tab that must end up on screen already SELECTED — _InitializeTab
    // selects a new tab first for exactly this reason (2026-10-03: a tab opened from another tab's context
    // menu scrolled the strip to the tab the user had been on). Since 2026-10-05 the strip ignores that
    // request (_EnsureTabStripScrollViewer turns its bring-into-view off — MUX made the same request on every
    // content resize WHILE the user scrolled a big strip), so the order is now a belt: it still matters before
    // the strip's ScrollViewer is bound, or if turning that off failed (logged).
    // The settle is also a whole-window layout pass:
    // content swapped in just before it gets its first layout HERE, synchronously, and a terminal control
    // starts its connection on its first layout — for a brand-new tab that would be before
    // _LaunchClaudeSession has wired the session up, which is why _InitializeTab also settles BEFORE its
    // switch. (_TryMoveTab settles after its switch; a moved tab is an existing, already-wired one.)
    // Never throws: a failure is logged ([tabdrag-guard] + [exc]) and the strip settles on the next frame.
    void TerminalPage::_SettleTabStripLayout()
    {
        try
        {
            if (_tabView)
            {
                _tabView.UpdateLayout();
            }
        }
        catch (...)
        {
            // Surface the failure in OUR log too (a swallowed settle re-opens the crash window).
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[tabdrag-guard] settle UpdateLayout threw\n");
            ::Agentmaster::AgentLogCaughtException(L"tabdrag-guard settle UpdateLayout");
        }
    }

    // Agentmaster: per-tab native-drag policy (see the drag-AV resolution note above) + the settle.
    // Native MUX tab drag is permanently OFF, so EVERY TabViewItem is pinned CanDrag(false),
    // unconditionally — a UIElement with CanDrag(true) could still start a raw XAML drag even with
    // the list-level CanDragTabs off, which would walk the same broken WUX GetDraggedItems path.
    // (This function replaced _GuardTabDragUntilRegistered, whose settle-then-defer-to-Loaded
    // re-ENABLE existed to restore native drag once the item->container map committed; with native
    // drag gone the re-enable was not just moot but the one hole left in the disable — and its
    // per-Loaded log line stormed at ~30 Hz during a big window-restore. The settle stays: our own
    // reorder gesture + _RevealTabInStrip probe the same item->container map.)
    void TerminalPage::_ApplyTabDragPolicy(const MUX::Controls::TabViewItem& tabViewItem)
    {
        if (!_tabView || !tabViewItem)
        {
            return;
        }
        _SettleTabStripLayout();
        try
        {
            tabViewItem.CanDrag(false); // native drag never starts — the pointer-owned gesture is the only drag
        }
        CATCH_LOG();
    }

    // ======================================================================================
    // Agentmaster: the POINTER-OWNED tab reorder/tear-out gesture — the native-MUX-drag
    // replacement (see the drag-AV resolution note above; pure decision math in
    // AgentMaster/TabDragMath.h, unit-tested in the engine harness).
    //
    // Shape: press a tab header (mouse/pen, left button) -> a 6-DIP threshold activates the
    // gesture and captures the pointer ON THE TABVIEW (not the item — item churn during the
    // gesture must not disturb the capture) -> an accent INSERTION CARET tracks the midpoint
    // rule over the REALIZED headers (a caret-commit design: nothing moves until release, so
    // there is no per-crossing selection/content churn) -> edge auto-scroll (per-move + a
    // hold-still timer) slides the strip -> release IN the strip band commits ONE _TryMoveTab
    // (which already logs [nav] tab-move, announces for UIA, settles + respects the Manager
    // floor); release over ANOTHER Agentmaster window's tab-strip row DOCKS the tab into that
    // window at the aimed slot (the dock-target probes — see the cross-window DOCKING block
    // below — with the target's own insertion caret previewing the slot while hovered); release
    // CLEAR of everything else tears the tab out into a new window through the exact native
    // downstream (_sendDraggedTabToWindow, which logs [nav] tab-send-to-window and detaches the
    // managed session correctly); Esc cancels (nothing moved, nothing to revert). [nav] taxonomy
    // preserved: tab-drag-begin at activation, tab-drag-end at every exit.
    //
    // Deliberate limits: TOUCH is left to the strip's native panning (a touch-drag threshold
    // would fight the pan gesture); a window of ANOTHER Agentmaster PROCESS (the dev install
    // beside the release one) is not a dock target — its dock sink lives in the other process's
    // engine — so a release there tears out into a new window, exactly as before.
    // ======================================================================================

    namespace
    {
        constexpr double kTabDragThresholdDips = 6.0; // press->drag arming distance (either axis)
        constexpr double kTabDragEdgeBandDips = 36.0; // auto-scroll engages within this of a strip edge
        constexpr double kTabDragScrollStepDips = 28.0; // per move/tick step (~560 DIPs/s on the 50 ms timer)
        constexpr double kTabDragTearOutSlackDips = 32.0; // grace band around the strip before a release reads as tear-out
        constexpr double kTabDockStripSlackDips = 12.0; // cross-window dock: vertical grace around a TARGET window's strip row (tighter than the tear-out slack — a dock must aim at the tab bar, not the content below it)
        constexpr auto kTabDragAutoScrollTick = std::chrono::milliseconds{ 50 };
    }

    // Wire the gesture's pointer handlers onto the TabView — once, from Create(). AddHandler with
    // handledEventsToo: the ListViewItem internals mark presses Handled (selection), and during our
    // capture the routed events surface on the TabView itself. The lambdas hold get_weak() so a
    // torn-down page no-ops; the handlers die with the TabView's tree, so no RemoveHandler pass.
    void TerminalPage::_WireTabReorderGesture()
    {
        if (!_tabView)
        {
            return;
        }
        try
        {
            auto weak = get_weak();
            _tabView.AddHandler(winrt::WUX::UIElement::PointerPressedEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnPressed(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerMovedEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnMoved(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerReleasedEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnReleased(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerCaptureLostEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnCaptureLost(e);
                                    }
                                } }),
                                true);
            _tabView.AddHandler(winrt::WUX::UIElement::PointerCanceledEvent(),
                                winrt::box_value(winrt::WUX::Input::PointerEventHandler{ [weak](const auto&, const winrt::WUX::Input::PointerRoutedEventArgs& e) {
                                    if (auto page = weak.get())
                                    {
                                        page->_TabReorderOnCaptureLost(e);
                                    }
                                } }),
                                true);
            // The hold-still edge auto-scroll tick (started/stopped per gesture; SafeDispatcherTimer's
            // guarded Destroy makes teardown safe on any thread).
            _tabDragAutoScrollTimer.Interval(kTabDragAutoScrollTick);
            _tabDragAutoScrollTimer.Tick([weak](const auto&, const auto&) {
                if (auto page = weak.get())
                {
                    if (page->_tabReorder.active)
                    {
                        page->_TabReorderUpdate(page->_tabReorder.lastPoint);
                    }
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_WireTabReorderGesture"); // gesture unavailable; tabs still work (menu/keyboard moves)
        }
    }

    // A left press over a draggable tab header ARMS the gesture (nothing visible yet — a plain
    // click stays a plain click; the threshold in OnMoved is what activates).
    void TerminalPage::_TabReorderOnPressed(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if (_tabReorder.active)
            {
                return; // one gesture at a time — a second pointer never steals it
            }
            if (_tabReorder.armed)
            {
                // A stale arm: the release happened off-strip (no capture below threshold), so we
                // never saw it. This press supersedes it.
                _TabReorderReset();
            }
            if (!_tabView)
            {
                return;
            }
            // Touch pans the strip natively — a touch drag-threshold would fight it. Mouse + pen only.
            if (e.Pointer().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Touch)
            {
                return;
            }
            const auto pt = e.GetCurrentPoint(_tabView);
            if (!pt.Properties().IsLeftButtonPressed())
            {
                return; // right = context menu, middle = close — not drags
            }
            // Resolve the pressed header: walk OriginalSource up to a TabViewItem, refusing when an
            // interactive control (the close button, the strip's scroll RepeatButtons) sits between —
            // those own their own press (and would capture the pointer themselves anyway).
            winrt::MUX::Controls::TabViewItem tvi{ nullptr };
            auto node = e.OriginalSource().try_as<winrt::WUX::DependencyObject>();
            while (node)
            {
                if (node.try_as<winrt::WUX::Controls::Primitives::ButtonBase>())
                {
                    return;
                }
                if (auto asTvi = node.try_as<winrt::MUX::Controls::TabViewItem>())
                {
                    tvi = asTvi;
                    break;
                }
                node = winrt::WUX::Media::VisualTreeHelper::GetParent(node);
            }
            if (!tvi)
            {
                return;
            }
            const auto tab = _GetTabByTabViewItem(tvi);
            if (!tab)
            {
                return;
            }
            // (Every tab is movable: the Agent Manager is a page behind the Home button now, not a
            // pinned tab this gesture had to refuse.)
            _tabReorder.armed = true;
            _tabReorder.pointerId = e.Pointer().PointerId();
            _tabReorder.pressPoint = pt.Position();
            _tabReorder.lastPoint = _tabReorder.pressPoint;
            _tabReorder.tab = tab;
            _tabReorder.tvi = tvi;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnPressed");
            _TabReorderReset();
        }
    }

    void TerminalPage::_TabReorderOnMoved(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if ((!_tabReorder.armed && !_tabReorder.active) || e.Pointer().PointerId() != _tabReorder.pointerId)
            {
                return;
            }
            const auto pt = e.GetCurrentPoint(_tabView);
            const auto pos = pt.Position();
            _tabReorder.lastPoint = pos;
            if (!_tabReorder.active)
            {
                if (!pt.Properties().IsLeftButtonPressed())
                {
                    // The button went up somewhere we never saw the release (below threshold there
                    // is no capture) — the arm is stale.
                    _TabReorderReset();
                    return;
                }
                if (!Agentmaster::TabDragThresholdCrossed(pos.X - _tabReorder.pressPoint.X,
                                                          pos.Y - _tabReorder.pressPoint.Y,
                                                          kTabDragThresholdDips))
                {
                    return;
                }
                _TabReorderActivate(e);
                if (!_tabReorder.active)
                {
                    return; // activation refused (tab vanished / capture failed)
                }
            }
            _TabReorderUpdate(pos);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnMoved");
            _TabReorderCancel(L"exception");
        }
    }

    // Threshold crossed: capture the pointer on the TabView, lift the tab, arm Esc + the
    // auto-scroll tick, and log the gesture BEGIN.
    void TerminalPage::_TabReorderActivate(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        const auto idx = _TabReorderIndexOf(_tabReorder.tab);
        if (idx < 0)
        {
            _TabReorderReset(); // the tab was closed between press and threshold
            return;
        }
        if (!_tabView.CapturePointer(e.Pointer()))
        {
            _TabReorderReset();
            return;
        }
        _tabReorder.active = true;
        _tabReorder.originalIndex = idx;
        _tabReorder.lastSlot = -1;
        // The native tear-out recipe's drag offset (position the new window under the cursor),
        // captured at activation exactly like _onTabDragStarting does.
        try
        {
            if (_hostingHwnd)
            {
                const auto inverseScale = 1.0f / static_cast<float>(_tabReorder.tvi.XamlRoot().RasterizationScale());
                POINT cursorPos;
                if (GetCursorPos(&cursorPos) && ScreenToClient(*_hostingHwnd, &cursorPos))
                {
                    _tabReorder.dragOffset = { cursorPos.x * inverseScale, cursorPos.y * inverseScale };
                }
            }
        }
        CATCH_LOG();
        ::Agentmaster::LogNav(L"tab-drag-begin " + _DescribeTabForLog(_tabReorder.tab));
        // Agentmaster (transcript overlay): a tab drag is "moving a tab" — it closes the overlay the
        // moment the drag activates (a reorder, a tear-out and a dock all start here).
        _CloseTranscriptOverlay(L"tab drag");
        try
        {
            _tabReorder.tvi.Opacity(0.55); // the "lifted" cue; restored in _TabReorderReset
        }
        CATCH_LOG();
        _EnsureTabStripScrollViewer();
        // Esc cancels — a tunneling PreviewKeyDown on the page beats whichever control holds focus
        // (and keeps the Esc out of the focused terminal — a mid-drag Esc must not reach claude).
        _tabReorderKeyRevoker = Root().PreviewKeyDown(winrt::auto_revoke, [weak = get_weak()](const auto&, const winrt::WUX::Input::KeyRoutedEventArgs& k) {
            if (k.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                if (auto page = weak.get())
                {
                    if (page->_tabReorder.active)
                    {
                        k.Handled(true);
                        page->_TabReorderCancel(L"esc");
                    }
                }
            }
        });
        _tabDragAutoScrollTimer.Start();
    }

    // The per-move (and per-tick) decision pass: bail if the dragged tab vanished, edge
    // auto-scroll, then decide the insertion slot + place the caret (hidden in the tear-out zone —
    // the caret only ever promises a reorder).
    void TerminalPage::_TabReorderUpdate(const winrt::Windows::Foundation::Point& pos)
    {
        try
        {
            if (!_tabReorder.active || !_tabView)
            {
                return;
            }
            if (_TabReorderIndexOf(_tabReorder.tab) < 0)
            {
                _TabReorderCancel(L"tab closed mid-drag"); // e.g. the autorunner archived it under us
                return;
            }
            const double stripW = _tabView.ActualWidth();
            const double stripH = _tabView.ActualHeight();
            if (_tabStripScrollViewer)
            {
                const double step = Agentmaster::TabDragAutoScrollStep(pos.X, stripW, kTabDragEdgeBandDips, kTabDragScrollStepDips);
                if (step != 0.0)
                {
                    _tabStripScrollViewer.ChangeView(_tabStripScrollViewer.HorizontalOffset() + step, nullptr, nullptr, true);
                }
            }
            if (Agentmaster::ClassifyTabDragRelease(pos.X, pos.Y, stripW, stripH, kTabDragTearOutSlackDips) == Agentmaster::TabDragRelease::TearOut)
            {
                _tabReorder.lastSlot = -1; // a release here is a tear-out — or a DOCK into the window under the cursor
                _HideTabDragIndicator();
                _UpdateDockPreview(); // cross-window: light the insertion caret on the Agentmaster strip under the cursor (or clear it)
                return;
            }
            _ClearDockPreview(); // back over our own strip — a foreign window's caret must not linger
            const auto bands = _TabReorderBands();
            const int itemCount = static_cast<int>(_tabView.TabItems().Size());
            const int slot = Agentmaster::DecideTabInsertionSlot(bands, pos.X, itemCount);
            if (slot < 0)
            {
                _HideTabDragIndicator(); // nothing realized to decide against — keep the previous slot
                return;
            }
            // Slot 0 is a legal drop: no pinned tab reserves the front of the strip any more.
            _tabReorder.lastSlot = slot;
            double boundaryX{};
            if (Agentmaster::InsertionSlotBoundaryX(bands, slot, boundaryX))
            {
                _PositionTabDragIndicator(boundaryX);
            }
            else
            {
                _HideTabDragIndicator();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderUpdate");
        }
    }

    void TerminalPage::_TabReorderOnReleased(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if (e.Pointer().PointerId() != _tabReorder.pointerId)
            {
                return;
            }
            if (!_tabReorder.active)
            {
                if (_tabReorder.armed)
                {
                    _TabReorderReset(); // a plain click — selection already happened at press
                }
                return;
            }
            const auto pos = e.GetCurrentPoint(_tabView).Position();
            _TabReorderCommitRelease(pos);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnReleased");
            _TabReorderCancel(L"exception");
        }
    }

    // Commit the gesture: snapshot what the release needs, RESET FIRST (visuals/capture/flags —
    // the strip mutations below rebuild containers and must run outside the gesture state), then
    // reorder or tear out.
    void TerminalPage::_TabReorderCommitRelease(const winrt::Windows::Foundation::Point& pos)
    {
        const auto tab = _tabReorder.tab;
        const int slot = _tabReorder.lastSlot;
        const int originalIndex = _tabReorder.originalIndex;
        const auto dragOffset = _tabReorder.dragOffset;
        double stripW = 0.0;
        double stripH = 0.0;
        try
        {
            stripW = _tabView ? _tabView.ActualWidth() : 0.0;
            stripH = _tabView ? _tabView.ActualHeight() : 0.0;
        }
        CATCH_LOG();
        _TabReorderReset();
        if (!tab)
        {
            return;
        }
        const int cur = _TabReorderIndexOf(tab);
        if (cur < 0)
        {
            ::Agentmaster::LogNav(L"tab-drag-end (tab closed mid-drag)");
            return;
        }
        if (Agentmaster::ClassifyTabDragRelease(pos.X, pos.Y, stripW, stripH, kTabDragTearOutSlackDips) == Agentmaster::TabDragRelease::TearOut)
        {
            // Tear out into a NEW window through the exact native downstream. _sendDraggedTabToWindow
            // consumes _stashed.draggedTab (BuildStartupActions + [nav] tab-send-to-window +
            // _DetachClaudeTabForMove + _DetachTabFromWindow + _MoveContent + _RemoveTab, which nulls
            // the stash again) — the same path the native TabDroppedOutside took.
            winrt::com_ptr<Tab> tabImpl;
            tabImpl.copy_from(winrt::get_self<Tab>(tab));
            if (!tabImpl)
            {
                ::Agentmaster::LogNav(L"tab-drag-end (tear-out refused: not a terminal tab)");
                return;
            }
            // Cross-window DOCK (the retired OLE docking, reborn): released over ANOTHER Agentmaster
            // window's tab-strip row? WindowFromPoint is z-order-true (the topmost window at the
            // point), so an overlapped background window is never mis-targeted; our own window never
            // docks (a release over our OWN strip was classified Reorder above, and over our content
            // area it stays a tear-out — WindowFromPoint answers us and we exclude ourselves). The
            // target's probe answers synchronously (one Emperor UI thread) with its insertion slot;
            // the send then rides the EXACT native downstream — _sendDraggedTabToWindow ->
            // _MoveContent -> AppHost::_handleMoveContent routes by the numeric window id -> the
            // target's AttachContent lands the tab + moves it into the slot (its own _TryMoveTab
            // clamping it into range). Any failure falls through to the classic new-window
            // tear-out — never guess a drop.
            bool dockCommitted = false; // once the send started, a throw must NOT fall through to a SECOND send (the tab may be half-consumed)
            try
            {
                POINT screenPt{};
                if (_hostingHwnd && GetCursorPos(&screenPt))
                {
                    const HWND hitTop = GetAncestor(WindowFromPoint(screenPt), GA_ROOT);
                    if (hitTop && hitTop != *_hostingHwnd)
                    {
                        if (const auto probe = ::Agentmaster::FindWindowDockProbe(static_cast<void*>(hitTop)))
                        {
                            const auto hit = probe(screenPt.x, screenPt.y, ::Agentmaster::DockProbeMode::Query);
                            if (hit.slot >= 0 && hit.windowId != 0)
                            {
                                dockCommitted = true;
                                _stashed.draggedTab = tabImpl;
                                _stashed.dragOffset = dragOffset;
                                _sendDraggedTabToWindow(winrt::to_hstring(hit.windowId), static_cast<uint32_t>(hit.slot), std::nullopt);
                                ::Agentmaster::LogNav(L"tab-drag-end (docked into window " + std::to_wstring(hit.windowId) + L" at slot " + std::to_wstring(hit.slot) + L")");
                                SetForegroundWindow(hitTop); // attention follows the tab (a tear-out's new window foregrounds too); same-process, so permitted
                                return;
                            }
                        }
                    }
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_TabReorderCommitRelease dock");
                if (dockCommitted)
                {
                    // The dock's send chain threw mid-flight (the whole attach runs nested in this
                    // frame). The tab is in an unknown half-moved state — re-sending it to a new
                    // window could double-consume its content; leave it be and end the gesture.
                    ::Agentmaster::LogNav(L"tab-drag-end (dock send failed — tab left in place)");
                    return;
                }
                // The hit-test/probe itself failed — fall through to the classic tear-out.
            }
            _stashed.draggedTab = tabImpl;
            _stashed.dragOffset = dragOffset;
            // The native release-point recipe (_onTabDroppedOutside): position the new window so the
            // tab lands under the cursor; unavailable => default placement.
            std::optional<winrt::Windows::Foundation::Point> adjusted;
            try
            {
                if (const auto coreWindow = CoreWindow::GetForCurrentThread())
                {
                    const auto pointerPos = coreWindow.PointerPosition();
                    adjusted = winrt::Windows::Foundation::Point{ pointerPos.X - dragOffset.X, pointerPos.Y - dragOffset.Y };
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_TabReorderCommitRelease pointer-position"); // fall through — default window placement
            }
            _sendDraggedTabToWindow(winrt::hstring{ L"-1" }, 0, adjusted);
            ::Agentmaster::LogNav(L"tab-drag-end (torn out to a new window)");
            return;
        }
        if (slot < 0)
        {
            ::Agentmaster::LogNav(L"tab-drag-end (no same-window reorder)");
            return;
        }
        const int minIndex = 0; // no pinned tab owns index 0 (the Agent Manager is a page, not a tab)
        const int itemCount = static_cast<int>(_tabs.Size());
        const int target = Agentmaster::InsertionSlotToMoveTarget(slot, cur, minIndex, itemCount);
        if (target == cur)
        {
            ::Agentmaster::LogNav(L"tab-drag-end (no same-window reorder)");
            return;
        }
        _TryMoveTab(static_cast<uint32_t>(cur), target); // logs [nav] tab-move + UIA-announces + settles
        ::Agentmaster::LogNav(L"tab-drag-end from=" + std::to_wstring(originalIndex >= 0 ? originalIndex : cur) + L" to=" + std::to_wstring(target));
    }

    // External capture loss (alt-tab, a modal stealing the pointer, the window deactivating): the
    // gesture cannot continue — cancel WITHOUT mutating (a half-seen drag must never guess a drop).
    // Our own ReleasePointerCaptures in _TabReorderReset re-enters here, guarded by active==false
    // (reset clears the flags first).
    void TerminalPage::_TabReorderOnCaptureLost(const winrt::WUX::Input::PointerRoutedEventArgs& e)
    {
        try
        {
            if (!_tabReorder.active || e.Pointer().PointerId() != _tabReorder.pointerId)
            {
                return;
            }
            _TabReorderCancel(L"capture lost");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_TabReorderOnCaptureLost");
        }
    }

    void TerminalPage::_TabReorderCancel(const wchar_t* reason)
    {
        if (!_tabReorder.armed && !_tabReorder.active)
        {
            return;
        }
        const bool wasActive = _tabReorder.active;
        _TabReorderReset();
        if (wasActive)
        {
            // Caret-commit design: nothing moved during the gesture, so cancel == simply no move.
            ::Agentmaster::LogNav(std::wstring{ L"tab-drag-end (cancelled: " } + reason + L")");
        }
    }

    // Common teardown. Flags drop FIRST so the CaptureLost our own ReleasePointerCaptures raises
    // no-ops; every visual restore is individually guarded (a mid-teardown tree never blocks the
    // state clear).
    void TerminalPage::_TabReorderReset()
    {
        _tabReorder.armed = false;
        _tabReorder.active = false;
        _tabReorder.lastSlot = -1;
        _tabReorder.originalIndex = -1;
        _tabReorder.pointerId = 0;
        _tabDragAutoScrollTimer.Stop();
        _tabReorderKeyRevoker.revoke();
        try
        {
            if (_tabReorder.tvi)
            {
                _tabReorder.tvi.Opacity(1.0);
            }
        }
        CATCH_LOG();
        _HideTabDragIndicator();
        _ClearDockPreview(); // a foreign window's previewed caret must not outlive the gesture
        try
        {
            if (_tabView)
            {
                _tabView.ReleasePointerCaptures();
            }
        }
        CATCH_LOG();
        _tabReorder.tab = { nullptr };
        _tabReorder.tvi = { nullptr };
    }

    // The tab's CURRENT index in _tabs (identity compare), -1 when it is gone — indexes are
    // re-resolved at every decision point because tabs open/close/move under the gesture (the
    // autorunner, another window's cross-window move, …).
    int TerminalPage::_TabReorderIndexOf(const winrt::TerminalApp::Tab& tab) const
    {
        if (!tab || !_tabs)
        {
            return -1;
        }
        uint32_t idx{};
        if (_tabs.IndexOf(tab, idx))
        {
            return static_cast<int>(idx);
        }
        return -1;
    }

    // The realized, on-strip header bands in TabView coordinates — the gesture's whole world.
    // Virtualized-out tabs have no container and therefore NO band (never touched — the exact
    // hazard that killed the native drag is structurally absent here).
    std::vector<::Agentmaster::TabDragBand> TerminalPage::_TabReorderBands()
    {
        std::vector<::Agentmaster::TabDragBand> bands;
        if (!_tabView)
        {
            return bands;
        }
        const double stripW = _tabView.ActualWidth();
        const auto n = _tabView.TabItems().Size();
        bands.reserve(32);
        for (uint32_t i = 0; i < n; ++i)
        {
            const auto container = _tabView.ContainerFromIndex(i).try_as<winrt::MUX::Controls::TabViewItem>();
            if (!container)
            {
                continue; // virtualized out
            }
            const double w = container.ActualWidth();
            if (w <= 0.0)
            {
                continue; // realized but not laid out yet
            }
            winrt::Windows::Foundation::Point origin{};
            try
            {
                origin = container.TransformToVisual(_tabView).TransformPoint({ 0.0f, 0.0f });
            }
            catch (...)
            {
                continue; // expected control flow: a container detached mid-recycle has no transform (hot per-move loop — deliberately unlogged)
            }
            const double left = origin.X;
            const double right = left + w;
            if (right < 0.0 || left > stripW)
            {
                continue; // realized (cache buffer) but off the visible strip
            }
            bands.push_back({ static_cast<int>(i), left, right });
        }
        return bands; // ascending index == ascending x on the LTR strip
    }

    // The insertion caret: a thin accent bar parented into the TABVIEW'S TEMPLATE ROOT GRID —
    // deliberately NOT Root(): with showTabsInTitlebar (the default) the tab row is REMOVED from
    // Root() and re-homed into the titlebar (TerminalPage::Create -> SetTitleBarContent), so a
    // Root()-hosted caret would position against a collapsed row in a different subtree. The
    // template root travels WITH the TabView in both hosting modes, and coordinates stay local
    // (the root fills the TabView, so TabView-x == host-x up to the identity transform, which we
    // still apply for exactness). Root() is the fallback when the template isn't realized yet.
    // Created lazily; positioned only from pointer/timer code — never from a layout callback (the
    // E_LAYOUTCYCLE class). IsHitTestVisible(false) so it can never eat the release click.
    void TerminalPage::_EnsureTabDragIndicator()
    {
        try
        {
            // Resolve the host panel (re-resolved if the previous host went away with a template
            // reapply — the caret is re-parented rather than left dangling in a dead subtree).
            winrt::WUX::Controls::Panel host{ nullptr };
            if (_tabView && winrt::WUX::Media::VisualTreeHelper::GetChildrenCount(_tabView) > 0)
            {
                host = winrt::WUX::Media::VisualTreeHelper::GetChild(_tabView, 0).try_as<winrt::WUX::Controls::Panel>();
            }
            if (!host)
            {
                host = Root();
            }
            if (_tabDragIndicator && _tabDragIndicatorHost == host)
            {
                return; // already built + still parented right
            }
            if (_tabDragIndicator && _tabDragIndicatorHost)
            {
                // host changed (template reapplied) — move the caret
                uint32_t idx{};
                if (_tabDragIndicatorHost.Children().IndexOf(_tabDragIndicator, idx))
                {
                    _tabDragIndicatorHost.Children().RemoveAt(idx);
                }
                _tabDragIndicator = nullptr;
            }
            winrt::WUX::Controls::Border bar;
            bar.Width(3.0);
            bar.HorizontalAlignment(winrt::WUX::HorizontalAlignment::Left);
            bar.VerticalAlignment(winrt::WUX::VerticalAlignment::Top);
            bar.CornerRadius(winrt::WUX::CornerRadius{ 1.5, 1.5, 1.5, 1.5 });
            bar.IsHitTestVisible(false);
            bar.Visibility(winrt::WUX::Visibility::Collapsed);
            auto accent = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3B, 0x82, 0xF6); // fallback blue
            try
            {
                if (const auto res = winrt::WUX::Application::Current().Resources().TryLookup(winrt::box_value(L"SystemAccentColor")))
                {
                    accent = winrt::unbox_value_or<winrt::Windows::UI::Color>(res, accent);
                }
            }
            CATCH_LOG();
            bar.Background(winrt::WUX::Media::SolidColorBrush{ accent });
            winrt::WUX::Controls::Canvas::SetZIndex(bar, 1000); // above the strip chrome
            host.Children().Append(bar);
            _tabDragIndicator = bar;
            _tabDragIndicatorHost = host;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_EnsureTabDragIndicator"); // gesture still works, just caret-less
        }
    }

    void TerminalPage::_PositionTabDragIndicator(double xInTabView)
    {
        _EnsureTabDragIndicator();
        if (!_tabDragIndicator || !_tabDragIndicatorHost || !_tabView)
        {
            return;
        }
        try
        {
            const auto origin = _tabView.TransformToVisual(_tabDragIndicatorHost).TransformPoint({ static_cast<float>(xInTabView), 0.0f });
            _tabDragIndicator.Height(std::max(8.0, _tabView.ActualHeight()));
            _tabDragIndicator.Margin(winrt::WUX::Thickness{ origin.X - 1.5, origin.Y, 0.0, 0.0 });
            _tabDragIndicator.Visibility(winrt::WUX::Visibility::Visible);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PositionTabDragIndicator");
        }
    }

    void TerminalPage::_HideTabDragIndicator()
    {
        try
        {
            if (_tabDragIndicator)
            {
                _tabDragIndicator.Visibility(winrt::WUX::Visibility::Collapsed);
            }
        }
        CATCH_LOG();
    }

    // ======================================================================================
    // Agentmaster: cross-window tab DOCKING — the pointer-owned gesture's replacement for the
    // OLE docking that died with CanDragTabs(false). SOURCE side: while the drag rides the
    // tear-out zone, _UpdateDockPreview resolves the top-level window under the cursor
    // (WindowFromPoint — z-order-true) to a registered dock target (Engine::FindWindowDockProbe)
    // and has it light ITS OWN insertion caret (Preview); the release re-probes (Query) and sends
    // the tab through the exact native downstream (_sendDraggedTabToWindow with the target's
    // numeric window id). TARGET side: _DockProbeHit maps the screen point into its strip row
    // (screen -> client px -> island DIPs -> TabView-local) and answers the midpoint-rule
    // insertion slot over its realized bands — the same TabDragMath the local caret uses, so a
    // dock and a local reorder can never disagree about what a boundary means. Everything runs on
    // the ONE Emperor UI thread (WindowEmperor::CreateNewWindow — the same fact the native OLE
    // receive path relied on); every entry is guarded so any failure reads as a MISS, degrading
    // to the classic new-window tear-out (never guess a drop).
    // ======================================================================================

    ::Agentmaster::DockProbeResult TerminalPage::_DockProbeHit(long screenX, long screenY, ::Agentmaster::DockProbeMode mode)
    {
        ::Agentmaster::DockProbeResult miss{};
        try
        {
            if (mode == ::Agentmaster::DockProbeMode::HideCaret)
            {
                _HideTabDragIndicator();
                return miss;
            }
            if (!_tabView || !_hostingHwnd)
            {
                return miss;
            }
            POINT client{ screenX, screenY };
            if (!ScreenToClient(*_hostingHwnd, &client))
            {
                return miss;
            }
            const auto xamlRoot = _tabView.XamlRoot();
            if (!xamlRoot)
            {
                return miss;
            }
            const double scale = xamlRoot.RasterizationScale();
            if (scale <= 0.0)
            {
                return miss;
            }
            // The XAML island fills the client area at (0,0), so client px / scale == island-root DIPs.
            const double rootX = client.x / scale;
            const double rootY = client.y / scale;
            const auto origin = _tabView.TransformToVisual(nullptr).TransformPoint({ 0.0f, 0.0f });
            const double localX = rootX - origin.X;
            const double localY = rootY - origin.Y;
            const double stripH = _tabView.ActualHeight();
            // The DOCK zone is the whole tab-bar ROW: y against the strip band with a tight slack —
            // x is deliberately NOT tested (the window hit is already established by WindowFromPoint,
            // so a drop on the titlebar area right of the tabs reads as an APPEND, not a surprise new
            // window on top of the target). A collapsed/degenerate strip (focus mode) is no target.
            if (stripH < 8.0 || localY < -kTabDockStripSlackDips || localY > stripH + kTabDockStripSlackDips)
            {
                if (mode == ::Agentmaster::DockProbeMode::Preview)
                {
                    _HideTabDragIndicator(); // over this window, but not its strip row
                }
                return miss;
            }
            const auto bands = _TabReorderBands();
            const int itemCount = static_cast<int>(_tabView.TabItems().Size());
            int slot = Agentmaster::DecideTabInsertionSlot(bands, localX, itemCount);
            if (slot < 0)
            {
                slot = itemCount; // over the strip but nothing realized to decide against -> append
            }
            // (No floor: the target window's front slot is free — its Agent Manager is a page, not a tab.)
            if (mode == ::Agentmaster::DockProbeMode::Preview)
            {
                double boundaryX{};
                if (Agentmaster::InsertionSlotBoundaryX(bands, slot, boundaryX))
                {
                    _PositionTabDragIndicator(boundaryX);
                }
                else
                {
                    _HideTabDragIndicator(); // an append with no realized bands — nothing to anchor the caret to
                }
            }
            return { slot, static_cast<unsigned long long>(_WindowProperties.WindowId()) };
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_DockProbeHit"); // a torn tree reads as a miss
            return miss;
        }
    }

    void TerminalPage::_UpdateDockPreview()
    {
        try
        {
            POINT screenPt{};
            if (_hostingHwnd && GetCursorPos(&screenPt))
            {
                const HWND hitTop = GetAncestor(WindowFromPoint(screenPt), GA_ROOT);
                if (hitTop && hitTop != *_hostingHwnd)
                {
                    if (const auto probe = ::Agentmaster::FindWindowDockProbe(static_cast<void*>(hitTop)))
                    {
                        if (_dockHoverHwnd && _dockHoverHwnd != static_cast<void*>(hitTop))
                        {
                            _ClearDockPreview(); // switched targets — the old one's caret goes out first
                        }
                        probe(screenPt.x, screenPt.y, ::Agentmaster::DockProbeMode::Preview);
                        _dockHoverHwnd = static_cast<void*>(hitTop);
                        return;
                    }
                }
            }
            _ClearDockPreview(); // over nothing dockable (own window / foreign app / empty desktop)
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_UpdateDockPreview");
        }
    }

    void TerminalPage::_ClearDockPreview()
    {
        if (!_dockHoverHwnd)
        {
            return;
        }
        try
        {
            if (const auto probe = ::Agentmaster::FindWindowDockProbe(_dockHoverHwnd))
            {
                probe(0, 0, ::Agentmaster::DockProbeMode::HideCaret);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ClearDockPreview");
        }
        _dockHoverHwnd = nullptr;
    }

    // Agentmaster (M10 Increment 3): TerminalWindow hands us the record id it resolved from the
    // Emperor's -s <idx> (multi-window reopen), so _InitAgentmasterEngine claims THAT record (its
    // geometry already applied by TerminalWindow) instead of the front one. Must be set before
    // _OnFirstLayout. Empty => single-window (claim the front record).
    // Agentmaster (the RESTORE WAKE — PERSISTENCE.md §14): the same call enters this window in the wake barrier. It runs at
    // window CREATION, and the Emperor creates every reopened window before the first one lays out, so each of them waits
    // in the barrier before any restore ends — the first window to finish cannot wake its busy tabs while another window
    // is still restoring. _AnnounceRestoreWake never throws.
    void TerminalPage::SetAgentmasterWindowId(winrt::hstring windowId)
    {
        _assignedWindowId = windowId;
        if (!windowId.empty())
        {
            _AnnounceRestoreWake(std::wstring{ windowId });
        }
    }

    // Agentmaster: TerminalWindow flags a window built from MOVED content (tab tear-out into a new
    // window, or a cross-window tab move) BEFORE _OnFirstLayout, so _InitAgentmasterEngine mints a
    // fresh window record for it instead of front-popping a leftover one (which would drop the moved
    // session — see _isContentWindow + the claim block in _InitAgentmasterEngine).
    void TerminalPage::SetAgentmasterContentWindow(bool isContentWindow)
    {
        _isContentWindow = isContentWindow;
    }
}
