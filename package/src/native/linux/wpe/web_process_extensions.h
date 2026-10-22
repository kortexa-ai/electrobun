#pragma once
#include <glib.h>

namespace electrobun::wpe {

enum class WebProcessExtensions { Disabled, InvalidDirectory, Configured };

// The caller must invoke this on WebKit's owning thread, before creating any
// WebView. The callback keeps path validation testable without opening a view.
template<typename Configure>
WebProcessExtensions configureWebProcessExtensions(const char* directory, Configure configure) {
    if (!directory || !*directory) return WebProcessExtensions::Disabled;
    if (!g_path_is_absolute(directory) || !g_file_test(directory, G_FILE_TEST_IS_DIR))
        return WebProcessExtensions::InvalidDirectory;
    configure(directory);
    return WebProcessExtensions::Configured;
}

}
