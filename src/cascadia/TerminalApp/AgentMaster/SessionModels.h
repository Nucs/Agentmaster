// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — core data model for the Manager tab (Design A / C1 / Auto Testing).
// Plain C++ (no WinRT projection) so it compiles standalone; the XAML layer (M6) wraps
// these in observable view-models. See doc/agentmaster/IMPLEMENTATION.md.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Activity.h" // RunningApp (Fleet Observer live-enrichment field on SessionInfo)

namespace Agentmaster
{
    // Hook-driven lifecycle of a Claude Code session. Authoritative state comes from
    // Claude Code hooks (see doc/agentmaster/HOOKS.md), never from screen-scraping.
    enum class SessionState
    {
        Idle, // launched, no active turn
        Running, // mid-turn (PreToolUse/PostToolUse activity)
        WaitingForInput, // Stop hook fired: turn complete, ready for next user message
        NeedsApproval, // Notification(permission): tool-permission prompt awaiting y/n
        Error, // last turn ended in error
        Done // session ended (SessionEnd)
    };

    // Agentmaster (crash/restore state fidelity — Correctness Rule #16): map a PERSISTED SessionState
    // to the state a freshly-REOPENED session should SEED with, so a CRASH (or a window/session restore)
    // does not silently drop the fleet's "which sessions need me" triage. sessions.json persists the
    // real state, but the reopen path used to force every session to Idle unconditionally — harmless on
    // a clean shutdown (those sessions were deliberately closed) but on a crash it threw away the live
    // Running / WaitingForInput / NeedsApproval the user wanted back (every card landed in "Idle/Done").
    //
    // The mapping keeps ONLY the "AT REST, needs you" states — WaitingForInput and NeedsApproval — which
    // genuinely SURVIVE a `claude --resume`: the conversation is parked at a completed turn / an
    // unanswered question, and the transcript tail still says so. Everything else normalizes to Idle:
    //   * Running -> Idle: a resumed claude does NOT continue the interrupted turn (it waits for you),
    //       and the SessionScanner's primed-cursor gate (ShouldSynthesizeRunning) deliberately refuses to
    //       re-light Running off the initial history replay — so seeding Running would just STICK. Idle is
    //       the true post-resume state.
    //   * Error -> Idle: transient. The scanner re-derives Error from the tail (ShouldSynthesizeError
    //       fires from any non-Error/Done state) if the API error is still the active leaf.
    //   * Done -> Idle: "ended"; a session being reopened is alive again, not done.
    //   * Idle -> Idle: unchanged.
    // The SessionScanner then REFINES the seed on its first reconcile pass (recon-stop corrects a
    // NeedsApproval whose turn actually ended -> WaitingForInput; recon-run/recon-resume promote on fresh
    // work), the Waiting-for-you decay treats a reopened session as UNREAD (readUnixMs resets to 0) so a
    // restored WaitingForInput card keeps waiting until the user actually reads it (never instant-decays
    // off an ancient lastActivity), and hooks own it live: the resume's SessionStart PRESERVES these two
    // at-rest states (NextSessionState mirrors this exact set — a lazy-start SessionStart IS a resume, so
    // it reloads without continuing the turn), so the preserved seed SURVIVES activating the tab; only a
    // genuine new turn (UserPromptSubmit -> Running) changes it. (It used to reset to Idle on the
    // first visit — the lazy-start SessionStart clobber that also lost a manual "Move to Waiting-for-you".)
    // PURE + total.
    inline SessionState RestoredSessionState(SessionState persisted) noexcept
    {
        switch (persisted)
        {
        case SessionState::WaitingForInput:
        case SessionState::NeedsApproval:
            return persisted; // an at-rest "needs you" state survives a resume — preserve the triage cue
        case SessionState::Idle:
        case SessionState::Running:
        case SessionState::Error:
        case SessionState::Done:
        default:
            return SessionState::Idle; // in-flight / transient / ended -> a reopened session starts Idle
        }
    }

    // Agentmaster (crash/restore state fidelity — Rule #16, companion to RestoredSessionState): the
    // question-guard flag (SessionInfo::lastMessageWasQuestion — "the last turn ended on a clarifying
    // question", which HOLDS the Autorunner queue so a queued prompt never auto-ANSWERS the question) is
    // now PERSISTED so it survives a crash — but it is only meaningful at a PRESERVED needs-you
    // turn-boundary. DROP it whenever the restored state normalized to Idle: a session that was Running
    // at the crash had ALREADY answered any prior question (that is what STARTED the interrupted turn),
    // so its now-stale flag=true must not falsely hold the queue on reopen. Kept for WaitingForInput
    // (the fix — the queue stays held so a crash can't auto-answer a pending question) and harmlessly for
    // NeedsApproval (never Autorunner-ready anyway). PURE + total.
    inline bool RestoredQuestionFlag(SessionState restoredState, bool persistedFlag) noexcept
    {
        return restoredState == SessionState::Idle ? false : persistedFlag;
    }

