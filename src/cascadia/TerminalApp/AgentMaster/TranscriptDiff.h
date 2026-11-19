// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - the transcript overlay's DIFF tab, its pure half (doc/agentmaster/TRANSCRIPT_OVERLAY.md 3l): the
// conversation turn by turn with each turn's NET change to every file it touched, drawn as GitHub-style diff cards.
//
// What lives here (no WinRT, no PCH, no JSON: TranscriptTurns.cpp reads the transcript and the overlay draws; the m5
// harness tests every function below on its own):
//   * unified-diff text: parsing it (ParseUnifiedDiff - the "--- / +++" paths, the "@@ -a,b +c,d @@ section" hunks, the
//     rows) and writing it (UnifiedDiffText), each hunk's rows numbered for display (NumberHunkRows);
//   * TurnFileComposer - the edits ONE turn made to ONE file, composed into the turn's NET change (the user's pick "Net
//     diff per file", like a commit): Claude Code records each Edit / MultiEdit / Write-over-a-file as hunks numbered
//     against the file as it was right before THAT call, with 3 lines of context; the composer replays them on a SPARSE
//     model of the file - only the lines some hunk showed are known, every other line is a run of known length but unknown
//     text - and reads the net change off the model: the original lines deleted, the lines added and still there, with up
//     to 3 known lines of context. No file content is needed (Claude Code keeps the whole file only under 10 KB); when it
//     IS known (that small-file copy), the model starts from it and every context line is there;
//   * DedupeInOrder - the turn's Bash reasons, one row per DIFFERENT reason with its count (the user's "x5 in one row");
//   * LocateHunkNewSide - where a hunk's lines sit in the file AS IT IS NOW, for the cards' GitHub-style Expand (the user's
//     pick "The file as it is now": the lines around a change are read from the current file);
//   * the overlay's card arithmetic, kept here (not in the XAML unit) so the harness tests it: how many lines an expand row
//     can still show (DiffLinesAboveHunk, DiffRemainingAbove, DiffRemainingBelow) and how many one click shows
//     (DiffExpandTake), the numbers expanded lines continue (DiffNewFirst, DiffOldFirst, DiffSideCount), a line's numbers
//     cell (DiffNumberDigits, DiffNumbersText), what a row says (DiffExpandLabel), a line as a text block may draw it
//     (DiffDisplayLine), the fence info's two parts (SplitDiffInfo), a
//     Bash row's count (TakeDiffRepeatCount), the five diffstat squares (DiffBarSquares), an outline entry's title
//     (DiffOutlineTitle).
//
// Every literal is ASCII with \x escapes (the repository's convention for wide literals). Never throws except
// std::bad_alloc, which the callers' nets contain.

#pragma once

