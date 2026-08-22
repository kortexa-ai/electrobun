// nativeWrapper_wpe.cpp — parallel FFI surface for the bare-DRM Linux target.
//
// Linked into libNativeWrapper_wpe.so only. Exposes the same ELECTROBUN_EXPORT
// symbols as nativeWrapper.cpp (the GTK/CEF build), so the Bun side's
// dlopen/dlsym FFI binding works identically regardless of which library the
// app was built against.
//
// Partition (per linux-wpe.md §12):
//   - Real routings dispatch to currentDisplayBackend() / currentWebviewBackend()
//     (implemented in wpe/wpe_backend.cpp) or to AbstractView virtual methods.
//   - Degenerate routings report a single fullscreen display and do no-ops for
//     window chrome (kiosk never minimizes, never moves, never has a titlebar).
//   - Noop stubs exist only so dlsym resolves. First call logs a one-shot
//     warning to stderr; subsequent calls are silent.

#include "abstract_view.h"
#include "backend.h"
#include "../shared/callbacks.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>

using namespace electrobun;

// ---------------------------------------------------------------------------
// Callback typedefs (matching nativeWrapper.cpp's local aliases)
// ---------------------------------------------------------------------------

typedef void (*WindowCloseCallback)(uint32_t windowId);
typedef void (*WindowMoveCallback)(uint32_t windowId, double x, double y);
typedef void (*WindowResizeCallback)(uint32_t windowId, double x, double y, double width, double height);
typedef void (*WindowFocusCallback)(uint32_t windowId);
typedef void (*WindowBlurCallback)(uint32_t windowId);
typedef void (*GlobalShortcutCallback)(const char* accelerator);

#define ELECTROBUN_EXPORT __attribute__((visibility("default")))

// ---------------------------------------------------------------------------
// One-shot warn helper — so we get a single line per unsupported FFI on first
// call, then stay silent. Keeps kiosk logs clean while still discoverable.
// ---------------------------------------------------------------------------

static std::mutex g_warnMutex;
static std::unordered_set<std::string> g_warned;

static void warnOnce(const char* name) {
    std::lock_guard<std::mutex> lock(g_warnMutex);
    if (g_warned.insert(name).second) {
        fprintf(stderr, "[wpe] unimplemented FFI on embedded target: %s (further calls silent)\n", name);
    }
}

// ---------------------------------------------------------------------------
// Shared process state (for wiring setQuitRequestedHandler → signal handler
// later in Phase 2.5). Held here, consulted by the wpe backend's signal path.
// ---------------------------------------------------------------------------

static std::atomic<QuitRequestedHandler> g_quitRequestedHandler{nullptr};
QuitRequestedHandler wpeGetQuitRequestedHandler() { return g_quitRequestedHandler.load(); }

// Defined in wpe/wpe_backend.cpp. Returns the (single) AbstractView matching
// the given webviewId, or nullptr if it hasn't been created yet. Used by the
// FFI exports below that take only a webviewId (no AbstractView*) to dispatch
// to the impl.
namespace electrobun { AbstractView* wpeFindViewById(uint32_t webviewId); }

// ---------------------------------------------------------------------------
// Multi-window emulation on bare-DRM. The kiosk has exactly one DRM scanout,
// so the WpeBackend itself only ever has one "display." But the JS layer
// expects multiple BrowserWindows (About dialog, OAuth popup, etc.) to be
// independently closable. We bridge that by allocating a per-call
// WpeWindowEntry on createGTKWindow — its address is the unique "window
// handle" returned to JS, and closeWindow looks the entry up to fire the
// per-window close callback. The bun side's existing close-event chain
// (BrowserWindow.ts: delete from BrowserWindowMap, remove views,
// exitOnLastWindowClosed) then runs as it would on macOS/GTK.
// ---------------------------------------------------------------------------

struct WpeWindowEntry {
    uint32_t            windowId;
    WindowCloseCallback closeCallback;
    WindowShouldCloseHandler shouldCloseCallback;
    bool                usesCompositedChrome;
};

static std::mutex                                              g_windowsMutex;
static std::unordered_map<void*, std::unique_ptr<WpeWindowEntry>> g_windows;

static bool windowUsesCompositedChrome(void* window) {
    if (!window) return false;
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    auto it = g_windows.find(window);
    return it != g_windows.end() && it->second->usesCompositedChrome;
}

static void closeWpeWindow(void* window) {
    // Look up the per-window entry by handle. Fire its close callback so the
    // core runs BrowserWindow cleanup, removes that window's views, and
    // applies exitOnLastWindowClosed.
    std::unique_ptr<WpeWindowEntry> entry;
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        auto it = g_windows.find(window);
        if (it == g_windows.end()) return;
        entry = std::move(it->second);
        g_windows.erase(it);
    }
    if (entry && entry->closeCallback) {
        entry->closeCallback(entry->windowId);
    }
}

