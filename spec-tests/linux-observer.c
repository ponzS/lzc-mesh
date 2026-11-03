// Isolated GTK/Wayland OS-input fixture; no account, clipboard or file access.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <json-glib/json-glib.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
static bool quitting;static GtkWindow *window;static GtkWidget *focus_area;static char *status_path,*command_path;static int command_id,moves,width=1920,height=1080;
static double wheel_x,wheel_y;static JsonArray *events;static char *error_text="";static gint64 started;
static struct wl_display *display;static struct wl_seat *seat;static struct zwlr_virtual_pointer_manager_v1 *pm;static struct zwp_virtual_keyboard_manager_v1 *km;
static struct zwlr_virtual_pointer_v1 *pointer;static struct zwp_virtual_keyboard_v1 *keyboard;static struct xkb_state *state;
static uint32_t stamp(void){return (uint32_t)(g_get_monotonic_time()/1000);}
static void registry(void*d,struct wl_registry*r,uint32_t name,const char*iface,uint32_t version){
 if(!strcmp(iface,"wl_seat")&&!seat)seat=wl_registry_bind(r,name,&wl_seat_interface,1);
 if(!strcmp(iface,zwlr_virtual_pointer_manager_v1_interface.name))pm=wl_registry_bind(r,name,&zwlr_virtual_pointer_manager_v1_interface,1);
 if(!strcmp(iface,zwp_virtual_keyboard_manager_v1_interface.name))km=wl_registry_bind(r,name,&zwp_virtual_keyboard_manager_v1_interface,1);
}
static void removed(void*d,struct wl_registry*r,uint32_t name){}
static const struct wl_registry_listener registry_listener={registry,removed};
static gboolean dispatch(gint fd,GIOCondition condition,gpointer d){if(wl_display_dispatch(display)<0)return G_SOURCE_REMOVE;wl_display_flush(display);return G_SOURCE_CONTINUE;}
static bool input_init(void){
 display=wl_display_connect(NULL);if(!display)return false;struct wl_registry*r=wl_display_get_registry(display);wl_registry_add_listener(r,&registry_listener,NULL);wl_display_roundtrip(display);
 if(!seat||!pm||!km)return false;pointer=zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pm,seat);keyboard=zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(km,seat);
 struct xkb_context*context=xkb_context_new(0);struct xkb_rule_names names={.rules="evdev"};struct xkb_keymap*map=xkb_keymap_new_from_names(context,&names,0);if(!map)return false;
 state=xkb_state_new(map);char*text=xkb_keymap_get_as_string(map,XKB_KEYMAP_FORMAT_TEXT_V1);size_t size=strlen(text)+1;int fd=memfd_create("mesh-test-keymap",MFD_CLOEXEC);if(fd<0||write(fd,text,size)!=(ssize_t)size)return false;
 zwp_virtual_keyboard_v1_keymap(keyboard,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,fd,size);close(fd);free(text);xkb_keymap_unref(map);xkb_context_unref(context);
 wl_display_roundtrip(display);g_unix_fd_add(wl_display_get_fd(display),G_IO_IN,dispatch,NULL);return true;
}
static void key(uint32_t code,bool down){zwp_virtual_keyboard_v1_key(keyboard,stamp(),code,down);xkb_state_update_key(state,code+8,down?XKB_KEY_DOWN:XKB_KEY_UP);zwp_virtual_keyboard_v1_modifiers(keyboard,xkb_state_serialize_mods(state,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(state,XKB_STATE_MODS_LATCHED),xkb_state_serialize_mods(state,XKB_STATE_MODS_LOCKED),0);}
static void point(double x,double y){zwlr_virtual_pointer_v1_motion_absolute(pointer,stamp(),x,y,width,height);zwlr_virtual_pointer_v1_frame(pointer);wl_display_flush(display);}
static void draw(GtkDrawingArea*a,cairo_t*cr,int w,int h,gpointer data){cairo_set_source_rgb(cr,1,1,1);cairo_paint(cr);}
static void record(const char*kind,int code,bool down,guint modifiers){if(json_array_get_length(events)>=256)return;JsonObject*o=json_object_new();json_object_set_string_member(o,"kind",kind);json_object_set_int_member(o,"code",code);json_object_set_boolean_member(o,"down",down);json_object_set_int_member(o,"flags",modifiers);json_array_add_object_element(events,o);}
static gboolean key_down(GtkEventControllerKey*c,guint value,guint code,GdkModifierType mods,gpointer data){if(code==KEY_ESC+8){quitting=true;gtk_window_destroy(window);return TRUE;}if(code==KEY_F6+8||code==KEY_LEFTSHIFT+8||code==KEY_RIGHTSHIFT+8)record("key",code-8,true,mods);return TRUE;}
static void key_up(GtkEventControllerKey*c,guint value,guint code,GdkModifierType mods,gpointer data){if(code==KEY_F6+8||code==KEY_LEFTSHIFT+8||code==KEY_RIGHTSHIFT+8)record("key",code-8,false,mods);}
static void motion(GtkEventControllerMotion*c,double x,double y,gpointer d){moves++;}
static void press(GtkGestureClick*c,int n,double x,double y,gpointer d){record("button",gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(c)),true,0);}
static void release(GtkGestureClick*c,int n,double x,double y,gpointer d){record("button",gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(c)),false,0);}
static gboolean scroll(GtkEventControllerScroll*c,double x,double y,gpointer d){wheel_x+=x;wheel_y+=y;return TRUE;}
static double number(JsonObject*o,const char*k,double fallback){return json_object_get_double_member_with_default(o,k,fallback);}
static void command(JsonObject*c){command_id=json_object_get_int_member_with_default(c,"id",0);error_text="";const char*op=json_object_get_string_member_with_default(c,"op","");
 if(!strcmp(op,"reset")){json_array_unref(events);events=json_array_new();moves=0;wheel_x=wheel_y=0;gtk_window_present(window);gtk_widget_grab_focus(focus_area);}
 else if(!strcmp(op,"park")){if(!gtk_widget_has_focus(focus_area)){error_text="Fixture lost focus; park refused";return;}point(width/2.0,height/2.0);}
 else if(!strcmp(op,"quit")){quitting=true;gtk_window_destroy(window);return;}
 else if(!strcmp(op,"restore"))point(number(c,"x",.5),number(c,"y",.5));
 else if(!strcmp(op,"edge")){int dir=number(c,"direction",0);double p=number(c,"position",.5);point(dir==0?0:dir==1?width-1:p*(width-1),dir==2?0:dir==3?height-1:p*(height-1));}
 else if(!strcmp(op,"input")){if(!gtk_window_is_active(window)&&!json_object_get_boolean_member_with_default(c,"allow_captured",false)){error_text="Fixture lost focus; input refused";return;}JsonArray*list=json_object_get_array_member(c,"events");for(guint i=0;list&&i<json_array_get_length(list);i++){JsonObject*e=json_array_get_object_element(list,i);const char*t=json_object_get_string_member_with_default(e,"t","");bool down=json_object_get_boolean_member_with_default(e,"down",false);
  if(!strcmp(t,"key"))key(number(e,"code",KEY_F6),down);
  if(!strcmp(t,"motion"))zwlr_virtual_pointer_v1_motion(pointer,stamp(),wl_fixed_from_double(number(e,"dx",0)),wl_fixed_from_double(number(e,"dy",0)));
  if(!strcmp(t,"button"))zwlr_virtual_pointer_v1_button(pointer,stamp(),number(e,"button",1)==2?BTN_RIGHT:BTN_LEFT,down?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED);
  if(!strcmp(t,"wheel")){zwlr_virtual_pointer_v1_axis_source(pointer,WL_POINTER_AXIS_SOURCE_WHEEL);zwlr_virtual_pointer_v1_axis(pointer,stamp(),WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_double(-number(e,"dy",0)/12));}
  zwlr_virtual_pointer_v1_frame(pointer);wl_display_flush(display);g_usleep(20000);
 }}else error_text="Unknown fixture command";
}
static gboolean tick(gpointer d){
 if(g_get_monotonic_time()-started>1800*G_USEC_PER_SEC){gtk_window_destroy(window);return G_SOURCE_REMOVE;}
 JsonParser*p=json_parser_new();if(json_parser_load_from_file(p,command_path,NULL)){g_unlink(command_path);command(json_node_get_object(json_parser_get_root(p)));}g_object_unref(p);if(quitting)return G_SOURCE_REMOVE;
 JsonObject*o=json_object_new();json_object_set_int_member(o,"command",command_id);json_object_set_string_member(o,"error",error_text);json_object_set_array_member(o,"events",json_array_ref(events));json_object_set_int_member(o,"moves",moves);json_object_set_double_member(o,"wheel_x",wheel_x);json_object_set_double_member(o,"wheel_y",wheel_y);json_object_set_boolean_member(o,"focused",gtk_widget_has_focus(focus_area));
 double x=-1,y=-1;GdkDevice*device=gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));GdkSurface*surface=gdk_device_get_surface_at_position(device,&x,&y);bool inside=surface==gtk_native_get_surface(GTK_NATIVE(window));json_object_set_boolean_member(o,"pointer_inside",inside);json_object_set_double_member(o,"cursor_x",inside?x:-1);json_object_set_double_member(o,"cursor_y",inside?y:-1);
 JsonNode*root=json_node_new(JSON_NODE_OBJECT);json_node_take_object(root,o);char*text=json_to_string(root,false);char*temp=g_strconcat(status_path,".tmp",NULL);g_file_set_contents(temp,text,-1,NULL);g_rename(temp,status_path);g_free(temp);g_free(text);json_node_unref(root);return G_SOURCE_CONTINUE;
}
static void activate(GtkApplication*app,gpointer d){
 window=GTK_WINDOW(gtk_application_window_new(app));gtk_window_set_title(window,"LZC Mesh isolated input test");gtk_window_set_decorated(window,false);
 // A private exclusive test layer prevents focus from falling through to any
 // user application. The 2px border leaves production edge strips reachable.
 gtk_layer_init_for_window(window);gtk_layer_set_namespace(window,"lzc-mesh-test-observer");gtk_layer_set_layer(window,GTK_LAYER_SHELL_LAYER_OVERLAY);gtk_layer_set_keyboard_mode(window,GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
 for(int edge=0;edge<4;edge++){gtk_layer_set_anchor(window,edge,true);gtk_layer_set_margin(window,edge,2);}
 GtkWidget*area=gtk_drawing_area_new();focus_area=area;gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area),draw,NULL,NULL);gtk_widget_set_focusable(area,true);gtk_window_set_child(window,area);
 GtkEventController*k=gtk_event_controller_key_new();g_signal_connect(k,"key-pressed",G_CALLBACK(key_down),NULL);g_signal_connect(k,"key-released",G_CALLBACK(key_up),NULL);gtk_widget_add_controller(area,k);
 GtkEventController*m=gtk_event_controller_motion_new();g_signal_connect(m,"motion",G_CALLBACK(motion),NULL);gtk_widget_add_controller(area,m);
 GtkGesture*g=gtk_gesture_click_new();gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g),0);g_signal_connect(g,"pressed",G_CALLBACK(press),NULL);g_signal_connect(g,"released",G_CALLBACK(release),NULL);gtk_widget_add_controller(area,GTK_EVENT_CONTROLLER(g));
 GtkEventController*s=gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES|GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);g_signal_connect(s,"scroll",G_CALLBACK(scroll),NULL);gtk_widget_add_controller(area,s);
 GListModel*monitors=gdk_display_get_monitors(gdk_display_get_default());GdkRectangle bounds={0};GdkMonitor*monitor=g_list_model_get_item(monitors,0);if(monitor){gdk_monitor_get_geometry(monitor,&bounds);width=bounds.width;height=bounds.height;g_object_unref(monitor);}
 gtk_window_present(window);gtk_widget_grab_focus(area);g_timeout_add(50,tick,NULL);
}
int main(int argc,char**argv){if(argc!=2)return 2;status_path=g_strdup_printf("%s/linux-observer.json",argv[1]);command_path=g_strconcat(status_path,".command",NULL);events=json_array_new();started=g_get_monotonic_time();if(!input_init())return 3;GtkApplication*app=gtk_application_new("cloud.lazycat.lzcmesh.test",G_APPLICATION_NON_UNIQUE);g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);int result=g_application_run(G_APPLICATION(app),1,argv);if(keyboard)zwp_virtual_keyboard_v1_destroy(keyboard);if(pointer)zwlr_virtual_pointer_v1_destroy(pointer);wl_display_flush(display);wl_display_disconnect(display);g_object_unref(app);return result;}
