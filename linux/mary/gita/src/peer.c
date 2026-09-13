#include "gita/peer.h"

#include <errno.h>

int gita_peer_discover(gita_peer_found_fn found, void *user, int timeout_ms) { return -ENOSYS; }
int gita_peer_pair(const gita_peer *peer, const char *certificate_path, const char *key_path) { return -ENOSYS; }
int gita_node_id_from_spki(const unsigned char *spki_der, size_t len, char out[65]) { return -ENOSYS; }
