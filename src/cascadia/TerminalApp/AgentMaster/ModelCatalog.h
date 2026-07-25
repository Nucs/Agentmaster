// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the MODEL CATALOG: turning a provider's published model list into the
// {id, display name} rows the launch-model picker's "Specify a model..." prompt offers.
//
// The prompt links the two authoritative lists a user would otherwise have to hunt for:
//   * Claude — https://platform.claude.com/docs/en/api/go/models/list  (GET /v1/models)
//   * Codex  — https://github.com/openai/codex/blob/main/codex-rs/models-manager/models.json
// and can also FETCH them, so the id lands in the box instead of being typed from a browser tab.
//
// This header is the PURE half — text in, rows out, no I/O and no winhttp (the PromptAnchor.h /
// PendingInput.h idiom). That split is deliberate:
//   * it keeps the parsers unit-testable in the standalone harness (and linkable by the CLI)
//     with ZERO new libraries — the fetch lives in the UI layer (AgentModelMenu.h), which already
//     links winhttp through Updater.h's #pragma comment(lib);
//   * a provider changing its JSON is then a pure-function test change, not a network test.
// Input is UTF-16 (the caller decodes the UTF-8 body once — winrt::to_hstring does it), so this
// header needs neither windows.h nor a codepage conversion.
//
// TOTAL + never-throwing by construction: anything unrecognized yields an EMPTY vector, never a
// partial guess — the prompt then just says it could not read the list and the user types the id,
// which is the behavior with no network at all. A model list is a CONVENIENCE; the typed box is
// the contract.

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Json.h"

namespace Agentmaster
{
    // One row of a provider's catalog. `id` is what goes on the wire (`--model <id>`); `displayName`
    // is the human label the prompt shows beside it ("" when the provider publishes none — the
    // caller then shows the id alone).
    struct CatalogModel
    {
        std::wstring id;
        std::wstring displayName;
    };

    // Which published list to read. The two shapes are different enough (and stable enough) to
    // parse explicitly rather than sniff — a mis-sniffed list would silently offer wrong ids.
    enum class ModelCatalogSource
    {
        Claude, // api.anthropic.com/v1/models      -> {"data":[{"id":…,"display_name":…}, …]}
        Codex, // codex-rs/models-manager/models.json -> {"models":[{"slug":…,"display_name":…}, …]}
    };

    namespace detail
    {
        // Drop ids that are empty or duplicated (a paginated Claude fetch can legitimately repeat
        // one across pages). First occurrence wins — providers list newest-first.
        inline std::vector<CatalogModel> DedupeCatalog(std::vector<CatalogModel> rows)
        {
            std::unordered_set<std::wstring> seen;
            std::vector<CatalogModel> out;
            out.reserve(rows.size());
            for (auto& r : rows)
            {
                if (r.id.empty() || !seen.insert(r.id).second)
                {
                    continue;
                }
                out.push_back(std::move(r));
            }
            return out;
        }
    }

    // Claude: the Messages API's model list — `{"data":[{"type":"model","id":"claude-opus-4-5-…",
    // "display_name":"Claude Opus 4.5","created_at":"…"}], "has_more":false, …}`. We take `id`
    // (the `--model` value) and `display_name`; everything else (pagination cursors, created_at)
    // is the caller's business, not the picker's.
    inline std::vector<CatalogModel> ParseAnthropicModelsJson(std::wstring_view json)
    {
        std::vector<CatalogModel> out;
        const auto parsed = json::Parse(json);
        if (!parsed)
        {
            return out;
        }
        const auto* data = parsed->Find(L"data");
        if (!data || data->type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const auto& m : data->arr)
        {
            if (m.type != json::Value::Type::Obj)
            {
                continue;
            }
            CatalogModel row;
            row.id = m.StrAt(L"id");
            row.displayName = m.StrAt(L"display_name");
            out.push_back(std::move(row));
        }
        return detail::DedupeCatalog(std::move(out));
    }

