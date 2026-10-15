// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's text index (see SessionDbIndex.h).

#include "SessionDbIndex.h"

// sdkddkver.h first: the SDK's icu.h gates sections on NTDDI_VERSION (ClaudeInvisibleChars.cpp's rule).
#include <sdkddkver.h>
#include <icu.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // GetTickCount64 (the merge's step timings)

#include <algorithm>
#include <cstdlib> // strtoll (meta values)
#include <functional>
#include <map>
#include <mutex>

#include "DuckDbWrap.h"
#include "SessionDbSchema.h"

// The Windows SDK's ICU import library (icu.dll ships in System32 since Windows 10 1903).
#pragma comment(lib, "icu.lib")

namespace Agentmaster::SessionDb
{
    // ===== characters =================================================================================

    SessionDbTokenizer::CharClass IcuClassify(char32_t cp)
    {
        SessionDbTokenizer::CharClass c{};
        const auto cat = static_cast<UCharCategory>(u_charType(static_cast<UChar32>(cp)));
        switch (cat)
        {
        case U_UPPERCASE_LETTER:
        case U_TITLECASE_LETTER:
            c.word = true;
            c.upper = true;
            break;
        case U_LOWERCASE_LETTER:
            c.word = true;
            c.lower = true;
            break;
        case U_MODIFIER_LETTER:
        case U_OTHER_LETTER:
        case U_NON_SPACING_MARK:
        case U_ENCLOSING_MARK:
        case U_COMBINING_SPACING_MARK:
        case U_DECIMAL_DIGIT_NUMBER:
        case U_LETTER_NUMBER:
            c.word = true; // letters of every script, their combining marks, digits (SESSIONS_DB.md §6.3)
            break;
        default:
            c.word = cp == U'_';
            break;
        }
        c.folded = static_cast<char32_t>(u_tolower(static_cast<UChar32>(cp)));
        return c;
    }

    const SessionDbTokenizer::Classifier& ServiceClassifier()
    {
        static const SessionDbTokenizer::Classifier classifier{ &IcuClassify };
        return classifier;
    }

    std::wstring FoldForIndex(std::wstring_view text)
    {
        if (text.size() > SessionDbTokenizer::kMaxIndexedChars)
        {
            text = text.substr(0, SessionDbTokenizer::kMaxIndexedChars);
        }
        return SessionDbTokenizer::FoldText(text, ServiceClassifier());
    }

    // ===== the dictionary =============================================================================

    void Dictionary::Load(Connection& con)
    {
        std::unordered_map<std::wstring, uint32_t> ids;
        std::vector<std::wstring> terms;
        std::vector<DictTerm> stats;
        auto r = con.Query("SELECT term_id, term, df, sf FROM dict ORDER BY term_id");
        while (r.Next())
        {
            const uint32_t id = static_cast<uint32_t>(r.Int64(0));
            if (id == 0)
            {
                continue;
            }
            // Ids are dense from 1 (the merge assigns them in order); a gap is padded so id - 1 indexes.
            while (terms.size() + 1 < id)
            {
                terms.emplace_back();
                stats.emplace_back();
            }
            std::wstring t = FromUtf8(r.Str(1));
            ids.emplace(t, id);
            terms.push_back(std::move(t));
            stats.push_back(DictTerm{ id, static_cast<uint32_t>(r.Int64(2)), static_cast<uint32_t>(r.Int64(3)) });
        }
        uint64_t docs = 0, tokens = 0;
        {
            auto d = con.Query("SELECT count(*), coalesce(sum(tok_len), 0) FROM doc");
            if (d.Next())
            {
                docs = static_cast<uint64_t>(d.Int64(0));
                tokens = static_cast<uint64_t>(d.Int64(1));
            }
        }
        std::unique_lock lk{ _mtx };
        _ids = std::move(ids);
        _terms = std::move(terms);
        _stats = std::move(stats);
        _docs = docs;
        _tokens = tokens;
        _dirty.clear(); // the table IS the state now
        std::lock_guard bl{ _blobMtx };
        _blobDirty = true;
    }

    size_t Dictionary::Size() const
    {
        std::shared_lock lk{ _mtx };
        return _terms.size();
    }

    uint32_t Dictionary::Lookup(std::wstring_view term) const
    {
        std::shared_lock lk{ _mtx };
        const auto it = _ids.find(std::wstring{ term });
        return it == _ids.end() ? 0 : it->second;
    }

