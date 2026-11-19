// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TerminalPage, the SESSION NOTES partial (doc/agentmaster/SESSION_NOTES.md).
//
// The user's request of 2026-11-16: "I want Set Note and showing note on hover for tab and for sidemenu card. please
// suggest", then their picks on mockups the same day: the note right under the title of the tab's rich hover card; a small
// glyph after a noted tab's title; a glyph in the side-panel card's band whose hover opens the FULL session card ("can
// hovering the glyph show the tooltip and not that little classic tooltip you suggested?"); "Set Note…" right under Rename,
// "And add F3 hotkey and mention F3 like we do for Rename tab".
//
// What lives here:
//   - THE EDITOR CARD (§3c): a Root()-parented Popup built once — a header, a multi-line box, a hint + count line, and
//     Remove note / Cancel / Save. Enter is a new line, Ctrl+Enter saves, Esc cancels (both on the key-UP, so the key's ESC or
//     CR never reaches claude after the card hands the keyboard back); a press outside SAVES, like a tab rename's click-away.
//   - F3 (§3b): the agentOpenSessionNote action's handler — the focused tab's note, else the key passes through.
//   - THE SAVE (_SaveSessionNote): the store write at once, the repaint of this window's surfaces one tick later (the close
//     can run inside a pointer or key handler, where a tree mutation is not allowed).
//   - THE TAB MARKERS (§4b): the glyph after a noted tab's title (TabStatus.AgentNoteVisible), set at bind and after an edit
//     here, and re-applied from the notes epoch when another window edits a note.
// Elsewhere: the note block of the rich card (TtBuildNoteBlock, _PushSessionCard) is TerminalPage.AgentObserver.cpp's; the
// side panel's glyph and its hover card are TerminalPage.AgentSidePanel.cpp's; the menu items are Tab.cpp's and the session
// menu's (AgentManagerContent.Tree.cpp).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentAutomation.h" // AgentSetAutomationId — am.note.box / .save / .cancel / .remove (E2E_TESTING.md §6.3)
#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentPanelFocus.h" // AgentCaptureFocusReturn / AgentPanelHoldsKeyboard — the keyboard comes back where it was (TCM-K2)
#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the card's tooltips through the one islands-safe host
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId
#include "AgentMaster/SessionRegistry.h" // GetWithoutQueue — the session's title for the editor's header
#include "AgentMaster/SessionStore.h" // GetSessionNote / SetSessionNote / NormalizeSessionNote / LoadAllSessionNotes / SessionNotesEpoch

#include <chrono> // the card's tooltip delay; the save stamp

using namespace winrt::Windows::System; // VirtualKey
using namespace winrt::Windows::UI::Core; // CoreDispatcherPriority
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media; // SolidColorBrush, VisualTreeHelper

namespace winrt
{
    // The XAML namespace by its short name, as the other TerminalPage partials spell it (WUX::Input::KeyRoutedEventArgs …);
    // no header gives every unit this alias, so each unit that uses it declares it (a redeclaration to the same namespace is
    // allowed).
    namespace WUX = Windows::UI::Xaml;
}

namespace
{
    // The editor card's width: a paragraph of note at a readable line length, and narrow enough to sit under a tab.
    constexpr double kNoteEditorWidth = 360.0;
    // The box's height: about six lines of text before it scrolls (the TextBox scrolls its own content — this is a Popup,
    // not a ToolTip, so a TextBox's ScrollViewer is allowed here).
    constexpr double kNoteEditorBoxHeight = 118.0;
    // An estimate of the whole card's height, used only to keep a card opened near the window's bottom inside it.
    constexpr double kNoteEditorEstimatedHeight = 250.0;
    // The air kept between the card and the window's edges when its anchor would push it past one.
    constexpr double kNoteEditorEdgePx = 8.0;
    // The controls' tooltips wait like the Tags panel's (kTagPanelTipDelay): the card is a row of controls clicked one after
    // the other, and a tooltip on every pause would cover the next one.
    constexpr std::chrono::milliseconds kNoteEditorTipDelay{ 3000 };
    // The count turns amber in the cap's last tenth (4,500 of 5,000), so the limit is seen before typing stops at it. Derived
    // from the cap, so a new cap moves it along (it was a fixed 1,800 while notes held 2,000).
    constexpr size_t kNoteEditorCountWarnAt = ::Agentmaster::kMaxSessionNoteLength - ::Agentmaster::kMaxSessionNoteLength / 10;

