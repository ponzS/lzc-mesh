// Headless regression for production discovery/lifetime logic. All evdev I/O
// is fake; no real input nodes, desktop, keyboard content or clipboard is used.
#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static GThread *main_thread;
static GMutex fake_lock;
static int next_fd=100,opened,closed,io_on_main,hotplug,read_once;
static int kinds[4096];
typedef struct {int index;} FakeDir;
static void note_io(void){if(g_thread_self()==main_thread)g_atomic_int_inc(&io_on_main);}
static GDir *fake_dir_open(const char *path,guint flags,GError **error){note_io();return (GDir*)g_new0(FakeDir,1);}
static const char *fake_dir_read_name(GDir *d){FakeDir *dir=(FakeDir*)d;static const char *names[]={"event0","event1","event2","event3"};int n=g_atomic_int_get(&hotplug)?4:3;return dir->index<n?names[dir->index++]:NULL;}
static void fake_dir_close(GDir *d){g_free(d);}
static int fake_open(const char *path,int flags,...){
    note_io();g_usleep(120000); // slow device backend, without blocking the UI
    int kind=path[strlen(path)-1]-'0';g_mutex_lock(&fake_lock);int fd=next_fd++;assert(fd<4096);kinds[fd]=kind;g_mutex_unlock(&fake_lock);g_atomic_int_inc(&opened);return fd;
}
static int fake_close(int fd){note_io();g_usleep(30000);g_atomic_int_inc(&closed);return 0;}
static int fake_fstat(int fd,struct stat *st){note_io();memset(st,0,sizeof *st);st->st_mode=S_IFCHR;return 0;}
static void set_bit(void *data,unsigned n){((unsigned long*)data)[n/(8*sizeof(long))]|=1UL<<(n%(8*sizeof(long)));}
static int fake_ioctl(int fd,unsigned long request,...){
    note_io();va_list args;va_start(args,request);void *data=va_arg(args,void*);va_end(args);
    g_mutex_lock(&fake_lock);int kind=kinds[fd];g_mutex_unlock(&fake_lock);
    if(request==EVIOCGID){struct input_id *id=data;memset(id,0,sizeof *id);id->bustype=kind==2?BUS_VIRTUAL:BUS_USB;return 0;}
    memset(data,0,_IOC_SIZE(request));
    if(_IOC_NR(request)==_IOC_NR(EVIOCGBIT(EV_KEY,1)))set_bit(data,kind==0?KEY_A:BTN_LEFT);
    if(_IOC_NR(request)==_IOC_NR(EVIOCGBIT(EV_REL,1))&&kind!=0)set_bit(data,REL_X);
    return 0;
}
static gboolean unused_watch(gpointer data){return G_SOURCE_CONTINUE;}
static guint fake_fd_add(gint fd,GIOCondition flags,GUnixFDSourceFunc callback,gpointer data){assert(g_thread_self()==main_thread);return g_timeout_add_seconds(3600,unused_watch,NULL);}
static ssize_t fake_read(int fd,void *buffer,size_t size){
    if(g_atomic_int_compare_and_exchange(&read_once,1,0)){
        struct input_event event={.type=EV_KEY,.code=KEY_F6,.value=1};memcpy(buffer,&event,sizeof event);return sizeof event;
    }
    errno=EAGAIN;return -1;
}
#define g_dir_open fake_dir_open
#define g_dir_read_name fake_dir_read_name
#define g_dir_close fake_dir_close
#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define ioctl fake_ioctl
#define g_unix_fd_add fake_fd_add
#define read fake_read
#include "../lzc-mesh-client/linux/src/local_input.c"
#undef g_dir_open
#undef g_dir_read_name
#undef g_dir_close
#undef open
#undef close
#undef fstat
#undef ioctl
#undef g_unix_fd_add
#undef read

static int ready_count,activity_count,pulses;
static gint64 pulse_last,pulse_max;
static void ready(bool available){assert(g_thread_self()==main_thread);assert(available);ready_count++;}
static void activity(void){assert(g_thread_self()==main_thread);activity_count++;local_input_stop();}
static gboolean pulse(gpointer unused){gint64 now=g_get_monotonic_time();if(pulse_last&&now-pulse_last>pulse_max)pulse_max=now-pulse_last;pulse_last=now;pulses++;return G_SOURCE_CONTINUE;}
static void spin_ms(int ms){gint64 end=g_get_monotonic_time()+ms*1000;while(g_get_monotonic_time()<end){while(g_main_context_iteration(NULL,FALSE)){}g_usleep(1000);}}
static void wait_ready(int count){for(int i=0;i<3000&&ready_count<count;i++)spin_ms(1);assert(ready_count==count);}
int main(void){
    main_thread=g_thread_self();guint timer=g_timeout_add(10,pulse,NULL);
    gint64 t=g_get_monotonic_time();local_input_start(activity,ready);assert(g_get_monotonic_time()-t<100000);
    wait_ready(1);assert(devices&&devices->len==2);assert(pulses>=15);
    assert(pulse_max<150000); // metadata delay is 360ms, but never on this loop
    Device *d=g_ptr_array_index(devices,0);int fd=d->fd;guint watch=d->watch;
    g_atomic_int_set(&read_once,1);assert(readable(fd,G_IO_IN,d)==G_SOURCE_REMOVE);g_source_remove(watch);
    assert(activity_count==1&&!devices);
    // Cancel a slow scan, then start a new receive epoch before it completes.
    local_input_start(activity,ready);spin_ms(20);t=g_get_monotonic_time();local_input_stop();assert(g_get_monotonic_time()-t<100000);
    local_input_start(activity,ready);wait_ready(2);spin_ms(150);
    assert(ready_count==2&&devices->len==2); // no stale callback or duplicate FD
    g_atomic_int_set(&hotplug,1);
    for(int i=0;i<4000&&devices->len!=3;i++)spin_ms(1);
    assert(devices->len==3&&ready_count==2&&pulse_max<150000);
    t=g_get_monotonic_time();local_input_stop();assert(g_get_monotonic_time()-t<100000);
    for(int i=0;i<4000&&g_atomic_int_get(&opened)!=g_atomic_int_get(&closed);i++)spin_ms(1);
    spin_ms(30);g_source_remove(timer);
    assert(g_atomic_int_get(&opened)==g_atomic_int_get(&closed));
    assert(g_atomic_int_get(&io_on_main)==0);
    printf("PASS: slow metadata and cleanup off main loop, heartbeat max %.3fms, stale scan rejection, physical callback, hotplug, balanced %d FDs\n",pulse_max/1000.0,g_atomic_int_get(&opened));
    return 0;
}
