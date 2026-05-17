#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <stdlib.h>

/*
 * AscentOS System Dashboard v3.0 (GTK 3.0 Port)
 * Featuring Cairo-based rendering and modern GTK3 widgets.
 */

// Custom syscall for uptime
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

    // 3. Update Resource History
    state->history_idx = (state->history_idx + 1) % 50;
    state->cpu_history[state->history_idx] = 20.0 + (sin(state->time * 0.5) * 10.0) + (rand() % 5);
    state->mem_history[state->history_idx] = 45.0 + (cos(state->time * 0.3) * 5.0);

    // 4. Trigger redraws
    gtk_widget_queue_draw(state->draw_area);
    gtk_widget_queue_draw(state->resource_area);

    return TRUE;
}

// Visualizer Drawing (GTK3 Cairo)
static gboolean on_draw_visualizer(GtkWidget *widget, cairo_t *cr, gpointer data) {
    AppState *state = (AppState *)data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    int cx = w / 2;
    int cy = h / 2;

    // Dark Background
    cairo_set_source_rgb(cr, 0.03, 0.03, 0.05);
    cairo_paint(cr);

    if (state->show_grid) {
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.15);
        cairo_set_line_width(cr, 1.0);
        for (int i = 0; i < w; i += 30) {
            cairo_move_to(cr, i, 0);
            cairo_line_to(cr, i, h);
        }
        for (int j = 0; j < h; j += 30) {
            cairo_move_to(cr, 0, j);
            cairo_line_to(cr, w, j);
        }
        cairo_stroke(cr);
    }

    // Glowy Orbits
    for (int r = 0; r < 3; r++) {
        if (r == 0) cairo_set_source_rgb(cr, 1.0, 0.2, 0.2);
        else if (r == 1) cairo_set_source_rgb(cr, 0.2, 1.0, 0.2);
        else cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);

        double angle = state->time * (r + 1) * 0.5;
        int radius = 40 + r * 30;
        
        cairo_set_line_width(cr, 2.0);
        cairo_arc(cr, cx, cy, radius, 0, 2 * M_PI);
        cairo_stroke(cr);

        int sx = cx + (int)(cos(angle) * radius);
        int sy = cy + (int)(sin(angle) * radius);
        cairo_arc(cr, sx, sy, 5, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    return FALSE;
}

// Resource Graphs (GTK3 Cairo)
static gboolean on_draw_resources(GtkWidget *widget, cairo_t *cr, gpointer data) {
    AppState *state = (AppState *)data;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    // Background
    cairo_set_source_rgb(cr, 0.02, 0.02, 0.02);
    cairo_paint(cr);

    cairo_set_line_width(cr, 2.0);

    // Draw CPU Graph (Green)
    cairo_set_source_rgb(cr, 0.1, 0.9, 0.1);
    for (int i = 0; i < 49; i++) {
        int idx1 = (state->history_idx + i + 1) % 50;
        int idx2 = (state->history_idx + i + 2) % 50;
        double x1 = (i * (double)w) / 50.0;
        double x2 = ((i + 1) * (double)w) / 50.0;
        double y1 = h - (state->cpu_history[idx1] * (h / 100.0));
        double y2 = h - (state->cpu_history[idx2] * (h / 100.0));
        if (i == 0) cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
    }
    cairo_stroke(cr);

    // Draw MEM Graph (Cyan)
    cairo_set_source_rgb(cr, 0.1, 0.6, 1.0);
    for (int i = 0; i < 49; i++) {
        int idx1 = (state->history_idx + i + 1) % 50;
        int idx2 = (state->history_idx + i + 2) % 50;
        double x1 = (i * (double)w) / 50.0;
        double x2 = ((i + 1) * (double)w) / 50.0;
        double y1 = h - (state->mem_history[idx1] * (h / 100.0));
        double y2 = h - (state->mem_history[idx2] * (h / 100.0));
        if (i == 0) cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
    }
    cairo_stroke(cr);

    return FALSE;
}

int main(int argc, char *argv[]) {
    static AppState state;
    memset(&state, 0, sizeof(state));
    state.show_grid = TRUE;

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "AscentOS Dashboard (GTK3)");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 500);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 12);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    // Header
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_box, FALSE, FALSE, 0);

    GtkWidget *logo_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(logo_label), "<span weight='bold' size='x-large' foreground='#44AAFF'>ASCENT</span><span weight='bold' size='x-large'>OS</span>");
    gtk_box_pack_start(GTK_BOX(header_box), logo_label, FALSE, FALSE, 0);

    state.uptime_label = gtk_label_new("System Uptime: 00:00:00");
    gtk_box_pack_end(GTK_BOX(header_box), state.uptime_label, FALSE, FALSE, 0);

    // Notebook
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(main_vbox), notebook, TRUE, TRUE, 0);

    // Tab 1: Visualizer
    state.draw_area = gtk_drawing_area_new();
    g_signal_connect(state.draw_area, "draw", G_CALLBACK(on_draw_visualizer), &state);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), state.draw_area, gtk_label_new("Visualizer"));

    // Tab 2: System Info
    GtkWidget *info_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(info_vbox), 25);
    struct utsname un;
    if (uname(&un) == 0) {
        char info_str[1024];
        snprintf(info_str, sizeof(info_str), 
            "<span size='large' weight='bold'>System Specifications</span>\n\n"
            "<b>Kernel:</b> %s\n"
            "<b>Node:</b> %s\n"
            "<b>Release:</b> %s\n"
            "<b>Version:</b> %s\n"
            "<b>Machine:</b> %s\n"
            "<b>OS Type:</b> AscentOS / Musl / GTK3",
            un.sysname, un.nodename, un.release, un.version, un.machine);
        GtkWidget *info_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(info_label), info_str);
        gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
        gtk_box_pack_start(GTK_BOX(info_vbox), info_label, FALSE, FALSE, 0);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), info_vbox, gtk_label_new("System Info"));

    // Tab 3: Resources
    GtkWidget *res_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    state.resource_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(state.resource_area, -1, 240);
    g_signal_connect(state.resource_area, "draw", G_CALLBACK(on_draw_resources), &state);
    gtk_box_pack_start(GTK_BOX(res_vbox), state.resource_area, TRUE, TRUE, 0);
    GtkWidget *legend = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(legend), "<span foreground='#22EE22'>● CPU Usage</span>     <span foreground='#44AAFF'>● Memory Usage</span>");
    gtk_box_pack_start(GTK_BOX(res_vbox), legend, FALSE, FALSE, 8);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), res_vbox, gtk_label_new("Resources"));

    // Footer
    state.progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(main_vbox), state.progress, FALSE, FALSE, 0);

    GtkWidget *footer_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *quit_btn = gtk_button_new_with_label("Shutdown Dashboard");
    gtk_style_context_add_class(gtk_widget_get_style_context(quit_btn), "destructive-action");
    g_signal_connect(quit_btn, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    gtk_box_pack_end(GTK_BOX(footer_hbox), quit_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), footer_hbox, FALSE, FALSE, 0);

    g_timeout_add(50, on_timer, &state);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
