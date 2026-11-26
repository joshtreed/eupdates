// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * eupdates - count pending Gentoo (portage) package updates, for any status bar.
 *
 *   status   print the count once from the cache (default; instant)
 *   watch    keep running; print a new line whenever the output changes
 *   check    resolve the @world upgrade set now and refresh the cache
 *   list     print the cached list of pending packages
 *   upgrade  run the upgrade in $TERMINAL, then re-check
 *
 * Bars either poll a command (tmux, conky, i3status-rust...) or keep one
 * running and read its lines (waybar, polybar tail, i3blocks persist, eww...).
 * `status` serves the first kind, `watch` the second. Both refresh the cache on
 * their own -- a background check starts when the tree was synced, an emerge
 * finished, the world file changed, or the last check is older than a day -- so
 * no timer, cron job or init-system unit is needed. --no-auto turns that off for
 * people who would rather run `eupdates check` themselves.
 *
 * It NEVER contacts a mirror. `emerge --pretend` resolves against the
 * already-synced local tree + installed db only. The only rate-limited,
 * mirror-hitting operation on Gentoo is `emerge --sync`, which is deliberately
 * left out.
 *
 * Copyright 2026 Josh Reed. GNU GPL v3 or later; see LICENSE.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef EUPDATES_VERSION
#define EUPDATES_VERSION "0.1.0"
#endif

/* Every system path is overridable at build time so the tests can run without
 * root and without a real two-minute resolve. */
#if defined(EUPDATES_EMERGE_CMD) || defined(EUPDATES_GLSA_CMD) || defined(EUPDATES_TREE_TS)
#error "0.2: use EUPDATES_EMERGE / EUPDATES_GLSA (programs) and EUPDATES_PORTAGE_DIR"
#endif
#ifndef EUPDATES_EMERGE /* offline: resolves the local tree only */
#define EUPDATES_EMERGE "emerge"
#endif
#ifndef EUPDATES_GLSA /* offline: reads GLSA metadata shipped with the tree */
#define EUPDATES_GLSA "glsa-check"
#endif
#ifndef EUPDATES_PORTAGE_DIR /* user config: package.*, make.conf, repos.conf */
#define EUPDATES_PORTAGE_DIR "/etc/portage"
#endif
#ifndef EUPDATES_REPOS_DEFAULT /* portage's own repos.conf ([gentoo] location) */
#define EUPDATES_REPOS_DEFAULT "/usr/share/portage/config/repos.conf"
#endif
#ifndef EUPDATES_MTIMEDB /* rewritten by portage once per merge/unmerge */
#define EUPDATES_MTIMEDB "/var/cache/edb/mtimedb"
#endif
#ifndef EUPDATES_PROC /* where to look for a running emerge/emaint */
#define EUPDATES_PROC "/proc"
#endif
#ifndef EUPDATES_WORLD /* the @world selection */
#define EUPDATES_WORLD "/var/lib/portage/world"
#endif

static const char *DEFAULT_FORMAT = "{count}";
static const int CHECKING_STALE = 300; /* drop a stuck "checking" marker past this (s) */
static const int MERGE_QUIET = 60;     /* portage idle this long after a merge (s)     */
static const int DEFAULT_MAX_AGE = 24; /* re-check at least this often (hours)         */
static const int WATCH_TICK = 2;       /* watch: seconds between looks at the cache    */
static const int PREVIEW = 25;         /* tooltip package lines before eliding         */
static const int STALE_DAYS = 2;       /* nag about syncing past this age (days)       */

/* ---- small helpers ------------------------------------------------------- */

static void cache_path(char *buf, size_t n, const char *leaf) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/eupdates/%s", xdg, leaf);
    else
        snprintf(buf, n, "%s/.cache/eupdates/%s", home ? home : ".", leaf);
}

static void ensure_cache_dir(void) {
    char dir[4096];
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(dir, sizeof dir, "%s/eupdates", xdg);
    } else {
        snprintf(dir, sizeof dir, "%s/.cache", home ? home : ".");
        mkdir(dir, 0755); /* parent may be missing; ignore EEXIST */
        snprintf(dir, sizeof dir, "%s/.cache/eupdates", home ? home : ".");
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "eupdates: mkdir %s: %s\n", dir, strerror(errno));
}

