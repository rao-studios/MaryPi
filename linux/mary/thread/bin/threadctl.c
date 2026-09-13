/* threadctl: threadd from a terminal, as the user running it, over local.sock.
 *
 *   threadctl stats | schemas | parity | policy
 *   threadctl library [--limit N] [--after GROUP-ID]
 *   threadctl documents DOCUMENT-ID...
 *   threadctl search [--lane LANE]... [--group GROUP]... [--top N] TEXT
 *   threadctl deposit [--group GROUP] [--label TEXT] [--family FAMILY] [--document ID] [--chunk] TEXT...
 *   threadctl remove DOCUMENT-ID...
 *   threadctl graph [--entity NAME] [--query TEXT] [--hops N] [--limit N] [--documents]
 *   threadctl ledger [--limit N] [--kind KIND] [--document ID] [--request ID]
 *   threadctl file PATH-OR-ID
 *   threadctl drain | checkpoint | backup PATH
 *   threadctl raw JSON                      one request line, the reply printed as JSON */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"
#include "thread/local.h"

static const char *socket_path(void) {
    const char *path = getenv("THREAD_LOCAL_SOCKET");
    return path && *path ? path : THREAD_LOCAL_SOCKET_PATH;
}

static struct json_object *reply_line;
static int take_line(const char *line, size_t len, void *user) {
    reply_line = mc_json_parse(line, len);
    return 1;
}

/* One request; the reply, or NULL after printing why. */
static struct json_object *call(const char *path, struct json_object *req) {
    int fd = mc_connect_unix(path);
    if (fd < 0) {
        fprintf(stderr, "threadctl: cannot reach threadd at %s: %s\n", path, strerror(-fd));
        return NULL;
    }
    size_t n = 0;
    const char *text = mc_json_compact(req, &n);
    if (mc_write_all(fd, text, n) || mc_write_all(fd, "\n", 1)) {
        fprintf(stderr, "threadctl: cannot write to threadd\n");
        close(fd);
        return NULL;
    }
    mc_line_reader reader;
    mc_line_reader_init(&reader, THREAD_LOCAL_LINE_MAX);
    reply_line = NULL;
    char buf[65536];
    for (;;) {
        ssize_t got = read(fd, buf, sizeof buf);
        if (got <= 0) break;
        if (mc_line_reader_feed(&reader, buf, (size_t)got, take_line, NULL) == 1) break;
    }
    mc_line_reader_free(&reader);
    close(fd);
    if (!reply_line) {
        fprintf(stderr, "threadctl: threadd closed the connection without answering\n");
        return NULL;
    }
    const char *type = mc_json_type(reply_line);
    if (type && strcmp(type, "error") == 0) {
        fprintf(stderr, "threadctl: %s (%s)\n", mc_json_string(reply_line, "message"), mc_json_string(reply_line, "code"));
        json_object_put(reply_line);
        return NULL;
    }
    return reply_line;
}

static struct json_object *request(const char *type) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string(type));
    json_object_object_add(o, "source", json_object_new_string("threadctl"));
    return o;
}

static void when(int64_t ms_or_s, char *out, size_t cap) {
    time_t t = (time_t)(ms_or_s > 100000000000LL ? ms_or_s / 1000 : ms_or_s);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M", &tm);
}

static int print_json(struct json_object *o) {
    printf("%s\n", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(o);
    return 0;
}

static int library(const char *path, int argc, char **argv) {
    struct json_object *req = request("library");
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) json_object_object_add(req, "limit", json_object_new_int(atoi(argv[++i])));
        else if (strcmp(argv[i], "--after") == 0 && i + 1 < argc) json_object_object_add(req, "after", json_object_new_string(argv[++i]));
        else return 2;
    }
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    struct json_object *groups = mc_json_array(r, "groups");
    if (!groups || !json_object_array_length(groups)) printf("(no groups)\n");
    for (size_t g = 0; groups && g < json_object_array_length(groups); g++) {
        struct json_object *group = json_object_array_get_idx(groups, g);
        struct json_object *docs = mc_json_array(group, "documents");
        size_t n = docs ? json_object_array_length(docs) : 0;
        const char *label = mc_json_string(group, "label");
        printf("%s  %s  [%s]  (%zu document%s)\n", mc_json_string(group, "id"), label && *label ? label : "-", mc_json_string(group, "family"), n, n == 1 ? "" : "s");
        for (size_t d = 0; d < n; d++) {
            struct json_object *doc = json_object_array_get_idx(docs, d);
            int64_t created = 0;
            mc_json_int64(doc, "created_at", &created);
            char at[32];
            when(created, at, sizeof at);
            printf("    %s  %s  %s\n", mc_json_string(doc, "id"), at, mc_json_string(doc, "name"));
        }
    }
    bool more = false;
    if (mc_json_bool(r, "has_more", &more) && more) printf("(more)\n");
    json_object_put(r);
    return 0;
}

