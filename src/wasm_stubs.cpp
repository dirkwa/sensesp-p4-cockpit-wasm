// WASM stubs for the firmware modules that widget_factory.cpp links
// against. Each function satisfies the public interface declared in
// the corresponding firmware header (subject_registry.h,
// zone_registry.h, notifications_registry.h, net/sk_put.h) but
// substitutes in-process behavior for the SensESP / SignalK plumbing
// the firmware uses.
//
// What's preserved:
//   - The exact LVGL behavior (subjects feed values into lv_subject_*,
//     observers fire when values change, zone colors map via the same
//     color_for_state).
//   - The exact memory model (subjects survive layout swaps via the
//     same get_or_create cache).
//
// What's replaced:
//   - SensESP SKValueListener / SKPutRequest -> no-op (PUTs are routed
//     back to the JS bridge via puts_emit; SK reads come from
//     subject_registry::feed_value which the bridge calls when the
//     designer wants to simulate a new SK value).
//   - WS notifications.* delta consumer -> in-memory snapshot the JS
//     bridge populates from a JSON blob.
//   - Zone meta delivery -> JS-supplied table per path.

#include "subject_registry.h"
#include "zone_registry.h"
#include "notifications_registry.h"
#include "net/sk_put.h"

#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>

namespace jlp {

// ----- SubjectRegistry --------------------------------------------------

lv_subject_t* SubjectRegistry::get_or_create(const std::string& path,
                                             SubjectKind kind) {
  auto it = map_.find(path);
  if (it != map_.end()) {
    if (it->second.entry->kind != kind) return nullptr;
    return &it->second.entry->subject;
  }
  auto entry = std::make_unique<SubjectEntry>();
  entry->path = path;
  entry->kind = kind;
  std::memset(entry->str_buf,  0, sizeof(entry->str_buf));
  std::memset(entry->str_prev, 0, sizeof(entry->str_prev));
  switch (kind) {
    case SubjectKind::Float:
      lv_subject_init_float(&entry->subject, 0.f);
      break;
    case SubjectKind::Int:
    case SubjectKind::Bool:
      lv_subject_init_int(&entry->subject, 0);
      break;
    case SubjectKind::String:
      lv_subject_init_string(&entry->subject,
                             entry->str_buf, entry->str_prev,
                             sizeof(entry->str_buf), "");
      break;
  }
  lv_subject_t* out = &entry->subject;
  map_.emplace(path, Slot{std::move(entry)});
  return out;
}

lv_subject_t* SubjectRegistry::lookup(const std::string& path) const {
  auto it = map_.find(path);
  if (it == map_.end()) return nullptr;
  return const_cast<lv_subject_t*>(&it->second.entry->subject);
}

std::vector<std::string> SubjectRegistry::paths() const {
  std::vector<std::string> out;
  out.reserve(map_.size());
  for (const auto& kv : map_) out.push_back(kv.first);
  return out;
}

void SubjectRegistry::garbage_collect(const std::set<std::string>& live) {
  for (auto it = map_.begin(); it != map_.end();) {
    if (live.count(it->first) == 0) it = map_.erase(it);
    else ++it;
  }
}

SubjectRegistry& registry() {
  static SubjectRegistry r;
  return r;
}

// ----- ZoneRegistry -----------------------------------------------------

void ZoneRegistry::hook_sk_ws() { /* no SK in WASM */ }

const Zone* ZoneRegistry::match(const std::string& path,
                                float raw_value) const {
  auto it = map_.find(path);
  if (it == map_.end()) return nullptr;
  for (const auto& z : it->second) {
    if (raw_value >= z.lower && raw_value <= z.upper) return &z;
  }
  return nullptr;
}

const std::string& ZoneRegistry::description(const std::string& path) const {
  static const std::string empty;
  auto it = descriptions_.find(path);
  return it == descriptions_.end() ? empty : it->second;
}

void ZoneRegistry::apply_meta(const std::string& path,
                              const JsonObjectConst& meta) {
  std::vector<Zone> zones;
  JsonArrayConst arr = meta["zones"];
  if (!arr.isNull()) {
    for (JsonObjectConst z : arr) {
      Zone zone;
      zone.lower = z["lower"] | -1e30f;
      zone.upper = z["upper"] |  1e30f;
      const char* s = z["state"] | "nominal";
      if      (!std::strcmp(s, "alert"))     zone.state = ZoneState::Alert;
      else if (!std::strcmp(s, "warn"))      zone.state = ZoneState::Warn;
      else if (!std::strcmp(s, "warning"))   zone.state = ZoneState::Warn;
      else if (!std::strcmp(s, "alarm"))     zone.state = ZoneState::Alarm;
      else if (!std::strcmp(s, "emergency")) zone.state = ZoneState::Emergency;
      else if (!std::strcmp(s, "normal"))    zone.state = ZoneState::Normal;
      else                                   zone.state = ZoneState::Nominal;
      zones.push_back(zone);
    }
  }
  map_[path] = std::move(zones);
  const char* desc = meta["description"] | (const char*)nullptr;
  if (desc) descriptions_[path] = desc;
}

ZoneRegistry& zones() {
  static ZoneRegistry r;
  return r;
}

uint32_t color_for_state(ZoneState s) {
  // Maritime-helm palette: one step warmer than SK spec defaults.
  // KEEP IN LOCKSTEP with firmware zone_registry.cpp:color_for_state.
  switch (s) {
    case ZoneState::Nominal:
    case ZoneState::Normal:    return 0x3fb950;  // green
    case ZoneState::Alert:     return 0xd29922;  // yellow
    case ZoneState::Warn:      return 0xfb8500;  // orange
    case ZoneState::Alarm:     return 0xf85149;  // red
    case ZoneState::Emergency: return 0xa371f7;  // purple
  }
  return 0x8b949e;
}

// ----- NotificationsRegistry --------------------------------------------

void NotificationsRegistry::hook_sk_ws() { /* no SK in WASM */ }

void NotificationsRegistry::apply(const std::string& path_after_prefix,
                                  const JsonVariantConst& value) {
  if (value.isNull()) {
    map_.erase(path_after_prefix);
  } else {
    Notification n;
    n.path = path_after_prefix;
    n.message = (const char*)(value["message"] | "");
    n.state = parse_not_state(value["state"] | "normal");
    map_[path_after_prefix] = std::move(n);
  }
  fire_observers();
}

const Notification* NotificationsRegistry::most_severe() const {
  const Notification* best = nullptr;
  uint8_t best_sev = 0;
  for (const auto& kv : map_) {
    if (acked_.count(kv.first)) continue;
    uint8_t sev = (uint8_t)kv.second.state;
    if (sev > best_sev) { best_sev = sev; best = &kv.second; }
  }
  return best;
}

std::vector<Notification>
NotificationsRegistry::snapshot(bool include_cleared) const {
  std::vector<Notification> out;
  out.reserve(map_.size());
  for (const auto& kv : map_) {
    if (acked_.count(kv.first)) continue;
    if (!include_cleared) {
      if (kv.second.state == NotState::Normal ||
          kv.second.state == NotState::Nominal) continue;
    }
    out.push_back(kv.second);
  }
  // Sort by severity descending so the worst floats to the top —
  // mirrors firmware behaviour.
  std::sort(out.begin(), out.end(),
            [](const Notification& a, const Notification& b) {
              return (uint8_t)a.state > (uint8_t)b.state;
            });
  return out;
}

void NotificationsRegistry::acknowledge(const std::string& path_after_prefix) {
  auto it = map_.find(path_after_prefix);
  if (it == map_.end()) return;
  acked_[path_after_prefix] = it->second.state;
  fire_observers();
}

bool NotificationsRegistry::is_acknowledged(
    const std::string& path_after_prefix) const {
  return acked_.count(path_after_prefix) > 0;
}

void NotificationsRegistry::fire_observers() {
  for (auto& s : observers_) {
    if (s.cb) s.cb();
  }
}

NotificationsRegistry& notifications() {
  static NotificationsRegistry r;
  return r;
}

// ----- sk_put -----------------------------------------------------------
// In firmware these PUT to the SignalK server. In WASM there's no SK to
// talk to — we just remember the last PUT per path so the JS bridge
// can surface "what would the device send right now?" for the
// inspector. A future iteration could call back into JS via
// EM_ASM/EM_JS to forward the PUT to the connected device.

namespace {
struct LastPut { std::string kind; std::string value; };
std::map<std::string, LastPut>& last_puts() {
  static std::map<std::string, LastPut> m;
  return m;
}
}  // namespace

void put_bool(const std::string& p, bool v) {
  last_puts()[p] = {"bool", v ? "true" : "false"};
}
void put_int(const std::string& p, int v) {
  last_puts()[p] = {"int", std::to_string(v)};
}
void put_float(const std::string& p, float v) {
  last_puts()[p] = {"float", std::to_string(v)};
}
void put_string(const std::string& p, const std::string& v) {
  last_puts()[p] = {"string", v};
}
void put_notification_ack(const std::string& path_after_prefix) {
  // In firmware this constructs an inbound SK delta. In WASM we
  // mirror the local-ack behavior so the alert overlay dismisses.
  notifications().acknowledge(path_after_prefix);
}

}  // namespace jlp
