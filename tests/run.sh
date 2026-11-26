#!/bin/sh
# eupdates test suite. Builds a test binary whose system paths and emerge
# command point into a scratch directory, so it needs no root, no portage and
# no real two-minute resolve. Run with `make test`.
set -eu

CC=${CC:-cc}
T=$(mktemp -d)
trap 'kill $WATCH 2>/dev/null || true; rm -rf "$T"' EXIT INT TERM
WATCH=

# Fake portage in a directory with a space in its name (no shell may touch
# these paths): a gentoo tree synced by rsync, an overlay synced by git, a
# user config dir, a world file, an mtimedb, and an "emerge" that records how
# it was run, sleeps briefly, prints canned merge lines and exits with a
# chosen code.
S="$T/sys dir"
mkdir -p "$T/cache" "$T/proc" "$S/repos/gentoo/metadata" "$S/repos/ov/.git" \
    "$S/portage/repos.conf" "$S/portage/package.use" "$S/portage/gnupg"
touch "$S/repos/gentoo/metadata/timestamp.chk" "$S/repos/ov/.git/FETCH_HEAD" "$S/world" \
    "$S/mtimedb" "$S/portage/package.use/local" "$S/portage/gnupg/pubring.kbx"
printf '[DEFAULT]\nmain-repo = gentoo\n\n[gentoo]\nlocation = %s/repos/gentoo\n' "$S" >"$S/repos.default"
printf '[ov]\n  location=%s/repos/ov  \n' "$S" >"$S/portage/repos.conf/ov.conf"
cat >"$S/emerge" <<'EOF'
#!/bin/sh
d=$(dirname "$0")
echo run >>"$d/runs"
printf '%s|%s|%s|%s\n' "$*" "${NOCOLOR-}" "${LC_ALL-}" "${EMERGE_DEFAULT_OPTS-}" >"$d/how"
read -r leaked && echo "stdin:$leaked" >>"$d/how"
sleep "${FAKE_SLEEP:-1}"
cat "$d/output"
exit "$(cat "$d/exit")"
EOF
printf '#!/bin/sh\necho 202601-01\n' >"$S/glsa-check"
chmod +x "$S/emerge" "$S/glsa-check"
cat >"$S/output" <<'EOF'
These are the packages that would be merged, in order:
[ebuild     U  ] sys-libs/zlib-1.4::gentoo [1.3.2-r1::gentoo] USE="-minizip" 0 KiB
[binary   R    ] dev-libs/expat-2.11.0::gentoo  USE="unicode" 0 KiB
[ebuild  N     ] app-misc/new-1.0::gentoo  0 KiB
EOF
echo 0 >"$S/exit"

$CC -O2 -std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L \
    -DEUPDATES_EMERGE="\"$S/emerge\"" -DEUPDATES_GLSA="\"$S/glsa-check\"" \
    -DEUPDATES_PORTAGE_DIR="\"$S/portage\"" -DEUPDATES_REPOS_DEFAULT="\"$S/repos.default\"" \
    -DEUPDATES_WORLD="\"$S/world\"" \
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
runs() { [ -f "$S/runs" ] && wc -l <"$S/runs" | tr -d ' ' || echo 0; }
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

# 3. A sync -> one check. rsync stamps timestamp.chk with the mirror's (older)
# time, so the sync must be seen by its change time, not its mtime.
sleep 1.1
touch -d "@$(($(date +%s) - 3 * 86400 - 60))" "$S/repos/gentoo/metadata/timestamp.chk"
"$E" >/dev/null
wait_idle
expect "gentoo sync (old mirror mtime) triggers a check" 2 "$(runs)"
expect "{synced} is the snapshot age" 3 "$("$E" --no-auto --format '{synced}')"

# 3b. An overlay synced by git (no timestamp.chk, only FETCH_HEAD) -> one check.
sleep 1.1
touch "$S/repos/ov/.git/FETCH_HEAD"
"$E" >/dev/null
wait_idle
expect "overlay sync triggers a check" 3 "$(runs)"

# 4. World file edited -> one check.
sleep 1.1
touch "$S/world"
"$E" >/dev/null
wait_idle
expect "world change triggers a check" 4 "$(runs)"

# 4b. Portage config edited (package.use) -> one check; gnupg/ churn -> none.
sleep 1.1
echo "app-misc/new foo" >>"$S/portage/package.use/local"
"$E" >/dev/null
wait_idle
expect "config change triggers a check" 5 "$(runs)"
sleep 1.1
touch "$S/portage/gnupg/pubring.kbx"
"$E" >/dev/null
sleep 0.3
expect "gnupg change ignored" 5 "$(runs)"

# 5. An emerge just wrote mtimedb -> wait for it to go quiet, then check.
sleep 1.1
touch "$S/mtimedb"
"$E" >/dev/null
sleep 0.3
expect "fresh merge: no check yet" 5 "$(runs)"
age "$T/cache/eupdates/attempt" 300
age "$S/mtimedb" 120
"$E" >/dev/null
wait_idle
expect "quiet merge triggers a check" 6 "$(runs)"

# 6. Max age: an attempt older than EUPDATES_MAX_AGE hours -> check.
age "$T/cache/eupdates/attempt" 7200
EUPDATES_MAX_AGE=1 "$E" >/dev/null
wait_idle
expect "max age triggers a check" 7 "$(runs)"

# 7. --no-auto / EUPDATES_AUTO=0 never start a check.
age "$T/cache/eupdates/attempt" 999999
"$E" --no-auto >/dev/null
EUPDATES_AUTO=0 "$E" >/dev/null
sleep 0.3
expect "--no-auto starts nothing" 7 "$(runs)"

# 7b. Portage busy (an emerge or sync running) -> hold off, check once it ends.
mkdir -p "$T/proc/4242"
echo emerge >"$T/proc/4242/comm"
"$E" >/dev/null
sleep 0.3
expect "no check while emerge runs" 7 "$(runs)"
rm -r "$T/proc/4242"
"$E" >/dev/null
wait_idle
expect "check once emerge is gone" 8 "$(runs)"

# 8. Failing emerge: cache kept, and no retry storm afterwards.
echo 1 >"$S/exit"
"$E" check 2>/dev/null && bad "failed check exits 0" || ok "failed check exits non-zero"
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
grep -q "already running" "$T/err" && ok "concurrent check says so" || bad "concurrent check message"

# 9b. How emerge was run: argv without a shell, output pinned, the user's
# EMERGE_DEFAULT_OPTS kept (it decides what a real upgrade merges), stdin
# closed so an --ask there can never wait for input.
echo "stray input" | EMERGE_DEFAULT_OPTS=--with-bdeps=y "$E" check >/dev/null 2>&1
expect "emerge argv, env and stdin" \
    "--pretend --update --deep --newuse --color=n --nospinner @world|true|C|--with-bdeps=y" \
    "$(cat "$S/how")"

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
timeout 10 sh -c "\"$E\" watch --no-auto | head -n1 >/dev/null" && ok "watch exits on closed pipe" || bad "watch lingers after its reader closes"

# 13. Bad arguments.
"$E" --format 2>/dev/null && bad "--format without arg accepted" || ok "--format needs an argument"
"$E" bogus 2>/dev/null && bad "unknown command accepted" || ok "unknown command rejected"

echo
[ "$fails" -eq 0 ] && echo "all tests passed" || { echo "$fails failed"; exit 1; }
