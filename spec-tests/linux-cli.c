// Exercise the production hclient-cli adapter against a loopback fixture.
#include "../lzc-mesh-client/linux/src/lzc_cli.h"
#include <string.h>

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 5);
    SoupSession *session = soup_session_new_with_options("timeout", 2, NULL);
    char *error = NULL;
    gboolean token_mode = !strcmp(argv[2], "token");
    JsonNode *box = token_mode ? NULL :
        lzc_cli_default_box(session, NULL, argv[1], "fixture-credential", argv[3], &error);
    char *token = token_mode ? lzc_cli_auth_token(session, NULL, argv[1], "fixture-credential", &error) : NULL;
    if (!strcmp(argv[4], "error")) {
        g_assert_null(box);
        g_assert_null(token);
        g_assert_nonnull(error);
    } else if (token_mode) {
        g_assert_null(error);
        g_assert_cmpstr(token, ==, "fixture-cookie");
    } else {
        g_assert_null(error);
        g_assert_nonnull(box);
        JsonObject *object = json_node_get_object(box);
        g_assert_cmpstr(json_object_get_string_member(object, "boxName"), ==, argv[4]);
        g_assert_cmpstr(json_object_get_string_member(object, "loginUser"), ==, "fixture-user");
        g_assert_cmpint(json_object_get_int_member(object, "status"), ==, 2);
    }
    g_clear_pointer(&box, json_node_unref);
    g_free(token); g_free(error);
    g_object_unref(session);
    return 0;
}
