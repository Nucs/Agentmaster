// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the transcript overlay (AgentTranscriptOverlay.h has the design: what it shows, how it
// renders, what closes it). This file is the whole implementation:
//   * _BuildUi            — the host layer (backdrop + card), header (with the ↵ line-wrap toggle), level
//                           tabs + "Expand all", body;
//   * Show / Close        — open / retarget / close, the focus hand-offs; the remembered level
//                           (_RememberedLevel on open, _RememberLevel on every tab pick — settings.json);
//   * _ToggleWrapLines    — the line-wrap toggle: ApplyWrapMode per level view, the reader's place kept by
//                           FindScrollAnchor / RestoreScrollAnchor (_SyncWrapMode), the state remembered
//                           for every window (_RememberedWrapLines on open, _RememberWrapLines per click);
//   * _LoadLevelAsync     — the pool-thread render (TranscriptTurns / the Codex readers + MarkdownBlocks,
//                           and the tool areas to fold, MdFindToolAreas);
//   * _Pump / _BuildNext  — the UI-thread, time-sliced block -> XAML build, which folds each tool area
//                           into a summary row + a hidden panel (_BuildFold) or a one-line note;
//   * _BuildBlockElement  — one MdBlock -> one element: text / code / rule, wrapped in its quote bars
//                           and list indents; a Session chain row's "session N" become hyperlinks
//                           (_AppendSessionLinks), each scrolling session N's heading to the top
//                           (_JumpToSession / _ScrollToSessionHeading, or a pending jump done by _Pump).
#include "pch.h"
#include "AgentTranscriptOverlay.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — Rule #18 forensics for every contained failure
#include "AgentClipboard.h" // RobustCopyTextToClipboard — the retry-looped Win32 clipboard writer every Agentmaster copy uses
#include "AgentCopyActions.h" // kTranscriptCopyMenu — the levels (order, labels, descriptions) the tabs mirror
#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the islands-safe tooltip host
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeDisplayTranscript / ForkSourceTranscriptNote / LogNav / ShortId / AppendStateLog
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings — the remembered level (AppSettings::transcriptOverlayLevel)
#include "AgentMaster/ProcessInspect.h" // ReadConversationText / ReadConversationFollowupText / the Codex rollout resolve
#include "AgentMaster/TranscriptTurns.h" // RenderTranscriptTurnsFile / TurnsOptionsForLevel / TurnsLevelName

#include <winrt/Windows.UI.Xaml.Automation.h> // AutomationProperties (button names for screen readers)

#include <mmsystem.h> // PlaySoundW — the copy chime every Agentmaster copy plays
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <chrono>
#include <cwctype> // iswspace / towlower

// Every XAML event handler below (clicks, key presses, pointer presses, focus changes) and every
// dispatcher item contains its own exceptions: one escaping a routed-event handler comes back as a
// stowed-exception fail-fast (0xC000027B) — the whole process, every window — for what is at worst
// a reading view that failed to update (the CLAUDE.md tag-popup gotcha).

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Documents;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
// Narrow using-DECLARATIONS for the color helpers: `using namespace winrt::Windows::UI;` would also pull
// the nested Text namespace into scope (the CLAUDE.md XAML name-clash gotcha).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::System::DispatcherQueue;
using winrt::Windows::System::DispatcherQueuePriority;
using winrt::Windows::System::VirtualKey;

namespace
{
    // Above every other child of the page's Root (the Sessions page host, the command palette, the
    // dialogs' row-2 placeholders): the overlay must cover whatever is on screen.
    constexpr int32_t kHostZIndex = 900;

    // The input caps (see MdParseOptions). A Max-level render of the largest session on this machine
    // (a 29.5 MB transcript) was 1.5 M characters / ~10 k blocks; the caps sit well above real sessions
    // and exist only so a pathological one degrades into "the view stops here" instead of a stall.
    constexpr size_t kMaxMarkdownChars = 8'000'000; // characters parsed (the rest is still copyable)
    constexpr size_t kMaxBlocks = 25'000; // blocks built into XAML
    constexpr size_t kMaxBlockChars = 150'000; // characters in one block (a huge written file)

    // The build's time slicing: each low-priority dispatcher item builds blocks until this budget is
    // spent (but at least kMinBlocksPerSlice, so a slow machine still makes progress), then yields —
    // input and rendering run between slices, so the window never feels frozen while a long transcript
    // lays out.
    constexpr auto kSliceBudget = std::chrono::milliseconds(12);
    constexpr size_t kMinBlocksPerSlice = 16;

    // Elements per chunk panel. Appending to a panel re-measures it; a two-level stack (chunks of 200)
    // keeps each append from walking ten thousand siblings.
    constexpr size_t kChunkSize = 200;

    // Typography (DIPs).
    constexpr double kBodyFontSize = 14.0;
    constexpr double kCodeFontSize = 12.5;
    constexpr double kStatusFontSize = 12.0;
    constexpr double kQuoteBarWidth = 3.0;
    constexpr double kQuotePadding = 10.0;
    constexpr double kBulletMarkerWidth = 16.0;

    // The text appended where a block was cut at kMaxBlockChars.
    constexpr const wchar_t* kCutNote = L"\n\x2026 (cut here \x2014 the header's Copy button copies the whole text)";

    // A FOLD's summary row (a folded tool area): the chevron glyphs (Segoe Fluent Icons ChevronRight /
    // ChevronDown), the row text size, and where the row's text starts — the button's left padding (2)
    // + the chevron's width (12) + the gap after it (6). The "(no tool calls)" note starts at the same
    // x, so every turn's line under "● Claude" lines up whether it folds or not.
    constexpr const wchar_t* kFoldClosedGlyph = L"\xE76C";
    constexpr const wchar_t* kFoldOpenGlyph = L"\xE70D";
    constexpr double kFoldFontSize = 13.0;
    constexpr double kFoldChevronWidth = 12.0;
    constexpr double kFoldChevronGap = 6.0;
    constexpr double kFoldRowPaddingLeft = 2.0;
    constexpr double kFoldRowPaddingTop = 2.0;
    constexpr double kFoldTextIndent = kFoldRowPaddingLeft + kFoldChevronWidth + kFoldChevronGap;

    // The header's LINE-WRAP toggle. The glyph is Segoe Fluent Icons' ReturnKey (↵) — the same symbol the
    // summary panel's wrap toggle shows, but from the header's own icon font, whose 1-em line box keeps the
    // toggle exactly as tall as Copy / Refresh / Close (Segoe UI Symbol's ↵ would be ~4.6 px taller at
    // 14 px and grow the header row). The tooltips state the mode, what a click does, and that the choice
    // follows the user into every window.
    constexpr const wchar_t* kWrapGlyph = L"\xE751";
    constexpr const wchar_t* kWrapOnTip = L"Wrap long lines: ON. A line wider than the view continues on the next line.\n"
                                          L"Click to turn it off: long lines then stop at the right edge, and the rest of each line is hidden.\n"
                                          L"Your choice applies to the transcript in every window.";
    constexpr const wchar_t* kWrapOffTip = L"Wrap long lines: OFF. A line wider than the view stops at the right edge, and the rest of it is hidden "
                                           L"(the Copy button still copies every character).\n"
                                           L"Click to turn it on: long lines then continue on the next line.\n"
                                           L"Your choice applies to the transcript in every window.";
    // An ON toggle's face: the accent as a translucent tint (~28%) with a stronger hairline (~70%) — on the
    // dark card that reads as "pressed / on" without a solid accent block next to the plain icon buttons.
    constexpr uint8_t kToggleOnFillAlpha = 0x47;
    constexpr uint8_t kToggleOnBorderAlpha = 0xB3;

    // How deep the reading-position anchor descends: the level's content stack -> a chunk -> an open fold
    // panel -> that fold's chunk -> the element (4). A guard, never reached by the overlay's own tree.
    constexpr int kMaxAnchorDepth = 8;

    // A Session chain link scrolls its session's heading to the top of the view, this many DIPs below the
    // edge — a hair of air, so the heading never reads as cut off.
    constexpr double kSessionJumpGap = 8.0;

    // Point a level's scroll view at a line-wrap mode — the whole mechanism of the ↵ toggle, one property:
    //   wrap ON  -> HorizontalScrollBarVisibility DISABLED: the content is measured at the viewport's width,
    //               so every text block (TextWrapping::Wrap) wraps to it — the view as it always was;
    //   wrap OFF -> HIDDEN: the content is measured at INFINITE width, so no text block wraps (only real line
    //               breaks break), no bar shows, and the scroll presenter clips whatever runs past the right
    //               edge.
    // HorizontalScrollMode stays Disabled (set once in _EnsureLevelView), so touch and the mouse wheel can't
    // pan the clipped part into view: OFF means hidden, not a sideways scroll away. Stretched elements (code
    // panels, quote bars, rules) take the content's width, so with wrap OFF their right edges are cut off
    // too, exactly like the text.
    // Parameters: scroll — the level's scroll view; wrap — true for the wrapping view.
    // Throws winrt::hresult_error on a XAML failure (callers contain it).
    void ApplyWrapMode(const ScrollViewer& scroll, bool wrap)
    {
        scroll.HorizontalScrollBarVisibility(wrap ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Hidden);
    }

    // The reader's place in a level's scroll view, held as an ELEMENT rather than a pixel offset: the PATH of
    // child indices from the level's content stack down through its chunk / fold stacks to the deepest
    // element crossing the top edge of the view, plus how far down that element's layout slot the edge sat
    // (0..1). A line-wrap switch re-lays every block out at a new height, so the old pixel offset would land
    // in another passage; the element + fraction lands the reader back on the same text.
    struct ScrollAnchor
    {
        std::vector<uint32_t> path; // empty == no anchor: the view sat at its top, or the walk failed
        double fraction = 0.0; // 0 == the top edge was at the element's top, 1 == at its bottom
    };

    // Where a vertical stack's FIRST child starts below the top of the stack's own layout slot: its top
    // margin + top padding. Each later child then starts where the one before it ends — that child's
    // DesiredSize, margins included — because the overlay's stacks never set Spacing (a future Spacing
    // would only shift a restored position by that many pixels per earlier sibling: harmless, not wrong).
    // Throws winrt::hresult_error on a XAML failure.
    double StackContentTop(const StackPanel& stack)
    {
        return stack.Margin().Top + stack.Padding().Top;
    }

