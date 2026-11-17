import type { On, ProcessRunResult, RenderElement, RenderPropsOf, RenderSurface, RenderViewport } from 'claude-code'
import type { Engine, MockClock, Mounted } from 'claude-code/testing'
import { describe, expect, mock, test } from 'claude-code/testing'

import { keepNewest } from '../hooks/register'

/** The folder the Windows test session runs in: its working folder, root and repository top. */
const PROJECT = 'C:/proj'

/** The home folder the Windows test environment names (USERPROFILE). */
const HOME = 'C:/Users/me'

/** The folder the macOS test session runs in. */
const MAC_PROJECT = '/Users/me/proj'

/** The surfaces on this machine, where a file link opens the file. */
const LOCAL_SURFACES = ['terminal', 'desktop', 'vscode'] as const satisfies readonly RenderSurface[]

/** The fullscreen terminal, which reports clicks: a reply with a file link is the mod's own Markdown there. */
const FULLSCREEN: RenderViewport = { columns: 120, rows: 40, isFullscreen: true }

/** The terminal's main screen, which reports no clicks: replies stay the engine's own rows there. */
const MAIN_SCREEN: RenderViewport = { columns: 120, rows: 40, isFullscreen: false }

/** The machine a test session runs on: its folder and the environment `$.env.get` reads. */
type Machine = {
  /** The working folder, project root and (with a repository) its top. */
  project: string
  /** The variables set; every other name reads as unset. */
  env: Readonly<Record<string, string>>
  /**
   * Real paths `$.fs.stat(path, { resolve: true })` answers, by
   * {@link canonical} path: plays macOS's `/var` link to `/private/var`.
   * A path not listed resolves to itself.
   */
  realPaths?: Readonly<Record<string, string>>
  /**
   * Kinds `$.fs.stat` answers, by {@link canonical} path: plays a stray
   * `claude` FILE in the temp root. A path not listed is a folder.
   */
  kinds?: Readonly<Record<string, 'file' | 'dir'>>
}

/** The session id the test engine answers: it names the scratchpad's folder. */
const SESSION_ID = 'sess-1'

/** Windows under Windows Terminal (or Agentmaster), which sets WT_SESSION: hyperlinks on. */
const WINDOWS: Machine = { project: PROJECT, env: { USERPROFILE: HOME, WT_SESSION: 'test-tab' } }

/** macOS in iTerm2: hyperlinks on; HOME, no USERPROFILE. */
const MAC: Machine = { project: MAC_PROJECT, env: { HOME: '/Users/me', TERM_PROGRAM: 'iTerm.app', TERM_PROGRAM_VERSION: '3.5.10' } }

/** macOS in Apple Terminal, which draws no hyperlinks. */
const MAC_TERMINAL: Machine = { project: MAC_PROJECT, env: { HOME: '/Users/me', TERM_PROGRAM: 'Apple_Terminal', TERM_PROGRAM_VERSION: '455' } }

/** What the test world holds and records; tests change it between draws. */
type World = {
  /** The machine the session runs on. */
  machine: Machine
  /** Files and folders on the test disk, by {@link canonical} spelling. */
  disk: Set<string>
  /** What `git ls-files` lists, relative to the project; undefined plays a folder outside any repository. */
  repo: string[] | undefined
  /** What `git ls-files --deleted` lists: tracked files gone from the disk, which `repo` still holds. */
  deleted: string[]
  /** How many git commands the mod ran: none may run outside a repository (a Mac without git shows a dialog). */
  gitCalls: number
  /**
   * While set, every git command waits for it before it answers, with the
   * list it read when it was called: plays a slow `git ls-files`.
   */
  gitHold?: Promise<void>
  /** Every text the engine was handed at the AssistantMessage site, in draw order. */
  drawn: string[]
  /** The mocked clock, which answers `$.clock.now` and fires `$.clock.every`. */
  clock: MockClock
  /** The mod's `$.ui.log` lines (its caught failures), which every test expects empty. */
  logs: string[]
  /**
   * Every command the mod ran that was not git: the openers of pressed links,
   * argv whole, then `FILE_LINKS_TARGET=<value>` when the run set that variable.
   */
  opened: string[][]
  /** The exit status and error text the opener stand-in answers with; 0 and none when absent. */
  openerFails?: { exitCode: number; stderr: string }
  /** Every `$.ui.toast` line the mod showed. */
  toasts: string[]
  /**
   * What the AssistantMessage stand-in draws: `engine` answers core's own row
   * (`{ type: 'engine', ref: 0 }`), as Claude Code does; `tree` answers a
   * plugin's tree, as a plugin seated beneath file-links that draws the row
   * its own way would.
   */
  replyRow: 'engine' | 'tree'
}

/** Core's own row, as `next(e)` resolves to when no plugin beneath draws it. */
const CORE: RenderElement = { type: 'engine', ref: 0 }

/**
 * Spells a path one way, as the test disk keys it. The engine hands fs hooks
 * the path made absolute for the machine the kit runs on, so on this Windows
 * host `C:/x` may arrive as `C:\x` and a macOS `/x` as `C:\x`: the drive is
 * dropped, which keeps both platforms' paths comparable.
 *
 * @param path a path in any spelling
 * @returns it with `/` separators, no drive, no trailing separator, lower-cased
 */
