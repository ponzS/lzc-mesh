// WebSocket connection to the LZC Mesh hub and dispatch of hub messages.
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "app.h"
#include "direct.h"
#include "clipboard.h"
#include "files.h"
#include "lzc.h"
#include "net.h"
#include "wl_input.h"
#include "identity.h"
#include "input_wire.h"

static SoupSession *session;
static SoupWebsocketConnection *ws;
static bool connecting;
static guint connection_generation;
static GCancellable *pending_connect;
static guint ping_timer;
static gint64 last_motion_log;

bool net_busy(void) { return connecting; }

static bool message_allowed(JsonObject *o) {
    if (app_operational()) return true;
    const char *type = json_object_get_string_member_with_default(o, "t", "");
    const char *shared[] = {"edge", "motion", "button", "key", "wheel", "clipboard", "layout", "forget", "direct_begin", "direct_relay", "direct_route", "direct_legacy"};
    for (guint i = 0; i < G_N_ELEMENTS(shared); i++) if (!strcmp(type, shared[i])) return false;
    return true;
}

static void send_builder(JsonBuilder *b) {
    JsonNode *root = json_builder_get_root(b);
    JsonObject *object = json_node_get_object(root);
    const char *type = json_object_get_string_member_with_default(object,"t","");
    if (app.source_control_id && *app.source_control_id &&
        (!strcmp(type,"motion") || !strcmp(type,"key") || !strcmp(type,"button") || !strcmp(type,"wheel") || !strcmp(type,"release")))
        json_object_set_string_member(object,"_control_session",app.source_control_id);
    if (ws && soup_websocket_connection_get_state(ws) == SOUP_WEBSOCKET_STATE_OPEN && message_allowed(json_node_get_object(root))) {
        JsonGenerator *gen = json_generator_new(); json_generator_set_root(gen, root);
        char *text = json_generator_to_data(gen, NULL);
        if (!direct_input(text)) soup_websocket_connection_send_text(ws, text);
        g_free(text); g_object_unref(gen);
    }
    json_node_unref(root); g_object_unref(b);
}

static JsonBuilder *msg(const char *type) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "t");
    json_builder_add_string_value(b, type);
    return b;
}
static void num(JsonBuilder *b, const char *k, double v) { json_builder_set_member_name(b, k); json_builder_add_double_value(b, v); }
static void str(JsonBuilder *b, const char *k, const char *v) { json_builder_set_member_name(b, k); json_builder_add_string_value(b, v); }
static void boolean(JsonBuilder *b, const char *k, bool v) { json_builder_set_member_name(b, k); json_builder_add_boolean_value(b, v); }
static void done(JsonBuilder *b) { json_builder_end_object(b); send_builder(b); }

