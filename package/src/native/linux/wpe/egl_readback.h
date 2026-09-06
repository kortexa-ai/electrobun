// egl_readback.h — GPU compositor with a CPU readback output.
//
// WPEBackend-fdo's EGL export path keeps WebKit/WebGL on the GPU. We sample
// the exported EGLImages into one framebuffer and read it back once. When
// supported, the shader also applies the panel rotation and glReadPixels
// writes BGRA straight into the next DRM dumb buffer. This is intentionally
// the small Phase 3 bridge: a future zero-copy compositor can render the same
// layers directly into GBM scanout buffers.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace electrobun {
namespace wpe {

struct EglLayer {
    void* image = nullptr;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool transparent = false;
};

class EglReadback {
public:
    EglReadback();
    ~EglReadback();

    EglReadback(const EglReadback&) = delete;
    EglReadback& operator=(const EglReadback&) = delete;

    bool init(uint32_t width, uint32_t height, int rotationQuarters);

    // EGLDisplay, kept opaque here so EGL headers stay in the .cpp.
    void* display() const;

    // Draw layers in order (last = topmost), then expose a tightly packed
    // top-to-bottom BGRA buffer suitable for the existing DRM blitter.
    bool compose(const std::vector<EglLayer>& layers);
    const uint8_t* pixels() const;
    uint32_t stride() const;

    // Capability-gated fast path: apply the configured 0/90/180/270-degree
    // rotation in the shader and read BGRA directly into the native-orientation
    // DRM back buffer.
    bool canComposeToScanout() const;
    bool composeToScanout(
        const std::vector<EglLayer>& layers,
        uint8_t* pixels,
        uint32_t pitch,
        uint32_t width,
        uint32_t height);

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wpe
} // namespace electrobun