    DictTerm Dictionary::Stats(uint32_t id) const
    {
        std::shared_lock lk{ _mtx };
        return (id == 0 || id > _stats.size()) ? DictTerm{} : _stats[id - 1];
    }

    std::wstring Dictionary::TermText(uint32_t id) const
    {
        std::shared_lock lk{ _mtx };
        return (id == 0 || id > _terms.size()) ? std::wstring{} : _terms[id - 1];
    }

    uint64_t Dictionary::DocCount() const
    {
        std::shared_lock lk{ _mtx };
        return _docs;
    }

    double Dictionary::AvgDocLength() const
    {
        std::shared_lock lk{ _mtx };
        return _docs ? static_cast<double>(_tokens) / static_cast<double>(_docs) : 1.0;
    }

    void Dictionary::_rebuildBlobLocked() const
    {
        // One buffer "\x01term1\x01term2\x01..." so a substring query is ONE linear search over ~11 MB of
        // term text (§6.3 rule 1) instead of a million per-term searches.
        _blob.clear();
        _starts.clear();
        size_t total = 1;
        for (const auto& t : _terms)
        {
            total += t.size() + 1;
        }
        _blob.reserve(total);
        _starts.reserve(_terms.size());
        _blob.push_back(L'\x01');
        for (const auto& t : _terms)
        {
            _starts.push_back(static_cast<uint32_t>(_blob.size()));
            _blob += t;
            _blob.push_back(L'\x01');
        }
        _blobDirty = false;
    }

    std::vector<uint32_t> Dictionary::TermsContaining(std::wstring_view needle, size_t limit, bool prefix, bool suffix) const
    {
        std::vector<uint32_t> out;
        if (needle.empty())
        {
            return out;
        }
        std::shared_lock lk{ _mtx };
        std::lock_guard bl{ _blobMtx };
        if (_blobDirty)
        {
            _rebuildBlobLocked();
        }
        // An exact prefix+suffix request (a whole term) is a hash lookup.
        if (prefix && suffix)
        {
            if (const auto it = _ids.find(std::wstring{ needle }); it != _ids.end())
            {
                out.push_back(it->second);
            }
            return out;
        }
        const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
        auto it = _blob.begin();
        while (true)
        {
            const auto [found, foundEnd] = searcher(it, _blob.end());
            if (found == _blob.end())
            {
                break;
            }
            const uint32_t pos = static_cast<uint32_t>(found - _blob.begin());
            // The term holding the match: the last start <= pos.
            const auto st = std::upper_bound(_starts.begin(), _starts.end(), pos);
            if (st == _starts.begin())
            {
                it = found + 1;
                continue;
            }
            const size_t idx = static_cast<size_t>((st - _starts.begin()) - 1);
            const uint32_t termStart = _starts[idx];
            const size_t termLen = _terms[idx].size();
            const bool atStart = pos == termStart;
            const bool atEnd = pos + needle.size() == termStart + termLen;
            if ((!prefix || atStart) && (!suffix || atEnd))
            {
                out.push_back(static_cast<uint32_t>(idx + 1));
                if (limit && out.size() >= limit)
                {
                    break;
                }
                // Continue after THIS term (one id per term).
                it = _blob.begin() + termStart + termLen;
            }
            else
            {
                it = found + 1;
            }
        }
        return out;
    }

    std::vector<uint32_t> Dictionary::TermsWithSubsequence(std::wstring_view needle, size_t limit) const
    {
        std::vector<uint32_t> out;
        if (needle.empty())
        {
            return out;
        }
        std::shared_lock lk{ _mtx };
        for (size_t i = 0; i < _terms.size(); ++i)
        {
            const std::wstring& t = _terms[i];
            if (t.size() < needle.size())
            {
                continue;
            }
            size_t k = 0;
            for (size_t j = 0; j < t.size() && k < needle.size(); ++j)
            {
                if (t[j] == needle[k])
                {
                    ++k;
                }
            }
            if (k == needle.size())
            {
                out.push_back(static_cast<uint32_t>(i + 1));
                if (limit && out.size() >= limit)
                {
                    break;
                }
            }
        }
        return out;
    }

