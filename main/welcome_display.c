#include "welcome_display.h"
#include "wifi_ui.h"
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// VIEWE UEDX48480040E-WB-A: GC9503, 9-bit mode-0 control SPI + RGB565.
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    unsigned data_bytes;
    unsigned delay_ms;
} panel_command_t;
#include "gc9503_init.inc"

extern const uint8_t artwork[] asm("_binary_welcome_rgb565_start");
extern const uint8_t artwork_end[] asm("_binary_welcome_rgb565_end");
static esp_lcd_panel_handle_t panel;

static void spi_word(bool data, uint8_t byte)
{
    unsigned word = (data ? 0x100 : 0) | byte;
    gpio_set_level(39, 0);
    for (int bit = 8; bit >= 0; --bit) {
        gpio_set_level(48, 0);
        gpio_set_level(47, (word >> bit) & 1);
        esp_rom_delay_us(2);
        gpio_set_level(48, 1);
        esp_rom_delay_us(2);
    }
    gpio_set_level(48, 0);
    gpio_set_level(39, 1);
}

esp_err_t welcome_display_show(void)
{
    if (panel) return ESP_OK;
    if (artwork_end - artwork != 480 * 480 * 2) return ESP_ERR_INVALID_SIZE;
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << 39) | (1ULL << 48) | (1ULL << 47),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&pins);
    if (err != ESP_OK) return err;
    gpio_set_level(39, 1);
    gpio_set_level(48, 0);
    spi_word(false, 0x01); // Software reset: board has no dedicated LCD reset GPIO.
    vTaskDelay(pdMS_TO_TICKS(120));
    spi_word(false, 0xb1);
    spi_word(true, 0x10);
    spi_word(false, 0x3a);
    spi_word(true, 0x60); // Vendor panel uses RGB666 input with RGB565 bus wiring.
    for (unsigned i = 0; i < sizeof(vendor_specific_init_default) / sizeof(panel_command_t); ++i) {
        const panel_command_t *cmd = &vendor_specific_init_default[i];
        spi_word(false, cmd->cmd);
        for (unsigned j = 0; j < cmd->data_bytes; ++j) spi_word(true, cmd->data[j]);
        if (cmd->delay_ms) vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    }
    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 18000000, .h_res = 480, .v_res = 480,
            .hsync_pulse_width = 8, .hsync_back_porch = 40, .hsync_front_porch = 40,
            .vsync_pulse_width = 2, .vsync_back_porch = 4, .vsync_front_porch = 16,
        },
        .data_width = 16, .bits_per_pixel = 16, .num_fbs = 1,
        .bounce_buffer_size_px = 480 * 10, .dma_burst_size = 64,
        .hsync_gpio_num = 16, .vsync_gpio_num = 17, .de_gpio_num = 18,
        .pclk_gpio_num = 21, .disp_gpio_num = -1,
        .data_gpio_nums = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
        .flags.fb_in_psram = true,
    };
    err = esp_lcd_new_rgb_panel(&config, &panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_panel_draw_bitmap(panel, 0, 0, 480, 480, artwork);
    if (err != ESP_OK) {
        esp_lcd_panel_del(panel);
        panel = NULL;
        return err;
    }
    // Let a complete frame scan out before enabling the backlight.
    vTaskDelay(pdMS_TO_TICKS(50));
    err = gpio_set_level(38, 1);
    if (err != ESP_OK) return err;
    return wifi_ui_start(panel, artwork);
}
