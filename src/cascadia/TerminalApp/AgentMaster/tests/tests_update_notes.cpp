// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — UPDATER.md "The prompt window": the update prompt's RELEASE NOTES and its window's LAYOUT (UpdaterReleaseNotes.h and
// UpdaterPromptWindow.h, the parts of Updater.h):
//   1. the list page: drafts, tagless and off-channel releases left out, the fields read, a foreign page URL rebuilt;
//   2. the timeline: oldest to newest, the roles, the "you" arrow on a tab or in a gap, one tab per tag (the list's copy
//      wins), equal versions by date, the cap dropping the OLDEST so every update survives;
//   3. the Install section cut: a trailing one, a middle one, headings inside code fences, case, indent, "Installation";
//   4. the HTML (cmark's safe mode: raw HTML omitted, a javascript: link emptied) — a skip without the vcpkg cmark, like
//      TestMarkdownBlocks — and the length cap, the escape;
//   5. the date, the page model's JSON read back;
//   6. the page: its content-security policy, the nonce on its script and style, every placeholder filled, a hostile font
//      face sanitized;
//   7. the plain text: the updates newest first, CRLF only;
//   8. the window's layout at 100% and 150%, below its minimum;
//   9. DialogEcho::ShowCustomDialog runs its dialog once and returns its answer;
//  10. the link and kind helpers.
// The window itself (WebView2, the dark controls, the modal loop) needs a desktop: UPDATER.md "The prompt window", "Check after the build".

#include "m5_tests.h"

#include "../Updater.h"

#include <algorithm>

namespace
{
    namespace U = ::Agentmaster::Updater;

    // A release object as GitHub's list sends it (only the fields the notes read).
    json::Value Release(const std::wstring& tag, bool prerelease, bool draft, const std::wstring& body, const std::wstring& published = L"2026-10-29T05:37:58Z",
                        const std::wstring& htmlUrl = L"")
    {
        json::Value r = json::Value::MkObj();
        r.Set(L"tag_name", json::Value::MkStr(tag));
        r.Set(L"name", json::Value::MkStr(L"Agentmaster " + tag.substr(tag.empty() ? 0 : 1)));
        r.Set(L"prerelease", json::Value::MkBool(prerelease));
        r.Set(L"draft", json::Value::MkBool(draft));
        r.Set(L"published_at", json::Value::MkStr(published));
        r.Set(L"html_url", json::Value::MkStr(htmlUrl.empty() ? L"https://github.com/Nucs/Agentmaster/releases/tag/" + tag : htmlUrl));
        r.Set(L"body", json::Value::MkStr(body));
        return r;
    }

    // A release as the notes hold it.
    U::ReleaseNote Note(const std::wstring& tag, U::ReleaseKind kind = U::ReleaseKind::Stable, const std::wstring& published = L"")
    {
        U::ReleaseNote n;
        n.tag = tag;
        n.version = U::ParseVersion(tag);
        n.kind = kind;
        n.publishedAt = published;
        n.htmlUrl = U::ReleasePageForTag(tag);
        n.body = L"## Notes of " + tag + L"\n\n- a change\n";
        return n;
    }

    bool Contains(const std::wstring& haystack, const std::wstring& needle)
    {
        return haystack.find(needle) != std::wstring::npos;
    }

    size_t CountOf(const std::wstring& haystack, const std::wstring& needle)
    {
        size_t n = 0;
        for (size_t at = haystack.find(needle); at != std::wstring::npos; at = haystack.find(needle, at + needle.size()))
        {
            ++n;
        }
        return n;
    }
}

