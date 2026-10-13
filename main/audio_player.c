#include "audio_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"
#include <sys/stat.h>
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "MY_AUDIO.C";

static i2s_chan_handle_t tx_chan = NULL;

static bool audio_enabled = true;

static int current_volume = 16;
#define MAX_VOLUME 21



bool is_audio_enabled(void)
{
    return audio_enabled;
}

bool fileExists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void audio_i2s_init(void)
{
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
	ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

	i2s_std_config_t cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
													I2S_SLOT_MODE_MONO),
		.gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
            .bclk = 46,
            .ws   = 45,
            .dout = 42, 
			.din = I2S_GPIO_UNUSED,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv = false,
			},
		},
	};

	ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

	ESP_LOGI(TAG, "I2S initialized");
}

void play_mp3_file(const char *filename)
{
    char spiffs_path[128];

    // build SPIFFS path
    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs/%s", filename);

    ESP_LOGI(TAG, "Playing MP3 from: %s", spiffs_path);

    if (!fileExists(spiffs_path))
    {
        ESP_LOGE(TAG, "MP3 file not found: %s", spiffs_path);
        return;
    }

    mp3dec_ex_t dec;

    if (mp3dec_ex_open(&dec, spiffs_path, MP3D_SEEK_TO_SAMPLE))
    {
        ESP_LOGE(TAG, "Failed to open MP3: %s", spiffs_path);
        return;
    }

    ESP_LOGI(TAG, "MP3 opened. Total samples: %u", (unsigned)dec.samples);
    ESP_LOGI(TAG, "MP3 sample rate: %d", dec.info.hz);

    /* -------- FIX AUDIO SPEED -------- */

    i2s_std_clk_config_t clk_cfg = {
        .sample_rate_hz = dec.info.hz,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256
    };

    i2s_channel_disable(tx_chan);
    i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    i2s_channel_enable(tx_chan);

    /* -------------------------------- */

    mp3d_sample_t *buffer = heap_caps_malloc(
        2048 * sizeof(mp3d_sample_t),
        MALLOC_CAP_DMA
    );

    if (!buffer)
    {
        ESP_LOGE(TAG, "Buffer allocation failed");
        mp3dec_ex_close(&dec);
        return;
    }

    size_t total_read = 0;

    while (total_read < dec.samples)
    {
        int to_read = 1024;

        if (dec.samples - total_read < 1024)
            to_read = dec.samples - total_read;

        size_t readed = mp3dec_ex_read(&dec, buffer, to_read);

        if (readed == 0)
            break;

        // Software volume control
        for (int i = 0; i < readed; i++)
        {
            int32_t sample = buffer[i];

            // 50% headroom
            sample = (sample * 5) / 10;

            // volume control
            sample = (sample * current_volume) / MAX_VOLUME;

            // clipping protection
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            buffer[i] = (mp3d_sample_t)sample;
        }

        size_t bytes_written = 0;

       esp_err_t err = i2s_channel_write(
            tx_chan,
            buffer,
            readed * sizeof(mp3d_sample_t),
            &bytes_written,
            300
        );

        if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
            }

            if (bytes_written == 0) {
                ESP_LOGE(TAG, "I2S write timeout! Wrote 0 bytes (expected %u)",
                        readed * sizeof(mp3d_sample_t));
            } else if (bytes_written < readed * sizeof(mp3d_sample_t)) {
                ESP_LOGW(TAG, "Partial write: %d / %d bytes",
                        bytes_written, readed * sizeof(mp3d_sample_t));
            }
        total_read += readed;
    }

    free(buffer);

    mp3dec_ex_close(&dec);

    ESP_LOGI(TAG, "MP3 playback finished");
}


void init_audio(void)
{
    audio_i2s_init();
    load_volume_from_nvs();
    load_audio_enabled_from_nvs();
}

//-------------------------------------Set Volume ----------------------------------
void save_volume_to_nvs(int vol)
{
    nvs_handle_t handle;

    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_i32(handle, "volume", vol);
        nvs_commit(handle);
        nvs_close(handle);

        ESP_LOGI(TAG, "Volume saved to NVS: %d", vol);
    }
}

void load_volume_from_nvs(void)
{
    nvs_handle_t handle;
    int32_t vol = current_volume;

    if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK)
    {
        if (nvs_get_i32(handle, "volume", &vol) == ESP_OK)
        {
            current_volume = vol;
        }

        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Loaded volume from NVS: %d", current_volume);
}

void set_volume(int vol)
{
    if (vol < 0)
    {
        vol = 0;
    }

    if (vol > MAX_VOLUME)
    {
        vol = MAX_VOLUME;
    }

    current_volume = vol;

    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_i32(handle, "volume", current_volume);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Volume saved to NVS: %d", current_volume);
    }

    ESP_LOGI(TAG, "Volume set to: %d", current_volume);
}

void increase_volume(void)
{
    if (current_volume < MAX_VOLUME)
    {
        current_volume++;
    }

    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_i32(handle, "volume", current_volume);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Volume increased to: %d", current_volume);
}

void decrease_volume(void)
{
    if (current_volume > 0)
    {
        current_volume--;
    }

    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_i32(handle, "volume", current_volume);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Volume decreased to: %d", current_volume);
}

//---------------------------------------Get Volume--------------------------
int get_volume(void)
{
    return current_volume;
}


//-----------------------------------AudioON/AudioOFF--------------------------------

void save_audio_enabled_to_nvs(bool enabled)
{
    nvs_handle_t handle;

    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_u8(handle, "audio_en", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);

        ESP_LOGI(TAG, "Audio enabled state saved to NVS: %s", enabled ? "ON" : "OFF");
    }
}

void load_audio_enabled_from_nvs(void)
{
    nvs_handle_t handle;
    uint8_t enabled = 1;   // default = ON

    if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK)
    {
        if (nvs_get_u8(handle, "audio_en", &enabled) == ESP_OK)
        {
            audio_enabled = (enabled != 0);
        }
        else
        {
            audio_enabled = true;
        }

        nvs_close(handle);
    }
    else
    {
        audio_enabled = true;
    }

    ESP_LOGI(TAG, "Loaded audio enabled from NVS: %s", audio_enabled ? "ON" : "OFF");
}

void set_audio_enabled(bool enabled)
{
    audio_enabled = enabled;
    save_audio_enabled_to_nvs(audio_enabled);

    ESP_LOGI(TAG, "Audio is now %s", audio_enabled ? "ON" : "OFF");
}




