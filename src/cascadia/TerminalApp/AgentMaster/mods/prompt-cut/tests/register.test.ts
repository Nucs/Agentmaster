import type {
  On,
  PromptChangeInput,
  PromptChangeResult,
  PromptEditInput,
  PromptEditResult,
  PromptOrigin,
  PromptSubmitResult,
  RenderPropsOf,
} from 'claude-code'
import type { Engine, MockClock } from 'claude-code/testing'
import { describe, expect, mock, test } from 'claude-code/testing'

import {
  base64Bytes,
  expand,
  fits,
  formedBy,
  grouped,
  historyLinePattern,
  newlineCount,
  normalizePaste,
  parsePlaceholders,
  pasteContent,
  rebuild,
  recalledIds,
  utf8Base64,
  utf8Text,
} from '../hooks/placeholders'
import type { Placeholder, Resolution } from '../hooks/placeholders'

/** The home folder the tests' environment names (USERPROFILE): the config dir is `.claude` under it. */
const HOME = 'C:\\Users\\me'

/** The history file's path as the mod builds it, forward slashes (the kit hands fs hooks backslashes). */
const HISTORY = 'C:/Users/me/.claude/history.jsonl'

/** Where the paste cache's files are, forward slashes. */
const PASTE_CACHE = 'C:/Users/me/.claude/paste-cache'

/** A paste of four lines (three line breaks), as the clipboard holds it on Windows. */
const FOUR_LINES = 'first line\r\nsecond\tline\r\nthird line\r\nfourth line'

/** {@link FOUR_LINES} as the engine keeps it: line breaks `\n`, the tab four spaces. */
const FOUR_LINES_KEPT = 'first line\nsecond    line\nthird line\nfourth line'

/** The test world: what the stand-ins for core hold and record. */
type World = {
  /** The prompt box's draft. */
  box: string
  /** Where the caret stands in it. */
  cursor: number
  /** The system clipboard's text, as a reader prints it. */
  clipboard: string
  /** Whether the clipboard takes a copy (false: `no-clipboard`). */
  canCopy: boolean
  /** The texts copied, in order. */
  copies: string[]
  /** The texts the box was filled with, in order. */
  fills: string[]
  /** The toasts shown. */
  toasts: string[]
  /** The debug log's lines. */
  logs: string[]
  /** Every process run, as its argv. */
  runs: (readonly string[])[]
  /** history.jsonl, a line each. */
  history: string[]
  /** Files `$.fs.read` finds, keyed by forward-slash path. */
  files: Record<string, string>
  /** The commands the mod registered: their names, and whether they run mid-turn. */
  commands: { name: string; immediate: boolean }[]
  /** The prompts that reached the session (a `prompt.submit` no hook dropped). */
  sent: string[]
  /** The mocked clock. */
  clock: MockClock
}

/**
 * Seats the stand-ins for core beneath the plugin: the session's start, the
 * command registry, the prompt box (read, fill, edit, change), the
 * clipboard, toasts and the log, process runs (the clipboard reader and the
 * history search as Windows PowerShell answers them), file reads, a Windows
 * environment and a mocked clock.
 *
 * @param on the registrar whose hooks stand for core
 * @returns the world those hooks read and fill in
 */
function seat(on: On): World {
  const world: World = {
    box: '',
    cursor: 0,
    clipboard: '',
    canCopy: true,
    copies: [],
    fills: [],
    toasts: [],
    logs: [],
    runs: [],
    history: [],
    files: {},
    commands: [],
    sent: [],
    clock: mock.clock(on, { now: 1_000_000 }),
  }
  mock.env(on, { OS: 'Windows_NT', USERPROFILE: HOME })
  on('session.start', ($, e) => ({ cwd: e.cwd }))
  on('command.register', ($, e) => {
    world.commands.push({ name: e.name, immediate: e.immediate === true })
    return { value: { command: e.name } }
  })
  on('command.describe', ($, e) => ({ description: e.description, argumentHint: e.argumentHint, isHidden: e.isHidden }))
  on('prompt.read', () => ({ value: { text: world.box, cursor: world.cursor } }))
  on('prompt.fill', ($, e) => {
    world.fills.push(e.text)
    world.box = e.mode === 'append' ? world.box + e.text : e.mode === 'insert' ? world.box.slice(0, world.cursor) + e.text + world.box.slice(world.cursor) : e.text
    world.cursor = world.box.length
    return { isFilled: true, text: world.box, cursor: world.cursor }
  })
  on('prompt.edit', ($, e) => {
    // The editor's own splice, as core applies it to the draft it is handed.
    world.box = e.text.slice(0, e.start) + e.inputText + e.text.slice(e.end)
    world.cursor = e.start + e.inputText.length
    return { text: world.box, cursor: world.cursor }
  })
  on('prompt.change', ($, e) => ({ text: e.text }))
  on('prompt.submit', ($, e) => {
    world.sent.push(e.text)
    return { text: e.text }
  })
  on('ui.render', ($, e) => {
    const { Text } = $.ui.resolve(e)
    return Text({ children: ['engine'] })
  })
  on('ui.copy', ($, e) => {
    if (!world.canCopy) return { value: { isCopied: false, reason: 'no-clipboard' } }
    world.copies.push(e.text)
    return { value: { isCopied: true } }
  })
  on('ui.toast', ($, e) => {
    world.toasts.push(e.text)
    return { value: undefined }
  })
  on('ui.log', ($, e) => {
    world.logs.push(e.text)
    return { value: undefined }
  })
  on('process.run', ($, e) => {
    world.runs.push(e.argv)
    const stdin = e.init?.stdin
    if (e.argv[0] !== 'powershell') return { value: ran(0, world.clipboard) }
    if (stdin === undefined) return { value: ran(0, utf8Base64(world.clipboard)) }
    // The history search: the request comes as base64 of its JSON, and
    // .NET reads the pattern as JavaScript does.
    const request = JSON.parse(utf8Text(base64Bytes(stdin))) as { path: string; pattern: string }
    if (request.path.replace(/\\/g, '/') !== HISTORY) return { value: ran(1, '') }
    const pattern = new RegExp(request.pattern)
    const line = [...world.history].reverse().find(text => pattern.test(text))
    return { value: line === undefined ? ran(3, '') : ran(0, utf8Base64(line)) }
  })
  on('fs.read', ($, e) => {
    const text = world.files[e.path.replace(/\\/g, '/')]
    if (text === undefined) throw new Error(`ENOENT: ${e.path}`)
    return { value: text }
  })
  return world
}

