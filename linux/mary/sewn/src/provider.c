#include "sewn/provider.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "sewn/mistral.h"

struct row {
    const char *name;
    const char *display;
    const char *host;           /* NULL: not served this phase */
    const char *chat_model;
};

static const struct row rows[] = {
    [SEWN_PROVIDER_MISTRAL] = { "mistral", "Mistral (Hosted)", SEWN_MISTRAL_HOST, SEWN_CHAT_MODEL },
    [SEWN_PROVIDER_TINKER] = { "tinker", "Thinking Machines (Hosted)", NULL, NULL },
};

int sewn_provider_parse(const char *name, sewn_provider *out) {
    if (!name || !*name) {
        *out = SEWN_PROVIDER_MISTRAL;
        return 0;
    }
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (strcasecmp(name, rows[i].name) == 0) {
            *out = (sewn_provider)i;
            return 0;
        }
    }
    return -EINVAL;
}

const char *sewn_provider_name(sewn_provider p) { return rows[p].name; }
const char *sewn_provider_display_name(sewn_provider p) { return rows[p].display; }
bool sewn_provider_available(sewn_provider p) { return rows[p].host != NULL; }
const char *sewn_provider_host(sewn_provider p) { return rows[p].host; }

/* Core/ModelConfig.swift: a model counts as Mistral's by its name. */
bool sewn_is_mistral_model(const char *name) {
    static const char *const prefixes[] = { "mistral", "open-mi", "ministral", "codestral" };
    if (!name) return false;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strncasecmp(name, prefixes[i], strlen(prefixes[i])) == 0) return true;
    return false;
}

const char *sewn_provider_chat_model(sewn_provider p, const char *requested) {
    if (p == SEWN_PROVIDER_MISTRAL && requested && *requested && sewn_is_mistral_model(requested)) return requested;
    return rows[p].chat_model;
}
