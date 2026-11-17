import type { EngineInterface, PluginOptions, PromptBox, PromptEditInput, PromptFillMode, Register, RenderElement } from 'claude-code'

import type { PromptCutArm, PromptCutPaste } from '../types'
import {
  base64Bytes,
  expand,
  fits,
  formedBy,
  grouped,
  historyLinePattern,
  isText,
  parsePlaceholders,
  pasteContent,
  rebuild,
  recalledIds,
  utf8Base64,
  utf8Text,
} from './placeholders'
import type { Expansion, Placeholder, Resolution } from './placeholders'

/**
 * How long a cut waits for clipboard reads and rebuilds still running, in
 * milliseconds. A cut pressed right after a paste would otherwise find the
 * paste unread; the hook's own budget is 10 s, so the wait stays well under.
 */
const SETTLE_MS = 3_000

/**
 * How long a reader of the clipboard may run, in milliseconds. Windows
 * PowerShell takes about half a second to start; a reader that hangs (a
 * clipboard another program holds open) is killed rather than waited on.
 */
const CLIPBOARD_TIMEOUT_MS = 10_000

/**
 * How long the history search may run, in milliseconds: it reads the whole
 * history file (megabytes after months of use), once per recalled draft.
 */
const HISTORY_TIMEOUT_MS = 15_000

/**
 * How soon a second cut of the same box must follow the first that stopped at
 * a placeholder whose text is unknown, in milliseconds, to cut it as written.
 * Only for a slash-command draft, where Enter cannot be armed: it would run
 * the command.
 */
const CONFIRM_MS = 5_000

/**
 * How long an armed Enter stands, in milliseconds. Long enough to read the
 * toast and press Enter; short enough that an Enter meant as a send much
 * later is not taken for a cut. The hint row shows the arm while it stands.
 */
const ARM_MS = 60_000

/**
 * When to look for the draft the engine puts back in the box after the armed
 * Enter's drop, in milliseconds after the drop: the restore lands only once
 * the chain answered, so a few spaced looks (prompt-queue's own schedule,
 * proven on its queue drops) find it without polling for long.
 */
const RESTORE_LOOKS_MS: readonly number[] = [30, 120, 400, 1000]

/** The key of the hint row's `[Enter cuts ✕]` Button: a press disarms. */
const DISARM_KEY = 'prompt-cut-disarm'

/** How long a toast of the mod's stays up, in milliseconds. */
const TOAST_MS = 5_000

/**
 * How many placeholder records a session keeps. Each holds a paste's text, so
 * the oldest go first; a draft rarely holds more than a few pastes, and a
 * placeholder whose record was evicted only asks for a second press.
 */
const MAX_RECORDS = 200

/**
 * The flags every Windows PowerShell run takes: no profile (a user's profile
 * can print or take seconds) and no prompts.
 */
const POWERSHELL_FLAGS = ['-NoProfile', '-NonInteractive', '-Command'] as const

/**
 * Prints the clipboard's text as base64 of its UTF-8 bytes: no console code
 * page, byte-order mark or trailing newline can change a character of it.
 * An empty or non-text clipboard prints nothing.
 */
const READ_CLIPBOARD_PS =
  "$t = Get-Clipboard -Raw; if ($null -eq $t) { $t = '' }; " +
  '[Console]::Out.Write([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$t)))'

/**
 * Finds the newest line of a file that a regular expression matches and
 * prints it as base64 of its UTF-8 bytes; exit code 3 when none does. The
 * request (`{ path, pattern }`) comes on standard input as base64 of its
 * JSON, so no quoting of the command line can bend it. The pattern is
 * anchored, so each line fails at its first characters but the one sought.
 */
const FIND_LINE_PS = [
  "$ErrorActionPreference = 'Stop'",
  '$request = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([Console]::In.ReadToEnd().Trim())) | ConvertFrom-Json',
  '$pattern = New-Object Text.RegularExpressions.Regex ([string]$request.pattern)',
  '$lines = [IO.File]::ReadAllLines([string]$request.path, [Text.Encoding]::UTF8)',
  'for ($i = $lines.Length - 1; $i -ge 0; $i--) { if ($pattern.IsMatch($lines[$i])) { [Console]::Out.Write([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($lines[$i]))); exit 0 } }',
  'exit 3',
].join('; ')

/**
 * The same search in Perl, for macOS and Linux: the pattern and the file come
 * as arguments (no shell), the newest matching line goes to standard output.
 */
const FIND_LINE_PERL = 'BEGIN { $p = shift @ARGV } $l = $_ if /$p/; END { print $l if defined $l }'

/** The resolution of a placeholder nothing is known about: a cut stops at it once. */
const UNKNOWN: Resolution = { kind: 'unknown' }

/** The resolution of a placeholder whose text was never read. */
const UNREAD: Resolution = { kind: 'unread' }

/** The resolution of plain text that only looks like a placeholder. */
const LITERAL: Resolution = { kind: 'literal' }

/** The resolution of an image placeholder: it stays in the box. */
const IMAGE: Resolution = { kind: 'image' }

/** The operating system family, which picks the clipboard reader and the history search. */
type Platform = 'windows' | 'wsl' | 'macos' | 'linux'

/** The mod's options, read once per load from `register`'s `options`. */
type Config = {
  /**
   * The `clipboardCommand` option split into its words (no shell, no
   * quoting); empty to use the platform's own reader.
   */
  readonly clipboardCommand: readonly string[]
}

/** A fill another plugin made, kept until the box change it caused is seen. */
type Fill = {
  /** The text the fill wrote, as its chain left it. */
  readonly text: string
  /** Where it went: over the draft, after it, or at the cursor. */
  readonly mode: PromptFillMode
  /** The draft the fill landed on. */
  readonly before: string
  /** Where the cursor stood in `before` (an `insert` lands there). */
  readonly cursor: number
}

/**
 * What one placeholder id of a recalled prompt stood for, as the history
 * file's entry keeps it: its text, an image, or a text the entry no longer
 * holds (its paste-cache file is gone, or too big to read).
 */
type HistoryItem = { readonly kind: 'text'; readonly text: string } | { readonly kind: 'image' } | { readonly kind: 'unread' }