    uint32_t Dictionary::Intern(const std::wstring& term)
    {
        {
            std::shared_lock lk{ _mtx };
            if (const auto it = _ids.find(term); it != _ids.end())
            {
                return it->second;
            }
        }
        std::unique_lock lk{ _mtx };
        if (const auto it = _ids.find(term); it != _ids.end())
        {
            return it->second;
        }
        const uint32_t id = static_cast<uint32_t>(_terms.size() + 1);
        _terms.push_back(term);
        _stats.push_back(DictTerm{ id, 0, 0 });
        _ids.emplace(term, id);
        _dirty.push_back(id); // a new term: the next Save writes it
        std::lock_guard bl{ _blobMtx };
        _blobDirty = true;
        return id;
    }

    void Dictionary::AddCounts(uint32_t id, uint32_t df, uint32_t sf)
    {
        std::unique_lock lk{ _mtx };
        if (id == 0 || id > _stats.size())
        {
            return;
        }
        _dirty.push_back(id); // Save sorts and dedupes the marks
        _stats[id - 1].df += df;
        _stats[id - 1].sf += sf;
    }

    void Dictionary::AddDocs(uint64_t docs, uint64_t tokens)
    {
        std::unique_lock lk{ _mtx };
        _docs += docs;
        _tokens += tokens;
    }

    void Dictionary::SetDocStats(uint64_t docs, uint64_t tokens)
    {
        std::unique_lock lk{ _mtx };
        _docs = docs;
        _tokens = tokens;
    }

    void Dictionary::Save(Connection& con)
    {
        // Only the terms that are new or whose counts changed since the last save (a merge touches a small
        // share of a ~1 M-term dictionary): staged, then one delete + insert. A full rewrite on every merge
        // would cost the cold build hundreds of million-row rewrites.
        std::vector<uint32_t> dirty;
        std::vector<std::pair<std::wstring, DictTerm>> rows;
        {
            std::unique_lock lk{ _mtx };
            dirty.swap(_dirty);
            std::sort(dirty.begin(), dirty.end());
            dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
            rows.reserve(dirty.size());
            for (const uint32_t id : dirty)
            {
                if (id != 0 && id <= _terms.size() && !_terms[id - 1].empty())
                {
                    rows.emplace_back(_terms[id - 1], _stats[id - 1]);
                }
            }
        }
        if (rows.empty())
        {
            return;
        }
        try
        {
            con.Execute("CREATE TABLE IF NOT EXISTS stg_dict (term_id UINTEGER, term VARCHAR, df UINTEGER, sf UINTEGER); DELETE FROM stg_dict;");
            {
                auto a = con.MakeAppender("stg_dict");
                for (const auto& [t, st] : rows)
                {
                    a.UInt32(st.id).Str(ToUtf8(t)).UInt32(st.df).UInt32(st.sf).EndRow();
                }
                a.Close();
            }
            con.Execute("DELETE FROM dict WHERE term_id IN (SELECT term_id FROM stg_dict); INSERT INTO dict SELECT * FROM stg_dict; DELETE FROM stg_dict;");
        }
        catch (...)
        {
            // The caller's transaction rolls back; the ids must be written by the next save.
            std::unique_lock lk{ _mtx };
            _dirty.insert(_dirty.end(), dirty.begin(), dirty.end());
            throw;
        }
    }

    // ===== merge and re-cluster =======================================================================

    namespace
    {
        // The line store's key: (file_id << 32) | line_no, the ingest's formula (SessionDbStore.cpp), so one
        // file's lines are one contiguous id range.
        int64_t LineIdOf(int32_t fileId, uint32_t lineNo) noexcept
        {
            return (static_cast<int64_t>(fileId) << 32) | lineNo;
        }

