#include "voice_recorder.h"
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SAMPLE_RATE 16000
#define RECORD_SECONDS 5
#define SAMPLE_COUNT (SAMPLE_RATE * RECORD_SECONDS)
#define PCM_BYTES (SAMPLE_COUNT * sizeof(int16_t))
#define RECORD_PATH "/spiffs/recording.wav"
#define TEMP_PATH "/spiffs/recording.tmp"
#define BLOCK_FRAMES 256
#define MIC_GAIN 4
#define DMA_FRAMES 240
#define DMA_DESCRIPTORS 8
#define MIC_BCLK_GPIO GPIO_NUM_19
#define MIC_WS_GPIO GPIO_NUM_47
#define MIC_DATA_GPIO GPIO_NUM_20

static const char *TAG = "VOICE";
static volatile bool rx_overflow;

static bool on_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    rx_overflow = true;
    return false;
}

static void put_u32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}

static void wav_header(uint8_t header[44])
{
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    put_u32(header + 4, 36 + PCM_BYTES);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_u32(header + 16, 16);
    header[20] = 1; // PCM
    header[22] = 1; // mono
    put_u32(header + 24, SAMPLE_RATE);
    put_u32(header + 28, SAMPLE_RATE * 2);
    header[32] = 2;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, PCM_BYTES);
}

esp_err_t record_voice(void)
{
    // Buffer all five seconds before flash writes, which can stall capture.
    int16_t *pcm = heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) pcm = malloc(PCM_BYTES);
    if (!pcm) return ESP_ERR_NO_MEM;
    i2s_chan_handle_t rx = NULL;
    bool enabled = false;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_DESCRIPTORS;
    channel.dma_frame_num = DMA_FRAMES;
    esp_err_t err = i2s_new_channel(&channel, NULL, &rx);
    if (err != ESP_OK) goto cleanup;
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // Two 32-bit slots provide the required 64 BCLK pulses per frame.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        // GPIO12/13/14 belong to the RGB LCD. Rewire the INMP441 here.
        // GPIO47 is shared with LCD control SDA only during boot capture.
        // LCD CS stays high until RX is deleted and microphone BCLK is low.
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = MIC_BCLK_GPIO, .ws = MIC_WS_GPIO,
                     .dout = I2S_GPIO_UNUSED, .din = MIC_DATA_GPIO},
    };
    err = i2s_channel_init_std_mode(rx, &config);
    if (err != ESP_OK) goto cleanup;
    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_overflow};
    err = i2s_channel_register_event_callback(rx, &callbacks, NULL);
    if (err != ESP_OK) goto cleanup;
    rx_overflow = false;
    ESP_LOGI(TAG, "Speak now: recording %d seconds", RECORD_SECONDS);
    err = i2s_channel_enable(rx);
    if (err != ESP_OK) goto cleanup;
    enabled = true;
    int32_t raw[BLOCK_FRAMES * 2];
    size_t captured = 0;
    size_t warmup = SAMPLE_RATE / 10; // Drain 100 ms of microphone startup data.
    while (captured < SAMPLE_COUNT) {
        size_t frames = warmup ? warmup : SAMPLE_COUNT - captured;
        if (frames > BLOCK_FRAMES) frames = BLOCK_FRAMES;
        size_t received = 0;
        err = i2s_channel_read(rx, raw, frames * 2 * sizeof(int32_t), &received, 1000);
        if (err != ESP_OK) goto cleanup;
        if (rx_overflow || received == 0 || received % (2 * sizeof(int32_t))) {
            err = ESP_FAIL;
            goto cleanup;
        }
        frames = received / (2 * sizeof(int32_t));
        if (warmup) {
            warmup -= frames;
            continue;
        }
        for (size_t i = 0; i < frames; ++i) {
            // L/R grounded selects left. Signed 24-bit data occupies bits 31:8.
            int32_t sample = ((raw[i * 2] >> 8) * MIC_GAIN) / 256;
            if (sample > INT16_MAX) sample = INT16_MAX;
            if (sample < INT16_MIN) sample = INT16_MIN;
            pcm[captured++] = (int16_t)sample;
        }
    }
    err = i2s_channel_disable(rx);
    if (err != ESP_OK) goto cleanup;
    enabled = false;
    if (rx_overflow) { err = ESP_FAIL; goto cleanup; }
    ESP_LOGI(TAG, "Recording complete; saving WAV to SPIFFS");
    // Keep the previous recording until the new temporary file is complete.
    remove(TEMP_PATH);
    size_t total = 0, used = 0;
    err = esp_spiffs_info("spiffs", &total, &used);
    if (err != ESP_OK) goto cleanup;
    if (used + PCM_BYTES + 44 > total * 3 / 4) {
        ESP_LOGE(TAG, "Insufficient SPIFFS space (including filesystem headroom)");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    FILE *file = fopen(TEMP_PATH, "wb");
    if (!file) { err = ESP_FAIL; goto cleanup; }
    uint8_t header[44];
    wav_header(header);
    bool saved = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    if (saved) saved = fwrite(pcm, 1, PCM_BYTES, file) == PCM_BYTES;
    if (fclose(file) != 0) saved = false;
    if (!saved) {
        remove(TEMP_PATH);
        err = ESP_FAIL;
        goto cleanup;
    }
    // SPIFFS cannot rename over an existing destination.
    if ((remove(RECORD_PATH) != 0 && errno != ENOENT) ||
        rename(TEMP_PATH, RECORD_PATH) != 0) {
        ESP_LOGE(TAG, "Replacement failed; new WAV remains at " TEMP_PATH);
        err = ESP_FAIL;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Saved %s: %u samples, %u bytes", RECORD_PATH,
             (unsigned)SAMPLE_COUNT, (unsigned)(PCM_BYTES + 44));
cleanup:
    if (enabled) i2s_channel_disable(rx);
    if (rx) i2s_del_channel(rx);
    // Stop microphone clocks before GPIO47 is reused for LCD control.
    gpio_set_direction(MIC_BCLK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MIC_BCLK_GPIO, 0);
    free(pcm);
    if (err != ESP_OK) ESP_LOGE(TAG, "Recording/save failed%s: %s",
                               rx_overflow ? " (RX overflow)" : "", esp_err_to_name(err));
    return err;
}

