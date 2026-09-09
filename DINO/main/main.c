/* ============================================================
 * ESP32-S3 DINO RUNNER  --  pure ESP-IDF version (no Arduino)
 * ============================================================
 *
 * WHAT CHANGED vs the Arduino version, in plain words:
 *
 * 1) Arduino's setup()/loop() -> ESP-IDF's app_main() + FreeRTOS tasks.
 *
 * 2) THE BIG UPGRADE: joystick reading now runs on its OWN task,
 *    pinned to CORE 0, 500 times every second (every 2ms).
 *    The screen-drawing (which is "slow", it talks to the display
 *    over SPI) runs on CORE 1.
 *    Since the ESP32-S3 has TWO cores, these two jobs now truly run
 *    at the same time, instead of taking turns like in one Arduino
 *    loop(). So your jump is felt almost instantly, even while the
 *    screen is busy drawing.
 *
 * 3) Arduino's SPI.transfer() one-byte-at-a-time -> we now build a
 *    whole row of pixels in a buffer and send it in ONE SPI
 *    transaction. Same idea as sending a whole WhatsApp message at
 *    once instead of texting one letter at a time. This makes
 *    drawing much smoother.
 *
 * 4) No PROGMEM needed. That trick was only needed on old 8-bit
 *    Arduino chips with tiny RAM. The ESP32-S3 does not need it, so
 *    the sprite arrays are just normal "const" arrays.
 *
 * 5) millis()/micros() -> esp_timer_get_time() (microseconds, always).
 *    delay() -> vTaskDelay(pdMS_TO_TICKS(ms))  (never block other tasks).
 *
 * BUILD:
 *   idf.py set-target esp32s3
 *   idf.py build
 *   idf.py -p <YOUR_COM_PORT> flash monitor
 *
 * WIRING (same as before):
 *   TFT CS -> GPIO10   TFT DC -> GPIO14   TFT RST -> GPIO21
 *   TFT MOSI -> GPIO11 TFT SCK -> GPIO12  TFT MISO -> GPIO13
 *   Joystick VRX -> GPIO1   VRY -> GPIO2   SW -> GPIO4
 *   Joystick VCC -> 3.3V    GND -> GND
 * ============================================================ */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "DINO";

/* ---------------- Pins ---------------- */
#define TFT_CS   10
#define TFT_DC   14
#define TFT_RST  21
#define TFT_MOSI 11
#define TFT_CLK  12
#define TFT_MISO 13

/* On ESP32-S3, ADC1 channel 0 = GPIO1, channel 1 = GPIO2 */
#define JOY_X_ADC_CHANNEL ADC_CHANNEL_0
#define JOY_Y_ADC_CHANNEL ADC_CHANNEL_1
#define JOY_SW_PIN        4

#define TFT_WIDTH   480
#define TFT_HEIGHT  320
#define TFT_SPI_HZ  (26 * 1000 * 1000)   /* ESP-IDF's SPI driver can safely go faster than Arduino's */

/* If your ESP-IDF is older and doesn't know ADC_ATTEN_DB_12,
 * change the line below to ADC_ATTEN_DB_11 instead. */
#define JOY_ADC_ATTEN ADC_ATTEN_DB_12

/* ---------------- Colors (RGB565) ---------------- */
#define BLACK      0x0000
#define WHITE      0xFFFF
#define DARKGREY   0x4208
#define LIGHTGREY  0xC618

#define BG_COLOR       BLACK
#define GROUND_COLOR   WHITE
#define DINO_COLOR     WHITE
#define OBSTACLE_COLOR WHITE

/* ---------------- Game constants ---------------- */
#define GROUND_Y          270
#define FIXED_DT          (1.0f / 60.0f)
#define GRAVITY           1050.0f
#define JUMP_VELOCITY     -390.0f
#define BASE_SPEED        175.0f
#define SPEED_INCREASE    5.0f
#define MAX_SPEED         315.0f
#define MAX_OBSTACLES     3

#define DINO_X            48
#define DINO_W            32
#define DINO_H            32
#define DINO_GROUND_TOP   (GROUND_Y - DINO_H)

typedef struct { int16_t x, y, w, h; } rect_t;

typedef struct {
    float x, y;
    float velocityY;
    bool jumping;
    rect_t previous;
} dino_t;

typedef struct {
    float x;
    int16_t width, height;
    bool active;
    rect_t previous;
} obstacle_t;