/* mtime of a path, or 0 if it can't be stat'ed. */
static time_t mtime_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void touch(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static int count_lines(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int n = 0, c, prev = '\n';
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') n++;
        prev = c;
    }
    if (prev != '\n') n++; /* last line without a newline */
    fclose(fp);
    return n;
}

static int read_int_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int v = 0;
    if (fscanf(fp, "%d", &v) != 1) v = 0;
    fclose(fp);
    return v;
}

/* ctime of a path, or 0. A sync sets the files it copies to the mirror's
 * mtime, so only the change time says when this machine last synced. */
static time_t ctime_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_ctime : 0;
}

/* ---- repositories and config --------------------------------------------- */

struct repos {
    time_t synced;     /* newest sync of any repository (ctime), 0 unknown */
    char gentoo[4096]; /* location of the [gentoo] repository, "" unknown */
};

static void trim(char **s) {
    while (isspace((unsigned char)**s))
        (*s)++;
    char *e = *s + strlen(*s);
    while (e > *s && isspace((unsigned char)e[-1]))
        *--e = '\0';
}

/* A sync leaves its mark on metadata/timestamp.chk (rsync, and git mirrors of
 * the gentoo tree) or .git/FETCH_HEAD (every git fetch, overlays included). */
static time_t repo_synced(const char *loc) {
    char p[4096 + 32];
    snprintf(p, sizeof p, "%s/metadata/timestamp.chk", loc);
    time_t a = ctime_of(p);
    snprintf(p, sizeof p, "%s/.git/FETCH_HEAD", loc);
    time_t b = ctime_of(p);
    return a > b ? a : b;
}

static void scan_repos_file(const char *path, struct repos *r) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char *line = NULL, section[256] = "";
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1) {
        char *s = line;
        trim(&s);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) snprintf(section, sizeof section, "%.*s", (int)(e - s - 1), s + 1);
            continue;
        }
        if (strncmp(s, "location", 8) != 0) continue;
        char *v = s + 8;
        trim(&v);
        if (*v != '=') continue;
        v++;
        trim(&v);
        if (!*v) continue;
        time_t t = repo_synced(v);
        if (t > r->synced) r->synced = t;
        if (strcmp(section, "gentoo") == 0) snprintf(r->gentoo, sizeof r->gentoo, "%s", v);
    }
    free(line);
    fclose(fp);
}

/* Like portage: repos.conf is a file or a directory of files; hidden and
 * backup~ files are skipped. */
static void scan_repos_conf(const char *path, struct repos *r) {
    DIR *d = opendir(path);
    if (!d) {
        scan_repos_file(path, r);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || e->d_name[n - 1] == '~') continue;
        char p[4096 + 256];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        scan_repos_file(p, r);
    }
    closedir(d);
}

/* Every configured repository, overlays included: portage's defaults first,
 * then the user's repos.conf, which may move [gentoo]. */
static void find_repos(struct repos *r) {
    r->synced = 0;
    r->gentoo[0] = '\0';
    scan_repos_conf(EUPDATES_REPOS_DEFAULT, r);
    scan_repos_conf(EUPDATES_PORTAGE_DIR "/repos.conf", r);
}

/* Newest change under the user's portage config (package.use, accept_keywords,
 * make.conf, a new profile link...), `depth` directory levels down. Symlinks
 * are not followed, so the profile tree itself is not walked; gnupg/ holds
 * binary-package trust keys, not update policy. */
static time_t config_changed(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    time_t t = st.st_ctime;
    if (depth <= 0 || !S_ISDIR(st.st_mode)) return t;
    DIR *d = opendir(path);
    if (!d) return t;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || e->d_name[n - 1] == '~' ||
            strcmp(e->d_name, "gnupg") == 0)
            continue;
        char p[4096 + 256];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        time_t c = config_changed(p, depth - 1);
        if (c > t) t = c;
    }
    closedir(d);
    return t;
}

/* Age of the gentoo tree's snapshot (its timestamp.chk mtime is the mirror's
 * time), in days, or -1 unknown. */
