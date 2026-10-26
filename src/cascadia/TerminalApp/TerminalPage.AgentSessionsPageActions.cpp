// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation (7 partial files)
// The TerminalPage-side Agentmaster glue -- engine wiring + session lifecycle + observer +
// window-record + the Sessions browser -- connecting the Manager engine to WT's TerminalPage.
// Same class (TerminalPage, declared in TerminalPage.h), split across same-class partial TUs (the
// upstream TabManagement.cpp pattern) so TerminalPage.cpp stays close to upstream.
//
// Partial files in this group (★ marks THIS file):
//   TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Agent Manager page (+ its Home button), _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
//   TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
// ★ TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster Sessions browser: the row ACTIONS -- resume/fork-from-disk + the resume/fork prompt, keyboard selection nav, hide/unhide/reset, favorite toggle, in-place title rename, the Filter-by submenu state, and the shared full-window-page overlay registry. Partial TU of TerminalPage.AgentSessionsPage.cpp.
// Agentmaster — the full-window Sessions page (SESSIONS.md): a browser over EVERY on-disk
// Claude Code session in a selectable time window (default 1 month), opened by the Manager's
// "Sessions" button (right after Archived). Duplicates the Archive page's structure with a
// SEARCH BAR at the top: [ search for sessions ] (👤)(🤖)(📁)(📄)(🏷)(F) [☐ Open] [1 month] —
//   👤 = also search user (typed) messages      🤖 = also search agent + tools text
//   📁 = match directories accessed             📄 = match files accessed
//   🏷 = match the session title (incl. an open session's live tab title)
//   (F) = fuzzy   ·   ☐ Open = show only sessions open in a window right now (a ROW filter)
//   With 🏷 off and both message scopes off, terms match the working directory (+ 📁/📄) only (§1a).
//   Defaults: 📁+📄+🏷 ON (fast-phase-only — in-memory, no IO); 👤/🤖/(F)/Open OFF (either message
//   scope flips on the SLOW rg+transcript content scan; fuzzy is a noisy default).
//   [1 month] cycles 1d/3d/7d/14d/1mo/3mo on click; HOVER opens a From/To range popup (Q4).
// Query grammar (ParseSessionQuery, SessionSearch.h): whitespace-split terms AND-match;
// "quoted phrase" = exact contiguous match ((F) never applies inside quotes); a bare whole
// session-id GUID matches that session + its forks by IDENTITY (paste from hooks.log works).
// Backed by the TranscriptStore sidecar index (~/.agentmaster/sessions-index/<sid>.json,
// (size,mtime)-invalidated, incrementally accumulated) and the two-phase SessionSearch:
// FAST = in-memory over the index + the history.jsonl accelerator; SLOW = rg-prefiltered
// transcript content, scope-attributed in-process, generation-cancelled on re-type.
// Rows are enriched from the registry (an OPEN session gets its per-dir color chip + Jump)
// and the observer's presence table (claude's own busy/idle/waiting heartbeat).
// XAML-Islands hard rule throughout (the Archive page's discipline): every pointer handler
// DEFERS its visual-tree mutation to a clean dispatcher tick.
//
// This file implements TerminalPage methods (same class, separate TU — the TabManagement.cpp
// pattern).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the shared tooltip-dismissal recipe
#include "AgentStatusColors.h" // ResolveTagDisplayColor — the per-tag bookmark-ribbon color (the tag filter chips' leading icon)
#include "AgentMaster/ClaudeSpawn.h" // ClaudeProjectsDir / AppendStateLog
#include "AgentMaster/Engine.h" // EnsureClaudeAvailable (native-exe-only launch gate)
#include "AgentMaster/Persistence.h" // GetDirColor / AutoDirColorHex (the per-dir color chip)
#include "AgentMaster/ProcessInspect.h" // AnalyzeSessionTranscript / RenderSessionSummaryBox / FindPlanFileInTranscript / kSummarySepMark (detail summary box)
#include "AgentMaster/ProcessObserver.h" // Presence()
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionSearch.h" // the two-phase search
#include "AgentMaster/SessionStore.h" // durable per-session store (LoadAllStoredSessionTitles — the title overlay)
#include "AgentMaster/TranscriptStore.h" // EnumerateTranscripts / LoadOrRefreshSessionIndex / PickDisplayTitle
#include "AgentMaster/SessionFolderFilter.h" // Agentmaster (SESSIONS.md §10): the folder filter's pure half — the folder universe, the two orders, the find box, the OR predicate, GitRootMemo

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace std::chrono_literals;
#include "TerminalPage.AgentSessionsPage.Internal.h" // the shared Sess* file-local helpers

