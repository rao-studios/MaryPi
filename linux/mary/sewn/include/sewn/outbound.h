/* Every request sewnd sends, through the service's transport seams and into the calls
 * ledger: a purpose, a request id, the timing, the status and the byte counts — the
 * key only in a header, the body never recorded. */
#ifndef MARY_SEWN_OUTBOUND_H
#define MARY_SEWN_OUTBOUND_H

#include "common/buf.h"
#include "sewn/provider.h"
#include "sewn/server.h"

typedef struct sewn_outbound {
    sewn_provider provider;
    const char *purpose;
    const char *request_id;     /* or NULL */
} sewn_outbound;

/* svc->post_stream with a ledger row. The provider's host is the row's host. */
int sewn_post(sewn_service *svc, const sewn_outbound *o, const char *path, const char *key, const char *body, size_t body_len,
              sewn_bytes_fn on_bytes, sewn_stop_fn should_stop, void *user, long *status, char *message, size_t cap);
/* svc->get with a ledger row. */
int sewn_get(sewn_service *svc, const sewn_outbound *o, const char *path, const char *key, mc_buf *body, size_t max, long *status,
             char *message, size_t cap);
/* A row for a call made outside these wrappers (key.verify, the WebSocket). */
void sewn_record_call(sewn_service *svc, const sewn_outbound *o, const char *path, long status, int64_t ms, int64_t bytes_out,
                      int64_t bytes_in, const char *outcome);

#endif
