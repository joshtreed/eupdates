# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Works with any status bar, not just waybar.
  - `watch`: keeps running and prints a line whenever the output changes, for
    bars that stream a command (waybar without `interval`, polybar
    `tail = true`, i3blocks `interval=persist`, eww `deflisten`, yambar).
    Exits when the bar closes the pipe.
  - `--format TEMPLATE` with `{count}`, `{glsa}`, `{synced}`, `{state}`.
  - `--json`: one JSON object per line (waybar `return-type: json`).
  - `list`: the pending packages, one per line.
- Keeps itself current with no timer, cron job or service: `status` and
  `watch` start one detached, low-priority background check when the tree was
  synced, an emerge finished (`mtimedb` quiet for a minute), the world file
  changed, or `EUPDATES_MAX_AGE` hours (default 24) went by. Never while
  `emerge` or `emaint` runs.
- `--no-auto` / `EUPDATES_AUTO=0` for people who schedule `check` themselves.
- A lock, so only one check runs at a time however many bars and clicks ask.
- A failed check counts as an attempt, so a broken tree is not retried in a
  loop.
- Test suite (`make test`) against a fake emerge; runs in CI on glibc and musl.
- Integration guides for conky, dwm, eww, yambar and cron (`contrib/generic/`).

### Changed
- **Breaking:** the default output is now plain text (`{count}`), not waybar
  JSON. waybar users add `--json`; see `contrib/waybar/`.
- A background check notifies only when the pending count *rises*, or on the
  first count seen. A count that fell is the result of an emerge you just ran.
  `check` still reports every outcome.

### Removed
- **Breaking:** `--progress` (a manual `check` always shows the `checking`
  state now), `EUPDATES_WAYBAR_SIGNAL` and waybar signalling (`watch` replaces
  it), and the systemd units and `eupdates-count.sh` helper in `contrib/`.

### Planned (hardening toward 1.0.0)
- Replace `popen`/`/bin/sh` with `execve` and an explicitly scrubbed
  environment (`LC_ALL=C`, `COLUMNS=1000`, `EMERGE_DEFAULT_OPTS=`, `NOCOLOR`)
  so a user's emerge defaults or locale can't reshape the parsed output.
- Parse `emerge --columns` output (portage's most stable per-line format).
- Notice syncs of every configured repo, not just the main tree, and changes
  under `/etc/portage`.
- Optional `eix` fast backend (auto-detected, labelled approximate).

## [0.1.0] - 2026-07-23

### Added
- Initial public release: `status` / `check` / `upgrade` commands.
- waybar JSON output with pending-count, package-list tooltip, GLSA count, and
  a portage-tree staleness reminder.
- `--notify` / `EUPDATES_NOTIFY` (and `--no-notify`): best-effort `notify-send`
  on a changed count; a manual check also confirms when nothing changed.
- `--progress`: a manual check shows a `checking` class in the bar while it
  resolves, so a clicked refresh gives immediate feedback.
- Offline by design: resolves the local synced tree only; never syncs mirrors.
- Result caching under `$XDG_CACHE_HOME/eupdates`.
- `--help` / `--version`; `TERMINAL` and `EUPDATES_WAYBAR_SIGNAL` env knobs.
- Man page (`scdoc`), Makefile with `DESTDIR`/`PREFIX` support, example
  status-bar and systemd integration snippets.
