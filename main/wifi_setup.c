#include "wifi_setup.h"
#include "wifi_credentials.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum { REQUEST_SCAN, REQUEST_CONNECT, REQUEST_DISCONNECT, REQUEST_REFRESH };
enum { GOT_ADDRESS = BIT0, DISCONNECTED = BIT1 };
typedef struct {
    int kind;
    setup_network_t network;
    char password[64];
    bool from_saved;
    int64_t deadline_us;
} request_t;
static SemaphoreHandle_t lock;
static QueueHandle_t requests;
static EventGroupHandle_t events;
static setup_snapshot_t state;
static setup_network_t *networks;
static int64_t connected_at;
static int64_t attempt_deadline;
static bool accept_connection_events;
static wifi_credentials_t saved_credentials;
static esp_netif_t *netif;
static bool initialized;
static esp_event_handler_instance_t wifi_handler, ip_handler;

static void wipe(void *buffer, size_t size)
{
    volatile uint8_t *p = buffer;
    while (size--) *p++ = 0;
}

static void finish(const char *message)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    state.busy = false;
    state.connecting = false;
    snprintf(state.message, sizeof(state.message), "%s", message);
    ++state.revision;
    xSemaphoreGive(lock);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xSemaphoreTake(lock, portMAX_DELAY);
        if (state.connecting && accept_connection_events && esp_timer_get_time() < attempt_deadline) {
            state.stage = SETUP_GETTING_IP;
            ++state.revision;
        }
        xSemaphoreGive(lock);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(lock, portMAX_DELAY);
        state.connected = false;
        if (state.connecting && accept_connection_events) state.stage = SETUP_ASSOCIATING;
        if (!state.busy) snprintf(state.message, sizeof(state.message), "Disconnected. Select a network to retry.");
        ++state.revision;
        xSemaphoreGive(lock);
        xEventGroupSetBits(events, DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        xSemaphoreTake(lock, portMAX_DELAY);
        // A DHCP event arriving after the attempt timed out cannot reopen the
        // connected screen or cause unsuccessful credentials to be persisted.
        if (!accept_connection_events || (state.connecting && esp_timer_get_time() >= attempt_deadline)) {
            xSemaphoreGive(lock);
            return;
        }
        if (!state.connected) {
            connected_at = esp_timer_get_time();
            ++state.connection_id;
        }
        state.connected = true;
        if (state.connecting) state.stage = SETUP_SAVING;
        snprintf(state.ip, sizeof(state.ip), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(state.mask, sizeof(state.mask), IPSTR, IP2STR(&event->ip_info.netmask));
        snprintf(state.gateway, sizeof(state.gateway), IPSTR, IP2STR(&event->ip_info.gw));
        ++state.revision;
        xSemaphoreGive(lock);
        xEventGroupSetBits(events, GOT_ADDRESS);
    }
}

// Called only by the worker; no network operation blocks the touch task.
static esp_err_t initialize(void)
{
    if (initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) goto destroy_netif;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, &wifi_handler);
    if (err != ESP_OK) goto deinit;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, &ip_handler);
    if (err != ESP_OK) goto unregister_wifi;
    // The driver keeps trial credentials in RAM. We save our own NVS record
    // only after success, without overwriting audio settings or logging secrets.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) { initialized = true; return ESP_OK; }
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
unregister_wifi:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
deinit:
    esp_wifi_deinit();
destroy_netif:
    esp_netif_destroy_default_wifi(netif);
    netif = NULL;
    return err;
}

static bool supported(wifi_auth_mode_t mode)
{
    return mode == WIFI_AUTH_OPEN || mode == WIFI_AUTH_WPA_PSK ||
           mode == WIFI_AUTH_WPA2_PSK || mode == WIFI_AUTH_WPA_WPA2_PSK ||
           mode == WIFI_AUTH_WPA3_PSK || mode == WIFI_AUTH_WPA2_WPA3_PSK;
}

static esp_err_t scan(void)
{
    wifi_scan_config_t config = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) return err;
    uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (err != ESP_OK) { esp_wifi_clear_ap_list(); return err; }
    setup_network_t *found = count ? calloc(count, sizeof(*found)) : NULL;
    if (count && !found) { esp_wifi_clear_ap_list(); return ESP_ERR_NO_MEM; }
    unsigned used = 0;
    for (unsigned i = 0; i < count; ++i) {
        wifi_ap_record_t ap;
        err = esp_wifi_scan_get_ap_record(&ap);
        if (err != ESP_OK) break;
        if (!ap.ssid[0]) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < used; ++j) {
            if (found[j].auth == ap.authmode && !strncmp(found[j].ssid, (const char *)ap.ssid, 32)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue; // Results arrive strongest first.
        memcpy(found[used].ssid, ap.ssid, 32);
        found[used].rssi = ap.rssi;
        found[used].auth = ap.authmode;
        found[used].open = ap.authmode == WIFI_AUTH_OPEN;
        found[used].supported = supported(ap.authmode);
        ++used;
    }
    esp_wifi_clear_ap_list();
    if (err != ESP_OK) { free(found); return err; }
    xSemaphoreTake(lock, portMAX_DELAY);
    free(networks);
    networks = found;
    state.count = used;
    xSemaphoreGive(lock);
    finish(used ? "Select a network. 2.4 GHz Wi-Fi only." : "No networks found. Tap Scan to retry.");
    return ESP_OK;
}