function canonical(path: string): string {
  return path.replace(/\\/g, '/').replace(/^[A-Za-z]:/, '').replace(/\/$/, '').toLowerCase()
}

/**
 * Builds a `$.process.run` answer.
 *
 * @param exitCode the exit status
 * @param stdout what the child wrote
 * @returns the result, both streams whole
 */
function ran(exitCode: number, stdout: string): ProcessRunResult {
  return { exitCode, stdout, stderr: exitCode === 0 ? '' : 'fatal: not a git repository', isStdoutTruncated: false, isStderrTruncated: false }
}

/**
 * Seats the stand-ins for core beneath the plugin: the session's folders, the
 * environment, the disk, git, the transcript, the tools, and the
 * AssistantMessage site, whose stand-in records the text it is handed.
 *
 * @param on the registrar whose hooks stand for core
 * @param files the files and folders on the test disk
 * @param repo what git lists, relative to the project; undefined for no
 *   repository (then the project has no `.git` either)
 * @param deleted what `git ls-files --deleted` lists
 * @param machine the platform and environment; Windows Terminal by default
 * @returns the world those hooks read and fill in
 */
function seat(on: On, files: readonly string[], repo: string[] | undefined, deleted: string[] = [], machine: Machine = WINDOWS): World {
  const disk = new Set(files.map(canonical))
  // A repository is a folder holding `.git`: the mod looks for it before git runs.
  if (repo !== undefined) disk.add(canonical(`${machine.project}/.git`))
  const world: World = {
    machine,
    disk,
    repo,
    deleted,
    gitCalls: 0,
    drawn: [],
    clock: mock.clock(on),
    logs: [],
    opened: [],
    toasts: [],
    replyRow: 'engine',
  }
  on('session.cwd', () => ({ value: machine.project }))
  on('session.root', () => ({ value: machine.project }))
  on('env.get', ($, e) => ({ value: machine.env[e.name] }))
  on('session.id', () => ({ value: SESSION_ID }))
  on('fs.exists', ($, e) => ({ value: world.disk.has(canonical(e.path)) }))
  on('fs.stat', ($, e) => {
    const key = canonical(e.path)
    if (!world.disk.has(key)) throw new Error(`ENOENT: no such file or directory, stat '${e.path}'`)
    const realPath = machine.realPaths?.[key] ?? e.path
    const kind = machine.kinds?.[key] ?? 'dir'
    return { value: { kind, size: 0, mtimeMs: 0, isLink: false, ...(e.resolve ? { realPath } : {}) } }
  })
  on('session.messages', () => ({ value: [] }))
  on('session.start', ($, e) => ({ cwd: e.cwd }))
  on('command.register', ($, e) => ({ value: { command: e.name } }))
  on('turn.complete', ($, e) => ({ text: e.answer }))
  on('ui.log', ($, e) => {
    world.logs.push(`[${e.to}] ${e.text}`)
    return { value: undefined }
  })
  on('ui.toast', ($, e) => {
    world.toasts.push(e.text)
    return { value: undefined }
  })
  on('process.run', async ($, e) => {
    // A pressed link's opener (PowerShell, open, xdg-open): recorded, never run.
    if (e.argv[0] !== 'git') {
      const target = e.init?.env?.['FILE_LINKS_TARGET']
      world.opened.push([...e.argv, ...(target === undefined ? [] : [`FILE_LINKS_TARGET=${target}`])])
      const fails = world.openerFails
      return { value: fails === undefined ? ran(0, '') : { ...ran(fails.exitCode, ''), stderr: fails.stderr } }
    }
    world.gitCalls++
    if (world.repo === undefined) return { value: ran(128, '') }
    // The list is read at the call, as git reads the disk when it starts.
    const listed = [...(e.argv.includes('--deleted') ? world.deleted : world.repo)]
    if (world.gitHold !== undefined) await world.gitHold
    return { value: ran(0, listed.map(path => `${path}\0`).join('')) }
  })
  // The tools themselves: an answer of their own, so a test's `$.tool.call`
  // reaches the mod's hook and ends here.
  on('tool.call', () => ({ result: {} as never }))
  on('ui.render', ($, e) => {
    const { Text } = $.ui.resolve(e)
    if (e.component !== 'AssistantMessage') return Text({ children: ['engine'] })
    world.drawn.push(e.props.text)
    return world.replyRow === 'engine' ? CORE : Text({ children: ['a plugin beneath'] })
  })
  return world
}

/**
 * Starts the test session, as the REPL does, and lets the index build the
 * session.start hook leaves running finish.
 *
 * @param $ the engine, as the test holds it
 * @param world the test world
 */
async function start($: Engine, world: World): Promise<void> {
  await $.session.start({ cwd: world.machine.project, surface: 'terminal', isInteractive: true })
  await world.clock.settle()
}

/** How a test draws a reply block beyond its text: the layout and the block's own flags. */
type DrawOptions = {
  /** What the surface measured; absent, as a surface that never measured, which is how most tests draw. */
  viewport?: RenderViewport
  /** True for a block that holds a summary of the model's text. */
  isSummary?: true
  /** False for a block that does not open its reply (no bullet); true when absent. */
  isFirstOfReply?: boolean
}