/**
 * A prompt the history put back in the box (the up arrow, ctrl+r): the draft
 * as recalled, and the search of the history file for its entry, started by
 * the first cut that needs it and shared by the cuts after it.
 */
type Recall = {
  /** The draft as the history put it in the box. */
  readonly text: string
  /** The entry's pasted contents by id; undefined when no entry was found. Unset until a cut asks. */
  entry: Promise<ReadonlyMap<number, HistoryItem> | undefined> | undefined
}

/**
 * What the mod tracks while it is loaded (a reload starts it afresh; the
 * placeholder records themselves are in `$.state`).
 */
type Live = {
  /** The options. */
  readonly config: Config
  /** The box as last seen: after an edit, or at a change. Undefined before the first. */
  box: { readonly text: string; readonly cursor: number } | undefined
  /** The last fill another plugin made, until the change it caused is seen. */
  fill: Fill | undefined
  /** The prompt the history put in the box, until the box is cleared or cut. */
  recall: Recall | undefined
  /** Clipboard reads and rebuilds still running: a cut waits for them (bounded by {@link SETTLE_MS}). */
  readonly pending: Set<Promise<void>>
  /**
   * The armed Enter: the draft a cut stopped at because a paste's text is
   * unknown. The next Enter from the box cuts it through the engine's own
   * expansion (`prompt.submit`) instead of sending it. The module holds it,
   * so the submit hook decides without a `$` call; `$.state` mirrors it for
   * the hint row. Undefined when nothing is armed.
   */
  arm: { readonly text: string; readonly until: number; readonly timer: { cancel: () => void } | undefined } | undefined
  /**
   * The slash-command cut that stopped at an unknown placeholder: a second cut
   * of the same box before {@link CONFIRM_MS} passes cuts it as written.
   */
  confirm: { readonly text: string; readonly at: number } | undefined
  /** The platform, found on first use. */
  platform: Platform | undefined
}

/**
 * The message of a thrown value, for a log line or a toast.
 *
 * @param error what was thrown
 * @returns its message, or its text when it is no Error
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}

/**
 * Reads the options: the clipboard command split at whitespace.
 *
 * @param options the `userConfig` values, defaults filled in
 * @returns the mod's config
 */
function configOf(options: PluginOptions): Config {
  const command = options.clipboardCommand
  return { clipboardCommand: typeof command === 'string' ? command.trim().split(/\s+/).filter(word => word !== '') : [] }
}

/**
 * Writes a line to the debug log (`claude --debug`), never to the screen.
 *
 * @param $ the engine
 * @param text the line, without the mod's name
 */
function log($: EngineInterface, text: string): void {
  try {
    $.ui.log(`prompt-cut: ${text}`, { to: 'debug' })
  } catch {
    // A log the session refuses (it is ending) is no reason to fail a hook.
  }
}

/**
 * Shows a toast of the mod's for {@link TOAST_MS}. The terminal draws it in a
 * narrow box at the top right, titled with the mod's name (seen live on
 * 2.1.310): the text never repeats the name, and stays short. A refused
 * toast is logged, never thrown.
 *
 * @param $ the engine
 * @param text the line
 */
function toast($: EngineInterface, text: string): void {
  try {
    $.ui.toast(text, { timeoutMs: TOAST_MS })
  } catch (error) {
    log($, `a toast was refused: ${messageOf(error)}`)
  }
}

/**
 * Runs work on a timer of its own, as a dispatch apart from the hook that
 * asks for it, and counts it as pending until it ends: the hook returns at
 * once, its dispatch's abort (an Esc) cannot reach the work, and a cut
 * pressed meanwhile waits for it. A refused timer drops the work, logged.
 *
 * @param $ the engine, as the calling hook holds it
 * @param live the load's state; the work joins `live.pending` until it ends
 * @param work what to run; a failure is caught and logged
 */
function track($: EngineInterface, live: Live, work: () => Promise<void>): void {
  /** Resolves `done`; replaced by the promise's own resolver as it is made. */
  let finish = (): void => undefined
  const done = new Promise<void>(resolve => {
    finish = resolve
  })
  live.pending.add(done)
  /**
   * Ends the work's watch, once: out of `live.pending` and `done` resolved,
   * so a cut waiting on it goes on. Runs whether the work ended, failed, or
   * never started (a refused timer).
   */
  const end = (): void => {
    live.pending.delete(done)
    finish()
  }
  try {
    $.clock.after(1, () => {
      void work()
        .catch(error => log($, `background work failed: ${messageOf(error)}`))
        .finally(end)
    })
  } catch (error) {
    log($, `a timer was refused: ${messageOf(error)}`)
    end()
  }
}

/**
 * Waits for the pending work to end, or for `ms` to pass, whichever is first.
 *
 * @param $ the engine
 * @param pending the work still running
 * @param ms the longest wait, in milliseconds
 * @returns resolves when the work ended or the wait ran out
 */
async function settle($: EngineInterface, pending: ReadonlySet<Promise<void>>, ms: number): Promise<void> {
  if (pending.size === 0) return
  let timer: { cancel: () => void } | undefined
  const timeout = new Promise<void>(resolve => {
    timer = $.clock.after(ms, resolve)
  })
  await Promise.race([Promise.all([...pending]).then(() => undefined), timeout])
  timer?.cancel()
}

/**
 * Finds the operating system from variables only it sets: `OS` on Windows,
 * `WSL_DISTRO_NAME` in WSL, `__CF_USER_TEXT_ENCODING` on macOS; else Linux.
 *
 * @param $ the engine
 * @param live the load's state, which keeps the answer
 * @returns the platform
 */
async function platformOf($: EngineInterface, live: Live): Promise<Platform> {
  if (live.platform !== undefined) return live.platform
  let platform: Platform = 'linux'
  if ((await $.env.get('OS')) === 'Windows_NT') platform = 'windows'
  else if ((await $.env.get('WSL_DISTRO_NAME')) !== undefined) platform = 'wsl'
  else if ((await $.env.get('__CF_USER_TEXT_ENCODING')) !== undefined) platform = 'macos'
  live.platform = platform
  return platform
}

