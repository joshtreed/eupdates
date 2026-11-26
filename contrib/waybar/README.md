# waybar

Add a `custom/updates` module to `~/.config/waybar/config`:

```jsonc
"custom/updates": {
    "exec": "eupdates watch --json --format '󰏗 {count}'",
    "return-type": "json",
    "on-click": "eupdates check --notify",
    "on-click-right": "eupdates upgrade",
    "tooltip": true
}
```

No `interval` and no `signal`: `eupdates watch` keeps running and prints a new
line whenever the count changes, including after a sync or an emerge. Nothing
else (timer, cron, service) is needed.

Clicking runs a check now; the module shows the `checking` class until it
finishes, and `--notify` reports the result. Right-click opens an upgrade in
`$TERMINAL`.

Then add `"custom/updates"` to one of your `modules-left/center/right` arrays.

Style it in `style.css`:

```css
#custom-updates.updated  { opacity: 0.6; }  /* nothing pending           */
#custom-updates.pending  { }                /* updates waiting           */
#custom-updates.checking { opacity: 0.6; }  /* a manual check is running */
#custom-updates.unknown  { opacity: 0.4; }  /* no check finished yet     */
```

The format above uses a nerd-font glyph; use any text you like.
