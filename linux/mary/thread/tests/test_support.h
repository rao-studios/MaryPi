/* Shared by the store tests: a temporary store, and a scripted embedder whose vectors
 * are a bag of hashed words (texts that share words land close), with a scripted
 * extractor and switchable failures. */
#ifndef THREAD_TEST_SUPPORT_H
#define THREAD_TEST_SUPPORT_H

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/json.h"
#include "mary_test.h"
#include "thread/db.h"
#include "thread/store.h"

#define TEST_DIM 64

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

static int fake_embed_calls, fake_embed_texts, fake_extract_calls;
static int fake_embed_rc;                 /* 0, or the errno every embed answers */
static const char *fake_extract_answer;   /* NULL: an extractor that fails with -EIO */

static void bag_of_words(const char *text, float *out) {
    for (int i = 0; i < TEST_DIM; i++) out[i] = 0;
    char *copy = strdup(text);
    for (char *p = copy; *p; p++) *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    for (char *tok = strtok(copy, " \t\n.,;:!?'\""); tok; tok = strtok(NULL, " \t\n.,;:!?'\"")) {
        unsigned h = 2166136261u;
        for (const char *c = tok; *c; c++) { h ^= (unsigned char)*c; h *= 16777619u; }
        out[h % TEST_DIM] += 1;
    }
    free(copy);
    float norm = 0;
    for (int i = 0; i < TEST_DIM; i++) norm += out[i] * out[i];
    norm = sqrtf(norm);
    if (norm > 0) for (int i = 0; i < TEST_DIM; i++) out[i] /= norm;
}

static int fake_embed(const char *const *texts, size_t n, float *out, size_t dim, char *message, size_t cap, void *user) {
    fake_embed_calls++;
    fake_embed_texts += (int)n;
    if (fake_embed_rc) {
        snprintf(message, cap, "scripted failure");
        return fake_embed_rc;
    }
    for (size_t i = 0; i < n; i++) bag_of_words(texts[i], out + i * dim);
    return 0;
}

static int fake_extract(const char *const *texts, size_t n, const char *prompt, char **json_out, char *message, size_t cap, void *user) {
    fake_extract_calls++;
    if (!fake_extract_answer) {
        snprintf(message, cap, "the model is away");
        return -EIO;
    }
    *json_out = strdup(fake_extract_answer);
    return 0;
}

static const thread_embedder FAKE = { fake_embed, fake_extract, NULL, TEST_DIM, 64 };

static char test_dir[64];

static thread_store *open_store(bool with_embedder) {
    snprintf(test_dir, sizeof test_dir, "/tmp/thread-test-XXXXXX");
    MARY_ASSERT(mkdtemp(test_dir) != NULL);
    thread_store_options o = { .dir = test_dir, .embedder = with_embedder ? &FAKE : NULL, .worker = false, .dim = TEST_DIM };
    int error = 0;
    thread_store *s = thread_store_open_with(&o, &error);
    MARY_ASSERT(s != NULL);
    return s;
}

static void close_store(thread_store *s) {
    thread_store_close(s);
    remove_tree(test_dir);
}

static int deposit_text(thread_store *s, const char *owner, const char *group, const char *id, const char *text, const char *entity, const char *kind) {
    const char *texts[1] = { text };
    thread_graph_payload p = { 0 };
    if (entity) thread_payload_add_entity(&p, entity, kind ? kind : "concept");
    thread_deposit d = { .document_id = id, .group_id = group, .texts = texts, .n_texts = 1, .name = id, .graph = entity ? &p : NULL, .source = "threadctl" };
    int rc = thread_store_deposit(s, owner, &d, NULL, NULL);
    thread_payload_free(&p);
    return rc;
}

#endif
