/* Gita on MaryOS: the way out of the hard drive. Retrieval by other machines —
 * a peer Thread node asking this one for what it holds, and this one asking
 * them — with the accounting Sewn's Gita keeps (attribution spans ride in sewnd
 * today; royalties, the wallet and pricing are not ported). Declared for the
 * milestone that builds it; every call answers -ENOSYS until then.
 *
 *   discovery   mDNS / DNS-SD, service type _thread._tcp, the node id in TXT
 *   pairing     mutual TLS; a node's id is the SHA-256 of its certificate's
 *               SubjectPublicKeyInfo, proven by the handshake rather than claimed
 *               (a Swift Thread node simply asserts a UUID)
 *   sync        conduit over that TLS connection */
#ifndef MARY_GITA_PEER_H
#define MARY_GITA_PEER_H

#include <stddef.h>

#define GITA_PEER_SERVICE "_thread._tcp"

typedef struct gita_peer {
    char node_id[65];       /* hex SHA-256 of the SPKI */
    char host[256];
    int port;
} gita_peer;

typedef void (*gita_peer_found_fn)(const gita_peer *peer, void *user);

int gita_peer_discover(gita_peer_found_fn found, void *user, int timeout_ms);
int gita_peer_pair(const gita_peer *peer, const char *certificate_path, const char *key_path);
int gita_node_id_from_spki(const unsigned char *spki_der, size_t len, char out[65]);

#endif
