#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/io.h"
#include "common/peer.h"
#include "mary_test.h"

MARY_TEST(files_are_replaced_whole_and_privately) {
    char dir[] = "/tmp/mc-files-XXXXXX", path[64];
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/doc.json", dir);
    MARY_ASSERT_EQ(mc_write_file_atomic(path, "{\"v\":1}", 7, 0600), 0);
    MARY_ASSERT_EQ(mc_write_file_atomic(path, "{\"v\":22}", 8, 0600), 0);
    struct stat st;
    MARY_ASSERT_EQ(stat(path, &st), 0);
    MARY_ASSERT_EQ(st.st_mode & 0777, 0600);
    mc_buf b = { 0 };
    MARY_ASSERT_EQ(mc_read_file(path, &b, 64), 0);
    MARY_ASSERT_STR((char *)b.data, "{\"v\":22}");
    mc_buf_clear(&b);
    MARY_ASSERT_EQ(mc_read_file(path, &b, 4), -EFBIG);
    MARY_ASSERT_EQ(b.len, 0);
    MARY_ASSERT_EQ(mc_read_file("/tmp/no-such-mary-file", &b, 4), -ENOENT);
    mc_buf_free(&b);
    unlink(path);
    rmdir(dir);
}

MARY_TEST(unix_sockets_listen_connect_and_know_their_peer) {
    char dir[] = "/tmp/mc-sock-XXXXXX", path[64];
    MARY_ASSERT(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/s.sock", dir);
    int listener = mc_listen_unix(path, 0660);
    MARY_ASSERT(listener >= 0);
    int client = mc_connect_unix(path);
    MARY_ASSERT(client >= 0);
    uid_t uid = 0;
    MARY_ASSERT_EQ(mc_peer_credentials(client, &uid, NULL, NULL), 0);
    MARY_ASSERT_EQ(uid, geteuid());
    char name[64];
    mc_user_name(geteuid(), name, sizeof name);
    MARY_ASSERT(name[0] != 0);
    mc_user_name((uid_t)4000000000u, name, sizeof name);
    MARY_ASSERT_STR(name, "uid-4000000000");
    close(client);
    close(listener);
    unlink(path);
    rmdir(dir);
}

int main(void) {
    MARY_RUN(files_are_replaced_whole_and_privately);
    MARY_RUN(unix_sockets_listen_connect_and_know_their_peer);
    MARY_TEST_MAIN_END();
}
