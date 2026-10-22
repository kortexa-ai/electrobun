#include "views_path.h"
#include <cassert>
int main() {
    using electrobun::wpe::safeViewsPath;
    assert(safeViewsPath("mainview/index.html"));
    assert(safeViewsPath("mainview/assets/app.js"));
    assert(!safeViewsPath("../main.js"));
    assert(!safeViewsPath("mainview/../../main.js"));
    assert(!safeViewsPath("/etc/passwd"));
    assert(!safeViewsPath("mainview\\..\\main.js"));
}