static esp_err_t write_all(i2s_chan_handle_t tx, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    while (bytes) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx, p, bytes, &written, 1000);
        if (err != ESP_OK) return err;
        if (!written) return ESP_FAIL;
        p += written;
        bytes -= written;
    }
    return ESP_OK;
}

esp_err_t play_recorded_voice(int volume)
{
    FILE *file = fopen(RECORD_PATH, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    uint8_t expected[44], actual[44];
    wav_header(expected);
    // This player accepts the fixed-format WAV produced above.
    if (fread(actual, 1, 44, file) != 44 || memcmp(actual, expected, 44)) {
        fclose(file);
        return ESP_ERR_INVALID_ARG;
    }
    if (volume < 0) volume = 0;
    if (volume > 21) volume = 21;
    i2s_chan_handle_t tx = NULL;
    bool enabled = false;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_DESCRIPTORS;
    channel.dma_frame_num = DMA_FRAMES;
    channel.auto_clear = true;
    esp_err_t err = i2s_new_channel(&channel, &tx, NULL);
    if (err != ESP_OK) goto cleanup;
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // MAX98357A uses standard I2S; duplicate mono to both slots.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = 46, .ws = 45,
                     .dout = 42, .din = I2S_GPIO_UNUSED},
    };
    err = i2s_channel_init_std_mode(tx, &config);
    if (err != ESP_OK) goto cleanup;
    err = i2s_channel_enable(tx);
    if (err != ESP_OK) goto cleanup;
    enabled = true;
    ESP_LOGI(TAG, "Playing saved recording");
    int16_t mono[BLOCK_FRAMES], stereo[BLOCK_FRAMES * 2];
    size_t remaining = SAMPLE_COUNT;
    while (remaining) {
        size_t frames = remaining > BLOCK_FRAMES ? BLOCK_FRAMES : remaining;
        if (fread(mono, sizeof(int16_t), frames, file) != frames) {
            err = ESP_FAIL;
            goto cleanup;
        }
        for (size_t i = 0; i < frames; ++i) {
            int16_t sample = (int32_t)mono[i] * volume / 21;
            stereo[2 * i] = stereo[2 * i + 1] = sample;
        }
        err = write_all(tx, stereo, frames * 2 * sizeof(int16_t));
        if (err != ESP_OK) goto cleanup;
        remaining -= frames;
    }
    // Queue more than a full DMA ring of silence so the audio tail is transmitted.
    memset(stereo, 0, sizeof(stereo));
    for (size_t frames = 0; frames < (DMA_DESCRIPTORS + 1) * DMA_FRAMES; frames += BLOCK_FRAMES) {
        err = write_all(tx, stereo, sizeof(stereo));
        if (err != ESP_OK) goto cleanup;
    }
    ESP_LOGI(TAG, "Playback complete; reset the board to record again");
cleanup:
    if (enabled) i2s_channel_disable(tx);
    if (tx) i2s_del_channel(tx);
    fclose(file);
    if (err != ESP_OK) ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(err));
    return err;
}
