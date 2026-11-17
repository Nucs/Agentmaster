/**
 * The prompt box's placeholders and the text behind them, as Claude Code
 * 2.1.310 writes them. Pure: no `$`, so the tests call it directly and Node
 * runs it with `--experimental-strip-types`.
 *
 * The engine keeps a large paste out of the draft: the box shows
 * `[Pasted text #3 +21 lines]`, and the text sits in the draft's pasted
 * contents, which no `$` call reads (`$.prompt.read` answers the draft with
 * the placeholder in it). This module parses those placeholders, normalizes a
 * clipboard text the way the engine normalized the paste, checks that a text
 * fits a placeholder, expands a draft for the clipboard, and rebuilds the text
 * of a placeholder the engine made out of a longer draft (a truncation).
 */

/**
 * The placeholder grammar, the engine's own (`Fp` in 2.1.310's bundle): a
 * kind label, `#` and a positive id, an optional ` +M lines`, and the closing
 * dots of a truncation. Global: callers iterate it with `matchAll`.
 *
 * A text typed or pasted as plain text can spell the same thing; the engine
 * expands it only when its pasted contents hold the id, which is why
 * register.ts classifies each id rather than trusting the spelling.
 */
const PLACEHOLDER = /\[(Pasted text|Image|\.\.\.Truncated text|✦ Team setup guide) #(\d+)(?: \+(\d+) lines)?\.*\]/g

/**
 * The longest one-line paste the engine keeps inline (`p5`, 800 characters
 * on 2.1.310). A `[Pasted text #N]` without a line count only exists for a
 * longer one, which {@link fits} uses to refuse a short clipboard.
 */
export const INLINE_PASTE_MAX = 800

/**
 * What a placeholder stands for:
 * - `paste`: a large paste (`[Pasted text #N +M lines]`);
 * - `guide`: a pasted team setup guide (`[✦ Team setup guide #N +M lines]`),
 *   the trimmed paste;
 * - `truncated`: the middle the engine cut out of a draft over 10,000
 *   characters (`[...Truncated text #N +M lines...]`);
 * - `image`: a pasted image (`[Image #N]`), which no text clipboard carries.
 */
export type PlaceholderKind = 'paste' | 'guide' | 'truncated' | 'image'

/** One placeholder as it stands in a draft. */
export type Placeholder = {
  /** What it stands for. */
  readonly kind: PlaceholderKind
  /** The id the engine keys its pasted contents by; always above 0. */
  readonly id: number
  /** The `+M lines` count (newlines in the text behind it); undefined when the placeholder shows none. */
  readonly lines: number | undefined
  /** The placeholder exactly as written in the draft. */
  readonly match: string
  /** Where it starts in the draft, in UTF-16 code units. */
  readonly index: number
}

/**
 * Maps the engine's label to the placeholder's kind.
 *
 * @param label the label the grammar captured (`Pasted text`, `Image`, ...)
 * @returns the kind
 */
function kindOf(label: string): PlaceholderKind {
  if (label === 'Image') return 'image'
  if (label === '...Truncated text') return 'truncated'
  if (label === 'Pasted text') return 'paste'
  return 'guide'
}

/**
 * Lists the placeholders of a draft in the order they stand, as the engine's
 * own parser (`pu`) lists them: an id of 0 is no placeholder.
 *
 * @param text the draft, as `$.prompt.read` or an event carries it
 * @returns the placeholders, first to last; empty when there are none
 */
export function parsePlaceholders(text: string): Placeholder[] {
  // The cheap test first: this runs on every keystroke the box relays.
  if (!text.includes('[')) return []
  const found: Placeholder[] = []
  for (const match of text.matchAll(PLACEHOLDER)) {
    const id = Number(match[2])
    if (!(id > 0) || match.index === undefined) continue
    found.push({
      kind: kindOf(match[1] ?? ''),
      id,
      lines: match[3] === undefined ? undefined : Number(match[3]),
      match: match[0],
      index: match.index,
    })
  }
  return found
}

/**
 * The placeholders a change of the box brought in: those of `after` whose id
 * `before` did not hold. The engine keys its pasted contents by id alone, so
 * an id already in the draft is no new placeholder however it is spelled.
 *
 * @param before the draft before the change
 * @param after the draft after it
 * @returns the new placeholders, in the order they stand in `after`
 */
export function formedBy(before: string, after: string): Placeholder[] {
  const found = parsePlaceholders(after)
  if (found.length === 0) return []
  const had = new Set(parsePlaceholders(before).map(placeholder => placeholder.id))
  return found.filter(placeholder => !had.has(placeholder.id))
}

/**
 * Whether a placeholder stands for text (a paste, a guide or a truncation),
 * as opposed to an image, which stays in the box at a cut.
 *
 * @param placeholder the placeholder
 * @returns true for the three text kinds
 */
export function isText(placeholder: Placeholder): boolean {
  return placeholder.kind !== 'image'
}

/**
 * Counts line breaks as the engine does for a placeholder's `+M lines`
 * (`$1`): each `\r\n`, lone `\r` and lone `\n` is one.
 *
 * @param text the text
 * @returns how many line breaks it holds
 */
export function newlineCount(text: string): number {
  return (text.match(/\r\n|\r|\n/g) ?? []).length
}

/**
 * The escape sequences a terminal reads as colors and cursor moves: CSI
 * (with the 8-bit introducer too), OSC up to BEL or ST, and the other
 * two-character escapes. The engine strips them from a paste with
 * `Bun.stripANSI`; a clipboard rarely holds any.
 */
const ANSI = /[\u001B\u009B][[\]()#;?]*(?:(?:(?:(?:;[-a-zA-Z\d/#&.:=?%@~_]+)*|[a-zA-Z\d]+(?:;[-a-zA-Z\d/#&.:=?%@~_]*)*)?(?:\u0007|\u001B\\))|(?:(?:\d{1,4}(?:;\d{0,4})*)?[\dA-PR-TZcf-nq-uy=><~]))/g

/**
 * Turns a clipboard text into the text the engine kept for its paste, as its
 * paste handler does on 2.1.310: escape sequences out, every line break a
 * `\n`, every tab four spaces.
 *
 * @param raw the clipboard's text, as the reader printed it
 * @returns the normalized text
 */
export function normalizePaste(raw: string): string {
  return raw.replace(ANSI, '').replace(/\r\n|\r/g, '\n').replaceAll('\t', '    ')
}

/**
 * The text the engine keeps behind a paste placeholder, from the clipboard
 * the paste came from.
 *
 * Into an empty box, a paste that starts with `!` switches the box to shell
 * mode and loses the `!`; a team setup guide is kept trimmed.
 *
 * @param raw the clipboard's text
 * @param kind the new placeholder's kind (`paste` or `guide`)
 * @param wasEmpty whether the box was empty before the paste
 * @returns the text the placeholder stands for, if the clipboard held the paste
 */
export function pasteContent(raw: string, kind: PlaceholderKind, wasEmpty: boolean): string {
  let text = normalizePaste(raw)
  if (wasEmpty && text.startsWith('!')) text = text.slice(1)
  return kind === 'guide' ? text.trim() : text
}

/**
 * Whether a text can be the one behind a placeholder: the same line count,
 * and, for a placeholder that shows none, longer than any paste the engine
 * keeps inline. A clipboard that changed between the paste and its read
 * almost never passes both.
 *
 * @param text the candidate text
 * @param placeholder the placeholder it would stand behind
 * @returns true when it fits
 */
export function fits(text: string, placeholder: Placeholder): boolean {
  const lines = newlineCount(text)
  if (placeholder.lines === undefined) return lines === 0 && text.length > INLINE_PASTE_MAX
  return lines === placeholder.lines
}

/**
 * What a cut makes of a placeholder:
 * - `{ text }`: the text it stands for, which replaces it;
 * - `literal`: plain text that only looks like a placeholder, kept as written;
 * - `image`: an image, left in the box (no text clipboard carries it);
 * - `unread`: a real placeholder whose text was never read;
 * - `unknown`: nobody knows whether it is real.
 */
export type Resolution =
  | { readonly kind: 'text'; readonly text: string }
  | { readonly kind: 'literal' }
  | { readonly kind: 'image' }
  | { readonly kind: 'unread' }
  | { readonly kind: 'unknown' }

/** A draft expanded for the clipboard. */
export type Expansion = {
  /** The text to copy: every placeholder resolved to text replaced by it, images taken out. */
  readonly text: string
  /** The image placeholders taken out, in order: what stays in the box after the cut. */
  readonly images: readonly string[]
  /** The placeholders replaced by their text. */
  readonly expanded: readonly Placeholder[]
  /** The placeholders left as written because their text is unknown (`unread` or `unknown`). */
  readonly unresolved: readonly Placeholder[]
}

/**
 * Expands a draft for the clipboard: each placeholder as `resolve` says.
 * An image placeholder leaves with the one space after it, as the engine
 * drops an image from a queued prompt, or, with none after it (the draft's
 * end, a line break), the one space before it; a `literal`, `unread` or
 * `unknown` one stays as written.
 *
 * @param draft the box's text
 * @param resolve what each placeholder of the draft stands for
 * @returns the text to copy, the images to keep, and what was or was not expanded
 */
export function expand(draft: string, resolve: (placeholder: Placeholder) => Resolution): Expansion {
  const images: string[] = []
  const expanded: Placeholder[] = []
  const unresolved: Placeholder[] = []
  const parts: string[] = []
  let at = 0
  for (const placeholder of parsePlaceholders(draft)) {
    parts.push(draft.slice(at, placeholder.index))
    at = placeholder.index + placeholder.match.length
    const resolution = resolve(placeholder)
    if (resolution.kind === 'text') {
      parts.push(resolution.text)
      expanded.push(placeholder)
    } else if (resolution.kind === 'image') {
      images.push(placeholder.match)
      if (draft[at] === ' ') at += 1
      else {
        const last = parts.length - 1
        const previous = parts[last]
        if (previous !== undefined && previous.endsWith(' ')) parts[last] = previous.slice(0, -1)
      }
    } else {
      parts.push(placeholder.match)
      if (resolution.kind !== 'literal') unresolved.push(placeholder)
    }
  }
  parts.push(draft.slice(at))
  return { text: parts.join(''), images, expanded, unresolved }
}

/**
 * A text with every text placeholder replaced by its content and every image
 * placeholder taken out (no space handling): the form two drafts are compared
 * in when the engine rebuilt one out of the other.
 *
 * @param text the text
 * @param contentOf the content of a text placeholder; undefined when unknown
 * @returns the flat text, or undefined when some text placeholder is unknown
 */
function flatten(text: string, contentOf: (placeholder: Placeholder) => string | undefined): string | undefined {
  const parts: string[] = []
  let at = 0
  for (const placeholder of parsePlaceholders(text)) {
    parts.push(text.slice(at, placeholder.index))
    at = placeholder.index + placeholder.match.length
    if (!isText(placeholder)) continue
    const content = contentOf(placeholder)
    if (content === undefined) return undefined
    parts.push(content)
  }
  parts.push(text.slice(at))
  return parts.join('')
}

/**
 * Rebuilds the text behind a placeholder the engine made out of a longer
 * draft, from that draft and the one it became.
 *
 * Over 10,000 characters the engine keeps the first and last 500 and puts the
 * middle behind `[...Truncated text #N +M lines...]`, with the pastes there
 * expanded into it and the images there moved before it. Compared flat (text
 * placeholders expanded, images out), the new draft is the old one with the
 * middle taken out, so the middle is what lies between the new draft's head
 * and tail.
 *
 * @param source the draft before (or the draft a fill was to make)
 * @param result the draft after, holding `target`
 * @param target the new placeholder, as parsed from `result`
 * @param contentOf the content of every other text placeholder in either draft; undefined when unknown
 * @returns the text behind `target`, or undefined when the drafts do not line up or a placeholder in them is unknown
 */
export function rebuild(
  source: string,
  result: string,
  target: Placeholder,
  contentOf: (placeholder: Placeholder) => string | undefined,
): string | undefined {
  const whole = flatten(source, contentOf)
  const head = flatten(result.slice(0, target.index), contentOf)
  const tail = flatten(result.slice(target.index + target.match.length), contentOf)
  if (whole === undefined || head === undefined || tail === undefined) return undefined
  if (whole.length < head.length + tail.length || !whole.startsWith(head) || !whole.endsWith(tail)) return undefined
  const middle = whole.slice(head.length, whole.length - tail.length)
  return newlineCount(middle) === target.lines ? middle : undefined
}

/**
 * Escapes a text for a regular expression: every character JavaScript, .NET
 * and Perl read as syntax gets a backslash, so the three read it the same.
 *
 * @param text the text
 * @returns the text as a pattern that matches it literally
 */
function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

/**
 * The pattern of the `history.jsonl` line whose prompt the up arrow put in
 * the box as `text`: the line starts `{"display":<text as JSON>,
 * "pastedContents":`, with any id in each placeholder (a recall renumbers
 * them: the entry's `#3` comes back as `#4`), or the same after a `!` (a
 * shell-mode prompt, which the box shows without it).
 *
 * The history writes its lines with JSON.stringify, as this does, so the
 * escapes match; the placeholders hold no character JSON escapes. Anchored at
 * the line's start, and read alike by JavaScript, .NET and Perl.
 *
 * @param text the draft as recalled
 * @returns the pattern's source
 */
export function historyLinePattern(text: string): string {
  const variants = [text, `!${text}`].map(display => {
    const needle = `{"display":${JSON.stringify(display)},"pastedContents":`
    const parts: string[] = []
    let at = 0
    for (const placeholder of parsePlaceholders(needle)) {
      const id = `#${placeholder.id}`
      const cut = placeholder.match.indexOf(id)
      parts.push(
        escapeRegExp(needle.slice(at, placeholder.index)),
        escapeRegExp(placeholder.match.slice(0, cut)),
        '#\\d+',
        escapeRegExp(placeholder.match.slice(cut + id.length)),
      )
      at = placeholder.index + placeholder.match.length
    }
    parts.push(escapeRegExp(needle.slice(at)))
    return parts.join('')
  })
  return `^(?:${variants.join('|')})`
}

/**
 * Pairs the placeholders of a recalled draft with those of the history entry
 * it came from, in order: the recall renumbered them, and the entry's pasted
 * contents are keyed by the old ids.
 *
 * @param recalled the draft as the up arrow put it in the box
 * @param display the entry's `display`
 * @returns the entry's id for each recalled id; undefined when the two drafts' placeholders do not pair up
 */
export function recalledIds(recalled: string, display: string): Map<number, number> | undefined {
  const mine = parsePlaceholders(recalled)
  const theirs = parsePlaceholders(display)
  if (mine.length !== theirs.length) return undefined
  const pairs = new Map<number, number>()
  for (let i = 0; i < mine.length; i += 1) {
    const now = mine[i]
    const then = theirs[i]
    if (now === undefined || then === undefined || now.kind !== then.kind || now.lines !== then.lines) return undefined
    pairs.set(now.id, then.id)
  }
  return pairs
}

/** The base64 alphabet, each character's index its six-bit value. */
const BASE64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

/**
 * Decodes base64 into bytes, ignoring every character outside the alphabet
 * (the padding, line breaks, a byte-order mark a console put first).
 *
 * Written out rather than `atob`: a hooks module has no DOM, and the clipboard
 * readers print base64 so no console code page can touch the text.
 *
 * @param base64 the encoded text
 * @returns the bytes
 */
export function base64Bytes(base64: string): Uint8Array {
  const bytes = new Uint8Array(Math.floor((base64.length * 3) / 4) + 3)
  let length = 0
  let buffer = 0
  let bits = 0
  for (let i = 0; i < base64.length; i += 1) {
    const value = BASE64.indexOf(base64[i] ?? '')
    if (value < 0) continue
    // Only the low `bits` bits are pending: masking keeps the number small.
    buffer = ((buffer << 6) | value) & 0xffff
    bits += 6
    if (bits >= 8) {
      bits -= 8
      bytes[length] = (buffer >> bits) & 0xff
      length += 1
    }
  }
  return bytes.subarray(0, length)
}

/**
 * Encodes a text as base64 of its UTF-8 bytes: the form a request to a
 * PowerShell child goes in on its standard input, which no console code page
 * can then change. A lone surrogate is encoded as U+FFFD.
 *
 * @param text the text
 * @returns the base64, padded with `=`
 */
export function utf8Base64(text: string): string {
  const bytes: number[] = []
  for (const char of text) {
    let point = char.codePointAt(0) ?? 0xfffd
    if (point >= 0xd800 && point < 0xe000) point = 0xfffd
    if (point < 0x80) bytes.push(point)
    else if (point < 0x800) bytes.push(0xc0 | (point >> 6), 0x80 | (point & 0x3f))
    else if (point < 0x10000) bytes.push(0xe0 | (point >> 12), 0x80 | ((point >> 6) & 0x3f), 0x80 | (point & 0x3f))
    else bytes.push(0xf0 | (point >> 18), 0x80 | ((point >> 12) & 0x3f), 0x80 | ((point >> 6) & 0x3f), 0x80 | (point & 0x3f))
  }
  const out: string[] = []
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i] ?? 0
    const b = bytes[i + 1]
    const c = bytes[i + 2]
    out.push(
      BASE64[a >> 2] ?? '',
      BASE64[((a & 3) << 4) | ((b ?? 0) >> 4)] ?? '',
      b === undefined ? '=' : (BASE64[((b & 15) << 2) | ((c ?? 0) >> 6)] ?? ''),
      c === undefined ? '=' : (BASE64[c & 63] ?? ''),
    )
  }
  return out.join('')
}

