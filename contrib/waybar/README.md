# waybar

Add a `custom/updates` module to `~/.config/waybar/config`:

```jsonc
"custom/updates": {
    "format": "{}",
    "return-type": "json",
    "exec": "eupdates status",
    "interval": 3600,
    "signal": 8,
    "on-click": "eupdates check --progress --notify",
    "on-click-right": "eupdates upgrade",
    "tooltip": true
}
```

`--progress` makes a manual (clicked) check show a "checking" state in the bar
while it resolves; `--notify` pops a desktop notification when the count
changes. Background refreshes (timer/`.path`) omit `--progress` so they stay
silent, and take `--notify` only if you want them to (or set `EUPDATES_NOTIFY=1`
in the unit).

Then reference `"custom/updates"` in one of your `modules-left/center/right`
arrays.

Style it in `style.css`, e.g.:

```css
#custom-updates {
    padding: 0 8px;
}
#custom-updates.updated  { opacity: 0.6; }  /* nothing pending           */
#custom-updates.pending  { }                /* updates waiting           */
#custom-updates.checking { opacity: 0.6; }  /* a manual check is running */
#custom-updates.unknown  { opacity: 0.4; }  /* no check yet              */
```

The module text carries a nerd-font package glyph, so use a nerd-font.

`eupdates status` is cheap (reads the cache); the actual resolution happens on
`check`. Drive `check` from the systemd units in `../systemd/` so the count
refreshes on a timer and after every emerge, signalling waybar via
`SIGRTMIN+8`. If you change `"signal"`, set `EUPDATES_WAYBAR_SIGNAL` to match.
