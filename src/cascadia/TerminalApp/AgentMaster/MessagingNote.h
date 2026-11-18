// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session's NOTE in Telegram (MESSAGING.md §21.59; the note itself: SESSION_NOTES.md). The user's request
// (2026-11-18): "We need note in telegram bottom pinned message and through cog icon we can edit the note and such", and their
// picks on mockups the same day:
//   - the card shows the note right under its title, in an expandable quote (CardNoteBlockHtml, MessagingModels.h);
//   - ⚙️ Session's page gets 🗒️ Note ▸ beside 📜 All Screen ▸, opening a NOTE page: ✏️ Edit note · ➕ Add a line · 🗑 Remove note
//     (BuildCardButtonRows; the removal asks once more on the card, since the user picked no ↩ Undo);
//   - the text comes in a NOTE PROMPT: "Reply or next message" with "expiration msg" - after ✏️ / ➕ the user's NEXT message in
//     the topic, or a reply to the prompt, becomes the note, for kNoteWindowMs; a prompt nothing answered ends with a message
//     saying so (NoteExpiredHtml);
//   - and the extras ➕ Add a line, the /note command, and a line in the topic when the note is changed at the PC.
//
// Where the note lives: the session store (`note` + `noteAtUnixMs`, SessionStore.h), the one place the PC's editor writes too.
// The service never keeps a copy of its own beyond the text its cards SHOW; a write from either side moves SessionNotesEpoch(),
// which the PC's belts and the service's tick follow.
//
// This header is the pure half (no Telegram call, no service, no file): the window and what it does with a message, /note's
// reading, the composed note, the texts, the change verdict and the expiry verdict. The m5 suite pins every rule
// (tests_messaging_note.cpp). The service (Messaging.Note.cpp) owns the windows, posts and deletes the prompt, writes the store,
// and watches the epoch.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "MessagingQueueTickets.h" // ClassifyQueueReply (the ❌ / ➕ vocabulary of §21.57.7: a lone ❌ cancels a note prompt)
#include "SessionStore.h" // kMaxSessionNoteLength (the texts name the cap)
#include "TelegramFormat.h" // TgEscapeHtml, TgFirstLine, TgClockText, kTgTitleRowMarker

namespace Agentmaster::Msg
{
    // ===== the command and the window's constants ==========================================================================

    /// <summary>
    /// The command word (§21.59): "/note", typed in a session's topic. Bare = the whole note (NoteShowHtml); "/note &lt;text&gt;"
    /// writes the note; "/note + &lt;text&gt;" adds a line; "/note -" removes it (ParseNoteCommandArgs). It is in kAgentmasterCommands,
    /// so the topic never queues it for Claude Code, which has no /note of its own (checked on 2.1.322's command table).
    /// </summary>
    inline constexpr std::wstring_view kNoteCommand = L"note";

    /// <summary>
    /// How long a NOTE PROMPT waits for the text (§21.59, the user's pick "Reply or next message", 2026-11-18): five minutes from
    /// the tap. Inside it the user's next message in the topic becomes the note; past it, a message is what it always was (a prompt
    /// for Claude), and the prompt ends with NoteExpiredHtml. Long enough to type a note on a phone; short enough that a message
    /// meant for Claude much later is never taken.
    /// </summary>
    inline constexpr int64_t kNoteWindowMs = 5 * 60 * 1000;

    /// <summary>
    /// How long past the window the expiry waits for a VOICE NOTE that was sent inside it and is still being transcribed (§21.59):
    /// a transcription can take a while (a first worker start loads its model), and the note was sent in time. Past this the prompt
    /// expires anyway, and the transcript is read like any message (the window it was sent in is gone by then).
    /// </summary>
    inline constexpr int64_t kNoteWindowVoiceGraceMs = 3 * 60 * 1000;

    /// <summary>
    /// The slack for the two clocks a window compares (§21.59): a message's `date` is Telegram's (whole seconds), the window's ends
    /// are this PC's. Five seconds either way, so a message sent the instant the prompt went (or just as it ended) is not misread.
    /// </summary>
    inline constexpr int64_t kNoteClockSkewMs = 5 * 1000;

    /// <summary>How much of a note the PC-change line quotes (its first line, cut here): the card shows the rest.</summary>
    inline constexpr size_t kNoteChangeLineMaxChars = 160;

