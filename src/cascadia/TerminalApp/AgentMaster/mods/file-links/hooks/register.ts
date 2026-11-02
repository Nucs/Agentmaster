import type { EngineInterface, Register } from 'claude-code'
import {
  absoluteOf,
  baseName,
  bestOf,
  claudeTempOf,
  hyperlinksOf,
  joinPath,
  linkify,
  normalize,
  parentOf,
  platformOf,
  scan,
  scratchpadPathOf,
  type HyperlinkSupport,
  type Mention,
  type Platform,
  type TempEnv,
} from './links'

/**
 * file-links: draws the file paths and file names in Claude's replies as
 * `file:///` links, the form a CLAUDE.md file-link rule asks replies to
 * write, without the model spending tokens on them.
 *
 * Display only. The `AssistantMessage` hook hands the engine the reply's
 * markdown with each span that names an existing file wrapped in a link; the
 * engine's own renderer draws it as an OSC 8 hyperlink. In a terminal that
 * draws no hyperlinks the engine would show `text (url)`, so there the mod
 * leaves the reply as written (see hyperlinksOf). The stored reply, what the
 * model reads back, and what plugins seated above this one see stay as they
 * were.
 *
 * Windows and macOS (Linux too): the platform is told from the session's
 * folders, and decides which absolute forms count (`C:\x` and Git Bash `/c/x`
 * on Windows, `/x` on POSIX; `~/x` on both) and which home variable names the
 * home folder.
 *
 * Footgun: a plugin seated BENEATH this one at `AssistantMessage` sees the
 * rewritten text. chat-view matches rows to the transcript by their text, so
 * file-links must be seated beneath chat-view (enabledPlugins order), never
 * above it.
 */

/** What the module knows of the session. Module memory: a reload starts it over. */
type World = {
  /** The session's working folder, normalized; undefined until {@link place} ran. */
  cwd?: string
  /** The project root, normalized; often the working folder itself. */
  root?: string
  /** The home folder (USERPROFILE on Windows, HOME on macOS), normalized, for `~/x`. */
  home?: string
  /**
   * The folders the session's scratchpad may be named after, as the engine
   * spells them (Claude Code names it after the ORIGINAL working folder, which
   * no API gives): the working folder at `session.start`, then the working
   * folder and the project root as {@link place} read them.
   */
  projectSpellings: string[]
  /** The temp-root variables (see {@link TempEnv}); empty until {@link place} ran. */
  tempEnv: TempEnv
  /**
   * The scratchpad found for one session id, or none found (`folder`
   * undefined, asked again at a new generation: the engine makes the folder
   * only when the session first needs it). A new id (after `/clear`) asks again.
   */
  scratchpad?: { sessionId: string; folder?: string; generation: number }
  /**
   * The real path of Claude Code's temp folder, once it resolved: the temp
   * root does not change while the module runs, so a later lookup skips the
   * stat. A failed resolution is not kept (the folder may come later).
   */
  tempBase?: string
  /** The session's platform, told from its folders; `posix` until {@link place} ran. */
  platform: Platform
  /** Whether the terminal draws hyperlinks, as the engine decides it; off until {@link place} ran. */
  hyperlinks: HyperlinkSupport
  /**
   * The one read of the folders and the environment, shared by every caller:
   * the first draws come together, and each must wait for it rather than run
   * with no folders (a draw worked out then would be kept without its links).
   */
  placing?: Promise<void>
  /** The session repository's files by name; undefined outside a repository or before the first build. */
  index?: RepoIndex
  /**
   * Files the session's tools read or wrote, normalized, each with the order
   * it was last touched in: a bare name prefers the newest of them.
   */
  touched: Map<string, number>
  /** The touch counter, so a later touch outranks an earlier one. */
  touches: number
  /**
   * Answers of `$.fs.exists` by normalized path. A `false` is dropped at each
   * new generation (a file may have been written since); a `true` is kept.
   */
  exists: Map<string, boolean>
  /**
   * Texts already worked out, `<generation>\0<text>` to the text to draw, in
   * insertion order so the oldest goes first past {@link MAX_DRAWN}.
   */
  drawn: Map<string, string>
  /** Bumped whenever an answer may change; it keys {@link World.drawn}. */
  generation: number
  /** True when a lookup found nothing since the last index build: a build that changes the index then redraws. */
  hasMissed: boolean
  /** True after a turn ended: files may have come or gone, so the timer rebuilds the index. */
  isIndexStale: boolean
  /** True while a build runs, so two never overlap. */
  isIndexing: boolean
  /** When the index was last built, in `$.clock.now()` milliseconds; 0 before. */
  indexedAt: number
}