void net_send_hello(void) {
    if(!identity_init())return;
    char *name = g_strdup(g_get_host_name());
    JsonBuilder *b = msg(app.auth_verified?"hello":"device_hello");
    str(b,"public_key",identity_public());
    str(b, "id", app.device_id);
    str(b, "name", name);
    str(b, "platform", "linux");
    num(b, "w", app.screen_w);
    num(b, "h", app.screen_h);
    str(b, "version", LZC_MESH_VERSION);
    done(b);
    g_free(name);
    if (app.auth_verified && !app.sharing_enabled) net_send_sharing(false);
}
void net_send_edge(const char *dir, double pos) { JsonBuilder *b = msg("edge"); str(b, "dir", dir); num(b, "pos", pos); done(b); }
void net_send_motion(double dx, double dy) {
    uint64_t captured=direct_now_us();JsonBuilder *b=msg("motion");num(b,"dx",dx);num(b,"dy",dy);
    json_builder_set_member_name(b,"_capture_us");json_builder_add_int_value(b,(gint64)captured);done(b);
}
void net_send_button(int button, bool down) { done(input_wire_event("button","button",button,down)); }
void net_send_wheel(double dx, double dy, const char *source, bool stop_x, bool stop_y) {
    JsonBuilder *b = msg("wheel");
    num(b, "dx", dx); num(b, "dy", dy);
    str(b, "source", source);
    if (stop_x) boolean(b, "stop_x", true);
    if (stop_y) boolean(b, "stop_y", true);
    done(b);
}
void net_send_key(uint32_t code, bool down) { done(input_wire_event("key","code",code,down)); }
void net_send_release(void) { done(msg("release")); }
void net_send_layout(const char *id, double x, double y) { JsonBuilder *b = msg("layout"); str(b, "id", id); num(b, "x", x); num(b, "y", y); done(b); }
void net_send_forget(const char *id) { JsonBuilder *b = msg("forget"); str(b, "id", id); done(b); }
void net_send_transport_mode(const char *mode) { JsonBuilder *b = msg("settings"); str(b, "transport_mode", mode); done(b); }
void net_send_settings(bool swap) { JsonBuilder *b = msg("settings"); boolean(b, "swap_cmd_ctrl", swap); done(b); }
void net_send_clipboard(const char *text) { JsonBuilder *b = msg("clipboard"); str(b, "text", text); done(b); }
void net_send_enabled(const char *id, bool on) { JsonBuilder *b = msg("device_enabled"); str(b, "id", id); boolean(b, "enabled", on); done(b); }
void net_send_sharing(bool on) { JsonBuilder *b = msg("sharing"); boolean(b, "enabled", on); done(b); }
void net_send_clipboard_setting(bool on) { JsonBuilder *b = msg("settings"); boolean(b, "clipboard", on); done(b); }

static void update_clipboard_status(void) {
    if(app.hub_connected && !app.auth_verified)app_set_status(S_CLIPBOARD,ST_UNKNOWN,"设备未获信任，双向剪贴板已停用");
    else if (app.device_ready && !app.device_enabled) app_set_status(S_CLIPBOARD, ST_UNKNOWN, "客户端已关闭，双向剪贴板已停用");
    else if (!clipboard_available()) app_set_status(S_CLIPBOARD, ST_WARN, "当前合成器不支持 wlr-data-control，无法共享剪贴板");
    else if (!app.hub_connected) app_set_status(S_CLIPBOARD, ST_UNKNOWN, "等待连接中枢");
    else if (app.clipboard_enabled) app_set_status(S_CLIPBOARD, ST_OK, "已开启：复制的文本会同步到其他在线设备");
    else app_set_status(S_CLIPBOARD, ST_UNKNOWN, "已关闭（可在管理页面或托盘菜单中开启）");
}

static void mesh_device_free(gpointer p) {
    MeshDevice *d = p;
    g_free(d->id); g_free(d->name); g_free(d->platform); g_free(d->version);
    g_free(d);
}

static const char *device_name(const char *id) {
    for (guint i = 0; id && i < app.devices->len; i++) {
        MeshDevice *d = g_ptr_array_index(app.devices, i);
        if (g_strcmp0(d->id, id) == 0) return d->name;
    }
    return id ? id : "?";
}

static void update_control_status(void) {
    if(app.hub_connected && !app.auth_verified)app_set_status(S_CONTROL,ST_WARN,"设备未获信任，共享保持停用；请核对指纹并批准");
    else if (app.device_ready && !app.device_enabled) app_set_status(S_CONTROL, ST_WARN, "客户端已关闭，可在管理页面或本机重新开启");
    else if (app.captured) app_set_status(S_CONTROL, ST_OK, "本机正在控制 %s（Ctrl+Alt+Esc 收回）", device_name(app.active_target));
    else if (app.controlled) app_set_status(S_CONTROL, ST_OK, "%s 正在控制本机", device_name(app.active_source));
    else app_set_status(S_CONTROL, ST_UNKNOWN, "空闲：把光标移出屏幕边缘即可控制相邻设备");
}

void net_local_activity(void) {
    if (!app_operational() || !app.controlled) return;
    char *id = g_strdup(app.receive_control_id ? app.receive_control_id : app.active_control_id);
    mesh_input_gate_cancel(&app.input_gate);
    g_free(app.blocked_control_id); app.blocked_control_id = g_strdup(id);
    app.controlled = false;
    wl_input_local_takeover();
    if (id && *id) { JsonBuilder *b = msg("local_activity"); str(b,"control_id",id); done(b); }
    g_free(id);
    app_log("检测到本机实体输入，已收回控制并释放远程按键和按钮");
    update_control_status();
}

