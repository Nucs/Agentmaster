// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MarkdownBlocks: cmark's document tree walked once, top to bottom, and flattened into
// MdBlock records (see MarkdownBlocks.h for what a block carries and why the list is flat).
//
// The walk is cmark's own iterator (ENTER / EXIT events; leaf nodes get a single ENTER), so it is
// non-recursive and its stack depth is independent of how deeply a document nests. Container state
// lives on two explicit stacks: `_stack` (the quotes and list items around the current position —
// copied into every block) and `_lists` (per open list: its numbering, tightness and marker width).

#ifndef NOMINMAX
#define NOMINMAX // <windows.h> below must not define min/max macros over std::min / std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "MarkdownBlocks.h"

#include "ClaudeSpawn.h" // LogSwallowedException — Rule #18: a swallowed exception still reaches hooks.log

#include <windows.h> // WideCharToMultiByte / MultiByteToWideChar

// cmark's header defines short legacy aliases (NODE_TEXT, BULLET_LIST, ...) unless told not to; they
// are plain macros and would rewrite any identifier of the same name in this TU.
#define CMARK_NO_SHORT_NAMES
#include <cmark.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwctype> // iswspace / iswalnum
#include <memory>
#include <stdexcept> // std::length_error (ToUtf8's size refusal)

namespace Agentmaster
{
    namespace
    {
        // The vertical gaps MdGapAbove chooses between, in DIPs. Small on purpose: a transcript is
        // long, and the overlay is 80% of the window at most — every pixel of air costs content.
        constexpr double kGapParagraph = 8.0; // between ordinary paragraphs / quotes (CommonMark's paragraph spacing)
        constexpr double kGapTight = 3.0; // between the items of a tight list (transcript_turns' tool-call lines)
        constexpr double kGapTightCode = 4.0; // a code block inside / next to a tight list item (a hair more than text)
        constexpr double kGapAroundCode = 6.0; // a code block or rule next to ordinary text
        constexpr double kGapAfterLabel = 4.0; // the block right under a "**❯ User**" / "**● Claude**" label line
        constexpr double kGapHeadingMajor = 22.0; // above an h1 / h2 — a section break
        constexpr double kGapHeadingMinor = 16.0; // above an h3..h6 (transcript_turns' "### Turn N" headers)
        constexpr double kGapAfterHeading = 6.0; // the block right under a heading belongs to it

        // The longest label line MdIsLabelParagraph accepts: speaker lines are a few words, while a
        // long all-bold paragraph is emphasis, not a label, and must keep paragraph spacing after it.
        constexpr size_t kLabelMaxChars = 200;

        // The largest document ToUtf8 converts. WideCharToMultiByte takes int lengths; a transcript
        // anywhere near this (hundreds of MB of Markdown) is far past anything the overlay renders —
        // it caps its input long before — so refusing is the honest answer, not a silent wrap.
        constexpr size_t kMaxConvertibleChars = static_cast<size_t>(INT_MAX) / 4;

        // Frees a cmark node tree on scope exit — every exit, including an exception mid-walk.
        struct CmarkNodeDeleter
        {
            void operator()(cmark_node* node) const noexcept
            {
                cmark_node_free(node);
            }
        };

        // Frees a cmark iterator on scope exit (it must die before the tree it walks — declared
        // after the tree's owner, so destruction order guarantees that).
        struct CmarkIterDeleter
        {
            void operator()(cmark_iter* iter) const noexcept
            {
                cmark_iter_free(iter);
            }
        };

