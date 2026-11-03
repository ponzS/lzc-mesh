#define _GNU_SOURCE
#include "local_input.h"
#include <glib-unix.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

typedef struct { int fd; guint watch; char *path; unsigned capabilities; } Device;
static GPtrArray *devices;
static guint scan_timer;
static void (*on_activity)(void);

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
static gboolean readable(gint fd, GIOCondition condition, gpointer data) {
    Device *device=data;
    struct input_event events[32]; ssize_t bytes;
    if (condition & (G_IO_HUP|G_IO_ERR)) {
        device->watch=0;close(device->fd);device->fd=-1;
        return G_SOURCE_REMOVE;
    }
    while ((bytes=read(fd,events,sizeof(events)))>0) {
        for (size_t i=0;i<(size_t)bytes/sizeof(*events);i++) {
            const struct input_event *e=&events[i];
            if (local_input_event_activity(e) && on_activity) {
                // The callback may close every monitor, including this source.
                // No device or event value is accessed after calling it.
                void (*callback)(void)=on_activity; callback(); return G_SOURCE_CONTINUE;
            }
        }
    }
    return G_SOURCE_CONTINUE;
}
static gboolean scan(gpointer unused) {
    (void)unused;
    GDir *dir=g_dir_open("/dev/input",0,NULL);
    if (!dir) return G_SOURCE_CONTINUE;
    for (guint i=0;devices && i<devices->len;) {
        Device *device=g_ptr_array_index(devices,i);
        if (device->fd>=0) {i++;continue;}
        g_ptr_array_remove_index_fast(devices,i);g_free(device->path);g_free(device);
    }
    const char *name;
    while (devices && devices->len<64 && (name=g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name,"event") || !g_ascii_isdigit(name[5])) continue;
        char *path=g_build_filename("/dev/input",name,NULL);
        bool exists=false;
        for (guint i=0;i<devices->len;i++) if (!strcmp(((Device*)g_ptr_array_index(devices,i))->path,path)) { exists=true;break; }
        if (exists) { g_free(path);continue; }
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
        unsigned caps=fd>=0 ? physical_device(fd) : 0;
        if (!caps) { if(fd>=0)close(fd);g_free(path);continue; }
        Device *device=g_new0(Device,1);device->fd=fd;device->path=path;device->capabilities=caps;
        device->watch=g_unix_fd_add(fd,G_IO_IN|G_IO_HUP|G_IO_ERR,readable,device);
        g_ptr_array_add(devices,device);
    }
    g_dir_close(dir);return G_SOURCE_CONTINUE;
}
bool local_input_start(void (*activity)(void)) {
    local_input_stop();on_activity=activity;devices=g_ptr_array_new();scan(NULL);
    scan_timer=g_timeout_add_seconds(1,scan,NULL);
    unsigned caps=0;
    for(guint i=0;i<devices->len;i++)caps|=((Device*)g_ptr_array_index(devices,i))->capabilities;
    return caps==3;
}
void local_input_stop(void) {
    on_activity=NULL;
    if(scan_timer){g_source_remove(scan_timer);scan_timer=0;}
    if(!devices)return;
    for(guint i=0;i<devices->len;i++) {
        Device *device=g_ptr_array_index(devices,i);
        if(device->watch)g_source_remove(device->watch);
        if(device->fd>=0)close(device->fd);
        g_free(device->path);g_free(device);
    }
    g_ptr_array_free(devices,TRUE);devices=NULL;
}