        // Plan the slices of a sorted copy of `src` (line or line_hot): inclusive line_id ranges, ascending, of
        // about `chunkBytes` of raw JSON each, covering every row with line_id >= `fromLineId`.
        //
        // Why slices: on 2026-10-15 the one-statement re-sort (`INSERT INTO line_new SELECT * FROM line ORDER BY
        // line_id`) failed with "Out of Memory Error: failed to pin block" under the service's 1 GB limit on a
        // 325 K-line store holding 1.6 GB of raw JSON, and the maintenance retried it every ~20 s; the same
        // statement over half the ids passed (34 s), the sort alone passed, and the unsorted copy passed. The
        // sort and the ZSTD writer together are what does not fit, so every statement now sorts one slice.
        //
        // The plan comes from a per-file byte count (one small row per file; `bytes` is the stored length of each
        // line, +64 for the row's other columns): consecutive whole files while they fit, and line ranges of ONE
        // file where that file alone is larger than the budget. The ranges tile the id space from the first file
        // on, so no row can fall between two of them.
        //   con        — the writer connection.
        //   src        — the table name (this file's constants, never user input).
        //   fromLineId — plan only the rows at or past this id (0 = all).
        //   chunkBytes — the budget per slice; values under 1 MB are raised to 1 MB.
        // Returns the slices (empty when no row qualifies). Throws DbError.
        struct LineSlice
        {
            int64_t lo{ 0 }; // first line_id (inclusive)
            int64_t hi{ 0 }; // last line_id (inclusive)
            int64_t bytes{ 0 }; // the planned raw bytes (+64 per row) it holds
        };
        std::vector<LineSlice> PlanLineSlices(Connection& con, std::string_view src, int64_t fromLineId, int64_t chunkBytes)
        {
            const int64_t budget = std::max<int64_t>(chunkBytes, 1ll << 20);
            const std::string from{ src };
            std::vector<std::pair<int32_t, int64_t>> files; // (file_id, bytes) in file order
            {
                auto p = con.Prepare("SELECT file_id, sum(coalesce(bytes, 0) + 64)::BIGINT FROM " + from + " WHERE line_id >= ? GROUP BY file_id ORDER BY file_id");
                p.Bind(1, fromLineId);
                auto r = p.Execute();
                while (r.Next())
                {
                    files.emplace_back(static_cast<int32_t>(r.Int64(0)), r.Int64(1));
                }
            }
            std::vector<LineSlice> ranges; // ascending
            int64_t acc = 0;
            bool open = false;
            int32_t firstFile = 0;
            int32_t lastFile = 0;
            const auto close = [&] {
                if (open)
                {
                    ranges.push_back(LineSlice{ LineIdOf(firstFile, 0), LineIdOf(lastFile, 0xFFFFFFFFu), acc });
                    open = false;
                    acc = 0;
                }
            };
            for (const auto& [file, bytes] : files)
            {
                if (bytes > budget)
                {
                    // One file larger than a slice: cut it into line ranges of about the budget each. The ranges
                    // TILE the file's whole id space ([0 .. cut1], [cut1 + 1 .. cut2], ..., [last + 1 .. max]),
                    // so no row can fall between two of them whatever its line number.
                    close();
                    auto p = con.Prepare("SELECT line_no, coalesce(bytes, 0) + 64 FROM " + from + " WHERE file_id = ? AND line_id >= ? ORDER BY line_no");
                    p.Bind(1, static_cast<int64_t>(file)).Bind(2, fromLineId);
                    auto r = p.Execute();
                    // The first line number the next range covers: the resume point when it lies inside this file.
                    uint32_t next = (fromLineId >> 32) == file ? static_cast<uint32_t>(fromLineId & 0xFFFFFFFFll) : 0u;
                    int64_t sum = 0;
                    while (r.Next())
                    {
                        const auto lineNo = static_cast<uint32_t>(r.Int64(0));
                        sum += r.Int64(1);
                        if (sum >= budget && lineNo != 0xFFFFFFFFu)
                        {
                            ranges.push_back(LineSlice{ LineIdOf(file, next), LineIdOf(file, lineNo), sum });
                            next = lineNo + 1;
                            sum = 0;
                        }
                    }
                    // The rest of the id space; empty when the last row closed a range (an INSERT of no rows).
                    ranges.push_back(LineSlice{ LineIdOf(file, next), LineIdOf(file, 0xFFFFFFFFu), sum });
                    continue;
                }
                if (open && acc + bytes > budget)
                {
                    close();
                }
                if (!open)
                {
                    firstFile = file;
                    open = true;
                }
                lastFile = file;
                acc += bytes;
            }
            close();
            // A resume point inside the first file: the rows below it were placed already.
            for (auto& s : ranges)
            {
                s.lo = std::max(s.lo, fromLineId);
            }
            return ranges;
        }

        // Copy every row of `src` (line_hot) into `dst` (line) in line_id order, one slice per INSERT ... ORDER BY
        // (PlanLineSlices). The slices ascend, so the rows land in `dst` as one sorted run (DuckDB keeps an
        // INSERT ... ORDER BY's order: preserve_insertion_order is on).
        //   con        — the writer connection, inside the caller's transaction.
        //   src, dst   — table names (this file's constants, never user input).
        //   chunkBytes — the budget per statement.
        // Throws DbError.
        void CopyLinesSorted(Connection& con, std::string_view src, std::string_view dst, int64_t chunkBytes)
        {
            const auto slices = PlanLineSlices(con, src, 0, chunkBytes);
            auto ins = con.Prepare("INSERT INTO " + std::string{ dst } + " SELECT * FROM " + std::string{ src } + " WHERE line_id BETWEEN ? AND ? ORDER BY line_id");
            for (const LineSlice& s : slices)
            {
                ins.Bind(1, s.lo).Bind(2, s.hi);
                (void)ins.Execute();
            }
        }
    }

