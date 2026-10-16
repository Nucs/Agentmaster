import type { Register } from 'claude-code'

// A bare specifier, the way an npm dependency is normally imported. The package sits in this plugin's
// own node_modules folder; if the loader does not resolve it, this module does not load at all, and the
// headless run says why on stderr.
import { hello } from 'am-vendor-test'

/**
 * Writes `<AMP_OUT>/libprobe2.json` once the session starts: present only if the bare import resolved.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const outDir = (await $.env.get('AMP_OUT')) ?? ''
    await $.fs.write(`${outDir}/libprobe2.json`, JSON.stringify({ loaded: true, hello: hello() }))
    return started
  })
}
