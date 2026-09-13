#include <errno.h>

#include "gita/peer.h"
#include "mary_test.h"

MARY_TEST(peering_is_declared_not_built) {
    MARY_ASSERT_EQ(gita_peer_discover(NULL, NULL, 10), -ENOSYS);
    MARY_ASSERT_EQ(gita_peer_pair(NULL, NULL, NULL), -ENOSYS);
    char id[65];
    MARY_ASSERT_EQ(gita_node_id_from_spki(NULL, 0, id), -ENOSYS);
    MARY_ASSERT_STR(GITA_PEER_SERVICE, "_thread._tcp");
}

int main(void) {
    MARY_RUN(peering_is_declared_not_built);
    MARY_TEST_MAIN_END();
}
