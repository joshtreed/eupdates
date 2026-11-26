eupdates(1)

# NAME

eupdates - count pending Gentoo package updates, for any status bar

# SYNOPSIS

*eupdates* [_command_] [_options_]

# DESCRIPTION

*eupdates* reports how many Gentoo (portage) packages have pending updates, for
display in any status bar: waybar, polybar, i3blocks, eww, yambar, tmux, conky,
a dwm status script, or anything else that shows a command's output.

It resolves the update set with *emerge*(1) *--pretend* against the
*already-synced* local portage tree and installed database only, so the count
matches what a real *emerge -uDN @world* would merge. It never contacts a
mirror: the only mirror-hitting operation on Gentoo is *emerge --sync*, which
*eupdates* deliberately does not perform.

*emerge* is run directly, without a shell, and with the user's own portage
settings: *EMERGE_DEFAULT_OPTS* such as *--with-bdeps=y* change what a real
upgrade merges, so they count. Only the output is pinned (*NOCOLOR*, *LC_ALL=C*,
*--color=n*), and standard input is _/dev/null_, so an *--ask* default can never
wait for an answer.

That resolve takes as long as a real *emerge --pretend* (often a minute or two of
CPU), so its result is cached under _$XDG_CACHE_HOME/eupdates_ and every display
command only reads the cache.

# KEEPING THE COUNT CURRENT

No timer, cron job or init-system service is needed. Whenever *status* or
*watch* runs, it checks (with a few *stat*(2) calls) whether the cache is out of
date, and if so starts one detached, low-priority *check* in the background
while showing the previous count. The cache is out of date when, since the last
check:

- any repository was synced: the gentoo tree or an overlay, by rsync or git
  (every _location_ in _repos.conf_);
- the portage configuration under _/etc/portage_ changed (_package.use_,
  _package.accept_keywords_, _make.conf_, the profile link, ...);
- an emerge finished (portage's _/var/cache/edb/mtimedb_ has been quiet for a
  minute);
- the world file changed; or
- *EUPDATES_MAX_AGE* hours went by (default 24).

No check starts while an *emerge* or *emaint* process is running, because its
answer would be out of date before it landed. Only one check runs at a time,
however many bars and clicks ask for one. A failed check counts as an attempt,
so a broken tree is retried when something changes rather than continuously.

To decide for yourself when checks run, pass *--no-auto* (or set
*EUPDATES_AUTO=0*) and run *eupdates check* from cron, a timer or by hand, as
the user whose bar shows the count (the cache is per user).

# COMMANDS

*status*
	Print one line from the cache and exit. The default when no command is
	given. Use it with bars that run a command on an interval.

*watch*
	Keep running and print a new line whenever the output changes: at start,
	when a check finishes, while a manual check runs (state *checking*). Use
	it with bars that read a long-running command's lines (waybar without
	_interval_, polybar _tail = true_, i3blocks _interval=persist_, eww
	_deflisten_, yambar _script_). Exits when the bar closes the pipe.

*check*
	Resolve the *@world* upgrade set now and refresh the cache. While it
	runs, *watch* reports state *checking*. If another check is already
	running, says so and exits 0. Exits non-zero if *emerge* failed (for
	example an unresolved blocker); the previous cache is then kept rather
	than reporting zero updates.

*list*
	Print the cached list of pending packages, one per line, with the
	installed version where there is one. For popups and menus.

*upgrade*
	Launch *$TERMINAL* running an interactive *emerge* upgrade of *@world*,
	then run *check*.

# OPTIONS

*--format* _TEMPLATE_
	Output template for *status* and *watch*. Fields:

	*{count}*   pending updates (*?* before the first check)++
*{glsa}*    unresolved security advisories (needs *glsa-check*)++
*{synced}*  age of the gentoo tree's snapshot, in days++
*{state}*   *pending*, *updated*, *checking* or *unknown*

	Anything else is printed as is. Default: *{count}*.

*--json*
	Print one JSON object per line instead: _text_ (the template), _tooltip_
	(summary and package list), _class_ and _alt_ (the state). This is the
	shape waybar's _return-type: json_ expects.

*--no-auto*
	Never start a check on its own; see KEEPING THE COUNT CURRENT.

*--notify*
	Send a desktop notification (via *notify-send*) when the pending count
	changes. A background check reports only a count that rose (or the first
	count seen): a count that fell is the result of an emerge you just ran,
	so it is not news. *check* and *upgrade* report every outcome, including
	no change. A no-op if *notify-send* is not installed.

*--no-notify*
	Suppress notifications, overriding *EUPDATES_NOTIFY*.

*-h, --help*
	Print usage and exit.

*-V, --version*
	Print version and exit.

# ENVIRONMENT

*EUPDATES_AUTO*
	*0* has the same effect as *--no-auto*.

*EUPDATES_MAX_AGE*
	Start a check at least every this many hours (default: *24*).

*EUPDATES_NOTIFY*
	When set to a non-empty value other than *0*, behave as if *--notify*
	were given.

*EUPDATES_ICON*
	Icon for notifications: an XDG icon-theme name (default:
	*system-software-update*) or an absolute path to an image file.

*TERMINAL*
	Terminal emulator used by *upgrade* (default: *xterm*).

*XDG_CACHE_HOME*
	Cache directory (default: _~/.cache/eupdates_).

# EXIT STATUS

*status*, *watch* and *list* return 0 (*list* returns 1 before the first check).
*check* and *upgrade* return non-zero if the resolve failed.

# EXAMPLES

Polling bar (tmux):

	set -g status-right "#(eupdates --format 'pkg {count}')"

Streaming bar (polybar):

	exec = eupdates watch --format '{count}'++
tail = true

Waybar with tooltip and CSS classes:

	"exec": "eupdates watch --json --format '{count}'",++
"return-type": "json"

# SEE ALSO

*emerge*(1), *eix*(1), *glsa-check*(1)

# AUTHORS

Josh Reed. Licensed under the GNU GPL version 3 or later.
