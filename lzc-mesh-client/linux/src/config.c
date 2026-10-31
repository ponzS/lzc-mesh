// Persistent client settings in ~/.config/lzc-mesh/config.json.
#include <json-glib/json-glib.h>

#include "app.h"
#include "config.h"

static char *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "lzc-mesh", "config.json", NULL);
}

void config_save(void) {
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "device_id");
    json_builder_add_string_value(b, app.device_id);
    json_builder_set_member_name(b, "box");
    json_builder_add_string_value(b, app.box_override ? app.box_override : "");
    json_builder_set_member_name(b, "sharing");
    json_builder_add_boolean_value(b, app.sharing_enabled);
    json_builder_end_object(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, json_builder_get_root(b));
    json_generator_to_file(gen, path, NULL);
    g_object_unref(gen);
    g_object_unref(b);
    g_free(dir);
    g_free(path);
}

void config_load(void) {
    char *path = config_path();
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, path, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        app.device_id = g_strdup(json_object_get_string_member_with_default(o, "device_id", ""));
        app.box_override = g_strdup(json_object_get_string_member_with_default(o, "box", ""));
        app.sharing_enabled = json_object_get_boolean_member_with_default(o, "sharing", true);
    }
    g_object_unref(p);
    g_free(path);
    if (!app.device_id || !*app.device_id) {
        g_free(app.device_id);
        char *uuid = g_uuid_string_random();
        app.device_id = g_strdup_printf("linux-%s", uuid);
        g_free(uuid);
        config_save();
    }
    if (!app.box_override) app.box_override = g_strdup("");
}
