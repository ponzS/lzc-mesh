// Drag-to-arrange layout widget. Same model as the web page: devices are drawn
// scaled to fit, and releasing a screen sends its new position to the hub,
// which snaps it and broadcasts the result to every device.
#include <math.h>

#include "app.h"
#include "net.h"
#include "ui.h"

typedef struct {
    double scale, ox, oy;
    int drag;          // index in app.devices, -1 when idle
    double drag_dx, drag_dy;  // pointer offset inside the dragged rect (widget px)
    double drag_x, drag_y;    // current top-left of the dragged rect (widget px)
} View;

static View *view_of(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), "view"); }

static void compute(View *v, int W, int H) {
    if (!app.devices->len) return;
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (guint i = 0; i < app.devices->len; i++) {
        MeshDevice *d = g_ptr_array_index(app.devices, i);
        x0 = fmin(x0, d->x); y0 = fmin(y0, d->y);
        x1 = fmax(x1, d->x + d->w); y1 = fmax(y1, d->y + d->h);
    }
    double pad = 24;
    v->scale = fmin(fmin((W - 2 * pad) / ((x1 - x0) * 1.6), (H - 2 * pad) / ((y1 - y0) * 1.6)), 0.25);
    v->ox = pad + ((W - 2 * pad) - (x1 - x0) * v->scale) / 2 - x0 * v->scale;
    v->oy = pad + ((H - 2 * pad) - (y1 - y0) * v->scale) / 2 - y0 * v->scale;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static const char *platform_label(const char *p) {
    if (!g_strcmp0(p, "macos")) return "macOS";
    if (!g_strcmp0(p, "windows")) return "Windows";
    if (!g_strcmp0(p, "linux")) return "Linux";
    return p;
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int W, int H, gpointer data) {
    View *v = view_of(GTK_WIDGET(area));
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    for (int x = 8; x < W; x += 16)
        for (int y = 8; y < H; y += 16) cairo_rectangle(cr, x, y, 1, 1);
    cairo_fill(cr);
    PangoLayout *pl = gtk_widget_create_pango_layout(GTK_WIDGET(area), NULL);
    if (!app.devices->len) {
        pango_layout_set_text(pl, app.hub_connected ? "暂无设备" : "连接 LZC Mesh 服务后显示屏幕布局", -1);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_move_to(cr, (W - tw) / 2.0, (H - th) / 2.0);
        pango_cairo_show_layout(cr, pl);
        g_object_unref(pl);
        return;
    }
    if (v->drag < 0) compute(v, W, H);
    for (guint i = 0; i < app.devices->len; i++) {
        MeshDevice *d = g_ptr_array_index(app.devices, i);
        double x = v->ox + d->x * v->scale, y = v->oy + d->y * v->scale;
        if ((int)i == v->drag) x = v->drag_x, y = v->drag_y;
        double w = d->w * v->scale, h = d->h * v->scale;
        bool me = !g_strcmp0(d->id, app.device_id);
        bool active = !g_strcmp0(d->id, app.active_source) || !g_strcmp0(d->id, app.active_target);
        rounded(cr, x, y, w, h, 5);
        cairo_set_source_rgb(cr, me ? 0.93 : 0.99, me ? 0.93 : 0.99, me ? 0.93 : 0.99);
        cairo_fill_preserve(cr);
        double g = d->online ? 0.45 : 0.75;
        cairo_set_source_rgb(cr, active ? 0.07 : g, active ? 0.07 : g, active ? 0.07 : g);
        cairo_set_line_width(cr, active ? 2.5 : 1.5);
        if (!d->online) cairo_set_dash(cr, (double[]){4, 3}, 2, 0);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        char *markup = g_markup_printf_escaped("<b>%s</b>%s\n<span size='small' foreground='#777'>%s · %.0f×%.0f%s</span>", d->name,
                                               me ? "（本机）" : "", platform_label(d->platform), d->w, d->h, d->online ? "" : " · 离线");
        pango_layout_set_markup(pl, markup, -1);
        g_free(markup);
        pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
        pango_layout_set_width(pl, (int)(MAX(10, w - 8) * PANGO_SCALE));
        pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        cairo_set_source_rgb(cr, d->online ? 0.07 : 0.5, d->online ? 0.07 : 0.5, d->online ? 0.07 : 0.5);
        cairo_move_to(cr, x + 4, y + (h - th) / 2);
        pango_cairo_show_layout(cr, pl);
    }
    g_object_unref(pl);
}

static int hit(View *v, double px, double py) {
    for (int i = (int)app.devices->len - 1; i >= 0; i--) {
        MeshDevice *d = g_ptr_array_index(app.devices, i);
        double x = v->ox + d->x * v->scale, y = v->oy + d->y * v->scale;
        if (px >= x && py >= y && px <= x + d->w * v->scale && py <= y + d->h * v->scale) return i;
    }
    return -1;
}

static void drag_begin(GtkGestureDrag *g, double sx, double sy, GtkWidget *w) {
    View *v = view_of(w);
    v->drag = app.hub_connected ? hit(v, sx, sy) : -1;
    if (v->drag < 0) return;
    MeshDevice *d = g_ptr_array_index(app.devices, v->drag);
    v->drag_x = v->ox + d->x * v->scale;
    v->drag_y = v->oy + d->y * v->scale;
    v->drag_dx = sx - v->drag_x;
    v->drag_dy = sy - v->drag_y;
}

static void drag_update(GtkGestureDrag *g, double ox, double oy, GtkWidget *w) {
    View *v = view_of(w);
    if (v->drag < 0) return;
    double sx, sy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    v->drag_x = sx + ox - v->drag_dx;
    v->drag_y = sy + oy - v->drag_dy;
    gtk_widget_queue_draw(w);
}

static void drag_end(GtkGestureDrag *g, double ox, double oy, GtkWidget *w) {
    View *v = view_of(w);
    if (v->drag < 0) return;
    if ((guint)v->drag < app.devices->len && (fabs(ox) > 2 || fabs(oy) > 2)) {
        MeshDevice *d = g_ptr_array_index(app.devices, v->drag);
        net_send_layout(d->id, (v->drag_x - v->ox) / v->scale, (v->drag_y - v->oy) / v->scale);
    }
    v->drag = -1;
    gtk_widget_queue_draw(w);
}

GtkWidget *layout_view_new(void) {
    GtkWidget *area = gtk_drawing_area_new();
    View *v = g_new0(View, 1);
    v->drag = -1;
    g_object_set_data_full(G_OBJECT(area), "view", v, g_free);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
    gtk_widget_set_size_request(area, 480, 220);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), area);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), area);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), area);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
    return area;
}

void layout_view_refresh(GtkWidget *view) {
    if (view && view_of(view)->drag < 0) gtk_widget_queue_draw(view);
}
