// Shared clipboard on Linux through wlr-data-control: watches the selection
// without needing focus, and sets it when text arrives from another device.
#define _GNU_SOURCE
#include <fcntl.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "clipboard.h"
#include "net.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#define MAX_TEXT (256 * 1024)

static struct {
    struct wl_display *display;
    struct wl_seat *seat;
    struct zwlr_data_control_manager_v1 *manager;
    struct zwlr_data_control_device_v1 *device;
    struct zwlr_data_control_source_v1 *source;
    char *owned;      // text we are currently offering (from a remote device)
    char *last;       // last text seen or set, used to suppress echoes
    bool offer_text;  // current offer has a text mime type
    bool offer_secret;
    guint read_watch;
    int read_fd;
    bool active, skip_initial;
    GString *reading;
} C = {.read_fd = -1};

static bool enabled(void) { return C.active && app.clipboard_enabled && app_operational(); }

static void remember(const char *text) {
    g_free(C.last);
    C.last = g_strdup(text);
}

// ------------------------------------------------------------ outgoing (local copy)

static gboolean on_read(gint fd, GIOCondition cond, gpointer data) {
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0 && C.reading->len + n <= MAX_TEXT) {
        g_string_append_len(C.reading, buf, n);
        return G_SOURCE_CONTINUE;
    }
    bool ok = n == 0;
    close(fd);
    C.read_watch = 0; C.read_fd = -1;
    if (ok && C.reading->len && g_utf8_validate(C.reading->str, C.reading->len, NULL) && g_strcmp0(C.reading->str, C.last) != 0) {
        remember(C.reading->str);
        if (enabled()) {
            net_send_clipboard(C.reading->str);
            app_log("已同步本机剪贴板（%zu 字节）", C.reading->len);
        }
    }
    g_string_free(C.reading, TRUE);
    C.reading = NULL;
    return G_SOURCE_REMOVE;
}

static void offer_mime(void *d, struct zwlr_data_control_offer_v1 *o, const char *mime) {
    if (!strcmp(mime, "text/plain;charset=utf-8") || !strcmp(mime, "UTF8_STRING") || !strcmp(mime, "text/plain")) C.offer_text = true;
    // Password managers mark secrets; never forward them.
    if (!strcmp(mime, "x-kde-passwordManagerHint") || !strcmp(mime, "application/x-nspasteboard-concealed-type")) C.offer_secret = true;
}
static const struct zwlr_data_control_offer_v1_listener offer_listener = {offer_mime};

static void dev_offer(void *d, struct zwlr_data_control_device_v1 *dev, struct zwlr_data_control_offer_v1 *o) {
    C.offer_text = C.offer_secret = false;
    zwlr_data_control_offer_v1_add_listener(o, &offer_listener, NULL);
}

static void dev_selection(void *d, struct zwlr_data_control_device_v1 *dev, struct zwlr_data_control_offer_v1 *o) {
    if (C.skip_initial) { C.skip_initial = false; if (o) zwlr_data_control_offer_v1_destroy(o); return; }
    if (!o) return;
    // Our own offer: nothing to read.
    if (C.source && C.owned) {
        zwlr_data_control_offer_v1_destroy(o);
        return;
    }
    if (C.offer_text && !C.offer_secret && enabled() && !C.read_watch) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) == 0) {
            zwlr_data_control_offer_v1_receive(o, "text/plain;charset=utf-8", fds[1]);
            close(fds[1]);
            wl_display_flush(C.display);
            C.reading = g_string_new(NULL);
            C.read_fd = fds[0];
            C.read_watch = g_unix_fd_add(fds[0], G_IO_IN | G_IO_HUP | G_IO_ERR, on_read, NULL);
        }
    }
    zwlr_data_control_offer_v1_destroy(o);
}
static void dev_finished(void *d, struct zwlr_data_control_device_v1 *dev) {
    zwlr_data_control_device_v1_destroy(C.device);
    C.device = NULL;
}
static void dev_primary(void *d, struct zwlr_data_control_device_v1 *dev, struct zwlr_data_control_offer_v1 *o) {
    if (o) zwlr_data_control_offer_v1_destroy(o);
}
static const struct zwlr_data_control_device_v1_listener device_listener = {dev_offer, dev_selection, dev_finished, dev_primary};

// ------------------------------------------------------------ incoming (remote copy)

static void src_send(void *d, struct zwlr_data_control_source_v1 *s, const char *mime, int32_t fd) {
    if (C.owned) {
        const char *p = C.owned;
        size_t left = strlen(p);
        while (left) {
            ssize_t n = write(fd, p, left);
            if (n <= 0) break;
            p += n;
            left -= n;
        }
    }
    close(fd);
}
static void src_cancelled(void *d, struct zwlr_data_control_source_v1 *s) {
    zwlr_data_control_source_v1_destroy(s);
    if (C.source == s) {
        C.source = NULL;
        g_clear_pointer(&C.owned, g_free);
    }
}
static const struct zwlr_data_control_source_v1_listener source_listener = {src_send, src_cancelled};

void clipboard_set(const char *text, const char *from) {
    if (!enabled() || !C.device || !text || !*text || !g_utf8_validate(text, -1, NULL)) return;
    if (g_strcmp0(text, C.last) == 0) return;
    remember(text);
    if (C.source) zwlr_data_control_source_v1_destroy(C.source);
    g_free(C.owned);
    C.owned = g_strdup(text);
    C.source = zwlr_data_control_manager_v1_create_data_source(C.manager);
    zwlr_data_control_source_v1_add_listener(C.source, &source_listener, NULL);
    static const char *mimes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
    for (size_t i = 0; i < G_N_ELEMENTS(mimes); i++) zwlr_data_control_source_v1_offer(C.source, mimes[i]);
    zwlr_data_control_device_v1_set_selection(C.device, C.source);
    wl_display_flush(C.display);
    app_log("已接收来自 %s 的剪贴板（%zu 字节）", from && *from ? from : "其他设备", strlen(text));
}

bool clipboard_bind(struct wl_display *display, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
    if (strcmp(iface, zwlr_data_control_manager_v1_interface.name)) return false;
    C.display = display;
    C.manager = wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, MIN(version, 2));
    return true;
}

void clipboard_start(struct wl_seat *seat) {
    C.seat = seat;
    if (app_operational()) clipboard_set_active(true);
}

void clipboard_set_active(bool on) {
    C.active = on;
    if (!on) {
        if (C.read_watch) { g_source_remove(C.read_watch); C.read_watch = 0; }
        if (C.read_fd >= 0) { close(C.read_fd); C.read_fd = -1; }
        if (C.reading) { g_string_free(C.reading, TRUE); C.reading = NULL; }
        if (C.device) { zwlr_data_control_device_v1_destroy(C.device); C.device = NULL; }
        if (C.source) { zwlr_data_control_source_v1_destroy(C.source); C.source = NULL; }
        g_clear_pointer(&C.owned, g_free); g_clear_pointer(&C.last, g_free);
    } else if (C.manager && C.seat && !C.device) {
        C.skip_initial = true; // Existing clipboard content is not a new copy after wake.
        C.device = zwlr_data_control_manager_v1_get_data_device(C.manager, C.seat);
        zwlr_data_control_device_v1_add_listener(C.device, &device_listener, NULL);
    }
    if (C.display) wl_display_flush(C.display);
}

bool clipboard_available(void) { return C.manager && C.seat; }