void TestUpdateReleaseNotes()
{
    std::wprintf(L"Update prompt: release notes + window layout (UpdaterReleaseNotes.h / UpdaterPromptWindow.h):\n");

    // ---- 1. the list page ----
    {
        json::Value page = json::Value::MkArr();
        page.Push(Release(L"v0.8.2", false, false, L"stable notes"));
        page.Push(Release(L"v0.8.2-prerelease-nightly", true, false, L"nightly"));
        page.Push(Release(L"v0.8.1-prerelease", true, false, L"pre"));
        page.Push(Release(L"v0.8.0", false, true, L"a draft"));
        json::Value tagless = Release(L"v0.7.8", false, false, L"x");
        tagless.members[0].second = json::Value::MkStr(L"");
        page.Push(tagless);
        page.Push(Release(L"v0.7.9", false, false, L"elsewhere", L"2026-10-01T00:00:00Z", L"https://evil.example/releases/x"));

        const auto stable = U::ParseReleaseListPage(page, false, false);
        CHECK(stable.size() == 2 && stable[0].tag == L"v0.8.2" && stable[1].tag == L"v0.7.9", "list: the stable channel keeps the stables only (no draft, no tagless release)");
        CHECK(!stable.empty() && stable[0].kind == U::ReleaseKind::Stable && stable[0].name == L"Agentmaster 0.8.2" && stable[0].body == L"stable notes" &&
                  stable[0].publishedAt == L"2026-10-29T05:37:58Z" && stable[0].version.minor == 8 && stable[0].version.patch == 2,
              "list: the fields are read");
        CHECK(stable.size() == 2 && stable[1].htmlUrl == L"https://github.com/Nucs/Agentmaster/releases/tag/v0.7.9", "list: a page URL outside our releases is rebuilt from the tag");
        const auto pre = U::ParseReleaseListPage(page, true, false);
        CHECK(pre.size() == 3 && pre[1].tag == L"v0.8.1-prerelease" && pre[1].kind == U::ReleaseKind::Prerelease, "list: the pre-release opt-in adds the pre-release, never the nightly");
        const auto all = U::ParseReleaseListPage(page, true, true);
        CHECK(all.size() == 4 && all[1].kind == U::ReleaseKind::Nightly, "list: both opt-ins add the nightly, its kind by tag");
        const auto nightlyOnly = U::ParseReleaseListPage(page, false, true);
        CHECK(nightlyOnly.size() == 3 && nightlyOnly[1].kind == U::ReleaseKind::Nightly, "list: the nightly opt-in alone adds the nightly, not the pre-release");
        CHECK(U::ParseReleaseListPage(json::Value::MkObj(), true, true).empty(), "list: a page that is no array gives none");
    }

    // ---- 2. the timeline ----
    {
        const U::Version current = U::ParseVersion(L"0.7.10");
        std::vector<U::ReleaseNote> notes;
        notes.push_back(Note(L"v0.8.1"));
        notes.push_back(Note(L"v0.7.9"));
        notes.push_back(Note(L"v0.8.0"));
        notes.push_back(Note(L"v0.7.10-prerelease", U::ReleaseKind::Prerelease));
        notes.push_back(Note(L"v0.7.7"));
        const auto t = U::BuildReleaseTimeline(notes, current);
        CHECK(t.releases.size() == 5 && t.releases.front().tag == L"v0.7.7" && t.releases.back().tag == L"v0.8.1", "timeline: oldest to newest, whatever the input order");
        CHECK(t.roles.size() == 5 && t.roles[0] == U::ReleaseRole::Older && t.roles[1] == U::ReleaseRole::Older && t.roles[2] == U::ReleaseRole::Yours &&
                  t.roles[3] == U::ReleaseRole::Update && t.roles[4] == U::ReleaseRole::Update,
              "timeline: older / yours / update by version");
        CHECK(t.youOnTab && t.youIndex == 2 && t.selected == 4, "timeline: the arrow on your tab, the newest release selected");

        std::vector<U::ReleaseNote> gapNotes;
        gapNotes.push_back(Note(L"v0.8.1"));
        gapNotes.push_back(Note(L"v0.7.9"));
        gapNotes.push_back(Note(L"v0.8.0"));
        const auto gap = U::BuildReleaseTimeline(gapNotes, U::ParseVersion(L"0.7.11"));
        CHECK(!gap.youOnTab && gap.youIndex == 1 && gap.releases[1].tag == L"v0.8.0", "timeline: a version that is no release puts the arrow in the gap before the first update");

        U::ReleaseNote listCopy = Note(L"v0.8.1");
        listCopy.name = L"from the list";
        U::ReleaseNote offeredCopy = Note(L"V0.8.1");
        std::vector<U::ReleaseNote> twice;
        twice.push_back(listCopy);
        twice.push_back(offeredCopy);
        const auto dedup = U::BuildReleaseTimeline(twice, current);
        CHECK(dedup.releases.size() == 1 && dedup.releases[0].name == L"from the list", "timeline: one tab per tag (any case); the first copy (the list's) wins");

        std::vector<U::ReleaseNote> sameNumber;
        sameNumber.push_back(Note(L"v0.9.0", U::ReleaseKind::Stable, L"2026-11-02T00:00:00Z"));
        sameNumber.push_back(Note(L"v0.9.0-beta", U::ReleaseKind::Prerelease, L"2026-10-30T00:00:00Z"));
        const auto bySameNumber = U::BuildReleaseTimeline(sameNumber, current);
        CHECK(bySameNumber.releases.size() == 2 && bySameNumber.releases[0].tag == L"v0.9.0-beta" && bySameNumber.releases[1].tag == L"v0.9.0",
              "timeline: equal versions are ordered by their publish date");

        std::vector<U::ReleaseNote> many;
        for (int i = 1; i <= 40; ++i)
        {
            many.push_back(Note(L"v1.0." + std::to_wstring(i)));
        }
        const auto capped = U::BuildReleaseTimeline(many, U::ParseVersion(L"1.0.35"), 30);
        const auto cappedUpdates = std::count(capped.roles.begin(), capped.roles.end(), U::ReleaseRole::Update);
        CHECK(capped.releases.size() == 30 && capped.releases.front().tag == L"v1.0.11" && capped.releases.back().tag == L"v1.0.40", "timeline: the cap drops the OLDEST releases");
        CHECK(cappedUpdates == 5 && capped.youOnTab && capped.releases[capped.youIndex].tag == L"v1.0.35", "timeline: every update survives the cap, and your tab with them");
        const auto allUpdates = U::BuildReleaseTimeline(many, U::ParseVersion(L"0.1.0"), 30);
        CHECK(allUpdates.releases.size() == 30 && allUpdates.releases.back().tag == L"v1.0.40" && !allUpdates.youOnTab && allUpdates.youIndex == 0,
              "timeline: more updates than tabs keeps the newest 30, the arrow before the first");
        CHECK(U::BuildReleaseTimeline({}, current).releases.empty(), "timeline: no release, no tab");
    }

    // ---- 3. the Install section ----
    {
        const std::wstring trailing = L"## Agentmaster 0.8.1\n\nIntro.\n\n## Fixes\n\n- a fix\n\n## Install\n\nRun:\n\n```powershell\n## Install inside code\nirm x\n```\n\n- a step\n";
        const std::wstring cut = U::StripInstallSection(trailing);
        CHECK(Contains(cut, L"## Fixes") && Contains(cut, L"- a fix") && !Contains(cut, L"Install") && !Contains(cut, L"irm x") && !Contains(cut, L"a step"),
              "install: the trailing section goes, its code block with it");
        CHECK(!cut.empty() && cut.back() == L'x', "install: the blank lines before the cut are trimmed");
        const std::wstring middle = U::StripInstallSection(L"# T\n\n## Install\n\nsteps\n\n### Install sub\n\nmore\n\n## After\n\nkept\n");
        CHECK(Contains(middle, L"## After") && Contains(middle, L"kept") && !Contains(middle, L"steps") && !Contains(middle, L"more"),
              "install: a middle section ends at the next heading of its level; a deeper one stays cut");
        CHECK(U::StripInstallSection(L"Text\n\n```\n## Install\n```\n\nafter\n") == L"Text\n\n```\n## Install\n```\n\nafter", "install: a heading inside a code fence is code");
        CHECK(U::StripInstallSection(L"~~~~\n## Install\n~~~\nstill code\n~~~~\nout\n") == L"~~~~\n## Install\n~~~\nstill code\n~~~~\nout",
              "install: a shorter fence does not close a longer one");
        CHECK(U::StripInstallSection(L"## install ##\nx\n## Next\ny") == L"## Next\ny", "install: any case, a closing run of #s");
        CHECK(U::StripInstallSection(L"    ## Install\nx") == L"    ## Install\nx", "install: four spaces of indent make it code, not a heading");
        CHECK(U::StripInstallSection(L"## Installation\nx") == L"## Installation\nx", "install: only the word Install itself");
        CHECK(U::StripInstallSection(L"#Install\nx") == L"#Install\nx", "install: '#Install' without a space is text");
        CHECK(U::StripInstallSection(L"a\r\n## Install\r\nb\r\n## More\r\nc") == L"a\r\n## More\r\nc", "install: CRLF notes");
        CHECK(U::StripInstallSection(L"").empty(), "install: empty notes");
    }

    // ---- 4. the HTML ----
    {
        const std::wstring html = U::ReleaseNotesHtml(
            L"## New\n\n- **bold** and `code`\n  - nested\n\n<script>alert(1)</script>\n\n[x](javascript:alert(1)) [y](https://github.com/Nucs/Agentmaster)\n\n## Install\n\nsecret\n");
#if AGENTMASTER_UPDATER_HAVE_CMARK
        CHECK(Contains(html, L"<h2>New</h2>") && Contains(html, L"<strong>bold</strong>") && Contains(html, L"<code>code</code>"), "html: cmark renders the headings, the strong text, the code");
        CHECK(CountOf(html, L"<ul>") == 2, "html: a nested list is a nested <ul>");
        CHECK(!Contains(html, L"<script") && Contains(html, L"raw HTML omitted"), "html: raw HTML is omitted (cmark's safe mode)");
        CHECK(!Contains(html, L"javascript:") && Contains(html, L"href=\"https://github.com/Nucs/Agentmaster\""), "html: a javascript: link is emptied, an https one kept");
        CHECK(U::ReleaseNotesRendererName().rfind(L"cmark ", 0) == 0, "html: cmark renders in this build");
#else
        std::wprintf(L"  [skip] the cmark checks need the vcpkg cmark of a full build (obj\\x64\\vcpkg)\n");
        CHECK(Contains(html, L"<pre>") && Contains(html, L"&lt;script&gt;"), "html (no cmark): the notes as escaped preformatted text");
#endif
        CHECK(!Contains(html, L"secret"), "html: the Install section is cut before rendering");
        CHECK(Contains(U::ReleaseNotesHtml(L"  \n\n"), L"no notes"), "html: empty notes say so");
        CHECK(Contains(U::ReleaseNotesHtml(L"## Install\n\nonly install\n"), L"no notes"), "html: notes that are all Install say so too");
        bool cut = false;
        const std::wstring capped = U::CapMarkdown(L"line one\nline two\nline three", 15, cut);
        CHECK(cut && capped == L"line one", "html: a long body is cut at the end of a line");
        bool notCut = true;
        CHECK(U::CapMarkdown(L"short", 15, notCut) == L"short" && !notCut, "html: a short body is kept whole");
        CHECK(U::HtmlEscape(L"<a href=\"x\">'&'</a>") == L"&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;", "html: the escape covers & < > \" '");
    }

    // ---- 5. the date and the page model ----
    {
        CHECK(U::FormatReleaseDate(L"").empty() && U::FormatReleaseDate(L"not-a-date-at-all-xx").empty(), "date: text that is no timestamp gives nothing");
        CHECK(U::FormatReleaseDate(L"2026-13-01T00:00:00Z").empty() && U::FormatReleaseDate(L"2026-02-30T00:00:00Z").empty(), "date: an impossible date gives nothing");
        const std::wstring date = U::FormatReleaseDate(L"2026-10-29T12:00:00Z");
        CHECK(!date.empty() && (Contains(date, L"2026") || Contains(date, L"26")), "date: a GitHub timestamp in the user's short date");
        CHECK(U::ReleaseMetaLine(Note(L"v0.8.1", U::ReleaseKind::Prerelease)) == L"v0.8.1 \x00B7 pre-release", "meta: tag and kind, no date when it is unknown");
        CHECK(U::ReleaseTabLabel(Note(L"v0.7.10-prerelease", U::ReleaseKind::Prerelease)) == L"v0.7.10" && U::ReleaseTabLabel(Note(L"v1.0.0-hotfix")) == L"v1.0.0-hotfix" &&
                  U::ReleaseTabLabel(Note(L"-odd", U::ReleaseKind::Nightly)) == L"-odd",
              "label: a pre-release / nightly tab cuts its tag at the first '-', a stable one keeps it whole");

        std::vector<U::ReleaseNote> notes;
        notes.push_back(Note(L"v0.7.9"));
        notes.push_back(Note(L"v0.8.0-prerelease-nightly", U::ReleaseKind::Nightly));
        notes.push_back(Note(L"v0.8.1", U::ReleaseKind::Stable, L"2026-10-29T05:37:58Z"));
        const auto t = U::BuildReleaseTimeline(notes, U::ParseVersion(L"0.7.9"));
        const auto model = json::Parse(U::BuildReleaseNotesModelJson(t, U::ParseVersion(L"0.7.9"), L"Loading"));
        const json::Value* releases = model ? model->Find(L"releases") : nullptr;
        CHECK(model && releases && releases->type == json::Value::Type::Arr && releases->arr.size() == 3, "model: valid JSON with one release per tab");
        if (releases && releases->arr.size() == 3)
        {
            const json::Value& yours = releases->arr[0];
            const json::Value& nightly = releases->arr[1];
            const json::Value& newest = releases->arr[2];
            CHECK(yours.StrAt(L"role") == L"yours" && yours.StrAt(L"roleText") == L"Your version" && yours.StrAt(L"kindShort").empty(), "model: your release, no badge for a stable");
            CHECK(nightly.StrAt(L"role") == L"update" && nightly.StrAt(L"kind") == L"nightly" && nightly.StrAt(L"kindShort") == L"nightly", "model: a nightly update with its badge");
            CHECK(nightly.StrAt(L"label") == L"v0.8.0" && Contains(nightly.StrAt(L"meta"), L"v0.8.0-prerelease-nightly"),
                  "model: a nightly's tab shows its short version (the badge says nightly), its header the whole tag");
            CHECK(newest.StrAt(L"label") == L"v0.8.1" && newest.StrAt(L"url") == L"https://github.com/Nucs/Agentmaster/releases/tag/v0.8.1", "model: the label and the page");
            CHECK(Contains(newest.StrAt(L"html"), L"Notes of v0.8.1") && Contains(newest.StrAt(L"tooltip"), L"an update you get"), "model: the rendered notes and the tooltip");
        }
        const json::Value* you = model ? model->Find(L"you") : nullptr;
        CHECK(you && you->NumAt(L"index", -1) == 0 && you->BoolAt(L"onTab", false) && you->StrAt(L"label") == L"you", "model: the arrow on your tab");
        CHECK(model && model->NumAt(L"selected", -1) == 2 && model->StrAt(L"status") == L"Loading", "model: the newest selected, the status line");

        const auto gapModel = json::Parse(U::BuildReleaseNotesModelJson(U::BuildReleaseTimeline(notes, U::ParseVersion(L"0.7.95")), U::ParseVersion(L"0.7.95"), L""));
        const json::Value* gapYou = gapModel ? gapModel->Find(L"you") : nullptr;
        CHECK(gapYou && !gapYou->BoolAt(L"onTab", true) && gapYou->StrAt(L"label") == L"you (v0.7.95)", "model: an arrow in a gap names your version");
    }

    // ---- 6. the page ----
    {
        U::ReleaseNotesPageStyle style;
        style.fontFamily = L"Segoe UI</style><script>";
        style.fontPx = 12;
        const std::wstring page = U::BuildReleaseNotesPage(L"abc123", style);
        CHECK(Contains(page, L"script-src 'nonce-abc123'") && Contains(page, L"style-src 'nonce-abc123'") && Contains(page, L"default-src 'none'"), "page: the content-security policy");
        CHECK(Contains(page, L"<script nonce=\"abc123\">") && Contains(page, L"<style nonce=\"abc123\">") && CountOf(page, L"<script") == 1, "page: one script, carrying the nonce");
        CHECK(!Contains(page, L"{{"), "page: every placeholder is filled");
        CHECK(Contains(page, L"font: 12px/1.5 \"Segoe UIstylescript\"") && CountOf(page, L"</style>") == 1, "page: a hostile font face cannot end the style element");
        U::ReleaseNotesPageStyle huge;
        huge.fontPx = 400;
        CHECK(Contains(U::BuildReleaseNotesPage(L"n", huge), L"font: 40px/1.5"), "page: the font size is clamped");
        const std::wstring a = U::NewReleaseNotesNonce();
        const std::wstring b = U::NewReleaseNotesNonce();
        CHECK(a.size() == 32 && a != b && std::all_of(a.begin(), a.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }),
              "page: a fresh 32-hex-digit nonce each time");
    }

    // ---- 7. the plain text ----
    {
        std::vector<U::ReleaseNote> notes;
        notes.push_back(Note(L"v0.7.9"));
        notes.push_back(Note(L"v0.8.0"));
        notes.push_back(Note(L"v0.8.1"));
        notes.back().body = L"## New\n\nnewest line\n\n## Install\n\nnot here\n";
        const auto t = U::BuildReleaseTimeline(notes, U::ParseVersion(L"0.7.9"));
        const std::wstring text = U::BuildReleaseNotesPlainText(t, L"no runtime");
        CHECK(text.rfind(L"Release notes, as plain text: no runtime.\r\n", 0) == 0, "plain: the reason first");
        const size_t at081 = text.find(L"v0.8.1 \x00B7");
        const size_t at080 = text.find(L"v0.8.0 \x00B7");
        CHECK(at081 != std::wstring::npos && at080 != std::wstring::npos && at081 < at080 && !Contains(text, L"v0.7.9 \x00B7"), "plain: the updates newest first, yours left out");
        CHECK(Contains(text, L"newest line") && !Contains(text, L"not here"), "plain: the notes without their Install section");
        bool loneLf = false;
        for (size_t i = 0; i < text.size(); ++i)
        {
            loneLf = loneLf || (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'));
        }
        CHECK(!loneLf, "plain: CRLF line breaks only (an edit control's)");
        const std::wstring none = U::BuildReleaseNotesPlainText(U::BuildReleaseTimeline(notes, U::ParseVersion(L"9.0.0")), L"r");
        CHECK(Contains(none, L"v0.8.1 \x00B7") && Contains(none, L"v0.7.9 \x00B7"), "plain: with no update, every release");
    }

    // ---- 8. the window's layout ----
    {
        U::PromptLayoutMetrics m;
        m.dpi = 96;
        m.clientWidth = 1040;
        m.clientHeight = 640;
        m.instructionHeight = 22;
        m.contentHeight = 120;
        m.radioHeight = 22;
        m.radioCount = 6;
        m.buttonWidths = { 100, 110 };
        m.buttonHeight = 30;
        m.linkWidth = 110;
        m.linkHeight = 21;
        const auto l = U::ComputePromptLayout(m);
        const int columnLeft = 1040 - 16 - 344;
        CHECK(l.notes.left == 16 && l.notes.top == 16 && l.notes.right == columnLeft - 16 && l.notes.bottom == 640 - 54 - 16, "layout: the notes take everything left of the column");
        CHECK(l.content.left == columnLeft && l.content.right == 1040 - 16 && l.icon.left == columnLeft && l.instruction.left == columnLeft + 32 + 10,
              "layout: the column keeps its width against the right margin; the instruction beside the icon");
        CHECK(l.instruction.top == 16 + (32 - 22) / 2, "layout: a one-line instruction is centred on the icon");
        bool stacked = l.radios.size() == 6;
        for (size_t i = 1; stacked && i < l.radios.size(); ++i)
        {
            stacked = l.radios[i].top >= l.radios[i - 1].bottom;
        }
        CHECK(stacked && !l.radios.empty() && l.radios.front().top >= l.content.bottom && l.radios.back().bottom <= l.band.top - 16, "layout: the radios stack under the text, above the band");
        CHECK(l.band.top == 640 - 54 && l.band.bottom == 640 && l.buttons.size() == 2 && l.buttons[1].right == 1040 - 16 && l.buttons[0].right == l.buttons[1].left - 8 &&
                  l.buttons[0].right - l.buttons[0].left == 100 && l.buttons[1].top >= l.band.top && l.buttons[1].bottom <= l.band.bottom,
              "layout: the buttons right-aligned in the band, in order");
        CHECK(l.link.left == 16 && l.link.top >= l.band.top && l.link.bottom <= l.band.bottom, "layout: the link at the band's left");

        U::PromptLayoutMetrics tiny = m;
        tiny.clientWidth = 100;
        tiny.clientHeight = 100;
        const auto lt = U::ComputePromptLayout(tiny);
        CHECK(lt.minClientWidth == l.minClientWidth && lt.band.bottom == lt.minClientHeight && lt.band.right == lt.minClientWidth && lt.radios.back().bottom <= lt.band.top - 16,
              "layout: below the minimum it lays out AT the minimum, nothing overlapping");
        CHECK(l.minClientWidth == 16 + 380 + 16 + 344 + 16, "layout: the minimum width keeps a usable notes column");

        U::PromptLayoutMetrics hi = m;
        hi.dpi = 144;
        hi.clientWidth = 1560;
        hi.clientHeight = 960;
        const auto lh = U::ComputePromptLayout(hi);
        CHECK(lh.notes.left == 24 && lh.content.right - lh.content.left == 516 && lh.band.top == 960 - 81, "layout: 150% scales the margins, the column and the band");
        CHECK(U::PromptScale(16, 0) == 16 && U::PromptScale(16, 192) == 32, "layout: a DPI of 0 reads as 96");
    }

    // ---- 9. the custom dialog echo ----
    {
        int runs = 0;
        const int answer = DialogEcho::ShowCustomDialog(
            DialogEcho::Description{ L"title", { L"line" } },
            [&runs]() noexcept {
                ++runs;
                return 42;
            },
            [](int value) { return std::to_wstring(value); });
        CHECK(answer == 42 && runs == 1, "echo: a self-drawn dialog runs once and its answer comes back");
    }

    // ---- 10. the helpers ----
    {
        CHECK(U::detail::IsWebAddress(L"https://github.com/x") && U::detail::IsWebAddress(L"HTTP://example.com/a?b=c"), "links: http(s) addresses open");
        CHECK(!U::detail::IsWebAddress(L"file:///C:/x") && !U::detail::IsWebAddress(L"javascript:alert(1)") && !U::detail::IsWebAddress(L"https://a b") &&
                  !U::detail::IsWebAddress(L"https://") && !U::detail::IsWebAddress(L"https://a\"b"),
              "links: other schemes, blanks, quotes and an empty address are refused");
        CHECK(U::IsOwnReleasePage(L"https://github.com/Nucs/Agentmaster/releases/tag/v1") && !U::IsOwnReleasePage(L"https://github.com/Other/repo/releases/tag/v1") &&
                  !U::IsOwnReleasePage(L"https://github.com/Nucs/Agentmaster/releases/"),
              "links: only our own release pages");
        CHECK(U::ReleaseKindOf(L"v1.0.0-prerelease-NIGHTLY", false) == U::ReleaseKind::Nightly && U::ReleaseKindOf(L"v1.0.0", true) == U::ReleaseKind::Prerelease &&
                  U::ReleaseKindOf(L"v1.0.0", false) == U::ReleaseKind::Stable,
              "kind: a nightly by its tag, a pre-release by its flag");
        U::UpdatePrefs prefs;
        CHECK(U::detail::ChannelWords(prefs) == L"stable", "channel: stable by default");
        prefs.allowPrerelease = true;
        prefs.allowNightly = true;
        CHECK(U::detail::ChannelWords(prefs) == L"stable, pre-release and nightly", "channel: both opt-ins");

        U::UpdateInfo info;
        info.latestTag = L"v0.8.1";
        info.latest = U::ParseVersion(L"v0.8.1");
        info.isPrerelease = true;
        info.notes = L"offered notes";
        info.htmlUrl = L"https://example.com/not-ours";
        const U::ReleaseNote offered = U::OfferedReleaseNote(info);
        CHECK(offered.tag == L"v0.8.1" && offered.kind == U::ReleaseKind::Prerelease && offered.body == L"offered notes" &&
                  offered.htmlUrl == L"https://github.com/Nucs/Agentmaster/releases/tag/v0.8.1",
              "offered: what the check knows, its page our own");
    }
}