    // Now, as unix ms (the system clock): the save stamp the hover card's "edited … ago" counts from.
    //   returns - the current time in ms since 1970.
    //   throws  - nothing.
    int64_t NoteNowMs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // A stored note as the editor's box wants it: XAML's TextBox writes and expects a lone CR for each line break (its rich
    // edit base), while the store keeps LF (NormalizeSessionNote) — so a reopened note shows its lines, not one run of text.
    //   note    - the stored note (LF line breaks).
    //   returns - the same text with CR line breaks.
    //   throws  - std::bad_alloc.
    std::wstring BoxTextFromNote(const std::wstring& note)
    {
        std::wstring out = note;
        for (auto& c : out)
        {
            if (c == L'\n')
            {
                c = L'\r';
            }
        }
        return out;
    }

    // The first line of a session title (titles can hold line breaks) — the editor's header has room for one.
    //   title   - the title.
    //   returns - the text before the first CR or LF.
    //   throws  - std::bad_alloc.
    std::wstring FirstTitleLine(const std::wstring& title)
    {
        return title.substr(0, title.find_first_of(L"\r\n"));
    }
}

namespace winrt::TerminalApp::implementation
{
    // ===== opening the editor ==================================================================================================

    bool TerminalPage::_OpenNoteEditorForTab(const TerminalApp::Tab& tab)
    {
        const auto sid = _ClaudeSessionForTab(tab);
        if (sid.empty())
        {
            return false; // a shell tab: no session to hold a note (F3 then passes the key through)
        }
        // Anchor: the tab item's bottom-left, root-relative (the Tags panel's recipe — a top/left-aligned parented Popup's
        // offsets are root-relative). A failed transform keeps the default spot.
        double x = kNoteEditorEdgePx;
        double y = 40;
        try
        {
            if (const auto tvi = tab.TabViewItem())
            {
                const auto pt = tvi.TransformToVisual(Root()).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                x = pt.X;
                y = pt.Y + tvi.ActualHeight() + 2;
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_OpenNoteEditorForTab: place the card under the tab");
        }
        _OpenNoteEditorAt(sid, x, y);
        return true;
    }

    void TerminalPage::_OpenNoteEditorForElement(const std::wstring& sessionId, const winrt::Windows::UI::Xaml::FrameworkElement& anchor)
    {
        double x = kNoteEditorEdgePx;
        double y = 40;
        try
        {
            if (anchor)
            {
                // The element may have been recycled between the menu's open and the click: a transform of a dead element
                // throws, and the default spot holds.
                const auto pt = anchor.TransformToVisual(Root()).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                x = pt.X;
                y = pt.Y + anchor.ActualHeight() + 2;
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_OpenNoteEditorForElement: place the card under the element");
        }
        _OpenNoteEditorAt(sessionId, x, y);
    }

    void TerminalPage::_OpenNoteEditorAt(const std::wstring& sessionId, double x, double y)
    {
        if (sessionId.empty())
        {
            return;
        }
        const winrt::hstring sidH{ sessionId };
        // DEFERRED one tick: every caller sits inside a closing context menu's click (or F3's key-down), and the menu's popup
        // returns the focus to its target as it closes — that refocus must land BEFORE the card focuses its box.
        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), sidH, x, y]() {
            const auto page = weak.get();
            if (!page)
            {
                return;
            }
            // Contained (Rule #18): a failed open costs this one open, never the window — and an exception escaping a
            // dispatcher callback could fail the whole app.
            try
            {
                const std::wstring id{ sidH };
                // An editor already open for ANOTHER session saves it first (the click-away rule): what was typed is never
                // dropped by opening a second note.
                if (page->_noteEditorPopup && page->_noteEditorPopup.IsOpen() && page->_noteEditorSessionId != id)
                {
                    page->_CloseNoteEditorPopup(/*save*/ true, L"another session's note");
                }
                page->_EnsureNoteEditorPopup();
                if (!page->_noteEditorPopup)
                {
                    return;
                }
                const bool wasOpen = page->_noteEditorPopup.IsOpen();
                if (!wasOpen)
                {
                    // A fresh open fills the card from the store (one record-cache read) — a reopen of the same session's
                    // card (F3 again while it is up) keeps what is being typed.
                    const auto note = ::Agentmaster::GetSessionNote(id);
                    page->_noteEditorSessionId = id;
                    page->_noteEditorOpenedText = note.text;
                    std::wstring title;
                    if (page->_sessionRegistry)
                    {
                        if (const auto s = page->_sessionRegistry->GetWithoutQueue(id))
                        {
                            title = FirstTitleLine(s->title);
                        }
                    }
                    page->_noteEditorHeader.Text(winrt::hstring{ L"Note  \x00B7  " + (title.empty() ? ::Agentmaster::ShortId(id) : title) });
                    page->_noteEditorBox.Text(winrt::hstring{ BoxTextFromNote(note.text) });
                    page->_noteEditorRemoveBtn.IsEnabled(!note.text.empty()); // nothing to remove until a note is stored
                    page->_UpdateNoteEditorCount();
                }
                // Keep the card inside the window: an anchor near the right or bottom edge moves it in, never off-screen.
                double cx = x;
                double cy = y;
                const double rootW = page->Root().ActualWidth();
                const double rootH = page->Root().ActualHeight();
                if (rootW > kNoteEditorWidth + 2 * kNoteEditorEdgePx && cx > rootW - kNoteEditorWidth - kNoteEditorEdgePx)
                {
                    cx = rootW - kNoteEditorWidth - kNoteEditorEdgePx;
                }
                if (rootH > kNoteEditorEstimatedHeight + 2 * kNoteEditorEdgePx && cy > rootH - kNoteEditorEstimatedHeight - kNoteEditorEdgePx)
                {
                    cy = rootH - kNoteEditorEstimatedHeight - kNoteEditorEdgePx;
                }
                page->_noteEditorPopup.HorizontalOffset(std::max(0.0, cx));
                page->_noteEditorPopup.VerticalOffset(std::max(0.0, cy));
                // A key that went down before this open must not close (or save) the new card on its release.
                page->_noteEditorEscArmed = false;
                page->_noteEditorSaveArmed = false;
                page->_noteEditorPopup.IsOpen(true);
                ::Agentmaster::LogNav(L"note editor open " + ::Agentmaster::ShortId(id) + (wasOpen ? L" (already open)" : L""));
                // Focus the box one tick AFTER the open: on the first open the popup's child has only just been realized,
                // and a same-tick Focus on a TextBox that was never laid out can do nothing.
                page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak, wasOpen]() {
                    const auto p = weak.get();
                    if (!p)
                    {
                        return;
                    }
                    try
                    {
                        if (p->_noteEditorPopup && p->_noteEditorPopup.IsOpen() && p->_noteEditorBox)
                        {
                            // TCM-K2: where the keyboard is right before the box takes it — read now, after the menu's close
                            // returned the focus (a tab header answers "the current tab": AgentCaptureFocusReturn's rule).
                            if (!wasOpen)
                            {
                                p->_noteEditorReturnFocus = AgentCaptureFocusReturn(p->Root().XamlRoot(), p->_noteEditorCard, p->_tabView);
                            }
                            p->_noteEditorBox.Focus(FocusState::Programmatic);
                            // The caret at the END: the common edit is adding a line to the note, not retyping its start.
                            p->_noteEditorBox.SelectionStart(static_cast<int32_t>(p->_noteEditorBox.Text().size()));
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"_OpenNoteEditorAt: focus the box");
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_OpenNoteEditorAt");
            }
        });
    }

