# Other bars and scripts

`eupdates` works with anything that shows a command's output.

**Bars that run a command on an interval** (conky, dwm status scripts,
i3status-rust `custom` blocks, xmobar, lemonbar loops): call `eupdates` (same as
`eupdates status`) as often as you like. It prints one line and exits.

```sh
# dwm / dwmblocks style
while :; do
    xsetroot -name "pkg $(eupdates) | $(date +%H:%M)"
    sleep 60
done
```

```lua
-- conky
${execi 60 eupdates --format 'pkg {count}'}
```

**Bars that read a long-running command** (eww `deflisten`, yambar `script`,
sfwbar, a custom fifo): run `eupdates watch`. It prints a line at start and
whenever the output changes, and exits when the reader closes the pipe.

```yuck
; eww
(deflisten updates "eupdates watch --format '{count}'")
```

**Menus and popups:** `eupdates list` prints the pending packages, one per line.

**Full control:** if you would rather decide when checks run (cron, a user
timer, a script of your own), set `EUPDATES_AUTO=0` or pass `--no-auto`, and
run `eupdates check` yourself, as the user whose bar shows the count (the cache
lives in that user's `~/.cache`):

```cron
# crontab -e: check every 6 hours
0 */6 * * * eupdates check
```
