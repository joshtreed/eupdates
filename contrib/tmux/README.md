# tmux

Show the pending-update count in the status line via the
`contrib/eupdates-count.sh` helper (on `PATH`):

```tmux
# ~/.tmux.conf
set -g status-interval 3600
set -g status-right "#(eupdates-count.sh) updates | %H:%M "
```

tmux refreshes on `status-interval`; that only re-reads the cache. Refresh the
cache itself with `eupdates check` from a timer (see `../systemd/`).
