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
//   libwpe-1.0, libwpebackend-fdo-1.0 (EGLImage path; SHM fallback),
//   libwpewebkit-2.0, libwayland-server (for wl_shm_buffer),
//   libdrm, libgbm, EGL, GLESv2, libinput, libudev, glib-2.0,
//   xdg-desktop-portal (headless `default=none`; installed by the extractor).

#include "../abstract_view.h"
#include "../backend.h"
#include "../../shared/asar.h"
#include "../../shared/callbacks.h"
#include "../../shared/mime_types.h"

#include "drm_display.h"
#include "egl_readback.h"
#include "input.h"

#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // The launcher and ElectrobunCore can both run the shared default GLib
    // context from different threads. A callback already executing inside
    // that context is safe to run inline regardless of which runner acquired
    // it; dispatching synchronously from there would wait on ourselves.
    if (g_main_context_is_owner(g_main_context_default())) return true;

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

struct PendingWindowChromeAction {
    WindowChromeActionHandler handler;
    void* window;
    WindowChromeAction action;
};

static void queueWindowChromeAction(WindowChromeActionHandler handler,
                                    void* window,
                                    WindowChromeAction action) {
    if (!handler || !window) return;
    auto* pending = new PendingWindowChromeAction{handler, window, action};
    // Closing recycles the WebKitWebView, so wait until the current WebKit
    // signal has unwound before touching the owning logical window.
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer ud) -> gboolean {
            auto* p = static_cast<PendingWindowChromeAction*>(ud);
            p->handler(p->window, p->action);
            return G_SOURCE_REMOVE;
        },
        pending,
        +[](gpointer ud) {
            delete static_cast<PendingWindowChromeAction*>(ud);
        });
}