    // ===== building the editor (once) ==========================================================================================

    void TerminalPage::_EnsureNoteEditorPopup()
    {
        if (_noteEditorPopup)
        {
            return;
        }

        StackPanel body;
        body.Spacing(7);

        // The header: the note glyph in amber (the note block's edge color) and "Note · <title>", trimmed to one line.
        Grid header;
        {
            ColumnDefinition iconCol;
            iconCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition textCol;
            textCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            header.ColumnDefinitions().Append(iconCol);
            header.ColumnDefinitions().Append(textCol);
        }
        FontIcon glyph;
        glyph.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        glyph.Glyph(L"\xE70B"); // QuickNote — the glyph a noted tab and a noted side-panel card wear
        glyph.FontSize(14);
        glyph.Foreground(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xE0, 0xB0, 0x4C) });
        glyph.Margin(ThicknessHelper::FromLengths(0, 0, 7, 0));
        glyph.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(glyph, 0);
        header.Children().Append(glyph);
        _noteEditorHeader = TextBlock{};
        _noteEditorHeader.FontSize(12.5);
        _noteEditorHeader.TextTrimming(TextTrimming::CharacterEllipsis);
        _noteEditorHeader.TextWrapping(TextWrapping::NoWrap);
        _noteEditorHeader.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(_noteEditorHeader, 1);
        header.Children().Append(_noteEditorHeader);
        body.Children().Append(header);

        // The box: several lines, wrapping, capped at the store's own limit so typing stops where the store would cut.
        _noteEditorBox = TextBox{};
        _noteEditorBox.AcceptsReturn(true);
        _noteEditorBox.TextWrapping(TextWrapping::Wrap);
        _noteEditorBox.Height(kNoteEditorBoxHeight);
        _noteEditorBox.FontSize(12.5);
        _noteEditorBox.MaxLength(static_cast<int32_t>(::Agentmaster::kMaxSessionNoteLength));
        _noteEditorBox.PlaceholderText(L"Why this tab exists, what it waits for, what comes next\x2026");
        ScrollViewer::SetVerticalScrollBarVisibility(_noteEditorBox, ScrollBarVisibility::Auto);
        AgentSetAutomationId(_noteEditorBox, L"am.note.box");
        _noteEditorBox.TextChanged([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_UpdateNoteEditorCount(); // noexcept
            }
        });
        // Ctrl+Enter must not insert a line break: the box's own key handling runs before an instance KeyDown handler, so the
        // key is claimed in the TUNNELING PreviewKeyDown, before the box sees it. The save itself waits for the key-UP (the
        // card's hooks below), like Escape: the key's CR must go nowhere after the card hands the keyboard back.
        _noteEditorBox.PreviewKeyDown([weak = get_weak()](auto&&, const WUX::Input::KeyRoutedEventArgs& e) {
            const auto page = weak.get();
            if (page && e.Key() == VirtualKey::Enter && (::GetKeyState(VK_CONTROL) & 0x8000))
            {
                e.Handled(true);
                page->_noteEditorSaveArmed = true;
                page->_noteEditorEscArmed = false;
            }
        });
        // Escape: swallow the key-down only (the box would otherwise act on it); the card's key-up hook cancels.
        _noteEditorBox.KeyDown([](auto&&, const WUX::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == VirtualKey::Escape)
            {
                e.Handled(true);
            }
        });
        body.Children().Append(_noteEditorBox);

        // The hint + count line: the three keys, and how much of the cap is used.
        Grid hintRow;
        {
            ColumnDefinition hintCol;
            hintCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition countCol;
            countCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            hintRow.ColumnDefinitions().Append(hintCol);
            hintRow.ColumnDefinitions().Append(countCol);
        }
        TextBlock hint;
        hint.FontSize(11);
        hint.Opacity(0.6);
        hint.TextTrimming(TextTrimming::CharacterEllipsis);
        hint.Text(L"Enter: new line \x00B7 Ctrl+Enter: save \x00B7 Esc: cancel");
        Grid::SetColumn(hint, 0);
        hintRow.Children().Append(hint);
        _noteEditorCount = TextBlock{};
        _noteEditorCount.FontSize(11);
        _noteEditorCount.Opacity(0.6);
        _noteEditorCount.Margin(ThicknessHelper::FromLengths(8, 0, 0, 0));
        Grid::SetColumn(_noteEditorCount, 1);
        hintRow.Children().Append(_noteEditorCount);
        body.Children().Append(hintRow);

        // The buttons: Remove note at the left (apart from the two that close with or without the edit), Cancel and Save at
        // the right. Each DEFERS one tick: a save repaints the tab marker and the side panel (tree mutations), which must not
        // run inside the button's pointer handling.
        Grid buttons;
        {
            ColumnDefinition removeCol;
            removeCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition gapCol;
            gapCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition cancelCol;
            cancelCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition saveCol;
            saveCol.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            buttons.ColumnDefinitions().Append(removeCol);
            buttons.ColumnDefinitions().Append(gapCol);
            buttons.ColumnDefinitions().Append(cancelCol);
            buttons.ColumnDefinitions().Append(saveCol);
        }
        _noteEditorRemoveBtn = Button{};
        _noteEditorRemoveBtn.Content(winrt::box_value(winrt::hstring{ L"Remove note" }));
        AgentSetTip(_noteEditorRemoveBtn, L"Delete this session's note now \x2014 its tab and side panel card lose the note glyph.", kNoteEditorTipDelay);
        AgentSetAutomationId(_noteEditorRemoveBtn, L"am.note.remove");
        _noteEditorRemoveBtn.Click([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak]() {
                    const auto p = weak.get();
                    if (!p)
                    {
                        return;
                    }
                    const std::wstring id = p->_noteEditorSessionId;
                    // The removal is the stored note's, not the box's: write the empty note, then close WITHOUT saving the
                    // box (whatever it holds goes with the removal). noexcept both.
                    if (!id.empty())
                    {
                        p->_SaveSessionNote(id, std::wstring{});
                        p->_noteEditorOpenedText.clear();
                    }
                    p->_CloseNoteEditorPopup(/*save*/ false, L"remove");
                });
            }
        });
        Grid::SetColumn(_noteEditorRemoveBtn, 0);
        buttons.Children().Append(_noteEditorRemoveBtn);
        Button cancel;
        cancel.Content(winrt::box_value(winrt::hstring{ L"Cancel" }));
        cancel.Margin(ThicknessHelper::FromLengths(0, 0, 8, 0));
        AgentSetTip(cancel, L"Close without saving (Esc)", kNoteEditorTipDelay);
        AgentSetAutomationId(cancel, L"am.note.cancel");
        cancel.Click([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak]() {
                    if (const auto p = weak.get())
                    {
                        p->_CloseNoteEditorPopup(/*save*/ false, L"cancel");
                    }
                });
            }
        });
        Grid::SetColumn(cancel, 2);
        buttons.Children().Append(cancel);
        Button save;
        save.Content(winrt::box_value(winrt::hstring{ L"Save" }));
        AgentSetTip(save, L"Save the note (Ctrl+Enter) \x2014 a click outside the card saves too; an empty note removes it.", kNoteEditorTipDelay);
        AgentSetAutomationId(save, L"am.note.save");
        save.Click([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak]() {
                    if (const auto p = weak.get())
                    {
                        p->_CloseNoteEditorPopup(/*save*/ true, L"save");
                    }
                });
            }
        });
        Grid::SetColumn(save, 3);
        buttons.Children().Append(save);
        body.Children().Append(buttons);

        _noteEditorCard = Border{};
        _noteEditorCard.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark
        _noteEditorCard.Width(kNoteEditorWidth);
        _noteEditorCard.Background(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x26, 0x26, 0x26) });
        _noteEditorCard.BorderBrush(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3A, 0x3A, 0x3A) });
        _noteEditorCard.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _noteEditorCard.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
        _noteEditorCard.Padding(ThicknessHelper::FromLengths(11, 10, 11, 11));
        _noteEditorCard.Child(body);
        // The keys, anywhere in the card (the box, a focused button): Escape cancels, Ctrl+Enter saves — both on the KEY-UP.
        // Closing on the key-down handed the rest of the keystroke (its ESC / CR) to whatever got the keyboard next, and once
        // the card gives it back to the terminal that is claude: an interrupt, a submit (the Tags panel's TCM-K2 lesson). The
        // pair acts only on a key whose key-DOWN it saw inside the open card, so a key-up whose key-down happened before the
        // card opened never closes it. handledEventsToo: the box marks its own Escape (and the Ctrl+Enter) handled.
        _noteEditorCard.AddHandler(
            UIElement::KeyDownEvent(),
            winrt::box_value(WUX::Input::KeyEventHandler{ [weak = get_weak()](const IInspectable&, const WUX::Input::KeyRoutedEventArgs& e) {
                const auto page = weak.get();
                if (!page)
                {
                    return;
                }
                const auto key = e.Key();
                // Any other key-down disarms both: the next key-up must belong to the key that went down last.
                page->_noteEditorEscArmed = key == VirtualKey::Escape;
                page->_noteEditorSaveArmed = key == VirtualKey::Enter && (::GetKeyState(VK_CONTROL) & 0x8000);
                if (page->_noteEditorEscArmed || page->_noteEditorSaveArmed)
                {
                    e.Handled(true);
                }
            } }),
            true /* handledEventsToo */);
        _noteEditorCard.AddHandler(
            UIElement::KeyUpEvent(),
            winrt::box_value(WUX::Input::KeyEventHandler{ [weak = get_weak()](const IInspectable&, const WUX::Input::KeyRoutedEventArgs& e) {
                const auto page = weak.get();
                if (!page)
                {
                    return;
                }
                const auto key = e.Key();
                const bool cancel = key == VirtualKey::Escape && page->_noteEditorEscArmed;
                const bool commit = key == VirtualKey::Enter && page->_noteEditorSaveArmed;
                if (!cancel && !commit)
                {
                    return; // not one of the two, or its key-down was not seen inside the open card
                }
                page->_noteEditorEscArmed = false;
                page->_noteEditorSaveArmed = false;
                e.Handled(true); // set before acting: the close removes the focused element at once
                page->_CloseNoteEditorPopup(/*save*/ commit, commit ? L"ctrl+enter" : L"esc"); // noexcept; the save's repaint is deferred inside
            } }),
            true /* handledEventsToo */);

        _noteEditorPopup = winrt::Windows::UI::Xaml::Controls::Primitives::Popup{};
        _noteEditorPopup.Child(_noteEditorCard);
        // Top/left aligned so HorizontalOffset/VerticalOffset are root-relative (the Tags panel's recipe).
        _noteEditorPopup.HorizontalAlignment(HorizontalAlignment::Left);
        _noteEditorPopup.VerticalAlignment(VerticalAlignment::Top);
        Grid::SetRow(_noteEditorPopup, 0);
        Grid::SetRowSpan(_noteEditorPopup, 3); // span the whole Root grid (tab row + infobars + content)
        Root().Children().Append(_noteEditorPopup);

        // The click-away SAVES (a tab rename's rule: "clicking away always commits"): ONE handledEventsToo hook on Root(),
        // registered once and inert while the card is closed. A press whose source chain reaches the card is inside (keep
        // open); anything else — the terminal, the tab strip, a side-panel card — saves and closes. The close itself is not a
        // tree mutation; the save's repaint is deferred to the next tick (_SaveSessionNote), so this is safe inside the press.
        if (!_noteEditorOutsideHooked)
        {
            _noteEditorOutsideHooked = true;
            Root().AddHandler(
                UIElement::PointerPressedEvent(),
                winrt::box_value(WUX::Input::PointerEventHandler{ [weak = get_weak()](const IInspectable&, const WUX::Input::PointerRoutedEventArgs& e) {
                    const auto page = weak.get();
                    if (!page || !page->_noteEditorPopup || !page->_noteEditorPopup.IsOpen())
                    {
                        return;
                    }
                    try
                    {
                        auto d = e.OriginalSource().try_as<DependencyObject>();
                        while (d)
                        {
                            if (const auto b = d.try_as<Border>(); b && b == page->_noteEditorCard)
                            {
                                return; // pressed inside the card — keep it open
                            }
                            d = VisualTreeHelper::GetParent(d);
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"note editor: the outside-press walk");
                        return; // undecided: keep the card open (the next press decides again)
                    }
                    page->_CloseNoteEditorPopup(/*save*/ true, L"click outside");
                } }),
                true /* handledEventsToo — tab items and the terminal handle their presses */);
        }
    }

    // ===== closing, saving =====================================================================================================

    void TerminalPage::_CloseNoteEditorPopup(bool save, std::wstring_view reason) noexcept
    {
        // A key held across the close must not act on its release in a later card.
        _noteEditorEscArmed = false;
        _noteEditorSaveArmed = false;
        try
        {
            if (_noteEditorPopup && _noteEditorPopup.IsOpen())
            {
                const std::wstring id = _noteEditorSessionId;
                // Read the box BEFORE the close: the close takes the box out of the live tree.
                const std::wstring typed = (save && _noteEditorBox) ? std::wstring{ _noteEditorBox.Text() } : std::wstring{};
                // End the card's tooltips before it leaves the tree: a 3 s wait armed on a control would otherwise end on
                // the NEXT open (the controls are built once), over a control the pointer is no longer on.
                try
                {
                    if (_noteEditorCard)
                    {
                        AgentCloseTipsIn(_noteEditorCard);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"_CloseNoteEditorPopup: end the card's tooltips");
                }
                // TCM-K2: asked BEFORE the close — the close moves the focus at once, and the answer is lost after it.
                const bool heldKeyboard = AgentPanelHoldsKeyboard(Root().XamlRoot(), _noteEditorCard); // noexcept
                _noteEditorPopup.IsOpen(false);
                _noteEditorSessionId.clear();
                bool saved = false;
                if (save && !id.empty())
                {
                    // Write only what CHANGED: a click-away from an untouched note must not re-stamp it ("edited just now").
                    if (::Agentmaster::NormalizeSessionNote(typed) != _noteEditorOpenedText)
                    {
                        saved = _SaveSessionNote(id, typed); // noexcept; the repaint follows on the next tick
                    }
                }
                ::Agentmaster::LogNav(L"note editor close (" + std::wstring{ reason } + (saved ? L", saved) " : L") ") + ::Agentmaster::ShortId(id));
                _noteEditorOpenedText.clear();
                if (heldKeyboard)
                {
                    _ReturnKeyboardAfterPanel(_noteEditorReturnFocus, L"the note editor"); // noexcept
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_CloseNoteEditorPopup");
        }
        _noteEditorReturnFocus = nullptr; // the next open captures its own target
        _noteEditorSessionId.clear();
    }

    bool TerminalPage::_SaveSessionNote(const std::wstring& sessionId, const std::wstring& text) noexcept
    {
        try
        {
            if (sessionId.empty())
            {
                return false;
            }
            const std::wstring normalized = ::Agentmaster::NormalizeSessionNote(text);
            const bool ok = ::Agentmaster::SetSessionNote(sessionId, normalized, NoteNowMs());
            // One line per save in hooks.log, the note's SIZE only — the text is the user's, and a log is no place for it.
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[note] " + sessionId +
                                              (!ok ? std::wstring{ L" NOT saved: the session store refused the write (an unreadable record, or a write error)" } :
                                                     normalized.empty() ? std::wstring{ L" note removed" } :
                                                                          (L" note saved (" + std::to_wstring(normalized.size()) + L" chars)")) +
                                              L"\n");
            if (!ok)
            {
                return false;
            }
            // THIS window's surfaces, one tick later: the close that called this can run inside a pointer or key handler,
            // and the repaint creates or removes elements (the tab's x:Load glyph, the side panel's band glyph, the hover
            // card's content). Other windows follow through the notes epoch (their sweep, their side panel's tick).
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), sid = sessionId]() {
                const auto page = weak.get();
                if (!page)
                {
                    return;
                }
                page->_RefreshTabNote(sid); // noexcept
                page->_SidePanelNoteSessionNoteChanged(); // noexcept
                try
                {
                    page->_UpdateNoteHoverCard(sid, /*swapWhileOpen*/ true, /*kickSummary*/ false); // a no-op unless the glyph's card shows this session
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"_SaveSessionNote: refresh the note hover card");
                }
            });
            return true;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SaveSessionNote");
            return false;
        }
    }

    void TerminalPage::_CarryNoteIntoFork(const std::wstring& sourceId, const std::wstring& forkedId) noexcept
    {
        if (sourceId.empty() || forkedId.empty())
        {
            return; // no fork opened (a gate, a failed launch): nothing to carry into
        }
        try
        {
            // The store decides (CarryNoteIntoFork): the source's note under "a fork of <source8>", written only when the source
            // has a note and the fork none - a fork of an un-noted session gets no note, and no glyph.
            if (!::Agentmaster::CarryNoteIntoFork(sourceId, forkedId, NoteNowMs()))
            {
                return;
            }
            // The line names the two sessions, never the note's words (the user's text stays out of the log, as at every save).
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[note] " + forkedId + L" takes the note of " + ::Agentmaster::ShortId(sourceId) + L" (a fork)\n");
            // THIS window's surfaces one tick later, like a save's (_SaveSessionNote): the fork's tab got its marker at launch,
            // before the note existed; the repaint creates its x:Load glyph and the side panel's band glyph. Other windows and the
            // Telegram card follow through the notes epoch.
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), sid = forkedId]() {
                const auto page = weak.get();
                if (!page)
                {
                    return;
                }
                page->_RefreshTabNote(sid); // noexcept
                page->_SidePanelNoteSessionNoteChanged(); // noexcept
            });
        }
        catch (...)
        {
            // Rule #18. Recovery: the fork keeps whatever note it has (none, most likely); the fork itself is already open.
            ::Agentmaster::AgentLogCaughtException(L"_CarryNoteIntoFork");
        }
    }

    void TerminalPage::_UpdateNoteEditorCount() noexcept
    {
        try
        {
            if (!_noteEditorCount || !_noteEditorBox)
            {
                return;
            }
            const size_t n = _noteEditorBox.Text().size();
            _noteEditorCount.Text(winrt::hstring{ std::to_wstring(n) + L" / " + std::to_wstring(::Agentmaster::kMaxSessionNoteLength) });
            // Amber near the cap (the note block's edge color), so the limit is seen before typing stops at it.
            if (n >= kNoteEditorCountWarnAt)
            {
                _noteEditorCount.Foreground(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xE0, 0xB0, 0x4C) });
                _noteEditorCount.Opacity(1.0);
            }
            else
            {
                _noteEditorCount.ClearValue(TextBlock::ForegroundProperty());
                _noteEditorCount.Opacity(0.6);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_UpdateNoteEditorCount");
        }
    }

    // ===== F3 ==================================================================================================================

    // The agentOpenSessionNote action (F3 by default, beside F2's rename — the user's "And add F3 hotkey and mention F3 like we
    // do for Rename tab"): open the note editor for the FOCUSED tab's managed session, under the tab. Anywhere else — a shell
    // tab, the Agent Manager page (whose focused tree row answers F3 itself, before the key reaches this binding) — the action
    // is NOT handled, so the key goes on to the terminal: a shell keeps its own F3 (cmd.exe repeats the last command).
    void TerminalPage::_HandleAgentOpenSessionNote(const IInspectable& /*sender*/, const winrt::Microsoft::Terminal::Settings::Model::ActionEventArgs& args)
    {
        bool handled = false;
        try
        {
            if (!_managerPageVisible)
            {
                if (const auto tab = _GetFocusedTab())
                {
                    handled = _OpenNoteEditorForTab(tab);
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_HandleAgentOpenSessionNote");
        }
        args.Handled(handled);
    }

    // ===== the tab markers =====================================================================================================

    void TerminalPage::_SetTabAgentNote(const TerminalApp::Tab& tab, bool hasNote) noexcept
    {
        if (!tab)
        {
            return;
        }
        try
        {
            if (const auto status = tab.TabStatus())
            {
                status.AgentNoteVisible(hasNote); // the observable no-ops when unchanged; the header's glyph is x:Load-bound to it
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SetTabAgentNote");
        }
    }

    void TerminalPage::_RefreshTabNote(const std::wstring& sessionId) noexcept
    {
        try
        {
            if (sessionId.empty())
            {
                return;
            }
            const auto it = _claudeTabs.find(sessionId);
            if (it == _claudeTabs.end())
            {
                return; // hosted by another window (its own refresh or its sweep sets its marker), or not open
            }
            if (const auto tab = it->second.get())
            {
                _SetTabAgentNote(tab, !::Agentmaster::GetSessionNote(sessionId).text.empty());
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_RefreshTabNote");
        }
    }

    void TerminalPage::_RefreshAllTabNotesIfChanged() noexcept
    {
        try
        {
            // The epoch is loaded BEFORE the read (the SessionStore.h reader contract): a note written during the read leaves
            // the older epoch recorded, so the next tick reads again — refreshed once too often, never kept stale.
            const uint64_t epoch = ::Agentmaster::SessionNotesEpoch();
            if (epoch == _tabNotesEpoch)
            {
                return;
            }
            if (_claudeTabs.empty())
            {
                _tabNotesEpoch = epoch; // nothing to mark; the next bind reads its own note
                return;
            }
            if (!::Agentmaster::SessionStoreScanIsWarm())
            {
                // COLD store: a whole-store read would open every record file on this UI thread. Every tab read its own
                // note at bind already, so the markers wait for the start-up warm-up (the epoch stays unapplied).
                return;
            }
            const auto notes = ::Agentmaster::LoadAllSessionNotes(); // one directory listing through the scan cache
            for (const auto& [sessionId, weakTab] : _claudeTabs)
            {
                if (const auto tab = weakTab.get())
                {
                    _SetTabAgentNote(tab, notes.find(sessionId) != notes.end());
                }
            }
            _tabNotesEpoch = epoch;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_RefreshAllTabNotesIfChanged");
        }
    }
}
