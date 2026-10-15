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

    // What one TextIndex::ReclusterLines call did (the service log's re-cluster line).
    struct LineResortTally
    {
        int64_t slices{ 0 }; // slices planned (from the resume point on)
        int64_t inPlace{ 0 }; // slices found in place and left alone
        int64_t moved{ 0 }; // slices moved to the end of the table
        int64_t movedBytes{ 0 }; // their planned raw bytes
        bool resumed{ false }; // continued a re-sort an earlier call left unfinished
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

        // The most raw JSON one sorting statement over the line store may hold, in bytes (CopyLinesSorted in
        // SessionDbIndex.cpp). DuckDB's sort and the ZSTD column writer together outgrew a 1 GB memory limit on a
        // 1.6 GB line table ("failed to pin block"), while a 0.8 GB slice passed (measured 2026-10-15), so the
        // store sets a quarter of its memory limit here (Store::Open). ⚠ Too large brings the out-of-memory
        // error back; too small only costs more statements (each scans the source's key column once).
        int64_t sortChunkBytes{ 256ll * 1024 * 1024 };

        // Rows waiting in doc_hot (an estimate kept by the store; a merge reads the table).
        // Merge every doc_hot block into the index in ONE transaction, then move line_hot into line.
        //   con    — the writer connection (no transaction open).
        //   cancel — polled between steps; a cancelled merge rolls back and changes nothing.
        // Returns the number of blocks merged. Throws DbError.
        size_t Merge(Connection& con, const std::function<bool()>& cancel);

        // Re-cluster, part 1 — the text index (§6.4): rewrite `post` sorted by (term, doc) without the
        // tombstoned blocks, recompute `spost` exactly from it (one row per term and session), delete the
        // tombstoned blocks from `doc` / `doc_tool`, empty `tomb`, and recompute df / sf / N / avgdl. ONE
        // transaction. Every sort in it is over fixed-width keys, which DuckDB spills to disk; `doc` and
        // `doc_tool` are never sorted, because every merge appends them in doc_id order and a delete keeps that
        // order (the sorted copy of their TEXT is what the old one-statement re-cluster could not fit in memory).
        //   con    — the writer connection (no transaction open).
        //   cancel — polled before the transaction starts; may be empty.
        // Returns false when cancelled (nothing changed). Throws DbError (the transaction rolled back).
        bool ReclusterPostings(Connection& con, const std::function<bool()>& cancel);
        // Re-cluster, part 2 — the line store (§6.8): put `line` in line_id order (one file = one id range), slice
        // by slice (PlanLineSlices: about sortChunkBytes of raw JSON each, whole files where they fit). The
        // longest prefix of slices already in place (contiguous rows, in order) stays where it is; every later
        // slice moves to the END of the table in its own transaction (insert it sorted, delete the old copies),
        // which records the resume point (meta `line_resort_from`). So a cancel loses at most the slice in flight,
        // the next call continues where this one stopped, and the file grows by one slice at a time (a whole-table
        // copy would double it). Minutes on a large store (the ZSTD writer runs at ~30-90 MB/s). At the end the
        // run count (LineRuns) goes back to 1, plus the runs merges added meanwhile.
        //   con    — the writer connection (no transaction open). line_hot is left alone: merge first.
        //   cancel — polled before each slice; may be empty.
        //   tally  — out (optional): what this call did, for the log.
        // Returns true when the re-sort finished, false when cancelled (resumable). Throws DbError (the slice
        // in flight rolled back; the moved ones stay, and the resume point names the next).
        bool ReclusterLines(Connection& con, const std::function<bool()>& cancel, LineResortTally* tally);
        // True while a re-sort of the line store is under way (a slice moved, later ones remain): the store keeps
        // the re-cluster due until it finishes. Throws DbError.
        bool LineResortInProgress(Connection& con);
        // Both parts, postings first (the one-shot `recluster` mode). A cancel before a part skips the rest.
        // Throws DbError.
        void Recluster(Connection& con, const std::function<bool()>& cancel);
        // How many sorted runs `line` holds since its last rewrite: 1 for a sorted table, +1 for every merge whose
        // lines started below the table's largest line_id (meta `line_runs`; absent = 1). The store re-sorts the
        // line table when this passes kLineRunsForResort (SessionDbStore.cpp). Throws DbError.
        int64_t LineRuns(Connection& con);

    private:
        // Move line_hot into line as ONE sorted run (CopyLinesSorted) and empty line_hot, counting a new run in
        // meta `line_runs` when the hot lines start below the line store's largest id. Runs inside the caller's
        // transaction (both merge paths). Throws DbError.
        void _moveHotLines(Connection& con);

        Dictionary _dict;
    };
}