/**
 * Draws one reply block through the mod, as the transcript asks for it.
 *
 * @param $ the engine, as the test holds it
 * @param surface where it is drawn
 * @param text the block's markdown
 * @param requestId the row's id; a second draw of the same row unmounts first
 * @param options the layout and the block's flags; none draws an unmeasured
 *   block that opens its reply
 * @returns the mounted drawing
 */
async function draw(
  $: Engine,
  surface: RenderSurface,
  text: string,
  requestId: string,
  options: DrawOptions = {},
): Promise<Mounted<RenderSurface, 'AssistantMessage'>> {
  const props: RenderPropsOf['AssistantMessage'] = {
    text,
    isFirstOfReply: options.isFirstOfReply ?? true,
    ...(options.isSummary === true ? { isSummary: true } : {}),
  }
  const viewport = options.viewport === undefined ? {} : { viewport: options.viewport }
  return $.ui.mount({ plugin: 'file-links', surface, component: 'AssistantMessage', props, requestId, ...viewport })
}

describe('AssistantMessage', () => {
  test('hands the engine the reply with each placed span linked, on every local surface', async ($, on) => {
    const world = seat(on, [`${PROJECT}/README.md`, `${HOME}/notes.md`], ['README.md', 'docs/guide.md', 'hooks/register.ts'])
    await start($, world)
    const text = 'Read `README.md`, guide.md and C:\\Users\\me\\notes.md, not ui.render.'
    for (const surface of LOCAL_SURFACES) {
      const drawing = await draw($, surface, text, `m-${surface}`)
      expect(world.drawn.at(-1)).toBe(
        'Read [`README.md`](file:///C:/proj/README.md), [guide.md](file:///C:/proj/docs/guide.md) and [C:\\\\Users\\\\me\\\\](file:///C:/Users/me/)[notes.md](file:///C:/Users/me/notes.md), not ui.render.',
      )
      await drawing.unmount()
    }
    expect(world.logs).toEqual([])
  })

  test('a phone draws the reply as written', async ($, on) => {
    const world = seat(on, [`${PROJECT}/README.md`], ['README.md'])
    await start($, world)
    await draw($, 'mobile', 'Read `README.md`.', 'm1')
    expect(world.drawn).toEqual(['Read `README.md`.'])
    expect(world.logs).toEqual([])
  })

  test('a reply naming no file it can place is handed on as received', async ($, on) => {
    const world = seat(on, [], undefined)
    await start($, world)
    await draw($, 'terminal', 'See `hooks/x.ts`, y.md and C:\\nowhere\\z.md.', 'm1')
    expect(world.drawn).toEqual(['See `hooks/x.ts`, y.md and C:\\nowhere\\z.md.'])
    expect(world.logs).toEqual([])
  })

  test('a name several folders hold links to the shortest path', async ($, on) => {
    const world = seat(on, [], ['skills/codex-cli/SKILL.md', 'skills/curl/SKILL.md', 'skills/doctor/SKILL.md', 'docs/deep/er/SKILL.md'])
    await start($, world)
    await draw($, 'terminal', 'Each skill has a `SKILL.md`.', 'm1')
    expect(world.drawn.at(-1)).toBe('Each skill has a [`SKILL.md`](file:///C:/proj/skills/curl/SKILL.md).')
    expect(world.logs).toEqual([])
  })

  test('a tracked file deleted from the disk is left out, so the next copy wins', async ($, on) => {
    const world = seat(on, [], ['docs/x.md', 'docs/deeper/x.md'], ['docs/x.md'])
    await start($, world)
    await draw($, 'terminal', 'See `x.md` and `gone.md`.', 'm1')
    expect(world.drawn.at(-1)).toBe('See [`x.md`](file:///C:/proj/docs/deeper/x.md) and `gone.md`.')
    expect(world.logs).toEqual([])
  })

  test('a name several files share links to the one the session touched, over a shorter one', async ($, on) => {
    const world = seat(on, [`${PROJECT}/a/README.md`, `${PROJECT}/bb/README.md`], ['a/README.md', 'bb/README.md'])
    await start($, world)
    await draw($, 'terminal', 'See README.md.', 'm1')
    // Nothing touched yet: the shortest path wins.
    expect(world.drawn.at(-1)).toBe('See [README.md](file:///C:/proj/a/README.md).')
    await $.tool.call({ tool: 'Read', file_path: `${PROJECT}/bb/README.md` })
    await draw($, 'terminal', 'See README.md.', 'm2')
    expect(world.drawn.at(-1)).toBe('See [README.md](file:///C:/proj/bb/README.md).')
    expect(world.logs).toEqual([])
  })

  test('a file a Write made links at the next draw', async ($, on) => {
    const world = seat(on, [], ['README.md'])
    await start($, world)
    await draw($, 'terminal', 'Created `out.md`.', 'm1')
    expect(world.drawn.at(-1)).toBe('Created `out.md`.')
    world.disk.add(canonical(`${PROJECT}/out.md`))
    await $.tool.call({ tool: 'Write', file_path: `${PROJECT}/out.md`, content: 'x' })
    await draw($, 'terminal', 'Created `out.md`.', 'm2')
    expect(world.drawn.at(-1)).toBe('Created [`out.md`](file:///C:/proj/out.md).')
    expect(world.logs).toEqual([])
  })

  test('a turn that adds a file to the repository rebuilds the index and redraws the reply that missed it', async ($, on) => {
    const repo = ['README.md']
    const world = seat(on, [], repo)
    await start($, world)
    const drawing = await draw($, 'terminal', 'Next: docs/later.md', 'm1')
    expect(world.drawn.at(-1)).toBe('Next: docs/later.md')
    repo.push('docs/later.md')
    await $.turn.complete({ answer: 'done', durationMs: 1000, isAborted: false, turnId: 't1', reason: 'answer' })
    await world.clock.advance(15_000)
    // The kit redraws an invalidated drawing when the test reads it next.
    await drawing.find({ type: 'Text' })
    expect(world.drawn.at(-1)).toBe('Next: [docs/](file:///C:/proj/docs/)[later.md](file:///C:/proj/docs/later.md)')
    expect(world.logs).toEqual([])
  })

  test('a folder name written with its slash links to that folder, and the bare word stays prose', async ($, on) => {
    const world = seat(on, [`${PROJECT}/projects`], undefined)
    await start($, world)
    await draw($, 'terminal', 'Transcripts live under projects/ in projects.', 'm1')
    expect(world.drawn.at(-1)).toBe('Transcripts live under [projects/](file:///C:/proj/projects/) in projects.')
    expect(world.logs).toEqual([])
  })

  test('a path written as a folder never lands on a file of that name', async ($, on) => {
    // The repository holds a FILE `a/tools/hooks`; `tools/hooks/` names a folder.
    const world = seat(on, [], ['a/tools/hooks'])
    await start($, world)
    await draw($, 'terminal', 'See tools/hooks/ and hooks.', 'm1')
    expect(world.drawn.at(-1)).toBe('See tools/hooks/ and hooks.')
    expect(world.logs).toEqual([])
  })
})

