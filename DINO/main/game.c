/* ============================================================
 * GAME LOGIC
 * Everything about the dino, obstacles, physics, and drawing the
 * game state lives here. This file calls tft_driver.h to draw,
 * joystick_driver.h to read input, and buzzer_driver.h to play
 * sound — it never touches GPIO/SPI/ADC/PWM directly. That's the
 * whole point of splitting the files: to add a new obstacle or
 * tweak jump height, you only edit THIS file.
 * ============================================================ */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

#include "tft_driver.h"
#include "joystick_driver.h"
#include "buzzer_driver.h"
#include "sprites.h"
#include "game.h"

static const char *TAG = "GAME";

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

/* --- Crouch --- */
#define DINO_CROUCH_SCALE     2
#define DINO_CROUCH_H         (8 * DINO_CROUCH_SCALE)   /* 16 */
#define DINO_CROUCH_TOP       (GROUND_Y - DINO_CROUCH_H)

/* --- Flying bird obstacle ---
 * Birds keep the same size, but their vertical position changes.
 * LOW birds require crouching, MID birds require a timed jump, and
 * HIGH birds can be passed by simply running underneath. */
#define BIRD_SCALE          2
#define BIRD_RENDER_W       (20 * BIRD_SCALE)          /* 40 */
#define BIRD_RENDER_H       (12 * BIRD_SCALE)          /* 24 */
#define BIRD_LOW_TOP_Y      (GROUND_Y - 16 - BIRD_RENDER_H)
/* The middle bird's bottom is aligned with the top of the standing
 * dino, where the dino's face/head begins. It therefore blocks a
 * standing dino but leaves the crouching dino underneath it. */
#define BIRD_MID_TOP_Y      (DINO_GROUND_TOP - BIRD_RENDER_H)
#define BIRD_HIGH_TOP_Y     (GROUND_Y - 72 - BIRD_RENDER_H)
#define BIRD_FLAP_PERIOD_MS 150                        /* wing-flap animation speed */

/* Birds only start showing up once the game has been running a
 * while, and only some of the time. Tune these to change difficulty. */
#define BIRD_MIN_ELAPSED_SEC  8.0f
#define BIRD_SPAWN_ODDS       4   /* 1-in-N chance once eligible */

#define BG_COLOR       BLACK
#define GROUND_COLOR   WHITE
#define DINO_COLOR     WHITE
#define OBSTACLE_COLOR WHITE

typedef struct { int16_t x, y, w, h; } rect_t;

typedef struct {
    float x, y;
    float velocityY;
    bool jumping;
    bool crouching;
    rect_t previous;
} dino_t;

typedef enum { OBSTACLE_GROUND, OBSTACLE_BIRD } obstacle_type_t;
typedef enum { BIRD_LOW, BIRD_MID, BIRD_HIGH } bird_height_t;

typedef struct {
    float x;
    int16_t width, height;
    obstacle_type_t type;
    bird_height_t bird_height;
    bool active;
    rect_t previous;
} obstacle_t;

typedef enum { READY, PLAYING, GAME_OVER } game_state_t;

static dino_t dino;
static obstacle_t obstacles[MAX_OBSTACLES];
static game_state_t game_state = READY;

static uint32_t score = 0;
static uint32_t high_score = 0;
static uint32_t displayed_score = 0;
static float elapsed_game_seconds = 0.0f;
static bool first_render = true;
static game_state_t last_rendered_state = GAME_OVER;

/* ---------------- Helpers ---------------- */
static int rand_range(int lo, int hi) { /* random int in [lo, hi) */
    if (hi <= lo) return lo;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo));
}

static int16_t obstacle_top_y(const obstacle_t *o) {
    if (o->type == OBSTACLE_BIRD) {
        switch (o->bird_height) {
            case BIRD_LOW:  return BIRD_LOW_TOP_Y;
            case BIRD_MID:  return BIRD_MID_TOP_Y;
            case BIRD_HIGH: return BIRD_HIGH_TOP_Y;
        }
    }
    return (int16_t)(GROUND_Y - o->height);
}

static rect_t dino_rect(void) {
    int16_t h = dino.crouching ? DINO_CROUCH_H : DINO_H;
    rect_t r = { (int16_t)dino.x + 5, (int16_t)dino.y + 5, DINO_W - 10, h - 6 };
    return r;
}

static rect_t obstacle_rect(const obstacle_t *o) {
    int16_t top = obstacle_top_y(o);
    rect_t r = { (int16_t)o->x + 4, top + 3, o->width - 8, o->height - 3 };
    return r;
}