static int tree_age_days(void) {
    struct repos r;
    find_repos(&r);
    if (!r.gentoo[0]) return -1;
    char p[4096 + 32];
    snprintf(p, sizeof p, "%s/metadata/timestamp.chk", r.gentoo);
    time_t t = mtime_of(p);
    if (!t) return -1;
    double d = difftime(time(NULL), t) / 86400.0;
    return d < 0 ? 0 : (int)d;
}

/* ---- notifications ------------------------------------------------------- */

/* Best-effort desktop notification; silently a no-op if notify-send is absent.
 * The icon is an XDG icon-theme name or an absolute image path (EUPDATES_ICON). */
static void send_notify(const char *urgency, const char *body) {
    const char *icon = getenv("EUPDATES_ICON");
    if (!icon || !*icon) icon = "system-software-update";
    pid_t pid = fork();
    if (pid == 0) {
        execlp("notify-send", "notify-send", "-a", "eupdates", "-u", urgency, "-i", icon,
               "Package updates", body, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
}

static int env_flag(const char *name, int dflt) {
    const char *e = getenv(name);
    if (!e || !*e) return dflt;
    return strcmp(e, "0") != 0;
}

/* A background check reports only a count that ROSE, or the first count ever:
 * a count that fell is the user's own emerge, so it is not news. A manual
 * check reports every outcome, "no change" included, because it was asked for. */
static void notify_result(int old, int now, int manual) {
    char body[128];
    if (!manual && old >= 0 && now <= old) return;
    if (old != now) {
        if (now == 0)
            snprintf(body, sizeof body, "System up to date");
        else if (old < 0)
            snprintf(body, sizeof body, "%d update%s pending", now, now == 1 ? "" : "s");
        else
            snprintf(body, sizeof body, "%d update%s pending (was %d)", now,
                     now == 1 ? "" : "s", old);
        send_notify("normal", body);
    } else if (manual) {
        if (now == 0)
            snprintf(body, sizeof body, "System up to date");
        else
            snprintf(body, sizeof body, "No change \xe2\x80\x94 %d pending", now);
        send_notify("low", body);
    }
}

/* ---- check --------------------------------------------------------------- */

/* Write one list line for an `[ebuild ...] atom [old] ...` merge line.
 * Returns 1 if a package line was written, 0 otherwise. */
static int format_pkg(const char *line, FILE *out) {
    const char *p = strchr(line, ']');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '\n') return 0;

    const char *as = p, *ae = p; /* new atom cat/pkg-ver */
    while (*ae && !isspace((unsigned char)*ae) && *ae != ':')
        ae++;
    if (ae == as) return 0;

    const char *ob = NULL, *oe = NULL; /* [oldver] if present */
    for (const char *q = ae; *q && *q != '\n'; q++) {
        if (strncmp(q, "USE=", 4) == 0) break; /* past the version columns */
        if (*q == '[') {
            const char *re = q + 1;
            while (*re && !isspace((unsigned char)*re) && *re != ':' && *re != ']')
                re++;
            ob = q + 1;
            oe = re;
            break;
        }
    }

    fwrite(as, 1, (size_t)(ae - as), out);
    if (ob && oe > ob) {
        fputs("  (was ", out);
        fwrite(ob, 1, (size_t)(oe - ob), out);
        fputc(')', out);
    }
    fputc('\n', out);
    return 1;
}

/* Run argv (PATH lookup, no shell) with its stdout piped back. stdin and
 * stderr are /dev/null, so an --ask in EMERGE_DEFAULT_OPTS can never wait on
 * input. The user's portage settings (EMERGE_DEFAULT_OPTS included) are kept on
 * purpose: they decide what a real upgrade would merge. Only the output is
 * pinned: no colour, C locale. */
static FILE *run_reader(char *const argv[], pid_t *pid) {
    int p[2];
    if (pipe(p) != 0) return NULL;
    fflush(NULL);
    pid_t c = fork();
    if (c < 0) {
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    if (c == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul < 0) _exit(127);
        dup2(nul, 0);
        dup2(p[1], 1);
        dup2(nul, 2);
        close(p[0]);
        close(p[1]);
        if (nul > 2) close(nul);
        setenv("NOCOLOR", "true", 1);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    FILE *fp = fdopen(p[0], "r");
    if (!fp) {
        close(p[0]);
        waitpid(c, NULL, 0);
        return NULL;
    }
    *pid = c;
    return fp;
}

/* Close a run_reader() stream; the child's exit code, or -1 if it died. */
static int finish_reader(FILE *fp, pid_t pid) {
    fclose(fp);
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int count_glsa(void) {
    char *argv[] = {EUPDATES_GLSA, "--test", "affected", NULL};
    pid_t pid;
    FILE *fp = run_reader(argv, &pid);
    if (!fp) return 0;
    int n = 0;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1)
        if (isdigit((unsigned char)line[0])) n++; /* affected GLSA ids */
    free(line);
    finish_reader(fp, pid);
    return n;
}

/* Take the cache lock for the life of this process. Only one check runs at a
 * time, however many bars, watchers and clicks ask for one. Returns 0 on
 * success, 1 if another check holds it, -1 on error. */
static int take_lock(void) {
    char path[4096];
    cache_path(path, sizeof path, "lock");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    struct flock fl = {0};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) == 0) return 0; /* fd stays open: lock held until exit */
    int busy = (errno == EACCES || errno == EAGAIN);
    close(fd);
    return busy ? 1 : -1;
}

