#ifndef COLORS_H
#define COLORS_H

#include <stdint.h>

/* ============================================================
 * COLOR PALETTE (RGB565)
 *
 * One place to tune the whole game's look. This is a "desert
 * sunset" palette — a handful of warm colors that were picked to
 * sit well together, instead of a full rainbow. Every entity in
 * the game gets its own distinct color so it reads clearly against
 * the sky, but nothing clashes.
 *
 * Want to retheme the game later (e.g. a night level)? This is the
 * only file you should need to touch — everything else just asks
 * for these names.
 * ============================================================ */

/* ---- Basics ---- */
#define COLOR_BLACK          0x0000
#define COLOR_WHITE          0xFFFF

/* ---- World ----
 * COLOR_SKY doubles as the "clear/erase" color: every dynamic
 * sprite (dino, obstacles) is erased each frame by repainting its
 * old rectangle in this color, so it must match the background. */
#define COLOR_SKY            0xFEB5   /* soft peach sunset */
#define COLOR_SAND           0xDDB0   /* warm tan ground band */
#define COLOR_GROUND_LINE    0x6224   /* dark brown horizon line */

/* ---- Characters & obstacles (each one gets its own hue) ---- */
#define COLOR_DINO           0x554A   /* medium green */
#define COLOR_CACTUS         0x2326   /* dark forest green */
#define COLOR_BIRD           0xE2C7   /* burnt orange-red */

/* ---- Text / HUD ---- */
#define COLOR_TEXT_DARK      0x3944   /* dark chocolate-brown, main text */
#define COLOR_TEXT_MUTED     0x7AC9   /* muted taupe, subtitle text */
#define COLOR_SCORE          0xFE40   /* gold, makes the score number pop */
#define COLOR_DANGER         COLOR_BIRD /* "GAME OVER" reuses the bird's color */

#endif /* COLORS_H */
