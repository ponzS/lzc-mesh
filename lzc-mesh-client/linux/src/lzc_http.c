#define _GNU_SOURCE
#include "lzc_http.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/stat.h>
#include <unistd.h>

struct LzcLocalPeer { int pidfd; };

static void denied(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, message);
}

char *lzc_read_private_file(const char *path, gsize limit, GError **error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 077) || st.st_nlink != 1 ||
        st.st_size <= 0 || (guint64)st.st_size > limit) {
        if (fd >= 0) close(fd);
        denied(error, "本机凭证或运行配置必须是当前用户所有的私有普通文件（0600），且不能是符号链接");
        return NULL;
    }
    char *text = g_malloc(limit + 1);
    gsize size = 0;
    while (size <= limit) {
        ssize_t count = read(fd, text + size, limit + 1 - size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (count == 0 && (!size || size != (gsize)st.st_size))) goto invalid;
        if (!count) break;
        size += count;
        if (size > limit) goto invalid;
    }
    close(fd);
    if (memchr(text, 0, size)) {
        g_free(text); denied(error, "本机凭证或运行配置包含无效数据"); return NULL;
    }
    text[size] = 0;
    return text;
invalid:
    close(fd); g_free(text);
    denied(error, "本机凭证或运行配置在读取期间变化或超过大小限制");
    return NULL;
}

static gboolean peer_alive(LzcLocalPeer *peer) {
    if (!peer) return TRUE; // Desktop API has no CLI PID; the socket UID is still checked.
    struct pollfd descriptor = {.fd = peer->pidfd, .events = POLLIN};
    return poll(&descriptor, 1, 0) == 0;
}

LzcLocalPeer *lzc_local_peer_new(pid_t pid, const char *name, GError **error) {
    if (pid <= 1) { denied(error, "hclient-cli 的 PID 无效"); return NULL; }
    int fd = pidfd_open(pid, 0);
    if (fd < 0) { denied(error, "无法固定 hclient-cli 进程身份，请确认命令行客户端正在运行"); return NULL; }
    char *path = g_strdup_printf("/proc/%d/status", pid);
    char *status = NULL, *comm = NULL;
    gboolean ok = g_file_get_contents(path, &status, NULL, NULL);
    g_free(path);
    guint real = 0, effective = 0, saved = 0, fs = 0;
    char *uids = status ? strstr(status, "\nUid:") : NULL;
    ok = ok && uids && sscanf(uids, "\nUid:\t%u\t%u\t%u\t%u", &real, &effective, &saved, &fs) == 4 &&
         real == geteuid() && effective == geteuid() && saved == geteuid() && fs == geteuid();
    path = g_strdup_printf("/proc/%d/comm", pid);
    ok = ok && g_file_get_contents(path, &comm, NULL, NULL) && !strcmp(g_strchomp(comm), name);
    g_free(path); g_free(status); g_free(comm);
    LzcLocalPeer *peer = g_new0(LzcLocalPeer, 1);
    peer->pidfd = fd;
    if (!ok || !peer_alive(peer)) {
        lzc_local_peer_free(peer);
        denied(error, "hclient-cli 进程身份不匹配、已经退出或不属于当前用户");
        return NULL;
    }
    return peer;
}

void lzc_local_peer_free(LzcLocalPeer *peer) {
    if (peer) { close(peer->pidfd); g_free(peer); }
}

SoupSession *lzc_http_session_new(gboolean local) {
    SoupSession *session = soup_session_new_with_options("timeout", 8, NULL);
    soup_session_remove_feature_by_type(session, SOUP_TYPE_COOKIE_JAR);
    if (local) soup_session_set_proxy_resolver(session, NULL);
    return session;
}

static gboolean loopback(GUri *uri) {
    GInetAddress *ip = g_inet_address_new_from_string(g_uri_get_host(uri));
    gboolean ok = ip && g_inet_address_get_is_loopback(ip);
    g_clear_object(&ip);
    return ok;
}

// /proc/net/tcp{,6} prints address words in native byte order, then a hex port.
static char *tcp_address(GInetSocketAddress *address) {
    GInetAddress *ip = g_inet_socket_address_get_address(address);
    const guint8 *bytes = g_inet_address_to_bytes(ip);
    GString *text = g_string_new(NULL);
    for (gsize offset = 0; offset < g_inet_address_get_native_size(ip); offset += 4) {
        guint32 word;
        memcpy(&word, bytes + offset, 4);
        g_string_append_printf(text, "%08X", word);
    }
    g_string_append_printf(text, ":%04X", g_inet_socket_address_get_port(address));
    return g_string_free(text, FALSE);
}

