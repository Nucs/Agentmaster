// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the update PROMPT WINDOW (doc/agentmaster/UPDATER.md "The prompt window").
//
// A PART of Updater.h, which includes it at its very end: include Updater.h, never this file. ShowUpdatePrompt asks this
// window first; the Win32 TaskDialog it replaced is its fallback (the window could not be built, or
// AGENTMASTER_UPDATE_PROMPT=taskdialog).
//
// What it is: one dark, resizable window of our own (a window class and a modal loop, not a dialog template).
//   - LEFT, the release notes: a WebView2 showing our own HTML, rendered from each release's Markdown by cmark
//     (UpdaterReleaseNotes.h). A tab per release on the user's update channel, oldest to newest, older ones dimmed, the
//     installed version under a "you" arrow, every update colored. The offered release shows at once; the others arrive
//     from a worker that reads GitHub's release list while the window is already up.
//   - RIGHT, today's prompt word for word (the TaskDialog's strings and ids): the information icon, the main instruction,
//     the text, the six postpone radios; below, a band with "Release page →", "Update now" and "Postpone".
//   - DARK always, like every Agentmaster surface: a dark title bar (DWM), dark Win32 controls (Windows' DarkMode_Explorer
//     theme for the buttons and scroll bars; the radios custom-drawn, because Windows draws a themed radio's text black
//     whatever color it is given). High Contrast switches the whole window to Windows' system colors instead.
//   - WITHOUT WebView2 (no runtime on this PC, a thread that is no single-threaded COM apartment, a start that fails or
//     takes over 15 s, a WebView2 process that stops, AGENTMASTER_UPDATE_PROMPT=plain), the notes column shows the notes
//     as plain text in a read-only box: the same window, the same choices.
// Threading: everything runs on the caller's thread inside the window's own modal loop (the startup's main thread, the
// hourly check's worker, the cog's UI thread — the TaskDialog ran there too). WebView2's callbacks come through that loop.
// The release-list worker never touches a window: it leaves its result in a mailbox and posts one message.

#pragma once

#ifndef AGENTMASTER_UPDATER_PARTS
#error "UpdaterPromptWindow.h is a part of Updater.h: include Updater.h instead"
#endif

// WebView2's headers come from the WebView2 SDK NuGet package that the repository already restores for WinUI
// (packages\Microsoft.Web.WebView2.<version>\build\native\include); TerminalAppLib, the WindowsTerminal EXE and the m5 harness
// put that folder on their include path and link its static loader (WebView2LoaderStatic.lib) — the projects in their
// .vcxproj, the harness in run-m5-tests.bat when the package is there. Without the headers, the window still works and
// shows the notes as plain text.
#if __has_include(<WebView2.h>)
#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>
#define AGENTMASTER_UPDATER_HAVE_WEBVIEW2 1
#else
#define AGENTMASTER_UPDATER_HAVE_WEBVIEW2 0
#endif

#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <mutex>
#include <optional>

// System libraries only: each is in every Windows SDK, so a project that includes Updater.h without ever showing the
// prompt still links. The WebView2 loader is NOT pulled in this way — a project without the package's library folder
// would fail to link — but by the projects that show the prompt.
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

namespace Agentmaster::Updater
{
    // ============================ the switch ============================

    // AGENTMASTER_UPDATE_PROMPT: "taskdialog" shows the old TaskDialog instead of this window; "plain" shows this window with
    // the notes as plain text (the no-WebView2 path, for QA). Unset or anything else: this window with WebView2. An
    // environment variable rather than a setting: it exists for testing and for a user stuck with a broken WebView2.
    inline constexpr const wchar_t* kPromptModeEnvVar = L"AGENTMASTER_UPDATE_PROMPT";

    // ============================ the layout (pure) ============================

    // The layout's fixed measures, in device-independent pixels (96 DPI), scaled by PromptScale.
    inline constexpr int kPromptRightColumnDips = 344; // the prompt's column: today's text wraps as the TaskDialog wrapped it
    inline constexpr int kPromptMarginDips = 16;
    inline constexpr int kPromptGapDips = 16; // between the notes and the prompt's column
    inline constexpr int kPromptBandDips = 54; // the button band at the bottom
    inline constexpr int kPromptIconDips = 32; // the information icon, as the TaskDialog draws it
    inline constexpr int kPromptButtonHeightDips = 30;
    inline constexpr int kPromptButtonMinWidthDips = 100;
    inline constexpr int kPromptNotesMinWidthDips = 380; // the narrowest notes column the window can be shrunk to
    inline constexpr int kPromptNotesMinHeightDips = 220;
    inline constexpr int kPromptDefaultClientWidthDips = 1040; // the size the window opens at (capped by the screen)
    inline constexpr int kPromptDefaultClientHeightDips = 640;

    // A measure in DIPs at a DPI.
    //   dips    — the measure at 96 DPI.
    //   dpi     — the window's DPI (0 reads as 96).
    //   returns — pixels, rounded the way MulDiv rounds.
    //   throws  — nothing.
    inline int PromptScale(int dips, UINT dpi) noexcept
    {
        return ::MulDiv(dips, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
    }

    // The width the main instruction wraps at: the column minus the icon and its gap (the instruction sits beside the icon).
    //   dpi     — the window's DPI.
    //   returns — pixels.
    //   throws  — nothing.
    inline int PromptInstructionWidth(UINT dpi) noexcept
    {
        return PromptScale(kPromptRightColumnDips - kPromptIconDips - 10, dpi);
    }

    // The width the prompt's text and radios wrap at: the whole column.
    //   dpi     — the window's DPI.
    //   returns — pixels.
    //   throws  — nothing.
    inline int PromptContentWidth(UINT dpi) noexcept
    {
        return PromptScale(kPromptRightColumnDips, dpi);
    }

    // What the layout needs to know: the client size and the text measured at the DPI.
    struct PromptLayoutMetrics
    {
        UINT dpi{ 96 };
        int clientWidth{ 0 };
        int clientHeight{ 0 };
        int instructionHeight{ 0 }; // the main instruction wrapped at PromptInstructionWidth
        int contentHeight{ 0 }; // the text wrapped at PromptContentWidth
        int radioHeight{ 0 }; // one radio row
        int radioCount{ 0 };
        std::vector<int> buttonWidths; // in display order, left to right
        int buttonHeight{ 0 };
        int linkWidth{ 0 };
        int linkHeight{ 0 };
    };

    // Where everything goes, in client pixels.
    struct PromptLayout
    {
        RECT notes{}; // the release notes column (the WebView2 or the plain-text box, inside a 1-pixel frame)
        RECT icon{};
        RECT instruction{};
        RECT content{};
        std::vector<RECT> radios;
        RECT band{}; // the button band, the full width at the bottom
        std::vector<RECT> buttons; // in display order
        RECT link{}; // "Release page →", at the band's left
        int minClientWidth{ 0 }; // the smallest client the window allows (WM_GETMINMAXINFO)
        int minClientHeight{ 0 };
    };

    // Lay the window out. Pure, so the harness checks it at any size and DPI.
    //   m       — the measures.
    //   returns — the rectangles. The prompt's column keeps its width and sits at the right; the notes take everything else,
    //             so widening the window widens the notes. A client smaller than the minimum is laid out AT the minimum (the
    //             window clips the rest while a resize passes through) — controls never overlap.
    //   throws  — std::bad_alloc only.
    inline PromptLayout ComputePromptLayout(const PromptLayoutMetrics& m)
    {
        PromptLayout l;
        const UINT dpi = m.dpi == 0 ? 96 : m.dpi;
        const int margin = PromptScale(kPromptMarginDips, dpi);
        const int gap = PromptScale(kPromptGapDips, dpi);
        const int band = PromptScale(kPromptBandDips, dpi);
        const int column = PromptContentWidth(dpi);
        const int icon = PromptScale(kPromptIconDips, dpi);
        const int iconGap = PromptScale(10, dpi);
        const int textGap = PromptScale(12, dpi);
        const int radiosGap = PromptScale(14, dpi);
        const int radioSpacing = PromptScale(2, dpi);
        const int buttonGap = PromptScale(8, dpi);

        const int radioCount = (std::max)(0, m.radioCount);
        const int headerHeight = (std::max)(icon, m.instructionHeight);
        const int radiosHeight = radioCount > 0 ? radioCount * m.radioHeight + (radioCount - 1) * radioSpacing : 0;
        const int columnHeight = headerHeight + textGap + m.contentHeight + (radioCount > 0 ? radiosGap + radiosHeight : 0);
        l.minClientWidth = margin + PromptScale(kPromptNotesMinWidthDips, dpi) + gap + column + margin;
        l.minClientHeight = margin + (std::max)(columnHeight, PromptScale(kPromptNotesMinHeightDips, dpi)) + margin + band;

        const int width = (std::max)(m.clientWidth, l.minClientWidth);
        const int height = (std::max)(m.clientHeight, l.minClientHeight);
        const int bandTop = height - band;
        const int columnLeft = width - margin - column;

        l.band = RECT{ 0, bandTop, width, height };
        l.notes = RECT{ margin, margin, columnLeft - gap, bandTop - margin };
        l.icon = RECT{ columnLeft, margin, columnLeft + icon, margin + icon };
        // A one-line instruction is centred on the icon, as the TaskDialog does; a longer one starts at the icon's top.
        const int instructionTop = margin + (m.instructionHeight < icon ? (icon - m.instructionHeight) / 2 : 0);
        l.instruction = RECT{ columnLeft + icon + iconGap, instructionTop, columnLeft + column, instructionTop + m.instructionHeight };
        const int contentTop = margin + headerHeight + textGap;
        l.content = RECT{ columnLeft, contentTop, columnLeft + column, contentTop + m.contentHeight };
        int y = l.content.bottom + radiosGap;
        for (int i = 0; i < radioCount; ++i)
        {
            l.radios.push_back(RECT{ columnLeft, y, columnLeft + column, y + m.radioHeight });
            y += m.radioHeight + radioSpacing;
        }

        // The buttons sit right-aligned in the band, in display order; the link at its left.
        const int buttonTop = bandTop + (band - m.buttonHeight) / 2;
        l.buttons.resize(m.buttonWidths.size());
        int right = width - margin;
        for (size_t i = m.buttonWidths.size(); i-- > 0;)
        {
            l.buttons[i] = RECT{ right - m.buttonWidths[i], buttonTop, right, buttonTop + m.buttonHeight };
            right -= m.buttonWidths[i] + buttonGap;
        }
        const int linkTop = bandTop + (band - m.linkHeight) / 2;
        l.link = RECT{ margin, linkTop, margin + m.linkWidth, linkTop + m.linkHeight };
        return l;
    }

    namespace detail
    {
        // ============================ colors ============================

        // The window's colors. Dark by default — Agentmaster's surfaces are always dark (CLAUDE.md "Conventions"); in High
        // Contrast, Windows' own system colors, so the user's contrast theme wins over our palette.
        struct PromptPalette
        {
            COLORREF background{}; // the prompt's column
            COLORREF band{}; // the button band
            COLORREF notes{}; // behind the notes (and WebView2's background before its page paints)
            COLORREF border{}; // the notes' frame and the band's top line
            COLORREF text{};
            COLORREF dim{}; // secondary text: "Loading…", a disabled radio
            COLORREF accent{}; // the main instruction
            COLORREF link{};
            bool system{ false }; // true: High Contrast — no custom drawing, no dark themes
        };

        // Whether Windows runs a High Contrast theme.
        //   returns — true when HCF_HIGHCONTRASTON is set.
        //   throws  — nothing.
        inline bool IsHighContrastOn() noexcept
        {
            HIGHCONTRASTW hc{};
            hc.cbSize = sizeof(hc);
            return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        }

        // The palette for this run.
        //   highContrast — IsHighContrastOn().
        //   returns      — the dark palette (the Agent Manager's #2e2e2e family), or the system colors.
        //   throws       — nothing.
        inline PromptPalette MakePromptPalette(bool highContrast) noexcept
        {
            PromptPalette p;
            if (highContrast)
            {
                p.background = ::GetSysColor(COLOR_WINDOW);
                p.band = ::GetSysColor(COLOR_BTNFACE);
                p.notes = ::GetSysColor(COLOR_WINDOW);
                p.border = ::GetSysColor(COLOR_WINDOWTEXT);
                p.text = ::GetSysColor(COLOR_WINDOWTEXT);
                p.dim = ::GetSysColor(COLOR_GRAYTEXT);
                p.accent = ::GetSysColor(COLOR_WINDOWTEXT);
                p.link = ::GetSysColor(COLOR_HOTLIGHT);
                p.system = true;
                return p;
            }
            p.background = RGB(0x2e, 0x2e, 0x2e);
            p.band = RGB(0x25, 0x25, 0x25);
            p.notes = RGB(0x26, 0x26, 0x26);
            p.border = RGB(0x3c, 0x3c, 0x3c);
            p.text = RGB(0xe6, 0xe6, 0xe6);
            p.dim = RGB(0x8c, 0x8c, 0x8c);
            p.accent = RGB(0x8f, 0xd0, 0xff);
            p.link = RGB(0x4c, 0xc2, 0xff);
            return p;
        }

        // ============================ small helpers ============================

        // The module this code is linked into (the EXE or TerminalApp.dll): window classes are registered per module, so the
        // two copies of this header in one process never share — or fight over — a class.
        //   returns — the module handle; the process's EXE as a fallback.
        //   throws  — nothing.
        inline HINSTANCE PromptModule() noexcept
        {
            HMODULE self{};
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&PromptModule), &self);
            return self ? self : ::GetModuleHandleW(nullptr);
        }

