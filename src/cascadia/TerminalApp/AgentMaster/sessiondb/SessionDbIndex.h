// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's TEXT INDEX (SESSIONS_DB.md §6.3, §6.4, §6.8): the dictionary
// held in memory, the merge that moves new blocks from the hot tier into the inverted index, the
// re-cluster that keeps the postings sorted, and the term expansion the planner asks for.
//
// The LSM shape (§6.4): ingest appends blocks to doc_hot (searchable at once by a scan); a MERGE
// tokenizes them (SessionDbTokenizer), appends their postings to `post` / `spost` sorted by term, moves
// the rows into `doc` (a tool block's text into the ZSTD `doc_tool`) and updates the dictionary. A
// RE-CLUSTER rewrites `post` / `spost` / `doc` / `doc_tool` / `line` in key order so zonemaps prune range
// reads. Neither changes a search result — only its speed.
//
// Threading: the dictionary is read by query threads (TermsContaining / Lookup) while the writer merges;
// a shared_mutex guards it (merges take the exclusive lock only for the final swap of new terms).

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../SessionDbTokenizer.h"

namespace Agentmaster::SessionDb
{
    class Connection;

    // The ICU-backed classifier the service tokenizes and folds with (letters of every script, combining
    // marks, digits; Unicode simple lower-case mapping).
    SessionDbTokenizer::CharClass IcuClassify(char32_t cp);
    // The classifier as a std::function (what the tokenizer API takes).
    const SessionDbTokenizer::Classifier& ServiceClassifier();
    // Fold text the way the index does (FoldText with the ICU classifier), capped at the index cap.
    std::wstring FoldForIndex(std::wstring_view text);

    // One dictionary term.
    struct DictTerm
    {
        uint32_t id{ 0 };
        uint32_t df{ 0 }; // blocks holding it
        uint32_t sf{ 0 }; // sessions holding it (an upper bound until the next re-cluster)
    };

    // The in-memory dictionary.
    class Dictionary
    {
    public:
        // Replace the contents with the `dict` table's rows. Throws DbError.
        void Load(Connection& con);
        // Number of terms.
        size_t Size() const;
        // The id of an exact term, or 0 (ids start at 1).
        uint32_t Lookup(std::wstring_view term) const;
        // The term's statistics (zero for an unknown id).
        DictTerm Stats(uint32_t id) const;
        // The ids of every term that CONTAINS `needle` (rule 1 of §6.3), capped at `limit` (0 = no cap).
        // `prefix` / `suffix` restrict to terms that START / END with it (rule 2's first / last token).
        std::vector<uint32_t> TermsContaining(std::wstring_view needle, size_t limit, bool prefix = false, bool suffix = false) const;
        // The ids of every term holding `needle`'s characters IN ORDER with any gap (the fuzzy (F) expansion
        // over the dictionary, D9), capped at `limit` (0 = no cap). `needle` is folded.
        std::vector<uint32_t> TermsWithSubsequence(std::wstring_view needle, size_t limit) const;
        // The term text of an id ("" for an unknown id).
        std::wstring TermText(uint32_t id) const;
        // Total number of blocks indexed (BM25's N) and their average token length (avgdl).
        uint64_t DocCount() const;
        double AvgDocLength() const;

        // --- merge side (writer thread) ---
        // Get-or-create the id of `term` (a new term gets df = sf = 0). Exclusive lock inside.
        uint32_t Intern(const std::wstring& term);
        // Add to a term's df / sf. Exclusive lock inside.
        void AddCounts(uint32_t id, uint32_t df, uint32_t sf);
        // Record a merged batch's block count and token total (for N / avgdl).
        void AddDocs(uint64_t docs, uint64_t tokens);
        // Persist the terms that are new or changed since the last save (the `dict` table). Throws DbError
        // (the terms stay marked, so the next save writes them).
        void Save(Connection& con);
        // Reset the corpus statistics (a re-cluster recomputes them). Exclusive lock inside.
        void SetDocStats(uint64_t docs, uint64_t tokens);

    private:
        // Rebuild the concatenated search buffer (`_blob`, `_starts`) after terms were added. Caller holds
        // the exclusive lock.
        void _rebuildBlobLocked() const;

        mutable std::shared_mutex _mtx;
        std::unordered_map<std::wstring, uint32_t> _ids; // term -> id
        std::vector<std::wstring> _terms; // id - 1 -> term
        std::vector<DictTerm> _stats; // id - 1 -> stats
        uint64_t _docs{ 0 };
        uint64_t _tokens{ 0 };
        std::vector<uint32_t> _dirty; // term ids new or changed since the last Save (written incrementally)
        // Every term joined with '\x01' separators, for one fast substring scan over the whole dictionary.
        // Rebuilt lazily (a merge marks it dirty).
        mutable std::wstring _blob;
        mutable std::vector<uint32_t> _starts; // offset of term i (id i + 1) in _blob
        mutable bool _blobDirty{ true };
        mutable std::mutex _blobMtx; // serializes the lazy rebuild (readers hold the shared lock)
    };

    // The merge and re-cluster.
    class TextIndex
    {
    public:
        // Where the merge's step timings go (the service log); may be empty.
        std::function<void(const std::wstring&)> log;

        // Load the dictionary. Throws DbError.
        void Load(Connection& con);
        Dictionary& Dict()
        {
            return _dict;
        }
        const Dictionary& Dict() const
        {
            return _dict;
        }

        // Rows waiting in doc_hot (an estimate kept by the store; a merge reads the table).
        // Merge every doc_hot block into the index in ONE transaction, then move line_hot into line.
        //   con    — the writer connection (no transaction open).
        //   cancel — polled between steps; a cancelled merge rolls back and changes nothing.
        // Returns the number of blocks merged. Throws DbError.
        size_t Merge(Connection& con, const std::function<bool()>& cancel);
        // Rewrite post / spost / doc / doc_tool / line sorted by their read keys, dropping tombstoned blocks and
        // recomputing df / sf / N / avgdl exactly. Throws DbError.
        void Recluster(Connection& con, const std::function<bool()>& cancel);

    private:
        Dictionary _dict;
    };
}
