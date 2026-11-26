# tmux

```tmux
# ~/.tmux.conf
set -g status-interval 60
set -g status-right "#(eupdates --format 'pkg {count}') | %H:%M "
```

Each poll reads a small cache file. When the tree was synced, an emerge
finished or the world file changed, `eupdates` starts one check in the
background and the new count shows on a later poll. Nothing else (timer, cron,
service) is needed.
