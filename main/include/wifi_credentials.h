#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_CREDENTIALS_VERSION 1
typedef struct {
    uint32_t version;
    uint32_t auth;
    char ssid[33];
    char password[64];
} wifi_credentials_t;

bool wifi_credentials_valid(const wifi_credentials_t *credentials);
esp_err_t wifi_credentials_load(wifi_credentials_t *credentials);
// Called by the Wi-Fi worker only after DHCP confirms a successful connection.
esp_err_t wifi_credentials_save(const wifi_credentials_t *credentials);
