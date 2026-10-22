#include "web_process_extensions.h"
#include <glib/gstdio.h>
#include <cassert>
#include <cstring>

int main() {
    using namespace electrobun::wpe;
    auto* directory = g_dir_make_tmp("electrobun-extension-test-XXXXXX", nullptr);
    assert(directory);
    auto* missing = g_build_filename(directory, "missing", nullptr);
    auto* file = g_build_filename(directory, "regular-file", nullptr);
    assert(g_file_set_contents(file, "fixture", -1, nullptr));
    int calls = 0;
    auto configure = [&](const char* actual) {
        ++calls;
        assert(!strcmp(actual, directory));
    };
    assert(configureWebProcessExtensions(nullptr, configure) == WebProcessExtensions::Disabled);
    assert(configureWebProcessExtensions("", configure) == WebProcessExtensions::Disabled);
    assert(configureWebProcessExtensions(".", configure) == WebProcessExtensions::InvalidDirectory);
    assert(configureWebProcessExtensions("relative/path", configure) == WebProcessExtensions::InvalidDirectory);
    assert(configureWebProcessExtensions(missing, configure) == WebProcessExtensions::InvalidDirectory);
    assert(configureWebProcessExtensions(file, configure) == WebProcessExtensions::InvalidDirectory);
    assert(calls == 0);
    assert(configureWebProcessExtensions(directory, configure) == WebProcessExtensions::Configured);
    assert(calls == 1);
    assert(g_unlink(file) == 0);
    assert(g_rmdir(directory) == 0);
    g_free(file);
    g_free(missing);
    g_free(directory);
}
