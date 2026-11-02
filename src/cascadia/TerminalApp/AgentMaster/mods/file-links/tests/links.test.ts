import { describe, expect, test } from 'claude-code/testing'

import {
  absoluteOf,
  bestOf,
  claudeTempOf,
  fileUri,
  hyperlinksOf,
  linkify,
  MAX_MENTIONS,
  normalize,
  parentOf,
  pathOf,
  platformOf,
  projectKeyOf,
  scan,
  scratchpadPathOf,
  trimmedBounds,
  type HyperlinkEnv,
  type Mention,
  type Platform,
} from '../hooks/links'

/**
 * The files a test's resolver places, by the path as written: what register.ts
 * would find on disk, in the touched files or in the repository index.
 */
const PLACED: Record<string, string> = {
  'C:\\Users\\me\\notes.md': 'C:/Users/me/notes.md',
  'C:\\Users\\me\\.claude\\CLAUDE.md': 'C:/Users/me/.claude/CLAUDE.md',
  'C:\\Program Files\\Git\\bin\\bash.exe': 'C:/Program Files/Git/bin/bash.exe',
  'hooks/register.ts': 'C:/proj/hooks/register.ts',
  'register.ts': 'C:/proj/hooks/register.ts',
  'README.md': 'C:/proj/README.md',
  '__init__.py': 'C:/proj/pkg/__init__.py',
  'mods/chat-view/': 'C:/proj/mods/chat-view',
  'C:\\proj\\a (1).txt': 'C:/proj/a (1).txt',
}

/**
 * Links a text the way register.ts does, with {@link PLACED} as the resolver.
 *
 * @param text a reply's markdown
 * @returns the text to draw
 */
function linked(text: string): string {
  return linkify(text, scan(text), (mention: Mention) => PLACED[mention.path])
}

describe('pathOf', () => {
  test('reads drive, Git Bash and home paths as absolute', () => {
    expect(pathOf('C:\\x\\y.md', false)).toEqual({ path: 'C:\\x\\y.md', kind: 'absolute' })
    expect(pathOf('C:/x/y.md', false)).toEqual({ path: 'C:/x/y.md', kind: 'absolute' })
    expect(pathOf('/c/Users/me', false)).toEqual({ path: '/c/Users/me', kind: 'absolute' })
    expect(pathOf('~/.claude/settings.json', false)).toEqual({ path: '~/.claude/settings.json', kind: 'absolute' })
  })

  test('reads a path with a folder part as relative and a lone file name as a name', () => {
    expect(pathOf('hooks/register.ts', false)).toEqual({ path: 'hooks/register.ts', kind: 'relative' })
    expect(pathOf('./x/y', false)).toEqual({ path: './x/y', kind: 'relative' })
    expect(pathOf('mods/chat-view/', false)).toEqual({ path: 'mods/chat-view/', kind: 'relative' })
    expect(pathOf('register.test.ts', false)).toEqual({ path: 'register.test.ts', kind: 'name' })
    expect(pathOf('.gitignore', false)).toEqual({ path: '.gitignore', kind: 'name' })
  })

  test('cuts a line suffix off the path and keeps the kind', () => {
    expect(pathOf('register.ts:42', false)).toEqual({ path: 'register.ts', kind: 'name' })
    expect(pathOf('hooks/x.ts:12:5', false)).toEqual({ path: 'hooks/x.ts', kind: 'relative' })
    expect(pathOf('C:/x/y.ts#L10-L20', false)).toEqual({ path: 'C:/x/y.ts', kind: 'absolute' })
  })

  test('refuses what is no path: URLs, flags, globs, versions, decimals, words, a lone slash', () => {
    for (const written of ['https://x.com/a.md', 'file:///C:/x', 'mailto:a@b.c', '--plugin-dir', '*.ts', 'a|b', '2.1.292', '3.5', 'word', 'git status', '/', '//server/x', '']) {
      expect(pathOf(written, true)).toBeUndefined()
    }
  })

  test('reads a POSIX path as absolute, so a macOS reply links /Users/... paths', () => {
    expect(pathOf('/Users/me/x.md', false)).toEqual({ path: '/Users/me/x.md', kind: 'absolute' })
    expect(pathOf('/etc/hosts', false)).toEqual({ path: '/etc/hosts', kind: 'absolute' })
    expect(pathOf('/Users/me/Library/Application Support/x.json', true)).toEqual({
      path: '/Users/me/Library/Application Support/x.json',
      kind: 'absolute',
    })
    expect(pathOf('/Users/me/Library/Application Support/x.json', false)).toBeUndefined()
  })

  test('takes spaces only in a code span, and only in an absolute path', () => {
    expect(pathOf('C:\\Program Files\\Git', true)).toEqual({ path: 'C:\\Program Files\\Git', kind: 'absolute' })
    expect(pathOf('C:\\Program Files\\Git', false)).toBeUndefined()
    expect(pathOf('my folder/x.ts', true)).toBeUndefined()
  })
})

