// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * eupdates - count pending Gentoo (portage) package updates for status bars.
 *
 *   check    resolve the @world upgrade set, cache the list, signal waybar
 *   status   emit waybar JSON from the cache (default; no work, instant)
 *   upgrade  run the upgrade in $TERMINAL, then re-check
 *
 * It NEVER contacts a mirror. `emerge --pretend` resolves against the
 * already-synced local tree + installed db only. The only rate-limited,
 * mirror-hitting operation on Gentoo is `emerge --sync`, which is deliberately
 * left out. The tooltip reports how stale the tree is so a sync stays a
 * deliberate act.
 *
 * Copyright 2026 Josh Reed. GNU GPL v3 or later; see LICENSE.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef EUPDATES_VERSION
#define EUPDATES_VERSION "0.1.0"
#endif

/* Offline: resolves the local tree only, never fetches. */
static const char *EMERGE_CMD =
    "emerge --pretend --update --deep --newuse --color=n --nospinner @world 2>/dev/null";
/* Offline: reads the local GLSA metadata shipped with the tree. */
static const char *GLSA_CMD = "glsa-check --test affected 2>/dev/null";
static const char *TREE_TS = "/var/db/repos/gentoo/metadata/timestamp.chk";

static const char *ICON = "\xf3\xb0\x8f\x97";       /* U+F03D7 nf-md-package_up */
static const char *CHECK_ICON = "\xf3\xb0\x91\x90"; /* U+F0450 nf-md-refresh   */
static const int CHECKING_STALE = 300; /* drop a stuck "checking" marker past this (s) */
static const int PREVIEW = 25;         /* tooltip package lines before eliding  */
static const int STALE_DAYS = 2;       /* nag about syncing past this age       */

/* waybar "signal": N maps to SIGRTMIN+N; overridable for other setups. */
static int waybar_signal(void) {
    const char *e = getenv("EUPDATES_WAYBAR_SIGNAL");
    if (e && *e) {
        char *end;
        long v = strtol(e, &end, 10);
        if (*end == '\0' && v >= 1 && v <= 15) return (int)v;
    }
    return 8;
}

/* ---- cache paths --------------------------------------------------------- */

static void cache_path(char *buf, size_t n, const char *leaf) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/eupdates/%s", xdg, leaf);
    else
        snprintf(buf, n, "%s/.cache/eupdates/%s", home ? home : ".", leaf);
}

static void ensure_cache_dir(void) {
    char dir[512];
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

/* ---- JSON string emission ------------------------------------------------ */

static void json_escape(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 0x20)
                printf("\\u%04x", c); /* other control chars   */
            else
                putchar(c); /* UTF-8 bytes pass thru */
        }
    }
}

static void emit(const char *text, const char *tooltip, const char *cls) {
    fputs("{\"text\":\"", stdout);
    json_escape(text);
    fputs("\",\"tooltip\":\"", stdout);
    json_escape(tooltip);
    printf("\",\"class\":\"%s\"}\n", cls);
}

/* ---- check --------------------------------------------------------------- */

