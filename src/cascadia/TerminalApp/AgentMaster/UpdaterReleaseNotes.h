// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the update prompt's RELEASE NOTES (doc/agentmaster/UPDATER.md "The prompt window").
//
// A PART of Updater.h, which includes it at its very end: include Updater.h, never this file (the guard below refuses, so a
// translation unit can never see these functions without the declarations they use). It holds everything about the notes
// that is not a window, so the m5 harness tests it whole:
//   - the release LIST: GitHub's /releases pages, kept to the releases on the user's update channel (the check's own rule,
//     ReleaseAllowedOnChannel), paged until the list reaches the installed version (FetchReleaseNotesList);
//   - the TIMELINE the version tabs show: oldest to newest, each release older than, equal to ("yours") or newer than the
//     installed version (an UPDATE the user gets), at most 30 tabs, every update kept first (BuildReleaseTimeline);
//   - each release's notes as HTML: cmark's CommonMark renderer in its SAFE mode, after the notes' "## Install" section is
//     cut, because the updater installs for the user (ReleaseNotesHtml);
//   - the PAGE the WebView2 shows: a fixed document whose script builds the tab bar from a JSON model the window posts to it
//     (BuildReleaseNotesPage, BuildReleaseNotesModelJson), and the PLAIN TEXT the window shows when WebView2 cannot run
//     (BuildReleaseNotesPlainText).
// The window itself (the dark dialog, the WebView2 host, the worker that reads the list) is UpdaterPromptWindow.h.

#pragma once

#ifndef AGENTMASTER_UPDATER_PARTS
#error "UpdaterReleaseNotes.h is a part of Updater.h: include Updater.h instead"
#endif

// cmark (vcpkg, the "terminal" feature the transcript overlay's MarkdownBlocks parses with) is on the include path of every
// msbuild project, and of the m5 harness exactly when the harness also links cmark.lib (run-m5-tests.bat, AM_HAVE_CMARK).
// So the test is the include itself: a build without cmark compiles the escaped-text fallback instead of failing, and the
// two never mix inside one module (every translation unit of a project sees the same include path).
#if __has_include(<cmark.h>)
// cmark's header otherwise defines short legacy macros (NODE_TEXT, BULLET_LIST, ...) that would rewrite any identifier of
// the same name in every translation unit that includes Updater.h.
#ifndef CMARK_NO_SHORT_NAMES
#define CMARK_NO_SHORT_NAMES
#endif
#include <cmark.h>
#define AGENTMASTER_UPDATER_HAVE_CMARK 1
#else
#define AGENTMASTER_UPDATER_HAVE_CMARK 0
#endif

#include <objbase.h> // CoCreateGuid: the page's script nonce

namespace Agentmaster::Updater
{
    // ============================ limits ============================

    // The most version tabs the notes show: GitHub's first page, the scope chosen for the tab bar. Every update is kept before
    // any older release (BuildReleaseTimeline), so the cap only ever drops the oldest context, never a release the user gets.
    inline constexpr size_t kReleaseNotesMaxTabs = 30;
    // Releases per GitHub list page: the API's default. A bigger page costs one long download (~18 KB per release with its
    // notes and assets) where the usual case — one or two updates — needs the first page alone.
    inline constexpr int kReleaseNotesPageSize = 30;
    // The most pages one fetch reads. A user far behind (or a channel crowded with nightlies) still gets the updates those
    // pages hold, and a broken or hostile API can never keep the worker paging forever.
    inline constexpr int kReleaseNotesMaxPages = 3;
    // Each WinHTTP phase's timeout for one list page. The fetch runs on a worker while the prompt is already showing, so it
    // never delays the prompt; it only bounds how long the "Loading the other releases" line can last.
    inline constexpr DWORD kReleaseNotesPageTimeoutMs = 8000;
    // A release's notes longer than this (UTF-16 units) are cut at a line end, with a pointer to the release page: one
    // runaway body must not make the page's model message — and the plain-text box — megabytes long.
    inline constexpr size_t kReleaseNotesMaxMarkdownChars = 200000;

    // ============================ the model ============================

    // What kind of release a tag is: its tab's badge and its words ("stable" / "pre-release" / "nightly").
    enum class ReleaseKind : uint8_t
    {
        Stable,
        Prerelease,
        Nightly,
    };

    // Where a release stands against the installed version. The tab bar dims an Older release, points the "you" arrow at
    // Yours, and colors every Update — the releases the user gets by updating.
    enum class ReleaseRole : uint8_t
    {
        Older,
        Yours,
        Update,
    };

    // One release as the notes show it.
    struct ReleaseNote
    {
        std::wstring tag; // "v0.8.1": the identity — the timeline keeps one release per tag
        std::wstring name; // the release title ("Agentmaster 0.8.1"); "" for the offered release before the list arrives
        Version version; // parsed from the tag (ParseVersion): the order of the tabs and the role
        ReleaseKind kind{ ReleaseKind::Stable };
        std::wstring publishedAt; // GitHub's published_at, ISO 8601 UTC ("2026-10-29T05:37:58Z"); "" when unknown
        std::wstring htmlUrl; // the release page; always one of OUR release pages (see ParseReleaseListPage)
        std::wstring body; // the notes, Markdown as published
    };

    // The kind of a release from its tag and GitHub's prerelease flag: a nightly by its TAG (IsNightlyTag, the same rule the
    // channel gate uses, so a nightly published without the prerelease checkbox still reads as one), any other prerelease
    // by the flag.
    //   tag        — the release tag.
    //   prerelease — GitHub's "prerelease" flag.
    //   returns    — the kind.
    //   throws     — nothing.
    inline ReleaseKind ReleaseKindOf(std::wstring_view tag, bool prerelease) noexcept
    {
        if (IsNightlyTag(tag))
        {
            return ReleaseKind::Nightly;
        }
        return prerelease ? ReleaseKind::Prerelease : ReleaseKind::Stable;
    }

    // A kind in words, as the notes' meta line and the plain text say it.
    //   kind    — the kind.
    //   returns — "stable", "pre-release" or "nightly" (a static string).
    //   throws  — nothing.
    inline const wchar_t* ReleaseKindWords(ReleaseKind kind) noexcept
    {
        switch (kind)
        {
        case ReleaseKind::Prerelease:
            return L"pre-release";
        case ReleaseKind::Nightly:
            return L"nightly";
        case ReleaseKind::Stable:
        default:
            return L"stable";
        }
    }

    // A kind as the page's CSS class and its short tab badge ("" for a stable release, which wears no badge).
    //   kind    — the kind.
    //   badge   — true for the badge text, false for the class name.
    //   returns — the class ("stable" / "prerelease" / "nightly") or the badge ("" / "pre" / "nightly"); a static string.
    //   throws  — nothing.
    inline const wchar_t* ReleaseKindId(ReleaseKind kind, bool badge) noexcept
    {
        switch (kind)
        {
        case ReleaseKind::Prerelease:
            return badge ? L"pre" : L"prerelease";
        case ReleaseKind::Nightly:
            return L"nightly";
        case ReleaseKind::Stable:
        default:
            return badge ? L"" : L"stable";
        }
    }

