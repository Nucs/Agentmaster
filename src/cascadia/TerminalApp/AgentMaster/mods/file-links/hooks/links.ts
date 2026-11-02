/**
 * file-links' pure half: it finds the spans of a reply's markdown that may
 * name a file, and writes back, as `file:///` links, the ones the resolver in
 * register.ts placed. Nothing here calls `$`, so the tests check it directly.
 *
 * The scanner is no full markdown parser. It knows the constructs a reply puts
 * around paths: fenced code blocks (never touched), code spans (one mention
 * each, backticks kept in the link text), links, images and autolinks
 * (skipped whole, so a link the model wrote stays as written), backslash
 * escapes, and bare tokens split at whitespace with their wrapping
 * punctuation trimmed. What it misses stays as written: the cost of a miss is
 * a span without a link, never a changed text.
 */

/**
 * One span of a reply's markdown that may name a file: what a link would
 * replace, and what the resolver looks up.
 */
export type Mention = {
  /** Index in the text where the span starts (inclusive). */
  start: number
  /** Index in the text where the span ends (exclusive). */
  end: number
  /**
   * The span as written: a code span with its backticks (drawn as code inside
   * the link), or a bare token less its wrapping punctuation.
   */
  raw: string
  /**
   * The path the span names, its line suffix (`:12`, `#L12`) cut off: a file
   * URI cannot carry a line, so the suffix stays in the link text alone.
   */
  path: string
  /** How the resolver places `path`. */
  kind: PathKind
  /** True for a code span; false for a bare token, whose label is escaped. */
  isCode: boolean
}

/**
 * How a written path is placed, which picks the resolver's lookups.
 *
 * `absolute` stands alone (`C:\x`, `/c/x`, `~/x`); `relative` has a folder
 * part, tried under the session's folders and matched as a path suffix
 * (`hooks/x.ts`); `name` is a file name alone, found by name (`register.ts`),
 * the kind most open to picking the wrong file of several with that name.
 */
export type PathKind = 'absolute' | 'relative' | 'name'

/**
 * Which file system the session runs on, which decides what an absolute path
 * is: `windows` takes `C:\x`, `C:/x` and the Git Bash form `/c/x`; `posix`
 * (macOS, Linux) takes `/x`. Both take `~/x`.
 */
export type Platform = 'windows' | 'posix'

/**
 * The environment variables Claude Code reads to decide whether the terminal
 * draws hyperlinks (2.1.292: its `vb()` and the bundled `supports-hyperlinks`):
 * each as `$.env.get` answers it, undefined when unset.
 */
export type HyperlinkEnv = {
  FORCE_HYPERLINK?: string
  NETLIFY?: string
  WT_SESSION?: string
  TERM_PROGRAM?: string
  TERM_PROGRAM_VERSION?: string
  TERMINAL_EMULATOR?: string
  TMUX?: string
  LC_TERMINAL?: string
  TERM?: string
}

/**
 * The environment variables Claude Code 2.1.292 reads for its temp root:
 * `CLAUDE_CODE_TMPDIR` first, then the ones `os.tmpdir()` reads (`TEMP`,
 * `TMP`, `SystemRoot`/`windir` on Windows; `TMPDIR`, `TMP`, `TEMP` on POSIX),
 * each as `$.env.get` answers it, undefined when unset.
 */
export type TempEnv = {
  CLAUDE_CODE_TMPDIR?: string
  TEMP?: string
  TMP?: string
  TMPDIR?: string
  SystemRoot?: string
  windir?: string
}

/** Whether the terminal draws hyperlinks, and the fact that decided it (for `/file-links`). */
export type HyperlinkSupport = {
  /** True when the engine draws a markdown link as an OSC 8 hyperlink; false when it draws `text (url)`. */
  isOn: boolean
  /** The variable that decided, `WT_SESSION` or `TERM_PROGRAM=Apple_Terminal`; for a person to read. */
  why: string
}

/** A path and the way it is placed, as {@link pathOf} reads a span. */
export type WrittenPath = {
  /** The path, line suffix cut off. */
  path: string
  /** How it is placed. */
  kind: PathKind
}

/**
 * Line suffixes a reply hangs on a path: `:12`, `:12:5`, `:12-20`, `#L12`,
 * `#L12-L20`. Kept in the link text, cut from the lookup.
 */