describe('trimmedBounds', () => {
  test('peels punctuation and emphasis pairs, layer by layer', () => {
    const cut = (token: string) => {
      const { head, tail } = trimmedBounds(token)
      return token.slice(head, tail)
    }
    expect(cut('(see')).toBe('see')
    expect(cut('(**hooks/x.ts**),')).toBe('hooks/x.ts')
    expect(cut('"C:\\x\\y.md".')).toBe('C:\\x\\y.md')
    expect(cut('__init__.py')).toBe('__init__.py')
    expect(cut('_x.md_')).toBe('x.md')
    expect(cut('...')).toBe('')
  })
})

describe('scan and linkify', () => {
  test('links a code span, keeping its backticks inside the link text', () => {
    expect(linked('Edit `hooks/register.ts` next.')).toBe(
      'Edit [`hooks/register.ts`](file:///C:/proj/hooks/register.ts) next.',
    )
  })

  test('links a bare path and escapes the backslashes markdown would eat', () => {
    expect(linked('Read C:\\Users\\me\\.claude\\CLAUDE.md first')).toBe(
      'Read [C:\\\\Users\\\\me\\\\.claude\\\\CLAUDE.md](file:///C:/Users/me/.claude/CLAUDE.md) first',
    )
  })

  test('keeps a line suffix in the link text and out of the URI', () => {
    expect(linked('See `register.ts:42`.')).toBe('See [`register.ts:42`](file:///C:/proj/hooks/register.ts).')
  })

  test('keeps trailing punctuation and emphasis outside the link', () => {
    expect(linked('Open **README.md**, then hooks/register.ts.')).toBe(
      'Open **[README.md](file:///C:/proj/README.md)**, then [hooks/register.ts](file:///C:/proj/hooks/register.ts).',
    )
  })

  test('escapes emphasis characters in a bare name, so it draws as written', () => {
    expect(linked('Add __init__.py here')).toBe('Add [\\_\\_init\\_\\_.py](file:///C:/proj/pkg/__init__.py) here')
  })

  test('links a folder named with a trailing separator', () => {
    expect(linked('Under mods/chat-view/ sits the mod.')).toBe(
      'Under [mods/chat-view/](file:///C:/proj/mods/chat-view) sits the mod.',
    )
  })

  test('encodes spaces and parentheses in the URI', () => {
    expect(linked('Run `C:\\Program Files\\Git\\bin\\bash.exe` and open `C:\\proj\\a (1).txt`.')).toBe(
      'Run [`C:\\Program Files\\Git\\bin\\bash.exe`](file:///C:/Program%20Files/Git/bin/bash.exe) and open [`C:\\proj\\a (1).txt`](file:///C:/proj/a%20%281%29.txt).',
    )
  })

  test('leaves fenced code blocks alone, backtick and tilde, nested in a list too', () => {
    const text = [
      '```bash',
      'cat hooks/register.ts',
      '```',
      '- item',
      '  ~~~',
      '  README.md',
      '  ~~~',
      'after README.md',
    ].join('\n')
    expect(linked(text)).toBe(text.replace('after README.md', 'after [README.md](file:///C:/proj/README.md)'))
  })

  test('a shorter run inside a fence does not close it', () => {
    const text = ['````', '```', 'README.md', '````', 'README.md'].join('\n')
    expect(linked(text)).toBe(['````', '```', 'README.md', '````', '[README.md](file:///C:/proj/README.md)'].join('\n'))
  })

  test('leaves links, images, reference links and autolinks as the model wrote them', () => {
    const text = [
      'See [CLAUDE.md:27](file:///C:/x/CLAUDE.md) and ![shot](README.md).',
      'Also [README.md][1] and <https://example.com/README.md>.',
      'A URL https://example.com/hooks/register.ts stays.',
    ].join('\n')
    expect(linked(text)).toBe(text)
  })

  test('a lone bracket is plain text, and a path after it still links', () => {
    expect(linked('[draft] README.md')).toBe('[draft] [README.md](file:///C:/proj/README.md)')
  })

  test('an escaped backtick opens no code span, so the real span after it still links', () => {
    // Read without the escape, the first two backticks would pair up as a
    // code span holding " b " and leave `README.md` with a stray backtick.
    expect(linked('a \\` b `README.md`')).toBe('a \\` b [`README.md`](file:///C:/proj/README.md)')
  })

  test('links inside a table cell', () => {
    expect(linked('| file | role |\n| --- | --- |\n| `register.ts` | hooks |')).toBe(
      '| file | role |\n| --- | --- |\n| [`register.ts`](file:///C:/proj/hooks/register.ts) | hooks |',
    )
  })

  test('links several mentions of one line in order, and repeats', () => {
    expect(linked('README.md, then register.ts, then README.md')).toBe(
      '[README.md](file:///C:/proj/README.md), then [register.ts](file:///C:/proj/hooks/register.ts), then [README.md](file:///C:/proj/README.md)',
    )
  })

  test('hands back the very same string when nothing is placed', () => {
    const text = 'Nothing here names ui.render or e.props.text or 2.1.292 as a file.'
    expect(scan(text).length > 0).toBe(true)
    expect(linked(text)).toBe(text)
  })

  test('commands and sentences in code spans are not mentions', () => {
    expect(scan('Run `git status` and `claude plugin validate`.')).toEqual([])
  })

  test('stops at MAX_MENTIONS', () => {
    expect(scan(Array.from({ length: MAX_MENTIONS + 50 }, (_, i) => `f${i}.md`).join(' ')).length).toBe(MAX_MENTIONS)
  })

  test('reports the span, raw text, path and kind of each mention', () => {
    expect(scan('x `y/z.ts:3` w.md')).toEqual([
      { start: 2, end: 12, raw: '`y/z.ts:3`', path: 'y/z.ts', kind: 'relative', isCode: true },
      { start: 13, end: 17, raw: 'w.md', path: 'w.md', kind: 'name', isCode: false },
    ])
  })
})

