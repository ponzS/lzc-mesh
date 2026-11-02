// Entry point, shared state helpers and the periodic LazyCat/hub supervisor.
#include <gtk/gtk.h>
#include <stdarg.h>
#include <string.h>

#include "app.h"
#include "direct.h"
#include "lzc.h"
#include "net.h"
#include "ui.h"
#include "wl_input.h"
#include "config.h"
#include "tray.h"

App app;
static GtkApplication *gtk_app_ref;

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
    static const char *names[S_COUNT] = {"lzc-client", "lzc-network", "login", "service", "hub", "input", "control", "screen", "clipboard", "transport", "pointer-channel", "keyboard-channel"};
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

void app_status_changed(void) { ui_refresh_status(); tray_refresh(); }
void app_devices_changed(void) { ui_refresh_layout(); tray_refresh(); }

static const char *labels[S_COUNT] = {"懒猫微服客户端", "懒猫网络", "登录会话", "设备接力应用", "中枢连接", "输入权限", "控制状态", "本机屏幕", "共享剪贴板", "输入通道", "光标通道", "键盘通道"};

char *app_summary(StatusLevel *level) {
    if (app.hub_connected) {
        guint online = 0;
        for (guint i = 0; i < app.devices->len; i++) online += ((MeshDevice *)g_ptr_array_index(app.devices, i))->online;
        *level = ST_OK;
        return g_strdup_printf("已连接 · %u 台设备在线", online);
    }
    int keys[] = {S_LZC_CLIENT, S_LZC_NETWORK, S_LOGIN, S_SERVICE, S_HUB};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        Status *s = &app.status[keys[i]];
        if (s->level == ST_ERROR || s->level == ST_WARN) {
            *level = s->level;
            return g_strdup_printf("%s：%s", labels[keys[i]], s->text);
        }
    }
    *level = ST_UNKNOWN;
    return g_strdup("正在连接…");
}

static const char *device_name(const char *id) {
    for (guint i = 0; id && i < app.devices->len; i++) {
        MeshDevice *d = g_ptr_array_index(app.devices, i);
        if (g_strcmp0(d->id, id) == 0) return d->name;
    }
    return id ? id : "?";
}

char *app_control_line(void) {
    if (app.captured) return g_strdup_printf("正在控制 %s", device_name(app.active_target));
    if (app.controlled) return g_strdup_printf("%s 正在控制本机", device_name(app.active_source));
    return NULL;
}

void app_set_sharing(bool on) {
    if (on == app.sharing_enabled) return;
    app.sharing_enabled = on;
    config_save();
    // Give the cursor back at once if this computer is controlling another device.
    if (!on && app.captured) net_send_release();
    net_send_sharing(on);
    app_log("键鼠共享已%s", on ? "开启" : "暂停");
    app_status_changed();
}

void app_quit(void) {
    if (gtk_app_ref) g_application_quit(G_APPLICATION(gtk_app_ref));
}

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
    gtk_app_ref = gtk_app;
    ui_build(gtk_app);
    direct_init();
    tray_start();
    wl_input_init();
    lzc_refresh();
    g_timeout_add_seconds(3, supervise, NULL);
}

int main(int argc, char **argv) {
    for (int i = 0; i < S_COUNT; i++) app.status[i] = (Status){ST_UNKNOWN, g_strdup("检测中…")};
    app.devices = g_ptr_array_new();
    app.swap_cmd_ctrl = true;
    app.sharing_enabled = true;
    config_load();
    // Started at login: run in the tray without opening the window. GApplication
    // would reject unknown options, so the flag is removed before g_application_run.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--background")) {
            app.background = true;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--;
            break;
        }
    GtkApplication *gtk_app = gtk_application_new("cloud.lazycat.lzcmesh", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    direct_shutdown();
    wl_input_shutdown();
    g_object_unref(gtk_app);
    return rc;
}
