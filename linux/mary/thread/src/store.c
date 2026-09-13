#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/io.h"
#include "common/json.h"
#include "common/log.h"
#include "foundation/hash.h"
#include "thread/families.h"
#include "thread/ledger.h"
#include "thread/text.h"

#define LEGACY_FILE_MAX (8u << 20)

/* MARK: - ids and small helpers */

bool thread_id_valid(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n == 0 || n > THREAD_ID_MAX) return false;
    const unsigned char *p = (const unsigned char *)id;
    while (*p) {
        if (*p < 0x20 || *p == 0x7F || *p == '/' || thread_space_len(p)) return false;
        if (*p < 0x80) { p++; continue; }
        /* a well-formed UTF-8 sequence */
        int len = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (!len) return false;
        for (int i = 1; i < len; i++) if ((p[i] & 0xC0) != 0x80) return false;
        p += len;
    }
    return true;
}

sqlite3 *thread_store_db(thread_store *s) { return s->db; }
void thread_store_lock(thread_store *s) { pthread_mutex_lock(&s->lock); }
void thread_store_unlock(thread_store *s) { pthread_mutex_unlock(&s->lock); }
const char *thread_store_node_id(const thread_store *s) { return s->node_id; }

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
        mc_log(MC_LOG_ERROR, "store: %s: %s", sql, sqlite3_errmsg(db));
        return -EIO;
    }
    return 0;
}

static int run(sqlite3 *db, sqlite3_stmt *stmt, const char *what) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        mc_log(MC_LOG_ERROR, "store: %s: %s", what, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -EIO;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int exec2(sqlite3 *db, const char *sql, const char *a1, const char *a2) {
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(db, sql, &stmt);
    if (rc) return rc;
    if (a1) thread_db_bind_text(stmt, 1, a1);
    if (a2) thread_db_bind_text(stmt, 2, a2);
    return run(db, stmt, sql);
}

static void add_text_col(struct json_object *o, const char *key, sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) json_object_object_add(o, key, NULL);
    else json_object_object_add(o, key, json_object_new_string((const char *)sqlite3_column_text(stmt, col)));
}

bool thread_store_owns(thread_store *s, const char *owner, const char *document_id) {
    char *o = thread_db_text(s->db, "SELECT owner FROM documents WHERE id = ?", document_id, NULL);
    bool owns = o && strcmp(o, owner) == 0;
    free(o);
    return owns;
}

int64_t thread_store_revision(thread_store *s, const char *document_id) {
    return thread_db_int(s->db, "SELECT revision FROM documents WHERE id = ?", document_id, NULL);
}

void thread_store_log(thread_store *s, const char *kind, const char *source, const char *document_id, const char *group_id,
                      const char *request_id, int64_t count, int64_t ms, struct json_object *detail) {
    thread_ledger_row row = { kind, source, document_id, group_id, request_id, count, ms, detail };
    thread_ledger_record(s->db, &row);
}

struct json_object *thread_store_partitions_json(thread_store *s, const char *document_id) {
    struct json_object *arr = json_object_new_array();
    sqlite3_stmt *stmt = NULL;
    if (prepare(s->db, "SELECT rowid, seq, text, embedding IS NOT NULL, id FROM partitions WHERE document_id = ? ORDER BY seq", &stmt)) return arr;
    thread_db_bind_text(stmt, 1, document_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        struct json_object *p = json_object_new_object();
        json_object_object_add(p, "rowid", json_object_new_int64(sqlite3_column_int64(stmt, 0)));
        json_object_object_add(p, "seq", json_object_new_int(sqlite3_column_int(stmt, 1)));
        add_text_col(p, "text", stmt, 2);
        json_object_object_add(p, "embedded", json_object_new_boolean(sqlite3_column_int(stmt, 3)));
        add_text_col(p, "id", stmt, 4);
        json_object_array_add(arr, p);
    }
    sqlite3_finalize(stmt);
    return arr;
}

int thread_store_reload_vectors(thread_store *s, const char *document_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = prepare(s->db, "SELECT rowid, embedding FROM partitions WHERE document_id = ? AND embedding IS NOT NULL ORDER BY seq", &stmt);
    if (rc) return rc;
    thread_db_bind_text(stmt, 1, document_id);
    int64_t *rowids = NULL;
    float *vecs = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        float *e = thread_db_column_floats(stmt, 1, s->dim);
        if (!e) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            rowids = realloc(rowids, cap * sizeof *rowids);
            vecs = realloc(vecs, cap * s->dim * sizeof *vecs);
        }
        rowids[n] = sqlite3_column_int64(stmt, 0);
        memcpy(vecs + n * s->dim, e, s->dim * sizeof *vecs);
        free(e);
        n++;
    }
    sqlite3_finalize(stmt);
    if (n) rc = thread_vectors_put(s->vectors, document_id, rowids, vecs, n);
    else thread_vectors_drop(s->vectors, document_id);
    free(rowids);
    free(vecs);
    return rc;
}

/* MARK: - open */

static int new_uuid(char *out) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -errno;
    ssize_t got = read(fd, b, sizeof b);
    close(fd);
    if (got != (ssize_t)sizeof b) return -EIO;
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return 0;
}

static struct json_object *load_json(const char *path) {
    mc_buf b = { 0 };
    struct json_object *obj = mc_read_file(path, &b, LEGACY_FILE_MAX) == 0 ? mc_json_parse((const char *)b.data, b.len) : NULL;
    mc_buf_free(&b);
    return obj;
}

