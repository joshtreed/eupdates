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
mkdir -p "$S/repos/gentoo/profiles/default/linux"
ln -s "$S/repos/gentoo/profiles/default/linux" "$S/portage/make.profile"
cat >"$S/repos.default" <<EOF
[DEFAULT]
main-repo = gentoo

[gentoo]
location = $S/repos/gentoo
sync-type = rsync
sync-uri = $S/mirror/gentoo
sync-rsync-verify-metamanifest = yes
sync-rsync-verify-max-age = 3
sync-openpgp-key-path = $S/key.asc
EOF
printf '[ov]\n  location=%s/repos/ov  \n' "$S" >"$S/portage/repos.conf/ov.conf"
cat >"$S/emerge" <<'EOF'
#!/bin/sh
d=$(dirname "$0")
echo run >>"$d/runs"
printf '%s|%s|%s|%s\n' "$*" "${NOCOLOR-}" "${LC_ALL-}" "${EMERGE_DEFAULT_OPTS-}" >"$d/how"
read -r leaked && echo "stdin:$leaked" >>"$d/how"
echo "${PORTAGE_CONFIGROOT-}" >"$d/root"
sleep "${FAKE_SLEEP:-1}"
cat "$d/output"
exit "$(cat "$d/exit")"
EOF
printf '#!/bin/sh\necho 202601-01\n' >"$S/glsa-check"
# gemato records how it was run and passes or fails on demand; the terminal
# records the command `upgrade` gave it.
cat >"$S/gemato" <<'EOF'
#!/bin/sh
echo "$*" >>"${0%/*}/gemato.how"
exit "$(cat "${0%/*}/gemato.exit")"
EOF
cat >"$S/term" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"${0%/*}/term.how"
EOF
echo 0 >"$S/gemato.exit"
chmod +x "$S/emerge" "$S/glsa-check" "$S/gemato" "$S/term"
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
    -DEUPDATES_WORLD="\"$S/world\"" -DEUPDATES_GEMATO="\"$S/gemato\"" -DEUPDATES_SYSTEM_FRESH=2 \
    -DEUPDATES_MTIMEDB="\"$S/mtimedb\"" -DEUPDATES_PROC="\"$T/proc\"" -o "$T/eupdates" src/eupdates.c

# Syncing is tested on its own at the end; the rest count the system tree.
export XDG_CACHE_HOME="$T/cache" EUPDATES_NOTIFY=0 EUPDATES_SYNC=0
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
if timeout 10 sh -c "\"$E\" watch --no-auto | head -n1 >/dev/null"; then
    ok "watch exits on closed pipe"
else
    bad "watch lingers after its reader closes"
fi

# 13. Bad arguments.
if "$E" --format 2>/dev/null; then bad "--format without arg accepted"; else ok "--format needs an argument"; fi
if "$E" bogus 2>/dev/null; then bad "unknown command accepted"; else ok "unknown command rejected"; fi

# 14. Self-sync. A mirror for the gentoo tree (rsync, Manifest-verified) and
# an upstream for a git overlay; the system copies go stale after 2 s here.
export EUPDATES_SYNC=1
C="$T/cache/eupdates"
gm() { if [ -f "$S/gemato.how" ]; then wc -l <"$S/gemato.how" | tr -d ' '; else echo 0; fi; }
stamp() { date -u -d "@$(($(date +%s) - $1))" +%Y-%m-%dT%H:%M:%SZ; } # seconds ago
mirror() { # content, Manifest age in seconds
    mkdir -p "$S/mirror/gentoo/metadata" "$S/mirror/gentoo/profiles/default/linux"
    echo "$1" >"$S/mirror/gentoo/pkg"
    printf 'MANIFEST pkg 1\nTIMESTAMP %s\n' "$(stamp "$2")" >"$S/mirror/gentoo/Manifest"
    touch "$S/mirror/gentoo/metadata/timestamp.chk"
}
G() { git -c user.name=t -c user.email=t@t -c init.defaultBranch=main "$@"; }
mkdir -p "$S/upstream/gitov"
G -C "$S/upstream/gitov" init -q
echo one >"$S/upstream/gitov/file"
G -C "$S/upstream/gitov" add file && G -C "$S/upstream/gitov" commit -qm one
G clone -q "$S/upstream/gitov" "$S/repos/gitov"
printf '[gitov]\nlocation = %s/repos/gitov\nsync-type = git\nsync-uri = %s/upstream/gitov\n' \
    "$S" "$S" >"$S/portage/repos.conf/gitov.conf"
echo two >"$S/upstream/gitov/file"
G -C "$S/upstream/gitov" commit -qam two
mirror v1 60
sleep 2.1

# 14a. Stale system tree -> our copies are synced, verified and used.
"$E" check >/dev/null 2>"$T/err" || true
expect "rsync copy synced" v1 "$(cat "$C/repos/gentoo/pkg" 2>/dev/null)"
expect "gemato verified the new snapshot" "verify -q -s -K $S/key.asc --timeout 10 $C/repos/gentoo.new" \
    "$(cat "$S/gemato.how" 2>/dev/null)"
expect "git copy synced" two "$(cat "$C/repos/gitov/file" 2>/dev/null)"
expect "emerge pointed at our config root" "$C/root" "$(cat "$S/root")"
if grep -q "^location = $C/repos/gentoo\$" "$C/root/etc/portage/repos.conf/b-eupdates.conf" &&
    grep -q "^location = $C/repos/gitov\$" "$C/root/etc/portage/repos.conf/b-eupdates.conf"; then
    ok "repos.conf re-points both repositories"
else
    bad "repos.conf: $(cat "$C/root/etc/portage/repos.conf/b-eupdates.conf" 2>&1)"
fi
expect "profile follows the tree" "$C/repos/gentoo/profiles/default/linux" \
    "$(readlink "$C/root/etc/portage/make.profile")"
expect "user config linked through" "$S/portage/package.use" \
    "$(readlink "$C/root/etc/portage/package.use")"
expect "no stray errors" "" "$(cat "$T/err")"

# 14b. Again right away -> no second sync (once a day).
mirror v2 30
echo three >"$S/upstream/gitov/file"
G -C "$S/upstream/gitov" commit -qam three
"$E" check >/dev/null 2>&1 || true
expect "no resync within a day (rsync)" 1 "$(gm)"
expect "no resync within a day (git)" two "$(cat "$C/repos/gitov/file")"

# 14c. A day later -> synced again, unchanged files hard-linked not re-sent.
age "$C/repos/.gentoo.synced" 86400
age "$C/repos/.gitov.synced" 86400
"$E" check >/dev/null 2>&1 || true
expect "daily resync (rsync)" v2 "$(cat "$C/repos/gentoo/pkg")"
expect "daily resync (git)" three "$(cat "$C/repos/gitov/file")"
if [ ! -e "$C/repos/gentoo.new" ] && [ ! -e "$C/repos/gentoo.old" ]; then
    ok "swap leaves no staging dirs"
else bad "staging dirs left behind"; fi

# 14d. Verification fails -> old copy kept, retried after an hour, not before.
echo 1 >"$S/gemato.exit"
mirror evil 10
age "$C/repos/.gentoo.synced" 86400
"$E" check >/dev/null 2>"$T/err" || true
expect "bad signature keeps old copy" v2 "$(cat "$C/repos/gentoo/pkg")"
if grep -q "verification failed" "$T/err"; then ok "says verification failed"; else bad "no verification error"; fi
n=$(gm)
"$E" check >/dev/null 2>&1 || true
expect "no retry within the hour" "$n" "$(gm)"
age "$C/repos/.gentoo.tried" 3601
"$E" check >/dev/null 2>&1 || true
expect "retried after an hour" $((n + 1)) "$(gm)"
echo 0 >"$S/gemato.exit"

# 14e. Signed but stale (past max-age) or older than ours -> rejected.
mirror old $((4 * 86400))
age "$C/repos/.gentoo.synced" 86400
age "$C/repos/.gentoo.tried" 86400
"$E" check >/dev/null 2>&1 || true
expect "snapshot past max-age rejected" v2 "$(cat "$C/repos/gentoo/pkg")"
mirror rollback 86400
age "$C/repos/.gentoo.tried" 86400
"$E" check >/dev/null 2>&1 || true
expect "rollback rejected" v2 "$(cat "$C/repos/gentoo/pkg")"

# 14f. A git commit whose signature fails -> old checkout kept.
printf 'sync-git-verify-commit-signature = yes\nsync-openpgp-key-path = %s/key.asc\n' "$S" \
    >>"$S/portage/repos.conf/gitov.conf"
echo 1 >"$S/gemato.exit"
echo four >"$S/upstream/gitov/file"
G -C "$S/upstream/gitov" commit -qam four
age "$C/repos/.gitov.synced" 86400
"$E" check >/dev/null 2>&1 || true
expect "unsigned git commit not checked out" three "$(cat "$C/repos/gitov/file")"
echo 0 >"$S/gemato.exit"
age "$C/repos/.gitov.tried" 86400
"$E" check >/dev/null 2>&1 || true
expect "signed git commit checked out" four "$(cat "$C/repos/gitov/file")"
case "$(tail -n 1 "$S/gemato.how")" in
"gpg-wrap -K $S/key.asc --timeout 10 -- "*) ok "key refresh gets a short timeout" ;;
*) bad "no refresh timeout: $(tail -n 1 "$S/gemato.how")" ;;
esac
echo 'sync-openpgp-key-refresh = false-nowarn' >>"$S/portage/repos.conf/gitov.conf"
echo five >"$S/upstream/gitov/file"
G -C "$S/upstream/gitov" commit -qam five
age "$C/repos/.gitov.synced" 86400
"$E" check >/dev/null 2>&1 || true
case "$(tail -n 1 "$S/gemato.how")" in
"gpg-wrap -K $S/key.asc -R -- "*) ok "false-nowarn turns key refresh off" ;;
*) bad "false-nowarn ignored: $(tail -n 1 "$S/gemato.how")" ;;
esac

# 14g. A swap cut short (copy moved aside, never moved back) heals.
mirror v3 20
mv "$C/repos/gentoo" "$C/repos/gentoo.old"
age "$C/repos/.gentoo.synced" 86400
age "$C/repos/.gentoo.tried" 86400
"$E" check >/dev/null 2>&1 || true
expect "interrupted swap recovered" v3 "$(cat "$C/repos/gentoo/pkg" 2>/dev/null)"

# 14h. upgrade syncs the system tree first when the count came from our copy.
TERMINAL="$S/term" "$E" upgrade >/dev/null 2>&1 || true
case $(cat "$S/term.how") in *"sudo emerge --sync && sudo emerge --ask"*) ok "upgrade syncs first" ;;
*) bad "upgrade command: $(cat "$S/term.how")" ;; esac

# 14i. --no-sync / EUPDATES_SYNC=0: no syncing, system tree used as is.
age "$C/repos/.gentoo.synced" 86400
age "$C/repos/.gentoo.tried" 86400
n=$(gm)
"$E" check --no-sync >/dev/null 2>&1 || true
EUPDATES_SYNC=0 "$E" check >/dev/null 2>&1 || true
expect "--no-sync syncs nothing" "$n" "$(gm)"
expect "--no-sync reads the system tree" "" "$(cat "$S/root")"
TERMINAL="$S/term" "$E" upgrade --no-sync >/dev/null 2>&1 || true
case $(cat "$S/term.how") in *"emerge --sync"*) bad "upgrade --no-sync syncs" ;; *) ok "upgrade --no-sync does not sync" ;; esac

# 14j. Someone synced the system tree -> it is newer than ours, so it wins.
touch "$S/repos/gentoo/metadata/timestamp.chk" "$S/repos/gitov/.git/FETCH_HEAD"
"$E" check >/dev/null 2>&1 || true
expect "fresh system tree is used, not synced" "$n" "$(gm)"
expect "fresh system tree: no config root" "" "$(cat "$S/root")"
echo
if [ "$fails" -ne 0 ]; then
    echo "$fails failed"
    exit 1
fi
echo "all tests passed"