        // Whether a link may be opened in the user's browser: http or https, and nothing a shell could read as a second
        // argument (whitespace, quotes, control characters). The notes are our own, but they arrive over the network.
        //   url     — the address.
        //   returns — true when it may be opened.
        //   throws  — nothing.
        inline bool IsWebAddress(std::wstring_view url) noexcept
        {
            const auto startsWith = [&](std::wstring_view prefix) {
                return url.size() > prefix.size() && EqualsNoCase(url.substr(0, prefix.size()), prefix);
            };
            if (!startsWith(L"https://") && !startsWith(L"http://"))
            {
                return false;
            }
            return std::none_of(url.begin(), url.end(), [](wchar_t c) { return c <= L' ' || c == L'"' || c == 0x7f; });
        }

        // A string WebView2 hands out (CoTaskMemAlloc'd), freed when this goes out of scope — so a copy that throws can never
        // leak it, and no path can free it twice.
        struct CoTaskString
        {
            LPWSTR text{ nullptr };

            CoTaskString() noexcept = default;
            ~CoTaskString()
            {
                if (text)
                {
                    ::CoTaskMemFree(text);
                }
            }
            CoTaskString(const CoTaskString&) = delete;
            CoTaskString& operator=(const CoTaskString&) = delete;

            // The address to pass as the out-parameter (the string must be empty: nothing is freed here).
            LPWSTR* put() noexcept
            {
                return &text;
            }

            // The text as a std::wstring ("" when none).
            //   throws — std::bad_alloc only.
            std::wstring str() const
            {
                return text ? std::wstring{ text } : std::wstring{};
            }
        };

        // An HRESULT as the [update] trail and the plain-text reason print it.
        //   hr      — the code.
        //   returns — e.g. "0x80070002".
        //   throws  — std::bad_alloc only.
        inline std::wstring HResultText(HRESULT hr)
        {
            wchar_t buffer[16]{};
            ::swprintf(buffer, ARRAYSIZE(buffer), L"0x%08lX", static_cast<unsigned long>(hr));
            return buffer;
        }

        // The update channel in words, for the console echo.
        //   prefs   — the user's opt-ins.
        //   returns — "stable", "stable and pre-release", "stable and nightly" or "stable, pre-release and nightly".
        //   throws  — std::bad_alloc only.
        inline std::wstring ChannelWords(const UpdatePrefs& prefs)
        {
            if (prefs.allowPrerelease && prefs.allowNightly)
            {
                return L"stable, pre-release and nightly";
            }
            if (prefs.allowPrerelease)
            {
                return L"stable and pre-release";
            }
            return prefs.allowNightly ? L"stable and nightly" : L"stable";
        }

        // The COM apartment the window needs for WebView2, for the call's duration. WebView2 runs only on a single-threaded
        // apartment: the startup's main thread already is one (WinRT's init_apartment), the cog's UI thread too, and the
        // hourly check's worker gets one here — undone when the prompt closes.
        struct PromptApartment
        {
            bool singleThreaded{ false }; // WebView2 may run on this thread
            bool balance{ false }; // CoInitializeEx succeeded (S_OK or S_FALSE): CoUninitialize is owed

            PromptApartment() noexcept
            {
                const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                balance = SUCCEEDED(hr);
                singleThreaded = balance;
                if (hr == RPC_E_CHANGED_MODE)
                {
                    // Already initialized, but not as an STA by this call: ask what it is (an application STA is fine too).
                    APTTYPE type{};
                    APTTYPEQUALIFIER qualifier{};
                    singleThreaded = SUCCEEDED(::CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
                }
            }

            ~PromptApartment()
            {
                if (balance)
                {
                    ::CoUninitialize();
                }
            }

            PromptApartment(const PromptApartment&) = delete;
            PromptApartment& operator=(const PromptApartment&) = delete;
        };

        // ============================ the release-list mailbox ============================

        // Where the worker that reads the release list leaves its result. The window reads it on its own thread when the
        // worker's message arrives; the worker checks `window` under the lock, so once the window is gone (null) nothing is
        // posted to an HWND that may already belong to another window.
        struct ReleaseNotesMailbox
        {
            std::mutex lock;
            HWND window{ nullptr }; // the prompt window while it exists; null once it is going away
            UINT message{ 0 }; // the registered message that says "a result is here"
            bool ready{ false };
            std::optional<ReleaseTimeline> timeline; // the full timeline (for the plain text)
            std::wstring modelJson; // the page's model, every release's notes already rendered (cmark ran on the worker)
        };

        class UpdatePromptWindow;

        // The WebView2 callbacks' only link to the window. WebView2 can complete an asynchronous call AFTER the prompt closed
        // (the user answered within the second WebView2 needs to start); the callbacks hold this, not the window, and do
        // nothing but clean up once `window` is null.
        struct PromptWebGuard
        {
            UpdatePromptWindow* window{ nullptr };
            int pending{ 0 }; // WebView2 calls still to complete (the environment, the controller): the closing drain waits for them
        };

        // ============================ the window ============================

        // The prompt window. Build it with Create, run it with RunModal (which also tears it down), let it go.
        class UpdatePromptWindow
        {
        public:
            // The window's own control ids. The buttons and radios keep the TaskDialog's ids, so ShowUpdatePrompt maps the
            // answer exactly as it maps the TaskDialog's.
            static constexpr int kIdNotes = 4001;
            static constexpr int kIdLink = 4002;
            static constexpr int kIdFallback = 4003;
            static constexpr int kIdInstruction = 4004;
            static constexpr int kIdContent = 4005;
            // The window classes (per module, see PromptModule).
            static constexpr const wchar_t* kPromptClass = L"AgentmasterUpdatePrompt";
            static constexpr const wchar_t* kNotesClass = L"AgentmasterUpdateNotes";
            // WebView2 must show the page within this long, or the notes become plain text. A first start on a PC builds its
            // profile folder and can take seconds; a hung runtime must not leave the column on "Loading…" forever.
            static constexpr UINT_PTR kTimerWebViewStart = 1;
            static constexpr UINT kWebViewStartTimeoutMs = 15000;
            // Closes a retired WebView2 view (RetireWebView): WM_APP is free here, the class is this window's own.
            static constexpr UINT kMsgRetireWebView = WM_APP + 1;
            // After the prompt closes, how long it keeps pumping messages for WebView2 calls still under way, so a browser
            // process started for it is closed rather than left running until the app exits.
            static constexpr DWORD kWebViewDrainMs = 3000;

            // Copy what the window shows; nothing is created yet.
            //   cfg            — the TaskDialog ShowUpdatePrompt built: its title, instruction, text, buttons and radios.
            //   info           — the check's result: the offered release and its notes.
            //   stateDir       — the profile: the [update] trail, the channel prefs, WebView2's data folder.
            //   webViewBlocked — why WebView2 may not be used here ("" = it may): the notes then start as plain text.
            //   throws         — std::bad_alloc only.
            UpdatePromptWindow(const TASKDIALOGCONFIG& cfg, const UpdateInfo& info, std::wstring stateDir, std::wstring webViewBlocked) :
                _info(info), _stateDir(std::move(stateDir)), _webViewBlocked(std::move(webViewBlocked))
            {
                _title = DialogEcho::FieldText(cfg.pszWindowTitle);
                _instruction = DialogEcho::FieldText(cfg.pszMainInstruction);
                _content = DialogEcho::FieldText(cfg.pszContent);
                for (UINT i = 0; i < cfg.cButtons && cfg.pButtons; ++i)
                {
                    _buttonSpecs.push_back(Choice{ cfg.pButtons[i].nButtonID, DialogEcho::FieldText(cfg.pButtons[i].pszButtonText) });
                }
                for (UINT i = 0; i < cfg.cRadioButtons && cfg.pRadioButtons; ++i)
                {
                    _radioSpecs.push_back(Choice{ cfg.pRadioButtons[i].nButtonID, DialogEcho::FieldText(cfg.pRadioButtons[i].pszButtonText) });
                }
                _defaultButton = cfg.nDefaultButton != 0 ? cfg.nDefaultButton : (_buttonSpecs.empty() ? IDOK : _buttonSpecs.front().id);
                _defaultRadio = cfg.nDefaultRadioButton != 0 ? cfg.nDefaultRadioButton : (_radioSpecs.empty() ? 0 : _radioSpecs.front().id);
                _radio = _defaultRadio;
                const std::wstring page = ChangelogUrl(info);
                _linkUrl = IsOwnReleasePage(page) ? page : ReleasePageForTag(info.latestTag);
                _prefs = ReadPrefs(_stateDir);
                _current = ParseVersion(info.currentVersionStr);
                _mailbox = std::make_shared<ReleaseNotesMailbox>();
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                _guard = std::make_shared<PromptWebGuard>();
                _guard->window = this;
#endif
            }

            // Tears down what RunModal did not (a Create that failed half-way) and frees the GDI objects.
            ~UpdatePromptWindow()
            {
                Teardown(false);
                FreeGdi();
                if (_bigIcon)
                {
                    ::DestroyIcon(_bigIcon);
                }
                if (_smallIcon)
                {
                    ::DestroyIcon(_smallIcon);
                }
            }

            UpdatePromptWindow(const UpdatePromptWindow&) = delete;
            UpdatePromptWindow& operator=(const UpdatePromptWindow&) = delete;

            // Build the window, hidden: its controls, its size and place, the first notes (the offered release), the worker
            // that reads the release list, and WebView2's start.
            //   owner  — the window the prompt belongs to (null at startup: it then gets its own taskbar button).
            //   whyNot — receives why the window could not be built.
            //   returns — true when it is ready for RunModal; false leaves nothing behind (the caller shows the TaskDialog).
            //   throws  — nothing.
            bool Create(HWND owner, std::wstring& whyNot) noexcept
            {
                try
                {
                    _owner = owner && ::IsWindow(owner) ? owner : nullptr;
                    _createdTick = ::GetTickCount64();
                    const HINSTANCE module = PromptModule();
                    if (!RegisterClasses(module))
                    {
                        whyNot = L"its window class could not be registered (error " + std::to_wstring(::GetLastError()) + L")";
                        return false;
                    }
                    _mailboxMessage = ::RegisterWindowMessageW(L"Agentmaster.UpdatePrompt.ReleaseNotes");
                    _palette = MakePromptPalette(IsHighContrastOn());
                    CreateBrushes();

                    // Created at the target monitor's work area, so its DPI is that monitor's from the start; sized and centred
                    // once the fonts and text are measured at that DPI.
                    const RECT work = TargetWorkArea();
                    _hwnd = ::CreateWindowExW(ExStyle(), kPromptClass, _title.c_str(), Style(), work.left + (work.right - work.left) / 3, work.top + (work.bottom - work.top) / 3, 400, 300,
                                              _owner, nullptr, module, this);
                    if (!_hwnd)
                    {
                        whyNot = L"CreateWindowEx failed (error " + std::to_wstring(::GetLastError()) + L")";
                        return false;
                    }
                    ApplyTitleBar();
                    SetWindowIcons();
                    _dpi = WindowDpi();
                    CreateFonts();
                    LoadInfoIcon();
                    OpenRadioTheme();
                    if (!CreateChildren(module))
                    {
                        whyNot = L"a control could not be created (error " + std::to_wstring(::GetLastError()) + L")";
                        Teardown(false);
                        return false;
                    }
                    _childrenReady = true;
                    Measure();
                    PlaceWindow(work);
                    Layout();

                    {
                        std::lock_guard<std::mutex> lock{ _mailbox->lock };
                        _mailbox->window = _hwnd;
                        _mailbox->message = _mailboxMessage;
                    }
                    // The offered release shows at once, from what the check already has; the worker brings the rest.
                    std::vector<ReleaseNote> offered;
                    offered.push_back(OfferedReleaseNote(_info));
                    _timeline = BuildReleaseTimeline(std::move(offered), _current);
                    _modelJson = BuildReleaseNotesModelJson(_timeline, _current, L"Loading the other releases\x2026");
                    StartReleaseListWorker();
                    StartNotes();
                    return true;
                }
                catch (...)
                {
                    whyNot = L"an exception while it was built";
                    Teardown(false);
                    return false;
                }
            }