/**
 * Runs one reader of the clipboard and returns what it printed.
 *
 * @param $ the engine
 * @param argv the reader and its arguments
 * @param form how it prints: `raw` text, or `base64` of UTF-8 bytes
 * @returns the clipboard's text; undefined when the reader failed, could not start, or printed more than 4 MiB
 */
async function runReader($: EngineInterface, argv: readonly string[], form: 'raw' | 'base64'): Promise<string | undefined> {
  try {
    const run = await $.process.run(argv, { timeoutMs: CLIPBOARD_TIMEOUT_MS })
    if (run.exitCode !== 0 || run.isStdoutTruncated) {
      log($, `${argv[0]} read no clipboard (exit ${run.exitCode}${run.isStdoutTruncated ? ', over 4 MiB' : ''})`)
      return undefined
    }
    return form === 'base64' ? utf8Text(base64Bytes(run.stdout)) : run.stdout
  } catch (error) {
    log($, `${argv[0]} could not read the clipboard: ${messageOf(error)}`)
    return undefined
  }
}

/**
 * Reads the clipboard's text with the reader the `clipboardCommand` option
 * names, else the platform's own: Windows PowerShell's Get-Clipboard (also
 * from WSL), pbpaste on macOS, and wl-paste, xclip or xsel on Linux, the
 * first that answers.
 *
 * @param $ the engine
 * @param live the load's state (the option, the platform)
 * @returns the clipboard's text, or undefined when no reader answered
 */
async function readClipboard($: EngineInterface, live: Live): Promise<string | undefined> {
  if (live.config.clipboardCommand.length > 0) return runReader($, live.config.clipboardCommand, 'raw')
  switch (await platformOf($, live)) {
    case 'windows':
      return runReader($, ['powershell', ...POWERSHELL_FLAGS, READ_CLIPBOARD_PS], 'base64')
    case 'wsl':
      return runReader($, ['powershell.exe', ...POWERSHELL_FLAGS, READ_CLIPBOARD_PS], 'base64')
    case 'macos':
      return runReader($, ['pbpaste'], 'raw')
    case 'linux': {
      const readers: (readonly string[])[] = [
        ...((await $.env.get('WAYLAND_DISPLAY')) === undefined ? [] : [['wl-paste', '--no-newline']]),
        ['xclip', '-selection', 'clipboard', '-o'],
        ['xsel', '--clipboard', '--output'],
      ]
      for (const reader of readers) {
        const text = await runReader($, reader, 'raw')
        if (text !== undefined) return text
      }
      return undefined
    }
  }
}

/**
 * Reads the record of one placeholder id.
 *
 * @param $ the engine
 * @param id the placeholder's id
 * @returns the record; undefined when there is none (never written, or evicted)
 */
async function readPaste($: EngineInterface, id: number): Promise<PromptCutPaste | undefined> {
  const { value } = await $.state.get({ plugin: 'prompt-cut', key: 'paste', id: String(id) })
  return value ?? undefined
}

/**
 * Writes the record of one placeholder id, and keeps the session to the
 * newest {@link MAX_RECORDS} records, the oldest set to null.
 *
 * The order list is written with `ifVersion`, so two writes at once (two
 * pastes read in parallel) cannot lose an id; a write that keeps missing is
 * logged and leaves that id out of the eviction order only.
 *
 * @param $ the engine
 * @param id the placeholder's id
 * @param record what is known about it
 * @returns resolves once written
 */
async function writePaste($: EngineInterface, id: number, record: PromptCutPaste): Promise<void> {
  const key = String(id)
  await $.state.set({ plugin: 'prompt-cut', key: 'paste', id: key }, record)
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const { value = [], version } = await $.state.get({ plugin: 'prompt-cut', key: 'pasteIds' })
    const ids = [...value.filter(other => other !== key), key]
    const evicted = ids.slice(0, Math.max(0, ids.length - MAX_RECORDS))
    const written = await $.state.set({ plugin: 'prompt-cut', key: 'pasteIds' }, ids.slice(evicted.length), { ifVersion: version })
    if (!written.isSet) continue
    for (const old of evicted) await $.state.set({ plugin: 'prompt-cut', key: 'paste', id: old }, null)
    return
  }
  log($, `the record order of #${key} kept changing under its write; it is left out of eviction`)
}

/**
 * Records pastes the engine made into placeholders: the clipboard is read at
 * once, normalized as the engine normalized the paste, and kept for the
 * placeholder its line count fits; the others are recorded unread. An id
 * that already has a record is left alone (a draft put back, not a paste).
 *
 * @param $ the engine
 * @param live the load's state (the reader)
 * @param pastes the paste placeholders the change brought in
 * @param wasEmpty whether the box was empty before the paste
 * @returns resolves once the records are written
 */
async function capturePastes($: EngineInterface, live: Live, pastes: readonly Placeholder[], wasEmpty: boolean): Promise<void> {
  const fresh: Placeholder[] = []
  for (const placeholder of pastes) {
    if ((await readPaste($, placeholder.id)) === undefined && !fresh.some(other => other.id === placeholder.id)) fresh.push(placeholder)
  }
  if (fresh.length === 0) return
  const raw = await readClipboard($, live)
  // One paste is one clipboard: the first placeholder it fits takes it.
  let isTaken = false
  for (const placeholder of fresh) {
    const text = raw === undefined ? undefined : pasteContent(raw, placeholder.kind, wasEmpty)
    if (!isTaken && text !== undefined && fits(text, placeholder)) {
      isTaken = true
      await writePaste($, placeholder.id, { kind: 'text', text, lines: placeholder.lines ?? 0, via: 'clipboard' })
      log($, `${placeholder.match}: read ${grouped(text.length)} characters from the clipboard`)
    } else {
      await writePaste($, placeholder.id, { kind: 'unread', lines: placeholder.lines ?? null })
      log($, `${placeholder.match}: the clipboard ${raw === undefined ? 'could not be read' : 'held another text'}; recorded unread`)
    }
  }
}

/**
 * Records placeholders as the plain text they are, where the id has no
 * record yet (the engine expands every spelling of a real id).
 *
 * @param $ the engine
 * @param placeholders the placeholders that are only text
 * @returns resolves once recorded
 */