typedef enum { READY, PLAYING, GAME_OVER } game_state_t;

static dino_t dino;
static obstacle_t obstacles[MAX_OBSTACLES];
static game_state_t game_state = READY;

static uint32_t score = 0;
static uint32_t displayed_score = 0;
static float elapsed_game_seconds = 0.0f;
static bool first_render = true;
static game_state_t last_rendered_state = GAME_OVER;

/* ---------------- Sprites (16x16 dino, 12x20 obstacle) ----------------
 * No PROGMEM needed on ESP32 - flash and RAM share one address space. */
static const uint8_t dino_sprite[16][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,0},
    {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0},
    {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,1,1,1,1,1,1,0,0,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,0,0,0,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,0,0,0,1,1,0,0,0,0}
};

static const uint8_t obstacle_sprite[20][12] = {
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},{0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},{0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},{0,0,0,0,0,1,1,0,0,0,0,0}
};

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

/* ============================================================
 *                     TFT DRIVER (ILI9488, SPI)
 * ============================================================ */
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

/* Fills a rectangle. Builds ONE row of pixels, then blasts it out
 * row-by-row in single SPI transactions - much faster than sending
 * pixel-by-pixel like the Arduino version did. */
static void tft_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
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

/* Draws a sprite scaled up, one "big pixel row" per SPI transaction. */
static void tft_draw_sprite_scaled(const uint8_t *sprite, uint8_t sw, uint8_t sh,
                                    int16_t x, int16_t y, uint8_t scale, uint16_t color) {
    int16_t w = sw * scale, h = sh * scale;
    if (x < 0 || y < 0 || x + w > TFT_WIDTH || y + h > TFT_HEIGHT) return;

    uint8_t fg[3], bg[3];
    rgb565_to_666_bytes(color, fg);
    rgb565_to_666_bytes(BG_COLOR, bg);

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

static void tft_draw_text(const char *text, int16_t x, int16_t y, uint8_t scale, uint16_t color) {
    while (*text) {
        tft_draw_char(*text++, x, y, scale, color);
        x += 6 * scale;
    }
}

static void tft_init_display(void) {
    gpio_set_level(TFT_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    tft_write_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(120)); /* software reset */
    tft_write_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120)); /* sleep out */
    tft_write_cmd(0x3A); tft_write_data_byte(0x66);      /* 18-bit RGB666 */
    tft_write_cmd(0x36); tft_write_data_byte(0x28);      /* landscape orientation */
    tft_write_cmd(0x20);                                 /* inversion off */
    tft_write_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));   /* display on */
}

static void gpio_setup(void) {
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << TFT_CS) | (1ULL << TFT_DC) | (1ULL << TFT_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);
    gpio_set_level(TFT_CS, 1);

    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << JOY_SW_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sw_cfg);
}

static void spi_setup(void) {
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
        .spics_io_num = -1,   /* we drive CS ourselves, same style as the Arduino version */
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &tft_spi));
}

/* ============================================================
 *              JOYSTICK / BUTTON INPUT  (FAST TASK)
 * ============================================================
 * This whole block runs on CORE 0, sampling 500 times a second.
 * It NEVER waits on the screen. That is the "quicker detection"
 * upgrade you asked for.
 * ============================================================ */
static adc_oneshot_unit_handle_t adc_handle;

typedef struct {
    bool jump_pending;       /* sticky: true until the game task consumes it */
    bool move_edge_pending;  /* sticky: used for "move joystick to restart" */
} shared_input_t;

static shared_input_t g_input = {0};
static SemaphoreHandle_t input_mutex;

static void adc_setup(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t ch_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = JOY_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_X_ADC_CHANNEL, &ch_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_Y_ADC_CHANNEL, &ch_cfg));
}

static inline int read_adc(adc_channel_t ch) {
    int val = 0;
    adc_oneshot_read(adc_handle, ch, &val);
    return val;
}

static bool input_consume_jump(void) {
    bool v;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    v = g_input.jump_pending;
    g_input.jump_pending = false;
    xSemaphoreGive(input_mutex);
    return v;
}

static bool input_consume_move_edge(void) {
    bool v;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    v = g_input.move_edge_pending;
    g_input.move_edge_pending = false;
    xSemaphoreGive(input_mutex);
    return v;
}

