// wpe_backend.cpp — WPE WebKit + DRM/KMS backend for Electrobun on bare Linux.
//
// Phase 2.1: full implementation. Folds the proven wpe_hello logic into
// the class shape Electrobun's FFI expects (IDisplayBackend, IWebviewBackend,
// AbstractView). One backend instance per process; first window wins
// (DRM has exactly one scanout plane per display).
//
// Compiled only for the `linux-embedded` target. The GTK target uses
// nativeWrapper.cpp's GTK/WebKitGTK/CEF code path and never sees this file.
//
// Runtime dependencies:
//   libwpe-1.0, libwpebackend-fdo-1.0 (SHM path — no EGL),
//   libwpewebkit-2.0, libwayland-server (for wl_shm_buffer),
//   libdrm, libgbm, libinput, libudev, glib-2.0.

#include "../abstract_view.h"
#include "../backend.h"

#include "drm_display.h"
#include "input.h"

#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>
#include <wayland-server.h>

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace electrobun {
namespace wpe {

class WpeBackend;  // fwd

// ---------------------------------------------------------------------------
// WpeWebViewImpl
// ---------------------------------------------------------------------------

class WpeWebViewImpl : public AbstractView {
public:
    WpeWebViewImpl(uint32_t webviewId_, WpeBackend* backend,
                   WebKitWebView* webView,
                   struct wpe_view_backend_exportable_fdo* exportable)
        : AbstractView(webviewId_),
          backend_(backend),
          webView_(webView),
          exportable_(exportable) {}

    ~WpeWebViewImpl() override {
        if (webView_)    g_object_unref(webView_);
        if (exportable_) wpe_view_backend_exportable_fdo_destroy(exportable_);
    }

    struct wpe_view_backend* viewBackend() const {
        return exportable_ ? wpe_view_backend_exportable_fdo_get_view_backend(exportable_) : nullptr;
    }

    struct wpe_view_backend_exportable_fdo* exportable() const { return exportable_; }

    // AbstractView

    void loadURL(const char* urlString) override {
        if (webView_ && urlString) webkit_web_view_load_uri(webView_, urlString);
    }
    void loadHTML(const char* htmlString) override {
        if (webView_ && htmlString) webkit_web_view_load_html(webView_, htmlString, nullptr);
    }
    void goBack()     override { if (webView_) webkit_web_view_go_back(webView_); }
    void goForward()  override { if (webView_) webkit_web_view_go_forward(webView_); }
    void reload()     override { if (webView_) webkit_web_view_reload(webView_); }
    void remove()     override { isRemoved = true; /* WpeBackend cleans up on destruction */ }
    bool canGoBack()    override { return webView_ && webkit_web_view_can_go_back(webView_); }
    bool canGoForward() override { return webView_ && webkit_web_view_can_go_forward(webView_); }

    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
        if (webView_ && jsString) {
            webkit_web_view_evaluate_javascript(webView_, jsString, -1, nullptr, nullptr,
                                                nullptr, nullptr, nullptr);
        }
    }
    void callAsyncJavascript(const char*, const char*, uint32_t, uint32_t, void*) override {
        // TODO(phase2.next): mirror WebKitGTK callAsyncJavaScript pattern.
    }
    void addPreloadScriptToWebView(const char*) override { /* TODO(phase2.next): user content manager */ }
    void updateCustomPreloadScript(const char*) override { /* TODO(phase2.next) */ }

    void resize(const Rect& frame, const char* masksJson) override {
        visualBounds = frame;
        maskJSON = masksJson ? masksJson : "";
        // On a kiosk with first-window-wins, logical size == full display.
        // We still forward size so the web engine reflows correctly if JS
        // queries window inner dims.
        if (exportable_) {
            auto* vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable_);
            if (vb) wpe_view_backend_dispatch_set_size(vb, frame.width, frame.height);
        }
    }

    void applyVisualMask() override { /* not implemented on embedded */ }
    void removeMasks() override     { /* not implemented on embedded */ }
    void toggleMirrorMode(bool enable) override { mirrorModeEnabled = enable; }

    void findInPage(const char*, bool, bool) override {}
    void stopFindInPage() override {}
    void openDevTools() override {}   // no devtools on a bare kiosk
    void closeDevTools() override {}
    void toggleDevTools() override {}

private:
    friend class WpeBackend;
    WpeBackend*                              backend_    = nullptr;
    WebKitWebView*                           webView_    = nullptr;
    struct wpe_view_backend_exportable_fdo*  exportable_ = nullptr;
    // Per-slot last touch position so TouchUp (no coords) can dispatch at the right spot.
    int32_t                                  lastTouchX_[16] = {0};
    int32_t                                  lastTouchY_[16] = {0};
};