    // A role as the page's CSS class ("older" / "yours" / "update") or in words ("Older" / "Your version" / "Update").
    //   role    — the role.
    //   words   — true for the words the notes' header chip shows, false for the class.
    //   returns — a static string.
    //   throws  — nothing.
    inline const wchar_t* ReleaseRoleId(ReleaseRole role, bool words) noexcept
    {
        switch (role)
        {
        case ReleaseRole::Yours:
            return words ? L"Your version" : L"yours";
        case ReleaseRole::Update:
            return words ? L"Update" : L"update";
        case ReleaseRole::Older:
        default:
            return words ? L"Older" : L"older";
        }
    }

    // True for a link to one of OUR release pages (https://github.com/<kRepo>/releases/...). The notes open release pages in
    // the user's browser, so a page URL the API sent is used only when it points there; anything else is rebuilt from the
    // tag (ReleasePageForTag), the same distrust IsTrustedAssetUrl applies to the installer's downloads.
    //   url     — the URL as the API sent it.
    //   returns — true when it is a page under our repository's releases.
    //   throws  — std::bad_alloc only.
    inline bool IsOwnReleasePage(std::wstring_view url)
    {
        const std::wstring prefix = std::wstring{ L"https://github.com/" } + kRepo + L"/releases/";
        return url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0;
    }

    // The releases of one GitHub list page that the user's update channel admits, in the page's order (newest first).
    //   page            — the parsed page (a JSON array of release objects); anything else gives none.
    //   allowPrerelease — the user's pre-release opt-in.
    //   allowNightly    — the user's nightly opt-in.
    //   returns         — the releases. Drafts, releases without a tag and releases off the channel are left out, so the
    //                     tabs never offer notes of a release the check itself would never offer.
    //   throws          — std::bad_alloc only.
    inline std::vector<ReleaseNote> ParseReleaseListPage(const json::Value& page, bool allowPrerelease, bool allowNightly)
    {
        std::vector<ReleaseNote> out;
        if (page.type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const auto& rel : page.arr)
        {
            if (rel.type != json::Value::Type::Obj || rel.BoolAt(L"draft", false))
            {
                continue;
            }
            ReleaseNote note;
            note.tag = rel.StrAt(L"tag_name");
            if (note.tag.empty())
            {
                continue;
            }
            const bool prerelease = rel.BoolAt(L"prerelease", false);
            if (!ReleaseAllowedOnChannel(note.tag, prerelease, allowPrerelease, allowNightly))
            {
                continue;
            }
            note.name = rel.StrAt(L"name");
            note.version = ParseVersion(note.tag);
            note.kind = ReleaseKindOf(note.tag, prerelease);
            note.publishedAt = rel.StrAt(L"published_at");
            const std::wstring pageUrl = rel.StrAt(L"html_url");
            note.htmlUrl = IsOwnReleasePage(pageUrl) ? pageUrl : ReleasePageForTag(note.tag);
            note.body = rel.StrAt(L"body");
            out.push_back(std::move(note));
        }
        return out;
    }

    // The release the prompt offers, as far as the check already knows it: no title and no date (the /releases/latest and
    // list objects carry them, but UpdateInfo keeps neither). The notes show it at once, before the list arrives; the list's
    // own copy replaces it later (BuildReleaseTimeline keeps the first copy of a tag, and callers put the list first).
    //   info    — the check's result.
    //   returns — the offered release.
    //   throws  — std::bad_alloc only.
    inline ReleaseNote OfferedReleaseNote(const UpdateInfo& info)
    {
        ReleaseNote note;
        note.tag = info.latestTag;
        note.version = info.latest;
        note.kind = info.isNightly ? ReleaseKind::Nightly : info.isPrerelease ? ReleaseKind::Prerelease : ReleaseKind::Stable;
        const std::wstring page = ChangelogUrl(info);
        note.htmlUrl = IsOwnReleasePage(page) ? page : ReleasePageForTag(info.latestTag);
        note.body = info.notes;
        return note;
    }

    // ============================ the release list ============================

    // The outcome of reading the release list.
    struct ReleaseListFetch
    {
        bool ok{ false }; // at least the first page arrived and parsed
        std::wstring error; // why not, in a few words, when !ok
        std::vector<ReleaseNote> releases; // on the channel, in the API's order (newest first), every page read
        int pages{ 0 }; // the pages read
    };

    // Read GitHub's release list page by page, keeping the releases on the user's channel. Paging stops at the first page
    // that holds a release at or below the installed version (every update is then known, with some older context), at a
    // short page (the end of the list), or after kReleaseNotesMaxPages.
    //   current         — the installed version: the paging stop.
    //   allowPrerelease — the channel's pre-release opt-in.
    //   allowNightly    — the channel's nightly opt-in.
    //   timeoutMs       — each WinHTTP phase's timeout, per page (HttpsGet).
    //   returns         — what was read. ok is false, with a note, only when the FIRST page failed: a later page's failure
    //                     keeps what the earlier pages gave.
    //   throws          — std::bad_alloc only. It blocks on the network: call it on a worker, never on a UI thread.
    inline ReleaseListFetch FetchReleaseNotesList(const Version& current, bool allowPrerelease, bool allowNightly, DWORD timeoutMs)
    {
        ReleaseListFetch out;
        const std::wstring base = std::wstring{ L"/repos/" } + kRepo + L"/releases?per_page=" + std::to_wstring(kReleaseNotesPageSize) + L"&page=";
        for (int page = 1; page <= kReleaseNotesMaxPages; ++page)
        {
            std::wstring err;
            const std::string raw = HttpsGet(kApiHost, base + std::to_wstring(page), timeoutMs, err);
            std::optional<json::Value> parsed;
            if (!raw.empty())
            {
                parsed = json::Parse(detail::Utf8ToWide(raw));
            }
            if (!parsed || parsed->type != json::Value::Type::Arr)
            {
                if (page == 1)
                {
                    out.error = !err.empty() ? err : raw.empty() ? std::wstring{ L"no response" } : std::wstring{ L"unexpected response" };
                }
                break;
            }
            out.ok = true;
            out.pages = page;
            auto onChannel = ParseReleaseListPage(*parsed, allowPrerelease, allowNightly);
            const bool reachedYours = std::any_of(onChannel.begin(), onChannel.end(), [&](const ReleaseNote& n) {
                return CompareVersion(n.version, current) <= 0;
            });
            for (auto& n : onChannel)
            {
                out.releases.push_back(std::move(n));
            }
            // A page shorter than asked is the end of the list (counted over the RAW page: drafts and off-channel releases
            // are on it too).
            if (reachedYours || parsed->arr.size() < static_cast<size_t>(kReleaseNotesPageSize))
            {
                break;
            }
        }
        return out;
    }

