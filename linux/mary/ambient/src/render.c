#include "ambient/render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void ma_age_string(double seconds, char *out, size_t n) {
    if (!isfinite(seconds) || seconds < 0) { snprintf(out, n, "an unknown time"); return; }
    if (seconds < 90) snprintf(out, n, "%ds", (int)lround(seconds));
    else if (seconds < 3600) snprintf(out, n, "%dm", (int)lround(seconds / 60));
    else if (seconds < 86400.0 * 30) snprintf(out, n, "%dh", (int)lround(seconds / 3600));
    else snprintf(out, n, "an unknown time");
}

size_t ma_utf8_count(const char *s) {
    size_t points = 0;
    for (; *s; s++) if ((*s & 0xC0) != 0x80) points++;
    return points;
}

void ma_utf8_prefix(const char *s, size_t points, char *out, size_t n, bool ellipsis) {
    size_t i = 0, count = 0;
    while (s[i] && count < points) {
        i++;
        while ((s[i] & 0xC0) == 0x80) i++;
        count++;
    }
    if (i >= n) i = n - 1;
    memcpy(out, s, i);
    out[i] = 0;
    if (ellipsis && s[i] && i + 4 < n) strcat(out, "\xE2\x80\xA6");
}

static void append(char *out, size_t n, const char *text) { strncat(out, text, n - strlen(out) - 1); }

void ma_surface_line(const ma_surface *s, double now, char *out, size_t n) {
    char line[MA_SURFACE_LINE_MAX * 2] = "On screen: ";
    append(line, sizeof line, s->app_name);
    if (s->has_window && s->window_title[0]) {
        append(line, sizeof line, " \xE2\x80\x94 \"");
        append(line, sizeof line, s->window_title);
        append(line, sizeof line, "\"");
    }
    char qualifiers[512] = "";
    if (s->window_count > 1) {
        char windows[96];
        snprintf(windows, sizeof windows, "front of %d windows", s->window_count);
        if (s->minimized_count > 0) snprintf(windows + strlen(windows), sizeof windows - strlen(windows), ", %d minimized", s->minimized_count);
        append(qualifiers, sizeof qualifiers, windows);
    }
    if (s->has_focused) {
        if (qualifiers[0]) append(qualifiers, sizeof qualifiers, "; ");
        append(qualifiers, sizeof qualifiers, "focused: ");
        append(qualifiers, sizeof qualifiers, ma_element_descriptor(&s->focused));
    }
    if (qualifiers[0]) { append(line, sizeof line, " ("); append(line, sizeof line, qualifiers); append(line, sizeof line, ")"); }
    if (s->element_count > 0) {
        char named[MA_NOTABLE_LIMIT][MA_LABEL_CAP + 1];
        int seen[MA_NOTABLE_LIMIT], named_count = 0;
        if (s->has_focused && s->focused.label[0]) { snprintf(named[0], sizeof named[0], "%s", s->focused.label); seen[0] = s->focused.ordinal; named_count = 1; }
        for (int i = 0; i < s->element_count && named_count < MA_NOTABLE_LIMIT; i++) {
            const ma_element *e = &s->elements[i];
            bool dup = false;
            for (int k = 0; k < named_count; k++) if (seen[k] == e->ordinal) dup = true;
            if (dup || !e->label[0]) continue;
            snprintf(named[named_count], sizeof named[0], "%s", e->label);
            seen[named_count++] = e->ordinal;
        }
        if (named_count) {
            append(line, sizeof line, " \xE2\x80\x94 offering: ");
            for (int i = 0; i < named_count; i++) { if (i) append(line, sizeof line, ", "); append(line, sizeof line, named[i]); }
            int remainder = s->element_count - named_count;
            if (remainder > 0) { char more[32]; snprintf(more, sizeof more, ", +%d more", remainder); append(line, sizeof line, more); }
        }
    }
    if (s->page_not_yet_read) append(line, sizeof line, " \xE2\x80\x94 page not yet read");
    char age[32];
    ma_age_string(ma_surface_age(s, now), age, sizeof age);
    append(line, sizeof line, " \xE2\x80\x94 seen ");
    append(line, sizeof line, age);
    append(line, sizeof line, " ago");
    if (ma_utf8_count(line) > MA_SURFACE_LINE_CAP) ma_utf8_prefix(line, MA_SURFACE_LINE_CAP - 1, out, n, true);
    else snprintf(out, n, "%s", line);
}