static void on_state(JsonObject *o) {
    g_ptr_array_set_free_func(app.devices, mesh_device_free);
    g_ptr_array_set_size(app.devices, 0);
    app.device_ready = false;
    app.device_trusted = false;
    JsonArray *arr = json_object_get_array_member(o, "devices");
    for (guint i = 0; arr && i < json_array_get_length(arr); i++) {
        JsonObject *d = json_array_get_object_element(arr, i);
        MeshDevice *m = g_new0(MeshDevice, 1);
        m->id = g_strdup(json_object_get_string_member_with_default(d, "id", ""));
        m->name = g_strdup(json_object_get_string_member_with_default(d, "name", ""));
        m->platform = g_strdup(json_object_get_string_member_with_default(d, "platform", ""));
        m->version = g_strdup(json_object_get_string_member_with_default(d, "version", ""));
        m->x = json_object_get_double_member_with_default(d, "x", 0);
        m->y = json_object_get_double_member_with_default(d, "y", 0);
        m->w = json_object_get_double_member_with_default(d, "w", 0);
        m->h = json_object_get_double_member_with_default(d, "h", 0);
        m->online = json_object_get_boolean_member_with_default(d, "online", false);
        m->sharing = json_object_get_boolean_member_with_default(d, "sharing", true);
        m->enabled = json_object_get_boolean_member_with_default(d, "enabled", true);
        if (!g_strcmp0(m->id, app.device_id)) {
            app.device_ready = true; app.device_enabled = m->enabled;
            app.device_control = json_object_has_member(d, "enabled");
            app.device_trusted = json_object_get_boolean_member_with_default(d,"trusted",false);
        }
        g_ptr_array_add(app.devices, m);
    }
    g_clear_pointer(&app.active_source, g_free);
    g_clear_pointer(&app.active_target, g_free);
    g_clear_pointer(&app.active_control_id, g_free);
    JsonNode *active = json_object_get_member(o, "active");
    if (active && JSON_NODE_HOLDS_OBJECT(active)) {
        JsonObject *a = json_node_get_object(active);
        app.active_source = g_strdup(json_object_get_string_member_with_default(a, "source", ""));
        app.active_target = g_strdup(json_object_get_string_member_with_default(a, "target", ""));
        app.active_control_id = g_strdup(json_object_get_string_member_with_default(a,"control_id",""));
        if (app.controlled && (!app.receive_control_id || !*app.receive_control_id)) {
            g_free(app.receive_control_id); app.receive_control_id = g_strdup(app.active_control_id);
        }
    }
    JsonNode *settings = json_object_get_member(o, "settings");
    if (settings && JSON_NODE_HOLDS_OBJECT(settings)) {
        JsonObject *so = json_node_get_object(settings);
        app.swap_cmd_ctrl = json_object_get_boolean_member_with_default(so, "swap_cmd_ctrl", true);
        g_free(app.transport_mode);
        app.transport_mode = g_strdup(json_object_get_string_member_with_default(so, "transport_mode", "auto"));
        bool clip = json_object_get_boolean_member_with_default(so, "clipboard", true);
        if (clip != app.clipboard_enabled) app_log("共享剪贴板已%s", clip ? "开启" : "关闭");
        app.clipboard_enabled = clip;
    }
    app_refresh_activity();
    update_clipboard_status();
    guint online = 0;
    for (guint i = 0; i < app.devices->len; i++) online += ((MeshDevice *)g_ptr_array_index(app.devices, i))->online;
    app_set_status(S_HUB, ST_OK, "已连接 · 用户 %s · %u 台设备在线", app.hub_user ? app.hub_user : "?", online);
    update_control_status();
    app_devices_changed();
}

