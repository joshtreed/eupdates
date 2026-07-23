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
	then re-run *check*.

# OPTIONS

*--progress*
	For *check*/*upgrade*: write a transient marker and signal waybar before
	resolving, so *status* reports a *checking* class while the check runs.
	Intended for manual (clicked) checks; background refreshes omit it.

*--notify*
	For *check*/*upgrade*: send a desktop notification (via *notify-send*) when
	the pending count changes. A manual check (*--progress*) also notifies when
	nothing changed. A no-op if *notify-send* is not installed.

*--no-notify*
	Suppress notifications, overriding *EUPDATES_NOTIFY*.

*-h, --help*
	Print usage and exit.

*-V, --version*
	Print version and exit.

# ENVIRONMENT

*EUPDATES_NOTIFY*
	When set to a non-empty value other than *0*, *check* notifies on a changed
	count as if *--notify* were given.

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
