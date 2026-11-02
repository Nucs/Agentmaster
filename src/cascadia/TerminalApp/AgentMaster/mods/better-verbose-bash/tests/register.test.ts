import type { On, RenderElement, RenderPropsOf } from 'claude-code'
import { describe, expect, mock, test, type Engine } from 'claude-code/testing'
import {
  BULLET,
  BULLET_COLUMNS,
  BULLET_LINE,
  bulletFor,
  DESCRIPTION_CHARS,
  descriptionOf,
  dotStyleOf,
  isCoreRow,
  lineOf,
  MAC_BULLET,
} from '../hooks/register'

/** A finished Bash call drawn in full, with the model's description. */
const BASH_ROW: RenderPropsOf['ToolUse'] = {
  tool_use_id: 'toolu_bash',
  tool: 'Bash',
  input: { command: 'git status', description: 'Show working tree status' },
  isRunning: false,
  isErrored: false,
  isInterrupted: false,
  isExpanded: true,
  output: { stdout: 'On branch master', stderr: '', interrupted: false },
}

/**
 * Claude Code's own row exactly as the engine hands it to a hook: the engine
 * reference to core's drawing of the original props. The test kit draws no
 * row of its own, so a test's hook answers with this to stand for core.
 */
const CORE: RenderElement = { type: 'engine', ref: 0 }

/**
 * Stands in for Claude Code's own `ToolUse` row beneath the plugin: the
 * engine reference ({@link CORE}), the one drawing the mod puts its header
 * over. Its text is core's to draw, so it holds no Text a test could read.
 *
 * @param on the test's `on`
 */
function seatCore(on: On): void {
  on('ui.render', { component: 'ToolUse' }, () => CORE)
}

/**
 * Stands in for the row beneath the plugin as one readable Text naming the
 * tool and the command (`engine:Bash(git status)`), for the rows the mod
 * hands on unchanged: a test then sees that the row it gave back is the one
 * from beneath, whatever drew it.
 *
 * @param on the test's `on`
 */
function seatLabel(on: On): void {
  on('ui.render', { component: 'ToolUse' }, ($, e) => {
    const command: unknown = typeof e.props.input === 'object' && e.props.input !== null ? Reflect.get(e.props.input, 'command') : undefined
    return $.ui.resolve(e).Text({ children: [`engine:${e.props.tool}(${String(command)})`] })
  })
}

/**
 * Draws one tool row through the mod.
 *
 * @param $ the engine, as the test holds it
 * @param props the row's props
 * @param surface where it draws; the terminal unless a test names another
 * @returns the drawing
 */
function row($: Engine, props: RenderPropsOf['ToolUse'], surface: 'terminal' | 'desktop' | 'vscode' | 'mobile' = 'terminal') {
  return $.ui.mount({ plugin: 'better-verbose-bash', surface, component: 'ToolUse', props })
}

/** One element of a drawn tree, as a test reads it: its props and children. */
type Node = { readonly props: Readonly<Record<string, unknown>>; readonly children: readonly unknown[] }

/**
 * Reads an element of a drawn tree as a {@link Node}.
 *
 * @param element an element of `drawn()`'s tree, or one of its children
 * @returns the element's props and children
 * @throws when the element is no tree element (an engine reference, a
 *   string): the test expected one of the mod's boxes there
 */
function nodeOf(element: unknown): Node {
  const props: unknown = typeof element === 'object' && element !== null ? Reflect.get(element, 'props') : undefined
  const children: unknown = typeof element === 'object' && element !== null ? Reflect.get(element, 'children') : undefined
  if (typeof props !== 'object' || props === null) throw new Error(`not a tree element: ${JSON.stringify(element)}`)
  return { props: props as Record<string, unknown>, children: Array.isArray(children) ? children : [] }
}

/**
 * Reads the texts a drawing holds, in drawing order. Core's row is an engine
 * reference, not a Text, so it adds none.
 *
 * @param drawing what {@link row} resolved to
 * @returns each Text's text
 */
async function textsOf(drawing: Awaited<ReturnType<typeof row>>): Promise<string[]> {
  return (await drawing.findAll({ type: 'Text' })).map(text => text.text)
}