    // Find the reader's place (ScrollAnchor) in `scroll`, whose content is `content`, from the CURRENT
    // layout: the extent coordinates of every element are summed from its earlier siblings' DesiredSize
    // (a vertical stack places each child right below the previous one; a collapsed child's DesiredSize is
    // 0, so a folded tool list costs nothing and needs no special case). No TransformToVisual: a hidden
    // level's view keeps the layout it had when it was last shown, and this reads exactly that — consistent
    // with the VerticalOffset it kept.
    // Parameters: scroll — the level's scroll view; content — its content stack (scroll.Content()).
    // Returns the anchor; an empty path when the view sits at its top (the top stays the top in any
    // layout), when the top edge lies below every element, or when the walk failed.
    // Never throws (a failure is logged — Rule #18 — and yields no anchor, which keeps the raw offset).
    ScrollAnchor FindScrollAnchor(const ScrollViewer& scroll, const StackPanel& content) noexcept
    {
        ScrollAnchor anchor;
        try
        {
            if (!scroll || !content)
            {
                return anchor;
            }
            const double top = scroll.VerticalOffset();
            if (top < 1.0)
            {
                return anchor;
            }
            StackPanel stack = content;
            double start = StackContentTop(stack); // where `stack`'s first child starts, in extent coordinates
            for (int depth = 0; depth < kMaxAnchorDepth; ++depth)
            {
                const auto children = stack.Children();
                const uint32_t count = children.Size();
                double y = start;
                uint32_t index = 0;
                for (; index < count; ++index)
                {
                    const double height = children.GetAt(index).DesiredSize().Height;
                    if (y + height > top)
                    {
                        break; // this child crosses (or starts at) the top edge
                    }
                    y += height;
                }
                if (index >= count)
                {
                    break; // the edge is below every child (the stack's bottom padding): nothing to hold on to
                }
                anchor.path.push_back(index);
                const auto child = children.GetAt(index);
                // A stack crossing the edge (a chunk, an open fold): one of ITS children is a finer anchor.
                // A stack starting exactly at the edge is anchored as a whole (fraction 0) — same place.
                const auto inner = child.try_as<StackPanel>();
                if (inner && inner.Orientation() == Orientation::Vertical && inner.Children().Size() > 0 && y < top)
                {
                    stack = inner;
                    start = y + StackContentTop(inner);
                    continue;
                }
                const double height = child.DesiredSize().Height;
                anchor.fraction = height > 0.0 ? std::clamp((top - y) / height, 0.0, 1.0) : 0.0;
                return anchor;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: no anchor — the view keeps its raw offset (clamped by the new extent).
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay find scroll anchor");
        }
        anchor.path.clear(); // an incomplete walk is no anchor
        return anchor;
    }

    // Scroll `scroll` so the anchor's element sits at the top edge again, as far into it as before. Call it
    // only after the view was laid out under its new mode (UpdateLayout): it reads the NEW DesiredSizes,
    // and ChangeView clamps to the current extent.
    // Parameters: scroll / content — as for FindScrollAnchor; anchor — what FindScrollAnchor returned.
    // Returns whether a scroll was requested — false for an empty anchor, or when the tree no longer has the
    // anchor's path (the view then keeps its clamped raw offset).
    // Never throws (a failure is logged — Rule #18).
    bool RestoreScrollAnchor(const ScrollViewer& scroll, const StackPanel& content, const ScrollAnchor& anchor) noexcept
    {
        try
        {
            if (!scroll || !content || anchor.path.empty())
            {
                return false;
            }
            StackPanel stack = content;
            double start = StackContentTop(stack);
            for (size_t depth = 0; depth < anchor.path.size(); ++depth)
            {
                const auto children = stack.Children();
                const uint32_t index = anchor.path[depth];
                if (index >= children.Size())
                {
                    return false; // the tree changed under the anchor (never between find and restore today)
                }
                double y = start;
                for (uint32_t i = 0; i < index; ++i)
                {
                    y += children.GetAt(i).DesiredSize().Height;
                }
                const auto child = children.GetAt(index);
                if (depth + 1 < anchor.path.size())
                {
                    const auto inner = child.try_as<StackPanel>();
                    if (!inner)
                    {
                        return false;
                    }
                    stack = inner;
                    start = y + StackContentTop(inner);
                    continue;
                }
                const double offset = y + anchor.fraction * child.DesiredSize().Height;
                return scroll.ChangeView(nullptr, std::max(offset, 0.0), nullptr, true /* disableAnimation: a jump, not a glide */);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: none needed — the view keeps its clamped raw offset.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay restore scroll anchor");
        }
        return false;
    }

    // The view keeps one LevelView per TurnsLevel value in a fixed array of 5, and settings.json stores
    // the remembered level by NAME (TurnsLevelName) — both assume TranscriptTurns' five levels.
    static_assert(::Agentmaster::kTurnsLevelCount == 5, "AgentTranscriptOverlay::_levels holds 5 views; update it with the TurnsLevel set");

    // The TurnsLevel value whose TurnsLevelName is `name`, compared case-insensitively (a hand-edited
    // settings.json may say "Compact"); -1 when no level has that name (empty included).
    int LevelFromName(std::wstring_view name) noexcept
    {
        for (int level = 0; level < ::Agentmaster::kTurnsLevelCount; ++level)
        {
            const std::wstring_view known{ ::Agentmaster::TurnsLevelName(static_cast<::Agentmaster::TurnsLevel>(level)) };
            if (known.size() != name.size())
            {
                continue;
            }
            bool same = true;
            for (size_t i = 0; i < known.size(); ++i)
            {
                if (towlower(name[i]) != towlower(known[i]))
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return level;
            }
        }
        return -1;
    }

    // The note an EMPTY tool area becomes: its single item's text without the surrounding blanks and
    // parentheses — "(no tool calls)" -> "no tool calls". Falls back to "no tool calls" for an item with
    // no text. Throws std::bad_alloc only.
    std::wstring EmptyAreaNoteText(const ::Agentmaster::MdBlock& block)
    {
        std::wstring text;
        for (const auto& run : block.runs)
        {
            text += run.text;
        }
        const auto first = text.find_first_not_of(L" \t\r\n(");
        const auto last = text.find_last_not_of(L" \t\r\n)");
        if (first == std::wstring::npos || last == std::wstring::npos || last < first)
        {
            return L"no tool calls";
        }
        return text.substr(first, last - first + 1);
    }

    // One solid brush from ARGB bytes. The overlay makes each color ONCE (in _BuildUi) and shares the
    // brush across every element — a brush per element would multiply objects by the block count.
    SolidColorBrush MakeBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
    }

    // The heading sizes (h1..h6): a clear hierarchy without letting a transcript's many "### Turn N"
    // headers (h3) shout over the text.
    double HeadingFontSize(uint8_t level) noexcept
    {
        switch (level)
        {
        case 1:
            return 22.0;
        case 2:
            return 19.0;
        case 3:
            return 16.5;
        default:
            return 14.5;
        }
    }

    // The hanging-indent width of a list item's marker column: room for the widest marker its list
    // shows (MdContainer::markerChars) plus a gap before the text.
    double MarkerColumnWidth(const ::Agentmaster::MdContainer& item) noexcept
    {
        if (!item.ordered)
        {
            return kBulletMarkerWidth;
        }
        return 8.0 + 7.5 * static_cast<double>(item.markerChars);
    }

    // Only plain web links become clickable Hyperlinks. A transcript quotes whatever its messages held;
    // a file:, ms-settings: or custom-protocol link launching on one stray click is not a reading
    // view's business, so those render as link-colored text (still visible, still copyable).
    bool IsWebUrl(const std::wstring& url) noexcept
    {
        const auto startsWith = [&url](std::wstring_view prefix) {
            if (url.size() < prefix.size())
            {
                return false;
            }
            for (size_t i = 0; i < prefix.size(); ++i)
            {
                if (towlower(url[i]) != prefix[i])
                {
                    return false;
                }
            }
            return true;
        };
        return startsWith(L"https://") || startsWith(L"http://");
    }

    // 12345 -> "12,345" for the status line.
    std::wstring WithThousands(size_t value)
    {
        std::wstring digits = std::to_wstring(value);
        for (ptrdiff_t at = static_cast<ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3)
        {
            digits.insert(static_cast<size_t>(at), 1, L',');
        }
        return digits;
    }

    // A duration for the status line: "640 ms" under a second, "2.4 s" above.
    std::wstring FormatDurationMs(int64_t ms)
    {
        if (ms < 1000)
        {
            return std::to_wstring(std::max<int64_t>(ms, 0)) + L" ms";
        }
        const int64_t tenths = (ms + 50) / 100;
        return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" s";
    }

    // Every run of the block is italic (or blank): transcript_turns' notes and stats lines
    // ("_4 tool calls · 1m 30s · …_"). They render dimmed — secondary information, still readable.
    bool AllItalic(const ::Agentmaster::MdBlock& block) noexcept
    {
        bool sawItalic = false;
        for (const auto& run : block.runs)
        {
            bool blank = true;
            for (const wchar_t ch : run.text)
            {
                if (!iswspace(ch))
                {
                    blank = false;
                    break;
                }
            }
            if (blank)
            {
                continue;
            }
            if ((run.style & ::Agentmaster::kMdItalic) == 0)
            {
                return false;
            }
            sawItalic = true;
        }
        return sawItalic;
    }

    // The speaker glyph a label line starts with: '\x276F' (❯ the user), '\x25CF' (● Claude or an
    // agent), '\x2709' (✉ a teammate / another session), or 0 for anything else.
    wchar_t LabelGlyph(const ::Agentmaster::MdBlock& block) noexcept
    {
        for (const auto& run : block.runs)
        {
            for (const wchar_t ch : run.text)
            {
                if (iswspace(ch))
                {
                    continue;
                }
                return (ch == L'\x276F' || ch == L'\x25CF' || ch == L'\x2709') ? ch : L'\0';
            }
        }
        return L'\0';
    }

    // Add to an element's margin (keeping what is already there): the block builder layers a list
    // indent and a top gap onto the same element in separate steps.
    void AddMargin(const FrameworkElement& element, double left, double top)
    {
        auto margin = element.Margin();
        margin.Left += left;
        margin.Top += top;
        element.Margin(margin);
    }
}

namespace winrt::TerminalApp::implementation
{
    AgentTranscriptOverlay::AgentTranscriptOverlay() :
        // The page constructs the overlay on its UI thread, so this is that thread's queue — every
        // build slice and render hand-off is posted to it.
        _dispatcher{ DispatcherQueue::GetForCurrentThread() }
    {
    }

    AgentTranscriptOverlay::~AgentTranscriptOverlay() = default;

    void AgentTranscriptOverlay::SetClosedHandler(std::function<void()> handler)
    {
        _onClosed = std::move(handler);
    }

    void AgentTranscriptOverlay::Attach(const Grid& pageRoot)
    {
        if (_host || !pageRoot)
        {
            return;
        }
        _pageRoot = pageRoot;
        _BuildUi();
        // Span EVERY row of the page (tab row when it isn't in the titlebar, infobars, content) so the
        // card's 90% x 80% is of the window, and the backdrop catches every click outside it.
        const auto rows = pageRoot.RowDefinitions().Size();
        Grid::SetRow(_host, 0);
        Grid::SetRowSpan(_host, static_cast<int32_t>(std::max<uint32_t>(rows, 1)));
        Canvas::SetZIndex(_host, kHostZIndex);
        pageRoot.Children().Append(_host);
    }

