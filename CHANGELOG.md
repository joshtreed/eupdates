# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned (hardening toward 1.0.0)
- Replace `popen`/`/bin/sh` with `execve` and an explicitly scrubbed
  environment (`LC_ALL=C`, `COLUMNS=1000`, `EMERGE_DEFAULT_OPTS=`, `NOCOLOR`)
  so a user's emerge defaults or locale can't reshape the parsed output.
- Parse `emerge --columns` output (portage's most stable per-line format).
- Generic output modes: bare integer (default), `--json`, `--format <tmpl>`,
  `--list`, so eupdates works in polybar/i3blocks/tmux/conky without wrappers.
- Broaden cache invalidation to every configured repo `timestamp.chk`,
  `/var/db/pkg`, `/var/lib/portage/world`, and `/etc/portage`.
- Optional `eix` fast backend (auto-detected, labelled approximate).
- musl CI leg.

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