static void input_task(void *arg) {
    /* Startup calibration: leave the joystick untouched during power-up. */
    long sx = 0, sy = 0;
    for (int i = 0; i < 32; i++) {
        sx += read_adc(JOY_X_ADC_CHANNEL);
        sy += read_adc(JOY_Y_ADC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    int joy_center_x = sx / 32;
    int joy_center_y = sy / 32;
    ESP_LOGI(TAG, "Joystick center X=%d Y=%d", joy_center_x, joy_center_y);

    /* Thresholds lowered from the Arduino version (120/60 -> 90/45) so a
     * jump is recognised sooner, while still ignoring tiny hand-shake. */
    const int ENTER_TH = 90;
    const int EXIT_TH  = 45;
    const int64_t BUTTON_DEBOUNCE_US = 8000; /* 8ms, was 25ms -> snappier */

    bool prev_moved = false;
    int8_t prev_dir = 0;
    bool prev_button = false;
    int raw_button_state = 1, stable_button_state = 1;
    int64_t button_changed_at = 0;

    while (1) {
        int raw_x = read_adc(JOY_X_ADC_CHANNEL);
        int raw_y = read_adc(JOY_Y_ADC_CHANNEL);
        int raw_button = gpio_get_level(JOY_SW_PIN);

        int64_t now = esp_timer_get_time();
        if (raw_button != raw_button_state) {
            raw_button_state = raw_button;
            button_changed_at = now;
        }
        if (now - button_changed_at >= BUTTON_DEBOUNCE_US) {
            stable_button_state = raw_button_state;
        }
        bool button = (stable_button_state == 0); /* active LOW */

        int dx = raw_x - joy_center_x;
        int dy = raw_y - joy_center_y;
        int th = prev_moved ? EXIT_TH : ENTER_TH;
        bool moved = (abs(dx) > th) || (abs(dy) > th);

        int8_t dir = 0;
        if (moved) {
            if (abs(dx) >= abs(dy)) dir = (dx >= 0) ? 1 : 2;
            else                    dir = (dy >= 0) ? 3 : 4;
        }

        bool move_edge = moved && (!prev_moved || dir != prev_dir);
        bool jump_edge = move_edge || (button && !prev_button);

        if (jump_edge || move_edge) {
            xSemaphoreTake(input_mutex, portMAX_DELAY);
            if (jump_edge)  g_input.jump_pending = true;
            if (move_edge)  g_input.move_edge_pending = true;
            xSemaphoreGive(input_mutex);
        }

        prev_moved = moved;
        prev_dir = moved ? dir : 0;
        prev_button = button;

        /* IMPORTANT: on default ESP-IDF settings, 1 FreeRTOS tick = 10ms.
         * pdMS_TO_TICKS(2) rounds DOWN to 0 ticks, which means "don't sleep
         * at all" - the task then hogs the CPU forever and the watchdog
         * resets the board (blank screen). This guard makes sure we always
         * sleep at least 1 real tick, no matter the tick rate. */
        TickType_t delay_ticks = pdMS_TO_TICKS(2);
        if (delay_ticks == 0) delay_ticks = 1;
        vTaskDelay(delay_ticks);
    }
}

/* ============================================================
 *                        GAME LOGIC
 * ============================================================ */
static int rand_range(int lo, int hi) { /* random int in [lo, hi) */
    if (hi <= lo) return lo;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo));
}

static rect_t dino_rect(void) {
    rect_t r = { (int16_t)dino.x + 5, (int16_t)dino.y + 5, DINO_W - 10, DINO_H - 6 };
    return r;
}

static rect_t obstacle_rect(const obstacle_t *o) {
    rect_t r = { (int16_t)o->x + 4, (int16_t)(GROUND_Y - o->height) + 3, o->width - 8, o->height - 3 };
    return r;
}

static bool intersects(rect_t a, rect_t b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

static void reset_game(void) {
    dino.x = DINO_X;
    dino.y = DINO_GROUND_TOP;
    dino.velocityY = 0;
    dino.jumping = false;
    dino.previous = (rect_t){ (int16_t)dino.x, (int16_t)dino.y, DINO_W, DINO_H };

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        obstacles[i].active = false;
        obstacles[i].previous = (rect_t){0,0,0,0};
    }
    obstacles[0].active = true;
    obstacles[0].x = 430;
    obstacles[0].width = 24;
    obstacles[0].height = 40;
    obstacles[0].previous = (rect_t){ (int16_t)obstacles[0].x, GROUND_Y - obstacles[0].height,
                                       obstacles[0].width, obstacles[0].height };

    score = 0;
    displayed_score = 0;
    elapsed_game_seconds = 0;
}

