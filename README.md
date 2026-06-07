# sensesp-p4-cockpit-wasm

LVGL + JLP widget factory compiled to WebAssembly. Renders the
**same `widget_factory.cpp`** the device runs, byte-for-byte, into
an HTML `<canvas>` via SDL2 (emscripten port).

Purpose: pixel-perfect designer preview without a connected device.
The designer can drop layout JSON in and get back the same pixels the
P4 panel would draw — no SVG approximation, no font drift, no
LVGL-version skew.

## Layout

- `src/main.cpp` — JS bridge (`jlp_init`, `jlp_apply_layout`,
  `jlp_set_value_*`).
- `src/wasm_stubs.cpp` — in-process implementations of the firmware's
  `SubjectRegistry`, `ZoneRegistry`, `NotificationsRegistry`,
  `net/sk_put.h`. Identical interfaces, no SensESP / SignalK
  plumbing — values come from JS via `jlp_set_value_*`.
- `src/lv_conf.h` — LVGL config mirroring the firmware's
  `sensesp-cockpit-display/lv_conf.h`. Anything pixel-affecting
  (color depth, theme, fonts) must stay in lockstep.
- `public/` — build output (`jlp_wasm.js`, `jlp_wasm.wasm`) + a
  standalone `index.html` smoke test.
- `CMakeLists.txt` — pulls LVGL + `widget_factory.cpp` from the
  firmware's local PIO checkout, so versions never drift.

## Build

```bash
source ~/emsdk/emsdk_env.sh
emcmake cmake -B build -S .
cmake --build build -j
```

Outputs `public/jlp_wasm.js` (175 KB) + `public/jlp_wasm.wasm`
(~1.6 MB).

## Smoke test

```bash
cd public && python3 -m http.server 8090
# open http://localhost:8090
```

The page calls `jlp_init(1024, 600)`, then `jlp_apply_layout(...)`
with a tiny 4-widget layout (arc, label, bar, button).

## JS API

```js
import createJlpWasm from './jlp_wasm.js'

const mod = await createJlpWasm({ canvas: document.querySelector('#mycanvas') })

mod._jlp_init(1024, 600)

// allocate UTF-8 for a C string
const layout = JSON.stringify({ schema: 1, name: 'x', screens: [{ ... }] })
const len = mod.lengthBytesUTF8(layout) + 1
const ptr = mod._malloc(len)
mod.stringToUTF8(layout, ptr, len)

const errPtr = mod._jlp_apply_layout(ptr)
const err = mod.UTF8ToString(errPtr)
mod._free(ptr)
if (err) console.error('apply failed:', err)
```

Feed simulated SignalK values:

```js
const path = 'tanks.fuel'
const p = mod._malloc(mod.lengthBytesUTF8(path) + 1)
mod.stringToUTF8(path, p, mod.lengthBytesUTF8(path) + 1)
mod._jlp_set_value_float(p, 0.73)  // 73 %
mod._free(p)
```

## Dependencies

The build pulls source from the firmware's PIO checkout:
- LVGL → `../sensesp-p4-cockpit/.pio/libdeps/p4_cockpit/lvgl`
- widget_factory.cpp → `../sensesp-p4-cockpit/src/jlp/widgets/widget_factory.cpp`
- ArduinoJson → `../sensesp-p4-cockpit/.pio/libdeps/p4_cockpit/ArduinoJson`

So **build the firmware once first** before building this:

```bash
cd ../sensesp-p4-cockpit && pio run -e p4_cockpit
```

## What this is not

- **Not a runtime.** No SK websocket, no PUTs, no notifications
  registry consumer. Stubs are in-process only.
- **Not pixel-perfect against arbitrary browsers.** Anti-aliasing,
  GPU compositor, and sub-pixel rounding can shift a pixel or two
  across Chrome/Firefox/Safari. But the LVGL output is identical to
  the device.
- **Not the only preview path.** The designer also has a "live
  mirror" mode that polls the actual device's `/screenshot?fmt=jpeg`
  for true WYSIWYG when a panel is connected. WASM is the
  offline-pixel-perfect fallback.

## Keeping in sync with firmware

When `widget_factory.cpp` changes upstream, this repo's build picks
it up automatically (same source path). But two things must be kept
in lockstep manually:

1. **`src/lv_conf.h`** vs **firmware `lv_conf.h`** —
   any pixel-affecting LVGL knob (color depth, theme, fonts,
   `LV_USE_FLOAT`) must match. Check whenever the firmware's
   `lv_conf.h` changes.
2. **`src/wasm_stubs.cpp::color_for_state`** must match firmware
   `zone_registry.cpp::color_for_state` — the maritime palette
   lives in two places.

Pixel-perfect parity depends on both.
