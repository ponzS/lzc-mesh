// Tray icon for Linux: StatusNotifierItem (the protocol used by KDE, Waybar,
// quickshell, GNOME's AppIndicator extension...) plus its menu over
// com.canonical.dbusmenu, implemented directly with GDBus. No extra libraries.
//
// The icon is sent as ARGB32 pixmaps drawn with cairo (two small screens),
// solid when connected and sharing, faded otherwise.
#include <cairo.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "autostart.h"
#include "net.h"
#include "tray.h"
#include "ui.h"

#define ITEM_PATH "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"

static GDBusConnection *bus;
static guint item_reg, menu_reg, name_own, watcher_watch;
static char *bus_name;
static guint32 menu_revision = 1;
static bool registered;

// ------------------------------------------------------------------ icon

static GVariant *icon_pixmaps(bool on) {
    GVariantBuilder arr;
    g_variant_builder_init(&arr, G_VARIANT_TYPE("a(iiay)"));
    int sizes[] = {22, 32, 48};
    for (size_t k = 0; k < G_N_ELEMENTS(sizes); k++) {
        int s = sizes[k];
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s, s);
        cairo_t *cr = cairo_create(surf);
        double u = s / 16.0;
        cairo_set_line_width(cr, 1.3 * u);
        // Light-on-dark bars are the common case on Linux; the faded state reads on both.
        cairo_set_source_rgba(cr, 1, 1, 1, on ? 1.0 : 0.45);
        cairo_rectangle(cr, 1.2 * u, 2.2 * u, 9.6 * u, 6.6 * u);
        cairo_stroke(cr);
        cairo_rectangle(cr, 5.2 * u, 7.2 * u, 9.6 * u, 6.6 * u);
        if (app.captured || app.controlled) cairo_fill_preserve(cr);
        cairo_stroke(cr);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        // SNI wants ARGB32 in network byte order; cairo stores native-endian premultiplied.
        int stride = cairo_image_surface_get_stride(surf);
        unsigned char *data = cairo_image_surface_get_data(surf);
        GByteArray *bytes = g_byte_array_sized_new(s * s * 4);
        for (int y = 0; y < s; y++) {
            guint32 *row = (guint32 *)(data + y * stride);
            for (int x = 0; x < s; x++) {
                guint32 v = GUINT32_TO_BE(row[x]);
                g_byte_array_append(bytes, (guint8 *)&v, 4);
            }
        }
        cairo_surface_destroy(surf);
        GVariant *ay = g_variant_new_from_data(G_VARIANT_TYPE("ay"), bytes->data, bytes->len, TRUE, (GDestroyNotify)g_byte_array_unref, bytes);
        g_variant_builder_add(&arr, "(ii@ay)", s, s, ay);
    }
    return g_variant_builder_end(&arr);
}

static char *tooltip_text(void) {
    StatusLevel lv;
    char *summary = app_summary(&lv);
    char *ctrl = app_control_line();
    char *t = g_strdup_printf("%s%s%s%s", summary, app.sharing_enabled ? "" : " · 键鼠共享已暂停", ctrl ? "\n" : "", ctrl ? ctrl : "");
    g_free(summary);
    g_free(ctrl);
    return t;
}

// ------------------------------------------------------------------ StatusNotifierItem

static const char item_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='WindowId' type='i' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "<signal name='NewIcon'/><signal name='NewToolTip'/><signal name='NewStatus'><arg type='s'/></signal><signal name='NewTitle'/>"
    "</interface></node>";

static GVariant *item_get(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop, GError **err, gpointer u) {
    bool on = app_operational() && app.sharing_enabled;
    if (!strcmp(prop, "Category")) return g_variant_new_string("ApplicationStatus");
    if (!strcmp(prop, "Id")) return g_variant_new_string("lzc-mesh");
    if (!strcmp(prop, "Title")) return g_variant_new_string("懒猫设备接力");
    if (!strcmp(prop, "Status")) return g_variant_new_string("Active");
    if (!strcmp(prop, "WindowId")) return g_variant_new_int32(0);
    if (!strcmp(prop, "IconName")) return g_variant_new_string("");
    if (!strcmp(prop, "IconPixmap")) return icon_pixmaps(on);
    if (!strcmp(prop, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!strcmp(prop, "Menu")) return g_variant_new_object_path(MENU_PATH);
    if (!strcmp(prop, "ToolTip")) {
        char *t = tooltip_text();
        GVariant *v = g_variant_new("(s@a(iiay)ss)", "", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0), "懒猫设备接力", t);
        g_free(t);
        return v;
    }
    return NULL;
}

