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
 * Nobody has to sync either. A user cannot sync the system tree, so when no
 * one has synced it for two days, eupdates keeps its own copy of each
 * repository under its cache and syncs that, at most once a day (the rate
 * Gentoo's mirrors ask for), verified the way portage verifies it. Root never
 * reads those copies: they only feed this user's `emerge --pretend`, and
 * `upgrade` syncs the system tree itself before merging.
 *
 * Copyright 2026 Josh Reed. GNU GPL v3 or later; see LICENSE.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700 /* realpath */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef EUPDATES_VERSION
#define EUPDATES_VERSION "0.2.1"
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
#ifndef EUPDATES_RSYNC
#define EUPDATES_RSYNC "rsync"
#endif
#ifndef EUPDATES_GIT
#define EUPDATES_GIT "git"
#endif
#ifndef EUPDATES_GEMATO /* portage's Manifest and OpenPGP verifier */
#define EUPDATES_GEMATO "gemato"
#endif
#ifndef EUPDATES_SYSTEM_FRESH /* system copy synced this recently: use it (s) */
#define EUPDATES_SYSTEM_FRESH (2 * 86400)
#endif

static const char *DEFAULT_FORMAT = "{count}";
static const int CHECKING_STALE = 300; /* drop a stuck "checking" marker past this (s) */
static const int MERGE_QUIET = 60;     /* portage idle this long after a merge (s)     */
static const int DEFAULT_MAX_AGE = 24; /* re-check at least this often (hours)         */
static const int WATCH_TICK = 2;       /* watch: seconds between looks at the cache    */
static const int PREVIEW = 25;         /* tooltip package lines before eliding         */
static const int STALE_DAYS = 2;       /* nag about syncing past this age (days)       */
static const int SYNC_EVERY = 86400;   /* sync our own copy at most this often (s)    */
static const int SYNC_SLACK = 600;     /* ...less this, so a daily check isn't skipped */
static const int SYNC_RETRY = 3600;    /* wait this long after a failed sync (s)      */
static int sync_on = 1;                /* --no-sync / EUPDATES_SYNC=0 clear it        */

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

/* repos.conf, read the way portage reads it: INI sections, a later value wins,
 * and [DEFAULT] fills in keys a section leaves out. */
struct kv {
    char *sec, *key, *val;
};
struct conf {
    struct kv *e;
    size_t n, cap;
};

static void trim(char **s) {
    while (isspace((unsigned char)**s))
        (*s)++;
    char *e = *s + strlen(*s);
    while (e > *s && isspace((unsigned char)e[-1]))
        *--e = '\0';
}

static void conf_add(struct conf *c, const char *sec, const char *key, const char *val) {
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 64;
        struct kv *e = realloc(c->e, cap * sizeof *e);
        if (!e) return;
        c->e = e;
        c->cap = cap;
    }
    struct kv *kv = &c->e[c->n];
    kv->sec = strdup(sec);
    kv->key = strdup(key);
    kv->val = strdup(val);
    if (!kv->sec || !kv->key || !kv->val) {
        free(kv->sec);
        free(kv->key);
        free(kv->val);
        return;
    }
    c->n++;
}

static void conf_free(struct conf *c) {
    for (size_t i = 0; i < c->n; i++) {
        free(c->e[i].sec);
        free(c->e[i].key);
        free(c->e[i].val);
    }
    free(c->e);
    c->e = NULL;
    c->n = c->cap = 0;
}

/* A key's value in a section, falling back to [DEFAULT]; NULL if unset. */
static const char *conf_get(const struct conf *c, const char *sec, const char *key) {
    for (size_t i = c->n; i-- > 0;)
        if (strcmp(c->e[i].sec, sec) == 0 && strcmp(c->e[i].key, key) == 0)
            return c->e[i].val;
    return strcmp(sec, "DEFAULT") != 0 ? conf_get(c, "DEFAULT", key) : NULL;
}

