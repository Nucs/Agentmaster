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
#include "AgentMaster/Persistence.h" // LoadRecentModels / RememberRecentModel - the "Specify..." MRU
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
    // The input is an EDITABLE ComboBox: you can type any id, and the drop-down lists the ids you
    // used before (recent-models.json, most recent first) followed by the ones your Launch-models
    // list already configures — so the second time you need an unusual model it is one click.
    struct AgentSpecifyModelCard
    {
        winrt::Windows::UI::Xaml::Controls::Border card{ nullptr }; // parent THIS
        winrt::Windows::UI::Xaml::Controls::ComboBox box{ nullptr }; // focus THIS after showing
    };

    // The drop-down's BASE contents, present on every open and NEVER dropped afterwards: the
    // CONFIGURED launch models (Settings -> Launch models) first, then the recently-typed MRU.
    // Ordering + dedupe rules live in MergeModelIdGroups (ModelCatalog.h), where they are tested.
    inline std::vector<std::wstring> AgentSpecifyModelSuggestions(const std::vector<std::pair<std::wstring, std::wstring>>& configured)
    {
        std::vector<std::wstring> configuredIds;
        configuredIds.reserve(configured.size());
        for (const auto& [name, id] : configured)
        {
            configuredIds.push_back(id);
        }
        return ::Agentmaster::MergeModelIdGroups({ configuredIds, ::Agentmaster::LoadRecentModels() });
    }

    // Build the prompt. `onDone` fires EXACTLY once: with the trimmed id on Use, or with "" on
    // Cancel/Escape (the host closes its container either way). Committing also pushes the id onto
    // the recent-models MRU, which is what fills the drop-down next time.
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
        sub.Text(L"Launched as --model <id> for this one session. Type an id, or pick one from the list \x2014 your Launch models, then the ones you used before.");
        sub.FontSize(12);
        sub.Opacity(0.75);
        sub.TextWrapping(WUX::TextWrapping::Wrap);
        body.Children().Append(sub);

        // The editable drop-down. IsEditable is what makes a ComboBox a "type OR pick" control; its
        // Text property carries the typed value (SelectedItem alone would only ever return a listed id).
        //
        // `base` — your configured Launch models followed by the recently-typed MRU — is the list the
        // drop-down ALWAYS shows. It is captured here and re-applied by the fetch handler, so a fetch
        // can only ever ADD to it (the settings models can never be pushed out or replaced).
        const std::vector<std::wstring> base = AgentSpecifyModelSuggestions(configured);
        WUXC::ComboBox box;
        box.IsEditable(true);
        box.HorizontalAlignment(WUX::HorizontalAlignment::Stretch);
        box.PlaceholderText(L"e.g. opus, claude-opus-4-8, gpt-5.6-sol");
        for (const auto& s : base)
        {
            box.Items().Append(winrt::box_value(winrt::hstring{ s }));
        }
        box.Text(winrt::hstring{ seed });
        AgentSetTip(box, L"The exact value passed to --model. Anything the CLI accepts works: an ALIAS (opus / sonnet / fable) always launches the latest model of that family, a full id (claude-opus-4-8) pins that exact version. The drop-down always lists your Launch models (Settings \x2192 Sessions) first, then the ids you typed before, then anything Fetch loaded \x2014 Claude before Codex.");
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
        addLink(L"Claude models \x2197", ::Agentmaster::kAnthropicModelsDocsUrl, L"Anthropic's Models API reference \x2014 GET /v1/models lists every model id your account can use.");
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
        fetchBtn.Content(winrt::box_value(winrt::hstring{ L"Fetch list" }));
        AgentSetTip(fetchBtn, L"Download the published model lists and ADD them to the drop-down above \x2014 Claude ids first, then Codex; your Launch models and recent ids always stay on top. The Codex catalog is public; the Claude list needs an ANTHROPIC_API_KEY in your environment (a Claude subscription login does not issue one) \x2014 the status line says which half arrived.");
        actions.Children().Append(fetchBtn);

        WUXC::Button useBtn;
        useBtn.Content(winrt::box_value(winrt::hstring{ L"Use this model" }));
        AgentSetTip(useBtn, L"Launch with the id in the box (and remember it for next time).");
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
        // `fired` makes onDone exactly-once across every path (Use / Cancel / Escape), so a host
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

        const auto commit = [box, status, finish]() {
            std::wstring text{ box.Text() };
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
                box.Focus(WUX::FocusState::Programmatic);
                return;
            }
            ::Agentmaster::RememberRecentModel(id); // the drop-down's contents next time
            finish(winrt::hstring{ id });
        };

        useBtn.Click([commit](const winrt::Windows::Foundation::IInspectable&, const WUX::RoutedEventArgs&) { commit(); });
        cancelBtn.Click([finish](const winrt::Windows::Foundation::IInspectable&, const WUX::RoutedEventArgs&) { finish(winrt::hstring{}); });
        // Enter commits / Escape cancels from anywhere in the card (the box, a button, the list).
        card.KeyDown([commit, finish](const winrt::Windows::Foundation::IInspectable&, const WUX::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
            {
                e.Handled(true);
                commit();
            }
            else if (e.Key() == winrt::Windows::System::VirtualKey::Escape)
            {
                e.Handled(true);
                finish(winrt::hstring{});
            }
        });

        fetchBtn.Click([box, status, fetchBtn, base](const winrt::Windows::Foundation::IInspectable&, const WUX::RoutedEventArgs&) {
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
                std::thread([box, status, fetchBtn, dispatcher, base]() {
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
                        dispatcher.TryEnqueue([box, status, fetchBtn, base, claudeIds, codexIds, fetched, note]() {
                            try
                            {
                                fetchBtn.IsEnabled(true);
                                if (fetched > 0)
                                {
                                    // REBUILD, never replace: the configured Launch models + the MRU
                                    // (base) stay at the top no matter what a fetch returns, then
                                    // Anthropic ids, then Codex ids. Without this the box was cleared
                                    // to only the FETCHED list - which, with no ANTHROPIC_API_KEY,
                                    // means only Codex, hiding the configured models entirely.
                                    const std::wstring keep{ box.Text() }; // repopulating Items() clears the typed text
                                    box.Items().Clear();
                                    for (const auto& id : ::Agentmaster::MergeModelIdGroups({ base, claudeIds, codexIds }))
                                    {
                                        box.Items().Append(winrt::box_value(winrt::hstring{ id }));
                                    }
                                    box.Text(winrt::hstring{ keep });
                                    std::wstring msg = std::to_wstring(fetched) + L" models fetched (Claude " + std::to_wstring(claudeIds.size()) +
                                                       L", Codex " + std::to_wstring(codexIds.size()) + L") \x2014 open the drop-down; your Launch models stay on top.";
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