static void item_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer u) {
    // Left click opens the window; the host shows the dbusmenu on right click itself.
    if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate")) ui_present();
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable item_vtable = {item_call, item_get, NULL, {0}};

// ------------------------------------------------------------------ com.canonical.dbusmenu

enum { M_ROOT = 0, M_STATUS, M_CONTROL, M_SEP1, M_SHARING, M_CLIPBOARD, M_AUTOSTART, M_SEP2, M_SHOW, M_OPEN, M_SEP3, M_QUIT, M_CLIENT, M_COUNT };

static const char menu_xml[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' name='parentId' direction='in'/><arg type='i' name='recursionDepth' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/><arg type='u' name='revision' direction='out'/><arg type='(ia{sv}av)' name='layout' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' name='ids' direction='in'/><arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='a(ia{sv})' name='properties' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' name='id' direction='in'/><arg type='s' name='name' direction='in'/><arg type='v' name='value' direction='out'/></method>"
    "<method name='Event'><arg type='i' name='id' direction='in'/><arg type='s' name='eventId' direction='in'/><arg type='v' name='data' direction='in'/>"
    "<arg type='u' name='timestamp' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' name='events' direction='in'/><arg type='ai' name='idErrors' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' name='id' direction='in'/><arg type='b' name='needUpdate' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' name='ids' direction='in'/><arg type='ai' name='updatesNeeded' direction='out'/>"
    "<arg type='ai' name='idErrors' direction='out'/></method>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "</interface></node>";

static GVariant *item_props(int id) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    char *text = NULL;
    bool enabled = true, visible = true;
    int toggle = -1;  // -1: no toggle; 0/1: checkmark state
    switch (id) {
    case M_ROOT: g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu")); return g_variant_builder_end(&b);
    case M_STATUS: {
        StatusLevel lv;
        char *s = app_summary(&lv);
        text = app.hub_connected && !app.sharing_enabled ? g_strdup_printf("%s · 键鼠共享已暂停", s) : g_strdup(s);
        g_free(s);
        enabled = false;
        break;
    }
    case M_CONTROL:
        text = app_control_line();
        visible = text != NULL;
        enabled = false;
        break;
    case M_CLIENT: text = g_strdup("客户端运行"); toggle = app.device_enabled; enabled = app.hub_connected && app.device_ready && app.device_control; break;
    case M_SHARING: text = g_strdup("键鼠共享"); toggle = app.sharing_enabled; enabled = app_operational(); break;
    case M_CLIPBOARD: text = g_strdup("共享剪贴板"); toggle = app.clipboard_enabled; enabled = app_operational(); break;
    case M_AUTOSTART: text = g_strdup("开机自动启动"); toggle = autostart_enabled(); break;
    case M_SHOW: text = g_strdup("打开主窗口"); break;
    case M_OPEN: text = g_strdup("打开管理页面"); enabled = app.box_domain && *app.box_domain; break;
    case M_QUIT: text = g_strdup("退出懒猫设备接力"); break;
    default: g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator")); return g_variant_builder_end(&b);
    }
    g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(text ? text : ""));
    g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(enabled));
    g_variant_builder_add(&b, "{sv}", "visible", g_variant_new_boolean(visible));
    if (toggle >= 0) {
        g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
        g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(toggle));
    }
    g_free(text);
    return g_variant_builder_end(&b);
}

static GVariant *layout_node(int id, int depth) {
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    static const int order[] = {M_STATUS, M_CONTROL, M_SEP1, M_CLIENT, M_SHARING, M_CLIPBOARD, M_AUTOSTART, M_SEP2, M_SHOW, M_OPEN, M_SEP3, M_QUIT};
    if (id == M_ROOT && depth != 0)
        for (guint i = 0; i < G_N_ELEMENTS(order); i++) g_variant_builder_add(&kids, "v", layout_node(order[i], depth - 1));
    return g_variant_new("(i@a{sv}@av)", id, item_props(id), g_variant_builder_end(&kids));
}

static void on_menu_event(int id) {
    switch (id) {
    case M_CLIENT: app_set_enabled(!app.device_enabled); break;
    case M_SHARING: app_set_sharing(!app.sharing_enabled); break;
    case M_CLIPBOARD: net_send_clipboard_setting(!app.clipboard_enabled); break;
    case M_AUTOSTART: ui_toggle_autostart(); break;
    case M_SHOW: ui_present(); break;
    case M_OPEN: ui_open_management(); break;
    case M_QUIT: app_quit(); break;
    }
}

static GVariant *menu_get(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop, GError **err, gpointer u) {
    if (!strcmp(prop, "Version")) return g_variant_new_uint32(3);
    if (!strcmp(prop, "TextDirection")) return g_variant_new_string("ltr");
    if (!strcmp(prop, "Status")) return g_variant_new_string("normal");
    if (!strcmp(prop, "IconThemePath")) return g_variant_new_strv(NULL, 0);
    return NULL;
}

