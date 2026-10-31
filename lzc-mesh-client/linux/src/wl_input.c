// Wayland input for wlroots-style compositors (Hyprland, Sway, river...).
//
// Approach borrowed from lightos-remote-desktop's Wayland backend
// (zwlr_virtual_pointer + zwp_virtual_keyboard with an xkb evdev keymap for
// injection) plus layer-shell barriers for edge detection:
//  * edge detection: 1px transparent overlay strips on screen sides that have
//    a neighbour; the pointer entering a strip means "pushing against the edge".
//  * capture: a transparent full-screen overlay with exclusive keyboard focus,
//    a pointer lock, relative pointer events and a shortcuts inhibitor.
//  * injection: virtual pointer (absolute moves over the layout) and virtual
//    keyboard.
#define _GNU_SOURCE
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "app.h"
#include "net.h"
#include "wl_input.h"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

enum { D_LEFT, D_RIGHT, D_UP, D_DOWN };
static const char *dir_names[] = {"left", "right", "up", "down"};

typedef struct Output Output;

typedef struct {
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_buffer *buffer;
    Output *output;
    int dir;  // D_* for strips, -1 for the capture overlay
    int w, h;
} Surf;

struct Output {
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    uint32_t name;
    int x, y, w, h;
    Surf *strips[4];
};

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct zwlr_virtual_pointer_manager_v1 *vp_manager;
    struct zwp_virtual_keyboard_manager_v1 *vk_manager;
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_manager;

    struct zwlr_virtual_pointer_v1 *vpointer;
    struct zwp_virtual_keyboard_v1 *vkeyboard;
    struct zwp_relative_pointer_v1 *relative;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;

    GPtrArray *outputs;
    bool edges[4];
    Surf *capture;
    struct zwp_locked_pointer_v1 *lock;
    struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    bool locked;

    Surf *pointer_on;  // surface under the pointer (ours only)
    uint32_t enter_serial;
    double px, py;     // pointer position within pointer_on
    gint64 quiet_until;  // ignore edges until (after release / leave)
    gint64 last_edge;
    Output *last_edge_output;

    // Wheel accumulation per wl_pointer frame
    double axis[2];
    int axis120[2];
    bool got120;

    guint watch;
    GHashTable *held_keys;   // keys pressed by remote on this machine
    GHashTable *held_buttons;
} W;

static uint32_t now_ms(void) { return (uint32_t)(g_get_monotonic_time() / 1000); }
static void flush(void) { if (W.display) wl_display_flush(W.display); }

// ---------------------------------------------------------------- geometry

static void bounds(int *x0, int *y0, int *x1, int *y1) {
    *x0 = *y0 = G_MAXINT;
    *x1 = *y1 = G_MININT;
    for (guint i = 0; i < W.outputs->len; i++) {
        Output *o = g_ptr_array_index(W.outputs, i);
        if (o->w <= 0) continue;
        *x0 = MIN(*x0, o->x); *y0 = MIN(*y0, o->y);
        *x1 = MAX(*x1, o->x + o->w); *y1 = MAX(*y1, o->y + o->h);
    }
    if (*x0 == G_MAXINT) *x0 = *y0 = *x1 = *y1 = 0;
}

static void update_screen(void) {
    int x0, y0, x1, y1;
    bounds(&x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    if (w != app.screen_w || h != app.screen_h) {
        app.screen_w = w;
        app.screen_h = h;
        app_set_status(S_SCREEN, ST_OK, "%d×%d（%u 个显示器，逻辑像素）", w, h, W.outputs->len);
        if (app.hub_connected) net_send_hello();
    }
}

// ---------------------------------------------------------------- buffers & layer surfaces

static struct wl_buffer *transparent_buffer(int w, int h) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("lzc-mesh", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        if (fd >= 0) close(fd);
        return NULL;
    }
    // memfd pages are zero-filled: fully transparent ARGB.
    struct wl_shm_pool *pool = wl_shm_create_pool(W.shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial, uint32_t w, uint32_t h) {
    Surf *s = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (!s->buffer || (int)w != s->w || (int)h != s->h) {
        if (s->buffer) wl_buffer_destroy(s->buffer);
        s->w = (int)w;
        s->h = (int)h;
        s->buffer = transparent_buffer(s->w, s->h);
    }
    wl_surface_attach(s->surface, s->buffer, 0, 0);
    wl_surface_damage_buffer(s->surface, 0, 0, s->w, s->h);
    wl_surface_commit(s->surface);
}

static void surf_destroy(Surf *s);
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    Surf *s = data;
    if (s->output && s->dir >= 0) s->output->strips[s->dir] = NULL;
    if (W.capture == s) W.capture = NULL;
    surf_destroy(s);
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};

