// WASM/emscripten shim. notifications_registry.h (pulled unmodified
// from the firmware, per this repo's no-source-forks rule) includes
// freertos/FreeRTOS.h + semphr.h but uses none of their types in the
// header — the real FreeRTOS dependency lives in the firmware .cpp,
// which this preview doesn't compile. An empty header satisfies the
// include on a host toolchain that has no FreeRTOS.
#pragma once
