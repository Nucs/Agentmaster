// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - the "Specify a model..." PROMPT: the one card behind the launch-model picker's
// "type any model id" item (AgentModelMenu.h's AgentSpecifyModelOpener).
//
// It is split out of AgentModelMenu.h ON PURPOSE. The menu header is included by Tab.cpp - core
// Windows Terminal code - and this half pulls in Updater.h for its bounded WinHTTP GET, whose
// `#pragma comment(lib, ...)` drags winhttp + comctl32 into every TU that sees it (the same
// "winhttp/comctl must not fan out through this header" rule AgentManagerContent.h already
// follows for UpdateInfo). Only the two HOSTS that actually show the prompt include this:
// AgentManagerContent (its dimmed modal over _root) and TerminalPage (its Root() popup, serving
// the Sessions page and the WT tab menu). Both parent the SAME card, so the prompt cannot drift.
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine dir.
// Include from .cpp TUs only (relies on the pch projections, like AgentTipHelpers.h).

#pragma once

#include <algorithm>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AgentTipHelpers.h" // AgentSetTip - the islands-safe hover tooltip
#include "AgentMaster/ClaudeSpawn.h" // ParseLaunchModels + LogSwallowedException
#include "AgentMaster/ModelCatalog.h" // the published model lists (parse + endpoints + doc URLs)
#include "AgentMaster/Persistence.h" // LoadModelList / RememberSpecifiedModel / ResetModelList - the durable droplist
#include "AgentMaster/Updater.h" // HttpsGet - the ONE bounded WinHTTP GET (its pragma links winhttp)

namespace winrt::TerminalApp::implementation
{
    // ---- the "Specify a model..." prompt -------------------------------------------------------
    //
    // ONE card, built here and parented by the host (AgentManagerContent's in-content overlay /
    // TerminalPage's Root() popup), so the Manager menus, the Sessions page, the WT tab menu and the
    // Settings successor-model combos all show the SAME prompt. It is deliberately NOT a
    // ContentDialog: a text box inside one gets no keypresses under XAML Islands (Gotchas), which is
    // the whole reason the tag editor and the settings surface are hand-parented too.
    //
    // The input is an EDITABLE ComboBox: you can type any id, and the drop-down lists your configured
    // Launch models followed by the DURABLE MODEL LIST (model-list.json) — every id ever specified
    // here plus whatever "Fetch models" last downloaded — so the second time you need an unusual
    // model it is one click. Specifying a model KEEPS it in the list; "Fetch models" REBUILDS the
    // list from the published catalogs (the configured models are merged in from settings either
    // way, so they are never lost).
    //
    // The box opens PRE-FILLED with the model last committed here — in ANY window, in any earlier
    // run (model-list.json's "last" key, read from disk on every open). Typing replaces it, because
    // the ComboBox selects its text when it takes focus; Enter launches it again.
    struct AgentSpecifyModelCard
    {
        winrt::Windows::UI::Xaml::Controls::Border card{ nullptr }; // parent THIS
        winrt::Windows::UI::Xaml::Controls::ComboBox box{ nullptr }; // focus THIS after showing
    };

    // Just the ids out of a "Display name | model-id" list.
    inline std::vector<std::wstring> AgentConfiguredModelIds(const std::vector<std::pair<std::wstring, std::wstring>>& configured)
    {
        std::vector<std::wstring> ids;
        ids.reserve(configured.size());
        for (const auto& [name, id] : configured)
        {
            ids.push_back(id);
        }
        return ids;
    }

    // What the drop-down shows: the CONFIGURED launch models (Settings -> Launch models) first,
    // then the PERSISTED model list (model-list.json - every id specified here, plus whatever the
    // last Fetch loaded). Read fresh on every open, so a model another window specified is already
    // there. Ordering + dedupe live in MergeModelIdGroups (ModelCatalog.h), where they are tested.
    inline std::vector<std::wstring> AgentSpecifyModelSuggestions(const std::vector<std::pair<std::wstring, std::wstring>>& configured)
    {
        return ::Agentmaster::MergeModelIdGroups({ AgentConfiguredModelIds(configured), ::Agentmaster::LoadModelList() });
    }