/**
 * A finished process run, as `$.process.run` resolves one.
 *
 * @param exitCode the exit code
 * @param stdout what it printed
 * @returns the result
 */
function ran(exitCode: number, stdout: string): { exitCode: number; stdout: string; stderr: string; isStdoutTruncated: boolean; isStderrTruncated: boolean } {
  return { exitCode, stdout, stderr: '', isStdoutTruncated: false, isStderrTruncated: false }
}

/**
 * The box events the engine raises from its editor, as a test raises them.
 *
 * The kit's `$` raises `prompt.edit` and `prompt.change` as a session does,
 * but its type (EventCalls) lists neither on 2.1.310, so this view of the
 * `prompt` noun names the two calls the tests make.
 */
type BoxEvents = {
  /** Raises `prompt.edit` over every plugin, then the test's own hook. */
  edit: (input: PromptEditInput) => Promise<PromptEditResult>
  /** Raises `prompt.change` over every plugin, then the test's own hook. */
  change: (input: PromptChangeInput) => Promise<PromptChangeResult>
}

/**
 * Reads the kit's `prompt` noun with the two box events it raises.
 *
 * @param $ the engine, as the test holds it
 * @returns the same noun, typed with `edit` and `change`
 */
function boxOf($: Engine): BoxEvents {
  return $.prompt as unknown as BoxEvents
}

/**
 * Starts the test session as the REPL does.
 *
 * @param $ the engine, as the test holds it
 */
async function start($: Engine): Promise<void> {
  await $.session.start({ cwd: 'C:/work', surface: 'terminal', isInteractive: true })
}

/**
 * Types a text at the caret as the person does: one edit per character, each
 * with its key, then the work the edits left on the clock.
 *
 * @param $ the engine
 * @param world the test world
 * @param text what is typed
 */
async function type($: Engine, world: World, text: string): Promise<void> {
  for (const char of text) {
    const before = world.box
    const at = world.cursor
    await boxOf($).edit({ origin: { kind: 'composer' }, key: { key: char }, text: before, cursor: at, start: at, end: at, inputText: char })
  }
  await world.clock.advance(1)
}

/**
 * Plays a paste the engine turned into a placeholder, then the clipboard read
 * it starts. As 2.1.310 does it (seen live): the engine writes the
 * placeholder at the caret itself, and the box raises `prompt.change` with
 * origin `engine`; `via: 'edit'` plays the form its docs describe instead,
 * one `prompt.edit` with no key whose text is the placeholder alone.
 *
 * @param $ the engine
 * @param world the test world
 * @param placeholder what the engine put in the box (`[Pasted text #1 +3 lines]`)
 * @param options `settle`: run the clipboard read now (false leaves it on the clock); `via`: the event that carries it
 */
async function paste($: Engine, world: World, placeholder: string, options: { settle?: boolean; via?: 'change' | 'edit' } = {}): Promise<void> {
  const at = world.cursor
  if (options.via === 'edit') {
    await boxOf($).edit({ origin: { kind: 'composer' }, text: world.box, cursor: at, start: at, end: at, inputText: placeholder })
  } else {
    world.box = world.box.slice(0, at) + placeholder + world.box.slice(at)
    world.cursor = at + placeholder.length
    await boxOf($).change({ text: world.box, cursor: world.cursor, origin: { kind: 'engine' } })
  }
  if (options.settle !== false) await world.clock.advance(1)
}