describe('clicks (fullscreen terminal)', () => {
  /** Windows Terminal, with what `$.fs.stat` says of the files the tests press. */
  const WINDOWS_FILES: Machine = {
    ...WINDOWS,
    kinds: {
      [canonical(`${PROJECT}/README.md`)]: 'file',
      [canonical(`${PROJECT}/hooks/x.ts`)]: 'file',
      [canonical(`${PROJECT}/RULES.md`)]: 'file',
      [canonical(`${PROJECT}/run.ps1`)]: 'file',
    },
  }

  /** The files and folder on the test disk for the click tests. */
  const FILES = [`${PROJECT}/README.md`, `${PROJECT}/hooks`, `${PROJECT}/hooks/x.ts`, `${PROJECT}/RULES.md`, `${PROJECT}/run.ps1`]

  test('a reply with a file link is a Markdown answering its file links, laid over the engine\'s own row', async ($, on) => {
    const world = seat(on, FILES, ['README.md', 'hooks/x.ts'], [], WINDOWS_FILES)
    await start($, world)
    const text = 'See README.md, hooks/x.ts, [the rules](file:///C:/proj/RULES.md) and [docs](https://example.com).'
    const drawing = await draw($, 'terminal', text, 'm1', { viewport: FULLSCREEN })
    // The engine draws its own row (margin, bullet) around the one-row stub.
    expect(world.drawn).toEqual(['\u200B'])
    const tree = await drawing.drawn()
    expect(tree).toMatchObject({ type: 'Box', props: { flexDirection: 'column' } })
    const [chrome, overlay] = (tree as { children: unknown[] }).children
    expect(chrome).toEqual(CORE)
    // Lifted onto the stub's row, from where the engine's text starts.
    expect(overlay).toMatchObject({ type: 'Box', props: { marginTop: -1, marginLeft: 2 } })
    const markdown = await drawing.find({ type: 'Markdown' })
    expect(markdown?.key).toBe('reply')
    expect(markdown?.props['text']).toBe(
      'See [README.md](file:///C:/proj/README.md), [hooks/](file:///C:/proj/hooks/)[x.ts](file:///C:/proj/hooks/x.ts), [the rules](file:///C:/proj/RULES.md) and [docs](https://example.com).',
    )
    // The mod's links and the reply's own file link; the web link stays the terminal's.
    expect(markdown?.props['pressableLinks']).toEqual([
      'file:///C:/proj/README.md',
      'file:///C:/proj/hooks/',
      'file:///C:/proj/hooks/x.ts',
      'file:///C:/proj/RULES.md',
    ])
    // The bullet is the engine's, in its row: the mod draws none of its own.
    expect(await drawing.find({ type: 'Text', text: '●' })).toBeUndefined()
    expect(world.logs).toEqual([])
  })

  test('a block that does not open its reply lays its text at the row\'s own edge', async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'And README.md.', 'm1', { viewport: FULLSCREEN, isFirstOfReply: false })
    const tree = (await drawing.drawn()) as { children: unknown[] }
    expect(tree.children[1]).toMatchObject({ type: 'Box', props: { marginTop: -1, marginLeft: 0 } })
    expect(await drawing.find({ type: 'Markdown' })).toBeDefined()
    expect(world.logs).toEqual([])
  })

  test('a stub a plugin beneath draws its own way gets the mod\'s own row: bullet, margin, text', async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    world.replyRow = 'tree'
    await start($, world)
    const drawing = await draw($, 'terminal', 'See README.md.', 'm1', { viewport: FULLSCREEN })
    expect(await drawing.drawn()).toMatchObject({ type: 'Box', props: { flexDirection: 'row', marginTop: 1 } })
    expect((await drawing.find({ type: 'Text', text: '●' }))?.props['color']).toBe('text')
    expect((await drawing.find({ type: 'Markdown' }))?.props['text']).toBe('See [README.md](file:///C:/proj/README.md).')
    // The plugin beneath's own drawing of the stub is not shown.
    expect(await drawing.find({ type: 'Text', text: 'a plugin beneath' })).toBeUndefined()
    expect(world.logs).toEqual([])
  })

  test('a reply holding a character a Markdown refuses keeps the engine\'s row, links added', async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'See README.md.\r\nDone.', 'm1', { viewport: FULLSCREEN })
    expect(world.drawn).toEqual(['See [README.md](file:///C:/proj/README.md).\r\nDone.'])
    expect(await drawing.find({ type: 'Markdown' })).toBeUndefined()
    expect(world.logs).toEqual([])
  })

  test('a reply whose only file link names another machine keeps the engine\'s row', async ($, on) => {
    const world = seat(on, FILES, [], [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'See [share](file://server/share/x.md).', 'm1', { viewport: FULLSCREEN })
    expect(world.drawn).toEqual(['See [share](file://server/share/x.md).'])
    expect(await drawing.find({ type: 'Markdown' })).toBeUndefined()
    expect(world.logs).toEqual([])
  })

  test('a plain click opens a file once, a folder link its folder, and a script is shown, never run', async ($, on) => {
    const world = seat(on, FILES, ['README.md', 'hooks/x.ts'], [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'See hooks/x.ts, README.md and run.ps1.', 'm1', { viewport: FULLSCREEN })
    for (const href of ['file:///C:/proj/hooks/x.ts', 'file:///C:/proj/hooks/', 'file:///C:/proj/run.ps1']) {
      await drawing.press({ key: 'reply', link: { href } })
      // The open runs on its own after the press: let it finish.
      await world.clock.settle()
    }
    // The path rides in a variable, never in the command text.
    const open = ['powershell', '-NoProfile', '-NonInteractive', '-Command', 'Invoke-Item -LiteralPath $env:FILE_LINKS_TARGET']
    const reveal = [
      'powershell',
      '-NoProfile',
      '-NonInteractive',
      '-Command',
      `Start-Process -FilePath explorer.exe -ArgumentList ('/select,"' + $env:FILE_LINKS_TARGET + '"')`,
    ]
    expect(world.opened).toEqual([
      [...open, 'FILE_LINKS_TARGET=C:\\proj\\hooks\\x.ts'],
      [...open, 'FILE_LINKS_TARGET=C:\\proj\\hooks'],
      [...reveal, 'FILE_LINKS_TARGET=C:\\proj\\run.ps1'],
    ])
    expect(world.toasts).toEqual([])
    expect(world.logs).toEqual([])
  })

  test('a path whose name would break a command line reaches the opener whole, in the variable', async ($, on) => {
    const odd = `${PROJECT}/a&b,c; it's ‘q’ $x`
    const world = seat(on, [odd], undefined, [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'See [odd](file:///C:/proj/a%26b%2Cc%3B%20it%27s%20%E2%80%98q%E2%80%99%20%24x).', 'm1', {
      viewport: FULLSCREEN,
    })
    await drawing.press({ key: 'reply', link: { href: 'file:///C:/proj/a%26b%2Cc%3B%20it%27s%20%E2%80%98q%E2%80%99%20%24x' } })
    await world.clock.settle()
    expect(world.opened.at(-1)?.at(-1)).toBe("FILE_LINKS_TARGET=C:\\proj\\a&b,c; it's ‘q’ $x")
    // The command text is the same for every path.
    expect(world.opened.at(-1)?.at(-2)).toBe('Invoke-Item -LiteralPath $env:FILE_LINKS_TARGET')
    expect(world.logs).toEqual([])
  })

  test('an opener that fails says so with its first error line', async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    world.openerFails = { exitCode: 1, stderr: 'Invoke-Item : Access is denied.\r\nAt line:1 char:1' }
    await start($, world)
    const drawing = await draw($, 'terminal', 'See README.md.', 'm1', { viewport: FULLSCREEN })
    await drawing.press({ key: 'reply', link: { href: 'file:///C:/proj/README.md' } })
    await world.clock.settle()
    expect(world.toasts).toEqual(['file-links could not open C:/proj/README.md: Invoke-Item : Access is denied.'])
    expect(world.logs).toEqual([])
  })

  test('a click on a link to a missing file says so and runs nothing', async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'See [gone](file:///C:/proj/gone.md) and README.md.', 'm1', { viewport: FULLSCREEN })
    await drawing.press({ key: 'reply', link: { href: 'file:///C:/proj/gone.md' } })
    await world.clock.settle()
    expect(world.opened).toEqual([])
    expect(world.toasts).toEqual(['file-links: C:/proj/gone.md is not there'])
  })

  test('on macOS a click opens with `open`, a script is revealed with `open -R`, and the mod\'s own row draws ⏺', async ($, on) => {
    const machine: Machine = {
      ...MAC,
      kinds: { [canonical(`${MAC_PROJECT}/README.md`)]: 'file', [canonical(`${MAC_PROJECT}/go.command`)]: 'file' },
    }
    const world = seat(on, [`${MAC_PROJECT}/README.md`, `${MAC_PROJECT}/go.command`], undefined, [], machine)
    // The mod's own row (a plugin beneath drew the stub): its bullet is macOS's.
    world.replyRow = 'tree'
    await start($, world)
    const drawing = await draw($, 'terminal', 'Read README.md, then go.command.', 'm1', { viewport: FULLSCREEN })
    expect(await drawing.find({ type: 'Text', text: '⏺' })).toBeDefined()
    for (const href of ['file:///Users/me/proj/README.md', 'file:///Users/me/proj/go.command']) {
      await drawing.press({ key: 'reply', link: { href } })
      await world.clock.settle()
    }
    expect(world.opened).toEqual([
      ['open', '/Users/me/proj/README.md'],
      ['open', '-R', '/Users/me/proj/go.command'],
    ])
    expect(world.logs).toEqual([])
  })

  test('the main screen, a summary row and a reply with no file link keep the engine\'s own row', async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    await start($, world)
    await draw($, 'terminal', 'See README.md.', 'm1', { viewport: MAIN_SCREEN })
    expect(world.drawn.at(-1)).toBe('See [README.md](file:///C:/proj/README.md).')
    await draw($, 'terminal', 'Summed: README.md.', 'm2', { viewport: FULLSCREEN, isSummary: true })
    expect(world.drawn.at(-1)).toBe('Summed: [README.md](file:///C:/proj/README.md).')
    await draw($, 'terminal', 'Nothing to open, see [docs](https://example.com).', 'm3', { viewport: FULLSCREEN })
    expect(world.drawn.at(-1)).toBe('Nothing to open, see [docs](https://example.com).')
    expect(world.logs).toEqual([])
  })

  test('with plainClickOpens off the fullscreen reply is the engine\'s own row, links added', { options: { plainClickOpens: false } }, async ($, on) => {
    const world = seat(on, FILES, ['README.md'], [], WINDOWS_FILES)
    await start($, world)
    const drawing = await draw($, 'terminal', 'See README.md.', 'm1', { viewport: FULLSCREEN })
    expect(world.drawn.at(-1)).toBe('See [README.md](file:///C:/proj/README.md).')
    expect(await drawing.find({ type: 'Markdown' })).toBeUndefined()
    expect(world.logs).toEqual([])
  })
})