void net_inject(JsonObject *o) {
    const char *t = json_object_get_string_member_with_default(o, "t", "");
    double x = json_object_get_double_member_with_default(o, "x", 0.5);
    double y = json_object_get_double_member_with_default(o, "y", 0.5);
    const char *scope = json_object_get_string_member_with_default(o,"_control_session","");
    bool begin = !strcmp(t,"direct_active") && *json_object_get_string_member_with_default(o,"source","");
    if (!(begin ? mesh_input_gate_begin(&app.input_gate,scope) : mesh_input_gate_accept(&app.input_gate,scope))) return;
    if (!*scope && app.blocked_control_id && *app.blocked_control_id && !g_strcmp0(app.blocked_control_id,app.active_control_id) &&
        (!strcmp(t,"capture") || !strcmp(t,"enter") || !strcmp(t,"move") || !strcmp(t,"key") || !strcmp(t,"button") || !strcmp(t,"wheel"))) return;
    if(!strcmp(t,"device_challenge")) {
        const char *user=json_object_get_string_member_with_default(o,"user","");
        const char *nonce=json_object_get_string_member_with_default(o,"nonce","");
        if(g_strcmp0(user,app.hub_user)){net_disconnect();return;}
        char *proof=identity_proof(user,nonce);
        if(proof && *proof){JsonBuilder *b=msg("device_proof");str(b,"signature",proof);done(b);}else net_disconnect();
        g_free(proof);return;
    }
    if(!strcmp(t,"device_auth")) {
        const char *state=json_object_get_string_member_with_default(o,"status","");
        const char *fp=json_object_get_string_member_with_default(o,"fingerprint","");
        const char *token=json_object_get_string_member_with_default(o,"token","");
        app.auth_verified=!strcmp(state,"approved") && !strcmp(fp,identity_fingerprint()) && strlen(token)==44 && strspn(token,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")==44;
        g_clear_pointer(&app.file_token,g_free);if(app.auth_verified)app.file_token=g_strdup(token);
        app_set_status(S_TRUST,app.auth_verified?ST_OK:ST_WARN,"%s · 指纹 %s",app.auth_verified?"已批准":!strcmp(state,"key_mismatch")?"凭证不匹配，拒绝绑定":!strcmp(state,"revoked")?"信任已撤销":"等待管理页核对并批准",identity_fingerprint());
        app_refresh_activity();update_control_status();update_clipboard_status();
        if(app.auth_verified && !app.sharing_enabled)net_send_sharing(false);
        return;
    }
    if (!app_operational() && (!strcmp(t,"capture") || !strcmp(t,"enter") || !strcmp(t,"move") || !strcmp(t,"button") || !strcmp(t,"key") || !strcmp(t,"wheel") || !strcmp(t,"clipboard") || !strcmp(t,"files"))) return;
    if (!strcmp(t, "direct_active")) {
        g_free(app.active_source);g_free(app.active_target);
        app.active_source=g_strdup(json_object_get_string_member_with_default(o,"source",""));
        app.active_target=g_strdup(json_object_get_string_member_with_default(o,"target",""));
        update_control_status();
    } else if (!strcmp(t, "state")) on_state(o);
    else if (!strcmp(t, "welcome")) {
        if(json_object_get_int_member_with_default(o,"auth_version",0)!=1)app_set_status(S_TRUST,ST_ERROR,"需要支持设备凭证的新版中枢；共享保持停用");
        const char *user = json_object_get_string_member_with_default(o, "user", "");
        g_free(app.hub_user);
        app.hub_user = g_strdup(user);
        if (*json_object_get_string_member_with_default(o, "id", "")) app_log("已加入 %s 的设备组", user);
    } else if (!strcmp(t, "edges")) {
        wl_input_set_edges(json_object_get_boolean_member_with_default(o, "left", false),
                           json_object_get_boolean_member_with_default(o, "right", false),
                           json_object_get_boolean_member_with_default(o, "up", false),
                           json_object_get_boolean_member_with_default(o, "down", false));
    } else if (!strcmp(t, "capture")) {
        g_free(app.source_control_id); app.source_control_id = g_strdup(scope);
        if (app.controlled) wl_input_leave();
        app.controlled = false;
        app.captured = true;
        app_log("开始控制其他设备");
        wl_input_capture(true);
        update_control_status();
    } else if (!strcmp(t, "release")) {
        g_clear_pointer(&app.source_control_id,g_free);
        app.captured = false;
        wl_input_capture(false);
        if (json_object_get_boolean_member_with_default(o, "warp", false)) wl_input_warp(x, y);
        app_log("控制已收回本机");
        update_control_status();
    } else if (!strcmp(t, "enter")) {
        g_clear_pointer(&app.source_control_id,g_free);
        if (app.captured) wl_input_capture(false);
        app.captured = false;
        g_free(app.receive_control_id); app.receive_control_id = g_strdup(*scope ? scope : app.active_control_id);
        app.controlled = true;
        app_log("被远程控制：光标进入本机 (%.2f, %.2f)", x, y);
        wl_input_enter(x, y);
        update_control_status();
    } else if (!strcmp(t, "leave")) {
        app.controlled = false;
        wl_input_leave();
        app_log("远程控制结束");
        update_control_status();
    } else if (!strcmp(t, "move") && app.controlled) {
        wl_input_move(x, y);
        if (json_object_has_member(o,"_motion_trace")) direct_applied(json_object_get_object_member(o,"_motion_trace"));
        gint64 now = g_get_monotonic_time();
        if (now - last_motion_log > 2 * G_USEC_PER_SEC) {
            last_motion_log = now;
            app_log("收到远程光标 (%.3f, %.3f)", x, y);
        }
    } else if (!strcmp(t, "button") && app.controlled) {
        wl_input_button((int)json_object_get_int_member_with_default(o, "button", 1), json_object_get_boolean_member_with_default(o, "down", false));
    } else if (!strcmp(t, "wheel") && app.controlled) {
        wl_input_wheel(json_object_get_double_member_with_default(o, "dx", 0),
                       json_object_get_double_member_with_default(o, "dy", 0),
                       json_object_get_string_member_with_default(o, "source", NULL),
                       json_object_get_boolean_member_with_default(o, "stop_x", false),
                       json_object_get_boolean_member_with_default(o, "stop_y", false));
    } else if (!strcmp(t, "key") && app.controlled) {
        uint32_t code = (uint32_t)json_object_get_int_member_with_default(o, "code", 0);
        bool down = json_object_get_boolean_member_with_default(o, "down", false);
        wl_input_key(code, down);
    } else if (!strcmp(t, "files")) {
        files_update(json_object_get_array_member(o, "files"));
    } else if (!strcmp(t, "clipboard")) {
        if (app.clipboard_enabled)
            clipboard_set(json_object_get_string_member_with_default(o, "text", ""), json_object_get_string_member_with_default(o, "from", ""));
    } else if (!strcmp(t, "error")) {
        app_log("服务端错误：%s", json_object_get_string_member_with_default(o, "message", ""));
    }
}

static void on_message(SoupWebsocketConnection *c, gint type, GBytes *data, gpointer u) {
    if (c!=ws || soup_websocket_connection_get_state(c)!=SOUP_WEBSOCKET_STATE_OPEN || type != SOUP_WEBSOCKET_DATA_TEXT) return;
    gsize len;
    const char *text = g_bytes_get_data(data, &len);
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, text, (gssize)len, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        g_object_unref(p);
        return;
    }
    net_inject(json_node_get_object(json_parser_get_root(p)));
    char *copy=g_strndup(text,len);
    direct_hub(copy);
    g_free(copy);
    g_object_unref(p);
}

