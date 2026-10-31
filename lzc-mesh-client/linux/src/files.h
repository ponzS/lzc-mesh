#pragma once
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
// Shared files panel (list, upload, download to ~/Downloads, delete).
GtkWidget *files_panel_new(GtkWidget *window);
void files_update(JsonArray *files);