/** The session repository's files, as `git ls-files` lists them. */
type RepoIndex = {
  /** The repository's top folder, normalized. */
  top: string
  /** Every file git lists (tracked, and untracked but not ignored), absolute and normalized, by lower-cased name. */
  byName: Map<string, string[]>
  /** How many files it holds: a build that lists another count changed something. */
  size: number
}

/** The most worked-out texts kept; a long session's rows past it are worked out again when drawn. */
const MAX_DRAWN = 600

/** How often the session.start timer looks whether a turn ended since the last index build. */
const INDEX_POLL_MS = 15_000

/** How long `git ls-files` may take before the build gives up (a vast untracked tree). */
const INDEX_TIMEOUT_MS = 30_000

/** The tools whose calls name a file the session touches, and so may mention next. */
const FILE_TOOLS = ['Read', 'Edit', 'Write', 'NotebookEdit'] as const

/** How many folders the walk up to a repository's `.git` climbs before it gives up. */
const MAX_REPO_DEPTH = 40

/**
 * The most `$.fs.exists` answers kept; past it the oldest go first. A dropped
 * answer costs one more check when its path comes up again, nothing more.
 */
export const MAX_EXISTS = 5_000

/**
 * The most touched files kept; past it the least recently touched go first,
 * so a long session keeps preferring the files it works on now.
 */
export const MAX_TOUCHED = 2_000

/**
 * Starts a world with nothing known.
 *
 * @returns an empty world
 */
function newWorld(): World {
  return {
    platform: 'posix',
    hyperlinks: { isOn: false, why: 'not read yet' },
    projectSpellings: [],
    tempEnv: {},
    touched: new Map(),
    touches: 0,
    exists: new Map(),
    drawn: new Map(),
    generation: 0,
    hasMissed: false,
    isIndexStale: false,
    isIndexing: false,
    indexedAt: 0,
  }
}

/**
 * Bounds a Map that only grows: drops its oldest entries (insertion order)
 * until it holds at most `max`.
 *
 * @param map the Map; an entry re-inserted on use (delete, then set) counts
 *   as new, so the dropped ones are the least recently used
 * @param max the most entries to keep
 */
export function keepNewest<K, V>(map: Map<K, V>, max: number): void {
  for (const key of map.keys()) {
    if (map.size <= max) return
    map.delete(key)
  }
}

/**
 * Marks every earlier answer as possibly stale: drops the worked-out texts
 * (by moving to a new generation) and the cached misses, keeping the hits.
 *
 * @param world the module's world
 */
function bump(world: World): void {
  world.generation++
  world.drawn.clear()
  for (const [path, isThere] of world.exists) if (!isThere) world.exists.delete(path)
}

/**
 * Reads the session's folders and environment once per module load. A render
 * can come before `session.start`, so every hook that needs them awaits this
 * first; callers that come while the read runs await the same read.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world
 * @returns resolves once the folders, the platform and the hyperlink state are set
 * @throws never: see {@link readPlace}
 */
function place($: EngineInterface, world: World): Promise<void> {
  world.placing ??= readPlace($, world)
  return world.placing
}

/**
 * Does {@link place}'s one read: the working folder, the project root, the
 * home folder, the platform they show, whether the terminal draws hyperlinks,
 * and the temp-root variables the scratchpad lies under (every variable by
 * its literal name, as `$.env.get` requires).
 *
 * @param $ the engine interface of the hook that read first
 * @param world the module's world
 * @throws never: a value that cannot be read stays undefined; the lookups that
 *   need a missing folder are skipped, and a missing variable counts as unset
 */
