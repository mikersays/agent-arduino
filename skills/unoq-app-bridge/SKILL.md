---
name: unoq-app-bridge
description: Build "Arduino Apps" on THIS Arduino UNO Q that connect Python on the Linux side (Qualcomm Dragonwing, Debian) to a sketch on the STM32 microcontroller via the RouterBridge RPC system. Use when the user wants the microcontroller and Linux/Python to work together — e.g. a web UI or AI model that controls hardware, sensors streamed to Python, or any feature needing networking, files, the GPU, or AI plus real-time I/O. For a pure microcontroller-only sketch, use the unoq-sketch skill instead.
---

# Arduino Apps: bridging Python (Linux) ↔ sketch (MCU)

The UNO Q's superpower is running **Python on the Linux side** and an **Arduino sketch on the
MCU** together, linked by `Arduino_RouterBridge` over RPC. The managing tool is
**`arduino-app-cli`** (the `arduino-app-cli.service` daemon is already running on this board).

Use this when the feature needs *both* worlds: real-time/precise I/O (MCU) **and** networking,
web UI, files, the GPU, or on-device AI (Linux/Python).

## App anatomy

```
my-app/
├── app.yaml              # name, icon, description, and "bricks" (capabilities)
├── python/
│   └── main.py           # runs on Linux; uses arduino.app_utils + Bridge
└── sketch/
    └── sketch.ino        # runs on the MCU; uses Arduino_RouterBridge
```

`app.yaml` (verified shape from on-device examples):
```yaml
name: My App
icon: 💡
description: What it does

bricks:                   # optional capabilities provided by the platform
  - arduino:web_ui                    # serves a web interface
  - arduino:vlm:                      # vision-language model
      model: genie:qwen2_5_vl_7b_instruct
  - arduino:dbstorage_sqlstore        # SQL storage
```
Browse real bricks/examples on the device: `/var/lib/arduino-app-cli/examples/`.

## Managing apps (on-device CLI)

```bash
arduino-app-cli app new            # scaffold a new app (interactive)
arduino-app-cli app list           # list apps
arduino-app-cli app start  <name>  # build sketch + run python (also: restart, stop)
arduino-app-cli app logs   <name>  # tail the python logs
arduino-app-cli monitor            # attach to the MCU serial monitor
arduino-app-cli app import/export  # share apps as zip
```
Starting an app compiles & flashes the `sketch/` to the MCU and launches `python/main.py`.

## The Bridge RPC model

**One side `provide`s a function; the other side `call`s it by name.** Either direction works,
plus `notify` (fire-and-forget) for events.

### MCU sketch — expose functions to Python
```cpp
#include <Arduino_RouterBridge.h>

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Bridge.begin();
  Bridge.provide("set_led_state", set_led_state);   // Python can now call this
}

void loop() { }

void set_led_state(bool state) {
  digitalWrite(LED_BUILTIN, state ? LOW : HIGH);     // active-low
}
```

### Python (Linux) — call the MCU
```python
from arduino.app_utils import *
import time

state = False
def loop():
    global state
    time.sleep(1)
    state = not state
    Bridge.call("set_led_state", state)

App.run(user_loop=loop)
```

Python can also `Bridge.provide(...)` functions for the sketch to `Bridge.call(...)`, and either
side can `Bridge.notify(...)`. Provider arguments are typed and serialized for you — sketches can
receive `bool`, `int`, `std::vector<uint8_t>`, `std::array<uint32_t, N>`, etc.

See `examples/led-toggle/` for a complete, minimal bridged app.

## CRITICAL: concurrency on the MCU side

**Bridge providers run on a separate Zephyr thread from `loop()`.** Any state — or hardware like
the LED matrix — shared between a provider and `loop()` MUST be protected with a Zephyr mutex,
or you get races and corrupted output.

```cpp
#include <zephyr/kernel.h>
K_MUTEX_DEFINE(state_mtx);

void draw(std::vector<uint8_t> frame) {
  k_mutex_lock(&state_mtx, K_FOREVER);
  matrix.draw(frame.data());        // frame must be 104 bytes for the 8x13 matrix
  k_mutex_unlock(&state_mtx);
}
```
The on-device `led-matrix-painter` example
(`/var/lib/arduino-app-cli/examples/led-matrix-painter/sketch/sketch.ino`) is the canonical
reference for a mutex-guarded, bridge-driven LED matrix.

## Rules
1. **Provider thread ≠ loop thread** → mutex-guard all shared state and hardware.
2. **`Serial` already uses the bridge** on this board — don't fight it; use `arduino-app-cli
   monitor` to read it.
3. LED matrix buffers are **104 bytes** (8×13). See the unoq-sketch skill's `led-matrix.md`.
4. Keep `loop()` fast; do timing-sensitive work with `millis()` scheduling, not long `delay()`s,
   so providers stay responsive.
5. Pure-MCU project with no Linux/Python need? Use **unoq-sketch** instead — simpler.
