// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — a STAND-IN for ConversationImage.cpp, linked only when run-m5-tests.bat finds no vcpkg cmark (no full
// build ran on this machine). ConversationImage.cpp parses the turns view with MarkdownBlocks, which needs cmark, yet
// Messaging.Inbound.cpp (always in the suite) calls RenderConversationImages for the card's 📜 All Screen. Without this file
// the whole suite would fail to LINK for want of a library only the All Screen picture needs; with it the suite builds, and
// TestConversationImage reports its layout and end-to-end cases as skipped (they are compiled out without AM_HAVE_CMARK).
// The app's own build always compiles the real ConversationImage.cpp (TerminalAppLib.vcxproj).

#include "../ConversationImage.h"

namespace Agentmaster
{
    /// <summary>The stand-in's layout: an empty sheet. Nothing in the suite calls it without cmark (its tests are compiled out).</summary>
    /// <param name="blocks">Unused: there is no parser to have made them.</param>
    /// <param name="options">Unused.</param>
    /// <returns>An empty sheet.</returns>
    ConversationSheet BuildConversationSheet(const std::vector<MdBlock>& blocks, const ConversationSheetOptions& options)
    {
        (void)blocks;
        (void)options;
        return {};
    }

    /// <summary>The stand-in's pipeline: no pictures, and the reason (the suite was built without cmark).</summary>
    /// <param name="transcript">Unused.</param>
    /// <param name="options">Unused.</param>
    /// <returns>A result whose error names the missing library. Never throws.</returns>
    ConversationImageResult RenderConversationImages(const std::filesystem::path& transcript, const ConversationImageOptions& options) noexcept
    {
        (void)transcript;
        (void)options;
        ConversationImageResult result;
        result.error = L"this test build has no cmark, so the conversation cannot be parsed";
        return result;
    }
}
