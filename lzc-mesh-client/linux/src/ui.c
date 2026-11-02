// Main window: header with live connection status and the sharing switch, then two
// columns. Left: screen layout and shared files. Right: connection status, settings
// and the event log. Black / white / grey, the same structure as macOS and Windows.
#include <string.h>

#include "app.h"
#include "autostart.h"
#include "config.h"
#include "files.h"
#include "lzc.h"
#include "net.h"
#include "tray.h"
#include "ui.h"

static GtkWidget *window, *layout_view, *log_view, *box_entry, *open_btn;
static GtkWidget *sharing_switch, *swap_switch, *autostart_switch;
static GtkWidget *pill_dot, *pill_text, *control_text;
static GtkWidget *status_dot[S_COUNT], *status_text[S_COUNT];
static GtkWidget *transport_picker;
static const char *transport_modes[] = {"auto", "bluetooth", "direct", "relay"};
static bool updating;  // suppress switch signals while syncing from state

static const char *labels[S_COUNT] = {
    "懒猫微服客户端", "懒猫网络", "登录会话", "设备接力应用", "中枢连接", "输入权限", "控制状态", "本机屏幕", "共享剪贴板", "输入通道", "光标通道", "键盘通道",
};

static const char *css =
    "window.lzc { background: #f5f5f5; color: #111; }"
    ".lzc .header { padding: 14px 22px 8px 22px; }"
    ".lzc .app-title { font-weight: 600; font-size: 16px; }"
    ".lzc .card { background: #ffffff; border: 1px solid #e2e2e2; border-radius: 12px; padding: 14px 16px; }"
    ".lzc .card-title { font-weight: 600; font-size: 14px; }"
    ".lzc .muted { color: #777777; font-size: 12px; }"
    ".lzc .key { color: #777777; }"
    ".lzc .strong { font-weight: 600; }"
    ".lzc .dot { min-width: 8px; min-height: 8px; border-radius: 99px; background: #cccccc; margin-top: 5px; }"
    ".lzc .dot.ok { background: #111111; }"
    ".lzc .dot.warn { background: #888888; }"
    ".lzc .dot.error { background: #ffffff; box-shadow: inset 0 0 0 2px #111111; }"
    ".lzc .header .dot { margin-top: 0; }"
    ".lzc button { border-radius: 8px; padding: 4px 12px; min-height: 0; background: #ffffff; color: #111111;"
    "  border: 1px solid #e2e2e2; box-shadow: none; background-image: none; text-shadow: none; }"
    ".lzc button:hover { background: #f2f2f2; border-color: #9e9e9e; }"
    ".lzc button:active { background: #e8e8e8; }"
    ".lzc button:disabled { color: #aaaaaa; }"
    ".lzc button.primary { background: #111111; color: #ffffff; border-color: #111111; }"
    ".lzc button.primary:hover { background: #333333; }"
    ".lzc switch { background: #cccccc; border: none; border-radius: 99px; min-width: 34px; min-height: 20px; box-shadow: none; }"
    ".lzc switch:checked { background: #111111; }"
    ".lzc switch slider { background: #ffffff; border: none; border-radius: 99px; min-width: 16px; min-height: 16px; margin: 2px;"
    "  box-shadow: none; }"
    ".lzc entry { border-radius: 8px; border: 1px solid #e2e2e2; background: #ffffff; box-shadow: none; min-height: 28px; }"
    ".lzc entry:focus-within { border-color: #999999; outline: none; }"
    ".lzc textview, .lzc textview text { background: #ffffff; color: #404040; font-family: monospace; font-size: 11px; }"
    ".lzc list, .lzc row { background: #ffffff; }"
    ".lzc row { border-bottom: 1px solid #ececec; padding: 2px 0; }"
    ".lzc scrollbar { background: transparent; }";

// ------------------------------------------------------------------ updates

static void set_dot(GtkWidget *dot, StatusLevel lv) {
    gtk_widget_remove_css_class(dot, "ok");
    gtk_widget_remove_css_class(dot, "warn");
    gtk_widget_remove_css_class(dot, "error");
    if (lv == ST_OK) gtk_widget_add_css_class(dot, "ok");
    if (lv == ST_WARN) gtk_widget_add_css_class(dot, "warn");
    if (lv == ST_ERROR) gtk_widget_add_css_class(dot, "error");
}

static void sync_switches(void) {
    updating = true;
    gtk_switch_set_active(GTK_SWITCH(sharing_switch), app.sharing_enabled);
    gtk_switch_set_active(GTK_SWITCH(swap_switch), app.swap_cmd_ctrl);
    gtk_switch_set_active(GTK_SWITCH(autostart_switch), autostart_enabled());
    if (transport_picker) {
        guint selected=0;
        for (guint i=0;i<G_N_ELEMENTS(transport_modes);i++)
            if (!g_strcmp0(app.transport_mode,transport_modes[i])) selected=i;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(transport_picker),selected);
        gtk_widget_set_sensitive(transport_picker,app.hub_connected);
    }
    updating = false;
}