    /// <summary>
    /// The reaction on the user's message that WROTE the note (§21.59): 👌 - the note prompt's text, /note's words, a ❌ that closed a
    /// prompt. One of the emoji a bot may set (Telegram's fixed reaction list); the card showing the new note is the rest of the answer.
    /// </summary>
    inline constexpr std::wstring_view kNoteWrittenReaction = L"\U0001F44C";

    // ===== the window ======================================================================================================

    /// <summary>What a note prompt's text does with the note (§21.59): ✏️ Edit note / Write a note REPLACES it, ➕ Add a line APPENDS.</summary>
    enum class NoteWriteMode
    {
        Replace,
        Append,
    };

    /// <summary>
    /// One open NOTE PROMPT (§21.59): which session's note it writes, in which topic, how, and from when until when. At most one per
    /// session (a second tap replaces the first: its prompt message goes). In memory only, in the service (under its notes lock):
    /// a restart forgets every window, which is right - the prompt's five minutes are long over by the time a restart is done.
    /// </summary>
    struct NoteWindow
    {
        std::wstring sessionId; // the session whose note the text writes (the card's owner when the button was tapped)
        int64_t threadId{ 0 }; // the topic the prompt sits in (and the only one whose messages it reads)
        NoteWriteMode mode{ NoteWriteMode::Replace };
        int64_t openedMs{ 0 }; // when the tap opened it (this PC's wall clock, Unix ms)
        int64_t expiresMs{ 0 }; // openedMs + kNoteWindowMs
        // The prompt message: > 0 its id, -1 posted and not answered yet, 0 none (a post Telegram refused). It is deleted when the
        // window ends by any way, so a reply to a stale prompt can never be read later.
        int64_t promptMessageId{ 0 };
        // The service's own count of windows opened (never reused within a run): a prompt's late send answer and a window that was
        // replaced meanwhile are told apart by it, so an answer never lands on a newer window.
        uint64_t seq{ 0 };
    };

    /// <summary>What a message in the topic is to an open note prompt (DecideNoteWindowMessage).</summary>
    enum class NoteMessageVerdict
    {
        NotNote, // the message goes its usual way (a command, a queue gesture, sent outside the window, no text)
        Note, // its text is the note (written, then the window ends)
        Cancel, // a lone ❌ (or ❌ as a reply to the prompt): the window ends with the note unchanged
    };

    /// <summary>What DecideNoteWindowMessage reads of a message and of the window it may answer.</summary>
    struct NoteMessageFacts
    {
        bool held{ false }; // a new thread HELD the message (written before its session existed): never a note
        std::wstring_view text; // the message's text (a voice note's transcript); "" for a sticker or a bare photo
        int64_t replyToMessageId{ 0 }; // reply_to_message.message_id (0 = not a reply)
        int64_t promptMessageId{ 0 }; // the window's prompt (NoteWindow::promptMessageId; &lt;= 0 = none known)
        int64_t messageDateMs{ 0 }; // the message's own `date` × 1000 (0 = unknown: judged as sent now)
        int64_t openedMs{ 0 }; // NoteWindow::openedMs
        int64_t expiresMs{ 0 }; // NoteWindow::expiresMs (&lt;= 0 = no window: always NotNote)
    };

    namespace note_detail
    {
        /// <summary>Whether a character is a blank the note's edges and a command's words are trimmed of (the normalizer's set, plus CR).</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for space, tab, CR, LF, NBSP and the ideographic space.</returns>
        inline bool IsBlank(wchar_t c) noexcept
        {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\x00A0' || c == L'\x3000';
        }

