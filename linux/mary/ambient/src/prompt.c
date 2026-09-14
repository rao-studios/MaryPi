#include "ambient/prompt.h"

#include <stdio.h>
#include <string.h>

#include "common/buf.h"

void ma_live_work_from_world(const ma_world *world, const ma_roster *r, ma_live_work_world *out) {
    memset(out, 0, sizeof *out);
    if (!world) return;
    ma_place place = ma_world_place(world);
    snprintf(out->name, sizeof out->name, "%s", ma_place_display_name(&place, r));
    if (ma_world_is_direct_reference(world)) out->kind = MA_LIVE_WORK_DOCUMENT;
    else if (ma_place_is_application(&place)) out->kind = MA_LIVE_WORK_APPLICATION;
    else { out->kind = MA_LIVE_WORK_UNLED; out->name[0] = 0; }
}

void ma_live_work_from_surface(const ma_surface *surface, const ma_roster *r, ma_live_work_world *out) {
    memset(out, 0, sizeof *out);
    if (!surface) return;
    snprintf(out->name, sizeof out->name, "%s", ma_place_display_name(&surface->place, r));
    out->kind = surface->document_path[0] ? MA_LIVE_WORK_DOCUMENT : MA_LIVE_WORK_APPLICATION;
}

static const char WINDOW_SIGHT[] =
    "What you are shown is a WINDOW onto their work, never all of it \xE2\x80\x94 the text below says which characters of the "
    "whole it covers, and everything outside those bounds is simply text you have not been shown. Unseen is not absent. "
    "NEVER tell the user that a passage, a section or a subject isn't in their work because it isn't in this window: say "
    "plainly that it's outside the part you can see and that you're pulling it up, then answer from what comes back.";

static const char WHOLE_SIGHT[] =
    "You hold the WHOLE of that document, not a window onto it \xE2\x80\x94 all of its text is already in hand, so there is no "
    "part of THIS one that is \"outside what you can see\". NEVER say a passage or a subject is outside your window. But the "
    "user may keep SEVERAL open at once, and holding this one whole tells you nothing about the others: if something isn't "
    "here, it may simply be in another window. Never say it isn't in their documents \xE2\x80\x94 say it isn't in this one, and "
    "read the one that would have it.";

static const char UNLED_SIGHT[] =
    "You are NOT looking at their screen right now. Everything below is something you read earlier or just now, and it says "
    "so itself \xE2\x80\x94 so speak from it as something you read, never as something you can currently see. Do not name an app "
    "or a document as the thing in front of them: you do not know that here, and guessing it is how a question about one app "
    "gets answered about another.";

static const char HELD[] =
    "I am also still holding these from earlier in this conversation \xE2\x80\x94 I really read them, so they are not memories to "
    "hedge about, and each one states its own age. They do NOT supersede what is on screen above: where a held fact and the "
    "live text cover the same words, the live text is newer and wins, and anything past its stated age may have been edited "
    "since \xE2\x80\x94 give the age rather than asserting it is the wording now.";

static const char MENTIONS[] =
    "Also still held. Each one starts with a handle like [S1], which is how I pull that exact passage back up or change it "
    "\xE2\x80\x94 so I can promise to fetch it or fix it. Say what it is about, never the numbers: those are mine for finding it, "
    "and they are not something to read out or to work out for myself.";

