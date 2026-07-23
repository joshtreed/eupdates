# polybar

polybar wants plain text, so use the `contrib/eupdates-count.sh` helper (until
eupdates gains native plain output). Copy it somewhere on `PATH`, then:

```ini
[module/updates]
type = custom/script
exec = eupdates-count.sh
interval = 3600
label = %output% 
click-left = eupdates check
click-right = eupdates upgrade
format-prefix = " "
```

Refresh the count on a timer with `eupdates check` (see `../systemd/`); the
`interval` above just re-reads the cache.
