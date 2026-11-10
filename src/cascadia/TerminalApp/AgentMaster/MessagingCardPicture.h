// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the sticky card's SCREENSHOT (MESSAGING.md §21.42), taken only when the card's 🖼️ is tapped (§21.49). The
// user's request (2026-10-27): "the running since message, called also i think status message that is pinned to the
// bottom - whenever it updates, have it produce a screenshot (if possible) and have it shown and so every update we produce a
// screenshot to show in the status message".
//
// The card stays what it is - a RICH message (its title a chip, its buttons below) - and carries the session's terminal as
// its last block: <figure><img src="tg://photo?id=screen"/><figcaption>🖥 the screen at 19:50:12</figcaption></figure>, the
// photo given in the rich message's `media` list (Bot API 10.2's InputRichMessageMedia). Measured on the dev bot
// (2026-10-27, scratchpad card_picture_probe.py, every message deleted after): sendRichMessage with an uploaded photo kept
// the paragraph, the photo, its caption and the keyboard (441 ms for 4 KB); editMessageText with a NEW upload replaced the
// photo in place, keyboard kept (1,291 ms for a 4 MB PNG); editMessageText reusing the photo's file_id took 274 ms with no
// upload; a text-only rich edit dropped the photo and a later edit by file_id put it back; an identical edit answered
// "message is not modified"; a re-post by file_id needed no upload either.
//
// WHEN the screen is read changed with §21.49. Until then every update of the card's text read the hosting window's screen
// (a changed one drawn and uploaded in the card's own edit or post), and since 2026-11-01 the card also LOOKED at its screen
// again - 3 s after a change into a state at rest, with a backoff after a read that came too late, and every 1.5 s while a
// starting tab's claude drew (the launch watch). Production paid for it: the 6-hour flood hold the owner's chat sat in (a 429
// asking for 21,890 s, every method parked) came after 2.5 hours of 58-166 calls per 10 minutes, nearly all card edits
// uploading a new picture (43-128 drawn per 10 minutes), and the windows left ~35,000 screen reads unanswered within the
// 4 s and 30 s bounds, worst right after the restarts. The user's call (2026-11-10): "I think we abuse sending screenshots.
// to the right of Session button, add a picture emoji button [...] When user clicks on that emoji, it updates the bottom pinned
// tab with a picture like we do now automatically" - and the picture stays until the next tap.
//
// So the screen is read ONLY for a 🖼️ tap (MessagingService::_cardPictureFromTap): the hosting window reads it once; a screen
// that changed since the card's picture is drawn on the worker and uploaded by the card's next edit (no call of its own); an
// unchanged one reuses the photo Telegram holds, captioned with the moment it was read again. Every other post and edit of the
// card CARRIES the picture it has (CardPictureToCarry: by its file_id - no upload, no read), since a rich edit without it would
// drop the photo; a card that never had one goes as text until the first tap.
//
// This header is the pure half: the media id, the picture as a message carries it, the plan (upload, reuse, none), the picture
// a post or edit carries, how a tap's read settled (CardPictureRead), the tap's bounds and its toasts, the rich HTML block, the
// media list, reading the photo back from Telegram's answer, and the caption's clock. The m5 suite pins every rule
// (tests_messaging_card.cpp, section 1d).

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include "Json.h"
#include "ScreenSnapshot.h" // ScreenSnapshotShowsNothing: a blank screen (a tab whose claude is still starting) is never drawn

namespace Agentmaster::Msg
{
    /// <summary>
    /// The id the card's photo goes by in its rich message: the HTML's tg://photo?id=screen and the `media` entry's id (1-64
    /// characters of A-Z a-z 0-9 _ -, Telegram's rule), and - for a new upload - the multipart field the file goes in
    /// (attach://screen).
    /// </summary>
    inline constexpr std::wstring_view kCardPictureMediaId = L"screen";

    /// <summary>The file name a new card picture is uploaded under (Telegram keeps a photo's pixels, not its name).</summary>
    inline constexpr std::wstring_view kCardPictureFileName = L"screen.png";

