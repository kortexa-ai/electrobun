#pragma once
#include <string_view>

namespace electrobun::wpe {
inline bool safeViewsPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != path.npos) return false;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part == "." || part == "..") return false;
        if (slash == path.npos) break;
        path.remove_prefix(slash + 1);
    }
    return true;
}
}