describe('paths and URIs', () => {
  test('normalize folds separators, dot segments and the drive case', () => {
    expect(normalize('c:\\a\\.\\b\\..\\c\\')).toBe('C:/a/c')
    expect(normalize('C:/a//b')).toBe('C:/a/b')
    expect(normalize('C:/..')).toBe('C:/')
  })

  test('normalize keeps a climb past the root at the root, on both platforms', () => {
    expect(normalize('C:/a/../../x')).toBe('C:/x')
    expect(normalize('/a/../../x')).toBe('/x')
    expect(normalize('/..')).toBe('/')
  })

  test('absoluteOf on Windows spells Git Bash and home paths as drive paths, and refuses POSIX ones', () => {
    expect(absoluteOf('/c/Users/me/x.md', undefined, 'windows')).toBe('C:/Users/me/x.md')
    expect(absoluteOf('C:\\x\\y.md', undefined, 'windows')).toBe('C:/x/y.md')
    expect(absoluteOf('~/.claude/x.md', 'C:/Users/me', 'windows')).toBe('C:/Users/me/.claude/x.md')
    expect(absoluteOf('~/.claude/x.md', undefined, 'windows')).toBeUndefined()
    expect(absoluteOf('/etc/hosts', 'C:/Users/me', 'windows')).toBeUndefined()
    expect(absoluteOf('hooks/x.ts', 'C:/Users/me', 'windows')).toBeUndefined()
  })

  test('absoluteOf on macOS keeps /x as it is, reads /c/x as a folder, and refuses drive paths', () => {
    expect(absoluteOf('/Users/me/x.md', '/Users/me', 'posix')).toBe('/Users/me/x.md')
    expect(absoluteOf('/c/x.md', '/Users/me', 'posix')).toBe('/c/x.md')
    expect(absoluteOf('/Users/me/a/../b.md', '/Users/me', 'posix')).toBe('/Users/me/b.md')
    expect(absoluteOf('~/.claude/x.md', '/Users/me', 'posix')).toBe('/Users/me/.claude/x.md')
    expect(absoluteOf('C:\\x\\y.md', '/Users/me', 'posix')).toBeUndefined()
  })

  test('platformOf reads a drive as Windows and anything else as POSIX', () => {
    expect(platformOf('C:\\Users\\me')).toBe('windows')
    expect(platformOf('d:/work')).toBe('windows')
    expect(platformOf('/Users/me/proj')).toBe('posix')
    expect(platformOf(undefined)).toBe('posix')
  })

  test('parentOf climbs to a root on both platforms and stops there', () => {
    expect(parentOf('C:/a/b')).toBe('C:/a')
    expect(parentOf('C:/a')).toBe('C:/')
    expect(parentOf('C:/')).toBeUndefined()
    expect(parentOf('/a/b')).toBe('/a')
    expect(parentOf('/a')).toBe('/')
    expect(parentOf('/')).toBeUndefined()
  })

  test('fileUri encodes what a URI segment cannot hold', () => {
    expect(fileUri('C:\\Users\\me\\a b#c%d.md')).toBe('file:///C:/Users/me/a%20b%23c%25d.md')
    expect(fileUri('c:/x/(y)!.md')).toBe('file:///C:/x/%28y%29%21.md')
    expect(fileUri('C:/x/שלום.md')).toBe('file:///C:/x/%D7%A9%D7%9C%D7%95%D7%9D.md')
    expect(fileUri('/home/me/x.md')).toBe('file:///home/me/x.md')
    expect(fileUri('/Users/me/Library/Application Support/x.json')).toBe('file:///Users/me/Library/Application%20Support/x.json')
  })
})

