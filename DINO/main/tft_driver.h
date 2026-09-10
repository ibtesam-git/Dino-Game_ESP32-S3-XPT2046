#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <stdint.h>

/* ---------------- Pins ---------------- */
#define TFT_CS   10
#define TFT_DC   14
#define TFT_RST  21
#define TFT_MOSI 11
#define TFT_CLK  12
#define TFT_MISO 13

#define TFT_WIDTH   480
#define TFT_HEIGHT  320

/* Colors are plain RGB565 values — see colors.h for the game's
 * actual palette. This driver doesn't care what a color means, it
 * just pushes whatever 16-bit value you give it to the screen. */

/* Call these once, in this order, before drawing anything:
 *   tft_driver_init_gpio();
 *   tft_driver_init_spi();
 *   tft_driver_init_display();
 */
void tft_driver_init_gpio(void);
void tft_driver_init_spi(void);
void tft_driver_init_display(void);

/* Fills a rectangle with one solid color. */
void tft_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/* Draws a 1-bit sprite (from sprites.h) scaled up by "scale".
 * "fg_color" paints the "1" pixels. "bg_color" paints the "0"
 * (transparent) pixels — pass your current background/sky color
 * here so sprites blend in instead of showing a black box. */
void tft_draw_sprite_scaled(const uint8_t *sprite, uint8_t sw, uint8_t sh,
                             int16_t x, int16_t y, uint8_t scale,
                             uint16_t fg_color, uint16_t bg_color);

/* Draws text using the built-in 5x7 font. */
void tft_draw_text(const char *text, int16_t x, int16_t y, uint8_t scale, uint16_t color);

#endif /* TFT_DRIVER_H */