static void handleWindowChromeAction(void* window, WindowChromeAction action) {
    switch (action) {
        case WindowChromeAction::Close:
            closeWpeWindow(window);
            break;
        case WindowChromeAction::Maximize:
            currentDisplayBackend().setWindowMaximized(window, true);
            break;
        case WindowChromeAction::Restore:
            currentDisplayBackend().setWindowMaximized(window, false);
            break;
        case WindowChromeAction::Reveal:
            currentDisplayBackend().revealWindowChrome(window);
            break;
    }
}

// ---------------------------------------------------------------------------
// extern "C" — exported FFI surface
// ---------------------------------------------------------------------------

extern "C" {

// ===========================================================================
// Lifecycle
// ===========================================================================

ELECTROBUN_EXPORT int simpleTest() {
    printf("[wpe] simpleTest called\n");
    fflush(stdout);
    return 42;
}

ELECTROBUN_EXPORT void startEventLoop(const char* identifier, const char* name, const char* channel) {
    (void)identifier; (void)name; (void)channel;
    currentDisplayBackend().runEventLoop();
}

ELECTROBUN_EXPORT void stopEventLoop() {
    currentDisplayBackend().stopEventLoop();
}

ELECTROBUN_EXPORT void killApp() {
    // Deprecated alias — kept for FFI symbol compatibility.
    currentDisplayBackend().stopEventLoop();
}

ELECTROBUN_EXPORT void shutdownApplication() {
    // Deprecated alias — kept for FFI symbol compatibility.
    currentDisplayBackend().stopEventLoop();
}

ELECTROBUN_EXPORT void waitForShutdownComplete(int timeoutMs) {
    // WpeBackend::teardown is synchronous in Phase 2. Nothing to poll.
    (void)timeoutMs;
}

ELECTROBUN_EXPORT void forceExit(int code) {
    _exit(code);
}

ELECTROBUN_EXPORT void setQuitRequestedHandler(QuitRequestedHandler handler) {
    g_quitRequestedHandler.store(handler);
}

ELECTROBUN_EXPORT void shutdownNativeWrapper() {
    // Final cleanup hook. WpeBackend destructor handles drmDropMaster + fd close
    // via teardown() when the static instance is destroyed at process exit.
    // TODO(phase2.5): invoke teardown explicitly here to guarantee ordering
    // before Bun tears down its own runtime.
}

// ===========================================================================
// Window lifecycle (degenerate — kiosk has exactly one fullscreen window)
// ===========================================================================

ELECTROBUN_EXPORT void* createGTKWindow(uint32_t windowId, double x, double y, double width, double height, const char* title,
                                       WindowCloseCallback closeCallback, WindowMoveCallback moveCallback, WindowResizeCallback resizeCallback,
                                       WindowFocusCallback focusCallback, WindowBlurCallback blurCallback, WindowKeyHandler keyCallback,
                                       WindowShouldCloseHandler shouldCloseCallback,
                                       const char* titleBarStyle, bool transparent) {
    (void)x; (void)y;
    (void)moveCallback; (void)resizeCallback;
    (void)focusCallback; (void)blurCallback; (void)keyCallback;
    (void)transparent;
    // Initialize the DRM scanout on the first call. WpeBackend::createWindow
    // is idempotent — subsequent calls return the same DrmDisplay pointer —
    // so it's safe to invoke per BrowserWindow even though there's only one
    // physical display.
    WindowSpec spec{};
    spec.frame       = Rect{0, 0, (int)width, (int)height};
    spec.title       = title ? title : "";
    spec.borderless  = true;
    spec.transparent = false;
    spec.resizable   = false;
    spec.fullscreenable = true;
    (void)currentDisplayBackend().createWindow(spec);
    // Allocate a unique per-window handle. The pointer's identity is what
    // distinguishes this BrowserWindow from any other in subsequent FFI
    // calls (createWebview's hostWindow, closeWindow, etc.).
    auto entry = std::make_unique<WpeWindowEntry>();
    entry->windowId      = windowId;
    entry->closeCallback = closeCallback;
    entry->shouldCloseCallback = shouldCloseCallback;
    entry->usesCompositedChrome =
        !titleBarStyle || std::strcmp(titleBarStyle, "default") == 0;
    void* handle = entry.get();
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        g_windows.emplace(handle, std::move(entry));
    }
    return handle;
}

ELECTROBUN_EXPORT void* createWindowWithFrameAndStyleFromWorker(uint32_t windowId, double x, double y, double width, double height,
                                                                uint32_t styleMask, const char* titleBarStyle, bool transparent,
                                                                double trafficLightOffsetX, double trafficLightOffsetY,
                                                                WindowCloseCallback closeCallback, WindowMoveCallback moveCallback, WindowResizeCallback resizeCallback,
                                                                WindowFocusCallback focusCallback, WindowBlurCallback blurCallback, WindowKeyHandler keyCallback,
                                                                WindowShouldCloseHandler shouldCloseCallback) {
    (void)styleMask; (void)trafficLightOffsetX; (void)trafficLightOffsetY;
    return createGTKWindow(windowId, x, y, width, height, "Window",
                           closeCallback, moveCallback, resizeCallback,
                           focusCallback, blurCallback, keyCallback,
                           shouldCloseCallback, titleBarStyle, transparent);
}