static bool intersects(rect_t a, rect_t b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

/* ---------------- Game state ---------------- */
static void reset_game(void) {
    dino.x = DINO_X;
    dino.y = DINO_GROUND_TOP;
    dino.velocityY = 0;
    dino.jumping = false;
    dino.crouching = false;
    dino.previous = (rect_t){ (int16_t)dino.x, (int16_t)dino.y, DINO_W, DINO_H };

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        obstacles[i].active = false;
        obstacles[i].previous = (rect_t){0,0,0,0};
    }
    obstacles[0].active = true;
    obstacles[0].type = OBSTACLE_GROUND;
    obstacles[0].x = 430;
    obstacles[0].width = 24;
    obstacles[0].height = 40;
    obstacles[0].previous = (rect_t){ (int16_t)obstacles[0].x, obstacle_top_y(&obstacles[0]),
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

            bool can_be_bird = elapsed_game_seconds > BIRD_MIN_ELAPSED_SEC;
            bool make_bird = can_be_bird && (rand_range(0, BIRD_SPAWN_ODDS) == 0);

            if (make_bird) {
                obstacles[i].type   = OBSTACLE_BIRD;
                obstacles[i].width  = BIRD_RENDER_W;
                obstacles[i].height = BIRD_RENDER_H;
                /* Weighted positions: high birds are rare, middle birds
                 * are the most common, and low birds keep their normal
                 * chance. The bird size is not changed. */
                int bird_roll = rand_range(0, 10);
                obstacles[i].bird_height = (bird_roll == 0) ? BIRD_HIGH
                    : (bird_roll < 7 ? BIRD_MID : BIRD_LOW);
            } else {
                obstacles[i].type   = OBSTACLE_GROUND;
                obstacles[i].width  = 24;
                obstacles[i].height = (rand_range(0, 3) == 0) ? 52 : 40;
            }

            /* minX keeps every obstacle (bird included) spawning fully
             * off the right edge of the 480px-wide screen, so it always
             * enters by flying/scrolling in from the right. */
            float minX = 510.0f;
            float candidate = furthest + (float)rand_range(175, 275);
            obstacles[i].x = candidate > minX ? candidate : minX;
            obstacles[i].previous = (rect_t){ (int16_t)obstacles[i].x, obstacle_top_y(&obstacles[i]),
                                               obstacles[i].width, obstacles[i].height };
            return;
        }
    }
}

/* jump_flag: true if the input task detected a jump command this tick
 * crouch_held: true if the joystick is currently pushed down */