static Surf *surf_new(Output *o, int dir) {
    Surf *s = g_new0(Surf, 1);
    s->output = o;
    s->dir = dir;
    s->surface = wl_compositor_create_surface(W.compositor);
    wl_surface_set_user_data(s->surface, s);
    s->layer = zwlr_layer_shell_v1_get_layer_surface(W.layer_shell, s->surface, o ? o->wl : NULL,
                                                     ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, dir < 0 ? "lzc-mesh-capture" : "lzc-mesh-edge");
    uint32_t a = 0;
    const uint32_t T = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP, B = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
                   L = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT, R = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    switch (dir) {
    case D_LEFT: a = L | T | B; zwlr_layer_surface_v1_set_size(s->layer, 1, 0); break;
    case D_RIGHT: a = R | T | B; zwlr_layer_surface_v1_set_size(s->layer, 1, 0); break;
    case D_UP: a = T | L | R; zwlr_layer_surface_v1_set_size(s->layer, 0, 1); break;
    case D_DOWN: a = B | L | R; zwlr_layer_surface_v1_set_size(s->layer, 0, 1); break;
    default: a = T | B | L | R; zwlr_layer_surface_v1_set_size(s->layer, 0, 0); break;
    }
    zwlr_layer_surface_v1_set_anchor(s->layer, a);
    zwlr_layer_surface_v1_set_exclusive_zone(s->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer, dir < 0 ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                                                                        : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(s->layer, &layer_listener, s);
    wl_surface_commit(s->surface);
    return s;
}

static void surf_destroy(Surf *s) {
    if (!s) return;
    if (W.pointer_on == s) W.pointer_on = NULL;
    if (s->layer) zwlr_layer_surface_v1_destroy(s->layer);
    if (s->surface) wl_surface_destroy(s->surface);
    if (s->buffer) wl_buffer_destroy(s->buffer);
    g_free(s);
}

// A strip is only placed on output sides that are on the outer border of the desktop.
static void sync_strips(void) {
    if (!W.layer_shell) return;
    int x0, y0, x1, y1;
    bounds(&x0, &y0, &x1, &y1);
    for (guint i = 0; i < W.outputs->len; i++) {
        Output *o = g_ptr_array_index(W.outputs, i);
        bool outer[4] = {o->x == x0, o->x + o->w == x1, o->y == y0, o->y + o->h == y1};
        for (int d = 0; d < 4; d++) {
            bool want = W.edges[d] && outer[d] && o->w > 0 && !app.captured;
            if (want && !o->strips[d]) o->strips[d] = surf_new(o, d);
            if (!want && o->strips[d]) {
                surf_destroy(o->strips[d]);
                o->strips[d] = NULL;
            }
        }
    }
    flush();
}

// ---------------------------------------------------------------- outputs

static void xdg_pos(void *d, struct zxdg_output_v1 *x, int32_t px, int32_t py) { Output *o = d; o->x = px; o->y = py; }
static void xdg_size(void *d, struct zxdg_output_v1 *x, int32_t w, int32_t h) { Output *o = d; o->w = w; o->h = h; }
static void xdg_done(void *d, struct zxdg_output_v1 *x) { update_screen(); sync_strips(); }
static void xdg_name(void *d, struct zxdg_output_v1 *x, const char *n) {}
static void xdg_desc(void *d, struct zxdg_output_v1 *x, const char *n) {}
static const struct zxdg_output_v1_listener xdg_output_listener = {xdg_pos, xdg_size, xdg_done, xdg_name, xdg_desc};

static void output_watch(Output *o) {
    if (W.xdg_output_manager && !o->xdg) {
        o->xdg = zxdg_output_manager_v1_get_xdg_output(W.xdg_output_manager, o->wl);
        zxdg_output_v1_add_listener(o->xdg, &xdg_output_listener, o);
    }
}

// ---------------------------------------------------------------- pointer (local, while capturing / at edges)

static void hide_cursor(void) {
    if (W.pointer) wl_pointer_set_cursor(W.pointer, W.enter_serial, NULL, 0, 0);
}

static void maybe_edge(void) {
    Surf *s = W.pointer_on;
    if (!s || s->dir < 0 || app.captured || app.controlled || !app.hub_connected) return;
    gint64 now = g_get_monotonic_time();
    if (now < W.quiet_until || now - W.last_edge < 250 * 1000) return;
    W.last_edge = now;
    W.last_edge_output = s->output;
    int x0, y0, x1, y1;
    bounds(&x0, &y0, &x1, &y1);
    double gx = s->output->x + W.px, gy = s->output->y + W.py;
    double pos = (s->dir == D_LEFT || s->dir == D_RIGHT) ? (gy - y0) / MAX(1, y1 - y0) : (gx - x0) / MAX(1, x1 - x0);
    net_send_edge(dir_names[s->dir], CLAMP(pos, 0.0, 1.0));
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    W.enter_serial = serial;
    W.pointer_on = surface ? wl_surface_get_user_data(surface) : NULL;
    W.px = wl_fixed_to_double(sx);
    W.py = wl_fixed_to_double(sy);
    if (W.pointer_on && W.pointer_on == W.capture) hide_cursor();
    maybe_edge();
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface) { W.pointer_on = NULL; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy) {
    W.px = wl_fixed_to_double(sx);
    W.py = wl_fixed_to_double(sy);
    maybe_edge();
}
static int proto_button(uint32_t b) {
    switch (b) {
    case BTN_LEFT: return 1;
    case BTN_RIGHT: return 2;
    case BTN_MIDDLE: return 3;
    case BTN_SIDE: return 4;
    case BTN_EXTRA: return 5;
    default: return 0;
    }
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state) {
    int b = proto_button(button);
    if (app.captured && b) net_send_button(b, state == WL_POINTER_BUTTON_STATE_PRESSED);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    if (axis < 2) W.axis[axis] += wl_fixed_to_double(value);
}
static void p_frame(void *d, struct wl_pointer *p) {
    if (app.captured) {
        // Protocol wheel: 1/120 notch, positive = away from the user (scroll up / left).
        double v = W.got120 ? -W.axis120[0] : -W.axis[0] * 12.0;
        double h = W.got120 ? -W.axis120[1] : -W.axis[1] * 12.0;
        if (v != 0 || h != 0) net_send_wheel(h, v);
    }
    W.axis[0] = W.axis[1] = 0;
    W.axis120[0] = W.axis120[1] = 0;
    W.got120 = false;
}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) {}
static void p_axis_value120(void *d, struct wl_pointer *p, uint32_t axis, int32_t v) {
    if (axis < 2) W.axis120[axis] += v;
    W.got120 = true;
}
static void p_axis_rel(void *d, struct wl_pointer *p, uint32_t axis, uint32_t dir) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis, .frame = p_frame,
    .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
    .axis_value120 = p_axis_value120, .axis_relative_direction = p_axis_rel,
};

