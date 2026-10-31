#pragma once
#include <gtk/gtk.h>
void ui_build(GtkApplication *app);
void ui_present(void);
void ui_refresh_status(void);
void ui_refresh_layout(void);
void ui_append_log(const char *line);
GtkWidget *layout_view_new(void);
void layout_view_refresh(GtkWidget *view);
