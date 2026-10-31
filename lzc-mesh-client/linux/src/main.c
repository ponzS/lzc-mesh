// Entry point, shared state helpers and the periodic LazyCat/hub supervisor.
#include <gtk/gtk.h>
#include <stdarg.h>

#include "app.h"
#include "lzc.h"
#include "net.h"
#include "ui.h"
#include "wl_input.h"
#include "config.h"

App app;

void app_set_status(int which, StatusLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    Status *s = &app.status[which];
    if (s->level == level && g_strcmp0(s->text, text) == 0) {
        g_free(text);
        return;
    }
    s->level = level;
    g_free(s->text);
    s->text = text;
    static const char *names[S_COUNT] = {"lzc-client", "lzc-network", "login", "service", "hub", "input", "control", "screen"};
    static const char *lv[] = {"..", "ok", "warn", "ERROR"};
    g_printerr("[status] %s %s: %s\n", names[which], lv[level], text);
    app_status_changed();
}

void app_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%H:%M:%S");
    char *line = g_strdup_printf("%s  %s", stamp, text);
    g_printerr("%s\n", line);
    ui_append_log(line);
    g_free(line);
    g_free(stamp);
    g_date_time_unref(now);
    g_free(text);
}

void app_status_changed(void) { ui_refresh_status(); }
void app_devices_changed(void) { ui_refresh_layout(); }

const char *app_effective_box(void) {
    return (app.box_override && *app.box_override) ? app.box_override : app.box_name;
}

static gboolean supervise(gpointer data) {
    lzc_refresh();
    return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication *gtk_app, gpointer data) {
    static bool started = false;
    if (started) {
        ui_present();
        return;
    }
    started = true;
    ui_build(gtk_app);
    wl_input_init();
    lzc_refresh();
    g_timeout_add_seconds(3, supervise, NULL);
}

int main(int argc, char **argv) {
    for (int i = 0; i < S_COUNT; i++) app.status[i] = (Status){ST_UNKNOWN, g_strdup("检测中…")};
    app.devices = g_ptr_array_new();
    app.swap_cmd_ctrl = true;
    config_load();
    GtkApplication *gtk_app = gtk_application_new("cloud.lazycat.lzcmesh", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    wl_input_shutdown();
    g_object_unref(gtk_app);
    return rc;
}
