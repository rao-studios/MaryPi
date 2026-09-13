#if defined(__APPLE__)
#define _DARWIN_C_SOURCE    /* mkdtemp, pread under _POSIX_C_SOURCE */
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mary_test.h"
#include "sewn/key.h"
#include "sewn/peer.h"

static char dir[64];

static void fresh_dir(void) {
    snprintf(dir, sizeof dir, "/tmp/sewn-key-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
}

static void remove_dir(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/mistral.verified", dir);
    unlink(path);
    rmdir(dir);
}

static const char *KEY = "abcdEFGH1234ijklMNOP5678";

MARY_TEST(only_plausible_keys_are_accepted) {
    MARY_ASSERT(sewn_key_valid(KEY, strlen(KEY)));
    MARY_ASSERT(!sewn_key_valid("short", 5));
    MARY_ASSERT(!sewn_key_valid("has a space in the middle", 25));
    MARY_ASSERT(!sewn_key_valid("quote\"inside-the-key", 20));
    MARY_ASSERT(!sewn_key_valid("slash\\inside-the-key", 20));
    MARY_ASSERT(!sewn_key_valid("line\nbreak-inside-key", 21));
    MARY_ASSERT(!sewn_key_valid(NULL, 0));
}

MARY_TEST(a_stored_key_is_private_and_reads_back) {
    fresh_dir();
    sewn_key_store s;
    sewn_key_store_init(&s, dir);
    char out[SEWN_KEY_MAX + 1];
    MARY_ASSERT(!sewn_key_store_present(&s));
    MARY_ASSERT_EQ(sewn_key_store_get(&s, out, sizeof out), -ENOENT);
    MARY_ASSERT_EQ(sewn_key_store_set(&s, KEY, strlen(KEY)), 0);
    MARY_ASSERT(sewn_key_store_present(&s));
    char path[128];
    struct stat st;
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT_EQ(st.st_mode & 0777, 0600);
    MARY_ASSERT_EQ(sewn_key_store_get(&s, out, sizeof out), 0);
    MARY_ASSERT_STR(out, KEY);
    MARY_ASSERT_EQ(sewn_key_store_set(&s, "not a key", 9), -EINVAL);
    MARY_ASSERT_EQ(sewn_key_store_get(&s, out, sizeof out), 0);
    MARY_ASSERT_STR(out, KEY);   /* the stored key survives a bad replacement */
    remove_dir();
}

MARY_TEST(a_key_anyone_else_can_read_is_refused) {
    fresh_dir();
    sewn_key_store s;
    sewn_key_store_init(&s, dir);
    MARY_ASSERT_EQ(sewn_key_store_set(&s, KEY, strlen(KEY)), 0);
    char path[128], out[SEWN_KEY_MAX + 1];
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    MARY_ASSERT_EQ(chmod(path, 0640), 0);
    memset(out, 'x', sizeof out);
    MARY_ASSERT_EQ(sewn_key_store_get(&s, out, sizeof out), -EPERM);
    MARY_ASSERT_EQ(out[0], 0);
    remove_dir();
}

MARY_TEST(verification_is_remembered_until_the_key_changes) {
    fresh_dir();
    sewn_key_store s;
    sewn_key_store_init(&s, dir);
    MARY_ASSERT_EQ(sewn_key_store_set(&s, KEY, strlen(KEY)), 0);
    MARY_ASSERT_EQ(sewn_key_store_verified_at(&s), 0);
    MARY_ASSERT_EQ(sewn_key_store_mark_verified(&s, 1757700000000LL), 0);
    MARY_ASSERT_EQ(sewn_key_store_verified_at(&s), 1757700000000LL);
    MARY_ASSERT_EQ(sewn_key_store_set(&s, "zyxwVUTS9876rqpoNMLK", 20), 0);
    MARY_ASSERT_EQ(sewn_key_store_verified_at(&s), 0);
    remove_dir();
}

MARY_TEST(peers_are_who_the_kernel_says) {
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    sewn_peer peer;
    MARY_ASSERT_EQ(sewn_peer_of(sv[0], &peer), 0);
    MARY_ASSERT(peer.known);
    MARY_ASSERT_EQ(peer.uid, geteuid());
    close(sv[0]);
    close(sv[1]);
}

static bool only_1000_administers(uid_t uid, const char *group) { return uid == 1000 && strcmp(group, "sudo") == 0; }

MARY_TEST(only_the_admin_group_may_set_the_key) {
    sewn_peer mary = { .uid = 1000, .known = true }, guest = { .uid = 1001, .known = true };
    sewn_peer root = { .uid = 0, .known = true }, unknown = { 0 };
    MARY_ASSERT(sewn_peer_may(&mary, "key.set", "sudo", only_1000_administers));
    MARY_ASSERT(!sewn_peer_may(&guest, "key.set", "sudo", only_1000_administers));
    MARY_ASSERT(sewn_peer_may(&guest, "key.status", "sudo", only_1000_administers));
    MARY_ASSERT(sewn_peer_may(&guest, "turn.start", "sudo", only_1000_administers));
    MARY_ASSERT(sewn_peer_may(&root, "key.set", "sudo", NULL));
    MARY_ASSERT(!sewn_peer_may(&unknown, "key.status", "sudo", only_1000_administers));
    MARY_ASSERT(!sewn_uid_in_group(geteuid(), "no-such-group-for-mary"));
}

int main(void) {
    MARY_RUN(only_plausible_keys_are_accepted);
    MARY_RUN(a_stored_key_is_private_and_reads_back);
    MARY_RUN(a_key_anyone_else_can_read_is_refused);
    MARY_RUN(verification_is_remembered_until_the_key_changes);
    MARY_RUN(peers_are_who_the_kernel_says);
    MARY_RUN(only_the_admin_group_may_set_the_key);
    MARY_TEST_MAIN_END();
}