async function markLiteral($: EngineInterface, placeholders: readonly Placeholder[]): Promise<void> {
  for (const placeholder of placeholders) {
    if ((await readPaste($, placeholder.id)) === undefined) await writePaste($, placeholder.id, { kind: 'literal' })
  }
}

/**
 * Classifies the placeholders an edit of the person's brought into the box.
 *
 * On 2.1.310 the engine's paste placeholder arrives as a `prompt.change`
 * (origin `engine`), but an edit whose whole text, with no key, is one paste
 * placeholder is taken as a paste too: it is read from the clipboard. One
 * that came with a typed key, or inside a longer text, is plain text the
 * engine never expands, recorded `literal`.
 *
 * @param $ the engine
 * @param live the load's state
 * @param edit the edit, as `prompt.edit` carried it
 * @param formed the placeholders it brought in
 * @returns resolves once classified
 */
async function classifyEdit($: EngineInterface, live: Live, edit: PromptEditInput, formed: readonly Placeholder[]): Promise<void> {
  const [only] = formed
  if (only !== undefined && formed.length === 1 && edit.key === undefined && edit.inputText.trim() === only.match) {
    if (only.kind === 'paste' || only.kind === 'guide') await capturePastes($, live, [only], edit.text === '')
    return
  }
  await markLiteral($, formed)
}

/**
 * The text behind each text placeholder of a draft, as far as the records
 * know it: a `text` record's text where it fits, a `literal` one's own
 * spelling (plain text is its own content).
 *
 * @param $ the engine
 * @param placeholders the placeholders to look up
 * @returns the texts by id; an id missing is unknown
 */
async function knownTexts($: EngineInterface, placeholders: readonly Placeholder[]): Promise<Map<number, string>> {
  const texts = new Map<number, string>()
  for (const placeholder of placeholders) {
    if (!isText(placeholder) || texts.has(placeholder.id)) continue
    const record = await readPaste($, placeholder.id)
    if (record?.kind === 'literal') texts.set(placeholder.id, placeholder.match)
    else if (record?.kind === 'text' && fits(record.text, placeholder)) texts.set(placeholder.id, record.text)
  }
  return texts
}

/**
 * Records the text of the truncations a box change brought in, rebuilt from
 * the draft the engine cut them out of; a truncation that cannot be rebuilt
 * (a paste in its middle was unread) is recorded unread.
 *
 * @param $ the engine
 * @param source the draft before the engine cut it (or the draft a fill was to make)
 * @param result the draft after
 * @param formed the truncation placeholders `result` brought in
 * @returns resolves once recorded
 */
async function rebuildFormed($: EngineInterface, source: string, result: string, formed: readonly Placeholder[]): Promise<void> {
  const others = [...parsePlaceholders(source), ...parsePlaceholders(result)]
  const texts = await knownTexts($, others)
  for (const target of formed) {
    const text = rebuild(source, result, target, placeholder => texts.get(placeholder.id))
    await writePaste($, target.id, text === undefined ? { kind: 'unread', lines: target.lines ?? null } : { kind: 'text', text, lines: target.lines ?? 0, via: 'rebuilt' })
    log($, `${target.match}: ${text === undefined ? 'could not be rebuilt; recorded unread' : `rebuilt ${grouped(text.length)} characters`}`)
  }
}

/**
 * The draft a fill was to make before the engine collapsed any of it:
 * the fill's text over, after, or inside the draft it landed on.
 *
 * @param fill the fill, as the `prompt.fill` hook saw it
 * @returns the full draft
 */
function filledText(fill: Fill): string {
  if (fill.mode === 'replace') return fill.text
  if (fill.mode === 'append') return fill.before + fill.text
  const at = Math.max(0, Math.min(fill.cursor, fill.before.length))
  return fill.before.slice(0, at) + fill.text + fill.before.slice(at)
}

/**
 * The folder Claude Code keeps its history and paste cache in: the
 * `CLAUDE_CONFIG_DIR` variable, else `.claude` in the home folder.
 *
 * @param $ the engine
 * @returns the folder, without a trailing separator; undefined when no home folder is known
 */
async function configDir($: EngineInterface): Promise<string | undefined> {
  const explicit = await $.env.get('CLAUDE_CONFIG_DIR')
  const home = (await $.env.get('USERPROFILE')) ?? (await $.env.get('HOME'))
  const dir = explicit !== undefined && explicit !== '' ? explicit : home === undefined ? undefined : `${home}/.claude`
  return dir?.replace(/[\\/]+$/, '')
}

/**
 * Finds the newest line of a file that an anchored regular expression
 * matches, with Windows PowerShell (also from WSL) or Perl elsewhere.
 *
 * @param $ the engine
 * @param live the load's state (the platform)
 * @param path the file
 * @param pattern the pattern's source, read alike by .NET and Perl ({@link historyLinePattern})
 * @returns the line; undefined when none matched or the search failed (a missing file, no Perl)
 */
async function findLine($: EngineInterface, live: Live, path: string, pattern: string): Promise<string | undefined> {
  try {
    const platform = await platformOf($, live)
    if (platform === 'windows' || platform === 'wsl') {
      const run = await $.process.run([platform === 'wsl' ? 'powershell.exe' : 'powershell', ...POWERSHELL_FLAGS, FIND_LINE_PS], {
        stdin: utf8Base64(JSON.stringify({ path, pattern })),
        timeoutMs: HISTORY_TIMEOUT_MS,
      })
      if (run.exitCode !== 0 || run.isStdoutTruncated) return undefined
      return utf8Text(base64Bytes(run.stdout))
    }
    const run = await $.process.run(['perl', '-ne', FIND_LINE_PERL, pattern, path], { timeoutMs: HISTORY_TIMEOUT_MS })
    const line = run.stdout.replace(/\r?\n$/, '')
    return run.exitCode === 0 && line !== '' ? line : undefined
  } catch (error) {
    log($, `the history search failed: ${messageOf(error)}`)
    return undefined
  }
}

