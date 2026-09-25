# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Agentmaster - turn oracle.js's output (Claude Code's own invisible-character cleanup, run over
# vectors.json) into the C++ rows of tests_invisible.cpp's kInvisibleOracle table (DELIVERY.md
# section 15 "Re-extracting the rule").
#
# The rows are PURE ASCII: every non-ASCII UTF-16 unit becomes an escape - \uXXXX / \UXXXXXXXX by code
# point, and \xXXXX for controls and lone surrogates (a literal break " L" follows a \x escape whenever
# the next character is a hex digit, since a C++ \x escape swallows every hex digit after it). A test
# source must never carry a literal invisible character: editors silently drop or normalize them.
#
# Usage: python gen_table.py oracle.json rows.inc   -> paste rows.inc over kInvisibleOracle's rows.
# Exit:  0 on success; a non-ASCII byte in the output is an assertion failure (never written).
import json
import sys

HEX = set('0123456789abcdefABCDEF')


def unescape(s):
    """oracle.js's escaping back to a list of UTF-16 units (\\xXXXX fixed width, \\\\ and \\").

    s       -- an `input` / `text` field of oracle.js's JSON output.
    returns -- the exact UTF-16 code units (lone surrogates included) the field stands for.
    raises  -- ValueError / IndexError on a malformed escape (the file is not oracle.js output).
    """
    units, i = [], 0
    while i < len(s):
        if s[i] == '\\':
            if s[i + 1] == 'x':
                units.append(int(s[i + 2:i + 6], 16))
                i += 6
            else:
                units.append(ord(s[i + 1]))
                i += 2
        else:
            units.append(ord(s[i]))
            i += 1
    return units


def cpp_wide_literal(units):
    """A pure-ASCII C++ L"..." literal reproducing `units` exactly (see the header comment).

    units   -- UTF-16 code units; a valid surrogate pair is emitted as ONE \\U escape, a lone
               surrogate as a \\x escape (MSVC keeps it as a single wchar_t).
    returns -- the literal text, e.g. L"a\\x000D" L"b" for a, CR, b.
    raises  -- nothing.
    """
    out, pending_hex_break, i = [], False, 0
    while i < len(units):
        u = units[i]
        if 0xD800 <= u <= 0xDBFF and i + 1 < len(units) and 0xDC00 <= units[i + 1] <= 0xDFFF:
            cp = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00)
            out.append('\\U%08X' % cp)
            pending_hex_break = False
            i += 2
            continue
        if 0x20 <= u <= 0x7E:
            ch = chr(u)
            if pending_hex_break and ch in HEX:
                out.append('" L"')
            pending_hex_break = False
            out.append({'\\': '\\\\', '"': '\\"'}.get(ch, ch))
        elif u < 0xA0 or 0xD800 <= u <= 0xDFFF:
            out.append('\\x%04X' % u)
            pending_hex_break = True
        else:
            out.append('\\u%04X' % u)
            pending_hex_break = False
        i += 1
    return 'L"' + ''.join(out) + '"'


def main():
    """Read argv[1] (oracle.js output), write the C++ rows to argv[2].

    raises -- OSError on an unreadable/unwritable path, json.JSONDecodeError on a bad input file,
              AssertionError if a generated row would carry a non-ASCII character (never written).
    """
    oracle = json.load(open(sys.argv[1], encoding='utf-8'))
    rows = []
    for v in oracle:
        c = v['removedByClass']
        rows.append('        { "%s", %s, %s, %d, %d, %d, %d, %d, %d, %d, %s },' % (
            v['name'], cpp_wide_literal(unescape(v['input'])), cpp_wide_literal(unescape(v['text'])), v['removedTotal'],
            c['tags'], c['bidi'], c['zeroWidth'], c['selectors'], c['other'], v['keptConditional'],
            'true' if v['idempotent'] else 'false'))
    table = '\n'.join(rows) + '\n'
    assert all(ord(ch) < 0x80 for ch in table), 'generated rows must be pure ASCII'
    open(sys.argv[2], 'w', encoding='ascii', newline='\n').write(table)
    print(len(rows), 'rows ->', sys.argv[2])


if __name__ == '__main__':
    main()
