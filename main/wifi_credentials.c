#include "wifi_credentials.h"
#include <string.h>
#include "esp_wifi_types.h"
#include "nvs.h"

// One versioned blob keeps SSID, password and security mode together.
static const char *NAMESPACE = "wifi_setup";
static const char *KEY = "station";

bool wifi_credentials_valid(const wifi_credentials_t *c)
{
    if (!c || c->version != WIFI_CREDENTIALS_VERSION || !c->ssid[0] ||
        !memchr(c->ssid, 0, sizeof(c->ssid)) || !memchr(c->password, 0, sizeof(c->password))) return false;
    if (c->auth == WIFI_AUTH_OPEN) return c->password[0] == 0;
    if (c->auth != WIFI_AUTH_WPA_PSK && c->auth != WIFI_AUTH_WPA2_PSK &&
        c->auth != WIFI_AUTH_WPA_WPA2_PSK && c->auth != WIFI_AUTH_WPA3_PSK &&
        c->auth != WIFI_AUTH_WPA2_WPA3_PSK) return false;
    size_t length = strlen(c->password);
    return length >= 8 && length <= 63;
}

esp_err_t wifi_credentials_load(wifi_credentials_t *credentials)
{
    if (!credentials) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*credentials);
    err = nvs_get_blob(handle, KEY, credentials, &size);
    nvs_close(handle);
    if (err == ESP_OK && (size != sizeof(*credentials) || !wifi_credentials_valid(credentials))) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) memset(credentials, 0, sizeof(*credentials));
    return err;
}

esp_err_t wifi_credentials_save(const wifi_credentials_t *credentials)
{
    if (!wifi_credentials_valid(credentials)) return ESP_ERR_INVALID_ARG;
    // Do not rewrite flash on every successful reconnect to the same network.
    wifi_credentials_t previous;
    esp_err_t err = wifi_credentials_load(&previous);
    bool same = err == ESP_OK && previous.auth == credentials->auth &&
                !strcmp(previous.ssid, credentials->ssid) && !strcmp(previous.password, credentials->password);
    volatile unsigned char *p = (volatile unsigned char *)&previous;
    for (size_t i = 0; i < sizeof(previous); ++i) p[i] = 0;
    if (same) return ESP_OK;
    nvs_handle_t handle;
    err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, KEY, credentials, sizeof(*credentials));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