void ui_refresh_status(void) {
    if (!window) return;
    for (int i = 0; i < S_COUNT; i++) {
        Status *s = &app.status[i];
        set_dot(status_dot[i], s->level);
        gtk_label_set_text(GTK_LABEL(status_text[i]), s->text ? s->text : "");
        if (s->level == ST_ERROR) gtk_widget_add_css_class(status_text[i], "strong");
        else gtk_widget_remove_css_class(status_text[i], "strong");
    }
    StatusLevel lv;
    char *summary = app_summary(&lv);
    set_dot(pill_dot, lv);
    gtk_label_set_text(GTK_LABEL(pill_text), summary);
    if (lv == ST_OK) gtk_widget_remove_css_class(pill_text, "muted");
    else gtk_widget_add_css_class(pill_text, "muted");
    g_free(summary);
    char *ctrl = app_control_line();
    char *ctrl_line = ctrl ? g_strdup_printf("·  %s", ctrl) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(control_text), ctrl_line);
    g_free(ctrl_line);
    g_free(ctrl);
    gtk_widget_set_sensitive(open_btn, app.box_domain && *app.box_domain);
    sync_switches();
}

void ui_refresh_layout(void) {
    if (!window) return;
    layout_view_refresh(layout_view);
    ui_refresh_status();
}

void ui_append_log(const char *line) {
    if (!log_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, line, -1);
    gtk_text_buffer_insert(buf, &end, "\n", -1);
    if (gtk_text_buffer_get_line_count(buf) > 400) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(buf, &a);
        gtk_text_buffer_get_iter_at_line(buf, &b, 100);
        gtk_text_buffer_delete(buf, &a, &b);
    }
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view), mark);
    gtk_text_buffer_delete_mark(buf, mark);
}

// ------------------------------------------------------------------ actions

void ui_open_management(void) {
    if (!app.box_domain || !*app.box_domain) return;
    char *url = lzc_manage_url();
    GtkUriLauncher *l = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(l, window && gtk_widget_get_visible(window) ? GTK_WINDOW(window) : NULL, NULL, NULL, NULL);
    g_object_unref(l);
    g_free(url);
}

void ui_toggle_autostart(void) {
    char *err = NULL;
    bool on = !autostart_enabled();
    if (!autostart_set(on, &err)) app_log("设置开机启动失败：%s", err ? err : "?");
    else app_log("已%s开机自动启动", on ? "开启" : "关闭");
    g_free(err);
    if (window) sync_switches();
    tray_menu_changed();
}

static void on_open(GtkButton *b, gpointer u) { ui_open_management(); }

static void on_retry(GtkButton *b, gpointer u) {
    app_log("手动重新检测");
    lzc_invalidate_cookie();
    net_disconnect();
    lzc_refresh();
}

static void on_copy(GtkButton *b, gpointer u) {
    GString *s = g_string_new("懒猫设备接力 Linux " LZC_MESH_VERSION "\n");
    g_string_append_printf(s, "设备 ID: %s\n微服: %s\n", app.device_id, app.box_domain ? app.box_domain : "-");
    for (int i = 0; i < S_COUNT; i++) g_string_append_printf(s, "%s: %s\n", labels[i], app.status[i].text);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(buf, &a, &z);
    char *log = gtk_text_buffer_get_text(buf, &a, &z, FALSE);
    g_string_append_printf(s, "\n%s", log);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(window), s->str);
    g_free(log);
    g_string_free(s, TRUE);
    app_log("诊断信息已复制到剪贴板");
}

static void on_box_apply(GtkWidget *w, gpointer u) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(box_entry));
    char *name = g_strstrip(g_ascii_strdown(text, -1));
    if (g_str_has_suffix(name, ".heiyu.space")) name[strlen(name) - strlen(".heiyu.space")] = 0;
    for (char *c = name; *c; c++)
        if (!g_ascii_isalnum(*c) && *c != '-') {
            app_log("微服名只能包含字母、数字和 -");
            g_free(name);
            return;
        }
    g_free(app.box_override);
    app.box_override = name;
    config_save();
    app_log("使用微服：%s", *name ? name : "懒猫客户端默认微服");
    lzc_invalidate_cookie();
    net_disconnect();
    lzc_refresh();
}

