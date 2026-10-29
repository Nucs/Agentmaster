// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — THE WINDOW NAME (AGENT_MANAGER.md "The window name"): the pure rules for a name typed into the Agent
// Manager page's header. The name is Windows Terminal's own window name (WindowProperties::WindowName), which a named
// window shows as its caption (TerminalPage::Title) — the text its taskbar preview, its Alt+Tab entry and its taskbar
// label read — and which its WindowRecord saves (`name`, windows/<id>.json).
//
// Plain C++, no WinRT: the m5 harness tests it (TestWindowRecord).

#pragma once

#include <string>
#include <string_view>

namespace Agentmaster
{
    /// <summary>
    /// The longest window name the header keeps, in UTF-16 code units. A caption is read in a taskbar preview (about 30
    /// characters before its ellipsis) and in Alt+Tab (about 40); 256 is far past either, so the cap only stops a paste
    /// of a whole document from becoming the window's title, its saved record and every log line about it.
    /// </summary>
    inline constexpr size_t kMaxWindowNameLength = 256;

    /// <summary>
    /// The window name Windows Terminal reserves for its quake (drop-down) window. Naming a window this turns it INTO
    /// the quake window — docked at the top of the screen, in focus mode, with no tab strip — so it is a mode switch,
    /// not a title: the caption never shows it (TerminalPage::Title) and the window record never saves it
    /// (TerminalPage::_CaptureWindowRecord), so a hidden drop-down window does not come back by itself at the next start.
    /// </summary>
    inline constexpr std::wstring_view kQuakeWindowName = L"_quake";

    namespace detail
    {
        /// <summary>
        /// Is `c` a character a caption cannot show on one line: a C0 control (CR, LF and TAB included), DEL, a C1
        /// control, or the Unicode line / paragraph separators? Each one becomes a space in a normalized name.
        /// </summary>
        /// <param name="c">The UTF-16 code unit.</param>
        /// <returns>True for a control or line-breaking character.</returns>
        inline constexpr bool IsWindowNameControl(wchar_t c) noexcept
        {
            return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029;
        }

        /// <summary>
        /// Is `c` blank at the ends of a name: a space, a no-break space, the typographic spaces U+2000..U+200A, the
        /// zero-width space, the ideographic space, or a byte-order mark? Leading and trailing ones are dropped, so a
        /// name that is ONLY such characters normalizes to "" (unnamed) instead of an invisible caption.
        /// </summary>
        /// <param name="c">The UTF-16 code unit.</param>
        /// <returns>True for a blank character.</returns>
        inline constexpr bool IsWindowNameBlank(wchar_t c) noexcept
        {
            return c == L' ' || c == 0x00A0 || (c >= 0x2000 && c <= 0x200B) || c == 0x3000 || c == 0xFEFF;
        }

        /// <summary>
        /// Drop the blank characters at both ends of `text`, in place.
        /// </summary>
        /// <param name="text">The text to trim.</param>
        /// <exception cref="std::bad_alloc">Never in practice: erase only shrinks.</exception>
        inline void TrimWindowNameBlanks(std::wstring& text)
        {
            size_t end = text.size();
            while (end > 0 && IsWindowNameBlank(text[end - 1]))
            {
                --end;
            }
            size_t begin = 0;
            while (begin < end && IsWindowNameBlank(text[begin]))
            {
                ++begin;
            }
            text.erase(end);
            text.erase(0, begin);
        }
    }

    /// <summary>
    /// The window name to apply for what the user typed into the Agent Manager page's header: one line (every control
    /// and line-breaking character becomes a space), without blanks at either end, at most kMaxWindowNameLength code
    /// units (never cut between the two halves of a surrogate pair). Inner spacing is the user's and is kept.
    /// </summary>
    /// <remarks>
    /// An EMPTY result is meaningful: it UNNAMES the window, so its caption follows the focused tab's title again. The
    /// window menu's renamer (Windows Terminal's own) applies its text without this, as upstream does; the caption and
    /// the record take whatever name the window holds.
    /// </remarks>
    /// <param name="raw">The typed text.</param>
    /// <returns>The normalized name; "" to unname the window.</returns>
    /// <exception cref="std::bad_alloc">When the copy cannot be allocated.</exception>
    inline std::wstring NormalizeWindowName(std::wstring_view raw)
    {
        std::wstring out;
        out.reserve(raw.size());
        for (const wchar_t c : raw)
        {
            out.push_back(detail::IsWindowNameControl(c) ? L' ' : c);
        }
        detail::TrimWindowNameBlanks(out);
        if (out.size() > kMaxWindowNameLength)
        {
            out.resize(kMaxWindowNameLength);
            // A high surrogate left last has lost its low half: drop it rather than end the caption on a broken character.
            if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF)
            {
                out.pop_back();
            }
            // The cut may land inside a run of spaces.
            detail::TrimWindowNameBlanks(out);
        }
        return out;
    }
}