namespace winrt::TerminalApp::implementation
{
    // Resume ANY on-disk session into a managed tab: live here -> Jump; known-archived -> the
    // normal Restore seam; unknown to the registry -> upsert a minimal ARCHIVED record first,
    // then the SAME transcript-gated resume seam (claude --resume; fresh if the transcript
    // vanished). This reuses every existing guarantee: title pinning, per-dir tab color, hooks
    // correlation by the same id, Rule #6's gating. Resumes EXACTLY the clicked id — no continuation-tail
    // redirect (timing-based, unsound — see the `const target = sessionId` note below).
    // insertPosition (SESSIONS.md §9.14): the 0-based strip slot for the new tab; -1 = the end (Terminal's newTabPosition).
    // The AI search's resume tool passes the slot right after the tab the model chose. Returns the session whose tab is
    // open now (the target; the FRESH id a resume without a transcript degraded to; the target itself when it was live
    // already), or "" when nothing opened.
    std::wstring TerminalPage::_ResumeSessionFromDisk(const std::wstring& sessionId, const std::wstring& dir, const std::wstring& title, uint32_t insertPosition)
    {
        if (!_sessionRegistry || sessionId.empty() || dir.empty())
        {
            return {};
        }
        // Nav audit: the user picked this ROW to resume (right-click "Resume here", the detail-pane
        // button, or a double-click prompt). The clicked id is what launches; the downstream
        // [resume]/[restore-fresh] carry what actually opened (a vanished transcript degrades to fresh).
        ::Agentmaster::LogNav(L"sessions resume-click row=" + ::Agentmaster::ShortId(sessionId) + L" \"" + title.substr(0, 80) + L"\" dir=" + dir + (_openClaudeTabInBackground ? L" [bg]" : L""));
        // Resume EXACTLY the picked session — no timing-based "continuation tail" redirect. The user chose
        // this specific row; there is no solid on-disk signal that a later same-cwd session "continues" it
        // (/clear leaves no link, /compact is in-place), so the old redirect merged unrelated conversations
        // and made the picked session unresumable — it jumped to a different live tab instead. [Agentmaster]
        const std::wstring target = sessionId;
        const auto existing = _sessionRegistry->Get(target);
        if (existing && existing->live)
        {
            // Already OPEN somewhere in this app. Foreground: jump to it. Background bulk-open: leave
            // focus where it is (don't yank to an already-open tab) — it's already there to switch to.
            ::Agentmaster::LogNav(L"sessions resume -> jump " + ::Agentmaster::ShortId(target) + (target == sessionId ? L"" : (L" (resolved from " + ::Agentmaster::ShortId(sessionId) + L")")) + L" (already open)");
            if (!_openClaudeTabInBackground)
            {
                _ActivateClaudeSession(winrt::hstring{ target });
            }
            return target; // open already: that tab is the answer (nothing new was opened)
        }
        // Native-exe-only policy gate (auto-recovering): everything past here LAUNCHES a claude (seed an
        // archived-shaped record, then _RestoreArchivedSession --resume), so gate + prompt here — BEFORE
        // seeding a phantom record we'd otherwise leave behind. The already-live branch above only jumps
        // to an existing tab, so it stays ungated.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"sessions resume-done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
            return {};
        }
        if (!existing)
        {
            ::Agentmaster::SessionInfo s;
            s.id = target;
            s.workingDir = dir;
            s.title = title;
            s.state = ::Agentmaster::SessionState::Idle;
            s.live = false; // archived-shaped: exactly what _RestoreArchivedSession expects
            // Agentmaster (the runner's mode is per session and persisted, 2026-10-26): a session the registry never knew
            // has no saved plan, so it starts on the cog's Prompt Queue Runner defaults, as a fresh launch or an adopted
            // session does. The restore seam (_LaunchClaudeSession's restored branch) keeps whatever mode the record
            // carries — without these lines this first resume would come up Paused (the struct's Off), where it used
            // to get the cog's default from the restore seam's old re-stamp.
            s.autorunner.mode = _appSettings.defaultAutorunnerMode;
            s.autorunner.maxAutoSends = _appSettings.maxAutoSends;
            s.autorunner.stopOnError = _appSettings.stopOnError;
            s.autorunner.pauseOnHumanInput = _appSettings.pauseOnHumanInput;
            _sessionRegistry->Upsert(std::move(s));
        }
        const auto resumedTab = _RestoreArchivedSession(winrt::hstring{ target }, insertPosition);
        // Nav audit END (pairs with the resume-click BEGIN above): the actually-opened id. Normally ==
        // target; a resume whose transcript vanished degrades to a FRESH id (Rule #6), reported here. A
        // resume-click with neither a "resume -> jump" nor a resume-done means the launch crashed mid-open.
        const std::wstring resumedId = resumedTab ? _ClaudeSessionForTab(resumedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"sessions resume-done " + (resumedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(resumedId) + (resumedId == target ? std::wstring{} : (L" (target was " + ::Agentmaster::ShortId(target) + L" \x2014 fresh)")))));
        if (!_openClaudeTabInBackground)
        {
            _HideSessionsPage(); // foreground: land on the freshly opened tab. Background bulk-open keeps the list open.
        }
        return resumedId;
    }

    // Fork ANY on-disk session into a NEW managed conversation — the duplicate-tab fork's exact
    // recipe (TabManagement.cpp): `claude --resume <parent> --fork-session --session-id <new>`
    // (BuildClaudeSpawn mints the new id, so hooks/registry correlate from the first event), the
    // parent transcript untouched. Transcript-gated: a parent with no transcript (or one the
    // cleanup sweep deleted mid-view) degrades to a FRESH session in the same dir rather than
    // dying on "No conversation found". Safe on a LIVE parent — the fork writes its own file.
    // insertPosition (SESSIONS.md §9.14): the 0-based strip slot for the fork; -1 = the end (Terminal's newTabPosition).
    // Returns the NEW session's id, or "" when no tab opened (a gate, a failed launch, missing arguments).
    // forkTurn (HOOKS.md "Interrupted turns"): Interrupted — the user's "Fork interrupted" — records the stop intent on the new
    // fork (SessionInfo::stopForkedTurn, persisted) and arms the fork stop watch (TerminalPage.AgentTurns.cpp).
    std::wstring TerminalPage::_ForkSessionFromDisk(const std::wstring& parentId, const std::wstring& dir, const std::wstring& title, const std::wstring& modelOverride, uint32_t insertPosition, ::Agentmaster::ForkTurnChoice forkTurn)
    {
        if (!_sessionRegistry || parentId.empty() || dir.empty())
        {
            return {};
        }
        // Nav audit: the user picked this ROW to fork. The clicked id is what they SEE; the
        // continuation resolve below logs the actual fork SOURCE, and the downstream [fork] line
        // carries the NEW forked id ("[fork] <new> (forked from <source>)") — so grepping [nav]+[fork]
        // gives the full row-clicked -> source -> new-id chain the user asked to be able to follow.
        // A launch-model pick rides the line (model=<id>).
        ::Agentmaster::LogNav(L"sessions fork-click row=" + ::Agentmaster::ShortId(parentId) + L" \"" + title.substr(0, 80) + L"\" dir=" + dir + (modelOverride.empty() ? std::wstring{} : (L" model=" + modelOverride)) + (_openClaudeTabInBackground ? L" [bg]" : L""));
        // Native-exe-only policy gate (auto-recovering): a fork is a claude launch (--fork-session).
        // _LaunchClaudeSession's backstop is SILENT, so gate + prompt here to surface the install notice.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"sessions fork-done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
            return {};
        }
        // Fork EXACTLY the picked session — no timing-based "continuation tail" redirect (it merged
        // unrelated same-cwd sessions: /clear leaves no on-disk successor link, /compact is in-place, a
        // plan-restart references its parent backward only). The clicked row's id/dir/title are the source.
        const std::wstring forkParentId = parentId;
        const std::wstring forkDir = dir;
        const std::wstring forkBaseTitle = title;
        const std::wstring forkBase = !forkBaseTitle.empty() ? forkBaseTitle : ::Agentmaster::DeriveSessionTitle(forkDir);
        const std::wstring ttl = ::Agentmaster::DeriveForkTitle(forkBase); // bump " (fork N)" instead of stacking
        const std::wstring forkFrom = ::Agentmaster::ClaudeConversationExists(forkParentId) ? forkParentId : std::wstring{};
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[sessions-page->fork] source=" + forkParentId + (forkFrom.empty() ? L" (no transcript -> fresh session)" : L"") + L"\n");
        // PENDING_INPUT.md §11 — the FORK CARRY: snapshot the source's unsent draft + paste journal BEFORE the
        // launch. A row can be any on-disk session: a live one hosted here (the live box answers), one in
        // another window or a closed one with a persisted memory (the remembered draft answers), or one the
        // registry never knew (nothing to carry — _CaptureDraftForFork returns empty).
        auto carry = _CaptureDraftForFork(forkParentId);
        const auto forkedTab = _LaunchClaudeSession(winrt::hstring{ forkDir }, winrt::hstring{ ttl }, std::nullopt, forkFrom, insertPosition, modelOverride);
        _CarryDraftIntoFork(forkParentId, forkedTab ? _ClaudeSessionForTab(forkedTab) : std::wstring{}, std::move(carry));
        // Nav audit END (pairs with the fork-click BEGIN above): the NEW forked id — exactly "the id of the
        // new forked session" the user asked to be able to trace, alongside the source it forked from. A
        // fork-click with NO matching fork-done means the launch crashed/hung between the two (the
        // crash-resilience the begin/end pairing buys). The id is resolved from the just-created tab, so it
        // reflects what actually opened (a no-transcript parent degrades to a FRESH session, noted here).
        const std::wstring forkedId = forkedTab ? _ClaudeSessionForTab(forkedTab) : std::wstring{};
        // HOOKS.md "Interrupted turns": "Fork interrupted" — persist the intent, then watch the fork.
        if (forkTurn == ::Agentmaster::ForkTurnChoice::Interrupted && !forkedId.empty())
        {
            _sessionRegistry->Update(forkedId, [](::Agentmaster::SessionInfo& s) { s.stopForkedTurn = true; });
            _ArmForkTurnStop(forkedId, L"fork");
        }
        ::Agentmaster::LogNav(L"sessions fork-done " + (forkedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(forkedId))) + L" from=" + ::Agentmaster::ShortId(forkParentId) + (forkFrom.empty() ? std::wstring{ L" (fresh \x2014 no parent transcript)" } : std::wstring{}) + (forkTurn == ::Agentmaster::ForkTurnChoice::Interrupted ? std::wstring{ L" (fork interrupted)" } : std::wstring{}));
        if (!_openClaudeTabInBackground)
        {
            _HideSessionsPage(); // foreground: land on the fork. Background bulk-open keeps the list open.
        }
        return forkedId;
    }

    // Agentmaster: the Sessions-page double-click prompt — Resume / Fork / Cancel. Double-clicking a
    // CLOSED on-disk session used to resume it silently; offer the choice instead (the adopt dialog's
    // Fork-a-copy-vs-Resume idiom — _ConfirmChoice), since forking a recognized conversation is just as
    // common as continuing it. Resume (Primary) = continue this conversation (claude --resume); Fork
    // (Secondary) = branch a NEW conversation from it (--fork-session, the original transcript
    // untouched); Cancel = nothing. Shown only for a NOT-live row — a live session's double-click jumps
    // straight to its tab (handled before this is called). forkTitle is empty for a never-prompted row
    // so the fork seam derives a smart name.
    winrt::fire_and_forget TerminalPage::_PromptResumeOrForkSession(std::wstring sessionId, std::wstring dir, std::wstring title, std::wstring forkTitle)
    {
        // Agentmaster (terminate-net): a dialog lane — ShowDialog on a tearing-down presenter, the
        // dialog build, or the follow-on resume/fork launch throwing would std::terminate the app
        // (fire_and_forget). Contain + log; a failed prompt leaves the row as-is (re-invokable).
        try
        {
        const auto presenter{ _dialogPresenter.get() };
        if (!presenter)
        {
            // No presenter to confirm with -> preserve the prior behavior (resume) rather than
            // stranding the double-click.
            _ResumeSessionFromDisk(sessionId, dir, title);
            co_return;
        }

        ContentDialog dialog;
        dialog.Tag(winrt::box_value(L"agentmaster-dark")); // Agentmaster: force dark (Agent Manager UI) — see TerminalWindow::ShowDialog
        dialog.Title(winrt::box_value(L"Open session"));
        dialog.Content(winrt::box_value(
            title.empty() ?
                winrt::hstring{ L"Resume continues this conversation in a managed tab (claude --resume). Fork branches a NEW conversation from it \x2014 a copy you can diverge freely; the original transcript is untouched (--fork-session)." } :
                winrt::hstring{ L"\x201C" + title + L"\x201D \x2014 Resume continues this conversation in a managed tab (claude --resume). Fork branches a NEW conversation from it \x2014 a copy you can diverge freely; the original transcript is untouched (--fork-session)." }));
        dialog.PrimaryButtonText(L"Resume");
        dialog.SecondaryButtonText(L"Fork");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary); // safe default = Resume (continue where you left off)

        const auto weak = get_weak();
        const auto result = co_await presenter.ShowDialog(dialog);
        const auto strong = weak.get(); // ShowDialog awaits; re-acquire before touching state
        if (!strong)
        {
            co_return;
        }
        if (result == ContentDialogResult::Primary)
        {
            _ResumeSessionFromDisk(sessionId, dir, title);
        }
        else if (result == ContentDialogResult::Secondary)
        {
            // HOOKS.md "Interrupted turns": a fork of a session in the middle of a turn asks "interrupted or uninterrupted?"
            // first (a second dialog, only when it applies).
            ForkLaunchRequest request;
            request.sourceId = sessionId;
            request.dir = dir;
            request.title = forkTitle;
            request.origin = L"sessions-open-dialog";
            _ForkAskingAboutRunningTurn(std::move(request));
        }
        // else Close/Cancel -> do nothing
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PromptResumeOrForkSession");
        }
    }

    // Recolor the row highlights for _sessionsSelectedId WITHOUT rebuilding the table (the
    // archive page's _UpdateArchiveSelectionHighlight pattern) — row taps + keyboard nav.
    void TerminalPage::_UpdateSessionsSelectionHighlight()
    {
        if (!_sessionsRowsHost)
        {
            return;
        }
        for (const auto& child : _sessionsRowsHost.Children())
        {
            const auto border = child.try_as<Border>();
            if (!border)
            {
                continue;
            }
            const std::wstring id{ winrt::unbox_value_or<winrt::hstring>(border.Tag(), L"") };
            border.Background(id == _sessionsSelectedId ? SessBrush(0x30, 0x60, 0xA0, 0xE0) : SessBrush(0x14, 0xFF, 0xFF, 0xFF));
        }
    }

    // Up/Down keyboard navigation over the VISIBLE (sorted + filtered) rows. No selection yet:
    // Down picks the first row, Up the last; with one, the selection moves ±1 and WRAPS at the
    // ends (rotates). The selected row is scrolled into view.
    void TerminalPage::_MoveSessionsSelection(int delta)
    {
        if (_sessionsVisibleOrder.empty() || !_sessionsRowsHost)
        {
            return;
        }
        _DisarmSessionsRenameTimer(); // moving the selection by keyboard cancels a pending slow-double-click arm
        const int n = static_cast<int>(_sessionsVisibleOrder.size());
        int idx = -1;
        if (!_sessionsSelectedId.empty())
        {
            for (int i = 0; i < n; ++i)
            {
                if (_sessionsVisibleOrder[i] == _sessionsSelectedId)
                {
                    idx = i;
                    break;
                }
            }
        }
        // A selection filtered out of view counts as none (idx -1): Down = first, Up = last.
        const int next = (idx < 0) ? (delta > 0 ? 0 : n - 1) : (((idx + delta) % n + n) % n);
        _sessionsSelectedId = _sessionsVisibleOrder[next];
        ::Agentmaster::LogNav(L"sessions select " + ::Agentmaster::ShortId(_sessionsSelectedId) + L" via=key" + (delta > 0 ? L"\x2193" : L"\x2191") + (_sessionsQueryText.empty() ? std::wstring{} : (L" q=\"" + _sessionsQueryText + L"\"")));
        _UpdateSessionsSelectionHighlight();
        _ShowSessionsDetail(_sessionsSelectedId);
        // PREFETCH in the direction of travel so the FOLLOWING Up/Down lands on a warm cache —
        // Down warms the two rows below, Up the two above (background, deduped, no wrap at the ends).
        _PrefetchSessionsSummaries(_sessionsSelectedId, delta > 0 ? 1 : -1);
        // Key-nav scrolls WITHOUT pointer input — a row tip open under the stationary mouse
        // never gets the PointerExited that would close it when its row scrolls away.
        SessCloseTipsIn(_sessionsRowsHost);
        for (const auto& child : _sessionsRowsHost.Children())
        {
            if (const auto b = child.try_as<Border>(); b && std::wstring{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") } == _sessionsSelectedId)
            {
                b.StartBringIntoView();
                break;
            }
        }
    }

    // Append an id to AppSettings.hiddenSessionIds via a freshest-disk read-modify-write (so it
    // sticks across restarts AND doesn't clobber another field / another window's concurrent write
    // — the splitter/treeSort pattern) and refresh THIS window's in-memory copy. Idempotent — an
    // already-hidden id is a no-op (no duplicate write); returns true only when it was newly added.
    // NO UI side effects: the caller re-renders. Shared by the Sessions-page row right-click "Hide
    // from list" AND the auto-hide-on-delete seam (_RemoveSessionRecord). The transcript on disk is
    // NEVER touched — a browse-list preference only.
    bool TerminalPage::_AddSessionIdToHiddenList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return false;
        }
        auto s = ::Agentmaster::LoadAppSettings();
        bool changed = false;
        if (std::find(s.hiddenSessionIds.begin(), s.hiddenSessionIds.end(), sessionId) == s.hiddenSessionIds.end())
        {
            s.hiddenSessionIds.push_back(sessionId);
            ::Agentmaster::SaveAppSettings(s);
            changed = true;
        }
        _appSettings.hiddenSessionIds = s.hiddenSessionIds;
        return changed;
    }

    // Row right-click "Hide from list": persist the id (the RMW above) and re-render the table (the
    // render chokepoint filters it out, unless the "Hidden" reveal filter is on). Resettable from
    // the Settings cog. Already-hidden ids are a no-op.
    void TerminalPage::_HideSessionFromList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        ::Agentmaster::LogNav(L"sessions hide " + ::Agentmaster::ShortId(sessionId));
        _AddSessionIdToHiddenList(sessionId);
        // If the hidden row was selected, drop the selection so the detail pane doesn't keep
        // showing a session that's no longer in the list.
        if (_sessionsSelectedId == sessionId)
        {
            _sessionsSelectedId.clear();
            _ShowSessionsDetail(_sessionsSelectedId); // -> "Select a session"
        }
        _RenderSessionsTable();
    }

    // Row right-click "Unhide" — the inverse of Hide, offered on a row that is currently in the
    // hidden set (only reachable while the "Hidden" reveal filter shows it). Drop the id from
    // AppSettings.hiddenSessionIds (freshest-disk RMW, mirroring Hide) so the session returns to the
    // list normally, refresh the in-memory copy, and re-render. A no-op if the id wasn't hidden.
    void TerminalPage::_UnhideSessionFromList(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        ::Agentmaster::LogNav(L"sessions unhide " + ::Agentmaster::ShortId(sessionId));
        auto s = ::Agentmaster::LoadAppSettings();
        const auto it = std::find(s.hiddenSessionIds.begin(), s.hiddenSessionIds.end(), sessionId);
        if (it != s.hiddenSessionIds.end())
        {
            s.hiddenSessionIds.erase(it);
            ::Agentmaster::SaveAppSettings(s);
        }
        _appSettings.hiddenSessionIds = s.hiddenSessionIds;
        _RenderSessionsTable();
    }

    // Settings cog "Reset hidden sessions" (wired via SetResetHiddenSessionsHandler): clear the
    // whole hidden set (freshest-disk RMW) and re-render the Sessions page if it is built, so every
    // hidden session reappears. No re-gather needed — the rows are still in _sessionsRows; only the
    // render-time filter changes. A no-op when nothing is hidden.
    void TerminalPage::_ResetHiddenSessions()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        if (s.hiddenSessionIds.empty())
        {
            _appSettings.hiddenSessionIds.clear();
            return;
        }
        s.hiddenSessionIds.clear();
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.hiddenSessionIds.clear();
        _RenderSessionsTable(); // no-op if the page was never built (host null)
    }

    // FAVORITES.md: flip the durable star (SessionStore "favorite" key). The CURRENT state is read
    // from disk (authoritative), so this is correct whether called from the Sessions page (the ★
    // column / row menu) or the session tab's right-click menu (where the page — and _sessionsFavorites
    // — may not even be loaded). The in-memory set is kept in step for an open page, then re-render
    // (a no-op when the page was never built, so the tab-menu path costs nothing extra).
    void TerminalPage::_ToggleSessionFavorite(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        const bool nowFav = !::Agentmaster::IsSessionFavorite(sessionId); // flip the on-disk truth
        ::Agentmaster::SetSessionFavorite(sessionId, nowFav);
        ::Agentmaster::LogNav(L"favorite " + ::Agentmaster::ShortId(sessionId) + (nowFav ? L" on" : L" off"));
        if (nowFav)
        {
            _sessionsFavorites.insert(sessionId);
        }
        else
        {
            _sessionsFavorites.erase(sessionId);
        }
        _RefreshTabFavoriteCrown(sessionId); // FAVORITES.md: live-update the tab-strip crown if this window hosts the session
        _RenderSessionsTable(); // refresh the ★ glyph + (if the Favorite filter is on) the visible set
    }

    // ===== in-place TITLE editing (Edit Title / slow-double-click) ==========================
    // The Sessions browser lets you rename a session in place — the durable per-session TITLE the
    // SessionStore keeps (the SAME store the favorite star uses). Two entry points: the row
    // right-click "Edit Title" and the Windows-Explorer "slow double-click" gesture (re-click the
    // already-selected row). Both swap the row's Title cell for a focused TextBox (a ContentDialog
    // text box gets no keypresses under XAML Islands — the Manager's in-place rename idiom). Commit
    // persists the trimmed title; for a session known to the registry it routes through the live
    // rename so the open tab + Explorer/board lens track it (Rule #11).

    void TerminalPage::_BeginSessionsRename(const std::wstring& sessionId)
    {
        if (sessionId.empty() || !_sessionsRowsHost)
        {
            return;
        }
        if (!_sessRenamingId.empty())
        {
            if (_sessRenamingId == sessionId)
            {
                return; // already editing this row
            }
            _CommitSessionsRename(); // a different row is already mid-edit — commit it before switching (clicking a Border doesn't steal the editor's focus, so LostFocus wouldn't have fired)
        }
        _DisarmSessionsRenameTimer(); // any pending slow-double-click arm is now consumed
        // Make the target row the selection so the editor renders in a known, visible row and the
        // detail pane tracks it (the right-click path may fire on a row that wasn't selected yet).
        _sessionsSelectedId = sessionId;
        _sessRenamingId = sessionId;
        _sessRenameBox = nullptr; // bootstrap: the next render builds + focuses the editor (the guard lets THIS render through)
        _RenderSessionsTable(); // colors the row selected + swaps its Title cell for the editor
        _ShowSessionsDetail(sessionId);
    }

    void TerminalPage::_CommitSessionsRename()
    {
        if (_sessRenamingId.empty())
        {
            return; // idempotent: a deferred Enter-commit + the LostFocus that follows must collapse to one
        }
        const std::wstring id = _sessRenamingId;
        std::wstring name = _sessRenameBox ? std::wstring{ _sessRenameBox.Text() } : std::wstring{};
        // Trim surrounding whitespace/newlines — a blank/whitespace edit keeps the old title (matches
        // the Manager's _CommitRename + the WT tab renamer; a title never goes empty, Rule #11).
        const auto first = name.find_first_not_of(L" \t\r\n");
        const auto last = name.find_last_not_of(L" \t\r\n");
        name = (first == std::wstring::npos) ? std::wstring{} : name.substr(first, last - first + 1);

        _sessRenamingId.clear();
        _sessRenameBox = nullptr;
        if (!name.empty())
        {
            _PersistEditedSessionTitle(id, name);
        }
        // Exit edit mode: the Title cell renders as text again. With an active query, re-run the
        // search so the new title's membership + ranking update (the 🏷 title scope); otherwise a
        // plain re-render is enough. Refresh the detail header either way.
        if (!_sessionsQueryText.empty())
        {
            _RunSessionsSearch();
        }
        else
        {
            _RenderSessionsTable();
        }
        _ShowSessionsDetail(_sessionsSelectedId);
    }

    void TerminalPage::_CancelSessionsRename()
    {
        if (_sessRenamingId.empty())
        {
            return;
        }
        _sessRenamingId.clear();
        _sessRenameBox = nullptr;
        _RenderSessionsTable(); // back to the text Title cell, nothing written
    }

    // The persistence core. Rule #11 — the title is ONE value. A session KNOWN to the process-wide
    // registry (open OR archived) goes through the live rename seam: it writes SessionInfo.title
    // (-> the Engine observer mirrors it to <profile>/session-store/<sid>.json AND sessions.json) and
    // re-pins the WT tab when the session is open in this window. A pure ON-DISK session (never
    // managed by this app) isn't in the registry, so write the durable store directly — exactly the
    // overlay the Sessions browser reads for closed/historical rows (LoadAllStoredSessionTitles).
    // Either way the new title is reflected into the in-memory rows + the search index entry so the
    // table (display + sort + the 🏷 title search) and the detail header update without a re-gather.
    void TerminalPage::_PersistEditedSessionTitle(const std::wstring& sessionId, const std::wstring& title)
    {
        if (sessionId.empty() || title.empty())
        {
            return;
        }
        ::Agentmaster::LogNav(L"sessions edit-title " + ::Agentmaster::ShortId(sessionId) + L" -> \"" + title.substr(0, 80) + L"\"");
        if (_sessionRegistry && _sessionRegistry->Get(sessionId).has_value())
        {
            _RenameClaudeSession(winrt::hstring{ sessionId }, winrt::hstring{ title }); // registry (+ tab if open); the Engine observer mirrors to the durable store + sessions.json
        }
        else
        {
            ::Agentmaster::SetStoredSessionTitle(sessionId, title); // pure on-disk row — write the durable store directly
        }
        for (size_t i = 0; i < _sessionsRows.size(); ++i)
        {
            if (_sessionsRows[i].id == sessionId)
            {
                _sessionsRows[i].title = title;
                if (i < _sessionsEntries.size() && _sessionsEntries[i].sessionId == sessionId)
                {
                    _sessionsEntries[i].liveTitle = title; // keep the 🏷 title search in step with the edit
                }
                break;
            }
        }
    }

    void TerminalPage::_ArmSessionsRenameTimer(const std::wstring& sessionId)
    {
        if (!_sessRenameArmTimer || sessionId.empty())
        {
            return;
        }
        _sessRenamePendingId = sessionId;
        _sessRenameArmTimer.Stop();
        _sessRenameArmTimer.Start(); // a DispatcherTimer restarts its interval on Start()
    }

    void TerminalPage::_DisarmSessionsRenameTimer()
    {
        _sessRenamePendingId.clear();
        if (_sessRenameArmTimer)
        {
            _sessRenameArmTimer.Stop();
        }
    }

    // ===== the Sessions-page row right-click "Filter" facets ================================
    // A row's right-click "Filter \xBB" submenu narrows the visible set to rows matching the clicked
    // ("anchor") row in some dimension. Each facet ANDs with the search text + the scope/Open/Hidden
    // toggles (all applied together at the _RenderSessionsTable chokepoint), and facets stack across
    // dimensions (dir AND branch AND time-bucket AND fork-family). Picking a facet a row already
    // matches toggles it OFF. Transient browse-state — nothing persisted, nothing touched on disk.

    // The AND-predicate over every active facet: true == keep this row. An inactive facet imposes no
    // constraint. Directory is matched filesystem-aware (NormDirKey, Rule #8); branch is exact (git
    // refs are case-sensitive); a time bucket is the half-open [start, end) created-time window; the
    // fork family is set membership over the precomputed connected component.
    bool TerminalPage::_SessionsRowPassesRowFilter(const _SessionsRow& r) const
    {
        const auto& f = _sessionsRowFilter;
        if (f.hasDir && ::Agentmaster::NormDirKey(f.dir) != ::Agentmaster::NormDirKey(r.dir))
        {
            return false;
        }
        if (f.hasBranch && f.branch != r.branch)
        {
            return false;
        }
        if (f.timeGran != _SessionsRowFilterState::TimeGran::None && !(r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs))
        {
            return false;
        }
        if (f.hasFamily && f.familyIds.count(r.id) == 0)
        {
            return false;
        }
        // Bookmark tags (the header chips row): a row must carry ALL selected tags — the facets-AND
        // rule every other dimension follows, so stacking chips NARROWS. Case-insensitive
        // (FoldTagName), against the gather-loaded _sessionsTags (an untagged row can't match).
        if (!f.tags.empty())
        {
            const auto it = _sessionsTags.find(r.id);
            if (it == _sessionsTags.end())
            {
                return false;
            }
            for (const auto& want : f.tags)
            {
                const std::wstring folded = ::Agentmaster::FoldTagName(want);
                bool has = false;
                for (const auto& t : it->second)
                {
                    if (::Agentmaster::FoldTagName(t) == folded)
                    {
                        has = true;
                        break;
                    }
                }
                if (!has)
                {
                    return false;
                }
            }
        }
        // Agentmaster (SESSIONS.md §10): the FOLDER FILTER — the one facet whose values combine with OR (the
        // user's rule): a row in ANY picked folder stays, matched against its launch folder AND its inferred
        // folder (exact, or with the subfolders when "Include subfolders" is on). A row whose inferred folder
        // is not computed yet matches by its launch folder only; the inference pass re-renders when it lands.
        if (!f.folders.empty() && !::Agentmaster::SessionInPickedFolders(f.folders, _sessFolders.includeSubfolders, r.dir, r.inferredDir))
        {
            return false;
        }
        return true;
    }

    // Does the facet for `kind`'s DIMENSION currently exist AND equal row r's value? Drives both the
    // submenu's \x2713 (so the item that would toggle the active facet OFF reads as "checked") and the
    // apply-toggle direction below. For the time dimension it is granularity-specific: with a WEEK
    // facet active, the SameDay query returns false (so picking SameDay REPLACES week with day).
    bool TerminalPage::_SessionsRowFilterMatchesAnchor(int kind, const _SessionsRow& r) const
    {
        const auto& f = _sessionsRowFilter;
        switch (static_cast<_SessionsRowFilterKind>(kind))
        {
        case _SessionsRowFilterKind::SameDirectory:
            return f.hasDir && ::Agentmaster::NormDirKey(f.dir) == ::Agentmaster::NormDirKey(r.dir);
        case _SessionsRowFilterKind::SameBranch:
            return f.hasBranch && f.branch == r.branch;
        case _SessionsRowFilterKind::SameDay:
            return f.timeGran == _SessionsRowFilterState::TimeGran::Day && r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs;
        case _SessionsRowFilterKind::SameWeek:
            return f.timeGran == _SessionsRowFilterState::TimeGran::Week && r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs;
        case _SessionsRowFilterKind::SameMonth:
            return f.timeGran == _SessionsRowFilterState::TimeGran::Month && r.createdMs >= f.timeStartMs && r.createdMs < f.timeEndMs;
        case _SessionsRowFilterKind::ForkFamily:
            return f.hasFamily && f.familyIds.count(r.id) != 0;
        }
        return false;
    }

    // The connected component of the fork graph that contains anchorId, over the GATHERED rows: a BFS
    // treating each row's forkedFromId as an UNDIRECTED edge to its parent, so a fork, its parent, and
    // their siblings all land in one family. A parent outside the current window is harmless — it just
    // sits in the set (no such row renders). The anchor is always included (a lone session = {anchor}).
    std::unordered_set<std::wstring> TerminalPage::_ComputeForkFamily(const std::wstring& anchorId) const
    {
        std::unordered_multimap<std::wstring, std::wstring> adj;
        for (const auto& r : _sessionsRows)
        {
            if (!r.forkedFromId.empty())
            {
                adj.emplace(r.id, r.forkedFromId);
                adj.emplace(r.forkedFromId, r.id);
            }
        }
        std::unordered_set<std::wstring> seen{ anchorId }, out;
        std::vector<std::wstring> stack{ anchorId };
        while (!stack.empty())
        {
            const std::wstring cur = std::move(stack.back());
            stack.pop_back();
            out.insert(cur);
            const auto range = adj.equal_range(cur);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (seen.insert(it->second).second)
                {
                    stack.push_back(it->second);
                }
            }
        }
        return out;
    }

    // Toggle the facet for `kind`'s dimension to the anchor row's value — or OFF when the anchor
    // already matches it (so the same submenu item is a clean toggle). Picking a different time
    // granularity REPLACES the time facet (one granularity at a time); the dir/branch/family facets
    // are independent dimensions that coexist as AND. Re-renders + refreshes the chip.
    void TerminalPage::_ApplySessionsRowFilter(int kind, const std::wstring& anchorId)
    {
        const _SessionsRow* a = nullptr;
        for (const auto& r : _sessionsRows)
        {
            if (r.id == anchorId)
            {
                a = &r;
                break;
            }
        }
        if (!a)
        {
            return; // the anchor scrolled out of the gathered set
        }
        auto& f = _sessionsRowFilter;
        const bool alreadyMatches = _SessionsRowFilterMatchesAnchor(kind, *a);
        switch (static_cast<_SessionsRowFilterKind>(kind))
        {
        case _SessionsRowFilterKind::SameDirectory:
            if (alreadyMatches)
            {
                f.hasDir = false;
                f.dir.clear();
            }
            else
            {
                f.hasDir = true;
                f.dir = a->dir;
            }
            break;
        case _SessionsRowFilterKind::SameBranch:
            if (a->branch.empty())
            {
                break; // nothing to pin on
            }
            if (alreadyMatches)
            {
                f.hasBranch = false;
                f.branch.clear();
            }
            else
            {
                f.hasBranch = true;
                f.branch = a->branch;
            }
            break;
        case _SessionsRowFilterKind::SameDay:
        case _SessionsRowFilterKind::SameWeek:
        case _SessionsRowFilterKind::SameMonth:
        {
            if (alreadyMatches)
            {
                f.timeGran = _SessionsRowFilterState::TimeGran::None;
                f.timeStartMs = 0;
                f.timeEndMs = 0;
                f.timeLabel.clear();
                break;
            }
            int gran = 0; // SameDay
            auto wantGran = _SessionsRowFilterState::TimeGran::Day;
            if (static_cast<_SessionsRowFilterKind>(kind) == _SessionsRowFilterKind::SameWeek)
            {
                gran = 1;
                wantGran = _SessionsRowFilterState::TimeGran::Week;
            }
            else if (static_cast<_SessionsRowFilterKind>(kind) == _SessionsRowFilterKind::SameMonth)
            {
                gran = 2;
                wantGran = _SessionsRowFilterState::TimeGran::Month;
            }
            const auto [s, e] = SessLocalBucket(a->createdMs, gran);
            if (s == 0 && e == 0)
            {
                break; // un-bucketable created time
            }
            f.timeGran = wantGran;
            f.timeStartMs = s;
            f.timeEndMs = e;
            f.timeLabel = SessBucketLabel(a->createdMs, gran);
            break;
        }
        case _SessionsRowFilterKind::ForkFamily:
            if (alreadyMatches)
            {
                f.hasFamily = false;
                f.familyIds.clear();
            }
            else
            {
                f.familyIds = _ComputeForkFamily(anchorId);
                f.hasFamily = !f.familyIds.empty();
            }
            break;
        }
        _UpdateSessionsFilterChip();
        _RenderSessionsTable();
    }

    // Drop EVERY facet (the chip click, and the submenu's "Clear filters") + re-render. Also
    // re-styles the header TAG CHIPS row (the cleared tag facet must read deselected there).
    void TerminalPage::_ClearSessionsRowFilter()
    {
        _sessionsRowFilter = _SessionsRowFilterState{};
        _UpdateSessionsFilterChip();
        _RebuildSessionsTagChips();
        _RefreshSessionsFolderMarks(); // Agentmaster (SESSIONS.md §10): the picked folders went with the facets — the toggle, the dropdown's check marks and chips must read "none"
        _RenderSessionsTable();
    }

    // Refresh the "\x2715 filter: \x2026" chip beside the search box — collapsed when no facet is
    // active, else a "\xB7"-joined summary of the active facets (the same dimensions the count line
    // mentions). Clicking the chip clears ALL facets (wired in _BuildSessionsPageShell).
    void TerminalPage::_UpdateSessionsFilterChip()
    {
        if (!_sessFilterChip)
        {
            return;
        }
        const auto& f = _sessionsRowFilter;
        if (!f.Any())
        {
            _sessFilterChip.Visibility(Visibility::Collapsed);
            return;
        }
        std::wstring parts;
        const auto add = [&](const std::wstring& p) {
            if (!parts.empty())
            {
                parts += L" \x00B7 ";
            }
            parts += p;
        };
        if (f.hasDir)
        {
            add(L"dir " + SessLeaf(f.dir));
        }
        if (f.hasBranch)
        {
            add(L"branch " + f.branch);
        }
        if (f.timeGran != _SessionsRowFilterState::TimeGran::None)
        {
            add(f.timeLabel);
        }
        if (f.hasFamily)
        {
            add(L"fork family (" + std::to_wstring(f.familyIds.size()) + L")");
        }
        for (const auto& t : f.tags)
        {
            add(L"tag \x201C" + t + L"\x201D"); // bookmark tags: each selected chip reads here too
        }
        if (!f.folders.empty())
        {
            // Agentmaster (SESSIONS.md §10): the picked folders, joined with "or" — they are the one facet
            // that combines with OR, and the chip should say so rather than read like the AND-ed facets.
            std::wstring names;
            for (const auto& folder : f.folders)
            {
                if (!names.empty())
                {
                    names += L" or ";
                }
                names += SessLeaf(folder);
            }
            add((f.folders.size() == 1 ? std::wstring{ L"folder " } : std::wstring{ L"folders " }) + names + (_sessFolders.includeSubfolders ? L" (+ subfolders)" : L""));
        }
        _sessFilterChip.Content(winrt::box_value(winrt::hstring{ L"\x2715 " + parts }));
        // Re-titled here as well as at build, so the chip's live tip is self-contained (the panel
        // leads with the title; the text is the facet list this render produced).
        SessSetTip(_sessFilterChip,
                   L"Active filter",
                   winrt::hstring{ L"Showing only sessions matching " + parts + L".\n\nThese narrow the list on top of the search box and the toggles \x2014 everything has to match at once. Click to clear them all." });
        _sessFilterChip.Visibility(Visibility::Visible);
    }

    // ===== the header TAG CHIPS row (bookmark tags) ==========================================
    // Re-list the chips under the filter toggles: one blue, partially-transparent ToggleButton
    // chip per GLOBAL tag (the union of every session's SessionStore "tags" — _sessionsTags,
    // loaded in the gather), sorted by max(session activity) desc like the tab menu's Tag panel
    // (activity here from the GATHERED rows, so it covers every on-disk session, not just the
    // registry). A chip's checked state mirrors the tag facet (_sessionsRowFilter.tags); the
    // native ToggleButton checked visual (the solid accent fill) is the "selected" look over the
    // translucent-blue rest state. Also PRUNES selected tags whose last carrier vanished (a
    // re-gather after untagging everywhere), so a stale facet can't strand the table empty with
    // no chip left to click it off. Collapses the whole row while no tags exist.
    void TerminalPage::_RebuildSessionsTagChips()
    {
        if (!_sessTagChipsPanel || !_sessTagChipsScroll)
        {
            return; // the page was never built
        }
        _sessTagChipsPanel.Children().Clear();

        std::unordered_map<std::wstring, int64_t> activity;
        for (const auto& r : _sessionsRows)
        {
            activity[r.id] = r.lastActivityMs;
        }
        const auto universe = ::Agentmaster::CollectGlobalTags(_sessionsTags, activity);

        // Prune facet tags that no longer exist in the universe (their chips are about to vanish).
        {
            std::unordered_set<std::wstring> live;
            for (const auto& info : universe)
            {
                live.insert(::Agentmaster::FoldTagName(info.name));
            }
            auto& sel = _sessionsRowFilter.tags;
            const size_t before = sel.size();
            sel.erase(std::remove_if(sel.begin(), sel.end(), [&](const std::wstring& t) { return live.count(::Agentmaster::FoldTagName(t)) == 0; }),
                      sel.end());
            if (sel.size() != before)
            {
                _UpdateSessionsFilterChip(); // the ✕ chip must drop the vanished tag too
            }
        }

        if (universe.empty())
        {
            _sessTagChipsScroll.Visibility(Visibility::Collapsed); // the Auto header row takes no height
            return;
        }
        _sessTagChipsScroll.Visibility(Visibility::Visible);

        std::unordered_set<std::wstring> selected;
        for (const auto& t : _sessionsRowFilter.tags)
        {
            selected.insert(::Agentmaster::FoldTagName(t));
        }
        // Per-tag DISPLAY colors (user-picked > name-hash), ONE disk read like _RenderSessionsTable —
        // for the leading bookmark ribbon each chip now shows (matching the Tags column / tab badges).
        const auto tagColors = ::Agentmaster::LoadAllTagColors();
        for (const auto& info : universe)
        {
            const bool on = selected.count(::Agentmaster::FoldTagName(info.name)) > 0;
            Primitives::ToggleButton chip;
            chip.MinWidth(0);
            chip.MinHeight(0);
            chip.Padding(Thickness{ 10, 2, 10, 3 });
            // A gently-rounded RECTANGLE, not the earlier radius-10 pill/capsule (per request:
            // less circle-ish) — matches the square-cornered filter toggles/buttons beside it.
            chip.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            chip.FontSize(12);
            chip.BorderThickness(Thickness{ 1, 1, 1, 1 });
            // The requested look: a blue, PARTIALLY TRANSPARENT chip at rest. The checked state
            // deliberately keeps the ToggleButton's native solid-accent fill (the same "selected"
            // language as the 👤/🤖/📁 toggles above), so selection reads instantly.
            chip.Background(SessBrush(0x42, 0x00, 0x78, 0xD4));
            chip.BorderBrush(SessBrush(0x66, 0x4F, 0xA3, 0xE3));
            chip.IsChecked(on);
            // Content: a leading BOOKMARK RIBBON in the tag's own color (the same 6.5x9.3 shape the Tags
            // column + the tab badges draw) before the name — so a filter chip reads as that exact tag's
            // bookmark, and its color ties it to the ribbons in the list. (per request: (<bookmark> tag).)
            {
                StackPanel chipContent;
                chipContent.Orientation(Orientation::Horizontal);
                chipContent.Spacing(6);
                chipContent.VerticalAlignment(VerticalAlignment::Center);
                winrt::Windows::UI::Xaml::Shapes::Polygon chipRibbon; // the tab badges' 6.5x9.3 bookmark shape
                chipRibbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
                chipRibbon.Points().Append(winrt::Windows::Foundation::Point{ 6.5f, 0.0f });
                chipRibbon.Points().Append(winrt::Windows::Foundation::Point{ 6.5f, 9.3f });
                chipRibbon.Points().Append(winrt::Windows::Foundation::Point{ 3.25f, 6.5f });
                chipRibbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 9.3f });
                chipRibbon.Fill(SolidColorBrush{ ResolveTagDisplayColor(info.name, tagColors) });
                chipRibbon.Stroke(SolidColorBrush{ winrt::Windows::UI::Colors::Black() });
                chipRibbon.StrokeThickness(0.75);
                chipRibbon.VerticalAlignment(VerticalAlignment::Center);
                // Nudge the ribbon DOWN 3px — the geometric center sits optically high beside the
                // text's ink; a render-transform shifts only the visual (no layout math to break).
                TranslateTransform chipRibbonNudge;
                chipRibbonNudge.Y(3.0);
                chipRibbon.RenderTransform(chipRibbonNudge);
                chipContent.Children().Append(chipRibbon);
                TextBlock chipLabel; // no explicit Foreground — inherits the ToggleButton's (adapts to the checked/hover states)
                chipLabel.Text(winrt::hstring{ info.name });
                chipLabel.VerticalAlignment(VerticalAlignment::Center);
                chipContent.Children().Append(chipLabel);
                chip.Content(chipContent);
            }
            SessSetTip(chip,
                       winrt::hstring{ L"Tag \x201C" + info.name + L"\x201D" },
                       winrt::hstring{ (on ? std::wstring{ L"Click to stop filtering by this tag. " } :
                                             std::wstring{ L"Click to show only the sessions carrying it. " }) +
                                       (info.sessionCount == 1 ? std::wstring{ L"1 session carries it." } :
                                                                 std::to_wstring(info.sessionCount) + L" sessions carry it.") +
                                       L"\n\nTags stack with each other and with the search \x2014 a session has to carry every tag you pick. Whatever is picked also reads in the \x2715 filter chip on the right." });
            const winrt::hstring tagName{ info.name };
            chip.Click([this, tagName](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                // Defer — the toggle rebuilds this very chips row + the table (the page's
                // pointer-handler tree-mutation discipline).
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), tagName]() {
                    if (const auto self = weak.get())
                    {
                        self->_ToggleSessionsTagFilter(std::wstring{ tagName });
                    }
                });
            });
            _sessTagChipsPanel.Children().Append(chip);
        }
    }

    // Flip one tag in the row-filter facet (case-insensitive identity), then refresh the three
    // surfaces that show it: the ✕ filter chip (right), the chips row (selected styling), and the
    // table (the facet composes AND at the render chokepoint). Deliberately UNLOGGED — a pure
    // view-filter toggle, like the other row-filter facets.
    void TerminalPage::_ToggleSessionsTagFilter(const std::wstring& tag)
    {
        if (tag.empty())
        {
            return;
        }
        auto& sel = _sessionsRowFilter.tags;
        const std::wstring folded = ::Agentmaster::FoldTagName(tag);
        const auto it = std::find_if(sel.begin(), sel.end(), [&](const std::wstring& t) { return ::Agentmaster::FoldTagName(t) == folded; });
        if (it != sel.end())
        {
            sel.erase(it);
        }
        else
        {
            sel.push_back(tag);
        }
        _UpdateSessionsFilterChip();
        _RebuildSessionsTagChips();
        _RenderSessionsTable();
    }

    // ===== the FOLDER FILTER (SESSIONS.md §10) ================================================
    // The filters row's "📂 Folders ▾" toggle opens a dropdown of the folders the listed sessions worked
    // in — a session's LAUNCH folder (its transcript's cwd) and its INFERRED folder (where its tool calls
    // concentrate; _InferSessionsRowDirs) — split 50% / 50%: RECENT (the folder whose newest session was
    // active last, first) over MOST USED (the folder more sessions worked in, first), both over every
    // session the page gathered for its time window. Clicking a folder adds it to the filter or removes
    // it; the picked folders combine with OR (a session in ANY of them stays — the user's rule) and the
    // folder facet ANDs with the search, the toggles and the other facets (_SessionsRowPassesRowFilter).
    // A find box narrows both halves and its Enter adds the top RECENT match, or a typed absolute path as
    // is. The pure half (the universe, the orders, the predicate) is AgentMaster/SessionFolderFilter.h.
    // Transient browse-state like every row facet: nothing is persisted, nothing on disk is touched.

    // How long the pointer must REST on a control inside the dropdown before its FLOATING tooltip opens:
    // 3 s, the Tags panel's value (the user's request for that dense panel, 2026-10-10). Normally no
    // floating tip opens inside the dropdown at all — its card is a LocalTooltip scope of the page, so the
    // tips render in the page's description panel at once. The delay matters only in the cramped fallback
    // (a window too narrow for that panel), where floating tips take over and a ~133 ms tip on every row of
    // a list would pop up at each pause of the pointer.
    static constexpr std::chrono::milliseconds kSessFoldersTipDelay{ 3000 };
    // The dropdown body's width (DIP; the card adds its 10 px padding and 1 px border on each side).
    static constexpr double kSessFoldersBodyWidth = 560.0;
    // The two halves' default height together (header + list, twice); each LIST takes 50% of what the two
    // headers leave (equal Star rows). Lowered at open when the page has less room below the toggle.
    static constexpr double kSessFoldersListsHeight = 400.0;
    // The least height the halves get when the page is short (about two rows per half).
    static constexpr double kSessFoldersListsMinHeight = 160.0;
    // What the card needs besides the halves (the find row, the hint, the chips row, the footer, padding):
    // the budget _OpenSessionsFoldersPopup keeps free below the halves.
    static constexpr double kSessFoldersChromeHeight = 170.0;
    // At most this many rows per half. A month here holds ~40 folders and three months ~60; the cap only
    // keeps a corpus with thousands of distinct folders from building thousands of buttons per keystroke.
    static constexpr size_t kSessFoldersMaxRows = 150;
    // The longest folder name the toggle shows when exactly one folder is picked; a longer one ends in "…".
    static constexpr size_t kSessFoldersButtonNameMax = 24;

    // Build the folder filter ONCE, from _BuildSessionsPageShell: the trigger toggle into the filters bar,
    // the dropdown card + popup into the page host, and the outside-press dismissal. The card is made a
    // LocalTooltip scope of the page by the shell, after the page's panel is initialized.
    //   bar  - the filters row's left-packed controls (the toggle is appended to it).
    //   host - the page's full-bleed host Grid (the popup is parented into it, so its offsets are
    //          host-relative and its find box takes keypresses under XAML Islands).
    // Returns nothing; a second call is a no-op. UI thread only.
    // Throws: what the XAML element builds throw (hresult_error) and std::bad_alloc — the shell build is
    // itself on a contained dispatcher path (_ShowSessionsPage).
    void TerminalPage::_BuildSessionsFolderFilter(const StackPanel& bar, const Grid& host)
    {
        if (_sessFolders.button || !bar || !host)
        {
            return; // built once per page, and the page shell itself is built once
        }

        // --- the trigger. A ToggleButton for the CHECKED look — the solid accent the scope toggles beside it
        // wear — which here means "a folder filter is active". A click flips IsChecked by itself, so the
        // handler puts it back from the filter state (a programmatic IsChecked raises no Click) and only
        // toggles the dropdown, one tick later (the popup's lists are rebuilt: the page's rule that a
        // pointer handler never mutates the tree while the event is still routing).
        _sessFolders.button = SessToggle(L"\U0001F4C2 Folders \x25BE", L"Folders", L"Show only the sessions that worked in the folders you pick.");
        _sessFolders.button.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            // The checked look follows the filter, never the click: undo the click's own flip at once (a
            // property write, no tree change); the label is rewritten only on the deferred tick.
            _sessFolders.button.IsChecked(!_sessionsRowFilter.folders.empty());
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (const auto self = weak.get())
                {
                    self->_ToggleSessionsFoldersPopup();
                }
            });
        });
        bar.Children().Append(_sessFolders.button);

        StackPanel body;
        body.Spacing(6);
        body.Width(kSessFoldersBodyWidth);

        // --- row 1: [ find box | + ]. The "+" shows only while Enter would add something.
        Grid findRow;
        {
            ColumnDefinition c0;
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition c1;
            c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            findRow.ColumnDefinitions().Append(c0);
            findRow.ColumnDefinitions().Append(c1);
        }
        _sessFolders.findBox = TextBox{};
        _sessFolders.findBox.PlaceholderText(L"find a folder, or type a path to add");
        _sessFolders.findBox.VerticalAlignment(VerticalAlignment::Center);
        AgentSetTitledTip(_sessFolders.findBox,
                          L"Find a folder",
                          L"Type part of a folder's path to narrow both lists below \x2014 every word has to appear in it. Enter (or +) adds the top match of the recent list; when you typed a whole path such as K:\\source\\app, it adds that folder itself, even one no listed session worked in (useful with Include subfolders).",
                          kSessFoldersTipDelay);
        _sessFolders.findBox.TextChanged([this](const winrt::Windows::Foundation::IInspectable&, const TextChangedEventArgs&) {
            // Coalesce a typing burst into ONE re-list on a LOW-priority tick (the lists are a hundred-odd
            // buttons). _AddSessionsFolderFromFindBox flushes a queued re-list before it reads the match.
            if (_sessFolders.listsQueued)
            {
                return;
            }
            _sessFolders.listsQueued = true;
            Dispatcher().RunAsync(CoreDispatcherPriority::Low, [weak = get_weak()]() {
                if (const auto self = weak.get())
                {
                    if (!self->_sessFolders.listsQueued)
                    {
                        return; // flushed already (an Enter that ran first)
                    }
                    self->_sessFolders.listsQueued = false;
                    if (self->_sessFolders.popup && self->_sessFolders.popup.IsOpen())
                    {
                        self->_RebuildSessionsFolderLists();
                    }
                }
            });
        });
        _sessFolders.findBox.KeyDown([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == VirtualKey::Enter)
            {
                e.Handled(true);
                // Deferred: the add re-lists the dropdown and re-renders the table (tree mutations).
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                    if (const auto self = weak.get())
                    {
                        self->_AddSessionsFolderFromFindBox();
                    }
                });
            }
            else if (e.Key() == VirtualKey::Escape)
            {
                e.Handled(true);
                _CloseSessionsFoldersPopup(true); // the box goes away with the card: focus back to the page's search
            }
        });
        Grid::SetColumn(_sessFolders.findBox, 0);
        findRow.Children().Append(_sessFolders.findBox);

        _sessFolders.addBtn = Button{};
        _sessFolders.addBtn.Content(winrt::box_value(winrt::hstring{ L"+" }));
        _sessFolders.addBtn.Margin(Thickness{ 6, 0, 0, 0 });
        _sessFolders.addBtn.VerticalAlignment(VerticalAlignment::Center);
        _sessFolders.addBtn.Visibility(Visibility::Collapsed); // shown by _RebuildSessionsFolderLists once Enter would add something
        _sessFolders.addBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (const auto self = weak.get())
                {
                    self->_AddSessionsFolderFromFindBox();
                }
            });
        });
        Grid::SetColumn(_sessFolders.addBtn, 1);
        findRow.Children().Append(_sessFolders.addBtn);
        body.Children().Append(findRow);

        // --- row 2: the list, split 50% / 50% — RECENT over MOST USED, each a header (Auto) over a scrolling
        // list (Star). Two Star rows of equal weight are what make the split exactly half and half, whatever
        // either half holds; the Grid's Height is set per open to the room below the toggle.
        _sessFolders.lists = Grid{};
        {
            const auto lrow = [&](double v, GridUnitType t) {
                RowDefinition r;
                r.Height(GridLengthHelper::FromValueAndType(v, t));
                _sessFolders.lists.RowDefinitions().Append(r);
            };
            lrow(0, GridUnitType::Auto); // RECENT header
            lrow(1, GridUnitType::Star); // RECENT list
            lrow(0, GridUnitType::Auto); // MOST USED header
            lrow(1, GridUnitType::Star); // MOST USED list
        }
        _sessFolders.lists.Height(kSessFoldersListsHeight);
        const auto makeHalf = [&](int headerRow, TextBlock& header, StackPanel& list, const wchar_t* tipTitle, const wchar_t* tipText) {
            header = SessText(L"", 11, true, 0.6);
            header.Margin(Thickness{ 2, headerRow == 0 ? 0.0 : 8.0, 0, 3 });
            AgentSetTitledTip(header, winrt::hstring{ tipTitle }, winrt::hstring{ tipText }, kSessFoldersTipDelay);
            Grid::SetRow(header, headerRow);
            _sessFolders.lists.Children().Append(header);
            list = StackPanel{};
            list.Spacing(1);
            ScrollViewer scroll;
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.Padding(Thickness{ 0, 0, 10, 0 }); // keep the rows' right edge clear of the scroll bar
            scroll.Content(list);
            Grid::SetRow(scroll, headerRow + 1);
            _sessFolders.lists.Children().Append(scroll);
        };
        makeHalf(0, _sessFolders.recentHeader, _sessFolders.recentList, L"Recent folders", L"The folders of your most recent sessions: each folder is placed by the newest session that worked in it, launched there or inferred there. The time at the right is that session's last activity.");
        makeHalf(2, _sessFolders.usedHeader, _sessFolders.usedList, L"Most used folders", L"The folders the most sessions worked in, over every session the page lists for its time window. The number at the right counts the sessions: launched there, or launched elsewhere and inferred there from their tool calls.");
        body.Children().Append(_sessFolders.lists);

        // --- row 3: the hint — inference still running / nothing matches / what Enter adds.
        _sessFolders.hint = SessText(L"", 11, false, 0.6);
        _sessFolders.hint.TextWrapping(TextWrapping::Wrap);
        _sessFolders.hint.TextTrimming(TextTrimming::None);
        body.Children().Append(_sessFolders.hint);

        // --- row 4: the picked folders as chips (each removes its folder), horizontally scrolling.
        {
            ScrollViewer pickedScroll;
            pickedScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
            pickedScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
            pickedScroll.HorizontalScrollMode(ScrollMode::Enabled);
            pickedScroll.VerticalScrollMode(ScrollMode::Disabled);
            _sessFolders.pickedPanel = StackPanel{};
            _sessFolders.pickedPanel.Orientation(Orientation::Horizontal);
            _sessFolders.pickedPanel.Spacing(4);
            pickedScroll.Content(_sessFolders.pickedPanel);
            body.Children().Append(pickedScroll);
        }

        // --- row 5: [ ☐ Include subfolders ............ Clear ]
        Grid footer;
        {
            ColumnDefinition c0;
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition c1;
            c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            footer.ColumnDefinitions().Append(c0);
            footer.ColumnDefinitions().Append(c1);
        }
        _sessFolders.subfoldersBox = CheckBox{};
        _sessFolders.subfoldersBox.Content(winrt::box_value(winrt::hstring{ L"Include subfolders" }));
        _sessFolders.subfoldersBox.MinWidth(0);
        _sessFolders.subfoldersBox.IsChecked(_sessFolders.includeSubfolders);
        AgentSetTitledTip(_sessFolders.subfoldersBox,
                          L"Include subfolders",
                          L"Off: a picked folder lists the sessions that worked in exactly that folder \x2014 the counts in the lists. On: also every session that worked in a folder below it, so K:\\source covers all of its repositories. Kept until you change it, also across Clear.",
                          kSessFoldersTipDelay);
        _sessFolders.subfoldersBox.Click([this](const winrt::Windows::Foundation::IInspectable& s, const RoutedEventArgs&) {
            const auto box = s.try_as<CheckBox>();
            const bool on = box && box.IsChecked() && box.IsChecked().Value();
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), on]() {
                if (const auto self = weak.get())
                {
                    self->_SetSessionsFoldersIncludeSubfolders(on);
                }
            });
        });
        Grid::SetColumn(_sessFolders.subfoldersBox, 0);
        footer.Children().Append(_sessFolders.subfoldersBox);

        _sessFolders.clearBtn = Button{};
        _sessFolders.clearBtn.Content(winrt::box_value(winrt::hstring{ L"Clear" }));
        _sessFolders.clearBtn.VerticalAlignment(VerticalAlignment::Center);
        AgentSetTitledTip(_sessFolders.clearBtn, L"Clear", L"Stop filtering by folder: every picked folder is dropped. Include subfolders keeps its setting.", kSessFoldersTipDelay);
        _sessFolders.clearBtn.Click([this](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
                if (const auto self = weak.get())
                {
                    self->_ClearSessionsFolderFilter();
                }
            });
        });
        Grid::SetColumn(_sessFolders.clearBtn, 1);
        footer.Children().Append(_sessFolders.clearBtn);
        body.Children().Append(footer);

        // --- the card: an Agentmaster dark surface (Conventions), the range popup's look. Escape anywhere
        // in it closes the dropdown; Tab cycles inside it and the arrow keys move between its controls
        // (XY focus), so the rows are reachable from the keyboard without a list control of our own.
        _sessFolders.card = Border{};
        _sessFolders.card.RequestedTheme(ElementTheme::Dark);
        _sessFolders.card.Background(SessBrush(0xFF, 0x26, 0x26, 0x26));
        _sessFolders.card.BorderBrush(SessBrush(0xFF, 0x3A, 0x3A, 0x3A));
        _sessFolders.card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _sessFolders.card.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 6, 6, 6, 6 });
        _sessFolders.card.Padding(Thickness{ 10, 8, 10, 8 });
        _sessFolders.card.TabFocusNavigation(winrt::Windows::UI::Xaml::Input::KeyboardNavigationMode::Cycle);
        _sessFolders.card.XYFocusKeyboardNavigation(winrt::Windows::UI::Xaml::Input::XYFocusKeyboardNavigationMode::Enabled);
        _sessFolders.card.Child(body);
        _sessFolders.card.KeyDown([this](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == VirtualKey::Escape)
            {
                e.Handled(true);
                _CloseSessionsFoldersPopup(true);
            }
        });

        _sessFolders.popup = Primitives::Popup{};
        _sessFolders.popup.Child(_sessFolders.card);
        // Top/left aligned in the host's first row, so HorizontalOffset / VerticalOffset are host-relative
        // (the range popup's recipe — _OpenSessionsFoldersPopup reads the toggle's position in the host).
        _sessFolders.popup.HorizontalAlignment(HorizontalAlignment::Left);
        _sessFolders.popup.VerticalAlignment(VerticalAlignment::Top);
        Grid::SetRow(_sessFolders.popup, 0);
        Grid::SetRowSpan(_sessFolders.popup, 2);
        host.Children().Append(_sessFolders.popup);

        // --- outside-press dismissal: ONE handledEventsToo hook on the page host (the Tags panel's recipe —
        // pointer-based, never focus-based). A press whose source chain reaches the card is inside; a press
        // on the toggle is left to the toggle's own Click (which closes an open dropdown); anything else on
        // the page closes it. Closing a popup is not a tree mutation, so doing it inside the handler is safe.
        if (!_sessFolders.outsideHooked)
        {
            _sessFolders.outsideHooked = true;
            host.AddHandler(
                UIElement::PointerPressedEvent(),
                winrt::box_value(winrt::Windows::UI::Xaml::Input::PointerEventHandler{ [weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
                    const auto self = weak.get();
                    if (!self || !self->_sessFolders.popup || !self->_sessFolders.popup.IsOpen())
                    {
                        return;
                    }
                    auto d = e.OriginalSource().try_as<DependencyObject>();
                    while (d)
                    {
                        if (const auto b = d.try_as<Border>(); b && b == self->_sessFolders.card)
                        {
                            return; // inside the card
                        }
                        if (const auto t = d.try_as<Primitives::ToggleButton>(); t && t == self->_sessFolders.button)
                        {
                            return; // the toggle: its Click closes the dropdown (closing here would let the Click reopen it)
                        }
                        d = VisualTreeHelper::GetParent(d);
                    }
                    self->_CloseSessionsFoldersPopup(false);
                } }),
                true /* handledEventsToo — the table rows and buttons handle their own presses */);
        }

        _UpdateSessionsFoldersButton();
    }

    // The toggle's click: open the dropdown when it is closed, close it when it is open. The focus stays
    // on the toggle when it closes (the user just clicked it). UI thread only.
    // Returns nothing. Throws: what the open / close throw (see them).
    void TerminalPage::_ToggleSessionsFoldersPopup()
    {
        if (_sessFolders.popup && _sessFolders.popup.IsOpen())
        {
            _CloseSessionsFoldersPopup(false);
            return;
        }
        _OpenSessionsFoldersPopup();
    }

    // Open the dropdown under the toggle: placed host-relative (TransformToVisual — the toggle moves when
    // the window narrows), kept inside the page's right edge, its halves sized to the room below it, the
    // find box emptied and the lists rebuilt from the current rows; the find box gets the focus one tick
    // after the open (on the FIRST open the card was only just realized, and a same-tick Focus no-ops —
    // the Tags panel's lesson). No-op before the page is built. UI thread only.
    // Returns nothing.
    // Throws: hresult_error from the XAML calls and std::bad_alloc; the placement itself is contained (an
    // unmeasured toggle keeps the default spot) and logged (Rule #18).
    void TerminalPage::_OpenSessionsFoldersPopup()
    {
        if (!_sessFolders.popup || !_sessFolders.button || !_sessFolders.lists || !_sessionsPageHost)
        {
            return;
        }
        double x = 8.0;
        double y = 80.0;
        try
        {
            const auto pt = _sessFolders.button.TransformToVisual(_sessionsPageHost).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
            x = pt.X;
            y = pt.Y + _sessFolders.button.ActualHeight() + 4;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"Sessions folder dropdown: placement under the toggle");
        }
        const double hostW = _sessionsPageHost.ActualWidth();
        const double hostH = _sessionsPageHost.ActualHeight();
        const double cardW = kSessFoldersBodyWidth + 22.0; // + the card's padding and border on both sides
        if (hostW > 0 && x + cardW > hostW - 8.0)
        {
            x = hostW - 8.0 - cardW; // never past the page's right edge...
        }
        x = std::max(0.0, x); // ...and never past its left edge on a window narrower than the card
        // The halves get the default height, or what is left between the card's top and the page's bottom
        // once the find row, hint, chips and footer are paid for — never less than two rows per half.
        double listsH = kSessFoldersListsHeight;
        if (hostH > 0)
        {
            listsH = std::clamp(hostH - y - kSessFoldersChromeHeight, kSessFoldersListsMinHeight, kSessFoldersListsHeight);
        }
        _sessFolders.lists.Height(listsH);
        _sessFolders.popup.HorizontalOffset(x);
        _sessFolders.popup.VerticalOffset(y);
        if (_sessFolders.findBox && !_sessFolders.findBox.Text().empty())
        {
            _sessFolders.findBox.Text(L""); // a fresh open lists every folder (the queued re-list this raises is a harmless repeat)
        }
        if (_sessFolders.subfoldersBox)
        {
            _sessFolders.subfoldersBox.IsChecked(_sessFolders.includeSubfolders);
        }
        _RebuildSessionsFolderLists();
        _sessFolders.popup.IsOpen(true);
        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak()]() {
            if (const auto self = weak.get())
            {
                if (self->_sessFolders.popup && self->_sessFolders.popup.IsOpen() && self->_sessFolders.findBox)
                {
                    self->_sessFolders.findBox.Focus(FocusState::Programmatic);
                }
            }
        });
    }

    // Close the dropdown: its open tooltips first (a popup's tips do not close with it), then the popup,
    // and the page's description panel (it may still describe a row of the closed list). A closed or
    // never-built dropdown is a no-op, so every dismiss site may call it blindly.
    //   refocusSearch - true when the focus was inside the card (Escape): hand it to the page's search
    //                   box, the page's resting focus, instead of letting XAML pick a new home for it.
    // Returns nothing. UI thread only.
    // Throws: hresult_error from the XAML calls (the tooltip sweep is contained and logged — Rule #18).
    void TerminalPage::_CloseSessionsFoldersPopup(bool refocusSearch)
    {
        if (!_sessFolders.popup || !_sessFolders.popup.IsOpen())
        {
            return;
        }
        try
        {
            if (_sessFolders.card)
            {
                SessCloseTipsIn(_sessFolders.card);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"Sessions folder dropdown: close its tooltips");
        }
        _sessFolders.popup.IsOpen(false);
        _sessionsLocalTip.Hide();
        if (refocusSearch && _sessionsSearchBox)
        {
            _sessionsSearchBox.Focus(FocusState::Programmatic);
        }
    }

    // Re-list both halves from the gathered rows and the find text: the folder universe (every row the
    // table could list — never an AI-only row, a hidden row only while "Hidden" reveals them), filtered by
    // the find terms, in the two orders; then the headers, the hint, the "+" and (through
    // _RefreshSessionsFolderMarks) the check marks, the chips and the toggle. Also records what Enter adds
    // (topMatch) and the sessions' spellings (knownSpelling). No-op before the dropdown is built.
    // UI thread only; never from inside a pointer handler of a row (it replaces the rows).
    // Returns nothing. Throws: hresult_error from the XAML builds and std::bad_alloc.
    void TerminalPage::_RebuildSessionsFolderLists()
    {
        if (!_sessFolders.recentList || !_sessFolders.usedList || !_sessFolders.recentHeader || !_sessFolders.usedHeader)
        {
            return;
        }
        SessCloseTipsIn(_sessFolders.recentList); // the rows about to be cleared must not orphan an open tip
        SessCloseTipsIn(_sessFolders.usedList);
        _sessFolders.recentList.Children().Clear();
        _sessFolders.usedList.Children().Clear();

        // The universe: what the table could list — the hidden set stays out unless "Hidden" reveals it (a
        // folder whose every session is hidden would otherwise list a folder that shows nothing).
        const std::unordered_set<std::wstring> hidden(_appSettings.hiddenSessionIds.begin(), _appSettings.hiddenSessionIds.end());
        const bool showHidden = _sessHiddenBtn && _sessHiddenBtn.IsChecked() && _sessHiddenBtn.IsChecked().Value();
        std::vector<::Agentmaster::SessionFolderFacts> facts;
        facts.reserve(_sessionsRows.size());
        for (const auto& r : _sessionsRows)
        {
            if (r.aiExtra || (!showHidden && !hidden.empty() && hidden.count(r.id) != 0))
            {
                continue;
            }
            facts.push_back(::Agentmaster::SessionFolderFacts{ r.dir, r.inferredDir, r.lastActivityMs });
        }
        const auto folders = ::Agentmaster::CollectSessionFolders(facts);
        _sessFolders.knownSpelling.clear();
        for (const auto& f : folders)
        {
            _sessFolders.knownSpelling.emplace(f.key, f.path);
        }

        const std::wstring findText = _sessFolders.findBox ? std::wstring{ _sessFolders.findBox.Text() } : std::wstring{};
        const auto terms = ::Agentmaster::SplitFolderFindTerms(findText);
        std::vector<::Agentmaster::SessionFolderUse> recent;
        for (const auto& f : folders)
        {
            if (::Agentmaster::FolderMatchesFindTerms(f, terms))
            {
                recent.push_back(f);
            }
        }
        auto used = recent;
        ::Agentmaster::SortFoldersByRecency(recent);
        ::Agentmaster::SortFoldersByUse(used);
        _sessFolders.topMatch = recent.empty() ? std::wstring{} : recent.front().path;

        std::unordered_set<std::wstring> pickedKeys;
        for (const auto& p : _sessionsRowFilter.folders)
        {
            pickedKeys.insert(::Agentmaster::NormDirKey(p));
        }
        const int64_t now = SessNowMs();
        // One row: [✓][folder name][full path, dim][meta]. A Button, so it is focusable and Enter / Space
        // click it; its Tag carries the folder KEY, which _RefreshSessionsFolderMarks matches to re-mark it
        // in place (no rebuild — a rebuild would drop the focus of a keyboard user). Its click is deferred
        // one tick: the toggle re-renders the table behind the dropdown.
        const auto makeRow = [&](const ::Agentmaster::SessionFolderUse& f, const std::wstring& meta) -> Button {
            const bool on = pickedKeys.count(f.key) != 0;
            Grid g;
            {
                const auto col = [&](double v, GridUnitType t) {
                    ColumnDefinition c;
                    c.Width(GridLengthHelper::FromValueAndType(v, t));
                    g.ColumnDefinitions().Append(c);
                };
                col(18, GridUnitType::Pixel); // the check mark
                col(0, GridUnitType::Auto); // the folder's name
                col(1, GridUnitType::Star); // its full path
                col(0, GridUnitType::Auto); // the meta (age / count)
            }
            auto check = SessText(winrt::hstring{ on ? L"\x2713" : L"" }, 12, true, 1.0);
            check.Foreground(SessBrush(0xFF, 0x8A, 0xB4, 0xF8));
            Grid::SetColumn(check, 0);
            g.Children().Append(check); // child 0 — _RefreshSessionsFolderMarks finds the mark here
            std::wstring name = SessLeaf(f.path);
            if (name.empty())
            {
                name = f.path; // a drive root's "leaf" is the root itself
            }
            auto nameText = SessText(winrt::hstring{ name }, 12, true, 0.95);
            nameText.MaxWidth(220);
            nameText.Margin(Thickness{ 0, 0, 8, 0 });
            Grid::SetColumn(nameText, 1);
            g.Children().Append(nameText);
            auto pathText = SessText(winrt::hstring{ f.path }, 11, false, 0.55);
            Grid::SetColumn(pathText, 2);
            g.Children().Append(pathText);
            auto metaText = SessText(winrt::hstring{ meta }, 11, false, 0.7);
            metaText.Margin(Thickness{ 8, 0, 0, 0 });
            Grid::SetColumn(metaText, 3);
            g.Children().Append(metaText);

            Button b;
            b.Content(g);
            b.HorizontalAlignment(HorizontalAlignment::Stretch);
            b.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            b.Padding(Thickness{ 6, 3, 6, 3 });
            b.MinHeight(0);
            b.BorderThickness(Thickness{ 0, 0, 0, 0 });
            b.Background(on ? SessBrush(0x30, 0x60, 0xA0, 0xE0) : SessBrush(0, 0, 0, 0)); // the table's selected-row tint
            b.Tag(winrt::box_value(winrt::hstring{ f.key }));
            std::wstring tip = f.path + L"\n";
            tip += std::to_wstring(f.sessions) + (f.sessions == 1 ? L" session" : L" sessions") + L" worked here: ";
            tip += std::to_wstring(f.launched) + L" launched here";
            if (f.inferredOnly > 0)
            {
                tip += L", " + std::to_wstring(f.inferredOnly) + L" launched elsewhere and inferred here from their tool calls";
            }
            tip += L".";
            if (const auto when = SessLocalDateTime(f.lastActivityMs); !when.empty())
            {
                tip += L"\nLast active " + SessAgo(f.lastActivityMs, now) + L" ago (" + when + L").";
            }
            tip += on ? L"\n\nPicked. Click to remove it from the filter." : L"\n\nClick to show its sessions. Pick several folders and a session in ANY of them is listed.";
            AgentSetTitledTip(b, winrt::hstring{ name }, winrt::hstring{ tip }, kSessFoldersTipDelay);
            const winrt::hstring folder{ f.path };
            b.Click([this, folder](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), folder]() {
                    if (const auto self = weak.get())
                    {
                        self->_ToggleSessionsFolderFilter(std::wstring{ folder });
                    }
                });
            });
            return b;
        };
        for (size_t i = 0; i < recent.size() && i < kSessFoldersMaxRows; ++i)
        {
            const auto ago = SessAgo(recent[i].lastActivityMs, now);
            _sessFolders.recentList.Children().Append(makeRow(recent[i], ago.empty() ? std::wstring{} : (ago == L"now" ? ago : ago + L" ago")));
        }
        for (size_t i = 0; i < used.size() && i < kSessFoldersMaxRows; ++i)
        {
            _sessFolders.usedList.Children().Append(makeRow(used[i], std::to_wstring(used[i].sessions) + (used[i].sessions == 1 ? L" session" : L" sessions")));
        }

        // The headers: the half's name, its order, and how many folders it holds (of how many, when the
        // find text or the row cap narrows it).
        const auto headerText = [&](const wchar_t* name, const wchar_t* order) {
            std::wstring t = std::wstring{ name } + L"  \x00B7  " + order + L"  \x00B7  ";
            const size_t shown = std::min(recent.size(), kSessFoldersMaxRows);
            t += (shown != folders.size()) ? (std::to_wstring(shown) + L" of " + std::to_wstring(folders.size())) : std::to_wstring(shown);
            t += folders.size() == 1 ? L" folder" : L" folders";
            return winrt::hstring{ t };
        };
        _sessFolders.recentHeader.Text(headerText(L"RECENT", L"newest activity first"));
        _sessFolders.usedHeader.Text(headerText(L"MOST USED", L"most sessions first"));

        // The hint: the most useful single line — what Enter adds, why a list is empty, or that inferred
        // folders are still being found (they then appear on their own; the pass re-lists an open dropdown).
        const std::wstring pick = _SessionsFolderFindPick();
        std::wstring hint;
        if (_sessionsRows.empty())
        {
            hint = _sessionsIndexing.load() ? L"The sessions are still being read\x2026" : L"No session in this time window \x2014 widen it with the window button.";
        }
        else if (!terms.empty() && recent.empty())
        {
            hint = L"No folder matches \x201C" + ::Agentmaster::TrimFolderPath(findText) + L"\x201D.";
            if (!pick.empty())
            {
                hint += L" Enter adds the path itself: " + pick;
            }
        }
        else if (!pick.empty())
        {
            hint = L"Enter adds " + pick;
        }
        else
        {
            hint = L"Click folders to pick them \x2014 a session in ANY picked folder is listed. Each session counts in its launch folder and in the folder it was inferred to work in.";
        }
        if (_sessionsInferPasses.load() > 0)
        {
            hint += L"  (Still finding where some sessions worked\x2026)";
        }
        _sessFolders.hint.Text(winrt::hstring{ hint });
        if (_sessFolders.addBtn)
        {
            _sessFolders.addBtn.Visibility(pick.empty() ? Visibility::Collapsed : Visibility::Visible);
            if (!pick.empty())
            {
                AgentSetTitledTip(_sessFolders.addBtn, L"Add folder", winrt::hstring{ L"Add " + pick + L" to the folder filter (same as Enter)." }, kSessFoldersTipDelay);
            }
        }
        _RefreshSessionsFolderMarks();
    }

    // After a pick change: re-mark the existing rows IN PLACE (check mark + tint, matched by the folder key
    // in each row's Tag — no rebuild, so a keyboard user's focused row stays focused), rebuild the picked
    // chips, enable Clear, and update the toggle. Safe before the dropdown is built (only the toggle then).
    // UI thread only; never from inside a pointer handler of a chip (it replaces the chips).
    // Returns nothing. Throws: hresult_error from the XAML calls and std::bad_alloc.
    void TerminalPage::_RefreshSessionsFolderMarks()
    {
        _UpdateSessionsFoldersButton();
        if (!_sessFolders.recentList || !_sessFolders.usedList || !_sessFolders.pickedPanel)
        {
            return;
        }
        std::unordered_set<std::wstring> pickedKeys;
        for (const auto& p : _sessionsRowFilter.folders)
        {
            pickedKeys.insert(::Agentmaster::NormDirKey(p));
        }
        for (const auto& list : { _sessFolders.recentList, _sessFolders.usedList })
        {
            for (const auto& child : list.Children())
            {
                const auto b = child.try_as<Button>();
                if (!b)
                {
                    continue;
                }
                const bool on = pickedKeys.count(std::wstring{ winrt::unbox_value_or<winrt::hstring>(b.Tag(), L"") }) != 0;
                b.Background(on ? SessBrush(0x30, 0x60, 0xA0, 0xE0) : SessBrush(0, 0, 0, 0));
                if (const auto g = b.Content().try_as<Grid>(); g && g.Children().Size() > 0)
                {
                    if (const auto mark = g.Children().GetAt(0).try_as<TextBlock>())
                    {
                        mark.Text(on ? L"\x2713" : L"");
                    }
                }
            }
        }

        // The chips: one per picked folder, in pick order — also the one place a picked folder that no
        // list shows (a typed path, a folder outside the find text) can be seen and removed.
        SessCloseTipsIn(_sessFolders.pickedPanel);
        _sessFolders.pickedPanel.Children().Clear();
        if (_sessionsRowFilter.folders.empty())
        {
            _sessFolders.pickedPanel.Children().Append(SessText(L"No folder picked \x2014 every folder's sessions are listed.", 11, false, 0.5));
        }
        for (const auto& folder : _sessionsRowFilter.folders)
        {
            std::wstring name = SessLeaf(folder);
            if (name.empty())
            {
                name = folder;
            }
            Button chip;
            chip.Content(winrt::box_value(winrt::hstring{ name + L"  \x2715" }));
            chip.MinWidth(0);
            chip.MinHeight(0);
            chip.FontSize(11);
            chip.Padding(Thickness{ 8, 1, 8, 2 });
            chip.Background(SessBrush(0x42, 0x00, 0x78, 0xD4)); // the tag chips' translucent blue: a picked filter value
            AgentSetTitledTip(chip, winrt::hstring{ name }, winrt::hstring{ folder + L"\n\nClick to remove this folder from the filter." }, kSessFoldersTipDelay);
            const winrt::hstring chipFolder{ folder };
            chip.Click([this, chipFolder](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                // Deferred: the removal rebuilds this very chip row (and the table).
                Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), chipFolder]() {
                    if (const auto self = weak.get())
                    {
                        self->_ToggleSessionsFolderFilter(std::wstring{ chipFolder });
                    }
                });
            });
            _sessFolders.pickedPanel.Children().Append(chip);
        }
        if (_sessFolders.clearBtn)
        {
            _sessFolders.clearBtn.IsEnabled(!_sessionsRowFilter.folders.empty());
        }
    }

    // Add or remove ONE folder in the folder filter's OR set (filesystem-aware identity — Rule #8, so a case
    // or slash variant of a picked folder removes it), then refresh what shows the set: the ✕ filter chip,
    // the toggle, the dropdown's marks and chips, and the table. Deliberately unlogged, like every
    // view-filter facet.
    //   folder - the folder as listed or typed (trimmed here; empty is a no-op).
    // Returns nothing. UI thread only. Throws: what the refreshes throw.
    void TerminalPage::_ToggleSessionsFolderFilter(const std::wstring& folder)
    {
        const std::wstring display = ::Agentmaster::TrimFolderPath(folder);
        if (display.empty())
        {
            return;
        }
        const std::wstring key = ::Agentmaster::NormDirKey(display);
        auto& picked = _sessionsRowFilter.folders;
        const auto it = std::find_if(picked.begin(), picked.end(), [&](const std::wstring& p) { return ::Agentmaster::NormDirKey(p) == key; });
        if (it != picked.end())
        {
            picked.erase(it);
        }
        else
        {
            picked.push_back(display);
        }
        _UpdateSessionsFilterChip();
        _RefreshSessionsFolderMarks();
        _RenderSessionsTable();
    }

    // The find box's Enter / "+": ADD what _SessionsFolderFindPick names (never removes — a folder already
    // picked stays picked), then empty the box so the full lists return with the new mark. A re-list that a
    // keystroke queued is run FIRST, so the pick is computed from the text the user sees, not from the
    // lists of one keystroke earlier.
    // Returns nothing. UI thread only. Throws: what the refreshes throw.
    void TerminalPage::_AddSessionsFolderFromFindBox()
    {
        if (_sessFolders.listsQueued)
        {
            _sessFolders.listsQueued = false; // the queued tick sees this and does nothing
            _RebuildSessionsFolderLists();
        }
        const std::wstring pick = _SessionsFolderFindPick();
        if (pick.empty())
        {
            return;
        }
        const std::wstring key = ::Agentmaster::NormDirKey(pick);
        auto& picked = _sessionsRowFilter.folders;
        const bool already = std::any_of(picked.begin(), picked.end(), [&](const std::wstring& p) { return ::Agentmaster::NormDirKey(p) == key; });
        if (!already)
        {
            picked.push_back(pick);
            _UpdateSessionsFilterChip();
            _RenderSessionsTable();
        }
        if (_sessFolders.findBox)
        {
            _sessFolders.findBox.Text(L""); // queues the full re-list (TextChanged)
        }
        _RefreshSessionsFolderMarks();
    }

    // What the find box's Enter would add. A typed ABSOLUTE path (a drive or UNC folder — IsAbsoluteFolderPath)
    // is added as itself, in the sessions' own spelling when a listed folder has the same key (so the
    // chip reads like the list); any other text adds the top RECENT match of the last re-list.
    //   returns - the folder to add; empty when the box is empty or nothing matches.
    // UI thread only. Throws: std::bad_alloc.
    std::wstring TerminalPage::_SessionsFolderFindPick() const
    {
        if (!_sessFolders.findBox)
        {
            return {};
        }
        const std::wstring typed = ::Agentmaster::TrimFolderPath(std::wstring{ _sessFolders.findBox.Text() });
        if (typed.empty())
        {
            return {};
        }
        if (::Agentmaster::IsAbsoluteFolderPath(typed))
        {
            const auto it = _sessFolders.knownSpelling.find(::Agentmaster::NormDirKey(typed));
            return it != _sessFolders.knownSpelling.end() ? it->second : typed;
        }
        return _sessFolders.topMatch;
    }

    // The dropdown's Clear: drop every picked folder. "Include subfolders" keeps its setting (a preference,
    // not a pick). No-op when nothing is picked.
    // Returns nothing. UI thread only. Throws: what the refreshes throw.
    void TerminalPage::_ClearSessionsFolderFilter()
    {
        if (_sessionsRowFilter.folders.empty())
        {
            return;
        }
        _sessionsRowFilter.folders.clear();
        _UpdateSessionsFilterChip();
        _RefreshSessionsFolderMarks();
        _RenderSessionsTable();
    }

    // The "Include subfolders" box: store the preference, then refresh the ✕ chip (it says "+ subfolders")
    // and the toggle's tip; the table re-renders only when a folder is picked (otherwise nothing changes).
    //   on - the box's new state.
    // Returns nothing. UI thread only. Throws: what the refreshes throw.
    void TerminalPage::_SetSessionsFoldersIncludeSubfolders(bool on)
    {
        if (_sessFolders.includeSubfolders == on)
        {
            return;
        }
        _sessFolders.includeSubfolders = on;
        _UpdateSessionsFilterChip();
        _UpdateSessionsFoldersButton();
        if (!_sessionsRowFilter.folders.empty())
        {
            _RenderSessionsTable();
        }
    }

    // The toggle's face: "📂 Folders ▾" with nothing picked, "📂 <name> ▾" with one folder (a long name ends
    // in "…"), "📂 N folders ▾" with several; CHECKED (the accent fill) while any folder is picked; and its
    // tip, which names the picked folders in full. No-op before the toggle is built.
    // Returns nothing. UI thread only. Throws: hresult_error from the XAML calls, std::bad_alloc.
    void TerminalPage::_UpdateSessionsFoldersButton()
    {
        if (!_sessFolders.button)
        {
            return;
        }
        const auto& picked = _sessionsRowFilter.folders;
        std::wstring label = L"\U0001F4C2 ";
        if (picked.empty())
        {
            label += L"Folders";
        }
        else if (picked.size() == 1)
        {
            std::wstring name = SessLeaf(picked.front());
            if (name.empty())
            {
                name = picked.front();
            }
            if (name.size() > kSessFoldersButtonNameMax)
            {
                name = name.substr(0, kSessFoldersButtonNameMax - 1) + L"\x2026";
            }
            label += name;
        }
        else
        {
            label += std::to_wstring(picked.size()) + L" folders";
        }
        label += L" \x25BE";
        _sessFolders.button.Content(winrt::box_value(winrt::hstring{ label }));
        _sessFolders.button.IsChecked(!picked.empty());

        std::wstring tip = L"Show only the sessions that worked in the folders you pick \x2014 where a session was launched, or where its tool calls show it actually worked (its inferred folder). Pick several and a session in ANY of them is listed; the folder filter still combines with the search and every other filter. The list has two halves: the folders of your most recent sessions on top, the folders the most sessions worked in below.";
        if (!picked.empty())
        {
            tip += L"\n\nPicked:";
            for (const auto& p : picked)
            {
                tip += L"\n\x2022 " + p;
            }
            if (_sessFolders.includeSubfolders)
            {
                tip += L"\n(and the folders below them)";
            }
        }
        SessSetTip(_sessFolders.button, L"Folders", winrt::hstring{ tip });
    }

    // The follow-up INFERENCE pass of a gather: for each session the gather could not take from the cache,
    // infer the folder its tool calls concentrate in — off-thread, with the tab colors' rule (machine temp
    // excluded, InferWorkingDirectory, the git arm through ONE GitRootMemo for the whole pass) — then, on
    // the UI thread, store every answer in the cache, write them into the rows when the rows still belong
    // to the same gather, and repaint what shows them: the table (or, with the page hidden, a repaint owed
    // to its return), the selected row's detail pane, an open dropdown. Logs one [sessions-folders] line.
    //   jobs      - the gather's cache misses (moved in; each holds a copy of its tool paths).
    //   useGit    - AppSettings::inferGitRoot when the gather started (also the cache key's git half).
    //   gatherGen - the _sessionsGatherGen of that gather: results reach only its rows.
    // Returns nothing (fire_and_forget). Never lets an exception out: the whole body is contained and
    // logged (Rule #18 — an escape would std::terminate the app), and the running-pass count is given back
    // on every path.
    winrt::fire_and_forget TerminalPage::_InferSessionsRowDirs(std::vector<_SessionsInferJob> jobs, bool useGit, uint64_t gatherGen)
    {
        auto weakThis{ get_weak() };
        const auto dispatcher = Dispatcher(); // taken before the hop: after it, `this` may only be reached through weakThis
        _sessionsInferPasses.fetch_add(1);
        bool counted = true; // the pass still holds its count (given back once, on whichever path ends it)
        try
        {
            co_await winrt::resume_background();

            const auto started = std::chrono::steady_clock::now();
            // Temp scratch never votes (the tab-color scan's rule: a session's scratchpad files would infer
            // the scratchpad), collected once for the whole pass.
            const std::vector<std::wstring> tempRoots = ::Agentmaster::CollectMachineTempRoots();
            // The git arm: FindGitRootForDir's walk with every probed ancestor memoized, so sibling paths of
            // thousands of sessions cost one GetFileAttributesW per NEW folder (6,442 probes for a month here).
            ::Agentmaster::GitRootMemo gitMemo([](const std::wstring& dir) {
                return ::GetFileAttributesW((dir + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES;
            });
            std::function<std::wstring(const std::wstring&)> gitRootOf;
            if (useGit)
            {
                gitRootOf = [&gitMemo](const std::wstring& dir) { return gitMemo.RootOf(dir); };
            }
            std::vector<std::pair<std::wstring, _SessionsInferredDir>> results;
            results.reserve(jobs.size());
            for (auto& job : jobs)
            {
                auto paths = std::move(job.paths);
                ::Agentmaster::ExcludePathsUnderRoots(paths, tempRoots);
                std::wstring inferred = ::Agentmaster::InferWorkingDirectory(paths, job.workingDir, gitRootOf);
                if (::Agentmaster::NormDirKey(inferred) == ::Agentmaster::NormDirKey(job.workingDir))
                {
                    inferred.clear(); // the same folder as the launch folder: stored empty (the registry's convention)
                }
                results.emplace_back(std::move(job.id), _SessionsInferredDir{ job.sizeBytes, job.mtimeMs, useGit, std::move(inferred) });
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[sessions-folders] inferred " + std::to_wstring(results.size()) + L" session folder(s) in " + std::to_wstring(ms) +
                                              L" ms (" + std::to_wstring(gitMemo.Probes()) + L" .git probe(s), git arm " + (useGit ? L"on" : L"off") + L")\n");

            co_await winrt::resume_foreground(dispatcher);
            const auto self = weakThis.get();
            if (!self)
            {
                co_return; // the page is gone: nothing to give the count back to
            }
            self->_sessionsInferPasses.fetch_sub(1);
            counted = false;

            std::unordered_map<std::wstring, std::wstring> byId;
            byId.reserve(results.size());
            for (auto& [id, entry] : results)
            {
                byId.emplace(id, entry.inferred); // copied BEFORE the entry moves into the cache
                self->_sessionsInferredDirCache.insert_or_assign(id, std::move(entry));
            }
            const bool dropdownOpen = self->_sessFolders.popup && self->_sessFolders.popup.IsOpen();
            if (self->_sessionsGatherGen != gatherGen)
            {
                // A newer gather replaced the rows: its own lookup (or its own pass) answers for them. The
                // cache kept these answers, valid for exactly the (size, mtime) they read.
                if (dropdownOpen)
                {
                    self->_RebuildSessionsFolderLists(); // only the "still finding" hint may have changed
                }
                co_return;
            }
            bool changed = false;
            bool selectedChanged = false;
            for (auto& r : self->_sessionsRows)
            {
                if (r.aiExtra)
                {
                    continue;
                }
                const auto it = byId.find(r.id);
                if (it == byId.end() || r.inferredDir == it->second)
                {
                    continue;
                }
                r.inferredDir = it->second;
                changed = true;
                selectedChanged = selectedChanged || r.id == self->_sessionsSelectedId;
            }
            if (dropdownOpen)
            {
                self->_RebuildSessionsFolderLists(); // new folders, and the hint's "still finding" goes away
            }
            if (!changed)
            {
                co_return;
            }
            if (self->_sessionsPageVisible.load(std::memory_order_relaxed))
            {
                self->_RenderSessionsTable(); // folder matches + row tips ("Works in → …")
                if (selectedChanged)
                {
                    self->_ShowSessionsDetail(self->_sessionsSelectedId); // its "works in" line
                }
            }
            else
            {
                self->_sessFolders.renderOwed = true; // repainted when the page returns (the overlay's onRestore)
            }
        }
        catch (...)
        {
            if (counted)
            {
                if (const auto self = weakThis.get())
                {
                    self->_sessionsInferPasses.fetch_sub(1); // atomic: safe from whichever thread threw
                }
            }
            ::Agentmaster::AgentLogCaughtException(L"_InferSessionsRowDirs");
        }
    }

    // ===== the generic window-level page-overlay seam (_agentPageOverlays) ===================
    // Cross-page infrastructure (it lives in this TU as the newest page's home): each full-window
    // page registers ONCE at build; any global dismiss site — today the tab-switch handler in
    // TabManagement.cpp — closes ALL of them without naming any page, so a future page binds
    // automatically by registering.

    void TerminalPage::_RegisterAgentPageOverlay(const Grid& host, std::atomic<bool>* visibleMirror, std::function<void()> onDismiss, std::function<void()> onRestore)
    {
        _agentPageOverlays.push_back(_AgentPageOverlay{ host, visibleMirror, std::move(onDismiss), std::move(onRestore) });
    }

    void TerminalPage::_DismissAgentPageOverlays()
    {
        for (auto& p : _agentPageOverlays)
        {
            // Collapse only — the tree is NOT torn down, so a logically-open page keeps its typed search,
            // gathered rows, and selection in memory for _RestoreAgentPageOverlays to bring back (in
            // memory, no persistence). Run onDismiss BEFORE collapsing so it can snapshot live view state
            // (e.g. the table scroll offset) while the page is still laid out. The "should I re-open on
            // return" decision is the page's own restoreOnReturn intent (set by its Show/Hide), NOT this
            // transient collapse — so a Resume's synchronous tab-switch dismiss can't out-race a Hide.
            if (p.onDismiss)
            {
                p.onDismiss(); // e.g. close an owned Popup (a collapsed host does NOT hide those) + snapshot scroll
            }
            if (p.host)
            {
                p.host.Visibility(Visibility::Collapsed);
            }
            if (p.visibleMirror)
            {
                p.visibleMirror->store(false, std::memory_order_relaxed); // mirror == ON-SCREEN, now false
            }
        }
    }

    void TerminalPage::_RestoreAgentPageOverlays()
    {
        // The counterpart to _DismissAgentPageOverlays: when the Agent Manager page is shown again, bring back
        // any full-window page (e.g. Sessions) that is logically OPEN (restoreOnReturn — set by the page's
        // Show, cleared by its Hide). The page was merely COLLAPSED (never rebuilt), so its in-memory tree
        // — typed search text, gathered rows, selection — is intact; onRestore re-applies the transient
        // view state a collapse drops (scroll + focus).
        for (auto& p : _agentPageOverlays)
        {
            if (!p.restoreOnReturn)
            {
                continue;
            }
            if (p.host)
            {
                p.host.Visibility(Visibility::Visible);
            }
            if (p.visibleMirror)
            {
                p.visibleMirror->store(true, std::memory_order_relaxed);
            }
            if (p.onRestore)
            {
                p.onRestore();
            }
        }
    }

    void TerminalPage::_SetAgentPageOverlayOpenIntent(std::atomic<bool>* visibleMirror, bool open)
    {
        // A page's Show/Hide records whether _RestoreAgentPageOverlays should re-open it on return to the
        // Agent Manager page. Keyed by the page's visibility mirror (its stable identity in the registry). This is
        // the authoritative "logically open" bit — independent of the transient collapse/re-show, so an
        // explicit Hide (e.g. Resume landing on a new tab) always wins over a concurrent tab-switch.
        for (auto& p : _agentPageOverlays)
        {
            if (p.visibleMirror == visibleMirror)
            {
                p.restoreOnReturn = open;
                break;
            }
        }
    }
}
