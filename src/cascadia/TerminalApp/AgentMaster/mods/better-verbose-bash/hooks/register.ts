import type { EngineInterface, Register, RenderElement, RenderInput } from 'claude-code'

/**
 * better-verbose-bash: in the views where Claude Code draws a shell call in
 * full, the reason the model gave for the call becomes the row's header.
 *
 * Claude Code draws a call as `● Bash(git status)`, and the `description`
 * the model wrote for it ("Show working tree status") shows only in the
 * permission dialog. In the full views (`--verbose`, Verbose output in
 * `/config`, the ctrl+o transcript) this mod draws the row as
 *
 *     ● Show working tree status
 *       Bash(git status)
 *       ⎿  On branch master
 *
 * The second line on is Claude Code's own row (its command, wait and
 * progress lines), drawn by the engine; the mod adds the header and paints
 * blanks over the engine's bullet, so the row keeps one bullet. Display
 * only: the transcript and what the model reads back do not change.
 */

/**
 * The tools whose rows carry a shell `command` and the model's
 * `description` of it: `Bash`, and `PowerShell` where the build has it.
 *
 * A row of any other tool never reaches the hook (the matcher in
 * {@link register} names these), so it costs nothing.
 */
export const SHELL_TOOLS = ['Bash', 'PowerShell'] as const

/**
 * The most characters of a description the header draws; a longer one ends
 * in `…`. The model writes a few words; the cap only stops a runaway
 * description from pushing the command a screen down.
 */
export const DESCRIPTION_CHARS = 300

/** The bullet Claude Code draws at the head of a row on Windows and Linux. */
export const BULLET = '●'

/**
 * The bullet Claude Code draws at the head of a row on macOS. The header
 * must use the same glyph, or its bullet stands out among the rows the
 * engine drew.
 */
export const MAC_BULLET = '⏺'

/**
 * Columns the engine's bullet takes at the left of its row: the glyph and
 * the space after it (Claude Code's bullet box has `minWidth: 2`). The mod
 * blanks exactly these columns of the engine's bullet line, and its own
 * header's bullet box takes the same width, so the description starts in
 * the column where the engine's command does.
 */
export const BULLET_COLUMNS = 2

/**
 * The look of the header's bullet: the colors Claude Code gives its own
 * bullet for the same call state, so the header reads as the engine's.
 */
export type DotStyle = {
  /** `success` (green) for a call that succeeded, `error` (red) for one that failed. */
  readonly color?: 'success' | 'error'
  /** Dim while the call runs or waits its turn, as the engine draws it. */
  readonly dimColor?: true
}

/** The call state of a row, as its `ToolUse` props carry it. */
export type CallState = {
  /** True while the call runs. */
  readonly isRunning: boolean
  /** True when the call ended in an error (a refusal at the dialog is one). */
  readonly isErrored: boolean
  /** True when an abort ended the call (Esc, or a plugin's `$.turn.abort`). */
  readonly isInterrupted: boolean
  /** The stored result once the call resolved; undefined while it runs or waits. */
  readonly output?: unknown
}

/**
 * What the module keeps in one process. A hot reload runs {@link register}
 * again and starts it empty, which only costs one `$.env` read.
 */
type State = {
  /**
   * The bullet glyph of this machine, read once ({@link bulletOf}). Kept as
   * the promise, so the rows of the first frame share one read.
   */
  bullet: Promise<string> | undefined
}

/** The `e` of the hook: a `ToolUse` row drawn in the terminal. */
type ToolUseEvent = RenderInput<'ToolUse', 'terminal'>

/**
 * Turns a text into one drawable line: control characters and runs of
 * white space become one space, and the ends are trimmed. A line longer than
 * `max` is cut and ends in `…`.
 *
 * @param text the text, as the model wrote it
 * @param max the most characters to keep, the `…` included; at least 1
 * @returns the line; `""` when nothing printable is left
 */
export function lineOf(text: string, max: number): string {
  // Control characters first: a raw newline or tab in a header would break
  // the row into lines the engine's layout does not expect.
  const line = text.replace(/[\u0000-\u001f\u007f-\u009f]/g, ' ').replace(/\s+/g, ' ').trim()
  return line.length <= max ? line : `${line.slice(0, Math.max(0, max - 1)).trimEnd()}…`
}