/** The hint row as the engine raises it under a draft, no turn running. */
const HINT_PROPS: RenderPropsOf['PromptHint'] = { isDraft: true, isWorking: false, hint: '⏵⏵ auto mode on (shift+tab to cycle)' }

/**
 * Draws the hint row through the mod and returns what it drew, as JSON.
 *
 * @param $ the engine
 * @returns the drawn tree, serialized, for a `toContain` on a label
 */
async function hintText($: Engine): Promise<string> {
  const row = await $.ui.mount({ plugin: 'prompt-cut', surface: 'terminal', component: 'PromptHint', props: HINT_PROPS })
  const drawn = JSON.stringify(await row.drawn())
  await row.unmount()
  return drawn
}

/**
 * Plays the person's Enter on the box as the engine raises it: `prompt.submit`
 * from the composer, its text with the pastes expanded.
 *
 * @param $ the engine
 * @param text the submitted text, as the engine expanded it
 * @returns what the chain answered: `{ text }` when it entered, `{ drop }` when a hook took it
 */
async function enter($: Engine, text: string): Promise<PromptSubmitResult> {
  return $.prompt.submit({ text, wait: false, origin: { kind: 'composer' } })
}

/** What the person's key bound to `command:prompt-cut` raises. */
const COMMAND = { command: 'prompt-cut', args: '', origin: { kind: 'composer' } as PromptOrigin, presentation: { isFullscreen: true, columns: 120 } }

/**
 * Runs the cut as the person's key does, and returns the command's output.
 *
 * @param $ the engine
 * @returns the output text (undefined: no transcript row)
 */
async function cut($: Engine): Promise<string | undefined> {
  return (await $.command.run(COMMAND)).text
}

/**
 * Mirrors how Claude Code 2.1.310 truncates a draft over 10,000 characters
 * (`K5e` and `Ato` in its bundle): the pastes in the middle expanded, the
 * images there moved after the first 500 characters, the middle behind
 * `[...Truncated text #id +M lines...]`, the first and last 500 kept.
 *
 * @param text the draft
 * @param pastes the draft's pasted contents by id
 * @param id the id the truncation takes
 * @returns the draft the engine leaves in the box
 */
function engineTruncate(text: string, pastes: Record<number, { type: 'text'; content: string } | { type: 'image' }>, id: number): string {
  const headEnd = 500
  const tailStart = text.length - 500
  const moved: string[] = []
  let partly = text
  const found = parsePlaceholders(text)
  for (let i = found.length - 1; i >= 0; i -= 1) {
    const placeholder = found[i]!
    const entry = pastes[placeholder.id]
    if (entry === undefined) continue
    const end = placeholder.index + placeholder.match.length
    const isMiddle = end > headEnd && placeholder.index < tailStart
    if (entry.type !== 'text') {
      if (isMiddle) {
        moved.unshift(placeholder.match)
        partly = partly.slice(0, placeholder.index) + partly.slice(end)
      }
      continue
    }
    if (isMiddle) partly = partly.slice(0, placeholder.index) + entry.content + partly.slice(end)
  }
  const middle = partly.slice(500, -500)
  const truncated = `${partly.slice(0, 500)}[...Truncated text #${id} +${newlineCount(middle)} lines...]${partly.slice(-500)}`
  return moved.length === 0 ? truncated : truncated.slice(0, 500) + moved.join('') + truncated.slice(500)
}

/**
 * A long multi-line text: `count` numbered lines.
 *
 * @param count how many lines
 * @param label what each line says before its number
 * @returns the lines joined by `\n`
 */
function lines(count: number, label: string): string {
  return Array.from({ length: count }, (_, i) => `${label} ${i + 1}: the quick brown fox jumps over the lazy dog`).join('\n')
}