    /// <summary>
    /// The picture one card post or edit carries: either a NEW one to upload (`png`: a 🖼️ tap's drawing) or one Telegram already
    /// holds (`fileId`, reused with no upload), plus what it shows - the moment its screen was read and that screen's hash.
    /// Shared by the outbox items that carry it (a refused send goes again without it) and, while a tap's picture waits for the
    /// send that delivers it, by the card's state (MessagingService::CardState::tapPicture, which a send's answer matches by
    /// IDENTITY - the same object). Never changed once made.
    /// </summary>
    struct CardPicture
    {
        std::string png; // the PNG to upload ("" = reuse fileId); a picture is ~0.1-0.6 MB (§21.24's measures)
        std::wstring fileId; // Telegram's file_id of a photo it holds; used only when png is empty
        int64_t shotAtMs{ 0 }; // when the screen this picture shows was read (Unix ms): the caption's clock
        uint32_t shotHash{ 0 }; // CardPictureShotHash of that screen: a tap on an unchanged screen reuses the picture
    };

    /// <summary>
    /// The identity of a screen snapshot (ScreenSnapshot.h's serialized form, as the window answers it): FNV-1a 32 over its
    /// UTF-16 code units, never 0 (0 = "no picture recorded"). A collision only makes one tap reuse the previous picture: the
    /// next tap on a changed screen draws a new one.
    /// </summary>
    /// <param name="snapshot">The serialized snapshot.</param>
    /// <returns>The hash, 1..2^32-1.</returns>
    inline uint32_t CardPictureShotHash(std::wstring_view snapshot) noexcept
    {
        uint32_t h = 2166136261u;
        for (const wchar_t c : snapshot)
        {
            h ^= static_cast<uint32_t>(c);
            h *= 16777619u;
        }
        return h == 0 ? 1u : h;
    }

    /// <summary>What a 🖼️ tap's read does about the card's picture (PlanCardPicture).</summary>
    enum class CardPictureSource
    {
        None, // no picture: no screen could be read and the card never had a picture
        Reuse, // the picture Telegram already holds, by its file_id (no upload)
        Upload, // a new picture: the screen is drawn and uploaded with the card's next edit
    };

    /// <summary>
    /// How a 🖼️ tap's read settled - what the tap's continuation learns besides the picture itself (MessagingService::
    /// _withCardPicture hands both over). Only Fresh brings a picture for the card; every other outcome picks the tap's toast
    /// (CardPictureTapReadText), so "the window did not answer" is never told as "there is no screen to read".
    /// </summary>
    enum class CardPictureRead
    {
        Fresh, // the hosting window answered in time and the screen was read: the picture (drawn, or the held photo of that very
               // screen) shows this moment
        NoScreen, // the hosting window answered, but its tab shows no screen: a parked tab, or a terminal not up yet
        Stale, // no picture in time: the window did not answer within kCardPictureTapWaitMs (a busy or frozen UI thread), no window
               // answered (none hosts the tab), or the read or the drawing failed
        Blank, // the hosting window answered with a screen that shows NOTHING (ScreenSnapshotShowsNothing: a tab whose claude is
               // still starting) - never drawn: a black picture would replace the card's last real one
    };

    /// <summary>The decision for one 🖼️ tap's read: where its picture comes from, and the moment its caption names.</summary>
    struct CardPicturePlan
    {
        CardPictureSource source{ CardPictureSource::None };
        int64_t shotAtMs{ 0 }; // the caption's moment: now for a screen read now (drawn, or found unchanged), else the stored picture's
    };

    /// <summary>
    /// Plans a 🖼️ tap's picture from what was read and what the card shows: a screen read NOW that differs from the stored
    /// picture's (or with no stored picture) is drawn and uploaded; a screen read now that is the stored picture's own reuses
    /// it, captioned with NOW (the screen was looked at and is still that); no screen (a parked tab, no window, a read that
    /// failed) keeps the stored picture with its OWN moment ("the screen at 19:50" stays true), or none at all. Pure.
    /// </summary>
    /// <param name="readable">The window answered with a screen (a non-empty snapshot that shows something).</param>
    /// <param name="shotHash">That screen's CardPictureShotHash (ignored when not readable).</param>
    /// <param name="nowMs">Now (Unix ms).</param>
    /// <param name="storedFileId">The file_id of the picture the card last showed ("" = none).</param>
    /// <param name="storedShotHash">That picture's screen hash (0 = unknown: never reused for a read screen).</param>
    /// <param name="storedAtMs">The moment that picture's screen was read.</param>
    /// <returns>The plan.</returns>
    inline CardPicturePlan PlanCardPicture(bool readable, uint32_t shotHash, int64_t nowMs, std::wstring_view storedFileId, uint32_t storedShotHash, int64_t storedAtMs) noexcept
    {
        CardPicturePlan plan;
        if (readable)
        {
            if (!storedFileId.empty() && storedShotHash != 0 && storedShotHash == shotHash)
            {
                plan.source = CardPictureSource::Reuse;
            }
            else
            {
                plan.source = CardPictureSource::Upload;
            }
            plan.shotAtMs = nowMs;
            return plan;
        }
        if (!storedFileId.empty())
        {
            plan.source = CardPictureSource::Reuse;
            plan.shotAtMs = storedAtMs;
        }
        return plan;
    }

