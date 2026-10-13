#pragma once
#include "wifi_setup.h"
#include "agent_mode.h"

typedef enum { UI_WELCOME, UI_CONFIG, UI_PASSWORD, UI_CONNECTED, UI_CONNECTING, UI_SETTINGS, UI_AGENT } ui_screen_t;
typedef enum {
    UI_NONE, UI_OPEN, UI_BACK, UI_SCAN, UI_SELECT, UI_PREVIOUS, UI_NEXT,
    UI_EDIT, UI_REVEAL, UI_CONNECT, UI_KEY, UI_DELETE, UI_CAPS,
    UI_SYMBOLS, UI_SPACE, UI_DONE, UI_CONFIGURE, UI_REFRESH, UI_DISCONNECT, UI_CHOOSE,
    UI_SETTINGS_OPEN, UI_AGENT_TOGGLE
} ui_action_t;
typedef struct { ui_action_t action; int value; } ui_hit_t;
typedef struct {
    ui_screen_t screen;
    setup_snapshot_t wifi;
    setup_network_t selected;
    unsigned offset;
    char password[64], notice[64];
    bool show_password, caps;
    unsigned keyboard_page;
    ui_hit_t pressed;
    unsigned spinner_phase;
    bool agent_enabled;
    agent_snapshot_t agent;
} wifi_ui_view_t;
typedef struct {
    bool held, cancelled, releasing;
    int x, y;
    uint32_t began, released;
    ui_screen_t screen;
    ui_hit_t target;
} ui_touch_state_t;

void wifi_ui_render(uint16_t *pixels, const uint8_t *font, const uint16_t *background, const wifi_ui_view_t *view);
void wifi_ui_welcome(uint16_t *pixels, const uint8_t *font, const uint8_t *welcome, const wifi_ui_view_t *view);
ui_hit_t wifi_ui_hit(const wifi_ui_view_t *view, int x, int y);
ui_hit_t wifi_ui_touch(wifi_ui_view_t *view, ui_touch_state_t *touch, bool down, int x, int y, uint32_t ms);
void wifi_ui_edit(wifi_ui_view_t *view, ui_hit_t hit);
ui_screen_t wifi_ui_connection_screen(const setup_snapshot_t *wifi);
