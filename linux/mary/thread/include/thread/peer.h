/* Peering between MaryOS machines — Thread as MaryOS's hard drive, connecting to
 * the other MaryOS nodes on the network by itself. Declared for the milestone that
 * builds it; every call answers -ENOSYS until then.
 *
 *   discovery   mDNS / DNS-SD, service type _thread._tcp, the node id in TXT
 *   pairing     mutual TLS; a node's id is the SHA-256 of its certificate's
 *               SubjectPublicKeyInfo, proven by the handshake rather than claimed
 *               (a Swift Thread node simply asserts a UUID)
 *   sync        conduit over that TLS connection */
#ifndef MARY_THREAD_PEER_H
#define MARY_THREAD_PEER_H

#include <stddef.h>

#define THREAD_PEER_SERVICE "_thread._tcp"

typedef struct thread_peer {
    char node_id[65];       /* hex SHA-256 of the SPKI */
    char host[256];
    int port;
} thread_peer;

typedef void (*thread_peer_found_fn)(const thread_peer *peer, void *user);

int thread_peer_discover(thread_peer_found_fn found, void *user, int timeout_ms);
int thread_peer_pair(const thread_peer *peer, const char *certificate_path, const char *key_path);
int thread_node_id_from_spki(const unsigned char *spki_der, size_t len, char out[65]);

#endif
