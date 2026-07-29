// drm_display.h — DRM/KMS output for the WPE backend.
//
// Opens a DRM device, finds a connected display, picks a mode, and provides
// a framebuffer for the compositor to write into + a present() entry point
// that swaps scanout to the next frame.
//
// Replaces Cog's DRM scanout (which has a stride bug on non-1920px-wide
// panels — confirmed Phase 0 step 2). This implementation MUST use the
// pitch returned by drmModeAddFB2, never `width × bpp`.
//
// The display owns native-orientation scanout buffers. The EGL compositor
// normally applies panel rotation in its shader; the SHM path keeps a CPU
// rotation fallback.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../backend.h"

namespace electrobun {
namespace wpe {

enum class Rotation {
    None = 0,
    CW90 = 1,
    Rot180 = 2,
    CCW90 = 3,
};

struct DrmDisplayConfig {
    std::string cardPath = "/dev/dri/card1";  // Pi 5 has vc4 on card1
    uint32_t preferredWidth = 0;              // 0 = use preferred EDID mode
    uint32_t preferredHeight = 0;
    Rotation rotation = Rotation::None;       // applied in software during blit (Phase 2)
};

struct DrmFrame {
    uint8_t* pixels;      // CPU-writable BGRA (matches Vulkan VK_FORMAT_B8G8R8A8_SRGB)
    uint32_t width;       // logical width (after rotation)
    uint32_t height;      // logical height (after rotation)
    uint32_t pitch;       // BYTES per row in `pixels`. NEVER width * 4.
};

// Owns the DRM master, KMS buffers, and page-flip state for one connector.
// Intentionally single-display, single-plane for Phase 2. Multi-plane
// composition lives in Phase 4.
class DrmDisplay {
public:
    explicit DrmDisplay(const DrmDisplayConfig& cfg);
    ~DrmDisplay();

    // Initialize: open device, pick connector/crtc/mode, allocate dumb buffers.
    // Returns false on failure; call getLastError() for detail.
    bool init();

    // Logical resolution (post-rotation). Compositor should write this size.
    uint32_t logicalWidth() const;
    uint32_t logicalHeight() const;

    // Acquire the next writable frame buffer. Blocks until a buffer is
    // available (the previous present completed). Event-loop-integrated
    // callers should check flipPending() first and defer instead of
    // blocking — see fd()/handleEvents().
    DrmFrame acquire();

    // Submit the acquired frame for scanout. Non-blocking; returns when the
    // page-flip is queued (not when it completes). A subsequent acquire()
    // waits for completion.
    void present();

    // DRM device fd. Watch it for readability (page-flip completion events)
    // and call handleEvents() to drain them, so presentation never blocks
    // the caller's event loop.
    int fd() const;

    // True while a queued page flip has not completed — acquire() would block.
    bool flipPending() const;

    // Drain pending DRM events (page-flip completions). Call when fd() is
    // readable; blocks otherwise.
    void handleEvents();

    const std::string& getLastError() const;

private:
    // Implementation detail: opaque pimpl to keep libdrm headers out of
    // this header. drm_display.cpp includes xf86drm.h and friends.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wpe
} // namespace electrobun