/**
 * Reads the model's `description` of a shell call, as one header line.
 *
 * @param input the call's input as the row carries it; `unknown`, because a
 *   row's props type every tool's input at once
 * @returns the description as one line of at most {@link DESCRIPTION_CHARS};
 *   undefined when the call has none, or it is not a string with something
 *   printable in it (the row then stays as Claude Code draws it)
 */
export function descriptionOf(input: unknown): string | undefined {
  if (typeof input !== 'object' || input === null) return undefined
  const value: unknown = Reflect.get(input, 'description')
  if (typeof value !== 'string') return undefined
  const line = lineOf(value, DESCRIPTION_CHARS)
  return line === '' ? undefined : line
}

/**
 * Picks the header bullet's colors from the call's state, the way Claude
 * Code colors its own bullet: dim while the call has not resolved, red when
 * it failed or was interrupted, green when it succeeded.
 *
 * A call with no stored result that is not running is one waiting its turn
 * (a later call of a parallel batch): dim, like the engine's queued row.
 *
 * @param state the row's call state
 * @returns the bullet's colors
 */
export function dotStyleOf(state: CallState): DotStyle {
  if (state.isRunning) return { dimColor: true }
  if (state.isErrored || state.isInterrupted) return { color: 'error' }
  if (state.output === undefined) return { dimColor: true }
  return { color: 'success' }
}

/**
 * Tells the bullet glyph Claude Code uses on this machine from the home
 * folder: macOS keeps homes under `/Users/` and draws {@link MAC_BULLET};
 * Windows (`C:\Users\…`, or Git Bash's `/c/Users/…`) and Linux (`/home/…`)
 * draw {@link BULLET}.
 *
 * @param home the `HOME` variable; undefined when it is unset (Windows
 *   outside Git Bash)
 * @returns the glyph
 */
export function bulletFor(home: string | undefined): string {
  return home !== undefined && home.startsWith('/Users/') ? MAC_BULLET : BULLET
}

/**
 * Resolves this machine's bullet glyph ({@link bulletFor}), reading `HOME`
 * once per process.
 *
 * A failed read (a host with no environment for plugins, a test kit) draws
 * {@link BULLET}: a wrong glyph on a Mac beats a row that does not draw.
 *
 * @param $ the dispatch's engine interface
 * @param state the process memory from {@link register}
 * @returns the glyph; never rejects
 */
function bulletOf($: EngineInterface, state: State): Promise<string> {
  state.bullet ??= $.env.get('HOME').then(bulletFor, () => BULLET)
  return state.bullet
}

/**
 * The line of Claude Code's own row that holds its bullet: line 1, under the
 * one blank line of top margin the engine gives every tool row (a standalone
 * row and a row of an unfolded call group alike, 2.1.308).
 */
export const BULLET_LINE = 1

/**
 * Tells whether a drawing is Claude Code's own row: the engine reference
 * (`{ type: 'engine', ref }`) that `next(e)` resolves to when no plugin
 * seated beneath this mod drew the row itself.
 *
 * Only that row takes the header. The layout ({@link headerTree}) pulls the
 * row up over its one blank line of top margin and paints blanks over its
 * bullet on line {@link BULLET_LINE}; both hold for core's drawing alone. A
 * plugin beneath that drew the row its own way breaks both: chat-view answers
 * a call its view hides with an empty Box, where a header would stand alone,
 * and puts a dim `# …` line above a shell row in its Full and Max views, where
 * the pull-up would lay that line over the header and the blanks would miss
 * the bullet. Such a drawing is handed on as that plugin made it.
 *
 * A plugin sits beneath this mod when `enabledPlugins` lists it later, and
 * every installed plugin sits beneath this mod when it is loaded from a
 * folder (`--plugin-dir`, `CLAUDE_CODE_PLUGIN_DIRS`, Agentmaster's bundled
 * copy), so this check is what keeps the mod right in any seat.
 *
 * @param theirs what the hook's `next(e)` resolved to
 * @returns true for core's own drawing; false for any tree a plugin built
 */
export function isCoreRow(theirs: RenderElement): boolean {
  return theirs.type === 'engine'
}

