// LazyCat network status and sign-in credentials from the local desktop client
// or hclient-cli. Requests to the box then travel over the LazyCat network.
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "app.h"
#include "lzc.h"
#include "lzc_cli.h"
#include "net.h"

static SoupSession *session;
static SoupSession *local_session;
static bool refreshing;
static gint64 service_checked_at;

static SoupSession *http(void) {
    if (!session) {
        session = lzc_http_session_new(FALSE);
        soup_session_set_user_agent(session, "lzc-mesh-linux/" LZC_MESH_VERSION);
    }
    return session;
}

static SoupSession *local_http(void) {
    if (!local_session) {
        local_session = lzc_http_session_new(TRUE);
        soup_session_set_user_agent(local_session, "lzc-mesh-linux/" LZC_MESH_VERSION);
    }
    return local_session;
}

typedef struct {
    int port;
    char *token;
    char *base;
    bool cli;
    LzcLocalPeer *peer;
} LocalApi;

static bool read_cli_api(LocalApi *out, char **error) {
    char *dir = g_build_filename(g_get_home_dir(), ".config", "hportal-client", NULL);
    char *path = g_build_filename(dir, "hclient-cli-daemon.json", NULL);
    char *credential_path = g_build_filename(dir, "shellapi_cred", NULL);
    JsonParser *parser = json_parser_new();
    GError *io_error = NULL;
    char *contents = lzc_read_private_file(path, 64 * 1024, &io_error);
    bool ok = false;
    if (!contents || !json_parser_load_from_data(parser, contents, -1, NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        *error = g_strdup(io_error ? io_error->message : "hclient-cli 运行配置无效，请启动并登录懒猫客户端");
        goto out;
    }
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    JsonNode *pid_node = json_object_get_member(object, "pid");
    gint64 pid = pid_node && json_node_get_value_type(pid_node) == G_TYPE_INT64 ?
                 json_node_get_int(pid_node) : 0;
    if (pid <= 1 || pid > G_MAXINT || !(out->peer = lzc_local_peer_new(pid, "hclient-cli", &io_error))) {
        *error = g_strdup(io_error ? io_error->message : "hclient-cli 运行配置缺少有效的 PID");
        goto out;
    }
    JsonNode *address_node = json_object_get_member(object, "shellapi_address");
    const char *address = address_node && json_node_get_value_type(address_node) == G_TYPE_STRING ?
                          json_node_get_string(address_node) : NULL;
    GSocketConnectable *endpoint = address ? g_network_address_parse(address, 0, NULL) : NULL;
    if (!endpoint) {
        *error = g_strdup("hclient-cli 缺少有效的 shellapi_address，请重启命令行客户端");
        goto out;
    }
    const char *host = g_network_address_get_hostname(G_NETWORK_ADDRESS(endpoint));
    guint16 port = g_network_address_get_port(G_NETWORK_ADDRESS(endpoint));
    GInetAddress *ip = g_inet_address_new_from_string(host);
    bool loopback = ip && g_inet_address_get_is_loopback(ip) && port;
    g_clear_object(&ip);
    if (loopback) {
        out->base = g_strdup_printf(strchr(host, ':') ? "http://[%s]:%u" : "http://%s:%u", host, port);
        out->port = port;
    }
    g_object_unref(endpoint);
    if (!loopback) {
        *error = g_strdup("hclient-cli 的凭证接口必须使用本机回环地址");
        goto out;
    }
    out->token = lzc_read_private_file(credential_path, 1024, &io_error);
    if (!out->token) {
        *error = g_strdup(io_error ? io_error->message : "无法读取 hclient-cli 本机接口凭证");
        goto out;
    }
    g_strstrip(out->token);
    if (!*out->token || strlen(out->token) > 1024 || strpbrk(out->token, "\r\n")) {
        *error = g_strdup("hclient-cli 本机接口凭证无效");
        goto out;
    }
    out->cli = true;
    ok = true;
out:
    if (!ok) { g_clear_pointer(&out->token, g_free); g_clear_pointer(&out->base, g_free); }
    if (!ok) { lzc_local_peer_free(out->peer); out->peer = NULL; }
    g_clear_error(&io_error);
    g_free(contents);
    g_object_unref(parser);
    g_free(dir); g_free(path); g_free(credential_path);
    return ok;
}

static bool read_local_api(LocalApi *out, char **error) {
    char *path = g_build_filename(g_get_home_dir(), ".config", "lzc-client-desktop", "core.cfg", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return read_cli_api(out, error);
    }
    JsonParser *p = json_parser_new();
    bool ok = false;
    if (!json_parser_load_from_file(p, path, NULL)) {
        *error = g_strdup_printf("未找到懒猫微服客户端配置 %s，请安装并登录懒猫微服客户端", path);
    } else if (JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        out->port = (int)json_object_get_int_member_with_default(o, "http_port", 0);
        JsonObject *h = json_object_has_member(o, "extra_headers") ? json_object_get_object_member(o, "extra_headers") : NULL;
        out->token = g_strdup(h ? json_object_get_string_member_with_default(h, "LZC_MISC_AUTH_TOKEN", "") : "");
        ok = out->port > 0 && *out->token;
        if (!ok) *error = g_strdup("懒猫微服客户端配置不完整，请重启懒猫微服客户端");
    }
    g_object_unref(p);
    g_free(path);
    return ok;
}

static JsonNode *get_json(const char *url, const char *header, const char *value, char **error) {
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) {
        *error = g_strdup("无效地址");
        return NULL;
    }
    GError *err = NULL;
    SoupSession *transport = !g_strcmp0(g_uri_get_scheme(soup_message_get_uri(msg)), "http") ?
                             local_http() : http();
    GBytes *body = lzc_http_send_authenticated(transport, msg, NULL, header, value, &err);
    JsonNode *node = NULL;
    guint code = soup_message_get_status(msg);
    if (!body) {
        *error = g_strdup(err ? err->message : "请求失败");
    } else if (code != 200) {
        *error = g_strdup_printf("HTTP %u", code);
    } else {
        JsonParser *p = json_parser_new();
        gsize len;
        const char *data = g_bytes_get_data(body, &len);
        if (json_parser_load_from_data(p, data, (gssize)len, NULL)) node = json_node_copy(json_parser_get_root(p));
        else *error = g_strdup("响应不是 JSON");
        g_object_unref(p);
    }
    if (body) g_bytes_unref(body);
    g_clear_error(&err);
    g_object_unref(msg);
    return node;
}