ELECTROBUN_EXPORT uint32_t getWindowStyle(bool borderless, bool titled, bool closable, bool miniaturizable,
                                          bool resizable, bool fullSizeContentView, bool hiddenTitlebar, bool hiddenInsetTitlebar) {
    (void)borderless; (void)titled; (void)closable; (void)miniaturizable;
    (void)resizable; (void)fullSizeContentView; (void)hiddenTitlebar; (void)hiddenInsetTitlebar;
    return 0; // embedded kiosk ignores style masks
}

ELECTROBUN_EXPORT void setWindowTitle(void* window, const char* title) { (void)window; (void)title; }
ELECTROBUN_EXPORT void showWindow(void* window, bool activate)         { (void)window; (void)activate; }
ELECTROBUN_EXPORT void activateWindow(void* window)                    { (void)window; }
ELECTROBUN_EXPORT void hideWindow(void* window)                        { (void)window; }
ELECTROBUN_EXPORT bool isWindowVisible(void* window)                   { (void)window; return true; }
ELECTROBUN_EXPORT void closeWindow(void* window) {
    closeWpeWindow(window);
}

ELECTROBUN_EXPORT void requestWindowClose(void* window) {
    WindowShouldCloseHandler shouldClose = nullptr;
    uint32_t windowId = 0;
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        auto it = g_windows.find(window);
        if (it == g_windows.end()) return;
        windowId = it->second->windowId;
        shouldClose = it->second->shouldCloseCallback;
    }
    if (shouldClose) shouldClose(windowId);
    else closeWpeWindow(window);
}

ELECTROBUN_EXPORT void minimizeWindow(void* window)                  { (void)window; }
ELECTROBUN_EXPORT void restoreWindow(void* window)                   { (void)window; }
ELECTROBUN_EXPORT bool isWindowMinimized(void* window)               { (void)window; return false; }
ELECTROBUN_EXPORT void maximizeWindow(void* window) {
    currentDisplayBackend().setWindowMaximized(window, true);
}
ELECTROBUN_EXPORT void unmaximizeWindow(void* window) {
    currentDisplayBackend().setWindowMaximized(window, false);
}
ELECTROBUN_EXPORT bool isWindowMaximized(void* window) {
    return currentDisplayBackend().isWindowMaximized(window);
}
ELECTROBUN_EXPORT void setWindowFullScreen(void* window, bool full)  { (void)window; (void)full; }
ELECTROBUN_EXPORT bool isWindowFullScreen(void* window)              { (void)window; return true; }
ELECTROBUN_EXPORT void setWindowAlwaysOnTop(void* window, bool top)  { (void)window; (void)top; }
ELECTROBUN_EXPORT bool isWindowAlwaysOnTop(void* window)             { (void)window; return true; }
ELECTROBUN_EXPORT void setWindowVisibleOnAllWorkspaces(void* w, bool v) { (void)w; (void)v; }
ELECTROBUN_EXPORT bool isWindowVisibleOnAllWorkspaces(void* w)       { (void)w; return true; }
ELECTROBUN_EXPORT void setWindowPosition(void* w, double x, double y){ (void)w; (void)x; (void)y; }
ELECTROBUN_EXPORT void centerWindow(void* w)                          { (void)w; }
ELECTROBUN_EXPORT void setWindowButtonPosition(void* w, double x, double y) { (void)w; (void)x; (void)y; }
ELECTROBUN_EXPORT void getWindowButtonPosition(void* w, double* x, double* y) {
    (void)w;
    if (x) *x = 0;
    if (y) *y = 0;
}
ELECTROBUN_EXPORT void setWindowSize(void* w, double width, double height)  { (void)w; (void)width; (void)height; }
ELECTROBUN_EXPORT void setWindowFrame(void* w, double x, double y, double width, double height) {
    (void)w; (void)x; (void)y; (void)width; (void)height;
}

ELECTROBUN_EXPORT void setWindowIcon(void* w, const char* iconPath) { (void)w; (void)iconPath; }
ELECTROBUN_EXPORT void startWindowMove(void* window)                { (void)window; }
ELECTROBUN_EXPORT void stopWindowMove()                             {}

ELECTROBUN_EXPORT void getWindowFrame(void* window, double* outX, double* outY, double* outWidth, double* outHeight) {
    (void)window;
    // Report the full display as the window frame.
    // TODO(phase2.5): pull actual dimensions from currentDisplayBackend().
    if (outX) *outX = 0;
    if (outY) *outY = 0;
    if (outWidth)  *outWidth  = 0;
    if (outHeight) *outHeight = 0;
}