static void reset_roles(void) {
    g_clear_pointer(&app.source_control_id,g_free);
    if (app.captured) wl_input_capture(false);
    if (app.controlled) wl_input_leave();
    app.captured = app.controlled = false;
    wl_input_set_edges(false, false, false, false);
    update_control_status();
    update_clipboard_status();
}

static void on_closed(SoupWebsocketConnection *c, gpointer u) {
    if(c!=ws)return;
    app.auth_verified=app.device_trusted=false;g_clear_pointer(&app.file_token,g_free);
    app_log("与设备接力中枢的连接已断开（code %u）", soup_websocket_connection_get_close_code(c));
    if (ws == c) g_clear_object(&ws);
    app.hub_connected = false; app.device_ready = false;
    app_refresh_activity();
    if (ping_timer) g_source_remove(ping_timer), ping_timer = 0;
    app_set_status(S_HUB, ST_WARN, "连接已断开，稍后自动重连");
    direct_disconnect();
    reset_roles();
}

static gboolean ping(gpointer u) {
    done(msg("ping"));
    return G_SOURCE_CONTINUE;
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer u) {
    GError *err = NULL;
    SoupWebsocketConnection *conn = soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
    if(GPOINTER_TO_UINT(u)!=connection_generation) {
        if(conn){soup_websocket_connection_close(conn,SOUP_WEBSOCKET_CLOSE_NORMAL,NULL);g_object_unref(conn);}
        g_clear_error(&err);return;
    }
    connecting = false;g_clear_object(&pending_connect);
    if (!conn) {
        app_set_status(S_HUB, ST_ERROR, "连接失败：%s", err->message);
        app_log("连接设备接力中枢失败：%s", err->message);
        if (g_error_matches(err, SOUP_WEBSOCKET_ERROR, SOUP_WEBSOCKET_ERROR_NOT_WEBSOCKET)) lzc_invalidate_cookie();
        g_error_free(err);
        return;
    }
    ws = conn;
    soup_websocket_connection_set_keepalive_interval(ws, 15);
    soup_websocket_connection_set_max_incoming_payload_size(ws, 1 << 20);
    g_signal_connect(ws, "message", G_CALLBACK(on_message), NULL);
    g_signal_connect(ws, "closed", G_CALLBACK(on_closed), NULL);
    app.hub_connected = true;
    app.device_ready=app.device_trusted=app.auth_verified=false;g_clear_pointer(&app.file_token,g_free);
    app_refresh_activity();
    app_set_status(S_HUB, ST_OK, "已连接");
    app_log("已连接设备接力中枢");
    net_send_hello();
    direct_hub("{\"t\":\"hub_connected\"}");
    ping_timer = g_timeout_add_seconds(20, ping, NULL);
}

