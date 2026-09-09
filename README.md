# ESP32-S3 Dino Runner (ESP-IDF)

An offline, no-touch-controller Dino Runner game for the ESP32-S3, built on
**pure ESP-IDF** (no Arduino framework). Controlled with a KY-023 style
analog joystick, drawn on an ILI9488 480x320 SPI TFT display.

This project started as an Arduino IDE sketch and was fully ported to
ESP-IDF, with an added focus on **fast, non-blocking joystick input** using
FreeRTOS's dual-core scheduling.

---

## Features

- Full Dino Runner gameplay: jump, dodge obstacles, increasing speed, score
- Runs entirely offline — no Wi-Fi, no touch controller needed
- Joystick input sampled on a **dedicated task pinned to Core 0**, at
  ~500 Hz, completely decoupled from screen rendering
- Screen rendering (Core 1) uses buffered row transfers over SPI instead of
  per-pixel transfers, for smoother drawing
- Debounced button input, hysteresis-based joystick edge detection
- Dirty-rectangle rendering (only redraws what changed, not the whole screen
  every frame)

---

## Hardware

| Component        | Notes                                   |
|-------------------|------------------------------------------|
| ESP32-S3 N16R8    | 16MB flash / 8MB PSRAM dev board          |
| ILI9488 TFT       | 480x320, SPI interface, RGB666 color mode |
| Analog joystick   | KY-023 style, with push-button switch     |

### Wiring

| Signal        | ESP32-S3 GPIO |
|----------------|----------------|
| TFT CS         | GPIO 10        |
| TFT DC         | GPIO 14        |
| TFT RST        | GPIO 21        |
| TFT MOSI       | GPIO 11        |
| TFT SCK        | GPIO 12        |
| TFT MISO       | GPIO 13        |
| Joystick VRX   | GPIO 1         |
| Joystick VRY   | GPIO 2         |
| Joystick SW    | GPIO 4         |
| Joystick VCC   | 3.3V           |
| Joystick GND   | GND            |

---

## Architecture

The Arduino version ran everything (input reading + screen drawing) in a
single `loop()`, one after another. This version splits that work across
the ESP32-S3's two CPU cores using FreeRTOS tasks:

- **Core 0 — `input_task`**: samples the joystick and button every 2ms
  (~500 times/sec), completely independent of what the display is doing.
  Jump/restart signals are stored in a mutex-protected shared struct.
- **Core 1 — `game_task`**: runs the fixed-timestep physics simulation
  (60 Hz) and renders frames to the display (~40 FPS), consuming input
  signals set by `input_task`.

This means joystick input is detected almost instantly, even while the
display is busy drawing a frame.

---

## Project structure

```
DINO/
├── CMakeLists.txt              # top-level ESP-IDF project file
├── sdkconfig.defaults          # optional default build settings
└── main/
    ├── CMakeLists.txt          # tells ESP-IDF which source files to build
    └── main.c                  # all game logic, display driver, input task
```

---

## Building and flashing

Requires the ESP-IDF toolchain (v5.4.1 used during development) and a
serial/USB connection to the board.

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <YOUR_COM_PORT> flash monitor
```

On first boot, leave the joystick untouched — the firmware calibrates the
joystick's center position automatically during startup.

---

## Controls

| Action                | Input                                  |
|------------------------|-----------------------------------------|
| Start game (from READY)| Move joystick in any direction, or press button |
| Jump (while playing)   | Move joystick in any direction, or press button |
| Restart (after Game Over) | Move joystick in any direction        |

---

## Known notes / things to keep in mind

- FreeRTOS's default tick rate is 10ms per tick. Any `vTaskDelay()` call
  requesting less than that (e.g. 1-2ms) rounds down to 0 ticks unless
  guarded — this project includes a safety check so a task never sleeps for
  0 ticks (which would otherwise starve the watchdog and crash the board).
- For an even faster true joystick sampling rate, raise
  **Component config → FreeRTOS → Kernel → Tick rate (Hz)** to 1000 via
  `idf.py menuconfig`.

---

## Credits

Built for a Mechatronics Engineering coursework/hardware project, ported
from an original Arduino IDE sketch to ESP-IDF.
