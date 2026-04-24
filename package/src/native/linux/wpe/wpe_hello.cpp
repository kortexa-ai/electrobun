// wpe_hello.cpp — WPE WebKit rendering real HTML, directly to the DRM
// framebuffer, with no X / Wayland / compositor.
//
// Flow:
//   1. wpe_loader_init + wpe_fdo_initialize_shm — wire the SHM fdo backend.
//   2. Create a wpe_view_backend_exportable_fdo at landscape dimensions
//      (1920×480 on the bar panel).
//   3. Wrap it in a WebKitWebViewBackend, then a WebKitWebView.
//   4. webkit_web_view_load_html(...) with inline "Hello, Electrobun" HTML.
//   5. On each rendered frame, WPE calls our `export_shm_buffer` callback
//      with a wl_shm_buffer of CPU-readable pixels.
//   6. Blit those pixels into DrmDisplay's framebuffer, rotating 90° CCW so
//      content reads correctly on the physically-rotated bar panel. Respect
//      both source (wl_shm_buffer stride) and destination (DrmFrame pitch).
//   7. Release + request next frame.
//
// This is NOT integrated with Electrobun's FFI/build system — it's a
// standalone proof that WPE HTML → DRM scanout works end-to-end. The
// integration step (parallel libNativeWrapper_wpe.so, build.ts, CLI flag)
// is next.

#include "drm_display.h"
#include "input.h"

#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>
#include <wayland-server.h>

#include <glib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace electrobun::wpe;