static int documents(const char *path, int argc, char **argv) {
    if (argc == 0) return 2;
    struct json_object *req = request("documents"), *ids = json_object_new_array();
    for (int i = 0; i < argc; i++) json_object_array_add(ids, json_object_new_string(argv[i]));
    json_object_object_add(req, "ids", ids);
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    struct json_object *docs = mc_json_array(r, "documents");
    if (!docs || !json_object_array_length(docs)) printf("(none of those documents are yours)\n");
    for (size_t i = 0; docs && i < json_object_array_length(docs); i++) {
        struct json_object *c = json_object_array_get_idx(docs, i);
        int64_t created = 0;
        mc_json_int64(c, "created_at", &created);
        char at[32];
        when(created, at, sizeof at);
        const char *label = mc_json_string(c, "group_label");
        printf("== %s  (%s, %s, %s)\n", mc_json_string(c, "id"), label && *label ? label : mc_json_string(c, "group_id"), mc_json_string(c, "family"), at);
        struct json_object *texts = mc_json_array(c, "texts");
        for (size_t t = 0; texts && t < json_object_array_length(texts); t++) printf("%s\n", json_object_get_string(json_object_array_get_idx(texts, t)));
    }
    json_object_put(r);
    return 0;
}

static int search(const char *path, int argc, char **argv) {
    struct json_object *req = request("search"), *lanes = json_object_new_array(), *groups = json_object_new_array();
    int i = 0;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--lane") == 0 && i + 1 < argc) json_object_array_add(lanes, json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--group") == 0 && i + 1 < argc) json_object_array_add(groups, json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) json_object_object_add(req, "top_k", json_object_new_int(atoi(argv[++i])));
        else break;
    }
    if (i == argc) {
        json_object_put(req);
        json_object_put(lanes);
        json_object_put(groups);
        return 2;
    }
    char query[4096] = "";
    for (; i < argc; i++) {
        size_t l = strlen(query);
        snprintf(query + l, sizeof query - l, "%s%s", l ? " " : "", argv[i]);
    }
    json_object_object_add(req, "query", json_object_new_string(query));
    json_object_object_add(req, "lanes", lanes);
    json_object_object_add(req, "groups", groups);
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    struct json_object *results = mc_json_array(r, "results");
    if (!results || !json_object_array_length(results)) printf("(nothing close enough)\n");
    for (size_t k = 0; results && k < json_object_array_length(results); k++) {
        struct json_object *h = json_object_array_get_idx(results, k);
        double score = 0;
        mc_json_double(h, "score", &score);
        printf("%.3f  %s  [%s]  %s\n", score, mc_json_string(h, "document_id"), mc_json_string(h, "lane"), mc_json_string(h, "name"));
        const char *text = mc_json_string(h, "text");
        printf("       %.160s%s\n", text ? text : "", text && strlen(text) > 160 ? "…" : "");
    }
    struct json_object *trace = mc_json_object(r, "trace");
    if (trace) printf("(graph: %zu entities, %zu edges matched; %zu expansion edges)\n", json_object_array_length(mc_json_array(trace, "matched_entity_ids")),
                      json_object_array_length(mc_json_array(trace, "matched_relationship_ids")), json_object_array_length(mc_json_array(trace, "expansion_edge_ids")));
    json_object_put(r);
    return 0;
}

