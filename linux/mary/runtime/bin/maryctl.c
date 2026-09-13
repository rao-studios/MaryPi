/* maryctl: maryd from a terminal, as another client of the desktop's socket.
 *
 *   maryctl status                      state, key, wake word
 *   maryctl ask TEXT...                 prints the reply as it streams (Mary speaks it too)
 *   maryctl listen                      opens the microphone, as the Ask Mary button does
 *   maryctl stop
 *   maryctl skills                      what each app lets Mary do, as the desktop published it
 *   maryctl skill APP SKILL [ARGS-JSON] one call through the direct pipes */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/io.h"
#include "common/json.h"
#include "common/lines.h"
#include "runtime/desktop.h"

struct ctl {
    const char *command;
    const char *question;
    bool seen_question, left_idle, done;
    int status;
};

static const char *or_else(const char *s, const char *fallback) { return s ? s : fallback; }

static void print_skills(struct json_object *msg) {
    struct json_object *apps = mc_json_array(msg, "apps");
    size_t n = apps ? json_object_array_length(apps) : 0;
    if (!n) printf("(the desktop has not published any skills)\n");
    for (size_t i = 0; i < n; i++) {
        struct json_object *app = json_object_array_get_idx(apps, i), *skills = mc_json_array(app, "skills");
        bool enabled = true;
        mc_json_bool(app, "enabled", &enabled);
        printf("%s  %s  (%s, ask %s)\n", or_else(mc_json_string(app, "id"), "?"), or_else(mc_json_string(app, "name"), ""),
               enabled ? "on" : "off", or_else(mc_json_string(app, "ask"), "changes"));
        for (size_t k = 0; skills && k < json_object_array_length(skills); k++) {
            struct json_object *skill = json_object_array_get_idx(skills, k);
            bool on = true;
            mc_json_bool(skill, "enabled", &on);
            printf("    %-20s %s  [%s]%s\n", or_else(mc_json_string(skill, "id"), "?"), or_else(mc_json_string(skill, "title"), ""),
                   or_else(mc_json_string(skill, "effect"), "act"), on ? "" : " (off)");
        }
    }
}

static void conversation(struct ctl *c, const char *type, struct json_object *msg) {
    if (strcmp(type, "transcript") == 0) {
        bool final = false;
        mc_json_bool(msg, "final", &final);
        const char *text = or_else(mc_json_string(msg, "text"), "");
        if (!final) return;
        if (c->question) c->seen_question = c->seen_question || strcmp(text, c->question) == 0;
        else {
            c->seen_question = true;
            printf("you: %s\nmary: ", text);
            fflush(stdout);
        }
    } else if (strcmp(type, "reply.delta") == 0 && c->seen_question) {
        fputs(or_else(mc_json_string(msg, "text"), ""), stdout);
        fflush(stdout);
    } else if (strcmp(type, "reply.end") == 0 && c->seen_question) {
        bool cancelled = false;
        mc_json_bool(msg, "cancelled", &cancelled);
        printf("\n");
        c->status = cancelled ? 1 : c->status;
        c->done = true;
    } else if (strcmp(type, "error") == 0) {
        fprintf(stderr, "maryctl: %s: %s\n", or_else(mc_json_string(msg, "stage"), "error"), or_else(mc_json_string(msg, "message"), ""));
        c->status = 1;
    } else if (strcmp(type, "state") == 0) {
        const char *state = or_else(mc_json_string(msg, "state"), "");
        if (!c->question && isatty(2)) fprintf(stderr, "[%s]\n", state);
        if (strcmp(state, "error") == 0) {
            c->status = 1;
            c->done = true;
        } else if (strcmp(state, "idle") == 0) {
            c->done = c->left_idle;
        } else {
            c->left_idle = true;
        }
    }
}

static int on_line(const char *line, size_t len, void *user) {
    struct ctl *c = user;
    struct json_object *msg = mc_json_parse(line, len);
    const char *type = mc_json_type(msg);
    if (!type) {
    } else if (strcmp(c->command, "status") == 0) {
        if (strcmp(type, "hello") == 0) {
            bool key = false, wake = false;
            mc_json_bool(msg, "key_present", &key);
            mc_json_bool(msg, "wake", &wake);
            printf("state:     %s\nkey:       %s\nwake word: %s\n", or_else(mc_json_string(msg, "state"), "?"),
                   key ? "stored" : "not set (Settings › Mary)", wake ? "listening for \"Hey Mary\"" : "off");
            c->done = true;
        }
    } else if (strcmp(c->command, "ask") == 0 || strcmp(c->command, "listen") == 0) {
        conversation(c, type, msg);
    } else if (strcmp(c->command, "skills") == 0) {
        if (strcmp(type, "skills") == 0) {
            print_skills(msg);
            c->done = true;
        }
    } else if (strcmp(c->command, "skill") == 0) {
        if (strcmp(type, "skill.result") == 0) {
            bool ok = false;
            mc_json_bool(msg, "ok", &ok);
            struct json_object *result = mc_json_object(msg, "result");
            if (ok) printf("%s\n", result ? mc_json_compact(result, NULL) : "ok");
            else fprintf(stderr, "maryctl: %s\n", or_else(mc_json_string(msg, "error"), "failed"));
            c->status = ok ? 0 : 1;
            c->done = true;
        } else if (strcmp(type, "error") == 0) {
            fprintf(stderr, "maryctl: %s\n", or_else(mc_json_string(msg, "message"), "error"));
            c->status = 1;
            c->done = true;
        }
    }
    if (msg) json_object_put(msg);
    return c->done;
}

