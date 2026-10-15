// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the LIVE-BRANCH WALK over stored line rows (SESSIONS_DB.md §8.5: "the live branch
// walked in C++ at ingest").
//
// This is TranscriptStore::ActiveBranchUuids restated over the database's per-line columns instead of
// a transcript's text: the same nodes (every uuid line, copies included — each occurrence adds its
// parent edge), the same compaction crossings (a root's logical parent and preserved messages, stored
// in line_edge; the pre-boundary leaf, computed here from positions), the same stale-marker extension
// and the same cycle guards. The rule set is documented there at length; the comments here only mark
// where each step is. It must stay equal to that function: the database's summary (D14) and the app's
// analyzer must agree on which lines a double-ESC rewind abandoned. The m5 harness runs both over the
// same fixtures (TestSessionDbWalk).
//
// Pure, header-only.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Agentmaster::SessionDb
{
    // One stored line, as the walk reads it (file order).
    struct WalkRow
    {
        int32_t lineNo{ 0 };
        std::wstring uuid; // "" for state / marker lines
        std::wstring parent; // parentUuid ("" for a root)
        std::wstring leaf; // leafUuid (a leaf marker), "" otherwise
        bool sidechain{ false }; // isSidechain (never the main tail)
        bool compactRoot{ false }; // a compaction root (its crossing edges are in `extraEdges`)
    };

    // The set of live-branch uuids of one transcript, given its rows in file order and each compaction
    // root's crossing edges (line number -> targets).
    // Returns an EMPTY set when the file has no leaf marker, or its leaf names no node of the file —
    // the caller then keeps every line on the branch (ActiveBranchUuids' contract). Pure; cycle-safe.
    inline std::unordered_set<std::wstring> ActiveBranchFromRows(const std::vector<WalkRow>& rows,
                                                                 const std::unordered_map<int32_t, std::vector<std::wstring>>& extraEdges)
    {
        std::unordered_map<std::wstring, std::vector<std::wstring>> upEdges;
        const auto addEdge = [&upEdges](const std::wstring& node, const std::wstring& target) {
            if (target.empty() || target == node)
            {
                return;
            }
            auto& edges = upEdges[node];
            if (std::find(edges.begin(), edges.end(), target) == edges.end())
            {
                edges.push_back(target);
            }
        };
        struct PreBoundaryLeaf
        {
            std::wstring markerLeaf;
            std::wstring tailBefore;
            bool nodeAfterMarker{ false };
        };
        std::unordered_map<std::wstring, PreBoundaryLeaf> preBoundaryOf;
        std::wstring leaf;
        std::wstring tailUuid;
        std::wstring mainTailUuid;
        bool nodeAfterMarker = false;

        // Pass 1: every node's upward edges, the last leaf marker, the physical tail.
        for (const WalkRow& r : rows)
        {
            if (!r.leaf.empty())
            {
                leaf = r.leaf;
                nodeAfterMarker = false;
            }
            if (r.uuid.empty())
            {
                continue;
            }
            const bool firstSighting = upEdges.find(r.uuid) == upEdges.end();
            upEdges.try_emplace(r.uuid);
            addEdge(r.uuid, r.parent);
            if (r.parent.empty() && r.compactRoot)
            {
                if (const auto it = extraEdges.find(r.lineNo); it != extraEdges.end())
                {
                    for (const auto& t : it->second)
                    {
                        addEdge(r.uuid, t);
                    }
                }
                if (firstSighting)
                {
                    preBoundaryOf.emplace(r.uuid, PreBoundaryLeaf{ leaf, mainTailUuid, nodeAfterMarker });
                }
            }
            tailUuid = r.uuid;
            if (!r.sidechain)
            {
                mainTailUuid = r.uuid;
            }
            nodeAfterMarker = true;
        }

        // Each compaction root's pre-boundary leaf (ActiveBranchUuids' resolution order).
        for (const auto& [root, pre] : preBoundaryOf)
        {
            const bool tailKnown = !pre.tailBefore.empty() && upEdges.find(pre.tailBefore) != upEdges.end();
            const bool markerKnown = !pre.markerLeaf.empty() && upEdges.find(pre.markerLeaf) != upEdges.end();
            if (pre.nodeAfterMarker && tailKnown)
            {
                addEdge(root, pre.tailBefore);
            }
            else if (markerKnown)
            {
                addEdge(root, pre.markerLeaf);
            }
            else if (tailKnown)
            {
                addEdge(root, pre.tailBefore);
            }
        }

        std::unordered_set<std::wstring> active;
        if (leaf.empty() || upEdges.find(leaf) == upEdges.end())
        {
            return active; // no usable marker: keep every line
        }
        // The stale-marker extension: a tail written after the marker that descends from it is the live tip.
        std::wstring anchor = leaf;
        if (nodeAfterMarker && !tailUuid.empty() && tailUuid != leaf)
        {
            std::unordered_set<std::wstring> up;
            std::vector<std::wstring> pending{ tailUuid };
            while (!pending.empty())
            {
                std::wstring c = std::move(pending.back());
                pending.pop_back();
                if (c.empty() || !up.insert(c).second)
                {
                    continue;
                }
                if (c == leaf)
                {
                    anchor = tailUuid;
                    break;
                }
                if (const auto it = upEdges.find(c); it != upEdges.end())
                {
                    pending.insert(pending.end(), it->second.begin(), it->second.end());
                }
            }
        }
        // Pass 2: everything reachable upward from the anchor.
        std::vector<std::wstring> pending{ anchor };
        while (!pending.empty())
        {
            std::wstring cur = std::move(pending.back());
            pending.pop_back();
            if (cur.empty() || !active.insert(cur).second)
            {
                continue;
            }
            if (const auto it = upEdges.find(cur); it != upEdges.end())
            {
                pending.insert(pending.end(), it->second.begin(), it->second.end());
            }
        }
        return active;
    }
}
