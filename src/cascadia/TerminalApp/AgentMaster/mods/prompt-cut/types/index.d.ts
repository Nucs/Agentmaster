/**
 * What prompt-cut knows about one placeholder id of this session's prompt box,
 * kept so a cut can expand `[Pasted text #N +M lines]` back into its text:
 * - `text`: the engine made the placeholder, and this is the text behind it,
 *   read from the clipboard right after the paste (`clipboard`) or rebuilt
 *   from the longer draft the engine cut it out of (`rebuilt`);
 * - `unread`: the engine made the placeholder, but its text could not be read
 *   (the clipboard no longer held the paste, or the draft it came from was
 *   not known); a cut stops and asks for a second press before it cuts the
 *   placeholder as written;
 * - `literal`: the id was only ever typed or pasted as plain text that looks
 *   like a placeholder; a cut keeps it as written, as the engine does.
 *
 * Ids are the engine's own, minted per process: a draft recalled from an
 * older session can reuse one, which is why a `text` record is only used
 * where its line count still fits the placeholder.
 */
export type PromptCutPaste =
  | {
      readonly kind: 'text'
      /** The text behind the placeholder, as the engine keeps it (line breaks `\n`, tabs as four spaces). */
      readonly text: string
      /** The placeholder's `+M lines` count when it was read (0 when it showed none). */
      readonly lines: number
      /** Where the text came from: the clipboard at the paste, or a rebuild of a truncation. */
      readonly via: 'clipboard' | 'rebuilt'
    }
  | {
      readonly kind: 'unread'
      /** The placeholder's `+M lines` count; null when it showed none. */
      readonly lines: number | null
    }
  | { readonly kind: 'literal' }

/**
 * The armed Enter as the hint row shows it: the draft a cut stopped at
 * because a paste's text is unknown, and when the arm runs out. While it
 * stands, the next Enter from the box cuts that draft instead of sending it
 * (Claude Code expands every paste on its own send, which the mod catches and
 * drops). Null when nothing is armed.
 *
 * Kept in `$.state` only so that a change redraws the hint row: the module
 * holds the arm itself, and a reload of the mod (which starts the module
 * afresh) leaves nothing armed, whatever this says.
 */
export type PromptCutArm = {
  /** The draft as the box held it when armed, placeholders and all. */
  readonly text: string
  /** When the arm runs out, in milliseconds since the epoch. */
  readonly until: number
}

declare module 'claude-code' {
  interface PluginState {
    'prompt-cut': {
      /** The armed Enter (PromptCutArm), or null; read by the hint row. */
      armed: PromptCutArm | null
      /**
       * One record per placeholder id the box has held this session, keyed by
       * the id as a string; null once evicted. In `$.state` so a reload of the
       * mod (an edit, `/reload-plugins`) keeps what the pastes held.
       */
      paste: StateFamily<PromptCutPaste | null>
      /**
       * The ids of {@link paste} records still held, oldest first: the eviction
       * order that keeps the session's records to the newest 200.
       */
      pasteIds: readonly string[]
    }
  }
}