    void TextIndex::Load(Connection& con)
    {
        _dict.Load(con);
    }

    int64_t TextIndex::LineRuns(Connection& con)
    {
        const std::string v = GetMeta(con, "line_runs");
        const long long n = v.empty() ? 1 : std::strtoll(v.c_str(), nullptr, 10);
        return n < 1 ? 1 : static_cast<int64_t>(n);
    }

    void TextIndex::_moveHotLines(Connection& con)
    {
        // line_id is never negative (file ids are positive), so -1 stands for "no rows".
        const int64_t hotMin = con.ScalarInt64("SELECT coalesce(min(line_id), -1) FROM line_hot");
        if (hotMin < 0)
        {
            return;
        }
        const int64_t lineMax = con.ScalarInt64("SELECT coalesce(max(line_id), -1) FROM line");
        if (lineMax >= 0 && hotMin < lineMax)
        {
            // The batch does not simply extend the sorted order: one more run for ReclusterLines to undo.
            SetMeta(con, "line_runs", std::to_string(LineRuns(con) + 1));
        }
        CopyLinesSorted(con, "line_hot", "line", sortChunkBytes);
        con.Execute("DELETE FROM line_hot");
    }

    size_t TextIndex::Merge(Connection& con, const std::function<bool()>& cancel)
    {
        // Step 1 (outside the transaction): tokenize every hot block. Tokens come from the ORIGINAL text
        // (the camelCase split needs the case); the postings and per-session hit counts accumulate here.
        struct Posting
        {
            uint32_t term;
            uint32_t doc;
            uint16_t tf;
        };
        std::vector<Posting> postings;
        // (term, session) -> blocks, keyed term << 32 | session (sessions are positive 32-bit keys).
        std::unordered_map<uint64_t, uint32_t> sessionHits;
        std::vector<std::pair<uint32_t, int32_t>> tokLens; // (doc, tokens)
        uint32_t maxDoc = 0;
        uint64_t tokenTotal = 0;
        // Per-term scratch indexed by term id (dense): the block a term was last counted in, its count in
        // that block, and how many blocks of this merge hold it (df). Arrays instead of hash maps: the
        // tokenizer emits ~100 occurrences per block, and a per-occurrence hash insert dominated the merge.
        std::vector<uint32_t> lastDoc, tfInDoc, dfDelta;
        std::vector<uint32_t> docTerms; // the distinct terms of the current block
        // term -> id for this merge: transparent hashing looks a token up by view, so a known term costs no
        // allocation (the dictionary's own map is behind a lock and keyed by owned strings).
        struct ViewHash
        {
            using is_transparent = void;
            size_t operator()(std::wstring_view v) const noexcept
            {
                return std::hash<std::wstring_view>{}(v);
            }
        };
        std::unordered_map<std::wstring, uint32_t, ViewHash, std::equal_to<>> local;
        local.reserve(1u << 16);
        const auto idOf = [&](std::wstring_view term) -> uint32_t {
            if (const auto it = local.find(term); it != local.end())
            {
                return it->second;
            }
            std::wstring owned{ term };
            const uint32_t id = _dict.Intern(owned);
            local.emplace(std::move(owned), id);
            if (id >= lastDoc.size())
            {
                const size_t n = std::max<size_t>(id + 1, lastDoc.size() * 2);
                lastDoc.resize(n, 0);
                tfInDoc.resize(n, 0);
                dfDelta.resize(n, 0);
            }
            return id;
        };
        const uint64_t t0 = ::GetTickCount64();
        {
            auto r = con.Query("SELECT doc_id, sid_key, text FROM doc_hot ORDER BY doc_id");
            while (r.Next())
            {
                if (cancel && cancel())
                {
                    return 0;
                }
                const uint32_t doc = static_cast<uint32_t>(r.Int64(0));
                const int32_t sid = static_cast<int32_t>(r.Int64(1));
                maxDoc = std::max(maxDoc, doc);
                const std::wstring text = FromUtf8(r.Str(2));
                docTerms.clear();
                int32_t tokens = 0;
                SessionDbTokenizer::ForEachToken(text, ServiceClassifier(), [&](std::wstring_view term) {
                    const uint32_t id = idOf(term);
                    ++tokens;
                    if (lastDoc[id] != doc)
                    {
                        lastDoc[id] = doc; // doc ids start at 1, so 0 is "never"
                        tfInDoc[id] = 1;
                        docTerms.push_back(id);
                    }
                    else
                    {
                        ++tfInDoc[id];
                    }
                });
                for (const uint32_t id : docTerms)
                {
                    postings.push_back(Posting{ id, doc, static_cast<uint16_t>(std::min<uint32_t>(tfInDoc[id], 65535)) });
                    ++sessionHits[(static_cast<uint64_t>(id) << 32) | static_cast<uint32_t>(sid)];
                    ++dfDelta[id];
                }
                tokLens.emplace_back(doc, tokens);
                tokenTotal += static_cast<uint64_t>(tokens);
            }
        }
        if (tokLens.empty())
        {
            // Nothing hot: still move any hot lines (a tick can append lines without blocks).
            Transaction tx{ con };
            _moveHotLines(con);
            tx.Commit();
            return 0;
        }
        const uint64_t t1 = ::GetTickCount64();
        std::sort(postings.begin(), postings.end(), [](const Posting& a, const Posting& b) {
            return a.term != b.term ? a.term < b.term : a.doc < b.doc;
        });
        // The session postings in (term, session) order: each term's sessions are one prunable run. The
        // per-term session counts this merge adds (sf grows by the sessions new to the term in THIS merge;
        // across merges it is an upper bound until the next re-cluster recomputes it exactly).
        std::vector<std::pair<uint64_t, uint32_t>> spost(sessionHits.begin(), sessionHits.end());
        std::sort(spost.begin(), spost.end());
        std::vector<uint32_t> sfDelta(dfDelta.size(), 0);
        for (const auto& [key, hits] : spost)
        {
            ++sfDelta[static_cast<size_t>(key >> 32)];
        }

        try
        {
            Transaction tx{ con };
            {
                auto a = con.MakeAppender("post");
                for (const auto& p : postings)
                {
                    a.UInt32(p.term).UInt32(p.doc).UInt16(p.tf).EndRow();
                }
                a.Close();
            }
            {
                auto a = con.MakeAppender("spost");
                for (const auto& [key, hits] : spost)
                {
                    a.UInt32(static_cast<uint32_t>(key >> 32)).Int32(static_cast<int32_t>(key & 0xFFFFFFFFu)).UInt32(hits).EndRow();
                }
                a.Close();
            }
            const uint64_t t2 = ::GetTickCount64();
            // The blocks move into the compressed table with their BM25 lengths.
            con.Execute("CREATE TABLE IF NOT EXISTS stg_toklen (doc_id UINTEGER, tok_len INTEGER); DELETE FROM stg_toklen;");
            {
                auto a = con.MakeAppender("stg_toklen");
                for (const auto& [doc, n] : tokLens)
                {
                    a.UInt32(doc).Int32(n).EndRow();
                }
                a.Close();
            }
            // The text splits by role (SessionDbSchema.cpp, `doc`): conversation text stays in doc.text, a
            // tool block's text goes to the ZSTD doc_tool table.
            auto mv = con.Prepare(R"SQL(
INSERT INTO doc SELECT h.doc_id, h.sid_key, h.file_id, h.line_no, h.block, h.ts_ms, h.role, h.flags, h.tool, h.tool_use_id,
                       h.text_len, coalesce(t.tok_len, 0), CASE WHEN h.role IN (4, 5) THEN NULL ELSE h.text END
FROM doc_hot h LEFT JOIN stg_toklen t ON t.doc_id = h.doc_id WHERE h.doc_id <= ? ORDER BY h.doc_id)SQL");
            mv.Bind(1, static_cast<int64_t>(maxDoc));
            mv.Execute();
            auto mvTool = con.Prepare("INSERT INTO doc_tool SELECT doc_id, text FROM doc_hot WHERE role IN (4, 5) AND doc_id <= ? ORDER BY doc_id");
            mvTool.Bind(1, static_cast<int64_t>(maxDoc));
            mvTool.Execute();
            auto del = con.Prepare("DELETE FROM doc_hot WHERE doc_id <= ?");
            del.Bind(1, static_cast<int64_t>(maxDoc));
            del.Execute();
            con.Execute("DELETE FROM stg_toklen;");
            // The hot lines move too, sorted (§6.8: one merged batch holds each session's new lines in one run),
            // in memory-bounded slices.
            _moveHotLines(con);
            const uint64_t t3 = ::GetTickCount64();
            for (size_t id = 1; id < dfDelta.size(); ++id)
            {
                if (dfDelta[id] != 0)
                {
                    _dict.AddCounts(static_cast<uint32_t>(id), dfDelta[id], sfDelta[id]);
                }
            }
            _dict.AddDocs(tokLens.size(), tokenTotal);
            _dict.Save(con);
            const uint64_t t4 = ::GetTickCount64();
            tx.Commit();
            if (log)
            {
                log(L"[sessdb] merge steps (ms): tokenize " + std::to_wstring(t1 - t0) + L" (" + std::to_wstring(postings.size()) + L" postings), sort + postings " +
                    std::to_wstring(t2 - t1) + L", move blocks + lines " + std::to_wstring(t3 - t2) + L", dictionary " + std::to_wstring(t4 - t3) +
                    L", commit " + std::to_wstring(::GetTickCount64() - t4));
            }
        }
        catch (...)
        {
            // Rolled back: the in-memory dictionary took terms and counts the database does not have.
            _dict.Load(con);
            throw;
        }
        return tokLens.size();
    }