ELECTROBUN_EXPORT void getWindowPosition(void* window, double* outX, double* outY) {
    (void)window;
    if (outX) *outX = 0;
    if (outY) *outY = 0;
}

ELECTROBUN_EXPORT void getWindowSize(void* window, double* outWidth, double* outHeight) {
    (void)window;
    // TODO(phase2.5): expose display dims via an IDisplayBackend accessor.
    if (outWidth)  *outWidth  = 0;
    if (outHeight) *outHeight = 0;
}

// ===========================================================================
// Display enumeration (degenerate — always one fullscreen display)
// ===========================================================================

ELECTROBUN_EXPORT const char* getAllDisplays() {
    // TODO(phase2.5): populate bounds from DrmDisplay::logicalWidth()/Height().
    return strdup("[{\"id\":0,\"bounds\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0},"
                  "\"workArea\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0},"
                  "\"scaleFactor\":1,\"isPrimary\":true}]");
}

ELECTROBUN_EXPORT const char* getPrimaryDisplay() {
    return strdup("{\"id\":0,\"bounds\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0},"
                  "\"workArea\":{\"x\":0,\"y\":0,\"width\":0,\"height\":0},"
                  "\"scaleFactor\":1,\"isPrimary\":true}");
}

ELECTROBUN_EXPORT const char* getCursorScreenPoint() {
    return strdup("{\"x\":0,\"y\":0}");
}

ELECTROBUN_EXPORT uint64_t getMouseButtons() { return 0; }

ELECTROBUN_EXPORT bool captureScreenRegion(double x, double y, uint32_t width, uint32_t height,
                                            uint8_t* outRgba, uint64_t outLen) {
    (void)x; (void)y; (void)width; (void)height; (void)outRgba; (void)outLen;
    warnOnce("captureScreenRegion");
    return false;
}

// ===========================================================================
// Webview lifecycle (real routings)
// ===========================================================================

// Storage for setNextWebviewFlags; consumed on next initWebview.
// TODO(phase2.next): thread through WebviewSpec if wpe actually uses these.
static std::atomic<bool> g_nextStartTransparent{false};
static std::atomic<bool> g_nextStartPassthrough{false};

ELECTROBUN_EXPORT void setNextWebviewFlags(bool startTransparent, bool startPassthrough) {
    g_nextStartTransparent.store(startTransparent);
    g_nextStartPassthrough.store(startPassthrough);
}

// Trust class for the next initWebview call. 0 = trusted (default — view
// shares the WPEWebProcess of the seed via related-view), 1 = untrusted
// (view gets its own WPEWebProcess). Mirrors the setNextWebviewFlags
// pattern to avoid bloating initWebview's already-long FFI signature.
static std::atomic<int> g_nextTrust{0};

ELECTROBUN_EXPORT void setNextWebviewTrust(const char* trust) {
    g_nextTrust.store((trust && std::strcmp(trust, "untrusted") == 0) ? 1 : 0);
}

// Parent BrowserWindow's frame x/y for the next initWebview call. Used by
// the WPE backend to place non-main primary views (About dialogs, etc.)
// at the requested panel position — the inner BrowserView's own frame is
// hardcoded to (0, 0) by BrowserWindow.init for cross-target portability,
// so this is how we communicate "where on the panel does the BrowserWindow
// belong" without changing the cross-target API.
static std::atomic<int32_t> g_nextWindowFrameX{0};
static std::atomic<int32_t> g_nextWindowFrameY{0};

ELECTROBUN_EXPORT void setNextWebviewWindowFrame(int32_t x, int32_t y) {
    g_nextWindowFrameX.store(x);
    g_nextWindowFrameY.store(y);
}