static int deposit(const char *path, int argc, char **argv) {
    struct json_object *req = request("deposit");
    int i = 0;
    bool chunk = false;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--group") == 0 && i + 1 < argc) json_object_object_add(req, "group", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) json_object_object_add(req, "label", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--family") == 0 && i + 1 < argc) json_object_object_add(req, "family", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--document") == 0 && i + 1 < argc) json_object_object_add(req, "document_id", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) json_object_object_add(req, "name", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--chunk") == 0) chunk = true;
        else break;
    }
    if (i == argc) {
        json_object_put(req);
        return 2;
    }
    if (chunk) {
        char text[65536] = "";
        for (; i < argc; i++) {
            size_t l = strlen(text);
            snprintf(text + l, sizeof text - l, "%s%s", l ? " " : "", argv[i]);
        }
        json_object_object_add(req, "text", json_object_new_string(text));
        json_object_object_add(req, "chunk", json_object_new_boolean(true));
    } else {
        struct json_object *texts = json_object_new_array();
        for (; i < argc; i++) json_object_array_add(texts, json_object_new_string(argv[i]));
        json_object_object_add(req, "texts", texts);
    }
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    int64_t parts = 0;
    mc_json_int64(r, "partitions", &parts);
    printf("deposited %s as %s (%lld partition%s)\n", mc_json_string(r, "document_id"), mc_json_string(r, "family"), (long long)parts, parts == 1 ? "" : "s");
    json_object_put(r);
    return 0;
}

static int remove_documents(const char *path, int argc, char **argv) {
    if (argc == 0) return 2;
    struct json_object *req = request("remove"), *ids = json_object_new_array();
    for (int i = 0; i < argc; i++) json_object_array_add(ids, json_object_new_string(argv[i]));
    json_object_object_add(req, "ids", ids);
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    int64_t removed = 0;
    mc_json_int64(r, "removed", &removed);
    printf("removed %lld\n", (long long)removed);
    json_object_put(r);
    return 0;
}

static int graph(const char *path, int argc, char **argv) {
    struct json_object *req = request("graph");
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--entity") == 0 && i + 1 < argc) json_object_object_add(req, "entity", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) json_object_object_add(req, "query", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--hops") == 0 && i + 1 < argc) json_object_object_add(req, "hops", json_object_new_int(atoi(argv[++i])));
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) json_object_object_add(req, "limit", json_object_new_int(atoi(argv[++i])));
        else if (strcmp(argv[i], "--documents") == 0) json_object_object_add(req, "documents", json_object_new_boolean(true));
        else {
            json_object_put(req);
            return 2;
        }
    }
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    int64_t ne = 0, nr = 0;
    mc_json_int64(r, "entity_count", &ne);
    mc_json_int64(r, "relationship_count", &nr);
    struct json_object *ents = mc_json_array(r, "entities"), *rels = mc_json_array(r, "relationships");
    printf("%zu of %lld entities · %zu of %lld relationships\n", ents ? json_object_array_length(ents) : 0, (long long)ne, rels ? json_object_array_length(rels) : 0, (long long)nr);
    for (size_t i = 0; ents && i < json_object_array_length(ents); i++) {
        struct json_object *e = json_object_array_get_idx(ents, i);
        int64_t mentions = 0;
        mc_json_int64(e, "mention_count", &mentions);
        printf("  %s  (%s, %lld mention%s, %zu document%s)  %s\n", mc_json_string(e, "name"), mc_json_string(e, "kind"), (long long)mentions, mentions == 1 ? "" : "s",
               json_object_array_length(mc_json_array(e, "document_ids")), json_object_array_length(mc_json_array(e, "document_ids")) == 1 ? "" : "s", mc_json_string(e, "id"));
    }
    for (size_t i = 0; rels && i < json_object_array_length(rels); i++) {
        struct json_object *x = json_object_array_get_idx(rels, i);
        int64_t weight = 0;
        mc_json_int64(x, "weight", &weight);
        printf("  %s —%s→ %s  ×%lld\n", mc_json_string(x, "subject_id"), mc_json_string(x, "predicate"), mc_json_string(x, "object_id"), (long long)weight);
    }
    json_object_put(r);
    return 0;
}

static int ledger(const char *path, int argc, char **argv) {
    struct json_object *req = request("ledger");
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) json_object_object_add(req, "limit", json_object_new_int(atoi(argv[++i])));
        else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) json_object_object_add(req, "kind", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--document") == 0 && i + 1 < argc) json_object_object_add(req, "document_id", json_object_new_string(argv[++i]));
        else if (strcmp(argv[i], "--request") == 0 && i + 1 < argc) json_object_object_add(req, "request_id", json_object_new_string(argv[++i]));
        else {
            json_object_put(req);
            return 2;
        }
    }
    struct json_object *r = call(path, req);
    json_object_put(req);
    if (!r) return 1;
    struct json_object *rows = mc_json_array(r, "rows");
    if (!rows || !json_object_array_length(rows)) printf("(nothing in the ledger)\n");
    for (size_t i = 0; rows && i < json_object_array_length(rows); i++) {
        struct json_object *row = json_object_array_get_idx(rows, i);
        int64_t at = 0, count = 0, ms = 0;
        mc_json_int64(row, "at_ms", &at);
        mc_json_int64(row, "count", &count);
        mc_json_int64(row, "ms", &ms);
        char when_s[32];
        when(at, when_s, sizeof when_s);
        const char *doc = mc_json_string(row, "document_id"), *reqid = mc_json_string(row, "request_id");
        printf("%s  %-9s %-9s %s%s%s  ×%lld  %lldms\n", when_s, mc_json_string(row, "kind"), mc_json_string(row, "source"), doc ? doc : "", doc && reqid ? " " : "", reqid ? reqid : "",
               (long long)count, (long long)ms);
    }
    json_object_put(r);
    return 0;
}

