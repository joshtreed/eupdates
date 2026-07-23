#!/bin/sh
# Print just the pending-update count as a bare integer.
#
# eupdates 0.1.0 emits waybar JSON; this helper extracts the number for bars
# that want plain text (polybar/i3blocks/tmux/conky). Native plain + --format
# output is on the roadmap, at which point this wrapper becomes unnecessary.
#
# It only reads the cache (cheap). Refresh the cache separately with
# `eupdates check` from a timer — see contrib/systemd/.
n=$(eupdates status | grep -o '"text":"[^"]*"' | grep -o '[0-9]\{1,\}' | head -n1)
echo "${n:-0}"
