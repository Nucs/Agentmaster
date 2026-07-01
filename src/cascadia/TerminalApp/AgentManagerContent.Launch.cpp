// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster Manager tab content -- C1 'Linked Lenses' (7 partial files)
// The pinned leftmost tab's UI (DESIGN section 9): a Triage Board + Explorer Tree + Auto Testing over
// ONE shared SessionRegistry, built imperatively. ONE class (AgentManagerContent) split from the
// former 10864-line .cpp into by-area TUs that share AgentManagerContent.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentManagerContent.cpp             - CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh
//   AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
// ★ AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster Manager tab: the LAUNCH bar -- the cwd box validation, the Claude/Codex agent toggle, launch/create/fork handlers, and the focus-triggered path-picker drop-down (recent dirs + subfolders + worktrees). Partial TU of AgentManagerContent.cpp.
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentStatusColors.h" // ParseArgbHexColor / FormatArgbHexColor — the cog's "status flashing color" picker <-> AppSettings::flashRingColor
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/ProfileBootstrap.h" // the cog's Profile row (active dir + Change… picker)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/Engine.h" // RecoverableWindows (the "Reopen Windows (N)" recover button)
#include "AgentMaster/ProcessInspect.h" // ReadTranscriptInfo (read-only Auto Testing of an external) + BringClaudeWindowToFront (EXTERNAL menu)
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd
#include "AgentMaster/Updater.h" // the in-app updater: the cog's "Check for updates" + the "vX available!" label

// Agentmaster: the build-stamped git commit + branch (the Settings page header). Generated into
// $(GeneratedFilesDir) by TerminalAppLib.vcxproj's AgentmasterGenerateBuildInfo target, which is
// on the include path. The __has_include guard + fallback defines keep this file compilable if
// the generator hasn't run yet (e.g. opened in an IDE before any build); a real build always
// regenerates the header first (BeforeTargets ClCompile).
#if __has_include("AgentmasterBuildInfo.g.h")
#include "AgentmasterBuildInfo.g.h"
#endif
#ifndef AGENTMASTER_COMMIT_HASH
#define AGENTMASTER_COMMIT_HASH L"unknown"
#endif
#ifndef AGENTMASTER_COMMIT_BRANCH
#define AGENTMASTER_COMMIT_BRANCH L"unknown"
#endif