const LINE_SUFFIX = /(?::\d+(?:[-:]\d+)*|#L\d+(?:-L?\d+)?)$/

/** A Windows drive path: `C:\x` or `C:/x`. */
const DRIVE_PATH = /^[A-Za-z]:[\\/]/

/** A bare Windows drive, `C:` (a temp root may be one). */
const DRIVE_ROOT = /^[A-Za-z]:$/

/** A Git Bash drive path, `/c/x` (or `/c` alone); group 1 is the drive letter. */
const MSYS_PATH = /^\/([A-Za-z])(?=\/|$)/

/** A path under the home folder: `~`, `~/x` or `~\x` (never `~user`). */
const HOME_PATH = /^~(?=[\\/]|$)/

/**
 * A POSIX absolute path: `/`, then a segment (`/Users/me/x.md`, `/etc/hosts`);
 * never `/` alone (a slash in prose) or `//x` (a network or URL spelling).
 */
const POSIX_PATH = /^\/(?=[^\/\s])/

/**
 * The longest project folder name Claude Code keeps whole (2.1.292 `$ce`);
 * a longer one is cut here and given a hash (see {@link projectKeyOf}).
 */
const MAX_PROJECT_KEY = 200

/**
 * Terminals Claude Code itself names as drawing hyperlinks, whatever the
 * library says (2.1.292 `vb()`: `TERM_PROGRAM` or `LC_TERMINAL` one of these).
 */
const HYPERLINK_TERMINALS = new Set(['ghostty', 'Hyper', 'kitty', 'alacritty', 'iTerm.app', 'iTerm2', 'WarpTerminal'])

/**
 * One segment of a relative path as replies write it: letters of any script,
 * digits, and the punctuation file names use here. No spaces: a bare token has
 * none, and a code span with spaces and no drive is a command more often than
 * a relative path.
 */
const SEGMENT = /^[\p{L}\p{N}_@+.\-]+$/u

/**
 * A file name standing alone: a stem, then an extension that starts with a
 * letter, so versions (`2.1.292`) and decimals (`3.5`) are never looked up;
 * or a dotfile (`.gitignore`). Dotted identifiers (`ui.render`) still pass and
 * cost a lookup that finds nothing.
 */
const FILE_NAME = /^(?:[\p{L}\p{N}_@+\-][\p{L}\p{N}_@+.\-]*\.\p{L}[\p{L}\p{N}_\-]{0,15}|\.\p{L}[\p{L}\p{N}_.\-]*)$/u

/** Characters no file name written in a reply holds: shell and glob syntax. */
const NEVER_IN_PATH = /[<>|"*?\n\t\r]/

/** A URL or URI: a link of its own (or no file), never looked up. */
const URL_LIKE = /^(?:[A-Za-z][A-Za-z0-9+.-]+:\/\/|(?:mailto|data|javascript|vscode|file):)/i

/** The longest span looked up; anything longer is prose or data, not a path. */
const MAX_PATH_CHARS = 400

/**
 * The most mentions one text yields: bounds the lookups a long reply costs.
 * The rest of the text keeps its spans as written.
 */
export const MAX_MENTIONS = 200

/** Punctuation that opens a bare token around a path: `(see`, `"x`. */
const LEADING = new Set(['(', '[', '{', '"', "'", '\u201C', '\u2018', '\u00AB', '<', '|'])

/** Punctuation that closes a bare token after a path: `x),` `x.` `x:`. */
const TRAILING = new Set([')', ']', '}', '"', "'", '\u201D', '\u2019', '\u00BB', '>', '|', ',', '.', ';', ':', '!', '?', '\u2026'])

/**
 * Emphasis runs that wrap a bare token on both sides (`**x**`, `_x_`,
 * `~~x~~`), longest first so `**` is never read as two `*`.
 */
const WRAPPERS = ['**', '__', '~~', '*', '_'] as const

/** An opening fence line: any indent (fences nest in list items), then 3+ backticks or tildes. */
const FENCE_OPEN = /^\s*(`{3,}|~{3,})/

/** A closing fence line: the fence run alone on its line. */
const FENCE_CLOSE = /^\s*(`{3,}|~{3,})\s*$/

/** ASCII punctuation: what a backslash escapes in markdown. */
const ESCAPABLE = /[!-/:-@[-`{-~]/

/**
 * Reads a span's text as a path worth looking up, and how to place it.
 *
 * @param written a code span's content, or a bare token less its wrapping
 *   punctuation
 * @param allowsSpaces true for a code span, where an absolute path may hold
 *   spaces (`C:\Program Files\Git`); a relative path never does
 * @returns the path (line suffix cut off) and its kind, or undefined when the
 *   text is no path: a URL, a flag, a glob, a command, a sentence, a version
 */
export function pathOf(written: string, allowsSpaces: boolean): WrittenPath | undefined {
  if (written.length === 0 || written.length > MAX_PATH_CHARS) return undefined
  if (URL_LIKE.test(written)) return undefined
  const path = written.replace(LINE_SUFFIX, '')
  if (path.length === 0 || NEVER_IN_PATH.test(path)) return undefined
  const hasSpace = /\s/.test(path)
  if (hasSpace && !allowsSpaces) return undefined
  // Leading and trailing spaces inside a code span are padding, not the path's.
  if (hasSpace && path !== path.trim()) return undefined
  // Every absolute form of either platform: absoluteOf, which knows the
  // session's platform, refuses the other's (`/x` on Windows unless it is the
  // Git Bash `/c/x`, `C:\x` on macOS) before any lookup is spent on it.
  if (DRIVE_PATH.test(path) || MSYS_PATH.test(path) || HOME_PATH.test(path) || POSIX_PATH.test(path)) {
    return { path, kind: 'absolute' }
  }
  if (hasSpace) return undefined
  const segments = path.split(/[\\/]/)
  // A trailing separator marks a folder (`mods/chat-view/`); it adds no segment.
  if (segments.length > 1 && segments.at(-1) === '') segments.pop()
  if (segments.length >= 2) {
    // A leading separator left here is a `//x` network or URL spelling (a
    // POSIX `/x` was taken above): its empty first segment fails SEGMENT, so
    // it is never looked up.
    return segments.every(segment => SEGMENT.test(segment)) ? { path, kind: 'relative' } : undefined
  }
  return FILE_NAME.test(path) ? { path, kind: 'name' } : undefined
}

/**
 * Finds every span of a reply's markdown that may name a file, in text order.
 *
 * Fenced code blocks are skipped whole (a link drawn there would show as raw
 * markdown), as are links, images and autolinks (the model's own links keep
 * their targets). Stops after {@link MAX_MENTIONS}.
 *
 * @param text the reply's markdown, as the `AssistantMessage` site hands it
 * @returns the mentions, each with its span, path and kind
 */
export function scan(text: string): Mention[] {
  const mentions: Mention[] = []
  // The marker run of the fence the walk is inside, or undefined outside one.
  let fence: string | undefined
  let offset = 0
  for (const line of text.split('\n')) {
    if (fence !== undefined) {
      const close = FENCE_CLOSE.exec(line)?.[1]
      // Only a run of the opening character, at least as long, ends a fence.
      if (close !== undefined && close[0] === fence[0] && close.length >= fence.length) fence = undefined
    } else {
      const open = FENCE_OPEN.exec(line)?.[1]
      if (open !== undefined) fence = open
      else scanLine(line, offset, mentions)
    }
    if (mentions.length >= MAX_MENTIONS) return mentions.slice(0, MAX_MENTIONS)
    offset += line.length + 1
  }
  return mentions
}

/**
 * Finds the mentions of one line outside a fence: code spans, then bare
 * tokens in the plain stretches between the constructs it skips.
 *
 * @param line the line, without its newline
 * @param base the line's index in the whole text, added to every span
 * @param out where the mentions go, in text order
 */
function scanLine(line: string, base: number, out: Mention[]): void {
  // Where the current plain stretch (text outside any construct) began.
  let plainStart = 0
  let i = 0
  while (i < line.length) {
    const c = line[i]
    if (c === '\\' && i + 1 < line.length && ESCAPABLE.test(line[i + 1]!)) {
      // An escaped character never opens a construct; both stay plain text.
      i += 2
      continue
    }
    if (c === '`') {
      const run = runLengthAt(line, i, '`')
      const close = closingRunOf(line, i + run, run)
      // A run with no closing run of the same length is literal backticks.
      if (close === -1) {
        i += run
        continue
      }
      bareMentions(line, plainStart, i, base, out)
      const content = line.slice(i + run, close)
      const written = pathOf(content.trim(), true)
      if (written !== undefined) {
        out.push({ start: base + i, end: base + close + run, raw: line.slice(i, close + run), ...written, isCode: true })
      }
      i = close + run
      plainStart = i
      continue
    }
    if (c === '[' || (c === '!' && line[i + 1] === '[')) {
      const end = linkEndOf(line, c === '!' ? i + 1 : i)
      if (end !== -1) {
        bareMentions(line, plainStart, i, base, out)
        i = end
        plainStart = i
        continue
      }
    }
    if (c === '<') {
      const close = line.indexOf('>', i + 1)
      // `<https://x>`, `<file:///x>`, `<br>`: an autolink or a tag, no space inside.
      if (close !== -1 && close > i + 1 && !/\s/.test(line.slice(i + 1, close))) {
        bareMentions(line, plainStart, i, base, out)
        i = close + 1
        plainStart = i
        continue
      }
    }
    i++
  }
  bareMentions(line, plainStart, line.length, base, out)
}

/**
 * Adds the bare-token mentions of one plain stretch of a line: each run of
 * non-space characters, less its wrapping punctuation, that reads as a path.
 *
 * @param line the line
 * @param from where the stretch starts in the line
 * @param to where it ends (exclusive)
 * @param base the line's index in the whole text
 * @param out where the mentions go, in text order
 */
function bareMentions(line: string, from: number, to: number, base: number, out: Mention[]): void {
  if (to <= from) return
  const stretch = line.slice(from, to)
  for (const match of stretch.matchAll(/\S+/g)) {
    const token = match[0]
    const { head, tail } = trimmedBounds(token)
    if (tail <= head) continue
    const text = token.slice(head, tail)
    const written = pathOf(text, false)
    if (written === undefined) continue
    const start = base + from + match.index + head
    out.push({ start, end: start + text.length, raw: text, ...written, isCode: false })
  }
}

/**
 * Finds the part of a bare token inside its wrapping punctuation and emphasis:
 * `(**hooks/x.ts**),` keeps `hooks/x.ts`.
 *
 * @param token a run of non-space characters
 * @returns the kept part's bounds in the token, `head` inclusive and `tail`
 *   exclusive; equal when nothing is left
 */
export function trimmedBounds(token: string): { head: number; tail: number } {
  let head = 0
  let tail = token.length
  // Peel layer by layer until a pass removes nothing: `(**x**),` needs the
  // punctuation off before the emphasis pair shows.
  for (;;) {
    const headBefore = head
    const tailBefore = tail
    // Emphasis first, as a pair, so `**x**` loses both runs together and a
    // name that only starts with one (`__init__.py`) keeps it.
    for (const wrapper of WRAPPERS) {
      if (tail - head > 2 * wrapper.length && token.startsWith(wrapper, head) && token.endsWith(wrapper, tail)) {
        head += wrapper.length
        tail -= wrapper.length
        break
      }
    }
    while (head < tail && LEADING.has(token[head]!)) head++
    while (tail > head && TRAILING.has(token[tail - 1]!)) tail--
    if (head === headBefore && tail === tailBefore) return { head, tail }
  }
}

/**
 * Counts a run of one character.
 *
 * @param line the line
 * @param at where the run starts
 * @param char the character
 * @returns how many of it follow from `at`
 */
function runLengthAt(line: string, at: number, char: string): number {
  let length = 0
  while (line[at + length] === char) length++
  return length
}

/**
 * Finds the backtick run that closes a code span: the next run of exactly the
 * opening run's length (a longer or shorter run is the span's content).
 *
 * @param line the line
 * @param from where the span's content starts
 * @param length the opening run's length
 * @returns the closing run's index, or -1 when the line holds none
 */
function closingRunOf(line: string, from: number, length: number): number {
  let i = from
  while (i < line.length) {
    if (line[i] !== '`') {
      i++
      continue
    }
    const run = runLengthAt(line, i, '`')
    if (run === length) return i
    i += run
  }
  return -1
}

/**
 * Finds where a link, an image's link part or a reference link that opens at
 * `open` ends, so its text and destination are skipped whole.
 *
 * @param line the line
 * @param open the index of the `[`
 * @returns the index after the construct, or -1 when the bracket opens none
 *   (a lone `[` is plain text)
 */
function linkEndOf(line: string, open: number): number {
  const close = closingOf(line, open, '[', ']')
  if (close === -1) return -1
  // Undefined when the `]` ends the line: neither test below holds then, so
  // a bracket pair at the line's end is plain text, as markdown reads it.
  const after = line[close + 1]
  if (after === '(') {
    const end = closingOf(line, close + 1, '(', ')')
    return end === -1 ? -1 : end + 1
  }
  if (after === '[') {
    const end = closingOf(line, close + 1, '[', ']')
    return end === -1 ? -1 : end + 1
  }
  return -1
}

/**
 * Finds the bracket that closes the one at `open`, counting nested pairs and
 * passing over escapes and code spans (a `]` inside backticks closes nothing).
 *
 * @param line the line
 * @param open the index of the opening bracket
 * @param opening the opening character
 * @param closing the closing character
 * @returns the closing bracket's index, or -1 when the line holds none
 */
function closingOf(line: string, open: number, opening: string, closing: string): number {
  let depth = 0
  let i = open
  while (i < line.length) {
    const c = line[i]
    if (c === '\\') {
      i += 2
      continue
    }
    if (c === '`') {
      const run = runLengthAt(line, i, '`')
      const end = closingRunOf(line, i + run, run)
      i = end === -1 ? i + run : end + run
      continue
    }
    if (c === opening) depth++
    else if (c === closing && --depth === 0) return i
    i++
  }
  return -1
}

/**
 * Writes the placed mentions back into the text as links, every other
 * character as it was.
 *
 * @param text the reply's markdown
 * @param mentions what {@link scan} found in it, in text order
 * @param targetOf the absolute path a mention names, or undefined to leave
 *   that span as written
 * @returns the text with each placed span as `[span](file:///...)`; the same
 *   string when nothing was placed
 */
export function linkify(text: string, mentions: readonly Mention[], targetOf: (mention: Mention) => string | undefined): string {
  let out = ''
  let last = 0
  let isChanged = false
  for (const mention of mentions) {
    const target = targetOf(mention)
    if (target === undefined) continue
    out += `${text.slice(last, mention.start)}[${labelOf(mention)}](${fileUri(target)})`
    last = mention.end
    isChanged = true
  }
  return isChanged ? out + text.slice(last) : text
}

/**
 * Spells a mention as link text.
 *
 * @param mention the mention
 * @returns a code span as written (drawn as code inside the link, its content
 *   never parsed); a bare token with markdown's own punctuation escaped, so
 *   the link shows the name as written: `C:\Users\me\.claude` keeps the
 *   backslash markdown would eat, `__init__.py` draws no bold
 */
function labelOf(mention: Mention): string {
  if (mention.isCode) return mention.raw
  return mention.raw.replace(/[\\[\]`*_~]/g, char => `\\${char}`)
}

/**
 * Spells an absolute path as a `file:` URI, the form a CLAUDE.md file-link
 * rule asks for: forward slashes, an upper-case drive, every segment
 * percent-encoded.
 *
 * @param path an absolute path, `C:\x`, `C:/x` or `/x`
 * @returns `file:///C:/x` (or `file:///x`); spaces, `#`, `?`, `%`,
 *   parentheses and non-ASCII characters encoded, so the markdown link
 *   destination holds and the terminal opens the file, not a fragment
 */
export function fileUri(path: string): string {
  const slashed = path.replace(/\\/g, '/')
  const drive = /^([A-Za-z]):(?=\/|$)/.exec(slashed)
  const rest = drive === null ? slashed : slashed.slice(2)
  const encoded = rest.split('/').map(encodeSegment).join('/')
  if (drive === null) return `file://${encoded.startsWith('/') ? '' : '/'}${encoded}`
  return `file:///${drive[1]!.toUpperCase()}:${encoded}`
}

/**
 * Percent-encodes one path segment for a URI.
 *
 * @param segment a file or folder name
 * @returns it with every character a URI segment may not hold encoded,
 *   `encodeURIComponent`'s set plus `!'()*`, which markdown or a terminal
 *   could take as syntax
 */
function encodeSegment(segment: string): string {
  return encodeURIComponent(segment).replace(/[!'()*]/g, char => `%${char.charCodeAt(0).toString(16).toUpperCase()}`)
}

/**
 * Spells an absolute path as the session's file system takes it.
 *
 * @param path an absolute path as a reply writes it: `~/x` on both platforms;
 *   `C:\x`, `C:/x` or the Git Bash `/c/x` on Windows; `/x` on POSIX, where
 *   `/c/x` is the folder `c` under the root, not a drive
 * @param home the home folder, for `~`; undefined when it is not known
 * @param platform the session's platform (see {@link platformOf})
 * @returns the path normalized (see {@link normalize}); undefined when it is
 *   no absolute path of this platform (`/etc/x` on Windows, `C:\x` on macOS),
 *   or names `~` while the home folder is not known
 */
export function absoluteOf(path: string, home: string | undefined, platform: Platform): string | undefined {
  if (HOME_PATH.test(path)) return home === undefined ? undefined : normalize(`${home}/${path.slice(1)}`)
  if (platform === 'posix') return POSIX_PATH.test(path) ? normalize(path) : undefined
  const msys = MSYS_PATH.exec(path)
  if (msys !== null) return normalize(`${msys[1]!.toUpperCase()}:${path.slice(2)}`)
  return DRIVE_PATH.test(path) ? normalize(path) : undefined
}

/**
 * Tells the session's platform from one of its absolute folders.
 *
 * The plugin API names no OS, but every Windows folder starts with a drive
 * (`C:\`) and every macOS or Linux folder with `/`.
 *
 * @param folder the working folder or the project root, as the engine gives
 *   it; undefined when neither could be read
 * @returns `windows` for a drive path; `posix` otherwise, the unknown case
 *   included (a POSIX reading never turns `/c/x` into a drive)
 */
export function platformOf(folder: string | undefined): Platform {
  return folder !== undefined && /^[A-Za-z]:[\\/]?/.test(folder) ? 'windows' : 'posix'
}

/**
 * Names a project's temp folder as Claude Code 2.1.292 does (its `uI`):
 * every character outside `[a-zA-Z0-9]` becomes `-`, and a name longer than
 * 200 characters is cut to 200 and given `-` plus the base-36 value of the
 * absolute Java string hash of the path as written.
 *
 * Footgun: Claude Code's own layout, read from one build; a later build may
 * name the folder otherwise, and then no scratchpad is found (a full path
 * into it still links). The hash covers the path's exact spelling, so pass
 * the path as the engine gives it, not a normalized one.
 *
 * @param path the session's original working folder, as the engine spells it
 *   (`C:\Users\me\proj`, `/Users/me/proj`)
 * @returns the folder name: `C--Users-me-proj`, `-Users-me-proj`
 */
export function projectKeyOf(path: string): string {
  const name = path.replace(/[^a-zA-Z0-9]/g, '-')
  if (name.length <= MAX_PROJECT_KEY) return name
  return `${name.slice(0, MAX_PROJECT_KEY)}-${Math.abs(javaHashOf(path)).toString(36)}`
}

/**
 * Hashes a text as Java's `String.hashCode` does, the hash Claude Code names
 * long project folders with (2.1.292 `kre`).
 *
 * @param text the text
 * @returns the 32-bit signed hash: each character `h = h * 31 + code`, wrapped
 */
function javaHashOf(text: string): number {
  let hash = 0
  for (let i = 0; i < text.length; i++) hash = ((hash << 5) - hash + text.charCodeAt(i)) | 0
  return hash
}

/**
 * Finds Claude Code's own temp folder, under which each session's scratchpad
 * lives (2.1.292: `join(CLAUDE_CODE_TMPDIR || os.tmpdir(), "claude")`).
 *
 * @param env the variables, as `$.env.get` answers them
 * @param platform which `os.tmpdir()` rule applies: on Windows `TEMP`, `TMP`,
 *   then `<SystemRoot or windir>\temp`; on POSIX `TMPDIR`, `TMP`, `TEMP`, then
 *   `/tmp`
 * @returns the folder, normalized but not resolved (the engine takes its real
 *   path; on macOS `/var/...` resolves to `/private/var/...`); undefined when
 *   no variable names a temp root, or the one that does is not a local
 *   absolute folder. A UNC root (`\\server\share`) counts as none on purpose:
 *   `$.fs` refuses network locations, so no scratchpad there could be checked.
 */
export function claudeTempOf(env: TempEnv, platform: Platform): string | undefined {
  // Truthy, not defined: Node's os.tmpdir(), which Claude Code calls, skips
  // an empty TEMP or TMPDIR the same way.
  const root = env.CLAUDE_CODE_TMPDIR || systemTempOf(env, platform)
  if (root === undefined) return undefined
  const isAbsolute = platform === 'windows' ? DRIVE_PATH.test(root) || DRIVE_ROOT.test(root) : root.startsWith('/')
  return isAbsolute ? joinPath(normalize(root), 'claude') : undefined
}

/**
 * Reads the system temp folder as Node's `os.tmpdir()` does.
 *
 * @param env the variables
 * @param platform the session's platform
 * @returns the temp folder as written; undefined on Windows with none of its
 *   variables set
 */
function systemTempOf(env: TempEnv, platform: Platform): string | undefined {
  if (platform === 'posix') return env.TMPDIR || env.TMP || env.TEMP || '/tmp'
  if (env.TEMP || env.TMP) return env.TEMP || env.TMP
  const system = env.SystemRoot || env.windir
  return system ? `${system}\\temp` : undefined
}

/**
 * Spells a session's scratchpad folder as Claude Code 2.1.292 lays it out:
 * `<claude temp>/<project key>/<session id>/scratchpad`.
 *
 * @param claudeTemp Claude Code's temp folder, normalized (see {@link claudeTempOf})
 * @param projectPath the session's original working folder, as the engine
 *   spells it (see {@link projectKeyOf})
 * @param sessionId the session's id (`$.session.id()`, its transcript's name)
 * @returns the scratchpad folder, normalized
 */
export function scratchpadPathOf(claudeTemp: string, projectPath: string, sessionId: string): string {
  return joinPath(claudeTemp, `${projectKeyOf(projectPath)}/${sessionId}/scratchpad`)
}

/**
 * Takes a normalized folder's parent, for the walk up to a repository's top.
 *
 * @param folder an absolute normalized folder (`C:/a/b`, `/a/b`)
 * @returns `C:/a` (or `/a`); `C:/` for `C:/a` and `/` for `/a`; undefined at
 *   a root, where the walk stops
 */
export function parentOf(folder: string): string | undefined {
  const cut = folder.lastIndexOf('/')
  // `C:/` and `/` end with their only separator: nothing lies above a root.
  if (cut === folder.length - 1) return undefined
  const parent = folder.slice(0, cut)
  return parent === '' || /^[A-Za-z]:$/.test(parent) ? `${parent}/` : parent
}

/**
 * Decides whether the terminal draws hyperlinks, as Claude Code 2.1.292 does
 * for a local session (its `vb()` over the bundled `supports-hyperlinks`).
 *
 * Where it does not, the engine draws a markdown link as `text (url)`, so a
 * link the mod adds there costs a long URL after every name: the mod then
 * leaves the reply as written. `FORCE_HYPERLINK=1` turns the engine's
 * hyperlinks on and `FORCE_HYPERLINK=0` off, here as in the engine. Footgun:
 * this mirrors one build's rule; a later build may name more terminals, and
 * a session a remote client attached to decides by that client's own report,
 * which no mod can read.
 *
 * @param env the variables, as `$.env.get` answers them
 * @param platform the session's platform: the library's per-terminal checks
 *   run on macOS and Linux only (its Windows build ends at `WT_SESSION`)
 * @returns whether the terminal draws hyperlinks, and the fact that decided
 */
export function hyperlinksOf(env: HyperlinkEnv, platform: Platform): HyperlinkSupport {
  const library = libraryHyperlinksOf(env, platform)
  // The engine takes a FORCE_HYPERLINK that is set at all (its
  // `"FORCE_HYPERLINK" in env`, an empty value included) as the library's
  // answer alone, before its own list; the library itself reads an empty
  // value as unset (`if (force)`). Both checks are the engine's, so they
  // differ here on purpose: unified, the mod would link in a terminal where
  // the engine draws `text (url)` (FORCE_HYPERLINK="" in kitty, say).
  if (env.FORCE_HYPERLINK !== undefined) return library
  if (library.isOn) return library
  const program = env.TERM_PROGRAM
  if (program !== undefined && HYPERLINK_TERMINALS.has(program)) return { isOn: true, why: `TERM_PROGRAM=${program}` }
  if (env.TERMINAL_EMULATOR === 'JetBrains-JediTerm') return { isOn: true, why: 'TERMINAL_EMULATOR=JetBrains-JediTerm' }
  if (env.WT_SESSION && program !== 'tmux' && !env.TMUX) return { isOn: true, why: 'WT_SESSION' }
  if (program === 'tmux') {
    // vb()'s own parse (split on `.`, then parseInt each part), not the
    // library's versionOf: the two read a build number such as `3402`
    // differently, and each check must read the version as the engine does.
    const [major = NaN, minor = NaN] = (env.TERM_PROGRAM_VERSION ?? '').split('.').map(part => parseInt(part, 10))
    if (major > 3 || (major === 3 && minor >= 4)) return { isOn: true, why: `tmux ${env.TERM_PROGRAM_VERSION}` }
  }
  if (env.LC_TERMINAL !== undefined && HYPERLINK_TERMINALS.has(env.LC_TERMINAL)) {
    return { isOn: true, why: `LC_TERMINAL=${env.LC_TERMINAL}` }
  }
  if (env.TERM?.includes('kitty')) return { isOn: true, why: `TERM=${env.TERM}` }
  if (program !== undefined) return { isOn: false, why: `TERM_PROGRAM=${program}` }
  // On Windows only Windows Terminal (and hosts like Agentmaster that set its
  // variable) turns hyperlinks on: name the variable a person can check.
  return { isOn: false, why: platform === 'windows' ? 'not Windows Terminal (WT_SESSION unset)' : 'no hyperlink terminal named' }
}

/**
 * The bundled `supports-hyperlinks` check, for an interactive session (a color
 * TTY, which its colour and TTY checks then pass).
 *
 * @param env the variables, as `$.env.get` answers them
 * @param platform the session's platform: the Windows build stops after
 *   `WT_SESSION`, where the macOS build goes on to the terminals it knows
 * @returns the library's answer, and the fact that decided
 */
function libraryHyperlinksOf(env: HyperlinkEnv, platform: Platform): HyperlinkSupport {
  const force = env.FORCE_HYPERLINK
  if (force) return { isOn: !(force.length > 0 && parseInt(force, 10) === 0), why: `FORCE_HYPERLINK=${force}` }
  if (env.NETLIFY) return { isOn: true, why: 'NETLIFY' }
  if (env.WT_SESSION !== undefined) return { isOn: true, why: 'WT_SESSION' }
  if (platform === 'windows') return { isOn: false, why: 'no Windows Terminal (WT_SESSION unset)' }
  const program = env.TERM_PROGRAM
  if (program !== undefined) {
    const version = versionOf(env.TERM_PROGRAM_VERSION ?? '')
    const why = `TERM_PROGRAM=${program}`
    switch (program) {
      case 'iTerm.app':
        return { isOn: version.major === 3 ? version.minor >= 1 : version.major > 3, why }
      case 'WezTerm':
        return { isOn: version.major >= 20200620, why }
      case 'vscode':
        return { isOn: version.major > 1 || (version.major === 1 && version.minor >= 72), why }
      case 'ghostty':
        return { isOn: true, why }
    }
  }
  if (env.TERM === 'alacritty') return { isOn: true, why: 'TERM=alacritty' }
  return { isOn: false, why: program === undefined ? 'no hyperlink terminal named' : `TERM_PROGRAM=${program}` }
}

/**
 * Reads a terminal's version as `supports-hyperlinks` does.
 *
 * @param written `TERM_PROGRAM_VERSION`: `3.4.19`, or 3-4 digits (`3402`, a
 *   build number the library reads as major 0, minor 34)
 * @returns its major and minor parts; NaN where a part is missing, which
 *   every comparison fails, so an unknown version counts as too old
 */
function versionOf(written: string): { major: number; minor: number } {
  if (/^\d{3,4}$/.test(written)) {
    const [, minor = ''] = /(\d{1,2})(\d{2})/.exec(written) ?? []
    return { major: 0, minor: parseInt(minor, 10) }
  }
  const [major = NaN, minor = NaN] = written.split('.').map(part => parseInt(part, 10))
  return { major, minor }
}

/**
 * Joins a relative path to a folder.
 *
 * @param folder an absolute folder, normalized
 * @param relative a relative path as written
 * @returns the joined path, normalized
 */
export function joinPath(folder: string, relative: string): string {
  return normalize(`${folder}/${relative}`)
}

/**
 * Normalizes an absolute path: forward slashes, an upper-case drive, no empty
 * or `.` segments, each `..` folded into its parent, no trailing slash.
 *
 * @param path an absolute path (`C:\x\..\y`, `/x/y`)
 * @returns the path, `C:/y`; a `..` past the root stays at the root, as the
 *   file system reads it
 */
export function normalize(path: string): string {
  const slashed = path.replace(/\\/g, '/')
  const drive = /^[A-Za-z]:/.exec(slashed)?.[0]?.toUpperCase() ?? ''
  const parts: string[] = []
  for (const part of slashed.slice(drive.length).split('/')) {
    if (part === '' || part === '.') continue
    if (part === '..') parts.pop()
    else parts.push(part)
  }
  return `${drive}/${parts.join('/')}`
}

/**
 * Takes a path's last segment.
 *
 * @param path a path with `/` or `\` separators
 * @returns its file or folder name; the path itself when it has no separator
 */
export function baseName(path: string): string {
  const cut = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'))
  return path.slice(cut + 1)
}

/**
 * Picks one file of several that a relative path or a name fits (the same
 * name in two folders): the shortest path, as the person asked (2026-10-19).
 *
 * Shortest means the fewest characters; a tie in length goes to the fewest
 * folders deep, then to the first in A-to-Z order, so the same files always
 * give the same link. The resolver asks this only after its stronger
 * signals missed: a file the session touched, and a file right under the
 * working folder or the project root.
 *
 * @param matches absolute normalized paths that fit
 * @returns the shortest match; undefined only when there is none
 */
export function bestOf(matches: readonly string[]): string | undefined {
  let best: string | undefined
  for (const path of matches) if (best === undefined || isShorterThan(path, best)) best = path
  return best
}

/**
 * Orders two candidate paths for {@link bestOf}.
 *
 * @param path a candidate
 * @param than the best candidate so far
 * @returns true when `path` is shorter: fewer characters, else fewer folders,
 *   else first in A-to-Z order (case-insensitive, then exact, so two
 *   spellings of one name still order the same way every time)
 */
function isShorterThan(path: string, than: string): boolean {
  if (path.length !== than.length) return path.length < than.length
  const depth = path.split('/').length
  const thanDepth = than.split('/').length
  if (depth !== thanDepth) return depth < thanDepth
  const folded = path.toLowerCase()
  const thanFolded = than.toLowerCase()
  return folded !== thanFolded ? folded < thanFolded : path < than
}