describe('index builds', () => {
  test('a turn that ends while a build runs gets its own rebuild once that build is done', async ($, on) => {
    const repo = ['README.md']
    const world = seat(on, [], repo)
    let release = () => {}
    world.gitHold = new Promise<void>(resolve => {
      release = resolve
    })
    // session.start's build now waits in git, with the list as it was.
    await start($, world)
    const drawing = await draw($, 'terminal', 'Next: docs/later.md', 'm1')
    expect(world.drawn.at(-1)).toBe('Next: docs/later.md')
    repo.push('docs/later.md')
    await $.turn.complete({ answer: 'done', durationMs: 1000, isAborted: false, turnId: 't1', reason: 'answer' })
    // The poll comes while the first build still runs: it must keep the flag.
    await world.clock.advance(15_000)
    world.gitHold = undefined
    release()
    await world.clock.settle()
    // The first build ended with the old list; the next poll builds again.
    await world.clock.advance(15_000)
    await drawing.find({ type: 'Text' })
    expect(world.drawn.at(-1)).toBe('Next: [docs/](file:///C:/proj/docs/)[later.md](file:///C:/proj/docs/later.md)')
    expect(world.logs).toEqual([])
  })
})

describe('keepNewest', () => {
  test('drops the oldest entries past the cap, and keeps a re-inserted one', () => {
    const map = new Map<string, number>([['a', 1], ['b', 2], ['c', 3]])
    // `a` used again: delete then set moves it to the end, as noteTouched does.
    map.delete('a')
    map.set('a', 4)
    keepNewest(map, 2)
    expect([...map.keys()]).toEqual(['c', 'a'])
    keepNewest(map, 5)
    expect([...map.keys()]).toEqual(['c', 'a'])
  })
})

