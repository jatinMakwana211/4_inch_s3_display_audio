#pragma once
#include "esp_lcd_types.h"
#include "esp_err.h"
#include <stdint.h>
esp_err_t wifi_ui_start(esp_lcd_panel_handle_t panel, const uint8_t *welcome);