/**
 * Looks up the history entry of a recalled prompt and what each of its
 * placeholders stood for: an inline text, a text in the paste cache
 * (`paste-cache/<hash>.txt`), or an image.
 *
 * The entry is the newest line of `history.jsonl` whose `display` is the
 * recalled draft with any placeholder ids (the recall renumbered them), or
 * that draft after a `!` (a shell-mode prompt). Its `pastedContents` are
 * keyed by the old ids; each is paired back with the recalled placeholder in
 * the same place. An old id the contents lack was plain text in that prompt.
 *
 * @param $ the engine
 * @param live the load's state (the platform)
 * @param recalled the draft as the up arrow put it in the box
 * @returns each recalled id's item, an id missing being plain text; undefined when no entry was found, it could not be read, or its placeholders do not pair up
 */
async function lookupHistory($: EngineInterface, live: Live, recalled: string): Promise<ReadonlyMap<number, HistoryItem> | undefined> {
  const dir = await configDir($)
  if (dir === undefined) return undefined
  const line = await findLine($, live, `${dir}/history.jsonl`, historyLinePattern(recalled))
  if (line === undefined) {
    log($, 'the recalled prompt has no entry in history.jsonl')
    return undefined
  }
  let entry: { display?: unknown; pastedContents?: unknown }
  try {
    const parsed: unknown = JSON.parse(line)
    entry = typeof parsed === 'object' && parsed !== null ? parsed : {}
  } catch (error) {
    log($, `the history entry is no JSON: ${messageOf(error)}`)
    return undefined
  }
  const pairs = typeof entry.display === 'string' ? recalledIds(recalled, entry.display.replace(/^!/, '')) : undefined
  if (pairs === undefined) {
    log($, 'the history entry and the recalled prompt hold different placeholders')
    return undefined
  }
  const contents = typeof entry.pastedContents === 'object' && entry.pastedContents !== null ? (entry.pastedContents as Record<string, unknown>) : {}
  const items = new Map<number, HistoryItem>()
  for (const [now, then] of pairs) {
    const value = contents[String(then)]
    if (typeof value !== 'object' || value === null) continue
    const item = value as { type?: unknown; content?: unknown; contentHash?: unknown }
    if (item.type === 'image') items.set(now, { kind: 'image' })
    else if (typeof item.content === 'string') items.set(now, { kind: 'text', text: item.content })
    else if (typeof item.contentHash === 'string' && /^[0-9a-f]{16}$/.test(item.contentHash)) {
      try {
        items.set(now, { kind: 'text', text: await $.fs.read(`${dir}/paste-cache/${item.contentHash}.txt`) })
      } catch (error) {
        // Gone (the cache is cleaned after a while) or over 4 MiB.
        log($, `paste-cache/${item.contentHash}.txt could not be read: ${messageOf(error)}`)
        items.set(now, { kind: 'unread' })
      }
    } else items.set(now, { kind: 'unread' })
  }
  return items
}

/**
 * Decides what a cut makes of each placeholder of the box.
 *
 * A placeholder of a recalled prompt follows the prompt's history entry (an
 * id the entry lacks was plain text there); any other follows its record. A
 * `text` record is used only where its line count fits the placeholder: an
 * id reused by a draft from another session is not that paste.
 *
 * @param $ the engine
 * @param live the load's state (the recall)
 * @param placeholders the box's placeholders
 * @returns the resolution of each id
 */
async function resolve($: EngineInterface, live: Live, placeholders: readonly Placeholder[]): Promise<Map<number, Resolution>> {
  const resolutions = new Map<number, Resolution>()
  const recall = live.recall
  const recalled = new Set(recall === undefined ? [] : placeholders.filter(placeholder => recall.text.includes(placeholder.match)).map(placeholder => placeholder.id))
  if (recall !== undefined && recalled.size > 0) recall.entry ??= lookupHistory($, live, recall.text)
  const entry = recall !== undefined && recalled.size > 0 ? await recall.entry : undefined
  for (const placeholder of placeholders) {
    if (resolutions.has(placeholder.id)) continue
    if (entry !== undefined && recalled.has(placeholder.id)) {
      const item = entry.get(placeholder.id)
      resolutions.set(placeholder.id, item === undefined ? LITERAL : item.kind === 'text' ? { kind: 'text', text: item.text } : item.kind === 'image' ? IMAGE : UNREAD)
      continue
    }
    const record = await readPaste($, placeholder.id)
    if (record?.kind === 'literal') resolutions.set(placeholder.id, LITERAL)
    else if (!isText(placeholder)) resolutions.set(placeholder.id, IMAGE)
    else if (record?.kind === 'text') resolutions.set(placeholder.id, fits(record.text, placeholder) ? { kind: 'text', text: record.text } : UNKNOWN)
    else resolutions.set(placeholder.id, record?.kind === 'unread' ? UNREAD : UNKNOWN)
  }
  return resolutions
}

/**
 * Counts the characters of a text as a person does: a surrogate pair is one.
 *
 * @param text the text
 * @returns its code points
 */
function charCount(text: string): number {
  return text.length - (text.match(/[\uD800-\uDBFF][\uDC00-\uDFFF]/g) ?? []).length
}

/**
 * Names placeholders for a toast: the first two as written, then how many more.
 *
 * @param placeholders the placeholders (at least one)
 * @returns e.g. `[Pasted text #2 +40 lines]` or `[Pasted text #2 +40 lines], [Pasted text #5] and 1 more`
 */
function named(placeholders: readonly Placeholder[]): string {
  const spelled = [...new Set(placeholders.map(placeholder => placeholder.match))]
  const shown = spelled.slice(0, 2).join(', ')
  return spelled.length > 2 ? `${shown} and ${spelled.length - 2} more` : shown
}

/**
 * The toast after a cut: how many characters went to the clipboard, how many
 * pastes were expanded, what was left as written, and the images kept.
 *
 * @param expansion what was cut
 * @returns the line
 */
function summary(expansion: Expansion): string {
  const pastes = new Set(expansion.expanded.map(placeholder => placeholder.id)).size
  const images = expansion.images.length
  let line = `Cut ${grouped(charCount(expansion.text))} characters to the clipboard`
  if (pastes > 0) line += `, ${pastes} paste${pastes === 1 ? '' : 's'} expanded`
  if (expansion.unresolved.length > 0) line += `; ${named(expansion.unresolved)} left as written`
  if (images > 0) line += `; ${images} image${images === 1 ? ' stays' : 's stay'} in the box`
  return `${line}.`
}

