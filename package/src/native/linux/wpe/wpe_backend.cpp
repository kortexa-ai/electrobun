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
#include "../../shared/asar.h"
#include "../../shared/mime_types.h"

#include "drm_display.h"
#include "input.h"

#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>
#include <wayland-server.h>

#include <glib.h>
#include <glib-unix.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace electrobun {
namespace wpe {

class WpeBackend;  // fwd

// ---------------------------------------------------------------------------
// Main-thread dispatch
//
// Bun's FFI calls our exports from a Worker thread (different OS thread from
// the one running g_main_loop_run). WebKit-WPE requires all WebView ops to
// run on the GMainLoop's thread — calling them off-thread crashes the
// process inside libWPEWebKit. Mirror nativeWrapper.cpp's dispatch_sync_main
// pattern: marshal the call to the main loop and block until it finishes.
// ---------------------------------------------------------------------------

static std::atomic<long> g_mainThreadTid{-1};

static inline bool onMainThread() {
    long t = g_mainThreadTid.load(std::memory_order_relaxed);
    return t > 0 && (long)syscall(SYS_gettid) == t;
}

static void dispatchSyncMain(std::function<void()> fn) {
    if (onMainThread()) { fn(); return; }

    auto* heap = new std::function<void()>(std::move(fn));
    std::promise<void> done;
    auto fut = done.get_future();

    struct Pack { std::function<void()>* fn; std::promise<void>* done; };
    Pack* pack = new Pack{heap, &done};

    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer ud) -> gboolean {
        auto* p = static_cast<Pack*>(ud);
        try { (*p->fn)(); } catch (...) {}
        p->done->set_value();
        return G_SOURCE_REMOVE;
    }, pack, +[](gpointer ud) {
        auto* p = static_cast<Pack*>(ud);
        delete p->fn;
        delete p;
    });

    fut.wait();
}

// ---------------------------------------------------------------------------
// views:// URL scheme handler — reads from app.asar or the filesystem layout.
// Mirrors the GTK-side handler in nativeWrapper.cpp so hello-embedded's
// `views://main/index.html` resolves identically on both backends.
// ---------------------------------------------------------------------------

static AsarArchive*    g_asarArchive = nullptr;
static std::once_flag  g_asarInitFlag;
static std::mutex      g_asarReadMutex;