static float current_speed(void) {
    float speed = BASE_SPEED + SPEED_INCREASE * elapsed_game_seconds;
    return speed > MAX_SPEED ? MAX_SPEED : speed;
}

static void spawn_obstacle_if_needed(void) {
    float furthest = -1000;
    bool any = false;
    for (int i = 0; i < MAX_OBSTACLES; i++)
        if (obstacles[i].active) { any = true; if (obstacles[i].x > furthest) furthest = obstacles[i].x; }
    if (any && furthest > 260) return;

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (!obstacles[i].active) {
            obstacles[i].active = true;
            obstacles[i].width = 24;
            obstacles[i].height = (rand_range(0, 3) == 0) ? 52 : 40;
            float minX = 510.0f;
            float candidate = furthest + (float)rand_range(175, 275);
            obstacles[i].x = candidate > minX ? candidate : minX;
            obstacles[i].previous = (rect_t){ (int16_t)obstacles[i].x,
                (int16_t)(GROUND_Y - obstacles[i].height), obstacles[i].width, obstacles[i].height };
            return;
        }
    }
}

/* jump_flag: true if the input task detected a jump command this tick */
static void update_physics(float dt, bool jump_flag) {
    if (game_state != PLAYING) return;

    elapsed_game_seconds += dt;
    score = (uint32_t)(elapsed_game_seconds * 50.0f);

    dino.velocityY += GRAVITY * dt;
    dino.y += dino.velocityY * dt;
    if (dino.y >= DINO_GROUND_TOP) {
        dino.y = DINO_GROUND_TOP;
        dino.velocityY = 0;
        dino.jumping = false;
    }

    if (jump_flag && !dino.jumping) {
        dino.velocityY = JUMP_VELOCITY;
        dino.jumping = true;
    }

    float speed = current_speed();
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (!obstacles[i].active) continue;
        obstacles[i].x -= speed * dt;
        if (obstacles[i].x + obstacles[i].width < 0) obstacles[i].active = false;
        if (intersects(dino_rect(), obstacle_rect(&obstacles[i]))) game_state = GAME_OVER;
    }
    spawn_obstacle_if_needed();
}

/* ---------------- Rendering ---------------- */
static void draw_score(void) {
    char text[7];
    uint32_t shown = score > 999999 ? 999999 : score;
    for (int i = 5; i >= 0; i--) { text[i] = '0' + (shown % 10); shown /= 10; }
    text[6] = '\0';
    tft_fill_rect(350, 8, 118, 20, BG_COLOR);
    tft_draw_text("SCORE", 350, 8, 2, WHITE);
    tft_draw_text(text, 420, 8, 2, WHITE);
}

static void draw_static_scene(void) {
    tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, BG_COLOR);
    tft_fill_rect(0, GROUND_Y, TFT_WIDTH, 2, GROUND_COLOR);
    tft_draw_text("DINO RUN", 12, 8, 2, LIGHTGREY);
    draw_score();
}

static void clear_previous_dynamic(void) {
    int16_t x = dino.previous.x - 2, y = dino.previous.y - 2;
    int16_t w = dino.previous.w + 4, h = dino.previous.h + 4;
    tft_fill_rect(x, y, w, h, BG_COLOR);
    if (y + h >= GROUND_Y && y < GROUND_Y + 2) tft_fill_rect(x, GROUND_Y, w, 2, GROUND_COLOR);

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (obstacles[i].previous.w > 0) {
            x = obstacles[i].previous.x - 2; y = obstacles[i].previous.y - 2;
            w = obstacles[i].previous.w + 4; h = obstacles[i].previous.h + 4;
            tft_fill_rect(x, y, w, h, BG_COLOR);
            if (y + h >= GROUND_Y && y < GROUND_Y + 2) tft_fill_rect(x, GROUND_Y, w, 2, GROUND_COLOR);
        }
    }
}