    bool TextIndex::ReclusterPostings(Connection& con, const std::function<bool()>& cancel)
    {
        if (cancel && cancel())
        {
            return false;
        }
        Transaction tx{ con };
        // Postings sorted by (term, doc) without the tombstoned blocks; the tombstoned blocks deleted from the
        // block tables; session postings recomputed EXACTLY from the new postings (one row per term and
        // session). The sorts and the join here touch fixed-width columns only (term_id, doc_id, tf, sid_key),
        // which DuckDB spills to disk under the memory limit. `doc` and `doc_tool` are NOT copied: every merge
        // appends them in doc_id order, so deleting the tombstoned rows leaves them sorted, and DuckDB's
        // checkpoint reclaims the deleted rows. (Until 2026-10-15 this step also re-sorted `doc`, `doc_tool` and
        // `line` with their text in one statement each: correct, but a sort of gigabytes of text that the
        // service's memory limit could not hold — the line part now lives in ReclusterLines, in slices.)
        con.Execute(R"SQL(
CREATE TABLE post_new (term_id UINTEGER, doc_id UINTEGER, tf USMALLINT);
INSERT INTO post_new SELECT term_id, doc_id, tf FROM post WHERE doc_id NOT IN (SELECT doc_id FROM tomb) ORDER BY term_id, doc_id;
DELETE FROM doc WHERE doc_id IN (SELECT doc_id FROM tomb);
DELETE FROM doc_tool WHERE doc_id IN (SELECT doc_id FROM tomb);
CREATE TABLE spost_new (term_id UINTEGER, sid_key INTEGER, hits UINTEGER);
INSERT INTO spost_new SELECT p.term_id, d.sid_key, count(*) FROM post_new p JOIN doc d ON d.doc_id = p.doc_id
    GROUP BY p.term_id, d.sid_key ORDER BY p.term_id, d.sid_key;
DROP TABLE post; ALTER TABLE post_new RENAME TO post;
DROP TABLE spost; ALTER TABLE spost_new RENAME TO spost;
DELETE FROM tomb;
)SQL");
        // Exact df / sf from the rewritten postings.
        con.Execute(R"SQL(
CREATE TABLE dict_new (term_id UINTEGER, term VARCHAR, df UINTEGER, sf UINTEGER);
INSERT INTO dict_new
SELECT d.term_id, d.term, coalesce(p.df, 0), coalesce(s.sf, 0)
FROM dict d
LEFT JOIN (SELECT term_id, count(*) AS df FROM post GROUP BY term_id) p ON p.term_id = d.term_id
LEFT JOIN (SELECT term_id, count(*) AS sf FROM spost GROUP BY term_id) s ON s.term_id = d.term_id
ORDER BY d.term_id;
DROP TABLE dict; ALTER TABLE dict_new RENAME TO dict;
)SQL");
        tx.Commit();
        // The views name the tables, and DuckDB binds a view at creation: re-create them over the new tables.
        EnsureSchema(con);
        _dict.Load(con);
        return true;
    }

