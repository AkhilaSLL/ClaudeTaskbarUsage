import { atom, read, update } from 'claude-code'
import type { EngineInterface, Register, SessionRateLimit, Timer } from 'claude-code'
import { meterReading, usageText } from './usage-text'

const REFRESH_MS = 60_000

// The usage text the SessionMode drawing reads: an update redraws it.
const text = atom({ plugin: 'usage-line', key: 'text' } as const, null)

// The 60 s refresh. A reload runs `register` again and starts this over.
let timer: Timer | undefined

// Set once the surface has raised SessionMode: the text is then drawn among
// the footer's mode labels, and the status entry (which the desktop app draws
// with the plugin's name before it) stays unset. Until then the status entry
// carries the text, so it shows even where SessionMode is never raised.
let isModeDrawn = false

/** The text of the taskbar meter's last reading, or undefined on any fault. */
async function readMeter($: EngineInterface): Promise<string | undefined> {
  try {
    let roaming = await $.env.get('APPDATA')

    if (roaming === undefined || roaming === '') {
      const profile = await $.env.get('USERPROFILE')
      if (profile === undefined || profile === '') return undefined
      roaming = `${profile}\\AppData\\Roaming`
    }

    const file = await $.fs.read(`${roaming}\\ClaudeTaskbarUsage\\last.json`)

    return typeof file === 'string' ? file : undefined
  } catch {
    return undefined
  }
}

async function show(
  $: EngineInterface,
  limits: readonly SessionRateLimit[],
): Promise<void> {
  const now = await $.clock.now()
  const line = usageText(limits, now, meterReading(await readMeter($), now))

  await update($, text, () => line ?? null)

  if (!isModeDrawn) $.ui.status(line)
}

async function refresh($: EngineInterface): Promise<void> {
  await show($, (await $.session.usage()).rateLimits)
}

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)

    timer?.cancel()
    timer = $.clock.every(REFRESH_MS, () => {
      void refresh($)
    })
    await refresh($)

    return started
  })

  on('ui.render', { component: 'SessionMode' }, async ($, e, next) => {
    if (!isModeDrawn) {
      isModeDrawn = true
      $.ui.status(undefined)
    }

    const line = await read($, text)
    if (line === null) return next(e)

    // The desktop app does not show a rewritten `modes`, but it draws a tree
    // (both checked in the app, 2026-11-17).
    const label = [line, ...e.props.modes].join(' & ')

    return h($.ui.resolve(e).Text, { dimColor: true }, label) as never
  })

  on('session.measure', async ($, e, next) => {
    await show($, e.rateLimits)

    return next(e)
  })
}
