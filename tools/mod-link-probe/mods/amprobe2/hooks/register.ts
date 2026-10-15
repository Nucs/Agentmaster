import type { Register } from 'claude-code'

/**
 * Says hello to the probe server once the session starts, so the server log shows whether a plugin
 * named by CLAUDE_CODE_PLUGIN_DIRS loads beside one named by --plugin-dir.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const tcp = (await $.env.get('AMP_TCP')) ?? ''
    try {
      await $.http.fetch(`${tcp}/hello?via=amprobe2-from-env-dirs`)
    } catch (error) {
      $.ui.log(`amprobe2: hello failed: ${String(error)}`, { to: 'debug' })
    }
    return started
  })
}
