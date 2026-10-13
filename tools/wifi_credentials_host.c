// Exercise the production persistence code with a controllable NVS boundary.
#include "wifi_credentials.h"
#include "esp_wifi_types.h"
#include "nvs.h"
#include <string.h>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)
static wifi_credentials_t stored, pending;
static size_t stored_size;
static bool exists;
static unsigned writes, commits, closes;
static esp_err_t open_error, read_error, write_error, commit_error;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    if (open_error) return open_error;
    if (mode == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < stored_size) { *size = stored_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, &stored, stored_size);
    *size = stored_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    if (write_error) return write_error;
    if (size != sizeof(pending)) return ESP_ERR_INVALID_ARG;
    memcpy(&pending, value, size);
    ++writes;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (commit_error) return commit_error;
    stored = pending;
    stored_size = sizeof(stored);
    exists = true;
    ++commits;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { ++closes; }

__declspec(dllexport) int credential_tests(void)
{
    wifi_credentials_t input = {.version=WIFI_CREDENTIALS_VERSION, .auth=WIFI_AUTH_WPA2_PSK};
    strcpy(input.ssid, "Test network");
    strcpy(input.password, "test-password");
    wifi_credentials_t output, empty = {0};
    memset(&output, 0xa5, sizeof(output));
    CHECK(wifi_credentials_load(&output) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(!memcmp(&output, &empty, sizeof(output)));
    CHECK(wifi_credentials_save(&input) == ESP_OK);
    CHECK(writes == 1 && commits == 1);
    CHECK(wifi_credentials_load(&output) == ESP_OK);
    CHECK(!strcmp(output.ssid, input.ssid) && !strcmp(output.password, input.password));
    CHECK(output.auth == WIFI_AUTH_WPA2_PSK);
    CHECK(wifi_credentials_save(&output) == ESP_OK);
    CHECK(writes == 1 && commits == 1); // No flash wear on an unchanged reconnect.

    strcpy(input.password, "new-password");
    CHECK(wifi_credentials_save(&input) == ESP_OK && commits == 2);
    input.password[3] = 0;
    CHECK(wifi_credentials_save(&input) == ESP_ERR_INVALID_ARG && commits == 2);
    CHECK(wifi_credentials_load(&output) == ESP_OK);
    CHECK(!strcmp(output.password, "new-password"));
    input = output;
    input.version = 99;
    CHECK(!wifi_credentials_valid(&input));
    input = output; input.auth = WIFI_AUTH_WEP;
    CHECK(!wifi_credentials_valid(&input));
    input.auth = WIFI_AUTH_ENTERPRISE;
    CHECK(!wifi_credentials_valid(&input));
    input = output; input.ssid[0] = 0;
    CHECK(!wifi_credentials_valid(&input));
    input = output; memset(input.ssid, 'S', sizeof(input.ssid));
    CHECK(!wifi_credentials_valid(&input));
    input = output; memset(input.password, 'P', sizeof(input.password));
    CHECK(!wifi_credentials_valid(&input));

    input = output;
    memset(input.ssid, 'S', 32); input.ssid[32] = 0;
    memset(input.password, 'P', 63); input.password[63] = 0;
    CHECK(wifi_credentials_save(&input) == ESP_OK);
    CHECK(wifi_credentials_load(&output) == ESP_OK);
    CHECK(strlen(output.ssid) == 32 && strlen(output.password) == 63);
    input.auth = WIFI_AUTH_OPEN;
    CHECK(wifi_credentials_save(&input) == ESP_ERR_INVALID_ARG);
    memset(input.password, 0, sizeof(input.password));
    CHECK(wifi_credentials_save(&input) == ESP_OK);
    CHECK(wifi_credentials_load(&output) == ESP_OK && !output.password[0]);

    // A reboot/loading operation rejects malformed, truncated or oversized blobs.
    stored.version = 99;
    CHECK(wifi_credentials_load(&output) == ESP_ERR_INVALID_STATE);
    CHECK(!memcmp(&output, &empty, sizeof(output)));
    stored = input; stored_size = sizeof(stored)-1;
    CHECK(wifi_credentials_load(&output) == ESP_ERR_INVALID_STATE);
    stored_size = sizeof(stored)+1;
    CHECK(wifi_credentials_load(&output) == ESP_ERR_NVS_INVALID_LENGTH);
    stored_size = sizeof(stored); memset(stored.password, 'X', sizeof(stored.password));
    CHECK(wifi_credentials_load(&output) == ESP_ERR_INVALID_STATE);
    CHECK(!memcmp(&output, &empty, sizeof(output)));

    stored = input;
    strcpy(input.ssid, "Replacement network");
    unsigned before = commits, closed = closes;
    write_error = ESP_FAIL;
    CHECK(wifi_credentials_save(&input) == ESP_FAIL);
    CHECK(commits == before && closes == closed+2);
    write_error = 0; commit_error = ESP_FAIL;
    CHECK(wifi_credentials_save(&input) == ESP_FAIL && commits == before);
    commit_error = 0; open_error = ESP_FAIL;
    CHECK(wifi_credentials_save(&input) == ESP_FAIL);
    CHECK(wifi_credentials_load(&output) == ESP_FAIL);
    CHECK(!memcmp(&output, &empty, sizeof(output)));
    open_error = 0; read_error = ESP_FAIL;
    CHECK(wifi_credentials_load(&output) == ESP_FAIL);
    CHECK(!memcmp(&output, &empty, sizeof(output)));
    CHECK(wifi_credentials_load(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(wifi_credentials_save(NULL) == ESP_ERR_INVALID_ARG);
    return 0;
}