static gboolean on_sharing(GtkSwitch *s, gboolean state, gpointer u) {
    if (!updating) app_set_sharing(state);
    return FALSE;
}
static gboolean on_swap(GtkSwitch *s, gboolean state, gpointer u) {
    if (!updating) net_send_settings(state);
    return FALSE;
}
static gboolean on_autostart(GtkSwitch *s, gboolean state, gpointer u) {
    if (!updating && state != autostart_enabled()) ui_toggle_autostart();
    return FALSE;
}

static gboolean on_close(GtkWindow *w, gpointer u) {
    // No tray to come back from: closing the window quits instead of leaving an
    // invisible process behind.
    if (!tray_available()) app_quit();
    return FALSE;  // hide_on_close takes care of hiding
}

// ------------------------------------------------------------------ building

static GtkWidget *label(const char *text, const char *cls, bool wrap) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    if (cls) gtk_widget_add_css_class(l, cls);
    if (wrap) {
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    } else {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    }
    return l;
}

static GtkWidget *button(const char *text, GCallback cb, bool primary) {
    GtkWidget *b = gtk_button_new_with_label(text);
    if (primary) gtk_widget_add_css_class(b, "primary");
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *card(const char *title, GtkWidget *trailing, GtkWidget *child) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "card");
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *t = label(title, "card-title", false);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_box_append(GTK_BOX(head), t);
    if (trailing) gtk_box_append(GTK_BOX(head), trailing);
    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *dot(void) {
    GtkWidget *d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(d, "dot");
    gtk_widget_set_valign(d, GTK_ALIGN_START);
    gtk_widget_set_halign(d, GTK_ALIGN_CENTER);
    return d;
}

static GtkWidget *setting_row(const char *title, const char *detail, GtkWidget **sw, GCallback cb) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_box_append(GTK_BOX(texts), label(title, NULL, false));
    gtk_box_append(GTK_BOX(texts), label(detail, "muted", true));
    *sw = gtk_switch_new();
    gtk_widget_set_valign(*sw, GTK_ALIGN_START);
    g_signal_connect(*sw, "state-set", cb, NULL);
    gtk_box_append(GTK_BOX(row), texts);
    gtk_box_append(GTK_BOX(row), *sw);
    return row;
}

static GtkWidget *build_header(void) {
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(h, "header");
    gtk_box_append(GTK_BOX(h), label("懒猫设备接力", "app-title", false));
    pill_dot = dot();
    gtk_widget_set_valign(pill_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(pill_dot, 8);
    pill_text = label("正在连接…", NULL, false);
    gtk_widget_set_valign(pill_text, GTK_ALIGN_CENTER);
    control_text = label("", "strong", false);
    gtk_widget_set_valign(control_text, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(control_text, TRUE);
    GtkWidget *sl = label("键鼠共享", "strong", false);
    gtk_widget_set_valign(sl, GTK_ALIGN_CENTER);
    sharing_switch = gtk_switch_new();
    gtk_widget_set_valign(sharing_switch, GTK_ALIGN_CENTER);
    g_signal_connect(sharing_switch, "state-set", G_CALLBACK(on_sharing), NULL);
    gtk_box_append(GTK_BOX(h), pill_dot);
    gtk_box_append(GTK_BOX(h), pill_text);
    gtk_box_append(GTK_BOX(h), control_text);
    gtk_box_append(GTK_BOX(h), sl);
    gtk_box_append(GTK_BOX(h), sharing_switch);
    return h;
}

static GtkWidget *build_status(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 7);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    for (int i = 0; i < S_COUNT; i++) {
        status_dot[i] = dot();
        GtkWidget *k = label(labels[i], "key", false);
        gtk_widget_set_size_request(k, 96, -1);
        status_text[i] = label(app.status[i].text, NULL, true);
        gtk_label_set_selectable(GTK_LABEL(status_text[i]), TRUE);
        gtk_widget_set_hexpand(status_text[i], TRUE);
        gtk_grid_attach(GTK_GRID(grid), status_dot[i], 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), k, 1, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), status_text[i], 2, i, 1, 1);
    }
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    open_btn = button("打开管理页面", G_CALLBACK(on_open), false);
    gtk_box_append(GTK_BOX(bar), button("重新检测", G_CALLBACK(on_retry), false));
    gtk_box_append(GTK_BOX(bar), open_btn);
    gtk_box_append(GTK_BOX(bar), button("复制诊断信息", G_CALLBACK(on_copy), false));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(box), grid);
    gtk_box_append(GTK_BOX(box), bar);
    return card("连接状态", NULL, box);
}

static void on_transport_mode(GObject *object, GParamSpec *pspec, gpointer unused) {
    guint selected=gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (!updating && selected<G_N_ELEMENTS(transport_modes)) net_send_transport_mode(transport_modes[selected]);
}