    // ============================ the timeline ============================

    // The tab bar's timeline: the releases oldest first, each with its role, and where the installed version stands.
    struct ReleaseTimeline
    {
        std::vector<ReleaseNote> releases; // oldest first: the tabs, left to right
        std::vector<ReleaseRole> roles; // one per release, same index
        // Where the "you" arrow points. youOnTab: releases[youIndex] IS the installed version (newest of equals). Otherwise the
        // installed version is no release on the channel (a local build, a nightly after the opt-in was turned off) and the
        // arrow sits in the gap BEFORE releases[youIndex] — the first update — or after the last tab when youIndex ==
        // releases.size() (nothing newer, which an offered update rules out).
        size_t youIndex{ 0 };
        bool youOnTab{ false };
        size_t selected{ 0 }; // the tab the notes open on: the newest release, the offered one
    };

    // Build the timeline from releases in any order.
    //   releases — the releases (the list's copies first, then any release merged in: the first copy of a tag wins, compared
    //              case-insensitively).
    //   current  — the installed version.
    //   maxTabs  — the cap (0: none). The NEWEST releases are kept, so every update survives before any older one.
    //   returns  — the timeline; empty when releases is.
    //   throws   — std::bad_alloc only.
    inline ReleaseTimeline BuildReleaseTimeline(std::vector<ReleaseNote> releases, const Version& current, size_t maxTabs = kReleaseNotesMaxTabs)
    {
        std::vector<ReleaseNote> unique;
        unique.reserve(releases.size());
        for (auto& r : releases)
        {
            const bool seen = std::any_of(unique.begin(), unique.end(), [&](const ReleaseNote& u) {
                return ::CompareStringOrdinal(u.tag.c_str(), static_cast<int>(u.tag.size()), r.tag.c_str(), static_cast<int>(r.tag.size()), TRUE) == CSTR_EQUAL;
            });
            if (!seen)
            {
                unique.push_back(std::move(r));
            }
        }
        // Oldest -> newest by version. Equal versions (a pre-release and the stable of the same number) by date — one ISO 8601
        // UTC format, so text order is time order, and an unknown date ("") sorts first — then by tag, so the order never
        // depends on the API's order.
        std::stable_sort(unique.begin(), unique.end(), [](const ReleaseNote& a, const ReleaseNote& b) {
            const int byVersion = CompareVersion(a.version, b.version);
            if (byVersion != 0)
            {
                return byVersion < 0;
            }
            if (a.publishedAt != b.publishedAt)
            {
                return a.publishedAt < b.publishedAt;
            }
            return a.tag < b.tag;
        });
        if (maxTabs > 0 && unique.size() > maxTabs)
        {
            unique.erase(unique.begin(), unique.end() - static_cast<std::ptrdiff_t>(maxTabs));
        }

        ReleaseTimeline t;
        t.releases = std::move(unique);
        t.roles.reserve(t.releases.size());
        size_t firstUpdate = t.releases.size();
        bool haveYours = false;
        size_t newestYours = 0;
        for (size_t i = 0; i < t.releases.size(); ++i)
        {
            const int against = CompareVersion(t.releases[i].version, current);
            const ReleaseRole role = against > 0 ? ReleaseRole::Update : against == 0 ? ReleaseRole::Yours : ReleaseRole::Older;
            t.roles.push_back(role);
            if (role == ReleaseRole::Update && firstUpdate == t.releases.size())
            {
                firstUpdate = i;
            }
            if (role == ReleaseRole::Yours)
            {
                haveYours = true;
                newestYours = i;
            }
        }
        t.youOnTab = haveYours;
        t.youIndex = haveYours ? newestYours : firstUpdate;
        t.selected = t.releases.empty() ? 0 : t.releases.size() - 1;
        return t;
    }

    // ============================ the notes' text ============================

