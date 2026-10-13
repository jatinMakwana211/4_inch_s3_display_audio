#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_PAGE_ROWS 4
#define WIFI_CONNECT_TIMEOUT_MS 15000U
typedef enum {
    SETUP_IDLE, SETUP_PREPARING, SETUP_ASSOCIATING, SETUP_GETTING_IP,
    SETUP_SAVING, SETUP_READY, SETUP_FAILED
} setup_stage_t;
typedef struct {
    char ssid[33];
    int8_t rssi;
    int auth;
    bool open, supported;
} setup_network_t;

typedef struct {
    bool busy, connected, connecting, saved_available, using_saved_credentials;
    setup_stage_t stage;
    uint32_t connect_remaining_ms;
    char connecting_ssid[33];
    unsigned revision, connection_id, count;
    char message[64], ssid[33];
    char ip[16], mask[16], gateway[16], dns[2][48];
    int rssi;
    uint32_t connected_seconds;
    setup_network_t rows[WIFI_PAGE_ROWS];
    unsigned row_count;
} setup_snapshot_t;

esp_err_t wifi_setup_start(void);
esp_err_t wifi_setup_scan(void);
esp_err_t wifi_setup_connect(const setup_network_t *network, const char *password);
// Uses credentials loaded from NVS; ESP_ERR_NOT_FOUND means show configuration.
esp_err_t wifi_setup_connect_saved(void);
esp_err_t wifi_setup_disconnect(void);
esp_err_t wifi_setup_refresh(void);
void wifi_setup_snapshot(unsigned offset, setup_snapshot_t *out);