void ma_fact_slot_phrase(const ma_fact *f, const ma_roster *r, char *out, size_t n) {
    switch (f->slot.kind) {
    case MA_SLOT_NAMED_READ: snprintf(out, n, "the part about \"%s\"", f->slot.phrase); return;
    case MA_SLOT_SELECTION: snprintf(out, n, "their highlight"); return;
    case MA_SLOT_OBJECT_SELECTION: snprintf(out, n, "what they have selected"); return;
    case MA_SLOT_VIEWPORT: snprintf(out, n, "%s", f->anchor[0] ? f->anchor : "what they're looking at"); return;
    case MA_SLOT_CURSOR: snprintf(out, n, "%s", f->anchor[0] ? f->anchor : "their cursor"); return;
    case MA_SLOT_FILE: {
        const char *focus = ma_place_focus(&f->place, r);
        snprintf(out, n, "%s", focus && strcmp(focus, "coding") == 0 ? "the file in front of them" : "the document in front of them");
        return;
    }
    case MA_SLOT_PROJECT: snprintf(out, n, "the project"); return;
    case MA_SLOT_GIT: snprintf(out, n, "version control"); return;
    case MA_SLOT_DIGEST: snprintf(out, n, "how things stand right now"); return;
    case MA_SLOT_HEARD: snprintf(out, n, "something said near her"); return;
    case MA_SLOT_GLIMPSED: snprintf(out, n, "something she saw a moment ago"); return;
    default: snprintf(out, n, "what they're looking at"); return;
    }
}

void ma_fact_bounds_phrase(const ma_fact *f, char *out, size_t n) {
    size_t content = ma_utf8_count(f->content);
    if (f->has_bounds && f->document_total > 0 && f->lower >= 0 && f->upper <= f->document_total && f->lower < f->upper) {
        snprintf(out, n, "characters %d\xE2\x80\x93%d of %d", f->lower, f->upper, f->document_total);
    } else if (f->document_total > 0 && content < (size_t)f->document_total) {
        snprintf(out, n, "about %zu characters of %d", content, f->document_total);
    } else if (f->has_bounds && f->lower < f->upper) {
        snprintf(out, n, "characters %d\xE2\x80\x93%d", f->lower, f->upper);
    } else {
        out[0] = 0;
    }
}

void ma_fact_age_phrase(const ma_fact *f, double now, char *out, size_t n) {
    char age[32];
    ma_age_string(ma_fact_age(f, now), age, sizeof age);
    snprintf(out, n, "%s %s ago%s", ma_registered_verb(f->registration), age, ma_fact_is_fresh(f, now) ? "" : ", so it may have moved on since");
}

void ma_fact_mention_line(const ma_fact *f, double now, const ma_roster *r, char *out, size_t n) {
    char line[MA_MENTION_MAX + MA_CONTENT_CAP] = "";
    if (f->passage_handle[0]) { append(line, sizeof line, "["); append(line, sizeof line, f->passage_handle); append(line, sizeof line, "] "); }
    append(line, sizeof line, ma_place_display_name(&f->place, r));
    if (f->subject[0]) { append(line, sizeof line, " \xC2\xB7 "); append(line, sizeof line, f->subject); }
    char phrase[256], bounds[96], age[96];
    ma_fact_slot_phrase(f, r, phrase, sizeof phrase);
    append(line, sizeof line, " \xE2\x80\x94 ");
    append(line, sizeof line, phrase);
    ma_fact_bounds_phrase(f, bounds, sizeof bounds);
    if (bounds[0]) { append(line, sizeof line, ", "); append(line, sizeof line, bounds); }
    ma_fact_age_phrase(f, now, age, sizeof age);
    append(line, sizeof line, ", ");
    append(line, sizeof line, age);
    if (f->spoken_at > 0) append(line, sizeof line, "; already spoken about");
    if (f->slot.kind == MA_SLOT_DIGEST && f->content[0]) { append(line, sizeof line, ": "); append(line, sizeof line, f->content); }
    else append(line, sizeof line, ".");
    snprintf(out, n, "%s", line);
}

void ma_fact_block(const ma_fact *f, double now, const ma_roster *r, size_t limit, char *out, size_t n) {
    char mention[MA_MENTION_MAX + MA_CONTENT_CAP];
    ma_fact_mention_line(f, now, r, mention, sizeof mention);
    if (f->slot.kind == MA_SLOT_DIGEST && f->content[0]) {
        if (ma_utf8_count(mention) > limit) ma_utf8_prefix(mention, limit > 0 ? limit - 1 : 0, out, n, true);
        else snprintf(out, n, "%s", mention);
        return;
    }
    size_t ml = strlen(mention);
    if (ml && mention[ml - 1] == '.') mention[ml - 1] = 0;
    append(mention, sizeof mention, ":");
    size_t header = ma_utf8_count(mention);
    if (limit <= header + 1) { snprintf(out, n, "%s", mention); return; }
    size_t room = limit - header - 1;
    char body[MA_CONTENT_CAP + 8];
    if (ma_utf8_count(f->content) > room) ma_utf8_prefix(f->content, room > 0 ? room - 1 : 0, body, sizeof body, true);
    else snprintf(body, sizeof body, "%s", f->content);
    if (!body[0]) snprintf(out, n, "%s", mention);
    else snprintf(out, n, "%s\n%s", mention, body);
}
