# systemd user units

Keep the update count fresh without running the expensive resolve on every bar
poll. Install `eupdates` to `~/.local/bin` (`make install PREFIX=$HOME/.local`),
copy these units to `~/.config/systemd/user/`, then:

```sh
systemctl --user daemon-reload
systemctl --user enable --now eupdates.timer eupdates.path
```

- `eupdates.timer` — runs `eupdates check` once daily (jittered), fully offline.
- `eupdates.path` — re-runs it after any emerge, by watching
  `/var/cache/edb/mtimedb`. Portage rewrites that file on every merge/unmerge,
  and it's world-readable (`root:portage`, mode `644`) — so this needs **no
  special group and no root configuration**. `emerge --pretend` (what a check
  runs) doesn't touch it, so a check can't re-trigger itself.

  On its own this misbehaves during a large `@world` upgrade: portage rewrites
  `mtimedb` once per merged package, a resolve takes a couple of minutes, and a
  `.path` unit re-arms as soon as the service it triggered exits — so the check
  runs back-to-back for the entire upgrade, competing with the compile. That is
  what `--settle 300` in `eupdates.service` is for: it waits for portage to go
  quiet first, so one emerge costs one resolve. Watching `/var/db/pkg` does
  *not* work — portage nests installs (`/var/db/pkg/<category>/<pkg>/`) one
  level below what a directory watch sees.

Neither ever syncs the portage tree. Keeping the tree fresh (`emerge --sync`)
is a separate, privileged, rate-limited job you control — sync no more than
once a day to stay friendly to the mirrors. A minimal system-level example:

```ini
# /etc/systemd/system/portage-sync.timer  (root)
[Timer]
OnCalendar=daily
RandomizedDelaySec=45m
Persistent=true
[Install]
WantedBy=timers.target
```

paired with a `portage-sync.service` running `emaint sync --auto`.
