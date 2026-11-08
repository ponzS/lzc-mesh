// Native file sharing. Requests and disk streaming are cancellable; no transfer
// or completion from a previous client lifecycle is resumed after wake.
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "app.h"
#include "files.h"

static GtkWidget *list_box, *empty_label, *status_label, *upload_button;
static SoupSession *session;
static GCancellable *cancellation;
static bool active;
static guint generation;

typedef struct {
    char *name, *temp;
    GCancellable *cancel;
    guint generation;
    GFileOutputStream *out;
} Job;

static bool ready(void) { return active && app_operational(); }
static bool valid(Job *job) { return ready() && job->generation == generation && !g_cancellable_is_cancelled(job->cancel); }
static Job *job_new(const char *name) {
    Job *job = g_new0(Job, 1); job->name = g_strdup(name); job->generation = generation; job->cancel = g_object_ref(cancellation); return job;
}
static void job_free(Job *job) {
    if (job->out) g_object_unref(job->out);
    if (job->temp) { GFile *f = g_file_new_for_path(job->temp); g_file_delete(f, NULL, NULL); g_object_unref(f); }
    g_object_unref(job->cancel); g_free(job->temp); g_free(job->name); g_free(job);
}
static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); char *text = g_strdup_vprintf(fmt, ap); va_end(ap);
    if (status_label) gtk_label_set_text(GTK_LABEL(status_label), text);
    g_free(text);
}
static SoupSession *http(void) {
    if (!session) session = soup_session_new_with_options("user-agent", "lzc-mesh-linux/" LZC_MESH_VERSION, "timeout", 0, NULL);
    return session;
}
static SoupMessage *request(const char *method, const char *name) {
    char *esc = g_uri_escape_string(name, NULL, FALSE);
    char *url = g_strdup_printf("https://lzc-mesh.%s/api/files/%s", app.box_domain, esc);
    SoupMessage *message = soup_message_new(method, url);
    soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    char *cookie = g_strdup_printf("Lzc-Auth-Token=%s", app.cookie ? app.cookie : "");
    char *device = g_uri_escape_string(g_get_host_name(), NULL, FALSE);
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    soup_message_headers_append(headers, "Cookie", cookie);
    soup_message_headers_append(headers, "X-Lzc-Mesh-Device", device);
    soup_message_headers_append(headers, "X-Lzc-Mesh-Device-Id", app.device_id);
    soup_message_headers_append(headers, "X-Lzc-Mesh-Device-Token", app.file_token?app.file_token:"");
    g_free(device); g_free(cookie); g_free(url); g_free(esc); return message;
}
static char *unique_dest(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, NULL);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(path); const char *dot = strrchr(name, '.');
        char *stem = dot && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
        char *alt = g_strdup_printf("%s (%d)%s", stem, n, dot && dot != name ? dot : "");
        path = g_build_filename(dir, alt, NULL); g_free(stem); g_free(alt);
    }
    return path;
}
static const char *downloads(void) {
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD); return dir ? dir : g_get_home_dir();
}
static void splice_done(GObject *src, GAsyncResult *result, gpointer data) {
    Job *job = data; GError *error = NULL;
    gssize count = g_output_stream_splice_finish(G_OUTPUT_STREAM(src), result, &error);
    if (valid(job) && count >= 0) {
        char *path = unique_dest(downloads(), job->name);
        GFile *from = g_file_new_for_path(job->temp), *to = g_file_new_for_path(path);
        if (g_file_move(from, to, G_FILE_COPY_NONE, job->cancel, NULL, NULL, &error)) {
            app_log("已下载 %s → %s", job->name, path); set_status("已下载到 %s", path);
        } else set_status("保存失败：%s", error ? error->message : "?");
        g_object_unref(from); g_object_unref(to); g_free(path);
    } else if (valid(job)) set_status("下载失败：%s", error ? error->message : "?");
    g_clear_error(&error); job_free(job);
}
static void download_done(GObject *src, GAsyncResult *result, gpointer data) {
    Job *job = data; GError *error = NULL;
    GInputStream *in = soup_session_send_finish(SOUP_SESSION(src), result, &error);
    SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(src), result);
    if (valid(job) && in && message && soup_message_get_status(message) == 200) {
        char *uuid = g_uuid_string_random(), *temp = g_strdup_printf(".lzc-mesh-%s.part", uuid);
        job->temp = g_build_filename(downloads(), temp, NULL); g_free(uuid); g_free(temp);
        GFile *file = g_file_new_for_path(job->temp);
        job->out = g_file_create(file, G_FILE_CREATE_PRIVATE, job->cancel, &error); g_object_unref(file);
        if (job->out) {
            g_output_stream_splice_async(G_OUTPUT_STREAM(job->out), in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                        G_PRIORITY_DEFAULT, job->cancel, splice_done, job);
            g_object_unref(in); return;
        }
    }
    if (valid(job)) set_status("下载失败：%s", error ? error->message : "请求失败");
    if (in) { g_input_stream_close(in, NULL, NULL); g_object_unref(in); }
    g_clear_error(&error); job_free(job);
}
static void on_download(GtkButton *button, gpointer unused) {
    if (!ready()) return;
    const char *name = g_object_get_data(G_OBJECT(button), "name");
    Job *job = job_new(name); SoupMessage *message = request("GET", name);
    set_status("正在下载 %s…", name);
    soup_session_send_async(http(), message, G_PRIORITY_DEFAULT, job->cancel, download_done, job); g_object_unref(message);
}
static void mutation_done(GObject *src, GAsyncResult *result, gpointer data) {
    Job *job = data; GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), result, &error);
    SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(src), result);
    if (valid(job)) {
        guint code = message ? soup_message_get_status(message) : 0;
        const char *method = message ? soup_message_get_method(message) : "";
        bool deleting = !strcmp(method, "DELETE");
        if ((deleting && (code == 204 || code == 404)) || (!deleting && code == 200)) {
            app_log(deleting ? "已删除 %s" : "已上传 %s", job->name);
            set_status(deleting ? "已删除 %s" : "已上传 %s", job->name);
        } else set_status("文件操作失败：%s", error ? error->message : "请求被拒绝");
    }
    if (bytes) g_bytes_unref(bytes);
    g_clear_error(&error); job_free(job);
}
static void on_delete(GtkButton *button, gpointer unused) {
    if (!ready()) return;
    const char *name = g_object_get_data(G_OBJECT(button), "name"); Job *job = job_new(name);
    SoupMessage *message = request("DELETE", name);
    soup_session_send_and_read_async(http(), message, G_PRIORITY_DEFAULT, job->cancel, mutation_done, job); g_object_unref(message);
}
static void upload_file(GFile *file) {
    if (!ready()) return;
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE, 0, cancellation, &error);
    GFileInputStream *in = info ? g_file_read(file, cancellation, &error) : NULL;
    if (!in) { set_status("无法读取文件：%s", error ? error->message : "?"); g_clear_error(&error); if (info) g_object_unref(info); return; }
    char *name = g_file_get_basename(file); Job *job = job_new(name);
    SoupMessage *message = request("PUT", name);
    soup_message_set_request_body(message, "application/octet-stream", G_INPUT_STREAM(in), g_file_info_get_size(info));
    set_status("正在上传 %s…", name);
    soup_session_send_and_read_async(http(), message, G_PRIORITY_DEFAULT, job->cancel, mutation_done, job);
    g_object_unref(message); g_object_unref(in); g_object_unref(info); g_free(name);
}
static void chooser_done(GObject *src, GAsyncResult *result, gpointer data) {
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), result, NULL);
    if (!files) return;
    if (ready() && GPOINTER_TO_UINT(data) == generation) for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        GFile *file = g_list_model_get_item(files, i); upload_file(file); g_object_unref(file);
    }
    g_object_unref(files);
}
static void on_upload(GtkButton *button, gpointer window) {
    if (!ready()) { set_status("开启客户端并连接中枢后才能共享文件"); return; }
    GtkFileDialog *dialog = gtk_file_dialog_new(); gtk_file_dialog_set_title(dialog, "选择要共享的文件");
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(window), cancellation, chooser_done, GUINT_TO_POINTER(generation)); g_object_unref(dialog);
}
static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer unused) {
    if (!ready() || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
    for (GSList *item = g_value_get_boxed(value); item; item = item->next) upload_file(item->data);
    return TRUE;
}
void files_set_active(bool on) {
    if (active == on) return;
    active = on; generation++;
    if (cancellation) g_cancellable_cancel(cancellation);
    g_clear_object(&cancellation);
    if (on) cancellation = g_cancellable_new();
    else { if (session) soup_session_abort(session); files_update(NULL); }
    if (upload_button) gtk_widget_set_sensitive(upload_button, on);
    if (list_box) gtk_widget_set_sensitive(list_box, on);
    set_status(on ? "文件共享已就绪" : "客户端已关闭，文件共享已停用");
}
void files_update(JsonArray *array) {
    if (!list_box) return;
    if (!ready()) array = NULL;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list_box))) gtk_list_box_remove(GTK_LIST_BOX(list_box), child);
    guint count = array ? json_array_get_length(array) : 0;
    gtk_widget_set_visible(empty_label, count == 0);
    gtk_label_set_text(GTK_LABEL(empty_label), app.device_ready && !app.device_enabled ? "客户端已关闭，开启后恢复文件共享" : "还没有共享文件。点击“上传文件”或把文件拖到这里。");
    gtk_widget_set_visible(gtk_widget_get_parent(gtk_widget_get_parent(list_box)), count > 0);
    for (guint i = 0; i < count; i++) {
        JsonObject *file = json_array_get_object_element(array, i);
        const char *name = json_object_get_string_member_with_default(file, "name", "");
        const char *from = json_object_get_string_member_with_default(file, "uploader", "");
        char *size = g_format_size(json_object_get_int_member_with_default(file, "size", 0));
        GDateTime *date = g_date_time_new_from_unix_local(json_object_get_int_member_with_default(file, "modified", 0));
        char *when = g_date_time_format(date, "%m-%d %H:%M");
        char *meta = g_strdup_printf("%s · %s · %s", size, *from ? from : "-", when);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *label = gtk_label_new(name), *detail = gtk_label_new(meta);
        gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE); gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(detail, "muted");
        GtkWidget *download = gtk_button_new_with_label("下载"), *remove = gtk_button_new_with_label("删除");
        g_object_set_data_full(G_OBJECT(download), "name", g_strdup(name), g_free); g_object_set_data_full(G_OBJECT(remove), "name", g_strdup(name), g_free);
        g_signal_connect(download, "clicked", G_CALLBACK(on_download), NULL); g_signal_connect(remove, "clicked", G_CALLBACK(on_delete), NULL);
        gtk_box_append(GTK_BOX(row), label); gtk_box_append(GTK_BOX(row), detail); gtk_box_append(GTK_BOX(row), download); gtk_box_append(GTK_BOX(row), remove);
        gtk_list_box_append(GTK_LIST_BOX(list_box), row);
        g_free(meta); g_free(size); g_free(when); g_date_time_unref(date);
    }
}
GtkWidget *files_panel_new(GtkWidget *window, GtkWidget **upload) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    upload_button = gtk_button_new_with_label("上传文件"); gtk_widget_add_css_class(upload_button, "primary");
    gtk_widget_set_sensitive(upload_button, FALSE); g_signal_connect(upload_button, "clicked", G_CALLBACK(on_upload), window); *upload = upload_button;
    list_box = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_box);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC); gtk_widget_set_size_request(scroll, -1, 150); gtk_widget_set_visible(scroll, FALSE);
    empty_label = gtk_label_new("连接并开启客户端后显示共享文件"); gtk_widget_add_css_class(empty_label, "muted"); gtk_widget_set_size_request(empty_label, -1, 150);
    status_label = gtk_label_new("所有设备都能下载；下载到“下载”文件夹，也可把文件拖进来"); gtk_widget_add_css_class(status_label, "muted");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0); gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), empty_label); gtk_box_append(GTK_BOX(box), scroll); gtk_box_append(GTK_BOX(box), status_label);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY); g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL); gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));
    return box;
}
