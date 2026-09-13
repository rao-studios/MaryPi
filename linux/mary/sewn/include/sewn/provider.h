/* Which backend answers a generation (Core/LLMProvider.swift): the one enum every
 * inference op switches on. Clients send it per request (`provider`); absent, Mistral.
 * The raw values are the wire, shared with Mary's LLMEngineChoice: "mistral" | "tinker".
 *
 * Only Mistral's row is filled this phase. Thinking Machines is a toggle on the desktop
 * and a name on the wire; a request naming it is answered with an engine error before
 * any socket is opened, so the later tinker.c adds a row here, not a wire change. */
#ifndef MARY_SEWN_PROVIDER_H
#define MARY_SEWN_PROVIDER_H

#include <stdbool.h>

typedef enum sewn_provider {
    SEWN_PROVIDER_MISTRAL = 0,
    SEWN_PROVIDER_TINKER = 1,
} sewn_provider;

/* Internal one-shots — compaction, the auto-memory note, graph extraction — run on
 * this whatever the provider (ModelConfig.defaultUtilityModel). */
#define SEWN_UTILITY_MODEL "mistral-tiny"
#define SEWN_ENGINE_UNAVAILABLE "Thinking Machines is not available yet"

/* NULL or "" → Mistral (Sewn's serverDefault). "tinker" → Tinker; any other value is
 * -EINVAL with *out untouched. */
int sewn_provider_parse(const char *name, sewn_provider *out);
const char *sewn_provider_name(sewn_provider p);
const char *sewn_provider_display_name(sewn_provider p);
/* Whether sewnd can serve it now: true for Mistral only. */
bool sewn_provider_available(sewn_provider p);
/* The hosted API's host, for the calls ledger; NULL when the row is not filled. */
const char *sewn_provider_host(sewn_provider p);
/* The provider's chat model, or the requested one when it belongs to that provider
 * (ModelConfig.resolveChatModel). */
const char *sewn_provider_chat_model(sewn_provider p, const char *requested);
bool sewn_is_mistral_model(const char *name);

#endif