    void AgentTranscriptOverlay::_BuildUi()
    {
        // --- shared brushes + fonts (one instance each, referenced by every element) ---
        _bodyBrush = MakeBrush(0xFF, 0xE6, 0xE6, 0xE6);
        _dimBrush = MakeBrush(0xFF, 0x9E, 0x9E, 0x9E);
        _inlineCodeBrush = MakeBrush(0xFF, 0xE2, 0xC0, 0x8D);
        _linkBrush = MakeBrush(0xFF, 0x6C, 0xB8, 0xFF);
        _markerBrush = MakeBrush(0xFF, 0x9A, 0x9A, 0x9A);
        _codeTextBrush = MakeBrush(0xFF, 0xD4, 0xD4, 0xD4);
        _codeBackBrush = MakeBrush(0xFF, 0x14, 0x14, 0x14);
        _codeBorderBrush = MakeBrush(0xFF, 0x2E, 0x2E, 0x2E);
        _ruleBrush = MakeBrush(0xFF, 0x3A, 0x3A, 0x3A);
        _quoteBarBrush = MakeBrush(0xFF, 0x55, 0x55, 0x55);
        _userBrush = MakeBrush(0xFF, 0x7F, 0xC1, 0xF0);
        _userBarBrush = MakeBrush(0xFF, 0x3E, 0x8E, 0xD0);
        _claudeBrush = MakeBrush(0xFF, 0xF0, 0xA0, 0x60);
        _claudeBarBrush = MakeBrush(0xFF, 0xC8, 0x74, 0x2C);
        _peerBrush = MakeBrush(0xFF, 0xB9, 0xA0, 0xEE);
        _peerBarBrush = MakeBrush(0xFF, 0x8A, 0x6C, 0xD0);
        _transparentBrush = MakeBrush(0x00, 0x00, 0x00, 0x00);
        auto accent = ColorHelper::FromArgb(0xFF, 0x3B, 0x82, 0xF6); // fallback when the theme has no accent
        try
        {
            if (const auto value = Application::Current().Resources().TryLookup(winrt::box_value(L"SystemAccentColor")))
            {
                accent = winrt::unbox_value_or<Color>(value, accent);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay accent lookup"); // the fallback blue stays
        }
        _accentBrush = SolidColorBrush{ accent };
        _toggleOnBrush = SolidColorBrush{ ColorHelper::FromArgb(kToggleOnFillAlpha, accent.R, accent.G, accent.B) };
        _toggleOnBorderBrush = SolidColorBrush{ ColorHelper::FromArgb(kToggleOnBorderAlpha, accent.R, accent.G, accent.B) };
        _monoFont = FontFamily{ L"Cascadia Mono, Cascadia Code, Consolas" };
        _textMenu = winrt::Microsoft::Terminal::UI::TextMenuFlyout{};

        // --- the host: a 3x3 star grid whose middle cell IS the card's 90% x 80% (no size math, no
        // SizeChanged handler — it follows every window resize through layout alone) ---
        _host = Grid{};
        _host.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark (CLAUDE.md conventions)
        _host.Visibility(Visibility::Collapsed);
        for (const double weight : { 1.0, 8.0, 1.0 }) // 10% · 80% · 10% of the height
        {
            RowDefinition row;
            row.Height(GridLength{ weight, GridUnitType::Star });
            _host.RowDefinitions().Append(row);
        }
        for (const double weight : { 5.0, 90.0, 5.0 }) // 5% · 90% · 5% of the width
        {
            ColumnDefinition column;
            column.Width(GridLength{ weight, GridUnitType::Star });
            _host.ColumnDefinitions().Append(column);
        }

        // The backdrop: a SIBLING of the card (not its parent), so a press inside the card never
        // bubbles to it — only a press that lands outside the card reaches this handler.
        _backdrop = Border{};
        _backdrop.Background(MakeBrush(0x66, 0x00, 0x00, 0x00));
        Grid::SetRowSpan(_backdrop, 3);
        Grid::SetColumnSpan(_backdrop, 3);
        _backdrop.PointerPressed([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const PointerRoutedEventArgs& e) {
            try
            {
                e.Handled(true); // the page behind must not see a press meant to close us
                const auto self = weak.get();
                if (!self || !self->_open)
                {
                    return;
                }
                // Deferred: collapsing the very layer under the pointer INSIDE its own press handler is the
                // tree mutation the pointer-handler rule forbids (the Archive / Sessions page lessons).
                self->_dispatcher.TryEnqueue([weak]() {
                    if (const auto overlay = weak.get())
                    {
                        overlay->Close(L"click outside", true);
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay backdrop press");
            }
        });
        _host.Children().Append(_backdrop);

        _card = Border{};
        _card.Background(MakeBrush(0xFF, 0x1E, 0x1E, 0x1E));
        _card.BorderBrush(MakeBrush(0xFF, 0x3C, 0x3C, 0x3C));
        _card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        Grid::SetRow(_card, 1);
        Grid::SetColumn(_card, 1);
        // Tab cycles inside the card: the overlay is modal, so keyboard focus must not walk out into
        // the page hidden behind it.
        _card.TabFocusNavigation(KeyboardNavigationMode::Cycle);
        _host.Children().Append(_card);

        Grid layout;
        for (const auto height : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star } })
        {
            RowDefinition row;
            row.Height(height);
            layout.RowDefinitions().Append(row);
        }
        _card.Child(layout);

        // --- header: [icon] Transcript  <session line ...>  [Wrap] [Copy] [Refresh] [Close] ---
        _headerRow = Grid{};
        for (const auto width : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star }, GridLength{ 0, GridUnitType::Auto } })
        {
            ColumnDefinition column;
            column.Width(width);
            _headerRow.ColumnDefinitions().Append(column);
        }
        _headerRow.Margin(Thickness{ 0, 4, 0, 0 });
        {
            FontIcon icon;
            icon.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            icon.Glyph(L"\xE736"); // ReadingMode — the same glyph as the menus' "Show Transcript"
            icon.FontSize(16);
            icon.Foreground(_bodyBrush);
            icon.Margin(Thickness{ 16, 0, 10, 0 });
            icon.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(icon, 0);
            _headerRow.Children().Append(icon);

            TextBlock title;
            title.Text(L"Transcript");
            title.FontSize(15);
            title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            title.Foreground(_bodyBrush);
            title.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(title, 1);
            _headerRow.Children().Append(title);

            _subtitleText = TextBlock{};
            _subtitleText.FontSize(13);
            _subtitleText.Foreground(_dimBrush);
            _subtitleText.TextWrapping(TextWrapping::NoWrap);
            _subtitleText.TextTrimming(TextTrimming::CharacterEllipsis);
            _subtitleText.VerticalAlignment(VerticalAlignment::Center);
            _subtitleText.Margin(Thickness{ 12, 0, 12, 0 });
            Grid::SetColumn(_subtitleText, 2);
            _headerRow.Children().Append(_subtitleText);

            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(2);
            buttons.Margin(Thickness{ 0, 6, 8, 6 });
            const auto makeIconButton = [this](const wchar_t* glyph, const wchar_t* tip, const wchar_t* name) {
                Button b;
                b.Background(_transparentBrush); // transparent (not null) — still hit-testable, the template still highlights hover
                b.BorderThickness(Thickness{ 0, 0, 0, 0 });
                b.Padding(Thickness{ 9, 6, 9, 6 });
                b.MinWidth(0);
                b.MinHeight(0);
                FontIcon glyphIcon;
                glyphIcon.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
                glyphIcon.Glyph(glyph);
                glyphIcon.FontSize(14);
                // Click-through glyph: a hit-testable FontIcon on a transparent button swallows presses
                // landing on its own pixels (the badge's mkIconBtn lesson) — the button must be the only target.
                glyphIcon.IsHitTestVisible(false);
                b.Content(glyphIcon);
                AgentSetTip(b, winrt::hstring{ tip });
                winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(b, winrt::hstring{ name });
                return b;
            };
            // The ↵ LINE-WRAP toggle — first in the group: a view setting, before the actions. ON (the
            // default): long lines wrap at the body's right edge. OFF: every line keeps its length and the
            // part past the right edge is hidden. GLOBAL — remembered in settings.json and read back on every
            // open, in any window (_ToggleWrapLines / Show). Built by hand, not by makeIconButton: its ON
            // state is a tinted FACE inside the content (see _wrapButtonFace), so the face carries the
            // padding the button carries elsewhere — 1 px hairline + 8 / 5 padding == the icon buttons'
            // 9 / 6, so the toggle is exactly their size.
            _wrapGlyph = FontIcon{};
            _wrapGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            _wrapGlyph.Glyph(kWrapGlyph);
            _wrapGlyph.FontSize(14);
            _wrapGlyph.IsHitTestVisible(false); // the button must be the only target (the badge's mkIconBtn lesson)
            _wrapButtonFace = Border{};
            _wrapButtonFace.BorderThickness(Thickness{ 1, 1, 1, 1 }); // always 1 px: a flip only recolors it, so nothing moves
            _wrapButtonFace.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // the button template's corner radius (ControlCornerRadius)
            _wrapButtonFace.Padding(Thickness{ 8, 5, 8, 5 });
            // Click-through too: a TINTED Border is hit-testable and would take the press itself; the
            // button's transparent background already makes its whole area the target.
            _wrapButtonFace.IsHitTestVisible(false);
            _wrapButtonFace.Child(_wrapGlyph);
            _wrapButton = Button{};
            _wrapButton.Background(_transparentBrush); // transparent (not null) — still hit-testable, the template still highlights hover
            _wrapButton.BorderThickness(Thickness{ 0, 0, 0, 0 });
            _wrapButton.Padding(Thickness{ 0, 0, 0, 0 }); // the face carries the padding (see above)
            _wrapButton.MinWidth(0);
            _wrapButton.MinHeight(0);
            _wrapButton.Content(_wrapButtonFace);
            _wrapButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_ToggleWrapLines();
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: none needed — the mode is applied and remembered before the only
                    // step that can throw here (the toggle's repaint); a stale toggle look fixes itself on
                    // the next click or open.
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay wrap toggle");
                }
            });
            buttons.Children().Append(_wrapButton);
            _UpdateWrapButton(); // the default look (ON) until Show reads the remembered state
            auto copyButton = makeIconButton(L"\xE8C8", L"Copy this level's whole transcript as Markdown \x2014 everything, even past where a very long view stops", L"Copy transcript");
            copyButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_CopyCurrentLevel();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay copy"); // no copy, no chime
                }
            });
            buttons.Children().Append(copyButton);
            auto refreshButton = makeIconButton(L"\xE72C", L"Render the transcript again \x2014 a working session keeps adding to it", L"Refresh transcript");
            refreshButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_Refresh();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay refresh"); // the view stays as it was
                }
            });
            buttons.Children().Append(refreshButton);
            auto closeButton = makeIconButton(L"\xE711", L"Close (Esc, or click outside the transcript)", L"Close transcript");
            closeButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                if (const auto self = weak.get())
                {
                    self->Close(L"close button", true);
                }
            });
            buttons.Children().Append(closeButton);
            Grid::SetColumn(buttons, 3);
            _headerRow.Children().Append(buttons);
        }
        Grid::SetRow(_headerRow, 0);
        layout.Children().Append(_headerRow);

        // --- the level tabs (the Copy menu's order — Default first) + the status line + "Expand all" ---
        _tabsRow = Grid{};
        for (const auto width : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star }, GridLength{ 0, GridUnitType::Auto } })
        {
            ColumnDefinition column;
            column.Width(width);
            _tabsRow.ColumnDefinitions().Append(column);
        }
        _tabsRow.Margin(Thickness{ 8, 0, 16, 0 });
        StackPanel tabs;
        tabs.Orientation(Orientation::Horizontal);
        tabs.Spacing(2);
        size_t position = 0;
        for (const auto& item : kTranscriptCopyMenu)
        {
            const int level = item.which - kCopyFieldTranscriptLevelBase;
            Button tab;
            tab.Background(_transparentBrush);
            tab.BorderThickness(Thickness{ 0, 0, 0, 0 });
            tab.Padding(Thickness{ 10, 5, 10, 0 });
            tab.MinWidth(0);
            tab.MinHeight(0);
            // The selection underline lives INSIDE the content, not on the button's border: the button
            // template's hover / press states set its BorderBrush, and a visual-state setter outranks a
            // local value — an underline drawn there would vanish under the pointer.
            StackPanel inner;
            TextBlock label;
            label.Text(item.label);
            label.FontSize(13);
            label.Foreground(_bodyBrush);
            Border underline;
            underline.Height(2);
            underline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
            underline.Margin(Thickness{ 0, 5, 0, 0 });
            underline.Background(_transparentBrush);
            inner.Children().Append(label);
            inner.Children().Append(underline);
            tab.Content(inner);
            AgentSetTip(tab, winrt::hstring{ std::wstring{ item.tip } + L"\n\nShortcut: " + std::to_wstring(position + 1) });
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(tab, winrt::hstring{ std::wstring{ item.label } + L" transcript" });
            tab.Click([weak = get_weak(), position](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_SelectTabPosition(position);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay tab click"); // the current level stays
                }
            });
            tabs.Children().Append(tab);
            _tabButtons.push_back(tab);
            _tabLabels.push_back(label);
            _tabUnderlines.push_back(underline);
            _tabLevels.push_back(level);
            ++position;
        }
        Grid::SetColumn(tabs, 0);
        _tabsRow.Children().Append(tabs);
        _statusText = TextBlock{};
        _statusText.FontSize(kStatusFontSize);
        _statusText.Foreground(_dimBrush);
        _statusText.HorizontalAlignment(HorizontalAlignment::Right);
        _statusText.VerticalAlignment(VerticalAlignment::Center);
        _statusText.TextWrapping(TextWrapping::NoWrap);
        _statusText.TextTrimming(TextTrimming::CharacterEllipsis);
        _statusText.Margin(Thickness{ 12, 0, 0, 0 });
        Grid::SetColumn(_statusText, 1);
        _tabsRow.Children().Append(_statusText);
        // "Expand all" / "Collapse all": opens (or folds back) every tool-call list of the CURRENT level.
        // Link-colored text, so it reads as an action next to the dim status; Collapsed until the current
        // level turns out to have folds (_UpdateExpandAllButton).
        _expandAllLabel = TextBlock{};
        _expandAllLabel.FontSize(kStatusFontSize);
        _expandAllLabel.Foreground(_linkBrush);
        _expandAllLabel.Text(L"Expand all");
        _expandAllButton = Button{};
        _expandAllButton.Background(_transparentBrush); // transparent (not null) — hit-testable, hover still highlights
        _expandAllButton.BorderThickness(Thickness{ 0, 0, 0, 0 });
        _expandAllButton.Padding(Thickness{ 10, 4, 0, 4 });
        _expandAllButton.MinWidth(0);
        _expandAllButton.MinHeight(0);
        _expandAllButton.VerticalAlignment(VerticalAlignment::Center);
        _expandAllButton.Visibility(Visibility::Collapsed);
        _expandAllButton.Content(_expandAllLabel);
        AgentSetTip(_expandAllButton, winrt::hstring{ L"Open or fold back every turn's tool calls at this level. A click on one turn's summary line opens just that turn." });
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_expandAllButton, winrt::hstring{ L"Expand all" });
        _expandAllButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                if (const auto self = weak.get())
                {
                    self->_ToggleExpandAll();
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay expand all"); // the folds stay as they were
            }
        });
        Grid::SetColumn(_expandAllButton, 2);
        _tabsRow.Children().Append(_expandAllButton);
        Grid::SetRow(_tabsRow, 1);
        layout.Children().Append(_tabsRow);

        Border divider;
        divider.Height(1);
        divider.Background(_ruleBrush);
        Grid::SetRow(divider, 2);
        layout.Children().Append(divider);

        _bodyHost = Grid{};
        Grid::SetRow(_bodyHost, 3);
        layout.Children().Append(_bodyHost);

        // --- keyboard: Esc closes; 1..5 pick a tab by position (bubbling from whatever inside the card
        // has focus — the scroll view, a selected text block, a button) ---
        _card.KeyDown([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const KeyRoutedEventArgs& e) {
            try
            {
                const auto self = weak.get();
                if (!self || !self->_open)
                {
                    return;
                }
                const auto key = e.Key();
                if (key == VirtualKey::Escape)
                {
                    e.Handled(true);
                    self->Close(L"esc", true);
                    return;
                }
                // Plain digits only: a Ctrl / Alt chord with a digit belongs to the app (switchToTab).
                if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
                {
                    return;
                }
                int tabPosition = -1; // (not `position`: /W4 + warnings-as-errors flags shadowing the loop counter above)
                if (key >= VirtualKey::Number1 && key <= VirtualKey::Number9)
                {
                    tabPosition = static_cast<int>(key) - static_cast<int>(VirtualKey::Number1);
                }
                else if (key >= VirtualKey::NumberPad1 && key <= VirtualKey::NumberPad9)
                {
                    tabPosition = static_cast<int>(key) - static_cast<int>(VirtualKey::NumberPad1);
                }
                if (tabPosition >= 0 && static_cast<size_t>(tabPosition) < self->_tabLevels.size())
                {
                    e.Handled(true);
                    self->_SelectTabPosition(static_cast<size_t>(tabPosition));
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay key");
            }
        });

        // --- the focus guard: while open, refuse a focus move from inside the card to anything else on
        // the PAGE (the terminal behind us, a Manager card its refresh re-focuses). Leaving the window,
        // or moving into a popup (the text context menu renders in the popup root, outside the page),
        // is allowed — and once closed (_open is cleared FIRST in Close) everything is allowed again. ---
        _card.LosingFocus([weak = get_weak()](const UIElement&, const LosingFocusEventArgs& args) {
            const auto self = weak.get();
            if (!self || !self->_open)
            {
                return;
            }
            try
            {
                const auto next = args.NewFocusedElement();
                if (!next || _IsUnder(next, self->_host) || !_IsUnder(next, self->_pageRoot))
                {
                    return;
                }
                args.TryCancel();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay focus guard"); // the focus move simply proceeds
            }
        });
    }

    void AgentTranscriptOverlay::Show(const TranscriptOverlaySource& source, int level) noexcept
    {
        try
        {
            if (!_host)
            {
                return;
            }
            const bool sameConversation = _open && source.sessionId == _source.sessionId && source.codex == _source.codex &&
                                          source.codexRolloutId == _source.codexRolloutId;
            if (!sameConversation)
            {
                // A different conversation (or a fresh open): nothing rendered so far applies. Bumping the
                // generation first makes any render / build slice still in flight for the old view a no-op.
                ++_generation;
                _ReleaseLevels();
                _source = source;
            }
            else if (!source.title.empty())
            {
                _source.title = source.title; // a rename since the last open
            }
            _open = true;
            _UpdateTitle();
            // The line-wrap state is, like the level, the one last chosen in ANY window — read fresh now, so
            // a click on the ↵ toggle in another window since this one last opened counts. Read BEFORE
            // _SelectLevel, which brings an already-built view of this conversation up to it.
            _wrapLines = _RememberedWrapLines();
            _UpdateWrapButton();
            _host.Visibility(Visibility::Visible);
            // kRememberedLevel (any negative value): the level last picked in ANY window, read fresh from
            // settings.json now — a pick made in another window since this one last opened counts.
            const int wanted = level < 0 ? _RememberedLevel() : level;
            _SelectLevel(std::clamp(wanted, 0, ::Agentmaster::kTurnsLevelCount - 1), true);
        }
        catch (...)
        {
            // Rule #18. Recovery: stay closed — a half-built overlay must not sit over the page eating clicks.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::Show");
            _open = false;
            try
            {
                if (_host)
                {
                    _host.Visibility(Visibility::Collapsed);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::Show collapse");
            }
        }
    }

    void AgentTranscriptOverlay::Close(std::wstring_view reason, bool restoreFocus) noexcept
    {
        if (!_open)
        {
            return;
        }
        // Cleared FIRST: the focus guard reads it, and collapsing the host while focus is inside moves
        // focus out — which must be allowed now.
        _open = false;
        ++_generation;
        try
        {
            // Tooltips live in the shared host's popup, which a collapsed parent does not hide.
            if (_headerRow)
            {
                AgentCloseTipsIn(_headerRow);
            }
            if (_tabsRow)
            {
                AgentCloseTipsIn(_tabsRow);
            }
            if (_host)
            {
                _host.Visibility(Visibility::Collapsed);
            }
            ::Agentmaster::LogNav(L"transcript-close " + ::Agentmaster::ShortId(_source.sessionId) + L" (" + std::wstring{ reason } + L")");
            // Release the rendered XAML LATER, at low priority: tearing down a long transcript's elements
            // here would delay whatever closed us (a key press, a tab switch). A reopen before it runs
            // finds _open set (or a newer generation) and keeps its own views.
            if (_dispatcher)
            {
                _dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [weak = get_weak(), generation = _generation]() {
                    const auto self = weak.get();
                    if (self && !self->_open && self->_generation == generation)
                    {
                        self->_ReleaseLevels();
                    }
                });
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::Close");
        }
        if (restoreFocus && _onClosed)
        {
            try
            {
                _onClosed();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay closed handler");
            }
        }
    }

    void AgentTranscriptOverlay::_SelectTabPosition(size_t position)
    {
        if (position >= _tabLevels.size())
        {
            return;
        }
        const int level = _tabLevels[position];
        if (level != _current)
        {
            ::Agentmaster::LogNav(L"transcript-level " + ::Agentmaster::ShortId(_source.sessionId) + L" " +
                                  ::Agentmaster::TurnsLevelName(static_cast<::Agentmaster::TurnsLevel>(level)));
        }
        _SelectLevel(level, true);
        // Every pick — the tab already showing included — is the level the next open uses, in any window
        // ("if the user picks Compact, the next transcript opens at Compact"). Another window may have
        // remembered a different level since this overlay opened, so even a re-pick writes.
        _RememberLevel(level);
    }

    int AgentTranscriptOverlay::_RememberedLevel() noexcept
    {
        try
        {
            const int level = LevelFromName(::Agentmaster::LoadAppSettings().transcriptOverlayLevel);
            if (level >= 0)
            {
                return level;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: open at Default — a failed settings read never blocks the overlay.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remembered level");
        }
        return static_cast<int>(::Agentmaster::TurnsLevel::Default);
    }

    void AgentTranscriptOverlay::_RememberLevel(int level) noexcept
    {
        try
        {
            const std::wstring name{ ::Agentmaster::TurnsLevelName(static_cast<::Agentmaster::TurnsLevel>(level)) };
            // FRESHEST disk, one field (the ColorPickupFlyout idiom): never write a stale in-memory copy
            // over a cog Save or another window's settings change.
            auto settings = ::Agentmaster::LoadAppSettings();
            if (settings.transcriptOverlayLevel != name)
            {
                settings.transcriptOverlayLevel = name;
                ::Agentmaster::SaveAppSettings(settings);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing — the switch already happened; only the remembered level is lost.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remember level");
        }
    }

    void AgentTranscriptOverlay::_SelectLevel(int level, bool focusBody)
    {
        // A session jump still pending on the level being LEFT (a chain link clicked before its heading was
        // built) is dropped: only the level on screen builds, so the jump would fire when the reader comes
        // back to it — scrolling away from wherever they are reading by then.
        if (level != _current && _current >= 0 && _current < static_cast<int>(_levels.size()))
        {
            _levels[_current].pendingSessionJump = 0;
        }
        _current = level;
        _UpdateTabVisuals();
        _EnsureLevelView(level);
        for (int i = 0; i < static_cast<int>(_levels.size()); ++i)
        {
            if (const auto& scroll = _levels[i].scroll)
            {
                scroll.Visibility(i == level ? Visibility::Visible : Visibility::Collapsed);
            }
        }
        auto& view = _levels[level];
        // A view last laid out under the other line-wrap mode — it was hidden when the ↵ toggle was clicked,
        // or another window changed the mode before this open — catches up now that it is visible, keeping
        // its reading position. A new view already has the mode (_EnsureLevelView): a no-op.
        _SyncWrapMode(view);
        if (view.state == LevelState::Idle)
        {
            _StartLoad(level);
        }
        else if (view.state == LevelState::Building)
        {
            _SchedulePump(); // resume a build this level paused when another tab was selected
        }
        _UpdateStatus();
        if (focusBody)
        {
            _FocusBodyDeferred();
        }
    }

    void AgentTranscriptOverlay::_EnsureLevelView(int level)
    {
        auto& view = _levels[level];
        if (view.scroll)
        {
            return;
        }
        view.content = StackPanel{};
        view.scroll = ScrollViewer{};
        view.scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        // Never a sideways scroll. With line wrap ON the content takes the viewport's width, so every text
        // block WRAPS to it instead of running off to the right; with it OFF the lines keep their length and
        // the part past the right edge is cut off — hidden, since this mode keeps touch and the mouse wheel
        // from panning it into view (ApplyWrapMode has the details).
        view.scroll.HorizontalScrollMode(ScrollMode::Disabled);
        ApplyWrapMode(view.scroll, _wrapLines);
        view.wrapLines = _wrapLines;
        view.scroll.ZoomMode(ZoomMode::Disabled);
        view.scroll.IsTabStop(true); // focusable, so PageUp / PageDown / Home / End / arrows scroll it
        view.scroll.Padding(Thickness{ 24, 14, 24, 18 });
        view.scroll.Content(view.content);
        _bodyHost.Children().Append(view.scroll);
    }

    void AgentTranscriptOverlay::_SetLevelMessage(int level, const std::wstring& text)
    {
        auto& view = _levels[level];
        view.message = text;
        if (!view.content)
        {
            return;
        }
        if (!view.messageText)
        {
            view.messageText = TextBlock{};
            view.messageText.FontSize(13);
            view.messageText.Foreground(_dimBrush);
            view.messageText.TextWrapping(TextWrapping::Wrap);
            view.messageText.Margin(Thickness{ 0, 6, 0, 0 });
            view.content.Children().InsertAt(0, view.messageText);
        }
        view.messageText.Text(winrt::hstring{ text });
    }

    void AgentTranscriptOverlay::_StartLoad(int level)
    {
        auto& view = _levels[level];
        view.state = LevelState::Loading;
        _SetLevelMessage(level, L"Rendering the transcript\x2026");
        _LoadLevelAsync(get_weak(), _dispatcher, _source, level, _generation);
    }

    winrt::fire_and_forget AgentTranscriptOverlay::_LoadLevelAsync(winrt::weak_ref<AgentTranscriptOverlay> weak,
                                                                   DispatcherQueue dispatcher,
                                                                   TranscriptOverlaySource source,
                                                                   int level,
                                                                   uint64_t generation)
    {
        // Every parameter is BY VALUE: the coroutine outlives the call that started it. It holds NO
        // strong reference to the overlay while on the pool thread — only `weak` — so the overlay (and
        // the page that owns it) can never be destroyed here, off its UI thread.
        try
        {
            co_await winrt::resume_background(); // reads + renders the transcript: never on the UI thread
            auto render = std::make_shared<LevelRender>();
            const auto started = std::chrono::steady_clock::now();
            _RenderLevel(source, level, *render);
            render->elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            const bool queued = dispatcher && dispatcher.TryEnqueue([weak, render, level, generation]() {
                // A throw escaping a DispatcherQueue handler is reported against the UI thread: contain it.
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_ApplyLevelRender(level, generation, std::move(*render));
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay apply render");
                }
            });
            if (!queued)
            {
                // No dispatcher, or it refused: the window closed while the transcript was rendering.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[transcript] " + ::Agentmaster::ShortId(source.sessionId) +
                                                                L" show: the window closed before the render finished\n");
            }
        }
        catch (...)
        {
            // Rule #18 — and the terminate-net every fire_and_forget needs (an escape IS winrt::terminate()).
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_LoadLevelAsync");
        }
    }

    void AgentTranscriptOverlay::_RenderLevel(const TranscriptOverlaySource& source, int level, LevelRender& out) noexcept
    {
        try
        {
            const auto turnsLevel = static_cast<::Agentmaster::TurnsLevel>(std::clamp(level, 0, ::Agentmaster::kTurnsLevelCount - 1));
            if (source.codex)
            {
                if (!source.codexRolloutId.empty())
                {
                    out.path = ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), source.codexRolloutId);
                }
            }
            else if (!source.sessionId.empty())
            {
                // The session's own transcript, else — for a fork with no messages yet — the one it was
                // forked from (the fork holds that conversation until its first message writes its own).
                auto display = ::Agentmaster::ResolveClaudeDisplayTranscript(source.sessionId, source.forkParentId);
                out.path = std::move(display.path);
                out.forkSourceId = std::move(display.forkSourceId);
            }
            if (out.path.empty())
            {
                out.message = source.codex ?
                                  L"Nothing to show yet \x2014 this Codex session has not been prompted, so it has no rollout." :
                                  L"Nothing to show yet \x2014 this session has not been prompted, so it has no transcript.";
                return;
            }
            // The SAME text the Copy menu's level copies (CopyTranscriptTurnsAsync): the turns view for
            // Claude; for a Codex rollout the brief (Minimal / Compact) or the whole conversation.
            std::wstring text;
            if (source.codex)
            {
                const bool brief = turnsLevel == ::Agentmaster::TurnsLevel::Minimal || turnsLevel == ::Agentmaster::TurnsLevel::Compact;
                text = brief ? ::Agentmaster::ReadConversationFollowupText(out.path, true, 0 /* whole file */) :
                               ::Agentmaster::ReadConversationText(out.path, true, 0 /* whole file */);
            }
            else
            {
                text = ::Agentmaster::RenderTranscriptTurnsFile(out.path, ::Agentmaster::TurnsOptionsForLevel(turnsLevel), /*utc*/ false).value_or(std::wstring{});
            }
            if (text.empty())
            {
                out.message = L"Nothing to show \x2014 the transcript could not be read, or it holds no messages at this level.";
                return;
            }
            if (!out.forkSourceId.empty())
            {
                // The fork's turns are its SOURCE's: say so above them. Prepended to the text itself (not
                // drawn beside it) so the header's Copy copies exactly what Copy ▸ Transcript copies for
                // this fork (CopyTranscriptTurnsAsync prepends the same note).
                text.insert(0, ::Agentmaster::ForkSourceTranscriptNote(out.forkSourceId));
            }
            out.lines = 1 + static_cast<size_t>(std::count(text.begin(), text.end(), L'\n'));
            std::wstring_view toParse{ text };
            if (toParse.size() > kMaxMarkdownChars)
            {
                // Cut at a line boundary so the last parsed line isn't half a construct.
                const auto cut = toParse.rfind(L'\n', kMaxMarkdownChars);
                toParse = toParse.substr(0, cut == std::wstring_view::npos ? kMaxMarkdownChars : cut);
                out.truncated = true;
            }
            ::Agentmaster::MdParseOptions options;
            options.maxBlocks = kMaxBlocks;
            options.maxBlockChars = kMaxBlockChars;
            auto parsed = ::Agentmaster::ParseMarkdownBlocks(toParse, options);
            out.blocks = std::move(parsed.blocks);
            out.truncated = out.truncated || parsed.truncatedBlocks;
            out.failed = parsed.failed;
            out.markdown = std::move(text); // after the parse — `toParse` viewed it
            // The tool-call lists the build folds (here, off the UI thread, with the parse: one linear pass
            // over the blocks, and the UI thread then only follows the indices).
            out.areas = ::Agentmaster::MdFindToolAreas(out.blocks);
            // The Session chain list and the session headings its links point to (one more linear pass;
            // an ordinary one-session transcript yields an empty chain).
            out.chain = ::Agentmaster::MdFindSessionChain(out.blocks);
            if (out.blocks.empty())
            {
                // A failed parse already logged its exception (ParseMarkdownBlocks, Rule #18) — say so
                // instead of claiming the level is empty.
                out.message = out.failed ? L"The transcript could not be rendered (the details are in hooks.log)." : L"Nothing to show at this level.";
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: whatever was produced, plus a message saying the render failed.
            out.failed = true;
            if (out.message.empty() && out.blocks.empty())
            {
                out.message = L"The transcript could not be rendered (the details are in hooks.log).";
            }
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_RenderLevel");
        }
    }

    void AgentTranscriptOverlay::_ApplyLevelRender(int level, uint64_t generation, LevelRender&& render)
    {
        if (generation != _generation || !_open || level < 0 || level >= static_cast<int>(_levels.size()))
        {
            return; // retargeted, refreshed or closed since this render started: it describes nothing on screen
        }
        auto& view = _levels[level];
        if (view.state != LevelState::Loading)
        {
            return;
        }
        view.markdown = std::move(render.markdown);
        view.renderMs = render.elapsedMs;
        view.lines = render.lines;
        view.truncated = render.truncated;
        view.failed = render.failed;
        const std::wstring head = L"[transcript] " + ::Agentmaster::ShortId(_source.sessionId) + L" show " +
                                  ::Agentmaster::TurnsLevelName(static_cast<::Agentmaster::TurnsLevel>(level)) + L": ";
        if (render.blocks.empty())
        {
            view.state = LevelState::Empty;
            _SetLevelMessage(level, render.message.empty() ? std::wstring{ L"Nothing to show at this level." } : render.message);
            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"nothing to show (" + view.message + L")" +
                                                            (render.path.empty() ? std::wstring{} : L" path=" + render.path) + L"\n");
        }
        else
        {
            view.blocks = std::move(render.blocks);
            view.built = 0;
            view.areas = std::move(render.areas);
            view.nextArea = 0;
            view.areaPanel = nullptr;
            view.areaEnd = 0;
            view.areaChunk = nullptr;
            view.areaChunkFill = 0;
            view.folds.clear();
            view.hasFolds = std::any_of(view.areas.begin(), view.areas.end(), [](const ::Agentmaster::MdToolArea& area) {
                return ::Agentmaster::MdToolAreaFoldable(area);
            });
            view.chain = std::move(render.chain);
            view.sessionHeadings.clear();
            view.pendingSessionJump = 0;
            view.state = LevelState::Building;
            // The placeholder gives way to content.
            if (view.messageText && view.content)
            {
                uint32_t index = 0;
                if (view.content.Children().IndexOf(view.messageText, index))
                {
                    view.content.Children().RemoveAt(index);
                }
                view.messageText = nullptr;
            }
            view.message.clear();
            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"chars=" + std::to_wstring(view.markdown.size()) + L" blocks=" +
                                                            std::to_wstring(view.blocks.size()) + (view.truncated ? L" (capped)" : L"") +
                                                            (view.failed ? L" (parse failed part-way)" : L"") + L" rendered in " +
                                                            std::to_wstring(view.renderMs) + L"ms" +
                                                            (render.forkSourceId.empty() ? std::wstring{} :
                                                                                           L" from the fork source " + ::Agentmaster::ShortId(render.forkSourceId) +
                                                                                               L"'s transcript (this fork has no messages yet)") +
                                                            L"\n");
            if (level == _current)
            {
                _SchedulePump();
            }
        }
        if (level == _current)
        {
            _UpdateStatus();
        }
    }

    void AgentTranscriptOverlay::_SchedulePump()
    {
        if (_pumpQueued || !_dispatcher)
        {
            return;
        }
        _pumpQueued = true;
        // LOW priority: input, layout and rendering all go first — the build only uses the idle time
        // between them, which is what keeps the window responsive while a long level lays out.
        const bool queued = _dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [weak = get_weak(), generation = _generation]() {
            try
            {
                if (const auto self = weak.get())
                {
                    self->_Pump(generation);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay pump item"); // the build stops; what is built stays
            }
        });
        if (!queued)
        {
            _pumpQueued = false;
        }
    }

    void AgentTranscriptOverlay::_Pump(uint64_t generation)
    {
        _pumpQueued = false;
        if (!_open || generation != _generation)
        {
            return;
        }
        auto& view = _levels[_current];
        if (view.state != LevelState::Building)
        {
            return;
        }
        try
        {
            const auto sliceStart = std::chrono::steady_clock::now();
            size_t madeThisSlice = 0;
            while (view.built < view.blocks.size())
            {
                _BuildNext(view); // one block (plus a fold row before an area's first one), or an empty area's note
                ++madeThisSlice;
                if (madeThisSlice >= kMinBlocksPerSlice && std::chrono::steady_clock::now() - sliceStart >= kSliceBudget)
                {
                    break; // yield: the next slice is queued below
                }
            }
            // A chain link clicked before its heading was built: this slice may have built it — jump now
            // (BEFORE _FinishLevel below, which drops a jump whose heading never came).
            if (const size_t pending = view.pendingSessionJump;
                pending > 0 && pending < view.sessionHeadings.size() && view.sessionHeadings[pending])
            {
                view.pendingSessionJump = 0;
                _ScrollToSessionHeading(view, pending);
            }
            if (view.built >= view.blocks.size())
            {
                _FinishLevel(view);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: stop building this level (what is built stays) and say so.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_Pump");
            view.failed = true;
            try
            {
                _FinishLevel(view);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_Pump finish");
                view.state = LevelState::Done;
            }
        }
        _UpdateStatus();
        if (view.state == LevelState::Building)
        {
            _SchedulePump();
        }
    }

    void AgentTranscriptOverlay::_FinishLevel(LevelView& view)
    {
        // Done FIRST: if the note below throws, the level must still stop building.
        view.state = LevelState::Done;
        std::vector<::Agentmaster::MdBlock>{}.swap(view.blocks); // the XAML holds the content now
        view.built = 0;
        // The area walk is over too: drop its state (a level that ENDS on a tool list still holds that
        // fold's panel here). The folds themselves stay in view.folds for "Expand all".
        std::vector<::Agentmaster::MdToolArea>{}.swap(view.areas);
        view.nextArea = 0;
        view.areaPanel = nullptr;
        view.areaEnd = 0;
        view.areaChunk = nullptr;
        view.areaChunkFill = 0;
        // The chain was only needed to build the links and to know which heading to record; the links and
        // view.sessionHeadings stay. A jump still pending targets a heading the build never reached (cut by
        // the block cap): drop it rather than keep a target that can never exist.
        view.chain = ::Agentmaster::MdSessionChain{};
        view.pendingSessionJump = 0;
        if (view.truncated || view.failed)
        {
            auto note = _MakeTextBlock(kStatusFontSize);
            note.Foreground(_dimBrush);
            note.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            note.Margin(Thickness{ 0, 16, 0, 0 });
            note.Text(view.failed ? winrt::hstring{ L"The rest of this level could not be rendered (the details are in hooks.log). The header's Copy button still copies the whole text." } :
                                    winrt::hstring{ L"The view stops here \x2014 this level is too long to show in full. The header's Copy button copies the whole text." });
            _AppendElement(view, note);
        }
    }

    void AgentTranscriptOverlay::_AppendElement(LevelView& view, const UIElement& element)
    {
        if (!view.chunk || view.chunkFill >= kChunkSize)
        {
            view.chunk = StackPanel{};
            view.content.Children().Append(view.chunk);
            view.chunkFill = 0;
        }
        view.chunk.Children().Append(element);
        ++view.chunkFill;
    }

    void AgentTranscriptOverlay::_AppendToFold(LevelView& view, const UIElement& element)
    {
        if (!view.areaChunk || view.areaChunkFill >= kChunkSize)
        {
            view.areaChunk = StackPanel{};
            view.areaPanel.Children().Append(view.areaChunk);
            view.areaChunkFill = 0;
        }
        view.areaChunk.Children().Append(element);
        ++view.areaChunkFill;
    }

    void AgentTranscriptOverlay::_BuildNext(LevelView& view)
    {
        const size_t index = view.built;
        // Past the folded area being filled: the blocks after it (the reply quote, the next turn) go back
        // to the level's chunks.
        if (view.areaPanel && index >= view.areaEnd)
        {
            view.areaPanel = nullptr;
            view.areaEnd = 0;
            view.areaChunk = nullptr;
            view.areaChunkFill = 0;
        }
        // A belt: skip any area the walk has passed (areas are found on these very blocks, in order and
        // without overlaps, so this never fires — but a stale index must never stall the build).
        while (view.nextArea < view.areas.size() && view.areas[view.nextArea].first < index)
        {
            ++view.nextArea;
        }
        const ::Agentmaster::MdBlock* above = index > 0 ? &view.blocks[index - 1] : nullptr;
        bool firstInFold = false;
        if (!view.areaPanel && view.nextArea < view.areas.size() && view.areas[view.nextArea].first == index)
        {
            const auto& area = view.areas[view.nextArea];
            ++view.nextArea;
            if (area.empty)
            {
                // "(no tool calls)": the note says the same in one dim line, lined up with the fold rows —
                // the bullet item itself is not built.
                _AppendElement(view, _BuildEmptyAreaNote(view.blocks[index], above));
                view.built = std::max(area.end, index + 1);
                return;
            }
            if (::Agentmaster::MdToolAreaFoldable(area))
            {
                view.areaPanel = _BuildFold(view, area, view.blocks[index], above);
                view.areaEnd = area.end;
                view.areaChunk = nullptr; // the new fold's first append opens its first chunk
                view.areaChunkFill = 0;
                firstInFold = true;
            }
            // Otherwise (Compact's own content: answers, compactions, an agent's messages) the list is
            // built open, exactly as before folding existed.
        }
        // A row of the Session chain list: its "session N" mentions become links — only those whose session
        // has a heading in this level (a one-session chain has none, so nothing there links). Only the
        // list's few blocks pay for the scan.
        const bool hasChain = view.chain.end > view.chain.first;
        std::vector<::Agentmaster::MdSessionRef> sessionRefs;
        if (hasChain && index >= view.chain.first && index < view.chain.end)
        {
            for (const auto& ref : ::Agentmaster::MdFindSessionRefs(view.blocks[index]))
            {
                if (ref.session < view.chain.headings.size() && view.chain.headings[ref.session] != ::Agentmaster::kMdNoBlock)
                {
                    sessionRefs.push_back(ref);
                }
            }
        }
        // The first block inside a fold starts its panel flush: the summary row above took the gap.
        const auto element = _BuildBlockElement(view, view.blocks[index], firstInFold ? nullptr : above, sessionRefs.empty() ? nullptr : &sessionRefs);
        if (view.areaPanel)
        {
            _AppendToFold(view, element);
        }
        else
        {
            _AppendElement(view, element);
        }
        // A session's heading — the very block the chain says its links point to (a session's FIRST
        // heading): record the element, so a click scrolls to it.
        if (hasChain)
        {
            const size_t session = ::Agentmaster::MdSessionHeadingNumber(view.blocks[index]);
            if (session > 0 && session < view.chain.headings.size() && view.chain.headings[session] == index)
            {
                if (view.sessionHeadings.size() <= session)
                {
                    view.sessionHeadings.resize(session + 1, winrt::Windows::UI::Xaml::FrameworkElement{ nullptr });
                }
                view.sessionHeadings[session] = element;
            }
        }
        ++view.built;
    }

    StackPanel AgentTranscriptOverlay::_BuildFold(LevelView& view, const ::Agentmaster::MdToolArea& area, const ::Agentmaster::MdBlock& firstBlock, const ::Agentmaster::MdBlock* above)
    {
        const bool open = view.expandAll;
        std::wstring summary = ::Agentmaster::MdToolAreaSummary(area);
        if (summary.empty())
        {
            summary = L"Tool calls"; // the summary failed (already logged): still a row that opens the list
        }

        // The panel the area's blocks are appended to — hidden while folded. A hair of air above its first
        // item so an opened list does not touch the row.
        StackPanel panel;
        panel.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        panel.Margin(Thickness{ 0, 3, 0, 0 });

        // The row: [chevron | summary], the summary wrapping in its own column on a narrow window. Neither
        // child takes hits — the button must be the only target (the badge's mkIconBtn lesson).
        FontIcon chevron;
        chevron.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        chevron.Glyph(open ? kFoldOpenGlyph : kFoldClosedGlyph);
        chevron.FontSize(9);
        chevron.Width(kFoldChevronWidth);
        chevron.Foreground(_dimBrush);
        chevron.VerticalAlignment(VerticalAlignment::Top);
        chevron.Margin(Thickness{ 0, 4, kFoldChevronGap, 0 }); // centered on the summary's FIRST line
        chevron.IsHitTestVisible(false);
        Grid::SetColumn(chevron, 0);

        TextBlock label;
        label.Text(winrt::hstring{ summary });
        label.FontSize(kFoldFontSize);
        label.Foreground(_dimBrush);
        label.TextWrapping(TextWrapping::Wrap);
        label.IsHitTestVisible(false);
        Grid::SetColumn(label, 1);

        Grid content;
        ColumnDefinition chevronColumn;
        chevronColumn.Width(GridLength{ 0, GridUnitType::Auto });
        ColumnDefinition labelColumn;
        labelColumn.Width(GridLength{ 1, GridUnitType::Star });
        content.ColumnDefinitions().Append(chevronColumn);
        content.ColumnDefinitions().Append(labelColumn);
        content.Children().Append(chevron);
        content.Children().Append(label);

        Button row;
        row.Background(_transparentBrush); // transparent (not null): hit-testable, and the template still shows hover
        row.BorderThickness(Thickness{ 0, 0, 0, 0 });
        row.Padding(Thickness{ kFoldRowPaddingLeft, kFoldRowPaddingTop, 8, kFoldRowPaddingTop });
        row.MinWidth(0);
        row.MinHeight(0);
        row.HorizontalAlignment(HorizontalAlignment::Left);
        row.HorizontalContentAlignment(HorizontalAlignment::Left);
        row.Content(content);
        // The gap the area's first block would have had under its header sits above the row instead.
        row.Margin(Thickness{ 0, ::Agentmaster::MdGapAbove(above, firstBlock), 0, 0 });
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(row, winrt::hstring{ L"Tool calls: " + summary });
        // WEAK references: the handler lives on the row, and the panel / chevron must never be kept alive
        // by a handler past the level's release (_ReleaseLevels drops the whole view).
        row.Click([weakPanel = winrt::make_weak(panel), weakChevron = winrt::make_weak(chevron)](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                const auto foldPanel = weakPanel.get();
                const auto foldChevron = weakChevron.get();
                if (foldPanel && foldChevron)
                {
                    _SetFoldOpen(foldPanel, foldChevron, foldPanel.Visibility() != Visibility::Visible);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay fold toggle"); // the fold stays as it was
            }
        });

        _AppendElement(view, row);
        _AppendElement(view, panel);
        view.folds.push_back(FoldParts{ panel, chevron });
        return panel;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildEmptyAreaNote(const ::Agentmaster::MdBlock& firstBlock, const ::Agentmaster::MdBlock* above)
    {
        TextBlock note;
        note.Text(winrt::hstring{ EmptyAreaNoteText(firstBlock) });
        note.FontSize(kFoldFontSize);
        note.Foreground(_dimBrush);
        note.TextWrapping(TextWrapping::Wrap);
        // Where a fold row's TEXT starts (past its padding + chevron), and as far down as that text sits.
        note.Margin(Thickness{ kFoldTextIndent, ::Agentmaster::MdGapAbove(above, firstBlock) + kFoldRowPaddingTop, 0, kFoldRowPaddingTop });
        return note;
    }

    void AgentTranscriptOverlay::_SetFoldOpen(const StackPanel& panel, const FontIcon& chevron, bool open)
    {
        panel.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        chevron.Glyph(open ? kFoldOpenGlyph : kFoldClosedGlyph);
    }

    void AgentTranscriptOverlay::_ToggleExpandAll()
    {
        auto& view = _levels[_current];
        view.expandAll = !view.expandAll;
        for (const auto& fold : view.folds)
        {
            _SetFoldOpen(fold.panel, fold.chevron, view.expandAll);
        }
        _UpdateExpandAllButton();
    }

    void AgentTranscriptOverlay::_UpdateExpandAllButton()
    {
        if (!_expandAllButton || !_expandAllLabel)
        {
            return;
        }
        const auto& view = _levels[_current];
        _expandAllButton.Visibility(view.hasFolds ? Visibility::Visible : Visibility::Collapsed);
        const winrt::hstring label{ view.expandAll ? L"Collapse all" : L"Expand all" };
        _expandAllLabel.Text(label);
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_expandAllButton, label);
    }

    void AgentTranscriptOverlay::_ToggleWrapLines()
    {
        _wrapLines = !_wrapLines;
        // The level on screen re-lays out now, keeping the reader on the same passage; a hidden level
        // catches up when it is next shown (_SelectLevel). Neither step throws.
        _SyncWrapMode(_levels[_current]);
        // Every window's next open uses it (Show reads it back). Also noexcept.
        _RememberWrapLines(_wrapLines);
        // Last: the only step that can throw, so a failed repaint never costs the switch itself.
        _UpdateWrapButton();
    }

    void AgentTranscriptOverlay::_UpdateWrapButton()
    {
        if (!_wrapButton || !_wrapButtonFace || !_wrapGlyph)
        {
            return;
        }
        _wrapButtonFace.Background(_wrapLines ? _toggleOnBrush : _transparentBrush);
        _wrapButtonFace.BorderBrush(_wrapLines ? _toggleOnBorderBrush : _transparentBrush);
        _wrapGlyph.Foreground(_wrapLines ? _bodyBrush : _dimBrush);
        // Re-setting a tip only updates its text (and the open tip's content, when this toggle's tip is the
        // one showing) — AgentSetTip wires its pointer handlers once per element.
        AgentSetTip(_wrapButton, winrt::hstring{ _wrapLines ? kWrapOnTip : kWrapOffTip });
        // A Button has no toggle state for a screen reader, so the name carries it.
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_wrapButton, winrt::hstring{ _wrapLines ? L"Wrap long lines: on" : L"Wrap long lines: off" });
    }

    void AgentTranscriptOverlay::_SyncWrapMode(LevelView& view) noexcept
    {
        try
        {
            if (!view.scroll || view.wrapLines == _wrapLines)
            {
                return;
            }
            // The anchor FIRST: the scroll offset belongs to the layout the view has NOW — for a level that
            // was hidden, the one it had when it was last shown (a collapsed subtree is not re-measured, so
            // its DesiredSizes and its offset still agree).
            const auto anchor = FindScrollAnchor(view.scroll, view.content);
            ApplyWrapMode(view.scroll, _wrapLines);
            view.wrapLines = _wrapLines; // after the apply: a throw above leaves the old value, so the next select retries
            if (!anchor.path.empty())
            {
                // Lay the view out NOW under the new width rule, so the anchor's new place — and the extent
                // ChangeView clamps to — is real (the board's UpdateLayout-then-ChangeView recipe). Legal
                // here: every caller is an input handler or a menu action, never a layout pass. On a long
                // Max level this re-measures every block at once — the same work the next frame would do.
                view.scroll.UpdateLayout();
                RestoreScrollAnchor(view.scroll, view.content, anchor);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the view keeps the mode it reached (view.wrapLines says which) and its raw,
            // clamped scroll offset.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay sync wrap mode");
        }
    }

    bool AgentTranscriptOverlay::_RememberedWrapLines() noexcept
    {
        try
        {
            return ::Agentmaster::LoadAppSettings().transcriptOverlayWrapLines;
        }
        catch (...)
        {
            // Rule #18. Recovery: wrap (the default) — a failed settings read never blocks the overlay.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remembered wrap");
        }
        return true;
    }

    void AgentTranscriptOverlay::_RememberWrapLines(bool wrap) noexcept
    {
        try
        {
            // FRESHEST disk, one field (the ColorPickupFlyout idiom, as _RememberLevel): never write a stale
            // in-memory copy over a cog Save or another window's settings change.
            auto settings = ::Agentmaster::LoadAppSettings();
            if (settings.transcriptOverlayWrapLines != wrap)
            {
                settings.transcriptOverlayWrapLines = wrap;
                ::Agentmaster::SaveAppSettings(settings);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing — the view already switched; only the remembered state is lost.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remember wrap");
        }
    }

    TextBlock AgentTranscriptOverlay::_MakeTextBlock(double fontSize) const
    {
        TextBlock text;
        text.FontSize(fontSize);
        text.Foreground(_bodyBrush);
        text.TextWrapping(TextWrapping::Wrap);
        text.IsTextSelectionEnabled(true);
        // The WT text menu (Copy / Select All), never the default: the default text flyout exists ONCE
        // PER THREAD in WinUI 2, and with several windows on this one thread it misbehaves or crashes
        // once the window that first opened it is gone (GH#18599 — why WT made TextMenuFlyout).
        text.ContextFlyout(_textMenu);
        // The touch/pen selection flyout is that same per-thread default: off.
        text.SelectionFlyout(nullptr);
        return text;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildBlockContent(LevelView& view, const ::Agentmaster::MdBlock& block, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs)
    {
        switch (block.kind)
        {
        case ::Agentmaster::MdBlockKind::Rule:
        {
            Border rule;
            rule.Height(1);
            rule.Background(_ruleBrush);
            rule.Margin(Thickness{ 0, 4, 0, 4 });
            return rule;
        }
        case ::Agentmaster::MdBlockKind::Code:
        {
            Border box;
            box.Background(_codeBackBrush);
            box.BorderBrush(_codeBorderBrush);
            box.BorderThickness(Thickness{ 1, 1, 1, 1 });
            box.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            box.Padding(Thickness{ 10, 7, 10, 7 });
            auto text = _MakeTextBlock(kCodeFontSize);
            text.FontFamily(_monoFont);
            text.Foreground(_codeTextBrush);
            text.Text(block.truncated ? winrt::hstring{ block.code + kCutNote } : winrt::hstring{ block.code });
            box.Child(text);
            return box;
        }
        case ::Agentmaster::MdBlockKind::Heading:
        case ::Agentmaster::MdBlockKind::Paragraph:
        default:
            break;
        }

        const bool heading = block.kind == ::Agentmaster::MdBlockKind::Heading;
        auto text = _MakeTextBlock(heading ? HeadingFontSize(block.headingLevel) : kBodyFontSize);
        // A LABEL paragraph's first line is tinted with its speaker's color, and the lines under it (the
        // Full / Max stats line folded into the same paragraph) are dimmed — set per run below.
        Brush labelBrush{ nullptr };
        if (heading)
        {
            text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            view.speakerBar = nullptr; // a heading starts a new turn / section: no speaker yet
        }
        else if (block.rawHtml)
        {
            // Raw HTML kept as text (MarkdownBlocks.h): monospace + dim, so markup reads as markup.
            text.FontFamily(_monoFont);
            text.FontSize(kCodeFontSize);
            text.Foreground(_dimBrush);
        }
        else if (::Agentmaster::MdIsLabelParagraph(block))
        {
            // A speaker label ("**❯ User**", "**● Claude**", "**✉ finder-a**"): tint it, and remember the
            // speaker so the quote that follows (their message / reply) wears a matching bar.
            switch (LabelGlyph(block))
            {
            case L'\x276F':
                labelBrush = _userBrush;
                view.speakerBar = _userBarBrush;
                break;
            case L'\x25CF':
                labelBrush = _claudeBrush;
                view.speakerBar = _claudeBarBrush;
                break;
            case L'\x2709':
                labelBrush = _peerBrush;
                view.speakerBar = _peerBarBrush;
                break;
            default:
                break;
            }
        }
        else if (AllItalic(block))
        {
            text.Foreground(_dimBrush); // notes / stats lines: secondary
        }

        bool inFirstLine = true; // for a label paragraph: still on the label line
        for (size_t runIndex = 0; runIndex < block.runs.size(); ++runIndex) // indexed: a session link names its run by index
        {
            const auto& source = block.runs[runIndex];
            if (source.text.empty())
            {
                continue;
            }
            // A Session chain row: a run holding "session N" goes in as plain pieces around hyperlinks
            // (_AppendSessionLinks). Such rows are never label paragraphs, so the label tint is not involved;
            // the first-line flag is kept honest anyway.
            if (sessionRefs && _AppendSessionLinks(text, source, runIndex, *sessionRefs))
            {
                if (source.text.find_first_of(L"\r\n") != std::wstring::npos)
                {
                    inFirstLine = false;
                }
                continue;
            }
            Run run;
            run.Text(winrt::hstring{ source.text });
            if (labelBrush && (source.style & (::Agentmaster::kMdCode | ::Agentmaster::kMdLink)) == 0)
            {
                run.Foreground(inFirstLine ? labelBrush : Brush{ _dimBrush });
            }
            if (source.text.find_first_of(L"\r\n") != std::wstring::npos)
            {
                inFirstLine = false; // the label line ended inside this run; everything after is "the rest"
            }
            if (source.style & ::Agentmaster::kMdBold)
            {
                run.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            if (source.style & ::Agentmaster::kMdItalic)
            {
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            }
            if (source.style & ::Agentmaster::kMdCode)
            {
                run.FontFamily(_monoFont);
                if (!block.rawHtml)
                {
                    run.Foreground(_inlineCodeBrush);
                }
            }
            if (source.style & ::Agentmaster::kMdLink)
            {
                winrt::Windows::Foundation::Uri uri{ nullptr };
                if (IsWebUrl(source.url))
                {
                    try
                    {
                        uri = winrt::Windows::Foundation::Uri{ winrt::hstring{ source.url } };
                    }
                    catch (...)
                    {
                        // A malformed URL in a quoted message: logged (throttled), rendered as plain link text.
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay link uri");
                    }
                }
                if (uri)
                {
                    Hyperlink link;
                    link.NavigateUri(uri);
                    link.Foreground(_linkBrush);
                    link.Inlines().Append(run);
                    text.Inlines().Append(link);
                    continue;
                }
                if ((source.style & ::Agentmaster::kMdCode) == 0)
                {
                    run.Foreground(_linkBrush);
                }
            }
            text.Inlines().Append(run);
        }
        if (block.truncated)
        {
            Run note;
            note.Text(kCutNote);
            note.Foreground(_dimBrush);
            note.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            text.Inlines().Append(note);
        }
        return text;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildBlockElement(LevelView& view, const ::Agentmaster::MdBlock& block, const ::Agentmaster::MdBlock* prev, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs)
    {
        FrameworkElement element = _BuildBlockContent(view, block, sessionRefs);
        const double gap = ::Agentmaster::MdGapAbove(prev, block);
        // The gap goes INSIDE the containers this block shares with the one above it: a quote bar they
        // share then runs unbroken through the gap, while a quote that STARTS here begins below it.
        const size_t shared = prev ? ::Agentmaster::MdSharedContainerDepth(*prev, block) : 0;
        const size_t depth = block.containers.size();
        if (shared >= depth)
        {
            AddMargin(element, 0, gap);
        }
        // Wrap from the innermost container out. Item indents with no marker on this block are not
        // wrappers — they fold into a left margin (`pendingIndent`) until the next real wrapper (a marker
        // column or a quote bar) or the outermost level takes them: a continuation line costs no Grid.
        double pendingIndent = 0.0;
        size_t quoteOrdinal = 0; // quotes outside position i, counted below (outermost == 0)
        for (size_t i = 0; i < depth; ++i)
        {
            if (block.containers[i].kind == ::Agentmaster::MdContainerKind::Quote)
            {
                ++quoteOrdinal;
            }
        }
        for (size_t i = depth; i-- > 0;)
        {
            const auto& container = block.containers[i];
            if (container.kind == ::Agentmaster::MdContainerKind::Item)
            {
                const double width = MarkerColumnWidth(container);
                if (container.marker.empty())
                {
                    pendingIndent += width;
                }
                else
                {
                    // This block is the item's FIRST: a [marker | content] grid. The marker can't sit
                    // in a margin — it needs its own column so wrapped lines hang under the text, not it.
                    if (pendingIndent > 0)
                    {
                        AddMargin(element, pendingIndent, 0);
                        pendingIndent = 0;
                    }
                    Grid row;
                    ColumnDefinition markerColumn;
                    markerColumn.Width(GridLength{ width, GridUnitType::Pixel });
                    ColumnDefinition contentColumn;
                    contentColumn.Width(GridLength{ 1, GridUnitType::Star });
                    row.ColumnDefinitions().Append(markerColumn);
                    row.ColumnDefinitions().Append(contentColumn);
                    TextBlock marker;
                    marker.Text(winrt::hstring{ container.marker });
                    marker.FontSize(kBodyFontSize);
                    marker.Foreground(_markerBrush);
                    marker.VerticalAlignment(VerticalAlignment::Top);
                    // A code panel's text starts below its padding: line the marker up with that text.
                    marker.Margin(Thickness{ 0, block.kind == ::Agentmaster::MdBlockKind::Code ? 7.0 : 0.0, 0, 0 });
                    Grid::SetColumn(marker, 0);
                    row.Children().Append(marker);
                    Grid::SetColumn(element, 1);
                    row.Children().Append(element);
                    element = row;
                }
            }
            else
            {
                --quoteOrdinal; // now the ordinal of THIS quote
                if (pendingIndent > 0)
                {
                    AddMargin(element, pendingIndent, 0);
                    pendingIndent = 0;
                }
                Border bar;
                bar.BorderThickness(Thickness{ kQuoteBarWidth, 0, 0, 0 });
                // The outermost quote wears the current speaker's color (their message / reply); quotes
                // nested inside a message stay neutral.
                bar.BorderBrush(quoteOrdinal == 0 && view.speakerBar ? view.speakerBar : Brush{ _quoteBarBrush });
                bar.Padding(Thickness{ kQuotePadding, 0, 0, 0 });
                bar.Child(element);
                element = bar;
            }
            if (i == shared)
            {
                AddMargin(element, 0, gap);
            }
        }
        if (pendingIndent > 0)
        {
            AddMargin(element, pendingIndent, 0);
        }
        return element;
    }

    bool AgentTranscriptOverlay::_AppendSessionLinks(const TextBlock& text, const ::Agentmaster::MdRun& source, size_t runIndex, const std::vector<::Agentmaster::MdSessionRef>& refs)
    {
        // One plain piece of the run, in the run's own bold / italic style (a mention never sits in a code or
        // link run — MdFindSessionRefs skips those).
        const auto makeRun = [&source](std::wstring_view piece) {
            Run run;
            run.Text(winrt::hstring{ piece });
            if (source.style & ::Agentmaster::kMdBold)
            {
                run.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            if (source.style & ::Agentmaster::kMdItalic)
            {
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            }
            return run;
        };
        const std::wstring_view whole{ source.text };
        bool any = false;
        size_t at = 0; // the first unit of the run not appended yet
        for (const auto& ref : refs)
        {
            // Another run's mention — or a range that would overlap / overrun this one (refs come in text
            // order from this very block, so never; the check keeps a bad range from slicing past the end).
            if (ref.run != runIndex || ref.offset < at || ref.length > whole.size() || ref.offset > whole.size() - ref.length)
            {
                continue;
            }
            any = true;
            if (ref.offset > at)
            {
                text.Inlines().Append(makeRun(whole.substr(at, ref.offset - at)));
            }
            Hyperlink link;
            link.Foreground(_linkBrush);
            link.Inlines().Append(makeRun(whole.substr(ref.offset, ref.length)));
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(link, winrt::hstring{ L"Go to session " + std::to_wstring(ref.session) });
            // No NavigateUri: Click is the whole behavior. A WEAK reference — the handler lives on the link,
            // and the overlay must never be kept alive by its own XAML.
            link.Click([weak = get_weak(), session = ref.session](const Hyperlink&, const HyperlinkClickEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_JumpToSession(session);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay session link"); // the view stays where it was
                }
            });
            text.Inlines().Append(link);
            at = ref.offset + ref.length;
        }
        if (!any)
        {
            return false;
        }
        if (at < whole.size())
        {
            text.Inlines().Append(makeRun(whole.substr(at)));
        }
        return true;
    }

    void AgentTranscriptOverlay::_JumpToSession(size_t session)
    {
        auto& view = _levels[_current];
        if (session < view.sessionHeadings.size() && view.sessionHeadings[session])
        {
            view.pendingSessionJump = 0; // a direct jump supersedes any earlier click still waiting
            _ScrollToSessionHeading(view, session);
            return;
        }
        // The heading is further down than the time-sliced build has reached: jump the moment it is built
        // (_Pump). A level that is not building can't build it any more (the block cap cut it): nothing to do.
        if (view.state == LevelState::Building)
        {
            view.pendingSessionJump = session;
        }
    }

    void AgentTranscriptOverlay::_ScrollToSessionHeading(LevelView& view, size_t session) noexcept
    {
        try
        {
            if (!view.scroll || !view.content || session >= view.sessionHeadings.size() || !view.sessionHeadings[session])
            {
                return;
            }
            const auto& heading = view.sessionHeadings[session];
            // Lay the view out first: a heading built in this very slice has no position yet, and ChangeView
            // clamps to the current extent (the board's UpdateLayout-then-ChangeView recipe). Legal here — the
            // callers are a link's Click and a build slice, never a layout pass.
            view.scroll.UpdateLayout();
            // The heading's top in the content's coordinates == the scroll offset that puts it at the top edge
            // (a session heading is a top-level block, never inside a collapsed fold, so it always has a
            // position). TransformToVisual is right HERE, unlike in FindScrollAnchor: this is the level on
            // screen, laid out the line above.
            const auto top = heading.TransformToVisual(view.content).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            view.scroll.ChangeView(nullptr, std::max(0.0, static_cast<double>(top.Y) - kSessionJumpGap), nullptr, true /* disableAnimation: an anchor jump, not a glide */);
            // Focus to the scroll view: the clicked link, left focused but scrolled away, would be brought back
            // into view the next time focus returns to it (BringIntoViewOnFocusChange — e.g. on a window
            // activation).
            view.scroll.Focus(FocusState::Programmatic);
        }
        catch (...)
        {
            // Rule #18. Recovery: the view stays where it was. Contained HERE, not left to the callers: a
            // pending jump runs inside a build slice (_Pump), whose catch would stop building the level — a
            // failed scroll must never cost the rest of the transcript.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay session jump");
        }
    }

    void AgentTranscriptOverlay::_UpdateStatus()
    {
        // The "Expand all" switch follows the same moments the status does: a level switch, a render
        // landing, a build slice.
        _UpdateExpandAllButton();
        if (!_statusText)
        {
            return;
        }
        const auto& view = _levels[_current];
        std::wstring status;
        switch (view.state)
        {
        case LevelState::Loading:
            status = L"Rendering\x2026";
            break;
        case LevelState::Building:
            status = L"Laying out " + WithThousands(view.built) + L" of " + WithThousands(view.blocks.size()) + L" blocks\x2026";
            break;
        case LevelState::Done:
            status = WithThousands(view.lines) + L" lines \x00B7 rendered in " + FormatDurationMs(view.renderMs);
            if (view.truncated)
            {
                status += L" \x00B7 the view is cut short";
            }
            if (view.failed)
            {
                status += L" \x00B7 incomplete";
            }
            break;
        case LevelState::Idle:
        case LevelState::Empty:
        default:
            break;
        }
        _statusText.Text(winrt::hstring{ status });
    }

    void AgentTranscriptOverlay::_UpdateTitle()
    {
        if (!_subtitleText)
        {
            return;
        }
        std::wstring line;
        if (!_source.title.empty())
        {
            line = _source.title + L"  \x00B7  ";
        }
        line += ::Agentmaster::ShortId(_source.sessionId);
        if (_source.codex)
        {
            line += L"  \x00B7  Codex";
        }
        _subtitleText.Text(winrt::hstring{ line });
    }

    void AgentTranscriptOverlay::_UpdateTabVisuals()
    {
        for (size_t i = 0; i < _tabLevels.size(); ++i)
        {
            const bool current = _tabLevels[i] == _current;
            // Opacity + underline only — no weight change, which would shift every tab to its right.
            _tabLabels[i].Opacity(current ? 1.0 : 0.62);
            _tabUnderlines[i].Background(current ? Brush{ _accentBrush } : Brush{ _transparentBrush });
        }
    }

    void AgentTranscriptOverlay::_ReleaseLevels() noexcept
    {
        try
        {
            if (_bodyHost)
            {
                _bodyHost.Children().Clear();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay release views");
        }
        for (auto& view : _levels)
        {
            view = LevelView{};
        }
    }

    void AgentTranscriptOverlay::_FocusBodyDeferred()
    {
        if (!_dispatcher)
        {
            return;
        }
        // Next tick: a scroll view created this tick has not had its first layout, and Focus on it
        // would silently do nothing.
        _dispatcher.TryEnqueue([weak = get_weak(), generation = _generation]() {
            const auto self = weak.get();
            if (!self || !self->_open || self->_generation != generation)
            {
                return;
            }
            try
            {
                if (const auto& scroll = self->_levels[self->_current].scroll)
                {
                    scroll.Focus(FocusState::Programmatic);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay focus body");
            }
        });
    }

    void AgentTranscriptOverlay::_CopyCurrentLevel()
    {
        const auto& view = _levels[_current];
        const auto levelName = ::Agentmaster::TurnsLevelName(static_cast<::Agentmaster::TurnsLevel>(_current));
        if (view.markdown.empty())
        {
            ::Agentmaster::LogNav(L"transcript-copy " + ::Agentmaster::ShortId(_source.sessionId) + L" " + levelName + L" (nothing rendered yet)");
            return;
        }
        if (RobustCopyTextToClipboard(view.markdown))
        {
            ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC); // the chime every Agentmaster copy plays
            ::Agentmaster::LogNav(L"transcript-copy " + ::Agentmaster::ShortId(_source.sessionId) + L" " + levelName + L" chars=" + std::to_wstring(view.markdown.size()));
        }
        else
        {
            ::Agentmaster::LogNav(L"transcript-copy " + ::Agentmaster::ShortId(_source.sessionId) + L" " + levelName + L" (the clipboard could not be opened)");
        }
    }

    void AgentTranscriptOverlay::_Refresh()
    {
        ::Agentmaster::LogNav(L"transcript-refresh " + ::Agentmaster::ShortId(_source.sessionId) + L" " +
                              ::Agentmaster::TurnsLevelName(static_cast<::Agentmaster::TurnsLevel>(_current)));
        ++_generation; // a render still running for the old views is dropped when it lands
        // A refresh re-renders the text, not the reader's choice: each level keeps its "Expand all".
        std::array<bool, static_cast<size_t>(::Agentmaster::kTurnsLevelCount)> expandAll{}; // == _levels.size() (static_assert above)
        for (size_t i = 0; i < _levels.size(); ++i)
        {
            expandAll[i] = _levels[i].expandAll;
        }
        _ReleaseLevels();
        for (size_t i = 0; i < _levels.size(); ++i)
        {
            _levels[i].expandAll = expandAll[i];
        }
        _SelectLevel(_current, true);
    }

    bool AgentTranscriptOverlay::_IsUnder(const DependencyObject& element, const DependencyObject& ancestor) noexcept
    {
        if (!element || !ancestor)
        {
            return false;
        }
        try
        {
            DependencyObject current = element;
            // A focused Hyperlink is a TextElement, not a visual: VisualTreeHelper can't walk it, so start
            // from the text control that hosts it.
            if (const auto textElement = current.try_as<TextElement>())
            {
                current = textElement.ElementStart().VisualParent();
            }
            for (int guard = 0; current && guard < 1024; ++guard)
            {
                if (current == ancestor)
                {
                    return true;
                }
                current = VisualTreeHelper::GetParent(current);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_IsUnder");
        }
        return false;
    }
}
