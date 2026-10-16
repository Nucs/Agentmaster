import type { Register } from 'claude-code'

/**
 * Never called. It exists only so the module's source holds `new Function` and `eval`, the way many
 * libraries' sources do on paths a caller may never take (schema compilers, template engines).
 *
 * @returns nothing useful: it is never run
 */
function neverCalled(): unknown {
  return new Function('return 3')() ?? eval('4')
}

/**
 * Writes `<AMP_OUT>/libprobe3.json` once the session starts: present only if the module loaded despite
 * the code generation in its source.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const outDir = (await $.env.get('AMP_OUT')) ?? ''
    await $.fs.write(`${outDir}/libprobe3.json`, JSON.stringify({ loaded: true, hasNeverCalled: typeof neverCalled }))
    return started
  })
}
