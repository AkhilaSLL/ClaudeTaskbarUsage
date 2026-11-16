# ClaudeTaskbarUsage

A small native Windows 11 program that shows the Claude plan usage in the taskbar, directly left
of the notification area:

    [Claude spark] 5h 3% | W 20% | F 27%

`5h` is the 5-hour session window, `W` the 7-day window, `F` the 7-day window scoped to the model
Fable. `F` shows only when the answer has a Fable window; without one the widget shows two values
and is narrower. One exe (about 100 KB), no main window,
no installer, no runtime dependency (C11, Win32, WinHTTP, statically linked; it imports only
Windows system DLLs). Idle: about 1 MB working set after start-up, about 4 MB private memory
with DirectWrite text (about 1.6 MB with the GDI fallback), no CPU between the 2 s placement
check and the data step.

## Use

- Hover: tooltip with each window ("5-hour: 24% used, resets 3:40 PM (in 2 h 13 min)"),
  extra windows ("Weekly Fable" directly after "Weekly", then for example "Weekly Opus"),
  "Updated 3 min ago" and a note when the data is stale.
- Double click: Refresh now. Left click: nothing.
- Right click: Refresh now / Update interval (5, 10, 15, 30 min) / Show remaining instead of used /
  Start with Windows / Exit.
- Colours: a value from 80 % used is orange, from 95 % red (the used value decides, also with
  "Show remaining"; the same for all three values). Stale data (failed request, login expired,
  rate limited) is dimmed; the icon is not.
- Icon: the Claude spark, taken from the 256 px icon of the installed Claude desktop app
  (`Square44x44Logo.targetsize-256_altform-unplated.png`) by `tools\logo_mask.py` into
  `src\logo_mask.h` (8-bit mask, source path and SHA-256 in its header), scaled at run time with an
  area-average filter to 20 px at 96 DPI and filled with the icon's orange.
- Settings: `%APPDATA%\ClaudeTaskbarUsage\settings.ini`; last good data: `last.json` there
  (percentages, reset times, fetch time; never the token).
- Single instance: a second start exits silently.

## Build

Needs `gcc` and `windres` from MSYS2 ucrt64 on PATH.

    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

Builds `build\ClaudeTaskbarUsage.exe` and `build\ctu_tests.exe` (with `-Wall -Wextra -Werror`),
runs the tests and fails on a test failure.

## Install, uninstall

    powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 [-RemoveSettings]

`install.ps1` stops an installed instance, copies the exe to
`%LOCALAPPDATA%\Programs\ClaudeTaskbarUsage\`, sets the HKCU `Run` value `ClaudeTaskbarUsage` and
starts it. `uninstall.ps1` stops it and removes the `Run` value and the install folder.

## Data rules

1. Credentials: `%USERPROFILE%\.claude\.credentials.json`, `claudeAiOauth.accessToken` and
   `expiresAt` (epoch ms). Read only, re-read before each request. The widget never writes this
   file. The token and the refresh token are never logged, printed or stored; buffers that held
   the token are wiped.
2. Token expired (the file exists): the widget lets the Claude CLI renew the login. It starts
   `%USERPROFILE%\.local\bin\claude.exe mcp list` (else `claude.exe` found on the search path)
   hidden, without the `CLAUDE*` and `ANTHROPIC*` environment variables, in the data folder, and
   waits at most 60 s ("Exit" ends it at once). The CLI renews the file; the widget reads it again
   and sends its request in the same step. At most one renewal per 30 minutes; "Refresh now"
   ignores the 30 minutes, not rule 5 or the 429 backoff. Side effect: `mcp list` connects once to
   each configured MCP server (about 20 s, about three times a day). `claude.exe auth status`
   does not renew the file. Without `claude.exe` there is no renewal.
   Still expired after the renewal, file missing or HTTP 401 (no renewal for these two): no
   request, last data dimmed, tooltip "Login expired. Open Claude Code to renew it."; next try at
   the next interval. While this note shows, the widget checks the modified time of the
   credentials file every 5 s. When the file changed, it tries again at once (rule 5 and the 429
   backoff still apply).
3. Request: `GET https://api.anthropic.com/api/oauth/usage` with `Authorization: Bearer ...`,
   `anthropic-beta: oauth-2025-04-20`, `Content-Type: application/json`,
   User-Agent `ClaudeTaskbarUsage/1.0`. WinHTTP, certificate checks on, redirects off (the token
   goes to no other host), 15 s timeouts, on a worker thread.