async function readPlace($: EngineInterface, world: World): Promise<void> {
  const unset = () => undefined
  const [cwd, root, profile, home, force, netlify, windowsTerminal, program, programVersion, emulator, tmux, lcTerminal, term] =
    await Promise.all([
      $.session.cwd().catch(unset),
      $.session.root().catch(unset),
      $.env.get('USERPROFILE').catch(unset),
      $.env.get('HOME').catch(unset),
      $.env.get('FORCE_HYPERLINK').catch(unset),
      $.env.get('NETLIFY').catch(unset),
      $.env.get('WT_SESSION').catch(unset),
      $.env.get('TERM_PROGRAM').catch(unset),
      $.env.get('TERM_PROGRAM_VERSION').catch(unset),
      $.env.get('TERMINAL_EMULATOR').catch(unset),
      $.env.get('TMUX').catch(unset),
      $.env.get('LC_TERMINAL').catch(unset),
      $.env.get('TERM').catch(unset),
    ])
  const [claudeTmp, temp, tmp, tmpDir, systemRoot, windir] = await Promise.all([
    $.env.get('CLAUDE_CODE_TMPDIR').catch(unset),
    $.env.get('TEMP').catch(unset),
    $.env.get('TMP').catch(unset),
    $.env.get('TMPDIR').catch(unset),
    $.env.get('SystemRoot').catch(unset),
    $.env.get('windir').catch(unset),
  ])
  world.tempEnv = { CLAUDE_CODE_TMPDIR: claudeTmp, TEMP: temp, TMP: tmp, TMPDIR: tmpDir, SystemRoot: systemRoot, windir }
  // Raw spellings, kept for the scratchpad's name: its hash covers the exact
  // string the engine had, so a normalized `C:/x` would name another folder.
  for (const spelling of [cwd, root]) {
    if (spelling !== undefined && !world.projectSpellings.includes(spelling)) world.projectSpellings.push(spelling)
  }
  world.cwd = cwd === undefined ? undefined : normalize(cwd)
  world.root = root === undefined ? undefined : normalize(root)
  world.platform = platformOf(cwd ?? root)
  // USERPROFILE first: on Windows a Git Bash HOME may name another folder;
  // on macOS USERPROFILE is unset and HOME is the home folder.
  const homeFolder = world.platform === 'windows' ? (profile ?? home) : (home ?? profile)
  world.home = homeFolder === undefined ? undefined : normalize(homeFolder)
  world.hyperlinks = hyperlinksOf(
    {
      FORCE_HYPERLINK: force,
      NETLIFY: netlify,
      WT_SESSION: windowsTerminal,
      TERM_PROGRAM: program,
      TERM_PROGRAM_VERSION: programVersion,
      TERMINAL_EMULATOR: emulator,
      TMUX: tmux,
      LC_TERMINAL: lcTerminal,
      TERM: term,
    },
    world.platform,
  )
}

/**
 * Notes a file a tool touched, as the newest touch.
 *
 * @param world the module's world, placed (its platform decides what is absolute)
 * @param path the tool's path as the model gave it (absolute, or relative to
 *   the working folder)
 * @returns true when the path was new to the world, so lookups that missed
 *   before may hit now
 */
function noteTouched(world: World, path: string): boolean {
  const absolute = absoluteOf(path, world.home, world.platform) ?? (world.cwd === undefined ? undefined : joinPath(world.cwd, path))
  if (absolute === undefined) return false
  const isNew = !world.touched.has(absolute)
  // Re-inserting moves the path to the Map's end, so iteration order is
  // touch order too; the counter ranks it, and the cap drops the oldest.
  world.touched.delete(absolute)
  world.touched.set(absolute, ++world.touches)
  keepNewest(world.touched, MAX_TOUCHED)
  return isNew
}