// ---------------------------------------------------------------------------
// WpeBackend
// ---------------------------------------------------------------------------

class WpeBackend : public IDisplayBackend, public IWebviewBackend {
public:
    WpeBackend() = default;
    ~WpeBackend() override { teardown(); }

    // IDisplayBackend

    void* createWindow(const WindowSpec& spec) override {
        (void)spec;  // DRM uses the native display mode regardless.
        if (display_) return display_.get();

        initWpeOnce();

        DrmDisplayConfig cfg{};
        cfg.rotation = rotationFromEnv();
        display_ = std::make_unique<DrmDisplay>(cfg);
        if (!display_->init()) {
            fprintf(stderr, "[WpeBackend] DrmDisplay::init failed: %s\n",
                    display_->getLastError().c_str());
            display_.reset();
            return nullptr;
        }

        // WPE view is laid out at landscape dims; we rotate-blit to the
        // (portrait) DRM framebuffer. If DrmDisplay config.rotation is None
        // and the panel's native mode is portrait, landscape == transpose.
        rotationQuarters_ = static_cast<int>(cfg.rotation);
        if (rotationQuarters_ == 1 || rotationQuarters_ == 3) {
            // If DrmDisplay rotates internally, our logical is already post-rotation.
            landscapeW_ = display_->logicalWidth();
            landscapeH_ = display_->logicalHeight();
        } else {
            // DrmDisplay didn't rotate; we do. Webview at the swapped dims.
            landscapeW_ = display_->logicalHeight();  // native vdisplay
            landscapeH_ = display_->logicalWidth();   // native hdisplay
        }

        InputDispatcherConfig icfg{};
        icfg.screenWidth          = display_->logicalWidth();
        icfg.screenHeight         = display_->logicalHeight();
        icfg.rotationQuarterTurns = rotationQuarters_;
        input_ = std::make_unique<InputDispatcher>(icfg,
            [this](const InputEvent& ev) { this->onInputEvent(ev); });
        if (!input_->start()) {
            fprintf(stderr, "[WpeBackend] InputDispatcher::start failed (continuing without input)\n");
        }

        return display_.get();
    }

    void runEventLoop() override {
        if (!mainLoop_) mainLoop_ = g_main_loop_new(nullptr, FALSE);
        g_main_loop_run(mainLoop_);
    }

    void stopEventLoop() override {
        if (mainLoop_ && g_main_loop_is_running(mainLoop_)) {
            g_main_loop_quit(mainLoop_);
        }
    }

    // IWebviewBackend