describe('platforms', () => {
  test('on macOS, POSIX, home, relative and repository paths link, and Windows forms stay as written', async ($, on) => {
    const files = ['/Users/me/notes.md', `${MAC_PROJECT}/README.md`, `${MAC_PROJECT}/hooks/x.ts`]
    const world = seat(on, files, ['README.md', 'hooks/x.ts', 'docs/guide.md'], [], MAC)
    await start($, world)
    await draw($, 'terminal', 'Open /Users/me/notes.md, ~/proj/README.md, `README.md`, hooks/x.ts, guide.md, C:\\proj\\README.md and /c/Users/me/notes.md.', 'm1')
    expect(world.drawn.at(-1)).toBe(
      'Open [/Users/me/](file:///Users/me/)[notes.md](file:///Users/me/notes.md), [\\~/proj/](file:///Users/me/proj/)[README.md](file:///Users/me/proj/README.md), [`README.md`](file:///Users/me/proj/README.md), [hooks/](file:///Users/me/proj/hooks/)[x.ts](file:///Users/me/proj/hooks/x.ts), [guide.md](file:///Users/me/proj/docs/guide.md), C:\\proj\\README.md and /c/Users/me/notes.md.',
    )
    expect(world.logs).toEqual([])
  })

  test('in Apple Terminal the reply stays as written, while the desktop app, which draws anchors, links', async ($, on) => {
    const world = seat(on, [`${MAC_PROJECT}/README.md`], ['README.md'], [], MAC_TERMINAL)
    await start($, world)
    const terminal = await draw($, 'terminal', 'See `README.md`.', 'm1')
    expect(world.drawn.at(-1)).toBe('See `README.md`.')
    await terminal.unmount()
    await draw($, 'desktop', 'See `README.md`.', 'm2')
    expect(world.drawn.at(-1)).toBe('See [`README.md`](file:///Users/me/proj/README.md).')
    expect(world.logs).toEqual([])
  })

  test('on Windows outside Windows Terminal the terminal reply stays as written', async ($, on) => {
    const world = seat(on, [`${PROJECT}/README.md`], ['README.md'], [], { project: PROJECT, env: { USERPROFILE: HOME } })
    await start($, world)
    await draw($, 'terminal', 'See `README.md`.', 'm1')
    expect(world.drawn.at(-1)).toBe('See `README.md`.')
    expect(world.logs).toEqual([])
  })

  test('outside any repository the mod runs no git at all', async ($, on) => {
    const world = seat(on, [`${MAC_PROJECT}/README.md`], undefined, [], MAC)
    await start($, world)
    await draw($, 'terminal', 'See `README.md` and guide.md.', 'm1')
    expect(world.drawn.at(-1)).toBe('See [`README.md`](file:///Users/me/proj/README.md) and guide.md.')
    expect(world.gitCalls).toBe(0)
    expect(world.logs).toEqual([])
  })
})