/**
 * Reads the files a resumed session's tools already touched, so a name
 * mentioned before this module loaded still finds its file.
 *
 * @param $ the session.start hook's engine interface
 * @param world the module's world
 * @throws never: a failed read leaves the touched files as they were
 */
async function readTouched($: EngineInterface, world: World): Promise<void> {
  // The platform must be known before a path is read as absolute or not.
  await place($, world)
  const messages = await $.session.messages().catch(() => undefined)
  if (messages === undefined) return
  let isChanged = false
  for (const message of messages) {
    for (const use of message.toolUses) {
      if (!(FILE_TOOLS as readonly string[]).includes(use.tool)) continue
      const path = use.input['file_path'] ?? use.input['notebook_path']
      if (typeof path === 'string' && noteTouched(world, path)) isChanged = true
    }
  }
  if (isChanged) bump(world)
}

/**
 * Finds the top of the repository a folder lies in: the nearest folder at or
 * above it that holds `.git` (a folder, or a file in a worktree or submodule).
 *
 * Read from the disk rather than asked of `git rev-parse`: on a Mac without
 * the command-line developer tools, any `git` call opens the system's install
 * dialog, so the mod runs git only where a repository is there.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world
 * @param folder an absolute normalized folder
 * @returns the repository's top folder, normalized; undefined outside one
 */
async function repoTopOf($: EngineInterface, world: World, folder: string): Promise<string | undefined> {
  let current: string | undefined = folder
  for (let depth = 0; current !== undefined && depth < MAX_REPO_DEPTH; depth++) {
    if (await existsAt($, world, joinPath(current, '.git'))) return current
    current = parentOf(current)
  }
  return undefined
}

/**
 * Builds the index of the session's repository with `git ls-files`, and
 * redraws the replies when the new index may place a span an earlier draw
 * left unlinked.
 *
 * @param $ the engine interface of the hook or timer that asked
 * @param world the module's world
 * @throws never: outside a repository, or when git fails or is missing, the
 *   index stays as it was (or absent) and the failure goes to the debug log
 */
async function buildIndex($: EngineInterface, world: World): Promise<void> {
  if (world.isIndexing) return
  world.isIndexing = true
  try {
    await place($, world)
    const folder = world.root ?? world.cwd
    if (folder === undefined) return
    const topFolder = await repoTopOf($, world, folder)
    if (topFolder === undefined) return
    // Untracked files count too (a file the session just wrote is one), but
    // not ignored ones (build output would make every name ambiguous).
    // `--cached` also lists tracked files deleted from the disk, so the
    // second list takes those out: a link must never open a missing file.
    const [listed, deleted] = await Promise.all([
      $.process.run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], {
        cwd: topFolder,
        timeoutMs: INDEX_TIMEOUT_MS,
      }),
      $.process.run(['git', 'ls-files', '-z', '--deleted'], { cwd: topFolder, timeoutMs: INDEX_TIMEOUT_MS }),
    ])
    if (listed.exitCode !== 0) return
    // A failed second list leaves the deleted files in: the index is still
    // right for every file on the disk, and a click on a stale one shows it.
    const gone = new Set(deleted.exitCode === 0 ? deleted.stdout.split('\0') : [])
    const byName = new Map<string, string[]>()
    let size = 0
    for (const relative of listed.stdout.split('\0')) {
      if (relative === '' || gone.has(relative)) continue
      const absolute = joinPath(topFolder, relative)
      const name = baseName(relative).toLowerCase()
      const files = byName.get(name)
      if (files === undefined) byName.set(name, [absolute])
      else files.push(absolute)
      size++
    }
    const isChanged = world.index === undefined || world.index.size !== size || world.index.top !== topFolder
    world.index = { top: topFolder, byName, size }
    world.indexedAt = await $.clock.now()
    if (!isChanged) return
    bump(world)
    // Only a draw that found nothing can gain from the new index: redrawing
    // every reply after each build would cost a hop per mounted row for nothing.
    if (world.hasMissed) {
      world.hasMissed = false
      $.ui.invalidate('ui.render')
    }
  } catch (error) {
    $.ui.log(`index build failed: ${String(error)}`, { to: 'debug' })
  } finally {
    world.isIndexing = false
  }
}

