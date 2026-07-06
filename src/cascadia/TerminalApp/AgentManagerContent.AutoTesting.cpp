// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster Manager tab content -- C1 'Linked Lenses' (7 partial files)
// The pinned leftmost tab's UI (DESIGN section 9): a Triage Board + Explorer Tree + Auto Testing over
// ONE shared SessionRegistry, built imperatively. ONE class (AgentManagerContent) split from the
// former 10864-line .cpp into by-area TUs that share AgentManagerContent.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentManagerContent.cpp             - CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh
//   AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
// ★ AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
//   AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster Manager tab: the FLIGHT PLAN -- plan rebuild + selection sync (managed + external read-only), prompt compose/history, the Autorunner toggle, the Summary tab, and plan templates. Partial TU of AgentManagerContent.cpp.
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentStatusColors.h" // ParseArgbHexColor / FormatArgbHexColor — the cog's "status flashing color" picker <-> AppSettings::flashRingColor
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/ProfileBootstrap.h" // the cog's Profile row (active dir + Change… picker)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/Engine.h" // RecoverableWindows (the "Reopen Windows (N)" recover button)
#include "AgentMaster/ProcessInspect.h" // ReadTranscriptInfo (read-only Auto Testing of an external) + BringClaudeWindowToFront (EXTERNAL menu)
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd
#include "AgentMaster/Updater.h" // the in-app updater: the cog's "Check for updates" + the "vX available!" label

// Agentmaster: the build-stamped git commit + branch (the Settings page header). Generated into
// $(GeneratedFilesDir) by TerminalAppLib.vcxproj's AgentmasterGenerateBuildInfo target, which is
// on the include path. The __has_include guard + fallback defines keep this file compilable if
// the generator hasn't run yet (e.g. opened in an IDE before any build); a real build always
// regenerates the header first (BeforeTargets ClCompile).
#if __has_include("AgentmasterBuildInfo.g.h")
#include "AgentmasterBuildInfo.g.h"
#endif
#ifndef AGENTMASTER_COMMIT_HASH
#define AGENTMASTER_COMMIT_HASH L"unknown"
#endif
#ifndef AGENTMASTER_COMMIT_BRANCH
#define AGENTMASTER_COMMIT_BRANCH L"unknown"
#endif