    /// <summary>
    /// The picture a card post or edit carries (§21.49): the picture a 🖼️ tap took, while it waits for the send that delivers it;
    /// else the photo the card shows, reused by its file_id (no upload, no screen read) and captioned with ITS moment; else none.
    /// Every post and edit of a card that has a picture carries one, because a rich edit without it drops the photo (measured,
    /// §21.42) - so a status update, a page flip or a re-post below newer messages keeps the picture until the next tap. Pure.
    /// </summary>
    /// <param name="tapPicture">The tap's picture waiting for its send (null = none).</param>
    /// <param name="storedFileId">The file_id of the photo the card shows (TopicRecord::cardPictureFileId; "" = none).</param>
    /// <param name="storedAtMs">The moment that photo's screen was read (its caption's clock).</param>
    /// <param name="storedShotHash">That screen's hash (kept with the photo, so the next tap reuses it when the screen is the same).</param>
    /// <returns>The picture to carry: `tapPicture` itself (the same object, so the send's answer can match it), a held picture, or
    /// null when the card has none.</returns>
    /// <exception cref="std::bad_alloc">If the held picture cannot be allocated.</exception>
    inline std::shared_ptr<const CardPicture> CardPictureToCarry(const std::shared_ptr<const CardPicture>& tapPicture, std::wstring_view storedFileId, int64_t storedAtMs, uint32_t storedShotHash)
    {
        if (tapPicture)
        {
            return tapPicture;
        }
        if (storedFileId.empty())
        {
            return nullptr;
        }
        auto held = std::make_shared<CardPicture>();
        held->fileId = std::wstring{ storedFileId };
        held->shotAtMs = storedAtMs;
        held->shotHash = storedShotHash;
        return held;
    }

    /// <summary>
    /// A moment as a LOCAL clock time with seconds, for the picture's caption: "19:50:12" on the same local day as `nowMs`,
    /// else "16 Oct 19:50:12". With seconds because two taps of one minute must not read alike. The caption is outside the card's
    /// hash (MessagingService::_refreshCard hashes the classic text and the buttons), so a new picture's caption never causes an
    /// edit by itself.
    /// </summary>
    /// <param name="unixMs">The moment (Unix ms).</param>
    /// <param name="nowMs">The clock the "same day" test is made against.</param>
    /// <returns>"HH:MM:SS" or "D Mon HH:MM:SS"; "?" when the CRT cannot convert the moment.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring CardPictureClockText(int64_t unixMs, int64_t nowMs)
    {
        const auto toLocal = [](int64_t ms, std::tm& out) noexcept -> bool {
            const time_t t = static_cast<time_t>(ms / 1000);
            return localtime_s(&out, &t) == 0; // the CRT's thread-safe form; it writes nothing on failure
        };
        std::tm when{};
        std::tm now{};
        if (unixMs <= 0 || !toLocal(unixMs, when))
        {
            return L"?";
        }
        const auto two = [](int v) { return (v < 10 ? L"0" : L"") + std::to_wstring(v); };
        std::wstring clock = two(when.tm_hour) + L":" + two(when.tm_min) + L":" + two(when.tm_sec);
        if (toLocal(nowMs, now) && now.tm_year == when.tm_year && now.tm_yday == when.tm_yday)
        {
            return clock;
        }
        static constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const int mon = when.tm_mon >= 0 && when.tm_mon < 12 ? when.tm_mon : 0;
        return std::to_wstring(when.tm_mday) + L" " + kMonths[mon] + L" " + clock;
    }

    /// <summary>
    /// The picture's block in the card's rich HTML: a figure of the photo (tg://photo?id=screen, given in the `media` list) and
    /// its caption "🖥 the screen at 19:50:12". It goes LAST, after the card's text and before its buttons: the card leads with
    /// its state (§21.26), and the screen shows what that state looks like. Rich HTML takes media only as a block of its own
    /// (Telegram's formatting rules), which a figure is.
    /// </summary>
    /// <param name="shotAtMs">The moment the screen was read (CardPicturePlan::shotAtMs).</param>
    /// <param name="nowMs">Now (the caption's "same day" test).</param>
    /// <returns>The HTML block (plain text in it: nothing to escape).</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring CardPictureFigureHtml(int64_t shotAtMs, int64_t nowMs)
    {
        std::wstring html = L"<figure><img src=\"tg://photo?id=";
        html += kCardPictureMediaId;
        html += L"\"/><figcaption>\U0001F5A5 the screen at ";
        html += CardPictureClockText(shotAtMs, nowMs);
        html += L"</figcaption></figure>";
        return html;
    }