static void rel_motion(void *d, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy,
                       wl_fixed_t udx, wl_fixed_t udy) {
    if (app.captured) net_send_motion(wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}
static const struct zwp_relative_pointer_v1_listener relative_listener = {rel_motion};

// ---------------------------------------------------------------- keyboard (local, while capturing)

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) { close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {
    if (app.captured) net_send_key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener keyboard_listener = {k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !W.pointer) {
        W.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(W.pointer, &pointer_listener, NULL);
        if (W.relative_manager) {
            W.relative = zwp_relative_pointer_manager_v1_get_relative_pointer(W.relative_manager, W.pointer);
            zwp_relative_pointer_v1_add_listener(W.relative, &relative_listener, NULL);
        }
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !W.keyboard) {
        W.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(W.keyboard, &keyboard_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

// ---------------------------------------------------------------- registry

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
#define BIND(var, type, ver) var = wl_registry_bind(r, name, &type, MIN(version, ver))
    if (!strcmp(iface, wl_compositor_interface.name)) BIND(W.compositor, wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) BIND(W.shm, wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !W.seat) {
        BIND(W.seat, wl_seat_interface, 8);
        wl_seat_add_listener(W.seat, &seat_listener, NULL);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) BIND(W.layer_shell, zwlr_layer_shell_v1_interface, 4);
    else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) BIND(W.xdg_output_manager, zxdg_output_manager_v1_interface, 3);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) BIND(W.vp_manager, zwlr_virtual_pointer_manager_v1_interface, 2);
    else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) BIND(W.vk_manager, zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) BIND(W.constraints, zwp_pointer_constraints_v1_interface, 1);
    else if (!strcmp(iface, zwp_relative_pointer_manager_v1_interface.name)) BIND(W.relative_manager, zwp_relative_pointer_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
        BIND(W.inhibit_manager, zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name)) {
        Output *o = g_new0(Output, 1);
        o->name = name;
        o->wl = wl_registry_bind(r, name, &wl_output_interface, MIN(version, 3));
        g_ptr_array_add(W.outputs, o);
        output_watch(o);
    }
#undef BIND
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
    for (guint i = 0; i < W.outputs->len; i++) {
        Output *o = g_ptr_array_index(W.outputs, i);
        if (o->name != name) continue;
        for (int k = 0; k < 4; k++) surf_destroy(o->strips[k]);
        if (W.capture && W.capture->output == o) W.capture->output = NULL;
        if (W.last_edge_output == o) W.last_edge_output = NULL;
        if (o->xdg) zxdg_output_v1_destroy(o->xdg);
        wl_output_destroy(o->wl);
        g_ptr_array_remove_index(W.outputs, i);
        g_free(o);
        update_screen();
        sync_strips();
        return;
    }
}
static const struct wl_registry_listener registry_listener = {reg_global, reg_remove};