describe('the scratchpad layout (Claude Code 2.1.292)', () => {
  test('projectKeyOf names a project as Claude Code does, a long one cut and hashed', () => {
    // Reference values computed apart from the mod (a Python Java-hash).
    expect(projectKeyOf('C:\\Users\\EliBelash\\.claude')).toBe('C--Users-EliBelash--claude')
    expect(projectKeyOf('/Users/me/proj')).toBe('-Users-me-proj')
    const long = `C:\\Users\\me\\${Array.from({ length: 8 }, (_, i) => `deeply-nested-project-folder-${String(i).padStart(2, '0')}`).join('\\')}`
    const key = projectKeyOf(long)
    expect(key.length).toBe(207)
    expect(key.endsWith('-deeply-nested-project-folder-fe6q2d')).toBe(true)
  })

  test('claudeTempOf follows CLAUDE_CODE_TMPDIR, then os.tmpdir(), on each platform', () => {
    expect(claudeTempOf({ TEMP: 'C:\\Users\\me\\AppData\\Local\\Temp\\' }, 'windows')).toBe('C:/Users/me/AppData/Local/Temp/claude')
    expect(claudeTempOf({ TMP: 'C:\\Temp' }, 'windows')).toBe('C:/Temp/claude')
    expect(claudeTempOf({ SystemRoot: 'C:\\Windows' }, 'windows')).toBe('C:/Windows/temp/claude')
    expect(claudeTempOf({}, 'windows')).toBeUndefined()
    expect(claudeTempOf({ CLAUDE_CODE_TMPDIR: 'D:\\scratch', TEMP: 'C:\\Temp' }, 'windows')).toBe('D:/scratch/claude')
    expect(claudeTempOf({ TMPDIR: '/var/folders/zz/abc/T/' }, 'posix')).toBe('/var/folders/zz/abc/T/claude')
    expect(claudeTempOf({ TMP: '/scratch' }, 'posix')).toBe('/scratch/claude')
    expect(claudeTempOf({}, 'posix')).toBe('/tmp/claude')
    expect(claudeTempOf({ CLAUDE_CODE_TMPDIR: '/data/tmp' }, 'posix')).toBe('/data/tmp/claude')
    expect(claudeTempOf({ CLAUDE_CODE_TMPDIR: 'relative/tmp' }, 'posix')).toBeUndefined()
  })

  test('scratchpadPathOf joins the temp folder, the project key, the session and "scratchpad"', () => {
    // This very session's scratchpad, as the system prompt names it.
    expect(scratchpadPathOf('C:/Users/ELIBEL~1/AppData/Local/Temp/claude', 'C:\\Users\\EliBelash\\.claude', '23c72bc8-8ae7-4eb0-841d-8ee42e311b68')).toBe(
      'C:/Users/ELIBEL~1/AppData/Local/Temp/claude/C--Users-EliBelash--claude/23c72bc8-8ae7-4eb0-841d-8ee42e311b68/scratchpad',
    )
    expect(scratchpadPathOf('/private/var/folders/zz/abc/T/claude', '/Users/me/proj', 's1')).toBe(
      '/private/var/folders/zz/abc/T/claude/-Users-me-proj/s1/scratchpad',
    )
  })
})

