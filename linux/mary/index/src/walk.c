#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "index/walk.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool ix_walk_admits(const char *root, const char *path, int64_t size) {
    if (size > (int64_t)IX_FILE_MAX) return false;
    size_t rl = strlen(root);
    if (strncmp(path, root, rl) != 0 || path[rl] != '/') return false;
    for (const char *p = path + rl + 1; *p;) {
        if (*p == '.') return false;                /* a dotted component */
        const char *slash = strchr(p, '/');
        if (!slash) break;
        p = slash + 1;
    }
    return true;
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static long walk_dir(const char *dir, dev_t device, ix_walk_fn fn, void *user, bool *stopped) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, name_cmp);
    long seen = 0;
    for (size_t i = 0; i < n && !*stopped; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        struct stat st;
        if (lstat(path, &st) != 0 || st.st_dev != device) continue;
        if (S_ISDIR(st.st_mode)) {
            seen += walk_dir(path, device, fn, user, stopped);
        } else if (S_ISREG(st.st_mode) && st.st_size <= (off_t)IX_FILE_MAX) {
            seen++;
#if defined(__APPLE__)
            int64_t mtime_ms = (int64_t)st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
            int64_t mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
            if (fn(path, (int64_t)st.st_size, mtime_ms, user)) *stopped = true;
        }
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return seen;
}

long ix_walk(const char *root, ix_walk_fn fn, void *user) {
    struct stat st;
    if (stat(root, &st) != 0) return -errno;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    bool stopped = false;
    return walk_dir(root, st.st_dev, fn, user, &stopped);
}