describe('the header', () => {
  test("a Bash call drawn in full: the description is the bulleted header, and core's row follows with blanks over its bullet", async ($, on) => {
    seatCore(on)
    const drawing = await row($, BASH_ROW)
    expect(await textsOf(drawing)).toEqual([BULLET, 'Show working tree status', '  '])
    // The shape the live terminal was checked with: a blank line above, the
    // engine row pulled up over its own top margin, and after it (so painted
    // over it) two blanks placed on its bullet, line 1, columns 0 and 1.
    const tree = nodeOf(await drawing.drawn())
    expect(tree.props).toMatchObject({ flexDirection: 'column', marginTop: 1 })
    const header = nodeOf(tree.children[0])
    const engine = nodeOf(tree.children[1])
    expect(header.props).toMatchObject({ flexDirection: 'row' })
    expect(engine.props).toEqual({ flexDirection: 'column', marginTop: -1 })
    // Core's row first, with no Box of the mod around it that Claude Code
    // would refuse (no overflow, position, display, size or offset).
    expect(engine.children[0]).toEqual(CORE)
    const blanks = nodeOf(engine.children[1])
    expect(blanks.props).toEqual({ position: 'absolute', top: BULLET_LINE, left: 0 })
    expect(engine.children).toHaveLength(2)
  })

  test('the header text is bold and wraps under itself; the bullet sits in a 2-column box', async ($, on) => {
    seatCore(on)
    const drawing = await row($, BASH_ROW)
    expect((await drawing.find({ type: 'Text', text: 'Show working tree status' }))?.props).toMatchObject({ bold: true, wrap: 'wrap' })
    const header = nodeOf(nodeOf(await drawing.drawn()).children[0])
    expect(nodeOf(header.children[0]).props).toMatchObject({ minWidth: BULLET_COLUMNS, flexShrink: 0 })
  })

  test('a PowerShell call is drawn the same way', async ($, on) => {
    seatCore(on)
    const drawing = await row($, { ...BASH_ROW, tool: 'PowerShell', input: { command: 'Get-ChildItem', description: 'List files' } })
    expect(await textsOf(drawing)).toEqual([BULLET, 'List files', '  '])
    expect(nodeOf(nodeOf(await drawing.drawn()).children[1]).children[0]).toEqual(CORE)
  })

  test('a description with line breaks, tabs or a runaway length draws as one capped line', async ($, on) => {
    seatCore(on)
    const long = 'x'.repeat(DESCRIPTION_CHARS + 50)
    const broken = await row($, { ...BASH_ROW, input: { command: 'ls', description: '  List\nthe\tfiles  ' } })
    expect(await textsOf(broken)).toEqual([BULLET, 'List the files', '  '])
    const capped = await row($, { ...BASH_ROW, tool_use_id: 'toolu_long', input: { command: 'ls', description: long } })
    const header = (await textsOf(capped))[1] ?? ''
    expect(header).toHaveLength(DESCRIPTION_CHARS)
    expect(header.endsWith('…')).toBe(true)
  })
})

describe('rows left as Claude Code draws them', () => {
  test('a compact row (isExpanded false or absent) never reaches the hook', async ($, on) => {
    seatLabel(on)
    expect(await textsOf(await row($, { ...BASH_ROW, isExpanded: false }))).toEqual(['engine:Bash(git status)'])
    const { isExpanded: _dropped, ...compact } = BASH_ROW
    expect(await textsOf(await row($, { ...compact, tool_use_id: 'toolu_compact' }))).toEqual(['engine:Bash(git status)'])
  })

  test('a call with no description, an empty one, or one of white space alone keeps its own row', async ($, on) => {
    seatLabel(on)
    expect(await textsOf(await row($, { ...BASH_ROW, input: { command: 'ls' } }))).toEqual(['engine:Bash(ls)'])
    expect(await textsOf(await row($, { ...BASH_ROW, tool_use_id: 'toolu_2', input: { command: 'ls', description: '' } }))).toEqual([
      'engine:Bash(ls)',
    ])
    expect(await textsOf(await row($, { ...BASH_ROW, tool_use_id: 'toolu_3', input: { command: 'ls', description: ' \n\t ' } }))).toEqual([
      'engine:Bash(ls)',
    ])
  })

  test('other tools keep their rows, a description in their input or not', async ($, on) => {
    seatLabel(on)
    const agent = { ...BASH_ROW, tool: 'Agent', input: { description: 'Explore the repo', prompt: 'look' } }
    expect(await textsOf(await row($, agent))).toEqual(['engine:Agent(undefined)'])
  })

  test('other surfaces keep their rows: no bullet column to clip there', async ($, on) => {
    seatLabel(on)
    for (const surface of ['desktop', 'vscode', 'mobile'] as const) {
      expect(await textsOf(await row($, { ...BASH_ROW, tool_use_id: `toolu_${surface}` }, surface))).toEqual(['engine:Bash(git status)'])
    }
  })
})

describe('a plugin seated beneath that drew the row its own way', () => {
  test('a row it hid (an empty Box, as chat-view answers a call its view leaves out) stays hidden: no header over nothing', async ($, on) => {
    on('ui.render', { component: 'ToolUse' }, ($, e) => $.ui.resolve(e).Box({}))
    const drawing = await row($, BASH_ROW)
    expect(await textsOf(drawing)).toEqual([])
    const tree = await drawing.drawn()
    expect(Reflect.get(tree, 'type')).toBe('Box')
    expect(nodeOf({ props: {}, ...tree }).children).toEqual([])
  })

  test("a row it redrew (a dim # line above core's row, as chat-view's Full and Max views draw a shell call) keeps that drawing", async ($, on) => {
    on('ui.render', { component: 'ToolUse' }, ($, e) => {
      const { Box, Text } = $.ui.resolve(e)
      return Box({ flexDirection: 'column', children: [Box({ marginLeft: 2, children: [Text({ dimColor: true, children: ['# Show working tree status'] })] }), CORE] })
    })
    const drawing = await row($, BASH_ROW)
    // No bullet and no header of the mod: its pull-up would lay the # line
    // over the header, and its blanks would land on the row's margin line.
    expect(await textsOf(drawing)).toEqual(['# Show working tree status'])
    const tree = nodeOf(await drawing.drawn())
    expect(tree.props).toEqual({ flexDirection: 'column' })
    expect(tree.children[1]).toEqual(CORE)
  })

  test('a row it drew as plain text keeps that text', async ($, on) => {
    seatLabel(on)
    expect(await textsOf(await row($, BASH_ROW))).toEqual(['engine:Bash(git status)'])
  })
})

