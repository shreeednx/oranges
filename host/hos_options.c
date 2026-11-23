/* The player's settings: oranges_options.ini, what the settings menu (hos_menu.c) changes. Lines of "key=value",
   '#' comments; read once at the start, and the whole file written again at each change (to a temporary file beside
   it, then renamed over it: a crash in the write leaves the old file whole). Lines it does not know are kept as they
   are, in their order, so a file from a newer build loses nothing.
   Where it is: HOS_OPTIONS_FILE=<path> (a test or scripted run brings its own and leaves the player's alone), else
   $XDG_CONFIG_HOME/oranges/oranges_options.ini, else ~/.config/oranges/oranges_options.ini (the folder made at the first
   write). No file: every setting at its default.
   Built alone by tests/test_host_options.py (only hos_log from the rest of host/). */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hos_options.h"

void hos_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define MAX_LINES 512
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char path[1024];
static char *lines[MAX_LINES];   /* the file's lines as read or set, without their newline */
static int nlines, loaded;

static char *dup_str(const char *s) {
    const size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void choose_path(void) {
    const char *own = getenv("HOS_OPTIONS_FILE"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (own && *own) snprintf(path, sizeof path, "%s", own);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/oranges/oranges_options.ini", xdg);
    else if (home && *home) snprintf(path, sizeof path, "%s/.config/oranges/oranges_options.ini", home);
    else snprintf(path, sizeof path, "oranges_options.ini");
}

/* the line's key (up to '=', spaces trimmed) is key: its value (spaces trimmed), else NULL */
static const char *value_of(const char *line, const char *key, size_t *len) {
    while (*line == ' ' || *line == '\t') line++;
    const size_t k = strlen(key);
    if (strncmp(line, key, k)) return NULL;
    const char *p = line + k;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) n--;
    *len = n;
    return p;
}

static void load_locked(void) {
    if (loaded) return;
    loaded = 1;
    choose_path();
    FILE *f = fopen(path, "r");
    if (!f) {
        hos_log("options: %s (none yet: the defaults)", path);
        return;
    }
    char line[1024];
    while (nlines < MAX_LINES && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        lines[nlines] = dup_str(line);
        if (lines[nlines]) nlines++;
    }
    fclose(f);
    hos_log("options: %s (%d lines)", path, nlines);
}

void hos_options_load(void) {
    pthread_mutex_lock(&lock);
    load_locked();
    pthread_mutex_unlock(&lock);
}

const char *hos_options_path(void) {
    hos_options_load();
    return path;
}

int hos_option(const char *key, char *out, size_t size) {
    pthread_mutex_lock(&lock);
    load_locked();
    int found = 0;
    for (int i = nlines - 1; i >= 0 && !found; i--) {   /* (the last one wins, as a person editing it would expect) */
        size_t n;
        const char *v = lines[i][0] == '#' ? NULL : value_of(lines[i], key, &n);
        if (!v) continue;
        if (size) snprintf(out, size, "%.*s", (int)n, v);
        found = 1;
    }
    pthread_mutex_unlock(&lock);
    return found;
}

int hos_option_int(const char *key, int fallback) {
    char v[64];
    if (!hos_option(key, v, sizeof v)) return fallback;
    char *end;
    const long n = strtol(v, &end, 10);
    return end == v ? fallback : (int)n;
}

static void make_parent(const char *file) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", file);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(dir, 0755) && errno != EEXIST) return;
            *p = '/';
        }
}

static void write_locked(void) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    make_parent(path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        static int logged;
        if (!logged++) hos_log("options: %s cannot be written", tmp);
        return;
    }
    for (int i = 0; i < nlines; i++) fprintf(f, "%s\n", lines[i]);
    const int bad = fflush(f) != 0;
    fclose(f);
    if (bad || rename(tmp, path)) hos_log("options: %s cannot be written", path);
}

void hos_option_set(const char *key, const char *value) {
    pthread_mutex_lock(&lock);
    load_locked();
    char line[1024];
    snprintf(line, sizeof line, "%s=%s", key, value);
    int at = -1;
    for (int i = 0; i < nlines; i++) {
        size_t n;
        if (lines[i][0] != '#' && value_of(lines[i], key, &n)) {
            if (at < 0) {
                at = i;
            } else {   /* (a key set twice by hand: one line left) */
                free(lines[i]);
                memmove(lines + i, lines + i + 1, (size_t)(nlines - i - 1) * sizeof *lines);
                nlines--, i--;
            }
        }
    }
    if (at >= 0 && !strcmp(lines[at], line)) {   /* (unchanged) */
        pthread_mutex_unlock(&lock);
        return;
    }
    if (at < 0 && nlines == 0) lines[nlines++] = dup_str("# oranges settings (the F1 menu writes this file)");
    char *d = dup_str(line);
    if (d && at >= 0) {
        free(lines[at]);
        lines[at] = d;
    } else if (d && nlines < MAX_LINES) {
        lines[nlines++] = d;
    } else {
        free(d);
    }
    write_locked();
    pthread_mutex_unlock(&lock);
}

void hos_option_unset(const char *key) {
    pthread_mutex_lock(&lock);
    load_locked();
    int removed = 0;
    for (int i = 0; i < nlines; i++) {
        size_t n;
        if (lines[i][0] != '#' && value_of(lines[i], key, &n)) {
            free(lines[i]);
            memmove(lines + i, lines + i + 1, (size_t)(nlines - i - 1) * sizeof *lines);
            nlines--, i--, removed = 1;
        }
    }
    if (removed) write_locked();
    pthread_mutex_unlock(&lock);
}
