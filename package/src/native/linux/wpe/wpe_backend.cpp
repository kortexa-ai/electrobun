// wpe_backend.cpp — WPE WebKit + DRM/KMS backend for Electrobun on bare Linux.
//
// Phase 2 skeleton. Ties DrmDisplay + InputDispatcher + libwpe + WPEBackend-fdo
// into an IDisplayBackend + IWebviewBackend implementation. A GLib main loop
// drives libwpe (via WPEBackend-fdo's GLib integration), libinput (via
// g_unix_fd_add in InputDispatcher), and DRM page-flip events (via a custom
// GSource wrapping the DRM fd).
//
// Status: right-shaped. The class wiring is in place; the WPE view backend
// is instantiated; HTML rendering into an offscreen buffer and blit into
// the DrmDisplay's frame are stubbed with TODOs.
//
// To actually render "Hello, Electrobun":
//   1. Finish DrmDisplay::acquire()/present() page-flip plumbing (drm_display.cpp)
//   2. Finish WPEBackend-fdo EGL target → CPU-readable buffer plumbing below
//   3. Blit WPE's offscreen pixels into DrmFrame::pixels, respecting pitch
//   4. Call drmModePageFlip via DrmDisplay::present()
//
// References:
// - libwpe: https://github.com/WebPlatformForEmbedded/libwpe
// - WPEBackend-fdo (offscreen target): part of the WPE ecosystem
// - wpewebkit-2.0 API: webkit_web_view_new, webkit_web_view_load_uri, etc.
// - Pi 5 vc4 driver: confirmed Phase 0 step 2 that DRM+WPE works; stride
//   handling must be correct, see drm_display.cpp.

#include "../abstract_view.h"
#include "../backend.h"

#include "drm_display.h"
#include "input.h"

#include <glib.h>

// libwpe / WPEBackend-fdo / wpewebkit-2.0 — guard with a macro so the skeleton
// compiles even when these headers aren't yet in the build's include path.
// Production build sets HAVE_WPE=1 via pkg-config wpe-webkit-2.0.
#if defined(HAVE_WPE) && HAVE_WPE
  #include <wpe/wpe.h>
  #include <wpe/fdo.h>
  #include <wpe/webkit.h>