/* Resolve @world and replace the cache. `manual` = asked for by a person
 * (shows the "checking" state and reports every outcome). */
static int run_check(int manual, int notify) {
    ensure_cache_dir();
    int lk = take_lock();
    if (lk == 1) {
        if (manual) {
            fputs("eupdates: a check is already running\n", stderr);
            if (notify) send_notify("low", "A check is already running");
        }
        return 0;
    }
    if (lk < 0) {
        fprintf(stderr, "eupdates: cannot lock the cache: %s\n", strerror(errno));
        return 1;
    }

    char list[4096], tmp[4096], sec[4096], mark[4096], stamp[4096], tmpleaf[64];
    cache_path(list, sizeof list, "list");
    cache_path(sec, sizeof sec, "security");
    cache_path(mark, sizeof mark, "checking");
    cache_path(stamp, sizeof stamp, "attempt");
    snprintf(tmpleaf, sizeof tmpleaf, "list.%ld.tmp", (long)getpid());
    cache_path(tmp, sizeof tmp, tmpleaf);

    /* Stamp the attempt first: a failed resolve still counts, so a broken tree
     * is retried when something changes, not on every watch tick. */
    touch(stamp);
    int old = count_lines(list); /* -1 if no prior cache */
    if (manual) touch(mark);     /* watchers show "checking" while we resolve */

    int rc = 1, n = 0;
    FILE *out = fopen(tmp, "w");
    char *argv[] = {EUPDATES_EMERGE, "--pretend",   "--update", "--deep", "--newuse",
                    "--color=n",     "--nospinner", "@world",   NULL};
    pid_t pid = 0;
    FILE *fp = out ? run_reader(argv, &pid) : NULL;
    if (!out) {
        fprintf(stderr, "eupdates: %s: %s\n", tmp, strerror(errno));
    } else if (!fp) {
        fprintf(stderr, "eupdates: cannot run emerge\n");
        fclose(out);
    } else {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, fp) != -1)
            if (strncmp(line, "[ebuild", 7) == 0 || strncmp(line, "[binary", 7) == 0)
                n += format_pkg(line, out);
        free(line);
        int st = finish_reader(fp, pid);
        fclose(out);
        /* emerge failed (blockers, broken tree, not installed): keep the old
         * cache rather than report zero. */
        if (st != 0)
            fputs("eupdates: emerge exited abnormally; keeping cache\n", stderr);
        else if (rename(tmp, list) != 0)
            fprintf(stderr, "eupdates: rename: %s\n", strerror(errno));
        else
            rc = 0;
    }
    unlink(tmp); /* no-op after a successful rename */

    if (rc == 0) {
        FILE *sf = fopen(sec, "w");
        if (sf) {
            fprintf(sf, "%d\n", count_glsa());
            fclose(sf);
        }
    }
    if (manual) unlink(mark);
    if (rc == 0 && notify) notify_result(old, n, manual);
    return rc;
}

/* ---- automatic refresh --------------------------------------------------- */

/* Is an emerge or emaint running right now (a merge, a sync)? Linux /proc;
 * anywhere else this answers "no" and the quiet-time rule still applies. */