/**
 * Writes the arm's mirror in `$.state`, which redraws the hint row. A refused
 * write leaves the row as it was; the module's own arm still decides.
 *
 * @param $ the engine
 * @param arm the arm to show, or null for none
 * @returns resolves once written, or once the failure is logged
 */
async function showArm($: EngineInterface, arm: PromptCutArm | null): Promise<void> {
  try {
    await $.state.set({ plugin: 'prompt-cut', key: 'armed' }, arm)
  } catch (error) {
    log($, `the hint row's arm was not written: ${messageOf(error)}`)
  }
}

/**
 * Takes the armed Enter back, if one stands: the next Enter sends again.
 *
 * @param $ the engine
 * @param live the load's state, whose arm is cleared and its timer cancelled
 * @returns resolves once the hint row's mirror is written
 */
async function disarm($: EngineInterface, live: Live): Promise<void> {
  const arm = live.arm
  if (arm === undefined) return
  live.arm = undefined
  arm.timer?.cancel()
  await showArm($, null)
}

/**
 * Arms the next Enter to cut the draft in the box, for a draft holding a
 * placeholder whose text no record and no history entry has.
 *
 * Claude Code expands every paste on its own send (`prompt.submit`) and
 * nowhere else a mod can read, so the cut waits for that Enter and takes it
 * over there. The draft is first written back over itself: a fill always puts
 * the box in prompt mode, so a shell-mode draft's Enter reaches
 * `prompt.submit` instead of running its command (a `!` prompt never does);
 * the pasted contents and the caret stay. The arm stands {@link ARM_MS}, and
 * any change of the draft takes it back.
 *
 * @param $ the engine, as the command's hook holds it
 * @param live the load's state, which takes the arm
 * @param box the box as the cut read it
 * @param unresolved the placeholders whose text is unknown, for the toast
 * @returns resolves once armed, or refused with a toast (a dialog holds the box)
 */
async function armEnter($: EngineInterface, live: Live, box: PromptBox, unresolved: readonly Placeholder[]): Promise<void> {
  const filled = await $.prompt.fill({ text: box.text, mode: 'replace', cursor: box.cursor })
  if (!filled.isFilled) {
    toast($, `Nothing cut: the box could not be prepared${filled.refusal === undefined ? '' : ` (${filled.refusal})`}.`)
    return
  }
  await disarm($, live)
  const until = (await $.clock.now()) + ARM_MS
  let timer: { cancel: () => void } | undefined
  try {
    timer = $.clock.after(ARM_MS, () => {
      // This arm only: a newer one has a timer of its own.
      if (live.arm?.until === until) void disarm($, live)
    })
  } catch (error) {
    log($, `the arm's timer was refused, so it stands until the draft changes: ${messageOf(error)}`)
  }
  live.arm = { text: box.text, until, timer }
  live.confirm = undefined
  await showArm($, { text: box.text, until })
  toast($, `Press Enter to cut: Claude Code expands ${named(unresolved)} only when you send, so prompt-cut takes that Enter and sends nothing. Typing cancels.`)
  log($, `armed Enter for ${named(unresolved)}`)
}

/**
 * Cuts a draft the armed Enter submitted: Claude Code hands `prompt.submit`
 * the text with every paste expanded, which goes to the clipboard (images
 * out: they stay in the box); the caller then drops the submit, so nothing
 * is sent, and the engine puts the draft back in the box, which
 * {@link clearRestored} empties again.
 *
 * @param $ the engine, as the prompt.submit hook holds it
 * @param live the load's state
 * @param submitted the submitted text, pastes expanded
 * @param armed the draft as armed, placeholders and all: its images stay
 * @returns the reason the submit is dropped with, which the engine shows
 */
async function cutSubmitted($: EngineInterface, live: Live, submitted: string, armed: string): Promise<string> {
  const flat = expand(submitted, placeholder => (placeholder.kind === 'image' ? IMAGE : LITERAL))
  const copied = await $.ui.copy({ text: flat.text })
  if (!copied.isCopied) return `nothing was cut: no clipboard took the text (${copied.reason}); the draft is back in the box`
  const images = parsePlaceholders(armed)
    .filter(placeholder => placeholder.kind === 'image')
    .map(placeholder => placeholder.match)
  clearRestored($, (await $.prompt.read()).text, [submitted, armed], images)
  live.recall = undefined
  log($, `cut ${grouped(flat.text.length)} characters at the armed Enter`)
  return `cut ${grouped(charCount(flat.text))} characters to the clipboard, pastes expanded (prompt-cut)`
}

/**
 * Empties the box again after the engine put the dropped draft back,
 * leaving the draft's images and whatever was typed since the drop.
 *
 * The engine restores a dropped prompt as `<typed since>\n<prompt>`, or the
 * prompt alone into an empty box, once the chain answered (prompt-queue's
 * finding, 2.1.296 on). On 2.1.310 an idle drop came back expanded, the
 * paste's text inline (seen live); prompt-queue saw the placeholder form, so
 * both count. The first look that finds the box changed decides: a box that
 * is that restore gets its earlier text back, the images after it; any other
 * change is the person's and is left alone. Never throws: the cut stands
 * either way.
 *
 * @param $ the engine, as the prompt.submit hook holds it
 * @param before the box at the drop (the submit emptied it; typing since stays)
 * @param forms the draft as the engine may put it back: the submitted text (pastes expanded) and the draft as armed
 * @param images the draft's image placeholders, kept in the box
 */