            // Show the window and wait for the answer in its own modal loop (the owner is disabled meanwhile), then tear it
            // down. A WM_QUIT that arrives meanwhile answers Cancel and is posted again for the caller's loop.
            //   returns — the pressed button (IDCANCEL for Esc, ✕, Alt+F4 or a quit) and the selected radio.
            //   throws  — nothing.
            PromptWindowAnswer RunModal() noexcept
            {
                try
                {
                    if (_owner && ::IsWindowEnabled(_owner))
                    {
                        ::EnableWindow(_owner, FALSE);
                        _ownerDisabled = true;
                    }
                    ::ShowWindow(_hwnd, SW_SHOW);
                    ::UpdateWindow(_hwnd);
                    ::SetForegroundWindow(_hwnd);
                    if (const HWND first = DefaultButtonWindow())
                    {
                        ::SetFocus(first);
                    }
                    MSG msg{};
                    while (!_done)
                    {
                        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
                        if (got == 0)
                        {
                            _quitSeen = true;
                            _quitCode = static_cast<int>(msg.wParam);
                            RequestClose(IDCANCEL);
                            break;
                        }
                        if (got == -1)
                        {
                            RequestClose(IDCANCEL);
                            break;
                        }
                        // Keys meant for the notes page stay the page's: the dialog navigation would otherwise turn an arrow
                        // key between the version tabs into a radio change, and Enter on a tab into "Update now".
                        if (!IsForTheNotesPage(msg) && ::IsDialogMessageW(_hwnd, &msg))
                        {
                            continue;
                        }
                        ::TranslateMessage(&msg);
                        ::DispatchMessageW(&msg);
                    }
                }
                catch (...)
                {
                    _pressed = IDCANCEL;
                    try
                    {
                        LogUpdate(_stateDir, L"prompt window: the modal loop CRASHED (exception \x2014 answered Cancel)");
                    }
                    catch (...)
                    {
                        // logger-failed: nothing left to report through
                    }
                }
                Teardown(!_quitSeen);
                if (_quitSeen)
                {
                    ::PostQuitMessage(_quitCode);
                }
                return PromptWindowAnswer{ _pressed, _radio };
            }

            // The link the band shows, and where it leads (the offered release's page).
            //   returns — the text / the URL.
            //   throws  — std::bad_alloc only.
            std::wstring LinkText() const
            {
                return L"Release page \x2192";
            }
            const std::wstring& LinkUrl() const noexcept
            {
                return _linkUrl;
            }

            // The console echo's line about the notes.
            //   returns — e.g. "release notes: beside the choices, one tab per release on the stable channel, opened on v0.8.1".
            //   throws  — std::bad_alloc only.
            std::wstring NotesDescription() const
            {
                return L"release notes: beside the choices, one tab per release on the " + ChannelWords(_prefs) + L" channel, opened on " + _info.latestTag;
            }

            // How the notes started, for the [update] trail's "shown" line.
            //   returns — e.g. "notes: WebView2 140.0.3485.54 starting" or "notes: plain text (<why>)".
            //   throws  — std::bad_alloc only.
            std::wstring NotesStartText() const
            {
                if (_notesMode == NotesMode::Plain)
                {
                    return L"notes: plain text (" + _plainReason + L")";
                }
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                return L"notes: WebView2 " + _webviewVersion + L" starting, rendered by " + ReleaseNotesRendererName();
#else
                return L"notes: starting";
#endif
            }

        private:
            // One button or radio of the TaskDialog: its id and its label.
            struct Choice
            {
                int id{ 0 };
                std::wstring text;
            };

            // Where the notes column is.
            enum class NotesMode : uint8_t
            {
                Starting, // WebView2 is starting: the column shows "Loading the release notes…"
                Web, // the page said it is ready
                Plain, // the plain-text box (no WebView2, or it failed)
            };

            // ---- the window's style ----

            // The frame: a caption with ✕ and a sizing border, no minimize or maximize (a prompt, not an app window).
            static DWORD Style() noexcept
            {
                return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
            }

            // WS_EX_CONTROLPARENT lets the dialog navigation into nested containers; WS_EX_DLGMODALFRAME draws a dialog frame
            // (no caption icon). Without an owner (the startup prompt) the window gets its own taskbar button, so a prompt
            // that opened behind another window can be found.
            DWORD ExStyle() const noexcept
            {
                return WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | (_owner ? 0 : WS_EX_APPWINDOW);
            }

            // ---- creation ----

            // Register the two window classes in this module (once; a second call finds them).
            //   module  — PromptModule().
            //   returns — false when a class can neither be found nor registered.
            //   throws  — nothing.
            static bool RegisterClasses(HINSTANCE module) noexcept
            {
                const auto ensure = [module](const wchar_t* name, WNDPROC proc) noexcept {
                    WNDCLASSEXW found{};
                    found.cbSize = sizeof(found);
                    if (::GetClassInfoExW(module, name, &found))
                    {
                        return true;
                    }
                    WNDCLASSEXW wc{};
                    wc.cbSize = sizeof(wc);
                    wc.style = CS_HREDRAW | CS_VREDRAW;
                    wc.lpfnWndProc = proc;
                    wc.hInstance = module;
                    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
                    wc.hbrBackground = nullptr; // WM_ERASEBKGND paints: a class brush would flash light before it
                    wc.lpszClassName = name;
                    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
                };
                return ensure(kPromptClass, &UpdatePromptWindow::MainProc) && ensure(kNotesClass, &UpdatePromptWindow::NotesProc);
            }

            // The work area of the monitor the prompt opens on: its owner's, else the one under the mouse, else the primary.
            //   returns — the work area in screen coordinates.
            //   throws  — nothing.
            RECT TargetWorkArea() const noexcept
            {
                HMONITOR monitor = nullptr;
                if (_owner)
                {
                    monitor = ::MonitorFromWindow(_owner, MONITOR_DEFAULTTONEAREST);
                }
                else
                {
                    POINT cursor{};
                    monitor = ::GetCursorPos(&cursor) ? ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST) : ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                }
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                if (monitor && ::GetMonitorInfoW(monitor, &info))
                {
                    return info.rcWork;
                }
                RECT area{ 0, 0, 1280, 800 };
                ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
                return area;
            }

            // The window's DPI (96 when Windows cannot say).
            UINT WindowDpi() const noexcept
            {
                const UINT dpi = _hwnd ? ::GetDpiForWindow(_hwnd) : 0;
                return dpi == 0 ? 96 : dpi;
            }

            // A dark title bar, matching the body: DWMWA_USE_IMMERSIVE_DARK_MODE (attribute 20; 19 on Windows 10 builds before
            // 20H1), and on Windows 11 the caption's own colors (attributes 35 and 36; Windows 10 refuses them, harmlessly).
            // Numbers rather than the SDK's names, so an older SDK still builds. Not in High Contrast.
            void ApplyTitleBar() noexcept
            {
                if (_palette.system)
                {
                    return;
                }
                const BOOL on = TRUE;
                if (FAILED(::DwmSetWindowAttribute(_hwnd, 20, &on, sizeof(on))))
                {
                    ::DwmSetWindowAttribute(_hwnd, 19, &on, sizeof(on));
                }
                const COLORREF caption = _palette.background;
                const COLORREF captionText = _palette.text;
                ::DwmSetWindowAttribute(_hwnd, 35, &caption, sizeof(caption));
                ::DwmSetWindowAttribute(_hwnd, 36, &captionText, sizeof(captionText));
            }

