#ifndef GAME_H
#define GAME_H

/* Starts the game task (physics + rendering) pinned to Core 1.
 * Call this once from app_main(), after the drivers are set up. */
void game_driver_start_task(void);

#endif /* GAME_H */