describe('hyperlinksOf', () => {
  /** Each case: the platform, the variables set, and whether Claude Code 2.1.292 draws hyperlinks then. */
  const CASES: readonly [Platform, HyperlinkEnv, boolean][] = [
    ['windows', { WT_SESSION: 'x' }, true],
    ['windows', {}, false],
    // The library's Windows build stops at WT_SESSION, and the engine's own
    // list has no vscode: Claude Code draws `text (url)` in VS Code there.
    ['windows', { TERM_PROGRAM: 'vscode', TERM_PROGRAM_VERSION: '1.95.0' }, false],
    ['windows', { WT_SESSION: 'x', TMUX: '/tmp/tmux' }, true],
    ['windows', { WT_SESSION: 'x', FORCE_HYPERLINK: '0' }, false],
    ['posix', { TERM_PROGRAM: 'vscode', TERM_PROGRAM_VERSION: '1.95.0' }, true],
    ['posix', { TERM_PROGRAM: 'vscode', TERM_PROGRAM_VERSION: '1.71.2' }, false],
    ['posix', { TERM_PROGRAM: 'Apple_Terminal', TERM_PROGRAM_VERSION: '455' }, false],
    ['posix', { TERM_PROGRAM: 'Apple_Terminal', FORCE_HYPERLINK: '1' }, true],
    ['posix', { TERM_PROGRAM: 'iTerm.app', TERM_PROGRAM_VERSION: '3.0.0' }, true],
    ['posix', { TERM_PROGRAM: 'WezTerm', TERM_PROGRAM_VERSION: '20240203-110809-5046fc22' }, true],
    ['posix', { TERM_PROGRAM: 'ghostty' }, true],
    ['posix', { TERM_PROGRAM: 'tmux', TERM_PROGRAM_VERSION: '3.4' }, true],
    ['posix', { TERM_PROGRAM: 'tmux', TERM_PROGRAM_VERSION: '3.3a' }, false],
    ['posix', { LC_TERMINAL: 'iTerm2' }, true],
    ['posix', { TERM: 'xterm-kitty' }, true],
    ['posix', { TERM: 'alacritty' }, true],
    ['posix', { TERMINAL_EMULATOR: 'JetBrains-JediTerm' }, true],
    ['posix', { TERM: 'xterm-256color' }, false],
    // An empty FORCE_HYPERLINK counts as set for the engine (the library's
    // answer alone) and as unset for the library: kitty is then off, as the
    // engine draws it, while iTerm 3.5 and Windows Terminal stay on.
    ['posix', { FORCE_HYPERLINK: '', TERM_PROGRAM: 'kitty' }, false],
    ['posix', { FORCE_HYPERLINK: '', TERM_PROGRAM: 'iTerm.app', TERM_PROGRAM_VERSION: '3.5.0' }, true],
    ['windows', { FORCE_HYPERLINK: '', WT_SESSION: 'x' }, true],
  ]

  test('answers as Claude Code 2.1.292 does, terminal by terminal', () => {
    for (const [platform, env, isOn] of CASES) {
      const { isOn: answer } = hyperlinksOf(env, platform)
      expect(`${platform} ${JSON.stringify(env)}: ${answer}`).toBe(`${platform} ${JSON.stringify(env)}: ${isOn}`)
    }
  })

  test('names the variable that decided', () => {
    expect(hyperlinksOf({ WT_SESSION: 'x' }, 'windows').why).toBe('WT_SESSION')
    expect(hyperlinksOf({ TERM_PROGRAM: 'Apple_Terminal' }, 'posix').why).toBe('TERM_PROGRAM=Apple_Terminal')
    expect(hyperlinksOf({ FORCE_HYPERLINK: '1' }, 'posix').why).toBe('FORCE_HYPERLINK=1')
    expect(hyperlinksOf({}, 'windows').why).toBe('not Windows Terminal (WT_SESSION unset)')
    expect(hyperlinksOf({}, 'posix').why).toBe('no hyperlink terminal named')
  })
})

