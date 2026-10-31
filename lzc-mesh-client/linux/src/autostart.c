#include <gio/gio.h>
#include <errno.h>
#include <glib/gstdio.h>

#include "autostart.h"

static char *entry_path(void) { return g_build_filename(g_get_user_config_dir(), "autostart", "lzc-mesh.desktop", NULL); }

bool autostart_enabled(void) {
    char *p = entry_path();
    bool on = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return on;
}

bool autostart_set(bool on, char **error) {
    char *p = entry_path();
    bool ok = true;
    if (!on) {
        if (g_file_test(p, G_FILE_TEST_EXISTS) && g_unlink(p) != 0) {
            ok = false;
            if (error) *error = g_strdup(g_strerror(errno));
        }
    } else {
        // Point at the binary that is running now, so a package install (/usr/bin) and a
        // per-user install (~/.local/bin) both work.
        char *exe = g_file_read_link("/proc/self/exe", NULL);
        char *dir = g_path_get_dirname(p);
        g_mkdir_with_parents(dir, 0700);
        char *content = g_strdup_printf(
            "[Desktop Entry]\nType=Application\nName=懒猫设备接力\nName[en]=LazyCat Device Relay\n"
            "Exec=\"%s\" --background\nIcon=lzc-mesh\nTerminal=false\nX-GNOME-Autostart-enabled=true\n", exe ? exe : "lzc-mesh");
        GError *err = NULL;
        ok = g_file_set_contents(p, content, -1, &err);
        if (!ok && error) *error = g_strdup(err->message);
        g_clear_error(&err);
        g_free(content);
        g_free(dir);
        g_free(exe);
    }
    g_free(p);
    return ok;
}
