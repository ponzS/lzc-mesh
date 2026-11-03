#pragma once
#include <stdbool.h>
#include <json-glib/json-glib.h>
// Keep key/button identities as JSON integers, not double-valued motion units.
static inline JsonBuilder *input_wire_event(const char *type, const char *field, gint64 code, bool down) {
    JsonBuilder *b=json_builder_new();json_builder_begin_object(b);
    json_builder_set_member_name(b,"t");json_builder_add_string_value(b,type);
    json_builder_set_member_name(b,field);json_builder_add_int_value(b,code);
    json_builder_set_member_name(b,"down");json_builder_add_boolean_value(b,down);
    return b;
}
