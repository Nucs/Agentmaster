// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.59: the session's NOTE in Telegram (the user's request of 2026-11-18: "We need note in
// telegram bottom pinned message and through cog icon we can edit the note and such", and their picks on mockups the same day:
// the note under the card's title, a 🗒️ Note ▸ page, "Reply or next message" with an expiration message, ➕ Add a line, /note, and
// a line when the note changes at the PC). This file pins the pure half (MessagingNote.h), the card's parts (MessagingModels.h)
// and the store's cut report:
//   1. the note prompt's window (DecideNoteWindowMessage): a reply to the prompt, the next message, a command, a queue gesture to
//      another message, a message sent outside the window (with the clocks' slack), a lone ❌, a held message, no text, "btw …";
//   2. the expiry (NoteSentInWindow, DecideNoteWindowTick): open, expired, waiting for a voice note sent in time, and its limit;
//   3. the composed note (ComposeWindowNote) and /note's reading (ParseNoteCommandArgs, the command's place in the topic's list);
//   4. the PC-change verdict (ClassifyNoteChange);
//   5. the texts: the prompt (its row marker, its copy block, escaped), the placeholder, the expiration message, /note's answer,
//      the PC-change line, the cut line, the usage, the toasts within Telegram's limits;
//   6. the card: the note block (escaped, its cap, never a split surrogate), its place right under the title, no change for an
//      un-noted card, the Session page's 🗒️ Note ▸, the Note page in its four forms, its confirmation, the page words;
//   7. NormalizeSessionNote's wasCut.
// The service's half (the window's state, the prompt's post and delete, the store write, the tick's pass) needs Telegram: the live
// check is the dev bot (MESSAGING.md §21.59, "Checks after the build").

#include "m5_tests.h"

#include <string>
#include <vector>

#include "../MessagingModels.h" // BuildCardButtonRows, BuildCardText, CardNoteBlockHtml, CardPage, ParseRemoteCommand, kAgentmasterCommands
#include "../MessagingNote.h"
#include "../SessionStore.h" // NormalizeSessionNote, kMaxSessionNoteLength

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    using Rows = std::vector<std::vector<std::pair<std::wstring, std::wstring>>>;

    /// <summary>The session id every card test here uses (36 characters, like a real one: the data-length checks need it).</summary>
    const std::wstring kNoteSid = L"89abcdef-0123-4567-89ab-cdef01234567";

    /// <summary>A live, started Claude session waiting for the user, titled, with an Auto runner (the card's common case).</summary>
    /// <returns>The record.</returns>
    SessionInfo NoteCardSession()
    {
        SessionInfo s = MakeSession(kNoteSid, SessionState::WaitingForInput);
        s.live = true;
        s.started = true;
        s.title = L"Notes card";
        s.workingDir = L"K:\\source\\Agentmaster";
        s.autorunner.mode = AutorunnerMode::Full;
        return s;
    }

    /// <summary>The facts of a window opened at 1,000,000 ms for five minutes, with prompt message 500.</summary>
    /// <param name="text">The message's text.</param>
    /// <param name="dateMs">The message's date (Unix ms; 0 = unknown).</param>
    /// <param name="replyTo">The message it replies to (0 = none).</param>
    /// <returns>The facts.</returns>
    NoteMessageFacts Facts(std::wstring_view text, int64_t dateMs, int64_t replyTo = 0)
    {
        NoteMessageFacts f;
        f.text = text;
        f.messageDateMs = dateMs;
        f.replyToMessageId = replyTo;
        f.promptMessageId = 500;
        f.openedMs = 1'000'000;
        f.expiresMs = 1'000'000 + kNoteWindowMs;
        return f;
    }

    /// <summary>Whether any button of the rows carries this callback data.</summary>
    /// <param name="rows">The rows.</param>
    /// <param name="data">The data.</param>
    /// <returns>True when a button has it.</returns>
    bool HasNoteData(const Rows& rows, const std::wstring& data)
    {
        for (const auto& row : rows)
        {
            for (const auto& b : row)
            {
                if (b.second == data)
                {
                    return true;
                }
            }
        }
        return false;
    }

    /// <summary>Whether a text holds a high surrogate that no low surrogate follows (a split pair: invalid UTF-16).</summary>
    /// <param name="s">The text.</param>
    /// <returns>True when a pair is split.</returns>
    bool HasSplitSurrogate(const std::wstring& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c >= 0xD800 && c <= 0xDBFF && (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF))
            {
                return true;
            }
        }
        return false;
    }
}

