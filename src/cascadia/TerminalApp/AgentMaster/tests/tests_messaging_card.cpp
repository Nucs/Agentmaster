// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.24: the sticky card's buttons, refactored on the user's request of 2026-10-18 —
//   1. two PAGES (MessagingModels.h BuildCardButtonRows / CardButtonFactsFor): Main holds ⚙ Session, and beside it
//      🔄 Restart Session only in Error or after an exit (the user's rule of 2026-10-19) or ▶ Reactivate for a parked tab;
//      ⚙ Session turns the keyboard into the Session page — ⑂ Fork and 🆕 New Session first (the user's placement), then
//      🔄 Restart Session with 🔧 Resume Session beside it in Error, then every other button the card had before (the runner,
//      the queue, the pin, the transcript, the screen as text), 🖼 Screen Image and ◀ Back; the card's text names the state's
//      action (🔄 Restart Session in Error / after an exit, ▶ Reactivate for a parked tab);
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
    std::wprintf(L"== TestMessagingCard (MESSAGING.md 21.24: the card's two pages, the Error line, the screen snapshot and its picture) ==\n");

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
        CHECK(page.size() == 6, "Session: six rows");
        CHECK(page.size() == 6 && page[0].size() == 2 && page[0][0].second == L"c:fork:" + kCardSid && page[0][0].first.find(L"Fork") != std::wstring::npos &&
                  page[0][1].second == L"c:newsession:" + kCardSid && page[0][1].first.find(L"New Session") != std::wstring::npos,
              "Session: Fork and New Session first, side by side (right after the cog's tap)");
        CHECK(page.size() == 6 && page[1].size() == 1 && page[1][0].second == L"c:restart:" + kCardSid && page[1][0].first.find(L"Restart Session") != std::wstring::npos,
              "Session: Restart Session next, alone while the session is not in Error");
        CHECK(HasData(page, L"c:runner:" + kCardSid) && HasLabel(page, L"Pause runner"), "Session: the runner button (Auto -> Pause runner)");
        CHECK(HasData(page, L"c:queue:" + kCardSid), "Session: Queue");
        CHECK(HasData(page, L"c:pin:" + kCardSid) && HasLabel(page, L"\U0001F4CC Pin"), "Session: Pin (unpinned)");
        CHECK(HasData(page, L"c:transcript:" + kCardSid), "Session: Transcript");
        CHECK(HasData(page, L"c:screen:" + kCardSid) && HasLabel(page, L"Screen Text"), "Session: Screen Text (the old Screen's verb)");
        CHECK(HasData(page, L"c:screenimg:" + kCardSid) && HasLabel(page, L"Screen Image"), "Session: Screen Image");
        CHECK(page.size() == 6 && page[5].size() == 1 && page[5][0].second == L"c:main:" + kCardSid && page[5][0].first.find(L"Back") != std::wstring::npos, "Session: Back alone on the last row");
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
        CHECK(errPage.size() == 6 && errPage[1].size() == 2 && errPage[1][0].second == L"c:restart:" + kCardSid && errPage[1][1].second == L"c:resume:" + kCardSid &&
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
        CHECK(donePage.size() == 6 && donePage[1].size() == 1 && donePage[1][0].second == L"c:restart:" + kCardSid, "Done, Session: Restart Session alone");
        CHECK(!HasData(donePage, L"c:resume:" + kCardSid) && !HasData(donePage, L"c:reactivate:" + kCardSid), "Done, Session: no Resume, no Reactivate");

        // A parked tab: Reactivate beside the cog on Main (the button it had before; the rule names only Restart); the Session page
        // has Restart alone (tapped on a parked tab, it starts it in place, as Reactivate does).
        const SessionInfo parked = CardSession(SessionState::WaitingForInput, false);
        const Rows parkedMain = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parked), CardPage::Main);
        CHECK(parkedMain.size() == 1 && parkedMain[0].size() == 2 && parkedMain[0][1].second == L"c:reactivate:" + kCardSid && parkedMain[0][1].first.find(L"Reactivate") != std::wstring::npos,
              "parked, Main: the cog, then Reactivate");
        CHECK(!HasData(parkedMain, L"c:restart:" + kCardSid), "parked, Main: no Restart (not in Error, not exited)");
        const Rows parkedPage = BuildCardButtonRows(kCardSid, CardButtonFactsFor(parked), CardPage::Session);
        CHECK(parkedPage.size() == 6 && parkedPage[1].size() == 1 && parkedPage[1][0].second == L"c:restart:" + kCardSid, "parked, Session: Restart Session alone");
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
            for (const CardPage p : { CardPage::Main, CardPage::Session })
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
              "the longest data (c:newsession:, c:reactivate:, c:transcript: + a 36-character id) is 49 bytes");
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