#include <algorithm>
#include <chrono>
#include <cmath> // std::pow — relative-luminance black/white contrast pick for the card title band
#include <filesystem> // create_directories — the "Create & Launch" affordance for a not-yet-existing dir
#include <system_error> // std::error_code — non-throwing create_directories
#include <thread> // background transcript read for an external's read-only plan
#include <shobjidl.h> // IFileOpenDialog — Browse for claude.exe (native-exe-only policy)

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with our Text() TextBlock helper below.
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using winrt::Windows::UI::Core::CoreCursor;
using winrt::Windows::UI::Core::CoreCursorType;
using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input; // KeyRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue, VirtualKey
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace Agentmaster;
// The shared tooltip recipe (AgentTipHelpers.h) — a using-DECLARATION so the file-scope
// helpers below (e.g. TimingText) can call it unqualified too.
using winrt::TerminalApp::implementation::AgentSetTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace winrt::TerminalApp::implementation
{
    void AgentManagerContent::_UpdateLaunchAgentButton()
    {
        if (!_launchAgentBtn)
        {
            return;
        }
        const bool codex = _launchCodex;
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        auto g = Text(L"\x25CF", 11, true, 1.0); // ●
        g.Foreground(SolidColorBrush{ codex ? Color{ 0xFF, 0x4E, 0xC9, 0xB0 } : Color{ 0xFF, 0x4F, 0x9C, 0xFF } });
        row.Children().Append(g);
        row.Children().Append(Text(codex ? L"Codex" : L"Claude", 11, false, 0.95));
        _launchAgentBtn.Content(row);
        _ReflowLaunchBar(); // the toggle width changed (Claude<->Codex) -> re-fit the row
    }

    void AgentManagerContent::_OnLaunch()
    {
        if (!_cwdBox)
        {
            return;
        }
        _NormalizeCwdBox(); // launch with — and remember — a normalized path (a session id is unaffected)
        std::wstring text{ _cwdBox.Text() };
        { // trim surrounding whitespace so the launch target matches exactly what _ValidateLaunchBox judged
            const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
            while (!text.empty() && isws(text.front())) { text.erase(text.begin()); }
            while (!text.empty() && isws(text.back())) { text.pop_back(); }
        }
        // Agentmaster (Codex-launch): a Codex launch is DIRECTORY-ONLY — codex has no typed-id resume/fork
        // here (Codex resume is reached via the Archive page / window-restore / EXTERNAL Adopt), and while
        // Codex is selected _ValidateLaunchBox keeps the box in directory semantics. Spawn a managed codex in
        // that dir, mirroring the EXTERNAL menu's "Open New Codex Session Here" (pid 0 = no source external,
        // adopt=false = a fresh independent session). _LaunchCodexSession registers the card immediately.
        if (_launchCodex)
        {
            if (_codexLaunchHandler)
            {
                if (!_EnsureLaunchDirExists(text)) // "Create & Launch": make the folder first if it doesn't exist yet
                {
                    return; // creation failed (already warned) — don't launch into a missing dir
                }
                _PushRecentDir(text); // remember it as "recently selected" (shared MRU with Claude launches)
                _ClosePathPicker();
                _codexLaunchHandler(0, winrt::hstring{ text }, false, false); // fresh launch: adopt=false, fork=false
            }
            return;
        }
        // Native-exe-only policy: every CLAUDE interaction (new session or resume) needs a native
        // claude.exe. With none detected, show the install/Browse modal instead of launching. (Codex
        // launches above are a separate runtime and are not gated on claude.exe.)
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _ShowClaudeMissing();
            return;
        }
        // A session id in the box = resume that conversation. The button reads "Resume session" and
        // is only ENABLED when the id was FOUND (_ValidateLaunchBox), so this path is reachable only
        // for a real on-disk transcript; resolve its dir/title and hand off to the page.
        if (const auto sid = LooksLikeSessionId(text))
        {
            std::wstring dir, title;
            if (_resumeSessionHandler && _ResolveSessionDirTitle(*sid, dir, title))
            {
                _ClosePathPicker();
                _resumeSessionHandler(winrt::hstring{ *sid }, winrt::hstring{ dir }, winrt::hstring{ title });
                _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after resume
                _ValidateLaunchBox(); // re-validate explicitly: the resume handler just opened+selected a new tab, so the Manager content is detached and TextChanged won't reliably fire to reset the underline/buttons
            }
            return;
        }
        // A working directory = a new, independent session there.
        if (_spawnHandler)
        {
            if (!_EnsureLaunchDirExists(text)) // "Create & Launch Claude": make the folder first if it doesn't exist yet
            {
                return; // creation failed (already warned) — don't launch into a missing dir
            }
            _PushRecentDir(text); // remember it as "recently selected"
            _ClosePathPicker();
            _spawnHandler(winrt::hstring{ text }, winrt::hstring{});
        }
    }

    // Agentmaster: ensure the launch target directory exists, creating it (and any missing parents)
    // when the user typed a not-yet-existing absolute path — the "Create & Launch" affordance. The
    // launch button only reads "Create & Launch …" (and only enables for a missing path) when
    // _ValidateLaunchBox judged the path creatable (LooksLikeCreatableDir), so this is the matching
    // commit step. Returns true if the dir exists (already, or after a successful create) and the
    // launch may proceed; false if creation failed (a buttons-only error is shown and the caller
    // aborts). An already-existing dir is a no-op pass-through; a non-creatable path can't reach here
    // (the button is disabled for it) but is rejected defensively.
    bool AgentManagerContent::_EnsureLaunchDirExists(const std::wstring& dir)
    {
        const std::wstring norm = NormPath(dir);
        if (IsDir(norm))
        {
            return true; // already there — nothing to create
        }
        if (!LooksLikeCreatableDir(norm))
        {
            return false; // not an absolute path we offered to create (button is disabled for this)
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ norm }, ec);
        if (ec || !IsDir(norm))
        {
            // Surface the failure (permission denied, a missing drive, a path too long, …) instead of
            // silently doing nothing after the user clicked "Create & Launch". Buttons-only ContentDialog
            // (a text box inside one gets no keypresses in XAML Islands, but an info dialog needs none).
            ContentDialog dialog;
            dialog.Title(winrt::box_value(L"Couldn't create folder"));
            dialog.Content(winrt::box_value(winrt::hstring{ L"Could not create the working directory:\n\n" } + winrt::hstring{ norm }));
            dialog.CloseButtonText(L"OK");
            if (_root)
            {
                try
                {
                    dialog.XamlRoot(_root.XamlRoot());
                    dialog.RequestedTheme(_root.ActualTheme());
                }
                catch (...)
                {
                }
            }
            try
            {
                dialog.ShowAsync();
            }
            CATCH_LOG(); // Agentmaster: a silently-swallowed ShowAsync = this "couldn't create folder" error dialog never appears, so the user sees the Launch just do nothing with no trace — log it
            return false;
        }
        return true;
    }

    // Agentmaster: the Fork button (shown only for a FOUND session id) -> fork that conversation
    // into a NEW one in the same dir (the page's _ForkSessionFromDisk: claude --resume --fork-session).
    void AgentManagerContent::_OnForkFromBox()
    {
        if (!_cwdBox)
        {
            return;
        }
        // Native-exe-only policy: a fork is a claude launch -> requires a native claude.exe.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            _ShowClaudeMissing();
            return;
        }
        const auto sid = LooksLikeSessionId(std::wstring{ _cwdBox.Text() });
        if (!sid) // the button is hidden for non-session-id input, but guard anyway
        {
            return;
        }
        std::wstring dir, title;
        if (_forkSessionHandler && _ResolveSessionDirTitle(*sid, dir, title))
        {
            _ClosePathPicker();
            _forkSessionHandler(winrt::hstring{ *sid }, winrt::hstring{ dir }, winrt::hstring{ title });
            _cwdBox.Text(L""); // Agentmaster: consume the session id — clear the box after fork
            _ValidateLaunchBox(); // re-validate explicitly: the fork handler just opened+selected a new tab, so the Manager content is detached and TextChanged won't reliably fire to reset the underline/buttons
        }
    }

    // Agentmaster: paint the Launch box's validation underline + drive the launch/fork buttons.
    // EMPTY -> neutral, "Launch Claude" enabled (defaults). A UUID -> session id: FOUND = green +
    // "Resume session" enabled + Fork shown; NOT found = red + disabled. Otherwise a directory:
    // EXISTS = neutral + "Launch Claude" enabled; a not-yet-existing ABSOLUTE path = amber +
    // "Create & Launch Claude" enabled (the folder is created on launch); a malformed / relative path
    // = red + disabled. (Per the design: green is reserved for a found session id; a valid folder
    // stays neutral; amber flags a folder that will be created.)
    void AgentManagerContent::_ValidateLaunchBox()
    {
        if (!_cwdBox || !_launchBtn)
        {
            return;
        }
        // Agentmaster (responsive launch bar): the launch button's text + the Fork button's visibility
        // change below (Launch Claude / Create & Launch Claude / Resume session / +Fork / the Codex twins),
        // which changes the launch-buttons group width — re-fit the row on EVERY exit path.
        auto reflowOnExit = wil::scope_exit([this]() noexcept { _ReflowLaunchBar(); });
        std::wstring trimmed{ _cwdBox.Text() };
        const auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (!trimmed.empty() && isws(trimmed.front()))
        {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && isws(trimmed.back()))
        {
            trimmed.pop_back();
        }

        const auto paint = [this](int state) { // 0 neutral (hidden), 1 green, 2 red, 3 amber (will create)
            if (!_cwdUnderline)
            {
                return;
            }
            // transparent (neutral) / green (found id) / red (missing dir or unknown id) / amber (a
            // not-yet-existing dir we'll CREATE on launch); kept always present (Height 2) so toggling
            // color never reflows the toolbar.
            const SolidColorBrush b = state == 1 ? Fill(0xFF, 0x4C, 0xAF, 0x50) :
                                      state == 2 ? Fill(0xFF, 0xE5, 0x39, 0x35) :
                                      state == 3 ? Fill(0xFF, 0xDA, 0xA5, 0x20) :
                                                   Fill(0x00, 0x00, 0x00, 0x00);
            _cwdUnderline.Background(b);
        };
        const auto showFork = [this](bool v) {
            if (_forkBtn)
            {
                _forkBtn.Visibility(v ? Visibility::Visible : Visibility::Collapsed);
            }
        };

        // Agentmaster (Codex-launch): while Codex is the selected agent the box is DIRECTORY-ONLY — codex
        // has no --session-id, so there is no typed-id resume (the green found-id state) and no Fork. Empty
        // or an existing dir => enabled "Launch Codex" (neutral underline); a not-yet-existing absolute path
        // => amber + "Create & Launch Codex"; a malformed / relative path => red + disabled.
        if (_launchCodex)
        {
            showFork(false);
            if (trimmed.empty())
            {
                paint(0);
                _launchBtn.IsEnabled(true);
                _launchBtn.Content(winrt::box_value(L"Launch Codex"));
            }
            else
            {
                // An existing dir launches as-is; a not-yet-existing absolute path flips to amber +
                // "Create & Launch Codex" (made on launch); a malformed/relative path stays red+disabled.
                const std::wstring norm = NormPath(trimmed);
                const bool exists = IsDir(norm);
                const bool creatable = !exists && LooksLikeCreatableDir(norm);
                paint(exists ? 0 : (creatable ? 3 : 2));
                _launchBtn.IsEnabled(exists || creatable);
                _launchBtn.Content(winrt::box_value(creatable ? L"Create & Launch Codex" : L"Launch Codex"));
            }
            return;
        }

        if (trimmed.empty())
        {
            paint(0);
            _launchBtn.IsEnabled(true);
            _launchBtn.Content(winrt::box_value(L"Launch Claude"));
            showFork(false);
            return;
        }
        if (const auto sid = LooksLikeSessionId(trimmed))
        {
            const bool found = ::Agentmaster::ClaudeConversationExists(*sid);
            paint(found ? 1 : 2);
            _launchBtn.IsEnabled(found);
            _launchBtn.Content(winrt::box_value(L"Resume session"));
            showFork(found);
            return;
        }
        // a working directory: green is reserved for session ids, so a valid dir stays neutral. A
        // not-yet-existing absolute path flips to amber + "Create & Launch Claude" (_OnLaunch makes the
        // folder first); a malformed / relative path that we won't create stays red + disabled.
        const std::wstring norm = NormPath(trimmed);
        const bool exists = IsDir(norm);
        const bool creatable = !exists && LooksLikeCreatableDir(norm);
        paint(exists ? 0 : (creatable ? 3 : 2));
        _launchBtn.IsEnabled(exists || creatable);
        _launchBtn.Content(winrt::box_value(creatable ? L"Create & Launch Claude" : L"Launch Claude"));
        showFork(false);
    }

    // Agentmaster: resolve a session id to its (working dir, title) for resume / fork. The registry
    // knows a managed (live or archived) session directly; otherwise read the cwd straight off the
    // on-disk transcript (the project FOLDER name is a lossy encoding, so read the line's real cwd).
    // Title is left empty in the transcript case — the resume/fork seam derives a smart name from the
    // dir. Returns false if no dir could be found (caller no-ops).
    bool AgentManagerContent::_ResolveSessionDirTitle(const std::wstring& id, std::wstring& dir, std::wstring& title)
    {
        dir.clear();
        title.clear();
        if (_registry)
        {
            if (const auto info = _registry->Get(id); info && !info->workingDir.empty())
            {
                dir = info->workingDir;
                title = info->title;
                return true;
            }
        }
        const std::wstring path = ::Agentmaster::ResolveClaudeTranscriptPath(id);
        if (path.empty())
        {
            return false;
        }
        const auto facts = ::Agentmaster::ReadTranscriptQuickFacts(path, 0);
        dir = facts.cwd;
        return !dir.empty();
    }

    std::vector<std::wstring> AgentManagerContent::_CollectRecentDirs(const std::wstring& current, const std::wstring& query) const
    {
        // The RECENT section length is a global setting (AppSettings::recentDirsLimit, default
        // 10); 0/garbage falls back to 10.
        const size_t limit = _appSettings.recentDirsLimit > 0 ? _appSettings.recentDirsLimit : 10;

        // Gather the full candidate pool first — recents (MRU) over live-session dirs — deduped,
        // with the box's own path excluded. In MRU mode we'd cap while gathering; in query mode
        // we rank the WHOLE pool before truncating, so the best matches surface even when they're
        // not the most recent. The pool is small (recents are capped at the same limit on save,
        // plus the live fleet), so gathering it whole is cheap.
        std::vector<std::wstring> pool;
        auto add = [&](const std::wstring& d) {
            if (d.empty())
            {
                return;
            }
            if (!current.empty() && PathEq(d, current))
            {
                return; // exclude the path that's currently in the box
            }
            for (const auto& e : pool)
            {
                if (PathEq(e, d))
                {
                    return; // dedup
                }
            }
            pool.push_back(d);
        };

        for (const auto& d : _recentDirs)
        {
            add(d);
        }
        // Supplement from live sessions (most-recently-active first) so the list is useful even
        // before anything has been launched this run. In query mode we need the WHOLE pool to
        // rank; in MRU mode we can stop (and skip the snapshot entirely) once it's full.
        const bool ranking = !query.empty();
        if (_registry && (ranking || pool.size() < limit))
        {
            auto snap = _registry->Snapshot();
            std::sort(snap.begin(), snap.end(), [](const SessionInfo& a, const SessionInfo& b) {
                return a.lastActivityUnixMs > b.lastActivityUnixMs;
            });
            for (const auto& s : snap)
            {
                add(s.workingDir);
                if (!ranking && pool.size() >= limit)
                {
                    break;
                }
            }
        }

        if (query.empty())
        {
            // Plain MRU order (recents first, then live-session dirs), capped to the limit — the
            // historical behavior when the box is empty or holds a rooted path.
            if (pool.size() > limit)
            {
                pool.resize(limit);
            }
            return pool;
        }

        // Query mode: a bare token was typed with no root path. Rank the pool by case-insensitive
        // fuzzy closeness to the token (Levenshtein approximate-substring distance over the whole
        // path — so a mid-path segment like "...\Agentmaster" matches "agent"), keep only the
        // genuine matches, and order them closest-first. The threshold allows roughly half the
        // token to differ, which keeps near-misses ("agen", "agnet") while dropping unrelated
        // recents; the gather order (MRU) is the stable tiebreaker so equally-close recents keep
        // their recency order. Very short tokens (<=2 chars) require an EXACT substring — one
        // allowed edit on a 2-char token would otherwise match almost anything — and the filter
        // tightens naturally as more characters are typed.
        const std::wstring q = ToLowerInvariant(query);
        const size_t threshold = q.size() <= 2 ? 0 : q.size() / 2;
        struct Scored
        {
            std::wstring dir;
            size_t dist;
            size_t order;
        };
        std::vector<Scored> scored;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            const size_t dist = FuzzySubstringDistance(q, ToLowerInvariant(pool[i]));
            if (dist <= threshold)
            {
                scored.push_back({ pool[i], dist, i });
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            if (a.dist != b.dist)
            {
                return a.dist < b.dist; // closest match topmost
            }
            return a.order < b.order; // ...then most-recent first
        });
        std::vector<std::wstring> out;
        for (const auto& s : scored)
        {
            if (out.size() >= limit)
            {
                break;
            }
            out.push_back(s.dir);
        }
        return out;
    }

    void AgentManagerContent::_PushRecentDir(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return;
        }
        _recentDirs.erase(std::remove_if(_recentDirs.begin(), _recentDirs.end(), [&](const std::wstring& e) { return PathEq(e, dir); }), _recentDirs.end());
        _recentDirs.insert(_recentDirs.begin(), dir);
        const size_t cap = _appSettings.recentDirsLimit > 0 ? _appSettings.recentDirsLimit : 10;
        if (_recentDirs.size() > cap)
        {
            _recentDirs.resize(cap);
        }
        ::Agentmaster::SaveRecentDirs(_recentDirs);
    }

    // Agentmaster: the "Browse…" row — the FIRST option in the path-picker dropdown. Visually it
    // mirrors _MakePathRow (a transparent, focus-neutral row) but its glyph is a folder icon and its
    // click opens the native folder dialog instead of drilling a typed path. Kept a TextBlock (not a
    // FontIcon) so its vertical rhythm matches the other rows' TextBlock glyphs; only the FontFamily
    // is swapped to Segoe Fluent Icons so the folder glyph renders.
    Button AgentManagerContent::_MakeBrowseRow()
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        {
            auto glyph = Text(L"\xE8B7", 13, false, 0.9); // Segoe Fluent Icons: Folder
            glyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            row.Children().Append(glyph);
        }
        row.Children().Append(Text(L"Browse\x2026", 13, true, 1.0)); // bold: this is the primary "pick a folder" action

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        // Like the path rows: must NOT take focus from the cwd box — a row grabbing focus would let
        // the box's LostFocus race ahead and tear down the popup (and this button) before the click
        // registers. Keeping focus on the box also keeps the popup open across picks.
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        AgentSetTip(btn, L"Browse for a working directory \x2014 the pick fills the box and is added to recents.");
        btn.Click([this](const IInspectable&, const RoutedEventArgs&) { _BrowseForLaunchDir(); });
        return btn;
    }

    // Agentmaster: open the native folder dialog for the Launch path box ("Browse…" row). A pick is
    // dropped into the box AND pushed onto the recent-dirs MRU immediately (so Browse picks are part
    // of recents even before the session is launched), then the box is re-validated + refocused. The
    // COM modal is deferred off the click tick (it needs the message pump — the same XAML-Islands rule
    // the claude.exe Browse and the profile picker both follow).
    void AgentManagerContent::_BrowseForLaunchDir()
    {
        if (!_dispatcher || !_cwdBox)
        {
            return;
        }
        // Seed the dialog at the box's current path when it points at a real folder (or, for a partial
        // leaf being typed, that leaf's existing parent), captured before the async hop.
        std::wstring seed;
        {
            const std::wstring norm = NormPath(std::wstring{ _cwdBox.Text() });
            if (IsDir(norm))
            {
                seed = norm;
            }
            else if (const auto parent = ParentDir(norm); parent && IsDir(*parent))
            {
                seed = *parent;
            }
        }
        _dispatcher.TryEnqueue([this, seed]() {
            const auto picked = PickFolder(::GetActiveWindow(), seed);
            if (!picked || picked->empty() || !_cwdBox)
            {
                return;
            }
            const std::wstring dir = NormPath(*picked);
            _PushRecentDir(dir); // a Browse pick joins the recents right away (shared MRU with launches)
            _cwdBox.Text(winrt::hstring{ dir }); // set the launch target (fires TextChanged -> _ValidateLaunchBox)
            _cwdBox.Select(static_cast<int32_t>(dir.size()), 0); // caret to end
            _ValidateLaunchBox(); // explicit: a dismissed picker can early-out of the TextChanged path
            _cwdBox.Focus(FocusState::Programmatic); // keep the box focused (don't strand focus on the dialog)
            // Refresh the open picker so the just-added recent shows; leave a closed one closed (the
            // user made a definitive choice via the modal).
            if (_pathPopup && _pathPopup.IsOpen())
            {
                _RebuildPathPicker();
            }
        });
    }

    Button AgentManagerContent::_MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText, const std::wstring& branch)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(Text(glyph, 13, false, 0.7));
        // Recents show their full path (displayText empty); folders show just the leaf —
        // the section header already states which directory they live in.
        row.Children().Append(Text(displayText.empty() ? winrt::hstring{ fullPath } : displayText, 13, false, 1.0));
        // Agentmaster: a non-empty `branch` (the RECENT rows pass it, via ReadGitBranchForDir) appends
        // "— <branch>" at the end — the GIT WORKTREES row's "<path> — <branch>" idiom — so a recent dir
        // that's a git repo shows the branch it's on. Empty (folders / non-repos) renders nothing.
        if (!branch.empty())
        {
            row.Children().Append(Text(L"\x2014", 13, false, 0.35));
            row.Children().Append(Text(winrt::hstring{ branch }, 13, false, 0.7));
        }

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        // The list is click-only and must NOT take focus from the cwd box. If a row grabbed
        // focus, the box's LostFocus could race ahead of this Click and tear down the popup
        // (and this very button) before the pick registers — so clicking a row would appear
        // to do nothing. Keeping focus on the box also keeps the popup open across picks.
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        const auto captured = fullPath;
        AgentSetTip(btn, winrt::hstring{ L"Use this folder \x2014 " } + winrt::hstring{ fullPath });
        btn.Click([this, captured](const IInspectable&, const RoutedEventArgs&) { _PickPath(captured); });
        return btn;
    }

    // Agentmaster: a "GIT WORKTREES" row — "<name> — <path> — <branch>" (branch also in the tooltip).
    // Mirrors _MakePathRow (a transparent, focus-neutral row that must NOT steal focus from the cwd
    // box — or the box's LostFocus would race ahead and tear down the popup before the click lands)
    // and, like it, a click drills the launch box into the worktree path via _PickPath, so a session
    // launches THERE. The leaf name is the worktree's identity; the path follows dim so the launch
    // destination is unambiguous (worktrees of one repo share a folder, differ by branch).
    Button AgentManagerContent::_MakeWorktreeRow(const std::wstring& fullPath, const std::wstring& name, const std::wstring& branch, bool isCurrent)
    {
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);
        row.VerticalAlignment(VerticalAlignment::Center);
        {
            auto glyph = Text(L"\xF1D3", 13, false, isCurrent ? 0.9 : 0.7); // Segoe Fluent Icons: BranchFork2
            glyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            row.Children().Append(glyph);
        }
        row.Children().Append(Text(winrt::hstring{ name }, 13, true, 1.0));
        row.Children().Append(Text(L"\x2014", 13, false, 0.35)); // em-dash separator: "<name> — <path>"
        row.Children().Append(Text(winrt::hstring{ fullPath }, 13, false, 0.55));
        if (!branch.empty())
        {
            row.Children().Append(Text(L"\x2014", 13, false, 0.35)); // second separator: "<path> — <branch>"
            row.Children().Append(Text(winrt::hstring{ branch }, 13, false, 0.7)); // the worktree's checked-out branch, at the end (a touch brighter than the path)
        }
        if (isCurrent)
        {
            row.Children().Append(Text(L"(current)", 11, false, 0.45)); // the worktree the box already points into
        }

        auto btn = Button{};
        btn.Content(row);
        btn.HorizontalAlignment(HorizontalAlignment::Stretch);
        btn.HorizontalContentAlignment(HorizontalAlignment::Left);
        btn.Background(SolidColorBrush{ Colors::Transparent() });
        btn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        btn.Padding(Thickness{ 8, 5, 8, 5 });
        btn.IsTabStop(false);
        btn.AllowFocusOnInteraction(false);
        const auto captured = fullPath;
        const winrt::hstring tip = branch.empty() ?
            (winrt::hstring{ L"Use this worktree \x2014 " } + winrt::hstring{ fullPath }) :
            (winrt::hstring{ L"Worktree on branch " } + winrt::hstring{ branch } + winrt::hstring{ L" \x2014 " } + winrt::hstring{ fullPath });
        AgentSetTip(btn, tip);
        btn.Click([this, captured](const IInspectable&, const RoutedEventArgs&) { _PickPath(captured); });
        return btn;
    }

    void AgentManagerContent::_RebuildPathPicker()
    {
        if (!_pathListHost)
        {
            return;
        }
        _pathListHost.Children().Clear();

        // Agentmaster: "Browse…" is always the FIRST option — a native folder dialog whose pick fills
        // the box and joins the recents. Appended before any section so it sits at the top in every
        // mode (empty box, fuzzy query, or a rooted path). The empty-state hint below keys off this
        // baseline so it still shows when Browse is the ONLY row.
        _pathListHost.Children().Append(_MakeBrowseRow());
        const uint32_t browseRowCount = _pathListHost.Children().Size();

        auto sectionLabel = [](const winrt::hstring& s) {
            auto lbl = Text(s, 11, true, 0.5);
            lbl.Margin(Thickness{ 6, 8, 6, 2 });
            return lbl;
        };

        const std::wstring current = _cwdBox ? std::wstring{ _cwdBox.Text() } : std::wstring{};

        // "No root path" = a bare token typed with no path anchor: no separator AND not drive-
        // qualified ("agent", not "K:\..." / "K:" / "/home" / "\\server"). In that case the box
        // text can't anchor a subfolder listing, so instead of the (useless) "(path not found)"
        // we treat the token as a fuzzy QUERY over the recent directories — matched case-
        // insensitively and ranked by Levenshtein closeness (closest first). A session id is a
        // bare token too but is never a directory query, so it stays in plain MRU mode.
        auto looksRooted = [](const std::wstring& s) {
            if (s.find_first_of(L"\\/") != std::wstring::npos)
            {
                return true; // has a path separator
            }
            if (s.size() >= 2 && s[1] == L':' &&
                ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')))
            {
                return true; // drive-qualified ("K:" / "K:foo")
            }
            return false;
        };
        const bool queryMode = !current.empty() && !looksRooted(current) && !LooksLikeSessionId(current).has_value();

        // RECENT (up to AppSettings::recentDirsLimit, current excluded). In query mode the section
        // is filtered + ranked by closeness to the typed token; otherwise it's plain MRU order.
        const auto recents = _CollectRecentDirs(current, queryMode ? current : std::wstring{});
        if (!recents.empty())
        {
            _pathListHost.Children().Append(sectionLabel(queryMode ?
                (winrt::hstring{ L"RECENT MATCHES FOR \"" } + winrt::hstring{ current } + winrt::hstring{ L"\"" }) :
                winrt::hstring{ L"RECENT" }));
            for (const auto& d : recents)
            {
                _pathListHost.Children().Append(_MakePathRow(d, L"\x21BB", winrt::hstring{}, ReadGitBranchForDir(d))); // Agentmaster: show the recent dir's git branch at the end, like the worktree rows
            }
        }

        // GIT WORKTREES of the repo containing the typed path (if any). ListGitWorktrees mirrors
        // ReadGitBranchForDir's pure-filesystem .git walk (a few small reads + one dir enum), so it is
        // cheap enough for this per-keystroke rebuild. Shown only when the path resolves into a repo
        // that has LINKED worktrees (size >= 2: the main worktree + at least one linked) — a plain repo
        // with no extra worktrees adds no section. Clicking a row drills the launch box into that
        // worktree exactly like picking a folder, so a session launches there.
        if (!current.empty() && !queryMode)
        {
            const auto worktrees = ListGitWorktrees(NormPath(current));
            if (worktrees.size() >= 2)
            {
                _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"GIT WORKTREES" }));
                for (const auto& w : worktrees)
                {
                    _pathListHost.Children().Append(_MakeWorktreeRow(w.path, w.name, w.branch, w.isCurrent));
                }
            }
        }

        // SUBFOLDERS of the current path (+ a parent up-nav).
        //
        // Agentmaster: typing a partial leaf FILTERS. The box text splits at its LAST separator
        // into (listDir, leaf):
        //   - a path ending in a separator ("K:\source\") has an empty leaf => list ALL of
        //     listDir, unfiltered;
        //   - a partial leaf ("K:\source\Agent") lists listDir's folders whose name STARTS WITH
        //     the leaf ("Agent*"), case-insensitively, with the EXACT match hoisted to the top.
        // When the WHOLE text is itself an existing directory ("K:\source", no trailing
        // separator), a SECOND section below ALSO lists that directory's own subfolders — so
        // "K:\source" shows the K:\source* siblings (above) AND everything inside K:\source.
        // (Clicking any folder row appends the separator, drilling into it.)
        //
        // Skipped in query mode: a bare token (no root path) has no directory to enumerate, so
        // this would only ever produce "(path not found)" — the RECENT MATCHES section above is
        // the whole answer there.
        if (!current.empty() && !queryMode)
        {
            std::wstring listDir = current; // the directory whose subfolders we enumerate
            std::wstring filter; // leaf prefix to match; empty => list everything
            const wchar_t lastCh = current.back();
            const bool endsSep = (lastCh == L'\\' || lastCh == L'/');
            if (!endsSep)
            {
                if (const auto pos = current.find_last_of(L"\\/"); pos != std::wstring::npos)
                {
                    filter = current.substr(pos + 1); // the leaf being typed
                    listDir = current.substr(0, pos + 1); // its parent (keep the trailing sep)
                }
                // else: a bare relative token with no separator — there's no parent in the box
                // to anchor a listing to, so fall through and list `current` itself (no filter).
            }

            // Append `dir`'s subfolders, optionally keeping only names that start with `filt`
            // (case-insensitive) and hoisting an exact match to the front (a directory's leaf is
            // unique within its parent, so at most one); the rest stay newest-modified-first.
            // Returns how many folder rows were appended.
            auto appendFolderRows = [&](const std::wstring& dir, const std::wstring& filt) -> size_t {
                auto subs = EnumSubdirs(dir);
                if (!filt.empty())
                {
                    std::wstring exact;
                    std::vector<std::wstring> matched;
                    for (auto& leaf : subs)
                    {
                        if (!LeafStartsWith(leaf, filt))
                        {
                            continue;
                        }
                        if (exact.empty() && LeafEquals(leaf, filt))
                        {
                            exact = std::move(leaf);
                        }
                        else
                        {
                            matched.push_back(std::move(leaf));
                        }
                    }
                    subs.clear();
                    if (!exact.empty())
                    {
                        subs.push_back(std::move(exact));
                    }
                    for (auto& m : matched)
                    {
                        subs.push_back(std::move(m));
                    }
                }
                for (const auto& leaf : subs)
                {
                    _pathListHost.Children().Append(_MakePathRow(JoinDir(dir, leaf), L"\x25B8", winrt::hstring{ leaf }));
                }
                return subs.size();
            };

            // Section A — matches in listDir (the typed leaf's siblings, or — with no leaf — the
            // whole directory). Carries the parent up-nav (one level above listDir).
            {
                const winrt::hstring hdr = filter.empty() ?
                    (winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ listDir }) :
                    (winrt::hstring{ L"MATCHES FOR \"" } + winrt::hstring{ filter } + winrt::hstring{ L"\" IN " } + winrt::hstring{ listDir });
                _pathListHost.Children().Append(sectionLabel(hdr));
            }
            if (const auto parent = ParentDir(listDir))
            {
                _pathListHost.Children().Append(_MakePathRow(*parent, L"\x2191", winrt::hstring{ L".. (parent)" }));
            }
            if (IsDir(listDir))
            {
                if (appendFolderRows(listDir, filter) == 0)
                {
                    _pathListHost.Children().Append(Text(filter.empty() ? L"(no subfolders)" : L"(no matches)", 12, false, 0.5));
                }
            }
            else
            {
                _pathListHost.Children().Append(Text(L"(path not found)", 12, false, 0.5));
            }

            // Section B — when the WHOLE text names an existing directory (and a leaf was matched
            // above), ALSO list everything INSIDE it, below the sibling matches.
            if (!endsSep && !filter.empty() && IsDir(current))
            {
                _pathListHost.Children().Append(sectionLabel(winrt::hstring{ L"SUBFOLDERS OF " } + winrt::hstring{ current }));
                if (appendFolderRows(NormPath(current), std::wstring{}) == 0)
                {
                    _pathListHost.Children().Append(Text(L"(no subfolders)", 12, false, 0.5));
                }
            }
        }

        if (_pathListHost.Children().Size() == browseRowCount)
        {
            // Only the Browse row is present (no recents / subfolders). In query mode that means the
            // token matched no recent directory; otherwise it's the initial empty-box hint.
            _pathListHost.Children().Append(Text(queryMode ?
                L"No recent directory matches — type a full path, or Browse\x2026 above." :
                L"Type a path, pick a recent directory, or Browse\x2026 above.",
                12, false, 0.6));
        }
    }

    void AgentManagerContent::_OpenPathPicker()
    {
        if (!_pathPopup || !_cwdBox)
        {
            return;
        }
        _pathPickerUserDismissed = false; // opening clears the dismiss latch

        // Anchor the popup just under the cwd box (left-aligned with it). Capture its left edge in
        // _root coordinates so the width below can be measured against the window's right edge.
        double leftInRoot = 0.0;
        try
        {
            const auto xform = _cwdBox.TransformToVisual(_root);
            const auto pt = xform.TransformPoint(Point{ 0.0f, static_cast<float>(_cwdBox.ActualHeight()) });
            leftInRoot = static_cast<double>(pt.X);
            _pathPopup.HorizontalOffset(leftInRoot);
            _pathPopup.VerticalOffset(static_cast<double>(pt.Y) + 2.0);
        }
        catch (...)
        {
        }

        if (_pathPanelBorder)
        {
            // Width is set EXPLICITLY here, not left to content/MaxWidth: the rows are short folder
            // leaf names, so a MaxWidth cap alone never grows the panel (it sizes to content and sits
            // at the floor). Instead the panel fills from its left anchor out to 10% short of the
            // window's right edge, with a readable floor when the window is narrow. The cwd box width
            // is the lower bound so it's never narrower than the box it drops from.
            const double minWidth = std::max<double>(560.0, _cwdBox.ActualWidth());
            const double rootWidth = _root ? _root.ActualWidth() : 0.0;
            const double rightLimit = rootWidth * 0.9; // leave a 10% gap on the window's right edge
            const double width = std::max<double>(minWidth, rightLimit - leftInRoot);
            _pathPanelBorder.Width(width);
        }
        _RebuildPathPicker();
        _pathPopup.IsOpen(true);
    }

    void AgentManagerContent::_ClosePathPicker()
    {
        if (_pathPopup)
        {
            _pathPopup.IsOpen(false);
        }
    }

    // Normalize the Launch path box in place. Platform-sensitive via NormPath (Windows: flip
    // '/'->'\' and trim insignificant trailing separators; POSIX: trim trailing '/'). Called
    // on commit (Enter), blur, list-pick, and launch — deliberately NOT per-keystroke, which
    // would eat a trailing separator the user types to descend into a folder.
    void AgentManagerContent::_NormalizeCwdBox()
    {
        if (!_cwdBox)
        {
            return;
        }
        const std::wstring cur{ _cwdBox.Text() };
        const std::wstring norm = NormPath(cur);
        if (norm != cur)
        {
            _cwdBox.Text(winrt::hstring{ norm });
            _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        }
    }

    void AgentManagerContent::_PickPath(const std::wstring& dir)
    {
        if (!_cwdBox)
        {
            return;
        }
        std::wstring norm = NormPath(dir); // selecting from the list normalizes too
        // Clicking a directory row DRILLS INTO it: append a separator so the rebuilt picker
        // lists that folder's children unfiltered (the trailing-separator rule above), turning
        // a click into a drill-down like the old picker. The slash is cosmetic — _NormalizeCwdBox
        // strips it again on commit / launch, and a drive root ("K:\") already carries one.
        if (IsDir(norm) && !norm.empty() && norm.back() != L'\\' && norm.back() != L'/')
        {
            norm += L'\\';
        }
        _cwdBox.Text(winrt::hstring{ norm }); // fires TextChanged -> _RebuildPathPicker (popup open)
        _cwdBox.Select(static_cast<int32_t>(norm.size()), 0); // caret to end
        _cwdBox.Focus(FocusState::Programmatic); // keep the box focused so the popup stays open
    }
}