ELECTROBUN_EXPORT AbstractView* initWebview(uint32_t webviewId,
                                            void* window,
                                            const char* renderer,
                                            const char* url,
                                            double x, double y,
                                            double width, double height,
                                            bool autoResize,
                                            const char* partitionIdentifier,
                                            DecideNavigationCallback navigationCallback,
                                            WebviewEventHandler webviewEventHandler,
                                            HandlePostMessage eventBridgeHandler,
                                            HandlePostMessage bunBridgeHandler,
                                            HandlePostMessage internalBridgeHandler,
                                            const char* electrobunPreloadScript,
                                            const char* customPreloadScript,
                                            const char* viewsRoot,
                                            bool transparent,
                                            bool sandbox) {
    (void)autoResize;
    (void)viewsRoot; (void)transparent;
    g_nextStartTransparent.store(false);
    g_nextStartPassthrough.store(false);

    WebviewSpec spec{};
    spec.webviewId            = webviewId;
    spec.hostWindow           = window;
    spec.frame                = Rect{(int)x, (int)y, (int)width, (int)height};
    spec.url                  = url ? url : "";
    spec.renderer             = renderer ? renderer : "";
    spec.sandboxed            = sandbox;
    spec.partition            = partitionIdentifier ? partitionIdentifier : "";
    spec.navigationHandler    = (void*)navigationCallback;
    spec.webviewEventHandler  = (void*)webviewEventHandler;
    spec.eventBridgeHandler   = (void*)eventBridgeHandler;
    spec.bunBridgeHandler     = (void*)bunBridgeHandler;
    spec.internalBridgeHandler= (void*)internalBridgeHandler;
    spec.windowChromeActionHandler = (void*)handleWindowChromeAction;
    spec.electrobunPreloadScript = electrobunPreloadScript ? electrobunPreloadScript : "";
    spec.customPreloadScript     = customPreloadScript     ? customPreloadScript     : "";
    spec.trust                   = (g_nextTrust.exchange(0) == 1) ? "untrusted" : "trusted";
    spec.usesCompositedChrome    = windowUsesCompositedChrome(window);
    spec.windowFrameX            = g_nextWindowFrameX.exchange(0);
    spec.windowFrameY            = g_nextWindowFrameY.exchange(0);

    auto view = currentWebviewBackend().createWebview(spec);
    return view ? view.get() : nullptr;
}

ELECTROBUN_EXPORT AbstractView* initWGPUView(uint32_t webviewId,
                                             void* window,
                                             double x, double y, double width, double height,
                                             bool autoResize, bool startTransparent, bool startPassthrough) {
    (void)webviewId; (void)window; (void)x; (void)y; (void)width; (void)height;
    (void)autoResize; (void)startTransparent; (void)startPassthrough;
    warnOnce("initWGPUView");
    return nullptr; // Phase 4 territory.
}

ELECTROBUN_EXPORT void loadURLInWebView(AbstractView* v, const char* url) {
    if (v && url) v->loadURL(url);
}

ELECTROBUN_EXPORT void loadHTMLInWebView(AbstractView* v, const char* html) {
    if (v && html) v->loadHTML(html);
}

ELECTROBUN_EXPORT void webviewGoBack(AbstractView* v)    { if (v) v->goBack(); }
ELECTROBUN_EXPORT void webviewGoForward(AbstractView* v) { if (v) v->goForward(); }
ELECTROBUN_EXPORT void webviewReload(AbstractView* v)    { if (v) v->reload(); }
ELECTROBUN_EXPORT void webviewRemove(AbstractView* v)    { if (v) v->remove(); }
ELECTROBUN_EXPORT bool webviewCanGoBack(AbstractView* v)    { return v && v->canGoBack(); }
ELECTROBUN_EXPORT bool webviewCanGoForward(AbstractView* v) { return v && v->canGoForward(); }

ELECTROBUN_EXPORT void resizeWebview(AbstractView* v, double x, double y, double width, double height, const char* masksJson) {
    if (!v) return;
    Rect frame{(int)x, (int)y, (int)width, (int)height};
    v->resize(frame, masksJson);
}

ELECTROBUN_EXPORT void evaluateJavaScriptWithNoCompletion(AbstractView* v, const char* js) {
    if (v && js) v->evaluateJavaScriptWithNoCompletion(js);
}

ELECTROBUN_EXPORT void setWebviewNavigationRules(AbstractView* v, const char* rulesJson) {
    if (v) v->setNavigationRulesFromJSON(rulesJson);
}

ELECTROBUN_EXPORT void webviewFindInPage(AbstractView* v, const char* searchText, bool forward, bool matchCase) {
    if (v) v->findInPage(searchText, forward, matchCase);
}
ELECTROBUN_EXPORT void webviewStopFind(AbstractView* v) { if (v) v->stopFindInPage(); }

ELECTROBUN_EXPORT void webviewOpenDevTools(AbstractView* v)   { if (v) v->openDevTools(); }
ELECTROBUN_EXPORT void webviewCloseDevTools(AbstractView* v)  { if (v) v->closeDevTools(); }
ELECTROBUN_EXPORT void webviewToggleDevTools(AbstractView* v) { if (v) v->toggleDevTools(); }

ELECTROBUN_EXPORT void   webviewSetPageZoom(AbstractView* v, double zoom) { (void)v; (void)zoom; warnOnce("webviewSetPageZoom"); }
ELECTROBUN_EXPORT double webviewGetPageZoom(AbstractView* v)              { (void)v; return 1.0; }

ELECTROBUN_EXPORT void updatePreloadScriptToWebView(AbstractView* v, const char* scriptIdentifier, const char* scriptContent, bool forMainFrameOnly) {
    (void)scriptIdentifier; (void)forMainFrameOnly;
    if (v && scriptContent) v->updateCustomPreloadScript(scriptContent);
}