static void refresh_details(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    esp_netif_dns_info_t dns[2] = {0};
    esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns[0]);
    esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns[1]);
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(state.ssid, ap.ssid, 32);
    state.ssid[32] = 0;
    state.rssi = ap.rssi;
    for (unsigned i = 0; i < 2; ++i) {
        if (dns[i].ip.type == ESP_IPADDR_TYPE_V4 && dns[i].ip.u_addr.ip4.addr) {
            snprintf(state.dns[i], sizeof(state.dns[i]), IPSTR, IP2STR(&dns[i].ip.u_addr.ip4));
        } else {
            snprintf(state.dns[i], sizeof(state.dns[i]), "Not assigned");
        }
    }
    ++state.revision;
    xSemaphoreGive(lock);
}

static void disconnect_station(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    accept_connection_events = false;
    state.connected = false;
    xSemaphoreGive(lock);
    xEventGroupClearBits(events, DISCONNECTED | GOT_ADDRESS);
    if (esp_wifi_disconnect() == ESP_OK) {
        xEventGroupWaitBits(events, DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
    }
    xEventGroupClearBits(events, DISCONNECTED | GOT_ADDRESS);
}

static esp_err_t connect_station(const request_t *request)
{
    disconnect_station();
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, request->network.ssid, strnlen(request->network.ssid, 32));
    if (!request->network.open) memcpy(config.sta.password, request->password, strlen(request->password));
    config.sta.threshold.authmode = request->network.auth;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe(&config, sizeof(config));
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    accept_connection_events = true;
    state.stage = SETUP_ASSOCIATING;
    ++state.revision;
    xSemaphoreGive(lock);

    // A single wall-clock deadline covers initialization, association, retries
    // and DHCP. Disconnect events do not restart the 15-second timer.
    bool retry = true;
    while (esp_timer_get_time() < request->deadline_us) {
        if (retry) {
            err = esp_wifi_connect();
            if (err != ESP_OK) return err;
        }
        int64_t remaining_us = request->deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) break;
        TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (!ticks) ticks = 1;
        EventBits_t bits = xEventGroupWaitBits(events, GOT_ADDRESS | DISCONNECTED, pdTRUE, pdFALSE, ticks);
        xSemaphoreTake(lock, portMAX_DELAY);
        bool connected = state.connected;
        xSemaphoreGive(lock);
        if ((bits & GOT_ADDRESS) && connected) {
            refresh_details();
            wifi_credentials_t credentials = {.version = WIFI_CREDENTIALS_VERSION, .auth = request->network.auth};
            memcpy(credentials.ssid, request->network.ssid, sizeof(credentials.ssid));
            if (!request->network.open) memcpy(credentials.password, request->password, sizeof(credentials.password));
            esp_err_t saved = wifi_credentials_save(&credentials);
            xSemaphoreTake(lock, portMAX_DELAY);
            if (saved == ESP_OK) {
                saved_credentials = credentials;
                state.saved_available = true;
            }
            state.stage = state.connected ? SETUP_READY : SETUP_FAILED;
            xSemaphoreGive(lock);
            wipe(&credentials, sizeof(credentials));
            finish(saved == ESP_OK ? "Connected successfully" : "Connected, but credentials could not be saved.");
            if (saved != ESP_OK) ESP_LOGW("WIFI_SETUP", "NVS save failed: %s", esp_err_to_name(saved));
            return ESP_OK;
        }
        retry = (bits & DISCONNECTED) != 0;
        if (retry) {
            remaining_us = request->deadline_us - esp_timer_get_time();
            // Short, bounded pause prevents rapid authentication retry loops.
            if (remaining_us > 0) vTaskDelay(pdMS_TO_TICKS(remaining_us > 300000 ? 300 : (remaining_us + 999) / 1000));
        }
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    accept_connection_events = false;
    state.stage = SETUP_FAILED;
    xSemaphoreGive(lock);
    disconnect_station();
    finish("Could not connect in 15s. Choose a network.");
    return ESP_OK;
}