            // The app's own icon for the taskbar and Alt+Tab (the process EXE's first icon; none in a test harness).
            void SetWindowIcons() noexcept
            {
                wchar_t exe[MAX_PATH * 2]{};
                if (::GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe)) == 0)
                {
                    return;
                }
                if (::ExtractIconExW(exe, 0, &_bigIcon, &_smallIcon, 1) == 0)
                {
                    return;
                }
                if (_bigIcon)
                {
                    ::SendMessageW(_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(_bigIcon));
                }
                if (_smallIcon)
                {
                    ::SendMessageW(_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(_smallIcon));
                }
            }

            // The brushes of the palette (owned: freed with the window).
            void CreateBrushes() noexcept
            {
                _backgroundBrush = ::CreateSolidBrush(_palette.background);
                _bandBrush = ::CreateSolidBrush(_palette.band);
                _notesBrush = ::CreateSolidBrush(_palette.notes);
                _borderBrush = ::CreateSolidBrush(_palette.border);
            }

            // The fonts at the window's DPI: Windows' message font (the TaskDialog's text font), a 4/3 larger one for the main
            // instruction (its 12 pt beside 9 pt), an underlined one for the link, and Consolas for the plain-text notes.
            // Replaces any earlier set: the new fonts reach the controls BEFORE the old ones are deleted, so no control ever
            // paints with a deleted font in between.
            void CreateFonts() noexcept
            {
                const HFONT oldFonts[] = { _font, _instructionFont, _linkFont, _monoFont };
                _font = _instructionFont = _linkFont = _monoFont = nullptr;
                NONCLIENTMETRICSW metrics{};
                metrics.cbSize = sizeof(metrics);
                LOGFONTW body{};
                if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, _dpi))
                {
                    body = metrics.lfMessageFont;
                }
                else
                {
                    body.lfHeight = -::MulDiv(9, static_cast<int>(_dpi), 72);
                    body.lfWeight = FW_NORMAL;
                    wcscpy_s(body.lfFaceName, L"Segoe UI");
                }
                _font = ::CreateFontIndirectW(&body);
                LOGFONTW instruction = body;
                instruction.lfHeight = ::MulDiv(body.lfHeight, 4, 3);
                instruction.lfWeight = FW_NORMAL;
                _instructionFont = ::CreateFontIndirectW(&instruction);
                LOGFONTW link = body;
                link.lfUnderline = TRUE;
                _linkFont = ::CreateFontIndirectW(&link);
                LOGFONTW mono = body;
                mono.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
                wcscpy_s(mono.lfFaceName, L"Consolas");
                _monoFont = ::CreateFontIndirectW(&mono);
                ApplyFonts();
                for (const HFONT old : oldFonts)
                {
                    if (old)
                    {
                        ::DeleteObject(old);
                    }
                }
            }

            // Hand the current fonts to the controls that exist.
            void ApplyFonts() noexcept
            {
                const auto set = [](HWND h, HFONT f) noexcept {
                    if (h && f)
                    {
                        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
                    }
                };
                set(_instructionCtl, _instructionFont);
                set(_contentCtl, _font);
                set(_linkCtl, _linkFont);
                set(_fallbackEdit, _monoFont);
                for (const HWND h : _radios)
                {
                    set(h, _font);
                }
                for (const HWND h : _buttons)
                {
                    set(h, _font);
                }
            }

            // The information icon at the DPI (the TaskDialog's TD_INFORMATION_ICON).
            void LoadInfoIcon() noexcept
            {
                if (_infoIcon)
                {
                    ::DestroyIcon(_infoIcon);
                    _infoIcon = nullptr;
                }
                const int size = PromptScale(kPromptIconDips, _dpi);
                if (FAILED(::LoadIconWithScaleDown(nullptr, IDI_INFORMATION, size, size, &_infoIcon)))
                {
                    _infoIcon = nullptr;
                }
            }

            // The page's font: the message font at 96 DPI, so its CSS pixels match the Win32 text beside it (WebView2 scales
            // CSS pixels by the monitor itself).
            ReleaseNotesPageStyle PageStyle() const
            {
                ReleaseNotesPageStyle style;
                NONCLIENTMETRICSW metrics{};
                metrics.cbSize = sizeof(metrics);
                if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, 96))
                {
                    style.fontFamily = metrics.lfMessageFont.lfFaceName;
                    const LONG height = metrics.lfMessageFont.lfHeight;
                    style.fontPx = static_cast<int>(height < 0 ? -height : height);
                }
                return style;
            }

            // The controls. Creation order is the Tab order: the notes, the radio group, the buttons, the link.
            //   module  — PromptModule().
            //   returns — false when any control could not be created.
            //   throws  — std::bad_alloc only.
            bool CreateChildren(HINSTANCE module)
            {
                const auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, void* param) noexcept {
                    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, _hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, param);
                };
                _notesHost = child(kNotesClass, L"Release notes", WS_TABSTOP | WS_CLIPCHILDREN, kIdNotes, this);
                _instructionCtl = child(WC_STATICW, _instruction.c_str(), SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kIdInstruction, nullptr);
                _contentCtl = child(WC_STATICW, _content.c_str(), SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kIdContent, nullptr);
                if (!_notesHost || !_instructionCtl || !_contentCtl)
                {
                    return false;
                }
                for (size_t i = 0; i < _radioSpecs.size(); ++i)
                {
                    // WS_GROUP + WS_TABSTOP on the first radio make the six one group: Tab enters it once, the arrow keys move
                    // inside it, and the auto radio style checks the one the arrows land on.
                    const HWND radio = child(WC_BUTTONW, _radioSpecs[i].text.c_str(), BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP | WS_TABSTOP : 0), _radioSpecs[i].id, nullptr);
                    if (!radio)
                    {
                        return false;
                    }
                    if (_radioSpecs[i].id == _defaultRadio)
                    {
                        ::SendMessageW(radio, BM_SETCHECK, BST_CHECKED, 0);
                    }
                    _radios.push_back(radio);
                }
                for (size_t i = 0; i < _buttonSpecs.size(); ++i)
                {
                    const DWORD kind = _buttonSpecs[i].id == _defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON;
                    const HWND button = child(WC_BUTTONW, _buttonSpecs[i].text.c_str(), kind | WS_TABSTOP | (i == 0 ? WS_GROUP : 0), _buttonSpecs[i].id, nullptr);
                    if (!button)
                    {
                        return false;
                    }
                    _buttons.push_back(button);
                }
                // The link is an owner-drawn BUTTON, not a SysLink: a button is a tab stop that Enter and Space press, and its
                // look is ours to draw in the dark band.
                const std::wstring linkText = LinkText();
                _linkCtl = child(WC_BUTTONW, linkText.c_str(), BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, kIdLink, nullptr);
                if (!_linkCtl)
                {
                    return false;
                }
                if (!_palette.system)
                {
                    // Windows' own dark control style (what Explorer uses in dark mode): dark push buttons, dark radio glyphs where
                    // the system has them. Not documented as an API, so a Windows without it simply keeps the light look.
                    for (const HWND h : _radios)
                    {
                        ::SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
                    }
                    for (const HWND h : _buttons)
                    {
                        ::SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
                    }
                }
                ApplyFonts();
                return true;
            }

            // Measure the text at the DPI: the wrapped heights, a radio row, the buttons' and the link's widths.
            void Measure() noexcept
            {
                const HDC dc = ::GetDC(_hwnd);
                if (!dc)
                {
                    return;
                }
                const auto wrappedHeight = [dc](HFONT font, const std::wstring& text, int width) noexcept {
                    RECT r{ 0, 0, width, 0 };
                    const HGDIOBJ old = ::SelectObject(dc, font);
                    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                    ::SelectObject(dc, old);
                    return static_cast<int>(r.bottom - r.top);
                };
                const auto lineWidth = [dc](HFONT font, const std::wstring& text) noexcept {
                    SIZE size{};
                    const HGDIOBJ old = ::SelectObject(dc, font);
                    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
                    ::SelectObject(dc, old);
                    return static_cast<int>(size.cx);
                };
                _instructionHeight = wrappedHeight(_instructionFont, _instruction, PromptInstructionWidth(_dpi));
                _contentHeight = wrappedHeight(_font, _content, PromptContentWidth(_dpi));
                TEXTMETRICW tm{};
                const HGDIOBJ old = ::SelectObject(dc, _font);
                ::GetTextMetricsW(dc, &tm);
                ::SelectObject(dc, old);
                const int line = static_cast<int>(tm.tmHeight);
                _radioHeight = (std::max)(line, static_cast<int>(RadioGlyphSize(dc).cy)) + PromptScale(6, _dpi);
                _buttonHeight = (std::max)(PromptScale(kPromptButtonHeightDips, _dpi), line + PromptScale(10, _dpi));
                _buttonWidths.clear();
                for (const auto& spec : _buttonSpecs)
                {
                    _buttonWidths.push_back((std::max)(PromptScale(kPromptButtonMinWidthDips, _dpi), lineWidth(_font, spec.text) + PromptScale(32, _dpi)));
                }
                _linkWidth = lineWidth(_linkFont, LinkText()) + PromptScale(8, _dpi);
                _linkHeight = line + PromptScale(6, _dpi);
                ::ReleaseDC(_hwnd, dc);
            }

            // Open the theme the radios' glyphs are drawn with, at the window's DPI: Windows' dark button style
            // ("DarkMode_Explorer::Button", what Explorer uses in dark mode), else the plain "Button" style. The window owns
            // this handle instead of borrowing the control's (GetWindowTheme): a radio whose check state just changed
            // repaints while the control has no theme handle to lend — measured in a Windows 11 desktop on 2026-10-29, the
            // two radios a click changed came back drawn as classic white circles while the other four stayed dark.
            // Not in High Contrast, where Windows draws the radios itself.
            void OpenRadioTheme() noexcept
            {
                if (_radioTheme)
                {
                    ::CloseThemeData(_radioTheme);
                    _radioTheme = nullptr;
                }
                if (_palette.system)
                {
                    return;
                }
                // No window: the handle is ours alone and changes no control's own theme.
                _radioTheme = ::OpenThemeDataForDpi(nullptr, L"DarkMode_Explorer::Button", _dpi);
                if (!_radioTheme)
                {
                    _radioTheme = ::OpenThemeDataForDpi(nullptr, L"Button", _dpi);
                }
            }

            // The radio glyph's size: the radio theme's part (DPI-aware), else 13 DIPs.
            SIZE RadioGlyphSize(HDC dc) const noexcept
            {
                SIZE size{ PromptScale(13, _dpi), PromptScale(13, _dpi) };
                if (_radioTheme)
                {
                    SIZE themed{};
                    if (SUCCEEDED(::GetThemePartSize(_radioTheme, dc, BP_RADIOBUTTON, RBS_UNCHECKEDNORMAL, nullptr, TS_DRAW, &themed)) && themed.cx > 0 && themed.cy > 0)
                    {
                        size = themed;
                    }
                }
                return size;
            }

            // The metrics ComputePromptLayout needs, for a client size.
            PromptLayoutMetrics Metrics(int clientWidth, int clientHeight) const
            {
                PromptLayoutMetrics m;
                m.dpi = _dpi;
                m.clientWidth = clientWidth;
                m.clientHeight = clientHeight;
                m.instructionHeight = _instructionHeight;
                m.contentHeight = _contentHeight;
                m.radioHeight = _radioHeight;
                m.radioCount = static_cast<int>(_radios.size());
                m.buttonWidths = _buttonWidths;
                m.buttonHeight = _buttonHeight;
                m.linkWidth = _linkWidth;
                m.linkHeight = _linkHeight;
                return m;
            }

            // Size and place the window: the default client size (at least the minimum, at most 92% of the work area),
            // centred on the owner when it is visible, else on the work area, and kept inside the work area.
            //   work — TargetWorkArea().
            void PlaceWindow(const RECT& work)
            {
                const PromptLayout minimum = ComputePromptLayout(Metrics(0, 0));
                const int clientWidth = (std::max)(PromptScale(kPromptDefaultClientWidthDips, _dpi), minimum.minClientWidth);
                const int clientHeight = (std::max)(PromptScale(kPromptDefaultClientHeightDips, _dpi), minimum.minClientHeight);
                RECT frame{ 0, 0, clientWidth, clientHeight };
                ::AdjustWindowRectExForDpi(&frame, Style(), FALSE, ExStyle(), _dpi);
                const int workWidth = work.right - work.left;
                const int workHeight = work.bottom - work.top;
                const int width = (std::min)(static_cast<int>(frame.right - frame.left), (std::max)(workWidth * 92 / 100, 1));
                const int height = (std::min)(static_cast<int>(frame.bottom - frame.top), (std::max)(workHeight * 92 / 100, 1));
                RECT around = work;
                if (_owner && ::IsWindowVisible(_owner) && !::IsIconic(_owner))
                {
                    ::GetWindowRect(_owner, &around);
                }
                int x = around.left + ((around.right - around.left) - width) / 2;
                int y = around.top + ((around.bottom - around.top) - height) / 2;
                x = (std::max)(static_cast<int>(work.left), (std::min)(x, static_cast<int>(work.right) - width));
                y = (std::max)(static_cast<int>(work.top), (std::min)(y, static_cast<int>(work.bottom) - height));
                ::SetWindowPos(_hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            }

            // Lay the controls out for the current client size and repaint.
            void Layout() noexcept
            {
                if (!_hwnd || !_childrenReady)
                {
                    return;
                }
                try
                {
                    RECT client{};
                    ::GetClientRect(_hwnd, &client);
                    _layout = ComputePromptLayout(Metrics(client.right, client.bottom));
                    const auto place = [](HWND h, const RECT& r) noexcept {
                        if (h)
                        {
                            ::SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
                        }
                    };
                    place(_notesHost, _layout.notes);
                    place(_instructionCtl, _layout.instruction);
                    place(_contentCtl, _layout.content);
                    for (size_t i = 0; i < _radios.size() && i < _layout.radios.size(); ++i)
                    {
                        place(_radios[i], _layout.radios[i]);
                    }
                    for (size_t i = 0; i < _buttons.size() && i < _layout.buttons.size(); ++i)
                    {
                        place(_buttons[i], _layout.buttons[i]);
                    }
                    place(_linkCtl, _layout.link);
                    ::InvalidateRect(_hwnd, nullptr, TRUE);
                }
                catch (...)
                {
                    // std::bad_alloc in the layout's vectors: the controls keep their places until the next size change.
                }
            }

            // ---- the window procedures ----

            // The prompt window's procedure: routes to OnMain. No exception may leave a window procedure (it would unwind
            // through user32's frames); a throw logs once and falls back to the default handling.
            static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
            {
                if (msg == WM_NCCREATE)
                {
                    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                }
                auto* self = reinterpret_cast<UpdatePromptWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (!self)
                {
                    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
                }
                try
                {
                    return self->OnMain(hwnd, msg, wParam, lParam);
                }
                catch (...)
                {
                    self->LogHandlerException(L"the prompt window");
                    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
                }
            }

            // The notes host's procedure: routes to OnNotes (the same no-throw rule).
            static LRESULT CALLBACK NotesProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
            {
                if (msg == WM_NCCREATE)
                {
                    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                }
                auto* self = reinterpret_cast<UpdatePromptWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (!self)
                {
                    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
                }
                try
                {
                    return self->OnNotes(hwnd, msg, wParam, lParam);
                }
                catch (...)
                {
                    self->LogHandlerException(L"the notes host");
                    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
                }
            }

            // Log a handler exception, once per window (a repeating paint must not flood hooks.log).
            void LogHandlerException(const wchar_t* where) noexcept
            {
                if (_handlerExceptionLogged)
                {
                    return;
                }
                _handlerExceptionLogged = true;
                try
                {
                    LogUpdate(_stateDir, std::wstring{ L"prompt window: a message handler of " } + where + L" CRASHED (exception \x2014 default handling)");
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
            }

            LRESULT OnMain(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
            {
                switch (msg)
                {
                case WM_ERASEBKGND:
                    PaintBackground(hwnd, reinterpret_cast<HDC>(wParam));
                    return 1;
                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    if (const HDC dc = ::BeginPaint(hwnd, &ps))
                    {
                        if (_infoIcon)
                        {
                            ::DrawIconEx(dc, _layout.icon.left, _layout.icon.top, _infoIcon, _layout.icon.right - _layout.icon.left, _layout.icon.bottom - _layout.icon.top, 0, nullptr, DI_NORMAL);
                        }
                        ::EndPaint(hwnd, &ps);
                    }
                    return 0;
                }
                case WM_CTLCOLORSTATIC:
                {
                    // The statics, and in High Contrast the radios (which color as statics); the instruction in the accent.
                    const HDC dc = reinterpret_cast<HDC>(wParam);
                    ::SetTextColor(dc, reinterpret_cast<HWND>(lParam) == _instructionCtl ? _palette.accent : _palette.text);
                    ::SetBkColor(dc, _palette.background);
                    return reinterpret_cast<LRESULT>(_backgroundBrush);
                }
                case WM_CTLCOLORBTN:
                    // The push buttons live in the band: their rounded corners show this brush.
                    return reinterpret_cast<LRESULT>(_bandBrush);
                case WM_NOTIFY:
                    if (const auto handled = OnNotify(lParam))
                    {
                        return *handled;
                    }
                    break;
                case WM_DRAWITEM:
                    if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam); item && item->hwndItem == _linkCtl)
                    {
                        PaintLink(*item);
                        return TRUE;
                    }
                    break;
                case WM_COMMAND:
                    OnCommand(static_cast<int>(LOWORD(wParam)), static_cast<int>(HIWORD(wParam)));
                    return 0;
                case DM_GETDEFID:
                    // IsDialogMessage asks the window for its default button when Enter is pressed: "Update now", like the
                    // TaskDialog.
                    return MAKELRESULT(_defaultButton, DC_HASDEFID);
                case WM_SETCURSOR:
                    if (reinterpret_cast<HWND>(wParam) == _linkCtl)
                    {
                        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                    break;
                case WM_GETMINMAXINFO:
                    if (_childrenReady)
                    {
                        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                        RECT frame{ 0, 0, _layout.minClientWidth, _layout.minClientHeight };
                        ::AdjustWindowRectExForDpi(&frame, Style(), FALSE, ExStyle(), _dpi);
                        info->ptMinTrackSize.x = frame.right - frame.left;
                        info->ptMinTrackSize.y = frame.bottom - frame.top;
                        return 0;
                    }
                    break;
                case WM_SIZE:
                    Layout();
                    return 0;
                case WM_MOVE:
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                    // WebView2 places its own popups (a tooltip, an IME window) from where it last saw its parent.
                    if (_controller)
                    {
                        _controller->NotifyParentWindowPositionChanged();
                    }
#endif
                    break;
                case WM_DPICHANGED:
                    OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
                    return 0;
                case WM_ACTIVATE:
                    OnActivate(LOWORD(wParam));
                    return 0;
                case WM_TIMER:
                    if (wParam == kTimerWebViewStart)
                    {
                        ::KillTimer(hwnd, kTimerWebViewStart);
                        if (_notesMode != NotesMode::Web)
                        {
                            ShowPlainNotes(L"WebView2 did not show the notes within " + std::to_wstring(kWebViewStartTimeoutMs / 1000) + L" seconds");
                        }
                        return 0;
                    }
                    break;
                case WM_CLOSE:
                    // ✕ and Alt+F4 answer Cancel ("Remind me next restart"); the loop ends, and RunModal destroys the window.
                    RequestClose(IDCANCEL);
                    return 0;
                case WM_DESTROY:
                    DetachMailbox();
                    return 0;
                case kMsgRetireWebView:
                    CloseRetiredWebView();
                    return 0;
                default:
                    if (_mailboxMessage != 0 && msg == _mailboxMessage)
                    {
                        OnReleaseNotesArrived();
                        return 0;
                    }
                    break;
                }
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
            }

            LRESULT OnNotes(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
            {
                switch (msg)
                {
                case WM_ERASEBKGND:
                {
                    const HDC dc = reinterpret_cast<HDC>(wParam);
                    RECT r{};
                    ::GetClientRect(hwnd, &r);
                    ::FillRect(dc, &r, _notesBrush);
                    ::FrameRect(dc, &r, _borderBrush);
                    return 1;
                }
                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    if (const HDC dc = ::BeginPaint(hwnd, &ps))
                    {
                        if (_notesMode == NotesMode::Starting)
                        {
                            RECT r{};
                            ::GetClientRect(hwnd, &r);
                            const HGDIOBJ old = ::SelectObject(dc, _font);
                            ::SetBkMode(dc, TRANSPARENT);
                            ::SetTextColor(dc, _palette.dim);
                            ::DrawTextW(dc, L"Loading the release notes\x2026", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                            ::SelectObject(dc, old);
                        }
                        ::EndPaint(hwnd, &ps);
                    }
                    return 0;
                }
                case WM_SIZE:
                    ResizeNotes();
                    return 0;
                case WM_SETFOCUS:
                    FocusNotes();
                    return 0;
                case WM_CTLCOLORSTATIC:
                case WM_CTLCOLOREDIT:
                    if (reinterpret_cast<HWND>(lParam) == _fallbackEdit)
                    {
                        // The read-only plain-text box colors as a static.
                        const HDC dc = reinterpret_cast<HDC>(wParam);
                        ::SetTextColor(dc, _palette.text);
                        ::SetBkColor(dc, _palette.notes);
                        return reinterpret_cast<LRESULT>(_notesBrush);
                    }
                    break;
                default:
                    break;
                }
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
            }

            // ---- painting ----

            // The prompt's column, the band below it and the band's top line.
            void PaintBackground(HWND hwnd, HDC dc) noexcept
            {
                RECT client{};
                ::GetClientRect(hwnd, &client);
                ::FillRect(dc, &client, _backgroundBrush);
                RECT band = _layout.band;
                if (band.bottom > band.top)
                {
                    ::FillRect(dc, &band, _bandBrush);
                    RECT line{ band.left, band.top, band.right, band.top + (std::max)(1, PromptScale(1, _dpi)) };
                    ::FillRect(dc, &line, _borderBrush);
                }
            }

            // A radio, drawn whole: the glyph through the radio's own theme (dark where Windows has a dark one), the label in
            // the palette's text color, the focus rectangle when the keyboard is in use.
            //   draw — the custom-draw request (CDDS_PREPAINT).
            void PaintRadio(const NMCUSTOMDRAW& draw) noexcept
            {
                const HWND radio = draw.hdr.hwndFrom;
                const HDC dc = draw.hdc;
                RECT rc = draw.rc;
                ::FillRect(dc, &rc, _backgroundBrush);
                const bool checked = ::SendMessageW(radio, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool disabled = (draw.uItemState & CDIS_DISABLED) != 0;
                const bool pressed = (draw.uItemState & CDIS_SELECTED) != 0;
                const bool hot = (draw.uItemState & CDIS_HOT) != 0;
                const int state = checked ? (disabled ? RBS_CHECKEDDISABLED : pressed ? RBS_CHECKEDPRESSED : hot ? RBS_CHECKEDHOT : RBS_CHECKEDNORMAL) :
                                            (disabled ? RBS_UNCHECKEDDISABLED : pressed ? RBS_UNCHECKEDPRESSED : hot ? RBS_UNCHECKEDHOT : RBS_UNCHECKEDNORMAL);
                const SIZE glyph = RadioGlyphSize(dc);
                const int top = rc.top + ((rc.bottom - rc.top) - glyph.cy) / 2;
                RECT glyphRect{ rc.left, top, rc.left + glyph.cx, top + glyph.cy };
                if (_radioTheme)
                {
                    ::DrawThemeBackground(_radioTheme, dc, BP_RADIOBUTTON, state, &glyphRect, nullptr);
                }
                else
                {
                    ::DrawFrameControl(dc, &glyphRect, DFC_BUTTON, DFCS_BUTTONRADIO | (checked ? DFCS_CHECKED : 0) | (disabled ? DFCS_INACTIVE : 0) | (pressed ? DFCS_PUSHED : 0));
                }
                wchar_t text[256]{};
                const int length = ::GetWindowTextW(radio, text, ARRAYSIZE(text));
                RECT textRect{ glyphRect.right + PromptScale(8, _dpi), rc.top, rc.right, rc.bottom };
                const HGDIOBJ old = ::SelectObject(dc, _font);
                ::SetBkMode(dc, TRANSPARENT);
                ::SetTextColor(dc, disabled ? _palette.dim : _palette.text);
                ::DrawTextW(dc, text, length, &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                // The focus rectangle hugs the label, as a themed radio draws it — only while keyboard cues are shown.
                const bool showFocus = (draw.uItemState & CDIS_FOCUS) != 0 && (::SendMessageW(radio, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
                if (showFocus)
                {
                    RECT measured = textRect;
                    ::DrawTextW(dc, text, length, &measured, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_CALCRECT);
                    const int textHeight = measured.bottom - measured.top;
                    RECT focus{ measured.left - 2, textRect.top + ((textRect.bottom - textRect.top) - textHeight) / 2 - 1, measured.right + 2, 0 };
                    focus.bottom = focus.top + textHeight + 2;
                    ::SetTextColor(dc, _palette.text);
                    ::DrawFocusRect(dc, &focus);
                }
                ::SelectObject(dc, old);
            }

            // The link button: its text in the link color, underlined, on the band; the focus rectangle when focused.
            //   item — the owner-draw request.
            void PaintLink(const DRAWITEMSTRUCT& item) noexcept
            {
                RECT rc = item.rcItem;
                ::FillRect(item.hDC, &rc, _bandBrush);
                const std::wstring text = LinkText();
                const HGDIOBJ old = ::SelectObject(item.hDC, _linkFont);
                ::SetBkMode(item.hDC, TRANSPARENT);
                ::SetTextColor(item.hDC, _palette.link);
                RECT textRect = rc;
                ::InflateRect(&textRect, -PromptScale(2, _dpi), 0);
                ::DrawTextW(item.hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
                if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0)
                {
                    ::SetTextColor(item.hDC, _palette.text);
                    ::DrawFocusRect(item.hDC, &rc);
                }
                ::SelectObject(item.hDC, old);
            }

            // NM_CUSTOMDRAW for the radios (dark only: High Contrast keeps Windows' drawing).
            //   lParam  — the WM_NOTIFY header.
            //   returns — the custom-draw answer, or nothing for any other notification.
            std::optional<LRESULT> OnNotify(LPARAM lParam) noexcept
            {
                const auto* header = reinterpret_cast<const NMHDR*>(lParam);
                if (!header || header->code != NM_CUSTOMDRAW || _palette.system || !IsRadio(header->hwndFrom))
                {
                    return std::nullopt;
                }
                const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
                if (draw->dwDrawStage == CDDS_PREPAINT)
                {
                    PaintRadio(*draw);
                    return static_cast<LRESULT>(CDRF_SKIPDEFAULT);
                }
                return static_cast<LRESULT>(CDRF_DODEFAULT);
            }

            // ---- input ----

            // A button, the link or Cancel.
            //   id   — the control id (IDCANCEL from Esc through IsDialogMessage).
            //   code — the notification (BN_CLICKED).
            void OnCommand(int id, int code) noexcept
            {
                if (id == IDCANCEL)
                {
                    RequestClose(IDCANCEL);
                    return;
                }
                if (code != BN_CLICKED)
                {
                    return;
                }
                if (id == kIdLink)
                {
                    OpenExternal(_linkUrl);
                    return;
                }
                // A radio's click is its own business (it checks itself); a button's answers the prompt.
                if (std::any_of(_buttonSpecs.begin(), _buttonSpecs.end(), [id](const Choice& c) { return c.id == id; }))
                {
                    RequestClose(id);
                }
            }

            // End the modal loop with an answer (the first answer wins).
            //   button — the pressed button's id, or IDCANCEL.
            void RequestClose(int button) noexcept
            {
                if (_done)
                {
                    return;
                }
                _pressed = button;
                _radio = CheckedRadio();
                _done = true;
                if (_hwnd)
                {
                    // Wakes GetMessage when this ran outside a dispatched message (a WebView2 callback can).
                    ::PostMessageW(_hwnd, WM_NULL, 0, 0);
                }
            }

            // The checked radio's id; the default one when none is checked.
            int CheckedRadio() const noexcept
            {
                for (size_t i = 0; i < _radios.size() && i < _radioSpecs.size(); ++i)
                {
                    if (::SendMessageW(_radios[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    {
                        return _radioSpecs[i].id;
                    }
                }
                return _defaultRadio;
            }

            // Whether a window is one of the radios.
            bool IsRadio(HWND h) const noexcept
            {
                return h && std::find(_radios.begin(), _radios.end(), h) != _radios.end();
            }

            // The default button's window.
            HWND DefaultButtonWindow() const noexcept
            {
                for (size_t i = 0; i < _buttons.size() && i < _buttonSpecs.size(); ++i)
                {
                    if (_buttonSpecs[i].id == _defaultButton)
                    {
                        return _buttons[i];
                    }
                }
                return _buttons.empty() ? _hwnd : _buttons.front();
            }

            // Whether a message is for a window inside the notes column other than the plain-text box (WebView2's own windows).
            bool IsForTheNotesPage(const MSG& msg) const noexcept
            {
                return _notesHost && msg.hwnd && msg.hwnd != _notesHost && msg.hwnd != _fallbackEdit && ::IsChild(_notesHost, msg.hwnd);
            }

            // Keep the keyboard where the user left it when the window loses and regains activation (a dialog template's
            // DefDlgProc does this; a window class of our own must).
            //   state — WA_ACTIVE, WA_CLICKACTIVE or WA_INACTIVE.
            void OnActivate(UINT state) noexcept
            {
                if (state == WA_INACTIVE)
                {
                    const HWND focus = ::GetFocus();
                    if (focus && ::IsChild(_hwnd, focus))
                    {
                        // Focus inside WebView2's own windows is restored through the host, which hands it back to the page.
                        _lastFocus = (focus != _fallbackEdit && _notesHost && ::IsChild(_notesHost, focus)) ? _notesHost : focus;
                    }
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                    else if (!focus && _controller)
                    {
                        _lastFocus = _notesHost; // the page had it (its window lives in the browser process)
                    }
#endif
                    return;
                }
                const HWND target = _lastFocus && ::IsWindow(_lastFocus) ? _lastFocus : DefaultButtonWindow();
                if (target)
                {
                    ::SetFocus(target);
                }
            }

            // A new DPI (another monitor, or a scale change): new fonts and icon, the measures again, the suggested rectangle.
            //   dpi       — the new DPI.
            //   suggested — Windows' suggested window rectangle.
            void OnDpiChanged(UINT dpi, const RECT* suggested) noexcept
            {
                _dpi = dpi == 0 ? 96 : dpi;
                CreateFonts();
                LoadInfoIcon();
                OpenRadioTheme();
                Measure();
                if (suggested)
                {
                    ::SetWindowPos(_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                Layout();
            }

            // Open a link in the user's browser, if it is an http(s) address (IsWebAddress); logged either way.
            //   url — the address.
            void OpenExternal(const std::wstring& url) noexcept
            {
                try
                {
                    if (!IsWebAddress(url))
                    {
                        LogUpdate(_stateDir, L"prompt window: a link was refused (not an http(s) address)");
                        return;
                    }
                    ::ShellExecuteW(_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    LogUpdate(_stateDir, L"prompt window: opened " + url);
                }
                catch (...)
                {
                    // std::bad_alloc building the log line: the link opened or not, nothing else depends on it
                }
            }

            // ---- the notes ----

            // Start the notes column: WebView2 when it may run here, else the plain text straight away.
            void StartNotes() noexcept
            {
                if (!_webViewBlocked.empty())
                {
                    ShowPlainNotes(_webViewBlocked);
                    return;
                }
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                StartWebView();
#else
                ShowPlainNotes(L"this build has no WebView2");
#endif
            }

            // Size the WebView2 or the plain-text box to the notes host, inside its 1-pixel frame.
            void ResizeNotes() noexcept
            {
                if (!_notesHost)
                {
                    return;
                }
                RECT inner{};
                ::GetClientRect(_notesHost, &inner);
                ::InflateRect(&inner, -1, -1);
                if (_fallbackEdit)
                {
                    ::SetWindowPos(_fallbackEdit, nullptr, inner.left, inner.top, inner.right - inner.left, inner.bottom - inner.top, SWP_NOZORDER | SWP_NOACTIVATE);
                }
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                if (_controller)
                {
                    _controller->put_Bounds(inner);
                }
#endif
            }

            // The keyboard arrives in the notes column (Tab, or a click on its frame): hand it to the page — to its first or
            // last control when Tab brought it, back to where it was otherwise — or to the plain-text box.
            void FocusNotes() noexcept
            {
                if (_fallbackEdit)
                {
                    ::SetFocus(_fallbackEdit);
                    return;
                }
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                if (_controller)
                {
                    const bool tabbing = (::GetKeyState(VK_TAB) & 0x8000) != 0;
                    const bool backwards = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    _controller->MoveFocus(tabbing ? (backwards ? COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS : COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT) :
                                                     COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
#endif
            }

            // Switch the notes column to the plain-text box, for good: WebView2 (if any) is closed, the box is created and filled
            // from the latest timeline, and the host stops being a tab stop so the box is one. Logged with the reason.
            //   reason — why ("the Microsoft Edge WebView2 Runtime is not installed").
            void ShowPlainNotes(const std::wstring& reason) noexcept
            {
                try
                {
                    if (_notesMode == NotesMode::Plain)
                    {
                        return;
                    }
                    const bool hadFocus = _notesHost && ::GetFocus() && (::GetFocus() == _notesHost || ::IsChild(_notesHost, ::GetFocus()));
                    _notesMode = NotesMode::Plain;
                    _plainReason = reason;
                    if (_hwnd)
                    {
                        ::KillTimer(_hwnd, kTimerWebViewStart);
                    }
                    RetireWebView();
                    if (_notesHost)
                    {
                        const auto style = static_cast<DWORD>(::GetWindowLongPtrW(_notesHost, GWL_STYLE));
                        ::SetWindowLongPtrW(_notesHost, GWL_STYLE, static_cast<LONG_PTR>(style & ~static_cast<DWORD>(WS_TABSTOP)));
                        const auto exStyle = static_cast<DWORD>(::GetWindowLongPtrW(_notesHost, GWL_EXSTYLE));
                        ::SetWindowLongPtrW(_notesHost, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle | WS_EX_CONTROLPARENT));
                        _fallbackEdit = ::CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                                                          0, 0, 0, 0, _notesHost, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdFallback)), PromptModule(), nullptr);
                        if (_fallbackEdit)
                        {
                            if (!_palette.system)
                            {
                                ::SetWindowTheme(_fallbackEdit, L"DarkMode_Explorer", nullptr); // a dark scroll bar
                            }
                            ::SendMessageW(_fallbackEdit, WM_SETFONT, reinterpret_cast<WPARAM>(_monoFont), FALSE);
                            ::SendMessageW(_fallbackEdit, EM_SETLIMITTEXT, 0, 0); // a multiline box's limit, else 32K characters
                            const int margin = PromptScale(8, _dpi);
                            ::SendMessageW(_fallbackEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
                            RefreshPlainText();
                            ResizeNotes();
                            if (hadFocus)
                            {
                                ::SetFocus(_fallbackEdit);
                            }
                        }
                        ::InvalidateRect(_notesHost, nullptr, TRUE);
                    }
                    LogUpdate(_stateDir, L"prompt window: notes as plain text (" + reason + L")");
                }
                catch (...)
                {
                    // std::bad_alloc: the column keeps what it shows; the prompt's choices are unaffected
                }
            }

            // Fill the plain-text box from the latest timeline.
            void RefreshPlainText() noexcept
            {
                try
                {
                    if (_fallbackEdit)
                    {
                        ::SetWindowTextW(_fallbackEdit, BuildReleaseNotesPlainText(_timeline, _plainReason).c_str());
                    }
                }
                catch (...)
                {
                    // std::bad_alloc: the box keeps its last text
                }
            }

            // The worker that reads the release list. It owns copies of everything it reads; it reaches the window only
            // through the mailbox.
            void StartReleaseListWorker() noexcept
            {
                try
                {
                    std::thread([mailbox = _mailbox, current = _current, prefs = _prefs, offered = OfferedReleaseNote(_info), stateDir = _stateDir]() mutable noexcept {
                        WorkReleaseList(mailbox, current, prefs, std::move(offered), stateDir);
                    }).detach();
                }
                catch (...)
                {
                    try
                    {
                        // No thread (or no memory for its copies): the offered release stays the only tab, and says why.
                        _modelJson = BuildReleaseNotesModelJson(_timeline, _current, L"Could not load the other releases (no worker thread).");
                        LogUpdate(_stateDir, L"release notes: no worker thread \x2014 the offered release only");
                    }
                    catch (...)
                    {
                        // std::bad_alloc again: the loading line stays; the choices are unaffected
                    }
                }
            }

            // The worker's body: read the list, build the timeline and the page's model (cmark runs here, off the window's
            // thread), log the outcome, and leave the result in the mailbox — unless the window is already gone.
            static void WorkReleaseList(const std::shared_ptr<ReleaseNotesMailbox>& mailbox, Version current, UpdatePrefs prefs, ReleaseNote offered, const std::wstring& stateDir) noexcept
            {
                try
                {
                    const ULONGLONG started = ::GetTickCount64();
                    ReleaseListFetch fetch = FetchReleaseNotesList(current, prefs.allowPrerelease, prefs.allowNightly, kReleaseNotesPageTimeoutMs);
                    const std::wstring offeredTag = offered.tag;
                    std::vector<ReleaseNote> all = std::move(fetch.releases);
                    all.push_back(std::move(offered)); // after the list's copies: the list's copy (with title and date) wins
                    ReleaseTimeline timeline = BuildReleaseTimeline(std::move(all), current);
                    const std::wstring status = fetch.ok ? std::wstring{} : L"Could not load the other releases (" + fetch.error + L"); this shows " + offeredTag + L" only.";
                    std::wstring model = BuildReleaseNotesModelJson(timeline, current, status);
                    const auto updates = std::count(timeline.roles.begin(), timeline.roles.end(), ReleaseRole::Update);
                    const std::wstring yours = timeline.youOnTab && timeline.youIndex < timeline.releases.size() ? timeline.releases[timeline.youIndex].tag : std::wstring{ L"not a release on the channel" };
                    LogUpdate(stateDir, fetch.ok ? L"release notes: " + std::to_wstring(timeline.releases.size()) + L" release(s) on the channel from " + std::to_wstring(fetch.pages) +
                                                       L" page(s), " + std::to_wstring(updates) + L" update(s), yours " + yours + L" (" +
                                                       std::to_wstring(::GetTickCount64() - started) + L" ms)" :
                                                   L"release notes: the list could not be loaded (" + fetch.error + L") \x2014 " + offeredTag + L" only");
                    std::lock_guard<std::mutex> lock{ mailbox->lock };
                    if (!mailbox->window)
                    {
                        return; // the prompt closed first: nobody to show it to
                    }
                    mailbox->timeline = std::move(timeline);
                    mailbox->modelJson = std::move(model);
                    mailbox->ready = true;
                    ::PostMessageW(mailbox->window, mailbox->message, 0, 0);
                }
                catch (...)
                {
                    try
                    {
                        LogUpdate(stateDir, L"release notes: the worker CRASHED (exception \x2014 the offered release only)");
                    }
                    catch (...)
                    {
                        // logger-failed: nothing left to report through
                    }
                }
            }

            // The worker's result arrived: take it and show it (the page gets the new model; the plain text is rebuilt).
            void OnReleaseNotesArrived() noexcept
            {
                try
                {
                    std::optional<ReleaseTimeline> timeline;
                    std::wstring model;
                    {
                        std::lock_guard<std::mutex> lock{ _mailbox->lock };
                        if (!_mailbox->ready)
                        {
                            return;
                        }
                        timeline = std::move(_mailbox->timeline);
                        model = std::move(_mailbox->modelJson);
                        _mailbox->timeline.reset();
                        _mailbox->ready = false;
                    }
                    if (timeline)
                    {
                        _timeline = std::move(*timeline);
                    }
                    if (!model.empty())
                    {
                        _modelJson = std::move(model);
                    }
                    if (_notesMode == NotesMode::Web)
                    {
                        PostModel();
                    }
                    else if (_notesMode == NotesMode::Plain)
                    {
                        RefreshPlainText();
                    }
                }
                catch (...)
                {
                    // std::bad_alloc moving the result: the column keeps what it shows
                }
            }

            // Tell the worker the window is going: from now on it posts nothing.
            void DetachMailbox() noexcept
            {
                if (_mailbox)
                {
                    std::lock_guard<std::mutex> lock{ _mailbox->lock };
                    _mailbox->window = nullptr;
                }
            }

            // Send the latest model to the page, once it said it is ready.
            void PostModel() noexcept
            {
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                try
                {
                    if (!_webview || _notesMode != NotesMode::Web || _modelJson.empty())
                    {
                        return;
                    }
                    const std::wstring message = L"{\"type\":\"model\",\"model\":" + _modelJson + L"}";
                    const HRESULT hr = _webview->PostWebMessageAsJson(message.c_str());
                    if (FAILED(hr))
                    {
                        ShowPlainNotes(L"the notes could not be handed to the page (" + HResultText(hr) + L")");
                    }
                }
                catch (...)
                {
                    // std::bad_alloc building the message: the page keeps its last model
                }
#endif
            }

#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
            // ---- WebView2 ----

            // Start WebView2: is a runtime installed, then the environment (asynchronously), its data folder in the profile —
            // never the package's own storage, which a package removal copies around (PROFILES.md §2d) — and the start timer.
            void StartWebView() noexcept
            {
                try
                {
                    CoTaskString version;
                    const HRESULT probe = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, version.put());
                    if (FAILED(probe) || !version.text)
                    {
                        ShowPlainNotes(L"the Microsoft Edge WebView2 Runtime is not installed");
                        return;
                    }
                    _webviewVersion = version.str();
                    std::wstring dataFolder;
                    if (!_stateDir.empty())
                    {
                        dataFolder = _stateDir + L"\\webview2";
                        std::error_code ignored;
                        std::filesystem::create_directories(std::filesystem::path{ dataFolder }, ignored);
                    }
                    const std::shared_ptr<PromptWebGuard> guard = _guard;
                    ++guard->pending;
                    const HRESULT started = ::CreateCoreWebView2EnvironmentWithOptions(
                        nullptr,
                        dataFolder.empty() ? nullptr : dataFolder.c_str(),
                        nullptr,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([guard](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                            --guard->pending;
                            if (guard->window)
                            {
                                guard->window->OnEnvironmentCreated(result, environment);
                            }
                            // else: the prompt closed first — the environment is released when this returns, and with it the
                            // browser process.
                            return S_OK;
                        }).Get());
                    if (FAILED(started))
                    {
                        // A synchronous failure is reported here, not through the callback (should both happen, a negative
                        // count only shortens the closing drain).
                        --guard->pending;
                        ShowPlainNotes(L"WebView2 could not start (" + HResultText(started) + L")");
                        return;
                    }
                    ::SetTimer(_hwnd, kTimerWebViewStart, kWebViewStartTimeoutMs, nullptr);
                }
                catch (...)
                {
                    ShowPlainNotes(L"WebView2 could not start (an exception)");
                }
            }

            // The environment is there: make the controller in the notes host.
            void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment) noexcept
            {
                try
                {
                    if (_notesMode == NotesMode::Plain || !_notesHost)
                    {
                        return; // given up meanwhile (the timer): the late environment is released by WebView2
                    }
                    if (FAILED(result) || !environment)
                    {
                        ShowPlainNotes(L"WebView2 could not start (" + HResultText(result) + L")");
                        return;
                    }
                    _environment = environment;
                    const std::shared_ptr<PromptWebGuard> guard = _guard;
                    ++guard->pending;
                    const HRESULT started = environment->CreateCoreWebView2Controller(
                        _notesHost,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([guard](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                            --guard->pending;
                            if (guard->window)
                            {
                                guard->window->OnControllerCreated(created, controller);
                            }
                            else if (controller)
                            {
                                controller->Close(); // the prompt closed while the view was made: no browser process may outlive it
                            }
                            return S_OK;
                        }).Get());
                    if (FAILED(started))
                    {
                        --guard->pending;
                        ShowPlainNotes(L"WebView2 could not make its view (" + HResultText(started) + L")");
                    }
                }
                catch (...)
                {
                    ShowPlainNotes(L"WebView2 could not start (an exception)");
                }
            }

            // The view is there: lock it down, size it, listen to it, and load the page.
            void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller) noexcept
            {
                try
                {
                    if (_notesMode == NotesMode::Plain || !_notesHost)
                    {
                        if (controller)
                        {
                            controller->Close();
                        }
                        return;
                    }
                    if (FAILED(result) || !controller)
                    {
                        ShowPlainNotes(L"WebView2 could not make its view (" + HResultText(result) + L")");
                        return;
                    }
                    _controller = controller;
                    if (FAILED(_controller->get_CoreWebView2(&_webview)) || !_webview)
                    {
                        ShowPlainNotes(L"WebView2 gave no view");
                        return;
                    }
                    ConfigureWebView();
                    ResizeNotes();
                    _expectOwnNavigation = true;
                    const std::wstring page = BuildReleaseNotesPage(NewReleaseNotesNonce(), PageStyle());
                    const HRESULT navigated = _webview->NavigateToString(page.c_str());
                    if (FAILED(navigated))
                    {
                        ShowPlainNotes(L"the notes page could not be loaded (" + HResultText(navigated) + L")");
                    }
                }
                catch (...)
                {
                    ShowPlainNotes(L"WebView2 could not make its view (an exception)");
                }
            }

            // Lock the view down to a page that shows notes, and listen to it. A WebView2 is a browser: without these, F5 would
            // reload, Ctrl+P print, a right-click offer "Save as" and "Inspect", and a link navigate the column away.
            void ConfigureWebView()
            {
                Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                if (SUCCEEDED(_webview->get_Settings(&settings)) && settings)
                {
                    settings->put_IsScriptEnabled(TRUE); // the page's own script builds the tabs (its policy runs no other)
                    settings->put_IsWebMessageEnabled(TRUE); // the model in; "ready", "open" and "escape" out
                    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                    settings->put_AreDevToolsEnabled(FALSE);
                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                    settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_AreHostObjectsAllowed(FALSE);
                    settings->put_IsBuiltInErrorPageEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(TRUE); // Ctrl+wheel zooms the notes
                    Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
                    if (SUCCEEDED(settings.As(&settings3)) && settings3)
                    {
                        // F5, Ctrl+R, Ctrl+P, Ctrl+F, Ctrl+Shift+I, ... are browser features; copy and select-all stay.
                        settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                    }
                }
                Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                if (SUCCEEDED(_controller.As(&controller2)) && controller2)
                {
                    // The column's own color until the page paints: no white flash in a dark window.
                    const COREWEBVIEW2_COLOR color{ 255, GetRValue(_palette.notes), GetGValue(_palette.notes), GetBValue(_palette.notes) };
                    controller2->put_DefaultBackgroundColor(color);
                }
                Microsoft::WRL::ComPtr<ICoreWebView2_13> webview13;
                if (SUCCEEDED(_webview.As(&webview13)) && webview13)
                {
                    Microsoft::WRL::ComPtr<ICoreWebView2Profile> profile;
                    if (SUCCEEDED(webview13->get_Profile(&profile)) && profile)
                    {
                        // Dark scroll bars and form controls; Windows' choice in High Contrast.
                        profile->put_PreferredColorScheme(_palette.system ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK);
                    }
                }

                const std::shared_ptr<PromptWebGuard> guard = _guard;
                EventRegistrationToken token{};
                _webview->add_NavigationStarting(
                    Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>([guard](ICoreWebView2* /*sender*/, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                        if (guard->window)
                        {
                            guard->window->OnNavigationStarting(args);
                        }
                        else if (args)
                        {
                            args->put_Cancel(TRUE);
                        }
                        return S_OK;
                    }).Get(),
                    &token);
                _webview->add_NavigationCompleted(
                    Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>([guard](ICoreWebView2* /*sender*/, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                        if (guard->window && args)
                        {
                            guard->window->OnNavigationCompleted(args);
                        }
                        return S_OK;
                    }).Get(),
                    &token);
                _webview->add_NewWindowRequested(
                    Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>([guard](ICoreWebView2* /*sender*/, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                        if (!args)
                        {
                            return S_OK;
                        }
                        args->put_Handled(TRUE); // never a second browser window: the link goes to the user's browser
                        if (guard->window)
                        {
                            try
                            {
                                CoTaskString uri;
                                if (SUCCEEDED(args->get_Uri(uri.put())) && uri.text)
                                {
                                    guard->window->OpenExternal(uri.str());
                                }
                            }
                            catch (...)
                            {
                                // std::bad_alloc copying the address: the link is not opened (the window stays as it is)
                            }
                        }
                        return S_OK;
                    }).Get(),
                    &token);
                _webview->add_WebMessageReceived(
                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>([guard](ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                        if (guard->window && args)
                        {
                            guard->window->OnWebMessage(args);
                        }
                        return S_OK;
                    }).Get(),
                    &token);
                _webview->add_ProcessFailed(
                    Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>([guard](ICoreWebView2* /*sender*/, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                        if (guard->window)
                        {
                            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                            if (args)
                            {
                                args->get_ProcessFailedKind(&kind);
                            }
                            guard->window->ShowPlainNotes(L"a WebView2 process stopped (kind " + std::to_wstring(static_cast<int>(kind)) + L")");
                        }
                        return S_OK;
                    }).Get(),
                    &token);
                _controller->add_MoveFocusRequested(
                    Microsoft::WRL::Callback<ICoreWebView2MoveFocusRequestedEventHandler>([guard](ICoreWebView2Controller* /*sender*/, ICoreWebView2MoveFocusRequestedEventArgs* args) -> HRESULT {
                        if (guard->window && args)
                        {
                            guard->window->OnMoveFocusRequested(args);
                        }
                        return S_OK;
                    }).Get(),
                    &token);
                _controller->add_AcceleratorKeyPressed(
                    Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>([guard](ICoreWebView2Controller* /*sender*/, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                        if (guard->window && args)
                        {
                            guard->window->OnAcceleratorKey(args);
                        }
                        return S_OK;
                    }).Get(),
                    &token);
            }

            // Only the page's own load may navigate the view; every other navigation (a link the script missed, a form) is
            // cancelled, and an http(s) one goes to the user's browser instead.
            void OnNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args) noexcept
            {
                if (!args)
                {
                    return;
                }
                if (_expectOwnNavigation)
                {
                    _expectOwnNavigation = false;
                    return;
                }
                args->put_Cancel(TRUE);
                try
                {
                    CoTaskString uri;
                    if (SUCCEEDED(args->get_Uri(uri.put())) && uri.text)
                    {
                        OpenExternal(uri.str());
                    }
                }
                catch (...)
                {
                    // std::bad_alloc copying the address: the navigation stays cancelled, nothing is opened
                }
            }

            // The page failed to load: plain text.
            void OnNavigationCompleted(ICoreWebView2NavigationCompletedEventArgs* args) noexcept
            {
                BOOL success = TRUE;
                if (FAILED(args->get_IsSuccess(&success)) || success)
                {
                    return;
                }
                COREWEBVIEW2_WEB_ERROR_STATUS status{};
                args->get_WebErrorStatus(&status);
                try
                {
                    ShowPlainNotes(L"the notes page did not load (web error " + std::to_wstring(static_cast<int>(status)) + L")");
                }
                catch (...)
                {
                    // std::bad_alloc building the reason: the page stays as it is
                }
            }

            // A message from the page: "ready" (send the model), "open" (a link), "escape" (Esc pressed in the page).
            void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept
            {
                try
                {
                    CoTaskString raw;
                    if (FAILED(args->get_WebMessageAsJson(raw.put())) || !raw.text)
                    {
                        return;
                    }
                    const auto message = json::Parse(raw.str());
                    if (!message || message->type != json::Value::Type::Obj)
                    {
                        return;
                    }
                    const std::wstring type = message->StrAt(L"type");
                    if (type == L"ready")
                    {
                        const bool first = _notesMode != NotesMode::Web;
                        _notesMode = NotesMode::Web;
                        ::KillTimer(_hwnd, kTimerWebViewStart);
                        ::InvalidateRect(_notesHost, nullptr, TRUE);
                        if (first)
                        {
                            LogUpdate(_stateDir, L"prompt window: the notes page is ready (WebView2 " + _webviewVersion + L", " + std::to_wstring(::GetTickCount64() - _createdTick) + L" ms)");
                        }
                        PostModel();
                    }
                    else if (type == L"escape")
                    {
                        RequestClose(IDCANCEL);
                    }
                    else if (type == L"open")
                    {
                        OpenExternal(message->StrAt(L"url"));
                    }
                }
                catch (...)
                {
                    // std::bad_alloc reading the message: it is dropped
                }
            }

            // Tab left the page at its first or last control: the keyboard moves on to the dialog's next or previous control.
            void OnMoveFocusRequested(ICoreWebView2MoveFocusRequestedEventArgs* args) noexcept
            {
                COREWEBVIEW2_MOVE_FOCUS_REASON reason{};
                args->get_Reason(&reason);
                const BOOL backwards = reason == COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS ? TRUE : FALSE;
                if (const HWND target = ::GetNextDlgTabItem(_hwnd, _notesHost, backwards))
                {
                    ::SetFocus(target);
                }
                args->put_Handled(TRUE);
            }

            // Keys the page does not keep: Esc and Alt+F4 answer Cancel, as they do anywhere else in the window.
            void OnAcceleratorKey(ICoreWebView2AcceleratorKeyPressedEventArgs* args) noexcept
            {
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT key = 0;
                args->get_KeyEventKind(&kind);
                args->get_VirtualKey(&key);
                const bool down = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
                if (down && (key == VK_ESCAPE || (key == VK_F4 && (::GetKeyState(VK_MENU) & 0x8000) != 0)))
                {
                    args->put_Handled(TRUE);
                    RequestClose(IDCANCEL);
                }
            }
#endif

            // Take the WebView2 view out of use NOW and close it a message later. ShowPlainNotes runs inside WebView2's own
            // callbacks (a failed load, a stopped process), and a controller must not be closed from within its own event
            // handler; hidden, it is out of sight at once, and kMsgRetireWebView closes it once that handler has returned.
            void RetireWebView() noexcept
            {
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                if (!_controller && !_webview && !_environment)
                {
                    return;
                }
                if (_controller)
                {
                    _controller->put_IsVisible(FALSE);
                }
                _retiredController = std::move(_controller);
                _retiredWebview = std::move(_webview);
                _retiredEnvironment = std::move(_environment);
                if (!_hwnd || !::PostMessageW(_hwnd, kMsgRetireWebView, 0, 0))
                {
                    CloseRetiredWebView(); // no window to post to: close it now rather than never
                }
#endif
            }

            // Close the retired view (its browser process ends with the last reference).
            void CloseRetiredWebView() noexcept
            {
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                if (_retiredController)
                {
                    _retiredController->Close();
                }
                _retiredWebview.Reset();
                _retiredController.Reset();
                _retiredEnvironment.Reset();
#endif
            }

            // Close every WebView2 view now: the current one and a retired one (teardown, outside every WebView2 callback).
            void CloseWebView() noexcept
            {
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                if (_controller)
                {
                    _controller->Close();
                }
                _webview.Reset();
                _controller.Reset();
                _environment.Reset();
                CloseRetiredWebView();
#endif
            }

            // ---- teardown ----

            // Undo everything Create did, in the order Windows wants: the worker detached, WebView2 closed, the owner enabled
            // BEFORE the window goes (so Windows hands activation back to it, not to some other app), the window destroyed;
            // then, when asked, a short pump for WebView2 calls still under way. Safe to call twice.
            //   drain — pump for pending WebView2 calls (false after a WM_QUIT: the app is going away, nothing may be pumped).
            void Teardown(bool drain) noexcept
            {
                DetachMailbox();
                if (_hwnd)
                {
                    ::KillTimer(_hwnd, kTimerWebViewStart);
                }
                CloseWebView();
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                const std::shared_ptr<PromptWebGuard> guard = _guard;
                if (guard)
                {
                    guard->window = nullptr; // late callbacks only clean up from now on
                }
#endif
                if (_ownerDisabled && _owner)
                {
                    ::EnableWindow(_owner, TRUE);
                    _ownerDisabled = false;
                }
                if (_hwnd)
                {
                    const HWND window = _hwnd;
                    _hwnd = nullptr;
                    ::DestroyWindow(window);
                }
                _notesHost = nullptr;
                _fallbackEdit = nullptr;
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
                if (drain && guard)
                {
                    // WebView2 completes its asynchronous calls through this thread's messages. The startup and the cog keep
                    // pumping after the prompt, but the hourly check's thread ends with it: without this pump a browser process
                    // started for a prompt answered within its first second would live until the app exits.
                    const ULONGLONG deadline = ::GetTickCount64() + kWebViewDrainMs;
                    while (guard->pending > 0)
                    {
                        const ULONGLONG now = ::GetTickCount64();
                        if (now >= deadline)
                        {
                            break;
                        }
                        ::MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                        MSG msg{};
                        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                        {
                            if (msg.message == WM_QUIT)
                            {
                                ::PostQuitMessage(static_cast<int>(msg.wParam)); // the caller's loop must still see it
                                return;
                            }
                            ::TranslateMessage(&msg);
                            ::DispatchMessageW(&msg);
                        }
                    }
                }
#else
                (void)drain;
#endif
            }

            // Free the fonts.
            void FreeFonts() noexcept
            {
                for (HFONT* font : { &_font, &_instructionFont, &_linkFont, &_monoFont })
                {
                    if (*font)
                    {
                        ::DeleteObject(*font);
                        *font = nullptr;
                    }
                }
            }

            // Free every GDI object the window made (after the window is gone: no control may still draw with them).
            void FreeGdi() noexcept
            {
                FreeFonts();
                for (HBRUSH* brush : { &_backgroundBrush, &_bandBrush, &_notesBrush, &_borderBrush })
                {
                    if (*brush)
                    {
                        ::DeleteObject(*brush);
                        *brush = nullptr;
                    }
                }
                if (_infoIcon)
                {
                    ::DestroyIcon(_infoIcon);
                    _infoIcon = nullptr;
                }
                if (_radioTheme)
                {
                    ::CloseThemeData(_radioTheme);
                    _radioTheme = nullptr;
                }
            }

            // ---- state ----

            // What the window shows (copied from the TaskDialog and the check, so nothing points into the caller's frame).
            std::wstring _title;
            std::wstring _instruction;
            std::wstring _content;
            std::vector<Choice> _buttonSpecs;
            std::vector<Choice> _radioSpecs;
            int _defaultButton{ IDOK };
            int _defaultRadio{ 0 };
            std::wstring _linkUrl;
            UpdateInfo _info;
            std::wstring _stateDir;
            std::wstring _webViewBlocked;
            UpdatePrefs _prefs;
            Version _current;

            // The windows.
            HWND _hwnd{ nullptr };
            HWND _owner{ nullptr };
            HWND _notesHost{ nullptr };
            HWND _instructionCtl{ nullptr };
            HWND _contentCtl{ nullptr };
            HWND _linkCtl{ nullptr };
            HWND _fallbackEdit{ nullptr };
            std::vector<HWND> _radios;
            std::vector<HWND> _buttons;
            HWND _lastFocus{ nullptr };
            bool _ownerDisabled{ false };
            bool _childrenReady{ false };
            bool _handlerExceptionLogged{ false };

            // Look and measure.
            UINT _dpi{ 96 };
            PromptPalette _palette{};
            HFONT _font{ nullptr };
            HFONT _instructionFont{ nullptr };
            HFONT _linkFont{ nullptr };
            HFONT _monoFont{ nullptr };
            HBRUSH _backgroundBrush{ nullptr };
            HBRUSH _bandBrush{ nullptr };
            HBRUSH _notesBrush{ nullptr };
            HBRUSH _borderBrush{ nullptr };
            HICON _infoIcon{ nullptr };
            HTHEME _radioTheme{ nullptr }; // the radios' glyph theme, the window's own (OpenRadioTheme)
            HICON _bigIcon{ nullptr };
            HICON _smallIcon{ nullptr };
            PromptLayout _layout;
            int _instructionHeight{ 0 };
            int _contentHeight{ 0 };
            int _radioHeight{ 0 };
            int _buttonHeight{ 0 };
            int _linkWidth{ 0 };
            int _linkHeight{ 0 };
            std::vector<int> _buttonWidths;

            // The answer.
            bool _done{ false };
            int _pressed{ IDCANCEL };
            int _radio{ 0 };
            bool _quitSeen{ false };
            int _quitCode{ 0 };

            // The notes.
            NotesMode _notesMode{ NotesMode::Starting };
            ReleaseTimeline _timeline; // the latest: the offered release alone until the worker's arrives
            std::wstring _modelJson; // the latest page model
            std::wstring _plainReason;
            std::shared_ptr<ReleaseNotesMailbox> _mailbox;
            UINT _mailboxMessage{ 0 };
            ULONGLONG _createdTick{ 0 };
#if AGENTMASTER_UPDATER_HAVE_WEBVIEW2
            std::shared_ptr<PromptWebGuard> _guard;
            Microsoft::WRL::ComPtr<ICoreWebView2Environment> _environment;
            Microsoft::WRL::ComPtr<ICoreWebView2Controller> _controller;
            Microsoft::WRL::ComPtr<ICoreWebView2> _webview;
            // A view ShowPlainNotes took out of use, closed by kMsgRetireWebView (or by the teardown, whichever comes first).
            Microsoft::WRL::ComPtr<ICoreWebView2Environment> _retiredEnvironment;
            Microsoft::WRL::ComPtr<ICoreWebView2Controller> _retiredController;
            Microsoft::WRL::ComPtr<ICoreWebView2> _retiredWebview;
            std::wstring _webviewVersion;
            bool _expectOwnNavigation{ false };
#endif
        };
    }

    // Show the update prompt in its window, with the release notes beside the choices; ShowUpdatePrompt's first choice.
    //   owner — the window the prompt belongs to (null at startup).
    //   info  — the check's result.
    //   cfg   — the TaskDialog ShowUpdatePrompt built: the window shows its words and returns its ids, so the caller maps the
    //           answer once for both.
    //   returns — the answer; nothing when the window could not be shown (AGENTMASTER_UPDATE_PROMPT=taskdialog, a failed
    //             creation, an exception before it showed) — the caller then shows the TaskDialog.
    //   throws  — nothing.
    inline std::optional<PromptWindowAnswer> ShowUpdatePromptWindow(HWND owner, const UpdateInfo& info, const TASKDIALOGCONFIG& cfg) noexcept
    {
        std::wstring stateDir;
        try
        {
            stateDir = Profiles::ResolveProfileDir();
            const std::wstring mode = detail::GetEnv(kPromptModeEnvVar);
            if (detail::EqualsNoCase(mode, L"taskdialog"))
            {
                LogUpdate(stateDir, L"prompt window: not used (AGENTMASTER_UPDATE_PROMPT=taskdialog) \x2014 the task dialog shows");
                return std::nullopt;
            }
            // Declared before the window, so the apartment outlives it: every WebView2 object is released before CoUninitialize.
            const detail::PromptApartment apartment;
            std::wstring webViewBlocked;
            if (detail::EqualsNoCase(mode, L"plain"))
            {
                webViewBlocked = L"AGENTMASTER_UPDATE_PROMPT=plain";
            }
            else if (!apartment.singleThreaded)
            {
                webViewBlocked = L"this thread is not a single-threaded COM apartment, which WebView2 needs";
            }
            detail::UpdatePromptWindow window{ cfg, info, stateDir, webViewBlocked };
            std::wstring whyNot;
            if (!window.Create(owner, whyNot))
            {
                LogUpdate(stateDir, L"prompt window: could not be created (" + whyNot + L") \x2014 the task dialog shows instead");
                return std::nullopt;
            }
            // The console echo (CLI.md §2b) reads like the TaskDialog's — the same words and choices — with this window's link
            // and a line for the notes; its answer in the TaskDialog's words too.
            TASKDIALOGCONFIG echoCfg = cfg;
            const std::wstring footer = L"<a href=\"" + window.LinkUrl() + L"\">" + window.LinkText() + L"</a>";
            echoCfg.pszFooter = footer.c_str();
            DialogEcho::Description description = DialogEcho::DescribeTaskDialog(echoCfg);
            description.lines.push_back(window.NotesDescription());
            LogUpdate(stateDir, L"prompt window: shown (" + window.NotesStartText() + L")");
            return DialogEcho::ShowCustomDialog(
                description,
                [&window]() noexcept { return window.RunModal(); },
                [&echoCfg](const PromptWindowAnswer& answer) { return DialogEcho::TaskDialogAnswer(echoCfg, S_OK, answer.button, answer.radio, FALSE); });
        }
        catch (...)
        {
            try
            {
                LogUpdate(stateDir.empty() ? Profiles::ResolveProfileDir() : stateDir, L"prompt window CRASHED (exception \x2014 the task dialog shows instead)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return std::nullopt;
        }
    }
}
