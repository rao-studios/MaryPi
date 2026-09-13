/* MaryFoundation/Core/ValueEnvelope.swift in C, with the pieces it names:
 * SourceScope (Core/SourceScope.swift), DataPrivacyClass (Core/ValueSchemas.swift)
 * and ValueProvenance. Declared only — nothing on MaryOS builds envelopes yet.
 * Strings are borrowed; the envelope owns `value`. */
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