#include <algorithm>
#include <chrono>
#include <cmath> // std::pow — relative-luminance black/white contrast pick for the card title band
#include <filesystem> // create_directories — the "Create & Launch" affordance for a not-yet-existing dir
#include <system_error> // std::error_code — non-throwing create_directories
#include <thread> // background transcript read for an external's read-only plan
#include <shobjidl.h> // IFileOpenDialog — Browse for claude.exe (native-exe-only policy)

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with our Text() TextBlock helper below.
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using winrt::Windows::UI::Core::CoreCursor;
using winrt::Windows::UI::Core::CoreCursorType;
using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input; // KeyRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue, VirtualKey
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace Agentmaster;
// The shared tooltip recipe (AgentTipHelpers.h) — a using-DECLARATION so the file-scope
// helpers below (e.g. TimingText) can call it unqualified too.
using winrt::TerminalApp::implementation::AgentSetTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace winrt::TerminalApp::implementation
{
    void AgentManagerContent::_RebuildPlan(const std::vector<SessionInfo>& sessions)
    {
        _planHeaderHost.Children().Clear();
        _planListHost.Children().Clear();

        // EXTERNAL scope: the Auto Testing is READ-ONLY (externals are observe-only — we host no
        // ConPTY, so nothing to drive). With an external selected, show its conversation's prompts;
        // with none selected, the nothing-selected hint.
        if (_treeScope == TreeScope::External)
        {
            _UpdateAutorunnerButton(AutorunnerMode::Off, false); // not drivable
            if (!_selectedExternalTitle.empty())
            {
                _RebuildExternalPlan();
            }
            else
            {
                _planHeaderHost.Children().Append(Text(L"External claudes are observe-only \x2014 click one in the tree to see its conversation (read-only), or right-click to Adopt / Open New Session Here / Bring Window To Front.", 13, false, 0.6));
                _PinPlanToBottomOnSubjectChange(L""); // nothing scrollable shown — re-arm for the next real selection
            }
            return;
        }

        const auto sel = _Selected(sessions);
        if (!sel || !sel->live) // an archived (closed) session isn't planned here — restore it first
        {
            _planHeaderHost.Children().Append(Text(L"Select a session to plan its prompts.", 13, false, 0.6));
            _UpdateAutorunnerButton(AutorunnerMode::Off, false); // no live session: dim the header toggle
            _PinPlanToBottomOnSubjectChange(L""); // re-arm so re-selecting a session pins to bottom again
            return;
        }

        // header
        auto titleRow = StackPanel{};
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        titleRow.Children().Append(Text(OneLine(sel->title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ sel->title }), 16, true, 1.0));
        {
            auto statePill = Pill(StateLabel(sel->state), StateColor(sel->state));
            AgentSetTip(statePill, L"Current state \x2014 this session's Triage state (hover a Triage Board column header for what each state means).");
            titleRow.Children().Append(statePill);
        }
        _planHeaderHost.Children().Append(titleRow);
        _planHeaderHost.Children().Append(Text(winrt::hstring{ _WorkDirOf(*sel) }, 12, false, 0.6)); // the EFFECTIVE work dir — matches the tree group / board card / tab color

        // reflect autorunner mode on the header toggle
        _UpdateAutorunnerButton(sel->autorunner.mode, true);

        // SemiAuto one-click confirm banner (the scheduler armed the next prompt).
        if (!sel->pendingConfirmPromptId.empty())
        {
            winrt::hstring label;
            for (const auto& p : sel->queue)
            {
                if (p.id == sel->pendingConfirmPromptId)
                {
                    label = p.label.empty() ? winrt::hstring{ p.text } : winrt::hstring{ p.label };
                    break;
                }
            }
            auto banner = StackPanel{};
            banner.Orientation(Orientation::Horizontal);
            banner.Spacing(8);
            banner.VerticalAlignment(VerticalAlignment::Center);
            banner.Children().Append(Text(L"\x2699 Tests Autorunner ready:", 12, true, 1.0));
            auto lbl = Text(label, 12, false, 0.9);
            lbl.MaxWidth(220);
            banner.Children().Append(lbl);
            auto sendBtn = Button{};
            sendBtn.Content(winrt::box_value(L"Send"));
            AgentSetTip(sendBtn, L"Semi-auto: send the next queued prompt that Tests Autorunner armed.");
            sendBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                if (_confirmHandler && !_selectedId.empty())
                {
                    _confirmHandler(winrt::hstring{ _selectedId }, true);
                }
            });
            auto skipBtn = Button{};
            skipBtn.Content(winrt::box_value(L"Skip"));
            AgentSetTip(skipBtn, L"Semi-auto: skip this armed prompt without sending it.");
            skipBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                if (_confirmHandler && !_selectedId.empty())
                {
                    _confirmHandler(winrt::hstring{ _selectedId }, false);
                }
            });
            banner.Children().Append(sendBtn);
            banner.Children().Append(skipBtn);

            auto bannerBorder = Border{};
            bannerBorder.Background(Fill(0x40, 0xDA, 0xA5, 0x20));
            bannerBorder.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            bannerBorder.Padding(Thickness{ 8, 4, 8, 4 });
            bannerBorder.Margin(Thickness{ 0, 6, 0, 0 });
            bannerBorder.Child(banner);
            _planHeaderHost.Children().Append(bannerBorder);
        }

        // The Auto Testing reflects EVERY message this session received — not only ones queued
        // here. Split the queue into a chronological "sent" summary (each tagged by origin:
        // queued-by-flight vs typed-straight-into-the-terminal) followed by the upcoming queue.
        std::vector<const QueuedPrompt*> sent;
        std::vector<const QueuedPrompt*> upcoming;
        for (const auto& p : sel->queue)
        {
            if (p.status == PromptStatus::Pending || p.status == PromptStatus::Held)
            {
                upcoming.push_back(&p);
            }
            else
            {
                sent.push_back(&p);
            }
        }
        // Order the summary by send time; an un-timestamped item (e.g. Skipped) sinks to the
        // bottom of the summary, just above the upcoming queue.
        std::stable_sort(sent.begin(), sent.end(), [](const QueuedPrompt* a, const QueuedPrompt* b) {
            const int64_t ka = a->sentAtUnixMs > 0 ? a->sentAtUnixMs : INT64_MAX;
            const int64_t kb = b->sentAtUnixMs > 0 ? b->sentAtUnixMs : INT64_MAX;
            return ka < kb;
        });

        if (sent.empty() && upcoming.empty())
        {
            _planListHost.Children().Append(Text(L"No messages yet. Type into the session, or queue one below.", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // re-arm: the first message that arrives will pin to bottom
            return;
        }

        // One clickable prompt row. `showOrigin` (the sent summary) swaps the gate badge for a
        // flight/typed chip so you can see which messages the Manager sent vs. you typed.
        auto appendRow = [&](const QueuedPrompt& p, bool showOrigin) {
            const bool selected = (p.id == _selectedPromptId);

            auto row = StackPanel{};
            row.Orientation(Orientation::Horizontal);
            row.Spacing(8);
            row.Children().Append(Text(PromptGlyph(p.status), 13, false, 0.9));
            auto lbl = Text(p.label.empty() ? winrt::hstring{ p.text } : winrt::hstring{ p.label }, 13, false, 1.0);
            lbl.MaxWidth(320);
            row.Children().Append(lbl);
            if (showOrigin)
            {
                const bool typed = (p.origin == PromptOrigin::Typed);
                // Amber "typed" (a human keystroke) vs. blue "flight" (queued + injected by us).
                auto originPill = Pill(typed ? winrt::hstring{ L"typed" } : winrt::hstring{ L"auto" },
                                       typed ? ColorHelper::FromArgb(0xFF, 0xD9, 0xA6, 0x2E) : ColorHelper::FromArgb(0xFF, 0x4F, 0x8B, 0xD0));
                AgentSetTip(originPill, typed ?
                                            winrt::hstring{ L"Typed \x2014 you typed this prompt straight into the terminal." } :
                                            winrt::hstring{ L"Auto \x2014 Agentmaster queued this prompt and sent it for you (Tests Autorunner or Send now)." });
                row.Children().Append(originPill);
            }
            else
            {
                row.Children().Append(Text(GateBadge(p.gate), 11, false, 0.5));
            }
            if (p.status == PromptStatus::Held)
            {
                row.Children().Append(Text(L"(held: agent asked a question)", 11, false, 0.6));
            }

            auto rowBtn = Button{};
            rowBtn.Content(row);
            rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
            rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
            rowBtn.Padding(Thickness{ 6, 3, 6, 3 });
            rowBtn.Margin(Thickness{ 0, 0, 0, 4 });
            rowBtn.Background(Fill(selected ? 0x40 : 0x14, 0x80, 0x80, 0x80));
            rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            const auto pid = p.id;
            rowBtn.Click([this, pid](const IInspectable&, const RoutedEventArgs&) {
                _selectedPromptId = pid;
                _NotifyLensChanged(); // M10
                _Refresh();
            });
            // Right-click menu: Copy (this prompt's text) on every row, plus queue ops (Move up /
            // Move down / Delete) on UPCOMING rows only (a sent/historical row can't be reordered).
            // showOrigin is true for the SENT summary, false for the UPCOMING queue.
            rowBtn.ContextFlyout(_MakePromptMenu(pid, !showOrigin));
            AgentSetTip(rowBtn, showOrigin ?
                                    winrt::hstring{ L"A message this session already received \x2014 right-click to copy it." } :
                                    winrt::hstring{ L"A queued prompt \x2014 click to select it; right-click to copy, move or delete it." });
            _planListHost.Children().Append(rowBtn);
        };

        // A small dim section caption (e.g. "SENT — 4  (1 typed)").
        auto caption = [&](const winrt::hstring& s, double topMargin) {
            auto c = Text(s, 11, true, 0.5);
            c.Margin(Thickness{ 0, topMargin, 0, 4 });
            _planListHost.Children().Append(c);
        };

        if (!sent.empty())
        {
            int typedCount = 0;
            for (const auto* p : sent)
            {
                if (p->origin == PromptOrigin::Typed)
                {
                    ++typedCount;
                }
            }
            caption(winrt::hstring{ L"SENT \x2014 " } + winrt::to_hstring(static_cast<int>(sent.size())) + L"  (" + winrt::to_hstring(typedCount) + L" typed)", 0.0);
            for (const auto* p : sent)
            {
                appendRow(*p, true);
            }
        }
        if (!upcoming.empty())
        {
            caption(winrt::hstring{ L"UPCOMING \x2014 " } + winrt::to_hstring(static_cast<int>(upcoming.size())), sent.empty() ? 0.0 : 8.0);
            for (const auto* p : upcoming)
            {
                appendRow(*p, false);
            }
        }
        // Default the view to the bottom (latest sent + the upcoming queue) the first time this
        // session's plan is shown — but not on a same-session _Refresh (keep the user's scroll).
        _PinPlanToBottomOnSubjectChange(sel->id);
    }

    // Agentmaster: the READ-ONLY Auto Testing for a selected EXTERNAL session — its conversation's
    // human prompts (read from the transcript on a background thread by _LoadExternalPlan). No
    // queue, no actions, no Autorunner: an external runs outside Agentmaster and we never drive it
    // (Rule #9/#13). The header offers Adopt as the path to make it controllable.
    void AgentManagerContent::_RebuildExternalPlan()
    {
        const bool isCodex = (_selectedExternalKind == ::Agentmaster::AgentKind::Codex);
        auto titleRow = StackPanel{};
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        titleRow.Children().Append(Text(_selectedExternalTitle.empty() ? winrt::hstring{ isCodex ? L"codex" : L"claude" } : winrt::hstring{ _selectedExternalTitle }, 16, true, 1.0));
        if (isCodex)
        {
            auto op = Pill(L"codex \x00B7 observe-only", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            AgentSetTip(op, L"Observe-only \x2014 a Codex session running outside Agentmaster: you can read its conversation but not drive it. Adopt it to take control.");
            titleRow.Children().Append(op);
        }
        else
        {
            auto op = Pill(L"external \x00B7 observe-only", Colors::Gray());
            AgentSetTip(op, L"Observe-only \x2014 a Claude session running outside Agentmaster: you can read its conversation but not drive it. Adopt it to take control.");
            titleRow.Children().Append(op);
        }
        _planHeaderHost.Children().Append(titleRow);
        if (!_selectedExternalCwd.empty())
        {
            _planHeaderHost.Children().Append(Text(winrt::hstring{ _selectedExternalCwd }, 12, false, 0.6));
        }
        // Observe-only here (we host no ConPTY). Both agents can be adopted from the tree's right-click
        // menu — Adopt offers Fork a copy (safe while the original runs) or Resume the same conversation.
        _planHeaderHost.Children().Append(Text(isCodex ? winrt::hstring{ L"Read-only \x2014 an OpenAI Codex session running outside Agentmaster. Right-click it in the tree and \x201C" L"Adopt\x201D to bring its conversation under management (Fork a copy, or Resume)." } : winrt::hstring{ L"Read-only \x2014 runs outside Agentmaster. Right-click it in the tree and \x201C" L"Adopt\x201D to bring its conversation under management (Fork a copy, or Resume)." }, 11, false, 0.5));

        // Agentmaster: the idle RECAP (away_summary) above the prompts — the same ">5-min what we did /
        // what's next" synthesis the MANAGED summary box shows, here for an OBSERVE-ONLY external. The
        // recap rides ExternalClaudeRow.recap, filled by the Fleet Observer from the transcript TAIL —
        // the SAME region the SessionScanner pulls a managed session's recap from (an external has no
        // scanner cursor, so the observer is its provider; see ProcessObserver). Looked up live by the
        // selected id from the cached rows, so it refreshes as the observer re-tails it. Shown in FULL
        // (wrapping), never length-capped, matching the managed summary box's "Recap:" section.
        for (const auto& exr : _externalClaudes)
        {
            if (exr.sessionId == _selectedExternalSessionId && !exr.recap.empty())
            {
                auto recapText = Text(winrt::hstring{ L"Recap: " + exr.recap }, 12, false, 0.85);
                recapText.TextWrapping(TextWrapping::Wrap);
                recapText.TextTrimming(TextTrimming::None);
                recapText.Margin(Thickness{ 0, 6, 0, 0 });
                AgentSetTip(recapText, L"Claude Code's idle recap (away_summary) \x2014 a >5-min \x201C" L"what we did / what's next\x201D synthesis, read from this session's transcript tail.");
                _planHeaderHost.Children().Append(recapText);
                break;
            }
        }

        if (_selectedExternalSessionId.empty())
        {
            _planListHost.Children().Append(Text(L"This external session hasn't been prompted yet \x2014 no conversation to show.", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // nothing scrollable
            return;
        }
        if (_externalPlanLoadedFor != _selectedExternalSessionId)
        {
            _planListHost.Children().Append(Text(L"Loading conversation\x2026", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // re-arm: pin once the prompts finish loading (the deferred render reaches the bottom call below)
            return;
        }
        if (_externalPlanPrompts.empty())
        {
            _planListHost.Children().Append(Text(L"No human prompts found in this conversation.", 12, false, 0.6));
            _PinPlanToBottomOnSubjectChange(L""); // nothing scrollable
            return;
        }

        const size_t total = _externalPlanPrompts.size();
        const size_t cap = 300; // bound the XAML we build for a very long conversation
        const size_t startIdx = (total > cap) ? (total - cap) : 0;
        {
            std::wstring capn = L"PROMPTS \x2014 " + std::to_wstring(total);
            if (startIdx > 0)
            {
                capn += L"  (showing last " + std::to_wstring(cap) + L")";
            }
            auto c = Text(winrt::hstring{ capn }, 11, true, 0.5);
            c.Margin(Thickness{ 0, 0, 0, 4 });
            _planListHost.Children().Append(c);
        }
        for (size_t i = startIdx; i < total; ++i)
        {
            const std::wstring& p = _externalPlanPrompts[i];
            std::wstring oneLine = p;
            const auto nl = oneLine.find_first_of(L"\r\n");
            if (nl != std::wstring::npos)
            {
                oneLine = oneLine.substr(0, nl);
            }
            if (oneLine.size() > 200)
            {
                oneLine = oneLine.substr(0, 197) + L"\x2026";
            }

            auto rowSp = StackPanel{};
            rowSp.Orientation(Orientation::Horizontal);
            rowSp.Spacing(8);
            rowSp.Children().Append(Text(winrt::to_hstring(static_cast<int>(i + 1)), 11, false, 0.4));
            auto lbl = Text(winrt::hstring{ oneLine }, 13, false, 0.95);
            lbl.MaxWidth(360);
            rowSp.Children().Append(lbl);

            auto rowBorder = Border{};
            rowBorder.Padding(Thickness{ 6, 3, 6, 3 });
            rowBorder.Margin(Thickness{ 0, 0, 0, 4 });
            rowBorder.Background(Fill(0x14, 0x80, 0x80, 0x80));
            rowBorder.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            rowBorder.Child(rowSp);
            _planListHost.Children().Append(rowBorder);
        }
        _PinPlanToBottomOnSubjectChange(L"x:" + _selectedExternalSessionId);
    }

    // Agentmaster: scroll the Auto Testing to the bottom the FIRST time a subject is shown (a managed
    // session's plan, or an external's read-only conversation). The newest SENT message + the UPCOMING
    // queue live at the bottom, so a freshly-opened plan defaults to "where things stand" rather than
    // the oldest message. Gated on the subject CHANGING — a same-subject _Refresh (a background state
    // change, claude floating its OSC title, a queue edit) must keep the user's current scroll, so we
    // never re-pin while you're reading history. The scroll is deferred to a clean tick and forces a
    // layout pass first, because _planListHost was just (re)populated this frame and the ScrollViewer's
    // ScrollableHeight isn't valid until it re-measures.
    void AgentManagerContent::_PinPlanToBottomOnSubjectChange(const std::wstring& subjectKey)
    {
        if (subjectKey == _planAutoScrolledFor)
        {
            return; // same subject as last pin (or both empty) — leave the user's scroll alone
        }
        _planAutoScrolledFor = subjectKey;
        if (subjectKey.empty() || !_planScroll)
        {
            return; // nothing scrollable shown (a hint / loading / empty state)
        }
        auto weak = get_weak();
        auto disp = _dispatcher;
        if (!disp)
        {
            return;
        }
        disp.TryEnqueue([weak]() {
            auto self = weak.get();
            if (!self || !self->_planScroll)
            {
                return;
            }
            self->_planScroll.UpdateLayout(); // realize the rows appended this frame so ScrollableHeight is real
            self->_planScroll.ChangeView(nullptr, self->_planScroll.ScrollableHeight(), nullptr, true); // jump (no animation) to the bottom
        });
    }

    // ---- selection / scope --------------------------------------------------

    std::optional<SessionInfo> AgentManagerContent::_Selected(const std::vector<SessionInfo>& sessions) const
    {
        for (const auto& s : sessions)
        {
            if (s.id == _selectedId)
            {
                return s;
            }
        }
        return std::nullopt;
    }

    void AgentManagerContent::_SelectSession(const std::wstring& id)
    {
        // Agentmaster: selecting a managed session — by a card/row click OR a tab switch (the page's
        // SelectSession() routes here) — drops that session's working directory into the Launch box, so
        // "Launch Claude" / Open-New-Session is pre-aimed where you're working. Done BEFORE the
        // already-selected early-out so a re-select re-aims it. The box is not focused during a card
        // click / tab switch, so its TextChanged path-picker logic early-outs (it never pops the
        // dropdown). We must re-validate EXPLICITLY here, not lean on TextChanged: a programmatic
        // _cwdBox.Text() set lands while the Manager content is OFF the live visual tree (a tab switch
        // makes another tab active, and MUX TabView hosts only the selected tab's content). The Text DP
        // value persists (you see the right path on return), but TextChanged does NOT reliably fire while
        // detached — so without this call the underline/button stay frozen on the previously-typed
        // value's RED/disabled state even though a valid working dir now shows (the reported bug).
        if (_cwdBox && _registry && !id.empty())
        {
            // Pre-aim Launch / Open-New-Session with the session's EFFECTIVE work dir (_WorkDirOf):
            // under the Inferred mode a new session opens where the selected one actually WORKS,
            // matching the dir its card/row/tab color show — else the launch cwd, as before.
            if (const auto s = _registry->Get(id); s && !_WorkDirOf(*s).empty())
            {
                _cwdBox.Text(winrt::hstring{ _WorkDirOf(*s) });
                _ValidateLaunchBox();
            }
        }

        // Selecting a managed session clears any external (read-only) selection — the Auto Testing is
        // one surface; a managed selection wins (it is drivable).
        const bool hadExternal = !_selectedExternalSessionId.empty();
        if (_selectedId == id && !hadExternal)
        {
            return;
        }
        if (_selectedId != id)
        {
            _ResetPromptHistory(); // prompt history is per-session — a new session starts fresh
        }
        _selectedExternalSessionId.clear();
        _selectedExternalCwd.clear();
        _selectedExternalTitle.clear();
        _selectedId = id;
        _selectedPromptId.clear();
        _NotifyLensChanged(); // M10: selection is part of the per-window lens
        _Refresh();
    }

    // Agentmaster: the board header's "Clear" button — deselect whatever managed session OR external is
    // selected (the Auto Testing then shows nothing-selected). A no-op when nothing is selected. Clears
    // BOTH selection kinds at once (the Launch box is left as-is — it is an independent launch target).
    void AgentManagerContent::_ClearSelection()
    {
        if (_selectedId.empty() && _selectedExternalSessionId.empty())
        {
            return; // nothing selected
        }
        _ResetPromptHistory(); // no session selected -> no history to recall
        _selectedId.clear();
        _selectedPromptId.clear();
        _selectedExternalSessionId.clear();
        _selectedExternalCwd.clear();
        _selectedExternalTitle.clear();
        _NotifyLensChanged(); // selection is part of the per-window lens
        _Refresh();
    }

    // Agentmaster (Linked Lenses): a managed board card / tree row was entered or left by the
    // pointer. Forward the raw event (id, entering) to the page, which owns the effective-hover
    // bookkeeping (the enter-B-before-leave-A matching + the "clear on leaving the Manager tab"
    // reset) — so this control holds no hover state to desync from the page. The page pills that
    // session's terminal tab while the Manager tab is active. NOT part of the lens — hover is
    // transient and per-pointer, never persisted.
    void AgentManagerContent::_ReportHover(const std::wstring& id, bool entering)
    {
        if (_hoverSessionHandler)
        {
            _hoverSessionHandler(winrt::hstring{ id }, entering);
        }
    }

    // Agentmaster: select an EXTERNAL (observe-only) row -> the Auto Testing shows its conversation
    // READ-ONLY. We host no ConPTY for it (Rule #9/#13), so this never binds an injector; it only
    // surfaces what was prompted. Clears the managed selection (one Auto-Testing surface).
    void AgentManagerContent::_SelectExternal(const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& title, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath)
    {
        _ResetPromptHistory(); // an external's Auto Testing is read-only — no managed history to recall
        // Nav audit: the user selected an EXTERNAL (unmanaged) session to inspect — board External
        // card or Explorer-Tree EXTERNAL row — surfacing its read-only conversation. Distinct from a
        // managed select (no tab-focus equivalent — we host no tab for it).
        ::Agentmaster::LogNav(L"manager select-external " + ::Agentmaster::ShortId(sessionId) + L" cwd=" + cwd);
        _selectedId.clear();
        _selectedPromptId.clear();
        _selectedExternalSessionId = sessionId;
        _selectedExternalCwd = cwd;
        _selectedExternalTitle = title;
        // Agentmaster: like a managed select, aim the Launch box at this external's cwd (so Launch /
        // Open-New-Session here is one keystroke). Unfocused box => no path-picker pop, and re-validate
        // EXPLICITLY — a programmatic Text set on the (detached, non-active-tab) Manager content does not
        // reliably raise TextChanged, so the launch underline/button would otherwise stay stuck on a
        // previously-typed invalid value's RED/disabled state (see _SelectSession).
        if (_cwdBox && !cwd.empty())
        {
            _cwdBox.Text(winrt::hstring{ cwd });
            _ValidateLaunchBox();
        }
        _selectedExternalKind = kind; // Phase C1: the read-only plan reader (Claude transcript vs Codex rollout)
        _selectedExternalRolloutPath = rolloutPath;
        // Linked Lenses: selecting an external — from the Explorer Tree OR a Triage-Board External
        // card — puts all three regions in agreement. Switch the tree to EXTERNAL so it lists the
        // externals with this one highlighted, and the Auto Testing renders its read-only conversation
        // (its render is gated on EXTERNAL scope, see _RebuildPlan). A no-op when invoked from the
        // tree (already EXTERNAL); the meaningful case is a board card click from LOCAL/GLOBAL.
        if (_treeScope != TreeScope::External)
        {
            _treeScope = TreeScope::External;
            _UpdateTreeScopeButton();
            _UpdateBoardScopeButton(); // keep the board's 2-way toggle in step (External reads GLOBAL there) — it shares the one scope state
        }
        _LoadExternalPlan(sessionId, cwd, kind, rolloutPath); // kicks off the (cached) background transcript/rollout read
        _NotifyLensChanged();
        _Refresh();
    }

    // Read the external conversation's human prompts on a BACKGROUND thread (a transcript can be
    // multi-MB; never parse it on the UI thread), then post the result back via the dispatcher. Cached
    // per id (_externalPlanLoadedFor) so re-selecting the same external doesn't re-read. A session
    // with no transcript yet (empty id) loads nothing (the plan shows "not prompted yet").
    void AgentManagerContent::_LoadExternalPlan(const std::wstring& sessionId, const std::wstring& cwd, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath)
    {
        if (_externalPlanLoadedFor == sessionId && !sessionId.empty())
        {
            return; // already loaded for this id
        }
        _externalPlanPrompts.clear();
        _externalPlanLoadedFor.clear(); // empty => loading / none
        if (sessionId.empty())
        {
            return; // never-prompted external: nothing to read
        }
        auto weak = get_weak();
        auto disp = _dispatcher;
        std::thread([weak, disp, sessionId, cwd, kind, rolloutPath]() {
            // Whole transcript (maxBytes 0), cap the prompt count so a giant conversation stays
            // bounded. Codex (Phase C1) reads its date-sharded rollout (the path carried on the row);
            // Claude reads <projects>/<encode(cwd)>/<id>.jsonl. Both yield the human prompts in order.
            std::vector<std::wstring> prompts = (kind == ::Agentmaster::AgentKind::Codex)
                                                    ? ::Agentmaster::ReadCodexRolloutInfo(rolloutPath, 0, 1000).userPrompts
                                                    : ::Agentmaster::ReadTranscriptInfo(cwd, sessionId, 0, 1000).userPrompts;
            if (!disp)
            {
                return;
            }
            disp.TryEnqueue([weak, sessionId, prompts = std::move(prompts)]() mutable {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                // Stale guard: the user may have selected a different external while we were reading.
                if (self->_selectedExternalSessionId != sessionId)
                {
                    return;
                }
                self->_externalPlanLoadedFor = sessionId;
                self->_externalPlanPrompts = std::move(prompts);
                self->_Refresh();
            });
        }).detach();
    }

    void AgentManagerContent::_SetScope(const std::wstring& dir)
    {
        _scopeDir = dir;
        _NotifyLensChanged(); // M10: scope + collapsed-dir toggles funnel through here
        _Refresh();
    }

    // ---- action handlers ----------------------------------------------------

    // Agentmaster (Codex-launch): repaint the launch-bar agent toggle from _launchCodex. Codex wears the
    // SAME teal (0xFF4EC9B0) as every other codex surface (the EXTERNAL pill, the managed Board/tree pill),
    // so the agent reads one color everywhere; Claude is a neutral blue accent (the default, unchanged).
    void AgentManagerContent::_OnAddPrompt()
    {
        if (_selectedId.empty() || !_registry || !_addPromptBox)
        {
            return;
        }
        std::wstring text{ _addPromptBox.Text() };
        if (text.empty())
        {
            return;
        }
        std::wstring label = text.substr(0, 56);
        std::replace(label.begin(), label.end(), L'\n', L' ');
        std::replace(label.begin(), label.end(), L'\r', L' ');

        const auto id = _selectedId;
        _registry->Update(id, [&](SessionInfo& s) {
            QueuedPrompt p;
            p.id = NewSessionId();
            p.label = label;
            p.text = text;
            s.queue.push_back(std::move(p));
        });
        // Nav audit: the user QUEUED a prompt to this session (the envelope). The first line is the
        // identifying context; Autorunner/Send-now later consumes it (scheduler [send]/[confirm-send]).
        ::Agentmaster::LogNav(L"queue " + ::Agentmaster::ShortId(id) + L" \"" + label + L"\"");
        _addPromptBox.Text(L"");
        _Refresh();
        _FocusPromptBox(); // Agentmaster: keep focus in the editor so the user can queue the next prompt
    }

    void AgentManagerContent::_OnSendNow()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }
        // The "!" button ASKS before it fires — "Send now" injects into a live claude immediately.
        // Build a short preview of what WILL be sent (the compose box if non-empty, else the
        // selected / first-Pending queued prompt — the same target _DoSendNow picks), then confirm.
        std::wstring preview = _addPromptBox ? std::wstring{ _addPromptBox.Text() } : std::wstring{};
        if (preview.empty())
        {
            if (auto s = _registry->Get(_selectedId))
            {
                const QueuedPrompt* target = nullptr;
                if (!_selectedPromptId.empty())
                {
                    for (const auto& p : s->queue)
                    {
                        if (p.id == _selectedPromptId)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (!target)
                {
                    for (const auto& p : s->queue)
                    {
                        if (p.status == PromptStatus::Pending)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (target)
                {
                    preview = target->label.empty() ? target->text : target->label;
                }
            }
        }
        if (preview.empty())
        {
            return; // nothing composed and nothing pending to send
        }
        std::wstring shown = preview.substr(0, 200);
        std::replace(shown.begin(), shown.end(), L'\n', L' ');
        std::replace(shown.begin(), shown.end(), L'\r', L' ');
        auto weak = get_weak();
        _Confirm(L"Send now?",
                 winrt::hstring{ L"Send this prompt to the session right now?\n\n\x201C" } + winrt::hstring{ shown } + winrt::hstring{ L"\x201D" },
                 L"Send",
                 [weak]() { if (auto self = weak.get()) { self->_DoSendNow(); } });
    }

    // The actual inject for "Send now" (runs after the confirm).
    void AgentManagerContent::_DoSendNow()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }

        // "Send now" sends what's in the compose box (and records it in the Auto Testing as a
        // Sent item) so typing + Send is one intuitive action. With an empty box it instead
        // sends the selected (or first Pending) already-queued prompt.
        std::wstring composed = _addPromptBox ? std::wstring{ _addPromptBox.Text() } : std::wstring{};

        std::wstring textToSend;
        std::wstring sentPromptId; // Agentmaster: the prompt just marked Sent, for rollback on a failed inject
        if (!composed.empty())
        {
            std::wstring label = composed.substr(0, 56);
            std::replace(label.begin(), label.end(), L'\n', L' ');
            std::replace(label.begin(), label.end(), L'\r', L' ');
            _registry->Update(_selectedId, [&](SessionInfo& s) {
                QueuedPrompt p;
                p.id = ::Agentmaster::NewSessionId();
                p.label = label;
                p.text = composed;
                p.status = PromptStatus::Sent; // it's being sent right now
                p.attempts = 1;
                p.sentAtUnixMs = NowMs(); // timestamp so it sorts into the "sent" summary
                p.echoed = false; // await this injection's UserPromptSubmit echo (don't double-record)
                sentPromptId = p.id; // remember it so a failed inject can roll it back (Rule #4)
                s.queue.push_back(std::move(p));
            });
            textToSend = composed;
            if (_addPromptBox)
            {
                _addPromptBox.Text(L"");
            }
        }
        else
        {
            const auto promptId = _selectedPromptId;
            _registry->Update(_selectedId, [&](SessionInfo& s) {
                QueuedPrompt* target = nullptr;
                if (!promptId.empty())
                {
                    for (auto& p : s.queue)
                    {
                        if (p.id == promptId)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (!target)
                {
                    for (auto& p : s.queue)
                    {
                        if (p.status == PromptStatus::Pending)
                        {
                            target = &p;
                            break;
                        }
                    }
                }
                if (target)
                {
                    textToSend = target->text;
                    sentPromptId = target->id; // remember it so a failed inject can roll it back (Rule #4)
                    target->status = PromptStatus::Sent;
                    target->attempts += 1;
                    target->sentAtUnixMs = NowMs();
                    target->echoed = false; // await this injection's UserPromptSubmit echo
                    target->enterRetries = 0; // fresh send -> reset the scheduler's Enter-retry watch
                }
            });
        }

        if (!textToSend.empty())
        {
            // Inject + submit via a bracketed paste so a multi-line body lands as ONE message
            // (BuildPromptSubmission, #6) instead of submitting on the first embedded line break.
            // Agentmaster: check the result and roll the prompt back to Pending on a
            // failed inject. "Send now" marked it Sent above; if the selected session has no stdin
            // injector bound (not a live/bound tab yet, or an observe-only external), injecting fails
            // and the prompt would otherwise be a stranded phantom Sent that was never delivered
            // (Correctness Rule #4). Reverting to Pending keeps it in the queue to retry.
            const bool delivered = _registry->Inject(_selectedId, ::Agentmaster::BuildPromptSubmission(textToSend));
            // Nav audit: the user hit Send-now (the !) for this session — the prompt's first line +
            // whether it actually reached a bound injector (an unbound/observe-only target rolls back).
            std::wstring snLabel = textToSend.substr(0, 56);
            std::replace(snLabel.begin(), snLabel.end(), L'\n', L' ');
            std::replace(snLabel.begin(), snLabel.end(), L'\r', L' ');
            ::Agentmaster::LogNav(L"send-now " + ::Agentmaster::ShortId(_selectedId) + L" \"" + snLabel + L"\"" + (delivered ? L"" : L" (no injector \x2014 rolled back to Pending)"));
            if (!delivered && !sentPromptId.empty())
            {
                _registry->Update(_selectedId, [&](SessionInfo& s) {
                    for (auto& p : s.queue)
                    {
                        if (p.id == sentPromptId && p.status == PromptStatus::Sent)
                        {
                            p.status = PromptStatus::Pending;
                            p.echoed = false;
                            if (p.attempts > 0)
                            {
                                p.attempts -= 1;
                            }
                            break;
                        }
                    }
                });
            }
        }
        _Refresh();
        _FocusPromptBox(); // Agentmaster: return focus to the editor so the user can keep composing
    }

    // Agentmaster: return keyboard focus to the compose box after a queue/send — clicking the icon
    // button moved focus to it, so this lets the user immediately type the next prompt. Programmatic
    // focus places the caret in the box. A no-op if the box isn't present.
    void AgentManagerContent::_FocusPromptBox()
    {
        if (!_addPromptBox)
        {
            return;
        }
        // Defer the focus to the dispatcher. When this follows the Send-now ContentDialog confirm, the
        // dialog restores focus to its pre-open element (the "!" button) AS it closes — which happens
        // AFTER _DoSendNow (the PrimaryButtonClick callback) returns — so a synchronous Focus() here
        // would be clobbered. A queued focus runs after the click handler unwinds, so the compose box
        // keeps it. (The queue path has no dialog, so this is just a harmless one-tick delay there.)
        if (_dispatcher)
        {
            auto weak = get_weak();
            _dispatcher.TryEnqueue([weak]() {
                auto self = weak.get();
                if (self && self->_addPromptBox)
                {
                    self->_addPromptBox.Focus(FocusState::Programmatic);
                }
            });
        }
        else
        {
            _addPromptBox.Focus(FocusState::Programmatic);
        }
    }

    // Agentmaster (prompt history): the selected session's previously SENT prompts (Flight + Typed),
    // newest first, with consecutive duplicates collapsed (the shell HISTCONTROL=ignoredups idiom).
    // Pending/Held queue items are the FUTURE, not history, so they're excluded; only status==Sent
    // (which both an injected Flight prompt and a Typed-into-the-terminal capture carry) is history.
    std::vector<std::wstring> AgentManagerContent::_BuildPromptHistory() const
    {
        std::vector<std::wstring> out;
        if (_selectedId.empty() || !_registry)
        {
            return out;
        }
        const auto sel = _registry->Get(_selectedId);
        if (!sel)
        {
            return out;
        }
        std::vector<const QueuedPrompt*> sent;
        for (const auto& p : sel->queue)
        {
            if (p.status == PromptStatus::Sent && !p.text.empty())
            {
                sent.push_back(&p);
            }
        }
        // Newest first (descending send time); stable so equal-stamp items keep their queue order.
        std::stable_sort(sent.begin(), sent.end(), [](const QueuedPrompt* a, const QueuedPrompt* b) {
            return a->sentAtUnixMs > b->sentAtUnixMs;
        });
        for (const auto* p : sent)
        {
            if (out.empty() || out.back() != p->text)
            {
                out.push_back(p->text);
            }
        }
        return out;
    }

    // Agentmaster (prompt history): write a recalled body into the compose box and park the caret at
    // the end (ready to edit / Enter). The _promptHistoryNavigating latch suppresses the box's
    // TextChanged -> _ResetPromptHistory so this recall isn't mistaken for a user edit. The UWP
    // TextBox raises TextChanged SYNCHRONOUSLY from the Text setter, so a plain bool around the set
    // is enough (set -> Text() -> any re-entrant TextChanged no-ops -> cleared, one stack frame).
    void AgentManagerContent::_ApplyPromptHistoryText(const std::wstring& text)
    {
        if (!_addPromptBox)
        {
            return;
        }
        _promptHistoryNavigating = true;
        // Always clear the latch, even if a setter throws — otherwise the TextChanged reset would stay
        // disabled and a subsequent real edit wouldn't leave history navigation.
        try
        {
            _addPromptBox.Text(winrt::hstring{ text });
            const int32_t len = static_cast<int32_t>(text.size());
            _addPromptBox.SelectionStart(len); // caret to the end (collapsed selection)
            _addPromptBox.SelectionLength(0);
        }
        catch (...)
        {
        }
        _promptHistoryNavigating = false;
    }

    // Agentmaster (prompt history): leave navigation — drop the snapshot/draft and return to the
    // "live draft" state (index -1). Called when the user edits the box (TextChanged), it's cleared
    // after a send, or the selected session changes (history is per-session).
    void AgentManagerContent::_ResetPromptHistory()
    {
        _promptHistoryIndex = -1;
        _promptHistory.clear();
        _promptHistoryDraft.clear();
    }

    // Agentmaster (prompt history): is the caret on the FIRST VISUAL ROW of the compose box? This is
    // the enter-history gate on the draft. "Visual row" (not logical line) so that with word-wrap on, a
    // long first logical line that wraps to several rows still lets Up move the caret up a row before
    // recalling — Up only enters history at the very top row. GetRectFromCharacterIndex respects wrap;
    // we compare the caret's Y to the first character's Y (same top => first row). Falls back to the
    // logical-line scan (no \n/\r before the caret — a UWP TextBox uses \r, a programmatic set may leave
    // \n) if the rect API is unavailable. An empty box / caret at 0 counts as the first row.
    bool AgentManagerContent::_PromptCaretOnFirstRow() const
    {
        if (!_addPromptBox)
        {
            return false;
        }
        const std::wstring text{ _addPromptBox.Text() };
        const int32_t caret = _addPromptBox.SelectionStart();
        if (caret <= 0 || text.empty())
        {
            return true; // start of the box (or empty) is always the first row
        }
        try
        {
            const auto firstR = _addPromptBox.GetRectFromCharacterIndex(0, false);
            // For the caret use the leading edge of the char at it; at end-of-text use the trailing
            // edge of the last char (index == length is out of range for the leading-edge form).
            const auto caretR = (caret >= static_cast<int32_t>(text.size())) ?
                                    _addPromptBox.GetRectFromCharacterIndex(static_cast<int32_t>(text.size()) - 1, true) :
                                    _addPromptBox.GetRectFromCharacterIndex(caret, false);
            // The caret is at/below the first row, so its top is >= the first row's top; "same row" if
            // within ~half a line height (tolerant of sub-pixel/baseline differences).
            const double tol = firstR.Height > 0 ? firstR.Height * 0.5 : 2.0;
            return caretR.Y <= firstR.Y + tol;
        }
        catch (...)
        {
            const int32_t limit = std::min<int32_t>(caret, static_cast<int32_t>(text.size()));
            for (int32_t i = 0; i < limit; ++i)
            {
                if (text[i] == L'\n' || text[i] == L'\r')
                {
                    return false;
                }
            }
            return true;
        }
    }

    void AgentManagerContent::_OnMovePrompt(int delta)
    {
        if (_selectedId.empty() || _selectedPromptId.empty() || !_registry)
        {
            return;
        }
        const auto pid = _selectedPromptId;
        _registry->Update(_selectedId, [&](SessionInfo& s) {
            auto& q = s.queue;
            for (size_t i = 0; i < q.size(); ++i)
            {
                if (q[i].id == pid)
                {
                    const long j = static_cast<long>(i) + delta;
                    if (j >= 0 && j < static_cast<long>(q.size()))
                    {
                        std::swap(q[i], q[static_cast<size_t>(j)]);
                    }
                    break;
                }
            }
        });
        _Refresh();
    }

    void AgentManagerContent::_OnDeletePrompt()
    {
        if (_selectedId.empty() || _selectedPromptId.empty() || !_registry)
        {
            return;
        }
        const auto pid = _selectedPromptId;
        _registry->Update(_selectedId, [&](SessionInfo& s) {
            auto& q = s.queue;
            q.erase(std::remove_if(q.begin(), q.end(), [&](const QueuedPrompt& p) { return p.id == pid; }), q.end());
        });
        _selectedPromptId.clear();
        _Refresh();
    }

    void AgentManagerContent::_OnAutorunnerChanged(int index)
    {
        if (_suppressAutorunnerEvent || _selectedId.empty() || !_registry)
        {
            return;
        }
        const AutorunnerMode mode = index == 2 ? AutorunnerMode::Full : index == 1 ? AutorunnerMode::SemiAuto :
                                                                                   AutorunnerMode::Off;
        // Nav audit: the user changed this session's Autorunner mode (the Auto-Testing header toggle).
        ::Agentmaster::LogNav(L"autorunner " + ::Agentmaster::ShortId(_selectedId) + L" -> " + (mode == AutorunnerMode::Full ? L"Full" : mode == AutorunnerMode::SemiAuto ? L"Semi" : L"Off"));
        _registry->Update(_selectedId, [&](SessionInfo& s) {
            s.autorunner.mode = mode;
            if (mode != AutorunnerMode::Off)
            {
                // Arming resets the per-run backstop counter and clears any stale confirm.
                s.autorunner.autoSendsThisRun = 0;
                s.pendingConfirmPromptId.clear();
            }
        });
    }

    // Agentmaster: the FLIGHT-PLAN-header Autorunner toggle — advance the selected session's mode
    // (Off -> Semi-auto -> Full -> Off), reusing _OnAutorunnerChanged's apply logic.
    void AgentManagerContent::_CycleAutorunner()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }
        const auto s = _registry->Get(_selectedId);
        if (!s || !s->live)
        {
            return; // nothing live to drive (the button is disabled in this state anyway)
        }
        // Off(0) -> Semi-auto(1) -> Full(2) -> Off — same index order the old combo used.
        const int next = s->autorunner.mode == AutorunnerMode::Off ? 1 :
                                                                    (s->autorunner.mode == AutorunnerMode::SemiAuto ? 2 : 0);
        _OnAutorunnerChanged(next); // writes the registry + clears the per-run backstops
        _Refresh(); // repaint the header toggle now (the registry observer also refreshes)
    }

    // Paint the Autorunner toggle: a colored state dot (gray circle = Off, amber half = Semi, green
    // disc = Full) + a label. Dim + disabled when no live session is selected.
    void AgentManagerContent::_UpdateAutorunnerButton(AutorunnerMode mode, bool enabled)
    {
        if (!_autorunnerBtn)
        {
            return;
        }
        winrt::hstring glyph;
        winrt::hstring label;
        Color dot{};
        switch (mode)
        {
        case AutorunnerMode::Full:
            glyph = L"\x25CF"; // ●
            label = L"Tests Autorunner: Full";
            dot = Colors::MediumSeaGreen();
            break;
        case AutorunnerMode::SemiAuto:
            glyph = L"\x25D0"; // ◐
            label = L"Tests Autorunner: Semi";
            dot = Colors::Goldenrod();
            break;
        case AutorunnerMode::Off:
        default:
            glyph = L"\x25CB"; // ○
            label = L"Tests Autorunner: Off";
            dot = Colors::Gray();
            break;
        }
        auto row = StackPanel{};
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        auto g = Text(glyph, 12, true, enabled ? 1.0 : 0.4);
        g.Foreground(SolidColorBrush{ dot });
        row.Children().Append(g);
        row.Children().Append(Text(label, 11, false, enabled ? 0.95 : 0.5));
        _autorunnerBtn.Content(row);
        _autorunnerBtn.IsEnabled(enabled);
    }

    // Agentmaster: select the Auto-Testing pane's [Summary | Auto Testing] tab. The choice is a GLOBAL app
    // setting (AppSettings::autoTestingShowsSummary), so — exactly like the Explorer-Tree / Triage-Board
    // sort toggles — mutate _appSettings, re-paint, then push it through the settings sink (the page
    // persists settings.json + broadcasts it to every OTHER window, which adopt it in ApplyExternalSettings).
    // A no-op when unchanged, so re-clicking the active tab doesn't churn persistence/broadcast.
    void AgentManagerContent::_SelectPlanPaneTab(bool summary)
    {
        // Auto Testing is a DEV-ONLY feature: in a release build the pane is Summary-only and the toggle
        // is hidden, so this can only ever be invoked under the AgentmasterDev package. Defensive no-op
        // otherwise (a future caller / keybinding can't switch the release pane off Summary).
        if (!::Agentmaster::Profiles::IsDevPackage())
        {
            return;
        }
        if (_appSettings.autoTestingShowsSummary == summary)
        {
            return; // already on this tab
        }
        _appSettings.autoTestingShowsSummary = summary;
        _UpdatePlanPaneTab();
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // persist globally (settings.json) + broadcast to other windows
        }
    }

    // Agentmaster: reflect the current Auto-Testing pane tab — accent the selected segment (held through
    // hover via PaintHoldButton, the scope-toggle accent) + bold it, leave the other on default chrome,
    // and show ONLY that tab's body (the empty Summary host vs the Auto Testing body). No-op until the
    // controls exist, so it's safe to call from SetSettings before _BuildLayout has run.
    void AgentManagerContent::_UpdatePlanPaneTab()
    {
        if (!_summaryTabBtn || !_autoTestTabBtn)
        {
            return;
        }
        const bool summary = _appSettings.autoTestingShowsSummary;
        const auto paintSegment = [](const Button& b, bool selected) {
            if (selected)
            {
                PaintHoldButton(b, 0xFF356AB8, 0xFF3E7DCE, 0xFF2B5391); // accent blue, holds through hover/press
                b.FontWeight(FontWeights::SemiBold());
            }
            else
            {
                ClearHoldButton(b); // default subtle chrome = the inactive segment
                b.FontWeight(FontWeights::Normal());
            }
        };
        paintSegment(_summaryTabBtn, summary);
        paintSegment(_autoTestTabBtn, !summary);
        if (_summaryHost)
        {
            _summaryHost.Visibility(summary ? Visibility::Visible : Visibility::Collapsed);
        }
        if (_autoTestBody)
        {
            _autoTestBody.Visibility(summary ? Visibility::Collapsed : Visibility::Visible);
        }
        _RefreshSummaryTab(); // populate/refresh the Summary tab when it becomes active (self-guards otherwise)
    }

    // Agentmaster (Summary tab): show the selected managed Claude session's summary box (the SAME
    // RenderSessionSummaryBox the Sessions page + per-tab overlay render) in the Auto-Testing Summary
    // tab. Cheap + idempotent: early-returns unless the Summary tab is active; renders from the
    // single-entry cache when (id, mtime) is unchanged; otherwise kicks the off-thread analyze. The
    // user messages are reversed to newest-first; the whole box rides one inner scrollbar.
    void AgentManagerContent::_RefreshSummaryTab()
    {
        if (!_summaryBoxHost || !_appSettings.autoTestingShowsSummary)
        {
            return; // Summary tab not built, or not the active tab — nothing to do
        }
        // Subject = the selected MANAGED CLAUDE session. Codex / external / nothing-selected get a
        // placeholder (the Auto Testing tab still serves those). The analyze cache key is the registry's
        // convLastActivityUnixMs — the same "last activity" signal the timing adornment uses.
        std::wstring id, dir;
        int64_t mtime = 0;
        if (!_selectedId.empty() && _registry)
        {
            if (const auto s = _registry->Get(_selectedId); s && s->kind == ::Agentmaster::AgentKind::Claude)
            {
                id = _selectedId;
                dir = s->workingDir;
                mtime = s->convLastActivityUnixMs;
            }
        }
        if (id.empty())
        {
            // Nothing summarizable selected — render the placeholder ONCE (sentinel), not every refresh.
            if (_summaryShownId != L"\x01none")
            {
                _summaryBoxHost.Children().Clear();
                auto hint = Text(_selectedId.empty() ? L"Select a Claude session to see its summary." : L"Summary is shown for Claude sessions.", 12, false, 0.5);
                hint.TextWrapping(TextWrapping::Wrap);
                hint.Margin(Thickness{ 2, 10, 2, 0 });
                _summaryBoxHost.Children().Append(hint);
                _summaryShownId = L"\x01none";
                _summaryShownMtime = -1;
            }
            return;
        }
        // Warm cache for this exact (id, mtime): render it (unless it's already on screen).
        if (_summaryCacheId == id && _summaryCacheMtime == mtime)
        {
            if (_summaryShownId != id || _summaryShownMtime != mtime)
            {
                _RenderSummaryBox(_summaryCacheText);
                _summaryShownId = id;
                _summaryShownMtime = mtime;
            }
            return;
        }
        // Need a (re)analyze. Show a loading line ONLY when switching to a different subject — if THIS
        // id's box (an older mtime) is already on screen, keep showing it (no flash) until the new one
        // lands. A busy session's mtime keeps changing, so flashing a spinner each tick would churn.
        if (_summaryShownId != id && _summaryShownId != L"\x01loading")
        {
            _summaryBoxHost.Children().Clear();
            auto hint = Text(L"Loading summary\x2026", 12, false, 0.5);
            hint.Margin(Thickness{ 2, 10, 2, 0 });
            _summaryBoxHost.Children().Append(hint);
            _summaryShownId = L"\x01loading";
            _summaryShownMtime = -1;
        }
        if (_summaryLoadingId != id) // one in-flight analyze per id — a busy session can't stack loads
        {
            _LoadSummaryForSession(id, dir, mtime);
        }
    }

    // Off-thread analyze + render of a Claude session's summary box (the Sessions-page recipe), posted
    // back via the dispatcher. Messages are reversed to newest-first before rendering. Cached single-
    // entry by (id, mtime); a stale result (selection moved on, or the Summary tab was left) is dropped.
    void AgentManagerContent::_LoadSummaryForSession(const std::wstring& id, const std::wstring& dir, int64_t mtime)
    {
        _summaryLoadingId = id;
        auto weak = get_weak();
        auto disp = _dispatcher;
        std::thread([weak, disp, id, dir, mtime]() {
            std::wstring text;
            const std::wstring path = ::Agentmaster::ResolveClaudeTranscriptPath(id);
            if (!path.empty())
            {
                auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
                // Newest-first: reverse the chronological user messages so RenderSessionSummaryBox (which
                // numbers in vector order) lists 1 = the most recent. Only MESSAGES are reordered.
                std::reverse(a.userMsgs.begin(), a.userMsgs.end());
                std::wstring planFile = a.planFilePath;
                if (planFile.empty() && a.hasPlanContent && !a.parentSessionId.empty())
                {
                    // A plan-start session's plan file lives in its PARENT transcript (session-end.js).
                    const std::wstring parentPath = ::Agentmaster::ResolveClaudeTranscriptPath(a.parentSessionId);
                    if (!parentPath.empty())
                    {
                        planFile = ::Agentmaster::FindPlanFileInTranscript(parentPath);
                    }
                }
                // full=false: the trimmed, space-saving variant (the per-tab overlay's view) — omits the
                // id / Dir / Folder / Resume / Branch header (redundant for an already-selected session),
                // leaving the value-add (Recap, Tasks, Messages, Files). Fits the narrow pane.
                text = ::Agentmaster::RenderSessionSummaryBox(a, id, dir, path, L"claude --resume " + id, L"", L"", planFile, /*full*/ false);
            }
            if (!disp)
            {
                return;
            }
            disp.TryEnqueue([weak, id, mtime, text = std::move(text)]() {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                // Cache the analyze regardless of current selection (a quick re-select stays warm).
                self->_summaryCacheId = id;
                self->_summaryCacheMtime = mtime;
                self->_summaryCacheText = text;
                if (self->_summaryLoadingId == id)
                {
                    self->_summaryLoadingId.clear();
                }
                // Render only if the Summary tab is still active AND this id is still the selected subject.
                if (!self->_appSettings.autoTestingShowsSummary || self->_selectedId != id)
                {
                    return;
                }
                self->_RenderSummaryBox(text);
                self->_summaryShownId = id;
                self->_summaryShownMtime = mtime;
            });
        }).detach();
    }

    // Render a RenderSessionSummaryBox result into _summaryBoxHost — mirrors the Sessions page's
    // SessAppendSummaryBox: split on '\n'; a lone kSummarySepMark line becomes a full-width rule; every
    // other run becomes a monospace, wrapped, selectable TextBlock. Replaces the panel's content.
    void AgentManagerContent::_RenderSummaryBox(const std::wstring& text)
    {
        if (!_summaryBoxHost)
        {
            return;
        }
        _summaryBoxHost.Children().Clear();
        if (text.empty())
        {
            auto hint = Text(L"No messages recorded for this session yet.", 12, false, 0.5);
            hint.TextWrapping(TextWrapping::Wrap);
            hint.Margin(Thickness{ 2, 10, 2, 0 });
            _summaryBoxHost.Children().Append(hint);
            return;
        }
        std::wstring seg;
        const auto flush = [&]() {
            if (seg.empty())
            {
                return;
            }
            TextBlock tb{};
            tb.FontFamily(FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            tb.TextWrapping(TextWrapping::Wrap);
            tb.IsTextSelectionEnabled(true);
            tb.Opacity(0.85);
            tb.Text(winrt::hstring{ seg });
            _summaryBoxHost.Children().Append(tb);
            seg.clear();
        };
        size_t i = 0;
        while (i <= text.size())
        {
            const size_t nl = text.find(L'\n', i);
            const size_t end = (nl == std::wstring::npos) ? text.size() : nl;
            const std::wstring lineStr = text.substr(i, end - i);
            if (lineStr.size() == 1 && lineStr[0] == ::Agentmaster::kSummarySepMark)
            {
                flush(); // close the run above the rule
                Border rule{};
                rule.Height(1);
                rule.HorizontalAlignment(HorizontalAlignment::Stretch); // border to border
                rule.Background(Fill(0x40, 0xFF, 0xFF, 0xFF));
                rule.Margin(Thickness{ 0, 4, 0, 4 });
                _summaryBoxHost.Children().Append(rule);
            }
            else
            {
                if (!seg.empty())
                {
                    seg += L"\n";
                }
                seg += lineStr;
            }
            if (nl == std::wstring::npos)
            {
                break;
            }
            i = nl + 1;
        }
        flush();
    }

    void AgentManagerContent::_RefreshTemplateCombo()
    {
        if (!_templateCombo)
        {
            return;
        }
        _templateCombo.Items().Clear();
        for (const auto& t : _templates)
        {
            _templateCombo.Items().Append(winrt::box_value(winrt::hstring{ t.name }));
        }
        if (!_templates.empty())
        {
            _templateCombo.SelectedIndex(0);
        }
    }

    void AgentManagerContent::_OnSaveTemplate()
    {
        if (_selectedId.empty() || !_registry)
        {
            return;
        }
        const auto sel = _registry->Get(_selectedId);
        if (!sel || sel->queue.empty())
        {
            return;
        }
        std::wstring name = _templateNameBox ? std::wstring{ _templateNameBox.Text() } : std::wstring{};
        if (name.empty())
        {
            name = (sel->title.empty() ? std::wstring{ L"plan" } : sel->title) + L"-plan";
        }
        _templates.push_back(::Agentmaster::MakeTemplateFromQueue(name, sel->queue));
        ::Agentmaster::SaveTemplates(_templates);
        // Nav audit: the user saved the selected session's queue as a reusable plan template.
        ::Agentmaster::LogNav(L"template-save \"" + name + L"\" prompts=" + std::to_wstring(sel->queue.size()) + L" from=" + ::Agentmaster::ShortId(_selectedId));
        if (_templateNameBox)
        {
            _templateNameBox.Text(L"");
        }
        _RefreshTemplateCombo();
        if (_templateCombo && !_templates.empty())
        {
            _templateCombo.SelectedIndex(static_cast<int32_t>(_templates.size()) - 1);
        }
    }

    void AgentManagerContent::_OnApplyTemplate(bool toWholeDirectory)
    {
        if (!_registry || !_templateCombo)
        {
            return;
        }
        const auto idx = _templateCombo.SelectedIndex();
        if (idx < 0 || static_cast<size_t>(idx) >= _templates.size())
        {
            return;
        }
        const auto tmpl = _templates[static_cast<size_t>(idx)];

        if (!toWholeDirectory)
        {
            if (_selectedId.empty())
            {
                return;
            }
            _registry->Update(_selectedId, [&](SessionInfo& s) { ::Agentmaster::AppendTemplateToQueue(s.queue, tmpl); });
            // Nav audit: the user applied a template's prompts to THIS session's queue (consequential — it
            // adds what Autorunner will send; more than a single `queue`, so logged even though the per-prompt
            // queue micro-edits aren't).
            ::Agentmaster::LogNav(L"template-apply \"" + tmpl.name + L"\" prompts=" + std::to_wstring(tmpl.prompts.size()) + L" -> " + ::Agentmaster::ShortId(_selectedId));
            _FocusPromptBox(); // Agentmaster: return focus to the compose box after applying a template
            return;
        }

        // Apply-to-many: every session in the scoped (or selected) working directory.
        std::wstring dir = _scopeDir;
        if (dir.empty())
        {
            if (const auto sel = _Selected(_registry->Snapshot()))
            {
                dir = _WorkDirOf(*sel);
            }
        }
        if (dir.empty())
        {
            return;
        }
        int applied = 0;
        for (const auto& s : _registry->Snapshot())
        {
            // Match by the EFFECTIVE work dir (_WorkDirOf) — the same key the tree groups and the
            // scope filters by, so a broadcast hits exactly the sessions shown under that group.
            if (PathEq(_WorkDirOf(s), dir))
            {
                _registry->Update(s.id, [&](SessionInfo& ss) { ::Agentmaster::AppendTemplateToQueue(ss.queue, tmpl); });
                ++applied;
            }
        }
        // Nav audit: the user broadcast a template to EVERY session in a directory — the most consequential
        // template action (queues prompts across many sessions at once).
        ::Agentmaster::LogNav(L"template-apply \"" + tmpl.name + L"\" prompts=" + std::to_wstring(tmpl.prompts.size()) + L" -> dir=" + dir + L" sessions=" + std::to_wstring(applied));
        _FocusPromptBox(); // Agentmaster: return focus to the compose box after applying a template
    }

    // ---- Launch path-picker drop-down ---------------------------------------

}
