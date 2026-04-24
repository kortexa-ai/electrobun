// abstract_view.h — polymorphic root of all Linux webview implementations.
//
// Historically AbstractView was defined inline in nativeWrapper.cpp and pulled
// in <gtk/gtk.h> through its `GtkWidget* widget` member. Phase 2's WPE backend
// lives in a separate translation unit and must not depend on GTK headers.
//
// This header forward-declares GtkWidget so the field remains a typed pointer
// (opaque; only GTK-using TUs call gtk_* on it). All other members are
// backend-neutral.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

#include "backend.h"
#include "../shared/glob_match.h"
#include "../shared/linux_dpi.h"

// Forward declaration. Only TUs that actually call gtk_* need <gtk/gtk.h>;
// everyone else gets an opaque pointer type.
typedef struct _GtkWidget GtkWidget;

namespace electrobun {

class AbstractView {
public:
    uint32_t webviewId;
    GtkWidget* widget = nullptr;   // GtkWidget* for WebKit backend; nullptr for CEF; nullptr for WPE.
                                   // Phase 2 may repurpose this as an opaque handle later if useful.
    bool isMousePassthroughEnabled = false;
    bool mirrorModeEnabled = false;
    bool fullSize = false;
    bool pendingStartTransparent = false;
    bool pendingStartPassthrough = false;
    bool isReceivingInput = true;
    bool isRemoved = false;
    std::string maskJSON;
    Rect visualBounds = {};
    bool creationFailed = false;

    // Pending resize state (cross-thread)
    std::mutex pendingResizeMutex;
    std::atomic<uint64_t> pendingResizeGeneration{0};
    uint64_t appliedResizeGeneration = 0;
    bool hasPendingResize = false;
    LogicalRect pendingResizeFrame = {};
    std::string pendingResizeMasks;

    // Navigation rules for URL filtering
    std::vector<std::string> navigationRules;

    // Root directory for views:// protocol resolution
    std::string viewsRoot;

    AbstractView(uint32_t webviewId_) : webviewId(webviewId_) {}
    virtual ~AbstractView() {}

    void setNavigationRulesFromJSON(const char* rulesJson) {
        navigationRules.clear();
        if (!rulesJson || std::strlen(rulesJson) == 0) {
            return;
        }

        std::string json(rulesJson);
        size_t pos = json.find('[');
        if (pos == std::string::npos) return;

        pos++;
        while (pos < json.length()) {
            size_t strStart = json.find('"', pos);
            if (strStart == std::string::npos) break;

            size_t strEnd = strStart + 1;
            while (strEnd < json.length()) {
                if (json[strEnd] == '"' && json[strEnd - 1] != '\\') break;
                strEnd++;
            }
            if (strEnd >= json.length()) break;

            std::string rule = json.substr(strStart + 1, strEnd - strStart - 1);
            navigationRules.push_back(rule);

            pos = strEnd + 1;
        }
    }

    bool shouldAllowNavigationToURL(const std::string& url) {
        if (navigationRules.empty()) {
            return true;
        }

        bool allowed = true;
        for (const std::string& rule : navigationRules) {
            bool isBlockRule = !rule.empty() && rule[0] == '^';
            std::string pattern = isBlockRule ? rule.substr(1) : rule;

            if (electrobun::globMatch(pattern, url)) {
                allowed = !isBlockRule;
            }
        }
        return allowed;
    }

    // Pure virtual interface
    virtual void loadURL(const char* urlString) = 0;
    virtual void loadHTML(const char* htmlString) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reload() = 0;
    virtual void remove() = 0;
    virtual bool canGoBack() = 0;
    virtual bool canGoForward() = 0;
    virtual void evaluateJavaScriptWithNoCompletion(const char* jsString) = 0;
    virtual void callAsyncJavascript(const char* messageId, const char* jsString,
                                     uint32_t webviewId, uint32_t hostWebviewId,
                                     void* completionHandler) = 0;
    virtual void addPreloadScriptToWebView(const char* jsString) = 0;
    virtual void updateCustomPreloadScript(const char* jsString) = 0;
    virtual void resize(const Rect& frame, const char* masksJson) = 0;
    virtual void resizeLogical(const LogicalRect& frame, const char* masksJson) {
        const Rect integerFrame = {
            static_cast<int>(frame.x),
            static_cast<int>(frame.y),
            static_cast<int>(frame.width),
            static_cast<int>(frame.height),
        };
        resize(integerFrame, masksJson);
    }
    virtual void applyVisualMask() = 0;
    virtual void removeMasks() = 0;
    virtual void toggleMirrorMode(bool enable) = 0;

    virtual void setTransparent(bool /*transparent*/) {}
    virtual void setPassthrough(bool enable) { isMousePassthroughEnabled = enable; }
    virtual void setHidden(bool /*hidden*/) {}

    virtual void findInPage(const char* searchText, bool forward, bool matchCase) = 0;
    virtual void stopFindInPage() = 0;

    virtual void openDevTools() = 0;
    virtual void closeDevTools() = 0;
    virtual void toggleDevTools() = 0;

    void storePendingResize(const LogicalRect& frame, const char* masksJson) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        pendingResizeFrame = frame;
        pendingResizeMasks = masksJson ? masksJson : "";
        hasPendingResize = true;
        pendingResizeGeneration++;
    }

    bool consumePendingResize(LogicalRect& outFrame, std::string& outMasks) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        if (!hasPendingResize) return false;
        uint64_t gen = pendingResizeGeneration.load();
        if (gen == appliedResizeGeneration) return false;
        outFrame = pendingResizeFrame;
        outMasks = pendingResizeMasks;
        appliedResizeGeneration = gen;
        hasPendingResize = false;
        return true;
    }
};

} // namespace electrobun