namespace {

struct App {
    DrmDisplay*                              display        = nullptr;
    wpe_view_backend_exportable_fdo*         exportable     = nullptr;
    WebKitWebView*                           webview        = nullptr;
    GMainLoop*                               loop           = nullptr;
    uint32_t                                 framesRendered = 0;
    uint32_t                                 landscapeW     = 0;   // width of the WPE view (physical landscape width)
    uint32_t                                 landscapeH     = 0;   // height of the WPE view
    // Track last touch coords per-slot so TouchUp (which carries no coords) can dispatch at the right point.
    int32_t                                  lastTouchX[16] = {0};
    int32_t                                  lastTouchY[16] = {0};
};

// Copy a landscape-oriented source (W×H) into a portrait-oriented destination
// (H×W), rotating 90° CCW so what WPE renders "up" ends up "up" on the
// physically rotated bar panel.
//
// Coordinate derivation (for the record):
//   Let L[y][x] be the landscape source, y ∈ [0, landscapeH), x ∈ [0, landscapeW).
//   Let D[r][c] be the portrait destination, r ∈ [0, landscapeW), c ∈ [0, landscapeH).
//   CCW 90° rotation: D[r][c] = L[c][landscapeW - 1 - r].
//
// TODO(phase4): delete — absorb into the composite shader as a sampler transform.
static void blitRotateCCW(const uint8_t* src, int32_t srcStride,
                          uint32_t landscapeW, uint32_t landscapeH,
                          uint8_t* dst, uint32_t dstPitch,
                          uint32_t dstW, uint32_t dstH) {
    // Clamp in case the WPE buffer and the DRM buffer disagree (shouldn't
    // happen if we set both to the same dims, but defensive never hurts).
    uint32_t rMax = std::min<uint32_t>(dstH, landscapeW);
    uint32_t cMax = std::min<uint32_t>(dstW, landscapeH);

    for (uint32_t r = 0; r < rMax; r++) {
        uint32_t* dstRow = (uint32_t*)(dst + r * dstPitch);
        const uint32_t sx = landscapeW - 1 - r;
        for (uint32_t c = 0; c < cMax; c++) {
            const uint32_t sy = c;
            const uint32_t* srcPx = (const uint32_t*)(src + sy * srcStride + sx * 4);
            dstRow[c] = *srcPx;
        }
    }
}

static void onExportShm(void* data, wpe_fdo_shm_exported_buffer* buffer) {
    App* app = static_cast<App*>(data);

    wl_shm_buffer* shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    if (!shm) {
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(app->exportable, buffer);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(app->exportable);
        return;
    }

    wl_shm_buffer_begin_access(shm);
    const uint8_t* src       = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
    int32_t        srcStride = wl_shm_buffer_get_stride(shm);
    int32_t        srcW      = wl_shm_buffer_get_width(shm);
    int32_t        srcH      = wl_shm_buffer_get_height(shm);

    DrmFrame dst = app->display->acquire();

    // Expect src dims to match landscapeW × landscapeH; trust them anyway.
    blitRotateCCW(src, srcStride,
                  (uint32_t)srcW, (uint32_t)srcH,
                  dst.pixels, dst.pitch,
                  dst.width, dst.height);

    wl_shm_buffer_end_access(shm);

    app->display->present();

    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(app->exportable, buffer);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(app->exportable);

    app->framesRendered++;
}

static gboolean exitTimeout(gpointer data) {
    App* app = static_cast<App*>(data);
    fprintf(stderr, "wpe_hello: timed out; rendered %u frames\n", app->framesRendered);
    g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

} // anon

int main(int argc, char** argv) {
    // Optional first arg: a URL (http://, file://, data:). No arg = inline HTML below.
    const char* url = (argc > 1) ? argv[1] : nullptr;
    // 1. Wire the fdo backend into libwpe.
    //    The loader looks up this .so in WPE_BACKEND or the default ldconfig paths.
    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    if (!wpe_fdo_initialize_shm()) {
        fprintf(stderr, "wpe_fdo_initialize_shm failed\n");
        return 1;
    }

    // 2. DRM display. No software rotation in DrmDisplay itself — we rotate
    //    manually during the blit so the WPE view can be at landscape dims.
    DrmDisplayConfig cfg{};
    cfg.rotation = Rotation::None;
    DrmDisplay display(cfg);
    if (!display.init()) {
        fprintf(stderr, "DrmDisplay init failed: %s\n", display.getLastError().c_str());
        return 1;
    }

    App app{};
    app.display    = &display;
    // WPE renders at physical-landscape dims (swap of the native portrait mode).
    // For a 480×1920 panel, that's 1920×480.
    app.landscapeW = display.logicalHeight();  // mode.vdisplay (since rotation==None, logical == native)
    app.landscapeH = display.logicalWidth();
    fprintf(stderr, "wpe_hello: WPE view %ux%u, DRM buffer %ux%u (pitch respected in blit)\n",
            app.landscapeW, app.landscapeH, display.logicalWidth(), display.logicalHeight());

    // 3. Exportable SHM view backend.
    wpe_view_backend_exportable_fdo_client client = {};
    client.export_shm_buffer = onExportShm;
    app.exportable = wpe_view_backend_exportable_fdo_create(&client, &app,
                                                            app.landscapeW, app.landscapeH);
    if (!app.exportable) {
        fprintf(stderr, "wpe_view_backend_exportable_fdo_create failed\n");
        return 1;
    }

    // 4. WebKitWebView on top.
    wpe_view_backend* vb = wpe_view_backend_exportable_fdo_get_view_backend(app.exportable);
    WebKitWebViewBackend* webviewBackend = webkit_web_view_backend_new(vb, nullptr, nullptr);
    app.webview = webkit_web_view_new(webviewBackend);

    // Route JS `console.log` / `console.error` to the WebProcess's stdout so
    // we can see them in /tmp/wpe_hello.log (definitive proof JS is running).
    WebKitSettings* settings = webkit_web_view_get_settings(app.webview);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);

    // 5. Load inline HTML. Centered, bold, readable at a glance.
    const char* kHtml =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'><style>"
        "  html,body { margin:0; padding:0; width:100%; height:100%; }"
        "  body {"
        "    background: linear-gradient(90deg,#1e2a56,#314580);"
        "    color: white;"
        "    display: flex;"
        "    align-items: center;"
        "    justify-content: center;"
        "    font-family: sans-serif;"
        "    font-weight: 700;"
        "    font-size: 110px;"
        "    letter-spacing: 2px;"
        "  }"
        "  .dot { color:#ffbe2e; }"
        "</style></head>"
        "<body><span>Hello, Electrobun<span class='dot'>.</span></span></body></html>";

    if (url) {
        fprintf(stderr, "wpe_hello: loading %s\n", url);
        webkit_web_view_load_uri(app.webview, url);
    } else {
        fprintf(stderr, "wpe_hello: loading inline HTML\n");
        webkit_web_view_load_html(app.webview, kHtml, nullptr);
    }

