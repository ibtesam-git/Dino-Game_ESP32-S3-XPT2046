/* ============================================================
 * JOYSTICK / BUTTON INPUT  (fast task, runs on CORE 0)
 *
 * This file only cares about "what is the joystick doing right now".
 * It never touches the screen and never talks to game.c directly —
 * game.c just calls the functions in joystick_driver.h to ask.
 *
 * Three things this file tracks:
 *   - jump_pending      -> one-shot: "a jump/start/restart just happened"
 *   - move_edge_pending -> one-shot: "the joystick just moved to a new spot"
 *   - crouch_held       -> continuous: "is it being pushed down right now"
 * ============================================================ */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "joystick_driver.h"

static const char *TAG = "JOYSTICK";

/* On ESP32-S3, ADC1 channel 0 = GPIO1, channel 1 = GPIO2 */
#define JOY_X_ADC_CHANNEL ADC_CHANNEL_0
#define JOY_Y_ADC_CHANNEL ADC_CHANNEL_1

/* If your ESP-IDF is older and doesn't know ADC_ATTEN_DB_12,
 * change this to ADC_ATTEN_DB_11 instead. */
#define JOY_ADC_ATTEN ADC_ATTEN_DB_12

/* --- Tuning values ---
 * ENTER_TH / EXIT_TH: how far from center counts as "moved" (with a bit
 * of hysteresis so it doesn't flicker on/off near the edge).
 * CROUCH_TH: how far DOWN counts as "crouching". If crouch feels like
 * it triggers too easily or not easily enough, change this number.
 * JOY_DOWN_SIGN: if pushing the stick DOWN makes the dino jump instead
 * of crouch (i.e. it's backwards for your specific joystick), just
 * flip this from 1 to -1. */
#define ENTER_TH   260
#define EXIT_TH    140
#define CROUCH_TH  150
#define JOY_DOWN_SIGN 1

#define BUTTON_DEBOUNCE_US 8000  /* 8ms */

static adc_oneshot_unit_handle_t adc_handle;

typedef struct {
    bool jump_pending;
    bool move_edge_pending;
    bool crouch_held;
} shared_input_t;

static shared_input_t g_input = {0};
static SemaphoreHandle_t input_mutex;

static inline int read_adc(adc_channel_t ch) {
    int val = 0;
    adc_oneshot_read(adc_handle, ch, &val);
    return val;
}

bool joystick_consume_jump(void) {
    bool v;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    v = g_input.jump_pending;
    g_input.jump_pending = false;
    xSemaphoreGive(input_mutex);
    return v;
}

bool joystick_consume_move_edge(void) {
    bool v;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    v = g_input.move_edge_pending;
    g_input.move_edge_pending = false;
    xSemaphoreGive(input_mutex);
    return v;
}

bool joystick_is_crouch_held(void) {
    bool v;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    v = g_input.crouch_held;
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
        int down_amount = JOY_DOWN_SIGN * dy;

        /* --- crouch: continuous, checked every loop, no memory needed --- */
        bool crouch_now = (down_amount > CROUCH_TH) && (abs(down_amount) > abs(dx));

        /* --- jump / restart: left, right, or UP only. Down is reserved
         * for crouching, so pushing down no longer triggers a jump. --- */
        int th = prev_moved ? EXIT_TH : ENTER_TH;
        bool horizontal = (abs(dx) > th) && (abs(dx) >= abs(dy));
        bool pushed_up  = (down_amount < -th) && (abs(dy) > abs(dx));
        bool moved = horizontal || pushed_up;

        int8_t dir = 0;
        if (horizontal) dir = (dx >= 0) ? 1 : 2;
        else if (pushed_up) dir = 4;

        bool move_edge = moved && (!prev_moved || dir != prev_dir);
        bool jump_edge = move_edge || (button && !prev_button);

        xSemaphoreTake(input_mutex, portMAX_DELAY);
        if (jump_edge) g_input.jump_pending = true;
        if (move_edge) g_input.move_edge_pending = true;
        g_input.crouch_held = crouch_now;
        xSemaphoreGive(input_mutex);

        prev_moved = moved;
        prev_dir = moved ? dir : 0;
        prev_button = button;

        /* Safety: on default settings 1 FreeRTOS tick = 10ms, so
         * pdMS_TO_TICKS(2) rounds DOWN to 0 ticks, which means "don't
         * sleep at all" and crashes the board. Never let that happen. */
        TickType_t delay_ticks = pdMS_TO_TICKS(2);
        if (delay_ticks == 0) delay_ticks = 1;
        vTaskDelay(delay_ticks);
    }
}

void joystick_driver_init(void) {
    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << JOY_SW_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sw_cfg);

    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t ch_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = JOY_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_X_ADC_CHANNEL, &ch_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_Y_ADC_CHANNEL, &ch_cfg));

    input_mutex = xSemaphoreCreateMutex();
}

void joystick_driver_start_task(void) {
    xTaskCreatePinnedToCore(input_task, "input_task", 4096, NULL, 6, NULL, 0);
}