    /// <summary>
    /// The rich message's `media` list for a card picture: one InputRichMessageMedia whose id is kCardPictureMediaId and whose
    /// media is a photo - "attach://screen" for a new upload (the file then goes in the multipart field "screen"), else the
    /// held photo's file_id.
    /// </summary>
    /// <param name="picture">The picture (png set = an upload; else its fileId).</param>
    /// <returns>The JSON array.</returns>
    /// <exception cref="std::bad_alloc">If the JSON cannot be allocated.</exception>
    inline json::Value CardPictureMediaJson(const CardPicture& picture)
    {
        auto photo = json::Value::MkObj();
        photo.Set(L"type", json::Value::MkStr(L"photo"));
        photo.Set(L"media", json::Value::MkStr(picture.png.empty() ? picture.fileId : L"attach://" + std::wstring{ kCardPictureMediaId }));
        auto entry = json::Value::MkObj();
        entry.Set(L"id", json::Value::MkStr(std::wstring{ kCardPictureMediaId }));
        entry.Set(L"media", std::move(photo));
        auto list = json::Value::MkArr();
        list.Push(std::move(entry));
        return list;
    }

    /// <summary>The photo a sent or edited card shows, read back from Telegram's answer (CardPictureFromMessage).</summary>
    struct CardPictureShown
    {
        std::wstring fileId; // the largest size's file_id: what the next post or edit reuses ("" = the message shows no photo)
        std::wstring uniqueId; // its file_unique_id (the same picture across sizes and messages; for the log)
        int width{ 0 }; // the largest size's width in pixels
        int height{ 0 }; // and height
    };

    /// <summary>
    /// The photo a rich message shows, from the Message Telegram returned for a post or an edit: its rich_message's first photo
    /// block, the LARGEST of the sizes Telegram made of it (reusing that size's file_id keeps the full resolution).
    /// </summary>
    /// <param name="message">The returned Message (TgResult::result); anything else reads as no photo.</param>
    /// <returns>The photo, or an empty fileId when the message shows none (a text-only card, a refused picture, a classic form).</returns>
    /// <exception cref="std::bad_alloc">If the ids cannot be copied.</exception>
    inline CardPictureShown CardPictureFromMessage(const json::Value& message)
    {
        CardPictureShown shown;
        if (message.type != json::Value::Type::Obj)
        {
            return shown;
        }
        const auto* rich = message.Find(L"rich_message");
        const auto* blocks = rich && rich->type == json::Value::Type::Obj ? rich->Find(L"blocks") : nullptr;
        if (!blocks || blocks->type != json::Value::Type::Arr)
        {
            return shown;
        }
        for (const auto& block : blocks->arr)
        {
            if (block.type != json::Value::Type::Obj || block.StrAt(L"type") != L"photo")
            {
                continue;
            }
            const auto* sizes = block.Find(L"photo");
            if (!sizes || sizes->type != json::Value::Type::Arr)
            {
                continue;
            }
            int64_t bestArea = -1;
            for (const auto& size : sizes->arr)
            {
                if (size.type != json::Value::Type::Obj || size.StrAt(L"file_id").empty())
                {
                    continue;
                }
                const int64_t w = size.I64At(L"width", 0);
                const int64_t h = size.I64At(L"height", 0);
                if (w * h > bestArea)
                {
                    bestArea = w * h;
                    shown.fileId = size.StrAt(L"file_id");
                    shown.uniqueId = size.StrAt(L"file_unique_id");
                    shown.width = static_cast<int>(w);
                    shown.height = static_cast<int>(h);
                }
            }
            if (!shown.fileId.empty())
            {
                return shown; // the first photo block is the card's picture (the card has no other)
            }
        }
        return shown;
    }

    /// <summary>
    /// How many refusals of a card WITH a picture in a row switch the pictures off for the rest of the run: a refusal is retried
    /// once without the picture (the card's text must go), and three in a row mean the chat or this Bot API will not take them -
    /// the cards then go as text, a 🖼️ tap says why (CardPictureTapRefusal::PicturesStopped), and a log line says so. A picture
    /// that goes through resets the count.
    /// </summary>
    inline constexpr int kCardPictureRefusalsToStop = 3;