    bool TextIndex::LineResortInProgress(Connection& con)
    {
        return !GetMeta(con, "line_resort_from").empty();
    }

    bool TextIndex::ReclusterLines(Connection& con, const std::function<bool()>& cancel, LineResortTally* tally)
    {
        LineResortTally local;
        LineResortTally& t = tally ? *tally : local;
        // The resume point: present while a re-sort is under way (a slice moved, later slices remain). Every slice
        // below it is in its final place already, at the end of the table, in order.
        const std::string fromText = GetMeta(con, "line_resort_from");
        t.resumed = !fromText.empty();
        const int64_t from = t.resumed ? std::max<long long>(0, std::strtoll(fromText.c_str(), nullptr, 10)) : 0;
        // Once one slice moved to the end, every later slice must follow it (it would otherwise sit before it).
        bool moving = t.resumed;
        int64_t prevMaxRowid = -1; // the last in-place slice's last row (the sorted prefix grows from the start)
        const std::vector<LineSlice> slices = PlanLineSlices(con, "line", from, sortChunkBytes);
        t.slices = static_cast<int64_t>(slices.size());
        auto stat = con.Prepare("SELECT count(*), coalesce(min(rowid), -1), coalesce(max(rowid), -1) FROM line WHERE line_id BETWEEN ? AND ?");
        // The slice's rows again, sorted, at the end of the table; then its old rows go. DuckDB numbers the rows a
        // transaction adds from MAX_ROW_ID (2^55) up until it commits, far above any committed row id, so
        // `rowid <= old max` deletes exactly the old copies. line_by_id is not UNIQUE, so the two copies may
        // coexist until then. (The INSERT reads before it writes: its ORDER BY sorts every row first.)
        auto ins = con.Prepare("INSERT INTO line SELECT * FROM line WHERE line_id BETWEEN ? AND ? ORDER BY line_id");
        auto del = con.Prepare("DELETE FROM line WHERE line_id BETWEEN ? AND ? AND rowid <= ?");
        for (const LineSlice& s : slices)
        {
            if (cancel && cancel())
            {
                return false; // the slices moved so far stay; the next call resumes at line_resort_from
            }
            int64_t n = 0;
            int64_t minRowid = -1;
            int64_t maxRowid = -1;
            {
                stat.Bind(1, s.lo).Bind(2, s.hi);
                auto r = stat.Execute();
                if (r.Next())
                {
                    n = r.Int64(0);
                    minRowid = r.Int64(1);
                    maxRowid = r.Int64(2);
                }
            }
            if (n == 0)
            {
                continue;
            }
            // In place: the slice's rows are contiguous (no other row, and no deleted one, between them) and come
            // after every earlier in-place slice. A cold build that ingested files in id order needs no move.
            if (!moving && maxRowid - minRowid + 1 == n && minRowid > prevMaxRowid)
            {
                prevMaxRowid = maxRowid;
                ++t.inPlace;
                continue;
            }
            Transaction tx{ con };
            if (!moving)
            {
                // The first move of this re-sort: remember the run count, so the runs that merges add DURING the
                // re-sort survive its end (a merge into an already placed range is a new run).
                SetMeta(con, "line_resort_runs", std::to_string(LineRuns(con)));
            }
            ins.Bind(1, s.lo).Bind(2, s.hi);
            (void)ins.Execute();
            del.Bind(1, s.lo).Bind(2, s.hi).Bind(3, maxRowid);
            (void)del.Execute();
            SetMeta(con, "line_resort_from", std::to_string(s.hi + 1));
            tx.Commit();
            moving = true;
            ++t.moved;
            t.movedBytes += s.bytes;
        }
        // Finished: one sorted run, plus the merges that landed in an already placed range meanwhile (counted
        // as runs by _moveHotLines since the first move; an over-count when a merge's lines landed in a range
        // still ahead, which the later move absorbed — the next re-sort comes a little early, never late).
        const std::string baseText = GetMeta(con, "line_resort_runs");
        const int64_t base = baseText.empty() ? 0 : std::strtoll(baseText.c_str(), nullptr, 10);
        const int64_t during = (moving && base > 0) ? std::max<int64_t>(0, LineRuns(con) - base) : 0;
        Transaction tx{ con };
        SetMeta(con, "line_runs", std::to_string(1 + during));
        con.Execute("DELETE FROM meta WHERE key IN ('line_resort_from', 'line_resort_runs')");
        tx.Commit();
        return true;
    }

    void TextIndex::Recluster(Connection& con, const std::function<bool()>& cancel)
    {
        if (ReclusterPostings(con, cancel))
        {
            (void)ReclusterLines(con, cancel, nullptr);
        }
    }
}