static void update_physics(float dt, bool jump_flag, bool crouch_held) {
    if (game_state != PLAYING) return;

    elapsed_game_seconds += dt;
    /* Ten times the original score rate: 500 points per second. */
    score = (uint32_t)(elapsed_game_seconds * 500.0f);
    if (score > high_score) high_score = score;

    bool on_ground = !dino.jumping;

    /* Crouching only works while standing on the ground — you can't
     * duck mid-air in this version. Beep once on the rising edge
     * (the moment you start crouching), not every frame it's held. */
    bool new_crouching = on_ground && crouch_held;
    if (new_crouching && !dino.crouching) {
        buzzer_play_crouch();
    }
    dino.crouching = new_crouching;

    if (dino.crouching) {
        dino.y = DINO_CROUCH_TOP;
        dino.velocityY = 0;
    } else {
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
            buzzer_play_jump();
        }
    }

    game_state_t state_before_collisions = game_state;

    float speed = current_speed();
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (!obstacles[i].active) continue;
        obstacles[i].x -= speed * dt;
        if (obstacles[i].x + obstacles[i].width < 0) obstacles[i].active = false;
        if (intersects(dino_rect(), obstacle_rect(&obstacles[i]))) game_state = GAME_OVER;
    }

    if (game_state == GAME_OVER && state_before_collisions != GAME_OVER) {
        buzzer_play_game_over();
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

static void draw_game_over_panel(void) {
    char score_text[7];
    char high_text[7];
    uint32_t shown_score = score > 999999 ? 999999 : score;
    uint32_t shown_high = high_score > 999999 ? 999999 : high_score;

    for (int i = 5; i >= 0; i--) {
        score_text[i] = '0' + (shown_score % 10);
        shown_score /= 10;
        high_text[i] = '0' + (shown_high % 10);
        shown_high /= 10;
    }
    score_text[6] = '\0';
    high_text[6] = '\0';

    tft_fill_rect(105, 100, 275, 112, BG_COLOR);
    tft_draw_text("GAME OVER", 160, 108, 3, WHITE);
    tft_draw_text("SCORE", 130, 145, 2, LIGHTGREY);
    tft_draw_text(score_text, 250, 145, 2, WHITE);
    tft_draw_text("HIGH SCORE", 130, 163, 2, LIGHTGREY);
    tft_draw_text(high_text, 250, 163, 2, WHITE);
    tft_draw_text("PRESS TO RESTART", 135, 190, 2, LIGHTGREY);
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

    /* A new run must clear the complete previous screen. The game-over
     * panel is larger than the moving-object erase rectangles, so erasing
     * only the old sprite locations leaves stale text behind until a new
     * object happens to pass over it. Redraw the whole static scene when
     * leaving READY or GAME_OVER, before drawing the new frame. */
    if (state_changed && (last_rendered_state == GAME_OVER || last_rendered_state == READY)) {
        draw_static_scene();
    }

    if (game_state == READY) {
        tft_draw_sprite_scaled(&dino_sprite[0][0], 16, 16, DINO_X, DINO_GROUND_TOP, 2, DINO_COLOR);
        if (state_changed) {
            tft_fill_rect(120, 112, 250, 80, BG_COLOR);
            tft_draw_text("PRESS JOYSTICK", 145, 125, 2, WHITE);
            tft_draw_text("UP OR BUTTON", 155, 145, 2, LIGHTGREY);
        }
    } else {
        if (dino.crouching) {
            tft_draw_sprite_scaled(&dino_crouch_sprite[0][0], 16, 8,
                (int16_t)dino.x, (int16_t)dino.y, DINO_CROUCH_SCALE, DINO_COLOR);
        } else {
            tft_draw_sprite_scaled(&dino_sprite[0][0], 16, 16,
                (int16_t)dino.x, (int16_t)dino.y, 2, DINO_COLOR);
        }

        /* Flip between the two bird frames on a fixed timer so it
         * reads as flapping wings instead of a static image sliding
         * across the screen. */
        bool wing_up = ((esp_timer_get_time() / 1000) % (BIRD_FLAP_PERIOD_MS * 2))
                       < BIRD_FLAP_PERIOD_MS;
        const uint8_t *bird_frame = wing_up ? &bird_sprite[0][0] : &bird_sprite_flap[0][0];

        for (int i = 0; i < MAX_OBSTACLES; i++) {
            if (!obstacles[i].active) continue;
            if (obstacles[i].type == OBSTACLE_BIRD) {
                tft_draw_sprite_scaled(bird_frame, 20, 12,
                    (int16_t)obstacles[i].x, obstacle_top_y(&obstacles[i]),
                    BIRD_SCALE, OBSTACLE_COLOR);
            } else {
                tft_draw_sprite_scaled(&obstacle_sprite[0][0], 12, 20,
                    (int16_t)obstacles[i].x, GROUND_Y - obstacles[i].height, 2, OBSTACLE_COLOR);
            }
        }
    }

    if (game_state == GAME_OVER && state_changed) {
        draw_game_over_panel();
    }

    int16_t dh = dino.crouching ? DINO_CROUCH_H : DINO_H;
    dino.previous = (rect_t){ (int16_t)dino.x, (int16_t)dino.y, DINO_W, dh };
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        obstacles[i].previous = obstacles[i].active
            ? (rect_t){ (int16_t)obstacles[i].x, obstacle_top_y(&obstacles[i]), obstacles[i].width, obstacles[i].height }
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
 * GAME TASK  (physics + rendering, runs on CORE 1)
 * ============================================================ */
static void game_task(void *arg) {
    tft_driver_init_display();
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

        bool jump_now = joystick_consume_jump();
        bool move_edge_now = joystick_consume_move_edge();
        bool crouch_now = joystick_is_crouch_held();

        if (game_state == READY && jump_now) {
            reset_game();
            game_state = PLAYING;
        } else if (game_state == GAME_OVER && move_edge_now) {
            reset_game();
            game_state = PLAYING;
        }

        while (accumulator >= FIXED_DT) {
            update_physics(FIXED_DT, jump_now, crouch_now);
            jump_now = false; /* only trigger the jump once per real jump command */
            accumulator -= FIXED_DT;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_frame_ms >= 25) { /* ~40 FPS */
            render_frame();
            last_frame_ms = now_ms;
            fps_counter++;
        }

        if (now_ms - fps_window_start >= 1000) {
            ESP_LOGI(TAG, "FPS: %u  State: %d", (unsigned)fps_counter, (int)game_state);
            fps_counter = 0;
            fps_window_start = now_ms;
        }

        TickType_t frame_delay_ticks = pdMS_TO_TICKS(1);
        if (frame_delay_ticks == 0) frame_delay_ticks = 1;
        vTaskDelay(frame_delay_ticks);
    }
}

void game_driver_start_task(void) {
    xTaskCreatePinnedToCore(game_task, "game_task", 8192, NULL, 5, NULL, 1);
}