static void worker(void *arg)
{
    request_t request;
    for (;;) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = initialize();
        if (err == ESP_OK) {
            switch (request.kind) {
            case REQUEST_SCAN: err = scan(); break;
            case REQUEST_CONNECT: err = connect_station(&request); break;
            case REQUEST_DISCONNECT:
                disconnect_station();
                // Also discard the Wi-Fi driver's RAM copy of the password.
                { wifi_config_t empty = {0}; esp_wifi_set_config(WIFI_IF_STA, &empty); }
                finish("Disconnected. Select a network to connect.");
                break;
            case REQUEST_REFRESH: refresh_details(); finish("Network details refreshed"); break;
            }
        }
        if (err != ESP_OK) {
            if (request.kind == REQUEST_CONNECT) {
                if (initialized) disconnect_station();
                xSemaphoreTake(lock, portMAX_DELAY);
                state.stage = SETUP_FAILED;
                xSemaphoreGive(lock);
            }
            char message[64];
            snprintf(message, sizeof(message), "Wi-Fi error: %s. Please retry.", esp_err_to_name(err));
            finish(message);
        }
        wipe(&request, sizeof(request));
    }
}

esp_err_t wifi_setup_start(void)
{
    if (requests) return ESP_OK;
    // NVS is initialized by app_main. Missing/invalid data simply opens setup.
    esp_err_t loaded = wifi_credentials_load(&saved_credentials);
    state.saved_available = loaded == ESP_OK;
    if (loaded != ESP_OK && loaded != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW("WIFI_SETUP", "No usable saved Wi-Fi credentials: %s", esp_err_to_name(loaded));
    lock = xSemaphoreCreateMutex();
    events = xEventGroupCreate();
    requests = xQueueCreate(1, sizeof(request_t));
    if (!lock || !events || !requests ||
        xTaskCreate(worker, "wifi_setup", 6144, NULL, 4, NULL) != pdPASS) {
        if (requests) vQueueDelete(requests);
        if (events) vEventGroupDelete(events);
        if (lock) vSemaphoreDelete(lock);
        requests = NULL; events = NULL; lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t submit(request_t *request, const char *message)
{
    if (!requests) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state.busy) { xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE; }
    state.busy = true;
    if (request->kind == REQUEST_CONNECT) {
        request->deadline_us = esp_timer_get_time() + (int64_t)WIFI_CONNECT_TIMEOUT_MS * 1000;
        attempt_deadline = request->deadline_us;
        accept_connection_events = false;
        state.connected = false;
        state.connecting = true;
        state.using_saved_credentials = request->from_saved;
        state.stage = SETUP_PREPARING;
        snprintf(state.connecting_ssid, sizeof(state.connecting_ssid), "%s", request->network.ssid);
    }
    snprintf(state.message, sizeof(state.message), "%s", message);
    ++state.revision;
    bool sent = xQueueSend(requests, request, 0) == pdTRUE;
    if (!sent) { state.busy = false; state.connecting = false; }
    xSemaphoreGive(lock);
    return sent ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_setup_scan(void)
{
    request_t request = {.kind = REQUEST_SCAN};
    return submit(&request, "Scanning nearby 2.4 GHz networks...");
}
esp_err_t wifi_setup_connect(const setup_network_t *network, const char *password)
{
    if (!network || !network->ssid[0] || !password || !network->supported ||
        (!network->open && (strlen(password) < 8 || strlen(password) > 63))) return ESP_ERR_INVALID_ARG;
    request_t request = {.kind = REQUEST_CONNECT, .network = *network};
    if (!network->open) snprintf(request.password, sizeof(request.password), "%s", password);
    esp_err_t err = submit(&request, "Connecting... waiting for an IP address");
    wipe(&request, sizeof(request));
    return err;
}
esp_err_t wifi_setup_disconnect(void)
{
    request_t request = {.kind = REQUEST_DISCONNECT};
    return submit(&request, "Disconnecting...");
}
esp_err_t wifi_setup_connect_saved(void)
{
    if (!lock) return ESP_ERR_INVALID_STATE;
    request_t request = {.kind = REQUEST_CONNECT, .from_saved = true};
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!state.saved_available) { xSemaphoreGive(lock); return ESP_ERR_NOT_FOUND; }
    memcpy(request.network.ssid, saved_credentials.ssid, sizeof(request.network.ssid));
    request.network.auth = saved_credentials.auth;
    request.network.open = saved_credentials.auth == WIFI_AUTH_OPEN;
    request.network.supported = true;
    memcpy(request.password, saved_credentials.password, sizeof(request.password));
    xSemaphoreGive(lock);
    esp_err_t err = submit(&request, "Connecting to the saved network...");
    wipe(&request, sizeof(request));
    return err;
}
esp_err_t wifi_setup_refresh(void)
{
    request_t request = {.kind = REQUEST_REFRESH};
    return submit(&request, "Refreshing network details...");
}
void wifi_setup_snapshot(unsigned offset, setup_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = state;
    if (state.connecting) {
        int64_t remaining = attempt_deadline - esp_timer_get_time();
        out->connect_remaining_ms = remaining > 0 ? (remaining + 999) / 1000 : 0;
    }
    out->connected_seconds = state.connected ? (esp_timer_get_time() - connected_at) / 1000000 : 0;
    for (unsigned i = offset; i < state.count && out->row_count < WIFI_PAGE_ROWS; ++i)
        out->rows[out->row_count++] = networks[i];
    xSemaphoreGive(lock);
}