/**
 * Draws the row: a bulleted header line with the description, and Claude
 * Code's own row under it, its bullet painted over with blanks.
 *
 * The layout, line by line:
 * - The outer Box keeps one blank line above the row, as every transcript
 *   row has.
 * - The header: the bullet in a 2-column box, then the description,
 *   wrapping under itself when it is long.
 * - Claude Code's row, in a Box with `marginTop: -1`: that pulls the row up
 *   over its own top margin, so its first line, `Bash(command)`, sits right
 *   under the header. A row drawn without that margin would draw over the
 *   header's last line; Claude Code draws one only after a speaker label
 *   (the views of agent teams).
 * - Beside the row, a Box placed absolutely on the row's bullet (line
 *   {@link BULLET_LINE}, columns 0 and 1), painted after the row, writes two
 *   blanks over the bullet and the space after it. The rest of the row (its
 *   command, its wrapped lines, the `⎿` of a wait or progress line, all at
 *   column 2 or later) is untouched.
 *
 * Why an overlay and not a clip: Claude Code refuses a tree that puts its
 * row under a Box with `overflow`, `position`, `display`, a size or an
 * offset ("engine node under a Box with prop …"): the engine's row must be
 * drawn whole. A Box beside the row, not around it, may take them.
 *
 * @param $ the engine interface
 * @param e the row's render event; its surface picks the element table and
 *   its props the bullet's colors
 * @param theirs Claude Code's own row, as the hook's `next(e)` resolved it;
 *   only core's drawing comes here ({@link isCoreRow})
 * @param description the header's text, one line ({@link descriptionOf})
 * @param bullet the bullet glyph ({@link bulletOf})
 * @returns the row's tree
 */
export function headerTree($: EngineInterface, e: ToolUseEvent, theirs: RenderElement, description: string, bullet: string): RenderElement {
  const { Box, Text } = $.ui.resolve(e)
  return Box({
    flexDirection: 'column',
    marginTop: 1,
    children: [
      Box({
        flexDirection: 'row',
        children: [
          Box({ minWidth: BULLET_COLUMNS, flexShrink: 0, children: [Text({ ...dotStyleOf(e.props), children: [bullet] })] }),
          // `bold`, not 2.1.308's `weight`: builds before it refuse a Text
          // with `weight` (the whole tree, so the header would not draw),
          // and 2.1.308 still reads `bold` (as `weight: 'semibold'`, which
          // the terminal draws bold).
          Text({ bold: true, wrap: 'wrap', children: [description] }),
        ],
      }),
      Box({
        flexDirection: 'column',
        marginTop: -1,
        children: [
          theirs,
          // After the row, so it paints over it: blanks where the bullet was.
          Box({
            position: 'absolute',
            top: BULLET_LINE,
            left: 0,
            children: [Text({ children: [' '.repeat(BULLET_COLUMNS)] })],
          }),
        ],
      }),
    ],
  })
}

/**
 * Registers the mod's one hook: a `ui.render` hook on the terminal's
 * `ToolUse` rows of {@link SHELL_TOOLS} that Claude Code draws in full.
 *
 * The matcher does the choosing: `isExpanded: true` is the engine's own word
 * for a row drawn in full (`--verbose`, Verbose output, the ctrl+o
 * transcript, and the rows of a call group a plugin unfolded); a compact row
 * leaves it false or absent and never reaches the hook. Other surfaces
 * (desktop, VS Code, phones) draw their own rows, with no bullet column to
 * clip, so they are left alone too. A row that a plugin seated beneath drew
 * its own way keeps that plugin's drawing ({@link isCoreRow}).
 *
 * @param on adds a hook
 */
export const register: Register = on => {
  const state: State = { bullet: undefined }

  on(
    'ui.render',
    { component: 'ToolUse', surface: 'terminal', props: { tool: [...SHELL_TOOLS], isExpanded: true } },
    async ($, e, next) => {
      const description = descriptionOf(e.props.input)
      // No description, or none with anything to show: Claude Code's row as
      // it is, so a call never loses its bullet for an empty header.
      if (description === undefined) return next(e)
      // The engine's row and the glyph at once: the first row of a process
      // waits on the one `$.env` read, never on the two in turn.
      const [theirs, bullet] = await Promise.all([next(e), bulletOf($, state)])
      // A row a plugin beneath drew its own way (hidden, or with lines of its
      // own) keeps that drawing: the header's layout fits core's row alone.
      if (!isCoreRow(theirs)) return theirs
      return headerTree($, e, theirs, description, bullet)
    },
  )
}
