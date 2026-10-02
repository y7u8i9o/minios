/* imed: the input method daemon (I1, docs/design/ime.md).  It binds the
 * input method of the seat through protocol/ime.xml, announces its
 * engines, and gives each key of the active context to the selected
 * engine.  The reply to the key follows the changes that the engine made,
 * so the compositor applies them before it gives a passed key to the
 * client.
 *
 *   imed        the engines of the configuration
 *   imed -t     the test engine of the boot tests as well */
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <wire/client.h>
#include <gui/keymap.h>
#include <minios/conf.h>
#include "core-client.h"
#include "seat-client.h"
#include "ime-client.h"
#include "imed.h"

struct wire_display *display;
struct wire_proxy *compositor, *shm, *im;
static struct wire_proxy *seat, *manager, *pointer;
static struct keymap *keymap;
static uint32_t depressed, locked, group;
static int active;
static uint32_t serial_done;

static const struct imed_engine *engines[8];
static int nengines;
static const struct imed_engine *current;

struct imed_table imed_table;
static char pending_commit[1024], pending_preedit[512];
static int preedit_changed, preedit_cursor, table_dirty;
static char shown_preedit[512];

/* ---- changes of the composition ---- */

void imed_commit(const char *text)
{
    strlcat(pending_commit, text, sizeof pending_commit);
}

void imed_preedit(const char *text, int cursor)
{
    strlcpy(pending_preedit, text, sizeof pending_preedit);
    preedit_cursor = cursor < 0 ? (int)strlen(pending_preedit) : cursor;
    preedit_changed = strcmp(pending_preedit, shown_preedit) != 0 || cursor >= 0;
}

void imed_table_changed(void)
{
    table_dirty = 1;
}

void imed_set_label(const char *label)
{
    if (im && current)
        input_method_set_status(im, current->name, label);
}

/* flush sends the changes of the last event together. */
static void flush(void)
{
    if (im && (pending_commit[0] || preedit_changed)) {
        if (pending_commit[0])
            input_method_commit_string(im, pending_commit);
        if (preedit_changed) {
            input_method_preedit_string(im, pending_preedit, preedit_cursor, preedit_cursor);
            strlcpy(shown_preedit, pending_preedit, sizeof shown_preedit);
        }
        input_method_commit(im, serial_done);
    }
    pending_commit[0] = '\0';
    preedit_changed = 0;
    if (table_dirty) {
        window_update(active && imed_table.n > 0);
        table_dirty = 0;
    }
}

static void clear_table(void)
{
    imed_table.n = imed_table.cursor = 0;
    imed_table.aux[0] = '\0';
    table_dirty = 1;
}

/* ---- configuration ---- */

/* imed_config returns a value of the desktop configuration, read again
 * when the file changed. */
const char *imed_config(const char *key)
{
    static char text[4096];
    static long stamp = -1;
    static char value[128];
    char path[256];
    struct stat st;
    conf_read_path(path, sizeof path);
    long now = stat(path, &st) == 0 ? (long)st.st_mtime * 1000003L + (long)st.st_size : 0;
    if (now != stamp) {
        stamp = now;
        text[0] = '\0';
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(text, 1, sizeof text - 1, f);
            text[n] = '\0';
            fclose(f);
        }
    }
    size_t klen = strlen(key);
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t v = n - klen - 1 < sizeof value - 1 ? n - klen - 1 : sizeof value - 1;
            memcpy(value, p + klen + 1, v);
            value[v] = '\0';
            return value;
        }
        p += n + (end ? 1 : 0);
    }
    return "";
}

/* ---- the input method ---- */

static void on_activate(void *user, struct wire_proxy *p)
{
    active = 1;
    table_dirty = 1;
}

/* A new focus starts without the composition of the old one, which the
 * compositor removed from the old context. */
static void on_deactivate(void *user, struct wire_proxy *p)
{
    active = 0;
    if (current)
        current->reset();
    pending_preedit[0] = shown_preedit[0] = '\0';
    pending_commit[0] = '\0';
    preedit_changed = 0;
    clear_table();
}

static void on_surrounding(void *user, struct wire_proxy *p, const char *text, uint32_t cursor, uint32_t anchor) {}
static void on_content_type(void *user, struct wire_proxy *p, uint32_t hints, uint32_t purpose) {}
static void on_cursor_rectangle(void *user, struct wire_proxy *p, int32_t x, int32_t y, int32_t w, int32_t h) {}

static void on_keymap(void *user, struct wire_proxy *p, uint32_t format, int fd, uint32_t size)
{
    keymap_free(keymap);
    keymap = keymap_from_fd(fd, size);
    close(fd);
}

static void on_modifiers(void *user, struct wire_proxy *p, uint32_t dep, uint32_t lock, uint32_t grp)
{
    depressed = dep;
    locked = lock;
    group = grp;
}

static void on_key(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    if (!serial) {
        /* A shortcut: the composition is committed as it is shown. */
        if (current)
            current->flush();
        flush();
        return;
    }
    int handled = 0;
    if (current && state) {
        int mods = (int)depressed | (int)(locked & KEYMAP_MOD_CAPS);
        int ch = keymap ? keymap_translate_group(keymap, key, mods, (int)group) : 0;
        if (keysym_is_symbol(ch) || keysym_is_dead(ch))
            ch = 0;
        handled = current->key(key, ch, (int)depressed);
    }
    flush();
    input_method_key_handled(im, serial, (uint32_t)handled);
}

