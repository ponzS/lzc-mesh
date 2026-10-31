// Shared files panel for the Linux client: lists the files on the LPK and
// uploads/downloads/deletes them over the LazyCat network with the session cookie.
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "app.h"
#include "files.h"

static GtkWidget *list_box, *empty_label, *status_label;
static SoupSession *session;

static SoupSession *http(void) {
    if (!session) session = soup_session_new_with_options("user-agent", "lzc-mesh-linux/" LZC_MESH_VERSION, "timeout", 0, NULL);
    return session;
}

static char *file_url(const char *name) {
    char *esc = g_uri_escape_string(name, NULL, FALSE);
    char *url = g_strdup_printf("https://lzc-mesh.%s/api/files/%s", app.box_domain, esc);
    g_free(esc);
    return url;
}

static SoupMessage *request(const char *method, const char *url) {
    SoupMessage *m = soup_message_new(method, url);
    char *cookie = g_strdup_printf("Lzc-Auth-Token=%s", app.cookie ? app.cookie : "");
    soup_message_headers_append(soup_message_get_request_headers(m), "Cookie", cookie);
    char *dev = g_uri_escape_string(g_get_host_name(), NULL, FALSE);
    soup_message_headers_append(soup_message_get_request_headers(m), "X-Lzc-Mesh-Device", dev);
    g_free(dev);
    g_free(cookie);
    return m;
}

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (status_label) gtk_label_set_text(GTK_LABEL(status_label), t);
    g_free(t);
}

static char *human_size(gint64 n) { return g_format_size((guint64)n); }

// ------------------------------------------------------------ download

typedef struct {
    char *name;
    char *dest;
} Download;

static void download_done(GObject *src, GAsyncResult *res, gpointer data) {
    Download *d = data;
    GError *err = NULL;
    GInputStream *in = soup_session_send_finish(SOUP_SESSION(src), res, &err);
    SoupMessage *m = soup_session_get_async_result_message(SOUP_SESSION(src), res);
    if (in && soup_message_get_status(m) == 200) {
        GFile *f = g_file_new_for_path(d->dest);
        GFileOutputStream *out = g_file_replace(f, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &err);
        if (out && g_output_stream_splice(G_OUTPUT_STREAM(out), in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, &err) >= 0) {
            app_log("已下载 %s → %s", d->name, d->dest);
            set_status("已下载到 %s", d->dest);
        } else {
            app_log("下载 %s 失败：%s", d->name, err ? err->message : "?");
            set_status("下载失败：%s", err ? err->message : "?");
        }
        if (out) g_object_unref(out);
        g_object_unref(f);
    } else {
        app_log("下载 %s 失败：%s", d->name, err ? err->message : soup_message_get_reason_phrase(m));
        set_status("下载失败");
    }
    g_clear_error(&err);
    if (in) g_object_unref(in);
    g_free(d->name);
    g_free(d->dest);
    g_free(d);
}

static char *unique_dest(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, NULL);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(path);
        const char *dot = strrchr(name, '.');
        char *stem = dot && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
        char *alt = g_strdup_printf("%s (%d)%s", stem, n, dot && dot != name ? dot : "");
        path = g_build_filename(dir, alt, NULL);
        g_free(stem);
        g_free(alt);
    }
    return path;
}

static void on_download(GtkButton *b, gpointer u) {
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (!dir) dir = g_get_home_dir();
    Download *d = g_new0(Download, 1);
    d->name = g_strdup(name);
    d->dest = unique_dest(dir, name);
    char *url = file_url(name);
    SoupMessage *m = request("GET", url);
    set_status("正在下载 %s…", name);
    soup_session_send_async(http(), m, G_PRIORITY_DEFAULT, NULL, download_done, d);
    g_object_unref(m);
    g_free(url);
}

// ------------------------------------------------------------ delete

static void delete_done(GObject *src, GAsyncResult *res, gpointer data) {
    char *name = data;
    GError *err = NULL;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    SoupMessage *m = soup_session_get_async_result_message(SOUP_SESSION(src), res);
    guint code = soup_message_get_status(m);
    if (code == 204 || code == 404) set_status("已删除 %s", name);
    else set_status("删除失败：%s", err ? err->message : soup_message_get_reason_phrase(m));
    if (b) g_bytes_unref(b);
    g_clear_error(&err);
    g_free(name);
}

static void on_delete(GtkButton *b, gpointer u) {
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    char *url = file_url(name);
    SoupMessage *m = request("DELETE", url);
    soup_session_send_and_read_async(http(), m, G_PRIORITY_DEFAULT, NULL, delete_done, g_strdup(name));
    g_object_unref(m);
    g_free(url);
}

// ------------------------------------------------------------ upload