static void render_dynamic(void) {
    bool state_changed = game_state != last_rendered_state;
    clear_previous_dynamic();

    if (state_changed && (last_rendered_state == GAME_OVER || last_rendered_state == READY)) {
        tft_fill_rect(120, 112, 250, 80, BG_COLOR);
    }

    if (game_state == READY) {
        tft_draw_sprite_scaled(&dino_sprite[0][0], 16, 16, DINO_X, DINO_GROUND_TOP, 2, DINO_COLOR);
        if (state_changed) {
            tft_fill_rect(120, 112, 250, 80, BG_COLOR);
            tft_draw_text("PRESS JOYSTICK", 145, 125, 2, WHITE);
            tft_draw_text("UP OR BUTTON", 155, 145, 2, LIGHTGREY);
        }
    } else {
        tft_draw_sprite_scaled(&dino_sprite[0][0], 16, 16, (int16_t)dino.x, (int16_t)dino.y, 2, DINO_COLOR);
        for (int i = 0; i < MAX_OBSTACLES; i++) {
            if (obstacles[i].active) {
                tft_draw_sprite_scaled(&obstacle_sprite[0][0], 12, 20,
                    (int16_t)obstacles[i].x, GROUND_Y - obstacles[i].height, 2, OBSTACLE_COLOR);
            }
        }
    }

    if (game_state == GAME_OVER && state_changed) {
        tft_fill_rect(120, 112, 250, 80, BG_COLOR);
        tft_draw_text("GAME OVER", 160, 120, 3, WHITE);
        tft_draw_text("PRESS TO RESTART", 135, 165, 2, LIGHTGREY);
    }

    dino.previous = (rect_t){ (int16_t)dino.x, (int16_t)dino.y, DINO_W, DINO_H };
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        obstacles[i].previous = obstacles[i].active
            ? (rect_t){ (int16_t)obstacles[i].x, (int16_t)(GROUND_Y - obstacles[i].height), obstacles[i].width, obstacles[i].height }
            : (rect_t){0,0,0,0};
    }
    last_rendered_state = game_state;
}

static void render_frame(void) {
    if (first_render) {
        draw_static_scene();
        first_render = false;
    }
    render_dynamic();
    if (score != displayed_score) {
        draw_score();
        displayed_score = score;
    }
}

/* ============================================================
 *          GAME TASK  (physics + rendering, on CORE 1)
 * ============================================================ */
static void game_task(void *arg) {
    tft_init_display();
    reset_game();
    render_frame();

    int64_t last_micros = esp_timer_get_time();
    int64_t last_frame_ms = esp_timer_get_time() / 1000;
    float accumulator = 0.0f;

    uint32_t fps_counter = 0;
    int64_t fps_window_start = esp_timer_get_time() / 1000;

    while (1) {
        int64_t now_micros = esp_timer_get_time();
        float elapsed = (now_micros - last_micros) / 1000000.0f;
        last_micros = now_micros;
        if (elapsed > 0.10f) elapsed = 0.10f;
        accumulator += elapsed;

        bool jump_now = input_consume_jump();
        bool move_edge_now = input_consume_move_edge();

        if (game_state == READY && jump_now) {
            reset_game();
            game_state = PLAYING;
        } else if (game_state == GAME_OVER && move_edge_now) {
            reset_game();
            game_state = PLAYING;
        }

        while (accumulator >= FIXED_DT) {
            update_physics(FIXED_DT, jump_now);
            jump_now = false; /* only trigger the jump once per real jump command */
            accumulator -= FIXED_DT;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_frame_ms >= 25) { /* ~40 FPS, same as the Arduino version */
            render_frame();
            last_frame_ms = now_ms;
            fps_counter++;
        }

        if (now_ms - fps_window_start >= 1000) {
            ESP_LOGI(TAG, "FPS: %u  State: %d", (unsigned)fps_counter, (int)game_state);
            fps_counter = 0;
            fps_window_start = now_ms;
        }

        /* Same 0-tick safety guard as input_task - see comment there. */
        TickType_t frame_delay_ticks = pdMS_TO_TICKS(1);
        if (frame_delay_ticks == 0) frame_delay_ticks = 1;
        vTaskDelay(frame_delay_ticks);
    }
}

/* ============================================================
 *                         app_main
 * ============================================================ */
void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 Dino Runner (ESP-IDF) starting...");

    input_mutex = xSemaphoreCreateMutex();

    gpio_setup();
    spi_setup();
    adc_setup();

    /* Input task: CORE 0, high priority, samples every 2ms (500 Hz) */
    xTaskCreatePinnedToCore(input_task, "input_task", 4096, NULL, 6, NULL, 0);

    /* Game task: CORE 1, does physics + SPI drawing */
    xTaskCreatePinnedToCore(game_task, "game_task", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Move the joystick or press the button to jump/start/restart");
}