static bool chromeActionForURL(const std::string& url,
                               WindowChromeAction& action) {
    if (url == "electrobun://chrome/close" ||
        url == "electrobun://chrome/close/") {
        action = WindowChromeAction::Close;
        return true;
    }
    if (url == "electrobun://chrome/maximize" ||
        url == "electrobun://chrome/maximize/") {
        action = WindowChromeAction::Maximize;
        return true;
    }
    if (url == "electrobun://chrome/restore" ||
        url == "electrobun://chrome/restore/") {
        action = WindowChromeAction::Restore;
        return true;
    }
    if (url == "electrobun://chrome/reveal" ||
        url == "electrobun://chrome/reveal/") {
        action = WindowChromeAction::Reveal;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// views:// URL scheme handler — reads from app.asar or the filesystem layout.
// Mirrors the GTK-side handler in nativeWrapper.cpp so hello-embedded's
// `views://main/index.html` resolves identically on both backends.
// ---------------------------------------------------------------------------

static AsarArchive*    g_asarArchive = nullptr;
static std::once_flag  g_asarInitFlag;
static std::mutex      g_asarReadMutex;

// Per-request logging is off unless ELECTROBUN_LOG_VIEWS is set — the
// handler runs for every asset fetch on page load.
static bool viewsLogEnabled() {
    static const bool enabled = g_getenv("ELECTROBUN_LOG_VIEWS") != nullptr;
    return enabled;
}

static void handleViewsURIScheme(WebKitURISchemeRequest* request, gpointer /*userData*/) {
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    if (viewsLogEnabled()) {
        fprintf(stderr, "[wpe views://] request uri=%s\n", uri ? uri : "(null)"); fflush(stderr);
    }
    // Strip ?query and #fragment from the URL before resolving against
    // ASAR / disk — a request like `views://main/index.html?t=12345` should
    // serve `main/index.html`. (The GTK handler in nativeWrapper.cpp has the
    // same omission; tracked in §17 follow-ups.)
    std::string pathBuf;
    const char* fullPath = "index.html";
    if (uri && std::strncmp(uri, "views://", 8) == 0) {
        pathBuf = uri + 8;
        size_t cut = pathBuf.find_first_of("?#");
        if (cut != std::string::npos) pathBuf.resize(cut);
        fullPath = pathBuf.c_str();
    }

    // Resolved once — cwd doesn't change after launch, and this handler runs
    // per asset fetch.
    static gchar* resourcesDir = nullptr;
    static gchar* asarPath = nullptr;
    static std::once_flag pathsInitFlag;
    std::call_once(pathsInitFlag, []() {
        gchar* cwd = g_get_current_dir();
        resourcesDir = g_build_filename(cwd, "..", "Resources", nullptr);
        asarPath = g_build_filename(resourcesDir, "app.asar", nullptr);
        g_free(cwd);
    });

    gchar* fileContents = nullptr;
    gsize  fileSize = 0;
    bool   foundFile = false;

    if (g_file_test(asarPath, G_FILE_TEST_EXISTS)) {
        std::call_once(g_asarInitFlag, []() {
            // asarPath has static storage now — referenced directly, not captured.
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
        if (viewsLogEnabled()) {
            fprintf(stderr, "[wpe views://] serving %s (%zu bytes, %s)\n", fullPath, (size_t)fileSize, mime.c_str()); fflush(stderr);
        }
        GInputStream* stream = g_memory_input_stream_new_from_data(fileContents, fileSize, g_free);
        webkit_uri_scheme_request_finish(request, stream, fileSize, mime.c_str());
        g_object_unref(stream);
    } else {
        // 404s always log — they're a real signal, not per-request noise.
        fprintf(stderr, "[wpe views://] 404 for %s\n", fullPath); fflush(stderr);
        GError* err = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "File not found: %s", fullPath);
        webkit_uri_scheme_request_finish_error(request, err);
        g_error_free(err);
    }
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
          exportable_(exportable),
          createdAt_(std::chrono::steady_clock::now()) {}

    ~WpeWebViewImpl() override {
        const bool hadPendingFrame = pendingEgl_ || pendingShm_;
        releasePendingBuffer();
        if ((hadPendingFrame || awaitingFrameComplete_) && exportable_) {
            wpe_view_backend_exportable_fdo_dispatch_frame_complete(exportable_);
        }
        if (webView_)    g_object_unref(webView_);
        if (exportable_) wpe_view_backend_exportable_fdo_destroy(exportable_);
    }

    struct wpe_view_backend* viewBackend() const {
        return exportable_ ? wpe_view_backend_exportable_fdo_get_view_backend(exportable_) : nullptr;
    }

    struct wpe_view_backend_exportable_fdo* exportable() const { return exportable_; }

    void releasePendingBuffer() {
        if (!exportable_) return;
        if (pendingEgl_) {
            wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
                exportable_, pendingEgl_);
            pendingEgl_ = nullptr;
        }
        if (pendingShm_) {
            if (usesEgl_) {
                wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(
                    exportable_, pendingShm_);
            } else {
                wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
                    exportable_, pendingShm_);
            }
            pendingShm_ = nullptr;
        }
    }

    void requestWindowChromeAction(WindowChromeAction action) {
        queueWindowChromeAction(windowChromeActionHandler_, hostWindow_, action);
    }

    // AbstractView

    void loadURL(const char* urlString) override {
        fprintf(stderr, "[WpeWebViewImpl] loadURL %s (webView=%p)\n",
                alwaysTopmost_ ? "<internal-chrome>" :
                    (urlString ? urlString : "(null)"),
                (void*)webView_); fflush(stderr);
        if (webView_ && urlString) {
            loadRequestedAt_ = std::chrono::steady_clock::now();
            firstFrameAfterLoadPending_ = true;
            webkit_web_view_load_uri(webView_, urlString);
        }
    }
    void loadHTML(const char* htmlString) override {
        if (!webView_ || !htmlString) return;
        std::string html = htmlString;
        dispatchSyncMain([this, html = std::move(html)]() {
            if (webView_) {
                webkit_web_view_load_html(webView_, html.c_str(), nullptr);
            }
        });
    }
    void goBack()     override { if (webView_) webkit_web_view_go_back(webView_); }
    void goForward()  override { if (webView_) webkit_web_view_go_forward(webView_); }
    void reload()     override { if (webView_) webkit_web_view_reload(webView_); }
    void remove() override;  // body below the WpeBackend definition
    bool canGoBack()    override { return webView_ && webkit_web_view_can_go_back(webView_); }
    bool canGoForward() override { return webView_ && webkit_web_view_can_go_forward(webView_); }

    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
        if (!webView_ || !jsString) return;
        std::string js = jsString;
        dispatchSyncMain([this, js = std::move(js)]() {
            if (webView_) {
                webkit_web_view_evaluate_javascript(
                    webView_, js.c_str(), -1, nullptr, nullptr,
                    nullptr,
                    +[](GObject* source, GAsyncResult* result, gpointer) {
                        GError* error = nullptr;
                        JSCValue* value =
                            webkit_web_view_evaluate_javascript_finish(
                                WEBKIT_WEB_VIEW(source), result, &error);
                        if (error) {
                            fprintf(stderr,
                                    "[WpeWebViewImpl] evaluate JavaScript failed: %s\n",
                                    error->message);
                            g_error_free(error);
                        }
                        if (value) g_object_unref(value);
                    },
                    nullptr);
            }
        });
    }
    void callAsyncJavascript(const char*, const char* jsString,
                             uint32_t, uint32_t, void*) override {
        // Mainline Electrobun does not bind callAsyncJavaScript on the Bun
        // side (see package/src/bun/proc/native.ts:268 — "Users can use RPC
        // for JavaScript execution"). The macOS impl is fully written but
        // unreachable; GTK/CEF/Win are stubs. We match GTK: fire-and-forget
        // the JS so the symbol does the lexically-closest thing if anyone
        // ever wires the Bun binding back up.
        evaluateJavaScriptWithNoCompletion(jsString);
    }
    void addPreloadScriptToWebView(const char* jsString) override {
        // Inject at DOCUMENT_START on every navigation. Used to set up the
        // window.__electrobun* globals + the Electrobun preload pipeline so
        // the standard RPC / drag-regions / webview-tag features work on
        // WPE the same way they do on macOS/GTK. WpeBackend::primeWpeView
        // assigns userContentManager_ before this is callable; createWebview
        // is the first user of it.
        if (!userContentManager_ || !jsString || !*jsString) return;
        WebKitUserScript* script = webkit_user_script_new(jsString,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(userContentManager_, script);
        webkit_user_script_unref(script);
    }
    void updateCustomPreloadScript(const char* jsString) override {
        // Mirrors nativeWrapper.cpp:2473 (GTK path). Wipe all user scripts,
        // re-inject electrobun preload (load-bearing for window.__electrobun*
        // globals) followed by the new custom preload. Script-message
        // handlers are separate registrations and survive remove_all_scripts.
        customPreloadScript_ = jsString ? jsString : "";
        if (!userContentManager_) return;
        webkit_user_content_manager_remove_all_scripts(userContentManager_);
        if (!electrobunPreloadScript_.empty()) {
            addPreloadScriptToWebView(electrobunPreloadScript_.c_str());
        }
        if (!customPreloadScript_.empty()) {
            addPreloadScriptToWebView(customPreloadScript_.c_str());
        }
    }

    void resize(const Rect& frame, const char* masksJson) override {
        visualBounds = frame;
        frame_ = frame;
        maskJSON = masksJson ? masksJson : "";
        // Forward size so the web engine reflows + the SHM buffer matches
        // the view's bounds; the composite blit reads SHM dimensions and
        // honors them.
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

    // ---- Navigation callbacks (set by WpeBackend::createWebview) ----
    //
    // Mirrors the WebKitGTK members in nativeWrapper.cpp so the same FFI
    // surface (decide-policy + load-changed + load-failed) reaches Bun.
    DecideNavigationCallback navigationCallback_ = nullptr;
    WebviewEventHandler      eventHandler_       = nullptr;
    bool                     lastNavigationWasBlocked_ = false;

    // ---- Webview→Bun message bridges (set by WpeBackend::createWebview) ----
    //
    // The three script-message-handler channels Electrobun expects on every
    // webview. JS-side gets window.webkit.messageHandlers.{bunBridge,
    // internalBridge, eventBridge}.postMessage(json); the registered
    // signal handlers in WpeBackend forward the payload to these JSCallbacks
    // which marshal back to Bun. WPE primeWpeView creates the
    // userContentManager_ and registers the handlers so even before
    // createWebview runs, the JS bridges exist (they just route to a null
    // callback that early-returns).
    HandlePostMessage bunBridgeHandler_      = nullptr;
    HandlePostMessage internalBridgeHandler_ = nullptr;
    HandlePostMessage eventBridgeHandler_    = nullptr;
    WebKitUserContentManager* userContentManager_ = nullptr;  // not owned

    // Latest preload scripts for this view. updateCustomPreloadScript wipes
    // and re-injects both, so we need to remember the electrobun one.
    std::string electrobunPreloadScript_;
    std::string customPreloadScript_;

    // ---- Multi-view pool state ----
    //
    // primeWpeView seeds the pool with one WpeWebViewImpl (satisfies the
    // pre-loop WPE-FDO requirement, §13/§15). createWebview lazily grows
    // the pool by one when claiming would empty it, capped by
    // ELECTROBUN_WPE_MAX_VIEWS as a runaway-leak safety net. Views start in
    // the free pool with about:blank loaded; createWebview pops one and
    // binds the user's webviewId/handlers/preloads; remove() returns it
    // to the pool.
    bool inFreePool_   = true;   // true until createWebview claims this view
    bool alwaysTopmost_ = false; // chrome views set this; rendering & input
                                 // dispatch keep them above app views
    // Trust class of this slot. Set at WebKitWebView creation time and
    // permanent for the WebKitWebView's lifetime — recycle never crosses
    // the boundary because the WebProcess identity is fixed:
    //   trusted   → constructed with the related-view link to the shared
    //               trusted source view, so all trusted views share one
    //               WPEWebProcess.
    //   untrusted → constructed without related-view, so WPE spawns a
    //               fresh WPEWebProcess for this view.
    bool trusted_     = true;
    Rect frame_ = {};            // bounds within the rotated landscape space

    // Most-recent frame this view exported. EGL is the normal GPU path; SHM
    // is retained as a startup fallback. Held across composites so static
    // views (chrome) keep contributing pixels even when they are not
    // producing new frames.
    struct wpe_fdo_egl_exported_image* pendingEgl_ = nullptr;
    struct wpe_fdo_shm_exported_buffer* pendingShm_ = nullptr;
    bool usesEgl_ = false;

    // True while this view has produced a frame whose frame_complete ack is
    // deferred until the composite that includes it is presented. Ties the
    // WebProcess's frame production to the panel's refresh rate instead of
    // free-running raster (see onViewExportedShm / composeAndPresent).
    bool awaitingFrameComplete_ = false;

    // ---- Navigation event handlers ----
    //
    // Ported verbatim from the WebKitGTK path in nativeWrapper.cpp
    // (onDecidePolicy / onLoadChanged / onLoadFailed). WPE uses the same
    // webkit_* C API as GTK, so the bodies are identical except for the
    // GDK ctrl+click block — the kiosk panel has no keyboard modifiers, so
    // that whole code path is dropped here. Navigation rules come from the
    // AbstractView base directly (single-webview target → no g_webviewMap
    // lookup needed; the GTK map only exists because GTK has many views).
    gboolean onDecidePolicy(WebKitWebView* /*webview*/,
                            WebKitPolicyDecision* decision,
                            WebKitPolicyDecisionType type) {
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            WebKitNavigationPolicyDecision* nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
            WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
            WebKitURIRequest* request = webkit_navigation_action_get_request(action);
            const char* uri = webkit_uri_request_get_uri(request);

            std::string url = uri ? uri : "";
            WindowChromeAction chromeAction = WindowChromeAction::Close;
            if (chromeActionForURL(url, chromeAction)) {
                webkit_policy_decision_ignore(decision);
                requestWindowChromeAction(chromeAction);
                return TRUE;
            }
            bool shouldAllow = shouldAllowNavigationToURL(url);

            // Fire will-navigate event with allowed status.
            if (eventHandler_) {
                std::string escapedUrl;
                for (char c : url) {
                    switch (c) {
                        case '"':  escapedUrl += "\\\""; break;
                        case '\\': escapedUrl += "\\\\"; break;
                        default:   escapedUrl += c;       break;
                    }
                }
                std::string eventData = "{\"url\":\"" + escapedUrl + "\",\"allowed\":" +
                                        (shouldAllow ? "true" : "false") + "}";
                eventHandler_(webviewId, strdup("will-navigate"), strdup(eventData.c_str()));
            }

            if (!shouldAllow) {
                lastNavigationWasBlocked_ = true;
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            lastNavigationWasBlocked_ = false;
        }
        return FALSE;
    }

    void onLoadChanged(WebKitWebView* webview, WebKitLoadEvent event) {
        if (!eventHandler_) return;
        const char* uri = webkit_web_view_get_uri(webview);
        switch (event) {
            case WEBKIT_LOAD_STARTED:
                eventHandler_(webviewId, "load-started", uri);
                break;
            case WEBKIT_LOAD_REDIRECTED:
                eventHandler_(webviewId, "load-redirected", uri);
                break;
            case WEBKIT_LOAD_COMMITTED:
                eventHandler_(webviewId, "load-committed", uri);
                break;
            case WEBKIT_LOAD_FINISHED:
                eventHandler_(webviewId, "load-finished", uri);
                if (!lastNavigationWasBlocked_) {
                    eventHandler_(webviewId, "did-navigate", uri);
                }
                break;
        }
    }

    gboolean onLoadFailed(WebKitWebView* /*webview*/, WebKitLoadEvent /*event*/,
                          gchar* uri, GError* /*error*/) {
        if (eventHandler_) {
            eventHandler_(webviewId, "load-failed", uri);
        }
        return FALSE;
    }

private:
    friend class WpeBackend;
    WpeBackend*                              backend_    = nullptr;
    WebKitWebView*                           webView_    = nullptr;
    struct wpe_view_backend_exportable_fdo*  exportable_ = nullptr;
    void*                                    hostWindow_ = nullptr;
    WindowChromeActionHandler                windowChromeActionHandler_ = nullptr;
    bool                                     isHostPrimary_ = false;
    // Per-slot last touch position so TouchUp (no coords) can dispatch at the right spot.
    int32_t                                  lastTouchX_[16] = {0};
    int32_t                                  lastTouchY_[16] = {0};
    std::chrono::steady_clock::time_point     createdAt_;
    std::chrono::steady_clock::time_point     loadRequestedAt_;
    bool                                     firstFrameLogged_ = false;
    bool                                     firstFrameAfterLoadPending_ = false;

};

// ---------------------------------------------------------------------------
// WpeBackend
// ---------------------------------------------------------------------------

class WpeBackend : public IDisplayBackend, public IWebviewBackend {
public:
    WpeBackend() = default;
    ~WpeBackend() override { teardown(); }

    // FFI exports that take only a webviewId (no AbstractView*) reach the
    // impl through here.
    AbstractView* findViewById(uint32_t webviewId) {
        for (auto* v : activeViews_) {
            if (v && v->webviewId == webviewId) return v;
        }
        return nullptr;
    }

    // Called from WpeWebViewImpl::remove(). Removes from activeViews_,
    // resets bound state (so JS bridges/handlers don't fire on reused view),
    // navigates to about:blank, clears the user-content scripts, releases
    // any held SHM, and returns the impl to the free pool. The
    // shared_ptr in views_ keeps the impl alive; the WebKitWebView and
    // exportable_fdo are kept hot so the next createWebview is cheap.
    void recyclePooledView(WpeWebViewImpl* impl) {
        if (!impl || impl->inFreePool_) return;
        void* recycledHostWindow = impl->hostWindow_;
        const bool recycledHostPrimary = impl->isHostPrimary_;
        activeViews_.erase(
            std::remove(activeViews_.begin(), activeViews_.end(), impl),
            activeViews_.end());
        primaryView_ = activeViews_.empty() ? nullptr : activeViews_.back();
        if (recycledHostPrimary && recycledHostWindow) {
            windowRestoreFrames_.erase(recycledHostWindow);
            windowChromeRestoreFrames_.erase(recycledHostWindow);
            primaryBoundFor_.erase(recycledHostWindow);
        }

        // Tear down user state.
        impl->navigationCallback_   = nullptr;
        impl->eventHandler_         = nullptr;
        impl->bunBridgeHandler_     = nullptr;
        impl->internalBridgeHandler_= nullptr;
        impl->eventBridgeHandler_   = nullptr;
        impl->windowChromeActionHandler_ = nullptr;
        impl->hostWindow_    = nullptr;
        impl->isHostPrimary_ = false;
        impl->electrobunPreloadScript_.clear();
        impl->customPreloadScript_.clear();
        impl->isRemoved      = false;
        impl->alwaysTopmost_ = false;
        impl->webviewId      = 0;
        impl->frame_ = Rect{0, 0, (int)landscapeW_, (int)landscapeH_};
        impl->visualBounds = impl->frame_;

        if (impl->userContentManager_) {
            webkit_user_content_manager_remove_all_scripts(impl->userContentManager_);
        }
        if ((impl->pendingEgl_ || impl->pendingShm_) && impl->exportable()) {
            impl->releasePendingBuffer();
            wpe_view_backend_exportable_fdo_dispatch_frame_complete(impl->exportable());
        } else if (impl->awaitingFrameComplete_ && impl->exportable()) {
            // Deferred ack never fired (recycled between export and compose)
            // — release the WebProcess so the pooled view keeps rendering.
            wpe_view_backend_exportable_fdo_dispatch_frame_complete(impl->exportable());
        }
        impl->awaitingFrameComplete_ = false;
        if (impl->webView_) webkit_web_view_load_uri(impl->webView_, "about:blank");

        impl->inFreePool_ = true;
        freePool_.push_back(impl);
        scheduleCompose();  // re-render without the removed view
    }

    // IDisplayBackend

    void setWindowMaximized(void* window, bool maximized) override {
        if (!window) return;
        dispatchSyncMain([this, window, maximized]() {
            WpeWebViewImpl* primary = nullptr;
            WpeWebViewImpl* chrome = nullptr;
            for (auto* view : activeViews_) {
                if (!view || view->hostWindow_ != window) continue;
                if (view->isHostPrimary_) primary = view;
                if (view->alwaysTopmost_) chrome = view;
            }
            if (!primary) return;

            if (maximized) {
                if (windowRestoreFrames_.find(window) != windowRestoreFrames_.end()) {
                    return;
                }
                windowRestoreFrames_[window] = primary->frame_;
                primary->resize(
                    Rect{0, 0, (int)landscapeW_, (int)landscapeH_},
                    "[]");
                if (chrome) {
                    windowChromeRestoreFrames_[window] = chrome->frame_;
                    const int handleW = std::min(76, (int)landscapeW_);
                    const int handleH = std::min(28, (int)landscapeH_);
                    chrome->resize(
                        Rect{((int)landscapeW_ - handleW) / 2,
                             0, handleW, handleH},
                        "[]");
                }
                for (auto* view : activeViews_) {
                    if (view && view->hostWindow_ == window) {
                        view->evaluateJavaScriptWithNoCompletion(
                            "window.__electrobunSetChromeMaximized?.(true)");
                    }
                }
                fprintf(stderr,
                        "[WpeBackend] maximized host window %p (webviewId=%u)\n",
                        window, primary->webviewId);
            } else {
                auto it = windowRestoreFrames_.find(window);
                if (it == windowRestoreFrames_.end()) return;
                primary->resize(it->second, "[]");
                windowRestoreFrames_.erase(it);
                auto chromeIt = windowChromeRestoreFrames_.find(window);
                if (chrome && chromeIt != windowChromeRestoreFrames_.end()) {
                    chrome->resize(chromeIt->second, "[]");
                }
                windowChromeRestoreFrames_.erase(window);
                for (auto* view : activeViews_) {
                    if (view && view->hostWindow_ == window) {
                        view->evaluateJavaScriptWithNoCompletion(
                            "window.__electrobunSetChromeMaximized?.(false)");
                    }
                }
                fprintf(stderr,
                        "[WpeBackend] restored host window %p (webviewId=%u)\n",
                        window, primary->webviewId);
            }
            fflush(stderr);
            scheduleCompose();
        });
    }

    bool isWindowMaximized(void* window) override {
        if (!window) return false;
        bool maximized = false;
        dispatchSyncMain([this, window, &maximized]() {
            maximized =
                windowRestoreFrames_.find(window) != windowRestoreFrames_.end();
        });
        return maximized;
    }

    void revealWindowChrome(void* window) override {
        if (!window) return;
        dispatchSyncMain([this, window]() {
            auto restoreIt = windowChromeRestoreFrames_.find(window);
            if (restoreIt == windowChromeRestoreFrames_.end()) return;

            WpeWebViewImpl* chrome = nullptr;
            for (auto* view : activeViews_) {
                if (view && view->hostWindow_ == window &&
                    view->alwaysTopmost_) {
                    chrome = view;
                }
            }
            if (!chrome) return;

            const int chromeH = std::max(1, restoreIt->second.height);
            chrome->resize(
                Rect{0, 0, (int)landscapeW_, chromeH},
                "[]");
            fprintf(stderr,
                    "[WpeBackend] revealed maximized chrome for host window %p "
                    "(webviewId=%u)\n",
                    window, chrome->webviewId);
            fflush(stderr);
            scheduleCompose();
        });
    }

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

        // ELECTROBUN_FORCE_WPE_HELLO bisect branch removed — was a §13 diag
        // for cracking the dangling-stack-pointer bug (§15), no longer useful
        // and would crash now that onExportShm assumes per-view user_data.

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
        const bool isInternalChrome =
            spec.partition == "__electrobun_chrome__";
        fprintf(stderr, "[WpeBackend] createWebview: FFI entry tid=%ld webviewId=%u url='%s' trust='%s'\n",
                (long)syscall(SYS_gettid), spec.webviewId,
                isInternalChrome ? "<internal-chrome>" : spec.url.c_str(),
                spec.trust.c_str()); fflush(stderr);

        // Trust class drives WebProcess sharing (Stage 2). "" defaults to
        // "trusted" — same-origin app + chrome views share one WebProcess via
        // the related-view link captured on the seed view. "untrusted" gets a
        // fresh WebProcess for the OAuth-popup / external-content case.
        const bool wantTrusted = (spec.trust != "untrusted");

        // Find a recyclable view in the free pool that matches the requested
        // trust class. The pool is small (seed + lazy-grown views), so a
        // linear walk is cheap.
        WpeWebViewImpl* impl = nullptr;
        for (auto it = freePool_.begin(); it != freePool_.end(); ++it) {
            if ((*it)->trusted_ == wantTrusted) {
                impl = *it;
                freePool_.erase(it);
                break;
            }
        }
        if (!impl) {
            // No matching free view — lazy-grow. webkit_* APIs in
            // createOnePooledView require the main thread, so dispatchSyncMain
            // it. ELECTROBUN_WPE_MAX_VIEWS still caps total view count as a
            // runaway-leak safety net (retired in Stage 4).
            const int cap = maxViewsFromEnvOrDefault();
            if ((int)views_.size() >= cap) {
                fprintf(stderr, "[WpeBackend] createWebview: pool at cap (%d). "
                                "Set ELECTROBUN_WPE_MAX_VIEWS to bump.\n", cap);
                return nullptr;
            }
            // Trusted views attach to the shared trusted process via
            // related-view. Untrusted views pass relatedView=nullptr to get
            // their own WebProcess. trustedSourceView_ is set on the seed
            // view in primeWpeView, so it's always non-null for trusted
            // requests by the time createWebview runs.
            WebKitWebView* relatedView = wantTrusted ? trustedSourceView_ : nullptr;
            const uint32_t initialWidth =
                (uint32_t)std::max(1, spec.frame.width);
            const uint32_t initialHeight =
                (uint32_t)std::max(1, spec.frame.height);
            dispatchSyncMain([this, wantTrusted, relatedView,
                              initialWidth, initialHeight, &impl]() {
                impl = createOnePooledView(
                    wantTrusted, relatedView, initialWidth, initialHeight);
            });
            if (!impl) {
                fprintf(stderr, "[WpeBackend] createWebview: lazy createOnePooledView failed\n");
                return nullptr;
            }
            fprintf(stderr, "[WpeBackend] createWebview: lazy-grew pool to %zu views (%s)\n",
                    views_.size(), wantTrusted ? "trusted/shared" : "untrusted/own-process"); fflush(stderr);
        }
        impl->inFreePool_ = false;

        impl->webviewId = spec.webviewId;
        // The framework-owned chrome document has no user navigation
        // surface. Keeping it off the generic callback path also avoids
        // copying its base64 data URL through Bun FFI event strings.
        impl->navigationCallback_   = isInternalChrome
            ? nullptr
            : (DecideNavigationCallback)spec.navigationHandler;
        impl->eventHandler_         = isInternalChrome
            ? nullptr
            : (WebviewEventHandler)spec.webviewEventHandler;
        impl->bunBridgeHandler_     = (HandlePostMessage)spec.bunBridgeHandler;
        impl->internalBridgeHandler_= (HandlePostMessage)spec.internalBridgeHandler;
        impl->eventBridgeHandler_   = (HandlePostMessage)spec.eventBridgeHandler;
        impl->windowChromeActionHandler_ =
            (WindowChromeActionHandler)spec.windowChromeActionHandler;
        impl->hostWindow_           = spec.hostWindow;
        impl->electrobunPreloadScript_ = spec.electrobunPreloadScript;
        impl->customPreloadScript_     = spec.customPreloadScript;
        impl->frame_                = spec.frame;
        // Bare-DRM has no window manager. The first BrowserWindow is fitted to
        // the physical panel. With framework-owned composited chrome its app
        // view starts below the titlebar and receives the actual remaining
        // WebKit viewport; without chrome it fills the panel. Secondary
        // BrowserWindows honor their requested frame.
        const bool isPrimaryView =
            spec.hostWindow != nullptr &&
            primaryBoundFor_.insert(spec.hostWindow).second;
        impl->isHostPrimary_ = isPrimaryView;
        if (isPrimaryView && !firstHostWindow_) {
            firstHostWindow_ = spec.hostWindow;
        }
        const bool isMainPrimary =
            isPrimaryView && spec.hostWindow == firstHostWindow_;
        if (isMainPrimary) {
            if (spec.usesCompositedChrome && impl->frame_.y > 0) {
                const int contentY =
                    std::min(std::max(0, impl->frame_.y),
                             std::max(0, (int)landscapeH_ - 1));
                impl->frame_ = Rect{
                    0, contentY, (int)landscapeW_,
                    std::max(1, (int)landscapeH_ - contentY)};
            } else {
                impl->frame_ =
                    Rect{0, 0, (int)landscapeW_, (int)landscapeH_};
            }
        } else if (impl->frame_.width <= 0 || impl->frame_.height <= 0) {
            // Default any unset bounds to fullscreen.
            impl->frame_ = Rect{0, 0, (int)landscapeW_, (int)landscapeH_};
        } else if (isPrimaryView) {
            // BrowserView coordinates are relative to their BrowserWindow;
            // translate them into the panel compositor's coordinate space.
            impl->frame_.x += spec.windowFrameX;
            impl->frame_.y += spec.windowFrameY;
        }
        impl->visualBounds = impl->frame_;
        // Partition convention for chrome views: the magic string flags this
        // view as topmost within its host BrowserWindow. Other backends
        // (macOS/GTK/CEF/Win) treat it as a normal cookie partition.
        impl->alwaysTopmost_ = isInternalChrome;
        if (impl->alwaysTopmost_) {
            if (spec.hostWindow == firstHostWindow_) {
                impl->frame_.x = 0;
                impl->frame_.width = (int)landscapeW_;
            } else {
                impl->frame_.x += spec.windowFrameX;
                impl->frame_.y += spec.windowFrameY;
            }
            impl->visualBounds = impl->frame_;
        }

        // Tell WPE to render at the view's bounds size — the SHM buffer it
        // exports will match this exactly, which the composite blit copies
        // into compositeBuffer_ at (frame.x, frame.y).
        if (auto* vb = impl->viewBackend()) {
            wpe_view_backend_dispatch_set_size(vb,
                (uint32_t)impl->frame_.width,
                (uint32_t)impl->frame_.height);
        }
        if (!spec.electrobunPreloadScript.empty()) {
            impl->addPreloadScriptToWebView(spec.electrobunPreloadScript.c_str());
        }
        if (!spec.customPreloadScript.empty()) {
            impl->addPreloadScriptToWebView(spec.customPreloadScript.c_str());
        }
        // Keep each BrowserWindow's views as one z-order group. A newer
        // BrowserWindow (e.g. About) sits above the complete older group,
        // while the magic chrome view stays above app views in its own group.
        if (impl->alwaysTopmost_) {
            auto insertAt = activeViews_.end();
            for (auto it = activeViews_.begin(); it != activeViews_.end(); ++it) {
                if ((*it)->hostWindow_ == impl->hostWindow_) {
                    insertAt = std::next(it);
                }
            }
            activeViews_.insert(insertAt, impl);
        } else {
            auto hostChrome = std::find_if(
                activeViews_.begin(), activeViews_.end(),
                [impl](WpeWebViewImpl* view) {
                    return view->hostWindow_ == impl->hostWindow_ &&
                           view->alwaysTopmost_;
                });
            if (hostChrome != activeViews_.end()) {
                activeViews_.insert(hostChrome, impl);
            } else {
                auto insertAt = activeViews_.end();
                for (auto it = activeViews_.begin();
                     it != activeViews_.end(); ++it) {
                    if ((*it)->hostWindow_ == impl->hostWindow_) {
                        insertAt = std::next(it);
                    }
                }
                activeViews_.insert(insertAt, impl);
            }
        }
        // primaryView_ tracks the topmost active view for back-compat with
        // commit 1's still-single-source rendering (commit 2 replaces this
        // with a real composite walk over activeViews_).
        primaryView_ = activeViews_.back();

        if (!spec.url.empty() && !g_getenv("ELECTROBUN_SKIP_USER_URL")) {
            std::string url = spec.url;
            dispatchSyncMain([impl, url]() {
                fprintf(stderr, "[WpeBackend] createWebview: loadURL %s on main (view %p)\n",
                        impl->alwaysTopmost_ ? "<internal-chrome>" : url.c_str(),
                        (void*)impl); fflush(stderr);
                impl->loadURL(url.c_str());
            });
        }
        for (auto& v : views_) {
            if (v.get() == impl) return v;
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

        DrmDisplayConfig cfg{};
        cfg.rotation = Rotation::None;  // rotate in our blit
        display_ = std::make_unique<DrmDisplay>(cfg);
        if (!display_->init()) {
            fprintf(stderr, "[WpeBackend] primeWpeView: DrmDisplay init failed: %s\n",
                    display_->getLastError().c_str());
            display_.reset();
            return;
        }
        rotationQuarters_ = rotationFromRuntime();
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

        eglReadback_ = std::make_unique<EglReadback>();
        if (!eglReadback_->init(
                landscapeW_, landscapeH_, rotationQuarters_)) {
            fprintf(stderr,
                    "[WpeBackend] EGL readback unavailable; falling back to SHM: %s\n",
                    eglReadback_->lastError().c_str());
            fflush(stderr);
            eglReadback_.reset();
        }
        initWpeOnce(eglReadback_ ? eglReadback_->display() : nullptr);

        // Page-flip completions arrive on the DRM fd. Watching it from the
        // main loop lets composeAndPresent defer instead of blocking in
        // acquire() until vblank (which would stall input + bridge traffic).
        drmFdWatchId_ = g_unix_fd_add(display_->fd(), G_IO_IN,
            +[](gint, GIOCondition, gpointer ud) -> gboolean {
                auto* self = static_cast<WpeBackend*>(ud);
                if (!self->display_) return G_SOURCE_REMOVE;
                self->display_->handleEvents();
                if (self->composeDeferredForFlip_) {
                    self->composeDeferredForFlip_ = false;
                    self->composeAndPresent();
                }
                return G_SOURCE_CONTINUE;
            }, this);

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

        // §15 pre-loop requirement: at least one wpe_view_backend_exportable_fdo
        // must exist with a WebKitWebView wired to it before g_main_loop_run
        // starts iterating, otherwise the WebProcess never produces frame
        // exports. Seed the pool with exactly one view — createWebview grows
        // it lazily on demand. Stage 1 of the WebProcess pool optimization
        // (see electrobun-session.md): pays for one WebProcess at startup
        // instead of N, saves ~800 MB when the app uses fewer views than the
        // cap (the common case).
        // Seed view is trusted with no related-view link — it becomes the
        // shared trusted source for all subsequent trusted views (set inside
        // createOnePooledView).
        auto* seedImpl = createOnePooledView(
            /*trusted=*/true, /*relatedView=*/nullptr,
            landscapeW_, landscapeH_);
        if (!seedImpl) {
            fprintf(stderr, "[WpeBackend] primeWpeView: createOnePooledView (seed) failed\n");
            return;
        }
        freePool_.push_back(seedImpl);
        // Keep primaryView_ pointing at the seed view so legacy single-view
        // rendering paths have a target until createWebview claims it.
        primaryView_ = seedImpl;
        fprintf(stderr, "[WpeBackend] primeWpeView: pool seeded with 1 view\n"); fflush(stderr);
    }

    // Build one (exportable_fdo + WebKitWebView + signal connections + pooled
    // WpeWebViewImpl) tuple. Caller adds the impl to freePool_. All views
    // share the same wpe_view_backend_exportable_fdo_client instance; per-view
    // identity is conveyed through the per-exportable user_data (the impl
    // pointer), so the static SHM thunk can route the buffer to the right
    // view without a lookup.
    //
    // `trusted` tags the slot for trust-aware pool recycling. `relatedView`
    // (when non-null) is wired through the WebKitWebView's "related-view"
    // construct property, telling WPE WebKit to reuse the related view's
    // WPEWebProcess instead of spawning a new one. Trusted views pass the
    // shared trustedSourceView_ here; untrusted views pass nullptr to force
    // a fresh WebProcess.
    // `initialWidth`/`initialHeight` must match the first assigned frame:
    // resizing a preloaded WPE-FDO backend can leave a 512 px tile unpainted.
    WpeWebViewImpl* createOnePooledView(bool trusted,
                                        WebKitWebView* relatedView,
                                        uint32_t initialWidth,
                                        uint32_t initialHeight) {
        auto pendingImpl = std::make_shared<WpeWebViewImpl>(/*webviewId=*/0, this,
                                                            /*webView=*/nullptr,
                                                            /*exportable=*/nullptr);
        // Hand the impl pointer as the per-exportable user_data. onExportShm
        // re-casts it back. Holds a raw pointer; the shared_ptr lives in views_.
        WpeWebViewImpl* impl = pendingImpl.get();
        impl->trusted_ = trusted;

        struct wpe_view_backend_exportable_fdo* exportable = nullptr;
        if (eglReadback_) {
            // Static — WPE-FDO holds the pointer, doesn't copy.
            static wpe_view_backend_exportable_fdo_egl_client eglClient = {};
            eglClient.export_fdo_egl_image = &WpeBackend::onExportEglStatic;
            exportable = wpe_view_backend_exportable_fdo_egl_create(
                &eglClient, /*user_data=*/impl, initialWidth, initialHeight);
            impl->usesEgl_ = true;
        } else {
            static wpe_view_backend_exportable_fdo_client shmClient = {};
            shmClient.export_shm_buffer = &WpeBackend::onExportShmStatic;
            exportable = wpe_view_backend_exportable_fdo_create(
                &shmClient, /*user_data=*/impl, initialWidth, initialHeight);
        }
        if (!exportable) {
            fprintf(stderr, "[WpeBackend] createOnePooledView: exportable_fdo_create failed\n");
            return nullptr;
        }
        auto* vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
        WebKitWebViewBackend* webviewBackend = webkit_web_view_backend_new(vb, nullptr, nullptr);
        // related-view is a WebKit construct property (no public C wrapper in
        // the WPE port — see g_object_class_find_property probe). Setting it
        // makes the new WebKitWebView reuse the related view's WPEWebProcess.
        WebKitWebView* webView = nullptr;
        if (relatedView) {
            webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                "backend", webviewBackend,
                "related-view", relatedView,
                nullptr));
        } else {
            webView = webkit_web_view_new(webviewBackend);
        }
        WebKitSettings* settings = webkit_web_view_get_settings(webView);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
        // Match the GTK backend's media capabilities. WPE WebKit ships these
        // APIs disabled by preference, which otherwise removes
        // RTCPeerConnection from the JavaScript global entirely.
        webkit_settings_set_enable_media_stream(settings, TRUE);
        webkit_settings_set_enable_webrtc(settings, TRUE);
        webkit_settings_set_enable_media(settings, TRUE);

        // Bridges: per-view user content manager → per-view callbacks. The
        // signal handlers cast user_data back to WpeWebViewImpl* so multi-
        // view bridge messages route to the right webviewId.
        WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(webView);
        g_signal_connect(manager, "script-message-received::bunBridge",
                         G_CALLBACK(&WpeBackend::onBunBridgeMessageStatic), impl);
        webkit_user_content_manager_register_script_message_handler(manager, "bunBridge", nullptr);
        g_signal_connect(manager, "script-message-received::internalBridge",
                         G_CALLBACK(&WpeBackend::onInternalBridgeMessageStatic), impl);
        webkit_user_content_manager_register_script_message_handler(manager, "internalBridge", nullptr);
        g_signal_connect(manager, "script-message-received::eventBridge",
                         G_CALLBACK(&WpeBackend::onEventBridgeMessageStatic), impl);
        webkit_user_content_manager_register_script_message_handler(manager, "eventBridge", nullptr);
        // Dedicated native-only path for the injected decoration buttons.
        // Unlike the general bridges, this never forwards a C string through
        // Bun's asynchronous FFI callback machinery.
        g_signal_connect(manager, "script-message-received::electrobunChrome",
                         G_CALLBACK(&WpeBackend::onWindowChromeMessageStatic), impl);
        webkit_user_content_manager_register_script_message_handler(
            manager, "electrobunChrome", nullptr);

        // Navigation + load signals — same per-view binding as bridges.
        g_signal_connect(webView, "decide-policy", G_CALLBACK(&WpeBackend::onDecidePolicyStatic), impl);
        g_signal_connect(webView, "load-changed",  G_CALLBACK(&WpeBackend::onLoadChangedStatic),  impl);
        g_signal_connect(webView, "load-failed",   G_CALLBACK(&WpeBackend::onLoadFailedStatic),   impl);

        // Pre-load about:blank so the WebProcess is alive and the view is
        // ready to render the moment createWebview swaps in the user's URL.
        // This is also what satisfies WPE-FDO's pre-loop expectation that
        // each view goes through one full load before the loop iterates.
        webkit_web_view_load_uri(webView, "about:blank");

        impl->webView_ = webView;
        impl->exportable_ = exportable;
        impl->userContentManager_ = manager;
        impl->frame_ =
            Rect{0, 0, (int)initialWidth, (int)initialHeight};

        // Capture the first trusted view's WebKitWebView as the related-view
        // source for all subsequent trusted views — this is what makes them
        // share a WebProcess. The seed view in primeWpeView is created with
        // relatedView=nullptr, so it always sets this pointer first.
        if (trusted && !trustedSourceView_) {
            trustedSourceView_ = webView;
        }

        views_.push_back(pendingImpl);
        return impl;
    }

    int rotationFromRuntime() {
        // An explicit process override wins. This is useful when WPE should
        // intentionally differ from the console or fbcon is unavailable.
        const char* s = g_getenv("ELECTROBUN_ROTATE");
        if (s) {
            char* end = nullptr;
            const long degrees = std::strtol(s, &end, 10);
            if (end && *end == '\0') {
                switch (degrees) {
                    case 0:   return 0;
                    case 90:  return 1;
                    case 180: return 2;
                    case 270: return 3;
                }
            }
            fprintf(stderr,
                    "[WpeBackend] ignoring invalid ELECTROBUN_ROTATE=%s\n", s);
            fflush(stderr);
        }

        // A bare-DRM app replaces the framebuffer console on the same panel,
        // so inherit the console's configured orientation when available.
        // Linux defines 0/1/2/3 as 0/90/180/270 degrees respectively.
        for (const char* path : {
                 "/sys/class/graphics/fbcon/rotate",
                 "/sys/class/graphics/fbcon/rotate_all",
             }) {
            FILE* file = std::fopen(path, "r");
            if (!file) continue;
            int quarters = -1;
            const bool valid =
                std::fscanf(file, "%d", &quarters) == 1 &&
                quarters >= 0 && quarters <= 3;
            std::fclose(file);
            if (valid) {
                fprintf(stderr,
                        "[WpeBackend] rotation=%d discovered from %s\n",
                        quarters, path);
                fflush(stderr);
                return quarters;
            }
        }

        // Generic displays are unrotated unless the system says otherwise.
        return 0;
    }

    // ---- WPE initialization (once per process) ----
    static void initWpeOnce(void* eglDisplay) {
        static std::once_flag once;
        std::call_once(once, [eglDisplay]() {
            fprintf(stderr, "[WpeBackend] wpe_loader_init\n"); fflush(stderr);
            wpe_loader_init("libWPEBackend-fdo-1.0.so");
            if (eglDisplay) {
                fprintf(stderr,
                        "[WpeBackend] wpe_fdo_initialize_for_egl_display\n");
                fflush(stderr);
                if (!wpe_fdo_initialize_for_egl_display(eglDisplay)) {
                    fprintf(stderr,
                            "[WpeBackend] wpe_fdo_initialize_for_egl_display failed\n");
                }
            } else {
                fprintf(stderr, "[WpeBackend] wpe_fdo_initialize_shm\n");
                fflush(stderr);
                if (!wpe_fdo_initialize_shm()) {
                    fprintf(stderr,
                            "[WpeBackend] wpe_fdo_initialize_shm failed\n");
                }
            }
            if (!g_getenv("ELECTROBUN_NO_VIEWS_SCHEME")) {
                fprintf(stderr, "[WpeBackend] webkit_web_context_get_default\n"); fflush(stderr);
                WebKitWebContext* ctx = webkit_web_context_get_default();
                fprintf(stderr, "[WpeBackend] webkit_web_context_register_uri_scheme\n"); fflush(stderr);
                webkit_web_context_register_uri_scheme(
                    ctx, "views", handleViewsURIScheme, nullptr, nullptr);
                // Mark views:// as a secure origin so DOM Storage
                // (localStorage / sessionStorage), IndexedDB, Service
                // Workers, etc. work on app-bundled pages. Without this,
                // WebKit treats custom schemes as non-standard origins and
                // silently no-ops storage APIs — which broke the auto-inject
                // chrome bar's "remember hidden state across navigation".
                // CORS-enabled too, so fetch() across views://<host> URLs
                // works the way apps expect on http(s).
                if (auto* sm = webkit_web_context_get_security_manager(ctx)) {
                    webkit_security_manager_register_uri_scheme_as_secure(sm, "views");
                    webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "views");
                    fprintf(stderr, "[WpeBackend] views scheme marked secure + cors-enabled\n"); fflush(stderr);
                }
                fprintf(stderr, "[WpeBackend] views scheme registered\n"); fflush(stderr);
            }
        });
    }

    // ---- Navigation signal thunks ----
    //
    // user_data is the WpeWebViewImpl* the signal was connected on
    // (createOnePooledView passes the impl directly). The per-view eventHandler_
    // is null until createWebview binds the user's callbacks; until then the
    // thunks see no eventHandler_ and skip user-visible event emission.
    static gboolean onDecidePolicyStatic(WebKitWebView* w,
                                         WebKitPolicyDecision* d,
                                         WebKitPolicyDecisionType t,
                                         gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl) return FALSE;
        return impl->onDecidePolicy(w, d, t);
    }
    static void onLoadChangedStatic(WebKitWebView* w, WebKitLoadEvent e, gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl) return;
        impl->onLoadChanged(w, e);
    }
    static gboolean onLoadFailedStatic(WebKitWebView* w, WebKitLoadEvent e,
                                       gchar* uri, GError* error, gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl) return FALSE;
        return impl->onLoadFailed(w, e, uri, error);
    }

    // ---- Webview→Bun message bridge thunks ----
    //
    // Forward postMessage payloads from JS (via the user-content-manager's
    // script-message-received signal) to the per-webview FFI callback set
    // by createWebview. WPE-WebKit 2.0 deprecated WebKitJavascriptResult —
    // the signal now passes JSCValue* directly, so the GTK code in
    // nativeWrapper.cpp:2630-2715 has an extra `_get_js_value` step that
    // we skip here.
    //
    // Lifetime: same deferred-free contract as GTK (the Bun JSCallback may
    // still be using the string asynchronously when the signal handler
    // returns), but WITHOUT the thread-per-message + 1s sleep GTK uses —
    // that's a thread spawn per RPC message. A low-priority GLib timeout on
    // the already-running main loop frees the buffers instead.
    static void forwardBridgeMessage(HandlePostMessage handler,
                                     uint32_t webviewId,
                                     JSCValue* value) {
        if (!handler || !value) return;
        if (!JSC_IS_VALUE(value) || !jsc_value_is_string(value)) return;
        gchar* str_value = jsc_value_to_string(value);
        if (!str_value) return;
        size_t len = strlen(str_value);
        char* message_copy = new char[len + 1];
        std::memcpy(message_copy, str_value, len + 1);
        handler(webviewId, message_copy);

        struct DeferredFree { char* copy; gchar* str; };
        auto* deferred = new DeferredFree{message_copy, str_value};
        g_timeout_add_seconds_full(G_PRIORITY_LOW, 1,
            +[](gpointer) -> gboolean { return G_SOURCE_REMOVE; },
            deferred,
            +[](gpointer ud) {
                auto* d = static_cast<DeferredFree*>(ud);
                delete[] d->copy;
                g_free(d->str);
                delete d;
            });
    }
    static void onBunBridgeMessageStatic(WebKitUserContentManager*,
                                         JSCValue* value,
                                         gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl) return;
        forwardBridgeMessage(impl->bunBridgeHandler_, impl->webviewId, value);
    }
    static void onInternalBridgeMessageStatic(WebKitUserContentManager*,
                                              JSCValue* value,
                                              gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl) return;
        forwardBridgeMessage(impl->internalBridgeHandler_, impl->webviewId, value);
    }
    static void onEventBridgeMessageStatic(WebKitUserContentManager*,
                                           JSCValue* value,
                                           gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl) return;
        forwardBridgeMessage(impl->eventBridgeHandler_, impl->webviewId, value);
    }
    static void onWindowChromeMessageStatic(WebKitUserContentManager*,
                                            JSCValue* value,
                                            gpointer user_data) {
        auto* impl = static_cast<WpeWebViewImpl*>(user_data);
        if (!impl || !value || !JSC_IS_VALUE(value) ||
            !jsc_value_is_string(value)) {
            return;
        }

        gchar* raw = jsc_value_to_string(value);
        if (!raw) return;
        WindowChromeAction action = WindowChromeAction::Close;
        bool recognized = true;
        if (strcmp(raw, "close") == 0) {
            action = WindowChromeAction::Close;
        } else if (strcmp(raw, "maximize") == 0) {
            action = WindowChromeAction::Maximize;
        } else if (strcmp(raw, "restore") == 0) {
            action = WindowChromeAction::Restore;
        } else if (strcmp(raw, "reveal") == 0) {
            action = WindowChromeAction::Reveal;
        } else {
            recognized = false;
        }
        g_free(raw);

        if (recognized) {
            fprintf(stderr,
                    "[WpeBackend] received window chrome action %u "
                    "(webviewId=%u)\n",
                    static_cast<unsigned>(action), impl->webviewId);
            fflush(stderr);
            impl->requestWindowChromeAction(action);
        }
    }

    // ---- EGL/SHM export → DRM presentation ----

    static void onExportEglStatic(
        void* userData,
        struct wpe_fdo_egl_exported_image* image) {
        auto* impl = static_cast<WpeWebViewImpl*>(userData);
        const auto now = std::chrono::steady_clock::now();
        const uint32_t imageWidth =
            image ? wpe_fdo_egl_exported_image_get_width(image) : 0;
        const uint32_t imageHeight =
            image ? wpe_fdo_egl_exported_image_get_height(image) : 0;
        if (impl && !impl->firstFrameLogged_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl->createdAt_);
            fprintf(stderr,
                    "[WpeBackend] first WPE frame in %lld ms "
                    "(view %p, webviewId=%u, egl=%ux%u, frame=%dx%d)\n",
                    static_cast<long long>(elapsed.count()), (void*)impl,
                    impl->webviewId, imageWidth, imageHeight,
                    impl->frame_.width, impl->frame_.height);
            fflush(stderr);
            impl->firstFrameLogged_ = true;
        }
        if (impl && impl->firstFrameAfterLoadPending_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl->loadRequestedAt_);
            fprintf(stderr,
                    "[WpeBackend] first frame after loadURL in %lld ms (webviewId=%u)\n",
                    static_cast<long long>(elapsed.count()), impl->webviewId);
            fflush(stderr);
            impl->firstFrameAfterLoadPending_ = false;
        }
        static std::atomic<int> n{0};
        const int i = ++n;
        if (i <= 3 || (i % 60) == 0) {
            fprintf(stderr,
                    "[WpeBackend] onExportEgl #%d "
                    "(view %p, webviewId=%u, image=%ux%u)\n",
                    i, (void*)impl, impl ? impl->webviewId : 0u,
                    imageWidth, imageHeight);
            fflush(stderr);
        }
        if (!impl || !impl->backend_) return;
        impl->backend_->onViewExportedEgl(impl, image);
    }

    static void onExportShmStatic(void* userData, struct wpe_fdo_shm_exported_buffer* buffer) {
        auto* impl = static_cast<WpeWebViewImpl*>(userData);
        const auto now = std::chrono::steady_clock::now();
        if (impl && !impl->firstFrameLogged_) {
            struct wl_shm_buffer* shm =
                buffer
                    ? wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer)
                    : nullptr;
            const int32_t shmWidth = shm ? wl_shm_buffer_get_width(shm) : 0;
            const int32_t shmHeight = shm ? wl_shm_buffer_get_height(shm) : 0;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl->createdAt_);
            fprintf(stderr,
                    "[WpeBackend] first WPE frame in %lld ms "
                    "(view %p, webviewId=%u, shm=%dx%d, frame=%dx%d)\n",
                    static_cast<long long>(elapsed.count()), (void*)impl,
                    impl->webviewId, shmWidth, shmHeight,
                    impl->frame_.width, impl->frame_.height);
            fflush(stderr);
            impl->firstFrameLogged_ = true;
        }
        if (impl && impl->firstFrameAfterLoadPending_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl->loadRequestedAt_);
            fprintf(stderr,
                    "[WpeBackend] first frame after loadURL in %lld ms (webviewId=%u)\n",
                    static_cast<long long>(elapsed.count()), impl->webviewId);
            fflush(stderr);
            impl->firstFrameAfterLoadPending_ = false;
        }
        static std::atomic<int> n{0};
        int i = ++n;
        if (i <= 3 || (i % 60) == 0) {
            fprintf(stderr, "[WpeBackend] onExportShm #%d (view %p, webviewId=%u)\n",
                    i, (void*)impl, impl ? impl->webviewId : 0u); fflush(stderr);
        }
        if (!impl || !impl->backend_) return;
        impl->backend_->onViewExportedShm(impl, buffer);
    }

    void onViewExportedEgl(
        WpeWebViewImpl* view,
        struct wpe_fdo_egl_exported_image* image) {
        if (!view || !view->exportable()) return;

        view->releasePendingBuffer();
        view->pendingEgl_ = image;
        if (view->inFreePool_ || !display_) {
            wpe_view_backend_exportable_fdo_dispatch_frame_complete(
                view->exportable());
        } else {
            view->awaitingFrameComplete_ = true;
            scheduleCompose();
        }
    }

    // Per-view SHM frame arrival. The buffer is held on the view (replacing
    // any previous one — old frames are dropped when a newer one arrives
    // before composite). composeAndPresent walks all active views and blits
    // each pendingShm_ into compositeBuffer_ at view.frame_, then a single
    // rotate-blit pushes the composite to DRM.
    //
    // Static views (e.g. chrome that paints once) keep their pendingShm_
    // across composites; an animating view replaces its pendingShm_ on every
    // frame and the previous one is released-and-frame-completed at swap.
    void onViewExportedShm(WpeWebViewImpl* view,
                           struct wpe_fdo_shm_exported_buffer* buffer) {
        if (!view || !view->exportable()) return;

        // Release the previous held buffer (if any) back to WPE for reuse.
        view->releasePendingBuffer();
        view->pendingShm_ = buffer;
        // frame_complete tells WPE-FDO it can produce the next frame; every
        // exported frame must eventually get one or the renderer stalls.
        //
        // Pool-parked views (about:blank warmers) and no-display fallback:
        // never composited, so ack immediately to keep their render loop
        // alive. Active views: defer the ack until composeAndPresent has
        // queued the flip that includes this frame — that throttles the
        // WebProcess to the panel's refresh rate instead of free-running
        // raster (pure CPU savings on animated content, steadier pacing).
        if (view->inFreePool_ || !display_) {
            wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable());
        } else {
            view->awaitingFrameComplete_ = true;
            scheduleCompose();
        }
    }

    // Coalesce composite scheduling. If a frame is already pending we don't
    // schedule another — the next compose pass will pick up whatever each
    // view's pendingShm_ is at fire time.
    void scheduleCompose() {
        if (composeScheduled_) return;
        composeScheduled_ = true;
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer ud) -> gboolean {
            auto* self = static_cast<WpeBackend*>(ud);
            self->composeScheduled_ = false;
            self->composeAndPresent();
            return G_SOURCE_REMOVE;
        }, this, nullptr);
    }

    // True when `v` (with its current SHM buffer) would overwrite every pixel
    // of the composite space — the common kiosk case of one force-fitted
    // fullscreen view. Lets composeAndPresent skip the full-frame clear and
    // take the direct-blit fast path. wl_shm_buffer_get_width/height do not
    // require begin_access.
    bool viewCoversComposite(WpeWebViewImpl* v) const {
        if (!v || (!v->pendingEgl_ && !v->pendingShm_)) return false;
        if (v->frame_.x != 0 || v->frame_.y != 0 ||
            v->frame_.width  < (int)landscapeW_ ||
            v->frame_.height < (int)landscapeH_) return false;
        if (v->pendingEgl_) {
            return
                wpe_fdo_egl_exported_image_get_width(v->pendingEgl_) >=
                    landscapeW_ &&
                wpe_fdo_egl_exported_image_get_height(v->pendingEgl_) >=
                    landscapeH_;
        }
        struct wl_shm_buffer* shm =
            wpe_fdo_shm_exported_buffer_get_shm_buffer(v->pendingShm_);
        return shm &&
               wl_shm_buffer_get_width(shm)  >= (int)landscapeW_ &&
               wl_shm_buffer_get_height(shm) >= (int)landscapeH_;
    }

    // Walk activeViews_ in insertion order (back = topmost; later overlays
    // earlier). Each view's pendingShm_ is copied into compositeBuffer_ at
    // view.frame_; then a single rotate-blit pushes the composite to DRM.
    // Held buffers stay held — see onViewExportedShm.
    //
    // Single fullscreen view takes a fast path: rotate-blit its SHM straight
    // into the DRM back buffer, skipping the intermediate composite copy AND
    // the full-frame clear (one memory pass instead of three).
    void composeAndPresent() {
        if (!display_) return;

        // A flip is still in flight — acquire() would block the GLib main
        // loop until vblank, stalling input dispatch and bridge messages.
        // Defer: the DRM-fd watch re-runs us when the flip completes.
        if (display_->flipPending()) {
            composeDeferredForFlip_ = true;
            return;
        }

        DrmFrame dst = display_->acquire();  // non-blocking (no flip pending)

        if (eglReadback_) {
            std::vector<EglLayer> layers;
            layers.reserve(activeViews_.size());
            for (auto* view : activeViews_) {
                if (!view || !view->pendingEgl_) continue;
                layers.push_back(EglLayer{
                    .image = wpe_fdo_egl_exported_image_get_egl_image(
                        view->pendingEgl_),
                    .x = view->frame_.x,
                    .y = view->frame_.y,
                    .width = view->frame_.width,
                    .height = view->frame_.height,
                });
            }
            if (eglReadback_->canComposeToScanout()) {
                if (!eglReadback_->composeToScanout(
                        layers,
                        dst.pixels,
                        dst.pitch,
                        dst.width,
                        dst.height)) {
                    return;
                }
            } else {
                if (!eglReadback_->compose(layers)) return;
                blitWithRotation(
                    eglReadback_->pixels(), eglReadback_->stride(),
                    landscapeW_, landscapeH_,
                    dst.pixels, dst.pitch, dst.width, dst.height);
            }
        } else {
            const bool singleFullscreen =
                activeViews_.size() == 1 &&
                viewCoversComposite(activeViews_[0]);

            if (singleFullscreen) {
                WpeWebViewImpl* view = activeViews_[0];
                struct wl_shm_buffer* shm =
                    wpe_fdo_shm_exported_buffer_get_shm_buffer(view->pendingShm_);
                wl_shm_buffer_begin_access(shm);
                blitWithRotation(
                    (const uint8_t*)wl_shm_buffer_get_data(shm),
                    wl_shm_buffer_get_stride(shm),
                    landscapeW_, landscapeH_,
                    dst.pixels, dst.pitch, dst.width, dst.height);
                wl_shm_buffer_end_access(shm);
            } else {
                const size_t neededBytes =
                    (size_t)landscapeW_ * landscapeH_ * 4;
                // Clear to opaque black so uncovered regions don't show stale
                // pixels — skippable when some view overwrites every pixel.
                bool fullyCovered = false;
                for (auto* view : activeViews_) {
                    if (viewCoversComposite(view)) {
                        fullyCovered = true;
                        break;
                    }
                }
                if (compositeBuffer_.size() != neededBytes) {
                    compositeBuffer_.assign(neededBytes, 0);
                } else if (!fullyCovered) {
                    std::memset(compositeBuffer_.data(), 0, neededBytes);
                }
                const uint32_t compStride = landscapeW_ * 4;

                for (auto* view : activeViews_) {
                    if (!view || !view->pendingShm_) continue;
                    struct wl_shm_buffer* shm =
                        wpe_fdo_shm_exported_buffer_get_shm_buffer(
                            view->pendingShm_);
                    if (!shm) continue;

                    wl_shm_buffer_begin_access(shm);
                    const uint8_t* src =
                        (const uint8_t*)wl_shm_buffer_get_data(shm);
                    int32_t srcStride = wl_shm_buffer_get_stride(shm);
                    int32_t srcW = wl_shm_buffer_get_width(shm);
                    int32_t srcH = wl_shm_buffer_get_height(shm);

                    // Blit src → compositeBuffer_ at view.frame_, clipped to
                    // the source, view bounds, and composite bounds.
                    const int dstX0 = std::max(0, view->frame_.x);
                    const int dstY0 = std::max(0, view->frame_.y);
                    const int srcX0 = dstX0 - view->frame_.x;
                    const int srcY0 = dstY0 - view->frame_.y;
                    const int blitW = std::min({
                        (int)srcW - srcX0,
                        view->frame_.width - (dstX0 - view->frame_.x),
                        (int)landscapeW_ - dstX0,
                    });
                    const int blitH = std::min({
                        (int)srcH - srcY0,
                        view->frame_.height - (dstY0 - view->frame_.y),
                        (int)landscapeH_ - dstY0,
                    });
                    if (blitW > 0 && blitH > 0) {
                        for (int r = 0; r < blitH; r++) {
                            std::memcpy(
                                compositeBuffer_.data() +
                                    (size_t)(dstY0 + r) * compStride +
                                    (size_t)dstX0 * 4,
                                src + (size_t)(srcY0 + r) * srcStride +
                                    (size_t)srcX0 * 4,
                                (size_t)blitW * 4);
                        }
                    }
                    wl_shm_buffer_end_access(shm);
                }

                blitWithRotation(
                    compositeBuffer_.data(), compStride,
                    landscapeW_, landscapeH_,
                    dst.pixels, dst.pitch, dst.width, dst.height);
            }
        }

        display_->present();
        framesRendered_++;

        // The flip that includes every held frame is queued — release the
        // WebProcesses to produce their next frames (see onViewExportedShm).
        for (auto* view : activeViews_) {
            if (view && view->awaitingFrameComplete_ && view->exportable()) {
                view->awaitingFrameComplete_ = false;
                wpe_view_backend_exportable_fdo_dispatch_frame_complete(view->exportable());
            }
        }
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
    // coordinate system. We map back to pixel, then apply the inverse blit
    // rotation to get landscape (composite-space) pixel coords, then hit-test
    // against active views in reverse-z order (topmost first) to pick the
    // target. The final pointer event uses view-local coords so the page's
    // event.clientX/Y come out right.
    //
    // Touch slot bookkeeping lives on the backend (touchSlots_): a TouchDown
    // captures the target view + view-local coords; subsequent Motion/Up
    // events route to the same view even if the finger drifts outside its
    // bounds, matching standard "implicit pointer capture" behavior.
    void onInputEvent(const InputEvent& ev) {
        if (!display_ || activeViews_.empty()) return;

        const uint32_t screenW = display_->logicalWidth();
        const uint32_t screenH = display_->logicalHeight();
        const int32_t drmX = (int32_t)(ev.x * (double)screenW);
        const int32_t drmY = (int32_t)(ev.y * (double)screenH);

        int32_t landX = 0, landY = 0;
        switch (rotationQuarters_) {
            case 1:  landX = drmY;                            landY = (int32_t)landscapeH_ - 1 - drmX; break;
            case 2:  landX = (int32_t)landscapeW_ - 1 - drmX; landY = (int32_t)landscapeH_ - 1 - drmY; break;
            case 3:  landX = (int32_t)landscapeW_ - 1 - drmY; landY = drmX;                            break;
            default: landX = drmX;                            landY = drmY;                            break;
        }

        // Hit-test in reverse-z order: topmost view containing (landX,landY).
        // Matches nativeWrapper.cpp:7828 (rbegin/rend, first match wins).
        auto hitTest = [&](int32_t lx, int32_t ly) -> WpeWebViewImpl* {
            for (auto it = activeViews_.rbegin(); it != activeViews_.rend(); ++it) {
                WpeWebViewImpl* v = *it;
                if (!v) continue;
                const Rect& f = v->frame_;
                if (lx >= f.x && lx < f.x + f.width &&
                    ly >= f.y && ly < f.y + f.height) {
                    return v;
                }
            }
            return nullptr;
        };

        auto dispatchPointerTo = [&](WpeWebViewImpl* v, int32_t viewX, int32_t viewY,
                                     enum wpe_input_pointer_event_type t,
                                     uint32_t button, uint32_t state) {
            if (!v) return;
            auto* vb = v->viewBackend();
            if (!vb) return;
            struct wpe_input_pointer_event pe = {};
            pe.type   = t;
            pe.time   = ev.timeMs;
            pe.x      = viewX;
            pe.y      = viewY;
            pe.button = button;
            pe.state  = state;
            wpe_view_backend_dispatch_pointer_event(vb, &pe);
        };

        // Build a wpe_input_touch_event from the current touchSlots_ targeted
        // at `v` and dispatch it to that view's backend. The touchpoints array
        // contains every active slot bound to v; the slot that triggered this
        // dispatch (`triggerSlot`) carries `triggerType` (down/motion/up), all
        // other still-down slots are reported as motion. WebKit's touch-to-
        // click path applies its own slop tolerance, so fat-finger drift no
        // longer kills click synthesis (the wpe_input_pointer_event path
        // required exact mousedown→mouseup alignment, which fingers never
        // give you on a capacitive panel) and TouchEvent dispatch enables
        // overflow:scroll touch-scrolling natively in the page.
        auto dispatchTouchTo = [&](WpeWebViewImpl* v,
                                   enum wpe_input_touch_event_type triggerType,
                                   int triggerSlot) {
            if (!v) return;
            auto* vb = v->viewBackend();
            if (!vb) return;
            std::vector<struct wpe_input_touch_event_raw> points;
            points.reserve(16);
            for (int s = 0; s < 16; ++s) {
                if (touchSlots_[s].view != v) continue;
                struct wpe_input_touch_event_raw r = {};
                r.type = (s == triggerSlot)
                             ? triggerType
                             : wpe_input_touch_event_type_motion;
                r.time = ev.timeMs;
                r.id   = s;
                r.x    = touchSlots_[s].x;
                r.y    = touchSlots_[s].y;
                points.push_back(r);
            }
            struct wpe_input_touch_event te = {};
            te.touchpoints        = points.empty() ? nullptr : points.data();
            te.touchpoints_length = points.size();
            te.type               = triggerType;
            te.id                 = triggerSlot;
            te.time               = ev.timeMs;
            te.modifiers          = 0;
            wpe_view_backend_dispatch_touch_event(vb, &te);
        };

        switch (ev.type) {
            case InputEventType::TouchDown: {
                int s = std::max(0, std::min(15, ev.touchSlot));
                WpeWebViewImpl* target = hitTest(landX, landY);
                if (!target) target = activeViews_.back();  // fallback: topmost
                const int32_t viewX = landX - target->frame_.x;
                const int32_t viewY = landY - target->frame_.y;
                touchSlots_[s].view = target;
                touchSlots_[s].x    = viewX;
                touchSlots_[s].y    = viewY;
                target->lastTouchX_[s] = viewX;
                target->lastTouchY_[s] = viewY;
                dispatchTouchTo(target, wpe_input_touch_event_type_down, s);
                break;
            }
            case InputEventType::TouchMotion: {
                int s = std::max(0, std::min(15, ev.touchSlot));
                WpeWebViewImpl* target = touchSlots_[s].view;
                if (!target) break;  // motion without prior down — drop
                const int32_t viewX = landX - target->frame_.x;
                const int32_t viewY = landY - target->frame_.y;
                touchSlots_[s].x = viewX;
                touchSlots_[s].y = viewY;
                target->lastTouchX_[s] = viewX;
                target->lastTouchY_[s] = viewY;
                dispatchTouchTo(target, wpe_input_touch_event_type_motion, s);
                break;
            }
            case InputEventType::TouchUp: {
                int s = std::max(0, std::min(15, ev.touchSlot));
                WpeWebViewImpl* target = touchSlots_[s].view;
                if (!target) break;
                // Dispatch BEFORE clearing the slot so the up-touchpoint is
                // present in the touchpoints array with its final coordinates.
                dispatchTouchTo(target, wpe_input_touch_event_type_up, s);
                touchSlots_[s].view = nullptr;
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
        activeViews_.clear();
        freePool_.clear();
        views_.clear();
        primaryView_ = nullptr;
        if (input_) { input_->stop(); input_.reset(); }
        if (drmFdWatchId_) { g_source_remove(drmFdWatchId_); drmFdWatchId_ = 0; }
        eglReadback_.reset();
        display_.reset();
        if (mainLoop_) { g_main_loop_unref(mainLoop_); mainLoop_ = nullptr; }
    }

    std::unique_ptr<DrmDisplay>       display_;
    std::unique_ptr<EglReadback>      eglReadback_;
    std::unique_ptr<InputDispatcher>  input_;
    GMainLoop*                        mainLoop_ = nullptr;

    // WPE view dims (pre-rotation). For the 480×1920 bar, landscape is 1920×480.
    uint32_t                          landscapeW_ = 0;
    uint32_t                          landscapeH_ = 0;
    int                               rotationQuarters_ = 0;

    std::vector<std::shared_ptr<AbstractView>> views_;        // owns all pooled views
    std::vector<WpeWebViewImpl*>               freePool_;     // available for createWebview (mixed trust)
    std::vector<WpeWebViewImpl*>               activeViews_;  // in z-order; back = topmost
    WpeWebViewImpl*                            primaryView_ = nullptr;  // topmost active view
    // First trusted WebKitWebView created. All subsequent trusted views are
    // constructed with the "related-view" GObject property pointing at this
    // one, which makes WPE WebKit reuse its WPEWebProcess. Set once on the
    // seed view in primeWpeView; never reset (recycle keeps the WebKitWebView
    // alive, so the pointer stays valid for the process lifetime).
    WebKitWebView*                             trustedSourceView_ = nullptr;
    // First BrowserWindow's host handle. Used in createWebview to decide
    // whether to fullscreen-promote a primary view: only the main window's
    // primary view fills the whole panel; secondary BrowserWindows (About,
    // OAuth popups, etc.) honor their requested frame. Set on the first
    // primary-view bind; never reset.
    void*                                      firstHostWindow_   = nullptr;
    // Tracks which hostWindows have already had their primary (BrowserWindow's
    // implicit) view bound. Entries are removed when that primary is recycled.
    std::unordered_set<void*>                  primaryBoundFor_;
    // Presence means maximized; value is the primary view's compositor frame
    // to restore. This makes secondary windows expand across the actual panel
    // instead of merely hiding decorations inside their old rectangle.
    std::unordered_map<void*, Rect>             windowRestoreFrames_;
    // Normal compositor frame of the framework-owned chrome view. While
    // maximized that view shrinks to the top-center reveal handle; revealing
    // temporarily expands it across the panel without resizing app content.
    std::unordered_map<void*, Rect>             windowChromeRestoreFrames_;
    std::atomic<uint64_t>                      framesRendered_{0};
    bool                                       signalsInstalled_ = false;

    // Composite-pass scratch buffer in landscape (pre-rotation) space. Each
    // composeAndPresent walk copies all active views into here, then a single
    // blitWithRotation pushes the result to DRM. ~3.7MB at 1920x480.
    std::vector<uint8_t>                       compositeBuffer_;
    bool                                       composeScheduled_ = false;
    // Set when composeAndPresent found a flip still in flight; the DRM-fd
    // watch (drmFdWatchId_) re-runs the compose when the flip completes.
    bool                                       composeDeferredForFlip_ = false;
    guint                                      drmFdWatchId_ = 0;

    // Per-touch-slot capture state. TouchDown picks a target view; subsequent
    // Motion/Up on the same slot stick with that view (implicit pointer
    // capture). Cleared on Up.
    struct TouchSlotState {
        WpeWebViewImpl* view = nullptr;
        int32_t         x    = 0;
        int32_t         y    = 0;
    };
    TouchSlotState                             touchSlots_[16] = {};

    // Cap on lazy pool growth. Default 5 (chrome bar + ~4 app views). Override
    // with ELECTROBUN_WPE_MAX_VIEWS. Each WPE WebView slot spawns its own
    // WPEWebProcess (~200MB resident on Pi) — the cap is a runaway-leak safety
    // net, not a steady-state allocation. Retired in Stage 4 once related-view
    // sharing collapses trusted views onto a single shared WebProcess.
    static int maxViewsFromEnvOrDefault() {
        const char* s = g_getenv("ELECTROBUN_WPE_MAX_VIEWS");
        if (!s || !*s) return 5;
        int n = atoi(s);
        if (n < 1)  return 1;
        if (n > 32) return 32;
        return n;
    }
};

// Defined out-of-line so backend_->recyclePooledView is callable (the class
// body would otherwise see only a forward declaration of WpeBackend).
inline void WpeWebViewImpl::remove() {
    isRemoved = true;
    if (backend_) backend_->recyclePooledView(this);
}

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

AbstractView* wpeFindViewById(uint32_t webviewId) {
    return wpeBackendInstance().findViewById(webviewId);
}

} // namespace electrobun
