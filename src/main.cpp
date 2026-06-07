// JLP WASM entry point. Mirrors the firmware's layout-apply pipeline
// (parse JSON, build under a staging parent, swap atomically) but
// runs in the browser instead of on the P4. The widget LVGL code path
// is the firmware's widget_factory.cpp compiled byte-for-byte.
//
// JS bridge:
//   jlp_init(w, h)                — set up LVGL + SDL window (canvas).
//   jlp_apply_layout(json)        — render the layout, return "" on
//                                   success or a UTF-8 error string.
//   jlp_set_value_float(p, v)     — push a simulated SK float.
//   jlp_set_value_int  (p, v)     — push a simulated SK int.
//   jlp_set_value_bool (p, b)     — push a simulated SK bool.
//
// We deliberately don't import LayoutManager itself: the firmware's
// version pulls in LittleFS persistence and the post-swap WS restart
// hook, neither of which makes sense in a browser. The pipeline here
// is the same shape — staging parent, build, atomic swap, delete old
// — but everything else is intentionally trimmed.

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/bind.h>

#include <string>

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include <ArduinoJson.h>

#include "subject_registry.h"
#include "widgets/widget_factory.h"

static lv_display_t* disp = nullptr;
static lv_obj_t*     current_root = nullptr;

static EM_BOOL frame(double t, void*) {
  (void)t;
  lv_tick_inc(16);
  lv_timer_handler();
  return EM_TRUE;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int jlp_init(int w, int h) {
  if (disp) return 0;  // idempotent
  lv_init();
  disp = lv_sdl_window_create(w, h);
  if (!disp) return -1;
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), LV_PART_MAIN);
  emscripten_request_animation_frame_loop(frame, nullptr);
  return 0;
}

EMSCRIPTEN_KEEPALIVE
const char* jlp_apply_layout(const char* json) {
  static std::string err_storage;
  err_storage.clear();
  if (!disp) { err_storage = "jlp_init not called"; return err_storage.c_str(); }

  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json);
  if (de) {
    err_storage = std::string("parse: ") + de.c_str();
    return err_storage.c_str();
  }

  lv_obj_t* screen = lv_screen_active();
  lv_obj_t* staging = lv_obj_create(screen);
  lv_obj_set_size(staging, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(staging, 0, 0);
  lv_obj_set_style_bg_opa(staging, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(staging, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(staging, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(staging, 0, LV_PART_MAIN);
  lv_obj_clear_flag(staging, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(staging, LV_OBJ_FLAG_HIDDEN);

  // Pull the first screen's widgets only — the designer's WASM canvas
  // is single-screen by definition (the tab strip lives outside the
  // canvas surface). Switching tabs in the designer re-calls
  // jlp_apply_layout with the active screen's spec wrapped in the
  // same top-level shape.
  JsonArrayConst screens = doc["screens"];
  if (screens.isNull() || screens.size() == 0) {
    lv_obj_delete(staging);
    err_storage = "no screens";
    return err_storage.c_str();
  }
  JsonArrayConst widgets = screens[0]["widgets"];
  std::set<std::string> live_paths;
  jlp::BuildCtx ctx{ staging, jlp::registry(), live_paths };
  if (!widgets.isNull()) {
    for (JsonObjectConst spec : widgets) {
      std::string werr;
      lv_obj_t* w = jlp::build_widget(ctx, spec, &werr);
      if (!w) {
        lv_obj_delete(staging);
        err_storage = std::string("widget: ") + werr;
        return err_storage.c_str();
      }
    }
  }

  // Atomic swap: unhide, then delete the previous root.
  lv_obj_clear_flag(staging, LV_OBJ_FLAG_HIDDEN);
  if (current_root) lv_obj_delete(current_root);
  current_root = staging;

  return "";  // success
}

EMSCRIPTEN_KEEPALIVE
void jlp_set_value_float(const char* path, float v) {
  lv_subject_t* s = jlp::registry().lookup(path);
  if (!s) return;
  lv_subject_set_float(s, v);
}

EMSCRIPTEN_KEEPALIVE
void jlp_set_value_int(const char* path, int v) {
  lv_subject_t* s = jlp::registry().lookup(path);
  if (!s) return;
  lv_subject_set_int(s, v);
}

EMSCRIPTEN_KEEPALIVE
void jlp_set_value_bool(const char* path, int v) {
  lv_subject_t* s = jlp::registry().lookup(path);
  if (!s) return;
  lv_subject_set_int(s, v ? 1 : 0);
}

}  // extern "C"
