// drm_hello.cpp — standalone test that renders "Hello, Electrobun" using
// just DrmDisplay + Cairo. No WPE, no compositor, no X, no Wayland.
//
// Purpose: end-to-end proof of the DRM scanout path before we plug WPE in.
// If this renders clean text on the bar screen, DrmDisplay is correct
// (stride handling, page flip, mode set) and we can move on to WPE
// integration with confidence.
//
// Build: make -C .. (or: g++ ... see Makefile)
// Run:   sudo openvt -c 2 -s -f -- ./drm_hello
//        sudo chvt 1   # to return to tty1 after

#include "drm_display.h"

#include <cairo/cairo.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace electrobun::wpe;

static void paintFrame(const DrmFrame& frame, const char* text, double hueDegrees) {
    // Clear to a background color (cycling hue so consecutive frames are
    // distinguishable from a photo, and we get a visible pulse on screen).
    // HSV → RGB (manual, simple).
    double h = std::fmod(hueDegrees, 360.0) / 60.0;
    double c = 0.4;            // saturation × value = 0.4 (dim so text is readable)
    double x = c * (1 - std::fabs(std::fmod(h, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    if      (h < 1) { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else            { r = c; b = x; }

    uint32_t bg = (uint32_t)(0xFF << 24)
                | (uint32_t)(r * 255.0) << 16
                | (uint32_t)(g * 255.0) << 8
                | (uint32_t)(b * 255.0);

    for (uint32_t row = 0; row < frame.height; row++) {
        uint32_t* px = (uint32_t*)(frame.pixels + row * frame.pitch);
        for (uint32_t col = 0; col < frame.width; col++) px[col] = bg;
    }

    // Cairo surface wrapping our DRM buffer. ARGB32 stride matches our pitch.
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        frame.pixels, CAIRO_FORMAT_ARGB32,
        (int)frame.width, (int)frame.height, (int)frame.pitch);
    cairo_t* cr = cairo_create(surface);

    // The physical bar display is mounted landscape (1920 wide × 480 tall),
    // but KMS reports it as portrait 480×1920. Text drawn in the native
    // coordinate space is sideways; rotate 90° CCW so it reads correctly
    // in the physical (landscape) orientation.
    //
    // TODO(phase4): delete — Phase 4's composite shader absorbs rotation
    // as a sampler transform (see linux-wpe.md post-Phase-0 rotation note).
    bool portrait = frame.height > frame.width;

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);  // white
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, portrait ? 72.0 : 48.0);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);

    if (portrait) {
        cairo_translate(cr, frame.width / 2.0, frame.height / 2.0);
        cairo_rotate(cr, -M_PI / 2.0);                        // 90° CCW
        cairo_move_to(cr, -ext.width / 2.0 - ext.x_bearing,
                           ext.height / 2.0 - ext.y_bearing - ext.height);
    } else {
        cairo_move_to(cr, (frame.width  - ext.width)  / 2.0 - ext.x_bearing,
                          (frame.height - ext.height) / 2.0 - ext.y_bearing);
    }
    cairo_show_text(cr, text);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

int main(int argc, char** argv) {
    const char* text = (argc > 1) ? argv[1] : "Hello, Electrobun";

    DrmDisplayConfig cfg{};
    // Rotation is handled by the manual Cairo transform above; don't ask
    // DrmDisplay to do it (the config field is software-rotation and would
    // double-rotate us).
    cfg.rotation = Rotation::None;

    DrmDisplay display(cfg);
    if (!display.init()) {
        fprintf(stderr, "DrmDisplay init failed: %s\n",
                display.getLastError().c_str());
        return 1;
    }

    fprintf(stderr, "drm_hello: logical %ux%u, rendering \"%s\" for 10 sec\n",
            display.logicalWidth(), display.logicalHeight(), text);

    auto start = std::chrono::steady_clock::now();
    double hue = 180.0;  // start with cyan-ish
    while (true) {
        DrmFrame frame = display.acquire();
        paintFrame(frame, text, hue);
        display.present();

        hue += 1.5;  // slow color drift so each frame is visually distinct
        auto elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - start).count();
        if (elapsed > 60.0) break;
    }

    fprintf(stderr, "drm_hello: done.\n");
    return 0;
}