    std::shared_ptr<AbstractView> createWebview(const WebviewSpec& spec) override {
        if (!display_) {
            fprintf(stderr, "[WpeBackend] createWebview called before createWindow\n");
            return nullptr;
        }
        initWpeOnce();

        // First-window-wins: tie the one scanout plane to the first webview.
        // Subsequent webviews get created but won't be displayed in Phase 2
        // (multi-webview composition is Phase 4).
        if (primaryView_) {
            fprintf(stderr, "[WpeBackend] warning: only the first webview is displayed "
                            "on bare-DRM (embedded target). webviewId=%u will not scan out.\n",
                    spec.webviewId);
        }

        wpe_view_backend_exportable_fdo_client client = {};
        client.export_shm_buffer = &WpeBackend::onExportShmStatic;

        auto* exportable = wpe_view_backend_exportable_fdo_create(
            &client, /*userData=*/this,
            landscapeW_, landscapeH_);
        if (!exportable) {
            fprintf(stderr, "[WpeBackend] wpe_view_backend_exportable_fdo_create failed\n");
            return nullptr;
        }

        auto* vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
        WebKitWebViewBackend* webviewBackend = webkit_web_view_backend_new(vb, nullptr, nullptr);
        WebKitWebView* webView = webkit_web_view_new(webviewBackend);

        // Route console.log to the WebProcess's stdout so host-side logs can
        // see JS activity (handy for kiosk debugging without devtools).
        WebKitSettings* settings = webkit_web_view_get_settings(webView);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);

        auto view = std::make_shared<WpeWebViewImpl>(spec.webviewId, this, webView, exportable);
        if (!spec.url.empty())        view->loadURL(spec.url.c_str());
        if (!primaryView_)            primaryView_ = view.get();
        views_.push_back(view);
        return view;
    }

