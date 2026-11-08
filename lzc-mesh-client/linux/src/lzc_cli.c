// Only two read-only ShellCore RPCs are needed. Decode their protobuf fields
// without adding a gRPC runtime; unknown fields are skipped for forward compatibility.
// Verified against hclient-cli 1.5.1 and its embedded ShellCore protobuf descriptors.
#include "lzc_cli.h"
#include <stdint.h>
#include <string.h>

typedef struct { const guint8 *data; gsize size, pos; } Wire;
typedef struct { guint number, type; guint64 integer; Wire bytes; } Field;

static gboolean fail(char **error, const char *text) {
    if (error && !*error) *error = g_strdup(text);
    return FALSE;
}

static gboolean varint(Wire *wire, guint64 *value) {
    *value = 0;
    for (guint shift = 0; shift < 70 && wire->pos < wire->size; shift += 7) {
        guint8 byte = wire->data[wire->pos++];
        if (shift == 63 && byte > 1) return FALSE;
        *value |= (guint64)(byte & 127) << shift;
        if (!(byte & 128)) return TRUE;
    }
    return FALSE;
}

static gboolean field_next(Wire *wire, Field *field) {
    guint64 tag, size;
    memset(field, 0, sizeof *field);
    if (!varint(wire, &tag) || !(tag >> 3) || (tag >> 3) > 0x1fffffff) return FALSE;
    field->number = tag >> 3;
    field->type = tag & 7;
    switch (field->type) {
    case 0: return varint(wire, &field->integer);
    case 1: size = 8; break;
    case 2: if (!varint(wire, &size)) return FALSE; break;
    case 5: size = 4; break;
    default: return FALSE;
    }
    if (size > wire->size - wire->pos) return FALSE;
    field->bytes = (Wire){wire->data + wire->pos, (gsize)size, 0};
    wire->pos += size;
    return TRUE;
}

static char *field_text(const Field *field) {
    if (field->type != 2 || memchr(field->bytes.data, 0, field->bytes.size) ||
        !g_utf8_validate((const char *)field->bytes.data, field->bytes.size, NULL)) return NULL;
    return g_strndup((const char *)field->bytes.data, field->bytes.size);
}

static gboolean status_ok(const char *status) { return status && !strcmp(status, "0"); }

static GBytes *rpc(SoupSession *session, LzcLocalPeer *peer, const char *base, const char *credential,
                   const char *method, char **error) {
    char *url = g_strdup_printf("%s/space.heiyu.hportal.shell.ShellCore/%s", base, method);
    SoupMessage *message = soup_message_new("POST", url);
    g_free(url);
    if (!message) { fail(error, "hclient-cli 本地接口地址无效"); return NULL; }
    soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    soup_message_headers_append(headers, "X-Grpc-Web", "1");
    // Both RPCs take google.protobuf.Empty: one uncompressed, empty message.
    static const guint8 empty[] = {0, 0, 0, 0, 0};
    GBytes *request = g_bytes_new_static(empty, sizeof empty);
    soup_message_set_request_body_from_bytes(message, "application/grpc-web+proto", request);
    g_bytes_unref(request);
    GError *io_error = NULL;
    GBytes *body = lzc_http_send_authenticated(session, message, peer, "lzc-shellapi-cred", credential, &io_error);
    GBytes *result = NULL;
    if (!body) {
        fail(error, io_error ? io_error->message : "hclient-cli 本地接口不可达");
        goto out;
    }
    if (soup_message_get_status(message) != 200) {
        fail(error, "hclient-cli 本地接口返回 HTTP 错误");
        goto out;
    }
    headers = soup_message_get_response_headers(message);
    const char *content_type = soup_message_headers_get_content_type(headers, NULL);
    if (g_strcmp0(content_type, "application/grpc-web+proto")) {
        fail(error, "hclient-cli 本地接口不是 gRPC-Web");
        goto out;
    }
    const char *status = soup_message_headers_get_one(headers, "grpc-status");
    gboolean have_status = status_ok(status);
    if (status && !have_status) {
        fail(error, "hclient-cli 拒绝请求，请检查本机凭证或重启命令行客户端");
        goto out;
    }
    gsize size;
    const guint8 *data = g_bytes_get_data(body, &size);
    if (size > 1024 * 1024) goto malformed;
    for (gsize pos = 0; pos < size;) {
        if (size - pos < 5) goto malformed;
        guint8 flags = data[pos];
        guint32 length = ((guint32)data[pos+1] << 24) | ((guint32)data[pos+2] << 16) |
                         ((guint32)data[pos+3] << 8) | data[pos+4];
        pos += 5;
        if (length > size - pos) goto malformed;
        if (flags == 0) {
            if (result) goto malformed;
            result = g_bytes_new(data + pos, length);
        } else if (flags == 128) {
            if (memchr(data + pos, 0, length) || pos + length != size) goto malformed;
            char *trailers = g_strndup((const char *)data + pos, length);
            char **lines = g_strsplit(trailers, "\r\n", -1);
            for (guint i = 0; lines[i]; i++) {
                char *colon = strchr(lines[i], ':');
                if (!colon) continue;
                *colon = 0;
                if (!g_ascii_strcasecmp(lines[i], "grpc-status")) {
                    if (have_status || !status_ok(g_strstrip(colon + 1))) {
                        g_strfreev(lines); g_free(trailers);
                        fail(error, "hclient-cli RPC 失败，请确认已登录微服");
                        g_clear_pointer(&result, g_bytes_unref);
                        goto out;
                    }
                    have_status = TRUE;
                }
            }
            g_strfreev(lines);
            g_free(trailers);
        } else goto malformed; // Compressed messages are not requested or supported.
        pos += length;
    }
    if (!result || !have_status) goto malformed;
    goto out;
malformed:
    fail(error, "hclient-cli 返回了无效的 gRPC-Web 数据");
    g_clear_pointer(&result, g_bytes_unref);
out:
    g_clear_error(&io_error);
    g_clear_pointer(&body, g_bytes_unref);
    g_object_unref(message);
    return result;
}

