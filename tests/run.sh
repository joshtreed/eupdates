#!/bin/sh
# eupdates test suite. Builds a test binary whose system paths and emerge
# command point into a scratch directory, so it needs no root, no portage and
# no real two-minute resolve. Run with `make test`.
set -eu

CC=${CC:-cc}
T=$(mktemp -d)
trap 'kill $WATCH 2>/dev/null || true; rm -rf "$T"' EXIT INT TERM
WATCH=

# Fake portage: a tree timestamp, a world file, an mtimedb, and an "emerge"
# that sleeps briefly, prints canned merge lines and exits with a chosen code.
mkdir -p "$T/cache" "$T/sys" "$T/proc"
touch "$T/sys/timestamp.chk" "$T/sys/world" "$T/sys/mtimedb"
cat >"$T/sys/emerge" <<'EOF'
#!/bin/sh
d=$(dirname "$0")
echo run >>"$d/runs"
sleep "${FAKE_SLEEP:-1}"
cat "$d/output"
exit "$(cat "$d/exit")"
EOF
chmod +x "$T/sys/emerge"
cat >"$T/sys/output" <<'EOF'
These are the packages that would be merged, in order:
[ebuild     U  ] sys-libs/zlib-1.4::gentoo [1.3.2-r1::gentoo] USE="-minizip" 0 KiB
[binary   R    ] dev-libs/expat-2.11.0::gentoo  USE="unicode" 0 KiB
[ebuild  N     ] app-misc/new-1.0::gentoo  0 KiB
EOF
echo 0 >"$T/sys/exit"

S=$T/sys
$CC -O2 -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L \
    -DEUPDATES_EMERGE_CMD="\"$S/emerge\"" -DEUPDATES_GLSA_CMD="\"echo 202601-01\"" \
    -DEUPDATES_TREE_TS="\"$S/timestamp.chk\"" -DEUPDATES_WORLD="\"$S/world\"" \
    -DEUPDATES_MTIMEDB="\"$S/mtimedb\"" -DEUPDATES_PROC="\"$T/proc\"" -o "$T/eupdates" src/eupdates.c

export XDG_CACHE_HOME="$T/cache" EUPDATES_NOTIFY=0
E="$T/eupdates"
fails=0
ok() { printf 'ok   %s\n' "$1"; }
bad() {
    printf 'FAIL %s\n' "$1"
    fails=$((fails + 1))
}
expect() { # name, expected, actual
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$2', got '$3')"; fi
}
runs() { if [ -f "$S/runs" ]; then wc -l <"$S/runs" | tr -d ' '; else echo 0; fi; }
wait_idle() { # until no check holds the lock (fake emerge finished)
    i=0
    while [ "$i" -lt 50 ]; do
        "$E" status --no-auto >/dev/null
        if ! pgrep -f "$S/emerge" >/dev/null; then sleep 0.2 && return; fi
        sleep 0.1
        i=$((i + 1))
    done
}
age() { touch -d "@$(($(date +%s) - $2))" "$1"; } # path, seconds ago

# 1. First run: no cache -> prints "?" now, starts exactly one background check.
expect "first status prints ?" "?" "$("$E")"
"$E" >/dev/null
"$E" >/dev/null
wait_idle
expect "only one check started for 3 calls" 1 "$(runs)"
expect "count after first check" 3 "$("$E")"

# 2. Up to date: nothing changed -> no new check.
"$E" >/dev/null
sleep 0.3
expect "no check when nothing changed" 1 "$(runs)"

# 3. A sync (tree timestamp newer than the last attempt) -> one check.
sleep 1.1
touch "$S/timestamp.chk"
"$E" >/dev/null
wait_idle
expect "sync triggers a check" 2 "$(runs)"

# 4. World file edited -> one check.
sleep 1.1
touch "$S/world"
"$E" >/dev/null
wait_idle
expect "world change triggers a check" 3 "$(runs)"

