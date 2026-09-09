/* ============================================================
 * ESP32-S3 DINO RUNNER -- entry point only.
 *
 * This file does NOT contain game logic or driver code anymore.
 * It just sets things up and starts the two tasks:
 *   - joystick_driver.c  -> reads the joystick, on Core 0
 *   - game.c             -> physics + drawing,   on Core 1
 *
 * Want to change how the dino jumps? Edit game.c.
 * Want to change how the joystick is read? Edit joystick_driver.c.
 * Want to change how the screen is drawn? Edit tft_driver.c.
 * You should almost never need to touch this file again.
 * ============================================================ */

#include "esp_log.h"
#include "tft_driver.h"
#include "joystick_driver.h"
#include "game.h"

static const char *TAG = "DINO";

void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 Dino Runner (ESP-IDF) starting...");

    joystick_driver_init();
    tft_driver_init_gpio();
    tft_driver_init_spi();

    joystick_driver_start_task();
    game_driver_start_task();

    ESP_LOGI(TAG, "Move joystick or press button to jump/start/restart.");
    ESP_LOGI(TAG, "Push joystick DOWN to crouch under flying birds.");
}