static void on_select_engine(void *user, struct wire_proxy *p, const char *name)
{
    const struct imed_engine *next = NULL;
    for (int i = 0; i < nengines; i++)
        if (strcmp(engines[i]->name, name) == 0)
            next = engines[i];
    if (next == current)
        return;
    if (current) {
        current->flush();
        clear_table();
    }
    current = next;
    if (current)
        current->select();
    flush();
}

static void on_done(void *user, struct wire_proxy *p, uint32_t serial)
{
    serial_done = serial;
}

static const struct input_method_listener im_events = {
    on_activate, on_deactivate, on_surrounding, on_content_type, on_cursor_rectangle, on_keymap, on_modifiers,
    on_key, on_select_engine, on_done,
};

/* ---- pointer on the candidate window ---- */

static int pointer_x, pointer_y, pointer_inside;

static void on_ptr_enter(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y)
{
    pointer_inside = window_owns(s);
    pointer_x = wire_fixed_to_int(x);
    pointer_y = wire_fixed_to_int(y);
}
static void on_ptr_leave(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s) { pointer_inside = 0; }
static void on_ptr_motion(void *user, struct wire_proxy *p, uint32_t time, int32_t x, int32_t y)
{
    pointer_x = wire_fixed_to_int(x);
    pointer_y = wire_fixed_to_int(y);
}
static void on_ptr_button(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t button,
                          uint32_t state)
{
    if (!pointer_inside || !state || button != 1 || !current)
        return;
    int i = window_click(pointer_x, pointer_y);
    if (i >= 0 && current->candidate_clicked) {
        current->candidate_clicked(i);
        flush();
    }
}
static void on_ptr_axis(void *user, struct wire_proxy *p, uint32_t time, uint32_t axis, int32_t value)
{
    if (pointer_inside && imed_table.n) {
        window_scroll(value > 0 ? 1 : -1);
        flush();
    }
}
static void on_ptr_frame(void *user, struct wire_proxy *p) {}
static const struct pointer_listener pointer_events = {
    on_ptr_enter, on_ptr_leave, on_ptr_motion, on_ptr_button, on_ptr_axis, on_ptr_frame,
};

/* ---- globals ---- */

static void on_geometry(void *user, struct wire_proxy *o, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void on_mode(void *user, struct wire_proxy *o, int32_t w, int32_t h, int32_t r) {}
static void on_scale(void *user, struct wire_proxy *o, int32_t factor) { window_set_scale(factor > 0 ? factor : 1); }
static void on_transform(void *user, struct wire_proxy *o, uint32_t transform) {}
static void on_output_done(void *user, struct wire_proxy *o) {}
static const struct output_listener output_events = { on_geometry, on_mode, on_scale, on_transform, on_output_done };

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, "compositor") == 0) compositor = registry_bind(registry, name, iface, version, &compositor_interface, 1);
    else if (strcmp(iface, "shm") == 0) shm = registry_bind(registry, name, iface, version, &shm_interface, 1);
    else if (strcmp(iface, "seat") == 0) seat = registry_bind(registry, name, iface, version, &seat_interface, 1);
    else if (strcmp(iface, "input_method_manager") == 0)
        manager = registry_bind(registry, name, iface, version, &input_method_manager_interface, 1);
    else if (strcmp(iface, "output") == 0) {
        struct wire_proxy *o = registry_bind(registry, name, iface, version, &output_interface, 1);
        output_add_listener(o, &output_events, NULL);
    }
}
static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name) {}
static const struct registry_listener registry_events = { on_global, on_global_remove };

static void announce_engines(void)
{
    char list[1024] = "", line[160];
    for (int i = 0; i < nengines; i++) {
        snprintf(line, sizeof line, "%s\t%s\t%s\n", engines[i]->name, engines[i]->label, engines[i]->title);
        strlcat(list, line, sizeof list);
    }
    input_method_set_engines(im, list);
}

int main(int argc, char **argv)
{
    int test = argc > 1 && strcmp(argv[1], "-t") == 0;
    if (test)
        engines[nengines++] = &test_engine;
    display = wire_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "imed: no X12 server\n");
        return 1;
    }
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(display));
    registry_add_listener(registry, &registry_events, NULL);
    wire_display_roundtrip(display);
    wire_display_roundtrip(display);
    if (!compositor || !shm || !seat || !manager) {
        fprintf(stderr, "imed: missing globals\n");
        return 1;
    }
    im = input_method_manager_get_input_method(manager, seat);
    input_method_add_listener(im, &im_events, NULL);
    pointer = seat_get_pointer(seat);
    pointer_add_listener(pointer, &pointer_events, NULL);
    window_init();
    announce_engines();
    wire_display_flush(display);
    printf("imed: started with %d engines\n", nengines);
    fflush(stdout);
    for (;;) {
        wire_display_flush(display);
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        if (poll(&pf, 1, -1) < 0)
            continue;
        if (pf.revents & (POLLIN | POLLHUP))
            if (wire_display_dispatch(display) < 0)
                break;
    }
    return 0;
}
