// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.24: the sticky card's buttons, refactored on the user's request of 2026-10-18 —
//   1. two PAGES (MessagingModels.h BuildCardButtonRows / CardButtonFactsFor): Main holds ⚙ Session, and beside it
//      🔄 Restart Session only in Error or after an exit (the user's rule of 2026-10-19) or ▶ Reactivate for a parked tab;
//      ⚙ Session turns the keyboard into the Session page — ⑂ Fork and 🆕 New Session first (the user's placement), then
//      🔄 Restart Session with 🔧 Resume Session beside it in Error, then every other button the card had before (the runner,
//      the queue, the pin, the transcript, the screen as text), 🖼 Screen Image, 📜 All Screen ▸ on a row of its own (§21.39) and
//      ◀ Back; the card's text names the state's action (🔄 Restart Session in Error / after an exit, ▶ Reactivate for a parked tab);
//   1b. §21.39's level picker (the ALL SCREEN page 📜 All Screen ▸ opens: the File Diffs and Last Turn checkboxes, five levels,
//      the default one starred, Back to the Session page; its three checked twins - one per other pair of boxes - whose levels
//      carry +d, +l or +dl, each box flipping only itself), the level words and verbs, /allscreen's arguments (diffs, last), and
//      the default level's setting (Full unless the cog says another);
//   1c. §21.41's ⎋ Esc and 🌙 Deactivate (MessagingSessionControl.h, the user's request of 2026-10-27): the Esc / Deactivate row
//      above Back on the Session page (each button only where it applies), Esc NEVER on Main (the user's rule of 2026-10-28: "The
//      ESC button should be only inside Sessions menu"), the hosting window's two decisions, the answers' words, the texts and
//      the Esc gap;
//   1d. §21.42's screenshot in the card (MessagingCardPicture.h, the user's request of 2026-10-27): the screen's hash, the plan
//      (upload / reuse / keep / none), the caption's clock, the rich figure block, the media list, the photo read back from
//      Telegram's answer, the picture's record across a restart, and the cog's switch; and since 2026-11-01 (the picture follows
//      the status) the card's LOOKS at its screen (CardPictureTrack): a look's unchanged plan, the backoff, the settle look after a
//      change into a state at rest, the look a read that got no screen in time owes, a page flip's look, a look an update
//      overtakes, and the give-up after 8 looks; and the tab's start (the LAUNCH WATCH, the user's request of 2026-11-01): the blank
//      screen test (ScreenSnapshotShowsNothing), an edit that keeps the picture while the watch waits, the ask and its gate, the
//      watch from a blank screen to ONE picture once claude's screen held still, a screen that never holds still, the timeout,
//      one watch per update, a tab parked meanwhile, an edit that overtakes a watch look, and a look that begins the watch;
//   2. 🖼 Screen Image: the terminal's live screen drawn as a picture — the styled snapshot's format (ScreenSnapshot.h, shared
//      with ControlCore.cpp) and the renderer (ScreenImage.h), which draws a REAL PNG here with Direct2D + DirectWrite + WIC; the
//      test decodes it again and reads its pixels.
// The service's half (the callbacks, the keyboard edit, the worker thread, sendPhoto) and the control's snapshot read need
// Telegram and a window: the live check is the dev bot (MESSAGING.md §21.24 "Check after the build").

#include "m5_tests.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm> // (std::max) / (std::min): the <windows.h> macros stay out of the way
#include <cmath>
#include <cstdlib> // std::abs(int)
#include <cstring>

#include "../MessagingModels.h"
#include "../MessagingCardPicture.h" // 21.42: the card's screenshot (the plan, the rich block, the media list, the answer's photo)
#include "../MessagingSessionControl.h" // 21.41: the card's Esc and Deactivate (the window's decisions, the answers' words and texts)
#include "../MessagingTaps.h" // kTapToastMaxChars: every 21.41 text must fit a toast
#include "../ScreenImage.h"
#include "../ScreenSnapshot.h"
#include "../TelegramFormat.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    // A 36-character session id, the longest a card button carries.
    const std::wstring kCardSid = L"0123abcd-4567-89ef-0123-456789abcdef";

    using Rows = std::vector<std::vector<std::pair<std::wstring, std::wstring>>>;

    // Whether any button of `rows` carries exactly this callback data.
    bool HasData(const Rows& rows, const std::wstring& data)
    {
        for (const auto& row : rows)
        {
            for (const auto& [label, d] : row)
            {
                if (d == data)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Whether any button's label contains `text`.
    bool HasLabel(const Rows& rows, const std::wstring& text)
    {
        for (const auto& row : rows)
        {
            for (const auto& [label, d] : row)
            {
                if (label.find(text) != std::wstring::npos)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // The UTF-8 length of a callback data (Telegram counts bytes; the data is ASCII, so this is its length when it is).
    size_t Utf8Length(const std::wstring& s)
    {
        size_t n = 0;
        for (const wchar_t c : s)
        {
            n += c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
        }
        return n;
    }

    // A live session in a given state, its claude started (a parked tab: started = false).
    SessionInfo CardSession(SessionState state, bool started = true)
    {
        SessionInfo s = MakeSession(kCardSid, state);
        s.live = true;
        s.started = started;
        s.title = L"Agentmaster";
        s.autorunner.mode = AutorunnerMode::Full;
        return s;
    }

    // One cell of a test snapshot.
    ScreenCell Cell(std::wstring text, uint32_t fg, uint32_t bg, uint8_t flags = 0, uint8_t width = 1)
    {
        ScreenCell c;
        c.text = std::move(text);
        c.fg = fg;
        c.bg = bg;
        c.flags = flags;
        c.width = width;
        return c;
    }

    // Two cell lists equal in every field.
    bool SameCells(const std::vector<ScreenCell>& a, const std::vector<ScreenCell>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].text != b[i].text || a[i].width != b[i].width || a[i].fg != b[i].fg || a[i].bg != b[i].bg || a[i].flags != b[i].flags)
            {
                return false;
            }
        }
        return true;
    }

    // A decoded picture: 32-bit BGRA rows, top-down.
    struct Decoded
    {
        UINT width{ 0 };
        UINT height{ 0 };
        std::vector<uint8_t> bgra;

        // The pixel at (x, y) as 0xRRGGBB.
        uint32_t At(UINT x, UINT y) const
        {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            return (static_cast<uint32_t>(bgra[i + 2]) << 16) | (static_cast<uint32_t>(bgra[i + 1]) << 8) | bgra[i];
        }
    };

    // Decodes PNG bytes with WIC into 32bppBGRA (the test's own reader: the renderer's output read back by another code path).
    bool DecodePng(const std::string& png, Decoded& out)
    {
        using Microsoft::WRL::ComPtr;
        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        {
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
                SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(converter->GetSize(&out.width, &out.height)) && out.width > 0 && out.height > 0)
            {
                out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
                ok = SUCCEEDED(converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
            }
        }
        if (SUCCEEDED(co))
        {
            ::CoUninitialize();
        }
        return ok;
    }

    // A big-endian u32 of a PNG header field.
    uint32_t Be32(const std::string& s, size_t at)
    {
        return (static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3]));
    }

    // Each channel of two colors within `tolerance` of the other.
    bool Near(uint32_t a, uint32_t b, int tolerance)
    {
        for (int shift = 0; shift <= 16; shift += 8)
        {
            const int ca = static_cast<int>((a >> shift) & 0xFF);
            const int cb = static_cast<int>((b >> shift) & 0xFF);
            if (std::abs(ca - cb) > tolerance)
            {
                return false;
            }
        }
        return true;
    }
}

