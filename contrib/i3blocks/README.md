# i3blocks

Using the `contrib/eupdates-count.sh` helper (on `PATH`):

```ini
[updates]
command=eupdates-count.sh
interval=3600
label= 
```

`eupdates-count.sh` prints a bare integer. Refresh the underlying cache with
`eupdates check` from a timer (see `../systemd/`). To upgrade on click, wrap in
your own script that runs `eupdates upgrade` for the desired mouse button
(`$BLOCK_BUTTON`).
