eupdates(1)

# NAME

eupdates - count pending Gentoo package updates for status bars

# SYNOPSIS

*eupdates* [_command_]

# DESCRIPTION

*eupdates* reports how many Gentoo (portage) packages have pending updates,
for display in a status bar such as waybar, polybar, i3blocks or tmux.

It resolves the update set with *emerge*(1) *--pretend* against the
*already-synced* local portage tree and installed database only. It never
contacts a mirror: the only mirror-hitting operation on Gentoo is
*emerge --sync*, which *eupdates* deliberately does not perform. Keeping the
tree fresh is left to a separate, rate-limited sync of the operator's choosing;
the tooltip reports how stale the tree is as a reminder.

Results are cached under _$XDG_CACHE_HOME/eupdates_ so *status* is instant and
the expensive dependency resolution only runs on *check*.

# COMMANDS

*status*
	Emit waybar JSON (_text_, _tooltip_, _class_) from the cache. This is the
	default when no command is given and does no resolution, so it is safe to
	call on every status-bar poll. _class_ is *pending*, *updated*, *checking*
	(a manual *--progress* check is in flight), or *unknown* (no check yet).

*check*
	Resolve the *@world* upgrade set, refresh the cache, and signal waybar to
	redraw. This runs *emerge --pretend --update --deep --newuse @world* and
	can take several seconds; run it from a timer or on demand, not on every
	poll.

*upgrade*
	Launch *$TERMINAL* running an interactive *emerge* upgrade of *@world*,
	then re-run *check*. When that emerge exits 0, it also runs *world-done*.

*world-done* [_EPOCH_]
	Record that a *@world* upgrade finished now, or at _EPOCH_ (seconds since
	1970). *status* shows the age of that record in the tooltip ("last @world
	upgrade N days ago"). portage's own record, _/var/log/emerge.log_, is
	readable only by root and the _portage_ group, so eupdates keeps its own
	in the cache file _last-world_. Call this from any upgrade script that
	does not go through *upgrade*; run it as the user whose cache the status
	bar reads (for example *sudo -u "$SUDO_USER" -H eupdates world-done*).

# OPTIONS

*--progress*
	For *check*/*upgrade*: write a transient marker and signal waybar before
	resolving, so *status* reports a *checking* class while the check runs.
	Intended for manual (clicked) checks; background refreshes omit it.

*--settle* _SECONDS_
	For *check*: before resolving, wait until portage's _mtimedb_
	(/var/cache/edb/mtimedb) has gone _SECONDS_ without a write; any write
	during the wait restarts it. portage rewrites that file once per merged
	package, so this collapses a whole emerge into a single resolve afterwards
	rather than one per package. Intended for a path-unit-triggered *check*;
	0 (the default) disables it. A single package that builds for longer than
	_SECONDS_ can still let one resolve through mid-emerge.

*--notify*
	For *check*/*upgrade*: send a desktop notification (via *notify-send*) when
	the pending count changes. A background check reports only a count that rose
	(or the first count seen): a count that fell is the result of an emerge the
	user just ran, so it is not news. A manual check (*--progress*) reports every
	outcome, including no change. A no-op if *notify-send* is not installed.

*--no-notify*
	Suppress notifications, overriding *EUPDATES_NOTIFY*.

*-h, --help*
	Print usage and exit.

*-V, --version*
	Print version and exit.

# ENVIRONMENT

*EUPDATES_NOTIFY*
	When set to a non-empty value other than *0*, *check* notifies as if
	*--notify* were given (see it for which changes are reported).

*EUPDATES_ICON*
	Icon for notifications: an XDG icon-theme name (e.g. *system-software-update*,
	the default) or an absolute path to an image file.

*TERMINAL*
	Terminal emulator used by *upgrade* (default: *xterm*).

*EUPDATES_WAYBAR_SIGNAL*
	The *SIGRTMIN+N* signal number waybar listens on for this module
	(default: *8*). Must match the module's _signal_ field.

*XDG_CACHE_HOME*
	Cache directory (default: _~/.cache/eupdates_).

# EXIT STATUS

*status* returns 0. *check* returns non-zero if the *emerge* resolution
failed (for example an unresolved blocker); in that case the previous cache is
left untouched rather than reporting zero updates.

# SEE ALSO

*emerge*(1), *eix*(1), *glsa-check*(1)

# AUTHORS

Josh Reed. Licensed under the GNU GPL version 3 or later.
