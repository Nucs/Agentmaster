// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ORACLE for Claude Code's Enter-time invisible-character cleanup (DELIVERY.md §15).
//
// WHAT IT DOES. Locates the cleanup function inside the LOCAL claude.exe (a Bun single-file binary —
// its bundled JS is plain text inside the file), evaluates Claude's OWN code under node, and runs it
// over every vector in vectors.json. The output is ground truth for tests_invisible.cpp's
// kInvisibleOracle table (gen_table.py turns it into C++ rows), which pins the C++ mirror in
// ClaudeInvisibleChars.cpp. Nothing of claude's code is written anywhere: it is read from the install
// at run time and used in-process only.
//
// HOW IT FINDS THE CODE (minified names change every release, so it keys on STRUCTURE):
//   1. the counter literal `{tags:0,bidi:0,zeroWidth:0,selectors:0,other:0}` — inside the cleanup's
//      first statement; the `function <name>(<p>){let <v>=` right before it names the function;
//   2. the fast-path regex `/[^\t\n\x20-\x7e]/` BEFORE that function — the `var` statement holding it
//      starts the segment (all the property regexes + helpers the cleanup uses are declared there);
//   3. `return"other"}` AFTER the function — the end of the removal-class helper, the segment's last
//      function;
//   4. the well-formed helper: the cleanup calls `<helper>(<input>)` right after its fast-path return
//      (`...keptConditional:0};let <r>=<helper>(`) and that helper lives elsewhere in the bundle — it is
//      String.prototype.toWellFormed, so it is injected as such.
// When a release restructures the code, these anchors fail LOUDLY (exit 2 + which anchor); adjust them
// here rather than trusting a partial extraction.
//
// Usage:  node oracle.js [path\to\claude.exe] > oracle.json
//         (default path: %USERPROFILE%\.local\bin\claude.exe — the native installer's launcher)
// Output: a JSON array, one entry per vector: {name, spec, input, text, changed, removedTotal,
//         removedByClass, keptConditional, idempotent} with input/text as pure-ASCII C++-style
//         escapes (\xXXXX per non-ASCII UTF-16 unit) so the file can never carry an invisible char.

"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");

/**
 * Report a broken anchor / input and stop — a partial extraction must never produce a table.
 * @param {string} msg what failed (which anchor, which vector).
 * @returns {never} exits the process with code 2.
 */
function fail(msg) {
  process.stderr.write("[oracle] " + msg + "\n");
  process.exit(2);
}

const claudePath = process.argv[2] || path.join(os.homedir(), ".local", "bin", "claude.exe");
if (!fs.existsSync(claudePath)) {
  fail("claude binary not found: " + claudePath + " (pass its path as the first argument)");
}
const bin = fs.readFileSync(claudePath);

// 1. the function, by its counter literal — the literal also appears elsewhere in the bundle (the
//    submit path's own bookkeeping), so take the first occurrence that OPENS a function body.
const counter = Buffer.from("{tags:0,bidi:0,zeroWidth:0,selectors:0,other:0}");
let fnName = null;
let fnStart = -1;
for (let at = bin.indexOf(counter); at >= 0 && fnName === null; at = bin.indexOf(counter, at + 1)) {
  const headWindow = bin.subarray(Math.max(0, at - 200), at).toString("latin1");
  const head = /function ([\w$]+)\(([\w$]+)\)\{let ([\w$]+)=$/.exec(headWindow);
  if (head) {
    fnName = head[1];
    fnStart = at - (headWindow.length - head.index);
  }
}
if (fnName === null) fail("anchor 1 missing: no `function <name>(<p>){let <v>={tags:0,...}` in the binary");

// 2. the segment start: the `var` statement holding the fast-path regex, before the function
const fastRe = Buffer.from("/[^\\t\\n\\x20-\\x7e]/");
const fastAt = bin.lastIndexOf(fastRe, fnStart);
if (fastAt < 0 || fnStart - fastAt > 20000) fail("anchor 2 missing: the fast-path regex before " + fnName);
const varAt = bin.lastIndexOf(Buffer.from("var "), fastAt);
if (varAt < 0 || fastAt - varAt > 64) fail("anchor 2: no `var` statement opening the fast-path regex");

// 3. the segment end: the removal-class helper's final `return"other"}`
const endMark = Buffer.from('return"other"}');
const endAt = bin.indexOf(endMark, fnStart);
if (endAt < 0 || endAt - fnStart > 20000) fail("anchor 3 missing: `return\"other\"}` after " + fnName);
const segment = bin.subarray(varAt, endAt + endMark.length).toString("utf8");

// 4. the well-formed helper's name
const wf = /keptConditional:0\};let [\w$]+=([\w$]+)\(/.exec(segment);
if (!wf) fail("anchor 4 missing: the well-formed helper call after the fast-path return");
const wfName = wf[1];

let clean;
try {
  // eslint-disable-next-line no-new-func
  clean = new Function(wfName, segment + "\nreturn " + fnName + ";")((s) => s.toWellFormed());
} catch (e) {
  fail("the extracted segment does not evaluate standalone (" + e.message + ") — the bundle's layout changed");
}
process.stderr.write("[oracle] " + path.basename(claudePath) + ": cleanup `" + fnName + "` (" + segment.length +
  " chars, well-formed helper `" + wfName + "`)\n");

/**
 * Decode a vector spec: {HEX} is one code point (a lone-surrogate value becomes one raw UTF-16 unit),
 * everything else is literal printable ASCII — so vectors.json itself stays pure ASCII.
 * @param {string} s the spec.  @returns {string} the test input.  Never throws on a valid spec.
 */
const dec = (s) =>
  s.replace(/\{([0-9A-Fa-f]{1,6})\}/g, (_, h) => {
    const cp = parseInt(h, 16);
    return cp >= 0xd800 && cp <= 0xdfff ? String.fromCharCode(cp) : String.fromCodePoint(cp);
  });
/**
 * Escape a string to pure ASCII: printable ASCII verbatim (\ and " escaped), every other UTF-16 unit
 * as a fixed-width \xXXXX (gen_table.py's unescape is the exact inverse).
 * @param {string} s any string (lone surrogates included).  @returns {string} the escaped form.  Never throws.
 */
const esc = (s) => {
  let o = "";
  for (let k = 0; k < s.length; k++) {
    const u = s.charCodeAt(k);
    const c = s[k];
    if (c === "\\") o += "\\\\";
    else if (c === '"') o += '\\"';
    else if (u >= 0x20 && u <= 0x7e) o += c;
    else o += "\\x" + u.toString(16).toUpperCase().padStart(4, "0");
  }
  return o;
};

const vectors = JSON.parse(fs.readFileSync(path.join(__dirname, "vectors.json"), "utf8"));
const out = vectors.map(([name, spec]) => {
  const input = dec(spec);
  let r;
  try {
    r = clean(input);
  } catch (e) {
    fail("claude's cleanup threw on vector " + name + ": " + e.message + " (a helper it needs lives outside the segment)");
  }
  const again = clean(r.text);
  return {
    name,
    spec,
    input: esc(input),
    text: esc(r.text),
    changed: r.text !== input,
    removedTotal: r.removedTotal,
    removedByClass: r.removedByClass,
    keptConditional: r.keptConditional,
    idempotent: again.text === r.text && again.removedTotal === 0,
  };
});
process.stdout.write(JSON.stringify(out, null, 1));
