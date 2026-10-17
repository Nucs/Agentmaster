// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation (8 partial files)
// The TerminalPage-side Agentmaster glue -- engine wiring + session lifecycle + observer +
// window-record + the Sessions browser -- connecting the Manager engine to WT's TerminalPage.
// Same class (TerminalPage, declared in TerminalPage.h), split across same-class partial TUs (the
// upstream TabManagement.cpp pattern) so TerminalPage.cpp stays close to upstream.
//
// Partial files in this group (★ marks THIS file):
//   TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Agent Manager page (+ its Home button), _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
// ★ TerminalPage.AgentSessionsAi.cpp           - the Sessions browser's AI SEARCH: the "AI" toggle, Enter-to-ask, the reset button, the strip, and the model's list in the table
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster -- the Sessions page's AI SEARCH (SESSIONS.md §9, SDK_HARNESS.md).
//
// What the user sees: an "AI" toggle INSIDE the search box, at its right end, like the regex
// toggle of an editor's find box (flat while off, accent-filled while on). With it on, the box
// is a REQUEST line: "the session where I fixed the tab drag crash", Enter. A strip under the header shows
// the request, what happens ("Searching: ..."), then the model's one-sentence answer; the table
// shows exactly the sessions the model picked, in its order. A follow-up ("only the open ones")
// refines the list, because the conversation keeps its context. The crossed circle beside the
// toggle throws the conversation away and brings the normal list back. The strip's right end
// reads "thinking off  ctx 15k / 200k": a click on "thinking off" moves to the next thinking
// level (off -> low -> medium -> high -> xhigh -> max), kept for every window and after a restart.
//
// What is behind it: ONE conversation per process (SharedSessionSearchAgent, SessionSearchAgent.h)
// -- a hosted Sonnet 5.5 (thinking off, unless the strip's toggle picks a level), started through the Claude agent SDK, with two tool
// servers: one searches (the page's own search, SessionSearchTools.h), one shows. This file is
// only the VIEW of that conversation for one window:
//   - it sends requests (Ask), resets (Reset), and tells the agent when no page uses it;
//   - it listens to the agent's events, marshalled to this window's UI thread;
//   - it keeps the list the model showed, and hands it to _RenderSessionsTable (the page's one
//     render chokepoint), which shows those rows instead of the window's rows.
//
// A session the model shows may be OUTSIDE the page's time window (the page lists one month by
// default, the search covers three and can go further). Such a session gets a real row, marked
// aiExtra, appended to _sessionsRows / _sessionsEntries, so that everything a row can do (the
// detail pane, resume, fork, rename, favorite, tags) works on it without a special case.
//
// XAML-Islands rule of the page, kept here: a pointer handler never changes the visual tree; it
// defers to a clean dispatcher tick.
//
// This file implements TerminalPage methods (same class, separate TU -- the TabManagement.cpp
// pattern).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the shared tooltip recipe behind SessSetTip
#include "AgentStatusColors.h" // needed by the shared Sess* helpers (the tag ribbon colors)
#include "AgentMaster/ClaudeSpawn.h" // LogNav
#include "AgentMaster/Engine.h" // EnsureClaudeAvailable / SharedEngine().claudeExePath
#include "AgentMaster/Persistence.h" // needed by the shared Sess* helpers
#include "AgentMaster/ProcessInspect.h" // kSummarySepMark (the shared Sess* helpers)
#include "AgentMaster/ProcessObserver.h" // needed by the shared Sess* helpers
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionSearch.h"
#include "AgentMaster/SessionSearchAgent.h" // SharedSessionSearchAgent / SessionSearchEvent / SessionSearchSnapshot
#include "AgentMaster/SessionStore.h"
#include "AgentMaster/TranscriptStore.h" // SessionIndexEntry

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace std::chrono_literals;
#include "TerminalPage.AgentSessionsPage.Internal.h" // the shared Sess* file-local helpers

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        // How many Sessions pages (one per window) show the AI search right now. The agent is
        // one per process, so "nobody uses it" is a question about all windows: the page that
        // takes this to 0 frees the agent's session index and parks its process. Atomic because
        // a page's destructor can run on a thread-pool thread (the terminate-net note in
        // ~TerminalPage).
        std::atomic<int> g_sessionsAiPagesInUse{ 0 };

        // How long the idle claude.exe of the AI search lives after the last page left it. Long
        // enough that a user who closes the Sessions page, resumes a session and comes back
        // finds the same process (and with it the model's whole context); short enough that a
        // process nobody uses does not hold its memory for the rest of the day. After a park the
        // conversation continues from a digest of its record (HarnessConversation::ParkAfter).
        constexpr int64_t kSessionsAiParkAfterMs = 10 * 60 * 1000;

        // The search box's two placeholders. The AI one says the two things that differ from the
        // filter box: write a sentence, and Enter sends it.
        const wchar_t kSessionsFilterPlaceholder[] = L"search for sessions";
        const wchar_t kSessionsAiPlaceholder[] = L"describe the session you are looking for, then press Enter";

        // The AI toggle sits INSIDE the search box, at its right end. Its size is fixed, so the
        // room the box must leave for it is known before anything is laid out:
        // kSessionsAiToggleReserve = the gap between the text and the toggle + the toggle + its
        // inset from the box's right edge. DIPs.
        constexpr double kSessionsAiToggleWidth = 30.0;
        constexpr double kSessionsAiToggleHeight = 24.0; // the box is 32 high: 4 above and below
        constexpr double kSessionsAiToggleInset = 4.0;
        constexpr double kSessionsAiToggleGap = 2.0;
        constexpr double kSessionsAiToggleReserve = kSessionsAiToggleGap + kSessionsAiToggleWidth + kSessionsAiToggleInset;

        // The first element named `name` in the visual tree under `root`, depth first.
        //   root    - where to look; its own name is not tested.
        //   name    - the x:Name to find (a template part's name, here).
        //   returns - the element, or null when there is none (the template is not applied yet,
        //             or it has no such part).
        //   throws  - what the visual tree calls throw (hresult_error).
        // The tree under a TextBox is a dozen elements, so the walk is cheap.
        FrameworkElement SessAiFindDescendant(const DependencyObject& root, std::wstring_view name)
        {
            const int count = VisualTreeHelper::GetChildrenCount(root);
            for (int i = 0; i < count; ++i)
            {
                const auto child = VisualTreeHelper::GetChild(root, i);
                if (const auto element = child.try_as<FrameworkElement>(); element && std::wstring_view{ element.Name() } == name)
                {
                    return element;
                }
                if (auto found = SessAiFindDescendant(child, name))
                {
                    return found;
                }
            }
            return nullptr;
        }

        // `text` without the white space at its two ends.
        std::wstring SessAiTrim(std::wstring_view text)
        {
            const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
            size_t a = 0;
            size_t b = text.size();
            while (a < b && isSpace(text[a]))
            {
                ++a;
            }
            while (b > a && isSpace(text[b - 1]))
            {
                --b;
            }
            return std::wstring{ text.substr(a, b - a) };
        }
    }

    // One-time, from _BuildSessionsPageShell: add the AI search's controls to the page header.
    //   topLeft — the header's top-left grid (Back · title · search box in columns 0..2). The
    //             toggle goes INTO the search box's cell, over the box's right end; one Auto
    //             column is appended for the reset button.
    //   header  — the header grid (rows: top, filters, tag chips). One Auto row is appended for
    //             the strip.
    // Also hooks the search box: its Enter and Escape keys (they act in AI mode only), and the
    // events at which the box makes room for the toggle (_SessionsAiFitToggleIntoBox).
    // Throws what XAML throws (hresult_error); the caller is the page build, which has no catch
    // of its own — like every other part of the shell.
    void TerminalPage::_BuildSessionsAiHeader(const Grid& topLeft, const Grid& header)
    {
        // The toggle lives INSIDE the search box, at its right end — like the regex or
        // match-case toggle of an editor's find box (the user's request). It says how the box
        // reads its text, so it belongs to the box, not beside it.
        // It is NOT a part of the TextBox: it is a sibling in the same grid cell, appended after
        // the box, so it is drawn over the box's right end and takes the pointer there. The box
        // keeps its whole chrome (border, hover, the focus underline) around both, and leaves
        // the toggle room (_SessionsAiFitToggleIntoBox).
        const int boxColumn = _sessionsSearchBox ? Grid::GetColumn(_sessionsSearchBox) : 2;

        // Its Click only reads the new state; the mode change itself (it re-renders the table and
        // may open a dialog) runs on a clean tick.
        _sessAiBtn = SessToggle(L"AI", L"AI search", L"Ask for sessions in your own words instead of filtering by words. With it on, write what you remember \x2014 \x201Cthe session where I fixed the tab drag crash\x201D \x2014 and press Enter: a Claude model (Sonnet) searches your sessions and puts the ones that fit in the list, best match first. Then refine it: \x201Conly the open ones\x201D, \x201Colder than that\x201D.\n\nThere is one such conversation, shared by every window. It never shows up among your sessions, and it changes nothing: it can only search and show.");
        _sessAiBtn.Width(kSessionsAiToggleWidth);
        _sessAiBtn.Height(kSessionsAiToggleHeight);
        _sessAiBtn.Padding(Thickness{ 0, 0, 0, 0 });
        _sessAiBtn.FontSize(11);
        _sessAiBtn.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        // Flat while it is off: no fill and no border of its own, so the box's fill shows through
        // and the two read as one control. These are LOCAL values, which only the rest state
        // uses: the stock hover and pressed fills still show, and so does the accent fill of the
        // checked state — the one cue that the box is in AI mode. (No re-template, no resource
        // key: a visual state's setter outranks a local value, which is exactly what is wanted.)
        _sessAiBtn.Background(SessBrush(0, 0, 0, 0));
        _sessAiBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        _sessAiBtn.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 3, 3, 3, 3 });
        _sessAiBtn.HorizontalAlignment(HorizontalAlignment::Right);
        _sessAiBtn.VerticalAlignment(VerticalAlignment::Center);
        _sessAiBtn.Margin(Thickness{ 0, 0, kSessionsAiToggleInset, 0 });
        _sessAiBtn.Click([this](const winrt::Windows::Foundation::IInspectable& s, const RoutedEventArgs&) {
            const auto button = s.try_as<Primitives::ToggleButton>();
            const bool on = button && button.IsChecked() && button.IsChecked().Value();
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), on]() {
                if (auto self = weak.get())
                {
                    self->_SetSessionsAiMode(on);
                }
            });
        });
        Grid::SetColumn(_sessAiBtn, boxColumn);
        topLeft.Children().Append(_sessAiBtn); // after the box: over it

        // The reset: a crossed circle, in a column of its own right of the box. A FontIcon, not a
        // TextBlock: a TextBlock keeps room for descenders and the glyph then sits high beside
        // the box (the compose-bar lesson).
        {
            ColumnDefinition c;
            c.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            topLeft.ColumnDefinitions().Append(c);
        }
        const int resetColumn = static_cast<int>(topLeft.ColumnDefinitions().Size()) - 1;
        _sessAiResetBtn = Button{};
        {
            FontIcon icon;
            // Qualified: inside a TerminalPage member the bare name is the page's own FontFamily property.
            icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ L"Segoe UI Symbol" });
            icon.Glyph(L"\x2298"); // CIRCLED DIVISION SLASH
            icon.FontSize(15);
            _sessAiResetBtn.Content(icon);
        }
        _sessAiResetBtn.Padding(Thickness{ 6, 2, 6, 2 });
        _sessAiResetBtn.MinWidth(0);
        _sessAiResetBtn.MinHeight(0);
        _sessAiResetBtn.Margin(Thickness{ 6, 0, 0, 0 });
        _sessAiResetBtn.VerticalAlignment(VerticalAlignment::Center);
        _sessAiResetBtn.Visibility(Visibility::Collapsed); // AI mode only
        SessSetTip(_sessAiResetBtn, L"New conversation", L"Throw the AI search's conversation away and start a new one: the model forgets what you asked and what it showed, and the list goes back to normal. Use it when you start looking for something else \x2014 a follow-up refers to the list before it.\n\nIt clears the conversation in every window. Your sessions are not touched.");
        _sessAiResetBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                const auto self = weak.get();
                if (!self)
                {
                    return;
                }
                try
                {
                    ::Agentmaster::LogNav(L"sessions ai-reset");
                    ::Agentmaster::SharedSessionSearchAgent().Reset(); // the Reset event clears this page's list (and every other window's)
                    if (self->_sessionsSearchBox)
                    {
                        self->_sessionsSearchBox.Focus(FocusState::Programmatic); // the user who resets is about to ask again
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"Sessions AI search: reset");
                }
            });
        });
        Grid::SetColumn(_sessAiResetBtn, resetColumn);
        topLeft.Children().Append(_sessAiResetBtn);

        // The strip: a header row of its own, as wide as the table (the same column mirror the
        // filters row and the tag chips row use).
        {
            RowDefinition r;
            r.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.RowDefinitions().Append(r);
        }
        Grid stripRow;
        {
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                stripRow.ColumnDefinitions().Append(c);
            };
            col(0.6, GridUnitType::Star); // == body table column (the strip lives here)
            col(0, GridUnitType::Auto); // == body divider footprint
            col(0.4, GridUnitType::Star); // == body detail column (alignment space)
        }
        Grid::SetRow(stripRow, static_cast<int>(header.RowDefinitions().Size()) - 1);
        header.Children().Append(stripRow);

        _sessAiStrip = Border{};
        _sessAiStrip.Background(SessBrush(0xFF, 0x26, 0x26, 0x26));
        _sessAiStrip.BorderBrush(SessBrush(0xFF, 0x3A, 0x3A, 0x3A));
        _sessAiStrip.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _sessAiStrip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 6, 6, 6, 6 });
        _sessAiStrip.Padding(Thickness{ 10, 6, 10, 6 });
        _sessAiStrip.Margin(Thickness{ 0, 8, 0, 0 });
        _sessAiStrip.Visibility(Visibility::Collapsed); // AI mode only
        Grid inner;
        {
            const auto col = [&](double v, GridUnitType t) {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromValueAndType(v, t));
                inner.ColumnDefinitions().Append(c);
            };
            col(0, GridUnitType::Auto); // the state dot
            col(1, GridUnitType::Star); // the request line over the status line
            col(0, GridUnitType::Auto); // the thinking toggle, then the context size
        }
        _sessAiDot = Border{};
        _sessAiDot.Width(8);
        _sessAiDot.Height(8);
        _sessAiDot.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
        _sessAiDot.Margin(Thickness{ 0, 5, 8, 0 });
        _sessAiDot.VerticalAlignment(VerticalAlignment::Top);
        _sessAiDot.Background(SessBrush(0xFF, 0x80, 0x80, 0x80));
        Grid::SetColumn(_sessAiDot, 0);
        inner.Children().Append(_sessAiDot);

        StackPanel lines;
        lines.Spacing(2);
        _sessAiQuestionText = SessText(L"", 11, false, 0.6);
        _sessAiQuestionText.Visibility(Visibility::Collapsed); // until there is a request
        lines.Children().Append(_sessAiQuestionText);
        _sessAiStatusText = SessText(L"", 12, false, 0.95);
        _sessAiStatusText.TextWrapping(TextWrapping::Wrap);
        // SESSIONS.md §9.14: the model's plan while it works ("I'll 1. ... 2. ... 3."), then its descriptive reply —
        // several numbered lines, up to ~1,200 characters, no longer the one sentence MaxLines(4) was sized for. Every
        // line shows, and the text can be selected and copied; the box below caps the strip's height instead.
        _sessAiStatusText.MaxLines(0);
        _sessAiStatusText.IsTextSelectionEnabled(true);
        // A long reply scrolls inside the strip rather than pushing the session table down the page.
        ScrollViewer statusScroll;
        statusScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        statusScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        statusScroll.MaxHeight(180);
        statusScroll.Content(_sessAiStatusText);
        lines.Children().Append(statusScroll);
        Grid::SetColumn(lines, 1);
        inner.Children().Append(lines);

        // The right end reads "thinking off  ctx 15k / 200k": the thinking toggle, then the context
        // size, side by side and centered on each other.
        StackPanel rightEnd;
        rightEnd.Orientation(Orientation::Horizontal);
        rightEnd.VerticalAlignment(VerticalAlignment::Top);
        rightEnd.Margin(Thickness{ 10, 0, 0, 0 });

        // The thinking toggle: a FLAT button (no fill, no border at rest — local values that only
        // the rest state uses, so the stock hover and pressed fills still show), as small as the
        // strip's text, so the row keeps its height. A click moves to the next level; the change
        // itself runs on a clean tick (the page's pointer-handler rule).
        _sessAiThinkingText = SessText(L"thinking off", 11, false, 0.6);
        _sessAiThinkingBtn = Button{};
        _sessAiThinkingBtn.Content(_sessAiThinkingText);
        _sessAiThinkingBtn.Padding(Thickness{ 5, 0, 5, 1 });
        _sessAiThinkingBtn.MinWidth(0);
        _sessAiThinkingBtn.MinHeight(0);
        _sessAiThinkingBtn.Background(SessBrush(0, 0, 0, 0));
        _sessAiThinkingBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        _sessAiThinkingBtn.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 3, 3, 3, 3 });
        _sessAiThinkingBtn.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(_sessAiThinkingBtn, L"Thinking", L"How much the model thinks before it searches. Click for the next level: off \x2192 low \x2192 medium \x2192 high \x2192 xhigh \x2192 max \x2192 off.\n\nOff is the fastest: the model does not think before it answers (at most, the short notes it writes between two searches can count as thinking). The other levels turn on adaptive thinking (the model decides when to think), and the level (Claude Code\x2019s effort) sets how deep. A deeper level answers harder requests better and takes longer.\n\nIt applies from your next request, in every window, and is kept after a restart. The conversation keeps its context.");
        _sessAiThinkingBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (auto self = weak.get())
                {
                    self->_CycleSessionsAiThinking();
                }
            });
        });
        rightEnd.Children().Append(_sessAiThinkingBtn);

        _sessAiUsageText = SessText(L"", 11, false, 0.5);
        _sessAiUsageText.Margin(Thickness{ 4, 0, 0, 0 });
        _sessAiUsageText.VerticalAlignment(VerticalAlignment::Center);
        SessSetTip(_sessAiUsageText, L"Conversation size", L"How much the AI search's conversation holds, in tokens, of the most it can hold before Claude Code compacts it by itself: the model's own window or 500K, whichever is smaller. A longer conversation is not slower to answer, but a new subject is better served by a new conversation: the crossed circle.");
        rightEnd.Children().Append(_sessAiUsageText);
        Grid::SetColumn(rightEnd, 2);
        inner.Children().Append(rightEnd);

        _sessAiStrip.Child(inner);
        Grid::SetColumn(_sessAiStrip, 0);
        stripRow.Children().Append(_sessAiStrip);

        // The box makes room for the toggle that lies over its right end. At Loaded its style is
        // applied (its padding is known) and its template exists. GotFocus and TextChanged try
        // again until the box's own clear button was found: they are the two moments at which
        // the box can show that button, should a template create it only then.
        if (_sessionsSearchBox)
        {
            const auto fit = [this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                _SessionsAiFitToggleIntoBox();
            };
            _sessionsSearchBox.Loaded(fit);
            _sessionsSearchBox.GotFocus(fit);
            _sessionsSearchBox.TextChanged([this](const winrt::Windows::Foundation::IInspectable&, const TextChangedEventArgs&) {
                _SessionsAiFitToggleIntoBox();
            });
        }

        // The box's keys, in AI mode: Enter sends the request; Escape stops a request that runs.
        // KeyDown on the box (not the page host's PreviewKeyDown, which owns Up / Down): the two
        // keys mean this only while the caret is in the box.
        if (_sessionsSearchBox)
        {
            _sessionsSearchBox.KeyDown([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
                if (!_sessionsAiMode)
                {
                    return;
                }
                if (e.Key() == VirtualKey::Enter)
                {
                    e.Handled(true);
                    Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                        if (auto self = weak.get())
                        {
                            self->_SessionsAiAsk();
                        }
                    });
                }
                else if (e.Key() == VirtualKey::Escape)
                {
                    try
                    {
                        auto& agent = ::Agentmaster::SharedSessionSearchAgent();
                        if (agent.Snapshot().busy)
                        {
                            // Only then is the key ours: an Escape with nothing running must
                            // keep its meaning for whatever else the page does with it.
                            e.Handled(true);
                            ::Agentmaster::LogNav(L"sessions ai-stop");
                            agent.Interrupt();
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"Sessions AI search: stop a request");
                    }
                }
            });
        }
    }

    // Make room in the search box for the AI toggle that lies over its right end. Two changes,
    // each made once:
    //   1. The box's right padding grows by kSessionsAiToggleReserve, so the text and the
    //      placeholder end before the toggle. (Read from the box, not hard-coded: the style's
    //      padding stays what it is.)
    //   2. The box's own clear button — the template part "DeleteButton", the X that shows while
    //      the box has focus and text — is shifted left by the same amount with a render
    //      transform, so it stands left of the toggle and not under it. The geometry: the button
    //      is the template's last column, and the text column's padding grew by the reserve. So
    //      a button shifted by the reserve keeps its old distance from the text and ends where
    //      the toggle's room begins (text | X | toggle), and with the X hidden the text still
    //      ends before the toggle. A render transform moves the hit area with the picture.
    // Called at the box's Loaded, GotFocus and TextChanged; a no-op once both are done. Before
    // the box is loaded it does nothing (its style's padding is not known yet).
    // When the template has no such part, change 2 never happens and the X would show under the
    // toggle: a cosmetic fault only (the toggle takes the click), and every call tries again.
    // Never throws: a failure is logged, and the next call tries again.
    void TerminalPage::_SessionsAiFitToggleIntoBox()
    {
        if (!_sessionsSearchBox || (_sessAiBoxPadded && _sessAiClearButtonMoved))
        {
            return;
        }
        try
        {
            if (!_sessionsSearchBox.IsLoaded())
            {
                return;
            }
            if (!_sessAiBoxPadded)
            {
                auto padding = _sessionsSearchBox.Padding();
                padding.Right += kSessionsAiToggleReserve;
                _sessionsSearchBox.Padding(padding);
                _sessAiBoxPadded = true; // set once it is applied: a second Loaded must not add the room again
            }
            if (!_sessAiClearButtonMoved)
            {
                if (const auto clear = SessAiFindDescendant(_sessionsSearchBox, L"DeleteButton"))
                {
                    TranslateTransform shift;
                    shift.X(-kSessionsAiToggleReserve);
                    clear.RenderTransform(shift);
                    _sessAiClearButtonMoved = true;
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SessionsAiFitToggleIntoBox");
        }
    }

    // Turn the AI search on or off for this page.
    //   on — true: the search box becomes the request line, the strip and the reset button
    //        show, the agent is warmed up (its process starts, its index is read), and the
    //        conversation's last list — when it has one — replaces the table's rows.
    //        false: back to the filter box and the normal list; the conversation is kept.
    // "On" is refused, with the page's "Claude not found" notice, when there is no native
    // claude.exe: the AI search runs through it.
    // Never throws: a failure is logged and leaves the mode off.
    void TerminalPage::_SetSessionsAiMode(bool on)
    {
        try
        {
            if (on == _sessionsAiMode)
            {
                if (_sessAiBtn)
                {
                    _sessAiBtn.IsChecked(on); // keep the button honest after a refused "on"
                }
                return;
            }
            if (on && !::Agentmaster::EnsureClaudeAvailable())
            {
                ::Agentmaster::LogNav(L"sessions ai on (blocked: no native claude.exe)");
                if (_sessAiBtn)
                {
                    _sessAiBtn.IsChecked(false);
                }
                _PromptClaudeMissing();
                return;
            }
            if (on)
            {
                // First the one step that can fail for a reason other than memory: the first use
                // creates the process's agent. A mode that is on without its agent would show a
                // strip that never changes; a throw here leaves the mode off (the catch below).
                _SessionsAiAttach();
            }
            _sessionsAiMode = on;
            ::Agentmaster::LogNav(on ? L"sessions ai on" : L"sessions ai off");
            if (_sessAiBtn)
            {
                _sessAiBtn.IsChecked(on);
            }
            // The box changes its meaning, so it starts empty either way: a half-typed filter
            // must not be sent as a request, and a half-typed request must not filter the list.
            // (TextChanged does nothing while _sessionsAiMode is set, so clear the query here.)
            _sessionsQueryText.clear();
            if (_sessionsSearchBox)
            {
                _sessionsSearchBox.Text(L"");
                _sessionsSearchBox.PlaceholderText(on ? kSessionsAiPlaceholder : kSessionsFilterPlaceholder);
            }
            ::Agentmaster::SessionSearchSnapshot snapshot;
            if (on)
            {
                auto& agent = ::Agentmaster::SharedSessionSearchAgent();
                agent.SetClaudeExePath(::Agentmaster::SharedEngine().claudeExePath); // UI thread: the path's owner
                _SessionsAiSetInUse(_sessionsPageVisible.load(std::memory_order_relaxed));
                // While the page's own gather runs, it reads the same sidecar files: the agent
                // reads its index when that is done (_SessionsAiAfterGather), the process starts now.
                agent.Prewarm(!_sessionsIndexing.load());
                snapshot = agent.Snapshot();
                _sessionsAiEpoch = snapshot.epoch;
                _SessionsAiAdoptSnapshot(snapshot);
            }
            else
            {
                _SessionsAiSetInUse(false);
                _sessionsAiHasShown = false;
                _sessionsAiPreview = false;
                _sessionsAiListTurn = 0;
                _sessionsAiShownIds.clear();
                _sessionsAiEntries.clear();
                _sessionsAiReasons.clear(); // no AI list: no evidence lines under the titles
                _SessionsAiMergeExtraRows(); // removes the list's extra rows
            }
            _UpdateSessionsAiStrip(snapshot);
            _RunSessionsSearch(); // the (now empty) filter query, then the render — which applies the AI list when there is one
            if (_sessionsSearchBox)
            {
                _sessionsSearchBox.Focus(FocusState::Programmatic);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SetSessionsAiMode");
            // Keep the button honest: it must not read "on" for a mode that did not start.
            try
            {
                if (_sessAiBtn)
                {
                    _sessAiBtn.IsChecked(_sessionsAiMode);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SetSessionsAiMode: restore the toggle");
            }
        }
    }

    // Send the search box's text to the conversation. The newest request wins: one that still
    // runs is interrupted (SessionSearchAgent::Ask). An empty box does nothing. The box is then
    // cleared: the strip shows the request, and the box is free for the follow-up.
    // Never throws.
    void TerminalPage::_SessionsAiAsk()
    {
        try
        {
            if (!_sessionsAiMode || !_sessionsSearchBox)
            {
                return;
            }
            const std::wstring text = SessAiTrim(std::wstring{ _sessionsSearchBox.Text() });
            if (text.empty())
            {
                return;
            }
            // The path can be gone since the mode was turned on (the Settings cog's override).
            if (!::Agentmaster::EnsureClaudeAvailable())
            {
                ::Agentmaster::LogNav(L"sessions ai-ask (blocked: no native claude.exe)");
                _PromptClaudeMissing();
                return;
            }
            _SessionsAiAttach();
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            agent.SetClaudeExePath(::Agentmaster::SharedEngine().claudeExePath);
            _SessionsAiSetInUse(true); // a request is use, whatever the tally thought
            // The window the request comes from: the tabs server tells the model (asked_from), so with several windows it
            // can offer this one when it asks the user where to open a session (SESSIONS.md §9.14).
            const uint64_t turn = agent.Ask(text, _windowId);
            // The words stay out of the nav trail (the agent's own [ai-search] line has them).
            ::Agentmaster::LogNav(L"sessions ai-ask turn=" + std::to_wstring(turn) + L" chars=" + std::to_wstring(text.size()));
            _sessionsSearchBox.Text(L"");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SessionsAiAsk");
        }
    }

    // Register this page's listener on the process's agent — once. The agent calls a listener
    // on ITS threads; the listener only posts the event to this window's dispatcher.
    //   throws — what SharedSessionSearchAgent() throws on its first call (the agent could not
    //            be created), std::bad_alloc.
    void TerminalPage::_SessionsAiAttach()
    {
        if (_sessionsAiListenerToken != 0)
        {
            return;
        }
        const auto weakThis = get_weak();
        const auto dispatcher = Dispatcher(); // agile: callable from the agent's threads
        _sessionsAiListenerToken = ::Agentmaster::SharedSessionSearchAgent().AddListener([weakThis, dispatcher](const ::Agentmaster::SessionSearchEvent& e) {
            // ⚠ No strong reference to the page on this thread. A strong reference that is the
            // LAST one would run ~TerminalPage here, off the UI thread (the terminate-net note
            // in ~TerminalPage). The weak reference is resolved inside the dispatched call only.
            dispatcher.RunAsync(CoreDispatcherPriority::Normal, [weakThis, e]() {
                if (auto self = weakThis.get())
                {
                    try
                    {
                        self->_OnSessionsAiEvent(e);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"Sessions AI search: an agent event");
                    }
                }
            });
        });
    }

    // ~TerminalPage: remove the listener and leave the in-use tally (Rule #10: a closing window
    // detaches from the process-wide engine). Thread-safe — the destructor can run on a
    // thread-pool thread — and it touches no XAML. Never throws.
    void TerminalPage::_SessionsAiDetach() noexcept
    {
        try
        {
            if (_sessionsAiListenerToken != 0)
            {
                // A token exists only when the agent does: this call cannot create it.
                ::Agentmaster::SharedSessionSearchAgent().RemoveListener(_sessionsAiListenerToken);
                _sessionsAiListenerToken = 0;
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SessionsAiDetach");
        }
        _SessionsAiSetInUse(false);
    }

    // Count this page in or out of the pages that show the AI search.
    //   inUse — true: this page shows it now. false: it does not (the mode is off, the page was
    //           closed, or the window goes away).
    // The page that takes the count to 0 frees the agent's session index (tens of megabytes)
    // and parks its process after kSessionsAiParkAfterMs. Idempotent. Never throws.
    void TerminalPage::_SessionsAiSetInUse(bool inUse) noexcept
    {
        if (inUse == _sessionsAiInUse)
        {
            return;
        }
        _sessionsAiInUse = inUse;
        try
        {
            if (inUse)
            {
                g_sessionsAiPagesInUse.fetch_add(1);
                return;
            }
            if (g_sessionsAiPagesInUse.fetch_sub(1) > 1)
            {
                return; // another window's page still shows it
            }
            // _sessionsAiInUse was true, so the agent exists: this call cannot create it.
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            agent.ReleaseCatalog();
            agent.ParkProcessAfter(kSessionsAiParkAfterMs);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SessionsAiSetInUse");
        }
    }

    // The page came on screen or left it: opened (_ShowSessionsPage), closed (_HideSessionsPage),
    // or covered / uncovered by a tab switch (the page overlay registry's dismiss / restore
    // hooks). A page behind another tab counts as "not shown": the user may not come back for
    // hours, and an idle claude.exe holds about 400 MB. The park is delayed
    // (kSessionsAiParkAfterMs), so a short trip to a tab keeps the same process.
    //   shown — true when the page is on screen now, false when it is not.
    // With the AI search on, coming on screen starts the agent's process again (it may have
    // been parked). Its index is read at once — unless the page's own gather runs (at an open
    // it does): then the index follows that gather (_SessionsAiAfterGather), so that the two
    // do not parse the same transcripts at the same time.
    // Never throws.
    void TerminalPage::_SessionsAiNotePageShown(bool shown)
    {
        if (!_sessionsAiMode)
        {
            return;
        }
        _SessionsAiSetInUse(shown);
        if (!shown)
        {
            return;
        }
        try
        {
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            agent.SetClaudeExePath(::Agentmaster::SharedEngine().claudeExePath);
            agent.Prewarm(!_sessionsIndexing.load());
            _UpdateSessionsAiStrip(agent.Snapshot());
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SessionsAiNotePageShown");
        }
    }

    // The page's own gather landed (_RefreshSessionsRows, on the UI thread, right after it
    // replaced _sessionsRows / _sessionsEntries):
    //   1. the gather knows only the page's time window, so the AI list's rows outside it are
    //      gone — put them back;
    //   2. the sidecar files of the page's window are current now, so the agent can read its
    //      index without parsing the same transcripts a second time.
    // Never throws.
    void TerminalPage::_SessionsAiAfterGather()
    {
        try
        {
            _SessionsAiMergeExtraRows();
            if (_sessionsAiMode && _sessionsAiInUse)
            {
                ::Agentmaster::SharedSessionSearchAgent().Prewarm(true);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SessionsAiAfterGather");
        }
    }

    // One event of the agent, on this window's UI thread.
    //   e — the event, with the conversation's state after it (e.snapshot).
    // A page that does not show the AI search only notes the epoch: turning the mode on adopts
    // the conversation's state as a whole (_SetSessionsAiMode).
    //   throws — what the render throws (hresult_error); the dispatched caller logs it.
    void TerminalPage::_OnSessionsAiEvent(const ::Agentmaster::SessionSearchEvent& e)
    {
        using Kind = ::Agentmaster::SessionSearchEventKind;
        if (e.snapshot.epoch < _sessionsAiEpoch)
        {
            return; // an event of a conversation that was reset away
        }
        _sessionsAiEpoch = e.snapshot.epoch;
        if (!_sessionsAiMode)
        {
            return;
        }
        bool render = false;
        bool finalList = false; // the model's own pick arrived: select its first row
        switch (e.kind)
        {
        case Kind::Reset:
            _SessionsAiAdoptSnapshot(e.snapshot); // an empty conversation: no list, the normal rows come back
            _sessionsSelectedId.clear();
            render = true;
            break;
        case Kind::Candidates:
            // A search's rows, before the model picked from them. Showing them at once is what
            // makes the search feel instant (the model's pick comes about a second later and
            // replaces them). Not when they are empty — a blank table while the model tries
            // other words reads as "nothing found" — and never over the final list of the same
            // request.
            if (!e.sessionIds.empty() && (e.turn > _sessionsAiListTurn || (e.turn == _sessionsAiListTurn && _sessionsAiPreview)))
            {
                _sessionsAiPreview = true;
                _sessionsAiListTurn = e.turn;
                _sessionsAiShownIds = e.sessionIds;
                _sessionsAiEntries = e.entries;
                _sessionsAiReasons.clear(); // a search's rows: the model has read none of them yet, so no evidence
                _sessionsAiSortColumn = _sessionsSortColumn;
                _sessionsAiSortAscending = _sessionsSortAscending;
                _SessionsAiMergeExtraRows();
                render = true;
            }
            break;
        case Kind::Shown:
            _sessionsAiHasShown = true;
            _sessionsAiPreview = false;
            _sessionsAiListTurn = e.turn;
            _sessionsAiShownIds = e.sessionIds;
            _sessionsAiEntries = e.entries;
            _SessionsAiTakeReasons(e.sessionIds, e.reasons);
            _sessionsAiSortColumn = _sessionsSortColumn;
            _sessionsAiSortAscending = _sessionsSortAscending;
            _SessionsAiMergeExtraRows();
            render = true;
            finalList = true;
            break;
        case Kind::Answer:
        case Kind::Failed:
            if (_sessionsAiPreview)
            {
                // The request ended without a show call (the model answered in words, or it
                // failed): the preview was never the model's pick. Back to the conversation's
                // real list.
                _SessionsAiAdoptSnapshot(e.snapshot);
                render = true;
            }
            break;
        case Kind::StateChanged:
        case Kind::Asked:
        case Kind::Searching:
        case Kind::Compacting:
        default:
            break; // the strip below is all they change
        }
        _UpdateSessionsAiStrip(e.snapshot);
        if (!render)
        {
            return;
        }
        if (finalList)
        {
            // The best match is selected, so its detail pane is there without a click.
            _sessionsSelectedId = _sessionsAiShownIds.empty() ? std::wstring{} : _sessionsAiShownIds.front();
        }
        _RenderSessionsTable();
        if (finalList || e.kind == Kind::Reset)
        {
            _ShowSessionsDetail(_sessionsSelectedId); // an empty id shows the "select a session" text
            if (_sessionsRowsScroll)
            {
                _sessionsRowsScroll.ChangeView(nullptr, 0.0, nullptr, true); // a new list starts at its top
            }
        }
    }

    // Take the conversation's shown list as this page's list: at "AI on", after a reset, and
    // when a request ends while only a preview is on the screen.
    //   snapshot — the conversation's state. hasShown false = no list: the normal rows show.
    // The rows outside the page's window need their index entries; they come from the agent's
    // in-memory index (EntriesFor). When that index was released (no page used the AI search),
    // such rows are missing until the next request — the rows inside the window always show.
    //   throws — std::bad_alloc; what SharedSessionSearchAgent() throws on its first call.
    void TerminalPage::_SessionsAiAdoptSnapshot(const ::Agentmaster::SessionSearchSnapshot& snapshot)
    {
        _sessionsAiHasShown = snapshot.hasShown;
        _sessionsAiPreview = false;
        // The list's request is not recorded in the snapshot; the newest request is the latest
        // it can be. A preview of that request is then not shown over it, one of a later is.
        _sessionsAiListTurn = snapshot.hasShown ? snapshot.turns : 0;
        _sessionsAiShownIds = snapshot.shownIds;
        _SessionsAiTakeReasons(snapshot.shownIds, snapshot.shownReasons);
        _sessionsAiEntries.clear();
        if (snapshot.hasShown && !snapshot.shownIds.empty())
        {
            _sessionsAiEntries = ::Agentmaster::SharedSessionSearchAgent().EntriesFor(snapshot.shownIds);
        }
        _sessionsAiSortColumn = _sessionsSortColumn;
        _sessionsAiSortAscending = _sessionsSortAscending;
        _SessionsAiMergeExtraRows();
    }

    // Take the model's evidence for a shown list (SESSIONS.md §9.12): `reasons` is index-aligned
    // with `ids` (a shorter list leaves the rest without one); an empty why is not stored, so a
    // row with no evidence draws no quote line.
    //   throws — std::bad_alloc.
    void TerminalPage::_SessionsAiTakeReasons(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& reasons)
    {
        _sessionsAiReasons.clear();
        for (size_t i = 0; i < ids.size() && i < reasons.size(); ++i)
        {
            if (!reasons[i].empty())
            {
                _sessionsAiReasons.insert_or_assign(ids[i], reasons[i]);
            }
        }
    }

    // The model's why for a row of the AI list on screen, or "" — none while the AI search is off,
    // shows no list, or shows a search's preview (the model has read none of those rows yet).
    //   throws — std::bad_alloc.
    std::wstring TerminalPage::_SessionsAiReasonFor(const std::wstring& sessionId) const
    {
        if (!_sessionsAiMode || !_sessionsAiHasShown || _sessionsAiPreview)
        {
            return {};
        }
        const auto it = _sessionsAiReasons.find(sessionId);
        return it != _sessionsAiReasons.end() ? it->second : std::wstring{};
    }

    // Make _sessionsRows / _sessionsEntries hold exactly the extra rows the current AI list
    // needs: remove every aiExtra row, then add one for each listed session that the page's
    // gather did not list (it is outside the page's time window).
    // The two vectors stay index-aligned (row i <-> entry i), which the gather's title overlay
    // and the rename path rely on; when they are not aligned to begin with, only the rows are
    // changed (every reader of the entries checks the id).
    //   throws — std::bad_alloc.
    void TerminalPage::_SessionsAiMergeExtraRows()
    {
        const bool aligned = _sessionsEntries.size() == _sessionsRows.size();
        size_t keep = 0;
        for (size_t i = 0; i < _sessionsRows.size(); ++i)
        {
            if (_sessionsRows[i].aiExtra)
            {
                continue;
            }
            if (keep != i)
            {
                _sessionsRows[keep] = std::move(_sessionsRows[i]);
                if (aligned)
                {
                    _sessionsEntries[keep] = std::move(_sessionsEntries[i]);
                }
            }
            ++keep;
        }
        _sessionsRows.resize(keep);
        if (aligned)
        {
            _sessionsEntries.resize(keep);
        }
        if (!_sessionsAiMode || !(_sessionsAiHasShown || _sessionsAiPreview))
        {
            return; // no AI list: no extra rows
        }
        std::unordered_set<std::wstring> have;
        have.reserve(_sessionsRows.size() + _sessionsAiEntries.size());
        for (const auto& r : _sessionsRows)
        {
            have.insert(r.id);
        }
        const std::unordered_set<std::wstring> listed(_sessionsAiShownIds.begin(), _sessionsAiShownIds.end());
        for (const auto& entry : _sessionsAiEntries)
        {
            if (listed.count(entry.sessionId) == 0 || !have.insert(entry.sessionId).second)
            {
                continue; // not in the list any more, or the page has the row already
            }
            _SessionsRow row = _SessionsRowFromIndexEntry(entry);
            row.aiExtra = true;
            _sessionsRows.push_back(std::move(row));
            if (aligned)
            {
                _sessionsEntries.push_back(entry);
            }
        }
    }

    // _RenderSessionsTable's hook: while the AI search shows a list, the table shows EXACTLY
    // that list — not the window's rows after the filters.
    //   view      — in: the rows the normal filters chose. out, when this returns true: the
    //               listed sessions that have a row, in the model's order.
    //   keepOrder — out, when this returns true: true = keep that order (the caller must not
    //               sort); false = the user picked another sort since the list arrived, so the
    //               column sort applies to these rows like to any others.
    //   returns   — true when an AI list is active; false leaves `view` and `keepOrder` alone.
    // The Open / Favorite / Hidden / row filters do not apply to the list: the model chose
    // these sessions for the request, and a hidden session that the user asks for by content is
    // one they want to see.
    //   throws — std::bad_alloc.
    bool TerminalPage::_SessionsAiApplyView(std::vector<const _SessionsRow*>& view, bool& keepOrder) const
    {
        if (!_sessionsAiMode || !(_sessionsAiHasShown || _sessionsAiPreview))
        {
            return false;
        }
        std::unordered_map<std::wstring, const _SessionsRow*> byId;
        byId.reserve(_sessionsRows.size());
        for (const auto& r : _sessionsRows)
        {
            byId.emplace(r.id, &r);
        }
        view.clear();
        view.reserve(_sessionsAiShownIds.size());
        for (const auto& id : _sessionsAiShownIds)
        {
            if (const auto it = byId.find(id); it != byId.end())
            {
                view.push_back(it->second);
            }
        }
        keepOrder = _sessionsSortColumn == _sessionsAiSortColumn && _sessionsSortAscending == _sessionsAiSortAscending;
        return true;
    }

    // Repaint what the page shows OF THE CONVERSATION, outside the table: the strip (request,
    // status, context size), the state dot, and the reset button.
    //   snapshot — the conversation's state (an event's, or SessionSearchAgent::Snapshot()).
    // With the AI search off, the strip and the reset button are hidden and nothing else is done.
    //   throws — what XAML throws (hresult_error).
    void TerminalPage::_UpdateSessionsAiStrip(const ::Agentmaster::SessionSearchSnapshot& snapshot)
    {
        using State = ::Agentmaster::HarnessState;
        const auto visible = _sessionsAiMode ? Visibility::Visible : Visibility::Collapsed;
        if (_sessAiStrip)
        {
            _sessAiStrip.Visibility(visible);
        }
        if (_sessAiResetBtn)
        {
            _sessAiResetBtn.Visibility(visible);
            // Nothing to throw away in a conversation that has not started.
            _sessAiResetBtn.IsEnabled(snapshot.turns > 0 || snapshot.busy || snapshot.hasShown);
        }
        if (!_sessionsAiMode || !_sessAiStatusText || !_sessAiQuestionText || !_sessAiDot || !_sessAiUsageText)
        {
            return;
        }
        // The request.
        if (snapshot.question.empty())
        {
            _sessAiQuestionText.Visibility(Visibility::Collapsed);
        }
        else
        {
            _sessAiQuestionText.Text(winrt::hstring{ L"\x276F " + snapshot.question });
            _sessAiQuestionText.Visibility(Visibility::Visible);
        }
        // The status line and the dot. One of: working (blue), an error (red), the answer
        // (green dot, plain text), or — before any request — how to use it (gray dot, dim text).
        std::wstring status;
        Color dot = ColorHelper::FromArgb(0xFF, 0x80, 0x80, 0x80);
        Color ink = ColorHelper::FromArgb(0xFF, 0xF2, 0xF2, 0xF2);
        double inkOpacity = 0.95;
        if (snapshot.busy)
        {
            status = !snapshot.activity.empty() ? snapshot.activity : std::wstring{ L"Thinking\x2026" };
            if (_sessionsAiPreview)
            {
                status += L" \x2014 " + std::to_wstring(_sessionsAiShownIds.size()) + (_sessionsAiShownIds.size() == 1 ? L" candidate, choosing\x2026" : L" candidates, choosing\x2026");
            }
            // SESSIONS.md §9.14: the plan the model wrote before its first call ("I'll 1. ... 2. ... 3."), above what it
            // does right now — the user sees what it set out to do while it works.
            if (!snapshot.narration.empty())
            {
                status = snapshot.narration + L"\n" + status;
            }
            dot = ColorHelper::FromArgb(0xFF, 0x4E, 0xA1, 0xFF);
            ink = ColorHelper::FromArgb(0xFF, 0x8A, 0xB4, 0xF8);
        }
        else if (snapshot.answerIsError || snapshot.state == State::Failed)
        {
            status = !snapshot.answer.empty() ? snapshot.answer : std::wstring{ L"The search assistant could not start." };
            dot = ColorHelper::FromArgb(0xFF, 0xE5, 0x53, 0x3D);
            ink = ColorHelper::FromArgb(0xFF, 0xF2, 0x8B, 0x82);
        }
        else if (!snapshot.answer.empty())
        {
            status = snapshot.answer;
            dot = ColorHelper::FromArgb(0xFF, 0x5C, 0xB8, 0x5C);
        }
        else if (snapshot.state == State::Starting)
        {
            status = L"Starting the search assistant\x2026";
            dot = ColorHelper::FromArgb(0xFF, 0x4E, 0xA1, 0xFF);
            inkOpacity = 0.6;
        }
        else
        {
            // SESSIONS.md §9.14: the assistant also lists the windows' tabs, resumes or forks a session next to a tab
            // you choose, and deactivates a tab you name — say so where the user starts.
            status = L"Describe the session you are looking for, or ask what is open, to resume or fork a session next to a tab, or to deactivate a tab, and press Enter. A follow-up refines the list; the crossed circle starts over.";
            if (snapshot.state == State::Ready)
            {
                dot = ColorHelper::FromArgb(0xFF, 0x5C, 0xB8, 0x5C);
            }
            inkOpacity = 0.6;
        }
        _sessAiStatusText.Text(winrt::hstring{ status });
        _sessAiStatusText.Foreground(SolidColorBrush{ ink });
        _sessAiStatusText.Opacity(inkOpacity);
        _sessAiDot.Background(SolidColorBrush{ dot });
        // The thinking level the next request runs with — the agent's, so a click in another
        // window shows here too. Off stays dim like the context size; a level that thinks reads
        // brighter, since it changes how long a request takes.
        if (_sessAiThinkingText)
        {
            const bool thinks = ::Agentmaster::HarnessThinkingLevelThinks(snapshot.thinkingLevel);
            _sessAiThinkingText.Text(winrt::hstring{ std::wstring{ L"thinking " } + ::Agentmaster::HarnessThinkingLevelWord(snapshot.thinkingLevel) });
            _sessAiThinkingText.Opacity(thinks ? 0.95 : 0.6);
        }
        // The context size, once the model answered at least once.
        std::wstring usage;
        if (snapshot.usage.contextTokens > 0)
        {
            usage = L"ctx " + SessFormatTokens(snapshot.usage.contextTokens);
            if (snapshot.usage.contextWindow > 0)
            {
                usage += L" / " + SessFormatTokens(snapshot.usage.contextWindow);
            }
            if (snapshot.compactions > 0)
            {
                usage += L" \x00B7 compacted " + std::to_wstring(snapshot.compactions) + L"\x00D7";
            }
        }
        _sessAiUsageText.Text(winrt::hstring{ usage });
    }

    // The strip's thinking toggle was clicked: move the AI search to the next thinking level.
    //   - The next level comes from the AGENT's level (not this page's label), so two windows that
    //     click in turn walk one rotation; it skips a level whose effort the model does not have
    //     (the model's list, once its process reported one).
    //   - It is persisted first (a freshest-disk read-modify-write of one field, the transcript
    //     overlay's idiom: never a stale in-memory copy over a cog Save or another window's change),
    //     then given to the agent, whose StateChanged event repaints every window's strip.
    //   - It applies from the next request; one that runs now keeps its level.
    // Never throws: a failure is logged, and the agent keeps the level it had.
    void TerminalPage::_CycleSessionsAiThinking()
    {
        try
        {
            _SessionsAiAttach(); // AI mode is on (the toggle shows only then), so the agent exists; this only registers the listener when needed
            auto& agent = ::Agentmaster::SharedSessionSearchAgent();
            const auto snapshot = agent.Snapshot();
            const auto next = ::Agentmaster::NextHarnessThinkingLevel(snapshot.thinkingLevel, snapshot.supportedEfforts);
            const std::wstring word = ::Agentmaster::HarnessThinkingLevelWord(next);
            auto settings = ::Agentmaster::LoadAppSettings();
            if (settings.sessionsAiThinkingLevel != word)
            {
                settings.sessionsAiThinkingLevel = word;
                ::Agentmaster::SaveAppSettings(settings);
            }
            agent.SetThinkingLevel(next);
            ::Agentmaster::LogNav(L"sessions ai thinking -> " + word);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_CycleSessionsAiThinking");
        }
    }

    // A table row from an index entry alone — no directory listing, no registry. The same
    // field rules as the gather's rows (_RefreshSessionsRows), through the helpers the search
    // tool itself uses for the same columns, so a row reads the same whichever way it came.
    //   entry   — a session's index entry; its liveTitle (a stored or live tab title) wins as
    //             the title, like the gather's overlays.
    //   returns — the row; aiExtra is false (the caller marks an extra row).
    //   throws  — std::bad_alloc.
    TerminalPage::_SessionsRow TerminalPage::_SessionsRowFromIndexEntry(const ::Agentmaster::SessionIndexEntry& entry)
    {
        _SessionsRow r;
        r.id = entry.sessionId;
        r.path = entry.path;
        r.title = ::Agentmaster::SessionSearchTitleOf(entry);
        r.dir = ::Agentmaster::SessionSearchDirOf(entry);
        r.branch = entry.stats.gitBranch;
        r.fork = !entry.stats.forkedFromId.empty();
        r.forkedFromId = entry.stats.forkedFromId;
        r.createdMs = ::Agentmaster::SessionSearchCreatedMs(entry);
        r.lastActivityMs = ::Agentmaster::SessionSearchLastActivityMs(entry);
        r.sizeBytes = entry.sizeBytes;
        r.msgs = entry.stats.userPrompts;
        r.tools = entry.stats.toolUses;
        r.contextTokens = entry.stats.contextTokens;
        return r;
    }

    // ===== the AI search's window and tab tools, this window's half (SESSIONS.md §9.14) =====

    ::Agentmaster::WindowToolOutcome TerminalPage::_RunAgentWindowTool(const ::Agentmaster::WindowToolRequest& request) noexcept
    {
        using Kind = ::Agentmaster::WindowToolRequest::Kind;
        ::Agentmaster::WindowToolOutcome outcome;
        try
        {
            // Where a session's tab is in THIS window's strip (nullopt: not hosted here). _claudeTabs is the window's
            // own binding map, so a session another window hosts is never mistaken for one of ours.
            const auto indexOfSession = [this](const std::wstring& id) -> std::optional<uint32_t> {
                if (id.empty())
                {
                    return std::nullopt;
                }
                const auto it = _claudeTabs.find(id);
                const auto tab = it != _claudeTabs.end() ? it->second.get() : nullptr;
                uint32_t idx = 0;
                if (tab && _tabs.IndexOf(tab, idx))
                {
                    return idx;
                }
                return std::nullopt;
            };

            switch (request.kind)
            {
            case Kind::Census:
            {
                // One pass over _claudeTabs into a tab -> session map, then one over the strip: a window of 400+ tabs
                // must not pay _ClaudeSessionForTab's own pass per tab (quadratic on the UI thread).
                std::unordered_map<void*, std::wstring> sessionByTab;
                sessionByTab.reserve(_claudeTabs.size());
                for (const auto& [id, weakTab] : _claudeTabs)
                {
                    if (const auto t = weakTab.get())
                    {
                        sessionByTab.emplace(winrt::get_abi(t), id);
                    }
                }
                ::Agentmaster::WindowCensus& census = outcome.census;
                census.windowId = _windowId;
                census.title = std::wstring{ Title() };
                census.managerPageVisible = _managerPageVisible;
                const auto focused = _GetFocusedTabIndex(); // nullopt while the Agent Manager page is up
                const uint32_t count = _tabs.Size();
                census.tabs.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const auto tab = _tabs.GetAt(i);
                    ::Agentmaster::WindowTabInfo info;
                    info.index = i;
                    info.title = std::wstring{ tab.Title() };
                    if (const auto it = sessionByTab.find(winrt::get_abi(tab)); it != sessionByTab.end())
                    {
                        info.sessionId = it->second;
                    }
                    info.selected = focused && *focused == i;
                    census.tabs.push_back(std::move(info));
                }
                // Only the strip half: the caller reads each session's facts from the registry off this thread.
                outcome.handled = true;
                outcome.ok = true;
                break;
            }
            case Kind::Resume:
            case Kind::Fork:
            {
                outcome.handled = true; // the engine routed it to this window: its answer is ours, whatever it says
                const bool fork = request.kind == Kind::Fork;
                // The slot (InsertAt index) for the new tab: right after the asked tab; a fork's default right after its
                // source when the source is here (the tab menu's "Fork session" placement); else the END of the strip —
                // an explicit slot rather than Terminal's newTabPosition (which may mean "after the current tab"), so
                // what the model is told about the tab's place stays true. Clamped to the strip: InsertAt past the end
                // throws.
                const uint32_t size = _tabs.Size();
                uint32_t slot = size;
                if (!request.afterSessionId.empty())
                {
                    const auto at = indexOfSession(request.afterSessionId);
                    if (!at)
                    {
                        outcome.message = L"the tab to put it after is not in this window any more; list the tabs again and choose another.";
                        break;
                    }
                    slot = *at + 1;
                }
                else if (request.afterIndex >= 0)
                {
                    slot = (std::min)(static_cast<uint32_t>(request.afterIndex) + 1, size);
                }
                else if (fork)
                {
                    if (const auto at = indexOfSession(request.sessionId))
                    {
                        slot = *at + 1;
                    }
                }
                ::Agentmaster::LogNav(std::wstring{ L"ai-tool " } + (fork ? L"fork " : L"resume ") + ::Agentmaster::ShortId(request.sessionId) + L" slot=" + std::to_wstring(slot) + L"/" + std::to_wstring(size) +
                                      (request.focus ? L" [focus]" : L" [bg]") + (request.model.empty() ? std::wstring{} : L" model=" + request.model));
                // A background tab unless the request asks for focus: the bulk-open latch keeps the Sessions page up and
                // the selection where it is (a phone's request must not change the PC's screen; a page request must
                // not hide the page that is talking to the user). Restored on every exit.
                const bool previous = _openClaudeTabInBackground;
                _openClaudeTabInBackground = !request.focus;
                auto restore = wil::scope_exit([this, previous] { _openClaudeTabInBackground = previous; });
                std::wstring opened;
                if (!fork)
                {
                    opened = _ResumeSessionFromDisk(request.sessionId, request.dir, request.title, slot);
                }
                else
                {
                    // The tab menu's fork for a session the registry knows (Codex-aware, the unsent draft carried); the
                    // Sessions page's fork for one it never knew.
                    std::wstring forked;
                    if (!_ForkManagedSessionById(request.sessionId, slot, request.model, &forked))
                    {
                        forked = _ForkSessionFromDisk(request.sessionId, request.dir, request.title, request.model, slot);
                    }
                    opened = std::move(forked);
                }
                if (opened.empty())
                {
                    // The seams log why ([nav] ...-done); a missing claude.exe also put its install prompt on this window.
                    outcome.message = L"no tab opened: Claude Code may be missing (this window then shows its install prompt), or the launch failed (hooks.log has the details).";
                    break;
                }
                outcome.ok = true;
                outcome.sessionId = opened;
                if (const auto at = indexOfSession(opened))
                {
                    outcome.index = static_cast<int32_t>(*at);
                }
                if (request.focus)
                {
                    _FocusClaudeSessionTab(opened, /*bringWindowToFront*/ true); // the open selected it; this brings the window up
                }
                break;
            }
            case Kind::Deactivate:
            {
                const auto it = _claudeTabs.find(request.sessionId);
                const auto tab = it != _claudeTabs.end() ? it->second.get() : nullptr;
                if (!tab)
                {
                    break; // not hosted here: handled stays false, and the engine waits for the host
                }
                outcome.handled = true;
                const auto before = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(request.sessionId) : std::nullopt;
                if (!before)
                {
                    outcome.message = L"the app does not know that session.";
                    break;
                }
                // The park's own rules (_DeactivateTabsFromMenu skips these silently), said to the model instead.
                if (before->external)
                {
                    outcome.message = L"the tab runs a claude the user started by hand in a shell tab; Deactivate never parks it.";
                    break;
                }
                if (!before->started)
                {
                    outcome.ok = true;
                    outcome.message = L"It was already dormant.";
                    break;
                }
                if (_sessionRegistry->DeliveryGateHeld(request.sessionId))
                {
                    outcome.message = L"a prompt is being typed into that tab right now; try again in a few seconds.";
                    break;
                }
                ::Agentmaster::LogNav(L"ai-tool deactivate " + ::Agentmaster::ShortId(request.sessionId) + L" (the AI search, on the user's request)");
                // The tab menu's "Deactivate ▸ Deactivate Tab": the focused-tab rule (this window leaves the tab for its
                // Agent Manager page first, so the fresh control is not laid out and restarted) and the strip keeper.
                _DeactivateTabsFromMenu(tab, implementation::Tab::kDeactivateScopeThisTab);
                const auto after = _sessionRegistry->GetWithoutQueue(request.sessionId);
                outcome.ok = after && !after->started;
                if (!outcome.ok)
                {
                    outcome.message = L"the tab was not deactivated (hooks.log's [nav] deactivate-done line says why).";
                }
                break;
            }
            default:
                break;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the model is told it failed. `handled` keeps its value, so an open that threw after its
            // launch still counts as this window's (and the engine stops waiting).
            ::Agentmaster::AgentLogCaughtException(L"_RunAgentWindowTool");
            outcome.ok = false;
            if (outcome.message.empty())
            {
                outcome.message = L"the window failed while doing it (hooks.log has the details).";
            }
        }
        return outcome;
    }
}
