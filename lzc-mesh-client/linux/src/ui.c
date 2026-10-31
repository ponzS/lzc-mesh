// Main window: status of every dependency (for troubleshooting), the layout
// editor, the shared settings and a live event log.
#include <string.h>

#include "app.h"
#include "lzc.h"
#include "net.h"
#include "ui.h"

#include "config.h"

static GtkWidget *window, *layout_view, *swap_check, *log_view, *open_btn, *box_entry;
static GtkWidget *status_dot[S_COUNT], *status_text[S_COUNT];
static bool updating_swap;

static const char *labels[S_COUNT] = {
    "懒猫微服客户端", "懒猫网络", "登录会话", "LZC Mesh 应用", "中枢连接", "输入权限", "控制状态", "本机屏幕",
};

static const char *css =
    "window { background: #f5f5f5; color: #111; }"
    ".card { background: #ffffff; border: 1px solid #e2e2e2; border-radius: 10px; padding: 14px; }"
    ".title { font-weight: 600; font-size: 13px; }"
    ".muted { color: #777; font-size: 12px; }"
    ".key { color: #555; }"
    ".dot { min-width: 8px; min-height: 8px; border-radius: 4px; background: #c8c8c8; }"
    ".dot.ok { background: #111; }"
    ".dot.warn { background: #888; }"
    ".dot.error { background: #fff; border: 2px solid #111; min-width: 4px; min-height: 4px; }"
    ".error-text { color: #111; font-weight: 600; }"
    "textview, textview text { background: #fafafa; color: #333; font-family: monospace; font-size: 11px; }"
    "button { border-radius: 6px; }";

void ui_refresh_status(void) {
    if (!window) return;
    for (int i = 0; i < S_COUNT; i++) {
        Status *s = &app.status[i];
        GtkWidget *dot = status_dot[i];
        gtk_widget_remove_css_class(dot, "ok");
        gtk_widget_remove_css_class(dot, "warn");
        gtk_widget_remove_css_class(dot, "error");
        if (s->level == ST_OK) gtk_widget_add_css_class(dot, "ok");
        if (s->level == ST_WARN) gtk_widget_add_css_class(dot, "warn");
        if (s->level == ST_ERROR) gtk_widget_add_css_class(dot, "error");
        gtk_label_set_text(GTK_LABEL(status_text[i]), s->text ? s->text : "");
        if (s->level == ST_ERROR) gtk_widget_add_css_class(status_text[i], "error-text");
        else gtk_widget_remove_css_class(status_text[i], "error-text");
    }
    gtk_widget_set_sensitive(open_btn, app.box_domain && *app.box_domain);
}

void ui_refresh_layout(void) {
    if (!window) return;
    layout_view_refresh(layout_view);
    updating_swap = true;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(swap_check), app.swap_cmd_ctrl);
    updating_swap = false;
}

void ui_append_log(const char *line) {
    if (!log_view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, line, -1);
    gtk_text_buffer_insert(buf, &end, "\n", -1);
    // Keep the log bounded.
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

static GtkWidget *card(const char *title, GtkWidget *child) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "card");
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static void on_swap(GtkCheckButton *b, gpointer u) {
    if (!updating_swap) net_send_settings(gtk_check_button_get_active(b));
}

static void on_open(GtkButton *b, gpointer u) {
    char *url = lzc_manage_url();
    GtkUriLauncher *l = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(l, GTK_WINDOW(window), NULL, NULL, NULL);
    g_object_unref(l);
    g_free(url);
}

static void on_retry(GtkButton *b, gpointer u) {
    app_log("手动重新检测");
    lzc_invalidate_cookie();
    net_disconnect();
    lzc_refresh();
}