4. Answer: `five_hour` and `seven_day` with `utilization` (percent, floor, minimum 0, may be null)
   and `resets_at`. Every other key with a non-null `utilization` is an extra tooltip line.
   The `limits[]` list is always read as well (`kind`, `percent`, `resets_at`, `scope`):
   - `weekly_scoped` with `scope.model.display_name` becomes "Weekly <model>" (key
     `weekly_scoped:<model>`); the model "Fable" (any case) is the `F` value on the taskbar. Today
     the Fable limit exists only in this list.
   - `weekly_scoped` without a model name (for example a surface scope) becomes
     "Weekly (<scope>)", tooltip only.
   - `session` and `weekly_all` fill `5h` and `W` only when `five_hour` / `seven_day` gave no number
     (fallback); a key that is already in the list is not added twice.
   - Other kinds become tooltip lines.
5. At least 5 minutes between requests, also for "Refresh now". HTTP 429: back off 5 minutes,
   double on each further 429 up to 30 minutes (5, 10, 20, 30, 30), reset on success.
6. Claude Code's cache: `%USERPROFILE%\.claude.json`, key `cachedUsageUtilization`
   (`fetchedAtMs`, `utilization`). Read before each request and every 60 s when the file changed;
   when it is newer than our data it is used and counts as a fetch. An absent key is ignored.
7. The VS Code usage extension (`growthjack.claude-code-usage`): it polls the same endpoint every
   1 to 2 minutes with the same login and saves each answer in
   `quota-observations-v2.json` (labels in `quota-scope-labels-v1.json`). When it runs, our own
   request mostly gets HTTP 429, so its file is a shared source like Claude Code's cache.
   - The widget only reads these files (opened with full sharing, so the extension's
     temp-file-and-rename write never fails because of us); it never writes, moves or locks them.
   - Candidate folders, under `%APPDATA%`: `Code`, `Code - Insiders`, `VSCodium`, `Cursor`,
     `Antigravity`, each + `\User\globalStorage\growthjack.claude-code-usage`. The file with the
     newest modified time wins.
   - Read before each request and every 60 s, only when the file's time or size (or the labels
     file) changed, and only the last 64 KB. The complete records in that tail are found by brace
     matching (strings and escapes respected; the cut first record is skipped).
   - Records: `provider` `claude`, `source` `claude-official-api`, `schemaVersion` 2. Per
     `periodType` (`five-hour`, `seven-day`, `provider-scoped-seven-day`) the newest record; a
     window is used only when it is within 10 minutes of the newest record (one fetch = one set)
     and its `resetAt` is not past. Percent = `usedFraction` x 100, rounded to 6 decimals, floor.
   - The scoped record becomes "Weekly <label>" (key `weekly_scoped:<label>`, so Fable is the `F`
     value) only when the labels file has exactly one label; otherwise it is ignored.
   - When the newest record is newer than our data, the set is used and counts as a fetch. A
     missing, cut or unknown file is ignored silently.
8. Merge: the sources (our request, Claude Code's cache, the extension) are compared by data
   time; the newest wins as a whole set. Windows are never mixed or carried over from an older set.
9. Dimming: data from our request or Claude Code's cache is dimmed when the last attempt failed or
   the data is older than two intervals plus one minute. Data from the extension is dimmed only
   when it is older than two intervals plus one minute AND our own last request did not answer
   429 (a 429 shows that the extension still polls with success; the extension rewrites its file
   only when a value changed). The tooltip names the source: "Updated 3 min ago (live)",
   "(Claude Code cache)" or "(VS Code usage extension)"; with extension data and an own 429 it
   says "Own request rate limited; another tool is using the allowance."

## Test aids

- `--once <file>`: no window; one data step (cache and extension file first, then a request when
  allowed), one line of result to `<file>` (percentages, reset times, source `cache`, `extension`,
  `live`, `saved` or `sample`, the data time, or the error), exit code 0 (data, no error note) or 1.
  `last.json` stores the source too (`live`, `cache`, `extension`, `sample`); older files without
  it still load.
- `CTU_EXT_DIR=<folder>`: the only folder searched for the extension's files (replaces the
  candidate list). Without it, sample mode (`CTU_SAMPLE`) does not read the extension's files.
