// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_mod.h"
#include "../native/src/runtime_atlas_state.h"
#include "../native/src/runtime_map.h"
#include "../native/src/runtime_menu_sound.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {
const BbNativeHostV1* host{};
}

void BbRuntimeBridgeSetHost(const BbNativeHostV1* value) { host = value; }

extern "C" int runtime_map_location(BbMapLocation* out) {
    if (!out || !host || !host->location) return 0;
    BbNativeLocationV1 value{};
    if (!host->location(&value)) return 0;
    *out = {value.x, value.y, value.z, value.yaw, value.block, value.valid};
    return 1;
}
extern "C" int runtime_map_menu_open(void) {
    return host && host->menu_open ? host->menu_open() : 0;
}
extern "C" int runtime_map_menu_state(BbMapMenu* out) {
    if (!out || !host || !host->menu_state) return 0;
    BbNativeMenuV1 value{};
    if (!host->menu_state(&value)) return 0;
    *out = {value.supported, value.top_level, value.selected};
    return 1;
}
extern "C" uint32_t runtime_map_confirm_button(void) {
    return host && host->confirm_button ? host->confirm_button() : 0;
}
extern "C" int runtime_atlas_flag(uint32_t id) {
    return host && host->event_flag ? host->event_flag(id) : -1;
}
extern "C" uintptr_t runtime_atlas_player_token(void) {
    return host && host->player_token ? host->player_token() : 0;
}
extern "C" int runtime_atlas_travel_request(const BbAtlasTravelDestination* value) {
    if (!value || !host || !host->travel_request) return 0;
    BbNativeTravelDestinationV1 destination{};
    destination.return_point = value->return_point;
    destination.unlock_flag = value->unlock_flag;
    std::copy(std::begin(value->on), std::end(value->on), std::begin(destination.on));
    std::copy(std::begin(value->off), std::end(value->off), std::begin(destination.off));
    destination.on_count = value->on_count;
    destination.off_count = value->off_count;
    std::copy(std::begin(value->arrival), std::end(value->arrival), std::begin(destination.arrival));
    return host->travel_request(&destination);
}
extern "C" int runtime_atlas_travel_status(void) {
    return host && host->travel_status ? host->travel_status() : -1;
}
extern "C" int runtime_atlas_travel_supported(void) {
    return host && host->travel_supported ? host->travel_supported() : 0;
}
extern "C" void runtime_audio_menu_event(int select) {
    if (host && host->menu_audio_event) host->menu_audio_event(select);
}
extern "C" void runtime_audio_menu_stats(BbMenuSoundStats* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!host || !host->menu_audio_stats) return;
    BbNativeMenuSoundStatsV1 value{};
    host->menu_audio_stats(&value);
    *out = {value.decoded_move, value.decoded_select, value.queued_move,
            value.queued_select, value.mixed_move, value.mixed_select, value.muted};
}
