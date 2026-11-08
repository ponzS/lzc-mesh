// Exercise production configuration discovery and HTTP helpers with fixtures.
// Unused UI/supervisor sections are discarded by --gc-sections at link time.
#include "../lzc-mesh-client/linux/src/lzc.c"
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static void wrote_headers(SoupMessage *message, guint *count) { ++*count; }

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 4);
    gboolean expect = !strcmp(argv[3], "ok");
    char *error = NULL;
    GError *io_error = NULL;
    gboolean ok = FALSE;
    if (!strcmp(argv[1], "foreign")) {
        SoupSession *transport = lzc_http_session_new(TRUE);
        SoupMessage *message = soup_message_new("POST", argv[2]);
        guint written = 0;
        g_signal_connect(message, "wrote-headers", G_CALLBACK(wrote_headers), &written);
        GBytes *body = lzc_http_send_authenticated(transport, message, NULL, "lzc-shellapi-cred",
                                                  "fixture-credential", &io_error);
        g_assert_null(body);
        g_assert_error(io_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_assert_cmpuint(written, ==, 0);
        g_object_unref(message); g_object_unref(transport);
        ok = TRUE;
    } else if (!strcmp(argv[1], "file")) {
        char *text = lzc_read_private_file(argv[2], 1024, &io_error);
        ok = text != NULL;
        g_free(text);
    } else if (!strcmp(argv[1], "peer")) {
        LzcLocalPeer *peer = lzc_local_peer_new(atoi(argv[2]), "hclient-cli", &io_error);
        ok = peer != NULL;
        lzc_local_peer_free(peer);
    } else if (!strcmp(argv[1], "discover")) {
        LocalApi api = {0};
        ok = read_cli_api(&api, &error);
        if (ok) {
            char *token = lzc_cli_auth_token(local_http(), api.peer, api.base, api.token, &error);
            ok = token && !strcmp(token, "fixture-cookie");
            g_free(token);
        }
        g_free(api.token); g_free(api.base); lzc_local_peer_free(api.peer);
    } else if (!strcmp(argv[1], "rpc") || !strcmp(argv[1], "peer-exit")) {
        LzcLocalPeer *peer = NULL;
        if (!strcmp(argv[1], "peer-exit")) {
            pid_t pid = atoi(g_getenv("LZC_FIXTURE_PID"));
            peer = lzc_local_peer_new(pid, "hclient-cli", &io_error);
            g_assert_no_error(io_error);
            g_assert_nonnull(peer);
            g_assert_cmpint(kill(pid, SIGTERM), ==, 0); // Only the runner's disposable fixture child.
            g_usleep(100000);
        }
        // Deliberately pass a resolver configured by the caller. Production must
        // explicitly disable it before sending a local secret.
        SoupSession *transport = lzc_http_session_new(FALSE);
        const char *proxy = g_getenv("LZC_FIXTURE_PROXY");
        if (proxy) {
            GProxyResolver *resolver = g_simple_proxy_resolver_new(proxy, NULL);
            soup_session_set_proxy_resolver(transport, resolver);
            g_object_unref(resolver);
        }
        char *token = lzc_cli_auth_token(transport, peer, argv[2], "fixture-credential", &error);
        ok = token && !strcmp(token, "fixture-cookie");
        g_free(token); lzc_local_peer_free(peer); g_object_unref(transport);
    } else {
        const char *ca = g_getenv("LZC_FIXTURE_CA");
        if (ca) {
            GTlsDatabase *database = g_tls_file_database_new(ca, &io_error);
            g_assert_no_error(io_error);
            soup_session_set_tls_database(http(), database);
            g_object_unref(database);
        }
        const char *header = !strcmp(argv[1], "desktop") ? "LZC_MISC_AUTH_TOKEN" : "Cookie";
        JsonNode *node = get_json(argv[2], header, "fixture-credential", &error);
        ok = node != NULL;
        g_clear_pointer(&node, json_node_unref);
    }
    g_assert_cmpint(ok, ==, expect);
    if (!ok) g_assert_true(error != NULL || io_error != NULL);
    g_free(error); g_clear_error(&io_error);
    return 0;
}
