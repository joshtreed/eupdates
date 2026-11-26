# polybar

```ini
[module/updates]
type = custom/script
exec = eupdates watch --format '{count}'
tail = true
format-prefix = "pkg "
click-left = eupdates check --notify &
click-right = eupdates upgrade &
```

`tail = true` makes polybar read each line `eupdates watch` prints, so the
count updates on its own after a sync or an emerge. Nothing else (timer, cron,
service) is needed.

To show the state too, try `--format '{count} {state}'`, or `{glsa}` for the
number of unresolved security advisories.