    // Codex: the models-manager catalog — `{"models":[{"slug":"gpt-5.6-sol","display_name":…,
    // "description":…, …40 more tuning fields}]}`. The `slug` IS the id (`codex --model <slug>`).
    // A model marked hidden by `visibility` is still listed: the field is Codex's own UI hint, and
    // a user who came here to type an id explicitly is not served by us second-guessing it.
    inline std::vector<CatalogModel> ParseCodexModelsJson(std::wstring_view json)
    {
        std::vector<CatalogModel> out;
        const auto parsed = json::Parse(json);
        if (!parsed)
        {
            return out;
        }
        const auto* models = parsed->Find(L"models");
        if (!models || models->type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const auto& m : models->arr)
        {
            if (m.type != json::Value::Type::Obj)
            {
                continue;
            }
            CatalogModel row;
            row.id = m.StrAt(L"slug");
            row.displayName = m.StrAt(L"display_name");
            out.push_back(std::move(row));
        }
        return detail::DedupeCatalog(std::move(out));
    }

    // The one entry point the UI calls once it has a body — dispatches on the source so the caller
    // never has to know which key holds the array.
    inline std::vector<CatalogModel> ParseModelCatalog(ModelCatalogSource source, std::wstring_view json)
    {
        return source == ModelCatalogSource::Claude ? ParseAnthropicModelsJson(json) : ParseCodexModelsJson(json);
    }

    // Just the ids, in order — what a drop-down actually lists.
    inline std::vector<std::wstring> CatalogModelIds(const std::vector<CatalogModel>& rows)
    {
        std::vector<std::wstring> out;
        out.reserve(rows.size());
        for (const auto& r : rows)
        {
            out.push_back(r.id);
        }
        return out;
    }

    // Merge ordered id GROUPS into the one list the "Specify a model..." drop-down shows: group
    // order is preserved, first occurrence of an id wins, and a repeat is dropped case-INsensitively
    // ("Opus" and "opus" are one model, matching PushRecentModel's rule).
    //
    // Group order IS the product decision, so it lives here where a test can pin it:
    //   1. the CONFIGURED models (Settings -> Launch models)  — always present, whatever else happens
    //   2. the recently-typed MRU
    //   3. the fetched ANTHROPIC catalog
    //   4. the fetched CODEX catalog
    // Anthropic before Codex because this picker launches Claude sessions (a Codex id is the
    // occasional cross-reference, not the common case), and the configured list first because it is
    // the user's own curated set — it must never be pushed out by a fetch that returns hundreds of
    // ids, nor replaced by one that (with no ANTHROPIC_API_KEY) can only return Codex's.
    inline std::vector<std::wstring> MergeModelIdGroups(const std::vector<std::vector<std::wstring>>& groups)
    {
        const auto fold = [](const std::wstring& s) {
            std::wstring f;
            f.reserve(s.size());
            for (wchar_t c : s)
            {
                f.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
            }
            return f;
        };
        std::unordered_set<std::wstring> seen;
        std::vector<std::wstring> out;
        for (const auto& g : groups)
        {
            for (const auto& id : g)
            {
                if (id.empty() || !seen.insert(fold(id)).second)
                {
                    continue;
                }
                out.push_back(id);
            }
        }
        return out;
    }

    // ---- the endpoints (constants here so the fetch site and the docs link can never disagree) ----

    // Claude's model list is an AUTHENTICATED API: `GET https://api.anthropic.com/v1/models` with
    // `x-api-key: <key>` + `anthropic-version`. That key is the reason the fetch is best-effort —
    // Claude Code's usual auth is an OAuth subscription login, which issues NO api key, so most
    // installs have nothing to send (verified: the endpoint answers 401 unauthenticated). We read
    // the standard SDK env vars and, when neither is set, say so plainly instead of failing blind.
    inline constexpr std::wstring_view kAnthropicModelsHost = L"api.anthropic.com";
    inline constexpr std::wstring_view kAnthropicModelsPath = L"/v1/models?limit=100";
    inline constexpr std::wstring_view kAnthropicVersionHeader = L"2023-06-01";
    // The human-readable page the prompt links (what the user opens to read the list themselves).
    inline constexpr std::wstring_view kAnthropicModelsDocsUrl = L"https://platform.claude.com/docs/en/api/go/models/list";

    // Codex's catalog is a PUBLIC file in the codex repo — no credentials, so this fetch works for
    // everyone. raw.githubusercontent.com serves the file bytes (the /blob/ URL below is the HTML
    // page a human reads; fetching that would parse GitHub's markup, not the JSON).
    inline constexpr std::wstring_view kCodexModelsHost = L"raw.githubusercontent.com";
    inline constexpr std::wstring_view kCodexModelsPath = L"/openai/codex/main/codex-rs/models-manager/models.json";
    inline constexpr std::wstring_view kCodexModelsDocsUrl = L"https://github.com/openai/codex/blob/main/codex-rs/models-manager/models.json";
}