describe('scratchpad', () => {
  /** Windows Terminal with a TEMP folder: Claude Code's temp folder is `<TEMP>/claude`. */
  const WINDOWS_TEMP: Machine = { project: PROJECT, env: { ...WINDOWS.env, TEMP: 'C:\\Users\\me\\AppData\\Local\\Temp' } }

  /** The scratchpad Claude Code 2.1.292 makes for WINDOWS_TEMP's session. */
  const SCRATCH = `C:/Users/me/AppData/Local/Temp/claude/C--proj/${SESSION_ID}/scratchpad`

  test('names and relative paths link to files in the session scratchpad, after the working folder', async ($, on) => {
    const files = [
      'C:/Users/me/AppData/Local/Temp/claude',
      SCRATCH,
      `${SCRATCH}/notes.txt`,
      `${SCRATCH}/run1/summary.json`,
      `${SCRATCH}/README.md`,
      `${PROJECT}/README.md`,
    ]
    const world = seat(on, files, undefined, [], WINDOWS_TEMP)
    await start($, world)
    await draw($, 'terminal', 'See notes.txt, run1/summary.json, README.md and missing.txt.', 'm1')
    expect(world.drawn.at(-1)).toBe(
      `See [notes.txt](file:///${SCRATCH}/notes.txt), [run1/](file:///${SCRATCH}/run1/)[summary.json](file:///${SCRATCH}/run1/summary.json), [README.md](file:///C:/proj/README.md) and missing.txt.`,
    )
    expect(world.logs).toEqual([])
  })

  test('a tail of the scratchpad links into it: its session id and name, with or without a leading slash', async ($, on) => {
    const files = ['C:/Users/me/AppData/Local/Temp/claude', SCRATCH, `${SCRATCH}/run1`, `${SCRATCH}/run1/summary.json`]
    const world = seat(on, files, undefined, [], WINDOWS_TEMP)
    await start($, world)
    await draw($, 'terminal', `See ${SESSION_ID}/scratchpad, /${SESSION_ID}/scratchpad/run1/summary.json and scratchpad/.`, 'm1')
    const session = 'C:/Users/me/AppData/Local/Temp/claude/C--proj/sess-1'
    expect(world.drawn.at(-1)).toBe(
      `See [${SESSION_ID}/](file:///${session}/)[scratchpad](file:///${SCRATCH}), ` +
        `[/${SESSION_ID}/scratchpad/run1/](file:///${SCRATCH}/run1/)[summary.json](file:///${SCRATCH}/run1/summary.json) ` +
        `and [scratchpad/](file:///${SCRATCH}/).`,
    )
    expect(world.logs).toEqual([])
  })

  test('on macOS the scratchpad lies under the real path of TMPDIR (/var is /private/var)', async ($, on) => {
    const real = '/private/var/folders/zz/abc/T/claude'
    const machine: Machine = {
      project: MAC_PROJECT,
      env: { ...MAC.env, TMPDIR: '/var/folders/zz/abc/T/' },
      realPaths: { [canonical('/var/folders/zz/abc/T/claude')]: real },
    }
    const scratch = `${real}/-Users-me-proj/${SESSION_ID}/scratchpad`
    const world = seat(on, ['/var/folders/zz/abc/T/claude', scratch, `${scratch}/probe.py`], undefined, [], machine)
    await start($, world)
    await draw($, 'terminal', 'Run `probe.py`.', 'm1')
    expect(world.drawn.at(-1)).toBe(`Run [\`probe.py\`](file://${scratch}/probe.py).`)
    expect(world.logs).toEqual([])
  })

  test('/file-links names the scratchpad, or says none was found', async ($, on) => {
    const world = seat(on, ['C:/Users/me/AppData/Local/Temp/claude', SCRATCH], undefined, [], WINDOWS_TEMP)
    await start($, world)
    const run = () =>
      $.command.run({ command: 'file-links', args: '', origin: { kind: 'composer' }, presentation: { isFullscreen: true, columns: 160 } })
    expect((await run()).text).toContain(`scratchpad: ${SCRATCH}`)
    expect(world.logs).toEqual([])
  })

  test('a scratchpad a shell command makes mid-session links after the turn, and earlier replies redraw with it', async ($, on) => {
    const world = seat(on, ['C:/Users/me/AppData/Local/Temp/claude'], undefined, [], WINDOWS_TEMP)
    await start($, world)
    const first = await draw($, 'terminal', 'See notes.txt.', 'm1')
    expect(world.drawn.at(-1)).toBe('See notes.txt.')
    // A shell command makes the folder and the file: no tool call, no generation.
    world.disk.add(canonical(SCRATCH))
    world.disk.add(canonical(`${SCRATCH}/notes.txt`))
    await $.turn.complete({ answer: 'done', durationMs: 1000, isAborted: false, turnId: 't1', reason: 'answer' })
    await draw($, 'terminal', 'Also notes.txt.', 'm2')
    expect(world.drawn.at(-1)).toBe(`Also [notes.txt](file:///${SCRATCH}/notes.txt).`)
    // The find redraws the reply drawn while it was missing.
    await first.find({ type: 'Text' })
    expect(world.drawn).toContain(`See [notes.txt](file:///${SCRATCH}/notes.txt).`)
    expect(world.logs).toEqual([])
  })

  test('a claude FILE in the temp root is no scratchpad', async ($, on) => {
    const machine: Machine = { ...WINDOWS_TEMP, kinds: { [canonical('C:/Users/me/AppData/Local/Temp/claude')]: 'file' } }
    const world = seat(on, ['C:/Users/me/AppData/Local/Temp/claude', SCRATCH, `${SCRATCH}/notes.txt`], undefined, [], machine)
    await start($, world)
    await draw($, 'terminal', 'See notes.txt.', 'm1')
    expect(world.drawn.at(-1)).toBe('See notes.txt.')
    expect(world.logs).toEqual([])
  })

  test('a real path that is not absolute falls back to the temp folder as written', async ($, on) => {
    const machine: Machine = { ...WINDOWS_TEMP, realPaths: { [canonical('C:/Users/me/AppData/Local/Temp/claude')]: 'relative\\claude' } }
    const world = seat(on, ['C:/Users/me/AppData/Local/Temp/claude', SCRATCH, `${SCRATCH}/notes.txt`], undefined, [], machine)
    await start($, world)
    await draw($, 'terminal', 'See notes.txt.', 'm1')
    expect(world.drawn.at(-1)).toBe(`See [notes.txt](file:///${SCRATCH}/notes.txt).`)
    expect(world.logs).toEqual([])
  })

  test('with no temp variable set there is no scratchpad, and nothing fails', async ($, on) => {
    const world = seat(on, [`${PROJECT}/README.md`], undefined)
    await start($, world)
    await draw($, 'terminal', 'See README.md and notes.txt.', 'm1')
    expect(world.drawn.at(-1)).toBe('See [README.md](file:///C:/proj/README.md) and notes.txt.')
    const { text } = await $.command.run({
      command: 'file-links',
      args: '',
      origin: { kind: 'composer' },
      presentation: { isFullscreen: true, columns: 160 },
    })
    expect(text).toContain('scratchpad: none found')
    expect(world.logs).toEqual([])
  })
})

