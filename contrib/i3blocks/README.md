# i3blocks

Streaming (updates as soon as the count changes):

```ini
[updates]
command=eupdates watch --format 'pkg {count}'
interval=persist
```

In `persist` mode i3blocks sends clicks to the command's standard input rather
than running it again, so this block does not react to clicks. If you want
clicks, poll instead:

```ini
[updates]
command=case $BLOCK_BUTTON in 1) eupdates check --notify >/dev/null & ;; 3) eupdates upgrade >/dev/null & ;; esac; eupdates --format 'pkg {count}'
interval=60
```

Polling every 60 seconds is cheap: `eupdates` reads a small cache file and only
starts a background check when the tree was synced, an emerge finished, or the
world file changed.