#include <algorithm>
#include <climits>
#include <cmath> // std::lround - DiffBarSquares
#include <cstdint>
#include <cstdlib> // std::abs - LocateHunkNewSide's nearest occurrence
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Agentmaster
{
    // One hunk of a unified diff: its header's four numbers and section text, and its rows.
    struct DiffHunk
    {
        int oldStart = 0; // the old side's first line (1-based); with oldLines == 0, the line the insertion follows (0 = the top)
        int oldLines = 0; // how many old-side rows (' ' and '-')
        int newStart = 0; // the new side's first line (1-based); with newLines == 0, the line the deletion follows
        int newLines = 0; // how many new-side rows (' ' and '+')
        std::wstring section; // the text after the header's closing "@@" (a function name, say), without its leading blank; often empty
        std::vector<std::wstring> rows; // each starts with ' ' (context), '-' (removed), '+' (added) or '\\' ("\ No newline at end of file")
    };

    // A whole unified diff of one file, as ParseUnifiedDiff reads it.
    struct ParsedDiff
    {
        std::wstring oldPath; // the "--- " line's path ("" when the diff has none; "/dev/null" for a file created)
        std::wstring newPath; // the "+++ " line's path ("" when none; "/dev/null" for a file deleted)
        std::vector<DiffHunk> hunks; // in text order
        // Change rows before any "@@" header: the change has no line numbers (an older Claude Code recorded only the call's
        // own old_string / new_string). They are collected as ONE hunk with all four numbers 0.
        bool unnumbered = false;
        int added = 0; // '+' rows in every hunk
        int removed = 0; // '-' rows in every hunk
    };

    // One row of a hunk as the overlay draws it: its kind and its line numbers on each side (0 = none on that side).
    struct DiffRow
    {
        wchar_t kind = L' '; // ' ' context, '-' removed, '+' added, '\\' a "No newline at end of file" marker
        int oldNo = 0;
        int newNo = 0;
        std::wstring text; // the row without its leading kind character
    };

    namespace detail
    {
        // Reads a decimal number at text[pos...], advancing pos past it. Returns -1 when there is no digit there (or more
        // than 9, which no real line number has). Never throws.
        inline int ReadDiffNumber(std::wstring_view text, size_t& pos) noexcept
        {
            int value = 0;
            size_t digits = 0;
            while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9' && digits < 9)
            {
                value = value * 10 + (text[pos] - L'0');
                ++pos;
                ++digits;
            }
            return digits == 0 ? -1 : value;
        }

        // Parses "@@ -a[,b] +c[,d] @@[ section]" (an omitted count is 1, unified diff's own rule) into `hunk`'s numbers and
        // section. Returns false for a line that is no hunk header. Throws std::bad_alloc only (the section copy).
        inline bool ParseHunkHeader(std::wstring_view line, DiffHunk& hunk)
        {
            if (line.size() < 4 || line.substr(0, 4) != L"@@ -")
            {
                return false;
            }
            size_t pos = 4;
            const int oldStart = ReadDiffNumber(line, pos);
            int oldLines = 1;
            if (oldStart < 0)
            {
                return false;
            }
            if (pos < line.size() && line[pos] == L',')
            {
                ++pos;
                oldLines = ReadDiffNumber(line, pos);
            }
            if (oldLines < 0 || pos + 2 > line.size() || line.substr(pos, 2) != L" +")
            {
                return false;
            }
            pos += 2;
            const int newStart = ReadDiffNumber(line, pos);
            int newLines = 1;
            if (newStart < 0)
            {
                return false;
            }
            if (pos < line.size() && line[pos] == L',')
            {
                ++pos;
                newLines = ReadDiffNumber(line, pos);
            }
            if (newLines < 0 || pos + 3 > line.size() || line.substr(pos, 3) != L" @@")
            {
                return false;
            }
            pos += 3;
            hunk.oldStart = oldStart;
            hunk.oldLines = oldLines;
            hunk.newStart = newStart;
            hunk.newLines = newLines;
            hunk.section.clear();
            if (pos < line.size())
            {
                size_t first = pos;
                while (first < line.size() && line[first] == L' ')
                {
                    ++first;
                }
                hunk.section.assign(line.substr(first));
            }
            return true;
        }

        // Two file lines are the same line: equal once a trailing CR is ignored (a CRLF file read whole keeps its CRs; the
        // transcript's hunk rows never carry them). Never throws.
        inline bool SameDiffLine(std::wstring_view a, std::wstring_view b) noexcept
        {
            if (!a.empty() && a.back() == L'\r')
            {
                a.remove_suffix(1);
            }
            if (!b.empty() && b.back() == L'\r')
            {
                b.remove_suffix(1);
            }
            return a == b;
        }
    }

    // Reads a unified diff of one file: the optional "--- " / "+++ " paths, then every "@@" hunk with its rows. A row that
    // starts with none of ' ', '-', '+', '\\' (a stray line) is read as context; an empty line inside a hunk is an empty
    // context row (some writers drop the blank). Change rows before any header form one unnumbered hunk (`unnumbered`).
    // Parameters: text - the diff (LF or CRLF line ends).
    // Returns the parsed diff; empty hunks for a text with no row. Throws std::bad_alloc only.
    inline ParsedDiff ParseUnifiedDiff(std::wstring_view text)
    {
        ParsedDiff diff;
        DiffHunk* current = nullptr;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t end = text.find(L'\n', start);
            std::wstring_view line = text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
            start = end == std::wstring_view::npos ? text.size() + 1 : end + 1;
            if (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (end == std::wstring_view::npos && line.empty())
            {
                break; // the text's final line break: no row after it
            }
            if (current == nullptr && line.size() >= 4 && line.substr(0, 4) == L"--- ")
            {
                diff.oldPath.assign(line.substr(4));
                continue;
            }
            if (current == nullptr && line.size() >= 4 && line.substr(0, 4) == L"+++ ")
            {
                diff.newPath.assign(line.substr(4));
                continue;
            }
            DiffHunk header;
            if (detail::ParseHunkHeader(line, header))
            {
                diff.hunks.push_back(std::move(header));
                current = &diff.hunks.back();
                continue;
            }
            if (current == nullptr)
            {
                if (line.empty())
                {
                    continue; // a blank before any hunk
                }
                // Rows with no header: the unnumbered form (old_string / new_string).
                diff.hunks.push_back(DiffHunk{});
                current = &diff.hunks.back();
                diff.unnumbered = true;
            }
            std::wstring row{ line };
            if (row.empty())
            {
                row = L" ";
            }
            else if (row[0] != L' ' && row[0] != L'-' && row[0] != L'+' && row[0] != L'\\')
            {
                row.insert(0, 1, L' ');
            }
            if (row[0] == L'+')
            {
                ++diff.added;
            }
            else if (row[0] == L'-')
            {
                ++diff.removed;
            }
            current->rows.push_back(std::move(row));
        }
        return diff;
    }

    // A hunk's header line: "@@ -a,b +c,d @@" plus " section" when it has one.
    // Parameters: hunk - the hunk.
    // Returns the header text (no line break). Throws std::bad_alloc only.
    inline std::wstring FormatHunkHeader(const DiffHunk& hunk)
    {
        std::wstring header = L"@@ -" + std::to_wstring(hunk.oldStart) + L"," + std::to_wstring(hunk.oldLines) + L" +" + std::to_wstring(hunk.newStart) + L"," +
                              std::to_wstring(hunk.newLines) + L" @@";
        if (!hunk.section.empty())
        {
            header += L' ';
            header += hunk.section;
        }
        return header;
    }

    // Writes one file's unified diff: "--- old" and "+++ new" (each only when given), then every hunk's header and rows.
    // Parameters: oldPath / newPath - the two paths ("" leaves the line out; "/dev/null" marks a side that did not exist);
    //             hunks - the hunks, in order.
    // Returns the text, every line ended by "\n". Throws std::bad_alloc only.
    inline std::wstring UnifiedDiffText(std::wstring_view oldPath, std::wstring_view newPath, const std::vector<DiffHunk>& hunks)
    {
        std::wstring out;
        if (!oldPath.empty())
        {
            out += L"--- ";
            out += oldPath;
            out += L'\n';
        }
        if (!newPath.empty())
        {
            out += L"+++ ";
            out += newPath;
            out += L'\n';
        }
        for (const auto& hunk : hunks)
        {
            out += FormatHunkHeader(hunk);
            out += L'\n';
            for (const auto& row : hunk.rows)
            {
                out += row;
                out += L'\n';
            }
        }
        return out;
    }

    // A hunk's rows with their line numbers, as the overlay draws them: context rows carry both numbers, a removed row the
    // old one, an added row the new one, a "\ No newline" marker none. An unnumbered hunk (all four header numbers 0)
    // numbers nothing.
    // Parameters: hunk - the hunk.
    // Returns one DiffRow per row, in order. Throws std::bad_alloc only.
    inline std::vector<DiffRow> NumberHunkRows(const DiffHunk& hunk)
    {
        std::vector<DiffRow> rows;
        rows.reserve(hunk.rows.size());
        const bool numbered = hunk.oldStart != 0 || hunk.newStart != 0 || hunk.oldLines != 0 || hunk.newLines != 0;
        // An empty side's start is the line its change FOLLOWS (unified diff's rule): its first row would be one past it.
        int oldNo = hunk.oldLines == 0 ? hunk.oldStart + 1 : hunk.oldStart;
        int newNo = hunk.newLines == 0 ? hunk.newStart + 1 : hunk.newStart;
        for (const auto& source : hunk.rows)
        {
            DiffRow row;
            row.kind = source.empty() ? L' ' : source[0];
            row.text = source.empty() ? std::wstring{} : source.substr(1);
            if (numbered)
            {
                if (row.kind == L' ')
                {
                    row.oldNo = oldNo++;
                    row.newNo = newNo++;
                }
                else if (row.kind == L'-')
                {
                    row.oldNo = oldNo++;
                }
                else if (row.kind == L'+')
                {
                    row.newNo = newNo++;
                }
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // The turn's NET change to ONE file, composed from the hunks of every edit the turn made to it, in order - see the file
    // comment. Usage: construct (with the whole original when it is known), Apply each edit's hunks in the order the edits
    // were made, then read NetHunks. Once an edit does not fit what the model knows (its context or removed lines differ
    // from lines an earlier hunk showed - the file was changed between the two edits by something else, a shell command or
    // an editor - or the edit carries no line numbers), the composition is VOID: Apply answers false from then on and the
    // caller shows that file's edits one by one instead.
    class TurnFileComposer
    {
    public:
        // Parameters:
        //   originalFile - the whole file as it was before the turn's first edit to it (Claude Code's originalFile, kept for
        //                  files under 10 KB), or nullptr when unknown: the model then starts as one unknown run of open
        //                  length and learns the lines the hunks show.
        //   createdInTurn - the file did not exist before the turn (its first change created it): the model starts EMPTY, so
        //                  every line ends up added. Wins over originalFile.
        // Throws std::bad_alloc only.
        TurnFileComposer(const std::wstring* originalFile, bool createdInTurn)
        {
            if (createdInTurn)
            {
                return; // known and empty
            }
            if (originalFile != nullptr)
            {
                int number = 1;
                size_t start = 0;
                const std::wstring_view whole{ *originalFile };
                while (start < whole.size())
                {
                    const size_t end = whole.find(L'\n', start);
                    std::wstring_view line = whole.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
                    if (!line.empty() && line.back() == L'\r')
                    {
                        line.remove_suffix(1);
                    }
                    _segs.push_back(Seg{ Kind::Orig, number++, 1, std::wstring{ line } });
                    if (end == std::wstring_view::npos)
                    {
                        break;
                    }
                    start = end + 1;
                }
                return;
            }
            _segs.push_back(Seg{ Kind::Unknown, 1, kOpen, {} });
        }

        // Applies ONE edit: its hunks, numbered against the file as it was right before that edit (Claude Code's own
        // numbering), in their order. Each later hunk of the same edit is placed by the line-count change of the earlier
        // ones (its old start + that running difference).
        // Parameters: hunks - the edit's hunks (ParseUnifiedDiff of its diff text).
        // Returns false - and the composition is void from now on - when the edit does not fit (see the class comment).
        // Throws std::bad_alloc only.
        bool Apply(const std::vector<DiffHunk>& hunks)
        {
            if (_void)
            {
                return false;
            }
            int shift = 0;
            for (const auto& hunk : hunks)
            {
                if (hunk.oldStart == 0 && hunk.newStart == 0 && hunk.oldLines == 0 && hunk.newLines == 0 && !hunk.rows.empty())
                {
                    _void = true; // an unnumbered change: nowhere to put it
                    return false;
                }
                int oldCount = 0;
                int newCount = 0;
                for (const auto& row : hunk.rows)
                {
                    if (row.empty() || row[0] == L' ')
                    {
                        ++oldCount;
                        ++newCount;
                    }
                    else if (row[0] == L'-')
                    {
                        ++oldCount;
                    }
                    else if (row[0] == L'+')
                    {
                        ++newCount;
                    }
                }
                // Where the old side starts in the model's CURRENT version (0-based): the header's start (1-based) moved by
                // the earlier hunks of this edit. An EMPTY old side (an insertion with no context: an empty file) is numbered
                // two ways: classic unified diff names the line it follows ("@@ -5,0 +6,2 @@"), while Claude Code's own
                // structuredPatch (the jsdiff library) names the line it goes BEFORE ("@@ -6,0 +6,2 @@") - the new start
                // tells them apart: equal to the old start (net of this edit's earlier hunks) means the jsdiff form.
                int base = hunk.oldStart - 1;
                if (oldCount == 0)
                {
                    const bool jsdiffForm = hunk.oldStart > 0 && hunk.newStart - shift == hunk.oldStart;
                    base = jsdiffForm ? hunk.oldStart - 1 : hunk.oldStart;
                }
                const int at = base + shift;
                if (at < 0 || !ApplyHunk(at, hunk.rows, oldCount))
                {
                    _void = true;
                    return false;
                }
                shift += newCount - oldCount;
            }
            return true;
        }

        // Whether an Apply failed (the caller then shows the file's edits one by one).
        bool IsVoid() const noexcept
        {
            return _void;
        }

        // The net change: every original line deleted, every added line still there, grouped into hunks with up to
        // `context` known unchanged lines around each change (fewer where the model never saw the neighbors). Hunks whose
        // context windows touch are merged, as GitHub draws them. Numbers: the old side counts the file before the turn, the
        // new side the file after it.
        // Parameters: context - the context lines on each side of a change (3, as Claude Code and git use).
        // Returns the hunks, in file order; empty when the turn's edits cancelled out (or the composition is void).
        // Throws std::bad_alloc only.
        std::vector<DiffHunk> NetHunks(int context = 3) const
        {
            std::vector<DiffHunk> hunks;
            if (_void)
            {
                return hunks;
            }
            // Flatten the model into rows: '?' for an unknown run (a break in the context), ' ' / '-' / '+' for known lines.
            struct Flat
            {
                wchar_t kind;
                int oldNo; // the line's number before the turn (' ' and '-'), else the old line it follows
                int newNo; // after the turn (' ' and '+'), else the new line it follows
                const std::wstring* text;
            };
            std::vector<Flat> flat;
            flat.reserve(_segs.size());
            int newNo = 0; // the last new-side line number reached
            int oldNo = 0; // the last old-side line number reached
            for (const auto& seg : _segs)
            {
                switch (seg.kind)
                {
                case Kind::Unknown:
                    flat.push_back(Flat{ L'?', oldNo, newNo, nullptr });
                    if (seg.count != kOpen)
                    {
                        oldNo = seg.origNo + seg.count - 1;
                        newNo += seg.count;
                    }
                    break;
                case Kind::Orig:
                    oldNo = seg.origNo;
                    ++newNo;
                    flat.push_back(Flat{ L' ', oldNo, newNo, &seg.text });
                    break;
                case Kind::Del:
                    oldNo = seg.origNo;
                    flat.push_back(Flat{ L'-', oldNo, newNo, &seg.text });
                    break;
                case Kind::New:
                default:
                    ++newNo;
                    flat.push_back(Flat{ L'+', oldNo, newNo, &seg.text });
                    break;
                }
            }
            // Mark each change and up to `context` known unchanged rows on each side of it (never across an unknown run).
            std::vector<uint8_t> marked(flat.size(), 0);
            for (size_t i = 0; i < flat.size(); ++i)
            {
                if (flat[i].kind != L'-' && flat[i].kind != L'+')
                {
                    continue;
                }
                marked[i] = 1;
                int left = context;
                for (size_t j = i; j-- > 0 && left > 0;)
                {
                    if (flat[j].kind == L'?')
                    {
                        break;
                    }
                    marked[j] = 1;
                    if (flat[j].kind == L' ')
                    {
                        --left;
                    }
                }
                int right = context;
                for (size_t j = i + 1; j < flat.size() && right > 0; ++j)
                {
                    if (flat[j].kind == L'?')
                    {
                        break;
                    }
                    marked[j] = 1;
                    if (flat[j].kind == L' ')
                    {
                        --right;
                    }
                }
            }
            // Each maximal run of marked rows is a hunk. Its numbers: the first row on each side; an empty side's start is
            // the line its change follows (unified diff's rule).
            for (size_t i = 0; i < flat.size();)
            {
                if (!marked[i])
                {
                    ++i;
                    continue;
                }
                size_t j = i;
                DiffHunk hunk;
                int firstOld = -1;
                int firstNew = -1;
                for (; j < flat.size() && marked[j]; ++j)
                {
                    const auto& row = flat[j];
                    hunk.rows.push_back(std::wstring(1, row.kind) + *row.text);
                    if (row.kind != L'+')
                    {
                        ++hunk.oldLines;
                        if (firstOld < 0)
                        {
                            firstOld = row.oldNo;
                        }
                    }
                    if (row.kind != L'-')
                    {
                        ++hunk.newLines;
                        if (firstNew < 0)
                        {
                            firstNew = row.newNo;
                        }
                    }
                }
                // An empty side: the line before the run on that side (the run's first row's "follows" number).
                hunk.oldStart = firstOld >= 0 ? firstOld : flat[i].oldNo;
                hunk.newStart = firstNew >= 0 ? firstNew : flat[i].newNo;
                hunks.push_back(std::move(hunk));
                i = j;
            }
            return hunks;
        }

    private:
        enum class Kind : uint8_t
        {
            Unknown, // a run of `count` original lines nobody showed, starting at original line `origNo`
            Orig, // one original line still there: its number before the turn and its text
            New, // one line the turn added (still there)
            Del, // one original line the turn removed: zero lines wide in the current version
        };

        struct Seg
        {
            Kind kind;
            int origNo; // Unknown: its first original line; Orig / Del: the line's number before the turn; New: unused
            int count; // Unknown: its length (kOpen = to the end of the file, length unknown); others: 1
            std::wstring text;
        };

        // The length of the unknown run that reaches the end of the file: open-ended.
        static constexpr int kOpen = INT_MAX / 4;

        // The width a segment has in the current version: an unknown run its length, a kept or added line 1, a deleted line 0.
        static int Width(const Seg& seg) noexcept
        {
            switch (seg.kind)
            {
            case Kind::Unknown:
                return seg.count;
            case Kind::Del:
                return 0;
            case Kind::Orig:
            case Kind::New:
            default:
                return 1;
            }
        }

        // Makes the lines [at, at + count) of the current version single known segments - an unknown run they fall in is
        // split, its lines turned into Orig lines with the text the hunk's old side gives them - and returns the vector index
        // where current line `at` starts (after any deleted lines just before it), or -1 when the version has no line `at`
        // (or fewer than `count` from there). `texts` are the old-side texts, in order.
        int Materialize(int at, int count, const std::vector<std::wstring_view>& texts)
        {
            // Find the segment holding current line `at` (or, for count 0, the insertion point).
            int line = 0; // current-version index of _segs[i]'s first line
            size_t i = 0;
            for (; i < _segs.size(); ++i)
            {
                const int width = Width(_segs[i]);
                if (width == 0)
                {
                    continue;
                }
                if (line + width > at)
                {
                    break;
                }
                line += width;
            }
            if (count == 0)
            {
                // The insertion point: right before current line `at` - inside an unknown run, split it there.
                if (i < _segs.size() && _segs[i].kind == Kind::Unknown && at > line)
                {
                    SplitUnknown(i, at - line);
                    ++i;
                }
                if (i == _segs.size() && line < at)
                {
                    return -1; // past the end of a file of known length
                }
                // After any deleted lines sitting right at this point: deletions read before additions.
                while (i < _segs.size() && _segs[i].kind == Kind::Del)
                {
                    ++i;
                }
                return static_cast<int>(i);
            }
            // Split an unknown run so that line `at` starts a segment.
            if (i < _segs.size() && _segs[i].kind == Kind::Unknown && at > line)
            {
                SplitUnknown(i, at - line);
                ++i;
                line = at;
            }
            if (i >= _segs.size())
            {
                return -1;
            }
            const size_t first = i;
            // Walk `count` current lines from here, revealing unknown ones.
            int done = 0;
            for (size_t k = first; done < count; ++k)
            {
                if (k >= _segs.size())
                {
                    return -1;
                }
                Seg& seg = _segs[k];
                if (seg.kind == Kind::Del)
                {
                    continue;
                }
                if (seg.kind == Kind::Unknown)
                {
                    // Reveal its first line: the hunk says what it is.
                    if (seg.count != 1)
                    {
                        SplitUnknown(k, 1);
                    }
                    Seg& revealed = _segs[k];
                    revealed.kind = Kind::Orig;
                    revealed.count = 1;
                    revealed.text.assign(texts[static_cast<size_t>(done)]);
                }
                else if (!detail::SameDiffLine(seg.text, texts[static_cast<size_t>(done)]))
                {
                    return -1; // the hunk says this line reads differently: the file changed between two edits
                }
                ++done;
            }
            return static_cast<int>(first);
        }

        // Splits the unknown run at `index` into its first `head` lines and the rest (which keeps an open length open).
        void SplitUnknown(size_t index, int head)
        {
            Seg rest = _segs[index];
            rest.origNo += head;
            if (rest.count != kOpen)
            {
                rest.count -= head;
            }
            _segs[index].count = head;
            _segs.insert(_segs.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(rest));
        }

        // Applies one hunk whose old side starts at current line `at` (0-based; for an empty old side, the insertion point).
        bool ApplyHunk(int at, const std::vector<std::wstring>& rows, int oldCount)
        {
            std::vector<std::wstring_view> oldTexts;
            oldTexts.reserve(static_cast<size_t>(oldCount));
            for (const auto& row : rows)
            {
                if (row.empty())
                {
                    oldTexts.emplace_back();
                }
                else if (row[0] == L' ' || row[0] == L'-')
                {
                    oldTexts.push_back(std::wstring_view{ row }.substr(1));
                }
            }
            const int start = Materialize(at, oldCount, oldTexts);
            if (start < 0)
            {
                return false;
            }
            size_t index = static_cast<size_t>(start);
            for (const auto& row : rows)
            {
                const wchar_t kind = row.empty() ? L' ' : row[0];
                if (kind == L'\\')
                {
                    continue; // "\ No newline at end of file": not a line
                }
                if (kind == L'+')
                {
                    _segs.insert(_segs.begin() + static_cast<std::ptrdiff_t>(index), Seg{ Kind::New, 0, 1, row.substr(1) });
                    ++index;
                    continue;
                }
                // ' ' or '-': the next current line (Materialize made it known and checked it), past deleted lines.
                while (index < _segs.size() && _segs[index].kind == Kind::Del)
                {
                    ++index;
                }
                if (index >= _segs.size())
                {
                    return false;
                }
                if (kind == L'-')
                {
                    if (_segs[index].kind == Kind::New)
                    {
                        _segs.erase(_segs.begin() + static_cast<std::ptrdiff_t>(index)); // added earlier in the turn, gone again
                        continue;
                    }
                    _segs[index].kind = Kind::Del;
                }
                ++index;
            }
            return true;
        }

        std::vector<Seg> _segs;
        bool _void = false;
    };

    // The items in first-seen order, each once, with how many times it occurred (the Diff tab's Bash rows: "Run the tests"
    // five times is one row, x5).
    // Parameters: items - the reasons, in the order the calls were made.
    // Returns (item, count) pairs. Throws std::bad_alloc only.
    inline std::vector<std::pair<std::wstring, int>> DedupeInOrder(const std::vector<std::wstring>& items)
    {
        std::vector<std::pair<std::wstring, int>> out;
        for (const auto& item : items)
        {
            const auto it = std::find_if(out.begin(), out.end(), [&item](const std::pair<std::wstring, int>& seen) { return seen.first == item; });
            if (it != out.end())
            {
                ++it->second;
            }
            else
            {
                out.emplace_back(item, 1);
            }
        }
        return out;
    }

    // Where a hunk's NEW side (its ' ' and '+' rows, in order) sits in the file as it is now - what the Diff tab's Expand
    // reads the surrounding lines from. Every occurrence is a candidate; the one nearest the hunk's own new start wins (two
    // identical blocks of code in one file stay tied to the right one when the file has not shifted much).
    // Parameters: fileLines - the current file, one entry per line (a trailing CR is ignored); hunk - the hunk.
    // Returns the 0-based index of the new side's first line in fileLines, or -1 when the new side is not in the file (it
    // changed since that turn, or the hunk has no new side). Cost: one pass over the file per candidate start, each
    // stopping at its first different line - linear in practice. Throws std::bad_alloc only (the new side's views: the
    // caller's net logs it - the overlay's expand handler).
    inline int LocateHunkNewSide(const std::vector<std::wstring>& fileLines, const DiffHunk& hunk)
    {
        std::vector<std::wstring_view> side;
        side.reserve(hunk.rows.size());
        for (const auto& row : hunk.rows)
        {
            if (row.empty())
            {
                side.emplace_back();
            }
            else if (row[0] == L' ' || row[0] == L'+')
            {
                side.push_back(std::wstring_view{ row }.substr(1));
            }
        }
        if (side.empty() || side.size() > fileLines.size())
        {
            return -1;
        }
        const int want = (std::max)(hunk.newStart - 1, 0);
        int best = -1;
        for (size_t at = 0; at + side.size() <= fileLines.size(); ++at)
        {
            bool same = true;
            for (size_t k = 0; k < side.size(); ++k)
            {
                if (!detail::SameDiffLine(fileLines[at + k], side[k]))
                {
                    same = false;
                    break;
                }
            }
            if (same && (best < 0 || std::abs(static_cast<int>(at) - want) < std::abs(best - want)))
            {
                best = static_cast<int>(at);
            }
        }
        return best;
    }

    // ---- the overlay's card arithmetic (AgentTranscriptOverlay's diff cards), kept here so the harness tests it ----

    // How many lines one Expand click reveals (GitHub's step), and the gap between two hunks short enough to open whole.
    inline constexpr int kDiffExpandStep = 20;
    inline constexpr int kDiffGapExpandAll = 40;
    // The longest line a card's text block is handed: a minified file's single line would cost a text layout of megabytes.
    inline constexpr size_t kDiffMaxLineChars = 2000;

    // A hunk's first new-side / old-side line number: an empty side's start is the line its change FOLLOWS (unified diff's
    // rule), so its first line would be one past it - NumberHunkRows' own rule, so expanded lines continue its numbers.
    // Never throws.
    inline int DiffNewFirst(const DiffHunk& hunk) noexcept
    {
        return hunk.newLines == 0 ? hunk.newStart + 1 : hunk.newStart;
    }
    inline int DiffOldFirst(const DiffHunk& hunk) noexcept
    {
        return hunk.oldLines == 0 ? hunk.oldStart + 1 : hunk.oldStart;
    }

    // How many of a hunk's rows lie on one side: the context rows plus `changed` ('+' counts the new side, '-' the old).
    // Counted from the rows, not the header, so a header that disagrees with its rows cannot put expanded lines out of step
    // with the lines drawn. Never throws.
    inline int DiffSideCount(const DiffHunk& hunk, wchar_t changed) noexcept
    {
        int count = 0;
        for (const auto& row : hunk.rows)
        {
            if (row.empty() || row[0] == L' ' || row[0] == changed)
            {
                ++count;
            }
        }
        return count;
    }

    // How many lines lie above hunk `h` that a card does not show, by the hunks' OWN numbers (the file right after the
    // change): from the file's start for the first hunk, else from the end of the hunk before it. Never below 0. The file as
    // it is now may hold more or fewer (DiffRemainingAbove takes the smaller). Never throws.
    inline int DiffLinesAboveHunk(const std::vector<DiffHunk>& hunks, size_t h) noexcept
    {
        if (h >= hunks.size())
        {
            return 0;
        }
        const int first = DiffNewFirst(hunks[h]);
        if (h == 0)
        {
            return (std::max)(first - 1, 0);
        }
        const int previousEnd = DiffNewFirst(hunks[h - 1]) + DiffSideCount(hunks[h - 1], L'+');
        return (std::max)(first - previousEnd, 0);
    }

    // How many lines above hunk `h` a card can still reveal from the file as it is now: the smaller of what the hunks' own
    // numbers say lay there (DiffLinesAboveHunk) and what the file NOW holds there - from its start for the first hunk, else
    // from the end of the hunk before it when that one was found too - minus what was revealed already. The smaller, so a
    // later turn that inserted lines there never numbers a line below 1 or past the gap.
    // Parameters: hunks / located / revealedAbove - the card's (located[h]: LocateHunkNewSide, -1 = not in the file;
    //             it must be >= 0 for an answer above 0); h - the hunk.
    // Returns the count, never below 0. Never throws.
    inline int DiffRemainingAbove(const std::vector<DiffHunk>& hunks, const std::vector<int>& located, const std::vector<int>& revealedAbove, size_t h) noexcept
    {
        if (h >= hunks.size() || h >= located.size() || h >= revealedAbove.size() || located[h] < 0)
        {
            return 0;
        }
        int now = located[h]; // lines above it in the file now, from the file's start...
        if (h > 0 && located[h - 1] >= 0)
        {
            now = located[h] - (located[h - 1] + DiffSideCount(hunks[h - 1], L'+')); // ...or from the end of the hunk before it
        }
        const int then = DiffLinesAboveHunk(hunks, h);
        return (std::max)((std::min)(now, then) - revealedAbove[h], 0);
    }

    // How many lines below the LAST hunk a card can still reveal: the file now, past the hunk's new side, minus what was
    // revealed already.
    // Parameters: fileLineCount - the file now; located - the hunk's place (LocateHunkNewSide; below 0: none); hunk - the
    //             last hunk; revealedBelow - shown already.
    // Returns the count, never below 0. Never throws.
    inline int DiffRemainingBelow(size_t fileLineCount, int located, const DiffHunk& hunk, int revealedBelow) noexcept
    {
        if (located < 0)
        {
            return 0;
        }
        const long long left = static_cast<long long>(fileLineCount) - (static_cast<long long>(located) + DiffSideCount(hunk, L'+')) - revealedBelow;
        return static_cast<int>(std::clamp<long long>(left, 0, INT_MAX));
    }

    // How many of `remaining` lines one click reveals: all of them when they are at most one step, or - between two hunks
    // (not above the first one) - at most kDiffGapExpandAll (GitHub opens a short gap whole); otherwise one step.
    // Parameters: remaining - the row's DiffRemainingAbove / DiffRemainingBelow; betweenHunks - a gap row.
    // Returns the count. Never throws.
    inline int DiffExpandTake(int remaining, bool betweenHunks) noexcept
    {
        if (remaining <= kDiffExpandStep || (betweenHunks && remaining <= kDiffGapExpandAll))
        {
            return (std::max)(remaining, 0);
        }
        return kDiffExpandStep;
    }

    namespace detail
    {
        // "N line" / "N lines". Throws std::bad_alloc only.
        inline std::wstring DiffLineCount(int count)
        {
            return std::to_wstring(count) + (count == 1 ? L" line" : L" lines");
        }
    }

    // What an expand row says it shows. `remaining` is how many lines the row can still reveal; -1 = not known yet (below the
    // last hunk, before the file is read).
    // Parameters: below - the row below the last hunk; first - the row above the first hunk (from the file's start);
    //             remaining - see above.
    // Returns the label, ending "(from the file now)". Throws std::bad_alloc only.
    inline std::wstring DiffExpandLabel(bool below, bool first, int remaining)
    {
        std::wstring text;
        const std::wstring step = std::to_wstring(kDiffExpandStep);
        if (below)
        {
            text = remaining < 0                ? std::wstring{ L"Show more lines below" } :
                   remaining == 1               ? std::wstring{ L"Show the last line" } :
                   remaining <= kDiffExpandStep ? L"Show the last " + detail::DiffLineCount(remaining) :
                                                  L"Show " + step + L" more lines below";
        }
        else if (first)
        {
            text = remaining == 1               ? std::wstring{ L"Show the first line" } :
                   remaining <= kDiffExpandStep ? L"Show the first " + detail::DiffLineCount(remaining) :
                                                  L"Show " + step + L" more lines above";
        }
        else
        {
            text = remaining == 1                 ? std::wstring{ L"Show the line hidden here" } :
                   remaining <= kDiffGapExpandAll ? L"Show the " + detail::DiffLineCount(remaining) + L" hidden here" :
                                                    L"Show " + step + L" more lines (" + std::to_wstring(remaining) + L" hidden)";
        }
        return text + L" (from the file now)";
    }

    // The digits every line number of a card is padded to: the digits of `largest` (the biggest number the card can show,
    // its expansions included), at least three - so a card's numbers column never changes width as lines are revealed.
    // Never throws.
    inline int DiffNumberDigits(int largest) noexcept
    {
        int digits = 1;
        for (int value = (std::max)(largest, 0); value >= 10; value /= 10)
        {
            ++digits;
        }
        return (std::max)(digits, 3);
    }

    // A line's numbers cell as a card draws it, in a monospace font: the old number and the new number, each right-aligned in
    // `digits` characters (blank where the line has none on that side), two blanks between them, then a blank and its mark.
    // With 3 digits: " 39   39  " for context, " 40      -" removed, "      40 +" added, all blanks for a "\ No newline"
    // marker. One text per line, so the two numbers and the mark always line up with each other and with the first visual
    // line of the (wrapping) code beside them.
    // Parameters: row - the line (NumberHunkRows' numbering: 0 = none on that side); digits - DiffNumberDigits of the card.
    // Returns the cell's text, 2 * digits + 4 characters (more only for a number wider than `digits`). Throws std::bad_alloc only.
    inline std::wstring DiffNumbersText(const DiffRow& row, int digits)
    {
        const size_t width = static_cast<size_t>((std::max)(digits, 1));
        const auto side = [width](int number) {
            std::wstring cell = number > 0 ? std::to_wstring(number) : std::wstring{};
            if (cell.size() < width)
            {
                cell.insert(0, width - cell.size(), L' ');
            }
            return cell;
        };
        const wchar_t mark = (row.kind == L'+' || row.kind == L'-') ? row.kind : L' ';
        return side(row.oldNo) + L"  " + side(row.newNo) + L' ' + mark;
    }

    // One line of diff code as a text block is handed it: at most kDiffMaxLineChars characters (an ellipsis marks a cut; a
    // cut never splits a surrogate pair), its trailing CRs dropped, and every character a text block would break the line
    // at shown as its control picture instead - a break inside a line would read as two lines of code beside one number.
    // Parameters: text - the line.
    // Returns the line to draw. Throws std::bad_alloc only.
    inline std::wstring DiffDisplayLine(std::wstring_view text)
    {
        while (!text.empty() && text.back() == L'\r')
        {
            text.remove_suffix(1);
        }
        const bool cut = text.size() > kDiffMaxLineChars;
        std::wstring line{ text.substr(0, cut ? kDiffMaxLineChars : text.size()) };
        if (cut && !line.empty() && line.back() >= 0xD800 && line.back() <= 0xDBFF)
        {
            line.pop_back(); // half of a pair: dropped, never drawn as a box
        }
        for (auto& ch : line)
        {
            switch (ch)
            {
            case L'\r':
                ch = L'\x240D'; // SYMBOL FOR CARRIAGE RETURN
                break;
            case L'\n':
            case L'\x0085':
            case L'\x2028':
            case L'\x2029':
                ch = L'\x2424'; // SYMBOL FOR NEWLINE
                break;
            case L'\v':
                ch = L'\x240B';
                break;
            case L'\f':
                ch = L'\x240C';
                break;
            default:
                break;
            }
        }
        if (cut)
        {
            line += L'\x2026';
        }
        return line;
    }

    // A diff fence's info after "diff" (MarkdownBlocks' MdBlock::infoRest, written by TranscriptTurns' DiffFence):
    // "<shown path> · <what the block is>" - split at the FIRST " · " (a path holding " · " is not worth guessing about: its
    // tail then reads as the description).
    // Parameters: infoRest - the info; path / meta - receive the two parts (meta "" when there is none).
    // Throws std::bad_alloc only.
    inline void SplitDiffInfo(std::wstring_view infoRest, std::wstring& path, std::wstring& meta)
    {
        const size_t dot = infoRest.find(L" \x00B7 ");
        if (dot == std::wstring_view::npos)
        {
            path.assign(infoRest);
            meta.clear();
            return;
        }
        path.assign(infoRest.substr(0, dot));
        meta.assign(infoRest.substr(dot + 3));
    }

    // A Bash row's count: the " \x00D7N" (" xN") TranscriptTurns' DiffTurn appends to a repeated reason, taken off the end of
    // `text`. Text that only looks like it (no digits, more than six of them) is left alone.
    // Parameters: text - the row's last run; the count is erased from it.
    // Returns N (1 when the row ends with none). Never throws.
    inline int TakeDiffRepeatCount(std::wstring& text) noexcept
    {
        const size_t times = text.rfind(L" \x00D7");
        if (times == std::wstring::npos || times + 2 >= text.size() || text.size() - (times + 2) > 6)
        {
            return 1;
        }
        int count = 0;
        for (size_t i = times + 2; i < text.size(); ++i)
        {
            if (text[i] < L'0' || text[i] > L'9')
            {
                return 1; // not a count: the reason's own words
            }
            count = count * 10 + (text[i] - L'0');
        }
        text.erase(times);
        return (std::max)(count, 1);
    }

    // GitHub's five diffstat squares for a change of `added` / `removed` lines: one square per line while there are at most
    // five, else the five shared in proportion - each side that has lines keeps at least one. The rest are neutral.
    // Returns {green, red}. Never throws.
    inline std::pair<int, int> DiffBarSquares(int added, int removed) noexcept
    {
        added = (std::max)(added, 0);
        removed = (std::max)(removed, 0);
        const long long total = static_cast<long long>(added) + removed;
        if (total <= 5)
        {
            return { added, removed };
        }
        int green = static_cast<int>(std::lround(5.0 * static_cast<double>(added) / static_cast<double>(total)));
        green = std::clamp(green, added > 0 ? 1 : 0, removed > 0 ? 4 : 5);
        return { green, 5 - green };
    }

    // An outline entry's title: the first line of the message that started a turn, its runs of blanks collapsed, at most
    // 120 characters and an ellipsis (the entry trims to its width anyway; the cap only bounds what is kept).
    // Parameters: text - the message (or its first block).
    // Returns the title ("" for a blank message). Throws std::bad_alloc only.
    inline std::wstring DiffOutlineTitle(std::wstring_view text)
    {
        const size_t lineEnd = text.find_first_of(L"\r\n");
        const std::wstring_view first = text.substr(0, lineEnd);
        std::wstring title;
        bool blank = false;
        for (const wchar_t ch : first)
        {
            if (std::iswspace(ch))
            {
                blank = !title.empty();
                continue;
            }
            if (blank)
            {
                title += L' ';
                blank = false;
            }
            title += ch;
            if (title.size() >= 120)
            {
                title += L'\x2026';
                break;
            }
        }
        return title;
    }
}