static int portage_busy(void) {
    DIR *d = opendir(EUPDATES_PROC);
    if (!d) return 0;
    struct dirent *e;
    int busy = 0;
    while (!busy && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[4096], comm[32] = "";
        snprintf(path, sizeof path, "%s/%s/comm", EUPDATES_PROC, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = '\0';
            busy = strcmp(comm, "emerge") == 0 || strcmp(comm, "emaint") == 0;
        }
        fclose(f);
    }
    closedir(d);
    return busy;
}

static int max_age_secs(void) {
    const char *e = getenv("EUPDATES_MAX_AGE");
    if (e && *e) {
        char *end;
        long h = strtol(e, &end, 10);
        if (*end == '\0' && h >= 1 && h <= 24 * 365) return (int)(h * 3600);
    }
    return DEFAULT_MAX_AGE * 3600;
}

/* Does the cache need a fresh resolve? Yes when there has never been an attempt,
 * or since the last attempt any repository was synced, the world file or the
 * portage config changed, an emerge finished, or a day went by. Never while
 * portage is mid-merge/sync: the answer would be stale before it landed.
 * Two small repos.conf reads and a stat per config file (about 60 on a typical
 * system): cheap enough for every poll and every watch tick. */
static int out_of_date(void) {
    char stamp[4096];
    cache_path(stamp, sizeof stamp, "attempt");
    time_t last = mtime_of(stamp), now = time(NULL);
    time_t merged = mtime_of(EUPDATES_MTIMEDB);

    struct repos r;
    find_repos(&r);

    int due = last == 0 || r.synced > last || mtime_of(EUPDATES_WORLD) > last ||
              config_changed(EUPDATES_PORTAGE_DIR, 2) > last ||
              now - last >= max_age_secs() ||
              (merged > last && now - merged >= MERGE_QUIET);
    return due && !portage_busy();
}

/* Start one detached, niced check and return at once. Double-forked and in its
 * own session, so it outlives a bar reload and leaves no zombie; the lock in
 * run_check() turns any concurrent extras into no-ops. */
static void spawn_check(int notify) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        setpriority(PRIO_PROCESS, 0, 15);
        _exit(run_check(0, notify));
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
}

/* ---- output -------------------------------------------------------------- */

struct snapshot {
    int count; /* -1 = no check has run yet */
    int glsa;
    int synced; /* days since tree sync, -1 unknown */
    int checking;
    const char *state;
    time_t checked;
};

static void take_snapshot(struct snapshot *s) {
    char list[4096], sec[4096], mark[4096];
    cache_path(list, sizeof list, "list");
    cache_path(sec, sizeof sec, "security");
    cache_path(mark, sizeof mark, "checking");

    time_t m = mtime_of(mark);
    s->checking = m && difftime(time(NULL), m) < CHECKING_STALE;
    if (m && !s->checking) unlink(mark); /* a manual check died mid-flight */

    s->count = count_lines(list);
    s->checked = mtime_of(list);
    s->glsa = read_int_file(sec);
    s->synced = tree_age_days();
    s->state = s->checking     ? "checking"
               : s->count < 0  ? "unknown"
               : s->count == 0 ? "updated"
                               : "pending";
}

/* Expand {count} {glsa} {synced} {state}; anything else is copied as is. */
static void expand(FILE *out, const char *tpl, const struct snapshot *s) {
    for (const char *p = tpl; *p; p++) {
        if (*p == '{') {
            if (strncmp(p, "{count}", 7) == 0) {
                if (s->count < 0)
                    fputc('?', out);
                else
                    fprintf(out, "%d", s->count);
                p += 6;
                continue;
            }
            if (strncmp(p, "{glsa}", 6) == 0) {
                fprintf(out, "%d", s->glsa);
                p += 5;
                continue;
            }
            if (strncmp(p, "{synced}", 8) == 0) {
                if (s->synced < 0)
                    fputc('?', out);
                else
                    fprintf(out, "%d", s->synced);
                p += 7;
                continue;
            }
            if (strncmp(p, "{state}", 7) == 0) {
                fputs(s->state, out);
                p += 6;
                continue;
            }
        }
        fputc(*p, out);
    }
}

static void json_escape(FILE *out, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (c < 0x20)
                fprintf(out, "\\u%04x", c);
            else
                fputc(c, out); /* UTF-8 bytes pass through */
        }
    }
}