/**
 * Answers whether a path exists, from the world's cache when it can.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world
 * @param path an absolute normalized path
 * @returns true when a file or folder is there; false when not, or when the
 *   file system refuses the path (a network location)
 */
async function existsAt($: EngineInterface, world: World, path: string): Promise<boolean> {
  const known = world.exists.get(path)
  if (known !== undefined) return known
  const isThere = await $.fs.exists(path).catch(() => false)
  world.exists.set(path, isThere)
  keepNewest(world.exists, MAX_EXISTS)
  return isThere
}

/**
 * Finds this session's scratchpad folder from Claude Code's own layout (see
 * scratchpadPathOf), so the names and relative paths a reply writes about
 * files there link like those in the working folder.
 *
 * Claude Code names the folder after the session's ORIGINAL working folder,
 * which no API gives: each spelling in `projectSpellings` is tried, and the
 * first scratchpad that exists is taken. Footgun: this is one build's layout
 * (2.1.292). When a later build moves it, no scratchpad is found, and its
 * files link by their full path only.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world, placed
 * @returns the scratchpad folder, normalized; undefined when the session has
 *   none (yet), or no variable names a temp root
 * @throws never: every failure reads as no scratchpad
 */
async function scratchpadFolderOf($: EngineInterface, world: World): Promise<string | undefined> {
  const sessionId = await $.session.id().catch(() => undefined)
  if (sessionId === undefined) return undefined
  const known = world.scratchpad
  // A hit holds for the session; a miss only until the next generation (a
  // Write, a new index) or the end of a turn, since the engine makes the
  // folder when it is needed. A new session id (after /clear) is another
  // scratchpad: look again.
  if (known !== undefined && known.sessionId === sessionId && (known.folder !== undefined || known.generation === world.generation)) {
    return known.folder
  }
  const wasMissing = known !== undefined && known.sessionId === sessionId && known.folder === undefined
  const folder = await findScratchpad($, world, sessionId)
  if (folder !== undefined && wasMissing) {
    // Replies worked out while it was missing were kept without their
    // scratchpad links: drop them and redraw once (a hit is final, so this
    // runs once per session).
    bump(world)
    $.ui.invalidate('ui.render')
  }
  world.scratchpad = { sessionId, folder, generation: world.generation }
  return folder
}

/**
 * Looks for a session's scratchpad under Claude Code's temp folder, one
 * spelling of the project after another.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world, placed
 * @param sessionId the session's id
 * @returns the first scratchpad folder that exists, normalized; undefined
 *   when the temp folder or every candidate is missing
 */
async function findScratchpad($: EngineInterface, world: World, sessionId: string): Promise<string | undefined> {
  const base = world.tempBase ?? (await tempBaseOf($, world))
  if (base === undefined) return undefined
  for (const spelling of world.projectSpellings) {
    const folder = scratchpadPathOf(base, spelling, sessionId)
    // Asked of the disk, not the exists cache: this runs only when no answer
    // is kept, and a cached miss would hide a folder made since.
    if (await $.fs.exists(folder).catch(() => false)) return folder
  }
  return undefined
}

/**
 * Resolves Claude Code's temp folder to the real path its scratchpads lie
 * under, and keeps it in `world.tempBase` once it resolves.
 *
 * The engine builds on the REAL path of that folder (macOS: `/var/...` is
 * `/private/var/...`). Both spellings reach the same folder; the real one is
 * the one Claude is told, so links match what replies write.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world, placed
 * @returns the real path, normalized (the unresolved path when the engine
 *   gives no absolute real path); undefined when no temp root is named, or the
 *   path is missing or no folder (a stray `claude` file)
 */