void TestMessagingCard()
{
    // ASCII only, like the other groups' headers: the console's code page would garble a section sign.
    std::wprintf(L"== TestMessagingCard (MESSAGING.md 21.24: the card's two pages, the Error line, the screen snapshot and its picture; 21.41: Esc and Deactivate) ==\n");

    // ===== 1. the card's two pages =====
    {
        const SessionInfo waiting = CardSession(SessionState::WaitingForInput);
        const CardButtonFacts f = CardButtonFactsFor(waiting);
        CHECK(!f.error && !f.done && !f.reactivatable && f.runnerAuto && !f.pinned, "facts of a waiting session: no state action, the runner Auto, unpinned");

        // Main: the cog alone while nothing is wrong (the user's rule of 2026-10-19: Restart shows outside only in Error / Done).
        const Rows main = BuildCardButtonRows(kCardSid, f, CardPage::Main);
        CHECK(main.size() == 1 && main[0].size() == 1, "Main of a waiting session: the cog alone");
        CHECK(HasData(main, L"c:panel:" + kCardSid) && HasLabel(main, L"Session"), "Main: the cog Session button opens the Session page");
        CHECK(main.size() == 1 && !main[0].empty() && main[0][0].first.rfind(L"\x2699\xFE0F", 0) == 0, "Main: the first button is the cog (with VS16, drawn as the emoji)");
        CHECK(!HasData(main, L"c:restart:" + kCardSid) && !HasData(main, L"c:resume:" + kCardSid), "Main of a waiting session: no Restart, no Resume (both on the Session page)");
        CHECK(!HasData(main, L"c:fork:" + kCardSid) && !HasData(main, L"c:newsession:" + kCardSid), "Main: Fork and New Session are on the Session page (the user's placement)");
        CHECK(!HasData(main, L"c:queue:" + kCardSid) && !HasData(main, L"c:screen:" + kCardSid), "Main: the session's own buttons are on the other page");

        const Rows page = BuildCardButtonRows(kCardSid, f, CardPage::Session);
        CHECK(page.size() == 8, "Session: eight rows while claude runs (21.39 added All Screen's own row, 21.41 the Esc / Deactivate row)");
        CHECK(page.size() == 8 && page[0].size() == 2 && page[0][0].second == L"c:fork:" + kCardSid && page[0][0].first.find(L"Fork") != std::wstring::npos &&
                  page[0][1].second == L"c:newsession:" + kCardSid && page[0][1].first.find(L"New Session") != std::wstring::npos,
              "Session: Fork and New Session first, side by side (right after the cog's tap)");
        CHECK(page.size() == 8 && page[1].size() == 1 && page[1][0].second == L"c:restart:" + kCardSid && page[1][0].first.find(L"Restart Session") != std::wstring::npos,
              "Session: Restart Session next, alone while the session is not in Error");
        CHECK(HasData(page, L"c:runner:" + kCardSid) && HasLabel(page, L"Pause runner"), "Session: the runner button (Auto -> Pause runner)");
        CHECK(HasData(page, L"c:queue:" + kCardSid), "Session: Queue");
        CHECK(HasData(page, L"c:pin:" + kCardSid) && HasLabel(page, L"\U0001F4CC Pin"), "Session: Pin (unpinned)");
        CHECK(HasData(page, L"c:transcript:" + kCardSid), "Session: Transcript");
        CHECK(HasData(page, L"c:screen:" + kCardSid) && HasLabel(page, L"Screen Text"), "Session: Screen Text (the old Screen's verb)");
        CHECK(HasData(page, L"c:screenimg:" + kCardSid) && HasLabel(page, L"Screen Image"), "Session: Screen Image");
        CHECK(page.size() == 8 && page[4].size() == 2 && page[4][0].second == L"c:screen:" + kCardSid && page[4][1].second == L"c:screenimg:" + kCardSid,
              "Session: Screen Text and Screen Image keep their row (21.39 and 21.41 moved no existing button)");
        CHECK(page.size() == 8 && page[5].size() == 1 && page[5][0].second == L"c:allscreen:" + kCardSid && page[5][0].first.find(L"All Screen") != std::wstring::npos &&
                  page[5][0].first.rfind(L"\U0001F4DC", 0) == 0,
              "Session: All Screen alone on its own row under the screen buttons, with the scroll emoji (21.39)");
        CHECK(page.size() == 8 && page[6].size() == 2 && page[6][0].second == L"c:esc:" + kCardSid && page[6][0].first == L"\x238B Esc" &&
                  page[6][1].second == L"c:deactivate:" + kCardSid && page[6][1].first == L"\U0001F319 Deactivate",
              "Session: the Esc and Deactivate row right above Back, Esc first (21.41)");
        CHECK(page.size() == 8 && page[7].size() == 1 && page[7][0].second == L"c:main:" + kCardSid && page[7][0].first.find(L"Back") != std::wstring::npos, "Session: Back alone on the last row");
        CHECK(!HasData(main, L"c:allscreen:" + kCardSid), "Main: no All Screen (it is a Session-page button)");
        CHECK(!HasData(page, L"c:panel:" + kCardSid) && !HasData(page, L"c:resume:" + kCardSid) && !HasData(page, L"c:reactivate:" + kCardSid),
              "Session of a waiting session: no cog, no Resume, no Reactivate");

        SessionInfo paused = waiting;
        paused.autorunner.mode = AutorunnerMode::Off;
        paused.pinned = true;
        const Rows pausedPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(paused), CardPage::Session);
        CHECK(HasLabel(pausedPage, L"Runner auto") && !HasLabel(pausedPage, L"Pause runner"), "Session: a Paused runner offers Runner auto");
        CHECK(HasLabel(pausedPage, L"Unpin"), "Session: a pinned session offers Unpin");

        // Error: Restart Session beside the cog on Main; Restart and Resume side by side on the Session page.
        const SessionInfo error = CardSession(SessionState::Error);
        const CardButtonFacts ef = CardButtonFactsFor(error);
        CHECK(ef.error && !ef.done, "facts of a session in Error");
        const Rows errMain = BuildCardButtonRows(kCardSid, ef, CardPage::Main);
        CHECK(errMain.size() == 1 && errMain[0].size() == 2 && errMain[0][0].second == L"c:panel:" + kCardSid && errMain[0][1].second == L"c:restart:" + kCardSid &&
                  errMain[0][1].first.find(L"Restart Session") != std::wstring::npos,
              "Error, Main: the cog, then Restart Session");
        CHECK(!HasData(errMain, L"c:resume:" + kCardSid) && !HasData(errMain, L"c:reactivate:" + kCardSid), "Error, Main: no Resume Session (it is on the Session page), no Reactivate");
        const Rows errPage = BuildCardButtonRows(kCardSid, ef, CardPage::Session);
        CHECK(errPage.size() == 8 && errPage[1].size() == 2 && errPage[1][0].second == L"c:restart:" + kCardSid && errPage[1][1].second == L"c:resume:" + kCardSid &&
                  errPage[1][1].first.find(L"Resume Session") != std::wstring::npos,
              "Error, Session: Restart Session and Resume Session side by side");
        CHECK(!HasData(errPage, L"c:reactivate:" + kCardSid), "Error, Session: no Reactivate");

        // Done (claude exited): Restart Session on Main too; on the Session page Restart alone (Reactivate would be the same restart).
        const SessionInfo exited = CardSession(SessionState::Done);
        const CardButtonFacts df = CardButtonFactsFor(exited);
        CHECK(df.done && !df.error && df.reactivatable, "facts of a claude that exited");
        const Rows doneMain = BuildCardButtonRows(kCardSid, df, CardPage::Main);
        CHECK(doneMain.size() == 1 && doneMain[0].size() == 2 && doneMain[0][1].second == L"c:restart:" + kCardSid, "Done, Main: the cog, then Restart Session");
        CHECK(!HasData(doneMain, L"c:reactivate:" + kCardSid), "Done, Main: no Reactivate beside Restart (the same restart)");
        const Rows donePage = BuildCardButtonRows(kCardSid, df, CardPage::Session);
        CHECK(donePage.size() == 8 && donePage[1].size() == 1 && donePage[1][0].second == L"c:restart:" + kCardSid, "Done, Session: Restart Session alone");
        CHECK(!HasData(donePage, L"c:resume:" + kCardSid) && !HasData(donePage, L"c:reactivate:" + kCardSid), "Done, Session: no Resume, no Reactivate");

        // A parked tab: Reactivate beside the cog on Main (the button it had before; the rule names only Restart); the Session page
        // has Restart alone (tapped on a parked tab, it starts it in place, as Reactivate does).
        const SessionInfo parked = CardSession(SessionState::WaitingForInput, false);
        const Rows parkedMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parked), CardPage::Main);
        CHECK(parkedMain.size() == 1 && parkedMain[0].size() == 2 && parkedMain[0][1].second == L"c:reactivate:" + kCardSid && parkedMain[0][1].first.find(L"Reactivate") != std::wstring::npos,
              "parked, Main: the cog, then Reactivate");
        CHECK(!HasData(parkedMain, L"c:restart:" + kCardSid), "parked, Main: no Restart (not in Error, not exited)");
        const Rows parkedPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parked), CardPage::Session);
        CHECK(parkedPage.size() == 7 && parkedPage[1].size() == 1 && parkedPage[1][0].second == L"c:restart:" + kCardSid, "parked, Session: Restart Session alone");
        CHECK(!HasData(parkedPage, L"c:reactivate:" + kCardSid) && !HasData(parkedPage, L"c:resume:" + kCardSid), "parked, Session: no Reactivate (Restart starts it), no Resume");

        // A parked tab in Error (a cut-off turn after a crash): the Error rule wins.
        const SessionInfo parkedError = CardSession(SessionState::Error, false);
        const Rows pe = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parkedError), CardPage::Main);
        CHECK(pe.size() == 1 && pe[0].size() == 2 && pe[0][1].second == L"c:restart:" + kCardSid, "a parked tab in Error: Restart Session on Main (Error wins over parked)");
        CHECK(!HasData(pe, L"c:reactivate:" + kCardSid), "a parked tab in Error: no Reactivate on Main");
        const Rows pePage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parkedError), CardPage::Session);
        CHECK(HasData(pePage, L"c:resume:" + kCardSid) && HasData(pePage, L"c:restart:" + kCardSid), "a parked tab in Error: Restart and Resume on the Session page");

        // A closed session: nothing to restart in a tab, so Main is the cog alone.
        SessionInfo closed = CardSession(SessionState::Error);
        closed.live = false;
        const Rows cl = BuildCardButtonRows(kCardSid, CardButtonFactsFor(closed), CardPage::Main);
        CHECK(cl.size() == 1 && cl[0].size() == 1, "a closed session: Main is the cog alone (nothing to restart in a tab)");

        // Telegram's limits: callback data within 64 bytes, ASCII; every button with a label.
        bool fits = true;
        bool labeled = true;
        bool twoARow = true;
        for (const SessionInfo* s : { &waiting, &error, &exited, &parked, &parkedError })
        {
            for (const CardPage p : { CardPage::Main, CardPage::Session, CardPage::AllScreen, CardPage::AllScreenDiffs, CardPage::AllScreenLast, CardPage::AllScreenLastDiffs })
            {
                for (const auto& row : BuildCardButtonRows(kCardSid, CardButtonFactsFor(*s), p))
                {
                    twoARow = twoARow && row.size() <= 2;
                    for (const auto& [label, data] : row)
                    {
                        fits = fits && Utf8Length(data) <= 64 && Utf8Length(data) == data.size();
                        labeled = labeled && !label.empty();
                    }
                }
            }
        }
        CHECK(fits, "every card button's data is ASCII and within Telegram's 64 bytes (a 36-character id)");
        CHECK(labeled, "every card button has a label");
        CHECK(twoARow, "at most two buttons a row (a phone keeps the labels whole)");
        CHECK(CardCallbackData(kCardVerbNewSession, kCardSid).size() == 49 && CardCallbackData(L"reactivate", kCardSid).size() == 49 &&
                  CardCallbackData(L"transcript", kCardSid).size() == 49,
              "the longest data of the Main and Session pages (c:newsession:, c:reactivate:, c:transcript: + a 36-character id) is 49 bytes");
        CHECK(CardCallbackData(CardAllScreenLevelVerb(TurnsLevel::Compact), kCardSid).size() == 56 && CardCallbackData(CardAllScreenLevelVerb(TurnsLevel::Minimal), kCardSid).size() == 56 &&
                  CardCallbackData(CardAllScreenLevelVerb(TurnsLevel::Default), kCardSid).size() == 56,
              "the All Screen picker's unchecked data (c:allscreen-compact: / -minimal: / -default: + a 36-character id) is 56 bytes");
        CHECK(CardCallbackData(CardAllScreenLevelVerb(TurnsLevel::Compact, true), kCardSid).size() == 58 && CardCallbackData(kCardVerbAllScreenDiffs, kCardSid).size() == 50,
              "the All Screen picker's File Diffs data (c:allscreen-compact+d: + a 36-character id) is 58 bytes of 64; the checkbox's c:allscreen+d: 50");
        CHECK(CardCallbackData(CardAllScreenLevelVerb(TurnsLevel::Compact, true, true), kCardSid).size() == 59 &&
                  CardCallbackData(CardAllScreenLevelVerb(TurnsLevel::Compact, false, true), kCardSid).size() == 58 &&
                  CardCallbackData(kCardVerbAllScreenLast, kCardSid).size() == 50 && CardCallbackData(kCardVerbAllScreenLastDiffs, kCardSid).size() == 51,
              "the All Screen picker's longest data (c:allscreen-compact+dl: + a 36-character id) is 59 bytes of 64; Last Turn's level data 58, its page verbs 50 and 51");
    }

    // ===== 1b. MESSAGING.md 21.39 - All Screen's level picker (the ALL SCREEN page) and its default level =====
    {
        // The words, the setting's reading and the button verbs (every pair of the two boxes: File Diffs, Last Turn).
        bool words = true;
        for (const auto& choice : kAllScreenLevels)
        {
            const auto plain = ParseCardAllScreenLevelVerb(CardAllScreenLevelVerb(choice.level));
            const auto withDiffs = ParseCardAllScreenLevelVerb(CardAllScreenLevelVerb(choice.level, true));
            const auto withLast = ParseCardAllScreenLevelVerb(CardAllScreenLevelVerb(choice.level, false, true));
            const auto withBoth = ParseCardAllScreenLevelVerb(CardAllScreenLevelVerb(choice.level, true, true));
            words = words && AllScreenLevelFromWord(choice.word) == choice.level && AllScreenLevelWord(choice.level) == choice.word && plain && plain->level == choice.level &&
                    !plain->diffs && !plain->lastTurn && withDiffs && withDiffs->level == choice.level && withDiffs->diffs && !withDiffs->lastTurn && withLast &&
                    withLast->level == choice.level && !withLast->diffs && withLast->lastTurn && withBoth && withBoth->level == choice.level && withBoth->diffs &&
                    withBoth->lastTurn;
        }
        CHECK(words, "All Screen: every level's word, verb (with each pair of boxes) and level map to each other");
        CHECK(CardAllScreenLevelVerb(TurnsLevel::Full) == L"allscreen-full" && CardAllScreenLevelVerb(TurnsLevel::Full, true) == L"allscreen-full+d" &&
                  CardAllScreenLevelVerb(TurnsLevel::Full, false, true) == L"allscreen-full+l" && CardAllScreenLevelVerb(TurnsLevel::Full, true, true) == L"allscreen-full+dl",
              "All Screen: a level's verb is allscreen-<word>, plus +d (File Diffs), +l (Last Turn) or +dl (both)");
        // The four picker pages: each pair of boxes has its page, its page verb and its log word, and back.
        bool pages = !AllScreenBoxesOf(CardPage::Main) && !AllScreenBoxesOf(CardPage::Session);
        for (const bool d : { false, true })
        {
            for (const bool l : { false, true })
            {
                const CardPage page = AllScreenPageFor(d, l);
                const auto boxes = AllScreenBoxesOf(page);
                pages = pages && boxes && boxes->diffs == d && boxes->lastTurn == l;
            }
        }
        CHECK(pages, "All Screen: AllScreenPageFor and AllScreenBoxesOf map the four pairs of boxes to four pages and back; Main and Session are no picker page");
        CHECK(CardAllScreenPageVerb(false, false) == kCardVerbAllScreen && CardAllScreenPageVerb(true, false) == L"allscreen+d" &&
                  CardAllScreenPageVerb(false, true) == L"allscreen+l" && CardAllScreenPageVerb(true, true) == L"allscreen+dl",
              "All Screen: the page verbs are allscreen, allscreen+d, allscreen+l and allscreen+dl");
        CHECK(std::wstring{ CardPageLogWord(CardPage::Main) } == L"main" && std::wstring{ CardPageLogWord(CardPage::Session) } == L"session" &&
                  std::wstring{ CardPageLogWord(CardPage::AllScreen) } == L"allscreen" && std::wstring{ CardPageLogWord(CardPage::AllScreenDiffs) } == L"allscreen+diffs" &&
                  std::wstring{ CardPageLogWord(CardPage::AllScreenLast) } == L"allscreen+last" && std::wstring{ CardPageLogWord(CardPage::AllScreenLastDiffs) } == L"allscreen+diffs+last",
              "All Screen: hooks.log names each page (the three of before as they were)");
        CHECK(std::size(kAllScreenLevels) == 5 && kAllScreenLevels[0].level == TurnsLevel::Minimal && kAllScreenLevels[4].level == TurnsLevel::Max,
              "All Screen: the five transcript levels, least to most");
        CHECK(!AllScreenLevelFromWord(L"") && !AllScreenLevelFromWord(L"FULL") && !AllScreenLevelFromWord(L"fu") && !AllScreenLevelFromWord(L"fullest"),
              "All Screen: an empty, upper-case or partial word names no level (typed text is lower-cased first)");
        CHECK(kAllScreenDefaultLevel == TurnsLevel::Full && AllScreenLevelSetting(L"") == TurnsLevel::Full && AllScreenLevelSetting(L"bogus") == TurnsLevel::Full &&
                  AllScreenLevelSetting(L"a-word-much-longer-than-any-level-name") == TurnsLevel::Full,
              "All Screen: the default level is Full, and an empty, unknown or overlong stored word reads as Full");
        CHECK(AllScreenLevelSetting(L"compact") == TurnsLevel::Compact && AllScreenLevelSetting(L"Max") == TurnsLevel::Max,
              "All Screen: a stored word picks its level, in any case");
        CHECK(!ParseCardAllScreenLevelVerb(kCardVerbAllScreen) && !ParseCardAllScreenLevelVerb(kCardVerbAllScreenDiffs) && !ParseCardAllScreenLevelVerb(L"allscreen-") &&
                  !ParseCardAllScreenLevelVerb(L"allscreen-bogus") && !ParseCardAllScreenLevelVerb(L"allscreen-+d") && !ParseCardAllScreenLevelVerb(L"allscreen-full+x") &&
                  !ParseCardAllScreenLevelVerb(L"screenimg"),
              "All Screen: the page verbs (they open the picker) and malformed ones name no level");
        CHECK(!ParseCardAllScreenLevelVerb(kCardVerbAllScreenLast) && !ParseCardAllScreenLevelVerb(kCardVerbAllScreenLastDiffs) &&
                  !ParseCardAllScreenLevelVerb(L"allscreen-full+ld") && !ParseCardAllScreenLevelVerb(L"allscreen-full+dd") && !ParseCardAllScreenLevelVerb(L"allscreen-full+") &&
                  !ParseCardAllScreenLevelVerb(L"allscreen-+l") && !ParseCardAllScreenLevelVerb(L"allscreen-+dl") && !ParseCardAllScreenLevelVerb(L"allscreen-full+d+l"),
              "All Screen: Last Turn's page verbs name no level, and only the exact endings +d, +l, +dl are boxes");

        // /allscreen's arguments: a level word, "diffs" and "last", in any order and case; nothing = the default level, the whole
        // conversation, no diffs.
        const auto bare = ParseAllScreenArgs(L"", TurnsLevel::Full);
        const auto compactOnly = ParseAllScreenArgs(L"compact", TurnsLevel::Full);
        const auto diffsOnly = ParseAllScreenArgs(L"Diffs", TurnsLevel::Default);
        const auto both = ParseAllScreenArgs(L" diffs   MAX ", TurnsLevel::Full);
        CHECK(bare && bare->level == TurnsLevel::Full && !bare->diffs && !bare->lastTurn, "/allscreen bare: the default level, no diffs, the whole conversation");
        CHECK(compactOnly && compactOnly->level == TurnsLevel::Compact && !compactOnly->diffs && !compactOnly->lastTurn, "/allscreen compact: that level, no diffs");
        CHECK(diffsOnly && diffsOnly->level == TurnsLevel::Default && diffsOnly->diffs, "/allscreen diffs: the default level (here Default), with diffs, any case");
        CHECK(both && both->level == TurnsLevel::Max && both->diffs && !both->lastTurn, "/allscreen diffs MAX: either order, extra blanks, any case");
        CHECK(ParseAllScreenArgs(L"diff full", TurnsLevel::Full).has_value() && !ParseAllScreenArgs(L"full compact", TurnsLevel::Full) &&
                  !ParseAllScreenArgs(L"bogus", TurnsLevel::Full) && !ParseAllScreenArgs(L"full please", TurnsLevel::Full),
              "/allscreen: diff is accepted too; two levels or an unknown word are refused (the usage line answers)");
        const auto lastOnly = ParseAllScreenArgs(L"LAST", TurnsLevel::Full);
        const auto lastAll = ParseAllScreenArgs(L"last max diffs", TurnsLevel::Full);
        CHECK(lastOnly && lastOnly->level == TurnsLevel::Full && lastOnly->lastTurn && !lastOnly->diffs, "/allscreen last: the default level, the last turn only, any case");
        CHECK(lastAll && lastAll->level == TurnsLevel::Max && lastAll->lastTurn && lastAll->diffs, "/allscreen last max diffs: all three words, in any order");
        CHECK(!ParseAllScreenArgs(L"lastturn", TurnsLevel::Full) && !ParseAllScreenArgs(L"last turn", TurnsLevel::Full),
              "/allscreen: only the word last is the box (lastturn, or last turn as two words, gets the usage line)");

        // The Session page's button opens the picker: its label says so with the arrow.
        const SessionInfo waiting = CardSession(SessionState::WaitingForInput);
        const Rows session = BuildCardButtonRows(kCardSid, CardButtonFactsFor(waiting), CardPage::Session);
        CHECK(session.size() == 8 && session[5].size() == 1 && session[5][0].second == L"c:allscreen:" + kCardSid && session[5][0].first.size() > 1 &&
                  session[5][0].first.back() == L'\x25B8',
              "All Screen: the Session page's button opens the picker (its label ends with the page arrow)");

        // The picker: the File Diffs box first and the Last Turn box under it (both unchecked), one level a row, least to most,
        // the default (Full) with the star, then Back to the Session page.
        const Rows picker = BuildCardButtonRows(kCardSid, CardButtonFactsFor(waiting), CardPage::AllScreen);
        CHECK(picker.size() == 8, "All Screen picker: the File Diffs box, the Last Turn box, five levels and Back");
        CHECK(picker.size() == 8 && picker[0].size() == 1 && picker[0][0].first.rfind(L"\x2B1C ", 0) == 0 && picker[0][0].first.find(L"File Diffs") != std::wstring::npos &&
                  picker[0][0].second == L"c:allscreen+d:" + kCardSid,
              "All Screen picker: the first row is the UNCHECKED File Diffs box (the empty-box emoji U+2B1C), and its tap shows the checked picker");
        CHECK(picker.size() == 8 && picker[1].size() == 1 && picker[1][0].first.rfind(L"\x2B1C ", 0) == 0 && picker[1][0].first.find(L"Last Turn") != std::wstring::npos &&
                  picker[1][0].first.find(L"the whole conversation") != std::wstring::npos && picker[1][0].second == L"c:allscreen+l:" + kCardSid,
              "All Screen picker: the second row is the UNCHECKED Last Turn box (the whole conversation), and its tap shows the picker with it checked");
        bool oneARow = picker.size() == 8;
        bool ordered = picker.size() == 8;
        for (size_t i = 0; i < 5 && i + 2 < picker.size(); ++i)
        {
            const auto& row = picker[i + 2];
            oneARow = oneARow && row.size() == 1;
            ordered = ordered && !row.empty() && row[0].second == L"c:allscreen-" + std::wstring{ kAllScreenLevels[i].word } + L":" + kCardSid &&
                      row[0].first.find(kAllScreenLevels[i].label) != std::wstring::npos && row[0].first.find(kAllScreenLevels[i].hint) != std::wstring::npos;
        }
        CHECK(oneARow && ordered, "All Screen picker: one level a row, least to most, each with its name, its hint and its verb (no box)");
        CHECK(picker.size() == 8 && picker[5][0].first.rfind(L"\x2B50 ", 0) == 0 && picker[2][0].first.rfind(L"\U0001F4DC ", 0) == 0 && picker[6][0].first.rfind(L"\U0001F4DC ", 0) == 0,
              "All Screen picker: the default level (Full) wears the star EMOJI (U+2B50), the others the scroll");
        CHECK(picker.size() == 8 && picker[7].size() == 1 && picker[7][0].second == L"c:panel:" + kCardSid && picker[7][0].first.find(L"Back") != std::wstring::npos,
              "All Screen picker: Back returns to the Session page (not to Main)");

        // The checked picker: the box checked (its tap shows the unchecked one), every level carrying +d, the same star and Back.
        const Rows checked = BuildCardButtonRows(kCardSid, CardButtonFactsFor(waiting), CardPage::AllScreenDiffs);
        CHECK(checked.size() == 8 && checked[0][0].first.rfind(L"\x2611\xFE0F ", 0) == 0 && checked[0][0].second == L"c:allscreen:" + kCardSid,
              "All Screen picker, checked: the box shows checked (U+2611 + U+FE0F: drawn as the emoji everywhere), and its tap shows the unchecked picker");
        bool allDiffs = checked.size() == 8;
        for (size_t i = 0; i < 5 && i + 2 < checked.size(); ++i)
        {
            allDiffs = allDiffs && checked[i + 2][0].second == L"c:allscreen-" + std::wstring{ kAllScreenLevels[i].word } + L"+d:" + kCardSid &&
                       checked[i + 2][0].first == picker[i + 2][0].first;
        }
        CHECK(allDiffs, "All Screen picker, checked: every level's data carries +d (the box as shown), its label unchanged");
        CHECK(checked.size() == 8 && checked[5][0].first.rfind(L"\x2B50 ", 0) == 0 && checked[7][0].second == L"c:panel:" + kCardSid,
              "All Screen picker, checked: the same star and the same Back");

        // Last Turn (the user's request of 2026-10-29): its two pages, and each box flipping only itself on all four.
        const Rows last = BuildCardButtonRows(kCardSid, CardButtonFactsFor(waiting), CardPage::AllScreenLast);
        const Rows lastDiffs = BuildCardButtonRows(kCardSid, CardButtonFactsFor(waiting), CardPage::AllScreenLastDiffs);
        CHECK(last.size() == 8 && last[1][0].first.rfind(L"\x2611\xFE0F ", 0) == 0 && last[1][0].first.find(L"Last Turn") != std::wstring::npos &&
                  last[1][0].first.find(L"the newest turn only") != std::wstring::npos && last[0][0].first.rfind(L"\x2B1C ", 0) == 0,
              "All Screen picker, Last Turn: its box shows checked (the newest turn only), File Diffs still unchecked");
        CHECK(lastDiffs.size() == 8 && lastDiffs[0][0].first.rfind(L"\x2611\xFE0F ", 0) == 0 && lastDiffs[1][0].first.rfind(L"\x2611\xFE0F ", 0) == 0,
              "All Screen picker, both: both boxes show checked");
        // Each box's tap names the page with ITS box flipped and the other kept: from the unchecked page, from File Diffs, from
        // Last Turn and from both.
        CHECK(picker[0][0].second == L"c:allscreen+d:" + kCardSid && picker[1][0].second == L"c:allscreen+l:" + kCardSid &&
                  checked[0][0].second == L"c:allscreen:" + kCardSid && checked[1][0].second == L"c:allscreen+dl:" + kCardSid &&
                  last[0][0].second == L"c:allscreen+dl:" + kCardSid && last[1][0].second == L"c:allscreen:" + kCardSid &&
                  lastDiffs[0][0].second == L"c:allscreen+l:" + kCardSid && lastDiffs[1][0].second == L"c:allscreen+d:" + kCardSid,
              "All Screen picker: each box flips only itself, on every page (the two combine)");
        bool lastLevels = last.size() == 8 && lastDiffs.size() == 8;
        for (size_t i = 0; i < 5 && lastLevels; ++i)
        {
            lastLevels = last[i + 2][0].second == L"c:allscreen-" + std::wstring{ kAllScreenLevels[i].word } + L"+l:" + kCardSid &&
                         lastDiffs[i + 2][0].second == L"c:allscreen-" + std::wstring{ kAllScreenLevels[i].word } + L"+dl:" + kCardSid &&
                         last[i + 2][0].first == picker[i + 2][0].first && lastDiffs[i + 2][0].first == picker[i + 2][0].first;
        }
        CHECK(lastLevels, "All Screen picker, Last Turn: every level's data carries +l (or +dl with File Diffs), its label unchanged");
        CHECK(last[7][0].second == L"c:panel:" + kCardSid && lastDiffs[7][0].second == L"c:panel:" + kCardSid, "All Screen picker, Last Turn: the same Back");
        // Every mark on all four pages is an EMOJI (the user's words of 2026-10-26: the empty box "to match the checked checkbox",
        // "the star of full should be emoji"): neither the text box U+2610 nor the text star U+2605 comes back, and the checked box
        // keeps its U+FE0F.
        bool noTextGlyphs = true;
        for (const Rows* page : { &picker, &checked, &last, &lastDiffs })
        {
            for (const auto& row : *page)
            {
                for (const auto& button : row)
                {
                    const std::wstring& label = button.first;
                    noTextGlyphs = noTextGlyphs && label.find(L'\x2610') == std::wstring::npos && label.find(L'\x2605') == std::wstring::npos &&
                                   (label.find(L'\x2611') == std::wstring::npos || label.find(L"\x2611\xFE0F") != std::wstring::npos);
                }
            }
        }
        CHECK(noTextGlyphs, "All Screen picker: no text-glyph box or star on any of its four pages, and a checked box always carries U+FE0F");

        CardButtonFacts compact = CardButtonFactsFor(waiting);
        compact.allScreenLevel = TurnsLevel::Compact; // the cog's setting, as the service sets it
        const Rows compactPicker = BuildCardButtonRows(kCardSid, compact, CardPage::AllScreen);
        CHECK(compactPicker.size() == 8 && compactPicker[3][0].first.rfind(L"\x2B50 ", 0) == 0 && compactPicker[5][0].first.rfind(L"\U0001F4DC ", 0) == 0,
              "All Screen picker: the star follows the default level the cog sets");
        CHECK(BuildCardButtonRows(kCardSid, CardButtonFactsFor(CardSession(SessionState::Error)), CardPage::AllScreen).size() == 8,
              "All Screen picker: the same boxes and five levels whatever the session's state");

        // The setting round-trips through settings.json; an older file without it reads as Full.
        const AppSettings defaults;
        AppSettings set;
        set.messagingAllScreenLevel = L"compact";
        const AppSettings again = AppSettingsFromJson(ToJson(set));
        const AppSettings blank = AppSettingsFromJson(json::Value::MkObj());
        CHECK(defaults.messagingAllScreenLevel == L"full" && again.messagingAllScreenLevel == L"compact" && blank.messagingAllScreenLevel == L"full",
              "All Screen: the default level setting round-trips; absent, it is full");
    }

    // ===== 1c. MESSAGING.md 21.41 - the card's Esc and Deactivate (MessagingSessionControl.h) =====
    {
        const SessionInfo running = CardSession(SessionState::Running);
        const SessionInfo asking = CardSession(SessionState::NeedsApproval);
        const SessionInfo waiting = CardSession(SessionState::WaitingForInput);
        const SessionInfo idle = CardSession(SessionState::Idle);
        const SessionInfo error = CardSession(SessionState::Error);
        const SessionInfo exited = CardSession(SessionState::Done);
        const SessionInfo parked = CardSession(SessionState::WaitingForInput, false);
        SessionInfo codex = CardSession(SessionState::Running);
        codex.kind = AgentKind::Codex;
        SessionInfo parkedCodex = codex;
        parkedCodex.started = false;
        // An adopted claude: its started flag stays false (meaningless for it, SessionInfo::started) while its claude runs.
        SessionInfo adopted = CardSession(SessionState::Running, false);
        adopted.external = true;
        SessionInfo closed = running;
        closed.live = false;

        // Who gets which button.
        CHECK(SessionTurnInFlight(running) && SessionTurnInFlight(asking) && !SessionTurnInFlight(waiting) && !SessionTurnInFlight(idle) &&
                  !SessionTurnInFlight(error) && !SessionTurnInFlight(exited) && !SessionTurnInFlight(closed),
              "a turn is in flight while Running or Needs approval, in an open tab only");
        CHECK(SessionTakesEsc(running) && SessionTakesEsc(waiting) && SessionTakesEsc(error) && SessionTakesEsc(adopted) && !SessionTakesEsc(exited) &&
                  !SessionTakesEsc(parked) && !SessionTakesEsc(codex) && !SessionTakesEsc(closed),
              "Esc reaches a Claude session whose claude runs (an adopted one too), never an exited claude, a parked tab, Codex or a closed session");
        CHECK(SessionIsDeactivatable(running) && SessionIsDeactivatable(waiting) && SessionIsDeactivatable(exited) && SessionIsDeactivatable(codex) &&
                  !SessionIsDeactivatable(parked) && !SessionIsDeactivatable(adopted) && !SessionIsDeactivatable(closed),
              "Deactivate parks a started tab Agentmaster launched (Codex and an exited claude too), never a parked, adopted or closed one");
        const CardButtonFacts rf = CardButtonFactsFor(running);
        CHECK(rf.turnInFlight && rf.takesEsc && rf.deactivatable && !rf.reactivatable && !rf.error && !rf.done, "facts of a running session: a turn in flight, Esc and Deactivate apply");

        // Main: NEVER Esc - the user's rule of 2026-10-28 ("The ESC button should be only inside Sessions menu"); a turn in flight
        // leaves the cog alone on Main, and Esc is one tap further, on the Session page (below).
        const Rows runMain = BuildCardButtonRows(kCardSid, rf, CardPage::Main);
        CHECK(runMain.size() == 1 && runMain[0].size() == 1 && runMain[0][0].second == L"c:panel:" + kCardSid,
              "Running, Main: the cog alone (no Esc beside it any more)");
        const Rows askMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(asking), CardPage::Main);
        CHECK(askMain.size() == 1 && askMain[0].size() == 1, "Needs approval, Main: the cog alone");
        const Rows waitMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(waiting), CardPage::Main);
        const Rows idleMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(idle), CardPage::Main);
        CHECK(waitMain.size() == 1 && waitMain[0].size() == 1 && idleMain.size() == 1 && idleMain[0].size() == 1, "at rest (Waiting, Idle), Main: the cog alone, no Esc");
        CHECK(!HasData(BuildCardButtonRows(kCardSid, CardButtonFactsFor(error), CardPage::Main), L"c:esc:" + kCardSid) &&
                  !HasData(BuildCardButtonRows(kCardSid, CardButtonFactsFor(exited), CardPage::Main), L"c:esc:" + kCardSid),
              "Error and Done, Main: no Esc (Restart Session is their action)");
        const Rows codexMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(codex), CardPage::Main);
        CHECK(codexMain.size() == 1 && codexMain[0].size() == 1, "a running Codex session, Main: the cog alone (nothing is typed into Codex)");
        const Rows adoptedMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(adopted), CardPage::Main);
        CHECK(adoptedMain.size() == 1 && adoptedMain[0].size() == 1, "an adopted running claude, Main: the cog alone too");
        const Rows closedMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(closed), CardPage::Main);
        CHECK(closedMain.size() == 1 && closedMain[0].size() == 1, "a closed session, Main: the cog alone");
        bool escOnMain = false;
        // An explicit array: a braced list of pointers to const and non-const sessions would not deduce one element type.
        const SessionInfo* const everyState[] = { &running, &asking, &waiting, &idle, &error, &exited, &parked, &codex, &adopted, &closed };
        for (const SessionInfo* sp : everyState)
        {
            escOnMain = escOnMain || HasData(BuildCardButtonRows(kCardSid, CardButtonFactsFor(*sp), CardPage::Main), L"c:esc:" + kCardSid);
        }
        CHECK(!escOnMain, "no state puts Esc on Main (it is the Session page's alone)");
        CHECK(HasData(BuildCardButtonRows(kCardSid, rf, CardPage::Session), L"c:esc:" + kCardSid) &&
                  HasData(BuildCardButtonRows(kCardSid, CardButtonFactsFor(asking), CardPage::Session), L"c:esc:" + kCardSid) &&
                  HasData(BuildCardButtonRows(kCardSid, CardButtonFactsFor(adopted), CardPage::Session), L"c:esc:" + kCardSid),
              "a turn in flight (Running, Needs approval, an adopted claude) still has Esc on its Session page");

        // The Session page: the stopping row right above Back, each button only where it applies, no row when neither does.
        const Rows runPage = BuildCardButtonRows(kCardSid, rf, CardPage::Session);
        CHECK(runPage.size() == 8 && runPage[6].size() == 2 && runPage[6][0].second == L"c:esc:" + kCardSid && runPage[6][1].second == L"c:deactivate:" + kCardSid &&
                  runPage[7][0].second == L"c:main:" + kCardSid,
              "Running, Session: Esc and Deactivate side by side, then Back");
        const Rows errPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(error), CardPage::Session);
        CHECK(errPage.size() == 8 && errPage[6].size() == 2 && errPage[6][0].second == L"c:esc:" + kCardSid && errPage[6][1].second == L"c:deactivate:" + kCardSid,
              "Error, Session: Esc and Deactivate (its claude is up)");
        const Rows donePage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(exited), CardPage::Session);
        CHECK(donePage.size() == 8 && donePage[6].size() == 1 && donePage[6][0].second == L"c:deactivate:" + kCardSid && donePage[6][0].first == L"\U0001F319 Deactivate",
              "Done, Session: Deactivate alone on the row (no Esc into an exited claude)");
        const Rows parkedPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parked), CardPage::Session);
        CHECK(parkedPage.size() == 7 && !HasData(parkedPage, L"c:esc:" + kCardSid) && !HasData(parkedPage, L"c:deactivate:" + kCardSid) &&
                  parkedPage[6][0].second == L"c:main:" + kCardSid,
              "parked, Session: no Esc / Deactivate row (nothing runs, it is parked already); Back stays the last row");
        const Rows codexPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(codex), CardPage::Session);
        CHECK(codexPage.size() == 8 && codexPage[6].size() == 1 && codexPage[6][0].second == L"c:deactivate:" + kCardSid,
              "a running Codex session, Session: Deactivate alone (the park handles Codex, Esc does not reach it)");
        const Rows adoptedPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(adopted), CardPage::Session);
        CHECK(adoptedPage.size() == 8 && adoptedPage[6].size() == 1 && adoptedPage[6][0].second == L"c:esc:" + kCardSid,
              "an adopted claude, Session: Esc alone (Deactivate never parks the user's own shell)");
        CHECK(BuildCardButtonRows(kCardSid, CardButtonFactsFor(closed), CardPage::Session).size() == 7, "a closed session, Session: no Esc / Deactivate row");
        CHECK(!HasData(BuildCardButtonRows(kCardSid, rf, CardPage::AllScreen), L"c:esc:" + kCardSid) &&
                  !HasData(BuildCardButtonRows(kCardSid, rf, CardPage::AllScreenDiffs), L"c:deactivate:" + kCardSid),
              "the All Screen picker carries neither (it is a level picker)");

        // Telegram's limits for the new buttons and every new layout.
        CHECK(CardCallbackData(kCardVerbEsc, kCardSid).size() == 42 && CardCallbackData(kCardVerbDeactivate, kCardSid).size() == 49,
              "the Esc and Deactivate data (c:esc: and c:deactivate: + a 36-character id) are 42 and 49 bytes of 64");
        bool fits = true;
        bool twoARow = true;
        // A typed array: the list mixes const records and the adjusted (non-const) ones, which a braced list cannot deduce.
        const SessionInfo* const layouts[] = { &running, &asking, &error, &exited, &codex, &adopted, &closed };
        for (const SessionInfo* s : layouts)
        {
            for (const CardPage p : { CardPage::Main, CardPage::Session })
            {
                for (const auto& row : BuildCardButtonRows(kCardSid, CardButtonFactsFor(*s), p))
                {
                    twoARow = twoARow && !row.empty() && row.size() <= 2;
                    for (const auto& [label, data] : row)
                    {
                        fits = fits && !label.empty() && Utf8Length(data) <= 64 && Utf8Length(data) == data.size();
                    }
                }
            }
        }
        CHECK(fits && twoARow, "every 21.41 layout: ASCII data within 64 bytes, a label each, one or two buttons a row (never an empty row)");

        // The window's Esc decision.
        CHECK(DecideRemoteEsc(running, false) == RemoteEscResult::Pressed && DecideRemoteEsc(asking, false) == RemoteEscResult::Pressed,
              "Esc: a running turn and a waiting dialog get the key");
        CHECK(DecideRemoteEsc(waiting, false) == RemoteEscResult::Pressed && DecideRemoteEsc(error, false) == RemoteEscResult::Pressed,
              "Esc: at rest it is written too (one harmless Esc that closes a menu if one is open)");
        CHECK(DecideRemoteEsc(running, true) == RemoteEscResult::Busy, "Esc: never between a delivery's or a draft swap's keystrokes");
        CHECK(DecideRemoteEsc(parked, false) == RemoteEscResult::NotRunning && DecideRemoteEsc(exited, false) == RemoteEscResult::NotRunning &&
                  DecideRemoteEsc(closed, false) == RemoteEscResult::NotRunning && DecideRemoteEsc(parked, true) == RemoteEscResult::NotRunning,
              "Esc: nothing reads the key in a parked tab, an exited claude or a closed session (whatever the box)");
        CHECK(DecideRemoteEsc(codex, false) == RemoteEscResult::NotClaude && DecideRemoteEsc(parkedCodex, false) == RemoteEscResult::NotRunning,
              "Esc: a running Codex session is refused as Codex, a parked one as parked first");
        CHECK(DecideRemoteEsc(adopted, false) == RemoteEscResult::Pressed, "Esc: an adopted claude gets the key (its started flag is meaningless)");

        // The window's park decision.
        CHECK(DecideRemotePark(waiting, false, false) == RemoteParkResult::Parked && DecideRemotePark(idle, false, false) == RemoteParkResult::Parked &&
                  DecideRemotePark(error, false, false) == RemoteParkResult::Parked && DecideRemotePark(exited, false, false) == RemoteParkResult::Parked,
              "Deactivate: a tab at rest is parked (an errored or exited claude's too: it resumes on entry)");
        CHECK(DecideRemotePark(running, false, false) == RemoteParkResult::TurnRunning && DecideRemotePark(asking, false, false) == RemoteParkResult::TurnRunning,
              "Deactivate: a turn in flight is never cut off unasked");
        CHECK(DecideRemotePark(running, false, true) == RemoteParkResult::Parked && DecideRemotePark(asking, false, true) == RemoteParkResult::Parked,
              "Deactivate: after Park now a turn in flight is cut off");
        CHECK(DecideRemotePark(running, true, false) == RemoteParkResult::TurnRunning, "Deactivate: the turn question comes before a held box (the user decides first)");
        CHECK(DecideRemotePark(waiting, true, false) == RemoteParkResult::Busy && DecideRemotePark(running, true, true) == RemoteParkResult::Busy,
              "Deactivate: never while a delivery or a draft swap holds the box");
        CHECK(DecideRemotePark(parked, false, false) == RemoteParkResult::AlreadyParked, "Deactivate: a parked tab is parked already");
        CHECK(DecideRemotePark(adopted, false, true) == RemoteParkResult::Adopted && DecideRemotePark(adopted, true, false) == RemoteParkResult::Adopted,
              "Deactivate: an adopted claude's shell tab is never parked (adopted wins over its meaningless started flag)");
        CHECK(DecideRemotePark(closed, false, true) == RemoteParkResult::Closed, "Deactivate: a closed session has nothing to park");
        CHECK(DecideRemotePark(codex, false, true) == RemoteParkResult::Parked, "Deactivate: a Codex tab is parked like a Claude one");

        // The answers' words round-trip; anything else reads as Failed, never as success.
        bool escWords = true;
        for (const RemoteEscResult r : { RemoteEscResult::Pressed, RemoteEscResult::NotRunning, RemoteEscResult::NotClaude, RemoteEscResult::Busy, RemoteEscResult::Failed })
        {
            escWords = escWords && ParseRemoteEscResult(RemoteEscResultWord(r)) == r;
        }
        bool parkWords = true;
        for (const RemoteParkResult r : { RemoteParkResult::Parked, RemoteParkResult::AlreadyParked, RemoteParkResult::Adopted, RemoteParkResult::TurnRunning,
                                          RemoteParkResult::Busy, RemoteParkResult::Closed, RemoteParkResult::Failed })
        {
            parkWords = parkWords && ParseRemoteParkResult(RemoteParkResultWord(r)) == r;
        }
        CHECK(escWords && parkWords, "every Esc and park result's word reads back as that result");
        CHECK(ParseRemoteEscResult(L"") == RemoteEscResult::Failed && ParseRemoteEscResult(L"Pressed") == RemoteEscResult::Failed &&
                  ParseRemoteParkResult(L"") == RemoteParkResult::Failed && ParseRemoteParkResult(L"parked ") == RemoteParkResult::Failed,
              "an empty or unknown answer (an older window) reads as failed, never as pressed or parked");
        CHECK(kPressEscAction == L"press-esc" && kDeactivateAction == L"deactivate" && kDeactivateCutOffTurnArg == L"cut-off-turn" && kPressEscAction != kDeactivateAction,
              "the two window action words and the cut-off argument");

        // The texts: one plain sentence each, within the toast limit, no HTML (the toast goes as is; a line escapes it).
        std::vector<std::wstring> texts;
        for (const RemoteEscResult r : { RemoteEscResult::Pressed, RemoteEscResult::NotRunning, RemoteEscResult::NotClaude, RemoteEscResult::Busy, RemoteEscResult::Failed })
        {
            for (const SessionState st : { SessionState::Running, SessionState::NeedsApproval, SessionState::WaitingForInput })
            {
                texts.push_back(RemoteEscOutcomeText(r, st));
            }
        }
        for (const RemoteParkResult r : { RemoteParkResult::Parked, RemoteParkResult::AlreadyParked, RemoteParkResult::Adopted, RemoteParkResult::TurnRunning,
                                          RemoteParkResult::Busy, RemoteParkResult::Closed, RemoteParkResult::Failed })
        {
            texts.push_back(RemoteParkOutcomeText(r));
        }
        texts.emplace_back(kRemoteEscRepeatedText);
        texts.emplace_back(kRemoteEscUnansweredText);
        texts.emplace_back(kRemoteParkUnansweredText);
        bool textsFit = true;
        for (const auto& t : texts)
        {
            textsFit = textsFit && !t.empty() && t.size() <= kTapToastMaxChars && t.find_first_of(L"<>&") == std::wstring::npos;
        }
        CHECK(textsFit, "every Esc and park text is non-empty, within the 190-unit toast limit, and free of HTML");
        CHECK(RemoteEscOutcomeText(RemoteEscResult::Pressed, SessionState::Running) != RemoteEscOutcomeText(RemoteEscResult::Pressed, SessionState::NeedsApproval) &&
                  RemoteEscOutcomeText(RemoteEscResult::Pressed, SessionState::Running) != RemoteEscOutcomeText(RemoteEscResult::Pressed, SessionState::WaitingForInput),
              "a pressed Esc says what it did: a turn stopped, a dialog turned down, or nothing was running");

        // The Esc gap: two Esc presses close together open Claude Code's Rewind menu.
        CHECK(!RemoteEscTooSoon(0, 5000) && RemoteEscTooSoon(5000, 5000) && RemoteEscTooSoon(5000, 7999) && !RemoteEscTooSoon(5000, 8000),
              "Esc gap: never pressed goes; within 3 s of the last is refused; 3 s after it goes again");
        CHECK(RemoteEscTooSoon(9000, 5000) && kRemoteEscGapMs == 3000, "Esc gap: a clock that went backwards refuses (the safe side), and the gap is 3 s");
    }

    // ===== 1d. MESSAGING.md 21.42 - the card's screenshot (MessagingCardPicture.h) =====
    {
        // The screen's identity: deterministic, never 0, and a changed screen reads as changed.
        const std::wstring shotA = L"AMSS1 80 24 ffffff 000000 Cascadia Mono\nrow one";
        const std::wstring shotB = L"AMSS1 80 24 ffffff 000000 Cascadia Mono\nrow two";
        CHECK(CardPictureShotHash(shotA) == CardPictureShotHash(shotA) && CardPictureShotHash(shotA) != CardPictureShotHash(shotB) && CardPictureShotHash(L"") != 0,
              "picture: a screen's hash is stable, tells two screens apart, and is never 0");

        // The plan: a changed screen uploads, an unchanged one reuses (captioned now), an unreadable one keeps the last picture with
        // its own moment, and nothing at all goes without.
        const int64_t now = 1'800'000'000'000;
        const uint32_t h = CardPictureShotHash(shotA);
        const auto changed = PlanCardPicture(true, h, now, L"FILE1", h + 1, now - 60'000);
        const auto first = PlanCardPicture(true, h, now, L"", 0, 0);
        const auto same = PlanCardPicture(true, h, now, L"FILE1", h, now - 60'000);
        const auto parked = PlanCardPicture(false, 0, now, L"FILE1", h, now - 60'000);
        const auto nothing = PlanCardPicture(false, 0, now, L"", 0, 0);
        const auto unknownHash = PlanCardPicture(true, h, now, L"FILE1", 0, now - 60'000);
        CHECK(changed.source == CardPictureSource::Upload && changed.shotAtMs == now, "picture plan: a changed screen is drawn and uploaded, captioned now");
        CHECK(first.source == CardPictureSource::Upload && first.shotAtMs == now, "picture plan: a card's first picture is drawn and uploaded");
        CHECK(same.source == CardPictureSource::Reuse && same.shotAtMs == now, "picture plan: an unchanged screen reuses the held photo, captioned with the moment it was read again");
        CHECK(parked.source == CardPictureSource::Reuse && parked.shotAtMs == now - 60'000, "picture plan: an unreadable screen keeps the last picture with ITS moment");
        CHECK(nothing.source == CardPictureSource::None, "picture plan: no screen and no picture: the card goes as text");
        CHECK(unknownHash.source == CardPictureSource::Upload, "picture plan: a held photo whose screen hash is unknown is never taken for the screen read now");

        // The caption's clock: seconds on the same day, the date on another, "?" for no moment.
        const std::wstring clock = CardPictureClockText(now, now);
        CHECK(clock.size() == 8 && clock[2] == L':' && clock[5] == L':', "picture caption: HH:MM:SS on the same day");
        const std::wstring older = CardPictureClockText(now - 3LL * 24 * 3600 * 1000, now);
        CHECK(older.size() > 8 && older.find(L':') != std::wstring::npos && older.find(L' ') != std::wstring::npos, "picture caption: another day names the date before the clock");
        CHECK(CardPictureClockText(0, now) == L"?" && CardPictureClockText(-5, now) == L"?", "picture caption: no moment reads as a question mark");

        // The rich block: a figure of the photo named in the media list, its caption, nothing to escape.
        const std::wstring figure = CardPictureFigureHtml(now, now);
        CHECK(figure.rfind(L"<figure><img src=\"tg://photo?id=screen\"/><figcaption>", 0) == 0 && figure.size() > 20 &&
                  figure.substr(figure.size() - 22) == L"</figcaption></figure>" && figure.find(L"the screen at " + clock) != std::wstring::npos,
              "picture block: a figure of tg://photo?id=screen captioned with the screen's moment");
        CHECK(kCardPictureMediaId == L"screen" && kCardPictureMediaId.size() <= 64, "picture: the media id is Telegram-valid (1-64 of A-Z a-z 0-9 _ -)");

        // The media list: an upload names the attach part, a reuse names the held file_id.
        CardPicture upload;
        upload.png = "\x89PNG";
        upload.fileId = L"IGNORED";
        CardPicture reuse;
        reuse.fileId = L"AgACAgQAAxUH";
        const json::Value up = CardPictureMediaJson(upload);
        const json::Value re = CardPictureMediaJson(reuse);
        const auto mediaOf = [](const json::Value& list) -> std::wstring {
            if (list.type != json::Value::Type::Arr || list.arr.size() != 1)
            {
                return L"<not one entry>";
            }
            const auto* media = list.arr[0].Find(L"media");
            return list.arr[0].StrAt(L"id") + L"|" + (media ? media->StrAt(L"type") + L"|" + media->StrAt(L"media") : std::wstring{ L"<no media>" });
        };
        CHECK(mediaOf(up) == L"screen|photo|attach://screen", "picture media: a new picture is the attach part \"screen\", whatever fileId says");
        CHECK(mediaOf(re) == L"screen|photo|AgACAgQAAxUH", "picture media: a held picture is reused by its file_id");

        // Reading the photo back from Telegram's answer (the probe's shape): the LARGEST size of the first photo block.
        const auto answer = json::Parse(
            L"{\"message_id\":1381,\"rich_message\":{\"blocks\":[{\"type\":\"paragraph\"},{\"type\":\"photo\",\"photo\":["
            L"{\"file_id\":\"small\",\"file_unique_id\":\"u1\",\"width\":90,\"height\":30},"
            L"{\"file_id\":\"big\",\"file_unique_id\":\"u3\",\"width\":1600,\"height\":900},"
            L"{\"file_id\":\"mid\",\"file_unique_id\":\"u2\",\"width\":800,\"height\":450}],\"caption\":{}}]}}");
        const CardPictureShown shown = answer ? CardPictureFromMessage(*answer) : CardPictureShown{};
        CHECK(shown.fileId == L"big" && shown.uniqueId == L"u3" && shown.width == 1600 && shown.height == 900, "picture answer: the largest size's file_id is the one reused");
        const auto textOnly = json::Parse(L"{\"message_id\":1381,\"rich_message\":{\"blocks\":[{\"type\":\"paragraph\"}]}}");
        const auto classic = json::Parse(L"{\"message_id\":7,\"text\":\"a classic card\"}");
        CHECK(textOnly && CardPictureFromMessage(*textOnly).fileId.empty() && classic && CardPictureFromMessage(*classic).fileId.empty() &&
                  CardPictureFromMessage(json::Value::MkArr()).fileId.empty(),
              "picture answer: a text-only rich card, a classic card or anything else shows no photo");

        // The topic record keeps the picture across a restart; an older file has none.
        MessagingState st;
        TopicRecord t;
        t.sessionId = L"p";
        t.threadId = 11;
        t.cardPictureFileId = L"AgACAgQAAxUH";
        t.cardPictureAtMs = now;
        t.cardPictureShotHash = 0xFFFFFFF0u;
        st.topics[L"p"] = t;
        const auto back = StateFromJson(StateToJson(st));
        CHECK(back && back->topics.count(L"p") && back->topics.at(L"p").cardPictureFileId == L"AgACAgQAAxUH" && back->topics.at(L"p").cardPictureAtMs == now &&
                  back->topics.at(L"p").cardPictureShotHash == 0xFFFFFFF0u,
              "picture record: the file_id, its moment and its screen hash round-trip (a 32-bit hash survives the JSON number)");
        const auto olderFile = json::Parse(L"{\"topics\":[{\"sessionId\":\"o\",\"threadId\":5}]}");
        const auto oldState = olderFile ? StateFromJson(*olderFile) : std::nullopt;
        CHECK(oldState && oldState->topics.count(L"o") && oldState->topics.at(L"o").cardPictureFileId.empty() && oldState->topics.at(L"o").cardPictureShotHash == 0,
              "picture record: an older file has no picture (the card's next update draws one)");

        // The switch: on by default (the user's request), off round-trips, an older settings file reads as on.
        const AppSettings defaults;
        AppSettings off;
        off.messagingCardScreenshots = false;
        CHECK(defaults.messagingCardScreenshots && !AppSettingsFromJson(ToJson(off)).messagingCardScreenshots && AppSettingsFromJson(json::Value::MkObj()).messagingCardScreenshots,
              "picture switch: on by default, off round-trips, absent reads as on");
        CHECK(kCardPictureRefusalsToStop == 3, "picture: three refusals in a row stop the pictures for the run");
        // The wait for a picture is bounded, and below a tapped button's hold limit: a tap that waits for its card change (Restart,
        // a page flip that met a post) is still answered after the change lands, never by the limit alone.
        CHECK(kCardPictureWaitMs > 0 && kCardPictureWaitMs + 2000 < kTapAnswerHoldMaxMs, "picture: a card waits at most 4 s for its picture, well inside the 9 s tap hold");

        // ----- 2026-11-01, the picture follows the status: the card's LOOKS at its screen (CardPictureTrack) -----
        // The user's report: "When status changes, we update the message but I think we don't update the image". Production that
        // day: a window answered a card's read 5-9 s late, past the 4 s bound - the card went with its old picture, the late one
        // was dropped, and a card at rest is never edited again.

        // A look's plan: compared with the picture the card CARRIES, a screen that is that one is Unchanged (nothing drawn or sent).
        const auto lookSame = PlanCardPicture(true, h, now, L"FILE1", h + 1, now - 60'000, h);
        const auto lookChanged = PlanCardPicture(true, h + 7, now, L"FILE1", h, now - 60'000, h);
        const auto lookBackToStored = PlanCardPicture(true, h, now, L"FILE1", h, now - 60'000, h + 9);
        const auto lookParked = PlanCardPicture(false, 0, now, L"FILE1", h, now - 60'000, h);
        const auto noBasis = PlanCardPicture(true, h, now, L"FILE1", h, now - 60'000, 0);
        CHECK(lookSame.source == CardPictureSource::Unchanged, "look plan: the screen the card carries (an upload maybe still on its way) is Unchanged - nothing drawn, nothing sent");
        CHECK(lookChanged.source == CardPictureSource::Upload && lookChanged.shotAtMs == now, "look plan: a screen that changed since the card's picture is drawn and uploaded");
        CHECK(lookBackToStored.source == CardPictureSource::Reuse && lookBackToStored.shotAtMs == now,
              "look plan: a screen that is the stored photo's while the card carries another reuses the photo by file_id");
        CHECK(lookParked.source == CardPictureSource::Reuse && lookParked.shotAtMs == now - 60'000, "look plan: no screen keeps the stored picture with its own moment");
        CHECK(noBasis.source == CardPictureSource::Reuse, "look plan: no basis (0) never reads as unchanged");

        // The backoff and the schedule's merge.
        CHECK(CardPictureLookDelayMs(0) == 5000 && CardPictureLookDelayMs(1) == 10000 && CardPictureLookDelayMs(2) == 20000 && CardPictureLookDelayMs(3) == 40000 &&
                  CardPictureLookDelayMs(4) == 60000 && CardPictureLookDelayMs(1000) == 60000 && CardPictureLookDelayMs(-3) == 5000,
              "look backoff: 5, 10, 20, 40, then 60 s at most (a negative count reads as none)");
        CHECK(EarlierCardPictureLook(0, 900) == 900 && EarlierCardPictureLook(500, 900) == 500 && EarlierCardPictureLook(1500, 900) == 900,
              "look schedule: a new rule fills an empty schedule and never pushes an earlier look back");
        CHECK(kCardPictureSettleMs == 3000 && kCardPictureLookWaitMs == 30000 && kCardPictureLookTriesMax == 8 && kCardPictureLookWaitMs > kCardPictureWaitMs,
              "look constants: the settle look 3 s after a change, a look waits 30 s (no text waits for it), 8 looks in a row at most");

        // The production case: a status update's read got no screen in time (the 4 s wait ran out).
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, true, false, true, now); // waiting -> running: a read, and no settle look (a running card re-reads)
            CHECK(t.stepPending && t.stepReads && t.lookAtMs == 0 && t.lookTries == 0, "track: a status update into Running reads the screen and owes no settle look");
            CHECK(!CardPictureLookDue(t, now + 999'999), "track: nothing is due while no look is owed");
            CardPictureStepSettled(t, h, CardPictureRead::Stale, now + 4000); // the card went with its stored picture
            CHECK(!t.stepPending && t.handedShotHash == h && t.lookAtMs == now + 4000 + 5000, "track: a read that got no screen in time owes a look 5 s later");
            CHECK(!CardPictureLookDue(t, now + 8999) && CardPictureLookDue(t, now + 9000), "track: the look is due at its moment, not before");
            CardPictureLookStarted(t, 77, 41, true);
            CHECK(t.lookPending && t.lookAtMs == 0 && t.lookSeq == 77 && t.lookBase == 41 && !CardPictureLookDue(t, now + 99'000), "track: one look at a time");
            const auto step = CardPictureLookSettled(t, 77, 41, true, h + 5, CardPictureRead::Fresh, now + 15'000);
            CHECK(step == CardPictureLookStep::Send && !t.lookPending && t.handedShotHash == h + 5 && t.lookTries == 0 && t.lookAtMs == 0,
                  "track: the window answered the look with a changed screen - the card is edited with it, and the looks end");
        }

        // The early read: a change into a state at rest owes the settle look, whatever the update's own read found.
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, true, true, true, now); // running -> waiting for you
            CHECK(t.lookAtMs == now + kCardPictureSettleMs, "track: a change into a state at rest owes the settle look 3 s later");
            CHECK(!CardPictureLookDue(t, now + kCardPictureSettleMs), "track: never while the update's own read is pending (a look never overtakes it)");
            CardPictureStepSettled(t, h, CardPictureRead::Fresh, now + 200);
            CHECK(t.lookAtMs == now + kCardPictureSettleMs && CardPictureLookDue(t, now + kCardPictureSettleMs),
                  "track: a fresh read keeps the settle look - Claude Code may still have been drawing the new state");
            CardPictureLookStarted(t, 90, 12, true);
            CHECK(CardPictureLookSettled(t, 90, 12, false, 0, CardPictureRead::Unchanged, now + 3100) == CardPictureLookStep::Done && t.lookAtMs == 0 && !t.lookPending,
                  "track: the settle look found the screen the card carries - nothing is sent, nothing more is owed");
            CardPictureTrack again;
            CardPictureStepDecided(again, true, true, true, true, now);
            CardPictureStepDecided(again, true, true, false, true, now + 1000); // the session runs again before the settle look
            CHECK(again.lookAtMs == 0, "track: a change into Running cancels a settle look still owed (its own read covers it)");
        }

        // A page flip (Kept) that overtakes a read still running, or that puts an older picture back, owes a look.
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, false, false, true, now); // a text change (the draft): a read, no state change
            CHECK(t.lookAtMs == 0, "track: a text change without a state change owes no settle look");
            CardPictureStepDecided(t, false, false, false, true, now + 50); // a page flip 50 ms later overtakes that read
            CHECK(t.stepPending && !t.stepReads && t.lookAtMs == now + 50 + kCardPictureSettleMs, "track: a page flip that overtakes a read still running owes the look that read was");
            CardPictureStepSettled(t, h, CardPictureRead::Kept, now + 60);
            CHECK(!CardPictureLookDue(t, now + 50 + kCardPictureSettleMs - 1) && CardPictureLookDue(t, now + 50 + kCardPictureSettleMs), "track: ...due 3 s after the flip");

            CardPictureTrack older;
            CardPictureStepDecided(older, true, false, false, true, now);
            CardPictureStepSettled(older, h + 1, CardPictureRead::Fresh, now + 100); // a new upload is handed (still on its way)
            CHECK(older.lookAtMs == 0 && older.handedShotHash == h + 1, "track: a fresh read with no state change owes nothing");
            CardPictureStepDecided(older, false, false, false, true, now + 500); // a page flip carries the stored picture, older than that upload
            CardPictureStepSettled(older, h, CardPictureRead::Kept, now + 500);
            CHECK(older.handedShotHash == h && older.lookAtMs == now + 500 + kCardPictureSettleMs, "track: a page flip that puts an older picture back owes a look");

            CardPictureTrack same;
            CardPictureStepDecided(same, false, false, false, true, now);
            CardPictureStepSettled(same, h, CardPictureRead::Kept, now); // the first step of a run: nothing handed before
            CardPictureStepDecided(same, false, false, false, true, now + 10);
            CardPictureStepSettled(same, h, CardPictureRead::Kept, now + 10); // the same picture again
            CHECK(same.lookAtMs == 0, "track: a page flip that keeps the picture the card carries - or the first step of a run - owes nothing");
        }

        // A post or edit decided while a look runs wins; a look that got no screen in time backs off and gives up after 8.
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, true, true, true, now);
            CardPictureStepSettled(t, h, CardPictureRead::Stale, now + 4000);
            CHECK(t.lookAtMs == now + kCardPictureSettleMs, "track: a read that got no screen in time keeps an earlier settle look (never pushed back)");
            CardPictureLookStarted(t, 5, 100, true);
            CardPictureStepDecided(t, true, true, false, true, now + 5000); // the session runs again while the look waits for the window
            CHECK(t.lookPending && t.lookAtMs == 0, "track: a status update while a look runs leaves the look to settle and owes no new one (Running)");
            CHECK(CardPictureLookSettled(t, 5, 101, true, h + 3, CardPictureRead::Fresh, now + 6000) == CardPictureLookStep::Dropped && !t.lookPending,
                  "track: ...and that look is dropped when it settles - the update owns the card, an older text is never edited back over it");
            CHECK(CardPictureLookSettled(t, 5, 101, true, h + 3, CardPictureRead::Fresh, now + 6000) == CardPictureLookStep::Dropped, "track: a look that is not running is dropped");

            CardPictureTrack b;
            CardPictureStepDecided(b, true, true, true, true, now);
            CardPictureStepSettled(b, h, CardPictureRead::Stale, now);
            int64_t atMs = now; // the simulated clock (not `clock`: <ctime>'s function of that name)
            CardPictureLookStep last = CardPictureLookStep::Done;
            bool everyLookDue = true;
            std::vector<int64_t> pauses;
            for (int i = 0; i < kCardPictureLookTriesMax; ++i)
            {
                atMs = b.lookAtMs;
                everyLookDue = everyLookDue && CardPictureLookDue(b, atMs);
                CardPictureLookStarted(b, 1000 + static_cast<uint64_t>(i), 7, true);
                atMs += kCardPictureLookWaitMs; // the window never answers: each look's wait runs out
                last = CardPictureLookSettled(b, 1000 + static_cast<uint64_t>(i), 7, false, 0, CardPictureRead::Stale, atMs);
                if (last == CardPictureLookStep::Again)
                {
                    pauses.push_back(b.lookAtMs - atMs);
                }
            }
            const std::vector<int64_t> expectedPauses{ 10'000, 20'000, 40'000, 60'000, 60'000, 60'000, 60'000 };
            CHECK(everyLookDue && last == CardPictureLookStep::GiveUp && b.lookTries == kCardPictureLookTriesMax && pauses == expectedPauses,
                  "track: each look that gets no screen in time waits longer for the next (10, 20, 40, then 60 s), and the 8th gives up");
            CHECK(!CardPictureLookDue(b, atMs + 100'000'000), "track: after giving up, no look is due until the next status update");
            // A PARKED tab (its claude not started: tabStarted false) - no screen there is final.
            CardPictureStepDecided(b, true, true, true, false, atMs);
            CHECK(b.lookTries == 0 && b.lookAtMs == atMs + kCardPictureSettleMs, "track: the next status update starts the looks over");
            CHECK(!CardPictureStepSettled(b, h, CardPictureRead::NoScreen, atMs + 10) && b.launchUntilMs == 0,
                  "track: a parked tab's update (NoScreen) begins no launch watch");
            CHECK(b.lookAtMs == atMs + kCardPictureSettleMs, "track: a parked tab's update (NoScreen) owes no retry, only the settle look it had");
            CardPictureLookStarted(b, 3000, 7, false);
            CHECK(CardPictureLookSettled(b, 3000, 7, true, h, CardPictureRead::NoScreen, atMs + 3500) == CardPictureLookStep::Done && b.lookAtMs == 0 && b.lookTries == 0,
                  "track: a look at a parked tab with no screen ends the looks (never retried forever)");
        }

        // ----- 2026-11-01, the tab's start: the LAUNCH WATCH (the user's request: "when a tab activates, we should update once the
        // screenshot when the tab launched and claude is showing and no longer black screen") -----

        // A screen that shows nothing: every row blank (the format writes no cell of a blank row), however it was built.
        {
            const auto serialize = [](int cols, int rows, const std::vector<std::vector<ScreenCell>>& lines) {
                ScreenSnapshot s;
                s.cols = cols;
                s.rows = rows;
                s.defaultFg = 0xC0C0C0;
                s.defaultBg = 0x101010;
                s.fontFace = L"Cascadia Mono";
                s.lines = lines;
                s.lines.resize(static_cast<size_t>(rows));
                return SerializeScreenSnapshot(s);
            };
            const std::wstring empty = serialize(80, 24, {});
            const std::wstring spaces = serialize(80, 24, { { Cell(L" ", 0xC0C0C0, 0x101010), Cell(L" ", 0xC0C0C0, 0x101010), Cell(L"", 0xC0C0C0, 0x101010) } });
            const std::wstring text = serialize(80, 24, { {}, {}, { Cell(L" ", 0xC0C0C0, 0x101010), Cell(L">", 0xC0C0C0, 0x101010) } });
            const std::wstring lastRow = [&] {
                std::vector<std::vector<ScreenCell>> lines(24);
                lines[23] = { Cell(L"\x2500", 0x808080, 0x101010) }; // a rule on the bottom row only (claude's input box draws first)
                return serialize(80, 24, lines);
            }();
            const std::wstring colored = serialize(80, 24, { { Cell(L" ", 0xC0C0C0, 0x303080), Cell(L" ", 0xC0C0C0, 0x303080) } });
            const std::wstring underlined = serialize(80, 24, { { Cell(L" ", 0xC0C0C0, 0x101010, kScreenCellUnderline) } });
            CHECK(ScreenSnapshotShowsNothing(empty) && ScreenSnapshotShowsNothing(spaces), "blank screen: no cell at all, or only default spaces, shows nothing");
            CHECK(!ScreenSnapshotShowsNothing(text) && !ScreenSnapshotShowsNothing(lastRow), "blank screen: one glyph anywhere (the bottom row too) shows something");
            CHECK(!ScreenSnapshotShowsNothing(colored) && !ScreenSnapshotShowsNothing(underlined),
                  "blank screen: spaces in a color of their own, or under a line, show something (a program that paints with colors alone)");
            CHECK(!ScreenSnapshotShowsNothing(L"") && !ScreenSnapshotShowsNothing(L"AMSS1 80") && !ScreenSnapshotShowsNothing(empty.substr(0, empty.size() - 1)) &&
                      !ScreenSnapshotShowsNothing(empty + L"\n"),
                  "blank screen: \"\", a cut-off header, a missing or an extra row is not known blank (the drawing's own parse reports it)");
        }

        // The plan treats a blank screen as no screen: the stored picture is kept with its own moment, nothing is drawn.
        CHECK(PlanCardPicture(false, 0, now, L"FILE1", h, now - 60'000).source == CardPictureSource::Reuse, "launch: a blank screen (unreadable) keeps the stored picture");

        // An edit reads the screen only for a status update, never for the slow form upgrade, and never while the watch waits.
        {
            CardPictureTrack t;
            CHECK(CardPictureEditReads(t, true, false) && !CardPictureEditReads(t, false, false) && !CardPictureEditReads(t, true, true),
                  "launch: an edit reads for a changed text, not for the same text or the form upgrade");
            CardPictureLaunchBegin(t, now);
            CHECK(!CardPictureEditReads(t, true, false), "launch: while the watch waits, an edit keeps the card's picture (the watch sends claude's screen once)");
            CHECK(t.launchUntilMs == now + kCardPictureLaunchWatchMs && t.launchSpent && t.lookAtMs == now + kCardPictureLaunchPollMs,
                  "launch: the watch waits 120 s, its first look one poll later, and it counts for this status update");
            CHECK(!CardPictureLaunchExpire(t, now + kCardPictureLaunchWatchMs - 1) && t.launchUntilMs != 0 && CardPictureLaunchExpire(t, now + kCardPictureLaunchWatchMs) &&
                      t.launchUntilMs == 0 && CardPictureEditReads(t, true, false),
                  "launch: a watch past its deadline ends at the next decision, and edits read again");
        }

        // The look's ask and the gate.
        {
            CardPictureTrack t;
            t.handedShotHash = h;
            const auto plain = CardPictureLookAskFor(t, now);
            CHECK(plain.unchangedShotHash == h && !plain.launchGate, "launch ask: a look's basis is the picture the card carries; no gate without a watch");
            CardPictureLaunchBegin(t, now);
            t.launchShotHash = h + 2;
            const auto gated = CardPictureLookAskFor(t, now + 1500);
            CHECK(gated.launchGate && gated.launchShotHash == h + 2, "launch ask: the watch gates its looks with the screen it saw last");
            t.launchShows = kCardPictureLaunchSettleLooks;
            CHECK(!CardPictureLookAskFor(t, now + 1500).launchGate, "launch ask: after 3 changing looks the gate opens (a screen that never holds still still goes)");
            t.launchShows = 0;
            CHECK(!CardPictureLookAskFor(t, now + kCardPictureLaunchWatchMs - kCardPictureLaunchPollMs).launchGate, "launch ask: the watch's last look is never gated");
            CHECK(!CardPictureLaunchGateHolds(nullptr, true, h) && !CardPictureLaunchGateHolds(&plain, true, h), "launch gate: a post, an edit or a look outside a watch is never held");
            CardPictureLookAsk first = gated;
            first.launchShotHash = 0;
            CHECK(CardPictureLaunchGateHolds(&first, true, h) && !CardPictureLaunchGateHolds(&first, false, 0),
                  "launch gate: the watch's first screen with text waits for the next look; a blank one is not held (it is Blank)");
            CHECK(CardPictureLaunchGateHolds(&gated, true, h + 3) && !CardPictureLaunchGateHolds(&gated, true, h + 2),
                  "launch gate: a screen that changed since the last look is held; one that held still passes");
        }

        // The watch from start to picture: blank, blank, the screen drawing, then the same screen again -> ONE edit.
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, false, false, true, now); // the activation's edit: the "not running" line went (a read)
            CHECK(CardPictureStepSettled(t, h, CardPictureRead::Blank, now + 100) && t.launchUntilMs == now + 100 + kCardPictureLaunchWatchMs &&
                      t.handedShotHash == h && t.lookAtMs == now + 100 + kCardPictureLaunchPollMs,
                  "launch: an update whose read is blank keeps its picture and begins the watch (first look one poll later)");
            CHECK(!CardPictureStepSettled(t, h, CardPictureRead::Blank, now + 200), "launch: a second blank read of the same update begins no second watch");
            int64_t at = t.lookAtMs;
            CardPictureLookStarted(t, 50, 9, true);
            CHECK(CardPictureLookSettled(t, 50, 9, false, 0, CardPictureRead::Blank, at) == CardPictureLookStep::Again && t.lookAtMs == at + kCardPictureLaunchPollMs,
                  "launch: still blank - look again one poll later");
            at = t.lookAtMs;
            CardPictureLookStarted(t, 51, 9, true);
            CHECK(CardPictureLookSettled(t, 51, 9, true, h + 10, CardPictureRead::Moving, at) == CardPictureLookStep::Again && t.launchShotHash == h + 10 && t.launchShows == 1,
                  "launch: claude is drawing - the screen is remembered, nothing is sent");
            const auto ask = CardPictureLookAskFor(t, at + kCardPictureLaunchPollMs);
            CHECK(ask.launchGate && ask.launchShotHash == h + 10 && !CardPictureLaunchGateHolds(&ask, true, h + 10), "launch: the same screen at the next look passes the gate");
            at = t.lookAtMs;
            CardPictureLookStarted(t, 52, 9, true);
            CHECK(CardPictureLookSettled(t, 52, 9, true, h + 10, CardPictureRead::Fresh, at) == CardPictureLookStep::Send && t.launchUntilMs == 0 && t.handedShotHash == h + 10 &&
                      t.lookAtMs == 0,
                  "launch: claude's screen held still - the card is edited with it ONCE, and the watch is over");
            CHECK(!CardPictureLookDue(t, at + 999'999), "launch: nothing more is owed after the watch's picture");
        }

        // A screen that never holds still is taken after 3 changing looks; a screen the card carries already ends the watch silently.
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, false, false, true, now);
            CardPictureStepSettled(t, 0, CardPictureRead::Blank, now); // a new tab: no picture yet, the card went as text
            int64_t at = now;
            for (uint64_t i = 0; i < static_cast<uint64_t>(kCardPictureLaunchSettleLooks); ++i)
            {
                at = t.lookAtMs;
                CardPictureLookStarted(t, 60 + i, 3, true);
                CardPictureLookSettled(t, 60 + i, 3, true, h + 20 + static_cast<uint32_t>(i), CardPictureRead::Moving, at);
            }
            CHECK(t.launchShows == kCardPictureLaunchSettleLooks && !CardPictureLookAskFor(t, t.lookAtMs).launchGate,
                  "launch: after 3 changing looks the next one is drawn whatever it shows (a spinner, a clock)");
            at = t.lookAtMs;
            CardPictureLookStarted(t, 70, 3, true);
            CHECK(CardPictureLookSettled(t, 70, 3, true, h + 30, CardPictureRead::Fresh, at) == CardPictureLookStep::Send && t.launchUntilMs == 0, "launch: ...and sent");

            CardPictureTrack u;
            CardPictureStepDecided(u, true, false, false, true, now);
            CardPictureStepSettled(u, h, CardPictureRead::Blank, now);
            CardPictureLookStarted(u, 80, 4, true);
            CardPictureLookSettled(u, 80, 4, true, h, CardPictureRead::Moving, now + 1500);
            CardPictureLookStarted(u, 81, 4, true);
            CHECK(CardPictureLookSettled(u, 81, 4, false, h, CardPictureRead::Unchanged, now + 3000) == CardPictureLookStep::Done && u.launchUntilMs == 0,
                  "launch: claude's screen is the one the card carries already - the watch ends with no edit");
        }

        // The watch that never sees claude's screen, a tab parked meanwhile, the session running meanwhile, a look that finds it.
        {
            CardPictureTrack t;
            CardPictureStepDecided(t, true, false, false, true, now);
            CardPictureStepSettled(t, h, CardPictureRead::NoScreen, now); // a started tab whose terminal is not up yet
            CHECK(t.launchUntilMs == now + kCardPictureLaunchWatchMs, "launch: no screen on a STARTED tab is a terminal not up yet - the watch begins");
            CardPictureLookStarted(t, 90, 2, true);
            CHECK(CardPictureLookSettled(t, 90, 2, false, 0, CardPictureRead::Stale, now + 31'500) == CardPictureLookStep::Again && t.lookAtMs == now + 31'500 + kCardPictureLaunchPollMs,
                  "launch: a look the window did not answer in time looks again at the poll (the watch's deadline rules, not the backoff)");
            CardPictureLookStarted(t, 91, 2, true);
            CHECK(CardPictureLookSettled(t, 91, 2, false, 0, CardPictureRead::Blank, now + kCardPictureLaunchWatchMs) == CardPictureLookStep::WatchTimedOut && t.launchUntilMs == 0,
                  "launch: still blank at the deadline - the watch gives up");
            CHECK(!CardPictureStepSettled(t, h, CardPictureRead::Blank, now + kCardPictureLaunchWatchMs + 10) && t.launchUntilMs == 0,
                  "launch: one watch per status update - a later blank read of the same update waits for the next one");
            CardPictureStepDecided(t, true, false, false, true, now + kCardPictureLaunchWatchMs + 20);
            CHECK(!t.launchSpent && CardPictureStepSettled(t, h, CardPictureRead::Blank, now + kCardPictureLaunchWatchMs + 30), "launch: a new status update may watch again");

            CardPictureTrack p;
            CardPictureStepDecided(p, true, false, false, true, now);
            CardPictureStepSettled(p, h, CardPictureRead::Blank, now);
            CardPictureStepDecided(p, false, false, true, false, now + 500); // the tab is parked again while the watch waits
            CHECK(p.launchUntilMs == 0, "launch: a tab that no longer runs its claude ends the watch");
            CardPictureTrack q;
            CardPictureStepDecided(q, true, false, false, true, now);
            CardPictureStepSettled(q, h, CardPictureRead::Blank, now);
            CardPictureLookStarted(q, 95, 6, false);
            CHECK(q.launchUntilMs == 0 && CardPictureLookSettled(q, 95, 6, false, 0, CardPictureRead::NoScreen, now + 1500) == CardPictureLookStep::Done,
                  "launch: ...also when a look finds it parked");

            CardPictureTrack r;
            CardPictureStepDecided(r, true, false, false, true, now);
            CardPictureStepSettled(r, h, CardPictureRead::Blank, now);
            CardPictureLookStarted(r, 97, 8, true);
            CardPictureStepDecided(r, CardPictureEditReads(r, true, false), true, false, true, now + 700); // a status update while the watch looks
            CHECK(!r.stepReads && r.lookAtMs == now + 700 + kCardPictureSettleMs, "launch: an edit during the watch keeps the picture and owes the look it overtook");
            CardPictureStepSettled(r, h, CardPictureRead::Kept, now + 710);
            CHECK(r.launchUntilMs != 0 && r.lookAtMs == now + 710 + kCardPictureLaunchPollMs, "launch: ...and the watch's next look comes at the poll");
            // The edit took a new generation (CardState::pictureSeq 8 -> 9): the look that started on 8 settles after it.
            CHECK(CardPictureLookSettled(r, 97, 9, true, h + 1, CardPictureRead::Moving, now + 800) == CardPictureLookStep::Dropped && r.launchUntilMs != 0,
                  "launch: the overtaken look is dropped, the watch goes on");

            CardPictureTrack s;
            CardPictureStepDecided(s, true, true, true, true, now); // running -> waiting: a fresh read, then the settle look
            CardPictureStepSettled(s, h, CardPictureRead::Fresh, now + 100);
            CardPictureLookStarted(s, 99, 1, true);
            CHECK(CardPictureLookSettled(s, 99, 1, false, 0, CardPictureRead::Blank, now + 3000) == CardPictureLookStep::Watch && s.launchUntilMs == now + 3000 + kCardPictureLaunchWatchMs &&
                      s.lookAtMs == now + 3000 + kCardPictureLaunchPollMs,
                  "launch: a look that finds the tab's claude starting (blank) begins the watch");
        }
    }

    // ===== 2. the card's line for the state's action =====
    {
        const auto text = [](const SessionInfo& s) { return TgRenderTitleTokensClassic(BuildCardText(s, 5000, 0, false)); };
        const std::wstring err = text(CardSession(SessionState::Error));
        CHECK(err.find(L"\U0001F504 Restart Session restarts it") != std::wstring::npos, "Error: the line names Restart Session (the button Main shows in Error)");
        CHECK(err.find(L"Reactivate") == std::wstring::npos && err.find(L"Resume Session") == std::wstring::npos, "Error: no Reactivate / Resume Session wording on the line");
        const std::wstring parkedErr = text(CardSession(SessionState::Error, false));
        CHECK(parkedErr.find(L"\U0001F504 Restart Session starts it in its tab") != std::wstring::npos, "a parked tab in Error: Restart Session starts it (Error is read first)");
        CHECK(parkedErr.find(L"not running") == std::wstring::npos, "a parked tab in Error: not the bare not-running line");
        const std::wstring parked = text(CardSession(SessionState::WaitingForInput, false));
        CHECK(parked.find(L"not running") != std::wstring::npos && parked.find(L"\x25B6 Reactivate resumes it") != std::wstring::npos, "parked: the not-running line with Reactivate (unchanged)");
        const std::wstring done = text(CardSession(SessionState::Done));
        CHECK(done.find(L"claude exited") != std::wstring::npos && done.find(L"\U0001F504 Restart Session starts it again") != std::wstring::npos,
              "exited: the line names Restart Session (the button Main shows after an exit)");
        CHECK(done.find(L"Reactivate") == std::wstring::npos, "exited: no Reactivate wording any more");
    }

    // ===== 3. the styled snapshot's format =====
    {
        ScreenSnapshot s;
        s.cols = 12;
        s.rows = 3;
        s.defaultFg = 0xC0C0C0;
        s.defaultBg = 0x101010;
        s.fontFace = L"Cascadia Mono";
        s.lines.resize(3);
        // Row 0: plain, bold, a wide CJK glyph, an emoji as a surrogate pair (wide), a combining sequence, a ':' and a digit (the
        // format's own separators as TEXT), then trailing blanks that are not written.
        s.lines[0] = { Cell(L"a", 0xC0C0C0, 0x101010), Cell(L"b", 0xFF0000, 0x101010, kScreenCellBold), Cell(L"\x4E2D", 0x00FF00, 0x101010, 0, 2),
                       Cell(L"\xD83D\xDE00", 0xFFFF00, 0x000080, kScreenCellItalic, 2), Cell(L"e\x0301", 0xC0C0C0, 0x101010, kScreenCellUnderline),
                       Cell(L":", 0xC0C0C0, 0x101010, kScreenCellStrikethrough), Cell(L"5", 0xC0C0C0, 0x101010), Cell(L" ", 0xC0C0C0, 0x101010),
                       Cell(L" ", 0xC0C0C0, 0x101010) };
        // Row 1: blank all along (nothing written).
        s.lines[1] = { Cell(L" ", 0xC0C0C0, 0x101010), Cell(L"", 0xC0C0C0, 0x101010) };
        // Row 2: a cell whose text holds a line break and "1,2:3"-like characters, then a blank with a COLORED background at the
        // end (not blank: it paints), then an underlined space (not blank: it draws a line).
        s.lines[2] = { Cell(L"x\ny", 0xC0C0C0, 0x101010), Cell(L"1,2:3", 0xC0C0C0, 0x101010), Cell(L" ", 0xC0C0C0, 0x303030), Cell(L" ", 0xC0C0C0, 0x101010, kScreenCellUnderline) };

        const std::wstring text = SerializeScreenSnapshot(s);
        CHECK(text.rfind(L"AMSS1 12 3 c0c0c0 101010 13:Cascadia Mono\n", 0) == 0, "the header: version, size, default colors, the font with its length");
        CHECK(SerializeScreenSnapshot(s) == text, "deterministic: the same snapshot gives the same text");
        const auto back = ParseScreenSnapshot(text);
        CHECK(back.has_value(), "the text parses back");
        if (back)
        {
            CHECK(back->cols == 12 && back->rows == 3 && back->defaultFg == 0xC0C0C0 && back->defaultBg == 0x101010 && back->fontFace == L"Cascadia Mono", "the header round-trips");
            CHECK(back->lines.size() == 3, "every row comes back (a blank one as an empty row)");
            if (back->lines.size() == 3)
            {
                std::vector<ScreenCell> row0(s.lines[0].begin(), s.lines[0].begin() + 7);
                CHECK(SameCells(back->lines[0], row0), "row 0 round-trips cell by cell: wide, surrogate, combining, the separators as text; its trailing blanks are not written");
                CHECK(back->lines[1].empty(), "a blank row is written as nothing");
                CHECK(SameCells(back->lines[2], s.lines[2]), "row 2 round-trips: a line break inside a cell, a colored blank and an underlined blank at the end are kept");
            }
        }

        // A cell's text is cut at kScreenCellMaxChars, never inside a surrogate pair.
        ScreenSnapshot longCell;
        longCell.cols = 2;
        longCell.rows = 1;
        longCell.lines.resize(1);
        std::wstring seventy(70, L'z');
        longCell.lines[0] = { Cell(seventy, 1, 2) };
        auto lc = ParseScreenSnapshot(SerializeScreenSnapshot(longCell));
        CHECK(lc && lc->lines[0].size() == 1 && lc->lines[0][0].text.size() == kScreenCellMaxChars, "a 70-unit cell is cut to 64");
        std::wstring straddle(63, L'z');
        straddle += L"\xD83D\xDE00"; // the pair would sit on units 63 and 64: it is left out whole
        longCell.lines[0] = { Cell(straddle, 1, 2) };
        lc = ParseScreenSnapshot(SerializeScreenSnapshot(longCell));
        CHECK(lc && lc->lines[0][0].text.size() == 63, "a surrogate pair at the cut is left out whole (no lone high surrogate)");

        // Malformed texts are refused whole.
        const std::wstring good = SerializeScreenSnapshot(s);
        CHECK(!ParseScreenSnapshot(L"").has_value(), "refused: empty (the window could not read the screen)");
        CHECK(!ParseScreenSnapshot(L"AMSS2" + good.substr(5)).has_value(), "refused: another version");
        CHECK(!ParseScreenSnapshot(L"AMSS1 0 3 c0c0c0 101010 0:\n0\n\n\n\n").has_value(), "refused: no columns");
        CHECK(!ParseScreenSnapshot(L"AMSS1 1001 1 c0c0c0 101010 0:\n0\n\n").has_value(), "refused: wider than kScreenSnapshotMaxCols");
        CHECK(!ParseScreenSnapshot(good.substr(0, good.size() - 3)).has_value(), "refused: cut off");
        CHECK(!ParseScreenSnapshot(good + L"x").has_value(), "refused: trailing bytes");
        CHECK(!ParseScreenSnapshot(L"AMSS1 4 1 c0c0c0 101010 0:\n1\nc0c0c010101000\n0,3,1:a\n").has_value(), "refused: a width of 3");
        CHECK(!ParseScreenSnapshot(L"AMSS1 4 1 c0c0c0 101010 0:\n1\nc0c0c010101000\n1,1,1:a\n").has_value(), "refused: a style index past the table");
        CHECK(!ParseScreenSnapshot(L"AMSS1 4 1 c0c0c0 101010 0:\n1\nc0c0c010101010\n0,1,1:a\n").has_value(), "refused: a flag this version does not know");
        CHECK(!ParseScreenSnapshot(L"AMSS1 2 1 c0c0c0 101010 0:\n1\nc0c0c010101000\n0,1,1:a0,2,1:b\n").has_value(), "refused: cells covering more columns than the screen has");
        CHECK(!ParseScreenSnapshot(L"AMSS1 4 1 c0c0c0 101010 0:\n1\nc0c0c010101000\n0,1,65:" + std::wstring(65, L'q') + L"\n").has_value(), "refused: a cell longer than 64 units");
        CHECK(!ParseScreenSnapshot(L"AMSS1 4 1 c0c0g0 101010 0:\n0\n\n").has_value(), "refused: a color that is not hex");
        CHECK(ParseScreenSnapshot(L"AMSS1 4 1 c0c0c0 101010 0:\n1\nc0c0c010101000\n0,2,1:a0,2,1:b\n").has_value(), "accepted: two wide cells exactly filling four columns");
    }

    // ===== 4. the grid and the stretched glyphs (pure) =====
    {
        const ScreenImageOptions o; // 2400 px at most, cells 8..16 px wide, 16 px padding
        const ScreenImageGrid g80 = ComputeScreenImageGrid(80, 24, 1200.0 / 2048.0, 2620.0 / 2048.0, o);
        CHECK(g80.cellWidthPx == 16, "80 columns: the widest cell allowed (16 px)");
        CHECK(std::fabs(g80.fontPx * 1200.0f / 2048.0f - 16.0f) < 0.01f, "the font size makes the advance exactly the cell width");
        CHECK(g80.cellHeightPx == static_cast<int>(std::ceil(g80.lineHeightPx - 0.01f)) && g80.cellHeightPx >= g80.lineHeightPx - 0.01f, "the cell holds the font's line (rounded up)");
        CHECK(g80.widthPx == 80 * 16 + 32 && g80.heightPx == 24 * g80.cellHeightPx + 32, "the picture: the grid plus the padding");
        const ScreenImageGrid g200 = ComputeScreenImageGrid(200, 50, 0.6, 1.2, o);
        CHECK(g200.cellWidthPx == 11 && g200.widthPx == 200 * 11 + 32 && g200.widthPx <= o.maxWidthPx, "200 columns: an 11 px cell keeps the picture within 2400 px");
        const ScreenImageGrid g400 = ComputeScreenImageGrid(400, 10, 0.6, 1.2, o);
        CHECK(g400.cellWidthPx == 8 && g400.widthPx == 400 * 8 + 32, "400 columns: the minimum cell, the picture wider than the maximum (documented)");
        const ScreenImageGrid exact = ComputeScreenImageGrid(10, 1, 0.5, 2.0, o);
        CHECK(exact.cellHeightPx == static_cast<int>(std::ceil(exact.lineHeightPx - 0.01f)), "a line height on a whole pixel is not rounded up past it");
        CHECK(ComputeScreenImageGrid(0, 24, 0.6, 1.2, o).cellWidthPx == 0, "no columns: no grid");
        CHECK(ComputeScreenImageGrid(80, 0, 0.6, 1.2, o).widthPx == 0, "no rows: no grid");
        CHECK(ComputeScreenImageGrid(80, 24, 0.0, 1.2, o).cellWidthPx == 0, "a font with no advance: no grid");
        CHECK(ComputeScreenImageGrid(80, 24, std::nan(""), 1.2, o).cellWidthPx == 0, "a NaN proportion: no grid");

        CHECK(IsCellStretchGlyph(L"\x2500") && IsCellStretchGlyph(L"\x2502") && IsCellStretchGlyph(L"\x256D") && IsCellStretchGlyph(L"\x2570"), "box drawing (rules, corners) stretches");
        CHECK(IsCellStretchGlyph(L"\x2588") && IsCellStretchGlyph(L"\x259F"), "block elements stretch");
        CHECK(!IsCellStretchGlyph(L"\x25A0") && !IsCellStretchGlyph(L"a") && !IsCellStretchGlyph(L"") && !IsCellStretchGlyph(L"\x2500\x2500"), "anything else draws as it is");
    }

    // ===== 5. a real picture =====
    {
        ScreenSnapshot s;
        s.cols = 12;
        s.rows = 3;
        s.defaultFg = 0xC0C0C0;
        s.defaultBg = 0x101010;
        s.fontFace = L"Cascadia Mono";
        s.lines.resize(3);
        s.lines[0] = { Cell(L"A", 0xFFFFFF, 0xFF0000), Cell(L"b", 0xC0C0C0, 0x101010, kScreenCellBold), Cell(L" ", 0xC0C0C0, 0x00FF00) };
        s.lines[1] = { Cell(L"\x2588", 0x0000FF, 0x101010), Cell(L"\x2500", 0xFFFF00, 0x101010), Cell(L"\x2500", 0xFFFF00, 0x101010) };
        s.lines[2] = { Cell(L"\x4E2D", 0xC0C0C0, 0x101010, kScreenCellUnderline, 2) };

        const ScreenImage image = RenderScreenSnapshotPng(s);
        CHECK(image.error.empty(), "the picture is drawn (no error)");
        if (!image.error.empty())
        {
            std::wprintf(L"    render error: %ls\n", image.error.c_str());
        }
        CHECK(!image.fontFamily.empty(), "a font family was found");
        std::wprintf(L"    drawn in %ls: %dx%d px, cell %dx%d px, %zu bytes of PNG\n", image.fontFamily.c_str(), image.grid.widthPx, image.grid.heightPx, image.grid.cellWidthPx,
                     image.grid.cellHeightPx, image.png.size());
        CHECK(image.png.size() > 64 && std::memcmp(image.png.data(), "\x89PNG\r\n\x1a\n", 8) == 0, "the bytes are a PNG");
        CHECK(image.png.size() > 24 && Be32(image.png, 16) == static_cast<uint32_t>(image.grid.widthPx) && Be32(image.png, 20) == static_cast<uint32_t>(image.grid.heightPx),
              "the PNG's header carries the grid's size");
        Decoded d;
        const bool decoded = DecodePng(image.png, d);
        CHECK(decoded && d.width == static_cast<UINT>(image.grid.widthPx) && d.height == static_cast<UINT>(image.grid.heightPx), "the PNG decodes again, at the grid's size");
        if (decoded && d.width > 0)
        {
            const int pad = ScreenImageOptions{}.paddingPx;
            const int cw = image.grid.cellWidthPx;
            const int ch = image.grid.cellHeightPx;
            CHECK(d.At(2, 2) == 0x101010, "the padding is the screen's background");
            CHECK(d.At(static_cast<UINT>(pad + 11 * cw + cw / 2), static_cast<UINT>(pad + ch / 2)) == 0x101010, "a column left out of the row (a trailing blank) is the screen's background");
            CHECK(d.At(static_cast<UINT>(pad + 1), static_cast<UINT>(pad + 1)) == 0xFF0000, "a cell's own background fills its cell (the red A, at its corner)");
            CHECK(d.At(static_cast<UINT>(pad + 2 * cw + cw / 2), static_cast<UINT>(pad + ch / 2)) == 0x00FF00, "a colored blank paints its cell (green)");
            int ink = 0;
            for (int y = pad; y < pad + ch; ++y)
            {
                for (int x = pad; x < pad + cw; ++x)
                {
                    ink += d.At(static_cast<UINT>(x), static_cast<UINT>(y)) != 0xFF0000 ? 1 : 0;
                }
            }
            CHECK(ink > 4, "the A is drawn over its background (some pixels are not the background)");
            const UINT blockCx = static_cast<UINT>(pad + cw / 2);
            const UINT blockCy = static_cast<UINT>(pad + ch + ch / 2);
            CHECK(Near(d.At(blockCx, blockCy), 0x0000FF, 8), "a full block fills its cell's middle with its color");
            CHECK(Near(d.At(blockCx, static_cast<UINT>(pad + ch + 1)), 0x0000FF, 48) && Near(d.At(blockCx, static_cast<UINT>(pad + 2 * ch - 2)), 0x0000FF, 48),
                  "the block is STRETCHED to its cell's top and bottom (the rows of a frame join)");
            // The rule: EVERY pixel column of its two cells holds some of its ink, the boundary between the cells included (a seam
            // would be a column with none). Per column, not per row: antialiasing can split a one-pixel rule over two rows, half
            // covered each, which still lifts the yellow's red and green far above the background's 0x10.
            bool joined = true;
            for (int x = pad + cw; x < pad + 3 * cw; ++x)
            {
                int best = 0;
                for (int y = pad + ch; y < pad + 2 * ch; ++y)
                {
                    const uint32_t c = d.At(static_cast<UINT>(x), static_cast<UINT>(y));
                    best = (std::max)(best, static_cast<int>((std::min)((c >> 16) & 0xFF, (c >> 8) & 0xFF)));
                }
                joined = joined && best > 0x40;
            }
            CHECK(joined, "two box rules join across their cells' boundary (no seam between columns)");
        }

        ScreenSnapshot unknownFont = s;
        unknownFont.fontFace = L"No Such Font Family 4711";
        const ScreenImage fallback = RenderScreenSnapshotPng(unknownFont);
        CHECK(fallback.error.empty() && !fallback.png.empty() && fallback.fontFamily != unknownFont.fontFace, "a font the system lacks falls back to a monospace family");
        ScreenSnapshot none;
        const ScreenImage refused = RenderScreenSnapshotPng(none);
        CHECK(refused.png.empty() && !refused.error.empty(), "a screen with no size is refused with a reason");
    }
}
