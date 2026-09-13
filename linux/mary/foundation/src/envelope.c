#include "foundation/envelope.h"

#include <string.h>

static const char *const privacy_names[] = { "publicDefinition", "private", "sensitive", "secret" };
static const char *const resolution_names[] = { "unresolved", "device", "application", "window", "workspace", "document" };

const char *mf_privacy_name(mf_privacy privacy) {
    return privacy >= 0 && privacy < (int)(sizeof privacy_names / sizeof privacy_names[0]) ? privacy_names[privacy] : NULL;
}

bool mf_privacy_from_name(const char *name, mf_privacy *out) {
    for (size_t i = 0; name && i < sizeof privacy_names / sizeof privacy_names[0]; i++) {
        if (strcmp(privacy_names[i], name) == 0) {
            *out = (mf_privacy)i;
            return true;
        }
    }
    return false;
}

const char *mf_source_resolution_name(mf_source_resolution resolution) {
    return resolution >= 0 && resolution < (int)(sizeof resolution_names / sizeof resolution_names[0]) ? resolution_names[resolution] : NULL;
}

static bool present(const char *s) { return s && *s; }

mf_source_resolution mf_source_scope_resolution(const mf_source_scope *scope) {
    if (present(scope->document_id)) return MF_SOURCE_DOCUMENT;
    if (present(scope->workspace_id) || present(scope->project_id)) return MF_SOURCE_WORKSPACE;
    if (present(scope->window_id)) return MF_SOURCE_WINDOW;
    if (present(scope->application_id)) return MF_SOURCE_APPLICATION;
    if (present(scope->device_id)) return MF_SOURCE_DEVICE;
    return MF_SOURCE_UNRESOLVED;
}
