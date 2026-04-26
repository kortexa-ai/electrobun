// backend.h — minimal seam for pluggable display + webview backends on Linux.
//
// Phase 1: GTK+WebKit+CEF implementations live in nativeWrapper.cpp and are
// wrapped behind these interfaces. No new functionality.
// Phase 2: WPE+DRM/KMS implementation added as a separate translation unit
// (package/src/native/linux/wpe/) implementing the same interfaces.
//
// Design intent: keep this header small. Methods get added here only when a
// new backend actually needs them — not preemptively. Most existing FFI
// functions (clipboard, tray, file dialogs, global shortcuts, display
// enumeration) stay as direct GTK/X11 calls in nativeWrapper.cpp and become
// noop or platform-specific stubs on non-GTK backends as needed.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace electrobun {

// Backend-neutral rectangle. Replaces GdkRectangle in cross-backend
// signatures (e.g. AbstractView::resize). Field names match GdkRectangle
// so converting a GdkRectangle is a member-by-member assignment with no
// rename churn at call sites.
struct Rect {
    int x;
    int y;
    int width;
    int height;
};

// Forward declaration; AbstractView is defined in nativeWrapper.cpp and
// remains the polymorphic root of all webview implementations.
class AbstractView;

// Spec passed to IDisplayBackend::createWindow. Backend-neutral; each
// backend interprets the fields it cares about.
struct WindowSpec {
    Rect frame = {};
    bool borderless = false;
    bool transparent = false;
    bool resizable = true;
    bool fullscreenable = true;
    std::string title;
};

// Spec passed to IWebviewBackend::createWebview. Callbacks are kept as
// opaque void* function pointers to avoid pulling their full signatures
// into this header — the impl casts back to the concrete typedef defined
// in nativeWrapper.cpp.
struct WebviewSpec {
    uint32_t webviewId = 0;
    void* hostWindow = nullptr;    // opaque handle returned by createWindow
    Rect frame = {};
    std::string url;
    std::string renderer;          // "webkit", "cef", or future "wpe"
    bool sandboxed = false;
    std::string partition;
    void* navigationHandler = nullptr;
    void* webviewEventHandler = nullptr;   // WebviewEventHandler (3 args): fires for navigation/load events from native code
    void* eventBridgeHandler = nullptr;    // HandlePostMessage  (2 args): JSON-bridge for events from JS preload scripts
    void* bunBridgeHandler = nullptr;
    void* internalBridgeHandler = nullptr;
    // Compiled JS injected at document-start of every navigation. Sets up
    // window.__electrobun{WebviewId,WindowId,RpcSocketPort,SecretKeyBytes,*Bridge}
    // and runs Electrobun's full preload pipeline (RPC, drag regions, webview
    // tag support, lifecycle events). Empty string = no injection.
    std::string electrobunPreloadScript;
    // App-supplied preload script (BrowserWindow/BrowserView `preload` option).
    // Injected at document-start alongside electrobunPreloadScript and
    // re-applied by updateCustomPreloadScript. Empty string = no injection.
    std::string customPreloadScript;
};

// Display backend: owns the platform window, the output surface, and the
// main event loop. Bare-minimum surface — extend only when Phase 2's
// WPE backend actually needs more.
class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;

    // Create a top-level window. Returns an opaque handle that other
    // FFI functions accept as `void* window`.
    virtual void* createWindow(const WindowSpec& spec) = 0;

    // Run the platform main loop on the calling thread. Blocks until
    // stopEventLoop() is invoked from any thread.
    virtual void runEventLoop() = 0;

    // Request the main loop to exit. Safe to call from any thread.
    virtual void stopEventLoop() = 0;
};

// Webview backend: produces HTML pixels into a window created by an
// IDisplayBackend.
class IWebviewBackend {
public:
    virtual ~IWebviewBackend() = default;

    virtual std::shared_ptr<AbstractView> createWebview(const WebviewSpec& spec) = 0;
};

// Global accessors. Phase 1 implementations live in nativeWrapper.cpp and
// always return the GTK+WebKit (or GTK+CEF) pair. Phase 2 introduces a
// build-time selection that returns the WPE+DRM pair on the embedded target.
IDisplayBackend& currentDisplayBackend();
IWebviewBackend& currentWebviewBackend();

} // namespace electrobun