static void usage(FILE *to) {
    fprintf(to, "usage: maryctl [--socket PATH] status | ask TEXT... | listen | stop | skills | skill APP SKILL [ARGS-JSON]\n");
}

int main(int argc, char **argv) {
    char path[256];
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "--socket") == 0) {
        snprintf(path, sizeof path, "%s", argv[i + 1]);
        i += 2;
    } else if (mr_desktop_default_socket(path, sizeof path, false) < 0) {
        fprintf(stderr, "maryctl: no XDG_RUNTIME_DIR; pass --socket\n");
        return 1;
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    struct ctl c = { .command = argv[i] };
    struct json_object *request = NULL;
    mc_buf question = { 0 };
    const char *cmd = argv[i];
    if (strcmp(cmd, "ask") == 0 && i + 1 < argc) {
        for (int k = i + 1; k < argc; k++) {
            if (k > i + 1) mc_buf_append_str(&question, " ");
            mc_buf_append_str(&question, argv[k]);
        }
        c.question = (const char *)question.data;
        request = json_object_new_object();
        json_object_object_add(request, "type", json_object_new_string("ask"));
        json_object_object_add(request, "text", json_object_new_string(c.question));
    } else if ((strcmp(cmd, "listen") == 0 || strcmp(cmd, "stop") == 0 || strcmp(cmd, "skills") == 0) && i + 1 == argc) {
        request = json_object_new_object();
        json_object_object_add(request, "type", json_object_new_string(strcmp(cmd, "skills") == 0 ? "skills.list" : cmd));
    } else if (strcmp(cmd, "skill") == 0 && (argc - i == 3 || argc - i == 4)) {
        request = json_object_new_object();
        json_object_object_add(request, "type", json_object_new_string("skill.call"));
        json_object_object_add(request, "app", json_object_new_string(argv[i + 1]));
        json_object_object_add(request, "skill", json_object_new_string(argv[i + 2]));
        if (argc - i == 4) {
            struct json_object *args = mc_json_parse(argv[i + 3], strlen(argv[i + 3]));
            if (!args) {
                fprintf(stderr, "maryctl: the arguments are not JSON\n");
                json_object_put(request);
                return 2;
            }
            json_object_object_add(request, "args", args);
        }
    } else if (strcmp(cmd, "status") != 0 || i + 1 != argc) {
        usage(stderr);
        return 2;
    }

    mc_ignore_sigpipe();
    int fd = mc_connect_unix(path);
    if (fd < 0) {
        fprintf(stderr, "maryctl: cannot reach maryd at %s: %s\n", path, strerror(-fd));
        if (request) json_object_put(request);
        return 1;
    }
    bool long_wait = strcmp(cmd, "ask") == 0 || strcmp(cmd, "listen") == 0;
    struct timeval patience = { .tv_sec = long_wait ? 120 : 15 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
    if (request) {
        size_t len = 0;
        const char *text = mc_json_compact(request, &len);
        if (mc_write_all(fd, text, len) < 0 || mc_write_all(fd, "\n", 1) < 0) {
            fprintf(stderr, "maryctl: maryd hung up\n");
            close(fd);
            return 1;
        }
        json_object_put(request);
    }
    if (strcmp(cmd, "stop") == 0) {
        close(fd);
        return 0;
    }
    mc_line_reader lines;
    mc_line_reader_init(&lines, MR_LINE_MAX);
    char bytes[16384];
    while (!c.done) {
        ssize_t n = read(fd, bytes, sizeof bytes);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            fprintf(stderr, "maryctl: %s\n", n == 0 ? "maryd hung up" : errno == EAGAIN ? "no answer from maryd" : strerror(errno));
            c.status = 1;
            break;
        }
        mc_line_reader_feed(&lines, bytes, (size_t)n, on_line, &c);
    }
    mc_line_reader_free(&lines);
    mc_buf_free(&question);
    close(fd);
    return c.status;
}
