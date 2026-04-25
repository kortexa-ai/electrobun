// drm_display.cpp — DRM/KMS output implementation.
//
// Phase 2 skeleton: opens DRM, picks a mode, allocates dumb buffers, blits
// provided pixels to the scanout buffer with CORRECT STRIDE HANDLING, and
// page-flips. Supports software rotation as a Phase 2 stopgap.
//
// Status: right-shaped; init() path is sketched; acquire()/present() are
// stubbed. Fill in Phase 2 proper.
//
// References:
// - kernel.org DRM docs, drmModeAddFB2 / drmModePageFlip
// - Pi 5 vc4 driver (kernel drivers/gpu/drm/vc4/)
// - Cog's platform-drm plugin (reference for the parts that aren't broken)

#include "drm_display.h"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <string>

namespace electrobun {
namespace wpe {

namespace {

struct DumbBuffer {
    uint32_t handle = 0;   // GEM handle
    uint32_t fbId   = 0;   // KMS framebuffer ID (from drmModeAddFB2)
    uint32_t pitch  = 0;   // DRIVER-ASSIGNED stride in bytes — DO NOT compute as w*bpp
    uint64_t size   = 0;   // total buffer size in bytes
    uint8_t* map    = nullptr;
};

} // anon

struct DrmDisplay::Impl {
    DrmDisplayConfig cfg;
    std::string lastError;

    int fd = -1;
    uint32_t connectorId = 0;
    uint32_t crtcId = 0;
    drmModeModeInfo mode = {};

    // Double-buffered scanout.
    DumbBuffer buffers[2] = {};
    int frontBuffer = 0;  // currently-scanning buffer
    int backBuffer  = 1;  // next to render into

    // Logical dimensions (post-rotation).
    uint32_t logicalW = 0;
    uint32_t logicalH = 0;

    bool pageFlipPending = false;

    ~Impl() {
        for (auto& b : buffers) {
            if (b.fbId) drmModeRmFB(fd, b.fbId);
            if (b.map)  munmap(b.map, b.size);
            if (b.handle) {
                drm_mode_destroy_dumb destroy = {};
                destroy.handle = b.handle;
                drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            }
        }
        if (fd >= 0) {
            // Drop DRM master before closing. Without this, the next VT's
            // login session can't take the display on clean exit (the kernel
            // only releases master on process death, which happens later than
            // the user expects on a kiosk SIGTERM).
            drmDropMaster(fd);
            close(fd);
        }
    }

    void setError(const std::string& msg) {
        lastError = msg;
        fprintf(stderr, "[DrmDisplay] %s\n", msg.c_str());
    }

    bool openDevice() {
        fd = open(cfg.cardPath.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            setError(std::string("open(") + cfg.cardPath + ") failed: " + strerror(errno));
            return false;
        }
        // Become DRM master if possible (no-op if we already are).
        // TODO(phase2): handle logind seat grabbing for a proper systemd launch.
        drmSetMaster(fd);  // fine to ignore return — best effort.
        return true;
    }

    bool pickModeAndOutput() {
        drmModeResPtr res = drmModeGetResources(fd);
        if (!res) {
            setError("drmModeGetResources failed");
            return false;
        }

        // Find first connected connector with modes.
        drmModeConnectorPtr conn = nullptr;
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
                conn = c;
                break;
            }
            if (c) drmModeFreeConnector(c);
        }
        if (!conn) {
            drmModeFreeResources(res);
            setError("no connected connector with modes");
            return false;
        }
        connectorId = conn->connector_id;

        // Pick preferred mode (first one, per DRM convention), or a user-specified one.
        mode = conn->modes[0];
        if (cfg.preferredWidth && cfg.preferredHeight) {
            for (int i = 0; i < conn->count_modes; i++) {
                if (conn->modes[i].hdisplay == cfg.preferredWidth &&
                    conn->modes[i].vdisplay == cfg.preferredHeight) {
                    mode = conn->modes[i];
                    break;
                }
            }
        }

        // Find a CRTC that can drive this connector.
        // TODO(phase2): properly probe possible_crtcs via encoder.
        drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (!enc) {
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            setError("drmModeGetEncoder failed");
            return false;
        }
        crtcId = enc->crtc_id;

        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);

