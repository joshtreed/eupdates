# eupdates

<!-- TODO(josh): one-line tagline in your voice. Working draft: -->
> Counts pending Gentoo (portage) package updates for any status bar. Zero dependencies, one C file.

<!-- TODO(josh): badges once the repo is pushed — CI status, latest release, license (GPL-3.0). -->

<!-- TODO(josh): screenshot / asciinema of a bar module. Placeholder: -->
<!-- ![demo](doc/demo.png) -->

## Why

<!-- TODO(josh): your prose. Bullet points to draw from: -->
- Zero runtime dependencies beyond portage itself (which every Gentoo box has).
- No persistent database to maintain (unlike `eix`, which needs `eix-update`).
- Uses the **same** resolver as a real update (`emerge -puDN @world`), so the count matches what you'd actually merge.
- **Never touches a mirror** — it reads the already-synced local tree only. Syncing stays your call.
- Works with any status bar: plain text, a `--format` template, or JSON; poll it or stream it.
- No timer, cron job or service to set up: it notices syncs and emerges itself.
- Single purpose, fast to poll (results are cached; the slow resolve runs in the background).

## Install

From source:

```sh
make
sudo make install          # /usr/local by default; override PREFIX=/usr
```

Requirements: a C11 compiler, `scdoc` (man page), and Gentoo/portage at runtime. `glsa-check` (gentoolkit) is optional and enriches the tooltip with security-advisory counts.

<!-- TODO(josh): Gentoo ebuild / GURU overlay instructions once packaged. -->

## Usage

```
eupdates                 # print the count from cache and exit (any polling bar)
eupdates watch           # keep running, print a line whenever it changes (streaming bars)
eupdates check           # resolve @world now and refresh the cache (slow)
eupdates list            # pending packages, one per line
eupdates upgrade         # emerge upgrade in $TERMINAL, then re-check
eupdates --format '{count} {state}'   # choose the output
eupdates --json          # one JSON object per line (waybar return-type json)
```

### Keeping the count current

No timer, cron job or init-system service. Every `eupdates` / `eupdates watch`
call checks with a few `stat` calls whether the cache is out of date, and if so
starts one detached, low-priority `check` in the background while showing the
previous count. The cache is out of date when, since the last check:

- the portage tree was synced,
- an emerge finished (portage's `mtimedb` has been quiet for a minute),
- the world file changed, or
- `EUPDATES_MAX_AGE` hours went by (default 24).

No check starts while `emerge` or `emaint` is running. Only one check runs at a
time, however many bars ask. A failed check (for example an unresolved blocker)
keeps the previous count and is not retried until something changes.

Want to decide when checks run? Pass `--no-auto` or set `EUPDATES_AUTO=0`, and
run `eupdates check` from cron, a timer or by hand.

### How it works

`check` runs `emerge --pretend --update --deep --newuse @world` against the
local synced tree, counts the pending merges, and caches the list under
`$XDG_CACHE_HOME/eupdates`. Display commands only read that cache. Nothing here
contacts the network: only `emerge --sync` does, and eupdates never calls it.

## Status-bar integration

| Bar | How | Guide |
|---|---|---|
| waybar | `exec: eupdates watch --json`, no interval | `contrib/waybar/` |
| polybar | `exec = eupdates watch`, `tail = true` | `contrib/polybar/` |
| i3blocks | `interval=persist` with `watch`, or poll | `contrib/i3blocks/` |
| tmux | `#(eupdates)` | `contrib/tmux/` |
| conky, dwm, eww, yambar, cron, … | poll `eupdates` or stream `eupdates watch` | `contrib/generic/` |

## Options

| Command / flag | Effect |
|---|---|
| `status` | Print one line from the cache and exit (default). |
| `watch` | Keep running; print a line whenever the output changes. Exits when the bar closes the pipe. |
| `check` | Resolve `@world` now and refresh the cache. Non-zero exit if emerge failed. |
| `list` | Print the cached pending packages, one per line. |
| `upgrade` | Interactive `emerge` upgrade in `$TERMINAL`, then `check`. |
| `--format TPL` | Output template. Fields `{count}`, `{glsa}`, `{synced}` (days since sync), `{state}`. Default `{count}`. |
| `--json` | One JSON object per line: `text`, `tooltip`, `class`, `alt`. |
| `--no-auto` | Never start a check on its own. |
| `--notify` | Desktop notification (`notify-send`) when the count changes. A background check reports only a count that rose; `check` reports every outcome. |
| `--no-notify` | Suppress notifications (overrides `EUPDATES_NOTIFY`). |
| `-h`, `--help` / `-V`, `--version` | Usage / version. |

| Environment | Effect |
|---|---|
| `EUPDATES_AUTO` | `0` = `--no-auto`. |
| `EUPDATES_MAX_AGE` | Check at least every this many hours (default `24`). |
| `EUPDATES_NOTIFY` | Notify on change when set to non-empty and not `0`. |
| `EUPDATES_ICON` | Notification icon: XDG icon name (default `system-software-update`) or absolute image path. |
| `TERMINAL` | Terminal for `upgrade` (default `xterm`). |
| `XDG_CACHE_HOME` | Cache dir (default `~/.cache/eupdates`). |

`{state}`, `class` and `alt` are `pending`, `updated`, `checking` (a manual
check is running) or `unknown` (no check finished yet). `notify-send` is
optional; without it notifications are skipped.

## Roadmap

See `CHANGELOG.md`.

## Contributing

See `CONTRIBUTING.md`.

## License

GPL-3.0-or-later. Copyright © Josh Reed.