static gboolean on_wayland(gint fd, GIOCondition cond, gpointer d) {
    if ((cond & (G_IO_ERR | G_IO_HUP)) || wl_display_dispatch(W.display) < 0) {
        app_set_status(S_INPUT, ST_ERROR, "Wayland 连接中断，请重启客户端");
        W.watch = 0;
        return G_SOURCE_REMOVE;
    }
    flush();
    return G_SOURCE_CONTINUE;
}

static bool setup_virtual_keyboard(void) {
    W.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // evdev rules with the user's default layout so that keycodes map 1:1 to evdev codes.
    struct xkb_rule_names names = {.rules = "evdev"};
    W.keymap = xkb_keymap_new_from_names(W.xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!W.keymap) return false;
    W.xkb_state = xkb_state_new(W.keymap);
    char *map = xkb_keymap_get_as_string(W.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(map) + 1;
    int fd = memfd_create("lzc-mesh-keymap", MFD_CLOEXEC);
    bool ok = fd >= 0 && write(fd, map, size) == (ssize_t)size;
    free(map);
    if (!ok) {
        if (fd >= 0) close(fd);
        return false;
    }
    W.vkeyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(W.vk_manager, W.seat);
    zwp_virtual_keyboard_v1_keymap(W.vkeyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    close(fd);
    return true;
}

void wl_input_init(void) {
    W.outputs = g_ptr_array_new();
    W.held_keys = g_hash_table_new(NULL, NULL);
    W.held_buttons = g_hash_table_new(NULL, NULL);
    W.display = wl_display_connect(NULL);
    if (!W.display) {
        app_set_status(S_INPUT, ST_ERROR, "未检测到 Wayland 会话（WAYLAND_DISPLAY 未设置），当前仅支持 wlroots 类合成器");
        return;
    }
    W.registry = wl_display_get_registry(W.display);
    wl_registry_add_listener(W.registry, &registry_listener, NULL);
    wl_display_roundtrip(W.display);
    for (guint i = 0; i < W.outputs->len; i++) output_watch(g_ptr_array_index(W.outputs, i));
    wl_display_roundtrip(W.display);

    GString *missing = g_string_new(NULL);
    if (!W.layer_shell) g_string_append(missing, " wlr-layer-shell");
    if (!W.vp_manager) g_string_append(missing, " wlr-virtual-pointer");
    if (!W.vk_manager) g_string_append(missing, " virtual-keyboard");
    if (!W.constraints) g_string_append(missing, " pointer-constraints");
    if (!W.relative_manager) g_string_append(missing, " relative-pointer");
    if (!W.xdg_output_manager) g_string_append(missing, " xdg-output");
    if (W.vp_manager && W.seat) W.vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(W.vp_manager, W.seat);
    bool kb = W.vk_manager && W.seat && setup_virtual_keyboard();
    if (missing->len) {
        app_set_status(S_INPUT, ST_ERROR, "合成器缺少协议：%s。请使用 Hyprland / Sway 等 wlroots 合成器", missing->str);
    } else if (!kb) {
        app_set_status(S_INPUT, ST_ERROR, "无法创建虚拟键盘（xkb 键盘布局编译失败）");
    } else {
        const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
        app_set_status(S_INPUT, ST_OK, "Wayland 输入就绪（%s：虚拟指针、虚拟键盘、边缘检测、指针锁定）", desktop ? desktop : "wlroots");
    }
    g_string_free(missing, TRUE);
    update_screen();
    W.watch = g_unix_fd_add(wl_display_get_fd(W.display), G_IO_IN | G_IO_ERR | G_IO_HUP, on_wayland, NULL);
    flush();
}

void wl_input_shutdown(void) {
    if (!W.display) return;
    wl_input_leave();
    flush();
    wl_display_disconnect(W.display);
    W.display = NULL;
}

void wl_input_set_edges(bool left, bool right, bool up, bool down) {
    W.edges[D_LEFT] = left;
    W.edges[D_RIGHT] = right;
    W.edges[D_UP] = up;
    W.edges[D_DOWN] = down;
    if (W.display) sync_strips();
}

// ---------------------------------------------------------------- capture

static void locked(void *d, struct zwp_locked_pointer_v1 *l) {
    W.locked = true;
    app_log("本机指针已锁定，输入转发中");
}
static void unlocked(void *d, struct zwp_locked_pointer_v1 *l) { W.locked = false; }
static const struct zwp_locked_pointer_v1_listener lock_listener = {locked, unlocked};

void wl_input_capture(bool on) {
    if (!W.display) return;
    if (on && !W.capture) {
        Output *o = W.last_edge_output ? W.last_edge_output : (W.outputs->len ? g_ptr_array_index(W.outputs, 0) : NULL);
        W.capture = surf_new(o, -1);
        if (W.constraints && W.pointer)
            W.lock = zwp_pointer_constraints_v1_lock_pointer(W.constraints, W.capture->surface, W.pointer, NULL,
                                                             ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        if (W.lock) zwp_locked_pointer_v1_add_listener(W.lock, &lock_listener, NULL);
        if (W.inhibit_manager && W.seat)
            W.inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(W.inhibit_manager, W.capture->surface, W.seat);
    } else if (!on && W.capture) {
        if (W.inhibitor) zwp_keyboard_shortcuts_inhibitor_v1_destroy(W.inhibitor), W.inhibitor = NULL;
        if (W.lock) zwp_locked_pointer_v1_destroy(W.lock), W.lock = NULL;
        W.locked = false;
        surf_destroy(W.capture);
        W.capture = NULL;
        W.quiet_until = g_get_monotonic_time() + 400 * 1000;
    }
    sync_strips();
    flush();
}

// ---------------------------------------------------------------- injection

static void abs_move(double nx, double ny) {
    if (!W.vpointer || app.screen_w <= 0) return;
    uint32_t w = (uint32_t)app.screen_w, h = (uint32_t)app.screen_h;
    uint32_t x = (uint32_t)CLAMP(nx * w, 0, w - 1), y = (uint32_t)CLAMP(ny * h, 0, h - 1);
    zwlr_virtual_pointer_v1_motion_absolute(W.vpointer, now_ms(), x, y, w, h);
    zwlr_virtual_pointer_v1_frame(W.vpointer);
    flush();
}

void wl_input_warp(double nx, double ny) {
    W.quiet_until = g_get_monotonic_time() + 400 * 1000;
    abs_move(nx, ny);
}

void wl_input_enter(double nx, double ny) {
    W.quiet_until = g_get_monotonic_time() + 400 * 1000;
    abs_move(nx, ny);
}

void wl_input_move(double nx, double ny) { abs_move(nx, ny); }

static uint32_t linux_button(int b) {
    switch (b) {
    case 1: return BTN_LEFT;
    case 2: return BTN_RIGHT;
    case 3: return BTN_MIDDLE;
    case 4: return BTN_SIDE;
    case 5: return BTN_EXTRA;
    default: return 0;
    }
}

void wl_input_button(int button, bool down) {
    uint32_t b = linux_button(button);
    if (!W.vpointer || !b) return;
    if (down) g_hash_table_add(W.held_buttons, GUINT_TO_POINTER(b));
    else g_hash_table_remove(W.held_buttons, GUINT_TO_POINTER(b));
    zwlr_virtual_pointer_v1_button(W.vpointer, now_ms(), b, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(W.vpointer);
    flush();
}

static void wheel_axis(uint32_t axis, double v120) {
    if (v120 == 0) return;
    double value = -v120 / 12.0;  // 10 surface units per notch, positive = down/right
    double notches = -v120 / 120.0;
    if (notches == (int)notches) {
        zwlr_virtual_pointer_v1_axis_source(W.vpointer, WL_POINTER_AXIS_SOURCE_WHEEL);
        zwlr_virtual_pointer_v1_axis_discrete(W.vpointer, now_ms(), axis, wl_fixed_from_double(value), (int32_t)notches);
    } else {
        zwlr_virtual_pointer_v1_axis_source(W.vpointer, WL_POINTER_AXIS_SOURCE_FINGER);
        zwlr_virtual_pointer_v1_axis(W.vpointer, now_ms(), axis, wl_fixed_from_double(value));
    }
}

void wl_input_wheel(double dx, double dy) {
    if (!W.vpointer) return;
    wheel_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
    wheel_axis(WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
    zwlr_virtual_pointer_v1_frame(W.vpointer);
    flush();
}

void wl_input_key(uint32_t code, bool down) {
    if (!W.vkeyboard || code == 0 || code > 767) return;
    if (down) g_hash_table_add(W.held_keys, GUINT_TO_POINTER(code));
    else if (!g_hash_table_remove(W.held_keys, GUINT_TO_POINTER(code))) return;
    zwp_virtual_keyboard_v1_key(W.vkeyboard, now_ms(), code, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    xkb_state_update_key(W.xkb_state, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    zwp_virtual_keyboard_v1_modifiers(W.vkeyboard, xkb_state_serialize_mods(W.xkb_state, XKB_STATE_MODS_DEPRESSED),
                                      xkb_state_serialize_mods(W.xkb_state, XKB_STATE_MODS_LATCHED),
                                      xkb_state_serialize_mods(W.xkb_state, XKB_STATE_MODS_LOCKED),
                                      xkb_state_serialize_layout(W.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE));
    flush();
}

// Releases everything the remote side still holds so no key or button stays stuck.
void wl_input_leave(void) {
    if (!W.held_keys) return;
    GList *keys = g_hash_table_get_keys(W.held_keys);
    for (GList *l = keys; l; l = l->next) wl_input_key(GPOINTER_TO_UINT(l->data), false);
    g_list_free(keys);
    GList *buttons = g_hash_table_get_keys(W.held_buttons);
    for (GList *l = buttons; l; l = l->next) {
        uint32_t b = GPOINTER_TO_UINT(l->data);
        if (W.vpointer) zwlr_virtual_pointer_v1_button(W.vpointer, now_ms(), b, WL_POINTER_BUTTON_STATE_RELEASED);
    }
    g_list_free(buttons);
    g_hash_table_remove_all(W.held_buttons);
    if (W.vpointer) zwlr_virtual_pointer_v1_frame(W.vpointer);
    W.quiet_until = g_get_monotonic_time() + 400 * 1000;
    flush();
}