describe('the bullet', () => {
  test("it takes the engine's colors for the call's state", async ($, on) => {
    seatCore(on)
    /**
     * Draws a row and reads its header bullet's props.
     *
     * @param props the row's props, a call state per test line
     * @returns the bullet Text's props; undefined when the row drew no bullet of the mod
     */
    const bulletOf = async (props: RenderPropsOf['ToolUse']) => (await (await row($, props)).find({ type: 'Text', text: BULLET }))?.props
    expect(await bulletOf({ ...BASH_ROW, tool_use_id: 'toolu_ok' })).toMatchObject({ color: 'success' })
    const running = await bulletOf({ ...BASH_ROW, tool_use_id: 'toolu_run', isRunning: true, output: undefined })
    expect(running).toMatchObject({ dimColor: true })
    expect(running).not.toHaveProperty('color')
    expect(await bulletOf({ ...BASH_ROW, tool_use_id: 'toolu_err', isErrored: true, output: 'Exit code 1' })).toMatchObject({ color: 'error' })
    expect(await bulletOf({ ...BASH_ROW, tool_use_id: 'toolu_int', isInterrupted: true, output: 'Interrupted' })).toMatchObject({ color: 'error' })
    expect(await bulletOf({ ...BASH_ROW, tool_use_id: 'toolu_wait', output: undefined })).toMatchObject({ dimColor: true })
  })

  test('on a Mac (HOME under /Users/) the header uses the bullet Claude Code draws there', async ($, on) => {
    seatCore(on)
    mock.env(on, { HOME: '/Users/eli' })
    expect(await textsOf(await row($, BASH_ROW))).toEqual([MAC_BULLET, 'Show working tree status', '  '])
  })

  test('on Windows and Linux, and where HOME cannot be read, the header uses ●', async ($, on) => {
    seatCore(on)
    mock.env(on, { HOME: '/c/Users/eli' })
    expect((await textsOf(await row($, BASH_ROW)))[0]).toBe(BULLET)
  })
})

describe('the parts', () => {
  test('lineOf: control characters and runs of space become one space; a long line is cut with …', () => {
    expect(lineOf(' a\r\n b\u0007c\t ', 20)).toBe('a b c')
    expect(lineOf('abcdef', 4)).toBe('abc…')
    expect(lineOf('abcd', 4)).toBe('abcd')
    expect(lineOf('ab   cdef', 4)).toBe('ab…')
  })

  test('descriptionOf: a string with something printable, as one line; anything else is none', () => {
    expect(descriptionOf({ command: 'ls', description: 'List files' })).toBe('List files')
    expect(descriptionOf({ command: 'ls' })).toBeUndefined()
    expect(descriptionOf({ command: 'ls', description: 7 })).toBeUndefined()
    expect(descriptionOf({ command: 'ls', description: '\n' })).toBeUndefined()
    expect(descriptionOf('ls')).toBeUndefined()
    expect(descriptionOf(null)).toBeUndefined()
  })

  test("dotStyleOf: running and waiting are dim, failed and interrupted red, the rest green", () => {
    const done = { isRunning: false, isErrored: false, isInterrupted: false, output: {} }
    expect(dotStyleOf(done)).toEqual({ color: 'success' })
    expect(dotStyleOf({ ...done, isRunning: true, output: undefined })).toEqual({ dimColor: true })
    expect(dotStyleOf({ ...done, output: undefined })).toEqual({ dimColor: true })
    expect(dotStyleOf({ ...done, isErrored: true })).toEqual({ color: 'error' })
    expect(dotStyleOf({ ...done, isInterrupted: true })).toEqual({ color: 'error' })
  })

  test('bulletFor: only a home under /Users/ is a Mac', () => {
    expect(bulletFor('/Users/eli')).toBe(MAC_BULLET)
    expect(bulletFor('/c/Users/eli')).toBe(BULLET)
    expect(bulletFor('C:\\Users\\eli')).toBe(BULLET)
    expect(bulletFor('/home/eli')).toBe(BULLET)
    expect(bulletFor(undefined)).toBe(BULLET)
  })

  test("isCoreRow: only the engine's reference is core's row; any tree a plugin built is not", () => {
    expect(isCoreRow(CORE)).toBe(true)
    expect(isCoreRow({ type: 'engine', ref: 3 })).toBe(true)
    expect(isCoreRow({ type: 'Box' })).toBe(false)
    expect(isCoreRow({ type: 'Box', children: [CORE] })).toBe(false)
    expect(isCoreRow({ type: 'Text', children: ['engine:Bash(ls)'] })).toBe(false)
  })
})
