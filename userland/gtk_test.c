#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/*
 * AscentOS System Dashboard v2.0
 * Now featuring real system data, multi-tabbed UI, and premium resource graphs.
 */

// Custom syscall for uptime since sysroot might not have it wrapped
#define SYS_UPTIME 399
static long get_uptime_ms() {
    long ms;
    __asm__ volatile("syscall" : "=a"(ms) : "a"(SYS_UPTIME) : "rcx", "r11", "memory");
    return ms;
}

typedef struct {
    double time;
    gboolean show_grid;
    GtkWidget *draw_area;
    GtkWidget *progress;
    GtkWidget *uptime_label;
    GtkWidget *resource_area;
    double cpu_history[50];
    double mem_history[50];
    int history_idx;
} AppState;

static gboolean on_timer(gpointer data) {
    AppState *state = (AppState *)data;
    state->time += 0.05;

    // 1. Update Uptime
    long ms = get_uptime_ms();
    char buf[64];
    int secs = ms / 1000;
    int mins = secs / 60;
    int hrs = mins / 60;
    snprintf(buf, sizeof(buf), "System Uptime: %02d:%02d:%02d", hrs, mins % 60, secs % 60);
    gtk_label_set_text(GTK_LABEL(state->uptime_label), buf);

    // 2. Update Progress
    double frac = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(state->progress));
    frac += 0.005;
    if (frac > 1.0) frac = 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), frac);

    // 3. Update Resource History (Simulated for visuals)
    state->history_idx = (state->history_idx + 1) % 50;
    state->cpu_history[state->history_idx] = 20.0 + (sin(state->time * 0.5) * 10.0) + (rand() % 5);
    state->mem_history[state->history_idx] = 45.0 + (cos(state->time * 0.3) * 5.0);

    // 4. Trigger redraws
    gtk_widget_queue_draw(state->draw_area);
    gtk_widget_queue_draw(state->resource_area);

    return TRUE;
}

// Visualizer Drawing (Tab 1)
static gboolean on_visualizer_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    AppState *state = (AppState *)data;
    GdkGC *gc = widget->style->fg_gc[GTK_WIDGET_STATE(widget)];
    GdkColor color;
    int w = widget->allocation.width;
    int h = widget->allocation.height;
    int cx = w / 2;
    int cy = h / 2;

    // Dark Background
    color.red = 0x0808; color.green = 0x0808; color.blue = 0x0C0C;
    gdk_gc_set_rgb_fg_color(gc, &color);
    gdk_draw_rectangle(widget->window, gc, TRUE, 0, 0, w, h);

    if (state->show_grid) {
        color.red = 0x1A1A; color.green = 0x1A1A; color.blue = 0x2A2A;
        gdk_gc_set_rgb_fg_color(gc, &color);
        for (int i = 0; i < w; i += 30) gdk_draw_line(widget->window, gc, i, 0, i, h);
        for (int j = 0; j < h; j += 30) gdk_draw_line(widget->window, gc, 0, j, w, j);
    }

    // Glowy Orbits
    for (int r = 0; r < 3; r++) {
        color.red = (r == 0) ? 0xFFFF : 0x4444;
        color.green = (r == 1) ? 0xFFFF : 0x4444;
        color.blue = (r == 2) ? 0xFFFF : 0xBBBB;
        gdk_gc_set_rgb_fg_color(gc, &color);

        double angle = state->time * (r + 1) * 0.5;
        int radius = 40 + r * 30;
        gdk_draw_arc(widget->window, gc, FALSE, cx - radius, cy - radius, radius * 2, radius * 2, 0, 360 * 64);
        int sx = cx + (int)(cos(angle) * radius);
        int sy = cy + (int)(sin(angle) * radius);
        gdk_draw_arc(widget->window, gc, TRUE, sx - 5, sy - 5, 10, 10, 0, 360 * 64);
    }
    return TRUE;
}