static void upload_done(GObject *src, GAsyncResult *res, gpointer data) {
    char *name = data;
    GError *err = NULL;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    SoupMessage *m = soup_session_get_async_result_message(SOUP_SESSION(src), res);
    guint code = soup_message_get_status(m);
    if (code == 200) {
        app_log("已上传 %s", name);
        set_status("已上传 %s", name);
    } else {
        app_log("上传 %s 失败：%s", name, err ? err->message : soup_message_get_reason_phrase(m));
        set_status(code == 413 ? "上传失败：文件过大" : "上传失败");
    }
    if (b) g_bytes_unref(b);
    g_clear_error(&err);
    g_free(name);
}

static void upload_file(GFile *file) {
    GError *err = NULL;
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, 0, NULL, &err);
    GFileInputStream *in = info ? g_file_read(file, NULL, &err) : NULL;
    if (!in) {
        set_status("无法读取文件：%s", err ? err->message : "?");
        g_clear_error(&err);
        if (info) g_object_unref(info);
        return;
    }
    char *name = g_file_get_basename(file);
    char *url = file_url(name);
    SoupMessage *m = request("PUT", url);
    soup_message_set_request_body(m, "application/octet-stream", G_INPUT_STREAM(in), g_file_info_get_size(info));
    char *size = human_size(g_file_info_get_size(info));
    set_status("正在上传 %s（%s）…", name, size);
    soup_session_send_and_read_async(http(), m, G_PRIORITY_DEFAULT, NULL, upload_done, name);
    g_free(size);
    g_free(url);
    g_object_unref(m);
    g_object_unref(in);
    g_object_unref(info);
}

static void chooser_done(GObject *src, GAsyncResult *res, gpointer u) {
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!files) return;
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        GFile *f = g_list_model_get_item(files, i);
        upload_file(f);
        g_object_unref(f);
    }
    g_object_unref(files);
}

static void on_upload(GtkButton *b, gpointer win) {
    if (!app.hub_connected) {
        set_status("连接中枢后才能上传");
        return;
    }
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "选择要共享的文件");
    gtk_file_dialog_open_multiple(d, GTK_WINDOW(win), NULL, chooser_done, NULL);
    g_object_unref(d);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *v, double x, double y, gpointer u) {
    if (!G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST) || !app.hub_connected) return FALSE;
    for (GSList *l = g_value_get_boxed(v); l; l = l->next) upload_file(l->data);
    return TRUE;
}

// ------------------------------------------------------------ list

void files_update(JsonArray *arr) {
    if (!list_box) return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list_box))) gtk_list_box_remove(GTK_LIST_BOX(list_box), child);
    guint n = arr ? json_array_get_length(arr) : 0;
    gtk_widget_set_visible(empty_label, n == 0);
    gtk_widget_set_visible(gtk_widget_get_parent(gtk_widget_get_parent(list_box)), n > 0);
    for (guint i = 0; i < n; i++) {
        JsonObject *f = json_array_get_object_element(arr, i);
        const char *name = json_object_get_string_member_with_default(f, "name", "");
        const char *from = json_object_get_string_member_with_default(f, "uploader", "");
        char *size = human_size(json_object_get_int_member_with_default(f, "size", 0));
        GDateTime *t = g_date_time_new_from_unix_local(json_object_get_int_member_with_default(f, "modified", 0));
        char *when = g_date_time_format(t, "%m-%d %H:%M");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(name);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(label, TRUE);
        char *meta = g_strdup_printf("%s · %s · %s", size, *from ? from : "-", when);
        GtkWidget *ml = gtk_label_new(meta);
        gtk_widget_add_css_class(ml, "muted");
        GtkWidget *dl = gtk_button_new_with_label("下载");
        GtkWidget *rm = gtk_button_new_with_label("删除");
        g_object_set_data_full(G_OBJECT(dl), "name", g_strdup(name), g_free);
        g_object_set_data_full(G_OBJECT(rm), "name", g_strdup(name), g_free);
        g_signal_connect(dl, "clicked", G_CALLBACK(on_download), NULL);
        g_signal_connect(rm, "clicked", G_CALLBACK(on_delete), NULL);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), ml);
        gtk_box_append(GTK_BOX(row), dl);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(GTK_LIST_BOX(list_box), row);
        g_free(meta);
        g_free(when);
        g_date_time_unref(t);
        g_free(size);
    }
}

GtkWidget *files_panel_new(GtkWidget *window, GtkWidget **upload_button) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    // The upload button goes in the card's title row (returned to the caller).
    GtkWidget *up = gtk_button_new_with_label("上传文件");
    gtk_widget_add_css_class(up, "primary");
    g_signal_connect(up, "clicked", G_CALLBACK(on_upload), window);
    *upload_button = up;
    list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_box);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_widget_set_visible(scroll, FALSE);
    empty_label = gtk_label_new("还没有共享文件。点击“上传文件”或把文件拖到这里。");
    gtk_widget_add_css_class(empty_label, "muted");
    gtk_widget_set_size_request(empty_label, -1, 150);
    status_label = gtk_label_new("所有设备都能下载；下载到“下载”文件夹，也可把文件拖进来");
    gtk_widget_add_css_class(status_label, "muted");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), empty_label);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), status_label);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));
    return box;
}
