#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mary_test.h"
#include "thread/peer.h"
#include "thread/store.h"

/* rm -rf for a test's own temporary directory, without a shell. */
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

static char dir[64];

static thread_store *fresh(void) {
    snprintf(dir, sizeof dir, "/tmp/thread-store-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    int error = 0;
    thread_store *s = thread_store_open(dir, &error);
    MARY_ASSERT(s != NULL);
    return s;
}

static void destroy(thread_store *s) {
    thread_store_close(s);
    remove_tree(dir);
}

/* One item per text; group and ids as given. */
static int put(thread_store *s, const char *owner, const char *group, const char *label, const char *doc, const char *text, size_t *indexed) {
    char *texts[] = { (char *)text };
    Thread__V1__ThreadIndexItem item = THREAD__V1__THREAD_INDEX_ITEM__INIT;
    item.document_id = (char *)doc;
    item.n_texts = text ? 1 : 0;
    item.texts = texts;
    item.name = "a turn";
    Thread__V1__ThreadIndexItem *items[] = { &item };
    Thread__V1__ThreadIndexRequest req = THREAD__V1__THREAD_INDEX_REQUEST__INIT;
    req.group_id = (char *)(group ? group : "");
    req.group_label = (char *)(label ? label : "");
    req.n_items = 1;
    req.items = items;
    size_t ignored;
    return thread_store_index(s, owner, &req, indexed ? indexed : &ignored);
}

static Thread__V1__ThreadLibraryResponse *library(thread_store *s, const char *owner, int limit, const char *after) {
    Thread__V1__ThreadLibraryRequest req = THREAD__V1__THREAD_LIBRARY_REQUEST__INIT;
    req.limit = limit;
    req.after_id = (char *)(after ? after : "");
    return thread_store_library(s, owner, &req);
}

MARY_TEST(a_turn_is_indexed_listed_and_read_back) {
    thread_store *s = fresh();
    size_t indexed = 0;
    MARY_ASSERT_EQ(put(s, "mary", "mary-conversations", "Conversations", "mary-turn-1", "What is the capital of France?", &indexed), 0);
    MARY_ASSERT_EQ(indexed, 1);
    MARY_ASSERT_EQ(put(s, "mary", "mary-conversations", NULL, "mary-turn-2", "And of Portugal?", NULL), 0);
    Thread__V1__ThreadLibraryResponse *lib = library(s, "mary", 0, NULL);
    MARY_ASSERT_EQ(lib->n_groups, 1);
    MARY_ASSERT_STR(lib->groups[0]->label, "Conversations");            /* a later index without a label keeps it */
    MARY_ASSERT_EQ(lib->groups[0]->n_documents, 2);
    MARY_ASSERT_STR(lib->groups[0]->documents[1]->name, "a turn");
    MARY_ASSERT(lib->groups[0]->documents[0]->created_at > 1700000000);
    thread_library_response_free(lib);

    char *ids[] = { "mary-turn-2", "../etc/passwd", "no-such-turn", "mary-turn-1" };
    Thread__V1__ThreadDocumentsRequest dreq = THREAD__V1__THREAD_DOCUMENTS_REQUEST__INIT;
    dreq.n_document_ids = 4;
    dreq.document_ids = ids;
    Thread__V1__ThreadDocumentsResponse *docs = thread_store_documents(s, "mary", &dreq);
    MARY_ASSERT_EQ(docs->n_documents, 2);
    MARY_ASSERT_STR(docs->documents[0]->id, "mary-turn-2");                 /* request order */
    MARY_ASSERT_STR(docs->documents[1]->texts[0], "What is the capital of France?");
    MARY_ASSERT_STR(docs->documents[1]->group_label, "Conversations");
    MARY_ASSERT_STR(docs->documents[1]->media_type, "text");
    thread_documents_response_free(docs);

    char path[128];
    struct stat st;
    snprintf(path, sizeof path, "%s/documents/mary-turn-1.json", dir);
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT_EQ(st.st_mode & 0777, 0600);
    destroy(s);
}

MARY_TEST(the_node_id_is_made_once) {
    thread_store *s = fresh();
    char first[37];
    snprintf(first, sizeof first, "%s", thread_store_node_id(s));
    MARY_ASSERT_EQ(strlen(first), 36);
    MARY_ASSERT_EQ(first[14], '4');   /* a version 4 UUID */
    thread_store_close(s);
    int error = 0;
    s = thread_store_open(dir, &error);
    MARY_ASSERT_STR(thread_store_node_id(s), first);
    destroy(s);
}

MARY_TEST(one_owner_cannot_touch_anothers) {
    thread_store *s = fresh();
    MARY_ASSERT_EQ(put(s, "mary", "mary-conversations", "Conversations", "mary-turn-1", "private", NULL), 0);
    MARY_ASSERT_EQ(put(s, "guest", "mary-conversations", NULL, "guest-turn-1", "hello", NULL), -EPERM);
    MARY_ASSERT_EQ(put(s, "guest", "guest-notes", NULL, "mary-turn-1", "overwrite", NULL), -EPERM);
    Thread__V1__ThreadLibraryResponse *lib = library(s, "guest", 0, NULL);
    MARY_ASSERT_EQ(lib->n_groups, 0);                                      /* the refused index wrote nothing */
    thread_library_response_free(lib);
    char *ids[] = { "mary-turn-1" };
    Thread__V1__ThreadDocumentsRequest dreq = THREAD__V1__THREAD_DOCUMENTS_REQUEST__INIT;
    dreq.n_document_ids = 1;
    dreq.document_ids = ids;
    Thread__V1__ThreadDocumentsResponse *docs = thread_store_documents(s, "guest", &dreq);
    MARY_ASSERT_EQ(docs->n_documents, 0);
    thread_documents_response_free(docs);
    destroy(s);
}

MARY_TEST(ids_must_be_file_names) {
    MARY_ASSERT(thread_id_valid("mary-turn-1757700000000-ab12"));
    MARY_ASSERT(thread_id_valid("a:b.c_d"));
    MARY_ASSERT(!thread_id_valid(""));
    MARY_ASSERT(!thread_id_valid(".."));
    MARY_ASSERT(!thread_id_valid("a/b"));
    MARY_ASSERT(!thread_id_valid("with space"));
    thread_store *s = fresh();
    MARY_ASSERT_EQ(put(s, "mary", "../groups", NULL, "turn", "x", NULL), -EINVAL);
    MARY_ASSERT_EQ(put(s, "mary", "g", NULL, "../../escape", "x", NULL), -EINVAL);
    size_t indexed = 7;
    MARY_ASSERT_EQ(put(s, "mary", "g", NULL, "../../escape", NULL, &indexed), 0);   /* no texts: skipped, not checked */
    MARY_ASSERT_EQ(indexed, 0);
    destroy(s);
}

MARY_TEST(groups_page_by_id) {
    thread_store *s = fresh();
    for (const char *g = "cab"; *g; g++) {
        char id[2] = { *g, 0 }, doc[8];
        snprintf(doc, sizeof doc, "doc-%c", *g);
        MARY_ASSERT_EQ(put(s, "mary", id, NULL, doc, "text", NULL), 0);
    }
    Thread__V1__ThreadLibraryResponse *page = library(s, "mary", 2, NULL);
    MARY_ASSERT_EQ(page->n_groups, 2);
    MARY_ASSERT_STR(page->groups[0]->id, "a");
    MARY_ASSERT_STR(page->groups[1]->id, "b");
    MARY_ASSERT(page->has_more);
    thread_library_response_free(page);
    page = library(s, "mary", 2, "b");
    MARY_ASSERT_EQ(page->n_groups, 1);
    MARY_ASSERT_STR(page->groups[0]->id, "c");
    MARY_ASSERT(!page->has_more);
    thread_library_response_free(page);

    char *ids[] = { "doc-c" };
    Thread__V1__ThreadLibraryRequest req = THREAD__V1__THREAD_LIBRARY_REQUEST__INIT;
    req.n_document_ids = 1;
    req.document_ids = ids;
    page = thread_store_library(s, "mary", &req);
    MARY_ASSERT_EQ(page->n_groups, 1);
    MARY_ASSERT_STR(page->groups[0]->id, "c");
    thread_library_response_free(page);
    destroy(s);
}

MARY_TEST(a_document_moves_to_its_new_group) {
    thread_store *s = fresh();
    MARY_ASSERT_EQ(put(s, "mary", "inbox", NULL, "note", "first", NULL), 0);
    MARY_ASSERT_EQ(put(s, "mary", "archive", NULL, "note", "second", NULL), 0);
    Thread__V1__ThreadLibraryResponse *lib = library(s, "mary", 0, NULL);
    MARY_ASSERT_EQ(lib->n_groups, 2);
    MARY_ASSERT_STR(lib->groups[0]->id, "archive");
    MARY_ASSERT_EQ(lib->groups[0]->n_documents, 1);
    MARY_ASSERT_STR(lib->groups[1]->id, "inbox");
    MARY_ASSERT_EQ(lib->groups[1]->n_documents, 0);
    thread_library_response_free(lib);
    destroy(s);
}

MARY_TEST(peering_is_declared_not_built) {
    MARY_ASSERT_EQ(thread_peer_discover(NULL, NULL, 10), -ENOSYS);
    MARY_ASSERT_EQ(thread_peer_pair(NULL, NULL, NULL), -ENOSYS);
    char id[65];
    MARY_ASSERT_EQ(thread_node_id_from_spki(NULL, 0, id), -ENOSYS);
    MARY_ASSERT_STR(THREAD_PEER_SERVICE, "_thread._tcp");
}

int main(void) {
    MARY_RUN(a_turn_is_indexed_listed_and_read_back);
    MARY_RUN(the_node_id_is_made_once);
    MARY_RUN(one_owner_cannot_touch_anothers);
    MARY_RUN(ids_must_be_file_names);
    MARY_RUN(groups_page_by_id);
    MARY_RUN(a_document_moves_to_its_new_group);
    MARY_RUN(peering_is_declared_not_built);
    MARY_TEST_MAIN_END();
}
