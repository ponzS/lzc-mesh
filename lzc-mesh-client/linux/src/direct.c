// Marshal the shared transport worker's events onto the GLib main loop.
// Only adjacent replaceable positions are coalesced; action barriers stay intact.
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "../../transport/include.h"
#include "app.h"
#include "direct.h"
#include "bluetooth.h"
#include "net.h"

static MeshDirect *transport;
static GMutex transport_lock;
static GMutex queue_lock;
static GQueue queue=G_QUEUE_INIT;
static bool scheduled, accepting;
static uint64_t generation, next_generation;
typedef struct { char *text; char *key; uint64_t generation; } Event;

static char *motion_key(JsonObject *o) {
    const char *kind=json_object_get_string_member_with_default(o,"_lan","");
    if (!json_object_has_member(o,"message")) return NULL;
    JsonObject *m=json_object_get_object_member(o,"message");
    const char *t=json_object_get_string_member_with_default(m,"t","");
    if (!strcmp(kind,"inject") && !strcmp(t,"move")) return g_strdup("move");
    if (!strcmp(kind,"hub") && !strcmp(t,"direct_relay")) {
        JsonObject *d=json_object_get_object_member(m,"data");
        if (d && !g_strcmp0(json_object_get_string_member_with_default(d,"kind",""),"input")) {
            JsonObject *in=json_object_get_object_member(d,"input");
            if (in && !json_object_has_member(in,"event"))
                return g_strdup_printf("relay:%s:%s:%"G_GINT64_FORMAT,
                    json_object_get_string_member_with_default(in,"session",""),
                    json_object_get_string_member_with_default(in,"target",""),
                    json_object_get_int_member_with_default(in,"barrier",0));
        }
    }
    return NULL;
}
static void event_free(Event *e){g_free(e->text);g_free(e->key);g_free(e);}
static gboolean drain(gpointer unused) {
    for (int n=0;n<256;n++) {
        g_mutex_lock(&queue_lock);
        Event *e=g_queue_pop_head(&queue);
        if (!e){scheduled=false;g_mutex_unlock(&queue_lock);return G_SOURCE_REMOVE;}
        bool valid=accepting && e->generation==generation;
        g_mutex_unlock(&queue_lock);
        if(!valid){event_free(e);continue;}
        JsonParser *p=json_parser_new();
        if (json_parser_load_from_data(p,e->text,-1,NULL)) {
            JsonObject *o=json_node_get_object(json_parser_get_root(p));
            const char *kind=json_object_get_string_member_with_default(o,"_lan","");
            if (g_str_has_prefix(kind,"bt_")) {
                if (app_operational() || !strcmp(kind,"bt_stop") || !strcmp(kind,"bt_close")) bluetooth_event(o);
            }
            else if (!strcmp(kind,"metrics")) app_log("%s",json_object_get_string_member_with_default(o,"text",""));
            else if (!strcmp(kind,"channel_status")) {
                const char *channel=json_object_get_string_member_with_default(o,"channel","");
                app_set_status(!strcmp(channel,"pointer")?S_POINTER_CHANNEL:S_KEYBOARD_CHANNEL,ST_OK,"%s",json_object_get_string_member_with_default(o,"text",""));
            }
            else if (!strcmp(kind,"status")) app_set_status(S_TRANSPORT,ST_OK,"%s",json_object_get_string_member_with_default(o,"text",""));
            else if (json_object_has_member(o,"message")) {
                JsonObject *m=json_object_get_object_member(o,"message");
                if (!strcmp(kind,"inject")) net_inject(m);
                else if (!strcmp(kind,"hub")) {
                    JsonGenerator *gen=json_generator_new();
                    json_generator_set_root(gen,json_object_get_member(o,"message"));
                    char *text=json_generator_to_data(gen,NULL);net_send_raw(text);g_free(text);g_object_unref(gen);
                }
            }
        }
        g_object_unref(p);event_free(e);
    }
    return G_SOURCE_CONTINUE;
}
static void callback(void *context,const char *text) {
    Event *e=g_new0(Event,1);e->text=g_strdup(text);e->generation=(uint64_t)(uintptr_t)context;
    JsonParser *p=json_parser_new();
    if (json_parser_load_from_data(p,text,-1,NULL)) e->key=motion_key(json_node_get_object(json_parser_get_root(p)));
    g_object_unref(p);
    g_mutex_lock(&queue_lock);
    if (!accepting || e->generation!=generation) { g_mutex_unlock(&queue_lock); event_free(e); return; }
    Event *last=g_queue_peek_tail(&queue);
    if (e->key && last && g_strcmp0(e->key,last->key)==0) {event_free(g_queue_pop_tail(&queue));}
    g_queue_push_tail(&queue,e);
    bool start=!scheduled;scheduled=true;
    g_mutex_unlock(&queue_lock);
    if (start) g_idle_add_full(G_PRIORITY_HIGH,drain,NULL,NULL);
}
void direct_init(void){
    if (!app_operational()) return;
    g_mutex_lock(&transport_lock);
    if (!transport) {
        g_mutex_lock(&queue_lock); accepting=true;generation=++next_generation;uint64_t token=generation;g_mutex_unlock(&queue_lock);
        transport=mesh_direct_new(app.device_id,callback,(void *)(uintptr_t)token);
        if (transport) mesh_direct_hub(transport,"{\"t\":\"hub_connected\"}");
    }
    g_mutex_unlock(&transport_lock);
}
void direct_hub(const char *json){g_mutex_lock(&transport_lock);if (transport) mesh_direct_hub(transport,json);g_mutex_unlock(&transport_lock);}
bool direct_input(const char *json){g_mutex_lock(&transport_lock);bool result=transport && mesh_direct_input(transport,json);g_mutex_unlock(&transport_lock);return result;}
void direct_disconnect(void){g_mutex_lock(&transport_lock);if (transport) mesh_direct_disconnect(transport);g_mutex_unlock(&transport_lock);}
void direct_shutdown(void){
    g_mutex_lock(&queue_lock); accepting=false; generation=0; scheduled=false;
    while (!g_queue_is_empty(&queue)) event_free(g_queue_pop_head(&queue));
    g_mutex_unlock(&queue_lock);
    g_mutex_lock(&transport_lock);
    if (transport){mesh_direct_free(transport);transport=NULL;}
    g_mutex_unlock(&transport_lock);
}

uint64_t direct_now_us(void){return mesh_direct_now_us();}
void direct_applied(JsonObject *trace){
    JsonGenerator *gen=json_generator_new();JsonNode *node=json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node,trace);json_generator_set_root(gen,node);
    char *text=json_generator_to_data(gen,NULL);
    g_mutex_lock(&transport_lock);if(transport)mesh_direct_applied(transport,text);g_mutex_unlock(&transport_lock);
    g_free(text);json_node_unref(node);g_object_unref(gen);
}