describe('bestOf', () => {
  test('takes the only match, and none of none', () => {
    expect(bestOf([])).toBeUndefined()
    expect(bestOf(['C:/other/x.md'])).toBe('C:/other/x.md')
  })

  test('takes the shortest path, in characters, whatever the order given', () => {
    const files = ['C:/proj/skills/codex-cli/SKILL.md', 'C:/proj/skills/curl/SKILL.md', 'C:/proj/skills/doctor/SKILL.md']
    expect(bestOf(files)).toBe('C:/proj/skills/curl/SKILL.md')
    expect(bestOf([...files].reverse())).toBe('C:/proj/skills/curl/SKILL.md')
    expect(bestOf(['C:/proj/a/b/x.md', 'C:/proj/x.md'])).toBe('C:/proj/x.md')
    // Fewer characters wins over fewer folders: the shortest path, as asked.
    expect(bestOf(['C:/proj/documentation/x.md', 'C:/proj/a/b/c/x.md'])).toBe('C:/proj/a/b/c/x.md')
  })

  test('breaks a tie in length by the fewest folders, then A to Z', () => {
    expect(bestOf(['C:/proj/a/b/x.md', 'C:/proj/abc/x.md'])).toBe('C:/proj/abc/x.md')
    expect(bestOf(['C:/proj/b/x.md', 'C:/proj/a/x.md'])).toBe('C:/proj/a/x.md')
    expect(bestOf(['C:/proj/B/x.md', 'C:/proj/a/x.md'])).toBe('C:/proj/a/x.md')
  })
})
