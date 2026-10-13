#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "audio_player.h"
#include "voice_recorder.h"
#include "welcome_display.h"
#include "wifi_setup.h"

static const char *TAG = "SPIFFS_AUDIO";

static void playback_task(void *arg)
{
    load_volume_from_nvs();
    load_audio_enabled_from_nvs();
    for (int seconds = 3; seconds > 0; --seconds) {
        ESP_LOGI(TAG, "Recording starts in %d...", seconds);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_err_t err = record_voice();
    if (err == ESP_OK && is_audio_enabled()) {
        if (get_volume() == 0) ESP_LOGW(TAG, "Speaker volume is zero; playback will be silent");
        err = play_recorded_voice(get_volume());
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "Recording saved; speaker disabled in NVS");
    }
    // Release boot audio before starting networking, LCD, and Agent Mode.
    esp_err_t network_err = wifi_setup_start();
    if (network_err == ESP_OK) {
        network_err = wifi_setup_connect_saved();
        if (network_err == ESP_ERR_NOT_FOUND) network_err = ESP_OK;
    }
    if (network_err != ESP_OK) ESP_LOGE(TAG, "Network startup failed: %s", esp_err_to_name(network_err));
    if (err != ESP_OK) ESP_LOGE(TAG, "Record/play failed: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "Starting welcome display and touch UI");
    esp_err_t display_err = welcome_display_show();
    if (display_err != ESP_OK) ESP_LOGE(TAG, "Welcome display failed: %s", esp_err_to_name(display_err));
    vTaskDelete(NULL);
}

void app_main(void)
{
    // GPIO47 is microphone WS during capture and LCD control SDA at LCD setup.
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_38, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_38, 0));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_39, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_39, 1));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_48, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_48, 0));
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return;
    }
    if (xTaskCreate(playback_task, "spiffs_audio", 12 * 1024, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
    }
}
