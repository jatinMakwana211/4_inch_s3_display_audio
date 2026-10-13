#pragma once
#include "esp_err.h"

// Blocking operations; call sequentially after mounting SPIFFS.
// Do not initialize the MP3 player's I2S output at the same time.
// INMP441: SCK=GPIO19, WS=GPIO47, SD=GPIO20, L/R=GND.
// Boot recorder runs before LCD startup. Agent Mode subsequently owns live I2S.
esp_err_t record_voice(void);
esp_err_t play_recorded_voice(int volume);
