#pragma once
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
// Shared files panel (list, upload, download to ~/Downloads, delete).
GtkWidget *files_panel_new(GtkWidget *window, GtkWidget **upload_button);
void files_update(JsonArray *files);
void files_set_active(bool on);
