/* ============================================================
 * TFT DRIVER (ILI9488, SPI)
 * Everything about talking to the screen lives in this one file.
 * If you ever swap displays, this is the only file you touch.
 * ============================================================ */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "tft_driver.h"

#define TFT_SPI_HZ (26 * 1000 * 1000)

static spi_device_handle_t tft_spi;

/* One reusable row buffer: max 480 pixels wide * 3 bytes (RGB666) */
#define LINE_BUF_PIXELS TFT_WIDTH
static uint8_t line_buf[LINE_BUF_PIXELS * 3];

static inline void tft_cs_low(void)  { gpio_set_level(TFT_CS, 0); }
static inline void tft_cs_high(void) { gpio_set_level(TFT_CS, 1); }

static void tft_write_cmd(uint8_t cmd) {
    gpio_set_level(TFT_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    tft_cs_low();
    spi_device_polling_transmit(tft_spi, &t);
    tft_cs_high();
}

static void tft_write_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    gpio_set_level(TFT_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    tft_cs_low();
    spi_device_polling_transmit(tft_spi, &t);
    tft_cs_high();
}

static inline void tft_write_data_byte(uint8_t v) { tft_write_data(&v, 1); }

static void tft_set_addr_window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    uint8_t d[4];
    tft_write_cmd(0x2A);
    d[0] = x0 >> 8; d[1] = x0 & 0xFF; d[2] = x1 >> 8; d[3] = x1 & 0xFF;
    tft_write_data(d, 4);
    tft_write_cmd(0x2B);
    d[0] = y0 >> 8; d[1] = y0 & 0xFF; d[2] = y1 >> 8; d[3] = y1 & 0xFF;
    tft_write_data(d, 4);
    tft_write_cmd(0x2C);
}

static inline void rgb565_to_666_bytes(uint16_t color, uint8_t *out) {
    uint8_t r5 = (color >> 11) & 0x1F;
    uint8_t g6 = (color >> 5) & 0x3F;
    uint8_t b5 = color & 0x1F;
    out[0] = (r5 << 3) | (r5 >> 2);
    out[1] = (g6 << 2) | (g6 >> 4);
    out[2] = (b5 << 3) | (b5 >> 2);
}

void tft_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    tft_set_addr_window(x, y, x + w - 1, y + h - 1);

    uint8_t px[3];
    rgb565_to_666_bytes(color, px);
    for (int i = 0; i < w; i++) memcpy(&line_buf[i * 3], px, 3);

    gpio_set_level(TFT_DC, 1);
    tft_cs_low();
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = { .length = (size_t)w * 3 * 8, .tx_buffer = line_buf };
        spi_device_polling_transmit(tft_spi, &t);
    }
    tft_cs_high();
}

void tft_draw_sprite_scaled(const uint8_t *sprite, uint8_t sw, uint8_t sh,
                             int16_t x, int16_t y, uint8_t scale, uint16_t color) {
    int16_t w = sw * scale, h = sh * scale;
    if (x < 0 || y < 0 || x + w > TFT_WIDTH || y + h > TFT_HEIGHT) return;

    uint8_t fg[3], bg[3];
    rgb565_to_666_bytes(color, fg);
    rgb565_to_666_bytes(BLACK, bg);

    tft_set_addr_window(x, y, x + w - 1, y + h - 1);
    gpio_set_level(TFT_DC, 1);
    tft_cs_low();

    for (uint8_t sy = 0; sy < sh; sy++) {
        for (uint8_t sx = 0; sx < sw; sx++) {
            uint8_t on = sprite[sy * sw + sx];
            const uint8_t *px = on ? fg : bg;
            for (uint8_t xx = 0; xx < scale; xx++)
                memcpy(&line_buf[(sx * scale + xx) * 3], px, 3);
        }
        for (uint8_t yy = 0; yy < scale; yy++) {
            spi_transaction_t t = { .length = (size_t)w * 3 * 8, .tx_buffer = line_buf };
            spi_device_polling_transmit(tft_spi, &t);
        }
    }
    tft_cs_high();
}

/* ---------------- Tiny 5x7 font ---------------- */
static const uint8_t font5x7[][5] = {
    {0,0,0,0,0}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    {0x3E,0x45,0x49,0x51,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E}
};

static int font_index(char c) {
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    return 0;
}

static void tft_draw_char(char c, int16_t x, int16_t y, uint8_t scale, uint16_t color) {
    int idx = font_index(c);
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t bits = font5x7[idx][col];
        for (uint8_t row = 0; row < 7; row++) {
            if (bits & (1 << row))
                tft_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void tft_draw_text(const char *text, int16_t x, int16_t y, uint8_t scale, uint16_t color) {
    while (*text) {
        tft_draw_char(*text++, x, y, scale, color);
        x += 6 * scale;
    }
}

void tft_driver_init_gpio(void) {
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << TFT_CS) | (1ULL << TFT_DC) | (1ULL << TFT_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_set_level(TFT_CS, 1);
}

void tft_driver_init_spi(void) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .sclk_io_num = TFT_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LINE_BUF_PIXELS * 3,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = TFT_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,   /* we drive CS ourselves */
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &tft_spi));
}

void tft_driver_init_display(void) {
    gpio_set_level(TFT_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    tft_write_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(120)); /* software reset */
    tft_write_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120)); /* sleep out */
    tft_write_cmd(0x3A); tft_write_data_byte(0x66);      /* 18-bit RGB666 */
    tft_write_cmd(0x36); tft_write_data_byte(0x28);      /* landscape orientation */
    tft_write_cmd(0x20);                                 /* inversion off */
    tft_write_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));   /* display on */
}