void net_connect(void) {
    if (connecting || ws || !app.cookie || !app.box_domain || app.screen_w <= 0) return;
    if (!session) session = soup_session_new_with_options("user-agent", "lzc-mesh-linux/" LZC_MESH_VERSION, NULL);
    char *url = lzc_hub_url();
    SoupMessage *m = soup_message_new("GET", url);
    soup_message_add_flags(m, SOUP_MESSAGE_NO_REDIRECT);
    char *cookie = g_strdup_printf("Lzc-Auth-Token=%s", app.cookie);
    soup_message_headers_append(soup_message_get_request_headers(m), "Cookie", cookie);
    connecting = true;
    app_set_status(S_HUB, ST_WARN, "正在连接 %s", url);
    g_clear_object(&pending_connect);pending_connect=g_cancellable_new();
    guint generation=++connection_generation;
    soup_session_websocket_connect_async(session, m, NULL, NULL, G_PRIORITY_DEFAULT, pending_connect, on_connected, GUINT_TO_POINTER(generation));
    g_object_unref(m);
    g_free(cookie);
    g_free(url);
}

void net_disconnect(void) {
    ++connection_generation;
    if(pending_connect){g_cancellable_cancel(pending_connect);g_clear_object(&pending_connect);connecting=false;}
    app.auth_verified=app.device_trusted=false;g_clear_pointer(&app.file_token,g_free);app_refresh_activity();
    if (ws) soup_websocket_connection_close(ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
}

void net_send_raw(const char *text) {
    JsonParser *parser = json_parser_new();
    bool allowed = json_parser_load_from_data(parser, text, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)) && message_allowed(json_node_get_object(json_parser_get_root(parser)));
    if (allowed && ws && soup_websocket_connection_get_state(ws)==SOUP_WEBSOCKET_STATE_OPEN) soup_websocket_connection_send_text(ws,text);
    g_object_unref(parser);
}