describe('the placeholder grammar', () => {
  test('parses every kind, its id, line count and place, as the engine spells them', () => {
    const text = 'a [Pasted text #1 +21 lines] b [Image #2] c [...Truncated text #3 +7 lines...] d [Pasted text #4] e [✦ Team setup guide #5 +2 lines]'
    expect(parsePlaceholders(text).map(p => [p.kind, p.id, p.lines, p.match])).toEqual([
      ['paste', 1, 21, '[Pasted text #1 +21 lines]'],
      ['image', 2, undefined, '[Image #2]'],
      ['truncated', 3, 7, '[...Truncated text #3 +7 lines...]'],
      ['paste', 4, undefined, '[Pasted text #4]'],
      ['guide', 5, 2, '[✦ Team setup guide #5 +2 lines]'],
    ])
    expect(parsePlaceholders(text)[0]?.index).toBe(2)
  })

  test('takes no id 0, no other label and no text without a bracket', () => {
    expect(parsePlaceholders('[Pasted text #0 +3 lines] [Pasted file #2] [Pasted text #x]')).toEqual([])
    expect(parsePlaceholders('no placeholders here')).toEqual([])
  })

  test('finds a recalled prompt in the history whatever ids the recall gave its placeholders', () => {
    /**
     * Writes a history line as Claude Code does (JSON.stringify, `display` first).
     *
     * @param display the entry's draft
     * @returns the line
     */
    const line = (display: string): string => JSON.stringify({ display, pastedContents: { 3: { id: 3 } }, timestamp: 1 })
    const pattern = new RegExp(historyLinePattern('see [Pasted text #4 +39 lines] "q"\nnext'))
    expect(pattern.test(line('see [Pasted text #3 +39 lines] "q"\nnext'))).toBe(true)
    expect(pattern.test(line('!see [Pasted text #12 +39 lines] "q"\nnext'))).toBe(true)
    expect(pattern.test(line('see [Pasted text #3 +38 lines] "q"\nnext'))).toBe(false)
    expect(pattern.test(line('see [Pasted text #3 +39 lines] "q"\nnext, and more'))).toBe(false)
    expect(pattern.test(`{"x":1,${line('see [Pasted text #3 +39 lines] "q"\nnext').slice(1)}`)).toBe(false)
  })

  test('pairs a recalled draft’s placeholders with its entry’s, in order', () => {
    expect(recalledIds('a [Pasted text #4 +2 lines] [Image #5]', 'a [Pasted text #1 +2 lines] [Image #2]')).toEqual(new Map([[4, 1], [5, 2]]))
    expect(recalledIds('a [Pasted text #4 +2 lines]', 'a [Pasted text #1 +3 lines]')).toBeUndefined()
    expect(recalledIds('a [Pasted text #4 +2 lines]', 'a')).toBeUndefined()
  })

  test('formedBy names the ids a change brought in, whatever their spelling', () => {
    const formed = formedBy('x [Pasted text #1 +2 lines]', 'x [Pasted text #1 +2 lines] [Image #2] [Pasted text #1 +2 lines]')
    expect(formed.map(p => p.id)).toEqual([2])
  })
})

describe('a paste as the engine keeps it', () => {
  test('normalizes line breaks, tabs and escape sequences', () => {
    expect(normalizePaste('a\r\nb\rc\nd\te\u001b[31mred\u001b[0m')).toBe('a\nb\nc\nd    ered')
    expect(newlineCount('a\r\nb\rc\nd')).toBe(3)
  })

  test('drops the ! of a paste into an empty box, which switched it to shell mode, and trims a guide', () => {
    expect(pasteContent('!ls\nmore\nlines', 'paste', true)).toBe('ls\nmore\nlines')
    expect(pasteContent('!ls\nmore\nlines', 'paste', false)).toBe('!ls\nmore\nlines')
    expect(pasteContent('  guide\ntext  ', 'guide', false)).toBe('guide\ntext')
  })

  test('fits by line count, and a placeholder with none only a text longer than 800 characters', () => {
    const [threeLines, noLines] = parsePlaceholders('[Pasted text #1 +3 lines] [Pasted text #2]')
    expect(fits('a\nb\nc\nd', threeLines!)).toBe(true)
    expect(fits('a\nb\nc', threeLines!)).toBe(false)
    expect(fits('x'.repeat(801), noLines!)).toBe(true)
    expect(fits('x'.repeat(800), noLines!)).toBe(false)
    expect(fits(`${'x'.repeat(900)}\n`, noLines!)).toBe(false)
  })
})

describe('expanding a draft for the clipboard', () => {
  /**
   * Resolves the test drafts' ids: 1 is a paste's text, 2 an image, 3
   * literal, 4 unread, anything else unknown.
   *
   * @param placeholder the placeholder
   * @returns its resolution
   */
  const resolveTest = (placeholder: Placeholder): Resolution => {
    if (placeholder.id === 1) return { kind: 'text', text: 'PASTE\nTEXT' }
    if (placeholder.id === 2) return { kind: 'image' }
    if (placeholder.id === 3) return { kind: 'literal' }
    if (placeholder.id === 4) return { kind: 'unread' }
    return { kind: 'unknown' }
  }

  test('puts each text in its place, takes images out with the space after them, keeps literal text', () => {
    const result = expand('see [Image #2] and [Pasted text #1 +1 lines], typed [Pasted text #3 +9 lines]', resolveTest)
    expect(result.text).toBe('see and PASTE\nTEXT, typed [Pasted text #3 +9 lines]')
    expect(result.images).toEqual(['[Image #2]'])
    expect(result.expanded.map(p => p.id)).toEqual([1])
    expect(result.unresolved).toEqual([])
  })

  test('expands every spelling of an id, as the engine does, and reports what it could not expand', () => {
    const result = expand('[Pasted text #1 +1 lines] [Pasted text #1 +1 lines] [Pasted text #4 +2 lines] [Pasted text #9]', resolveTest)
    expect(result.text).toBe('PASTE\nTEXT PASTE\nTEXT [Pasted text #4 +2 lines] [Pasted text #9]')
    expect(result.unresolved.map(p => p.id)).toEqual([4, 9])
  })
})

