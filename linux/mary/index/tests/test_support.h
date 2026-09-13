/* A temporary home for the index tests. */
#ifndef INDEX_TEST_SUPPORT_H
#define INDEX_TEST_SUPPORT_H

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mary_test.h"

static char home[64];

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

static void make_home(void) {
    snprintf(home, sizeof home, "/tmp/index-home-XXXXXX");
    MARY_ASSERT(mkdtemp(home) != NULL);
}

static void put_file(const char *relative, const char *content) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", home, relative);
    for (char *p = path + strlen(home) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0700);
            *p = '/';
        }
    }
    FILE *f = fopen(path, "wb");
    MARY_ASSERT(f != NULL);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

#endif
