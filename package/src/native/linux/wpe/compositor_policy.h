#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace electrobun::wpe {
// WPE exports premultiplied BGRA. Opaque layers retain the fast memcpy path.
inline void compositeRow(uint8_t* dst, const uint8_t* src, size_t pixels, bool transparent) {
    if (!transparent) {
        std::memcpy(dst, src, pixels * 4);
        return;
    }
    for (size_t i = 0; i < pixels; ++i, dst += 4, src += 4) {
        const unsigned inverseAlpha = 255 - src[3];
        for (unsigned channel = 0; channel < 4; ++channel) {
            dst[channel] = static_cast<uint8_t>(std::min(255u,
                src[channel] + (dst[channel] * inverseAlpha + 127) / 255));
        }
    }
}
}