// Resource Graphs (Tab 3)
static gboolean on_resource_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    AppState *state = (AppState *)data;
    GdkGC *gc = widget->style->fg_gc[GTK_WIDGET_STATE(widget)];
    GdkColor color;
    int w = widget->allocation.width;
    int h = widget->allocation.height;

    // Dark grid background
    color.red = 0x0505; color.green = 0x0505; color.blue = 0x0505;
    gdk_gc_set_rgb_fg_color(gc, &color);
    gdk_draw_rectangle(widget->window, gc, TRUE, 0, 0, w, h);

    // Draw CPU Graph (Green)
    color.red = 0x2222; color.green = 0xEEEE; color.blue = 0x2222;
    gdk_gc_set_rgb_fg_color(gc, &color);
    for (int i = 0; i < 49; i++) {
        int idx1 = (state->history_idx + i + 1) % 50;
        int idx2 = (state->history_idx + i + 2) % 50;
        int x1 = (i * w) / 50;
        int x2 = ((i + 1) * w) / 50;
        int y1 = h - (int)(state->cpu_history[idx1] * (h / 100.0));
        int y2 = h - (int)(state->cpu_history[idx2] * (h / 100.0));
        gdk_draw_line(widget->window, gc, x1, y1, x2, y2);
    }

    // Draw MEM Graph (Cyan)
    color.red = 0x2222; color.green = 0xAAAA; color.blue = 0xFFFF;
    gdk_gc_set_rgb_fg_color(gc, &color);
    for (int i = 0; i < 49; i++) {
        int idx1 = (state->history_idx + i + 1) % 50;
        int idx2 = (state->history_idx + i + 2) % 50;
        int x1 = (i * w) / 50;
        int x2 = ((i + 1) * w) / 50;
        int y1 = h - (int)(state->mem_history[idx1] * (h / 100.0));
        int y2 = h - (int)(state->mem_history[idx2] * (h / 100.0));
        gdk_draw_line(widget->window, gc, x1, y1, x2, y2);
    }

    return TRUE;
}

static void on_destroy(GtkWidget *widget, gpointer data) { gtk_main_quit(); }

int main(int argc, char *argv[]) {
    static AppState state;
    memset(&state, 0, sizeof(state));
    state.show_grid = TRUE;

    if (!gtk_init_check(&argc, &argv)) return 1;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "AscentOS Dashboard");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 450);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkWidget *main_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 10);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    // Header
    GtkWidget *header_hbox = gtk_hbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_hbox, FALSE, FALSE, 0);

    GtkWidget *logo_label = gtk_label_new(" AscentOS ");
    PangoFontDescription *logo_font = pango_font_description_from_string("Sans Bold 18");
    gtk_widget_modify_font(logo_label, logo_font);
    pango_font_description_free(logo_font);
    gtk_box_pack_start(GTK_BOX(header_hbox), logo_label, FALSE, FALSE, 0);

    state.uptime_label = gtk_label_new("System Uptime: 00:00:00");
    gtk_box_pack_end(GTK_BOX(header_hbox), state.uptime_label, FALSE, FALSE, 0);

    // Notebook (Tabs)
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);

    // Tab 1: Visualizer
    GtkWidget *vis_vbox = gtk_vbox_new(FALSE, 5);
    state.draw_area = gtk_drawing_area_new();
    g_signal_connect(state.draw_area, "expose-event", G_CALLBACK(on_visualizer_expose), &state);
    gtk_box_pack_start(GTK_BOX(vis_vbox), state.draw_area, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vis_vbox, gtk_label_new("Visualizer"));

    // Tab 2: System Info
    GtkWidget *info_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(info_vbox), 20);
    struct utsname un;
    if (uname(&un) == 0) {
        char info_str[512];
        snprintf(info_str, sizeof(info_str), 
            "<b>Kernel Name:</b> %s\n"
            "<b>Node Name:</b> %s\n"
            "<b>Release:</b> %s\n"
            "<b>Version:</b> %s\n"
            "<b>Machine:</b> %s",
            un.sysname, un.nodename, un.release, un.version, un.machine);
        GtkWidget *info_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(info_label), info_str);
        gtk_box_pack_start(GTK_BOX(info_vbox), info_label, FALSE, FALSE, 0);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), info_vbox, gtk_label_new("System Info"));

    // Tab 3: Resource Monitor
    GtkWidget *res_vbox = gtk_vbox_new(FALSE, 5);
    state.resource_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(state.resource_area, -1, 200);
    g_signal_connect(state.resource_area, "expose-event", G_CALLBACK(on_resource_expose), &state);
    gtk_box_pack_start(GTK_BOX(res_vbox), state.resource_area, TRUE, TRUE, 0);
    GtkWidget *legend = gtk_label_new("<span color='#22EE22'>■ CPU Usage</span>   <span color='#22AAFF'>■ Memory Usage</span>");
    gtk_label_set_markup(GTK_LABEL(legend), gtk_label_get_text(GTK_LABEL(legend)));
    gtk_box_pack_start(GTK_BOX(res_vbox), legend, FALSE, FALSE, 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), res_vbox, gtk_label_new("Resources"));

    // Footer
    state.progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(main_vbox), state.progress, FALSE, FALSE, 0);

    GtkWidget *btn_box = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btn_box), GTK_BUTTONBOX_END);
    GtkWidget *quit_btn = gtk_button_new_with_label("Exit Dashboard");
    g_signal_connect(quit_btn, "clicked", G_CALLBACK(on_destroy), NULL);
    gtk_container_add(GTK_CONTAINER(btn_box), quit_btn);
    gtk_box_pack_start(GTK_BOX(main_vbox), btn_box, FALSE, FALSE, 0);

    g_timeout_add(50, on_timer, &state);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
