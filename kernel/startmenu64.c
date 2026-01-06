// ============================================================================
// startmenu64.c - Classic Windows 7 Aero Style Start Menu (Çalışan Versiyon)
// ============================================================================
#include "startmenu64.h"
#include "gui64.h"
#include "accounts64.h"
#include <stddef.h>

// Windows 7 Aero Renkleri
#define MENU_BG             RGB(245, 248, 255)
#define MENU_GLASS_OVERLAY  RGB(255, 255, 255)
#define MENU_BORDER         RGB(90, 130, 190)
#define MENU_SHADOW         RGB(0, 0, 0)

#define LEFT_PANEL_BG_TOP   RGB(230, 240, 255)
#define LEFT_PANEL_BG_BOTTOM RGB(200, 220, 250)

#define ITEM_HOVER_TOP      RGB(185, 215, 255)
#define ITEM_HOVER_BOTTOM   RGB(160, 200, 250)
#define ITEM_HOVER_BORDER   RGB(100, 160, 220)

#define TEXT_COLOR          RGB(20, 20, 40)
#define TEXT_COLOR_HOVER    RGB(0, 0, 0)

static void str_copy(char* dest, const char* src, size_t max_len) {
    size_t i = 0;
    while (i < max_len - 1 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void draw_gradient_rect(int x, int y, int w, int h, Color top, Color bottom) {
    uint8_t r1 = GET_RED(top), g1 = GET_GREEN(top), b1 = GET_BLUE(top);
    uint8_t r2 = GET_RED(bottom), g2 = GET_GREEN(bottom), b2 = GET_BLUE(bottom);

    for (int py = 0; py < h; py++) {
        float ratio = (float)py / h;
        ratio = ratio * ratio * (3.0f - 2.0f * ratio);

        uint8_t r = r1 + (uint8_t)((r2 - r1) * ratio);
        uint8_t g = g1 + (uint8_t)((g2 - g1) * ratio);
        uint8_t b = b1 + (uint8_t)((b2 - b1) * ratio);
        Color col = RGB(r, g, b);

        for (int px = 0; px < w; px++) {
            gui_put_pixel(x + px, y + py, col);
        }
    }
}

static void draw_glass_overlay(int x, int y, int w, int h) {
    for (int py = 0; py < h / 3; py++) {
        uint8_t alpha = 90 - py * 3;
        if (alpha > 255) alpha = 0;
        for (int px = 0; px < w; px++) {
            Color base = gui_get_pixel(x + px, y + py);
            Color overlay = gui_blend_colors(MENU_GLASS_OVERLAY, base, alpha);
            gui_put_pixel(x + px, y + py, overlay);
        }
    }
}

static void draw_shadow(int x, int y, int w, int h) {
    int size = 10;
    for (int sy = 0; sy < size; sy++) {
        float alpha_f = (1.0f - (float)sy / size) * 0.4f;
        uint8_t alpha = (uint8_t)(alpha_f * 255);
        for (int sx = -size; sx < w + size; sx++) {
            int px = x + sx;
            int py = y + h + sy;
            if (gui_is_valid_coord(px, py)) {
                Color base = gui_get_pixel(px, py);
                Color shadow = gui_blend_colors(MENU_SHADOW, base, alpha);
                gui_put_pixel(px, py, shadow);
            }
        }
    }
}

// gui_draw_rect yerine dört çizgiyle çerçeve
static void draw_border(int x, int y, int w, int h, Color color) {
    gui_draw_line(x, y, x + w - 1, y, color);           // üst
    gui_draw_line(x, y, x, y + h - 1, color);           // sol
    gui_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color); // sağ
    gui_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color); // alt
}

static void draw_menu_item(int x, int y, int w, int h, const StartMenuItem* item) {
    if (item->type == ITEM_TYPE_SEPARATOR) {
        int ly = y + h / 2;
        for (int px = x + 20; px < x + w - 20; px++) {
            gui_put_pixel(px, ly, RGB(180, 190, 210));
            gui_put_pixel(px, ly + 1, RGB(255, 255, 255));
        }
        return;
    }

    if (item->hovered) {
        draw_gradient_rect(x, y, w, h, ITEM_HOVER_TOP, ITEM_HOVER_BOTTOM);
        draw_border(x, y, w, h, ITEM_HOVER_BORDER);
    }

    // İkon
    int ix = x + 12;
    int iy = y + (h - 32) / 2;
    Color icon_bg = (item->type == ITEM_TYPE_SHUTDOWN || item->type == ITEM_TYPE_RESTART)
                    ? RGB(200, 70, 70) : RGB(70, 130, 220);
    gui_fill_rect(ix, iy, 32, 32, icon_bg);
    gui_draw_string(ix + 9, iy + 9, item->icon_text, RGB(255,255,255), 0);

    // Metin
    Color text_col = item->hovered ? TEXT_COLOR_HOVER : TEXT_COLOR;
    gui_draw_string(x + 56, y + (h - 8)/2, item->label, text_col, 0);
}

