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
//   TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Manager tab, _WireAgentManagerContent
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
    void TerminalPage::_ResumeSessionFromDisk(const std::wstring& sessionId, const std::wstring& dir, const std::wstring& title)
    {
        if (!_sessionRegistry || sessionId.empty() || dir.empty())
        {
            return;
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
            return;
        }
        // Native-exe-only policy gate (auto-recovering): everything past here LAUNCHES a claude (seed an
        // archived-shaped record, then _RestoreArchivedSession --resume), so gate + prompt here — BEFORE
        // seeding a phantom record we'd otherwise leave behind. The already-live branch above only jumps
        // to an existing tab, so it stays ungated.
        if (!::Agentmaster::EnsureClaudeAvailable())
        {
            ::Agentmaster::LogNav(L"sessions resume-done (blocked \x2014 no native claude.exe; install prompt shown)"); // pair the BEGIN so an unpaired begin always means a crash, never the gate
            _PromptClaudeMissing();
            return;
        }
        if (!existing)
        {
            ::Agentmaster::SessionInfo s;
            s.id = target;
            s.workingDir = dir;
            s.title = title;
            s.state = ::Agentmaster::SessionState::Idle;
            s.live = false; // archived-shaped: exactly what _RestoreArchivedSession expects
            _sessionRegistry->Upsert(std::move(s));
        }
        const auto resumedTab = _RestoreArchivedSession(winrt::hstring{ target });
        // Nav audit END (pairs with the resume-click BEGIN above): the actually-opened id. Normally ==
        // target; a resume whose transcript vanished degrades to a FRESH id (Rule #6), reported here. A
        // resume-click with neither a "resume -> jump" nor a resume-done means the launch crashed mid-open.
        const std::wstring resumedId = resumedTab ? _ClaudeSessionForTab(resumedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"sessions resume-done " + (resumedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(resumedId) + (resumedId == target ? std::wstring{} : (L" (target was " + ::Agentmaster::ShortId(target) + L" \x2014 fresh)")))));
        if (!_openClaudeTabInBackground)
        {
            _HideSessionsPage(); // foreground: land on the freshly opened tab. Background bulk-open keeps the list open.
        }
    }

    // Fork ANY on-disk session into a NEW managed conversation — the duplicate-tab fork's exact
    // recipe (TabManagement.cpp): `claude --resume <parent> --fork-session --session-id <new>`
    // (BuildClaudeSpawn mints the new id, so hooks/registry correlate from the first event), the
    // parent transcript untouched. Transcript-gated: a parent with no transcript (or one the
    // cleanup sweep deleted mid-view) degrades to a FRESH session in the same dir rather than
    // dying on "No conversation found". Safe on a LIVE parent — the fork writes its own file.
    void TerminalPage::_ForkSessionFromDisk(const std::wstring& parentId, const std::wstring& dir, const std::wstring& title, const std::wstring& modelOverride)
    {
        if (!_sessionRegistry || parentId.empty() || dir.empty())
        {
            return;
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
            return;
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
        const auto forkedTab = _LaunchClaudeSession(winrt::hstring{ forkDir }, winrt::hstring{ ttl }, std::nullopt, forkFrom, static_cast<uint32_t>(-1), modelOverride);
        // Nav audit END (pairs with the fork-click BEGIN above): the NEW forked id — exactly "the id of the
        // new forked session" the user asked to be able to trace, alongside the source it forked from. A
        // fork-click with NO matching fork-done means the launch crashed/hung between the two (the
        // crash-resilience the begin/end pairing buys). The id is resolved from the just-created tab, so it
        // reflects what actually opened (a no-transcript parent degrades to a FRESH session, noted here).
        const std::wstring forkedId = forkedTab ? _ClaudeSessionForTab(forkedTab) : std::wstring{};
        ::Agentmaster::LogNav(L"sessions fork-done " + (forkedId.empty() ? std::wstring{ L"(no tab \x2014 launch skipped/failed)" } : (L"new=" + ::Agentmaster::ShortId(forkedId))) + L" from=" + ::Agentmaster::ShortId(forkParentId) + (forkFrom.empty() ? std::wstring{ L" (fresh \x2014 no parent transcript)" } : std::wstring{}));
        if (!_openClaudeTabInBackground)
        {
            _HideSessionsPage(); // foreground: land on the fork. Background bulk-open keeps the list open.
        }
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
            _ForkSessionFromDisk(sessionId, dir, forkTitle);
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
        // The counterpart to _DismissAgentPageOverlays: when the Manager tab is re-selected, bring back
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
        // Manager tab. Keyed by the page's visibility mirror (its stable identity in the registry). This is
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