/* portage's booleans: yes/true on, no/false off. */
static int conf_bool(const struct conf *c, const char *sec, const char *key, int dflt) {
    const char *v = conf_get(c, sec, key);
    if (!v || !*v) return dflt;
    char low[8] = "";
    for (size_t i = 0; v[i] && i < sizeof low - 1; i++)
        low[i] = (char)tolower((unsigned char)v[i]), low[i + 1] = '\0';
    if (strcmp(low, "yes") == 0 || strcmp(low, "true") == 0) return 1;
    if (strcmp(low, "no") == 0 || strcmp(low, "false") == 0) return 0;
    return dflt;
}

static void conf_read_file(struct conf *c, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char *line = NULL, section[256] = "";
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1) {
        char *s = line;
        trim(&s);
        if (*s == '#' || *s == ';' || !*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) snprintf(section, sizeof section, "%.*s", (int)(e - s - 1), s + 1);
            continue;
        }
        if (!section[0]) continue;
        char *sep = s + strcspn(s, "=:");
        if (!*sep) continue;
        *sep = '\0';
        char *k = s, *v = sep + 1;
        trim(&k);
        trim(&v);
        for (char *p = k; *p; p++) /* option names are case-insensitive */
            *p = (char)tolower((unsigned char)*p);
        conf_add(c, section, k, v);
    }
    free(line);
    fclose(fp);
}

/* Like portage: repos.conf is a file or a directory of files, read in name
 * order; hidden and backup~ files are skipped. */
static int name_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void conf_read(struct conf *c, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        conf_read_file(c, path);
        return;
    }
    char **names = NULL;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t k = strlen(e->d_name);
        if (e->d_name[0] == '.' || e->d_name[k - 1] == '~') continue;
        char **nn = realloc(names, (n + 1) * sizeof *names);
        if (!nn) break;
        names = nn;
        if ((names[n] = strdup(e->d_name)) != NULL) n++;
    }
    closedir(d);
    qsort(names, n, sizeof *names, name_cmp);
    for (size_t i = 0; i < n; i++) {
        char p[4096 + 256];
        snprintf(p, sizeof p, "%s/%s", path, names[i]);
        conf_read(c, p);
        free(names[i]);
    }
    free(names);
}

/* Every configured repository, overlays included: portage's defaults first,
 * then the user's repos.conf, which may move or re-point [gentoo]. */
static void load_repos(struct conf *c) {
    memset(c, 0, sizeof *c);
    conf_read(c, EUPDATES_REPOS_DEFAULT);
    conf_read(c, EUPDATES_PORTAGE_DIR "/repos.conf");
}

/* The i-th distinct repository section with a location, or NULL past the end. */
static const char *conf_repo(const struct conf *c, size_t i) {
    size_t seen = 0;
    for (size_t k = 0; k < c->n; k++) {
        const char *s = c->e[k].sec;
        if (strcmp(s, "DEFAULT") == 0) continue;
        int dup = 0;
        for (size_t j = 0; j < k && !dup; j++)
            dup = strcmp(c->e[j].sec, s) == 0;
        if (dup) continue;
        const char *loc = conf_get(c, s, "location");
        if (!loc || !*loc) continue;
        if (seen++ == i) return s;
    }
    return NULL;
}