static void on_copy(GtkButton *b, gpointer u) {
    GString *s = g_string_new("LZC Mesh Linux " LZC_MESH_VERSION "\n");
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

static GtkWidget *box_row(void) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *k = gtk_label_new("使用的微服");
    gtk_widget_add_css_class(k, "key");
    box_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(box_entry), "留空使用懒猫客户端默认微服，例如 ponzs");
    gtk_editable_set_text(GTK_EDITABLE(box_entry), app.box_override ? app.box_override : "");
    gtk_widget_set_hexpand(box_entry, TRUE);
    g_signal_connect(box_entry, "activate", G_CALLBACK(on_box_apply), NULL);
    GtkWidget *apply = gtk_button_new_with_label("应用");
    g_signal_connect(apply, "clicked", G_CALLBACK(on_box_apply), NULL);
    gtk_box_append(GTK_BOX(row), k);
    gtk_box_append(GTK_BOX(row), box_entry);
    gtk_box_append(GTK_BOX(row), apply);
    return row;
}

static GtkWidget *status_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    for (int i = 0; i < S_COUNT; i++) {
        status_dot[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(status_dot[i], "dot");
        gtk_widget_set_valign(status_dot[i], GTK_ALIGN_CENTER);
        GtkWidget *k = gtk_label_new(labels[i]);
        gtk_widget_add_css_class(k, "key");
        gtk_label_set_xalign(GTK_LABEL(k), 0);
        status_text[i] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(status_text[i]), 0);
        gtk_label_set_wrap(GTK_LABEL(status_text[i]), TRUE);
        gtk_label_set_selectable(GTK_LABEL(status_text[i]), TRUE);
        gtk_widget_set_hexpand(status_text[i], TRUE);
        gtk_grid_attach(GTK_GRID(grid), status_dot[i], 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), k, 1, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), status_text[i], 2, i, 1, 1);
    }
    return grid;
}

void ui_build(GtkApplication *gtk_app) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), "LZC Mesh");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 820);
    // Keep running (and sharing input) when the window is closed; quit from the button.
    gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);
    g_application_hold(G_APPLICATION(gtk_app));

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(status), status_grid());
    gtk_box_append(GTK_BOX(status), box_row());
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *retry = gtk_button_new_with_label("重新检测");
    g_signal_connect(retry, "clicked", G_CALLBACK(on_retry), NULL);
    open_btn = gtk_button_new_with_label("打开管理页面");
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open), NULL);
    GtkWidget *copy = gtk_button_new_with_label("复制诊断信息");
    g_signal_connect(copy, "clicked", G_CALLBACK(on_copy), NULL);
    GtkWidget *quit = gtk_button_new_with_label("退出");
    g_signal_connect_swapped(quit, "clicked", G_CALLBACK(g_application_quit), gtk_app);
    gtk_box_append(GTK_BOX(actions), retry);
    gtk_box_append(GTK_BOX(actions), open_btn);
    gtk_box_append(GTK_BOX(actions), copy);
    gtk_box_append(GTK_BOX(actions), quit);
    gtk_box_append(GTK_BOX(status), actions);
    gtk_box_append(GTK_BOX(root), card("状态", status));

    GtkWidget *lay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *hint = gtk_label_new("拖动屏幕调整相对位置，松手后自动贴边并同步到所有设备。控制其他设备时按 Ctrl+Alt+Esc 收回光标。");
    gtk_widget_add_css_class(hint, "muted");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    layout_view = layout_view_new();
    swap_check = gtk_check_button_new_with_label("Mac 与 Windows/Linux 互控时交换 Command 和 Ctrl");
    g_signal_connect(swap_check, "toggled", G_CALLBACK(on_swap), NULL);
    gtk_box_append(GTK_BOX(lay), hint);
    gtk_box_append(GTK_BOX(lay), layout_view);
    gtk_box_append(GTK_BOX(lay), swap_check);
    GtkWidget *lay_card = card("屏幕布局", lay);
    gtk_widget_set_vexpand(lay_card, TRUE);
    gtk_box_append(GTK_BOX(root), lay_card);

    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), log_view);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_box_append(GTK_BOX(root), card("事件日志", scroll));

    gtk_window_set_child(GTK_WINDOW(window), root);
    ui_refresh_status();
    gtk_window_present(GTK_WINDOW(window));
    app_log("LZC Mesh Linux %s 启动，设备 ID %s", LZC_MESH_VERSION, app.device_id);
}

void ui_present(void) {
    if (window) gtk_window_present(GTK_WINDOW(window));
}