    // 6a. Hook libinput events → WPE pointer events so the button is live.
    //     Map normalized (x,y) from InputDispatcher back to DRM-portrait pixels,
    //     then apply the inverse of our CCW-90 blit rotation to land in the
    //     WPE view's landscape coordinate system.
    //
    //     WPE landscape is landscapeW × landscapeH (e.g. 1920 × 480).
    //     Our blit did: D[r][c] = L[c][landscapeW - 1 - r], so the inverse is:
    //         wpe_x = landscapeW - 1 - drm_y
    //         wpe_y = drm_x
    auto dispatchPointer = [&app](enum wpe_input_pointer_event_type t, int32_t x, int32_t y,
                                  uint32_t button, uint32_t state, uint32_t timeMs) {
        wpe_view_backend* vb = wpe_view_backend_exportable_fdo_get_view_backend(app.exportable);
        struct wpe_input_pointer_event ev = {};
        ev.type     = t;
        ev.time     = timeMs;
        ev.x        = x;
        ev.y        = y;
        ev.button   = button;
        ev.state    = state;
        wpe_view_backend_dispatch_pointer_event(vb, &ev);
    };

    InputDispatcherConfig icfg{};
    icfg.screenWidth          = app.display->logicalWidth();   // 480 on our panel
    icfg.screenHeight         = app.display->logicalHeight();  // 1920 on our panel
    icfg.rotationQuarterTurns = 0;

    InputDispatcher input(icfg, [&](const InputEvent& ev) {
        const auto screenW = app.display->logicalWidth();
        const auto screenH = app.display->logicalHeight();
        const int32_t drmX = (int32_t)(ev.x * (double)screenW);
        const int32_t drmY = (int32_t)(ev.y * (double)screenH);
        const int32_t wpeX = (int32_t)app.landscapeW - 1 - drmY;
        const int32_t wpeY = drmX;

        switch (ev.type) {
            case InputEventType::TouchDown: {
                int s = std::max(0, std::min((int)(sizeof(app.lastTouchX)/sizeof(app.lastTouchX[0]) - 1), ev.touchSlot));
                app.lastTouchX[s] = wpeX;
                app.lastTouchY[s] = wpeY;
                dispatchPointer(wpe_input_pointer_event_type_motion, wpeX, wpeY, 0, 0, ev.timeMs);
                dispatchPointer(wpe_input_pointer_event_type_button, wpeX, wpeY, 1, 1, ev.timeMs);
                fprintf(stderr, "[input] touch-down drm(%d,%d) -> wpe(%d,%d)\n",
                        drmX, drmY, wpeX, wpeY);
                break;
            }
            case InputEventType::TouchMotion: {
                int s = std::max(0, std::min((int)(sizeof(app.lastTouchX)/sizeof(app.lastTouchX[0]) - 1), ev.touchSlot));
                app.lastTouchX[s] = wpeX;
                app.lastTouchY[s] = wpeY;
                dispatchPointer(wpe_input_pointer_event_type_motion, wpeX, wpeY, 0, 0, ev.timeMs);
                break;
            }
            case InputEventType::TouchUp: {
                int s = std::max(0, std::min((int)(sizeof(app.lastTouchX)/sizeof(app.lastTouchX[0]) - 1), ev.touchSlot));
                const int32_t ux = app.lastTouchX[s];
                const int32_t uy = app.lastTouchY[s];
                dispatchPointer(wpe_input_pointer_event_type_button, ux, uy, 1, 0, ev.timeMs);
                fprintf(stderr, "[input] touch-up at wpe(%d,%d)\n", ux, uy);
                break;
            }
            case InputEventType::PointerMotion:
            case InputEventType::PointerButtonDown:
            case InputEventType::PointerButtonUp:
                // TODO(phase2.1): mouse path. We only have a touchscreen on this Pi.
                break;
            default:
                break;
        }
    });
    if (!input.start()) {
        fprintf(stderr, "wpe_hello: InputDispatcher failed to start; running without input\n");
    } else {
        fprintf(stderr, "wpe_hello: input wired; touch the screen to click\n");
    }

    // 6b. Run the main loop for 60 seconds, then exit cleanly.
    app.loop = g_main_loop_new(nullptr, FALSE);
    g_timeout_add_seconds(60, exitTimeout, &app);
    fprintf(stderr, "wpe_hello: running; rendering for 60 seconds\n");
    g_main_loop_run(app.loop);

    // Teardown.
    if (app.webview)    g_object_unref(app.webview);
    if (app.exportable) wpe_view_backend_exportable_fdo_destroy(app.exportable);
    if (app.loop)       g_main_loop_unref(app.loop);
    return 0;
}