    // Build the prompt. `onDone` fires EXACTLY once: with the trimmed id on Use / Enter, or with "" on
    // Cancel / Escape (the host closes its container either way). Committing also ADDS the id to the
    // durable model list, so every model ever specified here stays in the drop-down (until a Fetch
    // rebuilds the list), and makes it the LAST SPECIFIED model, which the next prompt opens with.
    //
    // The box opens with `seed` when the host passes one; otherwise with the last specified model
    // (LoadLastSpecifiedModel, read fresh on every build, so a commit in another window counts too).
    // The drop-down lists that id ahead of any longer id it is a prefix of (PlaceSeedAheadOfLongerIds),
    // because the ComboBox would otherwise rewrite the pre-filled text the moment the host focuses it.
    //   seed       - the id the box opens with; "" (what every host passes today) = the last specified
    //                model, or an empty box when this profile never committed one.
    //   configured - the Settings -> Launch models pairs (display name, id); their ids head the list.
    //   onDone     - the host's close callback, as above. Runs on the UI thread inside a XAML event,
    //                and may close the card before it returns.
    //   returns    - the card to parent (`card`) and the box to focus one tick after showing (`box`).
    // Throws winrt::hresult_error when XAML refuses to build an element (off the UI thread) and
    // std::bad_alloc; both hosts call this inside a try that logs the failure.
    inline AgentSpecifyModelCard AgentBuildSpecifyModelCard(const std::wstring& seed,
                                                            const std::vector<std::pair<std::wstring, std::wstring>>& configured,
                                                            std::function<void(winrt::hstring)> onDone)
    {
        namespace WUX = winrt::Windows::UI::Xaml;
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;
        using winrt::Windows::UI::ColorHelper;
        using winrt::Windows::UI::Xaml::Thickness;

        const auto fill = [](uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
            return WUX::Media::SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
        };

        WUXC::StackPanel body;
        body.Spacing(8);
        body.Width(440);

        WUXC::TextBlock title;
        title.Text(L"Specify a model");
        title.FontSize(16);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        body.Children().Append(title);

        WUXC::TextBlock sub;
        sub.Text(L"Launched as --model <id> for this one session. The box starts with the model you specified last; type another id, or pick a saved one \x2014 anything you specify here is kept in the list.");
        sub.FontSize(12);
        sub.Opacity(0.75);
        sub.TextWrapping(WUX::TextWrapping::Wrap);
        body.Children().Append(sub);

        // The editable drop-down. IsEditable is what makes a ComboBox a "type OR pick" control; its
        // Text property carries the typed value (SelectedItem alone would only ever return a listed id).
        //
        // `base` — your configured Launch models followed by the saved model list (model-list.json) —
        // is what the drop-down opens with. A Fetch rebuilds it from `configuredIds` plus what it
        // fetched, so the settings models always stay listed (they can never be pushed out or replaced).
        //
        // `initial` is what the box opens with: the host's seed, else the model last committed here
        // (any window, any earlier run). It is placed into `base` where the ComboBox's focus-time
        // type-ahead cannot replace it with a longer listed id (PlaceSeedAheadOfLongerIds explains the
        // trap); in the usual case that changes nothing, as the id already heads the saved list.
        const std::wstring initial = seed.empty() ? ::Agentmaster::LoadLastSpecifiedModel() : seed;
        const std::vector<std::wstring> configuredIds = AgentConfiguredModelIds(configured);
        const std::vector<std::wstring> base = ::Agentmaster::PlaceSeedAheadOfLongerIds(AgentSpecifyModelSuggestions(configured), initial);
        WUXC::ComboBox box;
        box.IsEditable(true);
        box.HorizontalAlignment(WUX::HorizontalAlignment::Stretch);
        box.PlaceholderText(L"e.g. opus, claude-opus-4-8, gpt-5.6-sol");
        for (const auto& s : base)
        {
            box.Items().Append(winrt::box_value(winrt::hstring{ s }));
        }
        // The ComboBox selects this text when the host focuses the box (WinUI's OnGotFocus calls
        // SelectAll for mouse and keyboard focus), so typing replaces it rather than appending to it.
        box.Text(winrt::hstring{ initial });
        AgentSetTip(box, L"The exact value passed to --model. Anything the CLI accepts works: an ALIAS (opus / sonnet / fable) always launches the latest model of that family, a full id (claude-opus-4-8) pins that exact version. The box starts with the model you specified last (in any window); Enter launches the id in the box, Escape closes without launching. The drop-down lists your Launch models (Settings \x2192 Sessions) first, then the SAVED list \x2014 every model you have specified here, plus whatever Fetch models last loaded (Claude before Codex). Specifying a model keeps it in the list; Fetch models rebuilds the list.");
        body.Children().Append(box);

        // Status line — the fetch result, or why a commit did nothing. Kept between the box and the
        // links so a message appears where the eye already is.
        WUXC::TextBlock status;
        status.FontSize(11);
        status.Opacity(0.8);
        status.TextWrapping(WUX::TextWrapping::Wrap);
        status.Visibility(WUX::Visibility::Collapsed);
        body.Children().Append(status);

        WUXC::TextBlock where;
        where.Text(L"Where to find model names");
        where.FontSize(11);
        where.Opacity(0.6);
        body.Children().Append(where);

        // The two published lists, as real links (HyperlinkButton opens the default browser itself —
        // no ShellExecute, no launch permission dance).
        WUXC::StackPanel links;
        links.Orientation(WUX::Controls::Orientation::Horizontal);
        links.Spacing(4);
        const auto addLink = [&links](std::wstring_view text, std::wstring_view url, std::wstring_view tip) {
            WUXC::HyperlinkButton lnk;
            lnk.Content(winrt::box_value(winrt::hstring{ text }));
            lnk.NavigateUri(winrt::Windows::Foundation::Uri{ winrt::hstring{ url } });
            lnk.FontSize(12);
            lnk.Padding(Thickness{ 4, 2, 4, 2 });
            AgentSetTip(lnk, winrt::hstring{ tip });
            links.Children().Append(lnk);
        };
        addLink(L"Claude models \x2197", ::Agentmaster::kAnthropicModelsDocsUrl, L"Anthropic's model overview \x2014 every Claude model with its full id (claude-opus-4-8) and its alias (opus), plus what each one is good at.");
        addLink(L"Codex models \x2197", ::Agentmaster::kCodexModelsDocsUrl, L"The OpenAI Codex CLI's own model catalog (models.json in the codex repo) \x2014 the slug field is the id.");
        body.Children().Append(links);

        // Fetch — fills the drop-down from the published lists instead of making the user read them.
        // Codex's catalog is a public file, so that half always works; Anthropic's endpoint needs an
        // API key, which a subscription (OAuth) login does not issue — hence the honest status line
        // rather than a silent failure. Off-thread + bounded, so a dead network cannot wedge the UI.
        WUXC::StackPanel actions;
        actions.Orientation(WUX::Controls::Orientation::Horizontal);
        actions.Spacing(6);

        WUXC::Button fetchBtn;
        fetchBtn.Content(winrt::box_value(winrt::hstring{ L"Fetch models" }));
        AgentSetTip(fetchBtn, L"REBUILD the saved model list from the published catalogs \x2014 Claude ids first, then Codex. This REPLACES the saved list (ids you specified earlier are cleared); your Launch models come from Settings and always stay on top. The Codex catalog is public; the Claude list needs an ANTHROPIC_API_KEY in your environment (a Claude subscription login does not issue one) \x2014 the status line says which half arrived.");
        actions.Children().Append(fetchBtn);

        WUXC::Button useBtn;
        useBtn.Content(winrt::box_value(winrt::hstring{ L"Use this model" }));
        AgentSetTip(useBtn, L"Launch with the id in the box (Enter in the box does the same) and remember it: the next prompt, in any window, starts with it.");
        WUXC::Button cancelBtn;
        cancelBtn.Content(winrt::box_value(winrt::hstring{ L"Cancel" }));
        AgentSetTip(cancelBtn, L"Close without launching anything.");

        // [ Fetch ] ................... [ Cancel ] [ Use this model ]
        WUXC::Grid row;
        {
            WUXC::ColumnDefinition c0, c1;
            c0.Width(WUX::GridLength{ 1, WUX::GridUnitType::Star });
            c1.Width(WUX::GridLength{ 0, WUX::GridUnitType::Auto });
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
        }
        WUXC::Grid::SetColumn(actions, 0);
        row.Children().Append(actions);
        WUXC::StackPanel right;
        right.Orientation(WUX::Controls::Orientation::Horizontal);
        right.Spacing(6);
        right.Children().Append(cancelBtn);
        right.Children().Append(useBtn);
        WUXC::Grid::SetColumn(right, 1);
        row.Children().Append(right);
        body.Children().Append(row);

        WUXC::Border card;
        card.RequestedTheme(WUX::ElementTheme::Dark); // Agentmaster surfaces are always dark
        card.Background(fill(0xFF, 0x26, 0x26, 0x26));
        card.BorderBrush(fill(0xFF, 0x3A, 0x3A, 0x3A));
        card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(WUX::CornerRadius{ 6, 6, 6, 6 });
        card.Padding(Thickness{ 14, 12, 14, 12 });
        card.HorizontalAlignment(WUX::HorizontalAlignment::Center);
        card.VerticalAlignment(WUX::VerticalAlignment::Center);
        card.Child(body);
        // A press inside the card must not reach a host backdrop that closes on outside-press.
        card.Tapped([](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::TappedRoutedEventArgs& e) { e.Handled(true); });

        // ---- behavior ----------------------------------------------------------------------
        // `fired` makes onDone exactly-once across every path (Use / Enter / Cancel / Escape), so a host
        // that also closes on backdrop-press can never deliver a second, contradictory answer.
        auto fired = std::make_shared<bool>(false);
        const auto finish = [onDone, fired](winrt::hstring value) {
            if (*fired)
            {
                return;
            }
            *fired = true;
            onDone(value);
        };

        // The ONE commit rule, shared by every path (Use, Enter in the box, Enter elsewhere in the
        // card), so they can never disagree. It takes the box as a PARAMETER rather than capturing it:
        // the box's own key handlers (below) call it, and a handler that held its own element would
        // keep that element alive forever (a C++/WinRT delegate is a strong reference, and nothing
        // collects the cycle).
        //   source - the prompt's box; its text is trimmed, refused when blank, else committed.
        // Throws std::bad_alloc, and whatever the host's onDone throws (finish runs it).
        const auto commitFrom = [status, finish](const WUXC::ComboBox& source) {
            std::wstring text{ source.Text() };
            const auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
            size_t b = 0, e = text.size();
            while (b < e && ws(text[b]))
            {
                ++b;
            }
            while (e > b && ws(text[e - 1]))
            {
                --e;
            }
            const std::wstring id = text.substr(b, e - b);
            if (id.empty())
            {
                // Deliberately NOT "same as Default": the user opened Specify to name a model, so a
                // blank commit is a mistake to point out, not a silent fall-back that launches
                // something they did not ask for.
                status.Text(L"Type a model id (or press Cancel to launch nothing).");
                status.Visibility(WUX::Visibility::Visible);
                source.Focus(WUX::FocusState::Programmatic);
                return;
            }
            // Every model SPECIFIED here joins the durable list, so it is in the drop-down from
            // now on (in this window and every other), and becomes the model the next prompt opens
            // with. Written BEFORE finish: the host may close this card and launch from onDone.
            ::Agentmaster::RememberSpecifiedModel(id);
            finish(winrt::hstring{ id });
        };
        // The Use button's and the card-level handler's form: the box is known here.
        const auto commit = [box, commitFrom]() { commitFrom(box); };

        useBtn.Click([commit](const winrt::Windows::Foundation::IInspectable&, const WUX::RoutedEventArgs&) { commit(); });
        cancelBtn.Click([finish](const winrt::Windows::Foundation::IInspectable&, const WUX::RoutedEventArgs&) { finish(winrt::hstring{}); });

        // Enter / Escape typed INTO the box: Enter launches the id in the box, Escape closes the prompt.
        // Three facts shape these two handlers:
        //   1. The card-level KeyDown below never receives these keys from the box. The editable
        //      ComboBox marks every key it processes Handled before acting on it (Enter commits its own
        //      search, Escape reverts it: WinUI ComboBox_Partial.cpp, MainKeyDown), so the bubbling event
        //      reaches the card already handled. PreviewKeyDown TUNNELS, so it runs BEFORE the ComboBox,
        //      and marking the key Handled there also keeps the ComboBox from acting on it.
        //   2. The action waits for the KEY-UP. Closing the prompt removes the focused box, and Windows
        //      still delivers the rest of the keystroke (the character message: a CR for Enter, an ESC for
        //      Escape) to whatever holds focus next. If that is a terminal, the CR would submit its
        //      claude's unsent prompt and the ESC would interrupt a running turn. By the key-up, the
        //      character has already gone to the box, which ignores it. Upstream's tab renamer closes on
        //      key-up for the same reason (TabHeaderControl.cpp).
        //   3. Only while the drop-down is CLOSED (with it open, Enter picks the highlighted id and Escape
        //      closes the list: the ComboBox's own keys, and the prompt stays open), and never for a
        //      Ctrl / Alt chord (Alt+Enter is Terminal's full-screen toggle, not a launch).
        // `armedKey` remembers the key that went down IN the box, so a key-up whose key-down happened
        // somewhere else (the menu Enter that opened this prompt) does nothing. The box comes from
        // `sender`, never a capture, for the reason given at commitFrom.
        auto armedKey = std::make_shared<winrt::Windows::System::VirtualKey>(winrt::Windows::System::VirtualKey::None);
        box.PreviewKeyDown([armedKey](const winrt::Windows::Foundation::IInspectable& sender, const WUX::Input::KeyRoutedEventArgs& e) {
            const auto key = e.Key();
            // Any other key-down disarms: the next key-up must belong to the key pressed last.
            *armedKey = winrt::Windows::System::VirtualKey::None;
            if (key != winrt::Windows::System::VirtualKey::Enter && key != winrt::Windows::System::VirtualKey::Escape)
            {
                return;
            }
            const auto source = sender.try_as<WUXC::ComboBox>();
            if (!source || source.IsDropDownOpen() || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
            {
                return; // the ComboBox's own key (an open list), or an app chord: not ours
            }
            *armedKey = key;
            e.Handled(true); // the ComboBox must not commit / revert its search: the key-up acts instead
        });
        box.PreviewKeyUp([armedKey, commitFrom, finish](const winrt::Windows::Foundation::IInspectable& sender, const WUX::Input::KeyRoutedEventArgs& e) {
            const auto key = e.Key();
            if (*armedKey == winrt::Windows::System::VirtualKey::None || key != *armedKey)
            {
                return; // its key-down was not ours (or not in this box)
            }
            *armedKey = winrt::Windows::System::VirtualKey::None;
            // Set before acting: the commit or cancel can close the card and remove this box synchronously.
            e.Handled(true);
            try
            {
                if (key == winrt::Windows::System::VirtualKey::Enter)
                {
                    if (const auto source = sender.try_as<WUXC::ComboBox>())
                    {
                        commitFrom(source);
                    }
                }
                else
                {
                    finish(winrt::hstring{});
                }
            }
            catch (...)
            {
                // An exception escaping a XAML event handler can fail the whole app; this one has no
                // recovery to run (the prompt stays open, or the host already closed it), so it only
                // keeps the throw diagnosable (Rule #18).
                ::Agentmaster::LogSwallowedException(L"specify-model key commit");
            }
        });
        // A key held while focus leaves the box must not act when it is released somewhere else.
        box.LostFocus([armedKey](const winrt::Windows::Foundation::IInspectable&, const WUX::RoutedEventArgs&) {
            *armedKey = winrt::Windows::System::VirtualKey::None;
        });

        // Enter / Escape from the REST of the card (a link, a button). Keys typed in the box never get
        // here (the box's handlers above take them, and the ComboBox marks its other keys Handled), and
        // a focused Button or link takes Enter as its own click, so in practice this serves Escape.
        // Same rule as the box: the key-down only ARMS and the key-up acts, so an Escape that closes the
        // prompt cannot hand its ESC character to whatever takes focus next (a terminal running claude
        // reads it as an interrupt). Same Ctrl / Alt exclusion too. `cardArmedKey` is separate from the
        // box's `armedKey`, so each pair of handlers acts only on key-downs it saw itself.
        auto cardArmedKey = std::make_shared<winrt::Windows::System::VirtualKey>(winrt::Windows::System::VirtualKey::None);
        card.KeyDown([cardArmedKey](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::KeyRoutedEventArgs& e) {
            const auto key = e.Key();
            *cardArmedKey = winrt::Windows::System::VirtualKey::None;
            if (key != winrt::Windows::System::VirtualKey::Enter && key != winrt::Windows::System::VirtualKey::Escape)
            {
                return;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
            {
                return; // an app chord (Alt+Enter = full screen): not ours
            }
            *cardArmedKey = key;
            e.Handled(true);
        });
        card.KeyUp([cardArmedKey, commit, finish](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::KeyRoutedEventArgs& e) {
            const auto key = e.Key();
            if (*cardArmedKey == winrt::Windows::System::VirtualKey::None || key != *cardArmedKey)
            {
                return; // its key-down did not reach the card (or was not Enter / Escape)
            }
            *cardArmedKey = winrt::Windows::System::VirtualKey::None;
            // Set before acting: the commit or cancel can close the card synchronously.
            e.Handled(true);
            try
            {
                if (key == winrt::Windows::System::VirtualKey::Enter)
                {
                    commit();
                }
                else
                {
                    finish(winrt::hstring{});
                }
            }
            catch (...)
            {
                // As in the box's key-up: nothing to recover, but the throw must stay diagnosable.
                ::Agentmaster::LogSwallowedException(L"specify-model card key");
            }
        });

        // Agentmaster (leak fix, 2026-10-14): the handler holds the box and the status line WEAKLY and reaches
        // the button through `sender`. Captured by value, the button sat in its own Click delegate (a
        // reference cycle nothing breaks), and the delegate kept the box and the status line alive with it,
        // so every opened prompt left its card behind. The worker thread below still captures strong
        // copies: those live only until the fetch's UI tail has run.
        fetchBtn.Click([weakBox = winrt::make_weak(box), weakStatus = winrt::make_weak(status), configuredIds](const winrt::Windows::Foundation::IInspectable& sender, const WUX::RoutedEventArgs&) {
            const auto fetchBtn = sender.try_as<WUXC::Button>();
            const auto box = weakBox.get();
            const auto status = weakStatus.get();
            if (!fetchBtn || !box || !status)
            {
                return; // the card is being torn down: nothing to fetch for
            }
            fetchBtn.IsEnabled(false);
            status.Text(L"Fetching model lists\x2026");
            status.Visibility(WUX::Visibility::Visible);
            // DispatcherQueue, NOT DependencyObject::Dispatcher(): the CoreDispatcher is not
            // dependable under XAML Islands (AgentManagerContent uses the queue throughout for
            // exactly this reason), and a null one here would mean the fetch result never lands.
            const auto dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            // Detached worker: WinHTTP blocks, and the UI thread must keep pumping. Everything the
            // thread touches is captured BY VALUE; the only UI contact is the marshalled tail below.
            try
            {
                std::thread([box, status, fetchBtn, dispatcher, configuredIds]() {
                    // Kept SEPARATE (not one merged pile) so the drop-down can order them
                    // Anthropic-before-Codex regardless of which arrived first on the wire.
                    std::vector<std::wstring> claudeIds, codexIds;
                    size_t fetched = 0;
                    std::wstring note;
                    try
                    {
                        // Codex first on the WIRE (it is public, so it is the half that reliably
                        // arrives) - but it lists LAST in the drop-down; see the merge below.
                        std::wstring err;
                        const auto codexBody = ::Agentmaster::Updater::HttpsGet(std::wstring{ ::Agentmaster::kCodexModelsHost },
                                                                                std::wstring{ ::Agentmaster::kCodexModelsPath },
                                                                                8000,
                                                                                err,
                                                                                L"User-Agent: Agentmaster\r\n");
                        codexIds = ::Agentmaster::CatalogModelIds(::Agentmaster::ParseCodexModelsJson(::Agentmaster::Updater::detail::Utf8ToWide(codexBody)));

                        // Claude: only when the environment carries a key. ANTHROPIC_API_KEY is the
                        // SDK's own variable; ANTHROPIC_AUTH_TOKEN is the bearer form used with a
                        // gateway/proxy. Neither exists for a plain subscription login.
                        std::wstring key = ::Agentmaster::Updater::detail::GetEnv(L"ANTHROPIC_API_KEY");
                        std::wstring bearer;
                        if (key.empty())
                        {
                            bearer = ::Agentmaster::Updater::detail::GetEnv(L"ANTHROPIC_AUTH_TOKEN");
                        }
                        if (!key.empty() || !bearer.empty())
                        {
                            std::wstring headers = L"User-Agent: Agentmaster\r\nanthropic-version: " + std::wstring{ ::Agentmaster::kAnthropicVersionHeader } + L"\r\n";
                            headers += key.empty() ? (L"Authorization: Bearer " + bearer + L"\r\n") : (L"x-api-key: " + key + L"\r\n");
                            std::wstring aerr;
                            const auto body = ::Agentmaster::Updater::HttpsGet(std::wstring{ ::Agentmaster::kAnthropicModelsHost },
                                                                               std::wstring{ ::Agentmaster::kAnthropicModelsPath },
                                                                               8000,
                                                                               aerr,
                                                                               headers);
                            claudeIds = ::Agentmaster::CatalogModelIds(::Agentmaster::ParseAnthropicModelsJson(::Agentmaster::Updater::detail::Utf8ToWide(body)));
                            if (claudeIds.empty())
                            {
                                note = aerr.empty() ? std::wstring{ L"the Claude list came back empty" } : (L"Claude list: " + aerr);
                            }
                        }
                        else
                        {
                            note = L"no ANTHROPIC_API_KEY set, so only the Codex list could be fetched \x2014 open the Claude link above for that one";
                        }
                        fetched = claudeIds.size() + codexIds.size();
                    }
                    catch (...)
                    {
                        ::Agentmaster::LogSwallowedException(L"specify-model catalog fetch");
                    }
                    // Back to the UI thread to touch any XAML. A failed enqueue just means the window
                    // went away mid-fetch — nothing to do, and nothing leaked (all captures are values).
                    try
                    {
                        if (!dispatcher)
                        {
                            return; // no UI lane to come back on (never observed on a UI thread)
                        }
                        dispatcher.TryEnqueue([box, status, fetchBtn, configuredIds, claudeIds, codexIds, fetched, note]() {
                            try
                            {
                                fetchBtn.IsEnabled(true);
                                if (fetched > 0)
                                {
                                    // Fetch is the RESET: the saved list becomes exactly what came
                                    // back (Anthropic first, then Codex), replacing whatever it held
                                    // - a previous fetch and any ids specified since. The configured
                                    // Launch models are NOT part of it; they live in settings and are
                                    // merged in front here, so they survive every reset by design.
                                    const auto fetchedIds = ::Agentmaster::MergeModelIdGroups({ claudeIds, codexIds });
                                    ::Agentmaster::ResetModelList(fetchedIds);
                                    const std::wstring keep{ box.Text() }; // repopulating Items() clears the typed text
                                    box.Items().Clear();
                                    for (const auto& id : ::Agentmaster::MergeModelIdGroups({ configuredIds, fetchedIds }))
                                    {
                                        box.Items().Append(winrt::box_value(winrt::hstring{ id }));
                                    }
                                    box.Text(winrt::hstring{ keep });
                                    std::wstring msg = L"List rebuilt from " + std::to_wstring(fetched) + L" fetched models (Claude " + std::to_wstring(claudeIds.size()) +
                                                       L", Codex " + std::to_wstring(codexIds.size()) + L") \x2014 saved; your Launch models stay on top.";
                                    if (!note.empty())
                                    {
                                        msg += L" (" + note + L")";
                                    }
                                    status.Text(winrt::hstring{ msg });
                                }
                                else
                                {
                                    status.Text(winrt::hstring{ note.empty() ? std::wstring{ L"Could not fetch a model list \x2014 use the links above and type the id." } : (L"Could not fetch: " + note) });
                                }
                                status.Visibility(WUX::Visibility::Visible);
                            }
                            catch (...)
                            {
                                ::Agentmaster::LogSwallowedException(L"specify-model catalog apply");
                            }
                        });
                    }
                    catch (...)
                    {
                        ::Agentmaster::LogSwallowedException(L"specify-model catalog marshal");
                    }
                }).detach();
            }
            catch (...)
            {
                // std::thread construction can throw (resource exhaustion) — recover the button, or
                // Fetch would be dead for the life of this prompt.
                fetchBtn.IsEnabled(true);
                status.Text(L"Could not start the fetch \x2014 use the links above and type the id.");
                ::Agentmaster::LogSwallowedException(L"specify-model catalog spawn");
            }
        });

        AgentSpecifyModelCard out;
        out.card = card;
        out.box = box;
        return out;
    }
}
