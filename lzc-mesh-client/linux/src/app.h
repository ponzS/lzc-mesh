// Shared application state of the Linux client. Everything runs on the GLib
// main loop, so no locking is needed.
#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { ST_UNKNOWN, ST_OK, ST_WARN, ST_ERROR } StatusLevel;

typedef struct {
    StatusLevel level;
    char *text;
} Status;

enum {
    S_LZC_CLIENT,   // LazyCat desktop client running and local API reachable
    S_LZC_NETWORK,  // selected box status as reported by the LazyCat client
    S_LOGIN,        // session cookie for the box obtained
    S_SERVICE,      // LZC Mesh LPK reachable on the box
    S_HUB,          // WebSocket to the hub
    S_INPUT,        // Wayland input protocols / permissions
    S_CONTROL,      // current control role
    S_SCREEN,       // local screen geometry used in the layout
    S_CLIPBOARD,    // shared clipboard state
    S_TRANSPORT,   // active LAN/relay path
    S_POINTER_CHANNEL,
    S_KEYBOARD_CHANNEL,
    S_COUNT
};

typedef struct {
    char *id;
    char *name;
    char *platform;
    char *version;
    double x, y, w, h;
    bool online;
    bool sharing;
} MeshDevice;

typedef struct {
    // Config
    char *device_id;
    char *box_override;  // empty: use the LazyCat default box
    bool sharing_enabled;  // keyboard/mouse sharing switch (tray / header), persisted
    bool background;       // started with --background (login): no window

    // LazyCat discovery
    char *box_name;
    char *box_domain;
    char *login_user;
    char *cookie;

    // Hub state
    bool hub_connected;
    char *hub_user;
    GPtrArray *devices;  // MeshDevice*
    char *active_source, *active_target;
    bool swap_cmd_ctrl;
    char *transport_mode;
    bool clipboard_enabled;

    // Local input role
    bool captured;    // this device controls another one
    bool controlled;  // another device controls this one
    int screen_w, screen_h;

    Status status[S_COUNT];
} App;

extern App app;

void app_set_status(int which, StatusLevel level, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
void app_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void app_devices_changed(void);
void app_status_changed(void);
const char *app_effective_box(void);
// One-line summary for the header pill and the tray ("已连接 · 3 台设备在线", or the first problem).
char *app_summary(StatusLevel *level);
// "正在控制 X" / "X 正在控制本机", or NULL when idle.
char *app_control_line(void);
void app_set_sharing(bool on);
void app_quit(void);
