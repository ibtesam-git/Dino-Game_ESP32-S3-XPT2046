#ifndef BUZZER_DRIVER_H
#define BUZZER_DRIVER_H

#include <stdint.h>

/* ============================================================
 * PASSIVE BUZZER DRIVER
 *
 * Wiring (3-pin passive buzzer module: VCC / GND / S):
 *   VCC -> 3V3
 *   GND -> GND
 *   S   -> GPIO5   (change BUZZER_PIN in buzzer_driver.c if needed)
 *
 * These modules have their own little driver transistor on board,
 * so the ESP32 doesn't need to supply any current for the buzzer
 * itself — it only needs to put a PWM square wave on "S". The
 * frequency of that square wave IS the pitch you hear.
 *
 * Every function here is non-blocking: it just drops a request in a
 * queue, and a dedicated low-priority task (running on Core 0,
 * alongside the joystick task) actually plays the tone. That way a
 * beep never stalls the physics/render loop on Core 1.
 * ============================================================ */

/* Call once at startup, after joystick_driver_init(). Sets up the
 * PWM channel on the buzzer pin and starts the background task. */
void buzzer_driver_init(void);

/* Queue a single tone: freq_hz is the pitch, duration_ms is how long
 * it plays. Non-blocking — if the buzzer is busy and its queue is
 * already full, the request is dropped instead of blocking you. */
void buzzer_beep(uint16_t freq_hz, uint16_t duration_ms);

/* Convenience helpers wired up to the game's sound events. */
void buzzer_play_jump(void);
void buzzer_play_crouch(void);
void buzzer_play_game_over(void); /* short descending 3-note tune */

#endif /* BUZZER_DRIVER_H */