static void write_tooltip(FILE *tb, const struct snapshot *s) {
    char checked[64] = "";
    struct tm tmv;
    if (s->checked && localtime_r(&s->checked, &tmv))
        strftime(checked, sizeof checked, "%a %b %d %I:%M %p", &tmv);

    if (s->checking) fputs("Checking for updates\xe2\x80\xa6\n", tb);
    if (s->count < 0)
        fputs("No update check has run yet", tb);
    else if (s->count == 0)
        fprintf(tb, "System up to date \xe2\x80\xa2 checked %s", checked);
    else
        fprintf(tb, "%d update%s pending \xe2\x80\xa2 checked %s", s->count,
                s->count == 1 ? "" : "s", checked);
    if (s->glsa > 0)
        fprintf(tb, "\n%d unresolved GLSA%s (run glsa-check)", s->glsa,
                s->glsa == 1 ? "" : "s");
    if (s->synced >= STALE_DAYS)
        fprintf(tb, "\nportage tree synced %d days ago \xe2\x80\x94 sync to refresh",
                s->synced);

    if (s->count > 0) {
        char list[4096];
        cache_path(list, sizeof list, "list");
        FILE *lf = fopen(list, "r");
        if (lf) {
            fputs("\n", tb);
            char *line = NULL;
            size_t cap = 0;
            int shown = 0;
            while (getline(&line, &cap, lf) != -1 && shown++ < PREVIEW) {
                fputs("\n  ", tb);
                line[strcspn(line, "\n")] = '\0';
                fputs(line, tb);
            }
            free(line);
            fclose(lf);
            if (s->count > PREVIEW)
                fprintf(tb,
                        "\n  \xe2\x80\xa6"
                        "and %d more",
                        s->count - PREVIEW);
        }
    }
}

/* One output line, newline-terminated, in a malloc'd string. */
static char *render(const char *fmt, int json) {
    struct snapshot s;
    take_snapshot(&s);
    char *buf = NULL, *text = NULL, *tip = NULL;
    size_t n = 0, tn = 0, pn = 0;

    FILE *tf = open_memstream(&text, &tn);
    expand(tf, fmt, &s);
    fclose(tf);

    FILE *out = open_memstream(&buf, &n);
    if (!json) {
        fputs(text, out);
    } else {
        FILE *tb = open_memstream(&tip, &pn);
        write_tooltip(tb, &s);
        fclose(tb);
        fputs("{\"text\":\"", out);
        json_escape(out, text);
        fputs("\",\"tooltip\":\"", out);
        json_escape(out, tip);
        fprintf(out, "\",\"class\":\"%s\",\"alt\":\"%s\"}", s.state, s.state);
        free(tip);
    }
    fputc('\n', out);
    fclose(out);
    free(text);
    return buf;
}

/* ---- commands ------------------------------------------------------------ */

static int cmd_status(const char *fmt, int json, int autocheck, int notify) {
    if (autocheck && out_of_date()) spawn_check(notify);
    char *line = render(fmt, json);
    fputs(line, stdout);
    free(line);
    return 0;
}

/* Has whoever reads our stdout closed it? A pipe whose read end is gone polls
 * as POLLERR on the write end, so this notices a bar reload at once instead of
 * at the next write. A terminal or file never reports it. */
static int reader_gone(void) {
    struct pollfd p = {.fd = STDOUT_FILENO, .events = 0};
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL));
}

static int cmd_watch(const char *fmt, int json, int autocheck, int notify) {
    char *last = NULL;
    for (;;) {
        if (autocheck && out_of_date()) spawn_check(notify);
        char *line = render(fmt, json);
        if (!last || strcmp(line, last) != 0) {
            fputs(line, stdout);
            if (fflush(stdout) != 0) return 0; /* the bar went away */
            free(last);
            last = line;
        } else {
            free(line);
        }
        if (reader_gone()) return 0; /* bar reloaded: don't linger until next change */
        sleep((unsigned)WATCH_TICK);
    }
}

static int cmd_list(void) {
    char list[4096];
    cache_path(list, sizeof list, "list");
    FILE *f = fopen(list, "r");
    if (!f) {
        fputs("eupdates: no update check has run yet\n", stderr);
        return 1;
    }
    char buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0)
        fwrite(buf, 1, k, stdout);
    fclose(f);
    return 0;
}