    // ===== The 🖼️ tap (§21.49) ==================================================================

    /// <summary>
    /// The longest a 🖼️ tap waits for its picture - the hosting window's screen read plus the drawing - before it settles as
    /// Stale. Longer than the tap's own hold (kTapAnswerHoldMaxMs, 9 s, MessagingTaps.h) on purpose: a busy window answered 5-9 s
    /// late in production (2026-11-01, the Agent Manager page's rebuilds), and past the hold the tap's answer says the work goes
    /// on (kTapStillWorkingToast) while the picture still reaches the card when it comes. Bounded so a frozen window's read is
    /// forgotten: a picture later than this is dropped (one log line), and the next tap asks again. The send loop checks the
    /// deadline every pass (MessagingService::_expireCardPictureWaits).
    /// </summary>
    inline constexpr int64_t kCardPictureTapWaitMs = 30 * 1000;

    /// <summary>
    /// The longest a card counts as BUSY with a 🖼️ tap - its read, drawing and the card edit that carries the picture - so a
    /// second tap meanwhile is answered "already taking it" instead of reading and uploading the same screen twice. A tap's work
    /// clears it as soon as it ends (no picture, or the carrying send answered); the bound is the belt for a send that is never
    /// answered (an item the outbox dropped), which would otherwise keep the button refusing until the next start.
    /// </summary>
    inline constexpr int64_t kCardPictureTapBusyMs = 60 * 1000;

    /// <summary>Whether a card is still busy with a 🖼️ tap that began at `sinceTickMs` (kCardPictureTapBusyMs). Pure.</summary>
    /// <param name="sinceTickMs">When the tap's read began (GetTickCount64 ms; 0 = no tap's work runs).</param>
    /// <param name="nowTickMs">Now, on the same monotonic clock.</param>
    /// <returns>True while a tap's work may still run.</returns>
    inline bool CardPictureTapBusy(int64_t sinceTickMs, int64_t nowTickMs) noexcept
    {
        return sinceTickMs != 0 && nowTickMs - sinceTickMs < kCardPictureTapBusyMs;
    }

    /// <summary>Why a 🖼️ tap asks no window for the screen (MessagingService::_cardPictureFromTap's own checks, in this order).</summary>
    enum class CardPictureTapRefusal
    {
        Closed, // the session is closed: its tab is gone, nothing shows its screen
        PicturesOff, // the cog's "Show the screen in each session's card" is off
        PicturesStopped, // Telegram refused kCardPictureRefusalsToStop card pictures in a row: none for the rest of this run
        NoRich, // the cards go as classic HTML (rich messages off, or a Bot API without them): no classic message holds a picture
        Unavailable, // no window can read screens (a host without the window hooks: the m5 harness)
        Parked, // the tab is parked (its claude not started): it shows nothing until ▶ Reactivate starts it
        NoCard, // the session has no topic, or its topic no card, to put the picture on
        Busy, // a tap's read, drawing or delivery for this card runs already (kCardPictureTapBusyMs at most)
    };

    // The PicturesStopped toast names the count in words of its own (a string_view constant cannot be built from the number).
    static_assert(kCardPictureRefusalsToStop == 3, "CardPictureTapRefusalText says \"3 card pictures in a row\"");

    /// <summary>The toast of a 🖼️ tap that asks no window (CardPictureTapRefusal): why, and what to do. Pure.</summary>
    /// <param name="why">The refusal.</param>
    /// <returns>The toast (a static string; within kTapToastMaxChars).</returns>
    inline std::wstring_view CardPictureTapRefusalText(CardPictureTapRefusal why) noexcept
    {
        switch (why)
        {
        case CardPictureTapRefusal::Closed:
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        case CardPictureTapRefusal::PicturesOff:
            return L"The cards' screen pictures are off (the cog's Messaging tab: \x201CShow the screen in each session's card\x201D).";
        case CardPictureTapRefusal::PicturesStopped:
            return L"Telegram refused 3 card pictures in a row, so the cards go without them until Agentmaster starts again.";
        case CardPictureTapRefusal::NoRich:
            return L"The cards go as plain HTML (rich messages are off), and a plain card cannot hold a picture.";
        case CardPictureTapRefusal::Parked:
            return L"This tab is parked: its screen shows nothing until \x25B6 Reactivate starts it.";
        case CardPictureTapRefusal::NoCard:
            return L"This session has no card to put the picture on.";
        case CardPictureTapRefusal::Busy:
            return L"Taking the screen already \x2014 it shows on the card in a moment.";
        case CardPictureTapRefusal::Unavailable:
        default:
            return L"Not available here.";
        }
    }

