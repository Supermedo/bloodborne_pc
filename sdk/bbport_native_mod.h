/* SPDX-License-Identifier: GPL-2.0-or-later
 * bbport native-mod ABI v1. This header is C-compatible and is the only host
 * header a native mod needs. Append fields in future ABI revisions; never
 * reorder an existing structure. Every structure starts with its byte size.
 */
#ifndef BBPORT_NATIVE_MOD_H
#define BBPORT_NATIVE_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBPORT_NATIVE_MOD_API_V1 1u

typedef enum BbNativeLogLevelV1 {
    BB_NATIVE_LOG_INFO = 0,
    BB_NATIVE_LOG_WARNING = 1,
    BB_NATIVE_LOG_ERROR = 2
} BbNativeLogLevelV1;

typedef struct BbNativeLocationV1 {
    float x, y, z, yaw;
    uint32_t block;
    int32_t valid;
} BbNativeLocationV1;

typedef struct BbNativeMenuV1 {
    int32_t supported;
    int32_t top_level;
    int32_t selected;
} BbNativeMenuV1;

typedef struct BbNativeTravelDestinationV1 {
    int32_t return_point;
    uint32_t unlock_flag;
    uint32_t on[8], off[8];
    uint8_t on_count, off_count;
    uint8_t reserved[2];
    float arrival[3];
} BbNativeTravelDestinationV1;

typedef struct BbNativeMenuSoundStatsV1 {
    uint32_t decoded_move, decoded_select;
    uint32_t queued_move, queued_select;
    uint32_t mixed_move, mixed_select;
    uint32_t muted;
} BbNativeMenuSoundStatsV1;

/* Stable event envelope. Key, scancode, button and axis values are SDL3's
 * public numeric values for ABI v1; mods must ignore event kinds they do not
 * understand. Mouse coordinates are SDL window coordinates. */
typedef enum BbNativeEventKindV1 {
    BB_NATIVE_EVENT_NONE = 0,
    BB_NATIVE_EVENT_KEY_DOWN,
    BB_NATIVE_EVENT_KEY_UP,
    BB_NATIVE_EVENT_TEXT_INPUT,
    BB_NATIVE_EVENT_MOUSE_MOTION,
    BB_NATIVE_EVENT_MOUSE_BUTTON_DOWN,
    BB_NATIVE_EVENT_MOUSE_BUTTON_UP,
    BB_NATIVE_EVENT_MOUSE_WHEEL,
    BB_NATIVE_EVENT_WINDOW_FOCUS_GAINED,
    BB_NATIVE_EVENT_WINDOW_FOCUS_LOST,
    BB_NATIVE_EVENT_GAMEPAD_BUTTON_DOWN,
    BB_NATIVE_EVENT_GAMEPAD_BUTTON_UP,
    BB_NATIVE_EVENT_GAMEPAD_AXIS,
    BB_NATIVE_EVENT_GAMEPAD_ADDED,
    BB_NATIVE_EVENT_GAMEPAD_REMOVED
} BbNativeEventKindV1;

typedef struct BbNativeEventV1 {
    uint32_t struct_size;
    uint32_t kind;
    uint64_t timestamp_ns;
    uint32_t window_id;
    int32_t device_id;
    int32_t key;
    int32_t scancode;
    int32_t button;
    int32_t axis;
    int32_t axis_value;
    uint8_t repeat;
    uint8_t reserved[3];
    uint32_t modifiers;
    float x, y, xrel, yrel;
    float wheel_x, wheel_y;
    char text[32];
} BbNativeEventV1;

typedef struct BbNativeGraphicsV1 {
    uint32_t struct_size;
    uint32_t vulkan_api_version;
    uintptr_t instance;
    uintptr_t physical_device;
    uintptr_t device;
    uintptr_t queue;
    uint32_t queue_family;
    uint32_t color_format;
    uint32_t image_count;
    uint32_t reserved;
} BbNativeGraphicsV1;

typedef struct BbNativeRenderV1 {
    uint32_t struct_size;
    uint32_t width, height;
    uintptr_t command_buffer;
    uintptr_t image_view;
} BbNativeRenderV1;

typedef struct BbNativeHostV1 {
    uint32_t struct_size;
    uint32_t api_version;
    void (*log)(uint32_t level, const char *mod_id, const char *message);
    int (*location)(BbNativeLocationV1 *out);
    int (*menu_open)(void);
    int (*menu_state)(BbNativeMenuV1 *out);
    uint32_t (*confirm_button)(void);
    int (*event_flag)(uint32_t id);             /* -1 unknown, 0 off, 1 on */
    uintptr_t (*player_token)(void);
    int (*travel_request)(const BbNativeTravelDestinationV1 *destination);
    int (*travel_status)(void);
    int (*travel_supported)(void);
    void (*menu_audio_event)(int select);
    void (*menu_audio_stats)(BbNativeMenuSoundStatsV1 *out);
} BbNativeHostV1;

typedef struct BbNativeModV1 {
    uint32_t struct_size;
    uint32_t api_version;
    const char *id;
    const char *name;
    int (*initialize)(const BbNativeGraphicsV1 *graphics);
    void (*shutdown)(void);
    /* Present thread, before visibility is queried. No Vulkan commands here. */
    void (*tick)(int host_overlay_open);
    int (*visible)(void);
    /* Called inside a dynamic-rendering color pass whose attachment is the
     * supplied image view. The module records commands; it does not begin/end
     * rendering or submit the command buffer. */
    int (*render)(const BbNativeRenderV1 *render);
    int (*handle_event)(const BbNativeEventV1 *event);
    uint32_t (*route_pad)(uint32_t buttons, uint8_t lx, uint8_t ly,
                          uint8_t rx, uint8_t ry);
    int (*captures_input)(void);
    int (*releases_mouse)(void);
} BbNativeModV1;

/* The returned descriptor and every string it references remain owned by the
 * module until shutdown. Return NULL for an unsupported host API. */
typedef const BbNativeModV1 *(*BbNativeModQueryV1)(
    uint32_t host_api, const BbNativeHostV1 *host);

#if defined(_WIN32)
#define BBPORT_NATIVE_MOD_EXPORT __declspec(dllexport)
#else
#define BBPORT_NATIVE_MOD_EXPORT __attribute__((visibility("default")))
#endif

BBPORT_NATIVE_MOD_EXPORT const BbNativeModV1 *bbport_native_mod_query(
    uint32_t host_api, const BbNativeHostV1 *host);

#ifdef __cplusplus
}
#endif
#endif