static void handleViewsURIScheme(WebKitURISchemeRequest* request, gpointer /*userData*/) {
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    fprintf(stderr, "[wpe views://] request uri=%s\n", uri ? uri : "(null)"); fflush(stderr);
    const char* fullPath = "index.html";
    if (uri && std::strncmp(uri, "views://", 8) == 0) {
        fullPath = uri + 8;
    }

    gchar* cwd = g_get_current_dir();
    gchar* resourcesDir = g_build_filename(cwd, "..", "Resources", nullptr);
    gchar* asarPath = g_build_filename(resourcesDir, "app.asar", nullptr);

    gchar* fileContents = nullptr;
    gsize  fileSize = 0;
    bool   foundFile = false;

    if (g_file_test(asarPath, G_FILE_TEST_EXISTS)) {
        std::call_once(g_asarInitFlag, [asarPath]() {
            g_asarArchive = asar_open(asarPath);
            if (!g_asarArchive) {
                fprintf(stderr, "[wpe] failed to open ASAR at %s\n", asarPath);
            }
        });
        if (g_asarArchive) {
            std::string asarFilePath = std::string("views/") + fullPath;
            std::lock_guard<std::mutex> lock(g_asarReadMutex);
            size_t asarFileSize = 0;
            const uint8_t* data = asar_read_file(g_asarArchive, asarFilePath.c_str(), &asarFileSize);
            if (data && asarFileSize > 0) {
                fileContents = (gchar*)g_memdup2(data, asarFileSize);
                fileSize = asarFileSize;
                foundFile = true;
                asar_free_buffer(data, asarFileSize);
            }
        }
    }

    if (!foundFile) {
        // Flat-file fallback: Resources/app/views/<fullPath>
        gchar* viewsDir = g_build_filename(resourcesDir, "app", "views", nullptr);
        gchar* filePath = g_build_filename(viewsDir, fullPath, nullptr);
        if (g_file_test(filePath, G_FILE_TEST_EXISTS)) {
            GError* error = nullptr;
            if (g_file_get_contents(filePath, &fileContents, &fileSize, &error)) {
                foundFile = true;
            } else if (error) {
                fprintf(stderr, "[wpe] failed to read %s: %s\n", filePath, error->message);
                g_error_free(error);
            }
        } else {
            fprintf(stderr, "[wpe] views:// file not found: %s\n", filePath);
        }
        g_free(viewsDir);
        g_free(filePath);
    }

    if (foundFile && fileContents) {
        std::string mime = electrobun::getMimeTypeFromUrl(fullPath);
        fprintf(stderr, "[wpe views://] serving %s (%zu bytes, %s)\n", fullPath, (size_t)fileSize, mime.c_str()); fflush(stderr);
        GInputStream* stream = g_memory_input_stream_new_from_data(fileContents, fileSize, g_free);
        webkit_uri_scheme_request_finish(request, stream, fileSize, mime.c_str());
        g_object_unref(stream);
    } else {
        fprintf(stderr, "[wpe views://] 404 for %s\n", fullPath); fflush(stderr);
        GError* err = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "File not found: %s", fullPath);
        webkit_uri_scheme_request_finish_error(request, err);
        g_error_free(err);
    }

    g_free(cwd);
    g_free(resourcesDir);
    g_free(asarPath);
}

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
        fprintf(stderr, "[WpeWebViewImpl] loadURL %s (webView=%p)\n", urlString ? urlString : "(null)", (void*)webView_); fflush(stderr);
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
        (void)spec;  // DRM uses the native display mode; no per-window options.
        // primeWpeView ran in runEventLoop before the loop started, so display_
        // already exists. The kiosk has exactly one window — we return its
        // opaque handle (the DrmDisplay pointer) on every call.
        if (!display_) {
            fprintf(stderr, "[WpeBackend] createWindow called before primeWpeView completed\n");
            return nullptr;
        }
        return display_.get();
    }

    void runEventLoop() override {
        long tid = (long)syscall(SYS_gettid);
        g_mainThreadTid.store(tid, std::memory_order_relaxed);
        fprintf(stderr, "[WpeBackend] runEventLoop: entering on tid=%ld (recorded as main thread)\n", tid); fflush(stderr);

        // WPE-FDO requires its wayland-server source AND the WebKitWebView to
        // exist before g_main_loop_run starts iterating. Setting these up
        // lazily from inside a dispatched callback (after the loop is already
        // running) leaves the WebProcess unable to export frames — empirically
        // zero onExportShm callbacks fire even though the wayland protocol
        // exchange looks identical to wpe_hello's. So we do the full setup
        // now, with a blank URL (the user's createWebview call later swaps in
        // their URL).
        primeWpeView();

        if (g_getenv("ELECTROBUN_FORCE_WPE_HELLO")) {
            fprintf(stderr, "[WpeBackend] FORCE: creating second exportable+view\n"); fflush(stderr);
            wpe_view_backend_exportable_fdo_client client = {};
            client.export_shm_buffer = &WpeBackend::onExportShmStatic;
            auto* exportable2 = wpe_view_backend_exportable_fdo_create(&client, this, landscapeW_, landscapeH_);
            auto* vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable2);
            WebKitWebViewBackend* webviewBackend = webkit_web_view_backend_new(vb, nullptr, nullptr);
            WebKitWebView* webView = webkit_web_view_new(webviewBackend);
            WebKitSettings* settings = webkit_web_view_get_settings(webView);
            webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
            webkit_web_view_load_html(webView,
                "<html><body style='background:#1e2a56;color:#ffbe2e;font-size:140px;text-align:center;padding-top:120px;font-family:sans-serif'>FORCE 2nd</body></html>",
                nullptr);
            auto view2 = std::make_shared<WpeWebViewImpl>(2, this, webView, exportable2);
            primaryView_ = view2.get();
            views_.push_back(view2);
            fprintf(stderr, "[WpeBackend] FORCE: second view ready, now primary=%p\n", (void*)view2.get()); fflush(stderr);
        }

        if (!mainLoop_) mainLoop_ = g_main_loop_new(nullptr, FALSE);

        auto quitFromSignal = +[](gpointer userData) -> gboolean {
            auto* self = static_cast<WpeBackend*>(userData);
            fprintf(stderr, "[WpeBackend] signal received, exiting event loop\n"); fflush(stderr);
            if (self->mainLoop_ && g_main_loop_is_running(self->mainLoop_)) {
                g_main_loop_quit(self->mainLoop_);
            }
            return G_SOURCE_REMOVE;
        };
        if (!signalsInstalled_) {
            g_unix_signal_add(SIGINT,  quitFromSignal, this);
            g_unix_signal_add(SIGTERM, quitFromSignal, this);
            signalsInstalled_ = true;
        }

        fprintf(stderr, "[WpeBackend] runEventLoop: g_main_loop_run starting\n"); fflush(stderr);
        g_main_loop_run(mainLoop_);
        fprintf(stderr, "[WpeBackend] runEventLoop: g_main_loop_run returned\n"); fflush(stderr);
    }

    void stopEventLoop() override {
        if (mainLoop_ && g_main_loop_is_running(mainLoop_)) {
            g_main_loop_quit(mainLoop_);
        }
    }

    // IWebviewBackend

    std::shared_ptr<AbstractView> createWebview(const WebviewSpec& spec) override {
        fprintf(stderr, "[WpeBackend] createWebview: FFI entry tid=%ld webviewId=%u url='%s'\n",
                (long)syscall(SYS_gettid), spec.webviewId, spec.url.c_str()); fflush(stderr);
        if (!primaryView_) {
            fprintf(stderr, "[WpeBackend] createWebview: no primed view (primeWpeView didn't run)\n");
            return nullptr;
        }
        // Bind the user's webviewId onto the existing primed view, and ask
        // WebKit to load the URL on the main thread (load_uri queues internally
        // and the actual load happens during loop iterations).
        primaryView_->webviewId = spec.webviewId;
        if (!spec.url.empty() && !g_getenv("ELECTROBUN_SKIP_USER_URL")) {
            std::string url = spec.url;  // capture by value
            dispatchSyncMain([this, url]() {
                fprintf(stderr, "[WpeBackend] createWebview: loadURL %s on main\n", url.c_str()); fflush(stderr);
                primaryView_->loadURL(url.c_str());
            });
        }
        // Find the shared_ptr we own for primaryView_ and return it (FFI keeps
        // the AbstractView* alive so long as this WpeBackend lives).
        for (auto& v : views_) {
            if (v.get() == primaryView_) return v;
        }
        return nullptr;
    }