ELECTROBUN_EXPORT void addPreloadScriptToWebView(AbstractView* v, const char* scriptContent, bool forMainFrameOnly) {
    (void)forMainFrameOnly;
    if (v && scriptContent) v->addPreloadScriptToWebView(scriptContent);
}

ELECTROBUN_EXPORT void callAsyncJavaScript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) {
    AbstractView* v = wpeFindViewById(webviewId);
    if (!v || !jsString || !completionHandler) return;
    v->callAsyncJavascript(messageId ? messageId : "", jsString,
                           webviewId, hostWebviewId, completionHandler);
}

// ===========================================================================
// Webview HTML content (used by views:// scheme machinery)
// ===========================================================================

ELECTROBUN_EXPORT const char* getWebviewHTMLContent(uint32_t webviewId) {
    (void)webviewId;
    warnOnce("getWebviewHTMLContent");
    return nullptr;
}

ELECTROBUN_EXPORT void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent) {
    (void)webviewId; (void)htmlContent;
    warnOnce("setWebviewHTMLContent");
}

// ===========================================================================
// Navigation action / script message helpers (legacy — unused on WPE)
// ===========================================================================

ELECTROBUN_EXPORT const char* getUrlFromNavigationAction(void* navigationAction) { (void)navigationAction; return nullptr; }
ELECTROBUN_EXPORT const char* getBodyFromScriptMessage(void* message)            { (void)message; return nullptr; }

// ===========================================================================
// OS integration — all noops on a bare-DRM kiosk
// ===========================================================================

ELECTROBUN_EXPORT bool moveToTrash(char* pathString)            { (void)pathString; warnOnce("moveToTrash");   return false; }
ELECTROBUN_EXPORT bool openExternal(const char* urlString)      { (void)urlString;  warnOnce("openExternal"); return false; }
ELECTROBUN_EXPORT bool openPath(const char* pathString)         { (void)pathString; warnOnce("openPath");     return false; }

ELECTROBUN_EXPORT const char* openFileDialog(const char* startingFolder, const char* allowedFileTypes, int canChooseFiles, int canChooseDirectories, int allowsMultipleSelection) {
    (void)startingFolder; (void)allowedFileTypes; (void)canChooseFiles; (void)canChooseDirectories; (void)allowsMultipleSelection;
    warnOnce("openFileDialog");
    return nullptr;
}

ELECTROBUN_EXPORT int showMessageBox(const char* type, const char* title, const char* message, const char* buttonsJson, int defaultButton, int cancelButton) {
    (void)type; (void)title; (void)message; (void)buttonsJson; (void)defaultButton; (void)cancelButton;
    warnOnce("showMessageBox");
    return 0;
}

// ===========================================================================
// Clipboard — noops on kiosk
// ===========================================================================

ELECTROBUN_EXPORT const char* clipboardReadText()                           { warnOnce("clipboardReadText"); return strdup(""); }
ELECTROBUN_EXPORT void        clipboardWriteText(const char* text)          { (void)text; }
ELECTROBUN_EXPORT void        clipboardWriteImage(const uint8_t* pngData, size_t size) { (void)pngData; (void)size; }
ELECTROBUN_EXPORT void        clipboardClear()                              {}
ELECTROBUN_EXPORT const char* clipboardAvailableFormats()                   { return strdup("[]"); }

// ===========================================================================
// Tray — noops on kiosk
// ===========================================================================

ELECTROBUN_EXPORT void* createTray(uint32_t trayId, const char* title, const char* pathToImage, bool isTemplate, uint32_t width, uint32_t height, void* clickHandler) {
    (void)trayId; (void)title; (void)pathToImage; (void)isTemplate; (void)width; (void)height; (void)clickHandler;
    warnOnce("createTray");
    return nullptr;
}
ELECTROBUN_EXPORT void        setTrayTitle(void* statusItem, const char* title)          { (void)statusItem; (void)title; }
ELECTROBUN_EXPORT void        setTrayImage(void* statusItem, const char* image)          { (void)statusItem; (void)image; }
ELECTROBUN_EXPORT void        setTrayMenuFromJSON(void* statusItem, const char* json)    { (void)statusItem; (void)json; }
ELECTROBUN_EXPORT void        setTrayMenu(void* statusItem, const char* menuConfig)      { (void)statusItem; (void)menuConfig; }
ELECTROBUN_EXPORT void        removeTray(void* statusItem)                               { (void)statusItem; }
ELECTROBUN_EXPORT const char* getTrayBounds(void* statusItem)                            { (void)statusItem; return strdup("{\"x\":0,\"y\":0,\"width\":0,\"height\":0}"); }

// ===========================================================================
// Application menu / context menu — noops on kiosk
// ===========================================================================

ELECTROBUN_EXPORT void setApplicationMenu(const char* jsonString, void* applicationMenuHandler) {
    (void)jsonString; (void)applicationMenuHandler;
}
ELECTROBUN_EXPORT void showContextMenu(const char* jsonString, void* contextMenuHandler) {
    (void)jsonString; (void)contextMenuHandler;
    warnOnce("showContextMenu");
}

