/* Direct pipes into MaryOS's applications. Swift's MaryComputerUse walks the
 * accessibility tree, captures pixels and posts input events; MaryOS's apps are its
 * own, so a skill call is a message instead (PORTING.md deviation 9):
 *
 *   maryd → desktop   skill.invoke{call_id, app, skill, args}
 *   desktop → maryd   skill.result{call_id, ok, result} or {call_id, ok: false,
 *                     error: "denied" | "needs_confirmation" | "failed" | "unknown"}
 *
 * The desktop calls the app's perform hook, the same code its menus run. This file is
 * maryd's side — call ids, matching results to calls, timeouts and a lost connection —
 * independent of how the bytes travel: the caller hands it a send function and feeds it
 * what arrives. Callbacks run on the thread that delivered the result, tick or
 * disconnect, never while a lock is held. */
#ifndef MARY_COMPUTER_USE_INVOKE_H
#define MARY_COMPUTER_USE_INVOKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct json_object;

typedef struct mcu_result {
    const char *call_id;
    bool ok;
    const char *error;              /* when !ok: the desktop's, or "timeout" / "disconnected" */
    struct json_object *result;     /* when ok; borrowed for the callback */
} mcu_result;

typedef void (*mcu_result_fn)(const mcu_result *result, void *user);
/* Writes one message to the desktop. 0, or -errno. */
typedef int (*mcu_send_fn)(struct json_object *message, void *user);

typedef struct mcu_pipes mcu_pipes;

mcu_pipes *mcu_pipes_new(mcu_send_fn send, void *send_user);
/* Ends every pending call "disconnected", then frees. */
void mcu_pipes_free(mcu_pipes *pipes);

/* Sends skill.invoke. `args` is borrowed (a reference is taken) and may be NULL.
 * The call id is copied into call_id when given. 0, or -errno when the message could
 * not be sent (done is not called then). */
int mcu_invoke(mcu_pipes *pipes, const char *app, const char *skill, struct json_object *args, int timeout_ms,
               mcu_result_fn done, void *user, char *call_id, size_t cap);

/* Feeds a message from the desktop. 1 when it finished a call, 0 when it is not a
 * skill.result, -ENOENT for a result nobody is waiting for (late, or unknown). */
int mcu_pipes_on_message(mcu_pipes *pipes, struct json_object *message);
/* Ends calls past their deadline "timeout". */
void mcu_pipes_tick(mcu_pipes *pipes, int64_t now_ms);
/* The connection went away: every pending call ends "disconnected". */
void mcu_pipes_disconnect(mcu_pipes *pipes);
size_t mcu_pipes_pending(const mcu_pipes *pipes);

/* Declared for reading app state through the same pipes; -ENOSYS for now. */
int mcu_app_state(mcu_pipes *pipes, const char *app, mcu_result_fn done, void *user);

#endif