// Check the SERVER half of this exact established TCP connection, rather than
// a listener snapshot before connect. Its identity cannot change on port reuse.
static int connection_uid(GSocketConnection *connection) {
    GSocketAddress *local = g_socket_connection_get_local_address(connection, NULL);
    GSocketAddress *remote = g_socket_connection_get_remote_address(connection, NULL);
    int result = -1;
    if (!G_IS_INET_SOCKET_ADDRESS(local) || !G_IS_INET_SOCKET_ADDRESS(remote)) goto out;
    GInetAddress *server_ip = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));
    if (!g_inet_address_get_is_loopback(server_ip)) goto out;
    char *server = tcp_address(G_INET_SOCKET_ADDRESS(remote));
    char *client = tcp_address(G_INET_SOCKET_ADDRESS(local));
    const char *path = g_inet_address_get_family(server_ip) == G_SOCKET_FAMILY_IPV6 ?
                       "/proc/self/net/tcp6" : "/proc/self/net/tcp";
    FILE *table = fopen(path, "re");
    if (table) {
        char *line = NULL;
        size_t capacity = 0;
        while (getline(&line, &capacity, table) > 0) {
            char a[80], b[80];
            guint state, uid;
            unsigned long long inode;
            if (sscanf(line, "%*u: %79s %79s %x %*s %*s %*s %u %*s %llu",
                       a, b, &state, &uid, &inode) != 5) continue;
            if (state == 1 && inode && !strcmp(a, server) && !strcmp(b, client)) {
                result = uid == geteuid() ? 1 : 0;
                break;
            }
        }
        g_free(line);
        fclose(table);
    }
    g_free(server); g_free(client);
out:
    g_clear_object(&local); g_clear_object(&remote);
    return result;
}

typedef struct {
    LzcLocalPeer *peer;
    GCancellable *cancel;
    const char *header, *credential;
    gboolean verified;
} Gate;

static void connected(SoupMessage *message, GSocketClientEvent event, GIOStream *stream, Gate *gate) {
    if (event == G_SOCKET_CLIENT_CONNECTING) gate->verified = FALSE;
    if (event != G_SOCKET_CLIENT_COMPLETE) return;
    if (G_IS_SOCKET_CONNECTION(stream)) {
        // The peer's accepted socket may appear just after connect completes.
        for (guint attempt = 0; attempt < 40 && peer_alive(gate->peer); attempt++) {
            int owned = connection_uid(G_SOCKET_CONNECTION(stream));
            if (owned >= 0) { gate->verified = owned == 1; break; }
            g_usleep(5000);
        }
    }
    if (!gate->verified) g_cancellable_cancel(gate->cancel);
}

static void starting(SoupMessage *message, Gate *gate) {
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    soup_message_headers_remove(headers, gate->header);
    if (gate->verified && peer_alive(gate->peer) && !g_cancellable_is_cancelled(gate->cancel))
        soup_message_headers_replace(headers, gate->header, gate->credential);
    else g_cancellable_cancel(gate->cancel);
}

GBytes *lzc_http_send_authenticated(SoupSession *session, SoupMessage *message,
                                    LzcLocalPeer *peer, const char *header,
                                    const char *credential, GError **error) {
    GUri *uri = soup_message_get_uri(message);
    gboolean local = !g_strcmp0(g_uri_get_scheme(uri), "http") && loopback(uri);
    if (!local && (peer || !g_strcmp0(header, "lzc-shellapi-cred") ||
                   !g_strcmp0(header, "LZC_MISC_AUTH_TOKEN") ||
                   g_strcmp0(g_uri_get_scheme(uri), "https"))) {
        denied(error, "拒绝向非本机明文接口或不安全的认证地址发送凭证");
        return NULL;
    }
    soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    if (!local) {
        soup_message_headers_replace(soup_message_get_request_headers(message), header, credential);
        return soup_session_send_and_read(session, message, NULL, error);
    }
    // Even callers with a configured resolver cannot proxy local credentials.
    soup_session_set_proxy_resolver(session, NULL);
    soup_message_add_flags(message, SOUP_MESSAGE_NEW_CONNECTION);
    soup_message_set_force_http1(message, TRUE);
    Gate gate = {.peer = peer, .cancel = g_cancellable_new(), .header = header, .credential = credential};
    soup_message_headers_remove(soup_message_get_request_headers(message), header);
    gulong network = g_signal_connect(message, "network-event", G_CALLBACK(connected), &gate);
    gulong start = g_signal_connect(message, "starting", G_CALLBACK(starting), &gate);
    GBytes *body = soup_session_send_and_read(session, message, gate.cancel, error);
    g_signal_handler_disconnect(message, network);
    g_signal_handler_disconnect(message, start);
    if (!gate.verified || !peer_alive(peer)) {
        g_clear_pointer(&body, g_bytes_unref);
        g_clear_error(error);
        denied(error, "无法确认本地连接属于当前用户，或 hclient-cli 已退出；认证已停止");
    }
    g_object_unref(gate.cancel);
    return body;
}
