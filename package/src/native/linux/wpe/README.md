# WPE/DRM backend

This directory contains the backend for Electrobun on bare-DRM
Linux (see `../../../../linux-wpe.md` for the full design). It targets:

- **Display:** DRM/KMS directly (no X11, no Wayland, no compositor).
- **Webview engine:** WPE WebKit 2.0 via `libwpe` + `WPEBackend-fdo`.
- **Input:** `libinput` on `seat0`.
- **Event loop:** GLib main context driving all three.

## Status

The backend is functional. It owns DRM scanout and input, creates WPE
exportable views, composites multiple view layers, and presents page flips
without blocking the GLib event loop.

| File | Status |
|------|--------|
| `drm_display.h` / `drm_display.cpp` | DRM mode selection, pitch-correct double buffers, and asynchronous page flips. |
| `egl_readback.h` / `egl_readback.cpp` | GLES compositor for WPE EGLImages. Uses V3D or the host's equivalent GPU instead of forcing WebKit through SHM/llvmpipe. |
| `input.h` / `input.cpp` | libinput context, GLib IO watch, event translation, and rotation-aware coordinates. |
| `wpe_backend.cpp` | WPE view pool, navigation/RPC bridge, window chrome, layer composition, and EGL-to-SHM fallback selection. |

## Rendering path

1. `WpeBackend` opens the connected DRM/KMS output and uses its preferred
   mode and driver-assigned buffer pitch.
2. `EglReadback` creates a GBM-backed EGL display on a render node.
3. WPEBackend-fdo exports each WebKit frame as an EGLImage, keeping WebGL
   rendering on the GPU.
4. GLES composites the active Electrobun views in z-order. When
   `GL_EXT_read_format_bgra` is available, the shader applies the configured
   0/90/180/270-degree rotation and reads directly into the next native DRM
   buffer. Otherwise it uses a portable RGBA readback plus CPU conversion.
5. The DRM fd's GLib watch queues page flips and acknowledges WPE frames at
   display cadence. If EGL setup fails, the backend falls back to its SHM
   renderer instead of failing startup.

This is still a readback bridge, not zero-copy scanout. A future renderer can
replace the final download with GBM buffer scanout without changing WPE view
management or Electrobun's window model.

## Portability boundary

The WPE EGL export, GLES composition, DRM page-flip, pitch handling, SHM
fallback, and four rotation transforms are generic to embedded Linux systems
providing WPEBackend-fdo, EGL/GBM/GLES, and DRM/KMS.

Deployment-specific facts stay runtime-selected:

- `ELECTROBUN_DRM_CARD` selects the KMS card; otherwise connected cards are
  probed.
- `ELECTROBUN_RENDER_NODE` selects the EGL render node; the conventional
  `/dev/dri/renderD128` is the current default.
- Rotation first honors `ELECTROBUN_ROTATE=0|90|180|270`, then inherits
  `/sys/class/graphics/fbcon/rotate` when the Linux console configured a
  panel orientation, and otherwise defaults to 0.
- The display mode comes from the connected output unless a preferred mode is
  configured.

## Prerequisites (verified in Phase 0)

- `apt install cog libwpewebkit-2.0-1 libwpebackend-fdo-1.0-1 libwpe-1.0-dev libwpebackend-fdo-1.0-dev libinput-dev libudev-dev libdrm-dev libgbm-dev libvulkan-dev xdg-desktop-portal`
- V3DV (Mesa 25.0.7+) with Vulkan 1.3, `VK_KHR_display` extension — confirmed present
- Rotation does not rely on a DRM CRTC rotation property. The EGL compositor
  applies it in its shader; the SHM fallback applies it during its CPU blit.

The embedded installer writes
`~/.config/xdg-desktop-portal/portals.conf` with `default=none`. This is
intentional for the bare-console target: xdg-desktop-portal still exposes its
built-in power-profile and realtime APIs, but does not probe GTK or another GUI
backend. Without it, WPE WebKit waits twice for 25 seconds before producing its
first frame on a session with no desktop.

## How to run

The resulting binary needs to run from a real TTY (or via `openvt -c 2 -s -f`)
to acquire DRM master. See the `../../../../linux-wpe.md` §8 dev loop.