    // Agentmaster: case-insensitive equality for a WT_SESSION / tabToken GUID string. The hook wire
    // carries the token as-is from the WT_SESSION env, while the Fleet Observer reads the same value
    // out of the PEB and the app formats it from a connection's SessionId() — the producers can differ
    // in case, so a tab can never be matched by ordinal compare. Shared here (was SessionRegistry.cpp-
    // local) because both the registry and the restart seam's tabToken fallback resolve by it. PURE.
    inline bool TabTokenEq(const std::wstring& a, const std::wstring& b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            wchar_t ca = a[i];
            wchar_t cb = b[i];
            if (ca >= L'A' && ca <= L'Z')
            {
                ca = static_cast<wchar_t>(ca - L'A' + L'a');
            }
            if (cb >= L'A' && cb <= L'Z')
            {
                cb = static_cast<wchar_t>(cb - L'A' + L'a');
            }
            if (ca != cb)
            {
                return false;
            }
        }
        return true;
    }

    enum class AutorunnerMode
    {
        Off,
        SemiAuto, // confirm each auto-send (recommended for the first N sends)
        Full // auto-send with no confirmation
    };

    // Agentmaster: session ordering — the basis for BOTH sort toggles. (1) The Explorer Tree's sort
    // (AppSettings::treeSort) after its LOCAL/GLOBAL/EXTERNAL scope toggle, ordering the directory
    // groups AND the rows within each (and the EXTERNAL census); it cycles Newest -> Oldest ->
    // MostActive -> Alpha -> ByPid. (2) The Triage Board's sort (AppSettings::boardSort) after its
    // LOCAL/GLOBAL toggle, ordering the cards within each state column; it cycles MostActive (its
    // DEFAULT) -> Newest -> Oldest -> Alpha — the tree's set minus ByPid (host/shell grouping is
    // meaningless once cards are split across state columns). Both are GLOBAL app settings, persisted
    // to settings.json so the choice is shared by every window and survives restart. One ExplorerSort
    // comparator (SortKeyLess) serves both consumers.
    enum class ExplorerSort
    {
        Newest, // most recently created first (conversation ctime, desc); a fresh/never-prompted session floats up
        Oldest, // oldest created first (ctime, asc)
        MostActive, // most recent activity first; a currently-running session ranks at the very top
        Alpha, // A->Z by title (case-insensitive)
        ByPid // group by host window/shell pid (externals: ExternalClaudeRow::hostPid; managed: the claude pid), then by most active within each group
    };

    // Agentmaster: how the tab/session RENAME box treats Enter / Shift+Enter as a COMMIT (accept +
    // save, exactly like clicking away). The rename box is multi-line (AcceptsReturn), so the key
    // that ISN'T the commit key inserts a newline into the title instead. Clicking away (focus loss)
    // ALWAYS commits in every mode — this only governs the keyboard shortcut. GLOBAL app setting
    // (AppSettings::tabRenameCommitMode), persisted to settings.json so the choice is shared by every
    // window and survives restart. The integer order is wire-stable AND mirrored by the raw-int
    // constants in TabHeaderControl.cpp (a static_assert in TerminalPage.AgentEngine.cpp locks it).
    enum class TabRenameCommitMode
    {
        ClickAwayOnly = 0, // None: only focus-loss commits; both Enter and Shift+Enter insert a newline
        ClickAwayOrShiftEnter = 1, // default: Shift+Enter commits; a plain Enter inserts a newline
        ClickAwayOrEnter = 2 // Enter commits; Shift+Enter inserts a newline
    };

    // Agentmaster (FAVORITES.md §5a): which glyph marks a FAVORITE (starred) session on its LIVE
    // tab strip, drawn over/around the state-colored status dot. Crown = the original — a small gold
    // crown perched at the dot's north-west. Star = the status dot becomes the FOREGROUND of a white,
    // golden-tipped star (the star is drawn BEHIND the dot, so its points radiate around the dot).
    // GLOBAL app setting (AppSettings::favoriteIcon), persisted to settings.json so the choice is
    // shared by every window and survives restart; applied live (cog Save + cross-window broadcast
    // re-assert every hosted favorited tab). Serialized as a string token (Persistence ToString /
    // FavoriteIconFromString); a missing key => Crown (the prior behavior). Tab-strip ONLY — the
    // board card / tree row / overlay HUD carry no favorite glyph.
    enum class FavoriteIcon
    {
        Crown = 0, // default: a gold crown at the status dot's north-west
        Star = 1 // the status dot as the foreground of a white, golden-tipped star (drawn behind the dot)
    };

    // Agentmaster (tab color modes): HOW a managed session's tab gets its color — the cog's TABS
    // "Tab coloring" dropdown. Rule #12's "ONE color per working directory" becomes the DEFAULT of
    // three modes rather than the only behavior:
    //   * WorkingDirectory — the original: every tab in a working dir shares that dir's PERMANENT
    //     color (dir-colors.json; user picks fan out to the whole dir).
    //   * Individual — every managed session gets its OWN color, dealt collision-free against the
    //     other OPEN sessions' colors and PERSISTED on the session record (SessionInfo::tabColorHex,
    //     sessions.json) so it survives close/restore; a user pick recolors ONLY that session.
    //   * InferredWorkingDirectory — WorkingDirectory semantics, but keyed by the directory the
    //     session ACTUALLY works in, INFERRED from the files its tool calls read/edit/create plus
    //     the absolute paths its shell commands name (InferWorkingDirectory over
    //     TranscriptStats::pathsAccessed — canonical tool path fields + ExtractPathsFromText's
    //     command mining — the deepest directory a strict majority of the touched paths share,
    //     re-detected as the transcript grows). A FORK inherits its source's inference at launch
    //     and, while it has no transcript of its own yet, is inferred from the source's (its
    //     content-to-be is a verbatim copy), so it wears the parent conversation's color from the
    //     first frame. Until an inference exists (no file ops yet; or a Codex session — its
    //     rollout isn't path-parsed) the launch cwd keys the color, exactly like WorkingDirectory.
    //   * NoColor — the cog's "Remove colors": NO tab wears a color, STRIP-WIDE — managed tabs are
    //     reset (their color re-derives from the maps on exit), while every OTHER tab's runtime
    //     color (an ex-claude shell tab still wearing its exited session's dir paint, a
    //     user-colored or restored pwsh tab, the Manager tab's per-window tint) is SUSPENDED:
    //     visual shed, value PARKED on the tab (Tab::SetTabColorSuspended) and still recorded by
    //     persistence (BuildStartupActions' setColor fold / GetPersistableTabColor), so leaving
    //     the mode restores it. "Change tab color" + the openTabColorPicker action are disabled on
    //     EVERY tab; a color that lands anyway (a restored tab's replayed setColor, a keybinding)
    //     is immediately parked by the _OnClaudeTabColorChanged chokepoint. The persisted colors
    //     (dir-colors.json + SessionInfo::tabColorHex + the record's managerTabColor + a shell
    //     tab's actionsJson) are deliberately KEPT — never read for painting, never dropped — so
    //     switching back to any other mode restores exactly the colors the fleet had
    //     (ResolveSessionColorHex answers empty). Grouping/dir semantics are the classic
    //     WorkingDirectory ones (EffectiveWorkingDir ignores a dormant inference here, like
    //     Individual).
    // GLOBAL app setting (AppSettings::tabColorMode), persisted to settings.json, applied live on
    // cog Save + the cross-window broadcast (every window repaints its hosted managed tabs).
    // Serialized as a string token (Persistence ToString / TabColorModeFromString); a missing key
    // => WorkingDirectory (the prior behavior).
    enum class TabColorMode
    {
        WorkingDirectory = 0, // default: one shared color per working directory (Rule #12 classic)
        Individual = 1, // every managed tab/session wears its own color
        InferredWorkingDirectory = 2, // shared color, keyed by the dir inferred from the files the session touches
        NoColor = 3 // "Remove colors": no tab is colored; persisted colors kept but not loaded
    };

    // Agentmaster (tab title naming): HOW an untitled session's default tab title is derived from
    // its working directory — the cog's TABS "Tab title naming" dropdown. Every technique starts
    // from the MEANINGFUL folder (the walk past generic bin/obj/Debug/... segments
    // DeriveSessionTitle always did), then:
    //   * LastWord (default) — the last '.'/' '/'-'/'_'-separated word of the folder name
    //     ("Potato.Tomato.SlangGang" -> "SlangGang"); a name with no separators stays whole
    //     ("PotatoTomato").
    //   * FolderName — the folder name as-is.
    //   * TwoFolders — "<parent>/<folder>" ("C:\repos\Potato.Tomato.SlangGang" ->
    //     "repos/Potato.Tomato.SlangGang"); a folder directly under a drive/share root (no parent
    //     folder) is just the folder name.
    //   * Capitals — the capital letters only ("PotaTo.Tomato.Slang" -> "PTTS"); a name with NO
    //     capitals falls back to its word initials uppercased ("potato tomato" -> "PT"), and a
    //     single all-lowercase word stays as-is (a one-letter title helps nobody).
    //   * Branch / BranchFolder / BranchTwoFolders — the working dir's CURRENT git branch
    //     (TitleNamingOptions::branch, an INPUT — the configured 1-arg DeriveSessionTitle resolves
    //     it via ReadGitBranchForDir; detached HEAD reads as the short SHA), alone or prefixed onto
    //     the folder ("feature/ui/Agentmaster") or onto "<parent>/<folder>"
    //     ("feature/ui/source/Agentmaster"). A dir with NO branch (not a git repo) omits the branch
    //     component + its separator — the bare Branch technique then falls back to the folder name
    //     (a title is never empty).
    // Every technique's output also normalizes '\' -> '/' (unconditional, no setting). Applied when
    // an UNTITLED session is launched / adopted / forked-from-disk — an existing or renamed title
    // never re-derives (Rule #11: the title is ONE value). Serialized as a string token
    // (Persistence ToString / TabTitleNamingFromString); a missing key => LastWord.
    enum class TabTitleNaming
    {
        LastWord = 0, // default: the last word of the folder name (whole name when it has no separators)
        FolderName = 1, // the meaningful folder name as-is
        TwoFolders = 2, // "<parent>/<folder>"
        Capitals = 3, // the folder name's capital letters only
        Branch = 4, // the git branch name (falls back to the folder name off-git)
        BranchFolder = 5, // "<branch>/<folder>"
        BranchTwoFolders = 6 // "<branch>/<parent>/<folder>"
    };

    // Agentmaster (tab title naming): the CASE transform applied to the derived title — the cog's
    // "Title case" dropdown beside the technique. Serialized as a string token (Persistence
    // ToString / TabTitleCaseFromString); a missing key => Default (keep the derived casing).
    enum class TabTitleCase
    {
        Default = 0, // as derived
        Lower = 1, // lowercase the whole title
        Upper = 2 // uppercase the whole title
    };

    // Agentmaster (tab title naming): the full recipe DeriveSessionTitle applies — the technique +
    // the output transforms (case; whitespace -> '_'; the unconditional '\' -> '/'). Defaults
    // mirror the AppSettings defaults, so TitleNamingOptions{} == the out-of-the-box naming (tests
    // + the cog preview rely on that).
    struct TitleNamingOptions
    {
        TabTitleNaming naming{ TabTitleNaming::LastWord };
        TabTitleCase caseMode{ TabTitleCase::Default };
        bool spacesToUnderscores{ false }; // convert any whitespace in the title to '_'
        // The working dir's git branch — an INPUT the caller resolves (keeps the 2-arg derive
        // PURE): the configured 1-arg DeriveSessionTitle fills it via ReadGitBranchForDir only for
        // the Branch* techniques; tests + the cog preview pass a made-up one. Empty => the branch
        // component (and its '/') is omitted.
        std::wstring branch{};
    };

    // When a queued prompt is allowed to fire.
    enum class PromptGate
    {
        OnTurnComplete, // default: fire on the Stop hook
        AfterDelay, // fire delayMs after the gate condition is met
        Manual // only via "Send now"
    };

    enum class PromptStatus
    {
        Pending,
        Sent,
        Held, // LEGACY — no longer produced. A pending question / "needs you" state now keeps the
              // prompt Pending (it stays queued and waits, like a Running mid-turn) instead of being
              // parked here. Still parsed (for older sessions.json) + rehabilitated to Pending.
        Skipped,
        Failed
    };

    // How a prompt entered the Auto Testing queue. The queue reflects EVERY message a session
    // received (DESIGN: "all messages user sent, not only via the auto testing"), so a prompt
    // the human typed straight into the ConPTY — captured from the UserPromptSubmit hook — is
    // recorded too, tagged `Typed`, alongside the `Autorun` prompts we queued/injected.
    // (The serialized value is "Autorun"; a pre-rename "Flight" still reads back as Autorun — see
    // PromptOriginFromString.)
    enum class PromptOrigin
    {
        Autorun, // queued and injected through Auto Testing / Tests Autorunner (our send) — was "Flight"
        Typed, // typed directly into the terminal by the human (captured via UserPromptSubmit)
    };

    struct QueuedPrompt
    {
        std::wstring id;
        std::wstring label; // short display name
        std::wstring text; // the prompt body (may be multi-line)
        PromptStatus status{ PromptStatus::Pending };
        PromptGate gate{ PromptGate::OnTurnComplete };
        uint32_t delayMs{ 0 };
        // Optional guard: hold auto-send unless the session output satisfies this.
        // Empty => the scheduler applies the default "not-a-question" guard.
        std::wstring guardPattern;
        std::optional<std::wstring> dependsOn; // prompt id that must be Sent first
        uint32_t attempts{ 0 };
        uint32_t maxAttempts{ 1 };
        int64_t sentAtUnixMs{ 0 };
        PromptOrigin origin{ PromptOrigin::Autorun }; // Autorun (we sent it) vs Typed (human typed it)
        // Transient (NOT persisted): an Autorun prompt we just injected expects ONE
        // UserPromptSubmit echo back; `echoed` marks that echo consumed so the registry does
        // not re-record our own injection as a `Typed` message. Reset to false at each send.
        bool echoed{ false };
        // Transient (NOT persisted): how many EXTRA Enter keystrokes the scheduler has re-pressed
        // for this Sent-but-unacknowledged Flight prompt. The Claude TUI can absorb the original
        // submit Enter as a NEWLINE when the keystroke lands before its input box is ready (the
        // ConPTY delivers text+CR faster than the Ink UI initializes), leaving the prompt typed but
        // never submitted and the turn never starting. The scheduler watches such a prompt and, if
        // the turn has not started within kEnterRetryIntervalMs, re-sends a lone Enter — capped at
        // kEnterRetryMax (Scheduler.h). Reset to 0 at each fresh send.
        uint32_t enterRetries{ 0 };
    };

    // Agentmaster (bounded queue history): the queue records EVERY message a session received
    // (PromptOrigin — Typed captures ride alongside Autorun prompts, by design), so on a long-lived
    // busy session it grew WITHOUT BOUND: each UserPromptSubmit / reconciler back-fill appended a
    // full prompt BODY that then lived — and persisted, and was echo/dedup-scanned — for the record's
    // lifetime. Cap the HISTORY, never the WORK: only completed entries (Sent / Skipped / Failed)
    // are dropped, oldest first; a Pending (or legacy Held) entry is NEVER dropped (Rule #4/#6 —
    // queued work and its statuses must survive), and neither is an entry some remaining entry still
    // dependsOn (the chain must stay resolvable). 200 keeps every consumer honest — prompt-history
    // navigation, the "sent" summary, the echo window's recent tail — while bounding the record; the
    // trim may therefore leave the queue ABOVE the cap when the overflow is all pending work. PURE +
    // total; the registry calls it at each append seam and on Upsert (so an oversized queue persisted
    // by an older build trims on load).
    inline constexpr size_t kMaxQueueHistoryEntries = 200;

    inline void TrimQueueHistory(std::vector<QueuedPrompt>& queue, size_t maxEntries = kMaxQueueHistoryEntries)
    {
        if (queue.size() <= maxEntries)
        {
            return;
        }
        std::unordered_set<std::wstring> dependedOn;
        for (const auto& p : queue)
        {
            if (p.dependsOn && !p.dependsOn->empty())
            {
                dependedOn.insert(*p.dependsOn);
            }
        }
        size_t excess = queue.size() - maxEntries;
        for (auto it = queue.begin(); excess > 0 && it != queue.end();)
        {
            const bool completed = it->status == PromptStatus::Sent || it->status == PromptStatus::Skipped || it->status == PromptStatus::Failed;
            if (completed && dependedOn.find(it->id) == dependedOn.end())
            {
                it = queue.erase(it);
                --excess;
            }
            else
            {
                ++it;
            }
        }
    }

    // Agentmaster: a ONE-LINE, capped preview of a prompt body — its FIRST line, at most `maxChars`
    // characters of text content. A trailing "..." is appended when EITHER the first line surpassed
    // the cap (truncated) OR there is real content after it (further lines), so "..." ALWAYS means
    // "there is more than what's shown". Leading blank lines / whitespace are skipped (a prompt that
    // opens with a newline still previews real text); trailing spaces are trimmed. Returns "" for an
    // all-whitespace (or empty) prompt — every caller reads that as "nothing to show" and omits its
    // row/line rather than rendering an empty quote.
    //
    // THE one preview rule, shared by the two surfaces that show a prompt in a single line: the
    // per-tab overlay's row 3 (the NEXT queued prompt — TAB_OVERLAY.md, whose FirstLinePreview now
    // delegates here) and the completion toast's line 3 (the prompt that JUST FINISHED —
    // NOTIFICATIONS.md §3). They pass different caps because the media differ (a wrapping HUD row vs
    // a toast line the shell hard-ellipsizes), but the truncation SEMANTICS must not drift — the
    // AgentStatusColors.h precedent: the second consumer is the cue to factor it. PURE.
    inline std::wstring PromptPreviewLine(const std::wstring& text, size_t maxChars)
    {
        const size_t start = text.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos)
        {
            return {}; // nothing but whitespace
        }
        const size_t nl = text.find_first_of(L"\r\n", start);
        std::wstring line = (nl == std::wstring::npos) ? text.substr(start) : text.substr(start, nl - start);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t'))
        {
            line.pop_back();
        }
        // Is there real (non-whitespace) content beyond the first line? If so, signal it with "..." too.
        const bool more = (nl != std::wstring::npos) && (text.find_first_not_of(L" \t\r\n", nl) != std::wstring::npos);
        if (line.size() > maxChars)
        {
            line = line.substr(0, maxChars) + L"..."; // surpassed the cap -> truncate + ellipsis
        }
        else if (more)
        {
            line += L"..."; // first line fits, but there's more below it
        }
        return line;
    }

    // Agentmaster (NOTIFICATIONS.md §3 — the completion toast's line 3): the prompt whose turn JUST
    // FINISHED, i.e. the NEWEST message the session actually received. Returns nullptr when the
    // session has no delivered message at all (a never-prompted launch, a managed Codex — which
    // records no prompts — or a session adopted after its last turn), and every caller then simply
    // omits the line rather than inventing one.
    //
    // "Delivered" == status Sent, which is exactly the set the Auto Testing's SENT summary shows:
    // BOTH origins count. An Autorun prompt is no less the user's message than a Typed one — they
    // queued it — and the toast answers "which of my requests just came back", so filtering by
    // origin would blank the line precisely for the autorunner-driven sessions whose completions you
    // are least likely to be watching.
    //
    // Newest = the greatest sentAtUnixMs, ties broken by POSITION (later wins). Position alone would
    // do for a live queue (both record seams — SessionRegistry's UserPromptSubmit capture and the
    // scanner's NoteExternalPrompt back-fill — stamp `now` and push_back, so append order IS
    // chronological), but the stamp is what keeps a queue REORDERED in the Auto Testing, or persisted
    // by an older build, honest; the positional tiebreak then covers legacy entries whose stamp is 0.
    //
    // Safe at toast-fire time, including a HELD toast fired seconds later: a new prompt would have
    // put the session back in Running, and DecideHeldToast DROPS a held toast on exactly that. PURE.
    inline const QueuedPrompt* LastDeliveredPrompt(const std::vector<QueuedPrompt>& queue) noexcept
    {
        const QueuedPrompt* best = nullptr;
        for (const auto& p : queue)
        {
            if (p.status != PromptStatus::Sent)
            {
                continue;
            }
            if (!best || p.sentAtUnixMs >= best->sentAtUnixMs) // >= : equal/zero stamps fall back to position
            {
                best = &p;
            }
        }
        return best;
    }

    // Agentmaster (COMMANDS.md §5b — the /handover-standby FILL): build the ConPTY input that
    // TYPES `text` into Claude's Ink TUI input box WITHOUT submitting it — BuildPromptSubmission's
    // bracketed paste minus the trailing submit CR, so the draft sits in the box exactly one Enter
    // away from sending. The bracketed-paste wrap (ESC[200~ … ESC[201~) is what keeps embedded
    // newlines LITERAL (Ink would otherwise submit on the first line break), so a multi-line
    // briefing lands as one multi-line draft. Embedded CR / CRLF normalize to LF for clean pasted
    // lines. The delimiters also make the fill injection-proof for this channel the same way the
    // submit path is: the text was C0-stripped upstream, so no ESC can terminate the paste early.
    // PURE.
    inline std::wstring BuildPromptFill(std::wstring_view text)
    {
        std::wstring body;
        body.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'\r')
            {
                body.push_back(L'\n');
                if (i + 1 < text.size() && text[i + 1] == L'\n')
                {
                    ++i; // collapse CRLF -> one LF
                }
            }
            else
            {
                body.push_back(text[i]);
            }
        }
        return L"\x1b[200~" + body + L"\x1b[201~";
    }

    // Agentmaster (COMMANDS.md §5b — the standby fill's HANDS-OFF latch): has the user already
    // driven this session? The pump's standby lane must never FILL (or re-fill) a session the
    // user owns — and sampling the STATE alone has a hole: a prompt submitted and COMPLETED
    // between pump ticks lands the state back at rest (Idle/WaitingForInput), where a box-empty
    // read past the verify window would be mistaken for an eaten paste and RE-FILL the
    // already-delivered briefing. So the latch also keys on proof a prompt was EVER submitted,
    // on BOTH channels: turns.lastPromptUnixMs (the push side — set only by a UserPromptSubmit
    // hook, sub-second) and convLastActivityUnixMs (the pull side — line-derived transcript
    // activity; a standby successor is always a FRESH spawn whose transcript does not exist
    // until its first real submit, so both read 0 until the user sends something). PURE.
    inline bool StandbySessionTakenOver(SessionState state, int64_t lastPromptUnixMs, int64_t convLastActivityUnixMs)
    {
        if (state != SessionState::Idle && state != SessionState::WaitingForInput)
        {
            return true; // a turn is in flight right now
        }
        return lastPromptUnixMs != 0 || convLastActivityUnixMs != 0; // one ever ran (completed between ticks)
    }

    // Agentmaster (PENDING_INPUT.md §10 — the restore RE-FILL's HANDS-OFF latch): the RESUME twin of
    // StandbySessionTakenOver above. That latch's "!= 0" test is correct only for a FRESH standby
    // successor (both signals are 0 until the first real submit); a RESUMED session carries HISTORY —
    // turns.lastPromptUnixMs may hold a pre-close value (a same-run close→resume keeps the registry
    // record in memory) and convLastActivityUnixMs is observer-refilled with the conversation's whole
    // past — so "!= 0" would read every restored session as taken over and the re-fill could never
    // run. The baseline is therefore the ARM instant (armedUnixMs, system clock, captured at the
    // resume launch): hands off iff a turn is in flight RIGHT NOW (Running — including the
    // recon-subagent/external-work promotion, where a fill would land mid-work), or a prompt was
    // submitted AT/after arming (the push channel), or the transcript shows real line activity
    // at/after arming (the pull channel — no-hook turns; a bare `--resume`'s untimestamped trailer
    // lines never count as line activity, so a quiet resume can't trip it). Historical values are
    // strictly BEFORE the arm instant, so they never block. A crash-preserved at-rest NeedsApproval
    // seed (RestoredSessionState keeps it) deliberately does NOT block: that is exactly a box worth
    // re-filling ("you were answering this when the app died"), and a GENUINE new blocked turn
    // implies a new prompt, which the timing channels catch. PURE.
    inline bool RestoredDraftSessionTakenOver(SessionState state, int64_t lastPromptUnixMs, int64_t convLastActivityUnixMs, int64_t armedUnixMs)
    {
        if (state == SessionState::Running)
        {
            return true; // a turn is in flight right now
        }
        return (lastPromptUnixMs != 0 && lastPromptUnixMs >= armedUnixMs) ||
               (convLastActivityUnixMs != 0 && convLastActivityUnixMs >= armedUnixMs);
    }

    // Agentmaster (#6 — multi-line submit): build the ConPTY input that types `text` into Claude's
    // Ink TUI and submits it as ONE message. A bare `text + CR` makes Ink submit on the FIRST embedded
    // line break (the WinUI compose TextBox emits CR per line), tearing a multi-line prompt across
    // submits — and the lone-CR Enter-retry can't reassemble it. Wrap the body in a bracketed paste
    // (ESC[200~ … ESC[201~) so Ink treats embedded newlines as literal pasted text, then a single
    // trailing CR (outside the paste) submits the whole block. Embedded CR / CRLF are normalized to LF
    // for clean pasted lines. Assumes Claude's TUI enables bracketed-paste mode (it does — it supports
    // multi-line paste). Also hardens single-line sends against the CR-eaten race: the submit CR now
    // follows a complete, delimited paste instead of riding in raw with the text. PURE; the paste
    // half is BuildPromptFill above (the /handover-standby fill-not-send channel), so the two can
    // never drift — this is exactly that fill plus the ONE submit CR.
    inline std::wstring BuildPromptSubmission(std::wstring_view text)
    {
        return BuildPromptFill(text) + L"\r";
    }

    // Agentmaster (PENDING_INPUT.md §9): ONE prompt submission, as handed to the hosting window's
    // submitter (SessionRegistry::SetPromptSubmitter). Every path that SENDS a queued prompt —
    // the autorunner's auto-send, its SemiAuto confirm, the Manager's Send-now, and the /handover
    // paste pump — goes through that one seam, so the draft swap can never apply to some of them
    // and not others. `promptId` is what an aborted swap rolls back; `refundAutoSend` is set only
    // by the autorunner paths, which incremented autoSendsThisRun before injecting (a Send-now
    // never touches that budget, so refunding it there would silently hand back a send).
    struct PromptSubmission
    {
        std::wstring sessionId;
        std::wstring promptId;
        std::wstring text;
        bool refundAutoSend{ false };
    };

    struct ApprovalPolicy
    {
        bool pauseForHuman{ true }; // default: do not auto-approve tool permissions
        std::vector<std::wstring> autoApproveTools; // allowlist of safe tools
    };

    struct AutorunnerState
    {
        AutorunnerMode mode{ AutorunnerMode::Off };
        uint32_t throttleMs{ 500 }; // min delay between auto-sends
        bool stopOnError{ true }; // pause the plan if a turn ends in error
        bool pauseOnHumanInput{ true }; // suspend while the human is typing
        uint32_t maxAutoSends{ 100 }; // runaway backstop
        uint32_t autoSendsThisRun{ 0 };
        ApprovalPolicy approval{};
    };

    // Agentmaster (event ordering + turn identity): per-session turn accounting for the
    // hook-driven state machine. Each hook is an independent fire-and-forget forwarder process,
    // so events can arrive LATE (the Stop path does transcript work the UserPromptSubmit path
    // doesn't) and prompts can be TYPED AHEAD (Claude Code fires UserPromptSubmit at Enter-time
    // for a prompt queued behind the in-flight turn, then consumes the queued batch as the next
    // turn with NO further UserPromptSubmit). NextSessionStateOrdered (HookEvents.h) reads/writes
    // this so a stale Stop cannot demote a newer turn and a Stop with a queued prompt behind it
    // stays Running. ALL transient (NOT persisted — turn identity is meaningless across restarts).
    struct TurnAccounting
    {
        int64_t lastPromptUnixMs{ 0 }; // hook FIRE time (wire ts) of the newest UserPromptSubmit
        int32_t queuedPrompts{ 0 }; // prompts submitted while a turn was in flight (type-ahead), pending behind it
    };

    // One Claude Code session = one claude.exe on a ConPTY connection, tracked by the
    // SessionRegistry. `id` is also injected into the child as CCMGR_SESSION_ID so hooks
    // can be correlated back to this record (see HOOKS.md).
    struct SessionInfo
    {
        std::wstring id;
        std::wstring title; // task / display name
        std::wstring workingDir; // the "M" axis: which working directory
        std::wstring branch; // git branch / worktree
        // Agentmaster (tab color modes — TabColorMode::Individual): this session's OWN tab color
        // ("#RRGGBB"; empty = none dealt/picked yet). Only meaningful while the GLOBAL
        // AppSettings::tabColorMode is Individual: dealt collision-free against the other OPEN
        // sessions' colors on first paint (_ApplySessionTabColor -> ChooseSessionAutoColor) and
        // set by a user color pick on the tab (no dir fan-out — individual). PERSISTED
        // (sessions.json) so a session keeps ITS color across close/restore — the per-session
        // analog of dir-colors.json's per-folder permanence (Rule #12). Kept (dormant) while the
        // mode is dir-keyed, so switching back to Individual restores the same colors. A tab-color
        // RESET clears it (the next launch deals a fresh one).
        std::wstring tabColorHex;
        // Agentmaster (tab color modes — TabColorMode::InferredWorkingDirectory): the working
        // directory this session was INFERRED to actually work in — the deepest directory a
        // majority of its tool-touched paths (files read/edited/created + searched dirs) share
        // (InferWorkingDirectory over the transcript's pathsAccessed). Re-detected as the
        // transcript grows (TerminalPage::_ScanInferredTabColors, mtime-gated off the scanner
        // tick) and updated through the registry; empty until the session has file ops (the
        // launch cwd then keys the color) and always empty for Codex (its rollout isn't
        // path-parsed). PERSISTED (sessions.json) — a derived CACHE, like the title — so a
        // reopened session wears its inferred color immediately instead of flipping from the cwd
        // color after the first scan. Consulted while the session INFERS (SessionInfersWorkingDir,
        // Persistence.h): tabColorMode == InferredWorkingDirectory — or, in EVERY mode, a session
        // LAUNCHED in the user's home dir (%USERPROFILE%, the default launch dir), whose cwd is
        // meaningless, so the inference is forced on for that tab.
        std::wstring inferredWorkingDir;
        // Agentmaster (Codex managed-session support): which coding agent this session is. Default
        // Claude, so every existing record/path is byte-for-byte unchanged. A Codex session rides the
        // SAME managed path as Claude (immediate card at launch, registry record, archive/restore,
        // per-window persistence); the divergences are confined to `codexSessionId` + the resume
        // commandline (Codex can't pin an id and has no hooks — its state comes from the C2 rollout
        // tail). Persisted.
        AgentKind kind{ AgentKind::Claude };
        // Codex only: the REAL rollout conversation uuid — the `codex resume <uuid>` target and the
        // "does a transcript exist" gate (the Codex analog of how Claude's `id` doubles as its resume
        // id). OUR `id` above is a minted, durable handle (the registry / persistence / tab-map key,
        // exactly Claude's id role); for Codex it is NOT the conversation id (Codex mints that itself,
        // embedded in the date-sharded rollout filename — no --session-id), so the resume target is
        // carried HERE, filled by the Fleet Observer once the rollout resolves. Empty until the first
        // turn writes it (a never-prompted Codex restore-freshes, like Claude); always empty for Claude.
        // Persisted.
        std::wstring codexSessionId;
        SessionState state{ SessionState::Idle };
        // The Waiting-for-you "unread" DECAY ANCHOR (wall-clock ms, PERSISTED). Stamped monotonically
        // by EVERY hook's wire `ts` — SessionStart included — and deliberately RESTARTED by the UI's
        // plain "Move to Waiting-for-you" triage promote (both the tab-menu and board-card surfaces),
        // so it means "when did this card last demand attention", NOT "when did Claude last talk to
        // the API". Display surfaces fall back to it for the "-lastActivityAgo" adornment / MOST
        // ACTIVE sort when the transcript timing is unresolved. It must NEVER feed the ⚡
        // server-cache hint (that is lastTurnUnixMs + convApiActivityUnixMs — ServerCacheStillWarm
        // below): a launch/adopt/resume SessionStart or a triage move stamps this "now" with zero
        // API traffic, which was exactly the reported ⚡ false positive.
        int64_t lastActivityUnixMs{ 0 };
        // Agentmaster (⚡ server-cache hint): the last hook-time EVIDENCE OF A REAL API TURN —
        // stamped monotonically by SessionRegistry::OnHookEvent from the wire `ts`, but ONLY for
        // events that mean THIS conversation actually just made an API request (IsApiTurnEvidence,
        // HookEvents.h: UserPromptSubmit / Pre-/PostToolUse / Notification / a REAL Stop). NEVER
        // stamped by SessionStart (launch / --resume / adopt / /clear send no request), SessionEnd,
        // a synthesized quiescent Stop (reconciliation-timed — incl. the no-API double-ESC
        // recon-error-release), SubagentStop or the scanner's external-work PostToolUse synth (a
        // subagent/teammate turn runs in its OWN context — in-process teammates fire SubagentStop
        // for hours after the lead's last real turn, and it never touches the lead's cache), or any
        // UI mutation (the triage moves / Mark Unread). Feeds the Triage-Board ⚡ "still
        // server-cached" hint together with the transcript-derived, subagent-fold-free
        // convApiActivityUnixMs (ServerCacheStillWarm takes the freshest of the two): the hook side
        // lights the ⚡ the instant a prompt is submitted (the observer's transcript enrichment is
        // silent + survey-lagged), the transcript side covers hook-less (observer-adopted) sessions.
        // Transient (NOT persisted; Persistence.cpp must not write it) — after a reopen there is
        // nothing to follow up INTO until the session is resumed, and a resumed session's truth is
        // re-derived from the transcript within one survey.
        int64_t lastTurnUnixMs{ 0 };
        // True for a session ADOPTED from a claude we did NOT launch (typed into a `+` tab,
        // not Launched by the Manager). It is observe-only until the app correlates it to its
        // ConPTY (via the WT_SESSION tabToken) and binds an injector. Cleared once it is
        // (re)launched as a managed session on restore. Persisted so the card survives reopen.
        bool external{ false };
        // Transient (NOT persisted): the latest hosting WT_SESSION (the ConPTY's stable id) that
        // hooks reported for this session (the wire `tabToken`). The app correlates a tab to its
        // session by this. It is STABLE across an in-session `/resume` — which mints a NEW Claude
        // session id but keeps the SAME ConPTY — so the periodic tab reconcile re-homes the tab to
        // the new id by matching this against each live terminal's WT_SESSION. Empty until a hook
        // arrives (and after a fresh load, until the session re-emits one) — EXCEPT a Manager launch
        // stamps it eagerly from the connection's WT_SESSION (see the forkParentId note below).
        std::wstring tabToken;
        // Agentmaster (PERSISTED): the SOURCE conversation id this session was FORKED from — set at
        // launch when this is a fork (`claude --resume <src> --fork-session --session-id <this.id>`; the
        // Sessions-page / duplicate-tab / adopt-external fork). It serves TWO purposes:
        //   (1) The --fork-session source-id ECHO GUARD. A `--fork-session` claude fires its FIRST
        //       SessionStart hook under the SOURCE id `<src>`, NOT the freshly-minted `<this.id>` we
        //       registered + bound at launch. Without this, the registry sees `<src>` as an unknown
        //       session and the bind/re-home path mistakes the echo for an in-session `/resume`,
        //       re-homing the fork's tab off `<this.id>` onto `<src>` — so the tab tracks the wrong
        //       (inactive, source) conversation while the real fork (`<this.id>`, which gets every later
        //       hook) is orphaned. SessionRegistry::OnHookEvent uses it (with the eagerly-stamped
        //       tabToken) to IGNORE that source-id startup echo. The echo is ONE-SHOT — the transient
        //       `forkEchoConsumed` below retires the guard once the echo was suppressed (or any own-id
        //       hook proves startup passed), so a LATER deliberate `/resume <src>` re-homes normally.
        //   (2) Restoring a NEVER-MESSAGED fork — the reason this is now PERSISTED (it was transient
        //       before). A fork's OWN transcript (`<this.id>.jsonl`) is written only on its FIRST turn,
        //       so a fork the user created but never sent a message to has NO transcript on disk — and a
        //       plain restore would transcript-gate to a brand-new EMPTY conversation, silently LOSING
        //       the forked branch (and churning the id) on EVERY restart. With `<src>` persisted,
        //       _LaunchClaudeSession re-forks from it into the SAME id when the fork's own transcript is
        //       absent but the source's still exists — re-materializing the identical branch with its
        //       identity (and the WindowRecord tab ref) intact. The in-place "Restart session"
        //       (BuildClaudeRestartSpec) re-forks the same way. Gated on the fork being transcript-less,
        //       so once the fork gets its own conversation it is resumed, never re-forked.
        // Empty for a non-fork. Cleared ONLY on an own-id hook that proves the fork PRODUCED CONTENT
        // (its own transcript now exists — UserPromptSubmit / Stop / a tool hook), NEVER on
        // SessionStart or SessionEnd: a relaunch's SessionStart and the dying process's SessionEnd both
        // fire on a still-transcript-less fork, and wiping the link there is exactly what silently
        // turned a restarted never-messaged fork into an EMPTY conversation (the re-fork had no source).
        std::wstring forkParentId;
        // Transient (NOT persisted) — the --fork-session source-id ECHO GUARD's one-shot latch. False
        // while the guard is ARMED (a freshly launched / re-forked fork still expects its startup
        // SessionStart to echo the SOURCE id); flipped true when that echo is suppressed OR any own-id
        // hook arrives (startup passed — no echo can come anymore), after which a SessionStart for the
        // source id on this ConPTY is a DELIBERATE `/resume <src>` and re-homes normally. Split from
        // forkParentId so retiring the echo guard no longer destroys the re-fork link above (the old
        // design cleared forkParentId itself, so the fork's own SessionEnd — an own-id hook — wiped the
        // link right when the restart seam needed it). Re-armed (reset false) by the restart seam when
        // it re-forks. Always false after a load (a restored fork's re-fork spawns a fresh echo).
        bool forkEchoConsumed{ false };
        // Transient runtime flag (NOT persisted): is this session OPEN (has a live tab +
        // claude.exe this run) or ARCHIVED (shut down but kept restorable)? The Triage Board /
        // Explorer Tree show only Open (live) sessions; Archived (!live) ones are listed behind
        // the Manager's "Archived" button and can be restored on demand. Set true when we Launch
        // / restore / adopt a session, false when the user archives it (closes its tab). Always
        // false on load (Correctness Rule #6: startup re-opens NOTHING — every persisted session
        // comes back Archived, restorable as a whole), so it never needs to round-trip to JSON.
        bool live{ false };
        // Agentmaster (eager-init / "Activate Tab"): has this session's ConPTY/claude actually
        // STARTED, i.e. has its hosting TermControl left ConnectionState::NotConnected at least
        // once? A WT background tab spawns its child lazily — only on the SwapChainPanel's first
        // non-zero layout, which fires when the tab is first SHOWN — so a window-restored / re-homed
        // managed tab that the user never clicked stays dormant (claude never resumes, no hooks, no
        // autorunner). This flag is the UI's "needs activation" signal: false == live but dormant
        // (the half-hollow status dot + the "Activate All Tabs (N)" count + the per-tab/menu
        // "Activate Tab" gate); it flips true when the control starts (naturally on focus, or in
        // place via TermControl::InitializeWithSize). Transient (NOT persisted) and maintained ONLY
        // by TerminalPage (the single owner of the controls) from ConnectionState() each liveness
        // tick + immediately on an Activate; always false on load (a restored session is dormant
        // until opened), so it never round-trips to JSON. Meaningless for an `external` session
        // (we host no control) — left false, and every consumer also gates on `!external`.
        bool started{ false };
        // Set from the most recent Stop hook's best-effort `lastMessageIsQuestion`. Feeds the
        // Autorunner question-guard (M7): a turn that ended on a clarifying question must NOT be
        // auto-answered (DecideAdvance holds the queued prompt while this is true). PERSISTED (Rule
        // #16, crash/restore fidelity): the flag rides sessions.json so a crash while the agent was
        // waiting on a question can't drop the guard and auto-answer it on reopen — restored ONLY
        // alongside a preserved needs-you state (RestoredQuestionFlag drops it when the state
        // normalizes to Idle, so a stale flag from an interrupted Running turn can't falsely hold the
        // queue). Written only by a fresh Stop (SessionRegistry::OnHookEvent), never cleared by
        // SessionStart — so it correctly stays true across a --resume until a real non-question turn.
        bool lastMessageWasQuestion{ false };
        // Agentmaster (Waiting-for-you "unread" model): the last time the user READ this session —
        // i.e. visited (switched to) its terminal tab, or had it as the focused tab while a turn
        // completed. Transient (NOT persisted), wall-clock ms (NowMs / system_clock), set from the
        // UI lane (TerminalPage::_VisitTabClearFlash / _EvaluateAgentFlash). A session is "unread for
        // the current turn" when readUnixMs < lastActivityUnixMs (new activity landed since the last
        // read). The Waiting-for-you -> Idle decay (SessionScanner::_maybeDecayWaiting /
        // ShouldDecayWaitingToIdle) fires only once the session has been READ *and* the timeout has
        // elapsed: an unread, past-timeout session keeps waiting until the user reads it. 0 == never read.
        int64_t readUnixMs{ 0 };
        // Agentmaster (Waiting-for-you "unread" model): the context-menu "Mark Unread" — a STICKY
        // manual mark. While set, the session shows in Waiting-for-you (the mark can PROMOTE an
        // Idle/Done session there) and the time-decay never demotes it; only a visit (read) or
        // archive clears it. Transient (NOT persisted), set/cleared from the UI lane
        // (TerminalPage::_MarkSessionUnread / _ClearSessionUnread), the engine twin of the per-window
        // red-flash-ring set so the BOARD STATE (in every window) reflects the mark, not just one
        // window's tab ring.
        bool manualUnread{ false };
        // Transient (NOT persisted): turn accounting for the ordered state machine — see
        // TurnAccounting above / NextSessionStateOrdered (HookEvents.h). Reset on
        // SessionStart / SessionEnd (a resume must not inherit stale turn identity).
        TurnAccounting turns{};
        // Transient (NOT persisted): the latest assistant message text the interval reconciler
        // (SessionScanner) tailed from this session's transcript. Hooks don't carry assistant
        // output — this captures it (the foundation for a live Auto-Testing "peek"). Written via
        // the registry's QUIET path so streaming text never triggers a persist/UI/scheduler
        // cascade. Empty until the scanner reads a transcript line.
        std::wstring lastAssistantText;
        // Agentmaster (API-error triage — Transient, NOT persisted; Persistence.cpp must not write
        // them): the reason a turn DIED, preserved while state == Error so the Triage-Board Error card
        // (and any other surface) can show WHAT failed, not just a crimson dot. errorMessage is the
        // synthetic isApiErrorMessage line's text ("API Error: Server is temporarily limiting requests
        // … Rate limited" / "Prompt is too long" / "Credit balance is too low" / …); errorStatus is the
        // companion HTTP code from the transcript's apiErrorStatus (429 / 529 / 500 / 404 / 401), or 0
        // for a client-side error that carries none. Set by the SessionScanner's recon-error synth when
        // it produces Error, and CLEARED the moment the session leaves Error (recovery), both through
        // SessionRegistry::OnHookEvent. Empty/0 whenever the session is not in Error.
        std::wstring errorMessage;
        int errorStatus{ 0 };
        // Agentmaster (Error triage dismissal — Transient, NOT persisted; Persistence.cpp must not
        // write it): the user manually moved this session's Error card to Idle/Done (the triage
        // "Move to Idle/Done", offered on an Error session like on a Waiting-for-you one). Error is
        // LEVEL-derived — the scanner's recon-error re-asserts it every pass while the API error is
        // still the active leaf — so a plain state flip would bounce back within one pass; this flag
        // suppresses re-deriving Error off the UNCHANGED, acknowledged tail. It expires WITH the error
        // it acknowledged: the scanner clears it the moment it consumes a fresh TURN event (the
        // conversation moved — a retry, or a NEW error line, itself a turn event, re-fires normally),
        // and a fresh Error entry clears it too (SessionRegistry::OnHookEvent's apiError branch), so a
        // stale ack can never mask a later, undismissed error. Set ONLY by the UI triage moves (the
        // board/tree menu + the tab menu), which also clear errorMessage/errorStatus (the "Empty/0
        // outside Error" invariant above) — a resumed/reopened session honestly re-derives Error from
        // the tail (the RestoredSessionState philosophy), like the rest of the unread-model family
        // (readUnixMs / manualUnread).
        bool errorDismissed{ false };
        // Transient (NOT persisted; Persistence.cpp must not write it): the Claude Code idle RECAP —
        // the latest {"type":"system","subtype":"away_summary"} body the SessionScanner tailed from this
        // session's transcript, normalized (NormalizeRecapText: the "(disable recaps in /config)" hint
        // stripped). Claude Code emits it when a session sits idle >5 min: a one-paragraph "what we did /
        // what's next". Mirrored via the registry's QUIET path (display-only, no persist/UI/scheduler
        // cascade). Empty until a recap is seen (recaps off, or never idle). Shown in the Triage-Board
        // card's hover tooltip; the summary panel / Sessions detail / copy derive their own from
        // AnalyzeSessionTranscript (SessionSummary.awaySummary), the same data by a different path.
        std::wstring recap;
        // Transient (not persisted): in SemiAuto, the scheduler arms the next prompt here
        // and the Auto Testing shows a one-click confirm. Empty when nothing awaits confirm.
        std::wstring pendingConfirmPromptId;
        // Agentmaster (PENDING_INPUT.md): the session's UNSENT input-box DRAFT — text the user typed
        // into Claude's input box but has NOT yet submitted. Read out-of-band from the rendered terminal
        // buffer by the UI lane (TerminalPage::_ScanPendingInput -> ControlCore::ReadPendingInputDraft;
        // the bottom-most ❯ prompt line wrapped by ── rules, PendingInput.h). This is the ONE session
        // fact hooks can never carry — a draft is by definition not yet submitted, so no UserPromptSubmit
        // ever fires for it — so it is the lone screen-READ fact (analogous to the presence heartbeat;
        // never authoritative for SessionState, Rule #7/#13). Updated via the registry's QUIET,
        // change-gated SetPendingInput (the draft changes as the user types — like lastAssistantText it
        // must never trigger the persist / UI / scheduler cascade). Empty => no pending draft.
        // PERSISTED (PENDING_INPUT.md §5 — the user's "persist and load on startup the message"):
        // Persistence.cpp writes it (omitted when empty) so an unsent draft SURVIVES an Agentmaster
        // restart/crash as a MEMORY — shown staleness-labeled on the reopened tab until its claude
        // starts, then REVALIDATED against the live box (an empty read debounce-clears it; claude
        // itself never restores its input box, so the memory is the only copy). The pendingInputUnixMs
        // stamp below is what makes the staleness honest.
        std::wstring pendingInput;
        // PERSISTED alongside pendingInput (0 when no draft): when the draft was last actually OBSERVED
        // in the live box — re-stamped on EVERY observed non-empty scan tick (quietly, like the text),
        // so "now - pendingInputUnixMs" > a few ticks ⇔ the value is a carried MEMORY (a restored /
        // dormant / archived session), not a live read. The board tip derives its "last seen <ago>"
        // from exactly this.
        int64_t pendingInputUnixMs{ 0 };
        // PERSISTED alongside pendingInput ("" when none): the PendingPaste.h resolver's verdict for
        // the draft's "[Pasted text #N +M lines]" / "[...Truncated ...]" placeholders — one line per
        // marker, naming the content-anchored, arithmetic-VERIFIED paste-cache file (or "unresolved";
        // ResolvePendingPasteRefs). Display/annotation only — resolved off-thread on a draft change,
        // recorded via the registry's QUIET SetPendingPasteRefs (no notify: it rides the same flip the
        // draft itself already raised).
        std::wstring pendingPasteRefs;

        // --- Fleet Observer live enrichment (OBSERVER.md §5c) ---
        // ALL transient (NOT persisted — Persistence.cpp must not write them; PIDs / WT_SESSION /
        // AM_SESSION / process facts are per-run and re-derived each launch by the observer). Filled
        // by SessionRegistry::ObserveClaude from the S-lane's out-of-band PEB read; provenance only,
        // never authoritative state (push hooks + the transcript tail own SessionState).
        uint32_t pid{}; // claude.exe PID (0 = unknown / not running here)
        std::wstring liveCwd; // PEB cwd (authoritative live dir; tracks `cd` across a relaunch)
        std::wstring model; // --model / CLAUDE_CODE_* (drives Manager/overlay adornments)
        std::wstring effort; // --effort / CLAUDE_CODE_EFFORT_LEVEL
        std::wstring permissionMode; // --permission-mode
        std::wstring sessionName; // CLAUDE_CODE_SESSION_NAME (bg jobs)
        bool background{}; // a background/daemon claude
        RunningApp runningApp{ RunningApp::Unknown }; // ours (Agentmaster) vs external (WindowsTerminal)
        std::wstring amSession; // owning Agentmaster instance stamp (empty if external)
        std::wstring ownerWindowId; // the window that hosts this session's tab (§19-Q1 attribution)
        // Claude's OWN self-reported heartbeat state from its presence file
        // (~/.claude/sessions/<pid>.json → "busy"/"idle"/"waiting"/"shell"; SESSIONS.md §7-Q5).
        // A display-only FACT fed by the S-lane (validated against pid liveness) — deliberately
        // NOT SessionState (push hooks + the transcript tail own state, Rule #13; STATE.md owns
        // any future promotion). Empty when no live presence file backs this session.
        std::wstring presenceStatus;
        // The "waiting" DETAIL from that same heartbeat ("waitingFor", e.g. "input needed" for a
        // pending AskUserQuestion) — non-empty only while presenceStatus=="waiting". Same
        // provenance + same rules as presenceStatus: a transient display FACT, never persisted.
        // It is what lets the scanner recognize "blocked on the user" with NO transcript line and
        // NO hook — the two signals that were measured to be, respectively, minutes-late and
        // droppable (see TranscriptStore.h SessionPresenceRow::waitingFor).
        std::wstring presenceWaitingFor;
        bool hookWired{}; // have we received ANY hook for this id this run? (provenance)
        int64_t lastHookUnixMs{}; // last authoritative push (hook) — provenance vs the pull
        int64_t lastObservedUnixMs{}; // last pull observation (the S-lane survey)
        // Transcript-derived timing (the conversation's true age + last activity), filled by the
        // S-lane (ObserveClaude) from the transcript's ctime/mtime. Drives the Manager's per-session
        // timing adornment (created-ago / active-for / last-activity-ago). 0 until the transcript
        // exists. Transient — re-derived each run (NOT persisted; Persistence.cpp must not write them).
        int64_t convCreatedUnixMs{}; // transcript ctime (≈ conversation start)
        int64_t convLastActivityUnixMs{}; // transcript mtime (≈ last activity)
        // The API-TURN half of the transcript timing: the line-derived last activity WITHOUT the
        // subagent side-file fold — the newest REAL user/assistant line of the PARENT conversation
        // itself, i.e. (approximately) when THIS conversation last made an API request. Feeds ONLY
        // the ⚡ ServerCacheStillWarm hint below: convLastActivityUnixMs above deliberately FOLDS
        // subagent/teammate side-file activity for the display adornment ("the tab is active"), but
        // a teammate's/subagent's API turn runs in its OWN context and never re-warms the LEAD
        // conversation's prefix cache — folding it kept ⚡ lit for the whole (minutes–hours) life of
        // a background team while the lead's cache was long cold. 0 until resolved (never the file
        // mtime). Transient — re-derived each run (NOT persisted; Persistence.cpp must not write it).
        int64_t convApiActivityUnixMs{};
        // Context-window occupancy: the NEWEST assistant message's usage tokens
        // (input + cache_creation + cache_read + output ≈ the size of the last request = current
        // context size). Filled QUIETLY by the SessionScanner as it tail-reads the transcript;
        // displayed on the Manager board card as a RAW TOKEN COUNT (PR feedback: a % needs a context-
        // window denominator that can't be reliably inferred from the model id, so the raw count is
        // shown instead). 0 until the first assistant turn. Transient — re-derived each run (NOT persisted).
        int64_t contextTokens{};
        // The CURRENT model — the model the session's NEWEST real assistant reply was produced by
        // (the transcript assistant line's `message.model`, e.g. "claude-fable-5"). This is the truth
        // the `model` field above can't be: `model` is the LAUNCH REQUEST (the cmdline `--model` /
        // CLAUDE_* env the observer reads from the PEB — usually EMPTY on a bare `claude`, and stale
        // the moment the user runs `/model` mid-session: the switch touches neither cmdline nor env
        // and writes NO transcript trailer, so the next assistant reply is the earliest it is
        // observable on disk). Written ONLY by the SessionScanner as it tails the transcript (the
        // contextTokens sibling — same assistant line, and the same first-sight full replay hands a
        // resumed/dormant session its historical model immediately) — change-gated, via the NOTIFYING
        // Update (unlike per-line-churning contextTokens: a model changes at most a handful of times
        // per session, and the board card / per-tab overlay / tab tooltip should repaint when it
        // does). The synthetic API-error line's pseudo-model "<synthetic>" is never recorded. Codex:
        // stays empty (its rollout-derived model already lives in `model` — display surfaces fall
        // back, see SessionDisplayModel). Transient — re-derived each run (NOT persisted;
        // Persistence.cpp must not write it).
        std::wstring currentModel;

        std::vector<QueuedPrompt> queue; // the Auto Testing
        AutorunnerState autorunner{};
    };

    // Agentmaster (the ⚡ "still server-cached" hint — the Triage-Board card AND the tab strip's SPARK
    // CROWN, which share THIS ONE predicate so the two surfaces can never disagree) — PURE + unit-tested.
    // Claude's server-side prompt cache stays warm ~cacheMinutes after the last REAL API request, so a
    // follow-up inside the window reuses the cached prefix (cheaper & faster). Shown only for a session
    // AT REST (never Running — see the gate below). "Real API request" is the operative phrase — the hint
    // reads ONLY the two API-turn signals:
    //   • convApiActivityUnixMs — the transcript's PARENT-line-derived last activity (real
    //     user/assistant line timestamps of THIS conversation; a `--resume`'s untimestamped trailer
    //     appends never move it, and — unlike the folded display value convLastActivityUnixMs — a
    //     subagent/teammate SIDE-FILE write never moves it either: those turns run in their OWN
    //     context and never re-warm the LEAD's prefix cache, so a background team writing for an
    //     hour must not keep ⚡ lit), fed by the Fleet Observer. Note a fork/adopt of a
    //     recently-active conversation legitimately reads warm: the cache is PREFIX-keyed, so the
    //     duplicated history IS still cached for the new session — that ⚡ is a true positive.
    //   • lastTurnUnixMs — the hook-time turn evidence (IsApiTurnEvidence, HookEvents.h — which
    //     likewise excludes SubagentStop + the scanner's external-work PostToolUse synth), lighting
    //     the hint the moment a prompt is submitted and covering a session whose transcript the
    //     observer can't resolve.
    // Deliberately NOT lastActivityUnixMs (the Waiting-for-you decay anchor): SessionStart at
    // launch / adopt / resume / /clear and the "Move to Waiting-for-you" triage promote stamp that
    // anchor "now" with zero API traffic — the reported ⚡ false positives ("shows right after
    // adopting / after Move to Waiting-for-you / on a never-prompted launch") this predicate
    // exists to exclude. Codex never shows the hint: the tooltip copy ("Claude's prompt cache")
    // and both signals are Claude-specific (a managed Codex has no hooks and its conv timing is
    // never fed), and before this gate a Codex card could ⚡ off a bare triage-move stamp.
    inline bool ServerCacheStillWarm(const SessionInfo& s, uint32_t cacheMinutes, int64_t nowMs) noexcept
    {
        // AT REST ONLY — never while Running. The hint answers "if I follow up NOW, is it cheap?", which
        // is a question only a session that is WAITING ON YOU can pose: WaitingForInput / NeedsApproval /
        // Error / Idle / Done. Mid-turn there is nothing to decide, and worse, the hint would be
        // PERMANENTLY LIT there — a Running session refreshes convApiActivityUnixMs / lastTurnUnixMs
        // continuously, so the window can never lapse while the turn is in flight. Excluding the one
        // state in which it is both uninformative and always-on is what makes it read as a signal
        // ("this one is still cheap to resume") instead of decoration.
        if (!s.live || s.kind != AgentKind::Claude || cacheMinutes == 0 || s.state == SessionState::Running)
        {
            return false;
        }
        const int64_t last = (s.convApiActivityUnixMs > s.lastTurnUnixMs) ? s.convApiActivityUnixMs : s.lastTurnUnixMs;
        return last > 0 && (nowMs - last) < static_cast<int64_t>(cacheMinutes) * 60000;
    }

    // Agentmaster (current-model adornment): the ONE display-model resolution every surface shares —
    // the transcript-derived CURRENT model when known (what the last reply actually ran on), else the
    // launch-request `model` (the cmdline/env read, also where a managed Codex's rollout model
    // lives). Returns "" only when neither is known (a never-prompted bare launch). PURE.
    inline const std::wstring& SessionDisplayModel(const SessionInfo& s) noexcept
    {
        return s.currentModel.empty() ? s.model : s.currentModel;
    }

    // Agentmaster (current-model adornment): the SHIPPED default Anthropic model-FAMILY words —
    // what ShortModelName recognizes when shortening a BARE model alias (one without a "claude-"
    // prefix, e.g. a cmdline `--model opus-4.6`; a full "claude-…" id never consults the list).
    // The AppSettings::modelFamilies default, so the Settings cog box shows this list ready to
    // EXTEND when Anthropic ships a new family — no code change needed (the way "mythos" would
    // have required one).
    inline constexpr std::wstring_view kDefaultModelFamilies = L"opus, sonnet, haiku, fable, mythos, instant";

    // Agentmaster (current-model adornment): parse a user-configured model-family list (the cog's
    // AppSettings::modelFamilies) — comma/semicolon/whitespace-separated words, lowercased,
    // deduped, input order kept. Empty/blank csv -> an empty vector (the display sites then fall
    // back to the built-ins via ShortModelName's empty-means-default rule, so a cleared/garbled
    // box can never break the model adornment). PURE + total.
    inline std::vector<std::wstring> ParseModelFamilies(std::wstring_view csv)
    {
        std::vector<std::wstring> out;
        std::wstring cur;
        const auto flush = [&out, &cur]() {
            if (cur.empty())
            {
                return;
            }
            for (auto& c : cur)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            for (const auto& existing : out)
            {
                if (existing == cur)
                {
                    cur.clear();
                    return;
                }
            }
            out.push_back(cur);
            cur.clear();
        };
        for (const wchar_t c : csv)
        {
            if (c == L',' || c == L';' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
            {
                flush();
            }
            else
            {
                cur.push_back(c);
            }
        }
        flush();
        return out;
    }

    // The built-in family list, parsed once (kDefaultModelFamilies as a vector — the fallback for
    // an empty/unset configured list).
    inline const std::vector<std::wstring>& DefaultModelFamilies()
    {
        static const std::vector<std::wstring> kFamilies = ParseModelFamilies(kDefaultModelFamilies);
        return kFamilies;
    }

    // Agentmaster (current-model adornment): shorten an Anthropic model id for display — the Triage-
    // Board card / per-tab overlay / tab tooltip show "opus-4.6" instead of
    // "claude-opus-4-6-20260105". Handles every id shape in the wild:
    //   claude-fable-5                                -> fable-5     (new date-less ids)
    //   claude-opus-4-8                               -> opus-4.8
    //   claude-haiku-4-5-20251001                     -> haiku-4.5   (dated ids)
    //   claude-3-7-sonnet-20250219                    -> sonnet-3.7  (old family-last ids)
    //   us.anthropic.claude-sonnet-4-5-20250929-v1:0  -> sonnet-4.5  (Bedrock; Vertex "@date" too)
    //   claude-instant-1.2                            -> instant-1.2
    //   opus / sonnet-5 / fable                       -> unchanged   (bare aliases users pass --model)
    // A NON-Anthropic id (a managed Codex's "gpt-5.1-codex") or anything unrecognized is returned
    // VERBATIM — never mangled — so callers can apply it unconditionally; likewise the API-error
    // pseudo-model "<synthetic>" (which producers already skip). Rules: the version is every short
    // (≤3-digit) numeric token joined by '.' (they surround the family in both the old and new id
    // orders), an 8-digit date / "-latest" / Bedrock "-v1:0" / a Vertex "@…" suffix never count, and
    // a trailing "[…]" marker (the 1M-context "[1m]" alias form) is preserved verbatim.
    // `families` = the recognized family words for the BARE-alias path (no "claude-" prefix — a
    // prefixed id shortens on ANY alpha family): the cog's AppSettings::modelFamilies via
    // ParseModelFamilies, REPLACING the built-ins so the settings list is the whole truth; empty
    // (default / a cleared box) falls back to DefaultModelFamilies(). PURE + total.
    inline std::wstring ShortModelName(std::wstring_view id, const std::vector<std::wstring>& families = {})
    {
        // Trim surrounding whitespace.
        size_t b = 0, e = id.size();
        while (b < e && (id[b] == L' ' || id[b] == L'\t'))
        {
            ++b;
        }
        while (e > b && (id[e - 1] == L' ' || id[e - 1] == L'\t'))
        {
            --e;
        }
        const std::wstring original{ id.substr(b, e - b) };
        if (original.empty())
        {
            return {};
        }
        // Split off a trailing "[...]" marker (e.g. "sonnet-5[1m]") — re-appended verbatim.
        std::wstring bracket;
        std::wstring core = original;
        if (core.back() == L']')
        {
            if (const auto open = core.rfind(L'['); open != std::wstring::npos)
            {
                bracket = core.substr(open);
                core.erase(open);
            }
        }
        // Lowercase (ids are ASCII; aliases may be typed capitalized).
        for (auto& c : core)
        {
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        // Anchor on the "claude" token when present (strips "us.anthropic." / "anthropic." prefixes).
        bool hadClaude = false;
        if (const auto p = core.find(L"claude"); p != std::wstring::npos &&
                                                 (p == 0 || core[p - 1] == L'.' || core[p - 1] == L'/' || core[p - 1] == L'-' || core[p - 1] == L'_'))
        {
            core.erase(0, p);
            hadClaude = true;
        }
        // Strip provider suffixes: Vertex "@20250929", Bedrock "-v1:0", "-latest".
        if (const auto at = core.find(L'@'); at != std::wstring::npos)
        {
            core.erase(at);
        }
        if (const auto colon = core.rfind(L':'); colon != std::wstring::npos)
        {
            if (const auto v = core.rfind(L"-v", colon); v != std::wstring::npos && v < colon)
            {
                core.erase(v);
            }
        }
        constexpr std::wstring_view latest = L"-latest";
        if (core.size() > latest.size() && std::wstring_view{ core }.substr(core.size() - latest.size()) == latest)
        {
            core.erase(core.size() - latest.size());
        }
        // Tokenize on '-' into the family (first all-alpha token besides "claude") + the short
        // numeric version tokens (digits, or digits with an embedded '.'; an 8-digit date is not one).
        std::wstring family;
        std::wstring version;
        size_t start = 0;
        for (size_t i = 0; i <= core.size(); ++i)
        {
            if (i < core.size() && core[i] != L'-')
            {
                continue;
            }
            const std::wstring_view tok = std::wstring_view{ core }.substr(start, i - start);
            start = i + 1;
            if (tok.empty() || tok == L"claude")
            {
                continue;
            }
            bool alpha = true, numeric = true;
            size_t digits = 0;
            for (const wchar_t c : tok)
            {
                if (c >= L'0' && c <= L'9')
                {
                    alpha = false;
                    ++digits;
                }
                else if (c == L'.')
                {
                    alpha = false;
                }
                else
                {
                    numeric = false;
                }
            }
            if (alpha && family.empty())
            {
                family = tok;
            }
            else if (numeric && digits > 0 && digits <= 3)
            {
                if (!version.empty())
                {
                    version += L'.';
                }
                version += tok;
            }
        }
        // Without a "claude" anchor only a KNOWN Anthropic family may be shortened — anything else
        // ("gpt-5.1-codex", "o4-mini", "<synthetic>") passes through verbatim. "Known" is the
        // CONFIGURED list (the cog's Model families box), the built-ins when none is set.
        if (!hadClaude)
        {
            const std::vector<std::wstring>& known = families.empty() ? DefaultModelFamilies() : families;
            bool recognized = false;
            for (const auto& f : known)
            {
                if (family == f)
                {
                    recognized = true;
                    break;
                }
            }
            if (!recognized)
            {
                return original;
            }
        }
        if (family.empty())
        {
            if (!hadClaude || version.empty())
            {
                return original; // nothing recognizable — never mangle
            }
            family = L"claude"; // e.g. "claude-2" -> "claude-2"
        }
        std::wstring out = family;
        if (!version.empty())
        {
            out += L'-';
            out += version;
        }
        out += bracket;
        return out;
    }

    // A reusable plan (DESIGN §10 "Plans across the fleet"): a named sequence of prompts
    // that can be applied to any session or broadcast to many. Persisted separately.
    struct PlanTemplate
    {
        std::wstring name;
        std::vector<QueuedPrompt> prompts; // status is reset to Pending (with fresh ids) on apply
    };

    // Persisted geometry for the Manager tab's draggable pane splitters (C1 "Linked
    // Lenses"). Each value is the FIRST track's share of the two tracks its splitter
    // divides, kept within (0,1) — the second track takes the remainder. Defaults match the
    // original 2:3 star ratios so first run looks unchanged.
    struct ManagerLayout
    {
        double boardFraction{ 0.4 }; // Triage Board height / (Board + Bottom)      [root rows]
        double treeFraction{ 0.4 }; // Explorer Tree width / (Tree + Auto Testing)   [bottom cols]
    };

    // Agentmaster (launch-model picker): the SHIPPED default launch-model list — what every
    // "Open New Session Here" model submenu offers until the user edits the list in the Settings
    // cog. One entry per line, "Display name | model-id": the left side is the submenu label, the
    // right is what the spawn passes as `--model <id>` (see ParseLaunchModels in ClaudeSpawn.h).
    //
    // The ids are Claude Code's ALIASES ("fable" / "opus" / "sonnet"), NOT pinned version ids
    // ("claude-opus-4-8"), and the labels drop the version with them. `claude --help` defines an
    // alias as "an alias for the LATEST model" ("Provide an alias for the latest model (e.g.
    // 'fable', 'opus', or 'sonnet') or a model's full name (e.g. 'claude-fable-5')"), so an alias
    // FOLLOWS Anthropic's next release on its own, while a pinned id silently rots — this list
    // still named Opus 4.8 long after Opus 5 shipped — and eventually fails to launch when that
    // version retires. A user who WANTS a pinned version just types the full id in the cog box;
    // the picker passes whatever is on the right side through to `--model` verbatim.
    inline constexpr std::wstring_view kDefaultLaunchModels =
        L"Fable | fable\n"
        L"Opus | opus\n"
        L"Sonnet | sonnet";

    // Agentmaster (launch-model picker): the launch-model lists we shipped as the default BEFORE
    // the alias switch above. A stored settings.json value that still equals one of these is a
    // list the user never edited, so it is UPGRADED to the current default on load
    // (LaunchModelsAreSupersededDefault / AppSettingsFromJson) — the /handover definition files'
    // "ours, unmodified => upgrade; user-edited => never touch" policy, applied to a setting.
    // Without it the presence-gated key would pin every EXISTING install to the stale versioned
    // ids forever, which is exactly the rot the switch is meant to end.
    //
    // The comparison is SEMANTIC (the parsed {name, id} pairs, not the bytes): the cog's TextBox
    // rewrites newlines to '\r' the first time the user opens and saves settings, so a byte
    // compare would miss most real installs. A user who deliberately TYPED the old list verbatim
    // is indistinguishable from one who never touched it and upgrades too — the accepted
    // trade-off of this policy (re-typing pinned ids restores them, and any reorder/rename/extra
    // entry already fails the match). Append-only, oldest first; never remove an entry — that
    // would strand the installs still carrying it.
    inline constexpr std::wstring_view kSupersededLaunchModels[] = {
        L"Fable 5 | claude-fable-5\nOpus 4.8 | claude-opus-4-8\nSonnet 5 | claude-sonnet-5",
    };

    // Global app settings — the Manager toolbar's Settings cog (next to "Pause Autorunner").
    // Every default reproduces the prior hardcoded behavior EXCEPT defaultAutorunnerMode (now
    // Full, the product default — "all new or opened sessions run on Autorunner"), so a missing
    // settings.json mostly changes nothing. Persisted to ~/.agentmaster/settings.json, loaded at
    // startup, and applied at two seams: the spawn recipe (Claude fields) and session OPEN — the
    // autorunner MODE is stamped onto EVERY opened session (new / adopted / restored); the other
    // autorunner backstops are stamped onto NEW sessions only. These are GLOBAL defaults/backstops;
    // per-session autorunner mode still lives in the Auto Testing (changeable after open).
    // Agentmaster (COMMANDS.md §6a — customizable slash commands): the shipped defaults of the
    // /handover family's command names. ONE definition each — AppSettings' struct defaults, the
    // Persistence absent-key fallbacks, the reconcile's "what a pre-feature install has on disk",
    // and the collision-heal targets all read these.
    inline constexpr std::wstring_view kDefaultHandoverCommandName = L"handover";
    inline constexpr std::wstring_view kDefaultHandoverHereCommandName = L"handover-here";
    inline constexpr std::wstring_view kDefaultHandoverStandbyCommandName = L"handover-standby";

    // Agentmaster (COMMANDS.md §6b): the SHIPPED DEFAULT regex settings of the /handover family.
    // These are REAL VALUES seeded into settings.json and shown in the cog's boxes — deliberately
    // NOT invisible code fallbacks: the user can SEE what the default rule is and edit from it
    // (the launchModels/modelFamilies idiom — presence-gated on load, so an absent key seeds the
    // default while a PRESENT empty string is a deliberate "fall back to the built-in behavior").
    //
    //  * the TITLE pair reproduces the classic "<origin> (handover)" naming EXACTLY, chaining
    //    included: the find pattern optionally EATS an existing " (handover)" / " (handover N)"
    //    suffix, so a successor of "Foo (handover)" resolves to "Foo (handover)" again — which the
    //    caller's uniqueness bump (DeriveSuffixedTitle) then walks to "(handover 2)", "(handover
    //    3)", … instead of STACKING "(handover) (handover)". That is the whole reason the pattern
    //    is not the naive `^(.*)$`: a stacked suffix was the exact bug DeriveForkTitle exists to
    //    prevent, and it must not come back through a default.
    //  * the FILE-MATCH pattern spells the file-NAME contract the shipped command definitions
    //    actually instruct — `HANDOVER-<topic>.md`, i.e. the name contains "HANDOVER-" (unanchored
    //    search, applied case-insensitively, so `handover-notes.md` matches too). It is DELIBERATELY
    //    tighter than the plain contains-"handover" leaf hint it replaced: a repo doc merely
    //    MENTIONING handover in its name (`handover.md`, `HANDOVER_NOTES.md`, `old-handover.md`)
    //    is no longer collected as a briefing. The hyphen is escaped (`\-`) — a valid ECMAScript
    //    identity escape, kept because it reads as "the separator is literal" in the cog's box.
    //    Files the pattern misses are still covered by the nothing-collected-yet tolerance (the
    //    batch's first markdown), which the DEFAULT keeps on — see Engine.cpp / CommandWatch.
    inline constexpr std::wstring_view kDefaultCommandTitleFindRegex = L"^(.*?)(?: \\(handover(?: \\d+)?\\))?$";
    inline constexpr std::wstring_view kDefaultCommandTitleReplace = L"$1 (handover)";
    inline constexpr std::wstring_view kDefaultCommandFileMatchRegex = L"HANDOVER\\-";

    // Agentmaster (COMMANDS.md §6c — WHERE a handover briefing is written): the shipped default
    // location. Both definitions carry ONE "WRITE IT IN: <phrase>" line rendered from the
    // commandHandoverWritePath setting, so the target folder is a SETTING instead of the
    // hard-coded "current working directory" the pre-§6c texts named — the answer to HANDOVER-*.md
    // litter accumulating at repo roots. The default is the session SCRATCHPAD (Claude Code's
    // per-session temp folder): a briefing is a transient hand-off whose CONTENT is injected into
    // the successor anyway, so the file itself does not belong in the user's repo. This token and
    // "" (a deliberately cleared box) mean the same thing.
    inline constexpr std::wstring_view kCommandWritePathScratchpad = L"scratchpad";
    // The pre-§6c location, offered as a preset: the session's working directory.
    inline constexpr std::wstring_view kCommandWritePathWorkingDir = L"./";

    // Agentmaster (COMMANDS.md §6/§6c): what a shipped slash-command DEFINITION file on disk IS,
    // relative to what Agentmaster would write for it now. The write policy never overwrites a file
    // it does not recognize, so this is how the cog can SAY that a hand-edited definition is frozen
    // (and offer the confirmed Reinstall) instead of silently ignoring the settings around it.
    // Lives here — not in ClaudeSpawn.h — so the UI headers can hold one without pulling in the
    // spawn machinery. Produced by InspectShippedCommandFileNamedIn / InspectHandoverCommandFiles.
    enum class ShippedCommandFileState
    {
        Missing, // no definition file (the next reconcile creates it)
        UpToDate, // ours, byte-identical to the current text under this name + write location
        OursStale, // ours (some shipped version), but not what we'd write — the next reconcile rewrites it
        UserOwned, // matches nothing we ever shipped: hand-edited or foreign — NEVER touched
    };

    struct AppSettings
    {
        // --- Claude sessions (spawn recipe; see ClaudeSpawn) ---
        // ON  => spawn with --dangerously-skip-permissions (auto-accepts tool prompts). OFF => no
        //        flag; the settings file instead carries the "other variation"
        //        permissions.defaultMode:"default" (normal prompts apply).
        //        ⚠ This does NOT skip the startup WORKSPACE-TRUST dialog — an older comment here
        //        said it did, and that was wrong (Claude's trust gate never consults the permission
        //        mode; PTY-probed false on 2.1.220). trustWorkspaceOnLaunch below is what suppresses
        //        that dialog.
        bool skipPermissions{ true };
        // Agentmaster: pre-trust a session's working directory before launching claude there, by
        // seeding ~/.claude.json projects[<repo-or-dir>].hasTrustDialogAccepted = true (the remedy
        // Claude Code itself prints; see ClaudeSpawn.h's workspace-trust block for the full gate).
        // ON (default) because an unattended ConPTY session CANNOT answer Claude's modal "Is this a
        // project you trust?" prompt: the tab parks on the dialog, fires no UserPromptSubmit, mints
        // no transcript id (so it can only ever show the unlinked observe badge), the Tests
        // Autorunner can't drive it, and a /handover injection is eaten by the menu — one manual
        // click per tab, which does not scale to a fleet. Worst hit is a session launched in the
        // HOME directory (the empty-defaultLaunchDir fallback), which Claude deliberately refuses to
        // remember: accepting there sets an in-memory flag only, so it re-prompts forever.
        // OFF => we never touch ~/.claude.json and Claude's normal trust flow applies.
        bool trustWorkspaceOnLaunch{ true };
        // "" (As Is) => don't override the model. Else == what you'd type after `/model `
        //  (e.g. "opus" / "sonnet" / a full id) -> emitted as the settings `model` key.
        std::wstring model{};
        // Agentmaster (launch-model picker): the models every "Open New Session Here" submenu
        // offers (the Manager board/tree + External row menus, the Sessions page row menu +
        // detail button, and the WT tab context menu) — picking one launches that session with
        // `--model <id>`, overriding the global `model` above for THAT session only ("Default"
        // launches exactly as before). One "Display name | model-id" entry per line (';' also
        // separates, '#' comments a line; a bare entry is both name and id — ParseLaunchModels,
        // ClaudeSpawn.h). Editable in the Settings cog (Sessions tab; the submenu tooltips point
        // there). An ABSENT settings.json key seeds these shipped defaults; a PRESENT value —
        // even "" (the user cleared the box; submenus then offer just "Default") — is kept
        // verbatim (Persistence gates the default on key presence, not emptiness).
        std::wstring launchModels{ kDefaultLaunchModels };
        // Agentmaster (current-model adornment): the Anthropic model-FAMILY words ShortModelName
        // recognizes when shortening a BARE model alias for display — the board card / per-tab
        // overlay / tab tooltip model text (a full "claude-…" id, the transcript's usual shape,
        // never consults this list; only a prefix-less `--model opus-4.6`-style alias does).
        // Comma/space-separated, case-insensitive; REPLACES the built-in list, so editing here is
        // how a NEW family ships without a code change. Blank/garbage self-heals: the display
        // sites fall back to the built-ins (ParseModelFamilies -> empty ->
        // DefaultModelFamilies inside ShortModelName), so a cleared box can never break the
        // adornment. An ABSENT settings.json key seeds this shipped default (presence-gated like
        // launchModels) so the cog box always shows the current list, ready to extend.
        std::wstring modelFamilies{ kDefaultModelFamilies };
        // false => emit includeCoAuthoredBy:false (drop Claude's commit/PR co-author byline).
        bool includeCoAuthoredBy{ true };
        // The GLOBAL extra environment injected into EVERY spawned session (Claude AND Codex), edited
        // in the Settings cog's "Environment variables" area (Global tab) as a multi-line NAME=VALUE
        // list (one per line; an old ';'-delimited single line still parses). Parsed by
        // ParseEnvAssignments; merged with the PER-DIRECTORY overrides (dir-env.json, NOT stored here —
        // it's keyed by working dir) at spawn via MergeSessionEnv/ResolveSessionEnv, per-dir winning.
        // CCMGR_* names are dropped (reserved for hook correlation); AM_SESSION/WT_SESSION are set by
        // the connection and a user value is ignored.
        std::wstring env{};
        // Agentmaster (native-exe-only policy): an explicit override path to the native claude.exe.
        // "" => auto-detect (PATH claude.exe, ~/.local/bin\claude.exe, or a claude.cmd's npm binary —
        // see ResolveClaudeExe). When set it MUST be an existing *.exe (the Settings UI validates +
        // browses for it). The whole app launches/forks/resumes ONLY when a native claude.exe resolves;
        // otherwise every claude interaction is gated behind an "install the native build" prompt. A
        // pure-Node `claude` (no native binary) is deliberately unsupported (the Fleet Observer + PEB
        // enrichment are all claude.exe-keyed).
        std::wstring claudeExePath{};

        // --- Autorunner defaults ---
        // The MODE seeds every OPENED session — new, adopted, AND restored/window-restored (a
        // reopened session is re-armed on this default, OVERRIDING its saved per-session mode),
        // and stays changeable afterward (the Auto Testing toggle / this cog). Default Full ==
        // "all new or opened sessions run on Autorunner". The backstops below are stamped onto
        // NEW sessions only; a restored one keeps its persisted maxAutoSends/stopOnError/pauseOnHumanInput.
        AutorunnerMode defaultAutorunnerMode{ AutorunnerMode::Full };
        uint32_t maxAutoSends{ 100 }; // runaway backstop
        bool stopOnError{ true }; // pause a plan when a turn ends in error
        bool pauseOnHumanInput{ true }; // suspend auto-send while the human is typing
        // Agentmaster (PENDING_INPUT.md §9 — the DRAFT SWAP): when a prompt is submitted into a
        // session whose input box already holds the user's UNSENT draft, take the draft out of the
        // way first and put it straight back afterwards, instead of pasting the prompt ON TOP of it
        // (which submitted draft + prompt as one message the user never wrote — the merge bug).
        // GLOBAL, and it governs EVERY submit path at once (the autorunner's auto-send, its SemiAuto
        // confirm, the Manager's Send-now, and the /handover paste pump all share one seam —
        // SessionRegistry::SubmitPrompt), so the four can never disagree about it. A session with no
        // draft is unaffected either way: the swap is skipped and the injection is byte-identical to
        // before. OFF restores the historical merge behavior verbatim.
        bool preserveDraftOnSend{ true };
        // Agentmaster (PENDING_INPUT.md §9): use Claude's own STASH toggle — Ctrl+S — as the swap's
        // first rung. It is a single keystroke over ONE slot: pressed with text in the box it stashes
        // the WHOLE box aside and empties it; pressed on an empty box it restores. Both of our presses
        // land on the correct side by construction (we clear only when the box has text, restore only
        // when it is verified empty). Whole-box and cursor-position independent, which is exactly why
        // it beats the line-scoped Ctrl+U kill-ring pair that remains the fallback. OFF ⇒ the swap
        // starts at that fallback instead; either way an unresponsive rung ends in a clean ABORT.
        bool draftSwapUseCtrlS{ true };
        // Agentmaster (PENDING_INPUT.md §10 — the restore RE-FILL): when a session whose record
        // carries a persisted unsent-draft MEMORY is reopened (Sessions-browser resume /
        // window-restore rehome / re-fork), TYPE the remembered draft back into the fresh claude's
        // EMPTY input box once it starts — the /handover-standby channel (BuildPromptFill: a
        // bracketed paste with NO submit CR, verified by reading the box back), so the draft sits
        // one Enter away exactly as it did before the restart. Claude itself never restores its own
        // box, so without this the memory only DISPLAYS (dots + tip) until honest revalidation
        // clears it ~5s after the tab starts. A draft whose paste placeholders all resolve is
        // re-filled EXPANDED (claude re-collapses + re-binds the paste at paste time); an
        // unresolvable placeholder REFUSES the whole fill (the §9 rule — a literal
        // "[Pasted text #N]" label would silently drop the content behind it on submit). OFF
        // restores the display-only memory behavior verbatim.
        bool restoreDraftOnResume{ true };

        // --- Behavior sugar ---
        bool confirmBeforeKill{ true }; // confirm before killing a session from the Manager
        // How the tab/session rename box commits via the keyboard (focus-loss always commits). GLOBAL
        // across windows. Default = Shift+Enter commits while a plain Enter still inserts a newline
        // (titles are multi-line). See TabRenameCommitMode.
        TabRenameCommitMode tabRenameCommitMode{ TabRenameCommitMode::ClickAwayOrShiftEnter };
        std::wstring defaultLaunchDir{}; // "" => the Launch cwd box defaults to %USERPROFILE%
        // Agentmaster (Waiting-for-you "unread" model): how long a session may sit in the Triage
        // Board's "Waiting-for-you" column before the time-decay is allowed to demote it to Idle.
        // The decay is gated on BOTH this timeout AND read-state: a WaitingForInput session demotes
        // to Idle only once (a) this many minutes have passed with no activity AND (b) the user has
        // READ it (visited its tab) since the last turn — an unread, past-timeout session keeps
        // waiting until read; a manually "Mark Unread"-ed session never time-decays at all. Enforced
        // by the SessionScanner (ShouldDecayWaitingToIdle). 0 == never decay (the cog's "Never"
        // toggle). Default 4320 (3 days). The OLD conflation of this with Claude's ~5-minute server
        // cache is split out into serverCacheMinutes (below), which now drives only the card's
        // "still cached" ⚡ indicator. The cog exposes a SLIDER of 1m .. 7d (1..10080) beside a
        // free-text "12h5m" box that may exceed 7d (the slider then sits maxed), plus a "Never" toggle.
        // NOTE: this was renamed from the legacy "waitingDecayMinutes" key DELIBERATELY — the meaning
        // changed (a 5-minute cache window -> a read-gated unread timeout), so a pre-existing
        // settings.json (which carried a value tuned for the old behavior, often 5) must NOT carry
        // over. The new key is absent there, so every existing install falls back to this 4320 default;
        // the orphaned old key is ignored and dropped on the next save (Persistence rebuilds the file).
        uint32_t waitingForYouTimeoutMinutes{ 4320 };
        // Agentmaster: Claude's SERVER-SIDE prompt-cache lifetime, in minutes (Anthropic caches the
        // prompt prefix ~5 minutes after the last turn, so a follow-up within the window is cheap).
        // Drives ONLY the Triage-Board card's "still cached" ⚡ indicator (shown for this many minutes
        // after a session's last activity) — it no longer gates the Waiting-for-you decay (that is
        // waitingForYouTimeoutMinutes). Default 5. 0 falls back to 5 (the indicator is a cosmetic hint).
        uint32_t serverCacheMinutes{ 5 };
        // How many recent working directories the Launch path-picker's "RECENT" section
        // remembers (in recent-dirs.json) and lists. Default 10. (0/garbage falls back to 10.)
        uint32_t recentDirsLimit{ 10 };
        // Agentmaster (bookmark tags): how many DISTINCT tags may exist GLOBALLY (the tag universe
        // is the union of every session's SessionStore "tags" lists — a tag lives while >=1 session
        // carries it). The cap gates only the creation of a NEW tag name from the tab menu's Tag
        // panel; tags already applied are never dropped by lowering it. Default 20, configurable in
        // the cog up to the hard ceiling 40 (ClampMaxTags below — load + save both clamp).
        uint32_t maxTags{ 20 };
        // Agentmaster (bookmark tags): the OPACITY of the bookmark-tag chips rendered in the rich
        // tab TOOLTIP (the name + its colored underscore), 0..1. A cog slider (10..100%); default
        // 0.9 (90% solid) — a hair of translucency so a dense tag row reads as secondary to the
        // title/state above it without disappearing. ONLY the tooltip's tag row honors it (the
        // tab-strip badge ribbons + the Sessions column stay fully opaque — they're the primary
        // affordance). ClampTooltipTagsOpacity below bands it on load + save (0/absent/garbage ->
        // 0.9), so a missing key reproduces the default.
        double tooltipTagsOpacity{ 0.9 };

        // --- Tab strip ---
        // Agentmaster: show the close (x) button on terminal tabs. ON (default) keeps the theme's
        // showCloseButton policy (Always / Hover / etc.); OFF forces every tab to "Never" — hiding
        // the X entirely (the pinned Manager tab is always X-less regardless). GLOBAL across windows;
        // applied live via _updateAllTabCloseButtons on Save + cross-window broadcast. Default true
        // reproduces prior behavior (theme-driven).
        bool showTabCloseButton{ true };
        // Agentmaster: close a tab when it is clicked with the MIDDLE mouse button. ON (default) is the
        // long-standing Windows Terminal behavior; OFF disables it on BOTH paths — the manual hook used
        // when the X is hidden (TerminalPage::_OnTabPointerPressed) AND WinUI's native middle-click
        // close when the X is shown (suppressed in TerminalPage::_OnTabCloseRequested). GLOBAL across
        // windows; applied live like showTabCloseButton. Default true reproduces prior behavior.
        bool closeTabOnMiddleClick{ true };
        // Agentmaster: ALWAYS show the tab-strip "Home" button (the affordance that jumps to the pinned
        // Manager tab), not only when the Manager tab has scrolled out of view. ON (default) keeps a
        // persistent Home button in the strip header whenever you're on a non-Manager tab; OFF restores
        // the scroll-triggered behavior (Home appears only once the Manager tab is scrolled off the left
        // edge). Either way it hides while the Manager tab itself is active. GLOBAL across windows;
        // applied live on Save + cross-window broadcast. A missing key => true (checked by default).
        bool alwaysShowHomeButton{ true };
        // Agentmaster: show the profile ICON on terminal tabs — the leftmost glyph in each tab, before
        // the status dot + title. OFF (the DEFAULT here) hides it entirely so it takes NO strip space,
        // via the same IconStyle::Hidden path WT's own theme "tab.iconStyle":"hidden" uses
        // (TerminalPage::_UpdateTabIcon forces Hidden when this is off; _UpdateAllTabIcons re-applies it
        // live to every tab). GLOBAL across windows; applied live on Save + cross-window broadcast.
        // A missing key => false (HIDDEN) — deliberately NOT stock WT's default (icons shown): this app
        // hides tab icons by default so the strip reads on the status dot + title. Flip it on to restore
        // the profile icons. The pinned Manager tab follows this like any other tab.
        bool showTabIcon{ false };
        // Agentmaster (tab title naming): HOW an untitled session's default tab title derives from
        // its working directory (TabTitleNaming — LastWord default / FolderName / TwoFolders /
        // Capitals), plus the output transforms: the case applied to the result (TabTitleCase) and
        // whether any whitespace becomes '_'. Consumed by DeriveSessionTitle whenever an UNTITLED
        // session is launched / adopted / forked-from-disk — an existing (or renamed) title never
        // re-derives (Rule #11). GLOBAL; the 1-arg DeriveSessionTitle reads these fresh from disk at
        // each derive, so a cog Save applies to the very next launch in every window. Missing keys
        // => LastWord / Default / false.
        TabTitleNaming tabTitleNaming{ TabTitleNaming::LastWord };
        TabTitleCase tabTitleCase{ TabTitleCase::Default };
        bool tabTitleSpacesToUnderscores{ false };
        // Agentmaster (FAVORITES.md §5a): the FAVORITE marker glyph on a live session's tab — Crown
        // (default, a gold crown at the status dot's north-west) or Star (the status dot foregrounded
        // on a white, golden-tipped star drawn behind it). GLOBAL across windows; applied live on Save
        // + cross-window broadcast (every hosted favorited tab is re-asserted). A missing key => Crown
        // (the prior behavior). Tab-strip ONLY — see FavoriteIcon.
        FavoriteIcon favoriteIcon{ FavoriteIcon::Crown };
        // Agentmaster (tab color modes): HOW managed tabs are colored — shared per working
        // directory (default, Rule #12 classic), individual per tab/session, shared per the
        // INFERRED working directory (detected from the files the session reads/edits/creates),
        // or NoColor ("Remove colors" — no tab colored, persisted colors kept but not loaded).
        // See TabColorMode. GLOBAL across windows; applied live on Save + cross-window broadcast
        // (TerminalPage::_ReapplyManagedTabColors repaints every hosted managed tab). A missing
        // key => WorkingDirectory (the prior behavior).
        TabColorMode tabColorMode{ TabColorMode::WorkingDirectory };
        // Agentmaster (tab color modes — "Use .git folder to infer"): whether the inferred-workdir
        // detection SNAPS to the enclosing git repository root. ON (default): each tool-touched
        // path resolves to its nearest ancestor holding a `.git` entry (dir, or a worktree /
        // submodule FILE — the nearest wins, so worktree work keys the worktree, not the outer
        // checkout), and a git root carrying the strict majority of the votes IS the inferred dir
        // — never a subfolder of it. The repo is the working area: a session concentrated in
        // `repo\src\x` then infers `repo`, which normally equals its launch cwd, so the inferred
        // mode agrees with shared-per-directory coloring for in-repo sessions (switching modes
        // doesn't recolor). No git majority (or OFF) => the plain deepest-majority directory vote
        // (InferWorkingDirectory / FindGitRootForDir). Only consulted while tabColorMode is
        // InferredWorkingDirectory; a change clears the scan gate so live sessions re-infer.
        // GLOBAL; persisted "inferGitRoot"; a missing key => ON.
        bool inferGitRoot{ true };
        // Agentmaster (status-dot RED FLASH RING color): the COLOR — with OPACITY in the alpha byte —
        // of the "unread" ring that pulses around a managed session's tab status dot when it leaves
        // Running for a needs-you state (Idle / WaitingForInput / NeedsApproval) on an unvisited tab
        // (TerminalPage::_EvaluateAgentFlash), and of the manual "Mark Unread" ring. Stored as an
        // "#AARRGGBB" hex string — the leading ALPHA byte is the ring's opacity, so the Settings cog's
        // color picker (alpha slider enabled) drives BOTH hue and opacity in one control; a legacy
        // "#RRGGBB" (no alpha) is read as fully opaque. GLOBAL across windows; applied live on Save +
        // cross-window broadcast (each window re-points its shared flash-ring brush). Default
        // "#CCFF0000" == red at 80% opacity (alpha 0xCC) — a slightly softer ring than a fully-opaque
        // one. A malformed / empty value falls back to that default at the (UI-layer) parse, never
        // wedging the ring.
        std::wstring flashRingColor{ L"#CCFF0000" };

        // Agentmaster (PENDING_INPUT.md): the unsent-draft "3 dots" indicator color, kept as a CONTRAST
        // PAIR so the dots are never invisible against the tab/card they ride on. The dots are painted the
        // LIGHT color on a DARK background and the DARK color on a LIGHT one — the algorithm picks by the
        // session's per-directory tab color (Rule #12) via BackgroundIsLight / PendingDotsColorFor
        // (AgentStatusColors.h, the WCAG luminance crossover). Both "#AARRGGBB" (the alpha byte is honored,
        // atop the dots' own opacity pulse); GLOBAL across windows, applied live on Save + the cross-window
        // broadcast (the flashRingColor idiom — the tab-strip dots re-read these on the next scan tick, the
        // board cards on the next rebuild). Defaults reproduce the prior single hardcoded gold shown on a
        // dark background (light = #FFE0A92B) and add a deep amber that reads on a light one (dark =
        // #FF5A3E00). A malformed/empty value falls back to its default at the (UI-layer) parse.
        std::wstring pendingDotsLightColor{ L"#FFE0A92B" }; // shown on a DARK tab/card background
        std::wstring pendingDotsDarkColor{ L"#FF5A3E00" }; // shown on a LIGHT tab/card background

        // Agentmaster (TAB_OVERLAY.md): show the per-tab "link badge" overlay pinned to the
        // top-right of each Claude session's terminal (status + autorunner mode + queued count +
        // link state). Default ON; a missing key => true (a no-op default, like the rest).
        bool showTabOverlay{ true };
        // Agentmaster (TAB_OVERLAY.md): the per-tab overlay (badge + its summary panel) sits dim at REST
        // and brightens to HOVER opacity on pointer-over / while the copy menu is open. Both are
        // user-configurable via the cog's TABS "Overlay opacity" dual-thumb slider (rest = left/transparent
        // dot, hover = right/solid dot). GLOBAL across windows, applied live on Save + cross-window
        // broadcast. INVARIANT: rest <= hover (the slider can't cross the dots; enforced again at load).
        // Defaults reproduce the prior hardcoded look (rest 0.50, hover 1.0).
        double tabOverlayRestOpacity{ 0.50 };
        double tabOverlayHoverOpacity{ 1.0 };
        // Agentmaster (TAB_OVERLAY.md summary panel): whether the per-tab SUMMARY PANEL (the 2nd
        // overlay, toggled by the badge's pencil button) is shown. GLOBAL across windows — like
        // showTabOverlay/treeSort it lives here in settings.json, NOT per-session: the pencil on any
        // tab flips this one value (a freshest-disk read-modify-write), every linked overlay in the
        // window applies it live, and it seeds every window on launch. Default ON (panel shown).
        bool showSummaryPanel{ true };
        // Agentmaster (TAB_OVERLAY.md summary panel): the panel's SIZE, stored as FRACTIONS of the
        // pane (so it scales with the window — the archiveSplitFraction idiom). The panel is anchored
        // top-right; the user drags its LEFT edge (width), BOTTOM edge (height), or BOTTOM-LEFT corner
        // (both). 0 == "auto" — the original look (width capped at 20% of the pane, height content-
        // driven up to a modest cap); a drag pins an explicit fraction. Width is clamped to (0.08, 0.5)
        // — at most HALF the pane wide; height to (0.06, 0.75) — at most THREE-QUARTERS of the pane
        // tall. GLOBAL like showSummaryPanel: written by the resize grips (freshest-disk RMW),
        // broadcast live to every linked overlay in the window, and seeds every window on launch.
        // Default 0 (a no-op default — the pre-resize behavior).
        double summaryPanelWidthFraction{ 0.0 };
        double summaryPanelHeightFraction{ 0.0 };
        // Agentmaster (TAB_OVERLAY.md summary panel): how the panel renders newlines INSIDE a message.
        // false (default) == the session-end.js look: each message is collapsed to ONE line, real
        // newlines escaped to a literal "\n" (and tabs to "\t"). true == preserve the message's real
        // newlines so a multi-line prompt reads as multiple lines. GLOBAL like showSummaryPanel — toggled
        // by the wrap-line icon at the right of the panel's times bar (a freshest-disk RMW), broadcast
        // live to every linked overlay in the window, and seeds every window on launch.
        bool summaryPanelWrapNewlines{ false };
        // Agentmaster (TAB_OVERLAY.md summary panel): TRUNCATE long messages in the numbered list. ON
        // (default) limits each message — to 6 lines when wrap is on (a 7th+ line collapses to "..."),
        // or to 500 characters when wrap is off; OFF shows EVERYTHING. GLOBAL like summaryPanelWrapNewlines:
        // written by the truncate toggle (left of the wrap toggle) in the panel's times bar, a freshest-
        // disk RMW, broadcast live to every linked overlay. Default true (truncate long messages).
        bool summaryPanelTruncate{ true };
        // Agentmaster (conversation lineage): SHOW PREVIOUS SESSION(S) in the summary panel — when a
        // session was `/compact`ed, the earlier (pre-compaction) segments are rendered ABOVE the current
        // Messages, each numbered + labeled. GLOBAL like the wrap/truncate toggles (written by the
        // previous-session toggle in the panel's times bar, freshest-disk RMW, broadcast live). Default
        // false (hidden; the toggle only appears when the session actually has a previous segment).
        bool summaryPanelShowPrevious{ false };
        // Agentmaster (tab color picker): how the "Change tab color..." flyout OPENS — whether its
        // "Custom" panel (the muxc::ColorPicker) and that picker's own More/Less expander (the
        // "advanced" RGB/HSV/Hex text inputs) start EXPANDED. Both default TRUE, so the picker opens
        // ready to type an exact color instead of making you drill down two levels every single time.
        // GLOBAL + persisted like the summary-panel toggles: each flip is a freshest-disk RMW written
        // by the flyout itself (never the cog form — so both preserve blocks restore them on a cog
        // Save), and the flyout re-reads them on every open, so a change in one window is picked up
        // by the next open anywhere. Advanced degrades SOFT: it drives WinUI's own MoreButton
        // template part, so if a future WinUI renames it the picker simply opens collapsed as before.
        bool tabColorPickerCustomOpen{ true };
        bool tabColorPickerAdvancedOpen{ true };
        // Agentmaster: Explorer Tree sort order (the toggle after the scope toggle). GLOBAL — it
        // applies to every window's tree and persists here. Default Newest. See ExplorerSort.
        ExplorerSort treeSort{ ExplorerSort::Newest };
        // Agentmaster: Triage Board sort order (the toggle after the board's LOCAL/GLOBAL scope toggle)
        // — orders the cards WITHIN each state column. A SEPARATE global setting from treeSort, so the
        // board and the tree sort independently and each remembers its own choice. GLOBAL like treeSort:
        // shared by every window, persisted here, survives restart (the changing window re-sorts live;
        // others adopt it on next launch). Default MostActive == most-recently-active first. The board
        // cycles MostActive/Newest/Oldest/Alpha — the tree's set minus ByPid (pid grouping is meaningless
        // once cards are split across state columns).
        ExplorerSort boardSort{ ExplorerSort::MostActive };
        // Agentmaster: which tab the Manager's FLIGHT-PLAN pane shows. Its top line is a two-state
        // [Summary | Auto Testing] segmented toggle; true == the (currently empty) Summary tab is
        // selected, false == the Auto Testing tab (the prompt queue / Autorunner / compose box). GLOBAL
        // across windows like treeSort/boardSort: a click in any window persists it here + broadcasts,
        // so every window's pane shows the same tab and the choice survives restart. Default true
        // (Summary). NOTE: distinct from showSummaryPanel — that is the per-tab OVERLAY's pencil-toggled
        // summary panel (TAB_OVERLAY.md); this is the Manager Auto-Testing pane's tab selection.
        bool autoTestingShowsSummary{ true };
        // Agentmaster: the Archive page's table|detail splitter position — the TABLE's share of
        // the two columns, kept within (0.05, 0.95). Applied as STAR ratios, so the split scales
        // with the window (window-size-relative, not pixels). GLOBAL like treeSort: written by
        // the splitter itself on drag release (read-modify-write of settings.json, not the cog),
        // seeded into every window's Archive shell. Default 0.5 == the original 50/50 split.
        double archiveSplitFraction{ 0.5 };
        // Agentmaster (updater; Updater.h): the in-app GitHub-release updater's state. allowUpdatePrerelease
        // is the Settings cog's "Allow updating to pre-release versions" toggle (default OFF — the check
        // uses /releases/latest, which excludes prereleases; ON uses the list endpoint, newest published).
        // allowUpdateNightly is the NIGHTLY opt-in (default OFF): a nightly is an unstable DEVELOPMENT
        // build whose tag CONTAINS "nightly" (e.g. v0.6.10-prerelease-nightly; published as a GitHub
        // prerelease) — a tier BELOW pre-release, ALWAYS skipped by every check (even with the
        // pre-release toggle on) unless this is set. The cog gates turning it ON behind an explicit
        // warning confirm (memory leaks / CPU issues / crashes — Updater.h IsNightlyTag /
        // ReleaseAllowedOnChannel); the two opt-ins are ORTHOGONAL (each admits only its own tier).
        // ALL FOUR are written OUTSIDE the cog form — the two opt-ins by each switch's own
        // INSTANT-APPLY freshest-disk RMW (flipping persists immediately, no Save), skip/postpone by the
        // updater prompt's JSON RMW (which the WindowsTerminal EXE can do without linking the engine — see
        // Updater.h; it writes INSIDE the {version, settings:{...}} envelope, where these fields round-trip)
        // — so the cog's Save PRESERVES all four from disk like the summary-panel fields:
        // skip == the exact tag the user chose to "Skip this version" (never re-prompt for it); postpone ==
        // the epoch-ms before which no check/prompt fires ("Remind me in 3/7/30 days"). All four default to
        // a no-op (stable-only check, prompt normally, nothing skipped, not postponed) so a missing
        // settings.json changes nothing.
        bool allowUpdatePrerelease{ false };
        bool allowUpdateNightly{ false };
        std::wstring updateSkippedVersion{};
        int64_t updatePostponedUntilUnixMs{ 0 };
        // Agentmaster (debug escape hatch; ProfileBootstrap.h IsDebugPackage): the Settings cog's
        // About-tab "Enable Debug Mode" toggle. The DURABLE, in-UI twin of the --debug /
        // AGENTMASTER_DEBUG launch flag — when ON it unlocks the DEV-only Auto Testing / Tests
        // Autorunner subsystem (the autorunner, the pane toggle, the board queue badge, the cog's
        // Tests Autorunner tab, the per-tab autorunner control) in a RELEASE install. Read ONCE at
        // startup (WindowEmperor -> Profiles::ApplyPersistedDebugMode -> a process-local one-way
        // override that IsDebugPackage() consults), so it takes effect on the NEXT start — exactly
        // like handing --debug to a fresh launch. Default OFF; a missing key => the normal
        // release behaviour (Auto Testing hidden + inert). The FORM owns this field.
        bool debugMode{ false };
        // Agentmaster (Sessions page; SESSIONS.md): the session ids the user chose to HIDE from the
        // global Sessions browser ("Hide from list" on a row's right-click menu). Persisted here so
        // a hide sticks across restarts; cleared from the Settings cog's "Reset hidden sessions"
        // (both paths a freshest-disk read-modify-write of settings.json). The Sessions page filters
        // these out of its table; nothing else reads the list — it is purely a browse-list
        // preference, never a lifecycle action (a hidden session is untouched on disk).
        std::vector<std::wstring> hiddenSessionIds{};

        // --- System notifications (the Settings cog's "Notifications" tab) ---
        // Agentmaster: raise a WINDOWS TOAST when a managed session's status leaves Running for
        // another state (the "your agent finished / needs you" cue). The toast reads:
        //     <session title>
        //     Has completed after <2h30m> and is <status>
        //     "<the truncated prompt whose turn just finished>"
        // ("after <duration>" is the Running span — how long the turn worked; omitted when the
        // Running entry wasn't observed, e.g. a session adopted mid-turn. The third line is
        // LastDeliveredPrompt through PromptPreviewLine — which request came back — and is omitted
        // entirely when the session has no delivered prompt, e.g. a managed Codex.) Fired by the ONE window
        // hosting the session's tab (TerminalPage::_EvaluateAgentNotification, riding the same
        // registry-observer push as the tab status dot), so exactly one toast per transition
        // regardless of how many windows are open. Clicking the toast jumps to the session's tab
        // (while the app is running). Master switch — default ON; a missing key => ON.
        bool notificationsEnabled{ true };
        // Which TARGET states notify (the transition is always FROM Running). All default ON ==
        // the "Running to anything else" default rule; uncheck a state in the cog to mute that
        // transition. A missing key => ON (the default rule).
        bool notifyOnWaiting{ true }; // Running -> WaitingForInput ("waiting for you")
        bool notifyOnNeedsApproval{ true }; // Running -> NeedsApproval ("needs approval")
        bool notifyOnIdle{ true }; // Running -> Idle
        bool notifyOnDone{ true }; // Running -> Done
        bool notifyOnError{ true }; // Running -> Error
        // Skip the toast when the session's tab is the FOCUSED tab of the ACTIVE window — you're
        // already looking at it (the flash ring's "the current tab is always considered visited"
        // rule, applied to toasts). Default ON; a missing key => ON.
        bool notifySuppressFocused{ true };
        // Play the Windows notification sound with the toast; OFF adds <audio silent="true"/> so
        // the toast is visual-only. Default ON; a missing key => ON.
        bool notifySound{ true };

        // --- Slash commands (COMMANDS.md §6a; the cog's "Commands" tab) ---
        // Agentmaster's /handover command family is user-customizable: each command can be RENAMED
        // (the bare word the user types after '/' — the shipped definition then materializes as
        // <name>.md and the CommandWatch binding keys on that name) and DISABLED (no definition
        // file materialized, no binding registered — the command simply doesn't exist). BOTH apply
        // at the NEXT START only: the definition files + the watch bindings are set up once at
        // engine init, deliberately never re-bound mid-run (the scanner worker reads the binding
        // set unsynchronized — engine-init registration is the happens-before). Names are stored
        // NORMALIZED (NormalizeCommandName: lowercase ASCII slug [a-z0-9-_], '/'-stripped, <=64
        // chars) and COLLISION-HEALED (ResolveCommandNameTriple — the watch's binding lookup is
        // name-exact, so no two commands can ever share a name). Missing keys reproduce the
        // shipped behavior exactly: /handover + /handover-here + /handover-standby, all enabled.
        std::wstring commandHandoverName{ L"handover" };
        bool commandHandoverEnabled{ true };
        std::wstring commandHandoverHereName{ L"handover-here" };
        bool commandHandoverHereEnabled{ true };
        // The STANDBY member (COMMANDS.md §5b): like /handover, but each successor's briefing is
        // TYPED into its Claude input box WITHOUT being sent — the user reviews the pre-filled
        // message and presses Enter themselves ("a handover in standby").
        std::wstring commandHandoverStandbyName{ L"handover-standby" };
        bool commandHandoverStandbyEnabled{ true };
        // ENGINE-owned markers (NOT shown in the cog; the envDefaultsVersion discipline): the name
        // whose definition file the LAST engine init actually materialized for each command —
        // "" == none (the command was disabled, or the write failed). The init reconcile compares
        // marker vs configured name to know WHICH old file a rename/disable must migrate away
        // (deleting it ONLY when it is byte-identical to something we shipped — a user-edited file
        // is never touched), then RMWs the marker to the new reality. Defaults to the DEFAULT
        // names, because a pre-feature install has the default-named files on disk with no marker
        // — so its first rename knows exactly what to clean up. The cog Save PRESERVES these from
        // disk (both preserve blocks), like every other out-of-form field. (For STANDBY the
        // default-name default is equally safe on an install predating the command: the reconcile's
        // migration removes a prior file only when it digest-matches something we shipped, so a
        // marker naming a file that never existed is a no-op remove followed by the create.)
        std::wstring commandHandoverMaterializedName{ L"handover" };
        std::wstring commandHandoverHereMaterializedName{ L"handover-here" };
        std::wstring commandHandoverStandbyMaterializedName{ L"handover-standby" };
        // SUCCESSOR SHAPING (COMMANDS.md §6b — the Commands tab's second half). Unlike the
        // names/enables above, most of these are consumed at ACTION time (_HandleCommandHandover
        // reads the live _appSettings when a handover fires), so they apply to the NEXT handover
        // immediately — no restart. The ONE exception is the file-match regex (binding-time,
        // restart-applied — noted on its field).
        //   * commandHandoverSuccessorModel / commandHandoverHereSuccessorModel /
        //     commandHandoverStandbySuccessorModel — the model the command's successors LAUNCH
        //     with, per command: "" == Default (the settings `model` decides, the shipped
        //     behavior), else a model id passed as this launch's `--model <id>` (the launch-model
        //     picker's per-launch override, reused verbatim — BuildClaudeCommandline's
        //     modelOverride). The cog offers "Default" + the launchModels list; a stored id no
        //     longer in that list still round-trips (shown as custom).
        std::wstring commandHandoverSuccessorModel{};
        std::wstring commandHandoverHereSuccessorModel{};
        std::wstring commandHandoverStandbySuccessorModel{};
        //   * commandHandoverTitleFindRegex / commandHandoverTitleReplace — ONE find/replace pair
        //     for the WHOLE family: when the find regex is non-empty, VALID (RegexUtil.h), and
        //     MATCHES the origin title, the successor's title = regex_replace(originTitle, find,
        //     replace) ($1 backrefs honored, every occurrence replaced) — still uniqueness-bumped
        //     past registry titles. SEEDED with the shipped defaults above (which reproduce the
        //     classic "<origin> (handover)" naming, chain-bumping included), so the rule is
        //     VISIBLE and editable rather than hidden in code; CLEARING either box falls back to
        //     the built-in DeriveSuffixedTitle suffixer, as do an invalid pattern, a pattern
        //     matching nowhere, and a blank result (the rewrite can never lose a title — Rule
        //     #11's never-empty invariant holds). Stored VERBATIM (a regex is freeform; the cog
        //     validates live, the consumers guard). Presence-gated on load like launchModels.
        std::wstring commandHandoverTitleFindRegex{ kDefaultCommandTitleFindRegex };
        std::wstring commandHandoverTitleReplace{ kDefaultCommandTitleReplace };
        //   * commandHandoverFileMatchRegex — the markdown await's FILE-MATCH pattern, shared by
        //     BOTH commands (they are one await family — same files, one owner; a per-command
        //     pattern would split the §3 supersede family). SEEDED with the shipped default
        //     (kDefaultCommandFileMatchRegex — `HANDOVER\-`, the regex spelling of the definitions'
        //     own `HANDOVER-<topic>.md` naming contract), so the rule is visible and editable; a
        //     leaf qualifies when the regex SEARCHES its file name (case-insensitive; anchor with
        //     ^/$ for a full-name match). CLEARING the box falls back to the built-in
        //     contains-"handover" leaf hint (LOOSER than this default), and an INVALID
        //     pattern does too (a broken pattern must not silently kill handovers — the cog warns
        //     live). A pattern DIFFERING from the shipped default also suppresses the legacy
        //     first-markdown fallback (it is a statement of intent — see CommandWatch).
        //     RESTART-APPLIED: the pattern rides the CommandWatch binding registered at engine
        //     init. NOTE: this gates what Agentmaster COLLECTS — the shipped definitions still
        //     instruct Claude to write HANDOVER-<topic>.md, so a custom pattern usually pairs
        //     with an edited definition.
        std::wstring commandHandoverFileMatchRegex{ kDefaultCommandFileMatchRegex };
        //   * commandHandoverDeleteFileAfterLaunch — delete a HANDOVER markdown after its
        //     successor is SUCCESSFULLY created and the delivery is SECURED: the content tier has
        //     the document on the successor's launch commandline, the paste tier has it parked
        //     DURABLY at the front of the successor's queue (sessions.json — restart-safe,
        //     Send-now-able) — in both the file is no longer load-bearing. The POINTER tier never
        //     deletes (the successor must read the file), and a failed spawn leaves its file.
        //     Default OFF — NEVER DELETE unless you asked for it: a briefing is a document the user
        //     may still want to read, and losing one is unrecoverable, so keeping it is the safe
        //     direction on EVERY install (a fresh one included — the earlier "fresh installs seed
        //     it ON because the shipped location is the scratchpad" pairing is gone, and the cog's
        //     Scratchpad preset now turns delete-after OFF rather than on).
        bool commandHandoverDeleteFileAfterLaunch{ false };
        //   * commandHandoverDeleteDeadlineMinutes — how long delete-after WAITS for the successor
        //     to actually START before giving up and KEEPING the file (the delete itself fires the
        //     moment the successor is up, so a prompt hand-off is unaffected by this value). It
        //     matters because a successor opened in a BACKGROUND tab starts LAZILY — WT builds the
        //     control on first layout, so its claude.exe may not run until the user visits the tab,
        //     which can be hours. Default 1440 (24 h): the briefing is cleaned up whenever you get
        //     around to opening it, and a successor never opened keeps its file forever. 0 == don't
        //     wait at all (delete only if the successor is already up on the next sweep). Clamped
        //     by ClampCommandHandoverDeleteDeadlineMinutes on load + Save.
        uint32_t commandHandoverDeleteDeadlineMinutes{ 1440 };
        //   * commandHandoverWritePath — WHERE both commands tell Claude to write the briefing
        //     (COMMANDS.md §6c). FAMILY-WIDE and FOLDER-ONLY: the file NAME keeps the
        //     HANDOVER-<topic>.md contract the rest of the pipeline keys on (the file-match regex,
        //     the one-successor-per-file fan-out, delete-after) — only the DIRECTORY moves, so no
        //     other stage changes. Values: kCommandWritePathScratchpad (the default) or "" == the
        //     session scratchpad; "./" == the working directory (the pre-§6c behavior); anything
        //     else is a folder, relative to the working directory or absolute. Consumed at
        //     MATERIALIZE time — it is rendered into the definition text — and the cog
        //     re-materializes on Save, so a change applies to the NEXT handover with no restart.
        std::wstring commandHandoverWritePath{ kCommandWritePathScratchpad };

        // --- shipped-default seeding markers (ENV_VARS.md §8; NOT shown in the cog) ---
        // Agentmaster ships a few defaults ONCE and then respects user edits/removals. These markers
        // record that the one-time seed ran, so a default a user deletes never returns:
        //  * envDefaultsVersion — the highest kEnvDefaultsVersion already seeded into `env`. 0 == none yet
        //    (a new install OR a pre-feature settings.json), so the seed runs once and bumps it. See
        //    ApplyEnvDefaults / SeedSessionEnvDefaults. v1 seeds CLAUDE_CODE_MAX_RETRIES=50000.
        //  * claudeCleanupDaysSeeded — whether we've already written the default cleanupPeriodDays=36500
        //    into the user's GLOBAL ~/.claude/settings.json (so Claude never purges global history). Only
        //    seeded when the user hasn't set it themselves; never re-seeded after they change/remove it.
        //    See SeedClaudeCleanupPeriodDaysIfNeeded. (cleanupPeriodDays itself lives in Claude's file, not
        //    here — it's editable from the cog's "Keep Claude history (days)" field via the repository.)
        uint32_t envDefaultsVersion{ 0 };
        bool claudeCleanupDaysSeeded{ false };
    };

    // Agentmaster (bookmark tags): clamp AppSettings::maxTags into its valid band — the cog allows
    // 1..40 (hard ceiling 40); 0/absent/garbage falls back to the 20 default. Shared by the
    // Persistence load and the cog Save so a hand-edited settings.json self-heals identically.
    inline uint32_t ClampMaxTags(uint32_t v)
    {
        if (v < 1)
        {
            return 20;
        }
        return v > 40 ? 40 : v;
    }

    // Agentmaster (COMMANDS.md §6b): clamp AppSettings::commandHandoverDeleteDeadlineMinutes into
    // 0..30 days. 0 is MEANINGFUL here (don't wait for the successor at all), so — unlike ClampMaxTags
    // — it is NOT folded back to the default; only a value past the ceiling is pinned. The ceiling
    // exists so a hand-edited settings.json can't park an entry in the in-memory sweep map forever.
    // Shared by the Persistence load and the cog Save so both self-heal identically.
    inline uint32_t ClampCommandHandoverDeleteDeadlineMinutes(uint32_t v)
    {
        constexpr uint32_t kMax = 30u * 24u * 60u; // 30 days
        return v > kMax ? kMax : v;
    }

    // Agentmaster (bookmark tags): clamp AppSettings::tooltipTagsOpacity into 0.1..1.0 — the cog
    // slider's 10..100% band. A non-finite / <=0 value (absent key, hand-edit) falls back to the
    // 0.9 default; anything above 1 pins to fully solid. Shared by the Persistence load and the cog
    // Save so a hand-edited settings.json self-heals identically (the ClampMaxTags idiom).
    inline double ClampTooltipTagsOpacity(double v)
    {
        if (!(v > 0.0)) // NaN or <= 0 -> the default
        {
            return 0.9;
        }
        if (v < 0.1)
        {
            return 0.1;
        }
        return v > 1.0 ? 1.0 : v;
    }


    // Agentmaster (COMMANDS.md §6a): normalize a user-typed command name into the form everything
    // downstream agrees on — the transcript echo parser lowercases ASCII (ParseCommandEcho), the
    // CommandWatch binding lookup is exact, and the name becomes a file LEAF (<name>.md under
    // <claude-config>\commands) AND a token substituted into the definition text ("/<name>") — so
    // the alphabet is a strict ASCII slug: [a-z0-9-_] only, everything else dropped (spaces, dots,
    // path separators, unicode — a hand-edited settings.json can never smuggle "..\" into the
    // commands-dir file operations). A leading '/' (the user typed "/foo") and leading whitespace
    // are tolerated and stripped; length is capped at 64 (Claude Code names are short slugs).
    // Empty in => empty out (the caller decides the fallback — ResolveCommandNamePair).
    inline std::wstring NormalizeCommandName(std::wstring_view raw)
    {
        std::wstring out;
        out.reserve(raw.size() < 64 ? raw.size() : 64);
        bool leading = true;
        for (wchar_t c : raw)
        {
            if (leading && (c == L'/' || c == L' ' || c == L'\t'))
            {
                continue; // tolerate a typed "/name" / pasted leading whitespace
            }
            leading = false;
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
            if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')
            {
                out.push_back(c);
                if (out.size() >= 64)
                {
                    break;
                }
            }
            // anything else is dropped (never a placeholder char — the result stays a clean slug)
        }
        return out;
    }

    // Agentmaster (COMMANDS.md §6c): normalize a user-typed handover WRITE PATH into the form the
    // definition renderer may safely inline. The value is rendered into ONE line of a markdown
    // instruction file (backtick-quoted), so anything that could break that line out — control
    // characters, a newline, the backtick/quote/pipe glyphs — is DROPPED rather than escaped
    // (a folder name containing them is not worth supporting; the pipe is also the fan-out
    // separator). Surrounding whitespace and one pasted quote/backtick pair are trimmed, a
    // trailing separator is dropped (so `./docs\` and `./docs` are the same folder) EXCEPT on the
    // degenerate roots ("./", "/", "C:\"), and the length is capped well under MAX_PATH's tail.
    // Empty in => empty out; the caller reads "" as the shipped default (the scratchpad).
    inline std::wstring NormalizeCommandWritePath(std::wstring_view raw)
    {
        std::wstring out;
        out.reserve(raw.size());
        for (const wchar_t c : raw)
        {
            if (c < 0x20 || c == 0x7F || c == L'"' || c == L'`' || c == L'|' || c == L'<' || c == L'>')
            {
                continue;
            }
            out.push_back(c);
            if (out.size() >= 240)
            {
                break;
            }
        }
        const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t'; };
        size_t b = 0;
        size_t e = out.size();
        while (b < e && isSpace(out[b]))
        {
            ++b;
        }
        while (e > b && isSpace(out[e - 1]))
        {
            --e;
        }
        out = out.substr(b, e - b);
        // A trailing separator is noise ("./docs\" == "./docs"), but "./", "/", "\" and "C:\" ARE
        // the value — never strip them down to something that means a different folder.
        if (out.size() > 2 && (out.back() == L'\\' || out.back() == L'/') &&
            !(out.size() == 3 && out[1] == L':'))
        {
            out.pop_back();
        }
        return out;
    }

    // Agentmaster (COMMANDS.md §6c): does this configured write path mean "the session scratchpad"
    // (the shipped default)? "" (a cleared box) and the token are the same answer, case-insensitively
    // for the token so a typed "Scratchpad" is understood.
    inline bool CommandWritePathIsScratchpad(std::wstring_view writePath)
    {
        const std::wstring v = NormalizeCommandWritePath(writePath);
        if (v.empty())
        {
            return true;
        }
        if (v.size() != kCommandWritePathScratchpad.size())
        {
            return false;
        }
        for (size_t i = 0; i < v.size(); ++i)
        {
            wchar_t c = v[i];
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
            if (c != kCommandWritePathScratchpad[i])
            {
                return false;
            }
        }
        return true;
    }

    // Agentmaster (COMMANDS.md §6a): resolve the CONFIGURED /handover-family names into the set
    // actually USED — normalize each, fall back to its default on empty, and COLLISION-HEAL: the
    // CommandWatch binding lookup is name-EXACT, so two bindings under one name would make the
    // second silently shadow the first (BindMarkdownAwait is last-wins), and two definition files
    // cannot share one <name>.md leaf. Deterministic rule, priority handover > here > standby: on
    // a collision the LATER command falls back to ITS default; if it is ALREADY on its default
    // (the earlier command squats that name — e.g. /handover named "handover-standby"), the
    // EARLIER one is evicted to ITS OWN default instead. Every heal move lands a name on its own
    // default and the three defaults are pairwise distinct, so the loop reaches a collision-free
    // fixpoint in <= 3 moves (the guard is a belt, not a real bound) and the healed set is always
    // three distinct, non-empty slugs. Shared by the Persistence load (a hand-edited settings.json
    // self-heals) and the cog Save (typed input heals identically).
    inline void ResolveCommandNameTriple(std::wstring& handoverName, std::wstring& handoverHereName, std::wstring& handoverStandbyName)
    {
        handoverName = NormalizeCommandName(handoverName);
        handoverHereName = NormalizeCommandName(handoverHereName);
        handoverStandbyName = NormalizeCommandName(handoverStandbyName);
        if (handoverName.empty())
        {
            handoverName = kDefaultHandoverCommandName;
        }
        if (handoverHereName.empty())
        {
            handoverHereName = kDefaultHandoverHereCommandName;
        }
        if (handoverStandbyName.empty())
        {
            handoverStandbyName = kDefaultHandoverStandbyCommandName;
        }
        std::wstring* const names[3] = { &handoverName, &handoverHereName, &handoverStandbyName };
        const std::wstring_view defaults[3] = { kDefaultHandoverCommandName, kDefaultHandoverHereCommandName, kDefaultHandoverStandbyCommandName };
        for (int guard = 0; guard < 8; ++guard)
        {
            bool changed = false;
            for (int i = 0; i < 3 && !changed; ++i)
            {
                for (int j = i + 1; j < 3 && !changed; ++j)
                {
                    if (*names[i] == *names[j])
                    {
                        if (*names[j] != defaults[j])
                        {
                            *names[j] = defaults[j]; // the later command yields to its own default
                        }
                        else
                        {
                            *names[i] = defaults[i]; // the earlier one squatted that default — evict it
                        }
                        changed = true;
                    }
                }
            }
            if (!changed)
            {
                break;
            }
        }
    }

    // ===== Workspace persistence (M10; see doc/agentmaster/PERSISTENCE.md) =====
    // The WindowEmperor runs every window in one process, so window-level UI state is
    // per-WINDOW: each window owns a WindowRecord — geometry + Manager-tab lens + ORDERED tab
    // refs — stored as windows/<windowId>.json. This is a THIN layer OVER the session-archive
    // model: sessions.json + SessionInfo.live stay the single source of truth for the fleet
    // (lifecycle, queues, open/archived). The WindowRecord never duplicates session data — a
    // Claude tab here is just the session's id (tab order + window<->session affinity) — so a
    // window re-applies its geometry/lens and re-homes its sessions without a second copy.

    // A window's position/size/launch-mode, captured from WT's WindowLayout and re-applied by
    // us on restore. The `has*` flags distinguish "unset" from a real 0 (e.g. a window at the
    // top-left corner). launchMode is WT's LaunchMode serialized as its JSON token
    // ("default"/"maximized"/"focus"/...); empty => default.
    struct WindowGeometry
    {
        bool hasPosition{ false };
        double x{ 0 };
        double y{ 0 };
        bool hasSize{ false };
        double width{ 0 };
        double height{ 0 };
        std::wstring launchMode;
    };

    enum class TabKind
    {
        Claude, // a Manager-owned claude.exe session — restored via `claude --resume <convId>`
        Codex, // a Manager-owned codex.exe session — restored via `codex resume <rolloutUuid>` (the uuid lives on the referenced SessionInfo.codexSessionId; sessionId here is our durable handle)
        Other, // any other WT tab — restored by replaying its stored ActionAndArgs JSON
    };

    // One ordered tab inside a window — a REFERENCE, not a copy. A Claude tab stores only the
    // session's id (its full record — queue, autorunner, live/archived — lives once in
    // sessions.json); `tabColor` is the optional "#RRGGBB" we paint it. An Other tab is opaque:
    // `actionsJson` is the WT ActionAndArgs (NewTab + SetTabColor + RenameTab) that recreates
    // it, so its own color/title ride along inside that blob. This records tab ORDER + the
    // window<->session affinity without duplicating any session data.
    struct TabEntry
    {
        TabKind kind{ TabKind::Claude };
        std::wstring sessionId; // valid when kind == Claude — references the session in sessions.json
        std::wstring tabColor; // optional "#RRGGBB" (Claude tabs); empty => none
        std::wstring actionsJson; // valid when kind == Other (opaque WT ActionAndArgs)
    };

    // A window's Manager-tab lens — the per-window VIEW over the one shared fleet (selection,
    // directory scope, which prompt is selected, which dir rows are collapsed, splitter sizes).
    // These are view preferences; the fleet itself lives in the shared SessionRegistry.
    struct ManagerState
    {
        std::wstring selectedId; // selected session card
        std::wstring scopeDir; // Explorer-tree directory scope ("" => all)
        std::wstring selectedPromptId; // selected Auto-Testing row
        std::vector<std::wstring> collapsedDirs; // Explorer-tree dirs the user collapsed
        ManagerLayout layout{}; // splitter fractions (was the global layout.json; now per-window)
        // Agentmaster: the ONE session scope behind BOTH toggles — the Explorer Tree's 3-way cycle
        // (LOCAL/GLOBAL/EXTERNAL) and the Triage Board's 2-way LOCAL/GLOBAL (the board has no
        // External mode; it reads External as Global). 0=LOCAL 1=GLOBAL 2=EXTERNAL. Persisted per
        // window in the lens so a reopened window keeps its scope; absent in an older record => 0
        // (LOCAL, the prior in-memory default).
        int treeScope{ 0 };
        // Agentmaster (bookmark tags): the Triage Board's TAG FILTER — the tags whose chips are
        // picked in the board header, display-cased, in click order. Empty (the default) == no tag
        // filter, every card shows. Semantics are **OR**, deliberately unlike the Sessions browser's
        // AND: the board is a triage surface you narrow to a few concerns at once ("show me anything
        // tagged release or hotfix"), where ANDing tags would only ever shrink toward the one card
        // carrying every tag. Matching is case-insensitive (FoldTagName), like everywhere else tags
        // are compared. Persisted per window in the lens, so a reopened window comes back to the same
        // narrowed board; absent in an older record => empty (no filter).
        std::vector<std::wstring> boardTagFilter;
        // Agentmaster (bookmark tags): the board's "Untagged" chip — does the board show the cards
        // carrying NO tag at all? It is the (N+1)th bucket beside the tag chips, and it is ON by
        // default, which is what keeps an untouched board showing everything. It does NOT participate
        // in boardTagFilter's OR: a tagged session is judged by the picks, an untagged one by this
        // flag, so turning it off is how you say "only tagged cards" (see _BoardTagFilterAccepts).
        // Absent in an older record => true (show them), i.e. the pre-feature behavior.
        bool boardShowUntagged{ true };
    };

    // Per-window UI state: geometry + lens + ordered tab refs. One file per window
    // (windows/<windowId>.json) so opening/closing windows never contend on a single document.
    // NOT the session source of truth — that is sessions.json (the archive model).
    struct WindowRecord
    {
        std::wstring windowId; // stable GUID, generated once per window and embedded in its Manager tab
        WindowGeometry geometry;
        std::vector<TabEntry> tabs; // ORDERED (left-to-right)
        // The focused tab, persisted by STABLE IDENTITY so it survives a restart (positions/indices
        // shift between runs — a session id does not). A Claude tab => selectedSessionId (the
        // conversation id, the same stable ref sessions.json uses). A non-Claude (shell) tab has no
        // cross-restart id (its WT session GUID is regenerated each run), so it falls back to
        // selectedTabIndex (an index into `tabs`). Manager / none => both unset (empty / -1); the
        // Manager tab is re-created at index 0, the natural default. On reopen the window re-selects this
        // tab so closing/reopening preserves the ACTIVE tab, not just the set of tabs.
        std::wstring selectedSessionId; // selected Claude tab's conversation id (stable); empty otherwise
        int selectedTabIndex{ -1 }; // fallback for a non-Claude selected tab: index into `tabs`; -1 = Manager/none
        ManagerState manager;
        // Agentmaster: the pinned Manager tab's user-chosen color ("#RRGGBB"; empty => none/default),
        // persisted PER WINDOW. Unlike a session tab's color (ONE value per working directory, Rule #12,
        // owned by dir-colors.json), the Manager tab is a per-window singleton with no dir — so its color
        // rides here in the window record, letting each Agentmaster window's home tab be color-coded
        // independently. Empty/absent (an older record) => no override (the default tab look).
        std::wstring managerTabColor;
    };
}
