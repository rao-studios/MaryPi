/* The record families: the sister schema every document on the hard drive is
 * stored under. On the Mac the family is read back from a document id's prefix
 * (MaryRuntime/Services/Servers/ThreadAddressFamilies.swift), because Thread
 * returns neither tags nor metadata; here every document carries its family, and
 * this table — declared once, shown by the Thread app's Schemas tab and
 * `threadctl schemas` — names the id and group patterns, the fields, the writer
 * and the lane each family belongs to. Classification of an id that arrived with
 * no family falls back to the longest matching pattern. The conversation, interaction,
 * ability, ability-schema and application families were retired (2026-09): Mary is the
 * operating system and already knows what the applications can do, and Sewn's memory
 * covers the conversation; a store still holding them is cleaned at open (user_version 2). */
#ifndef MARY_THREAD_FAMILIES_H
#define MARY_THREAD_FAMILIES_H

#include <stddef.h>

struct json_object;

typedef struct thread_family {
    const char *name;           /* "file", "memory", "behavior", "style", "routing" */
    const char *label;
    const char *description;
    const char *id_prefix;      /* "" when the id says nothing */
    const char *group_prefix;   /* "" when the group says nothing */
    const char *lane;           /* personal | behavioral | "" */
    const char *writer;         /* indexd | maryd | sewnd | desktop | — */
    const char *fields;         /* JSON: [{name, type, description}] */
    const char *graph;          /* prose: what the family adds to the graph */
} thread_family;

extern const thread_family thread_families[];
extern const size_t thread_family_count;

#define THREAD_LANE_COUNT 2
extern const char *const thread_lanes[THREAD_LANE_COUNT];   /* personal (file, memory, style), behavioral (behavior, routing) */

/* By name; NULL when unknown. */
const thread_family *thread_family_named(const char *name);
/* The family for a document: metadata's "family" when it names one, else the longest
 * id prefix, else the longest group prefix, else "unknown". `metadata` may be NULL. */
const char *thread_family_of(const char *document_id, const char *group_id, const char *metadata_json, size_t metadata_len);
/* The lane a family belongs to, or "" . */
const char *thread_family_lane(const char *family);
/* A JSON array of every family (name, label, description, id_prefix, group_prefix,
 * lane, writer, fields, graph). A new reference. */
struct json_object *thread_families_json(void);
/* Whether `lane` is one of the two. */
int thread_lane_valid(const char *lane);

#endif