describe('/file-links', () => {
  test('reports the folders, the index and the touched files', async ($, on) => {
    const world = seat(on, [], ['README.md', 'hooks/register.ts'])
    await start($, world)
    await $.tool.call({ tool: 'Read', file_path: `${PROJECT}/README.md` })
    const { text } = await $.command.run({
      command: 'file-links',
      args: '',
      origin: { kind: 'composer' },
      presentation: { isFullscreen: true, columns: 160 },
    })
    expect(text).toContain('cwd C:/proj')
    expect(text).toContain('home C:/Users/me')
    expect(text).toContain('platform: windows · terminal hyperlinks: on (WT_SESSION)')
    expect(text).toContain('index: 2 files in C:/proj')
    expect(text).toContain('touched files: 1')
    expect(world.logs).toEqual([])
  })

  test('says why a macOS terminal gets no links, and how to turn them on', async ($, on) => {
    const world = seat(on, [], undefined, [], MAC_TERMINAL)
    await start($, world)
    const { text } = await $.command.run({
      command: 'file-links',
      args: '',
      origin: { kind: 'composer' },
      presentation: { isFullscreen: true, columns: 160 },
    })
    expect(text).toContain('platform: posix · terminal hyperlinks: off (TERM_PROGRAM=Apple_Terminal)')
    expect(text).toContain('FORCE_HYPERLINK=1 turns links on')
    expect(text).toContain('home /Users/me')
  })
})
