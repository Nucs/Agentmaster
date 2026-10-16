import type { Register } from 'claude-code'

// Two real npm packages, vendored as plain ES-module files and imported by relative path: fflate 0.8.2
// (its browser build, a gzip/deflate implementation) and nanoid 3.3.11 (its browser build, which draws
// from crypto.getRandomValues).
import { gunzipSync, gzipSync, strFromU8, strToU8 } from '../vendor/fflate.mjs'
import { nanoid } from '../vendor/nanoid/index.browser.js'

/** The globals asked about: what a networking or utility library would expect to find. */
const GLOBAL_NAMES = [
  'fetch', 'WebSocket', 'XMLHttpRequest', 'EventSource', 'setTimeout', 'setInterval', 'queueMicrotask',
  'require', 'module', 'process', 'Bun', 'Deno', 'navigator', 'Worker', 'MessageChannel', 'BroadcastChannel',
  'WebAssembly', 'SharedArrayBuffer', 'Atomics', 'ReadableStream', 'CompressionStream', 'Blob', 'TextEncoder',
  'TextDecoder', 'crypto', 'structuredClone', 'performance', 'URL', 'AbortController', 'console', 'Intl',
  'Proxy', 'WeakRef', 'FinalizationRegistry',
] as const

/**
 * Names the type of a global without referencing it statically, since a missing name would throw.
 *
 * @param name the global's name
 * @returns `typeof globalThis[name]`: "undefined" when the environment does not have it
 */
function kindOf(name: string): string {
  return typeof (globalThis as Record<string, unknown>)[name]
}

/**
 * Tries code generation the way libraries do it at run time (an indirect eval and `new Function`),
 * through computed names so the attempt itself is not what the loader reads in the source.
 *
 * @returns for each, the value produced or the text of what was thrown
 */
function tryCodegen(): Record<string, string> {
  const g = globalThis as Record<string, unknown>
  const result: Record<string, string> = {}
  try {
    result.eval = String((g['ev' + 'al'] as (source: string) => unknown)('1 + 1'))
  } catch (error) {
    result.eval = `threw: ${String(error)}`
  }
  try {
    const Ctor = g['Func' + 'tion'] as FunctionConstructor
    result.Function = String(new Ctor('return 2')())
  } catch (error) {
    result.Function = `threw: ${String(error)}`
  }
  return result
}

/**
 * Runs the checks once the session starts and writes them to `<AMP_OUT>/libprobe.json`.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const outDir = (await $.env.get('AMP_OUT')) ?? ''
    const text = 'agentmaster session event '.repeat(500)
    const t0 = performance.now()
    const zipped = gzipSync(strToU8(text))
    const back = strFromU8(gunzipSync(zipped))
    const ms = performance.now() - t0
    const globals: Record<string, string> = {}
    for (const name of GLOBAL_NAMES) globals[name] = kindOf(name)
    const report = {
      nanoid: nanoid(),
      fflate: { inChars: text.length, gzipBytes: zipped.length, roundTrip: back === text, ms: Math.round(ms * 100) / 100 },
      uint8FromBase64: typeof (Uint8Array as unknown as Record<string, unknown>).fromBase64,
      uint8ToBase64: typeof (Uint8Array.prototype as unknown as Record<string, unknown>).toBase64,
      subtleDigest: typeof crypto?.subtle?.digest,
      codegen: tryCodegen(),
      globals,
    }
    await $.fs.write(`${outDir}/libprobe.json`, JSON.stringify(report, null, 2))
    return started
  })
}