function clearRestored($: EngineInterface, before: string, forms: readonly string[], images: readonly string[]): void {
  const prefix = before.trim() === '' ? '' : `${before}\n`
  const kept = images.join(' ')
  let isSettled = false
  for (const ms of RESTORE_LOOKS_MS) {
    try {
      $.clock.after(ms, () => {
        void (async () => {
          if (isSettled) return
          const box = await $.prompt.read()
          if (box.text === before) return
          isSettled = true
          const rest = box.text.startsWith(prefix) ? box.text.slice(prefix.length).trim() : undefined
          if (rest === undefined || !forms.some(form => form.trim() === rest)) return
          const text = prefix === '' ? kept : kept === '' ? before : `${before}\n${kept}`
          const filled = await $.prompt.fill({ text, mode: 'replace' })
          if (!filled.isFilled) toast($, 'Cut, but the draft is back in the box: clear it before Enter.')
        })().catch(error => log($, `the restored draft was not cleared: ${messageOf(error)}`))
      })
    } catch {
      // A refused timer leaves the restored draft in the box; the cut stands.
    }
  }
}

/**
 * Joins the hint row's text and tail as the engine draws them, for a hook
 * that draws the row itself: the same join prompt-queue, chat-view and
 * next-prompt use, so the row reads the same whichever is outermost.
 *
 * @param hint the row's text (`e.props.hint`); `''` when a mod above drew it
 * @param tail the text added after it (`e.props.tail`), if any
 * @returns the non-empty parts, trimmed, joined by ` · `
 */
function hintLine(hint: string, tail: string | undefined): string {
  return [hint, tail ?? '']
    .map(part => part.trim())
    .filter(part => part !== '')
    .join(' · ')
}

/**
 * Cuts the prompt box: its text, every paste expanded, goes to the clipboard,
 * and the box is emptied (its images stay: no text clipboard carries one).
 *
 * Waits first for clipboard reads still running, so a cut right after a paste
 * finds it read. A draft holding a placeholder whose text is unknown arms the
 * next Enter instead ({@link armEnter}): Claude Code's own send expands it.
 * A slash-command draft cannot be armed (Enter would run the command): it is
 * kept, and a second cut of the same box within {@link CONFIRM_MS} cuts it as
 * written. A press while armed takes the arm back. Nothing is emptied unless
 * the clipboard took the text.
 *
 * @param $ the engine, as the command's hook holds it
 * @param live the load's state
 * @returns resolves once the cut is done, armed, or refused with a toast
 */
async function cut($: EngineInterface, live: Live): Promise<void> {
  if (live.arm !== undefined) {
    await disarm($, live)
    toast($, 'Cut cancelled: Enter sends again.')
    return
  }
  await settle($, live.pending, SETTLE_MS)
  const box = await $.prompt.read()
  const resolutions = await resolve($, live, parsePlaceholders(box.text))
  const expansion = expand(box.text, placeholder => resolutions.get(placeholder.id) ?? UNKNOWN)
  // Nothing to cut: an empty box, blanks, or only images.
  if (expansion.text.trim() === '') return
  if (expansion.unresolved.length > 0) {
    if (!box.text.trimStart().startsWith('/')) {
      await armEnter($, live, box, expansion.unresolved)
      return
    }
    const now = await $.clock.now()
    const confirm = live.confirm
    if (confirm === undefined || confirm.text !== box.text || now - confirm.at > CONFIRM_MS) {
      live.confirm = { text: box.text, at: now }
      toast($, `Kept the box: ${named(expansion.unresolved)} hides a text prompt-cut never read, and Enter would run this command. Cut again within ${CONFIRM_MS / 1000} s to cut it as written.`)
      log($, `stopped at ${named(expansion.unresolved)} in a command line`)
      return
    }
  }
  live.confirm = undefined
  const copied = await $.ui.copy({ text: expansion.text })
  if (!copied.isCopied) {
    toast($, `Nothing cut: no clipboard took the text (${copied.reason}).`)
    return
  }
  const filled = await $.prompt.fill({ text: expansion.images.join(' '), mode: 'replace' })
  if (!filled.isFilled) {
    toast($, `Copied, but the box could not be emptied${filled.refusal === undefined ? '' : ` (${filled.refusal})`}.`)
    return
  }
  live.recall = undefined
  toast($, summary(expansion))
  log($, `cut ${grouped(expansion.text.length)} characters, ${expansion.expanded.length} placeholder(s) expanded`)
}

/**
 * Registers prompt-cut: the command `/prompt-cut`, which a key bound to
 * `command:prompt-cut` in `~/.claude/keybindings.json` runs (`ctrl+x ctrl+x`
 * here), cuts the whole prompt box to the clipboard with its pastes expanded
 * and empties the box, at once, also while a turn runs.
 *
 * The engine never hands a mod a paste's text, only `[Pasted text #N +M
 * lines]`, so the mod watches the box: each paste the engine turns into a
 * placeholder is read back from the clipboard it came from (`prompt.change`,
 * origin `engine`, on 2.1.310; `prompt.edit` too), a placeholder typed or
 * written as text is marked plain text, a truncation of a long draft is
 * rebuilt from the draft before it (`prompt.change`, `prompt.fill`), and a
 * prompt recalled from the history gets its pastes from `history.jsonl` and
 * the paste cache when it is cut. A paste whose text is still unknown arms
 * Enter (`[Enter cuts ✕]` on the hint row): the engine expands every paste on
 * its own send, which `prompt.submit` copies and drops, so nothing is sent.
 *
 * @param on registers the hooks
 * @param options the `userConfig` values: `clipboardCommand`
 */