# 5. An emerge just wrote mtimedb -> wait for it to go quiet, then check.
sleep 1.1
touch "$S/mtimedb"
"$E" >/dev/null
sleep 0.3
expect "fresh merge: no check yet" 3 "$(runs)"
age "$T/cache/eupdates/attempt" 300
age "$S/mtimedb" 120
"$E" >/dev/null
wait_idle
expect "quiet merge triggers a check" 4 "$(runs)"

# 6. Max age: an attempt older than EUPDATES_MAX_AGE hours -> check.
age "$T/cache/eupdates/attempt" 7200
EUPDATES_MAX_AGE=1 "$E" >/dev/null
wait_idle
expect "max age triggers a check" 5 "$(runs)"

# 7. --no-auto / EUPDATES_AUTO=0 never start a check.
age "$T/cache/eupdates/attempt" 999999
"$E" --no-auto >/dev/null
EUPDATES_AUTO=0 "$E" >/dev/null
sleep 0.3
expect "--no-auto starts nothing" 5 "$(runs)"

# 7b. Portage busy (an emerge or sync running) -> hold off, check once it ends.
mkdir -p "$T/proc/4242"
echo emerge >"$T/proc/4242/comm"
"$E" >/dev/null
sleep 0.3
expect "no check while emerge runs" 5 "$(runs)"
rm -r "$T/proc/4242"
"$E" >/dev/null
wait_idle
expect "check once emerge is gone" 6 "$(runs)"

# 8. Failing emerge: cache kept, and no retry storm afterwards.
echo 1 >"$S/exit"
if "$E" check 2>/dev/null; then bad "failed check exits 0"; else ok "failed check exits non-zero"; fi
expect "failed check keeps old count" 3 "$("$E" --no-auto)"
r=$(runs)
"$E" >/dev/null
"$E" >/dev/null
sleep 0.3
expect "no retry after a failure" "$r" "$(runs)"
echo 0 >"$S/exit"

# 9. Lock: a manual check while another runs -> second does not resolve.
FAKE_SLEEP=2 "$E" check &
bg=$!
sleep 0.5
r=$(runs)
"$E" check 2>"$T/err"
wait "$bg"
expect "concurrent check skipped" "$r" "$(runs)"
if grep -q "already running" "$T/err"; then ok "concurrent check says so"; else bad "concurrent check message"; fi

# 10. Output formats.
expect "--format fields" "3 pending, 1 glsa" "$("$E" --no-auto --format '{count} {state}, {glsa} glsa')"
expect "--format unknown field kept" "{nope} 3" "$("$E" --no-auto --format '{nope} {count}')"
j=$("$E" --no-auto --json --format 'U {count}')
case $j in
'{"text":"U 3","tooltip":"3 updates pending'*'"class":"pending","alt":"pending"}') ok "--json shape" ;;
*) bad "--json shape: $j" ;;
esac
case $j in *'sys-libs/zlib-1.4  (was 1.3.2-r1)'*) ok "--json tooltip lists packages" ;; *) bad "--json tooltip" ;; esac
expect "list" "sys-libs/zlib-1.4  (was 1.3.2-r1)" "$("$E" list | head -n1)"

# 11. watch: prints at start, prints again when the cache changes, not otherwise.
"$E" watch --no-auto >"$T/watch.out" &
WATCH=$!
sleep 0.5
printf '[ebuild     U  ] a/b-2::gentoo [1::gentoo]\n' >"$S/output"
"$E" check >/dev/null 2>&1
sleep 2.5
kill "$WATCH"
WATCH=
expect "watch lines" "$(printf '3\n1')" "$(cat "$T/watch.out")"

# 12. watch exits when the bar closes its pipe.
if timeout 10 sh -c "\"$E\" watch --no-auto | head -n1 >/dev/null"; then
    ok "watch exits on closed pipe"
else
    bad "watch lingers after its reader closes"
fi

# 13. Bad arguments.
if "$E" --format 2>/dev/null; then bad "--format without arg accepted"; else ok "--format needs an argument"; fi
if "$E" bogus 2>/dev/null; then bad "unknown command accepted"; else ok "unknown command rejected"; fi

echo
if [ "$fails" -ne 0 ]; then
    echo "$fails failed"
    exit 1
fi
echo "all tests passed"
