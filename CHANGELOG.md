# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.1] - 2026-11-26

### Fixed
- The test suite no longer fails when run from a portage ebuild, which sets
  `PORTAGE_CONFIGROOT` in the environment.

## [0.2.0] - 2026-11-26

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
- Notices syncs of every repository in `repos.conf` (overlays too, rsync or
  git), and edits under `/etc/portage` (`package.use`, keywords, `make.conf`,
  profile). A sync is seen by its change time: rsync gives `timestamp.chk` the
  mirror's (older) mtime, which could hide a sync from 0.1.0.
- Syncs on its own, so the count moves even if nobody runs `emerge --sync`. A
  user cannot sync the system tree, so when a repository's system copy is more
  than two days old, eupdates keeps its own copy under
  `$XDG_CACHE_HOME/eupdates/repos` and syncs that, at most once a day per
  repository (an hour apart after a failure). rsync repositories are verified
  as portage verifies them (`gemato`, signed Manifest, `max-age`, no
  rollback); git repositories check the commit signature when `repos.conf`
  asks. Anything that fails verification is thrown away and the old copy
  kept. Only this user's `emerge --pretend` reads the copies, through a
  private `PORTAGE_CONFIGROOT`; `upgrade` runs `emerge --sync` first when the
  count came from one. `--no-sync` / `EUPDATES_SYNC=0` turn it off.
- `--no-auto` / `EUPDATES_AUTO=0` for people who schedule `check` themselves.
- A lock, so only one check runs at a time however many bars and clicks ask.
- A failed check counts as an attempt, so a broken tree is not retried in a
  loop.
- Test suite (`make test`) against a fake emerge; runs in CI on glibc and musl.
- Integration guides for conky, dwm, eww, yambar and cron (`contrib/generic/`).

### Changed
- **Breaking:** the default output is now plain text (`{count}`), not waybar
  JSON. waybar users add `--json`; see `contrib/waybar/`.
- emerge and glsa-check run directly (fork/exec, no `/bin/sh`), with stdin
  on `/dev/null` so an `--ask` in `EMERGE_DEFAULT_OPTS` can never hang a
  check, and output pinned with `NOCOLOR=true` and `LC_ALL=C`. The user's
  `EMERGE_DEFAULT_OPTS` is kept on purpose: options like `--with-bdeps=y`
  change what a real upgrade merges.
- Build-time overrides renamed: `EUPDATES_EMERGE` / `EUPDATES_GLSA` (program
  paths) replace `EUPDATES_EMERGE_CMD` / `EUPDATES_GLSA_CMD` (shell strings);
  `EUPDATES_PORTAGE_DIR` and `EUPDATES_REPOS_DEFAULT` replace
  `EUPDATES_TREE_TS`. The old names are a compile error.
- A background check notifies only when the pending count *rises*, or on the
  first count seen. A count that fell is the result of an emerge you just ran.
  `check` still reports every outcome.

### Removed
- **Breaking:** `--progress` (a manual `check` always shows the `checking`
  state now), `EUPDATES_WAYBAR_SIGNAL` and waybar signalling (`watch` replaces
  it), and the systemd units and `eupdates-count.sh` helper in `contrib/`.

### Planned (hardening toward 1.0.0)
- Parse `emerge --columns` output (portage's most stable per-line format).
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

[Unreleased]: https://github.com/joshtreed/eupdates/compare/v0.2.1...HEAD
[0.2.1]: https://github.com/joshtreed/eupdates/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/joshtreed/eupdates/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/joshtreed/eupdates/releases/tag/v0.1.0