#endif

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace electrobun {
namespace wpe {

// ---------------------------------------------------------------------------
// WpeWebViewImpl — AbstractView impl backed by WebKitWebView (WPE port).
// ---------------------------------------------------------------------------

class WpeWebViewImpl : public AbstractView {
public:
    WpeWebViewImpl(uint32_t webviewId_, class WpeBackend* backend)
        : AbstractView(webviewId_), backend_(backend) {}

    void loadURL(const char* urlString) override {
#if defined(HAVE_WPE) && HAVE_WPE
        if (webView_ && urlString) webkit_web_view_load_uri(webView_, urlString);
#else
        (void)urlString; // TODO(phase2): wire to webkit_web_view_load_uri
#endif
    }

    void loadHTML(const char* htmlString) override {
#if defined(HAVE_WPE) && HAVE_WPE
        if (webView_ && htmlString) webkit_web_view_load_html(webView_, htmlString, nullptr);
#else
        (void)htmlString; // TODO(phase2): wire to webkit_web_view_load_html
#endif
    }

    void goBack() override { /* TODO(phase2): webkit_web_view_go_back(webView_); */ }
    void goForward() override { /* TODO(phase2) */ }
    void reload() override { /* TODO(phase2): webkit_web_view_reload(webView_); */ }
    void remove() override { isRemoved = true; /* TODO(phase2): webkit_web_view_try_close(webView_) + cleanup */ }
    bool canGoBack() override { return false; /* TODO(phase2) */ }
    bool canGoForward() override { return false; /* TODO(phase2) */ }

    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
#if defined(HAVE_WPE) && HAVE_WPE
        if (webView_ && jsString) {
            webkit_web_view_evaluate_javascript(webView_, jsString, -1, nullptr, nullptr,
                                                nullptr, nullptr, nullptr);
        }
#else
        (void)jsString;
#endif
    }

    void callAsyncJavascript(const char* /*messageId*/, const char* /*jsString*/,
                             uint32_t /*webviewId*/, uint32_t /*hostWebviewId*/,
                             void* /*completionHandler*/) override {
        // TODO(phase2): mirror WebKitGTK callAsyncJavaScript pattern; send
        // response back through the shared callback plumbing.
    }

    void addPreloadScriptToWebView(const char* /*jsString*/) override {
        // TODO(phase2): webkit_user_content_manager_add_script
    }
    void updateCustomPreloadScript(const char* /*jsString*/) override { /* TODO(phase2) */ }

    void resize(const Rect& frame, const char* masksJson) override {
        visualBounds = frame;
        maskJSON = masksJson ? masksJson : "";
        // TODO(phase2): forward to wpe_view_backend_dispatch_set_size() so the
        // web content engine reflows to this rectangle. Mask handling (for
        // click-through regions) is out of scope for Phase 2; document as
        // "not implemented on WPE/embedded" and move on.
    }

    void applyVisualMask() override { /* not implemented on WPE */ }
    void removeMasks() override { /* not implemented on WPE */ }
    void toggleMirrorMode(bool enable) override { mirrorModeEnabled = enable; }

    void findInPage(const char*, bool, bool) override { /* TODO(phase2) */ }
    void stopFindInPage() override { /* TODO(phase2) */ }
    void openDevTools() override { /* TODO(phase2, low priority for kiosk) */ }
    void closeDevTools() override { /* TODO(phase2) */ }
    void toggleDevTools() override { /* TODO(phase2) */ }

private:
    class WpeBackend* backend_ = nullptr;  // non-owning

#if defined(HAVE_WPE) && HAVE_WPE
    WebKitWebView* webView_ = nullptr;
    struct wpe_view_backend* wpeViewBackend_ = nullptr;
#else
    void* webView_ = nullptr;
    void* wpeViewBackend_ = nullptr;
#endif
};

// ---------------------------------------------------------------------------
// WpeBackend — IDisplayBackend + IWebviewBackend for bare-DRM Linux.
// ---------------------------------------------------------------------------

class WpeBackend : public IDisplayBackend, public IWebviewBackend {
public:
    WpeBackend() = default;
    ~WpeBackend() override { teardown(); }

    // IDisplayBackend

    void* createWindow(const WindowSpec& spec) override {
        // Only one "window" makes sense on bare DRM; we create the DrmDisplay
        // on first createWindow() and ignore spec.frame (the display has its
        // own native mode — we use its full size).
        if (display_) {
            // TODO(phase2): for multi-webview apps we might want to track
            // logical "windows" even though physically there's one scanout.
            return display_.get();
        }
        DrmDisplayConfig cfg{};
        cfg.rotation = rotationFromEnv();
        display_ = std::make_unique<DrmDisplay>(cfg);
        if (!display_->init()) {
            fprintf(stderr, "[WpeBackend] DrmDisplay::init failed: %s\n",
                    display_->getLastError().c_str());
            display_.reset();
            return nullptr;
        }

        // Start input pump bound to logical (post-rotation) screen size.
        InputDispatcherConfig icfg{};
        icfg.screenWidth  = display_->logicalWidth();
        icfg.screenHeight = display_->logicalHeight();
        icfg.rotationQuarterTurns = static_cast<int>(cfg.rotation);
        input_ = std::make_unique<InputDispatcher>(icfg,
            [this](const InputEvent& ev) { this->onInputEvent(ev); });
        if (!input_->start()) {
            fprintf(stderr, "[WpeBackend] InputDispatcher::start failed\n");
            // Continue anyway — the app can still render, just no input.
        }

        // Spec fields (title, borderless, etc.) are ignored on DRM.
        (void)spec;
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
        auto view = std::make_shared<WpeWebViewImpl>(spec.webviewId, this);

#if defined(HAVE_WPE) && HAVE_WPE
        // TODO(phase2):
        //   1. Create a WPEBackend-fdo "exportable" view backend that renders
        //      into an offscreen EGLImage or shared memory buffer we can read
        //      back from CPU (for the Phase 2 CPU-blit path).
        //   2. Call webkit_web_view_new_with_related_view or similar to create
        //      a WebKitWebView bound to that view backend.
        //   3. Install signal handlers for load-changed, permission-request,
        //      script-message-received etc. matching the WebKitGTK backend's
        //      callback surface.
        //   4. Call loadURL(spec.url.c_str()) to kick off initial navigation.
#else
        (void)spec;
#endif

        views_.push_back(view);
        return view;
    }

private:
    Rotation rotationFromEnv() const {
        // TODO(phase2): replace with config-driven selection once the CLI
        // flag is added (build.linux.embedded.rotate: 0 | 90 | 180 | 270).
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

    void onInputEvent(const InputEvent& ev) {
        // TODO(phase2): translate to wpe_input_keyboard_event / _pointer_event
        // / _touch_event and call wpe_view_backend_dispatch_* on each active
        // view's wpeViewBackend_. For now: log so we know it fired.
        fprintf(stderr, "[WpeBackend] input event type=%d\n", (int)ev.type);
    }

    void teardown() {
        if (input_) { input_->stop(); input_.reset(); }
        display_.reset();
        if (mainLoop_) { g_main_loop_unref(mainLoop_); mainLoop_ = nullptr; }
    }

    std::unique_ptr<DrmDisplay>       display_;
    std::unique_ptr<InputDispatcher>  input_;
    GMainLoop*                        mainLoop_ = nullptr;
    std::vector<std::shared_ptr<AbstractView>> views_;
    std::mutex mu_;
};

// ---------------------------------------------------------------------------
// currentDisplayBackend / currentWebviewBackend — singleton accessors.
// On the embedded .so (libNativeWrapper_wpe.so), these return the WpeBackend
// instance. The GTK .so defines these differently in nativeWrapper.cpp.
// ---------------------------------------------------------------------------

} // namespace wpe

// Linker-level selection: this TU is only compiled into libNativeWrapper_wpe.so.
// The GTK .so provides its own currentDisplayBackend/currentWebviewBackend.
namespace {
wpe::WpeBackend& instance() {
    static wpe::WpeBackend backend;
    return backend;
}
} // anon

IDisplayBackend& currentDisplayBackend()   { return instance(); }
IWebviewBackend& currentWebviewBackend()   { return instance(); }

} // namespace electrobun
