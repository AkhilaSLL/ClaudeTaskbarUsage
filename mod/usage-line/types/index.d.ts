/** The usage text the footer shows, or null while no window has a reading. */
export type UsageLine = string | null

declare module 'claude-code' {
  interface PluginState {
    'usage-line': { text: UsageLine }
  }
}