        /// <summary>The text without its leading and trailing blanks (IsBlank).</summary>
        /// <param name="s">The text.</param>
        /// <returns>A view into `s`.</returns>
        inline std::wstring_view TrimBlank(std::wstring_view s) noexcept
        {
            while (!s.empty() && IsBlank(s.front()))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && IsBlank(s.back()))
            {
                s.remove_suffix(1);
            }
            return s;
        }
    }

    /// <summary>
    /// What a message in a session's topic is to that session's open NOTE PROMPT (§21.59) - the rule the service applies FIRST, before
    /// /btw, the queue's gestures and a question card's text, because the user asked to write a note and is now writing it:
    /// - a message a new thread HELD, a message with no text, and every /command keep their meaning (NotNote): "/note &lt;text&gt;"
    ///   writes the note itself, and /status, /steer, /cc … must still work while a prompt waits;
    /// - a REPLY to the prompt is the note (Note), or the prompt's end when it is a lone ❌ (Cancel) - a reply is the clearest
    ///   answer there is, so the window's clock is not asked;
    /// - a ❌ / ➕ reply to ANOTHER message is the queue's gesture (§21.57.7: remove or re-queue a waiting message), never the note;
    /// - any other message counts only when it was SENT inside the window (its own `date`, with kNoteClockSkewMs of slack), so a
    ///   voice note transcribed after the window still counts when it was spoken in time;
    /// - then a lone ❌ is Cancel, and everything else is the note.
    /// </summary>
    /// <remarks>
    /// "btw …" and "by the way …" are notes here, not side questions: the prompt is the user's own request to write, and a note may
    /// well start that way. A typed /btw is a command and keeps its meaning.
    /// </remarks>
    /// <param name="f">The message and the window.</param>
    /// <returns>The verdict.</returns>
    inline NoteMessageVerdict DecideNoteWindowMessage(const NoteMessageFacts& f) noexcept
    {
        if (f.held || f.expiresMs <= 0)
        {
            return NoteMessageVerdict::NotNote;
        }
        const std::wstring_view text = note_detail::TrimBlank(f.text);
        if (text.empty() || text.front() == L'/')
        {
            return NoteMessageVerdict::NotNote;
        }
        const QueueReplyKind gesture = ClassifyQueueReply(text);
        if (f.promptMessageId > 0 && f.replyToMessageId == f.promptMessageId)
        {
            return gesture == QueueReplyKind::Remove ? NoteMessageVerdict::Cancel : NoteMessageVerdict::Note;
        }
        if (f.replyToMessageId > 0 && gesture != QueueReplyKind::None)
        {
            return NoteMessageVerdict::NotNote;
        }
        if (f.messageDateMs > 0 && (f.messageDateMs > f.expiresMs + kNoteClockSkewMs || f.messageDateMs + kNoteClockSkewMs < f.openedMs))
        {
            return NoteMessageVerdict::NotNote;
        }
        return gesture == QueueReplyKind::Remove ? NoteMessageVerdict::Cancel : NoteMessageVerdict::Note;
    }

    /// <summary>
    /// Whether a message sent at `dateMs` falls inside a window (§21.59), with kNoteClockSkewMs of slack at both ends - what the
    /// expiry asks of a voice note still in the transcription lane (NoteWindowTick).
    /// </summary>
    /// <param name="w">The window.</param>
    /// <param name="dateMs">The message's `date` × 1000.</param>
    /// <returns>True when it was sent while the window was open.</returns>
    inline bool NoteSentInWindow(const NoteWindow& w, int64_t dateMs) noexcept
    {
        return dateMs > 0 && dateMs + kNoteClockSkewMs >= w.openedMs && dateMs <= w.expiresMs + kNoteClockSkewMs;
    }

    /// <summary>What the service's tick does with an open window (DecideNoteWindowTick).</summary>
    enum class NoteWindowTick
    {
        Open, // inside its five minutes: nothing to do
        WaitForVoice, // past them, but a voice note sent inside them is still being transcribed: wait (kNoteWindowVoiceGraceMs at most)
        Expire, // the prompt ends: its message goes, and the expiration message says nothing was written
    };

    /// <summary>The tick's verdict on a window (§21.59): open, waiting for a voice note sent in time, or expired.</summary>
    /// <param name="w">The window.</param>
    /// <param name="nowMs">The wall clock (Unix ms).</param>
    /// <param name="voiceSentInWindowPending">A voice note of the window's topic, sent inside it (NoteSentInWindow), is still in the transcription lane.</param>
    /// <returns>The verdict.</returns>
    inline NoteWindowTick DecideNoteWindowTick(const NoteWindow& w, int64_t nowMs, bool voiceSentInWindowPending) noexcept
    {
        if (nowMs <= w.expiresMs)
        {
            return NoteWindowTick::Open;
        }
        if (voiceSentInWindowPending && nowMs <= w.expiresMs + kNoteWindowVoiceGraceMs)
        {
            return NoteWindowTick::WaitForVoice;
        }
        return NoteWindowTick::Expire;
    }

    /// <summary>
    /// The note a prompt's text makes (§21.59), before the store normalizes it: ✏️ replaces the note with the text; ➕ puts the text on
    /// a line of its own under the note (the text alone when there is no note yet). NormalizeSessionNote then trims and caps it.
    /// </summary>
    /// <param name="mode">Replace or Append.</param>
    /// <param name="current">The note as stored now ("" = none).</param>
    /// <param name="incoming">The message's text.</param>
    /// <returns>The raw new note.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring ComposeWindowNote(NoteWriteMode mode, std::wstring_view current, std::wstring_view incoming)
    {
        if (mode == NoteWriteMode::Replace || note_detail::TrimBlank(current).empty())
        {
            return std::wstring{ incoming };
        }
        std::wstring out{ current };
        out += L'\n';
        out.append(incoming);
        return out;
    }

    // ===== /note ===========================================================================================================

    /// <summary>What a /note command asks (ParseNoteCommandArgs).</summary>
    enum class NoteCommandKind
    {
        Show, // bare /note: the whole note, with when it was written
        Set, // /note <text>: the text replaces the note
        Append, // /note + <text>: the text goes on a line of its own under the note
        Remove, // /note - : the note goes
        Usage, // /note + with nothing after it: the usage line
    };

    /// <summary>A /note command's reading: what it asks and, for Set and Append, the text.</summary>
    struct NoteCommand
    {
        NoteCommandKind kind{ NoteCommandKind::Show };
        std::wstring text; // Set / Append: the words (trimmed of their outer blanks; the store normalizes them)
    };

    /// <summary>
    /// Reads /note's words (§21.59): none = Show; "-" (or the en or em dash a phone's keyboard may turn it into) = Remove; a leading
    /// "+" = Append of the rest ("+" alone = Usage); anything else = Set. A note that must start with "+" is written from the card's
    /// ✏️ Edit note (the prompt takes any text).
    /// </summary>
    /// <param name="args">The command's words (RemoteCommand::args: everything after "/note", line breaks kept).</param>
    /// <returns>The reading.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline NoteCommand ParseNoteCommandArgs(std::wstring_view args)
    {
        NoteCommand c;
        const std::wstring_view a = note_detail::TrimBlank(args);
        if (a.empty())
        {
            c.kind = NoteCommandKind::Show;
            return c;
        }
        if (a == L"-" || a == L"\x2013" || a == L"\x2014")
        {
            c.kind = NoteCommandKind::Remove;
            return c;
        }
        if (a.front() == L'+')
        {
            const std::wstring_view rest = note_detail::TrimBlank(a.substr(1));
            c.kind = rest.empty() ? NoteCommandKind::Usage : NoteCommandKind::Append;
            c.text = std::wstring{ rest };
            return c;
        }
        c.kind = NoteCommandKind::Set;
        c.text = std::wstring{ a };
        return c;
    }

    // ===== the change the topic hears about ================================================================================

    /// <summary>What a session's stored note is next to the one its card shows (ClassifyNoteChange).</summary>
    enum class NoteChange
    {
        None, // the same text: nothing to do
        FirstSight, // the service never showed a note for this session (a new topic, a session a /clear moved in, a start): show it, say nothing
        Changed, // another text: the card shows it, and the topic gets the PC-change line
        Removed, // the note is gone: the card drops it, and the topic gets the PC-change line
    };

    /// <summary>
    /// The verdict of the service's epoch pass for one topic (§21.59, the user's extra "Line when PC changes it"): the service records
    /// the note it SHOWS before it writes one itself, so a stored note that differs from the shown one was written elsewhere - the
    /// PC's editor in any window (the only other writer; a /clear's carry gives a NEW session, which is FirstSight).
    /// </summary>
    /// <param name="shown">The note the service shows for the session; null = it never showed one (FirstSight).</param>
    /// <param name="stored">The note the store holds now ("" = none).</param>
    /// <returns>The verdict.</returns>
    inline NoteChange ClassifyNoteChange(const std::wstring* shown, std::wstring_view stored) noexcept
    {
        if (!shown)
        {
            return NoteChange::FirstSight;
        }
        if (*shown == stored)
        {
            return NoteChange::None;
        }
        return stored.empty() ? NoteChange::Removed : NoteChange::Changed;
    }

    // ===== the texts =======================================================================================================

    /// <summary>
    /// The NOTE PROMPT (§21.59): a header line of the bot's own, the session's title row on line 2 (kTgTitleRowMarker: the send
    /// layer puts the row there, §21.54), how to answer and until when, and - for ✏️ on a session with a note - the note as it is,
    /// in a code block a phone copies with one tap, to paste and change. Posted with Telegram's ForceReply, so the phone opens the
    /// reply box under it by itself.
    /// </summary>
    /// <param name="mode">Replace (✏️ Edit note / Write a note) or Append (➕ Add a line).</param>
    /// <param name="currentNote">The note as stored ("" = none).</param>
    /// <param name="expiresMs">When the window ends (Unix ms).</param>
    /// <param name="nowMs">The clock TgClockText's "same day" test uses.</param>
    /// <returns>The Telegram HTML (escaped; with the row marker).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NotePromptHtml(NoteWriteMode mode, std::wstring_view currentNote, int64_t expiresMs, int64_t nowMs)
    {
        const std::wstring until = TgClockText(expiresMs, nowMs);
        std::wstring out;
        if (mode == NoteWriteMode::Append)
        {
            out = L"\x2795 <b>Add a line to the note</b>\n";
            out += kTgTitleRowMarker;
            out += L"\nSend the line now \x2014 your next message here, or a reply to this one, until " + until + L". \x274C cancels.";
            return out;
        }
        out = currentNote.empty() ? L"\U0001F5D2\xFE0F <b>Write a note</b>\n" : L"\U0001F5D2\xFE0F <b>New note</b>\n";
        out += kTgTitleRowMarker;
        out += L"\nSend it now \x2014 your next message here, or a reply to this one, until " + until + L". \x274C cancels.";
        if (!currentNote.empty())
        {
            out += L"\nThe note now, to copy and change:\n<pre>" + TgEscapeHtml(currentNote) + L"</pre>";
        }
        return out;
    }

    /// <summary>The reply box's placeholder under the prompt (ForceReply's input_field_placeholder, at most 64 characters).</summary>
    /// <param name="mode">Replace or Append.</param>
    /// <returns>"The new note" or "The line to add".</returns>
    inline std::wstring_view NotePromptPlaceholder(NoteWriteMode mode) noexcept
    {
        return mode == NoteWriteMode::Append ? std::wstring_view{ L"The line to add" } : std::wstring_view{ L"The new note" };
    }

    /// <summary>
    /// The EXPIRATION MESSAGE (§21.59, the user's "with expiration msg"): a prompt nothing answered in kNoteWindowMs ends, its own
    /// message goes, and this one says so - nothing was written, and the topic's messages are prompts for Claude again. Posted as a
    /// new message (an edit would not be noticed), silent.
    /// </summary>
    /// <param name="expiresMs">When the window ended (Unix ms).</param>
    /// <param name="nowMs">The clock TgClockText's "same day" test uses.</param>
    /// <returns>The Telegram HTML (with the row marker).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NoteExpiredHtml(int64_t expiresMs, int64_t nowMs)
    {
        std::wstring out = L"\x231B <b>Note prompt expired</b> \x00B7 " + TgClockText(expiresMs, nowMs) + L"\n";
        out += kTgTitleRowMarker;
        out += L"\nNo note came in time: the note is unchanged, and your messages go to Claude again. "
               L"\x2699\xFE0F Session \x2192 \U0001F5D2\xFE0F Note \x25B8 asks again.";
        return out;
    }

    /// <summary>
    /// /note's answer (§21.59): the whole note (escaped, line breaks kept) under a header that says when it was written, or - with no
    /// note - how to write one.
    /// </summary>
    /// <param name="note">The note as stored ("" = none).</param>
    /// <param name="atMs">noteAtUnixMs (0 = unknown: no time in the header).</param>
    /// <param name="nowMs">The clock TgClockText's "same day" test uses.</param>
    /// <returns>The Telegram HTML (with the row marker).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NoteShowHtml(std::wstring_view note, int64_t atMs, int64_t nowMs)
    {
        std::wstring out;
        if (note.empty())
        {
            out = L"\U0001F5D2\xFE0F <b>No note</b>\n";
            out += kTgTitleRowMarker;
            out += L"\n/note &lt;text&gt; writes one \x2014 or \x2699\xFE0F Session \x2192 \U0001F5D2\xFE0F Note \x25B8.";
            return out;
        }
        out = L"\U0001F5D2\xFE0F <b>Note</b>";
        if (atMs > 0)
        {
            out += L" \x00B7 edited " + TgClockText(atMs, nowMs);
        }
        out += L"\n";
        out += kTgTitleRowMarker;
        out += L"\n" + TgEscapeHtml(note);
        return out;
    }

    /// <summary>
    /// The line a topic gets when its session's note is changed or removed AT THE PC (§21.59, the user's extra "Line when PC changes
    /// it"): the header says which, and a change quotes the note's first line (kNoteChangeLineMaxChars); the card shows the rest.
    /// Silent, like the bot's other lines.
    /// </summary>
    /// <param name="newNote">The note as stored now ("" = removed).</param>
    /// <returns>The Telegram HTML (with the row marker).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NotePcChangeHtml(std::wstring_view newNote)
    {
        std::wstring out = newNote.empty() ? L"\U0001F5D2\xFE0F <b>Note removed at the PC</b>\n" : L"\U0001F5D2\xFE0F <b>Note changed at the PC</b>\n";
        out += kTgTitleRowMarker;
        if (!newNote.empty())
        {
            out += L"\n<i>" + TgEscapeHtml(TgFirstLine(newNote, kNoteChangeLineMaxChars)) + L"</i>";
        }
        return out;
    }

    /// <summary>The reply under a note that lost its tail to kMaxSessionNoteLength (§21.59): it was written, cut.</summary>
    /// <returns>The Telegram HTML (with the row marker).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NoteCutHtml()
    {
        std::wstring out = L"\x2702\xFE0F <b>Note cut</b>\n";
        out += kTgTitleRowMarker;
        out += L"\nA note keeps its first " + std::to_wstring(kMaxSessionNoteLength) + L" characters; the rest did not fit.";
        return out;
    }

    /// <summary>/note + with nothing after it (§21.59): the command's usage.</summary>
    /// <returns>The Telegram HTML (escaped).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NoteUsageHtml()
    {
        return L"Usage: /note shows the note \x00B7 /note &lt;text&gt; writes it \x00B7 /note + &lt;text&gt; adds a line \x00B7 /note - removes it.";
    }

    /// <summary>The toast of ✏️ / ➕ (§21.59): what to do now, and until when.</summary>
    /// <param name="mode">Replace or Append.</param>
    /// <param name="expiresMs">When the window ends (Unix ms).</param>
    /// <param name="nowMs">The clock TgClockText's "same day" test uses.</param>
    /// <returns>The toast (plain text; Telegram shows at most 200 characters).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NoteOpenToast(NoteWriteMode mode, int64_t expiresMs, int64_t nowMs)
    {
        const std::wstring until = TgClockText(expiresMs, nowMs);
        return mode == NoteWriteMode::Append ? L"\x2795 Send the line as your next message here (or a reply to the prompt), until " + until + L"." :
                                               L"\U0001F5D2\xFE0F Send the note as your next message here (or a reply to the prompt), until " + until + L".";
    }

    /// <summary>✖ Cancel the prompt's toast when a prompt was open: it closed, nothing written.</summary>
    inline constexpr std::wstring_view kNoteCancelledToast = L"\x2716\xFE0F Prompt closed \x2014 the note is unchanged.";
    /// <summary>✖ Cancel the prompt's toast when none was open any more (it expired or was answered meanwhile).</summary>
    inline constexpr std::wstring_view kNoteNoPromptToast = L"No note prompt is open.";
    /// <summary>🗑 Yes, remove the note's toast.</summary>
    inline constexpr std::wstring_view kNoteRemovedToast = L"\U0001F5D1\xFE0F Note removed.";
    /// <summary>🗑 Yes, remove the note's toast for a session whose note was gone already (removed at the PC meanwhile).</summary>
    inline constexpr std::wstring_view kNoteNothingToRemoveToast = L"This session has no note.";
    /// <summary>The answer when the session store refused the write (an unreadable record: Rule #16 keeps it, so nothing was written).</summary>
    inline constexpr std::wstring_view kNoteWriteFailedText = L"The note could not be saved: the session's record could not be written.";
}