async function tempBaseOf($: EngineInterface, world: World): Promise<string | undefined> {
  const claudeTemp = claudeTempOf(world.tempEnv, world.platform)
  if (claudeTemp === undefined) return undefined
  const stat = await $.fs.stat(claudeTemp, { resolve: true }).catch(() => undefined)
  if (stat === undefined || stat.kind !== 'dir') return undefined
  const real = stat.realPath
  // A real path that is not absolute would put every lookup somewhere else:
  // keep the unresolved spelling then, which reaches the folder too.
  const isAbsolute = real !== undefined && (real.startsWith('/') || /^[A-Za-z]:[\\/]/.test(real))
  world.tempBase = isAbsolute ? normalize(real) : claudeTemp
  return world.tempBase
}

/**
 * Spells a relative path or a name as the tail a matching file's path ends
 * with.
 *
 * @param written the path as written, `/` or `\` separated
 * @returns `/a/b`, lower-cased, `.` segments and a trailing separator dropped;
 *   undefined when it climbs (`..`), since a climb names no fixed tail
 */
function tailOf(written: string): string | undefined {
  const segments = written.replace(/\\/g, '/').split('/').filter(segment => segment !== '' && segment !== '.')
  if (segments.length === 0 || segments.includes('..')) return undefined
  return `/${segments.join('/').toLowerCase()}`
}

/**
 * Lists the touched files a relative path or a name fits, newest first.
 *
 * @param world the module's world
 * @param written the path as written, `/` or `\` separated
 * @returns the touched files whose path ends with it, compared without case;
 *   none for a path that climbs
 */
function touchedFits(world: World, written: string): string[] {
  const tail = tailOf(written)
  if (tail === undefined) return []
  return [...world.touched.entries()]
    .filter(([path]) => path.toLowerCase().endsWith(tail))
    .sort((a, b) => b[1] - a[1])
    .map(([path]) => path)
}

/**
 * Lists the repository's files a relative path or a name fits.
 *
 * @param world the module's world
 * @param written the path as written, `/` or `\` separated
 * @returns the indexed files whose path ends with it, compared without case;
 *   none when there is no index, and none for a path that climbs
 */
function indexFits(world: World, written: string): string[] {
  const tail = tailOf(written)
  if (tail === undefined) return []
  const files = world.index?.byName.get(baseName(tail)) ?? []
  return files.filter(path => path.toLowerCase().endsWith(tail))
}

/**
 * Takes the first of several paths that still exists: a touched file may have
 * been deleted since, or never written (a Write that failed).
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world
 * @param paths absolute normalized paths, best first
 * @returns the first that exists, or undefined
 */
async function firstExisting($: EngineInterface, world: World, paths: readonly string[]): Promise<string | undefined> {
  for (const path of paths) if (await existsAt($, world, path)) return path
  return undefined
}

/**
 * Finds the file a mention names, trying the most specific places first.
 *
 * An absolute path stands alone. A relative path is tried under the working
 * folder, the project root and the session's scratchpad, then matched
 * against the touched files, then against the repository. A name prefers the
 * newest touched file of that name (the one the conversation is about), then
 * the three folders, then the repository. Several repository files fit (the
 * same name in two folders): {@link bestOf} takes the shortest path. The
 * touched file comes first on purpose: after an edit of
 * `mods/prompt-queue/hooks/hooks.json`, a reply's `hooks.json` means that
 * file, not a shorter one in another folder.
 *
 * @param $ the calling hook's engine interface
 * @param world the module's world
 * @param mention the mention
 * @param scratchpad the session's scratchpad folder, tried after the working
 *   folder and the root; undefined when the session has none
 * @returns the absolute normalized path, or undefined when nothing fits
 */
async function targetOf($: EngineInterface, world: World, mention: Mention, scratchpad?: string): Promise<string | undefined> {
  if (mention.kind === 'absolute') {
    const absolute = absoluteOf(mention.path, world.home, world.platform)
    return absolute !== undefined && (await existsAt($, world, absolute)) ? absolute : undefined
  }
  // The scratchpad last of the three: a file of the project wins over a file
  // of the same name that Claude left in its scratchpad.
  const folders = [...new Set([world.cwd, world.root, scratchpad].filter((folder): folder is string => folder !== undefined))]
  if (mention.kind === 'name') {
    const touched = await firstExisting($, world, touchedFits(world, mention.path))
    if (touched !== undefined) return touched
  }
  for (const folder of folders) {
    const joined = joinPath(folder, mention.path)
    if (await existsAt($, world, joined)) return joined
  }
  if (mention.kind === 'relative') {
    const touched = await firstExisting($, world, touchedFits(world, mention.path))
    if (touched !== undefined) return touched
  }
  // git listed these files at the last build; one deleted since links to
  // nothing, which a click shows at once, so no existence check is spent here.
  // Several fit (the same name in two folders): the shortest path wins.
  return bestOf(indexFits(world, mention.path))
}

