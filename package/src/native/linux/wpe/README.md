# WPE/DRM backend — Phase 2 skeleton

This directory contains the **Phase 2** backend for Electrobun on bare-DRM
Linux (see `../../../../linux-wpe.md` for the full design). It targets:

- **Display:** DRM/KMS directly (no X11, no Wayland, no compositor).
- **Webview engine:** WPE WebKit 2.0 via `libwpe` + `WPEBackend-fdo`.
- **Input:** `libinput` on `seat0`.
- **Event loop:** GLib main context driving all three.

## Status

This is a **right-shaped skeleton** written after Phase 0 validation
(2026-04-24). Class wiring, headers, includes, and TODOs are in place.
Several code paths are stubbed and explicitly marked `TODO(phase2)`.

| File | Status |
|------|--------|
| `drm_display.h` / `drm_display.cpp` | init + mode/buffer setup written. **Stride handling is correct** (uses `drmModeAddFB2`-assigned pitch). `acquire()`/`present()` page-flip plumbing stubbed. |
| `input.h` / `input.cpp` | libinput context + GLib IO watch + event translation written. Based on the proven code from `~/src/wpe-phase0-step5/`. Rotation transform of pointer/touch coords TODO. |
| `wpe_backend.cpp` | `WpeBackend : IDisplayBackend, IWebviewBackend` + `WpeWebViewImpl : AbstractView` class shape complete. **WPE view backend creation (WPEBackend-fdo → offscreen buffer → CPU readback) is the biggest gap.** |

## What's needed to render "Hello, Electrobun" on the Pi 5 bar screen

1. **Finish `DrmDisplay::acquire()` and `DrmDisplay::present()`:**
   integrate `drmModePageFlip` with the GLib main loop via a custom
   `GSource` wrapping the DRM fd (so page-flip completion events pump
   through the same loop as libwpe and libinput). ~100 LOC.

2. **Instantiate a WPEBackend-fdo "exportable" view backend** in
   `WpeBackend::createWebview()`. Something like:
   ```c
   wpe_view_backend_exportable_fdo_egl_create(&callbacks, user_data, w, h);
   ```
   Then attach it to a `WebKitWebView`:
   ```c
   auto* viewBackend = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
   auto* webView     = webkit_web_view_new_with_related_view(/* ... */);
   ```
   WPE renders the page into an EGLImage we receive via the exportable
   callback. ~150 LOC.

3. **Blit EGLImage → DrmFrame:**
   simplest path is `glReadPixels` (or a PBO download) into CPU memory,
   then memcpy into `DrmFrame::pixels` **respecting `DrmFrame::pitch`**
   (not `width * 4` — that's the Cog bug we confirmed in Phase 0).
   Include CPU rotation if `rotationQuarterTurns != 0`. Mark with
   `TODO(phase4): delete — absorbed into composite shader`.
   ~80 LOC.

4. **Wire the FFI entry points.** Two options:
   - **(a) Build a separate `libNativeWrapper_wpe.so`** (the doc's
     preferred path) with its own copy of `nativeWrapper.cpp`-style
     FFI exports that route through `currentDisplayBackend()` /
     `currentWebviewBackend()` instead of calling GTK directly. The
     FFI functions that are GTK-only (clipboard, tray, file dialogs,
     global shortcuts) become noops or have minimal kiosk-appropriate
     implementations.
   - **(b) Single `.so` with runtime backend selection** in the
     existing `nativeWrapper.cpp` (change `createWindow`/`initWebview`/
     `runEventLoop` to consult `currentDisplayBackend()`).
     Simpler but fattens every binary with WPE code.
   - Doc picks (a). Commit 4 of Phase 1 (deferred) is the cleaner
     version of (b) if we ever want it.

5. **Build system (minimal Phase 6):** extend `package/build.ts`'s
   Linux branch to produce `libNativeWrapper_wpe.so` by compiling
   `nativeWrapper.cpp` + `wpe/*.cpp` with `HAVE_WPE=1` and linking
   `wpe-webkit-2.0 wpebackend-fdo-1.0 libinput libdrm libgbm` via
   `pkg-config`. ~50 LOC.

6. **CLI flag:** `build.linux.embedded: true` (or `bundleWPE: true`)
   in `src/cli/index.ts` selects the `_wpe.so` at bundle time.
   ~10 LOC.

**Total remaining to "Hello, Electrobun" on screen: ~400 LOC of real code**
plus build-system plumbing. Each step is independently verifiable with
the webcam feedback loop (run test → `ffmpeg ... /tmp/snap.jpg` →
inspect image).

## Prerequisites (verified in Phase 0)

- `apt install cog libwpewebkit-2.0-1 libwpebackend-fdo-1.0-1 libwpe-1.0-dev libwpebackend-fdo-1.0-dev libinput-dev libudev-dev libdrm-dev libgbm-dev libvulkan-dev xdg-desktop-portal`
- V3DV (Mesa 25.0.7+) with Vulkan 1.3, `VK_KHR_display` extension — confirmed present
- Rotation: **cannot** rely on DRM CRTC rotation property on V3D vc4;
  do it in software (Phase 2) or in composite shader (Phase 4+)

The embedded installer writes
`~/.config/xdg-desktop-portal/portals.conf` with `default=none`. This is
intentional for the bare-console target: xdg-desktop-portal still exposes its
built-in power-profile and realtime APIs, but does not probe GTK or another GUI
backend. Without it, WPE WebKit waits twice for 25 seconds before producing its
first frame on a session with no desktop.

## How to run (once implementations are filled in)

The resulting binary needs to run from a real TTY (or via `openvt -c 2 -s -f`)
to acquire DRM master. See the `../../../../linux-wpe.md` §8 dev loop.
