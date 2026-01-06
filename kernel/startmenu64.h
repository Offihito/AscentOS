#ifndef STARTMENU64_H
#define STARTMENU64_H

#include <stdint.h>
#include <stdbool.h>
#include "gui64.h"

#define START_MENU_WIDTH  520
#define START_MENU_HEIGHT 500
#define START_MENU_ITEM_HEIGHT 44
#define MAX_START_MENU_ITEMS 16

typedef enum {
    ITEM_TYPE_PROGRAM,
    ITEM_TYPE_SEPARATOR,
    ITEM_TYPE_SHUTDOWN,
    ITEM_TYPE_RESTART,
    ITEM_TYPE_SETTINGS
} StartMenuItemType;

typedef struct {
    bool active;
    bool visible;
    char label[64];
    char icon_text[8];
    StartMenuItemType type;
    int item_id;
    bool hovered;
} StartMenuItem;

typedef struct {
    int x, y;
    bool visible;
    StartMenuItem items[MAX_START_MENU_ITEMS];
    int item_count;
    int hovered_item;
} StartMenu;

// Fonksiyonlar
void startmenu_init(StartMenu* menu);
void startmenu_draw(const StartMenu* menu);
void startmenu_show(StartMenu* menu, int taskbar_y);
void startmenu_hide(StartMenu* menu);
void startmenu_toggle(StartMenu* menu, int taskbar_y);
void startmenu_handle_mouse_move(StartMenu* menu, int mouse_x, int mouse_y);
int startmenu_handle_mouse_click(StartMenu* menu, int mouse_x, int mouse_y);

#endif