/**
 * Works out the text to draw for a reply's markdown: each mention that names
 * a file found by {@link targetOf} wrapped in a `file:///` link.
 *
 * @param $ the render hook's engine interface
 * @param world the module's world
 * @param text the reply's markdown
 * @returns the text to draw; the same string when no span named a file
 */
async function drawnTextOf($: EngineInterface, world: World, text: string): Promise<string> {
  const mentions = scan(text)
  if (mentions.length === 0) return text
  await place($, world)
  // Asked once per text, and only when a mention is placed by folder: an
  // absolute path needs no scratchpad, and the answer is cached per session.
  const scratchpad = mentions.some(mention => mention.kind !== 'absolute') ? await scratchpadFolderOf($, world) : undefined
  // One lookup per distinct path and kind: a reply often names a file twice.
  const lookups = new Map<string, Promise<string | undefined>>()
  for (const mention of mentions) {
    const key = `${mention.kind}\0${mention.path}`
    if (!lookups.has(key)) lookups.set(key, targetOf($, world, mention, scratchpad))
  }
  const targets = new Map<string, string | undefined>()
  await Promise.all([...lookups].map(async ([key, lookup]) => targets.set(key, await lookup)))
  if ([...targets.values()].includes(undefined)) world.hasMissed = true
  return linkify(text, mentions, mention => targets.get(`${mention.kind}\0${mention.path}`))
}

/**
 * Keeps a worked-out text, dropping the oldest past {@link MAX_DRAWN}.
 *
 * @param world the module's world
 * @param key `<generation>\0<text>`
 * @param drawn the text to draw for it
 */
function remember(world: World, key: string, drawn: string): void {
  world.drawn.set(key, drawn)
  if (world.drawn.size > MAX_DRAWN) {
    const oldest = world.drawn.keys().next().value
    if (oldest !== undefined) world.drawn.delete(oldest)
  }
}

/**
 * Words the `/file-links` command answers with: what the module knows.
 *
 * @param world the module's world
 * @param now the time, in `$.clock.now()` milliseconds
 * @param scratchpad the session's scratchpad folder; undefined when none was found
 * @returns one line per fact
 */
function statusText(world: World, now: number, scratchpad: string | undefined): string {
  const index =
    world.index === undefined
      ? 'no repository index (not in a git repository, or not built yet)'
      : `index: ${world.index.size} files in ${world.index.top}, built ${Math.max(0, Math.round((now - world.indexedAt) / 1000))} s ago`
  const hyperlinks = world.hyperlinks.isOn
    ? `on (${world.hyperlinks.why})`
    : `off (${world.hyperlinks.why}): names stay as written in this terminal; FORCE_HYPERLINK=1 turns links on`
  return [
    `folders: cwd ${world.cwd ?? '?'} · root ${world.root ?? '?'} · home ${world.home ?? '?'}`,
    `platform: ${world.platform} · terminal hyperlinks: ${hyperlinks}`,
    scratchpad === undefined
      ? 'scratchpad: none found (Claude Code makes it when the session first needs it; its files link by full path)'
      : `scratchpad: ${scratchpad}`,
    index,
    `touched files: ${world.touched.size} · path answers cached: ${world.exists.size} · replies worked out: ${world.drawn.size} (generation ${world.generation})`,
  ].join('\n')
}