int ma_prompt_live_work(const ma_rendering *rendering, const ma_live_work_world *world, bool inspired_sight, mc_buf *out) {
    if (ma_rendering_is_empty(rendering)) return 0;
    char place[MA_NAME_MAX + 64], sight_app[1024];
    const char *sight;
    switch (world ? world->kind : MA_LIVE_WORK_UNLED) {
    case MA_LIVE_WORK_DOCUMENT:
        if (world->name[0]) snprintf(place, sizeof place, "document open in front of them in %s", world->name);
        else snprintf(place, sizeof place, "document open in front of them");
        sight = world->whole ? WHOLE_SIGHT : WINDOW_SIGHT;
        break;
    case MA_LIVE_WORK_APPLICATION:
        if (world->name[0]) snprintf(place, sizeof place, "%s window open in front of them", world->name);
        else snprintf(place, sizeof place, "window open in front of them");
        snprintf(sight_app, sizeof sight_app,
                 "What you hold of it is what you have READ \xE2\x80\x94 the blocks below are real observations, each one stating its "
                 "own age, and anything you have not read is simply unread, not absent. NEVER tell the user something isn't there "
                 "because it isn't in what you hold: look again, or read the part they mean, and answer from what comes back. And "
                 "never describe this as a document in one of their writing apps \xE2\x80\x94 it is %s, and calling it anything else "
                 "is a claim about their screen you cannot make.", world->name[0] ? world->name : "this application");
        sight = sight_app;
        break;
    default:
        if (inspired_sight) { snprintf(place, sizeof place, "work they have selected on screen"); sight = WINDOW_SIGHT; }
        else { snprintf(place, sizeof place, "work they have in front of them"); sight = UNLED_SIGHT; }
        break;
    }
    mc_buf body = { 0 };
    int rc = 0;
    for (int i = 0; i < rendering->surface_count; i++) {
        if (body.len) rc |= mc_buf_append_str(&body, "\n\n");
        rc |= mc_buf_append_str(&body, rendering->surface_lines[i]);
    }
    if (rendering->block_count || rendering->mention_count) {
        if (body.len) rc |= mc_buf_append_str(&body, "\n\n");
        rc |= mc_buf_append_str(&body, HELD);
        for (int i = 0; i < rendering->block_count; i++) { rc |= mc_buf_append_str(&body, "\n\n"); rc |= mc_buf_append_str(&body, rendering->blocks[i]); }
        if (rendering->mention_count) {
            rc |= mc_buf_append_str(&body, "\n\n");
            rc |= mc_buf_append_str(&body, MENTIONS);
            for (int i = 0; i < rendering->mention_count; i++) { rc |= mc_buf_append_str(&body, "\n- "); rc |= mc_buf_append_str(&body, rendering->mentions[i]); }
        }
    }
    rc |= mc_buf_append_str(out, "\n\nYou can see what the user is looking at right now \xE2\x80\x94 this is the live ");
    rc |= mc_buf_append_str(out, place);
    rc |= mc_buf_append_str(out, ", and it is the ground truth for their work: base what you say about the wording on THIS, never on "
                                 "a retrieved memory or an earlier mention of some other file or document; it supersedes them.\n\n");
    rc |= mc_buf_append_str(out, sight);
    rc |= mc_buf_append_str(out, "\n\nWhen they ask about a particular passage \xE2\x80\x94 to check it, fix it, tighten it, read it back "
                                 "\xE2\x80\x94 quote it and work with it directly: that IS the answer. Otherwise reference it naturally "
                                 "in a sentence or two rather than reciting the whole thing unasked.\n\n");
    rc |= mc_buf_append(out, body.data ? body.data : (const unsigned char *)"", body.len);
    mc_buf_free(&body);
    return rc ? -1 : 0;
}

void ma_spawn_line(const ma_place *spawn, const ma_roster *r, char *out, size_t n) {
    out[0] = 0;
    if (!spawn || !ma_place_is_application(spawn)) return;
    const char *name = ma_place_display_name(spawn, r), *focus = ma_place_focus(spawn, r);
    if (focus && strcmp(focus, "writing") == 0)
        snprintf(out, n, "%s is not in front right now. It is the place for this: calling one of its skills brings it up, with a fresh document if it has none, "
                         "and your hands can write there directly \xE2\x80\x94 this voice pass is not writing as it speaks.", name);
    else
        snprintf(out, n, "%s is not in front right now. It is the place for this: calling one of its skills brings it up, and your hands can act "
                         "there directly \xE2\x80\x94 this voice pass is not acting as it speaks.", name);
}

void ma_capability_line(const ma_place *lead, const ma_roster *r, char *out, size_t n) {
    out[0] = 0;
    if (!lead || !ma_place_is_application(lead)) return;
    const char *name = ma_place_display_name(lead, r), *focus = ma_place_focus(lead, r);
    if (focus && strcmp(focus, "coding") == 0)
        snprintf(out, n, "Right now you're pair-coding with the user in %s \xE2\x80\x94 your hands can write new code into their project and revise "
                         "the code already there; this voice pass is not writing as it speaks.", name);
    else if (focus && strcmp(focus, "writing") == 0)
        snprintf(out, n, "Right now you're co-writing with the user in %s \xE2\x80\x94 your hands can write new prose into their document and "
                         "revise the words already there; this voice pass is not writing as it speaks.", name);
    else
        snprintf(out, n, "Right now you're working alongside the user in %s \xE2\x80\x94 your hands can act there directly; this voice pass is "
                         "not acting as it speaks.", name);
}
