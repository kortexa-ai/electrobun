// input.cpp — libinput → WPE input events.
//
// Phase 2 skeleton. Real event dispatch stubbed — the start() method opens
// the libinput context and registers a GLib IO watch, but the translation
// from libinput events into InputEvent structs (and forwarding them to the
// callback) is sketched with TODOs.
//
// Proven working at the API level in Phase 0 step 5 (wpe-phase0-step5).
// This file re-packages that work into the class shape Electrobun uses.

#include "input.h"

#include <libinput.h>
#include <libudev.h>
#include <glib.h>
#include <glib-unix.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <cstring>

namespace electrobun {
namespace wpe {

namespace {

int open_restricted(const char* path, int flags, void* /*ud*/) {
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

void close_restricted(int fd, void* /*ud*/) {
    close(fd);
}

const struct libinput_interface kInterface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

} // anon

struct InputDispatcher::Impl {
    InputDispatcherConfig cfg;
    InputCallback callback;

    struct udev*     udev   = nullptr;
    struct libinput* li     = nullptr;
    guint            ioWatchId = 0;

    ~Impl() { teardown(); }

    void teardown() {
        if (ioWatchId) {
            g_source_remove(ioWatchId);
            ioWatchId = 0;
        }
        if (li) {
            libinput_unref(li);
            li = nullptr;
        }
        if (udev) {
            udev_unref(udev);
            udev = nullptr;
        }
    }

    // GLib IO callback: libinput fd is readable.
    static gboolean onFdReadable(gint /*fd*/, GIOCondition /*cond*/, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        libinput_dispatch(self->li);
        struct libinput_event* ev;
        while ((ev = libinput_get_event(self->li))) {
            self->translateAndDispatch(ev);
            libinput_event_destroy(ev);
        }
        return G_SOURCE_CONTINUE;
    }

    void translateAndDispatch(struct libinput_event* ev) {
        enum libinput_event_type t = libinput_event_get_type(ev);
        InputEvent out{};
        bool fire = false;

        switch (t) {
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                auto* ke = libinput_event_get_keyboard_event(ev);
                out.type = (libinput_event_keyboard_get_key_state(ke) == LIBINPUT_KEY_STATE_PRESSED)
                           ? InputEventType::KeyDown : InputEventType::KeyUp;
                out.keycode = libinput_event_keyboard_get_key(ke);
                out.timeMs = libinput_event_keyboard_get_time(ke);
                fire = true;
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* pe = libinput_event_get_pointer_event(ev);
                out.type = InputEventType::PointerMotion;
                out.dx = libinput_event_pointer_get_dx(pe);
                out.dy = libinput_event_pointer_get_dy(pe);
                // TODO(phase2): maintain running cursor (x,y), clamp to logical
                // screen size, apply rotation, set normalized x/y.
                out.timeMs = libinput_event_pointer_get_time(pe);
                fire = true;
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* pe = libinput_event_get_pointer_event(ev);
                out.type = (libinput_event_pointer_get_button_state(pe) == LIBINPUT_BUTTON_STATE_PRESSED)
                           ? InputEventType::PointerButtonDown : InputEventType::PointerButtonUp;
                out.button = libinput_event_pointer_get_button(pe);
                out.timeMs = libinput_event_pointer_get_time(pe);
                fire = true;
                break;
            }
            case LIBINPUT_EVENT_TOUCH_DOWN:
            case LIBINPUT_EVENT_TOUCH_MOTION: {
                auto* te = libinput_event_get_touch_event(ev);
                out.type = (t == LIBINPUT_EVENT_TOUCH_DOWN)
                           ? InputEventType::TouchDown : InputEventType::TouchMotion;
                out.touchSlot = libinput_event_touch_get_slot(te);
                // Transformed to 0..1 across the target screen.
                // TODO(phase2): apply rotationQuarterTurns to (x, y) here so
                // WPE sees the logical-landscape coordinate system.
                out.x = libinput_event_touch_get_x_transformed(te, cfg.screenWidth  ? cfg.screenWidth  : 1) / (cfg.screenWidth  ? cfg.screenWidth  : 1.0);
                out.y = libinput_event_touch_get_y_transformed(te, cfg.screenHeight ? cfg.screenHeight : 1) / (cfg.screenHeight ? cfg.screenHeight : 1.0);
                out.timeMs = libinput_event_touch_get_time(te);
                fire = true;
                break;
            }
            case LIBINPUT_EVENT_TOUCH_UP: {
                auto* te = libinput_event_get_touch_event(ev);
                out.type = InputEventType::TouchUp;
                out.touchSlot = libinput_event_touch_get_slot(te);
                out.timeMs = libinput_event_touch_get_time(te);
                fire = true;
                break;
            }
            default:
                // Ignore device added/removed, frame events, scroll wheel for now.
                break;
        }

        if (fire && callback) callback(out);
    }
};

InputDispatcher::InputDispatcher(const InputDispatcherConfig& cfg, InputCallback cb)
    : impl_(new Impl) {
    impl_->cfg = cfg;
    impl_->callback = std::move(cb);
}

InputDispatcher::~InputDispatcher() = default;

bool InputDispatcher::start() {
    impl_->udev = udev_new();
    if (!impl_->udev) {
        fprintf(stderr, "[InputDispatcher] udev_new failed\n");
        return false;
    }
    impl_->li = libinput_udev_create_context(&kInterface, impl_.get(), impl_->udev);
    if (!impl_->li) {
        fprintf(stderr, "[InputDispatcher] libinput_udev_create_context failed\n");
        return false;
    }
    if (libinput_udev_assign_seat(impl_->li, "seat0") != 0) {
        fprintf(stderr, "[InputDispatcher] libinput_udev_assign_seat(seat0) failed "
                        "— need root or input group membership\n");
        return false;
    }

    int fd = libinput_get_fd(impl_->li);
    impl_->ioWatchId = g_unix_fd_add(fd, G_IO_IN, Impl::onFdReadable, impl_.get());
    return true;
}

void InputDispatcher::stop() {
    impl_->teardown();
}

} // namespace wpe
} // namespace electrobun