void startmenu_init(StartMenu* menu) {
    menu->visible = false;
    menu->hovered_item = -1;
    menu->item_count = 0;

    int i = 0;
    menu->items[i].active = menu->items[i].visible = true;
    str_copy(menu->items[i].label, "Terminal", 64);
    str_copy(menu->items[i].icon_text, ">", 8);
    menu->items[i].type = ITEM_TYPE_PROGRAM;
    menu->items[i].item_id = 0;
    menu->items[i++].hovered = false;

    menu->items[i].active = menu->items[i].visible = true;
    str_copy(menu->items[i].label, "Settings", 64);
    str_copy(menu->items[i].icon_text, "*", 8);
    menu->items[i].type = ITEM_TYPE_SETTINGS;
    menu->items[i].item_id = 1;
    menu->items[i++].hovered = false;

    menu->items[i].type = ITEM_TYPE_SEPARATOR;
    menu->items[i].active = menu->items[i].visible = true;
    menu->items[i++].hovered = false;

    menu->items[i].active = menu->items[i].visible = true;
    str_copy(menu->items[i].label, "Shut down", 64);
    str_copy(menu->items[i].icon_text, "X", 8);
    menu->items[i].type = ITEM_TYPE_SHUTDOWN;
    menu->items[i].item_id = 100;
    menu->items[i++].hovered = false;

    menu->items[i].active = menu->items[i].visible = true;
    str_copy(menu->items[i].label, "Restart", 64);
    str_copy(menu->items[i].icon_text, "R", 8);
    menu->items[i].type = ITEM_TYPE_RESTART;
    menu->items[i].item_id = 101;
    menu->items[i++].hovered = false;

    menu->item_count = i;
}

void startmenu_draw(const StartMenu* menu) {
    if (!menu->visible) return;

    int x = menu->x;
    int y = menu->y;
    int w = START_MENU_WIDTH;
    int h = START_MENU_HEIGHT;

    draw_shadow(x, y, w, h);
    gui_fill_rect(x, y, w, h, MENU_BG);
    draw_glass_overlay(x, y, w, h);
    draw_border(x, y, w, h, MENU_BORDER);

    // Sol panel
    int left_w = 300;
    draw_gradient_rect(x, y, left_w, h, LEFT_PANEL_BG_TOP, LEFT_PANEL_BG_BOTTOM);

    // Sağ panel
    int right_x = x + left_w;
    gui_fill_rect(right_x, y, w - left_w, h, MENU_BG);

    // Kullanıcı alanı
    int user_h = 90;
    gui_fill_rect(right_x, y, w - left_w, user_h, RGB(240, 245, 255));

    // Kullanıcı ikonu (daire)
    int ux = right_x + 20;
    int uy = y + 20;
    for (int dy = 0; dy < 50; dy++) {
        for (int dx = 0; dx < 50; dx++) {
            int cx = dx - 25;
            int cy = dy - 25;
            if (cx*cx + cy*cy <= 625) {
                gui_put_pixel(ux + dx, uy + dy, RGB(80, 140, 220));
            }
        }
    }

    // Kullanıcı adı
    const char* username = accounts_get_current_username();
    gui_draw_string(ux + 60, uy + 15, username, TEXT_COLOR, 0);

    // Itemler
    int item_y = y + 20;
    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i].active || !menu->items[i].visible) continue;
        int item_h = (menu->items[i].type == ITEM_TYPE_SEPARATOR) ? 16 : START_MENU_ITEM_HEIGHT;
        draw_menu_item(x + 10, item_y, left_w - 20, item_h, &menu->items[i]);
        item_y += item_h + 8;
    }
}

void startmenu_show(StartMenu* menu, int taskbar_y) {
    menu->x = 6;
    menu->y = taskbar_y - START_MENU_HEIGHT - 5;
    menu->visible = true;
}

void startmenu_hide(StartMenu* menu) {
    menu->visible = false;
    for (int i = 0; i < menu->item_count; i++) menu->items[i].hovered = false;
}

void startmenu_toggle(StartMenu* menu, int taskbar_y) {
    if (menu->visible) startmenu_hide(menu);
    else startmenu_show(menu, taskbar_y);
}

void startmenu_handle_mouse_move(StartMenu* menu, int mx, int my) {
    if (!menu->visible) return;

    int x = menu->x, y = menu->y, w = START_MENU_WIDTH, h = START_MENU_HEIGHT;
    if (mx < x || mx >= x + w || my < y || my >= y + h) {
        for (int i = 0; i < menu->item_count; i++) menu->items[i].hovered = false;
        return;
    }

    int left_w = 300;
    int item_y = y + 20;
    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i].active || !menu->items[i].visible) continue;
        int item_h = (menu->items[i].type == ITEM_TYPE_SEPARATOR) ? 16 : START_MENU_ITEM_HEIGHT;

        bool hover = (mx >= x + 10 && mx < x + left_w - 10 &&
                      my >= item_y && my < item_y + item_h &&
                      menu->items[i].type != ITEM_TYPE_SEPARATOR);

        menu->items[i].hovered = hover;
        item_y += item_h + 8;
    }
}

int startmenu_handle_mouse_click(StartMenu* menu, int mx, int my) {
    if (!menu->visible) return -1;

    int x = menu->x, y = menu->y, w = START_MENU_WIDTH, h = START_MENU_HEIGHT;
    if (mx < x || mx >= x + w || my < y || my >= y + h) {
        startmenu_hide(menu);
        return -2;
    }

    int left_w = 300;
    int item_y = y + 20;
    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i].active || !menu->items[i].visible) continue;
        int item_h = (menu->items[i].type == ITEM_TYPE_SEPARATOR) ? 16 : START_MENU_ITEM_HEIGHT;

        if (mx >= x + 10 && mx < x + left_w - 10 &&
            my >= item_y && my < item_y + item_h &&
            menu->items[i].type != ITEM_TYPE_SEPARATOR) {

            startmenu_hide(menu);
            return menu->items[i].item_id;
        }
        item_y += item_h + 8;
    }

    startmenu_hide(menu);
    return -2;
}