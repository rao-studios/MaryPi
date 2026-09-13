/* The realtime transport sewnd uses: libwebsockets, one context and service
 * thread per session, wss:// to api.mistral.ai with the server certificate
 * verified against the system CA bundle (never self-signed, expired or with the
 * hostname check skipped) and the key sent once, as an Authorization header. */
#ifndef MARY_SEWN_WS_H
#define MARY_SEWN_WS_H

#include "sewn/transport.h"

#ifdef HAVE_LWS
extern const sewn_ws_ops sewn_lws_ops;
#endif

#endif
