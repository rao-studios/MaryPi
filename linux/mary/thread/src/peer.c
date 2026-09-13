#include "thread/peer.h"

#include <errno.h>

int thread_peer_discover(thread_peer_found_fn found, void *user, int timeout_ms) { return -ENOSYS; }
int thread_peer_pair(const thread_peer *peer, const char *certificate_path, const char *key_path) { return -ENOSYS; }
int thread_node_id_from_spki(const unsigned char *spki_der, size_t len, char out[65]) { return -ENOSYS; }