        // Compute logical dimensions after rotation.
        if (cfg.rotation == Rotation::CW90 || cfg.rotation == Rotation::CCW90) {
            logicalW = mode.vdisplay;
            logicalH = mode.hdisplay;
        } else {
            logicalW = mode.hdisplay;
            logicalH = mode.vdisplay;
        }
        return true;
    }

    bool allocDumbBuffer(DumbBuffer& b) {
        drm_mode_create_dumb create = {};
        create.width  = mode.hdisplay;
        create.height = mode.vdisplay;
        create.bpp    = 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
            setError(std::string("DRM_IOCTL_MODE_CREATE_DUMB: ") + strerror(errno));
            return false;
        }
        b.handle = create.handle;
        b.pitch  = create.pitch;   // <<< DRIVER-ASSIGNED pitch; respect it.
        b.size   = create.size;

        // Register as framebuffer.
        uint32_t handles[4] = { b.handle, 0, 0, 0 };
        uint32_t pitches[4] = { b.pitch, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        if (drmModeAddFB2(fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
                          handles, pitches, offsets, &b.fbId, 0) != 0) {
            setError("drmModeAddFB2 failed");
            return false;
        }

        // Map for CPU write.
        drm_mode_map_dumb map = {};
        map.handle = b.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
            setError("DRM_IOCTL_MODE_MAP_DUMB failed");
            return false;
        }
        b.map = (uint8_t*)mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        if (b.map == MAP_FAILED) {
            b.map = nullptr;
            setError(std::string("mmap: ") + strerror(errno));
            return false;
        }

        // Clear to opaque black so uninitialized rows don't scan out as garbage.
        std::memset(b.map, 0, b.size);
        return true;
    }

    bool setMode() {
        // Set the initial mode using the front buffer.
        int rc = drmModeSetCrtc(fd, crtcId, buffers[frontBuffer].fbId, 0, 0,
                                &connectorId, 1, &mode);
        if (rc != 0) {
            setError(std::string("drmModeSetCrtc: ") + strerror(-rc));
            return false;
        }
        return true;
    }
};

DrmDisplay::DrmDisplay(const DrmDisplayConfig& cfg) : impl_(new Impl) {
    impl_->cfg = cfg;
}

DrmDisplay::~DrmDisplay() = default;

bool DrmDisplay::init() {
    if (!impl_->openDevice()) return false;
    if (!impl_->pickModeAndOutput()) return false;
    if (!impl_->allocDumbBuffer(impl_->buffers[0])) return false;
    if (!impl_->allocDumbBuffer(impl_->buffers[1])) return false;
    if (!impl_->setMode()) return false;
    return true;
}

uint32_t DrmDisplay::logicalWidth()  const { return impl_->logicalW; }
uint32_t DrmDisplay::logicalHeight() const { return impl_->logicalH; }

namespace {
void pageFlipHandler(int /*fd*/, unsigned /*frame*/, unsigned /*sec*/,
                     unsigned /*usec*/, void* userData) {
    auto* pending = static_cast<bool*>(userData);
    *pending = false;
}
} // anon

DrmFrame DrmDisplay::acquire() {
    // Block until the previous page flip (if any) has completed; otherwise
    // the kernel will refuse a new flip with EBUSY.
    while (impl_->pageFlipPending) {
        drmEventContext evctx = {};
        evctx.version = DRM_EVENT_CONTEXT_VERSION;
        evctx.page_flip_handler = pageFlipHandler;
        drmHandleEvent(impl_->fd, &evctx);
    }
    auto& b = impl_->buffers[impl_->backBuffer];
    return DrmFrame{
        .pixels = b.map,
        .width  = impl_->mode.hdisplay,
        .height = impl_->mode.vdisplay,
        .pitch  = b.pitch,
    };
}

void DrmDisplay::present() {
    impl_->pageFlipPending = true;
    int rc = drmModePageFlip(impl_->fd, impl_->crtcId,
                             impl_->buffers[impl_->backBuffer].fbId,
                             DRM_MODE_PAGE_FLIP_EVENT,
                             &impl_->pageFlipPending);
    if (rc != 0) {
        impl_->pageFlipPending = false;
        impl_->setError(std::string("drmModePageFlip: ") + strerror(-rc));
        return;
    }
    std::swap(impl_->frontBuffer, impl_->backBuffer);
}

const std::string& DrmDisplay::getLastError() const { return impl_->lastError; }

} // namespace wpe
} // namespace electrobun
