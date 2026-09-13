/* The skills a turn may call, rendered as Mistral `tools` from the registry the
 * desktop publishes (skills/). The conversation calling skills is the next
 * milestone; until then there are no tools, and the instructions promise none. */
#ifndef MARY_BRAIN_TOOLS_H
#define MARY_BRAIN_TOOLS_H

struct json_object;
struct sk_registry;

/* NULL: no tools this milestone. */
struct json_object *mb_tools(const struct sk_registry *registry);

#endif