describe('rebuilding a truncation', () => {
  test('finds the middle the engine cut out, a paste in it expanded and an image moved out of it', () => {
    const pasted = lines(30, 'pasted')
    const draft = `${'h'.repeat(600)} [Pasted text #1 +29 lines] middle [Image #2] ${lines(150, 'typed')} ${'t'.repeat(600)}`
    const result = engineTruncate(draft, { 1: { type: 'text', content: pasted }, 2: { type: 'image' } }, 3)
    const [target] = parsePlaceholders(result).filter(p => p.kind === 'truncated')
    expect(target?.id).toBe(3)
    const middle = rebuild(draft, result, target!, p => (p.id === 1 ? pasted : undefined))
    // The engine's own middle: the draft with the paste expanded and the
    // image out, less its first and last 500 characters.
    const flat = draft.replace('[Pasted text #1 +29 lines]', pasted).replace('[Image #2]', '')
    expect(middle).toBe(flat.slice(500, -500))
  })

  test('gives up when a paste in the drafts is unknown, or the drafts do not line up', () => {
    const draft = `${'h'.repeat(600)} [Pasted text #1 +29 lines] ${lines(200, 'typed')}`
    const result = engineTruncate(draft, { 1: { type: 'text', content: lines(30, 'p') } }, 2)
    const [target] = parsePlaceholders(result).filter(p => p.kind === 'truncated')
    expect(rebuild(draft, result, target!, () => undefined)).toBeUndefined()
    expect(rebuild(`other text ${draft}`, result, target!, () => lines(30, 'p'))).toBeUndefined()
  })
})

describe('the codecs', () => {
  test('round-trip UTF-8 through base64, astral characters included', () => {
    const text = 'plain, é, 中文, 😀, line\nbreak'
    expect(utf8Text(base64Bytes(utf8Base64(text)))).toBe(text)
    expect(utf8Base64('hi')).toBe('aGk=')
  })

  test('ignore what is no base64 (a BOM, line breaks) and turn malformed UTF-8 into U+FFFD', () => {
    expect(utf8Text(base64Bytes('\uFEFFaGk=\r\n'))).toBe('hi')
    expect(utf8Text(new Uint8Array([0x61, 0xc3, 0x62, 0xed, 0xa0, 0x80]))).toBe('a\uFFFDb\uFFFD\uFFFD\uFFFD')
    expect(grouped(1234567)).toBe('1,234,567')
  })
})

