// input.h — libinput event pump for the WPE backend.
//
// Opens a udev libinput context on seat0, integrates into the GLib main loop
// via g_unix_fd_add, and forwards events to a callback that Phase 2's
// wpe_backend.cpp will wire into libwpe's input dispatch
// (wpe_view_backend_dispatch_keyboard_event / _pointer_event / _touch_event).
//
// Input coordinate transform: touch events arrive in panel-native
// millimeters. The callback receives *normalized* coords (0..1) after
// the rotation transform is applied, so WPE sees correct
// landscape-pixel-space.

#pragma once

#include <functional>
#include <memory>
#include <cstdint>

namespace electrobun {
namespace wpe {

enum class InputEventType {
    KeyDown,
    KeyUp,
    PointerMotion,
    PointerButtonDown,
    PointerButtonUp,
    PointerAxis,
    TouchDown,
    TouchUp,
    TouchMotion,
};

struct InputEvent {
    InputEventType type;
    uint32_t keycode;     // for Key*
    uint32_t button;      // for PointerButton*
    double x;             // normalized 0..1 for pointer/touch, after rotation
    double y;
    double dx;            // pointer motion delta
    double dy;
    int32_t touchSlot;    // for Touch*
    uint32_t timeMs;      // libinput event time (milliseconds)
};

using InputCallback = std::function<void(const InputEvent&)>;

struct InputDispatcherConfig {
    // Rotation applied to pointer/touch coordinates so WPE sees the logical
    // (landscape) coordinate space. Matches DrmDisplayConfig::rotation.
    int rotationQuarterTurns = 0;  // 0 = none, 1 = 90 CW, 2 = 180, 3 = 90 CCW
    // Logical (post-rotation) screen dimensions for pointer coordinate clamping.
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
};

class InputDispatcher {
public:
    InputDispatcher(const InputDispatcherConfig& cfg, InputCallback cb);
    ~InputDispatcher();

    // Start pumping events. Hooks into the calling thread's GLib main context
    // via g_unix_fd_add. Returns false on failure (e.g. libinput couldn't
    // open seat0 — probably permissions).
    bool start();

    // Stop pumping. Safe to call from any thread.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wpe
} // namespace electrobun
