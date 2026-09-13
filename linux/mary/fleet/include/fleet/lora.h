/* Fleet/Sources/FleetStore/FleetRegistry.swift (LoRAEntry) in C: one trained
 * adapter in fleet-db's registry. Strings are borrowed. */
#ifndef MARY_FLEET_LORA_H
#define MARY_FLEET_LORA_H

#include <stdbool.h>

typedef struct fleet_lora_entry {
    char cid[65];                   /* SHA-256 of the training input set, hex: the primary key */
    const char *label;              /* or NULL */
    const char *model_id;
    const char *dataset_id;         /* a UUID, or NULL */
    const char *schema_hash;        /* identity of the output schema: interchangeable LoRAs share it */
    const char *schema_description;
    int pair_count;
    int rank;
    float scale;
    int num_layers;
    int iterations;
    double created_at;              /* seconds since the Unix epoch */
    double updated_at;
    int generation;                 /* incremented each time this cid is retrained */
    const char *thread_id;          /* named slot: one LoRA per thread × ability; NULL for cid-only smoke LoRAs */
    const char *ability_id;
    double eval_exact_match;        /* share of held-out pairs reproduced exactly; NAN: never scored */
    int eval_cases;
} fleet_lora_entry;

/* A slotted entry names both a thread and an ability. */
bool fleet_lora_is_slotted(const fleet_lora_entry *entry);

#endif