describe('a cut', () => {
  test('puts the draft on the clipboard with its paste read back from the clipboard, and empties the box', async ($, on) => {
    const world = seat(on)
    await start($)
    await type($, world, 'fix this: ')
    world.clipboard = FOUR_LINES
    await paste($, world, '[Pasted text #1 +3 lines]')
    await type($, world, ' thanks')
    world.clipboard = 'something copied later'
    expect(await cut($)).toBeUndefined()
    expect(world.copies).toEqual([`fix this: ${FOUR_LINES_KEPT} thanks`])
    expect(world.box).toBe('')
    expect(world.toasts).toEqual(['Cut 65 characters to the clipboard, 1 paste expanded.'])
    // The reader is Windows PowerShell, printing base64.
    expect(world.runs[0]?.slice(0, 4)).toEqual(['powershell', '-NoProfile', '-NonInteractive', '-Command'])
  })

  test('pressed right after a paste, waits for the clipboard read', async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = FOUR_LINES
    await paste($, world, '[Pasted text #1 +3 lines]', { settle: false })
    const running = cut($)
    await world.clock.advance(1)
    await running
    expect(world.copies).toEqual([FOUR_LINES_KEPT])
  })

  test('with a paste whose text it never read, arms Enter: the engine’s own send expands it, and the cut drops that send', async ($, on) => {
    const world = seat(on)
    await start($)
    await type($, world, 'see ')
    world.clipboard = 'one line only'
    await paste($, world, '[Pasted text #1 +3 lines]')
    await type($, world, ' ')
    await paste($, world, '[Image #2]')
    await cut($)
    // Nothing cut yet: the draft was written over itself (out of shell mode)
    // and Enter armed, which the hint row shows.
    expect(world.copies).toEqual([])
    expect(world.fills).toEqual(['see [Pasted text #1 +3 lines] [Image #2]'])
    expect(world.toasts.at(-1)).toBe(
      'Press Enter to cut: Claude Code expands [Pasted text #1 +3 lines] only when you send, so prompt-cut takes that Enter and sends nothing. Typing cancels.',
    )
    expect(await hintText($)).toContain('[Enter cuts ✕]')
    // The person's Enter: the engine empties the box, then raises the submit
    // with the paste expanded (and the image kept as its placeholder).
    world.box = ''
    world.cursor = 0
    const result = await enter($, `see ${FOUR_LINES_KEPT} [Image #2]`)
    expect(result).toEqual({ drop: 'cut 52 characters to the clipboard, pastes expanded (prompt-cut)' })
    expect(world.copies).toEqual([`see ${FOUR_LINES_KEPT}`])
    expect(world.sent).toEqual([])
    // The engine puts the dropped draft back; the mod empties it again, the
    // image kept.
    world.box = 'see [Pasted text #1 +3 lines] [Image #2]'
    await world.clock.advance(1_000)
    expect(world.box).toBe('[Image #2]')
    expect(await hintText($)).not.toContain('[Enter cuts ✕]')
    // One Enter only: the next one sends.
    world.box = ''
    expect(await enter($, 'a new prompt')).toEqual({ text: 'a new prompt' })
    expect(world.sent).toEqual(['a new prompt'])
  })

  test('takes the arm back on a second press, an edit, the ✕ on the hint row, or after 60 s', async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = 'one line only'
    await paste($, world, '[Pasted text #1 +3 lines]')
    // A second press.
    await cut($)
    await cut($)
    expect(world.toasts.at(-1)).toBe('Cut cancelled: Enter sends again.')
    expect(await enter($, FOUR_LINES_KEPT)).toEqual({ text: FOUR_LINES_KEPT })
    // An edit of the draft.
    world.box = '[Pasted text #1 +3 lines]'
    world.cursor = world.box.length
    await cut($)
    await type($, world, '!')
    expect(await enter($, `${FOUR_LINES_KEPT}!`)).toEqual({ text: `${FOUR_LINES_KEPT}!` })
    // The ✕.
    world.box = '[Pasted text #1 +3 lines]'
    world.cursor = world.box.length
    await cut($)
    const row = await $.ui.mount({ plugin: 'prompt-cut', surface: 'terminal', component: 'PromptHint', props: HINT_PROPS })
    await row.press({ key: 'prompt-cut-disarm' })
    await row.unmount()
    expect(await enter($, FOUR_LINES_KEPT)).toEqual({ text: FOUR_LINES_KEPT })
    // The time.
    await cut($)
    await world.clock.advance(60_001)
    expect(await enter($, FOUR_LINES_KEPT)).toEqual({ text: FOUR_LINES_KEPT })
    expect(world.copies).toEqual([])
    expect(world.sent.length).toBe(4)
  })

  test('empties the restored draft whichever form the engine puts back (2.1.310: expanded)', async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = 'one line only'
    await paste($, world, '[Pasted text #1 +3 lines]')
    await cut($)
    world.box = ''
    expect(await enter($, FOUR_LINES_KEPT)).toEqual({ drop: 'cut 48 characters to the clipboard, pastes expanded (prompt-cut)' })
    // Seen live on 2.1.310: an idle drop comes back with the paste inline.
    world.box = FOUR_LINES_KEPT
    await world.clock.advance(1_000)
    expect(world.box).toBe('')
    // A box the person changed meanwhile is theirs, and stays.
    await paste($, world, '[Pasted text #1 +3 lines]')
    await cut($)
    world.box = ''
    await enter($, FOUR_LINES_KEPT)
    world.box = 'something else'
    await world.clock.advance(1_000)
    expect(world.box).toBe('something else')
  })

  test('keeps an armed Enter through the clear its own submit raises', async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = 'one line only'
    await paste($, world, '[Pasted text #1 +3 lines]')
    await cut($)
    // The submit empties the box before prompt.submit, and that change can
    // land first.
    world.box = ''
    await boxOf($).change({ text: '', cursor: 0, origin: { kind: 'clear' } })
    expect(await enter($, FOUR_LINES_KEPT)).toEqual({ drop: 'cut 48 characters to the clipboard, pastes expanded (prompt-cut)' })
    expect(world.copies).toEqual([FOUR_LINES_KEPT])
  })

  test('with a command line, cannot arm Enter (it would run the command): keeps the box once, then cuts it as written', async ($, on) => {
    const world = seat(on)
    await start($)
    await type($, world, '/review ')
    world.clipboard = 'one line only'
    await paste($, world, '[Pasted text #1 +3 lines]')
    await cut($)
    expect(world.copies).toEqual([])
    expect(world.toasts.at(-1)).toBe(
      'Kept the box: [Pasted text #1 +3 lines] hides a text prompt-cut never read, and Enter would run this command. Cut again within 5 s to cut it as written.',
    )
    await world.clock.advance(6_000)
    await cut($)
    expect(world.copies).toEqual([])
    await world.clock.advance(2_000)
    await cut($)
    expect(world.copies).toEqual(['/review [Pasted text #1 +3 lines]'])
    expect(world.toasts.at(-1)).toBe('Cut 33 characters to the clipboard; [Pasted text #1 +3 lines] left as written.')
  })

  test('takes a placeholder typed by hand, or pasted inside a short text, as the plain text it is', async ($, on) => {
    const world = seat(on)
    await start($)
    await type($, world, '"[Pasted text #1 +21 lines]" ')
    const at = world.cursor
    await boxOf($).edit({ origin: { kind: 'composer' }, text: world.box, cursor: at, start: at, end: at, inputText: 'see [Image #2] here' })
    await world.clock.advance(1)
    await cut($)
    expect(world.copies).toEqual(['"[Pasted text #1 +21 lines]" see [Image #2] here'])
    expect(world.box).toBe('')
  })

  test('leaves the images in the box, and copies the text around them', async ($, on) => {
    const world = seat(on)
    await start($)
    await type($, world, 'look at ')
    await paste($, world, '[Image #1]')
    await type($, world, ' and fix it')
    await cut($)
    expect(world.copies).toEqual(['look at and fix it'])
    expect(world.box).toBe('[Image #1]')
    expect(world.toasts.at(-1)).toBe('Cut 18 characters to the clipboard; 1 image stays in the box.')
  })

  test('cuts nothing from an empty box, blanks or images alone', async ($, on) => {
    const world = seat(on)
    await start($)
    await cut($)
    await type($, world, '   ')
    await cut($)
    expect(world.copies).toEqual([])
    expect(world.fills).toEqual([])
    expect(world.toasts).toEqual([])
  })

  test('empties nothing when no clipboard took the text', async ($, on) => {
    const world = seat(on)
    await start($)
    world.canCopy = false
    await type($, world, 'keep me')
    await cut($)
    expect(world.box).toBe('keep me')
    expect(world.toasts).toEqual(['Nothing cut: no clipboard took the text (no-clipboard).'])
  })

  test('reads the clipboard with the command the option names', { options: { clipboardCommand: 'my-reader  --text' } }, async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = 'a\nb\nc\nd'
    await paste($, world, '[Pasted text #1 +3 lines]')
    expect(world.runs).toEqual([['my-reader', '--text']])
    await cut($)
    expect(world.copies).toEqual(['a\nb\nc\nd'])
  })

  test('reads a paste that comes as an edit too, as the docs place it', async ($, on) => {
    const world = seat(on)
    await start($)
    await type($, world, 'see ')
    world.clipboard = FOUR_LINES
    await paste($, world, '[Pasted text #1 +3 lines]', { via: 'edit' })
    await cut($)
    expect(world.copies).toEqual([`see ${FOUR_LINES_KEPT}`])
  })

  test('marks a placeholder another plugin wrote as text plain text', async ($, on) => {
    const world = seat(on)
    await start($)
    const text = 'about [Pasted text #7 +2 lines] markers'
    await $.prompt.fill({ text, mode: 'replace', origin: { kind: 'plugin', name: 'other' } })
    await boxOf($).change({ text, cursor: text.length, origin: { kind: 'plugin', name: 'other' } })
    await world.clock.advance(1)
    await cut($)
    expect(world.copies).toEqual([text])
    expect(world.box).toBe('')
  })

  test('expands two pastes, each read from the clipboard at its own paste', async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = FOUR_LINES
    await paste($, world, '[Pasted text #1 +3 lines]')
    await type($, world, ' and ')
    world.clipboard = 'a\nb'
    await paste($, world, '[Pasted text #2 +1 lines]')
    await cut($)
    expect(world.copies).toEqual([`${FOUR_LINES_KEPT} and a\nb`])
    expect(world.toasts.at(-1)).toBe('Cut 56 characters to the clipboard, 2 pastes expanded.')
    expect(world.logs.filter(line => line.includes('read') && line.includes('from the clipboard')).length).toBe(2)
  })
})

