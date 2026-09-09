/* ============================================================
 * PASSIVE BUZZER DRIVER  (background task, runs on CORE 0)
 *
 * Uses the ESP32's LEDC peripheral to generate a PWM square wave on
 * the buzzer's "S" pin. LEDC is normally used for dimming LEDs, but
 * it's really just a hardware square-wave generator, which is
 * exactly what a passive buzzer needs to make a tone.
 *
 * A queue + dedicated task decouples sound from gameplay: game.c and
 * joystick_driver.c just call buzzer_play_xxx() and move on. This
 * task is the only thing that ever blocks/delays, and it does so on
 * Core 0, well away from the Core 1 render loop.
 * ============================================================ */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "buzzer_driver.h"

static const char *TAG = "BUZZER";

/* If your module's S pin needs to go somewhere else, change this. */
#define BUZZER_PIN            5

#define BUZZER_LEDC_TIMER     LEDC_TIMER_0
#define BUZZER_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BUZZER_LEDC_RES_BITS  LEDC_TIMER_10_BIT   /* duty range 0-1023 */
#define BUZZER_DUTY_ON        512                 /* ~50% = clean square wave */
#define BUZZER_DUTY_OFF       0

/* Silence between queued notes, so back-to-back beeps (like the
 * game-over tune) sound like separate notes instead of one blob. */
#define BUZZER_NOTE_GAP_MS    30

#define BUZZER_QUEUE_LEN      8

typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} buzzer_tone_t;

static QueueHandle_t buzzer_queue;

static inline TickType_t ms_to_ticks_safe(uint32_t ms) {
    /* Same safety trick used elsewhere in this project: on a 10ms
     * FreeRTOS tick, pdMS_TO_TICKS() of a short delay can round down
     * to 0, which means "don't wait at all". Never let that happen. */
    TickType_t t = pdMS_TO_TICKS(ms);
    return (t == 0) ? 1 : t;
}

static void buzzer_tone_on(uint16_t freq_hz) {
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_ON);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void buzzer_tone_off(void) {
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_OFF);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void buzzer_task(void *arg) {
    buzzer_tone_t tone;
    while (1) {
        if (xQueueReceive(buzzer_queue, &tone, portMAX_DELAY) == pdTRUE) {
            buzzer_tone_on(tone.freq_hz);
            vTaskDelay(ms_to_ticks_safe(tone.duration_ms));
            buzzer_tone_off();
            vTaskDelay(ms_to_ticks_safe(BUZZER_NOTE_GAP_MS));
        }
    }
}

void buzzer_beep(uint16_t freq_hz, uint16_t duration_ms) {
    buzzer_tone_t tone = { .freq_hz = freq_hz, .duration_ms = duration_ms };
    /* 0 tick timeout = never block the caller (e.g. the game task).
     * If the queue's full we just drop the beep. */
    xQueueSend(buzzer_queue, &tone, 0);
}

void buzzer_play_jump(void) {
    buzzer_beep(1200, 50);
}

void buzzer_play_crouch(void) {
    buzzer_beep(400, 60);
}

void buzzer_play_game_over(void) {
    /* Little descending 3-note "game over" jingle. */
    buzzer_beep(700, 120);
    buzzer_beep(500, 120);
    buzzer_beep(300, 200);
}

void buzzer_driver_init(void) {
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_LEDC_RES_BITS,
        .timer_num       = BUZZER_LEDC_TIMER,
        .freq_hz         = 1000,   /* placeholder; changed per-tone at play time */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {
        .gpio_num   = BUZZER_PIN,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel    = BUZZER_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BUZZER_LEDC_TIMER,
        .duty       = BUZZER_DUTY_OFF,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    buzzer_queue = xQueueCreate(BUZZER_QUEUE_LEN, sizeof(buzzer_tone_t));

    xTaskCreatePinnedToCore(buzzer_task, "buzzer_task", 2048, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "Buzzer ready on GPIO%d", BUZZER_PIN);
}
