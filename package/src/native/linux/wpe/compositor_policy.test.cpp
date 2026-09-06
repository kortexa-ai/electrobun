#include "compositor_policy.h"
#include <cassert>

int main() {
    uint8_t dst[4]{20, 40, 80, 255};
    const uint8_t empty[4]{0, 0, 0, 0};
    electrobun::wpe::compositeRow(dst, empty, 1, true);
    assert(dst[0] == 20 && dst[3] == 255);
    const uint8_t half[4]{50, 0, 0, 128};
    electrobun::wpe::compositeRow(dst, half, 1, true);
    assert(dst[0] == 60 && dst[1] == 20 && dst[2] == 40 && dst[3] == 255);
    electrobun::wpe::compositeRow(dst, half, 1, false);
    assert(dst[0] == 50 && dst[1] == 0 && dst[3] == 128);
}