/* The JSON files threadd kept before thread.db, imported once. */
static int migrate_legacy(thread_store *s) {
    char groups_dir[600], docs_dir[600], path[700];
    snprintf(groups_dir, sizeof groups_dir, "%s/groups", s->dir);
    snprintf(docs_dir, sizeof docs_dir, "%s/documents", s->dir);
    struct stat st;
    if (stat(docs_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    if (thread_db_int(s->db, "SELECT COUNT(*) FROM documents", NULL, NULL) > 0) return 0;
    int rc = thread_db_begin(s->db);
    if (rc) return rc;
    size_t imported = 0;
    DIR *dir = opendir(groups_dir);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", groups_dir, e->d_name);
        struct json_object *g = load_json(path);
        const char *id = mc_json_string(g, "id"), *owner = mc_json_string(g, "owner_id"), *label = mc_json_string(g, "label");
        int64_t created = 0;
        mc_json_int64(g, "created_at", &created);
        if (id && owner && thread_id_valid(id)) {
            const char *family = thread_family_of("", id, NULL, 0);
            const char *target = id;
            char renamed[THREAD_ID_MAX + 1];
            if (strcmp(id, "mary-conversations") == 0) {
                snprintf(renamed, sizeof renamed, "conversation-%s", owner);
                target = renamed;
                family = "conversation";
            }
            (void)family;
            sqlite3_stmt *stmt = NULL;
            if (prepare(s->db, "INSERT OR IGNORE INTO groups(id, owner, label, created_at) VALUES(?, ?, ?, ?)", &stmt) == 0) {
                thread_db_bind_text(stmt, 1, target);
                thread_db_bind_text(stmt, 2, owner);
                thread_db_bind_text(stmt, 3, label ? label : "");
                sqlite3_bind_int64(stmt, 4, created);
                run(s->db, stmt, "group");
            }
        }
        json_object_put(g);
    }
    if (dir) closedir(dir);
    dir = opendir(docs_dir);
    while (dir && (e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", docs_dir, e->d_name);
        struct json_object *d = load_json(path);
        const char *id = mc_json_string(d, "id"), *owner = mc_json_string(d, "owner_id"), *group = mc_json_string(d, "group_id");
        struct json_object *texts = mc_json_array(d, "texts");
        if (!id || !owner || !thread_id_valid(id) || !texts) {
            json_object_put(d);
            continue;
        }
        char group_target[THREAD_ID_MAX + 1] = "";
        if (group && *group) snprintf(group_target, sizeof group_target, strcmp(group, "mary-conversations") == 0 ? "conversation-%s" : "%s", strcmp(group, "mary-conversations") == 0 ? owner : group);
        size_t meta_len = 0;
        const char *meta_b64 = mc_json_string(d, "metadata");
        unsigned char *meta = meta_b64 && *meta_b64 ? mc_base64_decode_alloc(meta_b64, strlen(meta_b64), &meta_len) : NULL;
        const char *family = thread_family_of(id, group_target, (const char *)meta, meta_len);
        int64_t created = 0;
        mc_json_int64(d, "created_at", &created);
        const char *name = mc_json_string(d, "name"), *media = mc_json_string(d, "media_type");
        size_t nt = json_object_array_length(texts);
        const char **tv = calloc(nt ? nt : 1, sizeof *tv);
        for (size_t t = 0; t < nt; t++) tv[t] = json_object_get_string(json_object_array_get_idx(texts, t));
        char hash[MF_NUMERIC_HASH_MAX];
        mc_buf joined = { 0 };
        for (size_t t = 0; t < nt; t++) {
            if (t) mc_buf_append_str(&joined, "\n");
            mc_buf_append_str(&joined, tv[t]);
        }
        mf_numeric_hash(joined.data ? joined.data : (const unsigned char *)"", joined.len, hash);
        mc_buf_free(&joined);
        sqlite3_stmt *stmt = NULL;
        if (prepare(s->db, "INSERT OR IGNORE INTO documents(id, owner, group_id, family, name, media_type, metadata, content_hash, created_at, updated_ms) "
                           "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", &stmt) == 0) {
            thread_db_bind_text(stmt, 1, id);
            thread_db_bind_text(stmt, 2, owner);
            thread_db_bind_text(stmt, 3, *group_target ? group_target : NULL);
            thread_db_bind_text(stmt, 4, family);
            thread_db_bind_text(stmt, 5, name ? name : "");
            thread_db_bind_text(stmt, 6, media && *media ? media : "text");
            if (meta) sqlite3_bind_blob(stmt, 7, meta, (int)meta_len, SQLITE_TRANSIENT);
            else sqlite3_bind_null(stmt, 7);
            thread_db_bind_text(stmt, 8, hash);
            sqlite3_bind_int64(stmt, 9, created);
            sqlite3_bind_int64(stmt, 10, mc_wall_ms());
            run(s->db, stmt, "document");
        }
        for (size_t t = 0; t < nt; t++) {
            if (prepare(s->db, "INSERT INTO partitions(document_id, seq, text) VALUES(?, ?, ?)", &stmt) == 0) {
                thread_db_bind_text(stmt, 1, id);
                sqlite3_bind_int(stmt, 2, (int)t);
                thread_db_bind_text(stmt, 3, tv[t]);
                run(s->db, stmt, "partition");
            }
        }
        /* keyword entities until extraction runs */
        thread_graph_payload payload = { 0 };
        char **tags = NULL;
        size_t ntags = thread_tags(tv, nt, &tags);
        for (size_t t = 0; t < ntags; t++) thread_payload_add_entity(&payload, tags[t], "concept");
        thread_graph_upsert(s->db, id, &payload, s->dim);
        thread_enrich_enqueue(s, id, 1, &payload, true);
        thread_payload_free(&payload);
        thread_strings_free(tags, ntags);
        struct json_object *detail = json_object_new_object();
        json_object_object_add(detail, "migrated", json_object_new_boolean(true));
        thread_store_log(s, "index", "threadd", id, *group_target ? group_target : NULL, NULL, (int64_t)nt, 0, detail);
        json_object_put(detail);
        free(tv);
        free(meta);
        json_object_put(d);
        imported++;
    }
    if (dir) closedir(dir);
    rc = thread_db_commit(s->db);
    if (rc) return rc;
    char legacy[600], target[700];
    snprintf(legacy, sizeof legacy, "%s/legacy", s->dir);
    mkdir(legacy, 0700);
    snprintf(target, sizeof target, "%s/legacy/groups", s->dir);
    rename(groups_dir, target);
    snprintf(target, sizeof target, "%s/legacy/documents", s->dir);
    rename(docs_dir, target);
    snprintf(path, sizeof path, "%s/node-id", s->dir);
    snprintf(target, sizeof target, "%s/legacy/node-id", s->dir);
    rename(path, target);
    mc_log(MC_LOG_NOTICE, "imported %zu legacy document(s) into thread.db", imported);
    return 0;
}

thread_store *thread_store_open_with(const thread_store_options *o, int *error) {
    thread_store *s = calloc(1, sizeof *s);
    if (!s) {
        if (error) *error = -ENOMEM;
        return NULL;
    }
    snprintf(s->dir, sizeof s->dir, "%s", o->dir && *o->dir ? o->dir : THREAD_STATE_DIR);
    snprintf(s->db_path, sizeof s->db_path, "%s/%s", s->dir, THREAD_DB_FILE);
    s->embedder = o->embedder;
    s->dim = o->dim ? o->dim : (o->embedder && o->embedder->dim ? o->embedder->dim : THREAD_EMBEDDING_DIM);
    s->worker_wanted = o->worker;
    thread_policy_default(&s->policy);
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->jobs_lock, NULL);
    pthread_cond_init(&s->jobs_cond, NULL);
    int rc = 0;
    if (mkdir(s->dir, 0700) < 0 && errno != EEXIST) rc = -errno;
    if (rc == 0) rc = thread_db_open(s->db_path, &s->db);
    if (rc == 0) {
        char *node = thread_db_meta_get(s->db, "node_id");
        if (node && strlen(node) == 36) snprintf(s->node_id, sizeof s->node_id, "%s", node);
        else {
            /* the legacy node-id file keeps its identity */
            char path[600];
            snprintf(path, sizeof path, "%s/node-id", s->dir);
            mc_buf b = { 0 };
            if (mc_read_file(path, &b, 64) == 0 && b.len >= 36) {
                memcpy(s->node_id, b.data, 36);
                s->node_id[36] = 0;
            } else if ((rc = new_uuid(s->node_id)) != 0) {
                mc_buf_free(&b);
                free(node);
                goto fail;
            }
            mc_buf_free(&b);
            rc = thread_db_meta_set(s->db, "node_id", s->node_id);
        }
        free(node);
        if (rc == 0) {
            char dim[16];
            snprintf(dim, sizeof dim, "%zu", s->dim);
            thread_db_meta_set(s->db, "embedding_model", THREAD_EMBEDDING_MODEL);
            thread_db_meta_set(s->db, "embedding_dim", dim);
            char *policy = thread_db_meta_get(s->db, "extraction_policy");
            if (policy) {
                thread_policy_from_json(policy, strlen(policy), &s->policy);
                free(policy);
            }
        }
    }
    if (rc == 0) rc = migrate_legacy(s);
    if (rc == 0) {
        s->vectors = thread_vectors_new(s->dim);
        rc = s->vectors ? thread_vectors_load(s->vectors, s->db) : -ENOMEM;
    }
    if (rc == 0 && s->worker_wanted) rc = thread_enrich_start(s);
fail:
    if (rc) {
        thread_store_close(s);
        if (error) *error = rc;
        return NULL;
    }
    return s;
}

thread_store *thread_store_open(const char *dir, int *error) {
    thread_store_options o = { .dir = dir };
    return thread_store_open_with(&o, error);
}

void thread_store_close(thread_store *s) {
    if (!s) return;
    thread_enrich_stop(s);
    for (int i = 0; i < THREAD_QUERY_CACHE; i++) {
        free(s->queries[i].text);
        free(s->queries[i].vector);
    }
    thread_vectors_free(s->vectors);
    if (s->db) {
        thread_db_checkpoint(s->db);
        thread_db_close(s->db);
    }
    thread_policy_free(&s->policy);
    pthread_cond_destroy(&s->jobs_cond);
    pthread_mutex_destroy(&s->jobs_lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

int thread_store_backup(thread_store *s, const char *path) {
    pthread_mutex_lock(&s->lock);
    int rc = thread_db_backup(s->db, path);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int thread_store_checkpoint(thread_store *s) {
    pthread_mutex_lock(&s->lock);
    int rc = thread_db_checkpoint(s->db);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

/* MARK: - deposit */

static bool group_owned_by_other(thread_store *s, const char *owner, const char *group_id) {
    char *o = thread_db_text(s->db, "SELECT owner FROM groups WHERE id = ?", group_id, NULL);
    bool other = o && strcmp(o, owner) != 0;
    free(o);
    return other;
}

int thread_store_deposit(thread_store *s, const char *owner, const thread_deposit *d, char document_id[THREAD_ID_MAX + 1], size_t *partitions) {
    if (partitions) *partitions = 0;
    int64_t started = mc_now_ms();
    /* the texts */
    char **chunks = NULL;
    size_t n_texts = 0;
    const char *const *texts = NULL;
    if (d->texts && d->n_texts) {
        texts = d->texts;
        n_texts = d->n_texts;
    } else if (d->text && *d->text) {
        if (d->chunk) n_texts = thread_chunk(d->text, THREAD_CHUNK_MAX_CHARS, &chunks);
        else {
            chunks = malloc(sizeof *chunks);
            chunks[0] = strdup(d->text);
            n_texts = 1;
        }
        texts = (const char *const *)chunks;
    }
    if (!n_texts) {
        thread_strings_free(chunks, n_texts);
        return -EINVAL;
    }
    char id[THREAD_ID_MAX + 1];
    if (d->document_id && *d->document_id) snprintf(id, sizeof id, "%s", d->document_id);
    else {
        /* Database.computeHash: the ten most frequent words, sorted, hashed */
        mc_buf joined = { 0 };
        for (size_t t = 0; t < n_texts; t++) {
            if (t) mc_buf_append_str(&joined, " ");
            mc_buf_append_str(&joined, texts[t]);
        }
        static const char *const STOP[] = { "the", "and", "a", "an", "in", "on", "at", "for", "of", "to", "is", "it", "that", "this" };
        char **words = NULL;
        size_t nwords = 0;
        char *lower = strdup(joined.data ? (const char *)joined.data : "");
        for (char *p = lower; p && *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        /* frequencies */
        struct { char *w; int c; } *freq = NULL;
        size_t nf = 0;
        for (char *tok = strtok(lower, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
            /* trim punctuation */
            while (*tok && strchr(".,;:!?'\"()[]{}<>-", *tok)) tok++;
            size_t l = strlen(tok);
            while (l && strchr(".,;:!?'\"()[]{}<>-", tok[l - 1])) tok[--l] = 0;
            if (!*tok) continue;
            bool stop = false;
            for (size_t k = 0; k < sizeof STOP / sizeof STOP[0]; k++) if (strcmp(STOP[k], tok) == 0) stop = true;
            if (stop) continue;
            size_t f = 0;
            for (; f < nf; f++) if (strcmp(freq[f].w, tok) == 0) break;
            if (f == nf) {
                freq = realloc(freq, (nf + 1) * sizeof *freq);
                freq[nf].w = strdup(tok);
                freq[nf].c = 0;
                nf++;
            }
            freq[f].c++;
        }
        /* top 10 by count desc, word asc; then sort those alphabetically */
        for (size_t i = 0; i < nf; i++)
            for (size_t j = i + 1; j < nf; j++)
                if (freq[j].c > freq[i].c || (freq[j].c == freq[i].c && strcmp(freq[j].w, freq[i].w) < 0)) {
                    __typeof__(freq[0]) t = freq[i]; freq[i] = freq[j]; freq[j] = t;
                }
        nwords = nf < 10 ? nf : 10;
        words = calloc(nwords ? nwords : 1, sizeof *words);
        for (size_t i = 0; i < nwords; i++) words[i] = freq[i].w;
        for (size_t i = 0; i < nwords; i++)
            for (size_t j = i + 1; j < nwords; j++)
                if (strcmp(words[j], words[i]) < 0) { char *t = words[i]; words[i] = words[j]; words[j] = t; }
        mc_buf key = { 0 };
        for (size_t i = 0; i < nwords; i++) {
            if (i) mc_buf_append_str(&key, " ");
            mc_buf_append_str(&key, words[i]);
        }
        char hash[MF_NUMERIC_HASH_MAX];
        mf_numeric_hash(key.data ? key.data : (const unsigned char *)"", key.len, hash);
        snprintf(id, sizeof id, "%s", hash);
        for (size_t i = 0; i < nf; i++) free(freq[i].w);
        free(freq);
        free(words);
        free(lower);
        mc_buf_free(&key);
        mc_buf_free(&joined);
    }
    const char *group_id = d->group_id && *d->group_id ? d->group_id : NULL;
    if (!thread_id_valid(id) || (group_id && !thread_id_valid(group_id))) {
        thread_strings_free(chunks, n_texts);
        return -EINVAL;
    }
    pthread_mutex_lock(&s->lock);
    int rc = 0;
    char *existing_owner = thread_db_text(s->db, "SELECT owner FROM documents WHERE id = ?", id, NULL);
    if (existing_owner && strcmp(existing_owner, owner) != 0) rc = -EPERM;
    if (rc == 0 && group_id && group_owned_by_other(s, owner, group_id)) rc = -EPERM;
    if (rc == 0) rc = thread_db_begin(s->db);
    if (rc) {
        free(existing_owner);
        pthread_mutex_unlock(&s->lock);
        thread_strings_free(chunks, n_texts);
        return rc;
    }
    int64_t now_s = (int64_t)time(NULL), now_ms = mc_wall_ms();
    /* the group */
    if (group_id) {
        sqlite3_stmt *stmt = NULL;
        rc = prepare(s->db, "INSERT INTO groups(id, owner, label, created_at) VALUES(?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET label = CASE WHEN excluded.label <> '' THEN excluded.label ELSE label END", &stmt);
        if (rc == 0) {
            thread_db_bind_text(stmt, 1, group_id);
            thread_db_bind_text(stmt, 2, owner);
            thread_db_bind_text(stmt, 3, d->group_label ? d->group_label : "");
            sqlite3_bind_int64(stmt, 4, now_s);
            rc = run(s->db, stmt, "group");
        }
    }
    /* the document */
    char hash[MF_NUMERIC_HASH_MAX];
    {
        mc_buf joined = { 0 };
        for (size_t t = 0; t < n_texts; t++) {
            if (t) mc_buf_append_str(&joined, "\n");
            mc_buf_append_str(&joined, texts[t]);
        }
        mf_numeric_hash(joined.data ? joined.data : (const unsigned char *)"", joined.len, hash);
        mc_buf_free(&joined);
    }
    char *old_hash = existing_owner ? thread_db_text(s->db, "SELECT content_hash FROM documents WHERE id = ?", id, NULL) : NULL;
    int64_t old_parts = existing_owner ? thread_db_int(s->db, "SELECT COUNT(*) FROM partitions WHERE document_id = ?", id, NULL) : 0;
    int64_t unembedded = existing_owner ? thread_db_int(s->db, "SELECT COUNT(*) FROM partitions WHERE document_id = ? AND embedding IS NULL", id, NULL) : 0;
    bool keep_partitions = old_hash && strcmp(old_hash, hash) == 0 && (size_t)old_parts == n_texts && unembedded == 0;
    free(old_hash);
    const char *family = d->family && thread_family_named(d->family) ? d->family : thread_family_of(id, group_id, d->metadata, d->metadata_len);
    char **made_tags = NULL;
    size_t n_tags = 0;
    const char *const *tags = d->tags;
    if (tags) n_tags = d->n_tags;
    else {
        n_tags = thread_tags(texts, n_texts, &made_tags);
        tags = (const char *const *)made_tags;
    }
    struct json_object *tags_json = json_object_new_array();
    for (size_t t = 0; t < n_tags; t++) json_object_array_add(tags_json, json_object_new_string(tags[t]));
    if (rc == 0) {
        sqlite3_stmt *stmt = NULL;
        rc = prepare(s->db, "INSERT INTO documents(id, owner, group_id, family, name, media_type, metadata, content_hash, tags, revision, enrich_state, created_at, updated_ms) "
                            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 1, 'pending', ?10, ?11) "
                            "ON CONFLICT(id) DO UPDATE SET group_id = excluded.group_id, family = excluded.family, name = excluded.name, media_type = excluded.media_type, "
                            "metadata = excluded.metadata, content_hash = excluded.content_hash, tags = excluded.tags, revision = revision + 1, "
                            "enrich_state = CASE WHEN ?12 THEN enrich_state ELSE 'pending' END, updated_ms = excluded.updated_ms", &stmt);
        if (rc == 0) {
            thread_db_bind_text(stmt, 1, id);
            thread_db_bind_text(stmt, 2, owner);
            thread_db_bind_text(stmt, 3, group_id);
            thread_db_bind_text(stmt, 4, family);
            thread_db_bind_text(stmt, 5, d->name ? d->name : "");
            thread_db_bind_text(stmt, 6, d->media_type && *d->media_type ? d->media_type : "text");
            if (d->metadata && d->metadata_len) sqlite3_bind_blob(stmt, 7, d->metadata, (int)d->metadata_len, SQLITE_TRANSIENT);
            else sqlite3_bind_null(stmt, 7);
            thread_db_bind_text(stmt, 8, hash);
            thread_db_bind_text(stmt, 9, mc_json_compact(tags_json, NULL));
            sqlite3_bind_int64(stmt, 10, now_s);
            sqlite3_bind_int64(stmt, 11, now_ms);
            sqlite3_bind_int(stmt, 12, keep_partitions);
            rc = run(s->db, stmt, "document");
        }
    }
    if (rc == 0 && !keep_partitions) {
        rc = exec2(s->db, "DELETE FROM partitions WHERE document_id = ?", id, NULL);
        thread_vectors_drop(s->vectors, id);
        for (size_t t = 0; rc == 0 && t < n_texts; t++) {
            sqlite3_stmt *stmt = NULL;
            rc = prepare(s->db, "INSERT INTO partitions(document_id, seq, text, media_type) VALUES(?, ?, ?, ?)", &stmt);
            if (rc == 0) {
                thread_db_bind_text(stmt, 1, id);
                sqlite3_bind_int(stmt, 2, (int)t);
                thread_db_bind_text(stmt, 3, texts[t]);
                thread_db_bind_text(stmt, 4, d->media_type && *d->media_type ? d->media_type : "text");
                rc = run(s->db, stmt, "partition");
            }
        }
    }
    /* the graph: caller's entities, else the tags as concepts, else keywords and extraction */
    thread_graph_payload payload = { 0 };
    bool needs_extraction = false;
    if (d->graph && (d->graph->n_entities || d->graph->n_relationships)) {
        for (size_t i = 0; i < d->graph->n_entities; i++) thread_payload_add_entity(&payload, d->graph->entities[i].name, d->graph->entities[i].kind);
        for (size_t i = 0; i < d->graph->n_relationships; i++)
            thread_payload_add_relation(&payload, d->graph->relationships[i].subject, d->graph->relationships[i].predicate, d->graph->relationships[i].object);
    } else if (d->tags && d->n_tags) {
        for (size_t t = 0; t < n_tags; t++) thread_payload_add_entity(&payload, tags[t], "concept");
    } else {
        for (size_t t = 0; t < n_tags; t++) thread_payload_add_entity(&payload, tags[t], "concept");
        needs_extraction = true;
    }
    if (rc == 0) rc = thread_graph_detach(s->db, id);
    if (rc == 0) rc = thread_graph_upsert(s->db, id, &payload, s->dim);
    /* the file row */
    if (rc == 0 && d->file_path && *d->file_path) {
        sqlite3_stmt *stmt = NULL;
        rc = prepare(s->db, "INSERT INTO files(id, owner, path, kind, size, mtime_ms, content_hash, text_indexed, seen_ms) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
                            "ON CONFLICT(id) DO UPDATE SET path = excluded.path, kind = excluded.kind, size = excluded.size, mtime_ms = excluded.mtime_ms, "
                            "content_hash = excluded.content_hash, text_indexed = excluded.text_indexed, seen_ms = excluded.seen_ms", &stmt);
        if (rc == 0) {
            thread_db_bind_text(stmt, 1, id);
            thread_db_bind_text(stmt, 2, owner);
            thread_db_bind_text(stmt, 3, d->file_path);
            thread_db_bind_text(stmt, 4, d->file_kind && *d->file_kind ? d->file_kind : "document");
            sqlite3_bind_int64(stmt, 5, d->file_size);
            sqlite3_bind_int64(stmt, 6, d->file_mtime_ms);
            thread_db_bind_text(stmt, 7, d->file_hash ? d->file_hash : hash);
            sqlite3_bind_int(stmt, 8, d->file_text_indexed);
            sqlite3_bind_int64(stmt, 9, now_ms);
            rc = run(s->db, stmt, "file");
        }
    }
    int64_t revision = rc == 0 ? thread_store_revision(s, id) : 0;
    if (rc == 0) rc = thread_enrich_enqueue(s, id, revision, &payload, needs_extraction);
    if (rc == 0) {
        struct json_object *detail = json_object_new_object();
        json_object_object_add(detail, "family", json_object_new_string(family));
        json_object_object_add(detail, "partitions", json_object_new_int64((int64_t)n_texts));
        json_object_object_add(detail, "kept_embeddings", json_object_new_boolean(keep_partitions));
        json_object_object_add(detail, "revision", json_object_new_int64(revision));
        thread_store_log(s, existing_owner ? "index" : "deposit", d->source ? d->source : "grpc", id, group_id, d->request_id, (int64_t)n_texts, mc_now_ms() - started, detail);
        json_object_put(detail);
        rc = thread_db_commit(s->db);
    } else {
        thread_db_rollback(s->db);
    }
    thread_payload_free(&payload);
    json_object_put(tags_json);
    thread_strings_free(made_tags, n_tags);
    free(existing_owner);
    pthread_mutex_unlock(&s->lock);
    if (rc == 0) {
        thread_enrich_signal(s);
        if (document_id) snprintf(document_id, THREAD_ID_MAX + 1, "%s", id);
        if (partitions) *partitions = n_texts;
    }
    thread_strings_free(chunks, chunks ? n_texts : 0);
    return rc;
}

static void strings_of(struct json_object *arr, const char ***out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (!arr) return;
    size_t len = json_object_array_length(arr);
    const char **v = calloc(len ? len : 1, sizeof *v);
    for (size_t i = 0; i < len; i++) v[*n] = json_object_get_string(json_object_array_get_idx(arr, i)), (*n)++;
    *out = v;
}

int thread_store_deposit_json(thread_store *s, const char *owner, struct json_object *req, struct json_object **reply) {
    *reply = NULL;
    thread_deposit d = { 0 };
    d.document_id = mc_json_string(req, "document_id");
    d.group_id = mc_json_string(req, "group");
    d.group_label = mc_json_string(req, "label");
    d.family = mc_json_string(req, "family");
    d.text = mc_json_string(req, "text");
    bool chunk = false;
    mc_json_bool(req, "chunk", &chunk);
    d.chunk = chunk;
    d.name = mc_json_string(req, "name");
    d.media_type = mc_json_string(req, "media_type");
    d.source = mc_json_string(req, "source");
    d.request_id = mc_json_string(req, "request_id");
    struct json_object *meta = NULL;
    if (json_object_object_get_ex(req, "metadata", &meta) && meta) {
        size_t len = 0;
        d.metadata = json_object_is_type(meta, json_type_string) ? json_object_get_string(meta) : mc_json_compact(meta, &len);
        d.metadata_len = json_object_is_type(meta, json_type_string) ? strlen(d.metadata) : len;
    }
    const char **texts = NULL, **tags = NULL;
    size_t n_texts = 0, n_tags = 0;
    strings_of(mc_json_array(req, "texts"), &texts, &n_texts);
    d.texts = texts;
    d.n_texts = n_texts;
    if (mc_json_array(req, "tags")) {
        strings_of(mc_json_array(req, "tags"), &tags, &n_tags);
        d.tags = tags;
        d.n_tags = n_tags;
    }
    thread_graph_payload payload = { 0 };
    if (mc_json_array(req, "entities") || mc_json_array(req, "relationships")) {
        thread_payload_from_json(req, &payload);
        d.graph = &payload;
    }
    struct json_object *file = mc_json_object(req, "file");
    if (file) {
        d.file_path = mc_json_string(file, "path");
        d.file_kind = mc_json_string(file, "kind");
        mc_json_int64(file, "size", &d.file_size);
        mc_json_int64(file, "mtime_ms", &d.file_mtime_ms);
        d.file_hash = mc_json_string(file, "hash");
        bool indexed = false;
        mc_json_bool(file, "text_indexed", &indexed);
        d.file_text_indexed = indexed;
    }
    char id[THREAD_ID_MAX + 1];
    size_t partitions = 0;
    int rc = thread_store_deposit(s, owner, &d, id, &partitions);
    thread_payload_free(&payload);
    free(texts);
    free(tags);
    if (rc) return rc;
    *reply = json_object_new_object();
    json_object_object_add(*reply, "document_id", json_object_new_string(id));
    json_object_object_add(*reply, "partitions", json_object_new_int64((int64_t)partitions));
    pthread_mutex_lock(&s->lock);
    char *family = thread_db_text(s->db, "SELECT family FROM documents WHERE id = ?", id, NULL);
    pthread_mutex_unlock(&s->lock);
    json_object_object_add(*reply, "family", json_object_new_string(family ? family : "unknown"));
    free(family);
    return 0;
}

/* MARK: - remove and update */

int thread_store_remove_document(thread_store *s, const char *document_id) {
    int rc = thread_graph_detach(s->db, document_id);
    if (rc == 0) rc = exec2(s->db, "DELETE FROM documents WHERE id = ?", document_id, NULL);   /* cascades */
    thread_vectors_drop(s->vectors, document_id);
    return rc;
}

int thread_store_remove(thread_store *s, const char *owner, const char *const *ids, size_t n, const char *source, size_t *removed) {
    *removed = 0;
    pthread_mutex_lock(&s->lock);
    thread_ids targets = { 0 };
    if (n == 0) {
        sqlite3_stmt *stmt = NULL;
        if (prepare(s->db, "SELECT id FROM documents WHERE owner = ?", &stmt) == 0) {
            thread_db_bind_text(stmt, 1, owner);
            while (sqlite3_step(stmt) == SQLITE_ROW) thread_ids_add(&targets, (const char *)sqlite3_column_text(stmt, 0));
            sqlite3_finalize(stmt);
        }
    } else {
        for (size_t i = 0; i < n; i++) if (thread_id_valid(ids[i]) && thread_store_owns(s, owner, ids[i])) thread_ids_add(&targets, ids[i]);
    }
    int rc = thread_db_begin(s->db);
    for (size_t i = 0; rc == 0 && i < targets.n; i++) {
        rc = thread_store_remove_document(s, targets.v[i]);
        if (rc == 0) {
            thread_store_log(s, "remove", source ? source : "grpc", targets.v[i], NULL, NULL, 1, 0, NULL);
            (*removed)++;
        }
    }
    if (rc == 0) rc = thread_db_commit(s->db);
    else thread_db_rollback(s->db);
    thread_ids_free(&targets);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

static bool access_valid(const char *access) {
    return access && (strcmp(access, "available") == 0 || strcmp(access, "restricted") == 0);
}

int thread_store_update_group(thread_store *s, const char *owner, const char *group_id, const char *access, const char *label,
                              const char *description, const char *const *tags, size_t n_tags, bool update_metadata, bool *updated) {
    *updated = false;
    if (!thread_id_valid(group_id)) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    char *o = thread_db_text(s->db, "SELECT owner FROM groups WHERE id = ?", group_id, NULL);
    int rc = 0;
    if (!o || strcmp(o, owner) != 0) rc = o ? -EPERM : -ENOENT;
    free(o);
    if (rc == 0 && access && *access) {
        if (access_valid(access)) {
            rc = exec2(s->db, "UPDATE groups SET access = ? WHERE id = ?", access, group_id);
            if (rc == 0) rc = exec2(s->db, "UPDATE documents SET access = ? WHERE group_id = ?", access, group_id);
            *updated = rc == 0;
        }
    }
    if (rc == 0 && label && *label) {
        rc = exec2(s->db, "UPDATE groups SET label = ? WHERE id = ?", label, group_id);
        *updated = *updated || rc == 0;
    }
    if (rc == 0 && update_metadata) {
        struct json_object *arr = json_object_new_array();
        for (size_t i = 0; i < n_tags; i++) json_object_array_add(arr, json_object_new_string(tags[i]));
        rc = exec2(s->db, "UPDATE groups SET description = ? WHERE id = ?", description ? description : "", group_id);
        if (rc == 0) rc = exec2(s->db, "UPDATE groups SET tags = ? WHERE id = ?", mc_json_compact(arr, NULL), group_id);
        json_object_put(arr);
        *updated = *updated || rc == 0;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int thread_store_update_document(thread_store *s, const char *owner, const char *document_id, const char *access, const char *group_id, bool *updated) {
    *updated = false;
    if (!thread_id_valid(document_id)) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int rc = thread_store_owns(s, owner, document_id) ? 0 : -ENOENT;
    if (rc == 0 && access && *access && access_valid(access)) {
        rc = exec2(s->db, "UPDATE documents SET access = ? WHERE id = ?", access, document_id);
        *updated = rc == 0;
    }
    if (rc == 0 && group_id && *group_id) {
        char *o = thread_db_text(s->db, "SELECT owner FROM groups WHERE id = ?", group_id, NULL);
        if (o && strcmp(o, owner) == 0) {
            rc = exec2(s->db, "UPDATE documents SET group_id = ? WHERE id = ?", group_id, document_id);
            *updated = *updated || rc == 0;
        }
        free(o);
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

/* MARK: - reading */

static struct json_object *group_json(thread_store *s, const char *group_id) {
    sqlite3_stmt *stmt = NULL;
    if (prepare(s->db, "SELECT id, label, owner, access, description, tags, created_at FROM groups WHERE id = ?", &stmt)) return NULL;
    thread_db_bind_text(stmt, 1, group_id);
    struct json_object *g = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        g = json_object_new_object();
        add_text_col(g, "id", stmt, 0);
        add_text_col(g, "label", stmt, 1);
        add_text_col(g, "owner_id", stmt, 2);
        add_text_col(g, "access", stmt, 3);
        add_text_col(g, "description", stmt, 4);
        const char *tags = (const char *)sqlite3_column_text(stmt, 5);
        struct json_object *t = tags ? mc_json_parse(tags, strlen(tags)) : NULL;
        json_object_object_add(g, "tags", t ? t : json_object_new_array());
        json_object_object_add(g, "created_at", json_object_new_int64(sqlite3_column_int64(stmt, 6)));
        json_object_object_add(g, "family", json_object_new_string(thread_family_of("", group_id, NULL, 0)));
    }
    sqlite3_finalize(stmt);
    if (!g) return NULL;
    struct json_object *docs = json_object_new_array();
    if (prepare(s->db, "SELECT id, name, created_at, family FROM documents WHERE group_id = ? ORDER BY created_at, id", &stmt) == 0) {
        thread_db_bind_text(stmt, 1, group_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            struct json_object *d = json_object_new_object();
            add_text_col(d, "id", stmt, 0);
            add_text_col(d, "name", stmt, 1);
            json_object_object_add(d, "created_at", json_object_new_int64(sqlite3_column_int64(stmt, 2)));
            add_text_col(d, "family", stmt, 3);
            json_object_array_add(docs, d);
        }
        sqlite3_finalize(stmt);
    }
    json_object_object_add(g, "documents", docs);
    return g;
}

struct json_object *thread_store_library_json(thread_store *s, const char *owner, int limit, const char *after_id,
                                              const char *const *document_ids, size_t n_ids) {
    struct json_object *out = json_object_new_object(), *groups = json_object_new_array();
    bool has_more = false;
    pthread_mutex_lock(&s->lock);
    thread_ids ids = { 0 };
    if (n_ids) {
        for (size_t i = 0; i < n_ids; i++) {
            char *g = thread_id_valid(document_ids[i]) ? thread_db_text(s->db, "SELECT group_id FROM documents WHERE id = ? AND owner = ?", document_ids[i], owner) : NULL;
            if (g && *g) thread_ids_add(&ids, g);
            free(g);
        }
        /* sorted by id, as the standard path is */
        for (size_t i = 0; i < ids.n; i++)
            for (size_t j = i + 1; j < ids.n; j++)
                if (strcmp(ids.v[j], ids.v[i]) < 0) { char *t = ids.v[i]; ids.v[i] = ids.v[j]; ids.v[j] = t; }
    } else {
        sqlite3_stmt *stmt = NULL;
        if (prepare(s->db, "SELECT id FROM groups WHERE owner = ? AND (?2 = '' OR id > ?2) ORDER BY id", &stmt) == 0) {
            thread_db_bind_text(stmt, 1, owner);
            thread_db_bind_text(stmt, 2, after_id ? after_id : "");
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (limit > 0 && (int)ids.n == limit) {
                    has_more = true;
                    break;
                }
                thread_ids_add(&ids, (const char *)sqlite3_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }
    for (size_t i = 0; i < ids.n; i++) {
        struct json_object *g = group_json(s, ids.v[i]);
        if (g) json_object_array_add(groups, g);
    }
    thread_ids_free(&ids);
    pthread_mutex_unlock(&s->lock);
    json_object_object_add(out, "groups", groups);
    json_object_object_add(out, "has_more", json_object_new_boolean(has_more));
    return out;
}

struct json_object *thread_store_document_json(thread_store *s, const char *owner, const char *id) {
    sqlite3_stmt *stmt = NULL;
    if (prepare(s->db, "SELECT d.id, d.name, d.owner, d.group_id, COALESCE(g.label, ''), d.family, d.created_at, d.media_type, d.metadata, d.tags, d.enrich_state, d.updated_ms "
                       "FROM documents d LEFT JOIN groups g ON g.id = d.group_id WHERE d.id = ? AND d.owner = ?", &stmt)) return NULL;
    thread_db_bind_text(stmt, 1, id);
    thread_db_bind_text(stmt, 2, owner);
    struct json_object *d = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        d = json_object_new_object();
        add_text_col(d, "id", stmt, 0);
        add_text_col(d, "name", stmt, 1);
        add_text_col(d, "owner_id", stmt, 2);
        add_text_col(d, "group_id", stmt, 3);
        add_text_col(d, "group_label", stmt, 4);
        add_text_col(d, "family", stmt, 5);
        json_object_object_add(d, "created_at", json_object_new_int64(sqlite3_column_int64(stmt, 6)));
        add_text_col(d, "media_type", stmt, 7);
        if (sqlite3_column_type(stmt, 8) == SQLITE_BLOB || sqlite3_column_type(stmt, 8) == SQLITE_TEXT) {
            const unsigned char *bytes = sqlite3_column_blob(stmt, 8);
            int len = sqlite3_column_bytes(stmt, 8);
            struct json_object *meta = bytes ? mc_json_parse((const char *)bytes, (size_t)len) : NULL;
            if (meta) json_object_object_add(d, "metadata", meta);
            else json_object_object_add(d, "metadata", json_object_new_string_len(bytes ? (const char *)bytes : "", len));
        } else {
            json_object_object_add(d, "metadata", NULL);
        }
        const char *tags = (const char *)sqlite3_column_text(stmt, 9);
        struct json_object *t = tags ? mc_json_parse(tags, strlen(tags)) : NULL;
        json_object_object_add(d, "tags", t ? t : json_object_new_array());
        add_text_col(d, "enrich_state", stmt, 10);
        json_object_object_add(d, "updated_ms", json_object_new_int64(sqlite3_column_int64(stmt, 11)));
    }
    sqlite3_finalize(stmt);
    if (!d) return NULL;
    struct json_object *texts = json_object_new_array();
    if (prepare(s->db, "SELECT text FROM partitions WHERE document_id = ? ORDER BY seq", &stmt) == 0) {
        thread_db_bind_text(stmt, 1, id);
        while (sqlite3_step(stmt) == SQLITE_ROW) json_object_array_add(texts, json_object_new_string((const char *)sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    json_object_object_add(d, "texts", texts);
    return d;
}

struct json_object *thread_store_documents_json(thread_store *s, const char *owner, const char *const *ids, size_t n) {
    struct json_object *out = json_object_new_object(), *docs = json_object_new_array();
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < n; i++) {
        if (!thread_id_valid(ids[i])) continue;
        struct json_object *d = thread_store_document_json(s, owner, ids[i]);
        if (d) json_object_array_add(docs, d);
    }
    pthread_mutex_unlock(&s->lock);
    json_object_object_add(out, "documents", docs);
    return out;
}

struct json_object *thread_store_export_json(thread_store *s, const char *owner, const char *const *group_ids, size_t n_groups,
                                             const char *prefix, const char *after_id, int limit) {
    struct json_object *out = json_object_new_object(), *docs = json_object_new_array();
    pthread_mutex_lock(&s->lock);
    thread_ids ids = { 0 };
    sqlite3_stmt *stmt = NULL;
    if (prepare(s->db, "SELECT id, group_id FROM documents WHERE owner = ? AND (?2 = '' OR id > ?2) AND (?3 = '' OR substr(id, 1, length(?3)) = ?3) ORDER BY id", &stmt) == 0) {
        thread_db_bind_text(stmt, 1, owner);
        thread_db_bind_text(stmt, 2, after_id ? after_id : "");
        thread_db_bind_text(stmt, 3, prefix ? prefix : "");
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *g = (const char *)sqlite3_column_text(stmt, 1);
            bool wanted = n_groups == 0;
            for (size_t i = 0; !wanted && g && i < n_groups; i++) wanted = strcmp(group_ids[i], g) == 0;
            if (wanted) thread_ids_add(&ids, (const char *)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    size_t take = limit > 0 && (size_t)limit < ids.n ? (size_t)limit : ids.n;
    for (size_t i = 0; i < take; i++) {
        struct json_object *d = thread_store_document_json(s, owner, ids.v[i]);
        if (d) json_object_array_add(docs, d);
    }
    json_object_object_add(out, "documents", docs);
    json_object_object_add(out, "has_more", json_object_new_boolean(ids.n > take));
    thread_ids_free(&ids);
    pthread_mutex_unlock(&s->lock);
    return out;
}

struct json_object *thread_store_stats_json(thread_store *s) {
    struct json_object *o = json_object_new_object();
    pthread_mutex_lock(&s->lock);
    json_object_object_add(o, "node_id", json_object_new_string(s->node_id));
    json_object_object_add(o, "documents", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM documents", NULL, NULL)));
    json_object_object_add(o, "partitions", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM partitions", NULL, NULL)));
    json_object_object_add(o, "embedded", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM partitions WHERE embedding IS NOT NULL", NULL, NULL)));
    json_object_object_add(o, "groups", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM groups", NULL, NULL)));
    json_object_object_add(o, "owners", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(DISTINCT owner) FROM documents", NULL, NULL)));
    json_object_object_add(o, "entities", json_object_new_int64(thread_graph_entity_count(s->db)));
    json_object_object_add(o, "relationships", json_object_new_int64(thread_graph_relationship_count(s->db)));
    json_object_object_add(o, "predicates", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM predicates", NULL, NULL)));
    json_object_object_add(o, "files", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM files", NULL, NULL)));
    json_object_object_add(o, "ledger_rows", json_object_new_int64(thread_ledger_count(s->db)));
    json_object_object_add(o, "jobs_pending", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM jobs", NULL, NULL)));
    json_object_object_add(o, "jobs_failed", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM documents WHERE enrich_state = 'failed'", NULL, NULL)));
    struct stat st;
    json_object_object_add(o, "db_bytes", json_object_new_int64(stat(s->db_path, &st) == 0 ? (int64_t)st.st_size : 0));
    char wal[640];
    snprintf(wal, sizeof wal, "%s-wal", s->db_path);
    json_object_object_add(o, "wal_bytes", json_object_new_int64(stat(wal, &st) == 0 ? (int64_t)st.st_size : 0));
    json_object_object_add(o, "db_path", json_object_new_string(s->db_path));
    struct json_object *v = json_object_new_object();
    json_object_object_add(v, "count", json_object_new_int64((int64_t)thread_vectors_count(s->vectors)));
    json_object_object_add(v, "documents", json_object_new_int64((int64_t)thread_vectors_documents(s->vectors)));
    json_object_object_add(v, "bytes", json_object_new_int64((int64_t)thread_vectors_bytes(s->vectors)));
    json_object_object_add(v, "dim", json_object_new_int64((int64_t)s->dim));
    json_object_object_add(o, "vectors", v);
    json_object_object_add(o, "embedding_model", json_object_new_string(THREAD_EMBEDDING_MODEL));
    json_object_object_add(o, "embedder", json_object_new_boolean(s->embedder != NULL));
    json_object_object_add(o, "user_version", json_object_new_int64(thread_db_int(s->db, "PRAGMA user_version", NULL, NULL)));
    pthread_mutex_unlock(&s->lock);
    return o;
}

struct json_object *thread_store_schemas_json(thread_store *s) {
    struct json_object *arr = thread_families_json();
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < json_object_array_length(arr); i++) {
        struct json_object *f = json_object_array_get_idx(arr, i);
        const char *name = mc_json_string(f, "name");
        json_object_object_add(f, "count", json_object_new_int64(thread_db_int(s->db, "SELECT COUNT(*) FROM documents WHERE family = ?", name, NULL)));
        int64_t last = thread_db_int(s->db, "SELECT COALESCE(MAX(updated_ms), 0) FROM documents WHERE family = ?", name, NULL);
        json_object_object_add(f, "last_written_ms", json_object_new_int64(last > 0 ? last : 0));
    }
    pthread_mutex_unlock(&s->lock);
    return arr;
}

struct json_object *thread_store_policy_json(thread_store *s) {
    pthread_mutex_lock(&s->lock);
    struct json_object *o = thread_policy_json(&s->policy);
    pthread_mutex_unlock(&s->lock);
    return o;
}

int thread_store_policy_set(thread_store *s, struct json_object *policy) {
    size_t len = 0;
    const char *text = mc_json_compact(policy, &len);
    thread_policy parsed;
    int rc = thread_policy_from_json(text, len, &parsed);
    if (rc) return rc;
    pthread_mutex_lock(&s->lock);
    thread_policy_free(&s->policy);
    s->policy = parsed;
    rc = thread_db_meta_set(s->db, "extraction_policy", text);
    pthread_mutex_unlock(&s->lock);
    return rc;
}