void TestMessagingNote()
{
    // ASCII only, like the other groups' headers: the console's code page would garble a section sign.
    std::wprintf(L"== TestMessagingNote (MESSAGING.md 21.59: the session's note in Telegram - the prompt's window, the expiry, /note, the texts, the card) ==\n");

    // ===== 1. the note prompt's window =====
    {
        const int64_t in = 1'000'000 + 60'000; // a minute into the window
        CHECK(DecideNoteWindowMessage(Facts(L"Ship it after lunch", in)) == NoteMessageVerdict::Note, "window: a plain message sent inside the window is the note");
        CHECK(DecideNoteWindowMessage(Facts(L"Ship it after lunch", 0)) == NoteMessageVerdict::Note, "window: a message of unknown date is judged as sent now (the note)");
        CHECK(DecideNoteWindowMessage(Facts(L"Ship it", in, 500)) == NoteMessageVerdict::Note, "window: a reply to the prompt is the note");
        CHECK(DecideNoteWindowMessage(Facts(L"Ship it", 1'000'000 + kNoteWindowMs + 3'600'000, 500)) == NoteMessageVerdict::Note,
              "window: a reply to the prompt is the note whatever its date (the clearest answer; the window still exists)");
        CHECK(DecideNoteWindowMessage(Facts(L"\x274C", in, 500)) == NoteMessageVerdict::Cancel, "window: a cross mark replying to the prompt cancels it");
        CHECK(DecideNoteWindowMessage(Facts(L"x", in, 500)) == NoteMessageVerdict::Cancel, "window: an x replying to the prompt cancels it (the queue's X vocabulary)");
        CHECK(DecideNoteWindowMessage(Facts(L"+", in, 500)) == NoteMessageVerdict::Note, "window: a + replying to the prompt is text (only the X cancels)");
        CHECK(DecideNoteWindowMessage(Facts(L"\x274C", in)) == NoteMessageVerdict::Cancel, "window: a lone cross mark (not a reply) cancels the prompt");
        CHECK(DecideNoteWindowMessage(Facts(L"  \x274C\xFE0F  ", in)) == NoteMessageVerdict::Cancel, "window: a lone cross mark with VS16 and spaces cancels too");
        CHECK(DecideNoteWindowMessage(Facts(L"\x274C", in, 777)) == NoteMessageVerdict::NotNote, "window: a cross-mark REPLY to another message is the queue's gesture, never the note");
        CHECK(DecideNoteWindowMessage(Facts(L"\x2795", in, 777)) == NoteMessageVerdict::NotNote, "window: a plus reply to another message is the queue's gesture too");
        CHECK(DecideNoteWindowMessage(Facts(L"Remember the flags", in, 777)) == NoteMessageVerdict::Note,
              "window: a text replying to another message (the topic's creation message, a quote) is the note");
        CHECK(DecideNoteWindowMessage(Facts(L"/status", in)) == NoteMessageVerdict::NotNote, "window: a command keeps its meaning (/status)");
        CHECK(DecideNoteWindowMessage(Facts(L"  /note write this", in, 500)) == NoteMessageVerdict::NotNote,
              "window: a command keeps its meaning even as a reply to the prompt (/note writes the note itself)");
        CHECK(DecideNoteWindowMessage(Facts(L"btw the build is red", in)) == NoteMessageVerdict::Note, "window: 'btw ...' is the note, not a side question (the user is writing)");
        CHECK(DecideNoteWindowMessage(Facts(L"", in)) == NoteMessageVerdict::NotNote, "window: a message with no text (a sticker, a bare photo) is no note");
        CHECK(DecideNoteWindowMessage(Facts(L" \n\t ", in)) == NoteMessageVerdict::NotNote, "window: a blank text is no note");
        NoteMessageFacts held = Facts(L"Ship it", in);
        held.held = true;
        CHECK(DecideNoteWindowMessage(held) == NoteMessageVerdict::NotNote, "window: a message a new thread held is never the note");
        NoteMessageFacts none = Facts(L"Ship it", in);
        none.expiresMs = 0;
        CHECK(DecideNoteWindowMessage(none) == NoteMessageVerdict::NotNote, "window: no window, no note");
        const int64_t end = 1'000'000 + kNoteWindowMs;
        CHECK(DecideNoteWindowMessage(Facts(L"late", end + kNoteClockSkewMs)) == NoteMessageVerdict::Note, "window: a message within the clocks' slack after the end counts");
        CHECK(DecideNoteWindowMessage(Facts(L"late", end + kNoteClockSkewMs + 1000)) == NoteMessageVerdict::NotNote, "window: a message sent after the window goes its usual way");
        CHECK(DecideNoteWindowMessage(Facts(L"early", 1'000'000 - kNoteClockSkewMs)) == NoteMessageVerdict::Note, "window: a message within the slack before the open counts");
        CHECK(DecideNoteWindowMessage(Facts(L"early", 1'000'000 - kNoteClockSkewMs - 1000)) == NoteMessageVerdict::NotNote,
              "window: a message sent before the prompt opened is no note");
        NoteMessageFacts inFlight = Facts(L"Ship it", end + 600'000, -1);
        inFlight.promptMessageId = -1;
        CHECK(DecideNoteWindowMessage(inFlight) == NoteMessageVerdict::NotNote, "window: a prompt still in flight (-1) matches no reply; a late message is not the note");
        CHECK(kNoteWindowMs == 5 * 60 * 1000, "window: five minutes (the user's pick, MESSAGING.md 21.59)");
    }

    // ===== 2. the expiry =====
    {
        NoteWindow w;
        w.openedMs = 1'000'000;
        w.expiresMs = 1'000'000 + kNoteWindowMs;
        CHECK(DecideNoteWindowTick(w, w.expiresMs, false) == NoteWindowTick::Open, "expiry: open up to its end");
        CHECK(DecideNoteWindowTick(w, w.expiresMs + 1, false) == NoteWindowTick::Expire, "expiry: past its end with nothing pending: expire");
        CHECK(DecideNoteWindowTick(w, w.expiresMs + 1, true) == NoteWindowTick::WaitForVoice, "expiry: a voice note sent in time and still transcribing is waited for");
        CHECK(DecideNoteWindowTick(w, w.expiresMs + kNoteWindowVoiceGraceMs + 1, true) == NoteWindowTick::Expire, "expiry: the wait for a voice note has its limit");
        CHECK(NoteSentInWindow(w, w.openedMs + 1) && NoteSentInWindow(w, w.expiresMs + kNoteClockSkewMs) && NoteSentInWindow(w, w.openedMs - kNoteClockSkewMs),
              "expiry: a voice note sent inside the window (with the slack) counts");
        CHECK(!NoteSentInWindow(w, w.expiresMs + kNoteClockSkewMs + 1) && !NoteSentInWindow(w, w.openedMs - kNoteClockSkewMs - 1) && !NoteSentInWindow(w, 0),
              "expiry: one sent outside it (or of unknown date) does not");
    }

    // ===== 3. the composed note and /note =====
    {
        CHECK(ComposeWindowNote(NoteWriteMode::Replace, L"old", L"new") == L"new", "compose: Edit replaces the note");
        CHECK(ComposeWindowNote(NoteWriteMode::Append, L"first", L"second") == L"first\nsecond", "compose: Add a line puts the text on a line of its own under the note");
        CHECK(ComposeWindowNote(NoteWriteMode::Append, L"", L"only") == L"only" && ComposeWindowNote(NoteWriteMode::Append, L" \n ", L"only") == L"only",
              "compose: adding to no note (or a blank one) is writing it");

        const NoteCommand show = ParseNoteCommandArgs(L"");
        CHECK(show.kind == NoteCommandKind::Show && ParseNoteCommandArgs(L"  \n ").kind == NoteCommandKind::Show, "/note: bare (or blank words) shows the note");
        CHECK(ParseNoteCommandArgs(L"-").kind == NoteCommandKind::Remove && ParseNoteCommandArgs(L" \x2014 ").kind == NoteCommandKind::Remove &&
                  ParseNoteCommandArgs(L"\x2013").kind == NoteCommandKind::Remove,
              "/note: a dash removes (the en and em dashes a phone makes of it too)");
        const NoteCommand set = ParseNoteCommandArgs(L"  Ship it\nthen tag  ");
        CHECK(set.kind == NoteCommandKind::Set && set.text == L"Ship it\nthen tag", "/note: words write the note, trimmed, its line breaks kept");
        CHECK(ParseNoteCommandArgs(L"- not a dash alone").kind == NoteCommandKind::Set, "/note: a dash followed by words is a note, not a removal");
        const NoteCommand add = ParseNoteCommandArgs(L"+ deployed to dev");
        CHECK(add.kind == NoteCommandKind::Append && add.text == L"deployed to dev", "/note: + adds a line (the space after it dropped)");
        CHECK(ParseNoteCommandArgs(L"+deployed").text == L"deployed" && ParseNoteCommandArgs(L"+deployed").kind == NoteCommandKind::Append, "/note: +text without a space adds too");
        CHECK(ParseNoteCommandArgs(L"+").kind == NoteCommandKind::Usage && ParseNoteCommandArgs(L"+   ").kind == NoteCommandKind::Usage, "/note: + alone gets the usage");

        const RemoteCommand typed = ParseRemoteCommand(L"/Note Ship it\nthen tag");
        CHECK(typed.kind == RemoteKind::AgentmasterCommand && typed.name == kNoteCommand && typed.args == L"Ship it\nthen tag",
              "/note: Agentmaster's command in a session's topic (any case), its words with their line breaks");
        bool listed = false;
        for (const auto& c : kAgentmasterCommands)
        {
            listed = listed || c == kNoteCommand;
        }
        CHECK(listed, "/note: in kAgentmasterCommands (never queued for Claude Code)");
    }

    // ===== 4. the PC-change verdict =====
    {
        const std::wstring shown = L"Ship it";
        const std::wstring empty;
        CHECK(ClassifyNoteChange(nullptr, L"Ship it") == NoteChange::FirstSight && ClassifyNoteChange(nullptr, L"") == NoteChange::FirstSight,
              "change: a session never shown is a first sight (nothing said)");
        CHECK(ClassifyNoteChange(&shown, L"Ship it") == NoteChange::None, "change: the shown note stored: nothing to do (the service's own write)");
        CHECK(ClassifyNoteChange(&shown, L"Ship it today") == NoteChange::Changed, "change: another text was written elsewhere (the PC)");
        CHECK(ClassifyNoteChange(&shown, L"") == NoteChange::Removed, "change: the note is gone (removed at the PC)");
        CHECK(ClassifyNoteChange(&empty, L"") == NoteChange::None && ClassifyNoteChange(&empty, L"New") == NoteChange::Changed,
              "change: from no note, a new one is a change, none stays none");
    }

    // ===== 5. the texts =====
    {
        const int64_t now = 1'763'460'000'000; // a fixed moment; the clock text only needs the same day for both
        const std::wstring prompt = NotePromptHtml(NoteWriteMode::Replace, L"a < b & c", now + kNoteWindowMs, now);
        CHECK(prompt.find(L"<b>New note</b>") != std::wstring::npos && prompt.find(kTgTitleRowMarker) != std::wstring::npos,
              "texts: the prompt opens with its header, the title row's marker on line 2");
        CHECK(prompt.find(L"<pre>a &lt; b &amp; c</pre>") != std::wstring::npos, "texts: the prompt's copy block holds the note, escaped");
        CHECK(prompt.find(L"until " + TgClockText(now + kNoteWindowMs, now)) != std::wstring::npos && prompt.find(L"\x274C cancels") != std::wstring::npos,
              "texts: the prompt says until when, and that a cross mark cancels");
        const std::wstring first = NotePromptHtml(NoteWriteMode::Replace, L"", now + kNoteWindowMs, now);
        CHECK(first.find(L"<b>Write a note</b>") != std::wstring::npos && first.find(L"<pre>") == std::wstring::npos, "texts: with no note, 'Write a note' and no copy block");
        const std::wstring add = NotePromptHtml(NoteWriteMode::Append, L"kept", now + kNoteWindowMs, now);
        CHECK(add.find(L"<b>Add a line to the note</b>") != std::wstring::npos && add.find(L"<pre>") == std::wstring::npos, "texts: Add a line shows no copy block");
        CHECK(NotePromptPlaceholder(NoteWriteMode::Replace) == L"The new note" && NotePromptPlaceholder(NoteWriteMode::Append) == L"The line to add" &&
                  NotePromptPlaceholder(NoteWriteMode::Replace).size() <= 64,
              "texts: the reply box's placeholders (Telegram's 64-character limit)");
        const std::wstring expired = NoteExpiredHtml(now, now);
        CHECK(expired.find(L"Note prompt expired") != std::wstring::npos && expired.find(kTgTitleRowMarker) != std::wstring::npos &&
                  expired.find(L"go to Claude again") != std::wstring::npos,
              "texts: the expiration message says it expired and that messages go to Claude again");
        const std::wstring shownNote = NoteShowHtml(L"line <1>\nline 2", now - 60'000, now);
        CHECK(shownNote.find(L"<b>Note</b> \x00B7 edited ") != std::wstring::npos && shownNote.find(L"line &lt;1&gt;\nline 2") != std::wstring::npos,
              "texts: /note shows the whole note escaped, its line breaks kept, with its edit time");
        CHECK(NoteShowHtml(L"x", 0, now).find(L"edited") == std::wstring::npos, "texts: /note names no time when the stamp is unknown");
        CHECK(NoteShowHtml(L"", 0, now).find(L"<b>No note</b>") != std::wstring::npos && NoteShowHtml(L"", 0, now).find(L"/note &lt;text&gt;") != std::wstring::npos,
              "texts: /note with no note says how to write one");
        const std::wstring changed = NotePcChangeHtml(L"first <line>\nsecond");
        CHECK(changed.find(L"Note changed at the PC") != std::wstring::npos && changed.find(L"<i>first &lt;line&gt;</i>") != std::wstring::npos &&
                  changed.find(L"second") == std::wstring::npos,
              "texts: the PC-change line quotes the note's first line only, escaped");
        CHECK(NotePcChangeHtml(L"").find(L"Note removed at the PC") != std::wstring::npos && NotePcChangeHtml(L"").find(L"<i>") == std::wstring::npos,
              "texts: a removal at the PC says so, with no quote");
        CHECK(NoteCutHtml().find(std::to_wstring(kMaxSessionNoteLength)) != std::wstring::npos, "texts: the cut line names the cap");
        CHECK(NoteUsageHtml().find(L"&lt;text&gt;") != std::wstring::npos && NoteUsageHtml().find(L"<text>") == std::wstring::npos, "texts: the usage is escaped");
        CHECK(NoteOpenToast(NoteWriteMode::Replace, now + kNoteWindowMs, now).size() <= 200 && NoteOpenToast(NoteWriteMode::Append, now + kNoteWindowMs, now).size() <= 200 &&
                  kNoteCancelledToast.size() <= 200 && kNoteNoPromptToast.size() <= 200 && kNoteRemovedToast.size() <= 200 &&
                  kNoteNothingToRemoveToast.size() <= 200,
              "texts: every toast within Telegram's 200 characters");
        CHECK(kNoteWrittenReaction == L"\U0001F44C", "texts: the written note's reaction is the OK hand (in Telegram's reaction list)");
    }

    // ===== 6. the card =====
    {
        CHECK(CardNoteBlockHtml(L"").empty(), "card: no note, no block");
        const std::wstring block = CardNoteBlockHtml(L"a <b>\nnext");
        CHECK(block == L"\n<blockquote expandable>\U0001F5D2\xFE0F a &lt;b&gt;\nnext</blockquote>", "card: the note in an expandable quote, escaped, its line breaks kept");
        std::wstring longNote(1500, L'n');
        const std::wstring longBlock = CardNoteBlockHtml(longNote);
        CHECK(longBlock.find(L"\x2026\n/note shows all of it") != std::wstring::npos && longBlock.find(std::wstring(kCardNoteMaxChars, L'n')) != std::wstring::npos &&
                  longBlock.find(std::wstring(kCardNoteMaxChars + 1, L'n')) == std::wstring::npos,
              "card: a long note is cut at kCardNoteMaxChars and names /note");
        std::wstring pairAtCut(kCardNoteMaxChars - 1, L'p');
        pairAtCut += L"\U0001F600 tail that goes past the cap";
        CHECK(!HasSplitSurrogate(CardNoteBlockHtml(pairAtCut)), "card: the cut never splits a surrogate pair");

        const SessionInfo s = NoteCardSession();
        CardExtras plain;
        CardExtras noted;
        noted.noteText = L"Ship it after lunch";
        const std::wstring withoutNote = BuildCardText(s, 0, 0, false, &plain);
        const std::wstring withNote = BuildCardText(s, 0, 0, false, &noted);
        const size_t title = withNote.find(L"Notes card");
        const size_t quote = withNote.find(L"<blockquote expandable>");
        const size_t place = withNote.find(L"/source/Agentmaster");
        CHECK(title != std::wstring::npos && quote != std::wstring::npos && place != std::wstring::npos && title < quote && quote < place,
              "card: the note's quote comes right under the title, above the place line");
        CHECK(withoutNote.find(L"blockquote") == std::wstring::npos && withNote.size() > withoutNote.size(), "card: an un-noted card has no quote");
        CHECK(BuildCardText(s, 0, 0, false, nullptr) == withoutNote, "card: an un-noted card's text is what it was before 21.59 (no card edited for it)");

        CardButtonFacts f = CardButtonFactsFor(s);
        CHECK(!f.hasNote && !f.notePromptOpen, "card: the note facts are the service's, never read from the session");
        const Rows session = BuildCardButtonRows(kNoteSid, f, CardPage::Session);
        CHECK(HasNoteData(session, L"c:note:" + kNoteSid) && HasNoteData(session, L"c:allscreen:" + kNoteSid), "card: the Session page has Note beside All Screen");

        const Rows empty = BuildCardButtonRows(kNoteSid, f, CardPage::Note);
        CHECK(empty.size() == 2 && empty[0].size() == 1 && empty[0][0].second == L"c:noteedit:" + kNoteSid && empty[0][0].first.find(L"Write a note") != std::wstring::npos &&
                  empty[1].size() == 1 && empty[1][0].second == L"c:panel:" + kNoteSid,
              "card: the Note page of a session with no note is Write a note, then Back (to Session)");
        f.hasNote = true;
        const Rows noted2 = BuildCardButtonRows(kNoteSid, f, CardPage::Note);
        CHECK(noted2.size() == 3 && noted2[0].size() == 2 && noted2[0][0].second == L"c:noteedit:" + kNoteSid && noted2[0][1].second == L"c:noteadd:" + kNoteSid &&
                  noted2[1].size() == 1 && noted2[1][0].second == L"c:noterm:" + kNoteSid && noted2[2][0].second == L"c:panel:" + kNoteSid,
              "card: the Note page of a noted session is Edit note and Add a line, Remove note, Back");
        f.notePromptOpen = true;
        const Rows open = BuildCardButtonRows(kNoteSid, f, CardPage::Note);
        CHECK(open.size() == 4 && open[0].size() == 1 && open[0][0].second == L"c:notecancel:" + kNoteSid && open[0][0].first.find(L"Cancel") != std::wstring::npos,
              "card: while a note prompt waits, its Cancel comes first on the Note page");
        const Rows confirm = BuildCardButtonRows(kNoteSid, f, CardPage::NoteRemove);
        CHECK(confirm.size() == 2 && confirm[0][0].second == L"c:notermok:" + kNoteSid && confirm[1][0].second == L"c:note:" + kNoteSid &&
                  confirm[1][0].first.find(L"Keep it") != std::wstring::npos,
              "card: Remove note asks once more - Yes, remove the note, or Keep it (back to the Note page)");
        CHECK(!HasNoteData(BuildCardButtonRows(kNoteSid, f, CardPage::Main), L"c:note:" + kNoteSid), "card: Main has no Note button (it is the Session page's)");
        CHECK(std::wstring{ CardPageLogWord(CardPage::Note) } == L"note" && std::wstring{ CardPageLogWord(CardPage::NoteRemove) } == L"note-remove",
              "card: the trail's words for the two new pages");
        CHECK(!AllScreenBoxesOf(CardPage::Note) && !AllScreenBoxesOf(CardPage::NoteRemove), "card: the Note pages are not the All Screen picker");
        CHECK(CardCallbackData(kCardVerbNoteCancel, kNoteSid).size() == 49 && CardCallbackData(kCardVerbNoteRemoveYes, kNoteSid).size() == 47 &&
                  CardCallbackData(kCardVerbNotePage, kNoteSid).size() == 43,
              "card: the Note page's data (c:notecancel: the longest) within Telegram's 64 bytes");
    }

    // ===== 7. the store's cut report =====
    {
        bool cut = true;
        CHECK(NormalizeSessionNote(std::wstring(kMaxSessionNoteLength - 1, L'a'), &cut).size() == kMaxSessionNoteLength - 1 && !cut, "store: a note under the cap is not cut");
        CHECK(NormalizeSessionNote(std::wstring(kMaxSessionNoteLength + 500, L'a'), &cut).size() == kMaxSessionNoteLength && cut, "store: a note over the cap is cut, and says so");
        CHECK(NormalizeSessionNote(std::wstring(kMaxSessionNoteLength, L'a') + std::wstring(300, L' '), &cut).size() == kMaxSessionNoteLength && !cut,
              "store: trailing blanks are trimmed before the cap is asked (not a cut)");
        CHECK(NormalizeSessionNote(L"  hi  ").size() == 2, "store: wasCut is optional (null = not asked)");
    }
}
