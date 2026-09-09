#ifndef JOYSTICK_DRIVER_H
#define JOYSTICK_DRIVER_H

#include <stdbool.h>

#define JOY_SW_PIN 4

/* Call once at startup, before starting the task. Sets up the ADC
 * pins and the button pin, and calibrates the joystick's center
 * position (keep your hands off the joystick while this runs). */
void joystick_driver_init(void);

/* Starts the background task that reads the joystick 500 times a
 * second on Core 0. Call this once, after joystick_driver_init(). */
void joystick_driver_start_task(void);

/* Returns true ONCE for a jump/start/restart action, then clears
 * itself. Call this once per game frame. */
bool joystick_consume_jump(void);

/* Returns true ONCE when the joystick moves to a new direction
 * (used to detect "restart" after game over). */
bool joystick_consume_move_edge(void);

/* Returns true for as long as the joystick is held DOWN.
 * Unlike the two functions above, this does NOT clear itself —
 * it always reflects what the joystick is doing right now. */
bool joystick_is_crouch_held(void);

#endif /* JOYSTICK_DRIVER_H */