private:
    // ---- WPE initialization (once per process) ----
    static void initWpeOnce() {
        static std::once_flag once;
        std::call_once(once, []() {
            wpe_loader_init("libWPEBackend-fdo-1.0.so");
            if (!wpe_fdo_initialize_shm()) {
                fprintf(stderr, "[WpeBackend] wpe_fdo_initialize_shm failed\n");
            }
        });
    }

    Rotation rotationFromEnv() const {
        // Phase 2 uses an env var; Phase 2.4 will replace with a CLI flag read
        // from the app's electrobun.config.ts (build.linux.embedded.rotate).
        const char* s = g_getenv("ELECTROBUN_ROTATE");
        if (!s) return Rotation::None;
        int q = atoi(s);
        switch (q) {
            case 90:  return Rotation::CW90;
            case 180: return Rotation::Rot180;
            case 270: return Rotation::CCW90;
            default:  return Rotation::None;
        }
    }

    // ---- SHM export → DRM blit ----

    static void onExportShmStatic(void* userData, struct wpe_fdo_shm_exported_buffer* buffer) {
        static_cast<WpeBackend*>(userData)->onExportShm(buffer);
    }

    void onExportShm(struct wpe_fdo_shm_exported_buffer* buffer) {
        if (!primaryView_ || !display_) {
            // Can't blit without a target. Release and ask for the next frame
            // so WPE doesn't stall.
            if (primaryView_ && primaryView_->exportable()) {
                wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
                    primaryView_->exportable(), buffer);
                wpe_view_backend_exportable_fdo_dispatch_frame_complete(
                    primaryView_->exportable());
            }
            return;
        }

        struct wl_shm_buffer* shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
        if (shm) {
            wl_shm_buffer_begin_access(shm);
            const uint8_t* src       = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
            int32_t        srcStride = wl_shm_buffer_get_stride(shm);
            int32_t        srcW      = wl_shm_buffer_get_width(shm);
            int32_t        srcH      = wl_shm_buffer_get_height(shm);

            DrmFrame dst = display_->acquire();
            blitWithRotation(src, srcStride, (uint32_t)srcW, (uint32_t)srcH,
                             dst.pixels, dst.pitch, dst.width, dst.height);
            wl_shm_buffer_end_access(shm);

            display_->present();
            framesRendered_++;
        }

        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
            primaryView_->exportable(), buffer);
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(
            primaryView_->exportable());
    }

    // rotationQuarters_ of 0 = no rotation (straight copy), respect pitches.
    //                     1 = 90° CW  → D[r][c] = L[H - 1 - c][r]
    //                     2 = 180°    → D[r][c] = L[H - 1 - r][W - 1 - c]
    //                     3 = 90° CCW → D[r][c] = L[c][W - 1 - r]   (our bar panel case)
    //
    // TODO(phase4): delete — absorbed into a composite shader's sampler transform.
    void blitWithRotation(const uint8_t* src, int32_t srcStride,
                          uint32_t srcW, uint32_t srcH,
                          uint8_t* dst, uint32_t dstPitch,
                          uint32_t dstW, uint32_t dstH) const {
        switch (rotationQuarters_) {
            case 0: {
                uint32_t h = std::min(srcH, dstH);
                uint32_t w = std::min(srcW, dstW);
                for (uint32_t r = 0; r < h; r++) {
                    std::memcpy(dst + r * dstPitch, src + r * srcStride, w * 4);
                }
                return;
            }
            case 1: {  // CW 90: D[r][c] = L[H-1-c][r]
                uint32_t rMax = std::min(srcW, dstH);
                uint32_t cMax = std::min(srcH, dstW);
                for (uint32_t r = 0; r < rMax; r++) {
                    uint32_t* dstRow = (uint32_t*)(dst + r * dstPitch);
                    for (uint32_t c = 0; c < cMax; c++) {
                        const uint32_t sy = srcH - 1 - c;
                        const uint32_t sx = r;
                        dstRow[c] = *(const uint32_t*)(src + sy * srcStride + sx * 4);
                    }
                }
                return;
            }
            case 2: {  // 180
                uint32_t h = std::min(srcH, dstH);
                uint32_t w = std::min(srcW, dstW);
                for (uint32_t r = 0; r < h; r++) {
                    uint32_t* dstRow = (uint32_t*)(dst + r * dstPitch);
                    const uint32_t sy = srcH - 1 - r;
                    for (uint32_t c = 0; c < w; c++) {
                        const uint32_t sx = srcW - 1 - c;
                        dstRow[c] = *(const uint32_t*)(src + sy * srcStride + sx * 4);
                    }
                }
                return;
            }
            case 3:
            default: {  // CCW 90 (our bar panel): D[r][c] = L[c][W-1-r]
                uint32_t rMax = std::min(srcW, dstH);
                uint32_t cMax = std::min(srcH, dstW);
                for (uint32_t r = 0; r < rMax; r++) {
                    uint32_t* dstRow = (uint32_t*)(dst + r * dstPitch);
                    const uint32_t sx = srcW - 1 - r;
                    for (uint32_t c = 0; c < cMax; c++) {
                        const uint32_t sy = c;
                        dstRow[c] = *(const uint32_t*)(src + sy * srcStride + sx * 4);
                    }
                }
                return;
            }
        }
    }

    // ---- Input translation ----
    //
    // InputDispatcher gives us normalized (0..1) coords in the DRM-native
    // coordinate system (screenWidth × screenHeight). We map back to pixel,
    // then apply the inverse of the blit rotation to get WPE-landscape pixel
    // coords, then dispatch a WPE pointer event so the page handles the
    // touch as a mouse click (which the HTML `click` listener expects).
    void onInputEvent(const InputEvent& ev) {
        if (!primaryView_ || !display_) return;
        auto* vb = primaryView_->viewBackend();
        if (!vb) return;

        const uint32_t screenW = display_->logicalWidth();
        const uint32_t screenH = display_->logicalHeight();
        const int32_t drmX = (int32_t)(ev.x * (double)screenW);
        const int32_t drmY = (int32_t)(ev.y * (double)screenH);

        int32_t wpeX = 0, wpeY = 0;
        // Inverse of blitWithRotation's mapping.
        switch (rotationQuarters_) {
            case 1:  wpeX = drmY;                         wpeY = (int32_t)landscapeH_ - 1 - drmX; break;
            case 2:  wpeX = (int32_t)landscapeW_ - 1 - drmX; wpeY = (int32_t)landscapeH_ - 1 - drmY; break;
            case 3:  wpeX = (int32_t)landscapeW_ - 1 - drmY; wpeY = drmX;                           break;
            default: wpeX = drmX;                         wpeY = drmY;                              break;
        }

        auto dispatchPointer = [&](enum wpe_input_pointer_event_type t,
                                   uint32_t button, uint32_t state) {
            struct wpe_input_pointer_event pe = {};
            pe.type   = t;
            pe.time   = ev.timeMs;
            pe.x      = wpeX;
            pe.y      = wpeY;
            pe.button = button;
            pe.state  = state;
            wpe_view_backend_dispatch_pointer_event(vb, &pe);
        };

        switch (ev.type) {
            case InputEventType::TouchDown: {
                int s = std::max(0, std::min(15, ev.touchSlot));
                primaryView_->lastTouchX_[s] = wpeX;
                primaryView_->lastTouchY_[s] = wpeY;
                dispatchPointer(wpe_input_pointer_event_type_motion, 0, 0);
                dispatchPointer(wpe_input_pointer_event_type_button, 1, 1);
                break;
            }
            case InputEventType::TouchMotion: {
                int s = std::max(0, std::min(15, ev.touchSlot));
                primaryView_->lastTouchX_[s] = wpeX;
                primaryView_->lastTouchY_[s] = wpeY;
                dispatchPointer(wpe_input_pointer_event_type_motion, 0, 0);
                break;
            }
            case InputEventType::TouchUp: {
                int s = std::max(0, std::min(15, ev.touchSlot));
                // Dispatch release at the last known slot coords.
                struct wpe_input_pointer_event pe = {};
                pe.type   = wpe_input_pointer_event_type_button;
                pe.time   = ev.timeMs;
                pe.x      = primaryView_->lastTouchX_[s];
                pe.y      = primaryView_->lastTouchY_[s];
                pe.button = 1;
                pe.state  = 0;
                wpe_view_backend_dispatch_pointer_event(vb, &pe);
                break;
            }
            case InputEventType::PointerMotion:
            case InputEventType::PointerButtonDown:
            case InputEventType::PointerButtonUp:
            case InputEventType::PointerAxis:
                // TODO(phase2.next): mouse path. Needs running cursor state.
                break;
            case InputEventType::KeyDown:
            case InputEventType::KeyUp:
                // TODO(phase2.next): wpe_input_keyboard_event dispatch + xkb translation.
                break;
            default:
                break;
        }
    }

    void teardown() {
        views_.clear();
        primaryView_ = nullptr;
        if (input_) { input_->stop(); input_.reset(); }
        display_.reset();
        if (mainLoop_) { g_main_loop_unref(mainLoop_); mainLoop_ = nullptr; }
    }

    std::unique_ptr<DrmDisplay>       display_;
    std::unique_ptr<InputDispatcher>  input_;
    GMainLoop*                        mainLoop_ = nullptr;

    // WPE view dims (pre-rotation). For the 480×1920 bar, landscape is 1920×480.
    uint32_t                          landscapeW_ = 0;
    uint32_t                          landscapeH_ = 0;
    int                               rotationQuarters_ = 0;

    std::vector<std::shared_ptr<AbstractView>> views_;
    WpeWebViewImpl*                            primaryView_ = nullptr;  // non-owning; first webview
    std::atomic<uint64_t>                      framesRendered_{0};
};

} // namespace wpe

// Singleton accessors. Linked into libNativeWrapper_wpe.so only.
namespace {
wpe::WpeBackend& wpeBackendInstance() {
    static wpe::WpeBackend backend;
    return backend;
}
} // anon

IDisplayBackend& currentDisplayBackend() { return wpeBackendInstance(); }
IWebviewBackend& currentWebviewBackend() { return wpeBackendInstance(); }

} // namespace electrobun