static int file_record(const char *path, int argc, char **argv) {
    if (argc != 1) return 2;
    struct json_object *req = request("file.record");
    json_object_object_add(req, argv[0][0] == '/' ? "path" : "id", json_object_new_string(argv[0]));
    struct json_object *r = call(path, req);
    json_object_put(req);
    return r ? print_json(r) : 1;
}

static int simple(const char *path, const char *type, const char *key, const char *value) {
    struct json_object *req = request(type);
    if (key && value) json_object_object_add(req, key, json_object_new_string(value));
    struct json_object *r = call(path, req);
    json_object_put(req);
    return r ? print_json(r) : 1;
}

int main(int argc, char **argv) {
    const char *path = socket_path();
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "--socket") == 0) {
        path = argv[i + 1];
        i += 2;
    }
    int rc = 2;
    const char *cmd = i < argc ? argv[i] : "";
    int rest = argc - i - 1;
    char **args = argv + i + 1;
    if (strcmp(cmd, "library") == 0) rc = library(path, rest, args);
    else if (strcmp(cmd, "documents") == 0) rc = documents(path, rest, args);
    else if (strcmp(cmd, "search") == 0) rc = search(path, rest, args);
    else if (strcmp(cmd, "deposit") == 0) rc = deposit(path, rest, args);
    else if (strcmp(cmd, "remove") == 0) rc = remove_documents(path, rest, args);
    else if (strcmp(cmd, "graph") == 0) rc = graph(path, rest, args);
    else if (strcmp(cmd, "ledger") == 0) rc = ledger(path, rest, args);
    else if (strcmp(cmd, "file") == 0) rc = file_record(path, rest, args);
    else if (strcmp(cmd, "stats") == 0 || strcmp(cmd, "schemas") == 0 || strcmp(cmd, "parity") == 0 || strcmp(cmd, "policy") == 0 || strcmp(cmd, "checkpoint") == 0)
        rc = simple(path, cmd, NULL, NULL);
    else if (strcmp(cmd, "drain") == 0) rc = simple(path, "enrich.drain", NULL, NULL);
    else if (strcmp(cmd, "backup") == 0 && rest == 1) rc = simple(path, "backup", "path", args[0]);
    else if (strcmp(cmd, "raw") == 0 && rest == 1) {
        struct json_object *req = mc_json_parse(args[0], strlen(args[0]));
        if (!req) fprintf(stderr, "threadctl: the request is not JSON\n");
        else {
            struct json_object *r = call(path, req);
            json_object_put(req);
            rc = r ? print_json(r) : 1;
        }
        if (!req) rc = 1;
    }
    if (rc == 2)
        fprintf(stderr, "usage: threadctl [--socket PATH] stats | schemas | parity | policy | drain | checkpoint\n"
                        "       threadctl library [--limit N] [--after GROUP-ID]\n"
                        "       threadctl documents DOCUMENT-ID...\n"
                        "       threadctl search [--lane LANE]... [--group GROUP]... [--top N] TEXT...\n"
                        "       threadctl deposit [--group G] [--label T] [--family F] [--document ID] [--name N] [--chunk] TEXT...\n"
                        "       threadctl remove DOCUMENT-ID...\n"
                        "       threadctl graph [--entity NAME] [--query TEXT] [--hops N] [--limit N] [--documents]\n"
                        "       threadctl ledger [--limit N] [--kind KIND] [--document ID] [--request ID]\n"
                        "       threadctl file PATH-OR-ID\n"
                        "       threadctl backup PATH\n"
                        "       threadctl raw '{\"type\": ...}'\n");
    return rc;
}