describe('a recalled prompt', () => {
  /** The paste of the recalled prompt, as the paste cache keeps it. */
  const OLD_PASTE = 'old paste line 1\nold paste line 2\nold paste line 3'

  /**
   * Writes a history entry and its paste-cache file, then recalls the entry
   * into the box as the up arrow does: with its placeholders renumbered, as
   * 2.1.310 renumbers them (seen live: the entry's `#3` came back as `#4`).
   *
   * @param $ the engine
   * @param world the test world
   * @param display the entry's draft, with the entry's ids
   * @param pastedContents the entry's pasted contents, keyed by those ids
   * @param recalled the draft as the box shows it, renumbered
   */
  async function recall($: Engine, world: World, display: string, pastedContents: Record<string, unknown>, recalled: string): Promise<void> {
    world.history.push(JSON.stringify({ display, pastedContents: {}, timestamp: 1, project: 'C:\\other' }))
    world.history.push(JSON.stringify({ display, pastedContents, timestamp: 2, project: 'C:\\work' }))
    world.history.push(JSON.stringify({ display: 'a later prompt', pastedContents: {}, timestamp: 3, project: 'C:\\work' }))
    world.files[`${PASTE_CACHE}/0123456789abcdef.txt`] = OLD_PASTE
    world.box = recalled
    world.cursor = recalled.length
    await boxOf($).change({ text: recalled, cursor: recalled.length, origin: { kind: 'history' } })
  }

  test('gets its pastes from its history entry and the paste cache, over a record of this session for the same id', async ($, on) => {
    const world = seat(on)
    await start($)
    // This session pasted its own #5 of the same line count, then sent it:
    // the recall's #5 is another paste, which only the history knows.
    world.clipboard = 'new 1\nnew 2\nnew 3'
    await paste($, world, '[Pasted text #5 +2 lines]')
    world.box = ''
    world.cursor = 0
    await boxOf($).change({ text: '', cursor: 0, origin: { kind: 'clear' } })
    await recall(
      $,
      world,
      'use [Pasted text #1 +2 lines] and [Pasted text #2 +1 lines] "quoted"',
      { 1: { id: 1, type: 'text', contentHash: '0123456789abcdef' }, 2: { id: 2, type: 'text', content: 'inline\ntext' } },
      'use [Pasted text #5 +2 lines] and [Pasted text #6 +1 lines] "quoted"',
    )
    await cut($)
    expect(world.copies).toEqual([`use ${OLD_PASTE} and inline\ntext "quoted"`])
    expect(world.box).toBe('')
  })

  test('keeps an id its entry lacks as written (plain text in that prompt), and stops at a paste the cache lost', async ($, on) => {
    const world = seat(on)
    await start($)
    await recall(
      $,
      world,
      'typed [Pasted text #3 +5 lines], lost [Pasted text #4 +2 lines]',
      { 4: { id: 4, type: 'text', contentHash: 'fedcba9876543210' } },
      'typed [Pasted text #8 +5 lines], lost [Pasted text #9 +2 lines]',
    )
    await cut($)
    expect(world.copies).toEqual([])
    expect(world.toasts.at(-1)).toBe(
      'Press Enter to cut: Claude Code expands [Pasted text #9 +2 lines] only when you send, so prompt-cut takes that Enter and sends nothing. Typing cancels.',
    )
  })

  test('falls back to the session records when the history has no entry for the draft', async ($, on) => {
    const world = seat(on)
    await start($)
    world.clipboard = FOUR_LINES
    await paste($, world, '[Pasted text #1 +3 lines]')
    // Stepping back out of the history puts the person's own draft back.
    await boxOf($).change({ text: world.box, cursor: world.box.length, origin: { kind: 'history' } })
    await cut($)
    expect(world.copies).toEqual([FOUR_LINES_KEPT])
  })
})