static const char *main_repo(const struct conf *c) {
    const char *m = conf_get(c, "DEFAULT", "main-repo");
    return m && *m ? m : "gentoo";
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

/* Newest sync of any system repository, 0 unknown. */
static time_t repos_synced(const struct conf *c) {
    time_t t = 0;
    const char *s;
    for (size_t i = 0; (s = conf_repo(c, i)) != NULL; i++) {
        time_t r = repo_synced(conf_get(c, s, "location"));
        if (r > t) t = r;
    }
    return t;
}

/* ---- our own copies of the repositories ---------------------------------- */

enum sync_kind { SYNC_NONE, SYNC_RSYNC, SYNC_GIT };

/* How eupdates may sync a repository itself: rsync or git with an upstream
 * URI, auto-sync not off, and a name that is safe as a directory name.
 * Anything else (webrsync, mercurial, a local overlay) uses the system copy. */
static enum sync_kind sync_kind(const struct conf *c, const char *sec) {
    if (!*sec || sec[0] == '.' ||
        strspn(sec, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                    "0123456789_-.+") != strlen(sec))
        return SYNC_NONE;
    const char *type = conf_get(c, sec, "sync-type"), *uri = conf_get(c, sec, "sync-uri");
    if (!type || !uri || !*uri || !conf_bool(c, sec, "auto-sync", 1)) return SYNC_NONE;
    if (strcmp(type, "rsync") == 0) return SYNC_RSYNC;
    if (strcmp(type, "git") == 0) return SYNC_GIT;
    return SYNC_NONE;
}

/* repos/<name> (our copy), repos/.<name>.synced (start of the last good
 * sync) and repos/.<name>.tried (start of the last failed attempt). */
static void copy_path(char *buf, size_t n, const char *sec, const char *suffix) {
    char leaf[512];
    if (suffix[0] == '.' && strcmp(suffix, ".new") != 0 && strcmp(suffix, ".old") != 0)
        snprintf(leaf, sizeof leaf, "repos/.%s%s", sec, suffix);
    else
        snprintf(leaf, sizeof leaf, "repos/%s%s", sec, suffix);
    cache_path(buf, n, leaf);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Should portage read our copy of this repository rather than the system's?
 * Only when we synced ours more recently than anyone synced theirs. Fills
 * `out` with our copy's path. */
static int use_copy(const struct conf *c, const char *sec, char *out, size_t n) {
    if (!sync_on || sync_kind(c, sec) == SYNC_NONE) return 0;
    char stamp[4096];
    copy_path(stamp, sizeof stamp, sec, ".synced");
    copy_path(out, n, sec, "");
    time_t mine = mtime_of(stamp);
    return mine && is_dir(out) && mine > repo_synced(conf_get(c, sec, "location"));
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

/* Age of the newest snapshot of the main tree, ours or the system's, in days
 * (timestamp.chk's mtime is the mirror's time), or -1 unknown. */
static int tree_age_days(void) {
    struct conf c;
    load_repos(&c);
    const char *m = main_repo(&c), *loc = conf_get(&c, m, "location");
    char p[4096 + 32], mine[4096];
    time_t t = 0;
    if (loc && *loc) {
        snprintf(p, sizeof p, "%s/metadata/timestamp.chk", loc);
        t = mtime_of(p);
    }
    if (use_copy(&c, m, mine, sizeof mine)) {
        snprintf(p, sizeof p, "%s/metadata/timestamp.chk", mine);
        time_t o = mtime_of(p);
        if (o > t) t = o;
    }
    conf_free(&c);
    if (!t) return -1;
    double d = difftime(time(NULL), t) / 86400.0;
    return d < 0 ? 0 : (int)d;
}

/* ---- syncing our own copies ---------------------------------------------- */

/* Run argv (no shell) to the end with stdin and stdout on /dev/null; stderr is
 * kept so a failure says why. The exit code, or -1. */
static int run_quiet(char *const argv[]) {
    fflush(NULL);
    pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul < 0) _exit(127);
        dup2(nul, 0);
        dup2(nul, 1);
        if (nul > 2) close(nul);
        setenv("LC_ALL", "C", 1);
        setenv("GIT_TERMINAL_PROMPT", "0", 1); /* a dead URI fails, never prompts */
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(c, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* a/b into buf (just a when b is empty); 0 if it doesn't fit. */
static int join(char *buf, size_t n, const char *a, const char *b) {
    int k = snprintf(buf, n, *b ? "%s/%s" : "%s%s", a, b);
    return k >= 0 && (size_t)k < n;
}

/* rm -r, without following symlinks. */
static void rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char p[4096 + 256];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            rm_tree(p);
        }
        closedir(d);
    }
    rmdir(path);
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm). */
static long days_from_civil(long y, long m, long d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The signed `TIMESTAMP` in a tree's top Manifest, or 0. It is inside the
 * signature, so unlike file times a mirror cannot fake it. */
static time_t manifest_time(const char *tree) {
    char p[4096 + 16];
    snprintf(p, sizeof p, "%s/Manifest", tree);
    FILE *fp = fopen(p, "r");
    if (!fp) return 0;
    char *line = NULL;
    size_t cap = 0;
    time_t t = 0;
    while (!t && getline(&line, &cap, fp) != -1) {
        int y, mo, d, h, mi, s;
        if (sscanf(line, "TIMESTAMP %d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) == 6)
            t = (time_t)days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    }
    free(line);
    fclose(fp);
    return t;
}

/* gemato's -K key and -R (no key refresh) options for a repository. Fails
 * closed: a repository that asks for verification but names no key is not
 * synced. Returns the number of argv slots filled, or -1.
 *
 * The refresh gets a short connect timeout. gemato's own is 180 s per address
 * and it tries each address in turn, so on a network that drops some of a
 * host's IPv6 addresses (common behind VPNs) one refresh took 16 minutes. A
 * refresh that still fails makes verification fail, so this only fails
 * sooner, never more open. */
static int key_args(const struct conf *c, const char *sec, char **argv) {
    const char *key = conf_get(c, sec, "sync-openpgp-key-path");
    if (!key || !*key) {
        fprintf(stderr,
                "eupdates: %s: verification asked for but no sync-openpgp-key-path\n", sec);
        return -1;
    }
    int n = 0;
    argv[n++] = "-K";
    argv[n++] = (char *)key;
    const char *refresh = conf_get(c, sec, "sync-openpgp-key-refresh");
    if (!conf_bool(c, sec, "sync-openpgp-key-refresh", 1) ||
        (refresh && strcasecmp(refresh, "false-nowarn") == 0)) {
        argv[n++] = "-R";
    } else {
        argv[n++] = "--timeout";
        argv[n++] = "10";
    }
    return n;
}

/* rsync: fetch into <name>.new (hard-linking unchanged files from our current
 * copy, or copying them from the system's on the first run, so only changes
 * cross the wire), verify it as portage would, then swap it in. Anything short
 * of a clean, verified, current snapshot leaves the old copy in place. */
static int sync_rsync(const struct conf *c, const char *sec) {
    char cur[4096], nw[4096], old[4096], uri[4096], dest[4096 + 16], seed[4096 + 32];
    copy_path(cur, sizeof cur, sec, "");
    copy_path(nw, sizeof nw, sec, ".new");
    copy_path(old, sizeof old, sec, ".old");
    if (!is_dir(cur) && is_dir(old)) rename(old, cur); /* a swap cut short */
    rm_tree(nw);
    rm_tree(old);

    const char *u = conf_get(c, sec, "sync-uri"), *sys = conf_get(c, sec, "location");
    size_t ul = strlen(u);
    snprintf(uri, sizeof uri, "%s%s", u, ul && u[ul - 1] == '/' ? "" : "/");
    snprintf(dest, sizeof dest, "%s/", nw);
    if (is_dir(cur))
        snprintf(seed, sizeof seed, "--link-dest=%s", cur);
    else if (sys && is_dir(sys))
        snprintf(seed, sizeof seed, "--copy-dest=%s", sys);
    else
        seed[0] = '\0';

    /* portage's PORTAGE_RSYNC_OPTS */
    char *argv[] = {EUPDATES_RSYNC,
                    "--recursive",
                    "--links",
                    "--safe-links",
                    "--perms",
                    "--times",
                    "--omit-dir-times",
                    "--compress",
                    "--force",
                    "--whole-file",
                    "--delete",
                    "--timeout=180",
                    "--exclude=/distfiles",
                    "--exclude=/local",
                    "--exclude=/packages",
                    "--exclude=/.git",
                    seed[0] ? seed : uri,
                    seed[0] ? uri : dest,
                    seed[0] ? dest : NULL,
                    NULL};
    if (run_quiet(argv) != 0) {
        fprintf(stderr, "eupdates: %s: rsync from %s failed\n", sec, u);
        rm_tree(nw);
        return 1;
    }

    if (conf_bool(c, sec, "sync-rsync-verify-metamanifest", 0)) {
        char *va[16] = {EUPDATES_GEMATO, "verify", "-q", "-s"};
        int n = 4, k = key_args(c, sec, va + n);
        if (k < 0) {
            rm_tree(nw);
            return 1;
        }
        n += k;
        const char *jobs = conf_get(c, sec, "sync-rsync-verify-jobs");
        if (jobs && *jobs) {
            va[n++] = "-j";
            va[n++] = (char *)jobs;
        }
        va[n++] = nw;
        va[n] = NULL;
        if (run_quiet(va) != 0) {
            fprintf(stderr,
                    "eupdates: %s: Manifest verification failed; keeping old copy\n", sec);
            rm_tree(nw);
            return 1;
        }
        /* A validly signed but old snapshot: a stale or malicious mirror. */
        time_t got = manifest_time(nw), have = manifest_time(cur);
        const char *ma = conf_get(c, sec, "sync-rsync-verify-max-age");
        long max_days = ma ? strtol(ma, NULL, 10) : 0;
        if (!got || (max_days > 0 && difftime(time(NULL), got) > max_days * 86400.0) ||
            got < have) {
            fprintf(stderr, "eupdates: %s: mirror snapshot is stale or older than ours\n",
                    sec);
            rm_tree(nw);
            return 1;
        }
    }

    if (is_dir(cur) && rename(cur, old) != 0) {
        rm_tree(nw);
        return 1;
    }
    if (rename(nw, cur) != 0) {
        rename(old, cur);
        rm_tree(nw);
        return 1;
    }
    rm_tree(old);
    return 0;
}

/* git: start from a local clone of the system's copy (no hardlinks, so its
 * maintenance can never pull objects out from under ours), fetch the tip,
 * check its signature if the repository asks, and only then check it out. */
static int sync_git(const struct conf *c, const char *sec) {
    char cur[4096], gitdir[4096 + 8], safe[4096 + 32];
    copy_path(cur, sizeof cur, sec, "");
    snprintf(gitdir, sizeof gitdir, "%s/.git", cur);
    const char *uri = conf_get(c, sec, "sync-uri"), *sys = conf_get(c, sec, "location");

    if (!is_dir(gitdir)) {
        rm_tree(cur);
        int ok = 0;
        if (sys && *sys) {
            /* The system clone belongs to portage; reading it is fine. */
            snprintf(safe, sizeof safe, "safe.directory=%s", sys);
            char *cl[] = {
                EUPDATES_GIT,    "-c",        safe, "clone", "--quiet", "--no-hardlinks",
                "--no-checkout", (char *)sys, cur,  NULL};
            ok = run_quiet(cl) == 0;
            if (!ok) rm_tree(cur);
        }
        char *in[] = {EUPDATES_GIT, "init", "--quiet", cur, NULL};
        if (!ok && run_quiet(in) != 0) {
            rm_tree(cur);
            return 1;
        }
    }

    const char *branch = conf_get(c, sec, "sync-branch");
    char *fe[] = {EUPDATES_GIT, "-C",
                  cur,          "fetch",
                  "--quiet",    "--no-tags",
                  "--depth",    "1",
                  (char *)uri,  (char *)(branch && *branch ? branch : "HEAD"),
                  NULL};
    if (run_quiet(fe) != 0) {
        fprintf(stderr, "eupdates: %s: git fetch from %s failed\n", sec, uri);
        return 1;
    }

    if (conf_bool(c, sec, "sync-git-verify-commit-signature", 0)) {
        char *va[16] = {EUPDATES_GEMATO, "gpg-wrap"};
        int n = 2, k = key_args(c, sec, va + n);
        if (k < 0) return 1;
        n += k;
        char *tail[] = {"--", EUPDATES_GIT, "-C", cur, "verify-commit", "FETCH_HEAD", NULL};
        for (int i = 0; tail[i]; i++)
            va[n++] = tail[i];
        va[n] = NULL;
        if (run_quiet(va) != 0) {
            fprintf(stderr,
                    "eupdates: %s: commit signature check failed; keeping old copy\n", sec);
            return 1;
        }
    }

    char *re[] = {EUPDATES_GIT, "-C",     cur,          "reset",
                  "--quiet",    "--hard", "FETCH_HEAD", NULL};
    return run_quiet(re) != 0;
}

/* Bring our copies up to date where the system's are stale: at most once a
 * day per repository, an hour apart after a failure. */
static void sync_repos(const struct conf *c) {
    if (!sync_on) return;
    char dir[4096];
    cache_path(dir, sizeof dir, "repos");
    mkdir(dir, 0755);
    time_t now = time(NULL);
    const char *sec;
    for (size_t i = 0; (sec = conf_repo(c, i)) != NULL; i++) {
        enum sync_kind kind = sync_kind(c, sec);
        if (kind == SYNC_NONE) continue;
        time_t sys = repo_synced(conf_get(c, sec, "location"));
        if (sys && difftime(now, sys) < EUPDATES_SYSTEM_FRESH) continue;

        char synced[4096], tried[4096];
        copy_path(synced, sizeof synced, sec, ".synced");
        copy_path(tried, sizeof tried, sec, ".tried");
        time_t ms = mtime_of(synced), mt = mtime_of(tried);
        if (ms && difftime(now, ms) < SYNC_EVERY - SYNC_SLACK) continue;
        if (mt && difftime(now, mt) < SYNC_RETRY) continue; /* failed lately */

        touch(tried);
        int rc = kind == SYNC_RSYNC ? sync_rsync(c, sec) : sync_git(c, sec);
        if (rc == 0) {
            /* Stamp the start, not the end: a sync that took ten minutes
             * must not push tomorrow's past the next daily check. */
            touch(synced);
            struct timespec ts[2] = {{.tv_sec = now}, {.tv_sec = now}};
            utimensat(AT_FDCWD, synced, ts, 0);
            unlink(tried); /* it only marks a failure */
        }
    }
}

/* Make portage read our copies where they are newer: a private config root
 * whose etc/portage links every entry of the real one, plus a repos.conf
 * that re-points those repositories and a make.profile moved along with the
 * main tree. Returns 1 if any repository uses our copy (and sets
 * PORTAGE_CONFIGROOT for emerge and glsa-check), 0 otherwise. */
static int shadow_config(const struct conf *c) {
    char root[4096], etc[4096 + 32], rc[4096 + 64], p[4096 + 256], q[4096 + 256];
    cache_path(root, sizeof root, "root");
    rm_tree(root);

    char paths[32][4096];
    const char *names[32];
    size_t n = 0;
    const char *sec;
    for (size_t i = 0; (sec = conf_repo(c, i)) != NULL && n < 32; i++)
        if (use_copy(c, sec, paths[n], sizeof paths[n])) names[n++] = sec;
    if (n == 0) return 0;

    snprintf(etc, sizeof etc, "%s/etc", root);
    mkdir(root, 0755);
    mkdir(etc, 0755);
    snprintf(etc, sizeof etc, "%s/etc/portage", root);
    snprintf(rc, sizeof rc, "%s/repos.conf", etc);
    if (mkdir(etc, 0755) != 0 || mkdir(rc, 0755) != 0) return 0;

    /* Half a config would make emerge resolve against the wrong settings, so
     * any failure drops the whole shadow and counts against the system tree. */
    int bad = 0;
    DIR *d = opendir(EUPDATES_PORTAGE_DIR);
    if (!d) bad = 1;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, "repos.conf") == 0 || strcmp(e->d_name, "make.profile") == 0)
            continue;
        bad |= !join(p, sizeof p, EUPDATES_PORTAGE_DIR, e->d_name) ||
               !join(q, sizeof q, etc, e->d_name) || symlink(p, q) != 0;
    }
    if (d) closedir(d);

    /* The user's repos.conf first, then ours: a later location wins. */
    struct stat st;
    bad |= !join(p, sizeof p, EUPDATES_PORTAGE_DIR, "repos.conf") ||
           !join(q, sizeof q, rc, "a-system") ||
           (stat(p, &st) == 0 && symlink(p, q) != 0) ||
           !join(q, sizeof q, rc, "b-eupdates.conf");
    FILE *fp = bad ? NULL : fopen(q, "w");
    if (fp) {
        for (size_t i = 0; i < n; i++)
            fprintf(fp, "[%s]\nlocation = %s\n\n", names[i], paths[i]);
        bad |= fclose(fp) != 0;
    } else {
        bad = 1;
    }

    /* The profile lives in a repository; follow it into our copy. */
    char *real = bad || !join(p, sizeof p, EUPDATES_PORTAGE_DIR, "make.profile")
                     ? NULL
                     : realpath(p, NULL);
    if (real) {
        bad |= !join(q, sizeof q, real, "");
        for (size_t i = 0; i < n; i++) {
            char *loc = realpath(conf_get(c, names[i], "location"), NULL);
            size_t ll = loc ? strlen(loc) : 0;
            if (loc && strncmp(real, loc, ll) == 0 && real[ll] == '/')
                bad |= !join(q, sizeof q, paths[i], real + ll + 1);
            free(loc);
        }
        free(real);
        bad |= !join(p, sizeof p, etc, "make.profile") || symlink(q, p) != 0;
    }

    if (bad) {
        fputs("eupdates: cannot set up the config for our copies; using the system tree\n",
              stderr);
        rm_tree(root);
        return 0;
    }
    setenv("PORTAGE_CONFIGROOT", root, 1);
    return 1;
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

    /* Sync our own copies where nobody synced the system's, then point
     * portage at whichever copy of each repository is newer. */
    struct conf c;
    load_repos(&c);
    sync_repos(&c);
    shadow_config(&c);
    conf_free(&c);

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

    struct conf c;
    load_repos(&c);
    time_t synced = repos_synced(&c);
    conf_free(&c);

    int due = last == 0 || synced > last || mtime_of(EUPDATES_WORLD) > last ||
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
        fprintf(tb,
                sync_on ? "\nportage tree is %d days old \xe2\x80\x94 syncing it failed"
                          " (run `eupdates check` to see why)"
                        : "\nportage tree synced %d days ago \xe2\x80\x94 sync to refresh",
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
            free(last);
            last = line;
            if (fflush(stdout) != 0) break; /* the bar went away */
        } else {
            free(line);
        }
        if (reader_gone()) break; /* bar reloaded: don't linger until next change */
        sleep((unsigned)WATCH_TICK);
    }
    free(last);
    return 0;
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

/* Did the last count come from one of our copies? Then the system tree is
 * older than what we counted, and the upgrade has to sync it first. */
static int counted_from_copy(void) {
    struct conf c;
    load_repos(&c);
    char path[4096];
    const char *sec;
    int any = 0;
    for (size_t i = 0; !any && (sec = conf_repo(&c, i)) != NULL; i++)
        any = use_copy(&c, sec, path, sizeof path);
    conf_free(&c);
    return any;
}

static int cmd_upgrade(int notify) {
    const char *term = getenv("TERMINAL");
    if (!term || !*term) term = "xterm";
    const char *script =
        counted_from_copy()
            ? "sudo emerge --sync && "
              "sudo emerge --ask --verbose --update --deep --newuse @world; "
              "printf '\\nDone \\xe2\\x80\\x94 press enter to close.'; read _"
            : "sudo emerge --ask --verbose --update --deep --newuse @world; "
              "printf '\\nDone \\xe2\\x80\\x94 press enter to close.'; read _";
    pid_t pid = fork();
    if (pid == 0) {
        execlp(term, term, "-e", "sh", "-c", script, (char *)NULL);
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
            "Keeps its own synced copy of stale repositories (at most daily).\n"
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
            "  --no-sync      never sync; count against the system tree as it is\n"
            "  --notify       desktop-notify when the pending count changes\n"
            "  --no-notify    suppress notifications (overrides EUPDATES_NOTIFY)\n"
            "  -h, --help     show this help and exit\n"
            "  -V, --version  show version and exit\n"
            "\n"
            "Environment:\n"
            "  EUPDATES_AUTO      0 = same as --no-auto\n"
            "  EUPDATES_MAX_AGE   re-check at least every N hours (default: 24)\n"
            "  EUPDATES_SYNC      0 = same as --no-sync\n"
            "  EUPDATES_NOTIFY    notify on change when non-empty and not \"0\"\n"
            "  EUPDATES_ICON      notification icon: theme name or image path\n"
            "  TERMINAL           terminal for `upgrade` (default: xterm)\n"
            "  XDG_CACHE_HOME     cache location (default: ~/.cache/eupdates)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *cmd = NULL, *fmt = DEFAULT_FORMAT;
    int json = 0, no_auto = 0, no_sync = 0, cli_notify = 0, cli_no_notify = 0;
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
        } else if (strcmp(a, "--no-sync") == 0) {
            no_sync = 1;
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
    sync_on = !no_sync && env_flag("EUPDATES_SYNC", 1);
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