/* Write one tooltip line for an `[ebuild ...] atom [old] ...` merge line.
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

    const char *ob = NULL, *oe = NULL; /* [oldver] if present  */
    for (const char *q = ae; *q && *q != '\n'; q++) {
        if (strncmp(q, "USE=", 4) == 0) break; /* past the version cols */
        if (*q == '[') {
            const char *r = q + 1, *re = q + 1;
            while (*re && !isspace((unsigned char)*re) && *re != ':' && *re != ']')
                re++;
            ob = r;
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

static int count_glsa(void) {
    FILE *fp = popen(GLSA_CMD, "r");
    if (!fp) return 0;
    int n = 0;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1)
        if (isdigit((unsigned char)line[0])) n++; /* affected GLSA ids */
    free(line);
    pclose(fp);
    return n;
}

static void signal_waybar(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char sig[16];
        snprintf(sig, sizeof sig, "-RTMIN+%d", waybar_signal());
        execlp("pkill", "pkill", "-x", sig, "waybar", (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0); /* absent waybar -> nonzero, harmless */
    }
}

static int count_lines(const char *path, time_t *mtime); /* defined below */

/* Best-effort desktop notification; silently a no-op if notify-send is absent.
 * The icon is an XDG icon-theme name (resolved by the notification daemon) or an
 * absolute image path; override with EUPDATES_ICON. */
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

static int notify_enabled(int cli_notify, int cli_no_notify) {
    if (cli_no_notify) return 0;
    if (cli_notify) return 1;
    const char *e = getenv("EUPDATES_NOTIFY");
    return e && *e && strcmp(e, "0") != 0;
}

/* Compare old vs new pending count and notify per the rules: on any change, and
 * additionally (for a manual check) confirm even when nothing changed. */
static void notify_result(int old, int now, int manual) {
    char body[128];
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

static int cmd_check(int progress, int notify) {
    ensure_cache_dir();
    char list[512], tmp[512], sec[512], mark[512], tmpleaf[64];
    cache_path(list, sizeof list, "list");
    cache_path(sec, sizeof sec, "security");
    cache_path(mark, sizeof mark, "checking");
    snprintf(tmpleaf, sizeof tmpleaf, "list.%ld.tmp", (long)getpid());
    cache_path(tmp, sizeof tmp, tmpleaf);

    int old = count_lines(list, NULL); /* -1 if no prior cache */

    if (progress) {
        FILE *mf = fopen(mark, "w");
        if (mf) fclose(mf);
        signal_waybar(); /* show the "checking" state while we resolve */
    }

    FILE *out = fopen(tmp, "w");
    if (!out) {
        fprintf(stderr, "eupdates: %s: %s\n", tmp, strerror(errno));
        if (progress) {
            unlink(mark);
            signal_waybar();
        }
        return 1;
    }
    FILE *fp = popen(EMERGE_CMD, "r");
    if (!fp) {
        fprintf(stderr, "eupdates: cannot run emerge\n");
        fclose(out);
        unlink(tmp);
        if (progress) {
            unlink(mark);
            signal_waybar();
        }
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    int n = 0;
    while (getline(&line, &cap, fp) != -1)
        if (strncmp(line, "[ebuild", 7) == 0 || strncmp(line, "[binary", 7) == 0)
            n += format_pkg(line, out);
    free(line);
    int rc = pclose(fp);
    fclose(out);

    /* emerge failed (blockers, broken tree): keep the stale cache. */
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        fprintf(stderr, "eupdates: emerge exited abnormally; keeping cache\n");
        unlink(tmp);
        if (progress) {
            unlink(mark);
            signal_waybar();
        }
        return 1;
    }
    if (rename(tmp, list) != 0) {
        fprintf(stderr, "eupdates: rename: %s\n", strerror(errno));
        unlink(tmp);
        if (progress) {
            unlink(mark);
            signal_waybar();
        }
        return 1;
    }

    FILE *sf = fopen(sec, "w");
    if (sf) {
        fprintf(sf, "%d\n", count_glsa());
        fclose(sf);
    }

    if (progress) unlink(mark);
    signal_waybar();

    if (notify) notify_result(old, n, progress);
    return 0;
}

/* ---- status -------------------------------------------------------------- */

static int count_lines(const char *path, time_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (mtime) *mtime = st.st_mtime;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int n = 0, c;
    int prev = '\n';
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') n++;
        prev = c;
    }
    if (prev != '\n' && n == 0) n = 1; /* trailing line without newline */
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

static int tree_age_days(void) {
    struct stat st;
    if (stat(TREE_TS, &st) != 0) return -1;
    double d = difftime(time(NULL), st.st_mtime) / 86400.0;
    return d < 0 ? 0 : (int)d;
}

static int cmd_status(void) {
    char list[512], sec[512], mark[512];
    cache_path(list, sizeof list, "list");
    cache_path(sec, sizeof sec, "security");
    cache_path(mark, sizeof mark, "checking");

    /* A manual `check --progress` is in flight: show a checking state. */
    struct stat mst;
    if (stat(mark, &mst) == 0) {
        if (difftime(time(NULL), mst.st_mtime) < CHECKING_STALE) {
            char text[64];
            snprintf(text, sizeof text, "%s ", CHECK_ICON);
            emit(text, "Checking for updates\xe2\x80\xa6", "checking");
            return 0;
        }
        unlink(mark); /* stale: a check died mid-flight */
    }

    time_t mtime = 0;
    int count = count_lines(list, &mtime);
    if (count < 0) {
        emit("", "No update check has run yet", "unknown");
        return 0;
    }

    char checked[64] = "";
    struct tm tmv;
    if (localtime_r(&mtime, &tmv))
        strftime(checked, sizeof checked, "%a %b %d %I:%M %p", &tmv);

    int glsa = read_int_file(sec);
    int age = tree_age_days();

    char *tip = NULL;
    size_t tipn = 0;
    FILE *tb = open_memstream(&tip, &tipn);

    if (count == 0)
        fprintf(tb, "System up to date \xe2\x80\xa2 checked %s", checked);
    else
        fprintf(tb, "%d update%s pending \xe2\x80\xa2 checked %s", count,
                count == 1 ? "" : "s", checked);

    if (glsa > 0)
        fprintf(tb, "\n\xef\x80\xa3 %d unresolved GLSA%s (run glsa-check)", glsa,
                glsa == 1 ? "" : "s");
    if (age >= STALE_DAYS)
        fprintf(
            tb,
            "\n\xef\x80\x97 portage tree synced %d day%s ago \xe2\x80\x94 sync to refresh",
            age, age == 1 ? "" : "s");

    if (count > 0) {
        fputs("\n\n", tb);
        FILE *lf = fopen(list, "r");
        if (lf) {
            char *line = NULL;
            size_t cap = 0;
            int shown = 0;
            while (getline(&line, &cap, lf) != -1) {
                if (shown++ >= PREVIEW) break;
                fputs("  ", tb);
                fputs(line, tb);
            }
            free(line);
            fclose(lf);
            if (count > PREVIEW)
                fprintf(tb,
                        "  \xe2\x80\xa6"
                        "and %d more",
                        count - PREVIEW);
        }
    }
    fclose(tb);

    if (count == 0) {
        emit("", tip, "updated");
    } else {
        char text[64];
        snprintf(text, sizeof text, "%s %d", ICON, count);
        emit(text, tip, "pending");
    }
    free(tip);
    return 0;
}

/* ---- upgrade ------------------------------------------------------------- */

static int cmd_upgrade(int progress, int notify) {
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
    return cmd_check(progress, notify);
}

/* ---- main ---------------------------------------------------------------- */

static void usage(FILE *out, const char *prog) {
    fprintf(out,
            "Usage: %s [command]\n"
            "\n"
            "Count pending Gentoo (portage) @world updates for status bars.\n"
            "Resolves the already-synced local tree only; never contacts a mirror.\n"
            "\n"
            "Commands:\n"
            "  status    emit waybar JSON from the cache (default; instant)\n"
            "  check     resolve @world, refresh the cache, signal waybar\n"
            "  upgrade   run the upgrade in $TERMINAL, then re-check\n"
            "\n"
            "Options (for check / upgrade):\n"
            "  --progress     show a \"checking\" state in the bar while resolving\n"
            "  --notify       desktop-notify when the pending count changes\n"
            "  --no-notify    suppress notifications (overrides EUPDATES_NOTIFY)\n"
            "  -h, --help     show this help and exit\n"
            "  -V, --version  show version and exit\n"
            "\n"
            "Environment:\n"
            "  EUPDATES_NOTIFY          notify on change when non-empty and not \"0\"\n"
            "  EUPDATES_ICON            notification icon: theme name or image path\n"
            "  TERMINAL                 terminal for `upgrade` (default: xterm)\n"
            "  EUPDATES_WAYBAR_SIGNAL   SIGRTMIN+N waybar refresh signal (default: 8)\n"
            "  XDG_CACHE_HOME           cache location (default: ~/.cache/eupdates)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    int progress = 0, cli_notify = 0, cli_no_notify = 0;
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
        if (strcmp(a, "--progress") == 0) {
            progress = 1;
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

    int notify = notify_enabled(cli_notify, cli_no_notify);
    if (strcmp(cmd, "status") == 0) return cmd_status();
    if (strcmp(cmd, "check") == 0) return cmd_check(progress, notify);
    if (strcmp(cmd, "upgrade") == 0) return cmd_upgrade(progress, notify);
    fprintf(stderr, "eupdates: unknown command '%s'\n", cmd);
    usage(stderr, argv[0]);
    return 2;
}