static JsonNode *box_decode(Wire wire, gboolean *is_default) {
    // BoxInfo: 2 box_name, 4 box_domain, 6 status, 9 login_user,
    // 11 is_default_box. Field 10 is is_admin_login, not the default flag.
    char *name = NULL, *domain = NULL, *user = NULL;
    gint64 status = -1;
    *is_default = FALSE;
    while (wire.pos < wire.size) {
        Field field;
        if (!field_next(&wire, &field)) goto invalid;
        char **text = field.number == 2 ? &name : field.number == 4 ? &domain :
                      field.number == 9 ? &user : NULL;
        if (text) {
            char *value = field_text(&field);
            if (!value) goto invalid;
            g_free(*text); *text = value;
        } else if (field.number == 6 || field.number == 11) {
            if (field.type != 0) goto invalid;
            if (field.number == 6) {
                if (field.integer > G_MAXINT) goto invalid;
                status = field.integer;
            } else *is_default = field.integer != 0;
        }
    }
    if (!name || !*name || !domain || !*domain) goto invalid;
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "boxName", name);
    json_object_set_string_member(object, "boxDomain", domain);
    json_object_set_string_member(object, "loginUser", user ? user : "");
    json_object_set_int_member(object, "status", status);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    g_free(name); g_free(domain); g_free(user);
    return node;
invalid:
    g_free(name); g_free(domain); g_free(user);
    return NULL;
}

JsonNode *lzc_cli_default_box(SoupSession *session, LzcLocalPeer *peer, const char *base,
                              const char *credential, const char *box_override,
                              char **error) {
    GBytes *bytes = rpc(session, peer, base, credential, "QueryBoxList", error);
    if (!bytes) return NULL;
    gsize size;
    const guint8 *data = g_bytes_get_data(bytes, &size);
    Wire wire = {data, size, 0};
    JsonNode *selected = NULL;
    gboolean explicit_box = box_override && *box_override;
    while (wire.pos < wire.size) {
        Field field;
        if (!field_next(&wire, &field)) goto invalid;
        if (field.number != 1) continue;
        if (field.type != 2) goto invalid;
        gboolean is_default;
        JsonNode *box = box_decode(field.bytes, &is_default);
        if (!box) goto invalid;
        const char *name = json_object_get_string_member(json_node_get_object(box), "boxName");
        gboolean match = explicit_box ? !strcmp(name, box_override) : is_default;
        if (match) {
            if (selected) { json_node_unref(box); goto invalid; }
            selected = box;
        } else json_node_unref(box);
    }
    if (!selected) fail(error, explicit_box ? "hclient-cli 中没有指定的微服，请先登录该微服" :
                                            "hclient-cli 没有默认微服，请先登录并设置默认微服");
    g_bytes_unref(bytes);
    return selected;
invalid:
    fail(error, "hclient-cli 返回了无效的微服列表");
    g_clear_pointer(&selected, json_node_unref);
    g_bytes_unref(bytes);
    return NULL;
}

char *lzc_cli_auth_token(SoupSession *session, LzcLocalPeer *peer, const char *base,
                         const char *credential, char **error) {
    GBytes *bytes = rpc(session, peer, base, credential, "QueryLzcAuthToken", error);
    if (!bytes) return NULL;
    gsize size;
    const guint8 *data = g_bytes_get_data(bytes, &size);
    Wire wire = {data, size, 0};
    char *token = NULL;
    while (wire.pos < wire.size) {
        Field field;
        if (!field_next(&wire, &field)) goto invalid;
        if (field.number != 1) continue;
        g_clear_pointer(&token, g_free);
        token = field_text(&field);
        if (!token || !*token || strlen(token) > 4096 || strpbrk(token, "\r\n;")) goto invalid;
    }
    if (!token) goto invalid;
    g_bytes_unref(bytes);
    return token;
invalid:
    fail(error, "hclient-cli 没有返回有效的登录凭证");
    g_free(token); g_bytes_unref(bytes);
    return NULL;
}