static const char *box_status_text(int status) {
    switch (status) {
    case 0: return "未激活";
    case 1: return "连接中";
    case 2: return "已连接";
    case 3: return "连接失败";
    case 4: return "正在激活";
    default: return "未知";
    }
}

// Runs in a worker thread; returns results through the struct below.
typedef struct {
    StatusLevel client_level, net_level, login_level, service_level;
    char *client_text, *net_text, *login_text, *service_text;
    char *box_name, *box_domain, *login_user, *cookie;
    char *override;  // box name chosen in the client, empty for the LazyCat default box
    bool check_service;
} Probe;

static void probe_free(Probe *p) {
    g_free(p->client_text); g_free(p->net_text); g_free(p->login_text); g_free(p->service_text);
    g_free(p->box_name); g_free(p->box_domain); g_free(p->login_user); g_free(p->cookie);
    g_free(p->override);
    g_free(p);
}

static void probe_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    Probe *p = data;
    LocalApi api = {0};
    char *error = NULL;
    p->net_level = p->login_level = p->service_level = ST_UNKNOWN;
    p->net_text = g_strdup("等待懒猫微服客户端");
    p->login_text = g_strdup("等待懒猫网络");
    p->service_text = g_strdup("等待登录");
    if (!read_local_api(&api, &error)) {
        p->client_level = ST_ERROR;
        p->client_text = error;
        return;
    }
    char *base = api.cli ? g_strdup(api.base) : g_strdup_printf("http://127.0.0.1:%d", api.port);
    char *url = g_strdup_printf("%s/default-box", base);
    JsonNode *box = api.cli ? lzc_cli_default_box(local_http(), api.peer, base, api.token, p->override, &error) :
                             get_json(url, "LZC_MISC_AUTH_TOKEN", api.token, &error);
    g_free(url);
    if (!box || !JSON_NODE_HOLDS_OBJECT(box)) {
        p->client_level = ST_ERROR;
        p->client_text = g_strdup_printf("%s 本地接口不可用（%s：%s）", api.cli ? "hclient-cli" : "懒猫桌面客户端",
                                        base, error ? error : "无数据");
        if (box) json_node_unref(box);
        goto out;
    }
    p->client_level = ST_OK;
    p->client_text = g_strdup_printf("%s 运行中（%s）", api.cli ? "hclient-cli" : "懒猫桌面客户端", base);
    JsonObject *o = json_node_get_object(box);
    p->box_name = g_strdup(json_object_get_string_member_with_default(o, "boxName", ""));
    p->box_domain = g_strdup(json_object_get_string_member_with_default(o, "boxDomain", ""));
    p->login_user = g_strdup(json_object_get_string_member_with_default(o, "loginUser", ""));
    int status = (int)json_object_get_int_member_with_default(o, "status", -1);
    bool use_override = *p->override && g_strcmp0(p->override, p->box_name) != 0;
    if (use_override) {
        // A non-default box: the LazyCat client only reports state of its default box,
        // so reachability is verified below with the box's own /sys/whoami.
        g_free(p->box_name); p->box_name = g_strdup(p->override);
        g_free(p->box_domain); p->box_domain = g_strdup_printf("%s.heiyu.space", p->override);
        g_free(p->login_user); p->login_user = g_strdup("");
        status = 2;
    }
    if (!*p->box_domain) {
        p->net_level = ST_ERROR;
        g_free(p->net_text);
        p->net_text = g_strdup("懒猫微服客户端没有默认微服，请先在客户端中登录一台微服");
        json_node_unref(box);
        goto out;
    }
    p->net_level = status == 2 ? ST_OK : status == 1 || status == 4 ? ST_WARN : ST_ERROR;
    g_free(p->net_text);
    p->net_text = g_strdup_printf("%s · %s · 用户 %s%s", p->box_domain, box_status_text(status), *p->login_user ? p->login_user : "未登录",
                                  use_override || (api.cli && *p->override) ? "" : "（懒猫客户端默认微服）");
    json_node_unref(box);
    if (status != 2) goto out;

    if (api.cli) {
        g_clear_pointer(&error, g_free);
        p->cookie = lzc_cli_auth_token(local_http(), api.peer, base, api.token, &error);
    } else {
        char *esc = g_uri_escape_string(p->box_domain, NULL, FALSE);
        url = g_strdup_printf("%s/box-ext-info?url=https%%3A%%2F%%2F%s%%2F", base, esc);
        g_free(esc);
        g_clear_pointer(&error, g_free);
        JsonNode *ext = get_json(url, "LZC_MISC_AUTH_TOKEN", api.token, &error);
        g_free(url);
        if (ext && JSON_NODE_HOLDS_OBJECT(ext)) {
            JsonObject *eo = json_node_get_object(ext);
            JsonArray *cookies = json_object_has_member(eo, "cookies") ? json_object_get_array_member(eo, "cookies") : NULL;
            for (guint i = 0; cookies && i < json_array_get_length(cookies); i++) {
                JsonObject *ck = json_array_get_object_element(cookies, i);
                if (g_strcmp0(json_object_get_string_member_with_default(ck, "name", ""), "Lzc-Auth-Token") == 0)
                    p->cookie = g_strdup(json_object_get_string_member_with_default(ck, "value", ""));
            }
        }
        if (ext) json_node_unref(ext);
    }
    g_free(p->login_text);
    if (p->cookie && *p->cookie && (use_override || api.cli)) {
        // Verify the network path and the session in one request.
        char *who = g_strdup_printf("https://%s/sys/whoami", p->box_domain);
        char *ck = g_strdup_printf("Lzc-Auth-Token=%s", p->cookie);
        g_clear_pointer(&error, g_free);
        JsonNode *w = get_json(who, "Cookie", ck, &error);
        g_free(who);
        g_free(ck);
        g_free(p->net_text);
        if (w && JSON_NODE_HOLDS_OBJECT(w)) {
            g_free(p->login_user);
            p->login_user = g_strdup(json_object_get_string_member_with_default(json_node_get_object(w), "uid", ""));
            p->net_text = g_strdup_printf("%s · 已连接 · 用户 %s（%s）", p->box_domain, p->login_user,
                                         api.cli ? "hclient-cli" : "手动指定的微服");
        } else {
            p->net_level = ST_ERROR;
            p->net_text = g_strdup_printf("%s 不可达或会话无效（%s），请确认懒猫微服客户端已登录该微服", p->box_domain, error ? error : "?");
            g_clear_pointer(&p->cookie, g_free);
        }
        if (w) json_node_unref(w);
    }
    if (p->cookie && *p->cookie) {
        p->login_level = ST_OK;
        p->login_text = g_strdup_printf("已获取 %s 的登录会话", p->box_domain);
    } else {
        p->login_level = ST_ERROR;
        p->login_text = g_strdup_printf("没有拿到登录会话%s%s，请在懒猫微服客户端中重新登录", error ? "：" : "", error ? error : "");
        goto out;
    }

    if (!p->check_service) goto out;
    url = g_strdup_printf("https://lzc-mesh.%s/api/whoami", p->box_domain);
    char *cookie = g_strdup_printf("Lzc-Auth-Token=%s", p->cookie);
    g_clear_pointer(&error, g_free);
    JsonNode *who = get_json(url, "Cookie", cookie, &error);
    g_free(cookie);
    g_free(p->service_text);
    if (who && JSON_NODE_HOLDS_OBJECT(who)) {
        JsonObject *wo = json_node_get_object(who);
        p->service_level = ST_OK;
        p->service_text = g_strdup_printf("lzc-mesh.%s · 版本 %s", p->box_domain, json_object_get_string_member_with_default(wo, "version", "?"));
    } else {
        p->service_level = ST_ERROR;
        bool missing = error && (g_str_has_prefix(error, "HTTP 404") || g_str_has_prefix(error, "HTTP 502") || g_str_has_prefix(error, "HTTP 503"));
        p->service_text = g_strdup_printf("%s（%s）", missing ? "微服上未安装或未启动懒猫设备接力应用" : "无法访问懒猫设备接力应用", error ? error : "?");
    }
    if (who) json_node_unref(who);
    g_free(url);
