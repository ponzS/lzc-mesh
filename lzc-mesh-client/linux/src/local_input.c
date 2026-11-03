#define _GNU_SOURCE
#include "local_input.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

typedef struct { int fd; guint watch; char *path; unsigned capabilities; } Device;
typedef struct { guint64 generation; GHashTable *known; } Scan;
// These fields belong exclusively to the GLib main context. Workers receive
// immutable path snapshots, never pointers into the live devices array.
static GPtrArray *devices;
static guint scan_timer;
static guint64 generation;
static GCancellable *scan_cancel;
static bool scanning;
static int reported_caps=-1;
static void (*on_activity)(void);
static void (*on_ready)(bool);

static bool bit(const unsigned long *bits, unsigned n) { return (bits[n/(8*sizeof(*bits))] >> (n%(8*sizeof(*bits)))) & 1; }
static unsigned physical_device(int fd) {
    struct stat st; struct input_id id;
    unsigned long keys[(KEY_MAX+8*sizeof(long))/(8*sizeof(long))] = {0};
    unsigned long rel[(REL_MAX+8*sizeof(long))/(8*sizeof(long))] = {0};
    if (fstat(fd,&st) || !S_ISCHR(st.st_mode) || ioctl(fd,EVIOCGID,&id) || id.bustype==BUS_VIRTUAL) return 0;
    ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(keys)),keys);
    ioctl(fd,EVIOCGBIT(EV_REL,sizeof(rel)),rel);
    return ((bit(keys,KEY_A) || bit(keys,KEY_ENTER)) ? 1u : 0u)
        | ((bit(keys,BTN_LEFT) || bit(keys,BTN_TOUCH) || bit(rel,REL_X)) ? 2u : 0u);
}
static void device_free(gpointer value) {
    Device *device=value;
    // All live watches are detached on the main context before ownership moves
    // to a cleanup worker. Even close(evdev) can block on a device backend.
    if(device->fd>=0)close(device->fd);
    g_free(device->path);g_free(device);
}
static gpointer cleanup_worker(gpointer value) { g_ptr_array_unref(value);return NULL; }
static void dispose_devices(gpointer value) {
    GPtrArray *list=value;
    if(!list)return;
    if(!list->len){g_ptr_array_unref(list);return;}
    for(guint i=0;i<list->len;i++) {
        Device *device=g_ptr_array_index(list,i);
        if(device->watch){g_source_remove(device->watch);device->watch=0;}
    }
    GThread *thread=g_thread_new("mesh-input-close",cleanup_worker,list);
    g_thread_unref(thread);
}
static void close_later(int fd) {
    GPtrArray *list=g_ptr_array_new_with_free_func(device_free);
    Device *device=g_new0(Device,1);device->fd=fd;g_ptr_array_add(list,device);
    dispose_devices(list);
}
static gboolean readable(gint fd,GIOCondition condition,gpointer data) {
    Device *device=data;
    struct input_event events[32];ssize_t bytes;
    if(condition&(G_IO_HUP|G_IO_ERR)) {
        device->watch=0;device->fd=-1;close_later(fd);
        return G_SOURCE_REMOVE;
    }
    while((bytes=read(fd,events,sizeof(events)))>0) {
        for(size_t i=0;i<(size_t)bytes/sizeof(*events);i++) {
            if(local_input_event_activity(&events[i])&&on_activity) {
                // The callback may retire every device. Do not access device
                // or event data after it; removal of this watch is final.
                void (*callback)(void)=on_activity;device->watch=0;
                callback();return G_SOURCE_REMOVE;
            }
        }
    }
    return G_SOURCE_CONTINUE;
}
static void scan_free(gpointer value) {
    Scan *scan=value;g_hash_table_unref(scan->known);g_free(scan);
}
static void scan_worker(GTask *task,gpointer source,gpointer value,GCancellable *cancel) {
    (void)source;
    Scan *scan=value;
    GPtrArray *found=g_ptr_array_new_with_free_func(device_free);
    GDir *dir=g_dir_open("/dev/input",0,NULL);
    const char *name;
    while(dir&&!g_cancellable_is_cancelled(cancel)&&
          found->len+g_hash_table_size(scan->known)<64&&(name=g_dir_read_name(dir))) {
        if(!g_str_has_prefix(name,"event")||!g_ascii_isdigit(name[5]))continue;
        char *path=g_build_filename("/dev/input",name,NULL);
        if(g_hash_table_contains(scan->known,path)){g_free(path);continue;}
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
        unsigned caps=fd>=0?physical_device(fd):0;
        if(!caps){if(fd>=0)close(fd);g_free(path);continue;}
        Device *device=g_new0(Device,1);device->fd=fd;device->path=path;device->capabilities=caps;
        g_ptr_array_add(found,device);
    }
    if(dir)g_dir_close(dir);
    // Cancellation/result destruction also closes candidates off the UI thread.
    g_task_return_pointer(task,found,dispose_devices);
}
static void scan_done(GObject *source,GAsyncResult *result,gpointer unused) {
    (void)source;(void)unused;
    GTask *task=G_TASK(result);Scan *scan=g_task_get_task_data(task);
    GError *error=NULL;
    GPtrArray *found=g_task_propagate_pointer(task,&error);
    if(scan->generation!=generation||!devices||scan_cancel!=g_task_get_cancellable(task)) {
        dispose_devices(found);g_clear_error(&error);return;
    }
    scanning=false;
    if(error){g_clear_error(&error);dispose_devices(found);return;}
    while(found&&found->len) {
        Device *device=g_ptr_array_steal_index_fast(found,found->len-1);
        device->watch=g_unix_fd_add(device->fd,G_IO_IN|G_IO_HUP|G_IO_ERR,readable,device);
        g_ptr_array_add(devices,device);
    }
    if(found)g_ptr_array_unref(found);
    unsigned caps=0;
    for(guint i=0;i<devices->len;i++) {
        Device *device=g_ptr_array_index(devices,i);
        if(device->fd>=0)caps|=device->capabilities;
    }
    if(reported_caps!=(int)caps) {
        reported_caps=(int)caps;
        // This callback may stop/restart monitoring; access no global state after.
        if(on_ready)on_ready(caps==3);
    }
}
static gboolean scan_start(gpointer unused) {
    (void)unused;
    if(!devices||scanning)return G_SOURCE_CONTINUE;
    Scan *scan=g_new0(Scan,1);scan->generation=generation;
    scan->known=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    for(guint i=0;i<devices->len;) {
        Device *device=g_ptr_array_index(devices,i);
        if(device->fd<0){g_ptr_array_remove_index_fast(devices,i);continue;}
        g_hash_table_add(scan->known,g_strdup(device->path));i++;
    }
    scanning=true;
    GTask *task=g_task_new(NULL,scan_cancel,scan_done,NULL);
    g_task_set_task_data(task,scan,scan_free);
    g_task_run_in_thread(task,scan_worker);g_object_unref(task);
    return G_SOURCE_CONTINUE;
}
void local_input_start(void (*activity)(void),void (*ready)(bool)) {
    local_input_stop();on_activity=activity;on_ready=ready;reported_caps=-1;
    devices=g_ptr_array_new_with_free_func(device_free);
    scan_cancel=g_cancellable_new();scan_start(NULL);
    scan_timer=g_timeout_add_seconds(1,scan_start,NULL);
}
void local_input_stop(void) {
    generation++;on_activity=NULL;on_ready=NULL;
    if(scan_timer){g_source_remove(scan_timer);scan_timer=0;}
    if(scan_cancel){g_cancellable_cancel(scan_cancel);g_clear_object(&scan_cancel);}
    scanning=false;
    GPtrArray *old=devices;devices=NULL;
    dispose_devices(old);
}