static GtkWidget *build_settings(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(box), setting_row("开机自动启动", "登录系统后自动在托盘运行", &autostart_switch, G_CALLBACK(on_autostart)));
    gtk_box_append(GTK_BOX(box), setting_row("交换 Command 和 Ctrl", "Mac 与 Windows/Linux 互控时对调这两个键", &swap_switch, G_CALLBACK(on_swap)));
    const char *choices[]={"自动（局域网优先）","仅蓝牙","仅局域网直连","仅中枢转发",NULL};
    transport_picker=gtk_drop_down_new_from_strings(choices);
    gtk_widget_set_sensitive(transport_picker,FALSE);
    g_signal_connect(transport_picker,"notify::selected",G_CALLBACK(on_transport_mode),NULL);
    gtk_box_append(GTK_BOX(box),label("连接方式（同步到所有设备）",NULL,false));
    gtk_box_append(GTK_BOX(box),transport_picker);
    GtkWidget *boxcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(boxcol), label("使用的微服", NULL, false));
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    box_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(box_entry), "默认使用懒猫客户端的微服，例如 ponzs");
    gtk_editable_set_text(GTK_EDITABLE(box_entry), app.box_override ? app.box_override : "");
    gtk_widget_set_hexpand(box_entry, TRUE);
    g_signal_connect(box_entry, "activate", G_CALLBACK(on_box_apply), NULL);
    gtk_box_append(GTK_BOX(row), box_entry);
    gtk_box_append(GTK_BOX(row), button("应用", G_CALLBACK(on_box_apply), false));
    gtk_box_append(GTK_BOX(boxcol), row);
    gtk_box_append(GTK_BOX(box), boxcol);
    return card("设置", NULL, box);
}

static GtkWidget *build_log(void) {
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_WORD_CHAR);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), log_view);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_size_request(scroll, -1, 160);
    GtkWidget *c = card("事件日志", NULL, scroll);
    gtk_widget_set_vexpand(c, TRUE);
    return c;
}

void ui_build(GtkApplication *gtk_app) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    window = gtk_application_window_new(gtk_app);
    gtk_widget_add_css_class(window, "lzc");
    gtk_window_set_title(GTK_WINDOW(window), "懒猫设备接力");
    gtk_window_set_default_size(GTK_WINDOW(window), 1180, 800);
    // Keep running (and sharing input) when the window is closed; quit from the tray.
    gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close), NULL);
    g_application_hold(G_APPLICATION(gtk_app));

    // Left column
    GtkWidget *hint = label("拖动屏幕调整相对位置，松手后自动贴边并同步到所有设备。控制其他设备时按 Ctrl+Alt+Esc 收回光标。", "muted", true);
    layout_view = layout_view_new();
    GtkWidget *lay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(lay), hint);
    gtk_box_append(GTK_BOX(lay), layout_view);
    GtkWidget *lay_card = card("屏幕布局", NULL, lay);
    gtk_widget_set_vexpand(lay_card, TRUE);
    GtkWidget *upload = NULL;
    GtkWidget *files = files_panel_new(window, &upload);
    GtkWidget *files_card = card("文件共享", upload, files);
    gtk_widget_set_vexpand(files_card, FALSE);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_box_append(GTK_BOX(left), lay_card);
    gtk_box_append(GTK_BOX(left), files_card);

    // Right column. It scrolls on its own, so long status texts never make the window
    // taller than the screen or push the left column out of view.
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_box_append(GTK_BOX(right), build_status());
    gtk_box_append(GTK_BOX(right), build_settings());
    gtk_box_append(GTK_BOX(right), build_log());
    GtkWidget *right_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(right_scroll), right);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(right_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(right_scroll), TRUE);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(right_scroll), FALSE);
    gtk_widget_set_size_request(right_scroll, 420, -1);
    gtk_widget_set_vexpand(right_scroll, TRUE);

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_start(cols, 20);
    gtk_widget_set_margin_end(cols, 20);
    gtk_widget_set_margin_bottom(cols, 20);
    gtk_widget_set_vexpand(cols, TRUE);
    gtk_box_append(GTK_BOX(cols), left);
    gtk_box_append(GTK_BOX(cols), right_scroll);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_header());
    gtk_box_append(GTK_BOX(root), cols);
    gtk_window_set_child(GTK_WINDOW(window), root);
    ui_refresh_status();
    if (!app.background) gtk_window_present(GTK_WINDOW(window));
    app_log("懒猫设备接力 Linux %s 启动，设备 ID %s", LZC_MESH_VERSION, app.device_id);
}

void ui_present(void) {
    if (window) gtk_window_present(GTK_WINDOW(window));
}
