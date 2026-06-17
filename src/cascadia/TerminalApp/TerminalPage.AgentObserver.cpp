// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the Fleet Observer's UI lane + bind machinery on this window
// (OBSERVER.md §10/§11d; TAB_OVERLAY.md): publish this window's tab roster, bind
// unbound tabs via the correlation table (_BindClaudeSessionToTab — the shared tail
// of hook adoption AND observer discovery), the per-tab link-badge / observe-badge
// overlays, the scanner-ticked liveness sweep + bind reconcile, and the Explorer
// Tree's force-refresh.
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // GuidToPlainString (WT_SESSION keys)

#include "AgentManagerContent.h" // push the External census / RefreshNow
#include "AgentStatusColors.h" // AgentStatusColorFor — the shared state->color palette (tab dot)
#include "AgentTabOverlay.h" // build + own the per-tab overlays (complete com_ptr type)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog
#include "AgentMaster/Persistence.h" // DeriveSessionTitle / SaveSessions (bind tail)
#include "AgentMaster/ProcessObserver.h" // roster publish + Correlation/Activity/External tables
#include "AgentMaster/SessionRegistry.h"

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

namespace winrt::TerminalApp::implementation
{
    // Agentmaster (tab status dot): show/recolor (or hide, nullopt) the tab-strip status dot —
    // the "[icon] ● <title>" element TabHeaderControl.xaml binds to TerminalTabStatus. The dot is
    // the Explorer Tree's state dot brought onto the tab strip itself: state-colored for a managed
    // session, dim gray for an observed-only tab. It is a SEPARATE visual element, deliberately NOT
    // part of the title string — the one-title invariant (Rule #11: Explorer name == tab title ==
    // persisted SessionInfo.title) must never carry presentation glyphs through renames/persistence.
    // Independent of AppSettings.showTabOverlay (that toggle is the in-terminal HUD). Idempotent on
    // an unchanged color (a new SolidColorBrush per call would re-raise the INPC binding every
    // observer tick), so the per-tick badge path can re-assert it for free. UI thread only.
    void TerminalPage::_SetTabAgentDot(const TerminalApp::Tab& tab, const std::optional<winrt::Windows::UI::Color>& color)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto status = tab.TabStatus();
            if (!status)
            {
                return;
            }
            if (!color)
            {
                status.AgentStatusVisible(false); // WINRT_OBSERVABLE_PROPERTY no-ops when already false
                return;
            }
            if (status.AgentStatusVisible())
            {
                if (const auto cur = status.AgentStatusBrush().try_as<Media::SolidColorBrush>();
                    cur && cur.Color() == *color)
                {
                    return; // already showing exactly this color — don't churn the binding
                }
            }
            status.AgentStatusBrush(Media::SolidColorBrush{ *color });
            status.AgentStatusVisible(true);
        }
        CATCH_LOG();
    }

    // Agentmaster (tab status dot): the registry-observer reaction (bounced to this window's UI
    // thread by the engine-init observer). A session state change recolors its hosting tab's dot in
    // place; live=false hides it (the liveness sweep also hides explicitly before it drops the
    // _claudeTabs entry — whichever lands first wins, both are idempotent). A session this window
    // doesn't host is a cheap map-miss no-op (every window's observer sees every fleet event).
    void TerminalPage::_UpdateTabAgentDot(const std::wstring& sessionId, ::Agentmaster::SessionState state, bool live)
    {
        const auto it = _claudeTabs.find(sessionId);
        if (it == _claudeTabs.end())
        {
            return;
        }
        if (const auto tab = it->second.get())
        {
            _SetTabAgentDot(tab, live ? std::optional{ AgentStatusColorFor(state) } : std::nullopt);
        }
    }

    // Agentmaster (Linked Lenses): show/hide the "selected/active" pill behind a tab's header — the
    // filled accent background TabHeaderControl.xaml binds to TerminalTabStatus.AgentSelectionVisible
    // / AgentSelectionBrush. It makes a session's tab read as the active tab while you stay on the
    // Manager tab, WITHOUT changing the real TabView selection or the per-dir tab color (Rule #12) —
    // a separate presentation layer, exactly like the status dot. UI thread only.
    void TerminalPage::_SetTabSelectionPill(const TerminalApp::Tab& tab, bool on)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto status = tab.TabStatus();
            if (!status)
            {
                return;
            }
            if (!on)
            {
                status.AgentSelectionVisible(false); // WINRT_OBSERVABLE_PROPERTY no-ops when already false
                return;
            }
            // The "selected" fill: the system accent at partial alpha, so the title + dot stay legible
            // on top (and it reads as one consistent "this is the picked tab" tint regardless of the
            // tab's own dir color). Resolved the same way WT picks the active-pane border accent
            // (_updatePaneResources) — guard with HasKey, with a sane WT-blue fallback.
            auto accent = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x00, 0x78, 0xD4);
            const auto res = Application::Current().Resources();
            if (const auto accentKey = winrt::box_value(L"SystemAccentColor"); res.HasKey(accentKey))
            {
                accent = winrt::unbox_value_or<winrt::Windows::UI::Color>(res.Lookup(accentKey), accent);
            }
            accent.A = 0x66; // ~40% — present enough to read "selected" without washing out the title
            status.AgentSelectionBrush(SolidColorBrush{ accent });
            status.AgentSelectionVisible(true);
        }
        CATCH_LOG();
    }

    // Agentmaster (Linked Lenses): re-evaluate which tab (if any) wears the "selected/active" pill.
    // The target is the managed session HOVERED in the Manager (a live preview that follows the
    // mouse), else the SELECTED session — but ONLY while the Agent Manager tab is the active tab and
    // that session actually has a tab in THIS window. Otherwise nothing is pilled. Idempotent: a
    // no-op when the target is unchanged; on a change it clears the old tab's pill and sets the new.
    // Called on a Manager lens change, a hover push, and a tab switch (so it clears when you leave
    // the Manager tab and re-applies when you return). UI thread only.
    void TerminalPage::_UpdateManagerSelectionHighlight()
    {
        // Gate: the Manager tab must be the active (selected) tab.
        bool onManager = false;
        if (_managerTab && _tabView)
        {
            const auto idx = _tabView.SelectedIndex();
            if (idx >= 0 && idx < static_cast<int32_t>(_tabs.Size()))
            {
                onManager = (_tabs.GetAt(idx) == _managerTab);
            }
        }
        if (!onManager)
        {
            // Off the Manager tab, hover is meaningless. Clear it so returning to the Manager falls
            // back to the SELECTION until a genuine PointerEntered re-arms hover — otherwise a hover
            // left dangling by a keyboard tab-switch (no PointerExited) would wrongly pill on return.
            _managerHoverSessionId.clear();
        }

        std::wstring desired;
        if (onManager)
        {
            // Hover wins over the pinned lens selection (a preview); fall back to the selection,
            // read live from the content so a restored/seeded selection is honored without caching.
            std::wstring target = _managerHoverSessionId;
            if (target.empty())
            {
                if (const auto ipc = _agentManagerContent.get())
                {
                    if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                    {
                        target = std::wstring{ mgr->SelectedSessionId() };
                    }
                }
            }
            // Only pill a session whose tab lives in THIS window (a GLOBAL-scope card can name a
            // session hosted elsewhere — that window pills it, not this one).
            if (!target.empty() && _claudeTabs.find(target) != _claudeTabs.end())
            {
                desired = target;
            }
        }

        if (desired == _pilledSessionId)
        {
            return; // no change
        }
        // Clear the previously-pilled tab.
        if (!_pilledSessionId.empty())
        {
            if (const auto it = _claudeTabs.find(_pilledSessionId); it != _claudeTabs.end())
            {
                if (const auto t = it->second.get())
                {
                    _SetTabSelectionPill(t, false);
                }
            }
        }
        _pilledSessionId = desired;
        if (!desired.empty())
        {
            if (const auto it = _claudeTabs.find(desired); it != _claudeTabs.end())
            {
                if (const auto t = it->second.get())
                {
                    _SetTabSelectionPill(t, true);
                }
            }
        }
    }

    // Agentmaster (TAB_OVERLAY.md): build the per-tab "link badge" overlay for a Claude session and
    // install it into its terminal pane's top-right slot. Gated on AppSettings.showTabOverlay. A
    // re-attach replaces the prior overlay for that id (the old com_ptr's release detaches its
    // observer). Best-effort — a tab with no TerminalPaneContent is left alone.
    void TerminalPage::_AttachClaudeOverlay(const TerminalApp::Tab& tab, const std::wstring& sessionId)
    {
        if (!_appSettings.showTabOverlay || !_sessionRegistry || !tab || sessionId.empty())
        {
            return;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return;
        }
        TerminalApp::TerminalPaneContent termContent{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (termContent)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        termContent = term;
                    }
                }
            });
        }
        if (!termContent)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] " + sessionId + L" NOT attached (no TerminalPaneContent in tab)\n");
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        overlay->Initialize(sessionId, _sessionRegistry);
        // Summary panel (TAB_OVERLAY.md): its show/hide is the GLOBAL AppSettings::showSummaryPanel, not
        // per-session — seed this overlay with the current value, and wire the pencil to flip the global
        // setting (freshest-disk RMW) + broadcast live to every linked overlay in this window.
        overlay->SetSummaryEnabled(_appSettings.showSummaryPanel);
        // Summary panel SIZE (TAB_OVERLAY.md resize): a GLOBAL setting
        // (AppSettings::summaryPanelWidthFraction/HeightFraction) stored as fractions of the pane so it
        // scales with the window. Seed this overlay with the current fractions; the grips persist a new
        // size on drag release via the resize handler below.
        overlay->SetSummarySize(_appSettings.summaryPanelWidthFraction, _appSettings.summaryPanelHeightFraction);
        {
            auto weakThis = get_weak();
            overlay->SetSummaryToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryPanel();
                }
            });
            // A grip drag persists the new size GLOBALLY (the treeSort / archiveSplitFraction idiom): a
            // freshest-disk read-modify-write of just these two fields, keep this window's in-memory copy
            // in step (so a later cog Save can't regress it), then apply it LIVE to every linked overlay
            // in this window. Other already-open windows adopt it on their next launch.
            overlay->SetSummaryResizeHandler([weakThis](double wf, double hf) {
                auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                auto s = ::Agentmaster::LoadAppSettings();
                s.summaryPanelWidthFraction = wf;
                s.summaryPanelHeightFraction = hf;
                ::Agentmaster::SaveAppSettings(s);
                self->_appSettings.summaryPanelWidthFraction = wf;
                self->_appSettings.summaryPanelHeightFraction = hf;
                for (const auto& [id, ov] : self->_claudeOverlays)
                {
                    if (ov)
                    {
                        ov->SetSummarySize(wf, hf);
                    }
                }
            });
        }
        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent))
        {
            impl->SetAgentOverlay(overlay->Root());
            impl->SetAgentSummaryOverlay(overlay->SummaryRoot()); // 2nd slot: the pencil-toggled summary panel (TAB_OVERLAY.md)
            {
                // Push the live pane size into the overlay (on the wrapper's SizeChanged + once now) so it
                // can size the summary panel as a fraction of the pane (TAB_OVERLAY.md resize). Weak so the
                // pane handler can't keep the overlay alive past tab teardown.
                auto weakOverlay = overlay->get_weak();
                impl->SetSummaryPaneSizeHandler([weakOverlay](double w, double h) {
                    if (const auto ov = weakOverlay.get())
                    {
                        ov->OnSummaryPaneSize(w, h);
                    }
                });
            }
            impl->SetAgentManaged(true); // exclude this managed-session pane from broadcast input (item 2)
            _claudeOverlays[sessionId] = overlay; // replaces any prior overlay for this id
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] " + sessionId + L" attached\n");
        }
    }

    // Agentmaster (TAB_OVERLAY.md summary panel): the per-tab pencil button toggles the summary panel's
    // visibility, which is a GLOBAL setting (AppSettings::showSummaryPanel) so the choice is shared across
    // windows and survives restart. Mirror the treeSort / archiveSplitFraction pattern: a freshest-disk
    // read-modify-write of just this field (so a concurrent cog Save / another window can't be clobbered),
    // keep this window's in-memory copy in step, then apply it LIVE to every linked overlay in this window
    // (other already-open windows adopt it on their next launch, like treeSort).
    void TerminalPage::_ToggleSummaryPanel()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        const bool next = !s.showSummaryPanel;
        s.showSummaryPanel = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.showSummaryPanel = next;
        for (const auto& [id, ov] : _claudeOverlays)
        {
            if (ov)
            {
                ov->SetSummaryEnabled(next);
            }
        }
    }

    // Agentmaster (OBSERVER.md §4/§11d): attach-or-update a registry-LESS "○ <kind> · unlinked" badge
    // on a NON-bound tab the observer classified — a shell ("pwsh" / "cmd"), a never-prompted claude
    // ("claude": correlated but no transcript id yet, §11d), or codex. Keyed by WT_SESSION (there is no
    // sessionId). If a badge already exists for the tab its kind is updated in place (so a pwsh tab
    // that becomes a claude flips pwsh -> claude); the real _AttachClaudeOverlay replaces it once a
    // claude resolves an id. Idempotent — ShowActivity skips a re-render when the kind is unchanged.
    void TerminalPage::_SetTabActivityBadge(const TerminalApp::Tab& tab, const std::wstring& wtSession, const std::wstring& kind)
    {
        if (!tab || wtSession.empty())
        {
            return;
        }
        // Tab status dot: an OBSERVED-but-unmanaged tab (pwsh / cmd / unprompted claude / codex)
        // carries a dim gray dot — same visual language as the tree/board (state color = managed,
        // dim = merely observed). Set BEFORE the overlay gate below: the dot is tab-strip chrome,
        // independent of the in-terminal HUD toggle. Re-asserted every probe tick; _SetTabAgentDot
        // is idempotent on the unchanged color. (A tab that becomes managed gets its state-colored
        // dot from the bind tail, which runs after this badge is dropped.)
        _SetTabAgentDot(tab, winrt::Windows::UI::ColorHelper::FromArgb(0x70, 0x80, 0x80, 0x80));
        if (!_appSettings.showTabOverlay)
        {
            return;
        }
        if (const auto existing = _pendingOverlays.find(wtSession); existing != _pendingOverlays.end())
        {
            if (existing->second)
            {
                existing->second->ShowActivity(kind); // update the kind in place (no-op if unchanged)
            }
            return;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return;
        }
        TerminalApp::TerminalPaneContent termContent{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (termContent)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        termContent = term;
                    }
                }
            });
        }
        if (!termContent)
        {
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        overlay->ShowActivity(kind);
        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent))
        {
            impl->SetAgentOverlay(overlay->Root());
            _pendingOverlays[wtSession] = overlay;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] observe badge kind=" + kind + L" wt=" + wtSession + L"\n");
        }
    }

    // Agentmaster: reconcile a session's tab binding on a SessionStart, keyed by the STABLE
    // WT_SESSION `tabToken` (the ConPTY identity, which — unlike the Claude session id — never
    // changes for the life of a tab). Now fired for EVERY SessionStart, this one path handles:
    //   * ADOPT  — a `claude` the user typed into a `+` tab (a new, unknown id wired by the PATH
    //              shim): find the live ConPTY whose WT_SESSION == tabToken and bind a stdin
    //              injector to it, promoting it to full observe+control (Rule #3).
    //   * RE-HOME — a tab whose claude changed conversation id via the in-session `/resume` (the id
    //              changes, the tabToken does not). The same tab is found bound to an OLD id; that
    //              old id is archived (queue kept restorable) and the tab is re-pointed to the new
    //              id — even if the new id is a previously-known/archived one (which never creates a
    //              record here, so the old new-record-only adoption gate missed it).
    // Idempotent: a SessionStart for an already-bound session (every Manager-launched one) fast-
    // returns. Best-effort: a claude with no matching connection in this window stays observe-only
    // and the registry is left untouched. Runs on the UI thread (XAML walk).
    winrt::fire_and_forget TerminalPage::_AdoptExternalSession(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        if (!_sessionRegistry)
        {
            co_return;
        }

        const std::wstring id{ sessionId };

        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        const std::wstring token = lower(std::wstring{ tabToken });

        // Idempotent fast path: a SessionStart for a session already bound to a live tab (and
        // carrying its overlay, when overlays are enabled) needs nothing. This is the common case —
        // every Manager-launched session re-emits SessionStart and lands here, so the reconcile is
        // a cheap no-op for them (no walk, no registry writes, no cross-window notify spam).
        if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
        {
            if (const auto t = it->second.get())
            {
                const bool overlayOk = (_claudeOverlays.find(id) != _claudeOverlays.end()) || !_appSettings.showTabOverlay;
                if (overlayOk)
                {
                    co_return;
                }
            }
        }

        // Diagnostic: we are actually going to try to (re)bind this session to a tab (not a fast
        // no-op). Shows how many terminal tabs this window has, so a mismatch is visible in the log.
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[bind-try] " + id + L" token=" + token + L" tabs=" + std::to_wstring(_tabs.Size()) + L"\n");

        if (token.empty())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" observe-only (no tabToken)\n");
            co_return;
        }

        // Find the live ConPTY in THIS window whose WT_SESSION == token (the STABLE tab identity).
        TerminalApp::Tab hostTab{ nullptr };
        TerminalConnection::ITerminalConnection match{ nullptr };
        for (const auto& projectedTab : _tabs)
        {
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection found{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (found)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                const auto conn = ctrl.Connection();
                if (!conn)
                {
                    return;
                }
                if (lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId())) == token)
                {
                    found = conn;
                }
            });
            if (found)
            {
                hostTab = projectedTab;
                match = found;
                break;
            }
        }

        // No connection in this window hosts that WT_SESSION — a claude hosted outside this app (or
        // in another window). Stay observe-only; do NOT touch the registry (avoids notify spam on
        // every other window for a session it doesn't host).
        if (!match || !hostTab)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" observe-only (no connection for " + token + L")\n");
            co_return;
        }

        _BindClaudeSessionToTab(hostTab, match, id, std::wstring{ cwd }, L"WT_SESSION " + token);
        co_return;
    }

    // Agentmaster: bind a Claude session id to a specific live tab + ConPTY connection — the shared
    // tail of BOTH correlation paths: WT_SESSION-tabToken hook adoption (_AdoptExternalSession) and
    // the Fleet Observer's PEB correlation table (_ObserverProbe). Runs on the UI thread. Handles the in-
    // session /resume RE-HOME (a different id already bound to this tab is archived, its Flight Plan
    // kept restorable), derives/pins the title (one-title rule, Rule #11), binds the stdin injector
    // (Rule #3: inject by sessionId), marks the session live, colors the tab per working dir (Rule
    // #12), attaches the per-tab overlay, and persists. `origin` is a human tag for the log only.
    void TerminalPage::_BindClaudeSessionToTab(const TerminalApp::Tab& hostTab,
                                               const TerminalConnection::ITerminalConnection& conn,
                                               const std::wstring& id,
                                               const std::wstring& cwd,
                                               const std::wstring& origin)
    {
        if (!_sessionRegistry || !hostTab || !conn || id.empty())
        {
            return;
        }

        // RE-HOME (in-session `/resume`): if this SAME tab is currently bound to a DIFFERENT session
        // id, the conversation switched ids on a stable ConPTY. Supersede the old id — archive it
        // (its Flight Plan stays restorable) and drop its tab/overlay binding — then re-point below.
        // reHomedFromOtherId records that we did so, so the title block below does NOT let the old
        // (now-archived) conversation's still-pinned tab title bleed onto the new conversation
        // (Rule #11: each session owns its title; the new id must show ITS name, not the archived one's).
        bool reHomedFromOtherId = false;
        for (const auto& [oldId, weakOld] : _claudeTabs)
        {
            if (const auto t = weakOld.get(); t && t == hostTab && oldId != id)
            {
                // COPY the key first: `oldId` is a reference INTO the _claudeTabs node, and the
                // erase below frees that node — using `oldId` afterwards (the _claudeOverlays erase)
                // would hash freed memory -> AV. (Latent use-after-free; the observer's more frequent
                // re-homes exposed it.)
                const std::wstring superseded = oldId;
                _sessionRegistry->SetInjector(superseded, nullptr);
                _sessionRegistry->Update(superseded, [](::Agentmaster::SessionInfo& s) {
                    s.live = false;
                    s.pendingConfirmPromptId.clear();
                });
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[rehome] " + superseded + L" -> " + id + L" (same tab, new conversation id)\n");
                _claudeTabs.erase(superseded);
                _claudeOverlays.erase(superseded);
                reHomedFromOtherId = true; // the tab's pinned text is `superseded`'s title — don't bleed it onto `id`
                break; // one tab hosts one session
            }
        }

        // Give the card a readable title from its working dir if it has none.
        const std::wstring ttl = ::Agentmaster::DeriveSessionTitle(cwd);
        _sessionRegistry->Update(id, [&ttl](::Agentmaster::SessionInfo& s) {
            if (s.title.empty())
            {
                s.title = ttl;
            }
        });

        // Bind the id to this tab — full observe+control (Rule #3: inject by sessionId).
        const auto connection = conn;
        _sessionRegistry->SetInjector(id, [connection](const std::wstring& text) {
            const auto* begin = reinterpret_cast<const char16_t*>(text.data());
            connection.WriteInput(winrt::array_view<const char16_t>{ begin, begin + text.size() });
        });
        _claudeTabs[id] = winrt::make_weak(hostTab);
        _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
            s.live = true; // running in a tab we control now (covers /resume to a previously-archived id)
        });

        // One-title rule (Rule #11) — give the tab THIS session's own title:
        //   * Normal adopt/move bind: a name the user already gave the tab wins — mirror the tab's
        //     runtime text into the registry; otherwise pin the tab to the managed/derived name.
        //   * In-session `/resume` re-home (reHomedFromOtherId): the tab's runtime text is still the
        //     SUPERSEDED (now-archived) conversation's pinned title. It must NOT bleed onto the new
        //     conversation — that one owns its identity: its prior managed title if it had one, else
        //     the cwd-derived ttl set above (DeriveSessionTitle is never empty, so the else-branch pin
        //     always fires). Skip the tab-text-wins mirror and pin the tab to the new id's title,
        //     flipping the strip from the archived name to the new one.
        // Either way `id` is now in _claudeTabs, so later renames sync via _SyncClaudeTitleFromTab.
        if (const auto impl = _GetTabImpl(hostTab))
        {
            const std::wstring tabText{ impl->GetTabText() };
            if (!tabText.empty() && !reHomedFromOtherId)
            {
                _sessionRegistry->Update(id, [&tabText](::Agentmaster::SessionInfo& s) { s.title = tabText; });
            }
            else if (const auto s = _sessionRegistry->Get(id); s && !s->title.empty())
            {
                _SetClaudeTabTextPinned(impl, winrt::hstring{ s->title }); // pinned: registry->tab, no write-back
            }
        }
        _ApplyDirColorToTab(hostTab, cwd); // per-directory tab color
        _AttachClaudeOverlay(hostTab, id); // per-tab "link badge" overlay (TAB_OVERLAY.md)
        // Tab status dot: managed now — seed the strip dot from the session's current state (the
        // engine-init registry observer keeps it live from here on).
        if (const auto s = _sessionRegistry->Get(id))
        {
            _SetTabAgentDot(hostTab, AgentStatusColorFor(s->state));
        }
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" bound via " + origin + L"\n");
    }

    // Agentmaster: the scanner's interval liveness sweep, marshaled onto THIS window's UI thread
    // (the ConnectionState read + XAML tab walk are UI-thread-only, so the plain-C++ scanner can't
    // do it itself — it just TICKS this probe on its slow cadence). Walk this window's claude tabs;
    // any whose hosting ConPTY connection has reached Closed — the claude.exe exited (crashed with
    // no SessionEnd, ran `/exit`, or a clean SessionEnd that only set state=Done) — is archived in
    // place: flip the record to Archived (live=false), unbind its stdin injector, drop the
    // sessionId->tab mapping, and persist. The (now-dead) tab is LEFT for the user to read/close;
    // the session leaves the Triage Board and lists under "Archived", restorable via `claude
    // --resume`. No confirm dialog — the process is already gone (unlike the user-initiated archive
    // seam). Best-effort + idempotent: a tab with no terminal, or any live terminal, is left alone.
    winrt::fire_and_forget TerminalPage::_SweepClaudeLiveness()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        if (!_sessionRegistry || _claudeTabs.empty())
        {
            co_return;
        }

        // Collect first, mutate after — never erase from _claudeTabs while iterating it.
        std::vector<std::wstring> dead;
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            const auto tab = weakTab.get();
            if (!tab)
            {
                continue; // weak_ref already lapsed (tab fully torn down) — the close path owns it
            }
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl)
            {
                continue;
            }
            // The session's OWN connection WT_SESSION (== its tabToken, kept current by hooks + the
            // observer). When known, judge liveness by THIS session's connection specifically — NOT "any
            // terminal in the tab" — so a Claude pane closed/dead beside a still-live shell sibling (a
            // user split) is archived instead of lingering live=true (the sibling kept anyAlive true,
            // so the old rule never fired -> the card lingered until the WHOLE tab died).
            const auto info = _sessionRegistry->Get(id);
            std::wstring sessionWt = info ? info->tabToken : std::wstring{};
            for (auto& c : sessionWt)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            bool sawTerminal = false;
            bool anyAlive = false;
            bool foundSessionConn = false;
            bool sessionConnAlive = false;
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                sawTerminal = true;
                // < Closed == NotConnected / Connecting / Connected / Closing -> still alive.
                const bool alive = ctrl.ConnectionState() < TerminalConnection::ConnectionState::Closed;
                anyAlive = anyAlive || alive;
                if (!sessionWt.empty())
                {
                    if (const auto cc = ctrl.Connection())
                    {
                        std::wstring wt = ::Microsoft::Console::Utils::GuidToPlainString(cc.SessionId());
                        for (auto& c : wt)
                        {
                            if (c >= L'A' && c <= L'Z')
                            {
                                c = static_cast<wchar_t>(c - L'A' + L'a');
                            }
                        }
                        if (wt == sessionWt)
                        {
                            foundSessionConn = true;
                            sessionConnAlive = alive;
                        }
                    }
                }
            });
            bool isDead;
            if (!sessionWt.empty() && sawTerminal)
            {
                // We can pinpoint THIS session's connection: dead iff its pane is GONE from the tab
                // (closed — e.g. a closePane on one pane of a split) or its connection reached Closed
                // (claude exited). A live sibling pane no longer protects it.
                isDead = !foundSessionConn || !sessionConnAlive;
            }
            else
            {
                // No tabToken yet (a just-launched session before its first hook/observe) -> fall back to
                // the original rule: archive only a tab we positively saw a dead terminal in, with no
                // still-live terminal. (Never archive a tab whose content we couldn't read.)
                isDead = sawTerminal && !anyAlive;
            }
            if (isDead)
            {
                dead.push_back(id);
            }
        }

        if (dead.empty())
        {
            co_return;
        }
        for (const auto& id : dead)
        {
            // Tab status dot: clear it BEFORE dropping the _claudeTabs entry — the sweep leaves the
            // dead tab open for the user to read, and the registry-observer path can't reach it
            // after the map erase (its Update bounce would land on a map miss).
            if (const auto deadIt = _claudeTabs.find(id); deadIt != _claudeTabs.end())
            {
                if (const auto t = deadIt->second.get())
                {
                    _SetTabAgentDot(t, std::nullopt);
                }
            }
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
            });
            _sessionRegistry->SetInjector(id, nullptr);
            _claudeTabs.erase(id);
            _claudeOverlays.erase(id); // drop the per-tab overlay (detaches its registry observer)
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[liveness] dead -> archived " + id + L"\n");
        }
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        co_return;
    }

    // Agentmaster (TAB_OVERLAY.md): the periodic tab<->session BIND reconcile — the POLL backstop to
    // event-driven adoption. Ticked by the shared SessionScanner alongside the liveness sweep. For
    // every LIVE session that has reported a hosting WT_SESSION (tabToken), re-run the idempotent
    // bind (_AdoptExternalSession), UNLESS it is already fully set up in THIS window (bound tab +
    // overlay) — that skip keeps the common case free of work and log noise. This catches: a session
    // whose SessionStart adoption was missed; a missed overlay attach; and a tab whose claude changed
    // its session id via an in-session /resume (the id changed; the tabToken — the ConPTY — did not,
    // and ANY later hook refreshes s.tabToken, so the poll re-homes even with no fresh SessionStart).
    // Only LIVE sessions are reconciled: a re-home archives the superseded id (live=false), so two
    // ids sharing one ConPTY can't ping-pong over the tab.
    winrt::fire_and_forget TerminalPage::_ReconcileClaudeTabs()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry)
        {
            co_return;
        }
        // Storm guard (multi-window): the registry is a process-wide singleton (M9), so Snapshot()
        // returns the ENTIRE fleet — sessions hosted in OTHER windows included. A session can only be
        // bound where its ConPTY physically lives, so collect THIS window's live WT_SESSION tokens
        // (== ITerminalConnection::SessionId(), the tabToken) up front and reconcile ONLY sessions whose
        // tabToken is in that set. Without this, every window re-ran _AdoptExternalSession for every
        // OTHER window's sessions on EVERY scanner tick, and each call queues a fire_and_forget UI-thread
        // coroutine — so the dispatcher floods faster than the UI thread can drain it. Observed live: 2-3
        // windows over a 44-session fleet drove a ~100 line/s [bind-try]/[adopt] storm, a 7.4 GB working
        // set, and a Responding=False window that still processed input but NEVER rendered (transparent/
        // black). Each window now reconciles only its own tabs (a Manager-only window does zero attempts).
        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        std::unordered_set<std::wstring> windowTokens;
        for (const auto& projectedTab : _tabs)
        {
            if (projectedTab == _managerTab)
            {
                continue; // the Manager tab hosts no ConPTY
            }
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection conn{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (conn)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                conn = ctrl.Connection();
            });
            if (conn)
            {
                // The exact tab id: WT_SESSION == ITerminalConnection::SessionId() (the correlation key).
                windowTokens.insert(lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId())));
            }
        }

        const auto sessions = _sessionRegistry->Snapshot();
        size_t attempts = 0;
        for (const auto& s : sessions)
        {
            if (!s.live || s.tabToken.empty())
            {
                continue; // archived, or no hook has revealed a hosting ConPTY yet
            }
            // Only a session whose hosting ConPTY lives in THIS window is bindable here — skip the rest
            // of the (process-wide) fleet so a window never re-attempts another window's sessions (the
            // dispatcher-flood fix above). A window that doesn't host this tabToken isn't its reconciler.
            if (windowTokens.find(lower(s.tabToken)) == windowTokens.end())
            {
                continue;
            }
            // Skip sessions already fully set up in THIS window (bound tab + overlay) — no work, no log.
            const auto it = _claudeTabs.find(s.id);
            const bool boundHere = (it != _claudeTabs.end() && it->second.get() != nullptr);
            const bool overlayOk = (_claudeOverlays.find(s.id) != _claudeOverlays.end()) || !_appSettings.showTabOverlay;
            if (boundHere && overlayOk)
            {
                continue;
            }
            ++attempts;
            _AdoptExternalSession(winrt::hstring{ s.id }, winrt::hstring{ s.workingDir }, winrt::hstring{ s.tabToken });
        }
        if (attempts > 0)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[reconcile] sessions=" + std::to_wstring(sessions.size()) +
                                              L" windowTabs=" + std::to_wstring(windowTokens.size()) +
                                              L" attempts=" + std::to_wstring(attempts) +
                                              L" boundTabs=" + std::to_wstring(_claudeTabs.size()) + L"\n");
        }
        co_return;
    }

    // Agentmaster (Fleet Observer, OBSERVER.md §10): the UI lane of the PULL observer — the one
    // WinRT thread that touches XAML. Replaces _DiscoverClaudeTabsByCwd (the per-tab Toolhelp walk).
    // Two halves, each scanner tick (alongside the reconcile + liveness sweep):
    //   PUBLISH — build THIS window's tab roster {WT_SESSION, shell PID, already-bound} (both reads
    //     are µs: ITerminalConnection::SessionId() + RootProcessHandle()->GetProcessId) and hand it
    //     to the process-wide ProcessObserver, which surveys ALL claude PEBs off-thread in ONE
    //     snapshot and keys correlation on the exact WT_SESSION.
    //   READ + BIND — read the observer's correlation table and, for each of our UNBOUND tabs whose
    //     claude the observer resolved to an OURS conversation id, run the shared
    //     _BindClaudeSessionToTab (injector + overlay + title + color) — full observe+control with
    //     NO hooks / shim / settings, so a hand-typed `claude` after a `cd` still binds to the right
    //     tab + id. A short settle between publish and read lets the publish-triggered survey land,
    //     so a freshly-typed claude binds this tick (≤ ~one cadence) rather than next.
    // External (WindowsTerminal) claudes are observe-only (runningApp != Agentmaster) and never bind.
    winrt::fire_and_forget TerminalPage::_ObserverProbe()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || !_observer)
        {
            co_return;
        }

        // Lowercase a GUID-plain string to match the observer's roster key form (it lowercases too).
        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };

        // --- PASS 1 (UI thread): build this window's roster + remember each tab's conn for the bind. ---
        struct ProbeTab
        {
            winrt::weak_ref<TerminalApp::Tab> tab; // weak so the 300ms settle below never delays a tab teardown
            TerminalConnection::ITerminalConnection conn{ nullptr };
            std::wstring wt;
        };
        std::vector<ProbeTab> probeTabs;
        std::vector<::Agentmaster::TabRosterEntry> roster;
        for (const auto& projectedTab : _tabs)
        {
            if (projectedTab == _managerTab)
            {
                continue;
            }
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection conn{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (conn)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                conn = ctrl.Connection();
            });
            if (!conn)
            {
                continue; // not a terminal tab
            }
            // The exact tab id: WT_SESSION == ITerminalConnection::SessionId() (the correlation key).
            const std::wstring wt = lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId()));
            // The tab's shell PID, from the ConPTY root process HANDLE (the observer reads the claude
            // DESCENDANT's PEB; we hand it the shell so its tree walk is anchored to this exact tab).
            uint32_t shellPid = 0;
            if (const auto cpc = conn.try_as<TerminalConnection::ConptyConnection>())
            {
                if (const auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cpc.RootProcessHandle())))
                {
                    shellPid = ::GetProcessId(h);
                }
            }
            bool bound = false;
            for (const auto& [boundId, weakBound] : _claudeTabs)
            {
                if (const auto t = weakBound.get(); t && t == projectedTab)
                {
                    bound = true;
                    break;
                }
            }
            ::Agentmaster::TabRosterEntry e;
            e.wtSession = wt;
            e.shellPid = shellPid;
            e.bound = bound;
            roster.push_back(std::move(e));
            probeTabs.push_back({ winrt::make_weak(projectedTab), conn, wt });
        }

        // PUBLISH (the observer diffs + Wake()s its survey when this roster changed).
        _observer->PublishRoster(_windowId, std::move(roster));

        // Let a publish-triggered survey land before we read, so a just-appeared claude binds THIS
        // tick rather than next. resume_after resumes on the threadpool — hop back to the UI thread.
        co_await winrt::resume_after(std::chrono::milliseconds(300));
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || !_observer)
        {
            co_return;
        }

        // --- PASS 2 (UI thread): push the External census to the Manager, then bind our tabs. ---
        // External (WindowsTerminal) claudes aren't in any roster, so push them regardless of corr
        // (a window with zero OUR tabs can still surface the external group). The setter diffs.
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SetExternalClaudes(_observer->External());
            }
        }

        const auto corr = _observer->Correlation();
        const auto act = _observer->Activity(); // every tab's foreground activity (pwsh / cmd / claude / codex)
        std::unordered_map<std::wstring, ::Agentmaster::CorrelationRow> byWt;
        for (const auto& c : corr)
        {
            byWt[c.wtSession] = c;
        }
        std::unordered_map<std::wstring, ::Agentmaster::TabActivityRow> actByWt;
        for (const auto& a : act)
        {
            actByWt[a.wtSession] = a; // the whole row (Codex carries its model, to enrich the badge)
        }
        std::unordered_set<std::wstring> rosterWts; // this window's tabs this tick (for observe-badge pruning)
        for (const auto& pt : probeTabs)
        {
            rosterWts.insert(pt.wt);
            const auto hostTab = pt.tab.get();
            if (!hostTab)
            {
                continue; // tab torn down during the settle
            }
            // One session per tab; is this tab already bound (has a real overlay)?
            std::wstring boundId;
            for (const auto& [bid, weakBound] : _claudeTabs)
            {
                if (const auto t = weakBound.get(); t && t == hostTab)
                {
                    boundId = bid;
                    break;
                }
            }
            if (!boundId.empty())
            {
                _DropPendingOverlay(pt.wt); // bound -> the real overlay owns the slot now
                // Managed Codex (Codex-launch, lifecycle + state): a Codex record has NO hook/scanner
                // feed, so the C2 rollout-tail is its state authority. Pull the resolved rollout uuid +
                // the turn-state from the observer's activity row onto the record (cheap no-op when
                // unchanged); also stamps tabToken so the census dedups it out of the External group.
                if (const auto ait = actByWt.find(pt.wt); ait != actByWt.end() && ait->second.activity == ::Agentmaster::TabActivity::Codex)
                {
                    _ReconcileManagedCodex(boundId, ait->second);
                }
                continue;
            }

            const auto it = byWt.find(pt.wt);
            const bool oursClaude = (it != byWt.end()) && it->second.runningApp == ::Agentmaster::RunningApp::Agentmaster;

            if (oursClaude && !it->second.sessionId.empty())
            {
                // This window hosts the connection NOW (it's in OUR roster, walked from _tabs), so we are
                // its rightful local owner. Bind via the shared tail — keyed on the exact WT_SESSION, so a
                // hand-typed claude after a `cd` binds to the right id even with no hook; the bound overlay
                // replaces any pending badge in the slot.
                //
                // HasInjector here does NOT mean "skip". Reaching this point means the tab is NOT in our
                // _claudeTabs (the alreadyBound check above already `continue`d if it were). If the session
                // ALSO already has an injector, it was bound by ANOTHER window and the tab just arrived
                // here via a cross-window tear-out / move-to-window: WT moves the live ConPTY between
                // TerminalPages, the origin evicted its stale entry in _DetachClaudeTabForMove, and
                // WT_SESSION is stable across the move. RE-HOME it — a connection lives in exactly one
                // window at a time, so this window provably owns it now and binding can't double-bind.
                // _BindClaudeSessionToTab re-points the injector to THIS tab's surviving connection and
                // recreates the local _claudeTabs entry + overlay, giving Activate / Archive / Rename a
                // handle. (The old behavior — skipping on HasInjector — left a moved session headless in
                // the destination: a visible card with no local control.)
                const bool moved = _sessionRegistry->HasInjector(it->second.sessionId);
                _DropPendingOverlay(pt.wt);
                const std::wstring why = (moved ? L"observer re-home (moved) wt=" : L"observer wt=") + pt.wt;
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[discover] " + it->second.sessionId + L" cwd=" + it->second.cwd + L" (" + why + L")\n");
                _BindClaudeSessionToTab(hostTab, pt.conn, it->second.sessionId, it->second.cwd, why);
                continue;
            }

            // Non-bound and not a resolved claude -> show an "observe" badge for whatever the observer
            // classified this tab as: an unresolved claude (no transcript id yet, §11d), or a shell /
            // codex from the activity table. So a pwsh / cmd tab carries "○ pwsh · unlinked" too, and
            // it flips to "claude" the instant the user runs claude (then to the linked overlay on the
            // first prompt).
            std::wstring kind;
            if (oursClaude)
            {
                kind = L"claude"; // correlated to a claude, but no conversation id yet
            }
            else if (const auto ait = actByWt.find(pt.wt); ait != actByWt.end())
            {
                const auto& arow = ait->second;
                switch (arow.activity)
                {
                case ::Agentmaster::TabActivity::Powershell:
                    kind = L"pwsh";
                    break;
                case ::Agentmaster::TabActivity::Cmd:
                    kind = L"cmd";
                    break;
                case ::Agentmaster::TabActivity::Codex:
                {
                    // Codex is observe-only — enrich the badge with the model + turn state (Phase C2):
                    // "○ codex · gpt-5.5 · running · unlinked" (the kind string is the badge's
                    // idempotency key, so it re-renders as the model lands / the turn flips).
                    std::wstring k = arow.model.empty() ? std::wstring{ L"codex" } : (std::wstring{ L"codex  \x00B7  " } + arow.model);
                    if (arow.codexState == ::Agentmaster::CodexState::Running)
                    {
                        k += L"  \x00B7  running";
                    }
                    else if (arow.codexState == ::Agentmaster::CodexState::Waiting)
                    {
                        k += L"  \x00B7  waiting";
                    }
                    kind = std::move(k);
                    break;
                }
                case ::Agentmaster::TabActivity::ClaudeCode:
                    kind = L"claude"; // activity caught the claude before correlation did
                    break;
                default:
                    break; // Other / Unknown -> no badge
                }
            }
            if (!kind.empty())
            {
                _SetTabActivityBadge(hostTab, pt.wt, kind);
            }
            else if (!corr.empty() || !act.empty())
            {
                // A fresh survey says this tab is nothing we badge (or its claude exited) -> drop the badge.
                _DropPendingOverlay(pt.wt);
            }
        }
        // Tabs that left THIS window's roster (closed / moved) -> drop their pending badge.
        for (auto pit = _pendingOverlays.begin(); pit != _pendingOverlays.end();)
        {
            if (rosterWts.count(pit->first))
            {
                ++pit;
            }
            else
            {
                if (pit->second)
                {
                    pit->second->Root().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
                }
                pit = _pendingOverlays.erase(pit);
            }
        }
        co_return;
    }

    // Agentmaster (Codex-launch): reconcile a MANAGED Codex record from the Fleet Observer's per-tab
    // activity row — the Codex analog of the hook/scanner state feed (Codex has neither). Fills the
    // resolved rollout uuid (the `codex resume` target, needed for archive/restore), stamps tabToken
    // (the census-dedup key so a managed codex never ALSO shows as an External row), and maps the C2
    // rollout-tail turn-state onto SessionState. A cheap no-op when nothing changed (no registry/UI
    // churn each probe). Codex exposes no NeedsApproval/Error via PULL, so an Unknown turn-state leaves
    // the current state untouched.
    void TerminalPage::_ReconcileManagedCodex(const std::wstring& sessionId, const ::Agentmaster::TabActivityRow& act)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto s = _sessionRegistry->Get(sessionId);
        if (!s || s->kind != ::Agentmaster::AgentKind::Codex)
        {
            return; // only managed Codex records
        }
        std::optional<::Agentmaster::SessionState> mapped;
        switch (act.codexState)
        {
        case ::Agentmaster::CodexState::Running:
            mapped = ::Agentmaster::SessionState::Running;
            break;
        case ::Agentmaster::CodexState::Waiting:
            mapped = ::Agentmaster::SessionState::WaitingForInput;
            break;
        case ::Agentmaster::CodexState::Idle:
            mapped = ::Agentmaster::SessionState::Idle;
            break;
        default:
            break; // Unknown -> leave the current state
        }
        const bool uuidChanged = !act.sessionId.empty() && s->codexSessionId != act.sessionId;
        const bool tokenChanged = !act.wtSession.empty() && s->tabToken != act.wtSession;
        const bool stateChanged = mapped.has_value() && s->state != *mapped;
        if (!uuidChanged && !tokenChanged && !stateChanged)
        {
            return; // steady state -> no churn
        }
        _sessionRegistry->Update(sessionId, [&](::Agentmaster::SessionInfo& si) {
            if (uuidChanged)
            {
                si.codexSessionId = act.sessionId; // the `codex resume` target (persisted for archive/restore)
            }
            if (tokenChanged)
            {
                si.tabToken = act.wtSession; // census-dedup key (managed codex excluded from External)
            }
            if (stateChanged)
            {
                si.state = *mapped;
            }
        });
    }

    // Agentmaster: the Explorer Tree "refresh" button's action — force a fresh reload NOW instead of
    // waiting for the next observer/scanner tick. Wake() kicks an immediate FULL survey (re-reads each
    // claude's PEB + transcript -> re-enriches the registry and recomputes the External / Correlation /
    // Activity tables, bypassing the O7 steady-state debounce); _ObserverProbe() re-publishes this
    // window's roster, lets the survey settle, then re-pushes the External census + binds. The survey
    // enriches the registry SILENTLY (no change event, so it never drives churn — Rule #13), so once it
    // lands we force ONE Manager redraw so the refreshed LOCAL/GLOBAL timing + the EXTERNAL census both
    // show even when nothing structurally "changed". Covers every displayed scope.
    winrt::fire_and_forget TerminalPage::_RefreshObserverData()
    {
        auto weakThis = get_weak();
        if (_observer)
        {
            _observer->Wake(); // force an immediate full survey
        }
        _ObserverProbe(); // re-publish roster -> (settle) -> re-push External census + bind

        // Let the survey + probe land (the probe itself settles ~300 ms), then force a redraw so the
        // freshly enriched registry + recomputed census are reflected even when nothing "changed".
        co_await winrt::resume_after(std::chrono::milliseconds(450));
        co_await wil::resume_foreground(Dispatcher());
        if (auto self = weakThis.get())
        {
            if (const auto ipc = self->_agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->RefreshNow();
                }
            }
        }
        co_return;
    }

    // Agentmaster (OBSERVER.md §11d): drop this window's pending "claude · unlinked" badge for a tab
    // (by WT_SESSION) — collapse its element (we hold the overlay, not the pane) and release it. Used
    // when the tab binds a real session, the claude exits, or the tab leaves the roster.
    void TerminalPage::_DropPendingOverlay(const std::wstring& wtSession)
    {
        const auto it = _pendingOverlays.find(wtSession);
        if (it == _pendingOverlays.end())
        {
            return;
        }
        if (it->second)
        {
            it->second->Root().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        }
        _pendingOverlays.erase(it);
    }
}