static void menu_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer u) {
    if (!strcmp(method, "GetLayout")) {
        gint32 parent, depth;
        g_variant_get(params, "(ii@as)", &parent, &depth, NULL);
        if (parent < 0 || parent >= M_COUNT) parent = M_ROOT;
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@(ia{sv}av))", menu_revision, layout_node(parent, depth)));
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariantIter *ids;
        g_variant_get(params, "(aias)", &ids, NULL);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        gint32 id;
        while (g_variant_iter_next(ids, "i", &id))
            if (id >= 0 && id < M_COUNT) g_variant_builder_add(&b, "(i@a{sv})", id, item_props(id));
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&b)));
    } else if (!strcmp(method, "GetProperty")) {
        gint32 id;
        const char *name;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant *props = item_props(id >= 0 && id < M_COUNT ? id : M_ROOT);
        GVariant *v = g_variant_lookup_value(props, name, NULL);
        g_variant_unref(g_variant_ref_sink(props));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v ? v : g_variant_new_string("")));
        if (v) g_variant_unref(v);
    } else if (!strcmp(method, "Event")) {
        gint32 id;
        const char *ev;
        g_variant_get(params, "(i&svu)", &id, &ev, NULL, NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
        if (!strcmp(ev, "clicked")) on_menu_event(id);
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter *events;
        g_variant_get(params, "(a(isvu))", &events);
        gint32 id;
        const char *ev;
        GVariant *data;
        guint32 ts;
        GArray *clicked = g_array_new(FALSE, FALSE, sizeof(gint32));
        while (g_variant_iter_next(events, "(i&svu)", &id, &ev, &data, &ts)) {
            if (!strcmp(ev, "clicked")) g_array_append_val(clicked, id);
            g_variant_unref(data);
        }
        g_variant_iter_free(events);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0)));
        for (guint i = 0; i < clicked->len; i++) on_menu_event(g_array_index(clicked, gint32, i));
        g_array_free(clicked, TRUE);
    } else if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        GVariant *empty = g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai@ai)", empty, g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0)));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

static const GDBusInterfaceVTable menu_vtable = {menu_call, menu_get, NULL, {0}};

// ------------------------------------------------------------------ registration

static void register_with_watcher(void) {
    if (!bus || !bus_name) return;
    g_dbus_connection_call(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", bus_name), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
    registered = true;
}

static void watcher_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer u) {
    register_with_watcher();
    app_log("托盘图标已注册");
}

static void watcher_vanished(GDBusConnection *c, const char *name, gpointer u) {
    if (registered) app_log("没有找到系统托盘（StatusNotifierWatcher），托盘图标不可用；关闭窗口后可从应用菜单重新打开");
    registered = false;
}

void tray_start(void) {
    GError *err = NULL;
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        app_log("无法连接会话总线，托盘图标不可用：%s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    GDBusNodeInfo *item_info = g_dbus_node_info_new_for_xml(item_xml, NULL);
    GDBusNodeInfo *menu_info = g_dbus_node_info_new_for_xml(menu_xml, NULL);
    item_reg = g_dbus_connection_register_object(bus, ITEM_PATH, item_info->interfaces[0], &item_vtable, NULL, NULL, NULL);
    menu_reg = g_dbus_connection_register_object(bus, MENU_PATH, menu_info->interfaces[0], &menu_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(item_info);
    g_dbus_node_info_unref(menu_info);
    bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
    name_own = g_bus_own_name_on_connection(bus, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    watcher_watch = g_bus_watch_name_on_connection(bus, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_appeared, watcher_vanished, NULL, NULL);
}

bool tray_available(void) { return registered; }

void tray_refresh(void) {
    if (!bus || !item_reg) return;
    // Only signal real changes: hosts refetch icon / menu on every signal.
    static char *last_key;
    StatusLevel lv;
    char *summary = app_summary(&lv);
    char *ctrl = app_control_line();
    char *key = g_strdup_printf("%d|%d|%d|%d|%d|%d|%d|%s|%s|%s", app.device_enabled, app.device_ready, app.hub_connected, app.sharing_enabled, app.clipboard_enabled, app.captured, app.controlled,
                                summary, ctrl ? ctrl : "", app.box_domain ? app.box_domain : "");
    g_free(summary);
    g_free(ctrl);
    if (g_strcmp0(key, last_key) == 0) {
        g_free(key);
        return;
    }
    g_free(last_key);
    last_key = key;
    g_dbus_connection_emit_signal(bus, NULL, ITEM_PATH, "org.kde.StatusNotifierItem", "NewIcon", NULL, NULL);
    g_dbus_connection_emit_signal(bus, NULL, ITEM_PATH, "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL);
    // Labels and check states changed: bump the revision so hosts refetch the menu.
    menu_revision++;
    g_dbus_connection_emit_signal(bus, NULL, MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated", g_variant_new("(ui)", menu_revision, 0), NULL);
}

void tray_menu_changed(void) {
    // Autostart is not part of the state key above; force a menu refresh.
    if (!bus || !menu_reg) return;
    menu_revision++;
    g_dbus_connection_emit_signal(bus, NULL, MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated", g_variant_new("(ui)", menu_revision, 0), NULL);
}