    namespace detail
    {
        // Case-insensitive equality of two texts (ordinal, as the file system compares names).
        //   a, b    — the texts.
        //   returns — true when they are equal ignoring case.
        //   throws  — nothing.
        inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b) noexcept
        {
            return a.size() == b.size() &&
                   (a.empty() || ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
        }

        // A text without its leading and trailing spaces and tabs.
        //   text    — the text.
        //   returns — a view into text.
        //   throws  — nothing.
        inline std::wstring_view TrimBlanks(std::wstring_view text) noexcept
        {
            while (!text.empty() && (text.front() == L' ' || text.front() == L'\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r'))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        // The ATX heading level (1-6) of one Markdown line, CommonMark's rules: up to three spaces of indent, 1-6 '#', then a
        // space, a tab or the line's end; the text is the rest, trimmed, without a closing run of '#' that follows a blank.
        //   line    — the line (no line break).
        //   text    — receives the heading's text when the line is a heading.
        //   returns — the level; 0 when the line is no ATX heading (four spaces of indent make it code, "#5" is text).
        //   throws  — nothing.
        inline int AtxHeadingLevel(std::wstring_view line, std::wstring_view& text) noexcept
        {
            size_t i = 0;
            while (i < line.size() && i < 3 && line[i] == L' ')
            {
                ++i;
            }
            size_t hashes = 0;
            while (i + hashes < line.size() && line[i + hashes] == L'#')
            {
                ++hashes;
            }
            if (hashes == 0 || hashes > 6)
            {
                return 0;
            }
            const size_t after = i + hashes;
            if (after < line.size() && line[after] != L' ' && line[after] != L'\t' && line[after] != L'\r')
            {
                return 0;
            }
            std::wstring_view rest = TrimBlanks(line.substr((std::min)(after, line.size())));
            // A closing sequence: trailing '#'s preceded by a blank (or making up the whole rest) are not part of the text.
            size_t end = rest.size();
            while (end > 0 && rest[end - 1] == L'#')
            {
                --end;
            }
            if (end < rest.size() && (end == 0 || rest[end - 1] == L' ' || rest[end - 1] == L'\t'))
            {
                rest = TrimBlanks(rest.substr(0, end));
            }
            text = rest;
            return static_cast<int>(hashes);
        }

        // Whether one Markdown line opens a fenced code block: up to three spaces, then three or more '`' or '~'.
        //   line    — the line.
        //   fence   — receives the fence character.
        //   length  — receives the fence's length (a closing fence must be at least as long).
        //   returns — true for an opening fence.
        //   throws  — nothing.
        inline bool OpensFence(std::wstring_view line, wchar_t& fence, size_t& length) noexcept
        {
            size_t i = 0;
            while (i < line.size() && i < 3 && line[i] == L' ')
            {
                ++i;
            }
            if (i >= line.size() || (line[i] != L'`' && line[i] != L'~'))
            {
                return false;
            }
            const wchar_t c = line[i];
            size_t n = 0;
            while (i + n < line.size() && line[i + n] == c)
            {
                ++n;
            }
            if (n < 3)
            {
                return false;
            }
            // A backtick fence's info string may not contain a backtick (CommonMark), else the line is inline code.
            if (c == L'`' && line.substr(i + n).find(L'`') != std::wstring_view::npos)
            {
                return false;
            }
            fence = c;
            length = n;
            return true;
        }

        // Whether one Markdown line closes the fenced code block that `fence` x `length` opened: up to three spaces, at
        // least `length` fence characters, then only blanks.
        //   line, fence, length — the line and the open fence.
        //   returns — true for the closing fence.
        //   throws  — nothing.
        inline bool ClosesFence(std::wstring_view line, wchar_t fence, size_t length) noexcept
        {
            size_t i = 0;
            while (i < line.size() && i < 3 && line[i] == L' ')
            {
                ++i;
            }
            size_t n = 0;
            while (i + n < line.size() && line[i + n] == fence)
            {
                ++n;
            }
            return n >= length && TrimBlanks(line.substr(i + n)).empty();
        }
    }

    // The notes without their "Install" section: from an ATX heading whose text is "Install" (any case) to the next heading
    // of the same or a higher level, or the end. The house format of every release ends with "## Install" — the commands to
    // install BY HAND, which only distract in a prompt whose button installs for the user. Headings inside fenced code
    // blocks are code, not headings, on both sides of the cut.
    //   markdown — the notes.
    //   returns  — the notes without the section (unchanged when they have none); line breaks kept as they were.
    //   throws   — std::bad_alloc only.
    inline std::wstring StripInstallSection(std::wstring_view markdown)
    {
        std::wstring out;
        out.reserve(markdown.size());
        bool inFence = false;
        wchar_t fence = 0;
        size_t fenceLength = 0;
        int cutLevel = 0; // 0: keeping; 1-6: cutting the section a heading of this level opened
        size_t pos = 0;
        while (pos < markdown.size())
        {
            size_t end = markdown.find(L'\n', pos);
            const size_t next = end == std::wstring_view::npos ? markdown.size() : end + 1;
            const std::wstring_view line = markdown.substr(pos, (end == std::wstring_view::npos ? markdown.size() : end) - pos);
            bool keep = cutLevel == 0;
            if (inFence)
            {
                if (detail::ClosesFence(line, fence, fenceLength))
                {
                    inFence = false;
                }
            }
            else if (detail::OpensFence(line, fence, fenceLength))
            {
                inFence = true;
            }
            else
            {
                std::wstring_view text;
                if (const int level = detail::AtxHeadingLevel(line, text); level > 0)
                {
                    if (cutLevel > 0 && level <= cutLevel)
                    {
                        cutLevel = 0; // the next section at the same or a higher level ends the cut — and is kept
                        keep = true;
                    }
                    if (cutLevel == 0 && detail::EqualsNoCase(text, L"Install"))
                    {
                        cutLevel = level;
                        keep = false;
                    }
                }
            }
            if (keep)
            {
                out.append(markdown.substr(pos, next - pos));
            }
            pos = next;
        }
        // The cut leaves the blank lines that preceded it at the end; trim them so the page and the plain text end on text.
        while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' ' || out.back() == L'\t'))
        {
            out.pop_back();
        }
        return out;
    }

    // The notes cut to at most maxChars, at the end of a line.
    //   markdown — the notes.
    //   maxChars — the cap (UTF-16 units).
    //   cut      — receives whether anything was cut.
    //   returns  — the (possibly) shortened notes. A cut inside a fenced code block leaves the fence open; cmark closes it at
    //              the document's end, so the page shows the code without its tail rather than broken markup.
    //   throws   — std::bad_alloc only.
    inline std::wstring CapMarkdown(std::wstring_view markdown, size_t maxChars, bool& cut)
    {
        cut = markdown.size() > maxChars;
        if (!cut)
        {
            return std::wstring{ markdown };
        }
        const size_t lastBreak = markdown.substr(0, maxChars).find_last_of(L'\n');
        return std::wstring{ markdown.substr(0, lastBreak == std::wstring_view::npos ? maxChars : lastBreak) };
    }

    // Text made safe to put inside HTML: & < > " ' as character references.
    //   text    — the text.
    //   returns — the escaped text.
    //   throws  — std::bad_alloc only.
    inline std::wstring HtmlEscape(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size() + text.size() / 8);
        for (const wchar_t c : text)
        {
            switch (c)
            {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            case L'"':
                out += L"&quot;";
                break;
            case L'\'':
                out += L"&#39;";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    // What renders the notes in this build, for the [update] trail and the tests: cmark's version, or the fallback.
    //   returns — e.g. "cmark 0.31.1" or "escaped text (no cmark in this build)".
    //   throws  — std::bad_alloc only.
    inline std::wstring ReleaseNotesRendererName()
    {
#if AGENTMASTER_UPDATER_HAVE_CMARK
        return L"cmark " + detail::Utf8ToWide(cmark_version_string());
#else
        return L"escaped text (no cmark in this build)";
#endif
    }

    // One release's notes as an HTML fragment for the page: the Install section cut (StripInstallSection), the length capped
    // (CapMarkdown), then cmark's CommonMark renderer in its SAFE mode (CMARK_OPT_DEFAULT, safe since cmark 0.29): raw HTML
    // becomes "<!-- raw HTML omitted -->" and a javascript:, vbscript:, file: or data: (non-image) destination is emptied, so
    // the notes can never run script in the page — the page's content-security policy would refuse it anyway. Without cmark
    // (a harness built without the vcpkg tree) the notes show as escaped preformatted text.
    //   markdown — the notes as published.
    //   returns  — the fragment; a short "no notes" paragraph for empty notes.
    //   throws   — std::bad_alloc only.
    inline std::wstring ReleaseNotesHtml(std::wstring_view markdown)
    {
        bool cut = false;
        const std::wstring body = CapMarkdown(StripInstallSection(markdown), kReleaseNotesMaxMarkdownChars, cut);
        if (detail::TrimBlanks(body).empty())
        {
            return L"<p class=\"empty\">This release has no notes.</p>";
        }
        std::wstring html;
#if AGENTMASTER_UPDATER_HAVE_CMARK
        const std::string utf8 = detail::WideToUtf8(body);
        // CMARK_OPT_VALIDATE_UTF8 replaces an invalid byte sequence with U+FFFD instead of passing it on (WideToUtf8 makes
        // none, but the notes come from the network). The result is allocated by cmark's own allocator, so it is freed
        // through that allocator, in this module's CRT, however the conversion below ends.
        const auto freeHtml = [](char* p) noexcept {
            cmark_get_default_mem_allocator()->free(p);
        };
        const std::unique_ptr<char, decltype(freeHtml)> rendered{ cmark_markdown_to_html(utf8.data(), utf8.size(), CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8), freeHtml };
        html = rendered ? detail::Utf8ToWide(rendered.get()) : L"<pre>" + HtmlEscape(body) + L"</pre>";
#else
        html = L"<pre>" + HtmlEscape(body) + L"</pre>";
#endif
        if (cut)
        {
            html += L"<p class=\"empty\">The notes continue on the release page.</p>";
        }
        return html;
    }

    // A release's publish date in the user's own short date format, in local time.
    //   isoUtc  — GitHub's published_at ("2026-10-29T05:37:58Z"; fractions and offsets after the seconds are ignored, the
    //             time is taken as UTC, which is what GitHub sends).
    //   returns — e.g. "29/10/2026" or "10/29/2026"; "" when the text is no such timestamp or the date is impossible.
    //   throws  — std::bad_alloc only.
    inline std::wstring FormatReleaseDate(std::wstring_view isoUtc)
    {
        if (isoUtc.size() < 19 || isoUtc[4] != L'-' || isoUtc[7] != L'-' || (isoUtc[10] != L'T' && isoUtc[10] != L' ') || isoUtc[13] != L':' || isoUtc[16] != L':')
        {
            return {};
        }
        const auto number = [&](size_t at, size_t length, WORD& out) noexcept {
            unsigned value = 0;
            for (size_t i = at; i < at + length; ++i)
            {
                if (isoUtc[i] < L'0' || isoUtc[i] > L'9')
                {
                    return false;
                }
                value = value * 10 + static_cast<unsigned>(isoUtc[i] - L'0');
            }
            out = static_cast<WORD>(value);
            return true;
        };
        SYSTEMTIME utc{};
        if (!number(0, 4, utc.wYear) || !number(5, 2, utc.wMonth) || !number(8, 2, utc.wDay) || !number(11, 2, utc.wHour) || !number(14, 2, utc.wMinute) ||
            !number(17, 2, utc.wSecond))
        {
            return {};
        }
        // The conversion validates the date too (a 30 February fails here, and GetDateFormatEx refuses it below).
        SYSTEMTIME local{};
        if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        {
            return {};
        }
        wchar_t buffer[80]{};
        if (::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, buffer, ARRAYSIZE(buffer), nullptr) <= 0)
        {
            return {};
        }
        return buffer;
    }

    // A release's tab label. A pre-release or nightly tab shows its tag up to the first '-' ("v0.7.10-prerelease" ->
    // "v0.7.10"): its badge already says "pre" or "nightly", and the full tag stays in the tooltip and the notes' header, so
    // the strip fits more releases. A stable release keeps its whole tag (nothing else would tell "v1.0.0-hotfix" apart).
    //   release — the release.
    //   returns — the label; the whole tag when the cut would leave nothing.
    //   throws  — std::bad_alloc only.
    inline std::wstring ReleaseTabLabel(const ReleaseNote& release)
    {
        if (release.kind == ReleaseKind::Stable)
        {
            return release.tag;
        }
        const size_t dash = release.tag.find(L'-');
        return dash == std::wstring::npos || dash == 0 ? release.tag : release.tag.substr(0, dash);
    }

    // One release's meta line: its tag, its kind and its date ("v0.8.1 · stable · 29/10/2026").
    //   release — the release.
    //   returns — the line (no date part when the date is unknown).
    //   throws  — std::bad_alloc only.
    inline std::wstring ReleaseMetaLine(const ReleaseNote& release)
    {
        std::wstring meta = release.tag + L" \x00B7 " + ReleaseKindWords(release.kind);
        if (const std::wstring date = FormatReleaseDate(release.publishedAt); !date.empty())
        {
            meta += L" \x00B7 " + date;
        }
        return meta;
    }

    // ============================ the page ============================

    // The page's model: everything the tab bar and the notes show, as the JSON the window posts to the page
    // ({"type":"model","model":<this>}). The page holds no data of its own, so the first post and every later one (when the
    // list arrives) go through the same render.
    //   timeline — the tabs.
    //   current  — the installed version (the arrow's label when it is no release on the channel).
    //   status   — a line above the notes ("" for none): "Loading the other releases…", or why the list failed.
    //   returns  — the model as JSON text. Each release's notes are rendered here (ReleaseNotesHtml), so the caller pays
    //              cmark's cost once per model — on the list worker, not on the window's thread, for the big one.
    //   throws   — std::bad_alloc only.
    inline std::wstring BuildReleaseNotesModelJson(const ReleaseTimeline& timeline, const Version& current, std::wstring_view status)
    {
        json::Value list = json::Value::MkArr();
        for (size_t i = 0; i < timeline.releases.size(); ++i)
        {
            const ReleaseNote& r = timeline.releases[i];
            const ReleaseRole role = i < timeline.roles.size() ? timeline.roles[i] : ReleaseRole::Older;
            const std::wstring meta = ReleaseMetaLine(r);
            const wchar_t* fate = role == ReleaseRole::Update ? L"an update you get" : role == ReleaseRole::Yours ? L"the version you run" : L"older than yours";
            json::Value item = json::Value::MkObj();
            item.Set(L"tag", json::Value::MkStr(r.tag));
            item.Set(L"label", json::Value::MkStr(ReleaseTabLabel(r)));
            item.Set(L"role", json::Value::MkStr(ReleaseRoleId(role, false)));
            item.Set(L"roleText", json::Value::MkStr(ReleaseRoleId(role, true)));
            item.Set(L"kind", json::Value::MkStr(ReleaseKindId(r.kind, false)));
            item.Set(L"kindShort", json::Value::MkStr(ReleaseKindId(r.kind, true)));
            item.Set(L"meta", json::Value::MkStr(meta));
            item.Set(L"tooltip", json::Value::MkStr((r.name.empty() ? r.tag : r.name) + L" \x2014 " + meta + L" \x2014 " + fate));
            item.Set(L"url", json::Value::MkStr(r.htmlUrl));
            item.Set(L"html", json::Value::MkStr(ReleaseNotesHtml(r.body)));
            list.Push(std::move(item));
        }
        json::Value you = json::Value::MkObj();
        you.Set(L"index", json::Value::MkNum(static_cast<double>(timeline.youIndex)));
        you.Set(L"onTab", json::Value::MkBool(timeline.youOnTab));
        // When the installed version is no tab, the arrow names it: "you" alone would point at a gap and say nothing.
        you.Set(L"label", json::Value::MkStr(timeline.youOnTab ? std::wstring{ L"you" } : L"you (v" + VersionToString(current) + L")"));
        json::Value root = json::Value::MkObj();
        root.Set(L"releases", std::move(list));
        root.Set(L"you", std::move(you));
        root.Set(L"selected", json::Value::MkNum(static_cast<double>(timeline.selected)));
        root.Set(L"status", json::Value::MkStr(std::wstring{ status }));
        return json::Dump(root);
    }

    // A fresh nonce for the page's content-security policy: the policy runs only the script and the style that carry it, so
    // nothing else that ever reached the page — notes markup cmark let through, a future bug — could run script.
    //   returns — 32 hex digits from a random GUID (CoCreateGuid: 122 random bits); a tick-and-process mix if that fails.
    //   throws  — std::bad_alloc only.
    inline std::wstring NewReleaseNotesNonce()
    {
        GUID g{};
        if (FAILED(::CoCreateGuid(&g)))
        {
            g.Data1 = static_cast<unsigned long>(::GetTickCount64());
            g.Data2 = static_cast<unsigned short>(::GetCurrentProcessId());
            g.Data3 = static_cast<unsigned short>(::GetCurrentThreadId());
        }
        wchar_t buffer[40]{};
        ::swprintf(buffer, ARRAYSIZE(buffer), L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", g.Data1, static_cast<unsigned>(g.Data2), static_cast<unsigned>(g.Data3),
                   static_cast<unsigned>(g.Data4[0]), static_cast<unsigned>(g.Data4[1]), static_cast<unsigned>(g.Data4[2]), static_cast<unsigned>(g.Data4[3]),
                   static_cast<unsigned>(g.Data4[4]), static_cast<unsigned>(g.Data4[5]), static_cast<unsigned>(g.Data4[6]), static_cast<unsigned>(g.Data4[7]));
        return buffer;
    }

    // The page's look: the dialog's own font, so the HTML column reads like the Win32 column beside it.
    struct ReleaseNotesPageStyle
    {
        std::wstring fontFamily{ L"Segoe UI" }; // the dialog's message font face (letters, digits, spaces and '-' survive)
        int fontPx{ 12 }; // its size in CSS pixels (= DIPs, so the page scales with the monitor like the dialog)
    };

    namespace detail
    {
        // The page, in pieces: MSVC refuses one string literal past 16 KB, and each piece stays well below that. The
        // placeholders {{NONCE}}, {{FONT_FAMILY}} and {{FONT_PX}} are filled by BuildReleaseNotesPage. Characters outside
        // ASCII are written as HTML character references or String.fromCharCode, so the source encoding never matters.
        inline constexpr const wchar_t* kNotesPageHead = LR"AMPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src https: data:; style-src 'nonce-{{NONCE}}'; script-src 'nonce-{{NONCE}}'; base-uri 'none'; form-action 'none'">
<meta name="color-scheme" content="dark">
<title>Release notes</title>
<style nonce="{{NONCE}}">
)AMPAGE";

        inline constexpr const wchar_t* kNotesPageCss = LR"AMPAGE(:root { color-scheme: dark; --bg: #262626; --bar: #2e2e2e; --text: #e6e6e6; --dim: #8c8c8c; --line: #3c3c3c;
  --green: #3fb950; --green-bg: rgba(63, 185, 80, 0.16); --link: #4cc2ff; --amber: #d29922; --violet: #a371f7;
  --code-bg: #1e1e1e; --chip: #3a3a3a; --hover: #353535; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
  font: {{FONT_PX}}px/1.5 {{FONT_FAMILY}}, "Segoe UI", system-ui, sans-serif; }