private:
    // ---- Pre-loop WPE bring-up: DRM display + exportable + WebKitWebView. ----
    //
    // Must run before g_main_loop_run. The WebKitWebView and exportable are
    // long-lived; the user's createWindow/createWebview FFI calls just bind
    // the user's URL onto the existing view via load_uri (which is async and
    // safe to call from any thread that can dispatch to the main loop).
    void primeWpeView() {
        if (display_) return;  // already primed
        initWpeOnce();

        DrmDisplayConfig cfg{};
        cfg.rotation = Rotation::None;  // rotate in our blit
        display_ = std::make_unique<DrmDisplay>(cfg);
        if (!display_->init()) {
            fprintf(stderr, "[WpeBackend] primeWpeView: DrmDisplay init failed: %s\n",
                    display_->getLastError().c_str());
            display_.reset();
            return;
        }
        rotationQuarters_ = rotationFromEnvOrDefault();
        if (rotationQuarters_ == 1 || rotationQuarters_ == 3) {
            landscapeW_ = display_->logicalHeight();
            landscapeH_ = display_->logicalWidth();
        } else {
            landscapeW_ = display_->logicalWidth();
            landscapeH_ = display_->logicalHeight();
        }
        fprintf(stderr, "[WpeBackend] primeWpeView: wpe %ux%u, drm %ux%u, rot=%d\n",
                landscapeW_, landscapeH_, display_->logicalWidth(), display_->logicalHeight(),
                rotationQuarters_); fflush(stderr);

        if (!g_getenv("ELECTROBUN_NO_INPUT")) {
            InputDispatcherConfig icfg{};
            icfg.screenWidth          = display_->logicalWidth();
            icfg.screenHeight         = display_->logicalHeight();
            icfg.rotationQuarterTurns = rotationQuarters_;
            input_ = std::make_unique<InputDispatcher>(icfg,
                [this](const InputEvent& ev) { this->onInputEvent(ev); });
            if (!input_->start()) {
                fprintf(stderr, "[WpeBackend] primeWpeView: InputDispatcher::start failed (continuing without input)\n");
            }
        } else {
            fprintf(stderr, "[WpeBackend] primeWpeView: InputDispatcher SKIPPED\n"); fflush(stderr);
        }

        // The wpe_view_backend_exportable_fdo_client must out-live the
        // exportable. WPE-FDO holds the pointer (does NOT copy the struct);
        // if `client` is on primeWpeView's stack frame, after primeWpeView
        // returns to runEventLoop the stack memory gets reused and WPE-FDO's
        // callbacks dereference garbage → crash inside g_main_loop_run.
        // Make the client struct a function-local static so it lives forever.
        static wpe_view_backend_exportable_fdo_client client = {};
        client.export_shm_buffer = &WpeBackend::onExportShmStatic;
        auto* exportable = wpe_view_backend_exportable_fdo_create(&client, this, landscapeW_, landscapeH_);
        if (!exportable) {
            fprintf(stderr, "[WpeBackend] primeWpeView: exportable_fdo_create failed\n");
            return;
        }
        auto* vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
        WebKitWebViewBackend* webviewBackend = webkit_web_view_backend_new(vb, nullptr, nullptr);
        WebKitWebView* webView = webkit_web_view_new(webviewBackend);
        WebKitSettings* settings = webkit_web_view_get_settings(webView);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);

        // Load a placeholder HTML so the webview has content before the user's
        // createWebview swaps in their URL. Pre-loop load like wpe_hello.
        if (!g_getenv("ELECTROBUN_NO_PLACEHOLDER")) {
            webkit_web_view_load_html(webView,
                "<html><head><style>html,body{margin:0;padding:0;width:100%;height:100%;}"
                "body{background:linear-gradient(90deg,#1e2a56,#314580);"
                "color:white;display:flex;align-items:center;justify-content:center;"
                "font-family:sans-serif;font-weight:800;font-size:80px;letter-spacing:1px}"
                ".dot{color:#ffbe2e}</style></head>"
                "<body><span>Electrobun WPE<span class='dot'>.</span></span></body></html>",
                nullptr);
        }

        auto view = std::make_shared<WpeWebViewImpl>(/*webviewId=*/1, this, webView, exportable);
        primaryView_ = view.get();
        views_.push_back(view);
        fprintf(stderr, "[WpeBackend] primeWpeView: ready, primary=%p\n", (void*)view.get()); fflush(stderr);
    }

    int rotationFromEnvOrDefault() {
        // Default CCW90 for the kortexa bar panel (480x1920 portrait native).
        // Override via ELECTROBUN_ROTATE=0|90|180|270 (CW degrees from native).
        const char* s = g_getenv("ELECTROBUN_ROTATE");
        if (!s) return 3;  // CCW90 = 270° CW
        int q = atoi(s);
        switch (q) {
            case 0:   return 0;
            case 90:  return 1;
            case 180: return 2;
            case 270: return 3;
            default:  return 3;
        }
    }

    // ---- WPE initialization (once per process) ----
    static void initWpeOnce() {
        static std::once_flag once;
        std::call_once(once, []() {
            fprintf(stderr, "[WpeBackend] wpe_loader_init\n"); fflush(stderr);
            wpe_loader_init("libWPEBackend-fdo-1.0.so");
            fprintf(stderr, "[WpeBackend] wpe_fdo_initialize_shm\n"); fflush(stderr);
            if (!wpe_fdo_initialize_shm()) {
                fprintf(stderr, "[WpeBackend] wpe_fdo_initialize_shm failed\n");
            }
            if (!g_getenv("ELECTROBUN_NO_VIEWS_SCHEME")) {
                fprintf(stderr, "[WpeBackend] webkit_web_context_get_default\n"); fflush(stderr);
                WebKitWebContext* ctx = webkit_web_context_get_default();
                fprintf(stderr, "[WpeBackend] webkit_web_context_register_uri_scheme\n"); fflush(stderr);
                webkit_web_context_register_uri_scheme(
                    ctx, "views", handleViewsURIScheme, nullptr, nullptr);
                fprintf(stderr, "[WpeBackend] views scheme registered\n"); fflush(stderr);
            }
        });
    }

    // ---- SHM export → DRM blit ----

    static void onExportShmStatic(void* userData, struct wpe_fdo_shm_exported_buffer* buffer) {
        static std::atomic<int> n{0};
        int i = ++n;
        if (i <= 3 || (i % 60) == 0) {
            fprintf(stderr, "[WpeBackend] onExportShm #%d\n", i); fflush(stderr);
        }
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
    bool                                       signalsInstalled_ = false;
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