static int cmd_upgrade(int notify) {
    const char *term = getenv("TERMINAL");
    if (!term || !*term) term = "xterm";
    pid_t pid = fork();
    if (pid == 0) {
        execlp(term, term, "-e", "sh", "-c",
               "sudo emerge --ask --verbose --update --deep --newuse @world; "
               "printf '\\nDone \\xe2\\x80\\x94 press enter to close.'; read _",
               (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
    return run_check(1, notify);
}

/* ---- main ---------------------------------------------------------------- */

static void usage(FILE *out, const char *prog) {
    fprintf(out,
            "Usage: %s [command] [options]\n"
            "\n"
            "Count pending Gentoo (portage) @world updates, for any status bar.\n"
            "Resolves the already-synced local tree only; never contacts a mirror.\n"
            "\n"
            "Commands:\n"
            "  status    print the count once from the cache (default; instant)\n"
            "  watch     keep running; print a new line whenever the output changes\n"
            "  check     resolve @world now and refresh the cache\n"
            "  list      print the cached list of pending packages\n"
            "  upgrade   run the upgrade in $TERMINAL, then re-check\n"
            "\n"
            "Options:\n"
            "  --format TPL   output template; fields {count} {glsa} {synced} {state}\n"
            "                 (default: {count})\n"
            "  --json         one JSON object per line: text, tooltip, class, alt\n"
            "  --no-auto      never start a check on its own (run `check` yourself)\n"
            "  --notify       desktop-notify when the pending count changes\n"
            "  --no-notify    suppress notifications (overrides EUPDATES_NOTIFY)\n"
            "  -h, --help     show this help and exit\n"
            "  -V, --version  show version and exit\n"
            "\n"
            "Environment:\n"
            "  EUPDATES_AUTO      0 = same as --no-auto\n"
            "  EUPDATES_MAX_AGE   re-check at least every N hours (default: 24)\n"
            "  EUPDATES_NOTIFY    notify on change when non-empty and not \"0\"\n"
            "  EUPDATES_ICON      notification icon: theme name or image path\n"
            "  TERMINAL           terminal for `upgrade` (default: xterm)\n"
            "  XDG_CACHE_HOME     cache location (default: ~/.cache/eupdates)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *cmd = NULL, *fmt = DEFAULT_FORMAT;
    int json = 0, no_auto = 0, cli_notify = 0, cli_no_notify = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("eupdates %s\n", EUPDATES_VERSION);
            return 0;
        }
        if (strcmp(a, "--format") == 0) {
            if (i + 1 >= argc) {
                fputs("eupdates: --format requires a template\n", stderr);
                return 2;
            }
            fmt = argv[++i];
        } else if (strcmp(a, "--json") == 0) {
            json = 1;
        } else if (strcmp(a, "--no-auto") == 0) {
            no_auto = 1;
        } else if (strcmp(a, "--notify") == 0) {
            cli_notify = 1;
        } else if (strcmp(a, "--no-notify") == 0) {
            cli_no_notify = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "eupdates: unknown option '%s'\n", a);
            usage(stderr, argv[0]);
            return 2;
        } else if (!cmd) {
            cmd = a;
        } else {
            fprintf(stderr, "eupdates: unexpected argument '%s'\n", a);
            return 2;
        }
    }
    if (!cmd) cmd = "status";

    int notify = !cli_no_notify && (cli_notify || env_flag("EUPDATES_NOTIFY", 0));
    int autocheck = !no_auto && env_flag("EUPDATES_AUTO", 1);
    signal(SIGPIPE, SIG_DFL);

    if (strcmp(cmd, "status") == 0) return cmd_status(fmt, json, autocheck, notify);
    if (strcmp(cmd, "watch") == 0) return cmd_watch(fmt, json, autocheck, notify);
    if (strcmp(cmd, "check") == 0) return run_check(1, notify);
    if (strcmp(cmd, "list") == 0) return cmd_list();
    if (strcmp(cmd, "upgrade") == 0) return cmd_upgrade(notify);
    fprintf(stderr, "eupdates: unknown command '%s'\n", cmd);
    usage(stderr, argv[0]);
    return 2;
}
