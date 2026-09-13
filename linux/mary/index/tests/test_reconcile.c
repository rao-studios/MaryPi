/* Reconciliation against a fake threadd on a unix socket: it calls the first file
 * missing and the second stale, and records what indexd deposits. */
#include "test_support.h"

#include <pthread.h>
#include <sys/socket.h>

#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"
#include "index/reconcile.h"

static char sock[128];
static int pages, deposits, last_seen, removes;
static char deposited[4][256];
static char last_reply[256];

static int answer(const char *line, size_t len, void *user) {
    int fd = *(int *)user;
    struct json_object *req = mc_json_parse(line, len);
    const char *type = mc_json_type(req);
    struct json_object *reply = json_object_new_object();
    if (type && strcmp(type, "parity.report") == 0) {
        pages++;
        bool last = false;
        mc_json_bool(req, "last", &last);
        if (last) last_seen++;
        struct json_object *entries = mc_json_array(req, "entries"), *out = json_object_new_array();
        json_object_object_add(reply, "type", json_object_new_string("parity.report.result"));
        json_object_object_add(reply, "run_id", json_object_new_int64(42));
        for (size_t i = 0; entries && i < json_object_array_length(entries); i++) {
            struct json_object *e = json_object_array_get_idx(entries, i), *o = json_object_new_object();
            const char *path = mc_json_string(e, "path");
            json_object_object_add(o, "path", json_object_new_string(path));
            json_object_object_add(o, "status", json_object_new_string(strstr(path, "a.txt") ? "missing" : strstr(path, "b.md") ? "stale" : "recorded"));
            if (!strstr(path, "c.png")) json_object_array_add(out, o);
            else json_object_put(o);
        }
        json_object_object_add(reply, "entries", out);
        json_object_object_add(reply, "missing", json_object_new_int64(1));
        json_object_object_add(reply, "stale", json_object_new_int64(1));
        json_object_object_add(reply, "recorded", json_object_new_int64(1));
        json_object_object_add(reply, "orphaned", json_object_new_int64(last ? 2 : 0));
    } else if (type && strcmp(type, "deposit") == 0) {
        if (deposits < 4) snprintf(deposited[deposits], 256, "%s", mc_json_string(mc_json_object(req, "file"), "path"));
        deposits++;
        json_object_object_add(reply, "type", json_object_new_string("deposit.result"));
        json_object_object_add(reply, "document_id", json_object_new_string("file-x"));
    } else if (type && strcmp(type, "file.remove") == 0) {
        removes++;
        json_object_object_add(reply, "type", json_object_new_string("file.remove.result"));
        json_object_object_add(reply, "removed", json_object_new_boolean(true));
    } else {
        json_object_object_add(reply, "type", json_object_new_string("error"));
        json_object_object_add(reply, "message", json_object_new_string("no such op"));
    }
    size_t n = 0;
    const char *text = mc_json_compact(reply, &n);
    snprintf(last_reply, sizeof last_reply, "%.*s", (int)(n < 255 ? n : 255), text);
    mc_write_all(fd, text, n);
    mc_write_all(fd, "\n", 1);
    json_object_put(reply);
    json_object_put(req);
    return 0;
}

static void *serve(void *arg) {
    int listener = *(int *)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) break;
        mc_line_reader reader;
        mc_line_reader_init(&reader, 1 << 20);
        char buf[65536];
        for (;;) {
            ssize_t got = read(fd, buf, sizeof buf);
            if (got <= 0) break;
            mc_line_reader_feed(&reader, buf, (size_t)got, answer, &fd);
        }
        mc_line_reader_free(&reader);
        close(fd);
    }
    return NULL;
}

MARY_TEST(the_missing_and_the_stale_are_written_and_the_last_page_closes_the_run) {
    make_home();
    put_file("a.txt", "alpha");
    put_file("b.md", "beta");
    put_file("c.png", "gamma");
    snprintf(sock, sizeof sock, "%s/.thread.sock", home);
    int listener = mc_listen_unix(sock, 0600);
    MARY_ASSERT(listener >= 0);
    pthread_t t;
    pthread_create(&t, NULL, serve, &listener);
    ix_client client = { sock };
    ix_reconcile_stats stats;
    char message[256] = "";
    MARY_ASSERT_EQ(ix_reconcile(&client, home, "rao", &stats, message, sizeof message), 0);
    MARY_ASSERT_EQ(pages, 1);                 /* three files fit one page */
    MARY_ASSERT_EQ(last_seen, 1);
    MARY_ASSERT_EQ(stats.seen, 3);
    MARY_ASSERT_EQ(stats.run_id, 42);
    MARY_ASSERT_EQ(stats.missing, 1);
    MARY_ASSERT_EQ(stats.stale, 1);
    MARY_ASSERT_EQ(stats.orphaned, 2);
    MARY_ASSERT_EQ(stats.deposited, 2);
    MARY_ASSERT_EQ(deposits, 2);
    MARY_ASSERT(strstr(deposited[0], "a.txt") != NULL);
    MARY_ASSERT(strstr(deposited[1], "b.md") != NULL);
    /* a removal and a bad op through the same client */
    bool removed = false;
    MARY_ASSERT_EQ(ix_file_remove(&client, "/x", &removed, message, sizeof message), 0);
    MARY_ASSERT(removed);
    struct json_object *req = json_object_new_object(), *reply = NULL;
    json_object_object_add(req, "type", json_object_new_string("dance"));
    MARY_ASSERT_EQ(ix_call(&client, req, &reply, message, sizeof message), -EIO);
    MARY_ASSERT_STR(message, "no such op");
    MARY_ASSERT(reply == NULL);
    json_object_put(req);
    shutdown(listener, SHUT_RDWR);
    close(listener);
    pthread_join(t, NULL);
    /* threadd gone: the run fails without a partial write */
    deposits = 0;
    MARY_ASSERT(ix_reconcile(&client, home, "rao", &stats, message, sizeof message) < 0);
    MARY_ASSERT_EQ(deposits, 0);
    MARY_ASSERT(strstr(message, "not reachable") != NULL);
    remove_tree(home);
}

int main(void) {
    MARY_RUN(the_missing_and_the_stale_are_written_and_the_last_page_closes_the_run);
    MARY_TEST_MAIN_END();
}
