# 10 · Telegram: Claude's questions and dialogs (TGQ)

> Conventions and fixtures: [README.md](README.md). Spec: MESSAGING.md §21.30 (the question card, with its own
> "Check after the build" list), §21.34 (the /add-dir menu card); INTERACTIVE_TOOLS.md §10. Needs **F-TG** and the
> mock's `tool AskUserQuestion {…}` (single, several questions, `multiSelect`, `preview` per option).

**What shipped (0.8.0).** A Claude Code `AskUserQuestion` dialog becomes ONE managed message in the session's
topic — the question card — edited in place: options as buttons, the user's own words and notes through a
ForceReply prompt, previews inline or as one picture, several questions as TABS with the PC's review and Submit,
multi-select in TICK order, 💬 Chat about this, ✖️ Cancel (asked first; the PC's Esc), a plain message held and
asked about, Claude Code's idle timeout shown as such. The answer is composed byte for byte as the PC composes
it, through a `PermissionRequest` hook Agentmaster adds while the bot runs (Claude Code ≥ 2.1.139); the PC's own
dialog stays usable and the first answer wins. `/add-dir` sent from a topic turns its dialog into a MENU CARD that
picks "Yes, for this session" by itself after 60 s.

## Lifecycle model

```text
 claude opens AskUserQuestion ─► the hook asks Agentmaster (ask pipe) ─► the CARD (rings once)
    │                                                         │ tap · own words · note · tabs · review · Submit
    │                                                         ▼
    │                                ⏳ Sending your answer… ─► ✅ Answered from Telegram · 14:02 (confirmed by the transcript ≤ 60 s)
    ├─ the PC answers first ─► ✅ Answered at the PC (the hook RELEASED)        ├─ 💬 Chat about this ─► the turn goes on
    ├─ ✖️ Cancel (confirmed) ─► the STOP rejection + interrupt (the turn ends; the interrupt hold parks the runner)
    ├─ 💬 a reply instead (Claude Code's `response`) ─► "The user responded: …"
    ├─ Claude Code's AFK timer ─► ⏱ No answer in time — went on by itself after 1 min
    └─ the session ends / the topic goes / Esc at the PC ─► ⏹ No longer open here — <why>
```

---

### TGQ-01 · Answer a one-question card with a tap

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.30

**Phases**
1. **Ask.** *Do:* `[[mock ask "Which footer layout?" Left,Center,Right; expect "Center"; reply "ok"]]` (wait at the
   PC for nothing). *Expect:* ONE card in the topic, with sound once (not the sticky card too): `❓ Claude is asking ·
   <header>`, the question, the numbered options with their descriptions, the buttons `[ 1 · Left ] [ 2 · Center ]`
   …, `[✍️ Write my own answer]`, `[💬 Chat about this]`, `[✖️ Cancel]`. *Check:* [LOG] `[ask] <sid8> question open
   (request N); answerable from Telegram`, `… question card posted …`.
2. **Tap.** *Do:* tap `2 · Center`. *Expect:* ⏳ Sending… → ✅ Answered from Telegram · <time> (the question and the
   answer); the buttons go; the PC's dialog closes ("Allowed by PermissionRequest hook"). *Check:* [MOCK] the next
   request's tool_result equals the PC's own answer form; the mock's `expect` passes; [OS] no `agentmaster-ask.ps1`
   PowerShell left.

**Edge cases**
- **E1 The PC answers first** → the card reads ✅ Answered at the PC · <time> with the answer; the waiting hook is
  released within a tick.
- **E2 Esc at the PC** → ⏹ closed at the PC.
- **E3 The bot off** → no helper waits; the dialog is only at the PC.
- **E4 A claude started before the bot was on / older than 2.1.139** → posted show-only after 8 s with the reason and
  "🔄 Restart Session gives it the question hook".
- **E5 A subagent's question** → shown at the PC at once (not offered to the card).
- **E6 A button of a previous run** → "This question is no longer open here." and its keyboard goes.

---

### TGQ-02 · The user's own words and notes

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock (a preview question) · **Spec** MESSAGING.md §21.30

**Phases**
1. **Own answer (plain question).** *Do:* tap ✍️ Write my own answer → a ForceReply prompt "✍️ Your answer to: …"
   (placeholder "Your answer") → reply `something else`. *Expect:* 👌 on the reply, the prompt deleted, the answer
   sent (one-question card); [MOCK] the PC's own-words form.
2. **A note on a preview question.** *Do:* tap 📝 Add a note → reply `use rounded corners` → tap an option.
   *Expect:* the note goes with the option (annotations as the PC sends them).
3. **The note alone.** *Do:* add a note, tap 📨 Send the note alone. *Expect:* the answer is "(notes only)" with the
   note — exactly the PC's notes-only form.
4. **Change / remove the note** → ✏️ Change the note, ✖ Remove the note.
5. **A reply to the card itself** (not the prompt) → taken as the text the card waits for.

**Edge cases**
- **E1 600-character own answer + note** → still one message (shortened, never dropped).
- **E2 Hebrew / emoji in the own answer** → composed intact.

---

### TGQ-03 · Several questions: tabs, the review, Submit

> **Since** 0.8.0 · **P0** · **Needs** F-TG, mock (4 questions, one multi-select) · **Spec** MESSAGING.md §21.30 (evening)

**Phases**
1. **Card.** *Expect:* `❓ Claude is asking · 4 questions`; the list of questions (▶ the shown one); "Question 1 of
   4"; a TAB ROW (headers cut to 16 characters, else `Q<n>`, three to a row) + [📨 Submit]; the options of the shown
   question.
2. **A pick moves on** to the next unanswered question (wrapping); ✓ on answered tabs.
3. **Multi-select tab.** *Do:* tick Olives, then Cheese, add own words `extra basil, please`, ✅ Done ▶. *Expect:* ☐ / ☑
   toggles; the composed answer keeps the TICK order (`"Olives, Cheese, "extra basil, please""`).
4. **Review.** *Expect (after the last):* "Review your answers", "You have not answered all questions" while one is
   open, every question with → answer / → not answered and its note, "Ready to submit your answers?",
   [📨 Submit answers (a of n)], [💬 Chat about this] [✖️ Cancel].
5. **Submit.** *Expect:* the PC's form byte for byte (unanswered questions may be submitted).

**Edge cases**
- **E1 A tab tap** shows that question (and drops a pending typed-text wait / cancel confirm).
- **E2 A message typed on the review** → goes to the first unanswered question.
- **E3 A lone multi-select question** → has a review too (as at the PC).
- **E4 Previews too long for the card** → moved to 🖼; then the other questions' list goes; then descriptions are
  shortened; "(Shortened to fit one message — the PC shows it in full.)".

---

### TGQ-04 · A plain message while the card is open is asked about

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.30

**Phases**
1. **Held.** *Do:* with a card open, write `blue please` in the topic. *Expect:* HELD, and asked as a reply: "❓
   Claude's question is still open: … What is this message?" — [✍️ My answer] / [📝 My note] · [💬 A reply] · [📨 The
   next prompt].
