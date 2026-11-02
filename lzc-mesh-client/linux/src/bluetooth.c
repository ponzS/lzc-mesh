// RFCOMM descriptors retain the host network namespace when passed by SCM_RIGHTS.
// This adapter owns bytes only; the shared core authenticates TLS and input grants.
#include <gio/gio.h>
#include <glib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "bluetooth.h"
#include "direct.h"

typedef struct {atomic_int refs; int fd; char *id,*peer; atomic_bool closed;} Connection;
static GHashTable *connections,*dialing;
static guint generation;
static bool running;
static GMutex dial_lock;
static GList *pending_dials;
typedef struct {guint generation;char *peer,*address;guint channel;int fd;bool capability;int request_fd;bool canceled;} Dial;

static void notify(const char *kind,const char *id,const char *peer,const char *data,const char *error) {
    JsonBuilder *b=json_builder_new();json_builder_begin_object(b);
    const char *keys[]={"t","connection","peer","data","error"};
    const char *values[]={kind,id,peer,data,error};
    for(guint i=0;i<G_N_ELEMENTS(keys);i++) if(values[i]) {json_builder_set_member_name(b,keys[i]);json_builder_add_string_value(b,values[i]);}
    json_builder_end_object(b);JsonNode *root=json_builder_get_root(b);
    JsonGenerator *gen=json_generator_new();json_generator_set_root(gen,root);
    char *text=json_generator_to_data(gen,NULL);direct_hub(text);
    g_free(text);g_object_unref(gen);json_node_free(root);g_object_unref(b);
}
static Connection *retain(Connection *c){atomic_fetch_add(&c->refs,1);return c;}
static void release(gpointer data){Connection *c=data;if(atomic_fetch_sub(&c->refs,1)==1){close(c->fd);g_free(c->id);g_free(c->peer);g_free(c);}}
static void stop_connection(Connection *c){if(!atomic_exchange(&c->closed,true))shutdown(c->fd,SHUT_RDWR);}
static gboolean reader_done(gpointer data) {
    Connection *c=data;
    if(connections && g_hash_table_lookup(connections,c->id)==c)g_hash_table_remove(connections,c->id);
    release(c);return G_SOURCE_REMOVE;
}
static gpointer reader(gpointer data) {
    Connection *c=data;unsigned char bytes[16384];ssize_t n;
    while(!atomic_load(&c->closed) && (n=read(c->fd,bytes,sizeof(bytes)))>0){
        char *encoded=g_base64_encode(bytes,(gsize)n);notify("bt_data",c->id,NULL,encoded,NULL);g_free(encoded);
    }
    notify("bt_closed",c->id,NULL,NULL,"蓝牙连接已断开");stop_connection(c);
    g_idle_add(reader_done,c);return NULL;
}
static const char *broker_path(void){
    const char *override=g_getenv("LZC_MESH_BLUETOOTH_SOCKET");
    return override ? override : "/lzcapp/document/.lzc-mesh/host-bluetooth.sock";
}
static void close_request(Dial *d,int fd){g_mutex_lock(&dial_lock);d->request_fd=-1;close(fd);g_mutex_unlock(&dial_lock);}
static int broker(Dial *d,const char *request,int *received_fd){
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
    g_mutex_lock(&dial_lock);
    if(d->canceled){g_mutex_unlock(&dial_lock);close(fd);return -1;}
    d->request_fd=fd;g_mutex_unlock(&dial_lock);
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(strlen(broker_path())>=sizeof(addr.sun_path)){close_request(d,fd);return -1;}
    strcpy(addr.sun_path,broker_path());struct timeval timeout={.tv_sec=10};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr))<0 || send(fd,request,strlen(request),MSG_NOSIGNAL)!=(ssize_t)strlen(request)){close_request(d,fd);return -1;}
    char reply[512],control[CMSG_SPACE(sizeof(int))];struct iovec iov={reply,sizeof(reply)-1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    ssize_t n=recvmsg(fd,&msg,MSG_CMSG_CLOEXEC);close_request(d,fd);if(n<=0)return -1;reply[n]=0;
    *received_fd=-1;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len==CMSG_LEN(sizeof(int)))memcpy(received_fd,CMSG_DATA(c),sizeof(int));
    return strstr(reply,"true")?0:-1;
}
static void dial_free(Dial *d){g_free(d->peer);g_free(d->address);g_free(d);}
static gboolean dial_done(gpointer data){
    Dial *d=data;
    pending_dials=g_list_remove(pending_dials,d);
    if(d->generation!=generation || !running){if(d->fd>=0)close(d->fd);dial_free(d);return G_SOURCE_REMOVE;}
    if(d->capability){
        if(d->fd==-2)direct_hub("{\"t\":\"bt_local\",\"bluetooth\":{\"role\":\"connector\"}}");
        else notify("bt_local",NULL,NULL,NULL,"宿主机蓝牙桥接不可用");
    }else{
        g_hash_table_remove(dialing,d->peer);
        if(d->fd>=0){
            Connection *c=g_new0(Connection,1);atomic_init(&c->refs,1);atomic_init(&c->closed,false);c->fd=d->fd;c->id=g_uuid_string_random();c->peer=g_strdup(d->peer);
            g_hash_table_insert(connections,g_strdup(c->id),c);notify("bt_open",c->id,c->peer,NULL,NULL);
            g_thread_unref(g_thread_new("mesh-bt-read",reader,retain(c)));
        }else notify("bt_closed",NULL,NULL,NULL,"蓝牙连接失败，请检查配对和距离");
    }
    dial_free(d);return G_SOURCE_REMOVE;
}
static gpointer dial_worker(gpointer data){
    Dial *d=data;int fd=-1;
    char *request=d->capability?g_strdup("{\"op\":\"capabilities\"}\n"):g_strdup_printf("{\"op\":\"connect\",\"address\":\"%s\",\"channel\":%u}\n",d->address,d->channel);
    int result=broker(d,request,&fd);g_free(request);d->fd=d->capability?(result==0?-2:-1):fd;
    if(result!=0 && fd>=0){close(fd);d->fd=-1;}
    g_idle_add(dial_done,d);return NULL;
}
typedef struct {Connection *connection;guchar *bytes;gsize length;} Write;
static void write_worker(GTask *task,gpointer object,gpointer task_data,GCancellable *cancel){
    Write *w=task_data;gsize offset=0;
    while(offset<w->length && !atomic_load(&w->connection->closed)){
        ssize_t n=send(w->connection->fd,w->bytes+offset,w->length-offset,MSG_NOSIGNAL);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){stop_connection(w->connection);break;}offset+=(gsize)n;
    }
    if(offset==w->length)notify("bt_written",w->connection->id,NULL,NULL,NULL);
    else notify("bt_closed",w->connection->id,NULL,NULL,"蓝牙发送失败");
    release(w->connection);g_free(w->bytes);g_free(w);g_task_return_boolean(task,TRUE);
}
void bluetooth_stop(void) {
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "_lan", "bt_stop");
    bluetooth_event(object); json_object_unref(object);
}
void bluetooth_event(JsonObject *o){
    const char *kind=json_object_get_string_member_with_default(o,"_lan","");
    if(!connections){connections=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,release);dialing=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);}
    if(!strcmp(kind,"bt_stop")){
        running=false;generation++;
        g_mutex_lock(&dial_lock);
        for(GList *item=pending_dials;item;item=item->next){Dial *d=item->data;d->canceled=true;if(d->request_fd>=0)shutdown(d->request_fd,SHUT_RDWR);}
        g_mutex_unlock(&dial_lock);
        GHashTableIter it;gpointer value;g_hash_table_iter_init(&it,connections);
        while(g_hash_table_iter_next(&it,NULL,&value))stop_connection(value);
        g_hash_table_remove_all(connections);g_hash_table_remove_all(dialing);return;
    }
    if(!strcmp(kind,"bt_start")){
        running=true;generation++;Dial *d=g_new0(Dial,1);d->generation=generation;d->capability=true;d->fd=-1;d->request_fd=-1;pending_dials=g_list_prepend(pending_dials,d);
        g_thread_unref(g_thread_new("mesh-bt-capability",dial_worker,d));return;
    }
    if(!running)return;
    if(!strcmp(kind,"bt_connect")){
        const char *peer=json_object_get_string_member_with_default(o,"peer","");
        if(g_hash_table_contains(dialing,peer))return;
        GHashTableIter it;gpointer value;g_hash_table_iter_init(&it,connections);
        while(g_hash_table_iter_next(&it,NULL,&value))if(!g_strcmp0(((Connection*)value)->peer,peer))return;
        JsonObject *bt=json_object_get_object_member(o,"bluetooth");if(!bt)return;
        const char *address=json_object_get_string_member_with_default(bt,"address","");
        guint channel=(guint)json_object_get_int_member_with_default(bt,"channel",0);
        if(strlen(address)!=17 || channel<1 || channel>30)return;
        for(guint i=0;i<17;i++)if((i%3==2 && address[i]!=':') || (i%3!=2 && !g_ascii_isxdigit(address[i])))return;
        g_hash_table_add(dialing,g_strdup(peer));Dial *d=g_new0(Dial,1);d->generation=generation;d->peer=g_strdup(peer);d->address=g_strdup(address);d->channel=channel;d->fd=-1;d->request_fd=-1;pending_dials=g_list_prepend(pending_dials,d);
        g_thread_unref(g_thread_new("mesh-bt-connect",dial_worker,d));return;
    }
    const char *id=json_object_get_string_member_with_default(o,"connection","");Connection *c=g_hash_table_lookup(connections,id);if(!c)return;
    if(!strcmp(kind,"bt_close")){stop_connection(c);g_hash_table_remove(connections,id);}
    else if(!strcmp(kind,"bt_send")){
        const char *encoded=json_object_get_string_member_with_default(o,"data","");if(strlen(encoded)>90000){stop_connection(c);return;}
        Write *w=g_new0(Write,1);w->connection=retain(c);w->bytes=g_base64_decode(encoded,&w->length);
        GTask *task=g_task_new(NULL,NULL,NULL,NULL);g_task_set_task_data(task,w,NULL);g_task_run_in_thread(task,write_worker);g_object_unref(task);
    }
}