out:
    g_free(error);
    g_free(base);
    g_free(api.token);
    g_free(api.base);
    lzc_local_peer_free(api.peer);
}

static void probe_done(GObject *src, GAsyncResult *res, gpointer data) {
    Probe *p = g_task_get_task_data(G_TASK(res));
    refreshing = false;
    app_set_status(S_LZC_CLIENT, p->client_level, "%s", p->client_text);
    app_set_status(S_LZC_NETWORK, p->net_level, "%s", p->net_text);
    app_set_status(S_LOGIN, p->login_level, "%s", p->login_text);
    if (p->check_service) {
        service_checked_at = g_get_monotonic_time();
        app_set_status(S_SERVICE, p->service_level, "%s", p->service_text);
    }
    bool changed = g_strcmp0(app.box_domain, p->box_domain) != 0 || g_strcmp0(app.cookie, p->cookie) != 0;
    g_free(app.box_name); app.box_name = g_strdup(p->box_name);
    g_free(app.box_domain); app.box_domain = g_strdup(p->box_domain);
    g_free(app.login_user); app.login_user = g_strdup(p->login_user);
    g_free(app.cookie); app.cookie = g_strdup(p->cookie);
    bool ready = p->net_level == ST_OK && p->login_level == ST_OK && app.status[S_SERVICE].level == ST_OK;
    if (changed && (app.hub_connected || net_busy())) {
        app_log("懒猫微服或登录会话变化，重新连接");
        net_disconnect();
    }
    if (ready && !app.hub_connected && !net_busy()) net_connect();
    if (!ready && (app.hub_connected || net_busy())) net_disconnect();
    if (!ready) app_set_status(S_HUB, ST_UNKNOWN, "等待懒猫设备接力应用可用");
}

void lzc_refresh(void) {
    if (refreshing) return;
    refreshing = true;
    Probe *p = g_new0(Probe, 1);
    // The service probe is cheap but not needed every 3s while the hub is connected.
    p->override = g_strdup(app.box_override ? app.box_override : "");
    p->check_service = !app.hub_connected || g_get_monotonic_time() - service_checked_at > 30 * G_USEC_PER_SEC;
    GTask *task = g_task_new(NULL, NULL, probe_done, NULL);
    g_task_set_task_data(task, p, (GDestroyNotify)probe_free);
    g_task_run_in_thread(task, probe_thread);
    g_object_unref(task);
}

void lzc_invalidate_cookie(void) {
    g_clear_pointer(&app.cookie, g_free);
    service_checked_at = 0;
}

char *lzc_hub_url(void) { return g_strdup_printf("wss://lzc-mesh.%s/api/ws", app.box_domain); }
char *lzc_manage_url(void) { return g_strdup_printf("https://lzc-mesh.%s/", app.box_domain); }
