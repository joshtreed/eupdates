# eupdates

<!-- TODO(josh): one-line tagline in your voice. Working draft: -->
> Counts pending Gentoo (portage) package updates for status bars. Zero dependencies, one C file.

<!-- TODO(josh): badges once the repo is pushed — CI status, latest release, license (GPL-3.0). -->

<!-- TODO(josh): screenshot / asciinema of the waybar module. Placeholder: -->
<!-- ![demo](doc/demo.png) -->

## Why

<!-- TODO(josh): your prose. Bullet points to draw from: -->
- Zero runtime dependencies beyond portage itself (which every Gentoo box has).
- No persistent database to maintain (unlike `eix`, which needs `eix-update`).
- Uses the **same** resolver as a real update (`emerge -puDN @world`), so the count matches what you'd actually merge.
- **Never touches a mirror** — it reads the already-synced local tree only. Syncing stays your call.
- Single purpose, fast to poll (results are cached; the slow resolve runs only on `check`).

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
eupdates            # == eupdates status: waybar JSON from cache (instant)
eupdates check      # resolve @world, refresh cache, signal waybar (slow)
eupdates upgrade    # emerge upgrade in $TERMINAL, then re-check
eupdates --help
eupdates --version
```

`status` is safe to call on every bar poll — it only reads the cache. Run
`check` from a timer or on demand; it runs the full `emerge --pretend`
resolution and can take several seconds.

### How it works

`check` runs `emerge --pretend --update --deep --newuse @world` against the
local synced tree, counts the pending merges, and caches the list. `status`
formats that cache as waybar JSON. Nothing here contacts the network — only
`emerge --sync` does, and eupdates never calls it. The tooltip shows how many
days since the tree was synced so you know when a sync is due.

## Status-bar integration

### waybar

`~/.config/waybar/config` module (see `contrib/waybar/`):

```jsonc
"custom/updates": {
    "format": "{}",
    "return-type": "json",
    "exec": "eupdates status",
    "interval": 3600,
    "signal": 8,
    "on-click": "eupdates check",
    "on-click-right": "eupdates upgrade",
    "tooltip": true
}
```

Refresh the count after each poll interval, or drive `eupdates check` from a
systemd user timer / `.path` unit (examples in `contrib/systemd/`) and let it
signal waybar via `SIGRTMIN+8`. If you use a different signal number, set
`EUPDATES_WAYBAR_SIGNAL` to match.

### polybar / i3blocks / tmux

See `contrib/polybar/`, `contrib/i3blocks/`, `contrib/tmux/`.

## Options

| Command / flag  | Effect                                              |
|-----------------|-----------------------------------------------------|
| `status`        | Emit waybar JSON from cache (default).              |
| `check`         | Resolve `@world`, refresh cache, signal waybar.     |
| `upgrade`       | Interactive `emerge` upgrade in `$TERMINAL`.         |
| `--progress`    | On `check`/`upgrade`: show a "checking" state in the bar while resolving. |
| `--notify`      | On `check`/`upgrade`: desktop-notify (via `notify-send`) when the count changes; a manual check also confirms when nothing changed. |
| `--no-notify`   | Suppress notifications (overrides `EUPDATES_NOTIFY`). |
| `-h`, `--help`  | Usage.                                              |
| `-V`, `--version` | Version.                                          |

| Environment              | Effect                                           |
|--------------------------|--------------------------------------------------|
| `EUPDATES_NOTIFY`        | Notify on change when set to non-empty and not `0`. |
| `EUPDATES_ICON`          | Notification icon: an XDG icon-theme name (default `system-software-update`) or an absolute image path. |
| `TERMINAL`               | Terminal for `upgrade` (default `xterm`).         |
| `EUPDATES_WAYBAR_SIGNAL` | `SIGRTMIN+N` waybar refresh signal (default `8`). |
| `XDG_CACHE_HOME`         | Cache dir (default `~/.cache/eupdates`).          |

`status` emits `class` values `pending` / `updated` / `checking` / `unknown`
for CSS styling. Notifications need `notify-send` (libnotify) present; it's an
optional runtime helper — absent, notifications are simply skipped.

## Roadmap

See `CHANGELOG.md`. Near-term hardening (tracked): switch from `popen` to
`execve` with a scrubbed environment, use `emerge --columns` for a more stable
parse, add generic output modes (bare integer, `--format`, `--list`) so it's
not waybar-only, and broaden cache invalidation.

## Contributing

See `CONTRIBUTING.md`.

## License

GPL-3.0-or-later. Copyright © Josh Reed.