        // UTF-16 -> UTF-8 for cmark. An unpaired surrogate becomes U+FFFD (WideCharToMultiByte's
        // default replacement), which is what a renderer would show for it anyway.
        // Parameters: text — the whole document.
        // Returns the UTF-8 bytes; empty for empty input.
        // Throws std::length_error for a document past kMaxConvertibleChars (the int-length API
        // limit) and std::bad_alloc when the buffer can't be allocated — both reach the caller's
        // catch, which logs them and reports the parse as failed.
        std::string ToUtf8(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }
            if (text.size() > kMaxConvertibleChars)
            {
                throw std::length_error("MarkdownBlocks: document too large to convert");
            }
            const int wideLen = static_cast<int>(text.size());
            const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLen, nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
            {
                return {};
            }
            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLen, out.data(), needed, nullptr, nullptr);
            return out;
        }

        // UTF-8 (a cmark literal / URL / info string) -> UTF-16. cmark validated the input
        // (CMARK_OPT_VALIDATE_UTF8), so its strings are well-formed.
        // Parameters: text — a NUL-terminated cmark string, or nullptr (cmark answers nullptr for a
        //             field the node type doesn't have).
        // Returns the converted text; empty for nullptr / "".
        // Throws std::bad_alloc only.
        std::wstring FromUtf8(const char* text)
        {
            if (!text || !*text)
            {
                return {};
            }
            const size_t len = std::strlen(text);
            if (len > static_cast<size_t>(INT_MAX))
            {
                return {}; // a single cmark literal this size cannot come from a document ToUtf8 accepted
            }
            const int narrowLen = static_cast<int>(len);
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text, narrowLen, nullptr, 0);
            if (needed <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text, narrowLen, out.data(), needed);
            return out;
        }

        // The bullet glyph for an unordered list nested `depth` lists deep (0 == outermost):
        // • then ◦ then ▪ for everything deeper — the same progression WT's Markdown pane uses, so a
        // nested list reads the same in both.
        // Returns the glyph (no trailing space — the renderer's marker column provides the gap).
        std::wstring BulletFor(size_t depth)
        {
            switch (depth)
            {
            case 0:
                return L"\x2022"; // •
            case 1:
                return L"\x25E6"; // ◦
            default:
                return L"\x25AA"; // ▪
            }
        }

        // How many decimal digits `value` prints with (0 prints as "0" — one digit). Negative
        // values count their digits only; cmark never reports a negative list start.
        size_t DecimalDigits(long long value) noexcept
        {
            if (value < 0)
            {
                value = -value;
            }
            size_t digits = 1;
            while (value >= 10)
            {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        // The first word of a fence's info string ("python title=x" -> "python"), capped at 32
        // characters — the language label, nothing more. Empty when the fence named nothing.
        std::wstring FirstWord(const std::wstring& info)
        {
            size_t start = 0;
            while (start < info.size() && iswspace(info[start]))
            {
                ++start;
            }
            size_t end = start;
            while (end < info.size() && !iswspace(info[end]) && end - start < 32)
            {
                ++end;
            }
            return info.substr(start, end - start);
        }

        // Cut `text` to at most `maxChars` UTF-16 units without splitting a surrogate pair (a lone
        // high surrogate at the end would draw as a replacement glyph).
        // Returns true when anything was cut.
        bool CutAt(std::wstring& text, size_t maxChars)
        {
            if (maxChars == 0 || text.size() <= maxChars)
            {
                return false;
            }
            size_t keep = maxChars;
            if (keep > 0 && IS_HIGH_SURROGATE(text[keep - 1]))
            {
                --keep;
            }
            text.resize(keep);
            return true;
        }

        // ---- GitHub-style pipe tables (MdParseOptions::tables) -----------------------------------------
        // cmark is CommonMark only, so a table reaches the walker as a PARAGRAPH whose lines are its rows. The
        // walker reads that paragraph's RAW source lines (cmark keeps each block's line range) and applies GFM's
        // table rules to them here - the merged TEXT nodes could not tell an escaped "\|" from a cell border.
        // Everything below works on UTF-8 bytes: every byte the rules look at ('|', '\\', ':', '-', ' ', '\t',
        // '>') is ASCII, and in UTF-8 no byte of a multi-byte character is ever an ASCII byte.

        // `text` without leading and trailing spaces / tabs.
        std::string_view TrimAsciiSpace(std::string_view text) noexcept
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        // Whether the '|' that ends `row` is escaped: an odd number of backslashes right before it.
        // Parameters: row - a row whose last byte is '|'.
        bool EndsWithEscapedPipe(std::string_view row) noexcept
        {
            size_t backslashes = 0;
            for (size_t i = row.size() - 1; i-- > 0 && row[i] == '\\';)
            {
                ++backslashes;
            }
            return backslashes % 2 == 1;
        }

        // Whether `line` holds a cell border: a '|' that no backslash escapes.
        bool HasUnescapedPipe(std::string_view line) noexcept
        {
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '\\')
                {
                    ++i; // the escaped byte is literal, whatever it is
                }
                else if (line[i] == '|')
                {
                    return true;
                }
            }
            return false;
        }

        // A table row split into its cells' raw Markdown, each trimmed (GFM): one leading and one trailing
        // border '|' are optional and dropped; every other unescaped '|' separates two cells. "\|" becomes a
        // literal '|' in its cell - GFM's rule, which holds inside a code span too (cmark alone would keep the
        // backslash there); any other backslash escape is left for cmark to read.
        // Parameters: line - one row, container prefixes already stripped.
        // Returns at least one cell (an empty row gives one empty cell). Throws std::bad_alloc only.
        std::vector<std::string> SplitTableRow(std::string_view line)
        {
            std::string_view row = TrimAsciiSpace(line);
            if (!row.empty() && row.front() == '|')
            {
                row.remove_prefix(1);
            }
            if (!row.empty() && row.back() == '|' && !EndsWithEscapedPipe(row))
            {
                row.remove_suffix(1);
            }
            std::vector<std::string> cells;
            std::string cell;
            for (size_t i = 0; i < row.size(); ++i)
            {
                const char c = row[i];
                if (c == '\\' && i + 1 < row.size())
                {
                    if (row[i + 1] == '|')
                    {
                        cell += '|';
                    }
                    else
                    {
                        cell += c;
                        cell += row[i + 1];
                    }
                    ++i;
                    continue;
                }
                if (c == '|')
                {
                    cells.emplace_back(TrimAsciiSpace(cell));
                    cell.clear();
                    continue;
                }
                cell += c;
            }
            cells.emplace_back(TrimAsciiSpace(cell));
            return cells;
        }

        // Read `line` as a table's DELIMITER row ("|---|:-:|--:|"): every cell is dashes, optionally with a colon
        // at either end, and at least one dash.
        // Parameters:
        //   line  - the candidate row, container prefixes already stripped.
        //   align - receives one alignment per cell ("--" Default, ":-" Left, ":-:" Center, "-:" Right) on success.
        // Returns true when it is a delimiter row. Throws std::bad_alloc only.
        bool ParseDelimiterRow(std::string_view line, std::vector<MdAlign>& align)
        {
            align.clear();
            for (const std::string& cell : SplitTableRow(line))
            {
                std::string_view body{ cell };
                if (body.empty())
                {
                    return false;
                }
                const bool left = body.front() == ':';
                const bool right = body.back() == ':';
                if (left)
                {
                    body.remove_prefix(1);
                }
                if (right && !body.empty())
                {
                    body.remove_suffix(1);
                }
                if (body.empty() || body.find_first_not_of('-') != std::string_view::npos)
                {
                    return false;
                }
                align.push_back(left && right ? MdAlign::Center : right ? MdAlign::Right : left ? MdAlign::Left : MdAlign::Default);
            }
            return !align.empty();
        }

        // A paragraph's continuation line without the markers of the containers around it, so a quoted table's
        // "> | a | b |" reads "| a | b |". For each container, outermost first: a quote takes up to three spaces,
        // its '>' and one optional space or tab; a list item takes the line's leading whitespace. A line that
        // lacks a quote's '>' is a LAZY continuation (CommonMark lets a paragraph run on without it): nothing more
        // is stripped from it. Over-stripping whitespace is harmless - a table row is trimmed anyway.
        // Parameters: line - the raw source line; stack - the containers around the paragraph, outermost first.
        // Returns the rest of the line. Never throws.
        std::string_view StripContainerPrefixes(std::string_view line, const std::vector<MdContainer>& stack) noexcept
        {
            size_t pos = 0;
            for (const auto& container : stack)
            {
                if (container.kind == MdContainerKind::Quote)
                {
                    size_t p = pos;
                    for (int spaces = 0; spaces < 3 && p < line.size() && line[p] == ' '; ++spaces)
                    {
                        ++p;
                    }
                    if (p >= line.size() || line[p] != '>')
                    {
                        break; // a lazy continuation line: no further markers on it
                    }
                    ++p;
                    if (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
                    {
                        ++p;
                    }
                    pos = p;
                }
                else
                {
                    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
                    {
                        ++pos;
                    }
                }
            }
            return line.substr(pos);
        }

        // The walker: one instance per parse, fed every iterator event in order. Not thread-safe
        // (one parse == one thread), never shared.
        class BlockWalker
        {
        public:
            // Parameters:
            //   options — the caps, the soft-break rule and the table switch (borrowed: must outlive the walker).
            //   out     — where blocks are appended (borrowed: the caller's result).
            //   source  — the UTF-8 document cmark parsed (borrowed: must outlive the walker). Read only to find
            //             tables (MdParseOptions::tables): a paragraph's raw lines. Empty = no tables are found.
            BlockWalker(const MdParseOptions& options, MdParseResult& out, std::string_view source = {}) noexcept :
                _options{ options },
                _out{ out },
                _source{ source }
            {
            }

            // Handle one iterator event.
            // Parameters:
            //   node — the node the event is for (never null — cmark's iterator never yields one).
            //   ev   — CMARK_EVENT_ENTER or CMARK_EVENT_EXIT (leaf nodes only ever ENTER).
            // Returns false when the walk must STOP: MdParseOptions::maxBlocks was reached and the
            // block cap flag is set on the result. True to keep walking.
            // Throws std::bad_alloc (string / vector growth) — the caller logs it and fails the parse.
            bool Visit(cmark_node* node, cmark_event_type ev)
            {
                const bool entering = ev == CMARK_EVENT_ENTER;
                const auto type = cmark_node_get_type(node);
                switch (type)
                {
                case CMARK_NODE_BLOCK_QUOTE:
                    if (entering)
                    {
                        MdContainer quote;
                        quote.kind = MdContainerKind::Quote;
                        quote.id = _nextId++;
                        _stack.push_back(std::move(quote));
                    }
                    else if (!_stack.empty())
                    {
                        _stack.pop_back();
                    }
                    break;

                case CMARK_NODE_LIST:
                    if (entering)
                    {
                        _lists.push_back(_MakeListFrame(node));
                    }
                    else if (!_lists.empty())
                    {
                        _lists.pop_back();
                    }
                    break;

                case CMARK_NODE_ITEM:
                    if (entering)
                    {
                        _stack.push_back(_MakeItem());
                    }
                    else
                    {
                        // An item whose marker is STILL pending produced no block at all (a bare "-"
                        // line). Give it an empty paragraph, or its bullet / number would vanish and the
                        // list would read as if the item never existed.
                        if (!_stack.empty() && _stack.back().kind == MdContainerKind::Item && !_stack.back().marker.empty())
                        {
                            if (!_Begin(MdBlockKind::Paragraph))
                            {
                                return false;
                            }
                        }
                        if (!_stack.empty())
                        {
                            _stack.pop_back();
                        }
                    }
                    break;

                case CMARK_NODE_PARAGRAPH:
                case CMARK_NODE_HEADING:
                    if (entering)
                    {
                        const bool heading = type == CMARK_NODE_HEADING;
                        if (!heading && _options.tables)
                        {
                            // A paragraph that IS a pipe table becomes a Table block (plus a Paragraph for any lines
                            // above its header row). Its inline children still arrive as events, but _inInline stays
                            // false, so _Append drops them; emphasis / link nesting counts stay balanced on their own.
                            const TableOutcome table = _TryTable(node);
                            if (table == TableOutcome::Stop)
                            {
                                return false;
                            }
                            if (table == TableOutcome::Emitted)
                            {
                                break;
                            }
                        }
                        auto* block = _Begin(heading ? MdBlockKind::Heading : MdBlockKind::Paragraph);
                        if (!block)
                        {
                            return false;
                        }
                        if (heading)
                        {
                            block->headingLevel = static_cast<uint8_t>(std::clamp(cmark_node_get_heading_level(node), 1, 6));
                        }
                        _inInline = true; // the TEXT / CODE / ... children that follow land in this block
                    }
                    else
                    {
                        _EndInlineBlock();
                    }
                    break;

                case CMARK_NODE_CODE_BLOCK:
                {
                    auto* block = _Begin(MdBlockKind::Code);
                    if (!block)
                    {
                        return false;
                    }
                    block->code = FromUtf8(cmark_node_get_literal(node));
                    // cmark keeps the code block's final newline; drawn, it would be an empty last line.
                    if (!block->code.empty() && block->code.back() == L'\n')
                    {
                        block->code.pop_back();
                    }
                    block->truncated = CutAt(block->code, _options.maxBlockChars);
                    block->info = FirstWord(FromUtf8(cmark_node_get_fence_info(node)));
                    break;
                }

                case CMARK_NODE_HTML_BLOCK:
                {
                    // Kept as LITERAL text (see MarkdownBlocks.h): a raw HTML block in a transcript is
                    // something a message contained, and it must read exactly as written.
                    auto* block = _Begin(MdBlockKind::Paragraph);
                    if (!block)
                    {
                        return false;
                    }
                    block->rawHtml = true;
                    std::wstring text = FromUtf8(cmark_node_get_literal(node));
                    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
                    {
                        text.pop_back();
                    }
                    block->truncated = CutAt(text, _options.maxBlockChars);
                    if (!text.empty())
                    {
                        MdRun run;
                        run.text = std::move(text);
                        run.style = kMdCode;
                        block->runs.push_back(std::move(run));
                    }
                    break;
                }

                case CMARK_NODE_THEMATIC_BREAK:
                    if (!_Begin(MdBlockKind::Rule))
                    {
                        return false;
                    }
                    break;

                case CMARK_NODE_TEXT:
                    _Append(FromUtf8(cmark_node_get_literal(node)), 0);
                    break;

                case CMARK_NODE_SOFTBREAK:
                    _Append(_options.softBreaksAsLineBreaks ? std::wstring_view{ L"\n" } : std::wstring_view{ L" " }, 0);
                    break;

                case CMARK_NODE_LINEBREAK:
                    _Append(L"\n", 0);
                    break;

                case CMARK_NODE_CODE:
                    _Append(FromUtf8(cmark_node_get_literal(node)), kMdCode);
                    break;

                case CMARK_NODE_HTML_INLINE:
                    // Literal and PLAIN: "use List<int> here" must read as the sentence the user wrote,
                    // not as monospace markup in the middle of prose.
                    _Append(FromUtf8(cmark_node_get_literal(node)), 0);
                    break;

                case CMARK_NODE_EMPH:
                    if (entering)
                    {
                        ++_emph;
                    }
                    else if (_emph > 0)
                    {
                        --_emph;
                    }
                    break;

                case CMARK_NODE_STRONG:
                    if (entering)
                    {
                        ++_strong;
                    }
                    else if (_strong > 0)
                    {
                        --_strong;
                    }
                    break;

                case CMARK_NODE_LINK:
                    if (entering)
                    {
                        _links.push_back(FromUtf8(cmark_node_get_url(node)));
                    }
                    else if (!_links.empty())
                    {
                        _links.pop_back();
                    }
                    break;

                case CMARK_NODE_IMAGE:
                    // Never fetched (MarkdownBlocks.h): "[image: <alt>]", the whole thing linked to the
                    // image's URL so it can still be opened deliberately. The alt text arrives as this
                    // node's TEXT children between the two events.
                    if (entering)
                    {
                        _links.push_back(FromUtf8(cmark_node_get_url(node)));
                        _Append(L"[image: ", 0);
                    }
                    else
                    {
                        _Append(L"]", 0);
                        if (!_links.empty())
                        {
                            _links.pop_back();
                        }
                    }
                    break;

                default:
                    // DOCUMENT is the root; CUSTOM_BLOCK / CUSTOM_INLINE are never produced by the parser.
                    break;
                }
                return true;
            }

        private:
            // What _TryTable made of a paragraph.
            enum class TableOutcome : uint8_t
            {
                NotTable, // no table in it: the caller builds the Paragraph as usual
                Emitted, // the table (and the paragraph above its header, if any) was appended
                Stop, // MdParseOptions::maxBlocks was reached while appending: the walk must stop
            };

            // Index the source's line starts once (cmark numbers lines from 1, and ends a line at "\n", "\r\n" or a
            // lone "\r" - the same three counted here, so a line number means the same line on both sides).
            // Throws std::bad_alloc only.
            void _EnsureLineStarts()
            {
                if (!_lineStarts.empty())
                {
                    return;
                }
                _lineStarts.push_back(0);
                for (size_t i = 0; i < _source.size(); ++i)
                {
                    if (_source[i] == '\r')
                    {
                        if (i + 1 < _source.size() && _source[i + 1] == '\n')
                        {
                            ++i; // "\r\n" is one line end
                        }
                        _lineStarts.push_back(i + 1);
                    }
                    else if (_source[i] == '\n')
                    {
                        _lineStarts.push_back(i + 1);
                    }
                }
            }

            // Source line `line` (1-based, cmark's numbering), without its line end. Empty past the end.
            // Requires _EnsureLineStarts. Never throws.
            std::string_view _SourceLine(int line) const noexcept
            {
                if (line <= 0 || static_cast<size_t>(line) > _lineStarts.size())
                {
                    return {};
                }
                const size_t begin = _lineStarts[static_cast<size_t>(line) - 1];
                size_t end = static_cast<size_t>(line) < _lineStarts.size() ? _lineStarts[static_cast<size_t>(line)] : _source.size();
                while (end > begin && (_source[end - 1] == '\n' || _source[end - 1] == '\r'))
                {
                    --end;
                }
                return _source.substr(begin, end - begin);
            }

            // A cell's (or a few lines') inline Markdown as styled runs: parsed by cmark as a little document of its
            // own and walked by a fresh walker, so bold, italic, code and links come out exactly as a paragraph's
            // would. A text that does not read as ONE plain paragraph (a cell "- x" is a list item, "# x" a heading,
            // "<div>" an HTML block) is kept as written in one plain run instead - GFM shows such cells literally.
            // Parameters: markdown - the raw UTF-8 Markdown (a cell's text, "\|" already made a literal pipe).
            // Returns the runs; empty for an empty or blank text. Throws std::bad_alloc only.
            std::vector<MdRun> _InlineRuns(std::string_view markdown)
            {
                const std::string_view trimmed = TrimAsciiSpace(markdown);
                if (trimmed.empty())
                {
                    return {};
                }
                MdParseOptions sub;
                sub.softBreaksAsLineBreaks = _options.softBreaksAsLineBreaks; // no caps, no tables: one cell's text
                MdParseResult parsed;
                std::unique_ptr<cmark_node, CmarkNodeDeleter> document{ cmark_parse_document(trimmed.data(), trimmed.size(), CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8) };
                if (document)
                {
                    cmark_consolidate_text_nodes(document.get());
                    std::unique_ptr<cmark_iter, CmarkIterDeleter> iter{ cmark_iter_new(document.get()) };
                    if (iter)
                    {
                        BlockWalker walker{ sub, parsed };
                        for (auto ev = cmark_iter_next(iter.get()); ev != CMARK_EVENT_DONE; ev = cmark_iter_next(iter.get()))
                        {
                            walker.Visit(cmark_iter_get_node(iter.get()), ev);
                        }
                    }
                }
                if (parsed.blocks.size() == 1 && parsed.blocks.front().kind == MdBlockKind::Paragraph && !parsed.blocks.front().rawHtml)
                {
                    return std::move(parsed.blocks.front().runs);
                }
                MdRun plain;
                plain.text = FromUtf8(std::string{ trimmed }.c_str());
                std::vector<MdRun> runs;
                runs.push_back(std::move(plain));
                return runs;
            }

            // How many UTF-16 units `runs` hold - a table's share of MdParseOptions::maxBlockChars.
            static size_t _RunChars(const std::vector<MdRun>& runs) noexcept
            {
                size_t chars = 0;
                for (const auto& run : runs)
                {
                    chars += run.text.size();
                }
                return chars;
            }

            // If paragraph `node` holds a GFM pipe table, append it as a Table block - preceded by a Paragraph for the
            // paragraph's lines above the header row, if any - and say so. The rows are the paragraph's RAW source
            // lines (the first from the paragraph's own start column, the others without their container prefixes),
            // so an escaped "\|" is told from a cell border. The header row is the first line followed by a
            // delimiter row of the same cell count, and at least one of the two must hold an unescaped '|' (a lone
            // ":--" under a word is no table). Every later line of the paragraph is a body row (GFM: a table runs to
            // the end of its paragraph), padded or cut to the header's cell count. maxBlockChars keeps the header and
            // the body rows that fit, flagging the block truncated.
            // Parameters: node - a PARAGRAPH node, on its ENTER event.
            // Returns NotTable (nothing appended), Emitted, or Stop (the block cap was hit; the result is flagged).
            // Throws std::bad_alloc only.
            TableOutcome _TryTable(cmark_node* node)
            {
                const int startLine = cmark_node_get_start_line(node);
                const int endLine = cmark_node_get_end_line(node);
                if (_source.empty() || startLine <= 0 || endLine <= startLine)
                {
                    return TableOutcome::NotTable; // a table needs a header and a delimiter row: two lines at least
                }
                _EnsureLineStarts();
                std::vector<std::string_view> lines;
                lines.reserve(static_cast<size_t>(endLine - startLine + 1));
                for (int line = startLine; line <= endLine; ++line)
                {
                    std::string_view raw = _SourceLine(line);
                    if (line == startLine)
                    {
                        // The first line from the paragraph's own start (cmark's 1-based byte column): every container
                        // marker before it is already behind us.
                        const size_t column = static_cast<size_t>(std::max(cmark_node_get_start_column(node), 1)) - 1;
                        raw = raw.substr(std::min(column, raw.size()));
                    }
                    else
                    {
                        raw = StripContainerPrefixes(raw, _stack);
                    }
                    lines.push_back(raw);
                }
                size_t header = std::string_view::npos;
                std::vector<MdAlign> align;
                for (size_t i = 0; i + 1 < lines.size(); ++i)
                {
                    std::vector<MdAlign> candidate;
                    if (!ParseDelimiterRow(lines[i + 1], candidate))
                    {
                        continue;
                    }
                    if (!HasUnescapedPipe(lines[i]) && !HasUnescapedPipe(lines[i + 1]))
                    {
                        continue;
                    }
                    if (SplitTableRow(lines[i]).size() != candidate.size())
                    {
                        continue; // GFM: the header and the delimiter row must agree on the column count
                    }
                    header = i;
                    align = std::move(candidate);
                    break;
                }
                if (header == std::string_view::npos)
                {
                    return TableOutcome::NotTable;
                }
                if (header > 0)
                {
                    // The paragraph's lines above the header row stay a paragraph (GFM ends a paragraph where a table
                    // starts), their line breaks kept.
                    std::string above;
                    for (size_t i = 0; i < header; ++i)
                    {
                        if (i > 0)
                        {
                            above += '\n';
                        }
                        above.append(lines[i]);
                    }
                    auto* paragraph = _Begin(MdBlockKind::Paragraph);
                    if (!paragraph)
                    {
                        return TableOutcome::Stop;
                    }
                    paragraph->runs = _InlineRuns(above);
                }
                auto* table = _Begin(MdBlockKind::Table);
                if (!table)
                {
                    return TableOutcome::Stop;
                }
                table->tableAlign = align;
                const size_t columns = align.size();
                size_t chars = 0;
                for (size_t i = header; i < lines.size(); ++i)
                {
                    if (i == header + 1)
                    {
                        continue; // the delimiter row: its alignments are already read
                    }
                    const std::vector<std::string> cells = SplitTableRow(lines[i]);
                    MdTableRow row;
                    row.cells.resize(columns); // a short row's missing cells stay empty; a long row's excess is dropped
                    size_t rowChars = 0;
                    for (size_t c = 0; c < columns && c < cells.size(); ++c)
                    {
                        row.cells[c] = _InlineRuns(cells[c]);
                        rowChars += _RunChars(row.cells[c]);
                    }
                    if (_options.maxBlockChars != 0 && i != header && chars + rowChars > _options.maxBlockChars)
                    {
                        table->truncated = true; // the header always stays; the body rows past the cap go
                        break;
                    }
                    chars += rowChars;
                    table->tableRows.push_back(std::move(row));
                }
                return TableOutcome::Emitted;
            }

            // Per open list: what its items need to know when they open.
            struct ListFrame
            {
                uint32_t id = 0; // the list's own container-namespace id (MdContainer::listId of its items)
                bool ordered = false; // numbered list
                bool paren = false; // "1)" delimiter instead of "1."
                bool tight = false; // no blank lines between items
                int next = 1; // the number the next ordered item shows
                uint8_t markerChars = 1; // the widest marker any of its items shows, in characters
            };

            // Read a LIST node's numbering + tightness and size its marker column. Counting the items
            // up front is what makes "9." and "10." line up: every item of the list indents by the
            // width of its WIDEST number, not its own.
            ListFrame _MakeListFrame(cmark_node* list)
            {
                ListFrame frame;
                frame.id = _nextId++;
                frame.ordered = cmark_node_get_list_type(list) == CMARK_ORDERED_LIST;
                frame.paren = cmark_node_get_list_delim(list) == CMARK_PAREN_DELIM;
                frame.tight = cmark_node_get_list_tight(list) != 0;
                frame.next = cmark_node_get_list_start(list);
                if (frame.ordered)
                {
                    int items = 0;
                    for (auto* child = cmark_node_first_child(list); child; child = cmark_node_next(child))
                    {
                        ++items;
                    }
                    const long long highest = static_cast<long long>(frame.next) + std::max(items, 1) - 1;
                    // +1 for the delimiter; capped so a pathological start number can't size a huge column.
                    frame.markerChars = static_cast<uint8_t>(std::min<size_t>(DecimalDigits(highest) + 1, 12));
                }
                return frame;
            }

            // A new list item container: its list's facts + the marker its FIRST block will show.
            MdContainer _MakeItem()
            {
                MdContainer item;
                item.kind = MdContainerKind::Item;
                item.id = _nextId++;
                if (_lists.empty())
                {
                    item.marker = BulletFor(0); // an item outside a list: cmark never builds one — belt
                    return item;
                }
                auto& frame = _lists.back();
                item.listId = frame.id;
                item.tightList = frame.tight;
                item.ordered = frame.ordered;
                item.markerChars = frame.markerChars;
                if (frame.ordered)
                {
                    item.marker = std::to_wstring(frame.next) + (frame.paren ? L")" : L".");
                    if (frame.next < INT_MAX)
                    {
                        ++frame.next;
                    }
                }
                else
                {
                    // The bullet glyph follows the LIST nesting depth (ordered parents count too), so a
                    // bullet list inside a numbered one still steps to ◦.
                    item.marker = BulletFor(_lists.size() - 1);
                }
                return item;
            }

            // Open a new block of `kind` with the current containers. Consumes every pending item
            // marker: the first block of an item shows its marker, every later one gets a blank
            // hanging indent.
            // Returns the new block, or nullptr when MdParseOptions::maxBlocks is reached (the walk
            // must stop; the result's truncatedBlocks is set).
            MdBlock* _Begin(MdBlockKind kind)
            {
                _EndInlineBlock(); // cmark never nests blocks inside an open paragraph — a belt, not a path
                if (_options.maxBlocks != 0 && _out.blocks.size() >= _options.maxBlocks)
                {
                    _out.truncatedBlocks = true;
                    return nullptr;
                }
                MdBlock block;
                block.kind = kind;
                block.containers = _stack;
                for (auto& container : _stack)
                {
                    container.marker.clear();
                }
                _out.blocks.push_back(std::move(block));
                _blockChars = 0;
                return &_out.blocks.back();
            }

            // Append inline text to the open Paragraph / Heading, with the current emphasis, strong and
            // link state OR'ed onto `extraStyle`. Merges into the previous run when the style and URL
            // match. Honors MdParseOptions::maxBlockChars (cuts + flags the block once). A no-op when
            // no inline block is open (inline content outside a paragraph never happens in CommonMark).
            void _Append(std::wstring_view text, uint8_t extraStyle)
            {
                if (!_inInline || _out.blocks.empty() || text.empty())
                {
                    return;
                }
                auto& block = _out.blocks.back();
                std::wstring piece{ text };
                if (_options.maxBlockChars != 0)
                {
                    if (_blockChars >= _options.maxBlockChars)
                    {
                        block.truncated = true;
                        return;
                    }
                    if (CutAt(piece, _options.maxBlockChars - _blockChars))
                    {
                        block.truncated = true;
                    }
                    if (piece.empty())
                    {
                        return;
                    }
                }
                uint8_t style = extraStyle;
                if (_strong > 0)
                {
                    style |= kMdBold;
                }
                if (_emph > 0)
                {
                    style |= kMdItalic;
                }
                const std::wstring* url = nullptr;
                if (!_links.empty())
                {
                    style |= kMdLink;
                    url = &_links.back();
                }
                _blockChars += piece.size();
                if (!block.runs.empty())
                {
                    auto& last = block.runs.back();
                    const bool sameUrl = url ? last.url == *url : last.url.empty();
                    if (last.style == style && sameUrl)
                    {
                        last.text.append(piece);
                        return;
                    }
                }
                MdRun run;
                run.text = std::move(piece);
                run.style = style;
                if (url)
                {
                    run.url = *url;
                }
                block.runs.push_back(std::move(run));
            }

            // Close the open Paragraph / Heading: drop a trailing line break (it would draw an empty
            // last line) and any run it leaves empty. Idempotent.
            void _EndInlineBlock() noexcept
            {
                if (!_inInline)
                {
                    return;
                }
                _inInline = false;
                if (_out.blocks.empty())
                {
                    return;
                }
                auto& runs = _out.blocks.back().runs;
                while (!runs.empty())
                {
                    auto& text = runs.back().text;
                    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
                    {
                        text.pop_back();
                    }
                    if (!text.empty())
                    {
                        break;
                    }
                    runs.pop_back();
                }
            }

            const MdParseOptions& _options;
            MdParseResult& _out;
            std::string_view _source; // the UTF-8 document (tables read a paragraph's raw lines from it); may be empty
            std::vector<size_t> _lineStarts; // byte offset of each source line's start, built on the first table check
            std::vector<MdContainer> _stack; // the quotes / items around the current position, outermost first
            std::vector<ListFrame> _lists; // the open lists, outermost first
            std::vector<std::wstring> _links; // the open links' / images' URLs, innermost last
            uint32_t _nextId = 1; // the next container / list id (0 is never a real one)
            int _strong = 0; // open **strong** nodes
            int _emph = 0; // open *emphasis* nodes
            bool _inInline = false; // a Paragraph / Heading is open: inline events append to _out.blocks.back()
            size_t _blockChars = 0; // UTF-16 units appended to the open block so far (maxBlockChars)
        };
    }

    MdParseResult ParseMarkdownBlocks(std::wstring_view markdown, const MdParseOptions& options) noexcept
    {
        MdParseResult result;
        try
        {
            const std::string utf8 = ToUtf8(markdown);
            if (utf8.empty())
            {
                return result;
            }
            // VALIDATE_UTF8: a belt — ToUtf8's output is well-formed, but cmark then guarantees every
            // literal it hands back is too, which FromUtf8 relies on. No SMART (it would rewrite the
            // quotes and dashes a transcript quotes verbatim), no HARDBREAKS (soft breaks are decided
            // by MdParseOptions, per event).
            std::unique_ptr<cmark_node, CmarkNodeDeleter> document{ cmark_parse_document(utf8.data(), utf8.size(), CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8) };
            if (!document)
            {
                result.failed = true;
                AppendStateLog(L"hooks.log", L"[transcript] markdown parse failed: cmark returned no document (chars=" + std::to_wstring(markdown.size()) + L")\n");
                return result;
            }
            // Joins the adjacent TEXT nodes cmark splits at '[', '!', '<', ... so a paragraph is a few
            // runs instead of one per punctuation-delimited fragment.
            cmark_consolidate_text_nodes(document.get());
            std::unique_ptr<cmark_iter, CmarkIterDeleter> iter{ cmark_iter_new(document.get()) };
            if (!iter)
            {
                result.failed = true;
                AppendStateLog(L"hooks.log", L"[transcript] markdown parse failed: cmark returned no iterator\n");
                return result;
            }
            BlockWalker walker{ options, result, utf8 }; // the source is read only to find tables (options.tables)
            for (auto ev = cmark_iter_next(iter.get()); ev != CMARK_EVENT_DONE; ev = cmark_iter_next(iter.get()))
            {
                if (!walker.Visit(cmark_iter_get_node(iter.get()), ev))
                {
                    break; // the block cap was reached — what was parsed is the answer
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery = the blocks parsed so far, flagged failed; the overlay shows them and
            // says the rest could not be rendered.
            result.failed = true;
            LogSwallowedException(L"ParseMarkdownBlocks");
        }
        return result;
    }

    size_t MdSharedContainerDepth(const MdBlock& prev, const MdBlock& cur) noexcept
    {
        const size_t limit = std::min(prev.containers.size(), cur.containers.size());
        size_t depth = 0;
        // Ids are unique per container node, so an equal id at the same position IS the same quote /
        // item — no kind check needed.
        while (depth < limit && prev.containers[depth].id == cur.containers[depth].id)
        {
            ++depth;
        }
        return depth;
    }

    bool MdIsLabelParagraph(const MdBlock& b) noexcept
    {
        if (b.kind != MdBlockKind::Paragraph || b.rawHtml || b.runs.empty())
        {
            return false;
        }
        // Only the FIRST LINE is judged. At the Full / Max levels transcript_turns writes the turn's stats
        // line right under "**● Claude**" with a single newline, and CommonMark folds the two into ONE
        // paragraph — the label is still the first line, whatever follows it.
        size_t chars = 0;
        bool sawBold = false;
        for (const auto& run : b.runs)
        {
            const std::wstring_view text{ run.text };
            const auto lineEnd = text.find_first_of(L"\r\n");
            const auto firstLinePart = lineEnd == std::wstring_view::npos ? text : text.substr(0, lineEnd);
            chars += firstLinePart.size();
            bool blank = true;
            for (const wchar_t ch : firstLinePart)
            {
                if (!iswspace(ch))
                {
                    blank = false;
                    break;
                }
            }
            if (!blank)
            {
                if ((run.style & kMdBold) == 0)
                {
                    return false; // plain text on the first line: prose with some bold in it, not a label
                }
                sawBold = true;
            }
            if (lineEnd != std::wstring_view::npos)
            {
                break; // the first line ended inside this run
            }
        }
        return sawBold && chars <= kLabelMaxChars;
    }

    double MdGapAbove(const MdBlock* prev, const MdBlock& cur) noexcept
    {
        if (!prev)
        {
            return 0.0;
        }
        if (cur.kind == MdBlockKind::Heading)
        {
            return cur.headingLevel <= 2 ? kGapHeadingMajor : kGapHeadingMinor;
        }
        if (prev->kind == MdBlockKind::Heading)
        {
            return kGapAfterHeading;
        }
        if (MdIsLabelParagraph(*prev))
        {
            return kGapAfterLabel;
        }
        // A table is a framed panel like a code block: it gets the same little extra air on both sides.
        const bool codeInvolved = cur.kind == MdBlockKind::Code || prev->kind == MdBlockKind::Code ||
                                  cur.kind == MdBlockKind::Rule || prev->kind == MdBlockKind::Rule ||
                                  cur.kind == MdBlockKind::Table || prev->kind == MdBlockKind::Table;
        const size_t shared = MdSharedContainerDepth(*prev, cur);
        bool tight = false;
        if (shared < cur.containers.size())
        {
            // `cur` opens a container `prev` was not in. A new item of a TIGHT list is packed whether
            // it is the next sibling, the first item of a list nested in prev's item, or the start of a
            // list right under a paragraph; a quote (or a loose list) gets paragraph spacing.
            const auto& opened = cur.containers[shared];
            tight = opened.kind == MdContainerKind::Item && opened.tightList;
        }
        else if (!cur.containers.empty())
        {
            // Every container of `cur` was shared with `prev`: a continuation block (an item's second
            // paragraph or code block, or the text after a nested list). Packed when its innermost
            // container is an item of a tight list; a quote's paragraphs keep paragraph spacing.
            const auto& inner = cur.containers.back();
            tight = inner.kind == MdContainerKind::Item && inner.tightList;
        }
        if (tight)
        {
            return codeInvolved ? kGapTightCode : kGapTight;
        }
        return codeInvolved ? kGapAroundCode : kGapParagraph;
    }

    // ---- tool areas (MarkdownBlocks.h): what the transcript overlay folds -----------------------
    namespace
    {
        // The marks transcript_turns starts its speaker headers and list items with (TranscriptTurns.cpp
        // SpeakerBlock / EventLines). The two emoji are astral, so each is matched as its UTF-16
        // surrogate pair: one shared high half, a distinct low half.
        constexpr wchar_t kSpeakerMark = L'\x25CF'; // ● — a speaker header (Claude, an agent entry)
        constexpr wchar_t kPromptMark = L'\x276F'; // ❯ — a prompt / brief, or (nested) an answer
        constexpr wchar_t kMessageMark = L'\x2709'; // ✉ — a message that reached the agent
        constexpr wchar_t kAstralHigh = L'\xD83D'; // the high surrogate both emoji below share
        constexpr wchar_t kRemarkLow = L'\xDCAC'; // 💬 U+1F4AC — a remark between calls
        constexpr wchar_t kCompactionLow = L'\xDDDC'; // 🗜 U+1F5DC — a context compaction

        // The longest tool name a summary shows; a longer one (an MCP tool's qualified name) is cut
        // and ends in an ellipsis.
        constexpr size_t kToolNameMaxChars = 40;

        // A block's first VISIBLE UTF-16 unit: where an item's marker glyph (or its bold tool name) is.
        struct LeadUnit
        {
            wchar_t unit = L'\0'; // L'\0' == the block shows no text (an empty item, a code block)
            wchar_t next = L'\0'; // the unit right after it in the same run — a surrogate pair's low half
            uint8_t style = 0; // the kMd* bits of the run that holds it
            size_t run = 0; // that run's index in MdBlock::runs
        };

        // Scan `block`'s runs for the first non-blank unit. Never throws.
        LeadUnit FirstVisibleUnit(const MdBlock& block) noexcept
        {
            LeadUnit lead;
            for (size_t r = 0; r < block.runs.size(); ++r)
            {
                const std::wstring& text = block.runs[r].text;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    if (iswspace(text[i]))
                    {
                        continue;
                    }
                    lead.unit = text[i];
                    lead.next = i + 1 < text.size() ? text[i + 1] : L'\0';
                    lead.style = block.runs[r].style;
                    lead.run = r;
                    return lead;
                }
            }
            return lead;
        }

        // A SPEAKER HEADER (MdFindToolAreas): top-level, not raw HTML, first visible unit '●' in a bold
        // run. Never throws.
        bool IsSpeakerHeader(const MdBlock& block) noexcept
        {
            if (block.kind != MdBlockKind::Paragraph || block.rawHtml || !block.containers.empty())
            {
                return false;
            }
            const LeadUnit lead = FirstVisibleUnit(block);
            return lead.unit == kSpeakerMark && (lead.style & kMdBold) != 0;
        }

        // `block` is the FIRST block of a top-level list item — the only block of the item that carries
        // its marker (MdContainer::marker). Never throws.
        bool StartsTopLevelItem(const MdBlock& block) noexcept
        {
            return !block.containers.empty() && block.containers[0].kind == MdContainerKind::Item && !block.containers[0].marker.empty();
        }

        // `block` is the first block of an item NESTED directly inside a top-level item (depth 2): the
        // shape of the answer / output / background lines transcript_turns puts under a call. Never throws.
        bool StartsNestedItem(const MdBlock& block) noexcept
        {
            return block.containers.size() >= 2 && block.containers[1].kind == MdContainerKind::Item && !block.containers[1].marker.empty();
        }

        // The block's whole text without surrounding blanks — for the "(no tool calls)" test only.
        // Throws std::bad_alloc only (the caller's catch logs it).
        std::wstring TrimmedText(const MdBlock& block)
        {
            std::wstring text;
            for (const auto& run : block.runs)
            {
                text += run.text;
            }
            const auto first = text.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos)
            {
                return {};
            }
            const auto last = text.find_last_not_of(L" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        // Count one top-level item (its first block) into `area` by its leading mark. A bold lead is a
        // tool call: OpSummary writes "**Name**" first, so the bold run's text IS the tool name.
        // Throws std::bad_alloc only (a new tool name entry).
        void CountTopLevelItem(const MdBlock& block, MdToolArea& area)
        {
            const LeadUnit lead = FirstVisibleUnit(block);
            if (lead.unit == kAstralHigh && lead.next == kRemarkLow)
            {
                ++area.remarks;
            }
            else if (lead.unit == kAstralHigh && lead.next == kCompactionLow)
            {
                ++area.compactions;
            }
            else if (lead.unit == kMessageMark)
            {
                ++area.messages;
            }
            else if (lead.unit == kPromptMark)
            {
                ++area.prompts;
            }
            else if (lead.unit != L'\0' && (lead.style & kMdBold) != 0)
            {
                ++area.toolCalls;
                std::wstring name = block.runs[lead.run].text;
                const auto from = name.find_first_not_of(L" \t\r\n");
                const auto to = name.find_last_not_of(L" \t\r\n");
                name = from == std::wstring::npos ? std::wstring{} : name.substr(from, to - from + 1);
                if (CutAt(name, kToolNameMaxChars - 1))
                {
                    name += L'\x2026';
                }
                for (auto& [known, calls] : area.tools)
                {
                    if (known == name)
                    {
                        ++calls;
                        return;
                    }
                }
                area.tools.emplace_back(std::move(name), size_t{ 1 });
            }
            else
            {
                ++area.others;
            }
        }

        // "1 remark" / "3 remarks" — the summary's count phrases. Throws std::bad_alloc only.
        std::wstring CountPhrase(size_t count, std::wstring_view singular, std::wstring_view plural)
        {
            return std::to_wstring(count) + L" " + std::wstring{ count == 1 ? singular : plural };
        }
    }

    std::vector<MdToolArea> MdFindToolAreas(const std::vector<MdBlock>& blocks) noexcept
    {
        std::vector<MdToolArea> areas;
        try
        {
            for (size_t i = 0; i + 1 < blocks.size(); ++i)
            {
                if (!IsSpeakerHeader(blocks[i]) || !StartsTopLevelItem(blocks[i + 1]))
                {
                    continue;
                }
                MdToolArea area;
                area.first = i + 1;
                // Every block of the list shares its OUTERMOST container's list id: the items themselves,
                // their continuation blocks, and the lists / code blocks nested inside them.
                const uint32_t listId = blocks[i + 1].containers[0].listId;
                size_t j = i + 1;
                for (; j < blocks.size(); ++j)
                {
                    const MdBlock& block = blocks[j];
                    if (block.containers.empty() || block.containers[0].kind != MdContainerKind::Item || block.containers[0].listId != listId)
                    {
                        break;
                    }
                    if (StartsTopLevelItem(block))
                    {
                        CountTopLevelItem(block, area);
                    }
                    else if (StartsNestedItem(block) && FirstVisibleUnit(block).unit == kPromptMark)
                    {
                        ++area.answers;
                    }
                }
                area.end = j;
                // A speaker with no call and no remark: transcript_turns writes the single item
                // "(no tool calls)" (or "(no remarks)") — nothing to fold.
                if (area.end - area.first == 1 && area.others == 1 && area.toolCalls == 0)
                {
                    const std::wstring text = TrimmedText(blocks[area.first]);
                    if (text == L"(no tool calls)" || text == L"(no remarks)")
                    {
                        area.empty = true;
                        area.others = 0;
                    }
                }
                areas.push_back(std::move(area));
                i = j - 1; // resume after the list (the loop's ++i lands on the block that ended it)
            }
        }
        catch (...)
        {
            // Rule #18. Recovery = the areas found so far: the overlay folds those and shows the rest as
            // plain lists, which is the pre-fold rendering — nothing is lost.
            LogSwallowedException(L"MdFindToolAreas");
        }
        return areas;
    }

    bool MdToolAreaFoldable(const MdToolArea& area) noexcept
    {
        return !area.empty && area.toolCalls + area.remarks > 0;
    }

    std::wstring MdToolAreaSummary(const MdToolArea& area, size_t maxTools) noexcept
    {
        try
        {
            std::vector<std::wstring> parts;
            if (area.toolCalls > 0)
            {
                std::wstring calls = CountPhrase(area.toolCalls, L"tool call", L"tool calls");
                if (maxTools > 0 && !area.tools.empty())
                {
                    // Most-called first; a STABLE sort keeps the first-call order among equal counts.
                    auto byCalls = area.tools;
                    std::stable_sort(byCalls.begin(), byCalls.end(), [](const auto& a, const auto& b) {
                        return a.second > b.second;
                    });
                    const size_t shown = std::min(maxTools, byCalls.size());
                    calls += L": ";
                    for (size_t k = 0; k < shown; ++k)
                    {
                        if (k > 0)
                        {
                            calls += L", ";
                        }
                        calls += byCalls[k].first;
                        if (byCalls[k].second > 1)
                        {
                            calls += L" \x00D7"; // the multiplication sign: "Bash ×3"
                            calls += std::to_wstring(byCalls[k].second);
                        }
                    }
                    if (byCalls.size() > shown)
                    {
                        calls += L" +" + std::to_wstring(byCalls.size() - shown) + L" more";
                    }
                }
                parts.push_back(std::move(calls));
            }
            if (area.remarks > 0)
            {
                parts.push_back(CountPhrase(area.remarks, L"remark", L"remarks"));
            }
            if (area.messages > 0)
            {
                parts.push_back(CountPhrase(area.messages, L"message", L"messages"));
            }
            if (area.prompts > 0)
            {
                parts.push_back(CountPhrase(area.prompts, L"prompt", L"prompts"));
            }
            if (area.answers > 0)
            {
                parts.push_back(CountPhrase(area.answers, L"answer", L"answers"));
            }
            if (area.compactions > 0)
            {
                parts.push_back(CountPhrase(area.compactions, L"compaction", L"compactions"));
            }
            if (area.others > 0)
            {
                parts.push_back(CountPhrase(area.others, L"other line", L"other lines"));
            }
            std::wstring summary;
            for (size_t k = 0; k < parts.size(); ++k)
            {
                if (k > 0)
                {
                    summary += L" \x00B7 "; // the middle dot transcript_turns separates its facts with
                }
                summary += parts[k];
            }
            return summary;
        }
        catch (...)
        {
            // Rule #18. Recovery = no summary text; the overlay then falls back to a generic label.
            LogSwallowedException(L"MdToolAreaSummary");
            return {};
        }
    }

    // ---- session chain links (MarkdownBlocks.h): the "session N" mentions the overlay links ----------
    namespace
    {
        // What TranscriptTurns writes (ChainBlock / SessionHeader): a chain row names a session with the
        // lowercase word ("- session 2 · …", "… — forked from session 1 on …"), a session heading starts
        // "Session N of M" ("## Session 2 of 4 · `edd314dd` · …"), and a row puts the session's title between
        // curly quotes ("· “its title” —").
        constexpr std::wstring_view kSessionWord{ L"session " };
        constexpr std::wstring_view kSessionHeadingStart{ L"Session " };
        constexpr std::wstring_view kSessionHeadingOf{ L" of " };
        constexpr wchar_t kTitleOpen = L'\x201C';
        constexpr wchar_t kTitleClose = L'\x201D';

        // `text` holds `literal` at `at` — false when it would run past the end (so the compare below can
        // never be handed an out-of-range position). Never throws.
        bool TextHasAt(std::wstring_view text, size_t at, std::wstring_view literal) noexcept
        {
            return at <= text.size() && text.size() - at >= literal.size() && text.compare(at, literal.size(), literal) == 0;
        }

        // Read a session number at `at`: 1-4 digits making 1..kMdMaxSessionNumber. A 5th digit means it is
        // not a session number at all (never the first four digits of one).
        // Returns the number and moves `at` past the digits; returns 0 and leaves `at` alone when there is
        // none. Never throws.
        size_t ReadSessionNumber(std::wstring_view text, size_t& at) noexcept
        {
            size_t value = 0;
            size_t digits = 0;
            while (at + digits < text.size() && text[at + digits] >= L'0' && text[at + digits] <= L'9')
            {
                if (digits == 4)
                {
                    return 0;
                }
                value = value * 10 + static_cast<size_t>(text[at + digits] - L'0');
                ++digits;
            }
            if (digits == 0 || value == 0 || value > kMdMaxSessionNumber)
            {
                return 0;
            }
            at += digits;
            return value;
        }

        // The chain list's INTRO paragraph (MdFindSessionChain): top-level, not raw HTML, its first run the
        // bold "Session chain" — ChainBlock writes "**Session chain**, oldest first (…):". Never throws.
        bool IsChainIntro(const MdBlock& block) noexcept
        {
            return block.kind == MdBlockKind::Paragraph && !block.rawHtml && block.containers.empty() && !block.runs.empty() &&
                   (block.runs[0].style & kMdBold) != 0 && block.runs[0].text == L"Session chain";
        }
    }

    size_t MdSessionHeadingNumber(const MdBlock& block) noexcept
    {
        if (block.kind != MdBlockKind::Heading || !block.containers.empty() || block.runs.empty())
        {
            return 0;
        }
        // "Session N of M" sits wholly in the FIRST run: the short session id after it is a code span — a run
        // of its own.
        const std::wstring_view text{ block.runs[0].text };
        if ((block.runs[0].style & (kMdCode | kMdLink)) != 0 || !TextHasAt(text, 0, kSessionHeadingStart))
        {
            return 0;
        }
        size_t at = kSessionHeadingStart.size();
        const size_t session = ReadSessionNumber(text, at);
        if (session == 0 || !TextHasAt(text, at, kSessionHeadingOf))
        {
            return 0;
        }
        at += kSessionHeadingOf.size();
        return ReadSessionNumber(text, at) == 0 ? 0 : session;
    }

    std::vector<MdSessionRef> MdFindSessionRefs(const MdBlock& block) noexcept
    {
        std::vector<MdSessionRef> refs;
        try
        {
            bool inTitle = false; // inside a “…” title — carried across runs, since a title may hold a code span
            for (size_t r = 0; r < block.runs.size(); ++r)
            {
                const MdRun& run = block.runs[r];
                if ((run.style & (kMdCode | kMdLink)) != 0)
                {
                    continue; // a session id or a URL is never a mention
                }
                const std::wstring_view text{ run.text };
                for (size_t i = 0; i < text.size(); ++i)
                {
                    const wchar_t ch = text[i];
                    if (ch == kTitleOpen)
                    {
                        inTitle = true;
                        continue;
                    }
                    if (ch == kTitleClose)
                    {
                        inTitle = false;
                        continue;
                    }
                    // The cheap rejections first: nearly every unit is not an 's' that starts a word. The
                    // start of a run counts as a word start (a run boundary is a style change, and a row's
                    // mentions follow a space, a dash or the row's start).
                    if (inTitle || ch != L's' || (i > 0 && (iswalnum(text[i - 1]) || text[i - 1] == L'_')))
                    {
                        continue;
                    }
                    if (!TextHasAt(text, i, kSessionWord))
                    {
                        continue;
                    }
                    size_t at = i + kSessionWord.size();
                    const size_t session = ReadSessionNumber(text, at);
                    if (session == 0)
                    {
                        continue; // "this session", "session ids": the word without a number
                    }
                    refs.push_back(MdSessionRef{ r, i, at - i, session });
                    i = at - 1; // resume after the number (the loop's ++i lands on the unit that follows it)
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery = the mentions found so far: the overlay links those and leaves the rest as
            // plain text — the row as it read before links existed.
            LogSwallowedException(L"MdFindSessionRefs");
        }
        return refs;
    }

    MdSessionChain MdFindSessionChain(const std::vector<MdBlock>& blocks) noexcept
    {
        MdSessionChain chain;
        try
        {
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                if (const size_t session = MdSessionHeadingNumber(blocks[i]); session > 0)
                {
                    if (chain.headings.size() <= session)
                    {
                        chain.headings.resize(session + 1, kMdNoBlock); // bounded: session <= kMdMaxSessionNumber
                    }
                    if (chain.headings[session] == kMdNoBlock)
                    {
                        chain.headings[session] = i; // a session's FIRST heading is where its links go
                    }
                    continue;
                }
                if (chain.end == 0 && i + 1 < blocks.size() && IsChainIntro(blocks[i]) && StartsTopLevelItem(blocks[i + 1]))
                {
                    // Every block of the list shares its OUTERMOST container's list id (as MdFindToolAreas reads
                    // a list): the rows, and anything nested inside a row.
                    const uint32_t listId = blocks[i + 1].containers[0].listId;
                    size_t j = i + 1;
                    while (j < blocks.size() && !blocks[j].containers.empty() && blocks[j].containers[0].kind == MdContainerKind::Item &&
                           blocks[j].containers[0].listId == listId)
                    {
                        ++j;
                    }
                    chain.first = i + 1;
                    chain.end = j;
                    i = j - 1; // resume after the list (the loop's ++i lands on the block that ended it)
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery = what was found so far: links to the headings already seen work, a session
            // whose heading was not reached yet simply gets no link.
            LogSwallowedException(L"MdFindSessionChain");
        }
        return chain;
    }
}