- `CTU_SAMPLE=<path to a sample answer>`: the sample file replaces the network step (no token, no
  Claude Code cache, `last.json` read but not written). `CTU_SAMPLE_STATUS=<code>` sets the HTTP
  status of the sample (for example 429 or 401). Samples: `tests\samples\`.
- `CTU_CRED_FILE=<path>`: this file replaces `%USERPROFILE%\.claude\.credentials.json`. In sample
  mode with `CTU_CRED_FILE` set, the credential check and the renewal run; the sample file still
  replaces the network step. In sample mode without it: no credential check, no renewal.
- `CTU_RENEW_CMD=<command line>`: replaces `claude.exe mcp list` of data rule 2 (still hidden, with
  the filtered environment, the job object and the 60 s limit). `CTU_RENEW_CMD=off`: no renewal.
  Example: `cmd.exe /c copy /y <valid invented file> <CTU_CRED_FILE>`.
- `CTU_THEME=light|dark`: overrides the taskbar theme (the registry is not touched).
- `CTU_TEXT=gdi`: draws the text with the GDI fallback instead of DirectWrite (see "Text").
- `CTU_MODE=popup`: uses the fallback placement (topmost popup) instead of the taskbar child window.
- `CTU_DATA_DIR=<dir>`: settings and `last.json` there instead of `%APPDATA%\ClaudeTaskbarUsage`.
- `CTU_INSTANCE=<id>` (letters, digits, `-`, `_`; at most 32): a second instance with its own
  single-instance mutex (`Local\ClaudeTaskbarUsage.SingleInstance.<id>`), so a test build can run
  next to the installed widget. Use it with `CTU_DATA_DIR` and `CTU_SAMPLE`.
- `CTU_OFFSET_X=<pixels>`: moves the widget left (negative) or right from its place, so that a
  test instance does not cover the installed one.
- `tools\capture.ps1`, `tools\run_test_instance.ps1`, `tools\ui_test.ps1`: screenshots, a test
  instance, the menu command path (WM_COMMAND), the tooltip and single-instance checks.
- `tools\render_preview.c`: renders the widget off screen over a light and a dark taskbar colour
  (optional third value).
- `tools\logo_mask.py <icon.png> src\logo_mask.h <label>`: rebuilds the icon mask (Python 3 with
  Pillow; deterministic). Screenshots of the three-value form, the two-value form and the icon
  are in `docs\` (`fable_*.png`, `two_values*.png`, `logo_light*.png`, `logo_compare.png`).
- `run_test_instance.ps1 -ExtDir <folder> -SampleStatus <code>` sets `CTU_EXT_DIR` and
  `CTU_SAMPLE_STATUS`. `docs\ext_429_not_dimmed.png` (old extension data, own 429: bright),
  `docs\ext_old_500_dimmed.png` (old extension data, HTTP 500: dimmed) and
  `docs\ext_dimming_zoom.png` (both, 3x) show the dimming rule. Test samples of the extension's
  files (invented): `tests\samples\ext\`.

## Placement

The widget is a layered window (`WS_EX_LAYERED`, per-pixel alpha with `UpdateLayeredWindow`)
made a child of `Shell_TrayWnd`, so it hides with the taskbar under a full-screen app and moves
with auto-hide. It is placed a few pixels left of `TrayNotifyWnd`, over the full taskbar height,
and re-checked every 2 s and on display, setting and DPI changes; after an Explorer restart
(`TaskbarCreated`, or a parent handle that is no longer valid) it attaches again. The manifest
declares Windows 8+ compatibility, which a layered child window needs. The background
has alpha 1/255 (invisible) so that the whole rectangle takes the mouse.

## Text

- DirectWrite through Direct2D, like the taskbar clock: a software `ID2D1DCRenderTarget`
  (B8G8R8A8_UNORM, premultiplied alpha, 96 DPI) bound to a separate premultiplied layer DIB,
  grayscale text antialiasing, then composited (source-over) onto the widget image.
  `d2d1.dll` and `dwrite.dll` are loaded at run time from System32, so the exe still imports only
  system DLLs and the GDI fallback works without them. Factories are made once and released at exit.
- Rendering parameters: the system gamma, enhanced contrast and grayscale contrast, ClearType level
  0, flat pixel geometry, rendering mode NATURAL_SYMMETRIC.
- Font: "Segoe UI Variable Text" (else "Segoe UI"), 12 px at 96 DPI (scaled with the DPI);
  numbers SEMI_BOLD, labels regular. Baselines and x positions are whole pixels; the cap height
  is centred. The digits are 8 px high, the same as the clock's.
- Label alpha: 0.80 on a dark taskbar, 0.72 on a light one. Stale numbers use alpha 0.55; labels
  are not dimmed.
- GDI fallback (when Direct2D or DirectWrite fails): the numbers use the real semibold face
  "Segoe UI Variable Text Semibold" at FW_NORMAL, else "Segoe UI Semibold", else regular
  "Segoe UI"; `GetTextFaceW` must return the requested face, so GDI never synthesises bold.
- Screenshots (6x): `docs\text_before_6x.png` (old GDI text with synthetic bold),
  `docs\text_after_6x.png`, `docs\text_vs_clock_6x.png`, `docs\text_states_6x.png` (stale,
  orange/red, GDI fallback, light colours).

Known limit: the Windows 11 taskbar does not know about the widget. With many taskbar buttons
(labels shown, left-aligned) the buttons can run under the widget.