export const register: Register = on => {
  const world = newWorld()

  on('session.start', async ($, e, next) => {
    // The folder the session starts in is the likeliest name of its
    // scratchpad (Claude Code names it after the original working folder),
    // so it is tried first.
    if (!world.projectSpellings.includes(e.cwd)) world.projectSpellings.unshift(e.cwd)
    const started = await next(e)
    // Not awaited: the engine awaits session.start before the first prompt,
    // and at start-up this one hop took 2.4 s live (the engine is busy then).
    void $.command
      .register({
        name: 'file-links',
        description: 'Shows what file-links knows: the folders, the repository index, the touched files.',
      })
      .catch(error => $.ui.log(`/file-links not registered: ${String(error)}`, { to: 'debug' }))
    // Background work outlives this hook: the first draws need not wait for git.
    void buildIndex($, world)
    void readTouched($, world)
    // A turn may create or delete files (a shell command, a checkout): the
    // index is rebuilt after one, at most once per poll.
    $.clock.every(INDEX_POLL_MS, () => {
      // A build still running would turn this one away (buildIndex returns
      // at once then) after the flag was cleared, and the turn's files would
      // wait for a later turn: keep the flag for the next poll instead.
      if (!world.isIndexStale || world.isIndexing) return
      world.isIndexStale = false
      void buildIndex($, world)
    })
    return started
  })

  on('turn.complete', ($, e, next) => {
    world.isIndexStale = true
    // A scratchpad not found yet may exist now: a shell command can make it,
    // and that moves no generation. Marking the miss stale makes the next
    // draw look again; found ones stay (they do not move).
    if (world.scratchpad !== undefined && world.scratchpad.folder === undefined) world.scratchpad = { ...world.scratchpad, generation: -1 }
    return next(e)
  })

  // Notes each file a tool reads or writes, so a bare name links to the one
  // the conversation is about. Display bookkeeping only: the call itself is
  // handed on unchanged, and the .catch hands it on if the note ever fails.
  // The tools are written out (they are FILE_TOOLS): a literal matcher is one
  // `claude plugin validate` and the engine read from the source.
  on('tool.call', { tool: ['Read', 'Edit', 'Write', 'NotebookEdit'] }, async ($, e, next) => {
    await place($, world)
    const path: unknown = e.tool === 'NotebookEdit' ? e.notebook_path : e.file_path
    // The schema says string, but the hook runs before the tool checks its
    // input: a malformed call must reach the tool's own error, not a throw here.
    if (typeof path === 'string' && noteTouched(world, path)) bump(world)
    const result = await next(e)
    // A Write may have made the file a draw just found missing: drop that miss.
    if (e.tool === 'Write') bump(world)
    return result
  }).catch(($, e, next) => next(e))

  on('ui.render', { component: 'AssistantMessage' }, async ($, e, next) => {
    // A phone cannot open this machine's files: a link there is only noise.
    if (e.surface === 'mobile') return next(e)
    await place($, world)
    // A terminal without hyperlinks (Apple Terminal, a plain console) would
    // show each added link as `name (file:///...)`: leave the reply as
    // written there. The desktop app and VS Code draw anchors, so they link.
    if (e.surface === 'terminal' && !world.hyperlinks.isOn) return next(e)
    const text = e.props.text
    const key = `${world.generation}\0${text}`
    let drawn = world.drawn.get(key)
    if (drawn === undefined) {
      try {
        drawn = await drawnTextOf($, world, text)
      } catch (error) {
        // A failure must never cost the reply its drawing: draw it as written.
        $.ui.log(`linking a reply failed: ${String(error)}`, { to: 'debug' })
        drawn = text
      }
      remember(world, key, drawn)
    }
    // Every other prop (isFirstOfReply, isSummary, onScreen) is handed on as
    // received: the engine refuses a rewrite that changes them.
    return drawn === text ? next(e) : next({ ...e, props: { ...e.props, text: drawn } })
  })

  on('command.run', { command: 'file-links' }, async $ => {
    await place($, world)
    const scratchpad = await scratchpadFolderOf($, world)
    return { text: statusText(world, await $.clock.now(), scratchpad) }
  })
}