// ===========================================================================
// Snapshot — noop
// ===========================================================================

ELECTROBUN_EXPORT void getWebviewSnapshot(uint32_t hostId, uint32_t webviewId, double x, double y, double width, double height, void* completionHandler) {
    (void)hostId; (void)webviewId; (void)x; (void)y; (void)width; (void)height; (void)completionHandler;
    warnOnce("getWebviewSnapshot");
}

// ===========================================================================
// Global shortcuts — noops on kiosk
// ===========================================================================

ELECTROBUN_EXPORT void setGlobalShortcutCallback(GlobalShortcutCallback callback) { (void)callback; }
ELECTROBUN_EXPORT bool registerGlobalShortcut(const char* accelerator)            { (void)accelerator; return false; }
ELECTROBUN_EXPORT bool unregisterGlobalShortcut(const char* accelerator)          { (void)accelerator; return false; }
ELECTROBUN_EXPORT void unregisterAllGlobalShortcuts()                             {}
ELECTROBUN_EXPORT bool isGlobalShortcutRegistered(const char* accelerator)        { (void)accelerator; return false; }

// ===========================================================================
// Sessions / cookies — noops (kiosk typically has no persistence beyond defaults)
// ===========================================================================

ELECTROBUN_EXPORT const char* sessionGetCookies(const char* partitionIdentifier, const char* filterJson) {
    (void)partitionIdentifier; (void)filterJson; warnOnce("sessionGetCookies"); return strdup("[]");
}
ELECTROBUN_EXPORT bool sessionSetCookie(const char* partitionIdentifier, const char* cookieJson) {
    (void)partitionIdentifier; (void)cookieJson; warnOnce("sessionSetCookie"); return false;
}
ELECTROBUN_EXPORT bool sessionRemoveCookie(const char* partitionIdentifier, const char* urlStr, const char* cookieName) {
    (void)partitionIdentifier; (void)urlStr; (void)cookieName; warnOnce("sessionRemoveCookie"); return false;
}
ELECTROBUN_EXPORT void sessionClearCookies(const char* partitionIdentifier) {
    (void)partitionIdentifier;
}
ELECTROBUN_EXPORT void sessionClearStorageData(const char* partitionIdentifier, const char* storageTypesJson) {
    (void)partitionIdentifier; (void)storageTypesJson;
}

// ===========================================================================
// Handlers — store or ignore
// ===========================================================================

ELECTROBUN_EXPORT void setURLOpenHandler(void (*callback)(const char*))  { (void)callback; }

// ===========================================================================
// Symbols Bun's FFI binding expects beyond what nativeWrapper.cpp defines.
// Stubs here so dlopen resolves cleanly — noops on kiosk.
// ===========================================================================

ELECTROBUN_EXPORT const uint8_t* clipboardReadImage(size_t* sizeOut) {
    if (sizeOut) *sizeOut = 0;
    warnOnce("clipboardReadImage");
    return nullptr;
}

ELECTROBUN_EXPORT void setJSUtils(void* getMimeTypeFromUrl, void* getHtmlForWebview) {
    (void)getMimeTypeFromUrl; (void)getHtmlForWebview;
    // TODO(phase2.next): wire these for views:// scheme if Electrobun's
    // Bun side relies on host-provided helpers. For Phase 2 the WPE backend
    // handles mime + html inline (see wpe_backend.cpp handleViewsURIScheme).
}

ELECTROBUN_EXPORT void showItemInFolder(const char* pathString) {
    (void)pathString;
    warnOnce("showItemInFolder");
}

ELECTROBUN_EXPORT void showNotification(const char* title, const char* body, const char* subtitle, bool silent) {
    (void)title; (void)body; (void)subtitle; (void)silent;
    warnOnce("showNotification");
}

ELECTROBUN_EXPORT void testFFI2(void (*callback)()) {
    (void)callback;
}

ELECTROBUN_EXPORT void webviewSetTransparent(AbstractView* v, bool transparent) {
    if (v) v->setTransparent(transparent);
}

ELECTROBUN_EXPORT void webviewSetPassthrough(AbstractView* v, bool enable) {
    if (v) v->setPassthrough(enable);
}

ELECTROBUN_EXPORT void webviewSetHidden(AbstractView* v, bool hidden) {
    if (v) v->setHidden(hidden);
}
ELECTROBUN_EXPORT bool webviewSetSpellCheck(AbstractView* v, bool enabled) {
    (void)v; (void)enabled;
    return false;
}
ELECTROBUN_EXPORT void setAppReopenHandler(void (*callback)())           { (void)callback; }
ELECTROBUN_EXPORT void setDockIconVisible(bool visible)                  { (void)visible; }
ELECTROBUN_EXPORT bool isDockIconVisible()                               { return false; }