export const register: Register = (on, options) => {
  const live: Live = {
    config: configOf(options),
    box: undefined,
    fill: undefined,
    recall: undefined,
    pending: new Set(),
    arm: undefined,
    confirm: undefined,
    platform: undefined,
  }

  on('session.start', async ($, e, next) => {
    // The name is written out here and in the command.run matcher: the
    // validator reads a command a module answers itself only from a literal.
    try {
      await $.command.register({
        name: 'prompt-cut',
        description: 'Cut the prompt box to the clipboard, pastes expanded (bind a key to command:prompt-cut).',
        immediate: true,
      })
    } catch (error) {
      log($, `/prompt-cut was not registered: ${messageOf(error)}`)
    }
    // The arm's mirror outlives a reload; the module's arm does not.
    await showArm($, null)
    return next(e)
  })

  // Typed, the command has nothing to cut (sending it empties the box first),
  // so the typeahead and /help leave it out; a key bound to it still runs it.
  on('command.describe', { command: 'prompt-cut' }, ($, e, next) => next({ ...e, isHidden: true }))

  on('prompt.edit', async ($, e, next) => {
    const box = await next(e)
    live.box = { text: box.text, cursor: box.cursor }
    // The person changed the draft: the armed Enter was for the old one. A
    // caret move alone keeps it.
    if (live.arm !== undefined && box.text !== live.arm.text) void disarm($, live)
    // Typing moves on at once; only an edit that brought a placeholder in
    // starts work, on a timer of its own.
    const formed = formedBy(e.text, box.text)
    if (formed.length > 0) track($, live, () => classifyEdit($, live, e, formed))
    return box
  })

  on('prompt.fill', async ($, e, next) => {
    const before = live.box
    const result = await next(e)
    // Kept for the change it causes: a fill over 10,000 characters lands
    // truncated, and its middle is rebuilt from the text it was to write.
    if (result.isFilled) live.fill = { text: e.text, mode: e.mode, before: before?.text ?? '', cursor: before?.cursor ?? 0 }
    return result
  })

  on('prompt.change', async ($, e, next) => {
    const result = await next(e)
    const before = live.box?.text ?? ''
    const fill = live.fill
    live.fill = undefined
    live.box = { text: e.text, cursor: e.cursor }
    // Any other draft takes the arm back, but not a `clear`: the armed Enter
    // itself empties the box before prompt.submit is raised, and that change
    // may land first. Nor the mod's own write of the draft over itself.
    const isOwn = e.origin.kind === 'plugin' && e.origin.name === 'prompt-cut'
    if (live.arm !== undefined && e.origin.kind !== 'clear' && !isOwn && e.text !== live.arm.text) void disarm($, live)
    if (e.origin.kind === 'history') {
      // Its pastes are the history entry's, looked up when a cut needs them.
      live.recall = { text: e.text, entry: undefined }
      return result
    }
    if (e.origin.kind === 'clear') {
      live.recall = undefined
      return result
    }
    const formed = formedBy(before, e.text)
    if (formed.length === 0) return result
    const truncations = formed.filter(placeholder => placeholder.kind === 'truncated')
    const others = formed.filter(placeholder => placeholder.kind !== 'truncated')
    if (truncations.length > 0) {
      const source = e.origin.kind === 'plugin' && fill !== undefined ? filledText(fill) : before
      track($, live, () => rebuildFormed($, source, e.text, truncations))
    }
    if (e.origin.kind === 'plugin' && fill !== undefined) {
      // A placeholder a plugin wrote as text is plain text: the fill made no
      // pasted contents for it.
      const written = others.filter(placeholder => fill.text.includes(placeholder.match))
      if (written.length > 0) track($, live, () => markLiteral($, written))
    } else if (e.origin.kind === 'engine') {
      // How a paste lands on 2.1.310: the engine writes its placeholder
      // itself, as a change (seen live; the docs place pastes at prompt.edit).
      const pastes = others.filter(placeholder => placeholder.kind === 'paste' || placeholder.kind === 'guide')
      if (pastes.length > 0) track($, live, () => capturePastes($, live, pastes, before === ''))
    }
    return result
  })

  on('command.run', { command: 'prompt-cut' }, async $ => {
    try {
      await cut($, live)
    } catch (error) {
      toast($, `The cut failed: ${messageOf(error)}`)
      log($, `the cut failed: ${messageOf(error)}`)
    }
    // No text: the command leaves no row in the transcript, so a cut never
    // reaches the conversation the model reads.
    return {}
  })

  on('prompt.submit', { origin: { kind: 'composer' } }, async ($, e, next) => {
    // Decided on the module's own arm, no `$` call first: an Enter that is
    // not armed must reach the session whatever fails here.
    const arm = live.arm
    if (arm === undefined) return next(e)
    // One Enter only, cut or not: an arm that outlived it would take a later
    // send for a cut.
    await disarm($, live)
    if (e.command !== undefined || e.text.trim() === '') return next(e)
    try {
      return { drop: await cutSubmitted($, live, e.text, arm.text) }
    } catch (error) {
      // The person pressed Enter to cut: sending instead would hand the draft
      // to the model. Dropped, the engine puts it back in the box.
      log($, `the armed Enter's cut failed: ${messageOf(error)}`)
      return { drop: `prompt-cut could not cut it (${messageOf(error)}); the draft is back in the box` }
    }
  })

  on('ui.render', { component: 'PromptHint' }, async ($, e, next) => {
    // Read first, always: the read subscribes the row, so arming and
    // disarming redraw it even while it draws nothing of the mod's.
    await $.state.get({ plugin: 'prompt-cut', key: 'armed' })
    if (live.arm === undefined) return next(e)
    const { Box, Button, Text } = $.ui.resolve(e)
    // How mods share this row (prompt-queue, chat-view, next-prompt): the
    // outermost draws the line's text once and hands the mods beneath an
    // empty `hint` and no `tail`; a Box coming back is their parts, drawn
    // after the text. Anything else is the engine's own line, already drawn.
    const text = hintLine(e.props.hint, e.props.tail)
    const { tail: _tail, ...props } = e.props
    const theirs = await next({ ...e, props: { ...props, hint: '' } })
    const parts: RenderElement[] = []
    if (text !== '') parts.push(Text({ dimColor: true, wrap: 'truncate-end', children: [text] }))
    if (theirs.type === 'Box') parts.push(Box({ flexShrink: 0, children: [theirs] }))
    parts.push(Box({ flexShrink: 0, children: [Button({ key: DISARM_KEY, label: '[Enter cuts ✕]', plain: true, onPress: () => undefined })] }))
    // No flexGrow: the engine holds this tree in a box as wide as its
    // content beside its own mode label (prompt-queue's note, seen live).
    return Box({ columnGap: 1, children: parts })
  })

  on('ui.press', { plugin: 'prompt-cut' }, async ($, e) => {
    // Handled here with the press's own `$`, as prompt-queue and next-prompt
    // do: a Button's onPress closure holds the `$` of the render that drew it.
    if (e.element === DISARM_KEY && live.arm !== undefined) {
      await disarm($, live)
      toast($, 'Cut cancelled: Enter sends again.')
    }
    return { element: e.element }
  })
}