body { display: flex; flex-direction: column; overflow: hidden; }
#bar { position: relative; display: flex; align-items: stretch; flex: none; background: var(--bar); border-bottom: 1px solid var(--line); }
.nav { flex: none; width: 24px; border: 0; padding: 0; background: transparent; color: var(--dim); font: inherit; font-size: 1.5em; line-height: 1; cursor: pointer; }
.nav:hover:not(:disabled) { color: var(--text); background: var(--hover); }
.nav:disabled { opacity: 0.25; cursor: default; }
#strip { position: relative; flex: 1; display: flex; overflow-x: auto; overflow-y: hidden; scrollbar-width: none; }
#strip::-webkit-scrollbar { display: none; }
.tab { flex: none; display: flex; align-items: center; gap: 5px; margin: 0; padding: 7px 12px 5px; border: 0; border-bottom: 2px solid transparent;
  background: transparent; color: var(--text); font: inherit; white-space: nowrap; cursor: pointer; }
.tab + .tab { box-shadow: inset 1px 0 0 var(--line); }
.tab:hover { background: var(--hover); }
.tab.older { color: var(--dim); }
.tab.update { color: var(--green); }
.tab.selected { background: #3a3a3a; border-bottom-color: currentColor; }
.tab.update.selected { background: var(--green-bg); }
.tab:focus-visible { outline: 1px solid var(--text); outline-offset: -4px; }
.tab .dot { font-size: 0.7em; }
.tab .badge { font-size: 0.72em; padding: 0 4px; border: 1px solid currentColor; border-radius: 3px; }
.tab.prerelease .badge { color: var(--amber); }
.tab.nightly .badge { color: var(--violet); }
#yourow { position: relative; flex: none; height: 17px; background: var(--bg); }
#you { position: absolute; top: 0; display: flex; gap: 4px; font-size: 0.85em; line-height: 17px; white-space: nowrap; pointer-events: none; }
#you.flip { flex-direction: row-reverse; }
#status { flex: none; padding: 2px 16px 4px; color: var(--dim); font-size: 0.92em; }
#notes { flex: 1; overflow-y: auto; padding: 4px 18px 18px; outline: none; }
.relhead { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px; padding: 6px 0 6px; border-bottom: 1px solid var(--line); }
.chip { font-size: 0.8em; padding: 1px 8px; border-radius: 9px; background: var(--chip); }
.chip.update { background: var(--green-bg); color: var(--green); }
.chip.older { color: var(--dim); }
.meta { color: var(--dim); }
.gh { margin-left: auto; }
a { color: var(--link); text-decoration: none; }
a:hover, a:focus-visible { text-decoration: underline; }
.empty { color: var(--dim); font-style: italic; }
.md h1, .md h2, .md h3, .md h4, .md h5, .md h6 { font-weight: 600; line-height: 1.25; margin: 1.1em 0 0.5em; }
.md h1 { font-size: 1.6em; }
.md h2 { font-size: 1.35em; padding-bottom: 0.25em; border-bottom: 1px solid var(--line); }
.md h3 { font-size: 1.15em; }
.md h4, .md h5, .md h6 { font-size: 1em; }
.md p { margin: 0.55em 0; }
.md ul, .md ol { margin: 0.4em 0; padding-left: 1.7em; }
.md li { margin: 0.22em 0; }
.md li > p { margin: 0.3em 0; }
.md code { font-family: "Cascadia Mono", Consolas, monospace; font-size: 0.92em; padding: 0.1em 0.35em; border-radius: 4px; background: var(--chip); }
.md pre { margin: 0.7em 0; padding: 0.65em 0.9em; overflow-x: auto; background: var(--code-bg); border: 1px solid var(--line); border-radius: 6px; }
.md pre code { padding: 0; background: none; font-size: 0.9em; white-space: pre; }
.md blockquote { margin: 0.6em 0; padding: 0 0.9em; color: #b5b5b5; border-left: 3px solid #4a4a4a; }
.md hr { border: 0; border-top: 1px solid var(--line); margin: 1em 0; }
.md img { max-width: 100%; }
.md table { border-collapse: collapse; }
.md th, .md td { padding: 0.3em 0.6em; border: 1px solid var(--line); }
@media (forced-colors: active) { .tab.selected { border-bottom-color: Highlight; } .tab.update { color: LinkText; } }
)AMPAGE";

        inline constexpr const wchar_t* kNotesPageBody = LR"AMPAGE(</style>
</head>
<body>
<div id="bar">
<button id="prev" class="nav" type="button" tabindex="-1" aria-label="Show older releases">&#8249;</button>
<div id="strip" role="tablist" aria-label="Releases"></div>
<button id="next" class="nav" type="button" tabindex="-1" aria-label="Show newer releases">&#8250;</button>
</div>
<div id="yourow" aria-hidden="true"><span id="you" hidden><span id="youmark"></span><span id="youtext"></span></span></div>
<div id="status" role="status" hidden></div>
<div id="notes" role="tabpanel" tabindex="0"><p class="empty">Loading the release notes&#8230;</p></div>
<script nonce="{{NONCE}}">
)AMPAGE";

        inline constexpr const wchar_t* kNotesPageScript = LR"AMPAGE((function () {
  'use strict';
  var host = window.chrome && window.chrome.webview ? window.chrome.webview : null;
  var strip = document.getElementById('strip');
  var notes = document.getElementById('notes');
  var you = document.getElementById('you');
  var youMark = document.getElementById('youmark');
  var youText = document.getElementById('youtext');
  var youRow = document.getElementById('yourow');
  var status = document.getElementById('status');
  var prev = document.getElementById('prev');
  var next = document.getElementById('next');
  var DOT = String.fromCharCode(0x25CF), UP = String.fromCharCode(0x25B2), ARROW = String.fromCharCode(0x2192);
  var model = null, selected = -1;

  function post(message) { if (host) { host.postMessage(message); } }
  function make(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) { e.className = cls; }
    if (text !== undefined && text !== null) { e.textContent = text; }
    return e;
  }
  function tabs() { return strip.children; }
  function updateNav() {
    prev.disabled = strip.scrollLeft <= 0;
    next.disabled = strip.scrollLeft + strip.clientWidth >= strip.scrollWidth - 1;
  }
  // Where the "you" arrow points, in the strip's scrolled coordinates: the middle of your tab, or the gap before the first
  // update when your version is no release on the channel.
  function youX() {
    if (!model || !model.you) { return null; }
    var list = tabs();
    if (!list.length) { return null; }
    var i = model.you.index;
    if (model.you.onTab && list[i]) { return list[i].offsetLeft + list[i].offsetWidth / 2; }
    if (i <= 0) { return list[0].offsetLeft; }
    if (i >= list.length) { var last = list[list.length - 1]; return last.offsetLeft + last.offsetWidth; }
    return list[i].offsetLeft;
  }
  // The arrow glyph sits exactly under its point with the words to its right — or, when they would run past the right edge,
  // to its left — so the label is never cut at either side.
  function placeYou() {
    var x = youX();
    if (x === null) { you.hidden = true; return; }
    var left = x - strip.scrollLeft;
    if (left < -2 || left > strip.clientWidth + 2) { you.hidden = true; return; }
    you.hidden = false;
    you.classList.remove('flip');
    var anchor = strip.offsetLeft + left;
    var half = youMark.offsetWidth / 2;
    var start = anchor - half;
    if (start + you.offsetWidth > youRow.clientWidth - 2) {
      you.classList.add('flip');
      start = anchor + half - you.offsetWidth;
    }
    you.style.left = Math.max(0, start) + 'px';
  }
  function ensureVisible(tab) {
    var l = tab.offsetLeft, r = l + tab.offsetWidth;
    if (l < strip.scrollLeft) { strip.scrollLeft = Math.max(0, l - 8); }
    else if (r > strip.scrollLeft + strip.clientWidth) { strip.scrollLeft = r - strip.clientWidth + 8; }
  }
  // The strip opens at yours and the updates: your mark near the left edge, unless the newest would then be out of view.
  function revealStart() {
    var list = tabs();
    if (!list.length) { return; }
    var x = youX(), last = list[list.length - 1];
    var target = x === null ? 0 : Math.max(0, x - 56);
    var lastRight = last.offsetLeft + last.offsetWidth;
    if (lastRight - target > strip.clientWidth) { target = Math.max(0, lastRight - strip.clientWidth + 8); }
    strip.scrollLeft = target;
  }
  function select(i, focus) {
    if (!model || i < 0 || i >= model.releases.length) { return; }
    var list = tabs();
    for (var k = 0; k < list.length; ++k) {
      var on = k === i;
      list[k].classList.toggle('selected', on);
      list[k].setAttribute('aria-selected', on ? 'true' : 'false');
      list[k].tabIndex = on ? 0 : -1;
    }
    selected = i;
    var r = model.releases[i];
    notes.textContent = '';
    var head = make('div', 'relhead');
    head.appendChild(make('span', 'chip ' + r.role, r.roleText));
    head.appendChild(make('span', 'meta', r.meta));
    if (r.url) { var a = make('a', 'gh', 'Open on GitHub ' + ARROW); a.href = r.url; head.appendChild(a); }
    notes.appendChild(head);
    var body = make('div', 'md');
    body.innerHTML = r.html;
    notes.appendChild(body);
    notes.scrollTop = 0;
    notes.setAttribute('aria-labelledby', list[i].id);
    if (focus) { list[i].focus(); }
    ensureVisible(list[i]);
    placeYou();
    updateNav();
  }
  function render(m) {
    model = m;
    strip.textContent = '';
    m.releases.forEach(function (r, i) {
      var b = make('button', 'tab ' + r.role + ' ' + r.kind);
      b.type = 'button';
      b.id = 'tab' + i;
      b.tabIndex = -1;
      b.title = r.tooltip;
      b.setAttribute('role', 'tab');
      b.setAttribute('aria-controls', 'notes');
      b.setAttribute('aria-selected', 'false');
      b.appendChild(make('span', 'ver', r.label));
      if (r.role === 'update') { b.appendChild(make('span', 'dot', DOT)); }
      if (r.kindShort) { b.appendChild(make('span', 'badge', r.kindShort)); }
      b.addEventListener('click', function () { select(i, true); });
      strip.appendChild(b);
    });
    youMark.textContent = UP;
    youText.textContent = m.you.label;
    status.textContent = m.status || '';
    status.hidden = !m.status;
    if (m.releases.length) { select(Math.min(Math.max(m.selected, 0), m.releases.length - 1), false); }
    revealStart();
    placeYou();
    updateNav();
  }
  strip.addEventListener('keydown', function (e) {
    if (!model || !model.releases.length) { return; }
    var n = model.releases.length, i = selected;
    if (e.key === 'ArrowLeft') { i = Math.max(0, i - 1); }
    else if (e.key === 'ArrowRight') { i = Math.min(n - 1, i + 1); }
    else if (e.key === 'Home') { i = 0; }
    else if (e.key === 'End') { i = n - 1; }
    else { return; }
    e.preventDefault();
    select(i, true);
  });
  strip.addEventListener('scroll', function () { placeYou(); updateNav(); });
  strip.addEventListener('wheel', function (e) {
    if (Math.abs(e.deltaY) > Math.abs(e.deltaX)) { strip.scrollLeft += e.deltaY; e.preventDefault(); }
  }, { passive: false });
  prev.addEventListener('click', function () { strip.scrollBy({ left: -strip.clientWidth * 0.7, behavior: 'smooth' }); });
  next.addEventListener('click', function () { strip.scrollBy({ left: strip.clientWidth * 0.7, behavior: 'smooth' }); });
  window.addEventListener('resize', function () { placeYou(); updateNav(); });
  // Every link leaves the page through the window, which opens only http(s) addresses in the user's browser; an in-page
  // anchor scrolls instead. The window also refuses any navigation of its own, so a link the script missed goes nowhere.
  function linkOf(e) { return e.target && e.target.closest ? e.target.closest('a') : null; }
  function follow(a) {
    var href = a.getAttribute('href') || '';
    if (href.charAt(0) === '#') {
      var target = document.getElementById(decodeURIComponent(href.slice(1)));
      if (target) { target.scrollIntoView(); }
      return;
    }
    if (a.href) { post({ type: 'open', url: a.href }); }
  }
  document.addEventListener('click', function (e) { var a = linkOf(e); if (a) { e.preventDefault(); follow(a); } });
  document.addEventListener('auxclick', function (e) { var a = linkOf(e); if (a && e.button === 1) { e.preventDefault(); follow(a); } });
  document.addEventListener('keydown', function (e) { if (e.key === 'Escape') { e.preventDefault(); post({ type: 'escape' }); } });
  if (host) {
    host.addEventListener('message', function (e) {
      var m = e.data;
      if (m && m.type === 'model' && m.model) { render(m.model); }
    });
  }
  post({ type: 'ready' });
})();
)AMPAGE";

        inline constexpr const wchar_t* kNotesPageTail = LR"AMPAGE(</script>