// ===========================================================================
// WGPU — all Phase 4 stubs
// ===========================================================================

ELECTROBUN_EXPORT void  wgpuViewSetFrame(AbstractView* v, double x, double y, double w, double h) { (void)v; (void)x; (void)y; (void)w; (void)h; }
ELECTROBUN_EXPORT void  wgpuViewSetTransparent(AbstractView* v, bool transparent)                 { (void)v; (void)transparent; }
ELECTROBUN_EXPORT void  wgpuViewSetPassthrough(AbstractView* v, bool enablePassthrough)           { (void)v; (void)enablePassthrough; }
ELECTROBUN_EXPORT void  wgpuViewSetHidden(AbstractView* v, bool hidden)                           { (void)v; (void)hidden; }
ELECTROBUN_EXPORT void  wgpuViewRemove(AbstractView* v)                                           { (void)v; }
ELECTROBUN_EXPORT void* wgpuViewGetNativeHandle(AbstractView* v)                                  { (void)v; return nullptr; }

ELECTROBUN_EXPORT void* wgpuInstanceCreateSurfaceMainThread(void* instance, void* descriptor) { (void)instance; (void)descriptor; return nullptr; }
ELECTROBUN_EXPORT void* wgpuCreateSurfaceForView(void* wgpuInstance, AbstractView* v)         { (void)wgpuInstance; (void)v; return nullptr; }
ELECTROBUN_EXPORT void  wgpuSurfaceConfigureMainThread(void* surface, void* config)           { (void)surface; (void)config; }
ELECTROBUN_EXPORT void  wgpuSurfaceCapabilitiesFreeMembersShim(void* capabilities)            { (void)capabilities; }
ELECTROBUN_EXPORT void  wgpuSurfaceGetCurrentTextureMainThread(void* surface, void* surfaceTexture) { (void)surface; (void)surfaceTexture; }
ELECTROBUN_EXPORT int32_t wgpuSurfacePresentMainThread(void* surface)                         { (void)surface; return 0; }

ELECTROBUN_EXPORT uint64_t wgpuQueueOnSubmittedWorkDoneShim(void* queue, void* callbackInfo)  { (void)queue; (void)callbackInfo; return 0; }
ELECTROBUN_EXPORT uint64_t wgpuBufferMapAsyncShim(void* buffer, uint64_t mode, uint64_t offset, uint64_t size, void* callbackInfo) {
    (void)buffer; (void)mode; (void)offset; (void)size; (void)callbackInfo; return 0;
}
ELECTROBUN_EXPORT int32_t wgpuInstanceWaitAnyShim(void* instance, uint64_t futureId, uint64_t timeoutNS) {
    (void)instance; (void)futureId; (void)timeoutNS; return 0;
}

ELECTROBUN_EXPORT uint8_t* wgpuBufferReadSyncShim(void* instance, void* device, void* queue, void* buffer,
                                                  uint64_t bufferSize, uint64_t offset, uint64_t size,
                                                  uint64_t timeoutNS, uint64_t* outSize, int32_t* outStatus) {
    (void)instance; (void)device; (void)queue; (void)buffer;
    (void)bufferSize; (void)offset; (void)size; (void)timeoutNS;
    if (outSize)   *outSize = 0;
    if (outStatus) *outStatus = -1;
    return nullptr;
}

ELECTROBUN_EXPORT int32_t wgpuBufferReadSyncIntoShim(void* instance, void* device, void* queue, void* buffer,
                                                     uint64_t bufferSize, uint64_t offset, uint64_t size,
                                                     uint8_t* outData, uint64_t outDataSize, uint64_t timeoutNS) {
    (void)instance; (void)device; (void)queue; (void)buffer;
    (void)bufferSize; (void)offset; (void)size; (void)outData; (void)outDataSize; (void)timeoutNS;
    return -1;
}

ELECTROBUN_EXPORT void* wgpuBufferReadbackBeginShim(void* instance, void* device, void* queue, void* buffer,
                                                    uint64_t offset, uint64_t size) {
    (void)instance; (void)device; (void)queue; (void)buffer; (void)offset; (void)size; return nullptr;
}
ELECTROBUN_EXPORT int32_t wgpuBufferReadbackStatusShim(void* jobPtr) { (void)jobPtr; return -1; }
ELECTROBUN_EXPORT void    wgpuBufferReadbackFreeShim(void* jobPtr)   { (void)jobPtr; }

ELECTROBUN_EXPORT void wgpuRunGPUTest(void* abstractView) { (void)abstractView; }
ELECTROBUN_EXPORT void wgpuToggleGPUTestShader(void* abstractView) { (void)abstractView; }
ELECTROBUN_EXPORT void wgpuCreateAdapterDeviceMainThread(void* instancePtr, void* surfacePtr, void* outAdapterDevice) {
    (void)instancePtr; (void)surfacePtr; (void)outAdapterDevice;
}

} // extern "C"
