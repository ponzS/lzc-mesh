#include "../lzc-mesh-client/linux/src/input_wire.h"
#include <assert.h>
#include <stdio.h>
static void check(const char *type,const char *field,gint64 code,bool down) {
    JsonBuilder *b=input_wire_event(type,field,code,down);json_builder_end_object(b);
    JsonNode *root=json_builder_get_root(b);JsonGenerator *gen=json_generator_new();json_generator_set_root(gen,root);
    char *text=json_generator_to_data(gen,NULL);
    JsonParser *p=json_parser_new();assert(json_parser_load_from_data(p,text,-1,NULL));
    JsonObject *o=json_node_get_object(json_parser_get_root(p));JsonNode *n=json_object_get_member(o,field);
    assert(json_node_get_value_type(n)==G_TYPE_INT64);assert(json_node_get_int(n)==code);
    assert(json_object_get_boolean_member(o,"down")==down);
    g_free(text);g_object_unref(p);g_object_unref(gen);json_node_unref(root);g_object_unref(b);
}
int main(void) {
    for(gint64 key=1;key<=767;key++) {check("key","code",key,true);check("key","code",key,false);}
    for(gint64 button=1;button<=5;button++)check("button","button",button,true);
    puts("PASS: production Omarchy key/button serializer preserves all integer identities");
}
