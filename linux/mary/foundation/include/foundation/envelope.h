/* MaryFoundation/Core/ValueEnvelope.swift in C, with the pieces it names:
 * SourceScope (Core/SourceScope.swift), DataPrivacyClass (Core/ValueSchemas.swift)
 * and ValueProvenance. Strings are borrowed; the envelope owns `value`. The
 * resolution ladder is SourceScope.resolution: a document outranks a workspace or
 * project, a window, an application, a device; nothing proven is unresolved. */
#ifndef MARY_FOUNDATION_ENVELOPE_H
#define MARY_FOUNDATION_ENVELOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/value.h"

typedef enum mf_privacy {
    MF_PRIVACY_PUBLIC_DEFINITION,
    MF_PRIVACY_PRIVATE,
    MF_PRIVACY_SENSITIVE,
    MF_PRIVACY_SECRET,
} mf_privacy;

/* The Swift raw value ("publicDefinition", "private", …), or NULL out of range. */
const char *mf_privacy_name(mf_privacy privacy);
bool mf_privacy_from_name(const char *name, mf_privacy *out);

typedef enum mf_source_resolution {
    MF_SOURCE_UNRESOLVED,
    MF_SOURCE_DEVICE,
    MF_SOURCE_APPLICATION,
    MF_SOURCE_WINDOW,
    MF_SOURCE_WORKSPACE,
    MF_SOURCE_DOCUMENT,
} mf_source_resolution;

const char *mf_source_resolution_name(mf_source_resolution resolution);

/* Where a value came from. On MaryOS the application and window are the
 * desktop's own ids (lp_app.id, the window record), never an AX element. */
typedef struct mf_source_scope {
    const char *device_id;
    const char *application_id;
    int32_t process_id;
    bool has_process_id;
    const char *process_epoch;
    uint64_t activation_sequence;
    bool has_activation_sequence;
    const char *window_id;
    const char *workspace_id;
    const char *project_id;
    const char *document_id;
    const char *surface_id;
} mf_source_scope;

/* How far the scope is proven; an empty string counts as absent. */
mf_source_resolution mf_source_scope_resolution(const mf_source_scope *scope);

typedef struct mf_provenance {
    const char *adapter_id;
    const char *operation;
    const char *interaction_id;
    const char *perception_id;
    const char *const *parent_value_ids;
    size_t parent_value_count;
} mf_provenance;

typedef struct mf_envelope {
    char id[37];                    /* a UUID string */
    const char *type_id;            /* ValueTypeID */
    const char *schema_version;     /* SemanticVersion's raw value, "1.0.0" */
    mf_value *value;
    mf_source_scope scope;
    mf_provenance provenance;
    mf_privacy privacy;
    double created_at;              /* seconds since the Unix epoch */
    double expires_at;              /* NAN: never */
} mf_envelope;

#endif