    /// <summary>A refusal's word in hooks.log's "[nav] remote telegram cardpic" line. Pure.</summary>
    /// <param name="why">The refusal.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* CardPictureTapRefusalWord(CardPictureTapRefusal why) noexcept
    {
        switch (why)
        {
        case CardPictureTapRefusal::Closed:
            return L"closed";
        case CardPictureTapRefusal::PicturesOff:
            return L"pictures-off";
        case CardPictureTapRefusal::PicturesStopped:
            return L"pictures-stopped";
        case CardPictureTapRefusal::NoRich:
            return L"classic-cards";
        case CardPictureTapRefusal::Parked:
            return L"parked";
        case CardPictureTapRefusal::NoCard:
            return L"no-card";
        case CardPictureTapRefusal::Busy:
            return L"busy";
        case CardPictureTapRefusal::Unavailable:
        default:
            return L"unavailable";
        }
    }

    /// <summary>
    /// The toast of a 🖼️ tap whose read brought no picture: why, and what to do. "" for Fresh - the picture appearing on the card
    /// is the answer (the tapped button keeps spinning until the edit that carries it is in, MessagingTaps.h). Pure.
    /// </summary>
    /// <param name="read">How the read settled.</param>
    /// <param name="tabStarted">The session's tab runs its claude (SessionInfo live and started): NoScreen then means the terminal
    /// is not up yet; otherwise the tab is parked.</param>
    /// <returns>The toast (a static string; "" = none).</returns>
    inline std::wstring_view CardPictureTapReadText(CardPictureRead read, bool tabStarted) noexcept
    {
        switch (read)
        {
        case CardPictureRead::NoScreen:
            return tabStarted ? std::wstring_view{ L"The tab's screen is not up yet \x2014 tap \U0001F5BC\xFE0F again in a moment." } :
                                CardPictureTapRefusalText(CardPictureTapRefusal::Parked);
        case CardPictureRead::Blank:
            return L"Claude is still starting: its screen shows nothing yet \x2014 tap \U0001F5BC\xFE0F again in a moment.";
        case CardPictureRead::Stale:
            return L"No picture this time: the tab's window did not answer, or the screen could not be drawn \x2014 tap \U0001F5BC\xFE0F again.";
        case CardPictureRead::Fresh:
        default:
            return {};
        }
    }

    /// <summary>A read's word in hooks.log's "[nav] remote telegram cardpic" line ("fresh", "noscreen", "stale", "blank"). Pure.</summary>
    /// <param name="read">How the read settled.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* CardPictureReadWord(CardPictureRead read) noexcept
    {
        switch (read)
        {
        case CardPictureRead::NoScreen:
            return L"noscreen";
        case CardPictureRead::Stale:
            return L"stale";
        case CardPictureRead::Blank:
            return L"blank";
        case CardPictureRead::Fresh:
        default:
            return L"fresh";
        }
    }

    /// <summary>
    /// The toast after the send that carried a 🖼️ tap's picture was answered: "" when the card shows it; else why it does not -
    /// Telegram refused the picture (the card's text went again without it), or the call failed. The caller leaves out a card
    /// the user deleted ("message to edit not found": the card posted in its place carries the picture). Pure.
    /// </summary>
    /// <param name="answered">Telegram answered ok, or "message is not modified" (the card shows this very content already).</param>
    /// <param name="photoShown">The card shows the photo: the answer's message has one (CardPictureFromMessage), or it was not
    /// modified.</param>
    /// <param name="httpStatus">The call's HTTP status (0 = no answer at all: a transport failure).</param>
    /// <returns>The toast ("" = none).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring CardPictureDeliveryText(bool answered, bool photoShown, int httpStatus)
    {
        if (answered && photoShown)
        {
            return {};
        }
        if (answered)
        {
            return L"Telegram refused the picture; the card's text went on without it.";
        }
        if (httpStatus <= 0)
        {
            return L"The picture did not reach Telegram (no answer) \x2014 tap \U0001F5BC\xFE0F again.";
        }
        return L"The picture did not reach Telegram (HTTP " + std::to_wstring(httpStatus) + L") \x2014 tap \U0001F5BC\xFE0F again.";
    }
}