2. **My answer** → used as the answer to that question.
3. **A reply** → sent INSTEAD of answering (Claude Code's `response` field): the model reads "The user responded:
   blue please"; the card reads 💬 Replied from Telegram — instead of answering; answers made so far are not sent.
4. **The next prompt** → queued as the session's next prompt (sent after the question is answered).

**Edge cases**
- **E1 Editing the held message** → changes what it holds.
- **E2 A `/command`** while the card is open → stays a command.

---

### TGQ-05 · Chat about this, Cancel, the picture, the idle timeout

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.30

**Phases**
1. **💬 Chat about this.** *Expect:* Claude Code's own rejection with its clarify request (each answer so far and its
   note) is sent; the turn goes on; Claude's next message comes like any reply; the card reads 💬 You asked Claude to
   clarify.
2. **✖️ Cancel.** *Expect:* a confirmation "Cancel the question? Claude stops this turn and waits for you — as Esc does
   at the PC" [✖️ Yes, cancel] [◀ No, go back]; Yes → the STOP rejection with `interrupt`: the transcript holds what an
   Esc leaves (`User rejected tool use`, `[Request interrupted by user for tool use]`), the turn ends, the interrupt
   hold parks the runner (PQR-07); the card reads ✖️ Cancelled from Telegram.
3. **🖼 Previews as a picture.** *Expect:* ONE photo of the current question's previews (box drawing intact); the
   button busy until the photo arrives.
4. **Idle timeout.** *Do:* configure Claude Code's AFK timeout short (test env), leave the card. *Expect:* ⏱ No answer in
   time — Claude Code went on by itself after <n>, with what was selected before it; the hook released.

---

### TGQ-06 · /add-dir from a topic: the menu card

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock, a folder outside the session's cwd · **Spec** MESSAGING.md §21.34; DELIVERY.md §18

**Promise.** `/add-dir <path>` sent from a topic is queued and watched; its "Add directory to workspace" dialog
becomes ONE ringing card whose buttons answer it, and the default ("Yes, for this session") is picked by itself
after 60 s.

**Phases**
1. **Send.** *Do:* `/add-dir C:\e2e\w\other` in the topic. *Expect:* queued (✍), delivered; the PC shows the dialog;
   the topic gets the menu card as a reply to the message, with sound: `📁 Claude Code asks · Add directory to
   workspace`, the RESOLVED path, the hint, `⏱ Picks Yes, for this session by itself in 1 minute — or tap your
   answer.`, [⏱ 1 · Yes, for this session] [2 · Yes, and remember this directory] [3 · No].
2. **Tap 2.** *Expect:* "⏳ Answering · …" → the window presses `2` ONLY into that very dialog (re-read at the press) →
   "✅ Added and remembered · answered from Telegram · <time>" + Claude Code's message; [OS] the folder in
   `<cwd>\.claude\settings.local.json` `permissions.additionalDirectories`.
3. **Nobody answers** (repeat with another folder) → after 60 s option 1 is pressed: "✅ Added for this session · ⏱
   picked by itself after 60 s".
4. **The PC answers first** → "answered at the PC" (the first answer wins).

**Edge cases**
- **E1 A path already in the workspace / missing / not a directory** → no dialog; the result as a reply ("ℹ️ Already in
  the workspace · /add-dir").
- **E2 A bare `/add-dir`** → never sent: the usage line.
- **E3 A delivery holds the box** when the press is due (a draft swap) → asked again every 2 s up to 75 s.
- **E4 A dialog for ANOTHER path** on screen → no press (`other`).
- **E5 The session parked / closed meanwhile** → "⏹ No longer open here — <why>".
- **E6 A dialog this build cannot read** (changed by a Claude Code update) → after 15 reads: "⚠️ /add-dir opened a
  dialog this Agentmaster cannot read — answer it at the PC".
- **E7 No lost-send verdict while the dialog is open** (DELIVERY.md §18): the runner is not paused, no 👎.
