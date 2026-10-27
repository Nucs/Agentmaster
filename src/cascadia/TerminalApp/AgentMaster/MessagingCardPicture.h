// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the sticky card's SCREENSHOT (MESSAGING.md §21.42). The user's request (2026-10-27): "the running since
// message, called also i think status message that is pinned to the bottom - whenever it updates, have it produce a
// screenshot (if possible) and have it shown and so every update we produce a screenshot to show in the status message".
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
// So a card UPDATE (its text changed: a status, the since-clock, the context size of a running turn, the runner, the
// draft) reads the hosting window's screen (the 🖼 Screen Image snapshot) in the same moment; a screen that changed since the
// card's last picture is drawn and uploaded IN THE SAME CALL as the text (no extra call on the chat's ~20-a-minute budget);
// an unchanged screen reuses the picture Telegram already holds by its file_id (no upload). A change that is not an update -
// a page flip of the buttons, the slow form upgrade - keeps the current picture without reading the screen (a page flip
// must stay instant). A screen that cannot be read (a parked tab, no window hosts the tab) keeps the last picture, captioned
// with the moment it was taken; a card that never had one goes without.
//
// This header is the pure half: the media id, the picture as a message carries it, the plan (upload, reuse, none), the
// rich HTML block, the media list, reading the photo back from Telegram's answer, and the caption's clock. The m5 suite pins
// every rule (tests_messaging_card.cpp, section 1d).

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include "Json.h"

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
    /// The picture one card post or edit carries: either a NEW one to upload (`png`) or one Telegram already holds (`fileId`,
    /// reused with no upload), plus what it shows - the moment its screen was read and that screen's hash. Shared by the
    /// outbox items that carry it (a refused send goes again without it), never changed once made.
    /// </summary>
    struct CardPicture
    {
        std::string png; // the PNG to upload ("" = reuse fileId); a picture is ~0.1-0.6 MB (§21.24's measures)
        std::wstring fileId; // Telegram's file_id of a photo it holds; used only when png is empty
        int64_t shotAtMs{ 0 }; // when the screen this picture shows was read (Unix ms): the caption's clock
        uint32_t shotHash{ 0 }; // CardPictureShotHash of that screen: an unchanged screen reuses the picture
    };

    /// <summary>
    /// The identity of a screen snapshot (ScreenSnapshot.h's serialized form, as the window answers it): FNV-1a 32 over its
    /// UTF-16 code units, never 0 (0 = "no picture recorded"). A collision only shows the previous picture once more: the next
    /// change of the screen draws a new one.
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

    /// <summary>What a card post or edit does about its picture (PlanCardPicture).</summary>
    enum class CardPictureSource
    {
        None, // no picture: the card goes as text (no screen could be read and the card never had a picture)
        Reuse, // the picture Telegram already holds, by its file_id (no upload)
        Upload, // a new picture: the screen is drawn and uploaded in the same call
    };

    /// <summary>The decision for one card post or edit: where its picture comes from, and the moment its caption names.</summary>
    struct CardPicturePlan
    {
        CardPictureSource source{ CardPictureSource::None };
        int64_t shotAtMs{ 0 }; // the caption's moment: now for a screen read now (drawn, or found unchanged), else the stored picture's
    };

    /// <summary>
    /// Plans a card's picture from what was read and what the card shows: a screen read NOW that differs from the stored
    /// picture's (or with no stored picture) is drawn and uploaded; a screen read now that is the stored picture's own reuses
    /// it, captioned with NOW (the screen was looked at and is still that); no screen (a parked tab, no window, a read that
    /// failed) keeps the stored picture with its OWN moment ("the screen at 19:50" stays true), or none at all. Pure.
    /// </summary>
    /// <param name="readable">The window answered with a screen (a non-empty snapshot).</param>
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
    /// A moment as a LOCAL clock time with seconds, for the picture's caption: "19:50:12" on the same local day as `nowMs`,
    /// else "16 Oct 19:50:12". With seconds because a running card takes a new picture every ~20 s (its edit gap): two
    /// pictures of one minute must not read alike. The caption is outside the card's hash (MessagingService::_refreshCard
    /// hashes the classic text and the buttons), so a ticking clock here never causes an edit by itself.
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
    /// the cards then go as text, and a log line says so. A picture that goes through resets the count.
    /// </summary>
    inline constexpr int kCardPictureRefusalsToStop = 3;

    /// <summary>
    /// The longest a card post or edit waits for its picture - the hosting window's screen read plus the drawing - before it goes
    /// with the picture it shows (or none). A window answers in well under a second (the read itself is 0.3-0.6 ms, the drawing
    /// ~90-185 ms), but a FROZEN window (a UI stall) would hold its copy of the request unanswered: without this bound the card,
    /// whose new text is already recorded, would not change for as long as the freeze lasts - and that is when the phone is
    /// needed most. Past it the late picture is dropped (the card's send runs once). The send loop checks the deadlines
    /// (MessagingService::_expireCardPictureReads, every pass: at least every 0.5 s).
    /// </summary>
    inline constexpr int64_t kCardPictureWaitMs = 4 * 1000;
}