/**
 * Decodes UTF-8 bytes into a string; a malformed sequence becomes U+FFFD, as
 * a lenient decoder does.
 *
 * @param bytes the bytes
 * @returns the text
 */
export function utf8Text(bytes: Uint8Array): string {
  const chunks: string[] = []
  let units: number[] = []
  /**
   * Moves the pending code units into `chunks` as one string and starts a
   * new batch, so no `fromCharCode` call takes more arguments than an engine
   * allows (a spread of millions would throw a RangeError).
   */
  const flush = (): void => {
    chunks.push(String.fromCharCode(...units))
    units = []
  }
  let i = 0
  while (i < bytes.length) {
    const first = bytes[i] ?? 0
    let need = 0
    let point = 0
    if (first < 0x80) point = first
    else if (first >= 0xc2 && first < 0xe0) (need = 1), (point = first & 0x1f)
    else if (first >= 0xe0 && first < 0xf0) (need = 2), (point = first & 0x0f)
    else if (first >= 0xf0 && first < 0xf5) (need = 3), (point = first & 0x07)
    else point = 0xfffd
    let ok = true
    for (let k = 1; k <= need; k += 1) {
      const next = bytes[i + k]
      if (next === undefined || (next & 0xc0) !== 0x80) {
        ok = false
        break
      }
      point = (point << 6) | (next & 0x3f)
    }
    // Overlong forms and surrogates are malformed too: one U+FFFD, and the
    // next byte read afresh.
    if (!ok || (need === 2 && (point < 0x800 || (point >= 0xd800 && point < 0xe000))) || (need === 3 && (point < 0x10000 || point > 0x10ffff))) {
      units.push(0xfffd)
      i += 1
    } else {
      if (point > 0xffff) {
        point -= 0x10000
        units.push(0xd800 + (point >> 10), 0xdc00 + (point & 0x3ff))
      } else units.push(point)
      i += need + 1
    }
    // fromCharCode takes its units as arguments: a bounded batch each.
    if (units.length >= 8192) flush()
  }
  flush()
  return chunks.join('')
}

/**
 * Groups a count's digits by thousands for a toast (`12,345`).
 *
 * @param count a whole number
 * @returns the number as text
 */
export function grouped(count: number): string {
  return String(count).replace(/\B(?=(\d{3})+(?!\d))/g, ',')
}