</body>
</html>
)AMPAGE";

        // A font face name made safe for the page's CSS: only letters, digits, spaces and '-' survive, so a strange face
        // name can never end the declaration or the style element.
        //   face    — the face name.
        //   returns — the name quoted for CSS ("Segoe UI"), or the generic "system-ui" when nothing survives.
        //   throws  — std::bad_alloc only.
        inline std::wstring CssFontFamily(std::wstring_view face)
        {
            std::wstring clean;
            for (const wchar_t c : face)
            {
                if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L' ' || c == L'-')
                {
                    clean += c;
                }
            }
            if (TrimBlanks(clean).empty())
            {
                return L"system-ui";
            }
            return L"\"" + clean + L"\"";
        }
    }

    // The notes page: a fixed document with no release in it. Its script asks the window for the model ({"type":"ready"}),
    // renders the tab bar and the selected release's notes from it, and renders again on every later model; links leave
    // through the window ({"type":"open","url":...}) and Esc asks it to close ({"type":"escape"}).
    //   nonce — the content-security policy's nonce (NewReleaseNotesNonce): only the page's own script and style run.
    //   style — the dialog's font, so the HTML column reads like the Win32 one.
    //   returns — the document, for ICoreWebView2::NavigateToString (well under its 2 MB limit).
    //   throws  — std::bad_alloc only.
    inline std::wstring BuildReleaseNotesPage(std::wstring_view nonce, const ReleaseNotesPageStyle& style)
    {
        std::wstring page = detail::kNotesPageHead;
        page += detail::kNotesPageCss;
        page += detail::kNotesPageBody;
        page += detail::kNotesPageScript;
        page += detail::kNotesPageTail;
        detail::ReplaceAll(page, L"{{NONCE}}", nonce);
        detail::ReplaceAll(page, L"{{FONT_FAMILY}}", detail::CssFontFamily(style.fontFamily));
        detail::ReplaceAll(page, L"{{FONT_PX}}", std::to_wstring((std::max)(8, (std::min)(style.fontPx, 40))));
        return page;
    }

    // ============================ the plain text ============================

    // The notes as plain text, for the box the window shows when WebView2 cannot run: every UPDATE, newest first (what the
    // user gets by updating), each under its meta line and its page; every release, newest first, when none is an update.
    //   timeline — the tabs.
    //   reason   — why the notes are plain text, for the first line ("the Microsoft Edge WebView2 Runtime is not installed").
    //   returns  — the text with CRLF line breaks, as a Win32 edit control needs them.
    //   throws   — std::bad_alloc only.
    inline std::wstring BuildReleaseNotesPlainText(const ReleaseTimeline& timeline, std::wstring_view reason)
    {
        std::wstring text = L"Release notes, as plain text: " + std::wstring{ reason } + L".\n";
        const bool anyUpdate = std::any_of(timeline.roles.begin(), timeline.roles.end(), [](ReleaseRole r) { return r == ReleaseRole::Update; });
        for (size_t k = timeline.releases.size(); k-- > 0;)
        {
            const ReleaseRole role = k < timeline.roles.size() ? timeline.roles[k] : ReleaseRole::Older;
            if (anyUpdate && role != ReleaseRole::Update)
            {
                continue;
            }
            const ReleaseNote& r = timeline.releases[k];
            bool cut = false;
            std::wstring body = CapMarkdown(StripInstallSection(r.body), kReleaseNotesMaxMarkdownChars, cut);
            if (detail::TrimBlanks(body).empty())
            {
                body = L"(This release has no notes.)";
            }
            text += L"\n" + ReleaseMetaLine(r) + (role == ReleaseRole::Update ? L" \x2014 an update you get" : L"") + L"\n";
            text += r.htmlUrl + L"\n";
            text += std::wstring(48, L'\x2500') + L"\n";
            text += body + L"\n";
            if (cut)
            {
                text += L"(The notes continue on the release page.)\n";
            }
        }
        // An edit control breaks lines only at CRLF: normalize every LF and lone CR the notes carry.
        std::wstring crlf;
        crlf.reserve(text.size() + text.size() / 16);
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (c == L'\r')
            {
                crlf += L"\r\n";
                if (i + 1 < text.size() && text[i + 1] == L'\n')
                {
                    ++i;
                }
            }
            else if (c == L'\n')
            {
                crlf += L"\r\n";
            }
            else
            {
                crlf += c;
            }
        }
        return crlf;
    }
}