describe('a truncated draft', () => {
  test('a long fill by another plugin is rebuilt from the text it wrote', async ($, on) => {
    const world = seat(on)
    await start($)
    const long = lines(250, 'queued prompt')
    expect(long.length).toBeGreaterThan(10_000)
    await $.prompt.fill({ text: long, mode: 'replace', origin: { kind: 'plugin', name: 'prompt-queue' } })
    const truncated = engineTruncate(long, {}, 1)
    world.box = truncated
    world.cursor = truncated.length
    await boxOf($).change({ text: truncated, cursor: truncated.length, origin: { kind: 'plugin', name: 'prompt-queue' } })
    await world.clock.advance(1)
    await cut($)
    expect(world.copies).toEqual([long])
    expect(world.box).toBe('')
  })

  test('a draft typed past 10,000 characters is rebuilt from the draft before, its paste included', async ($, on) => {
    const world = seat(on)
    await start($)
    const pasted = lines(20, 'pasted')
    world.clipboard = pasted
    await type($, world, 'start ')
    await paste($, world, '[Pasted text #1 +19 lines]')
    // One burst of text past the limit, as a short paste of typing lands.
    const at = world.cursor
    await boxOf($).edit({ origin: { kind: 'composer' }, text: world.box, cursor: at, start: at, end: at, inputText: ` ${lines(220, 'typed')}` })
    await world.clock.advance(1)
    const before = world.box
    const truncated = engineTruncate(before, { 1: { type: 'text', content: pasted } }, 2)
    world.box = truncated
    world.cursor = truncated.length
    await boxOf($).change({ text: truncated, cursor: truncated.length, origin: { kind: 'engine' } })
    await world.clock.advance(1)
    await cut($)
    expect(world.copies).toEqual([before.replace('[Pasted text #1 +19 lines]', pasted)])
  })
})

describe('the command', () => {
  test('is registered to run mid-turn and left out of the typeahead', async ($, on) => {
    const world = seat(on)
    await start($)
    expect(world.commands).toEqual([{ name: 'prompt-cut', immediate: true }])
    const described = await $.command.describe({
      command: 'prompt-cut',
      description: 'Cut',
      isHidden: false,
      immediate: true,
      provider: { plugin: 'prompt-cut', tier: 'user' },
    })
    expect(described.isHidden).toBe(true)
    expect(world.logs).toEqual([])
  })
})
