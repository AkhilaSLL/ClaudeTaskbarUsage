# usage-line

A mod for Claude Code and the Claude desktop app (Code tab). It shows the Claude plan usage in the
footer row below the prompt box, for example:

    5h 23% | W 43% | F 41%

`5h` is the 5-hour window, `W` the weekly window and `F` the weekly window of the model Fable. The
values are percent used, rounded down.

## Install

In a terminal:

    claude plugin install usage-line --marketplace AkhilaSLL/ClaudeTaskbarUsage

In the Claude desktop app: click **+** next to the prompt box, then **Plugins**, **Add marketplace**
with `AkhilaSLL/ClaudeTaskbarUsage`, then **Add plugin**.

Then start a new session, or send one message in an open session. A session draws the text only
after its engine has started.

Remove it with `claude plugin uninstall usage-line@claude-taskbar-usage`.

## Where the values come from

- `5h` and `W`: the session's own rate-limit readings. A session has none until its first reply.
- `F`, and `5h` and `W` while the session has no reading: the last reading of the
  ClaudeTaskbarUsage taskbar meter, `%APPDATA%\ClaudeTaskbarUsage\last.json`, when it is at most
  30 minutes old. Without the meter there is no `F` part.

The mod sends no network request and reads no login file.

## Checks

    claude plugin validate mod/usage-line
    claude plugin test mod/usage-line
