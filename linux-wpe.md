# linux-wpe: Electrobun for bare-DRM Linux (Raspberry Pi / embedded)

Working notes for a new Electrobun target that runs on a Linux system with **no X11 and no Wayland** — straight DRM/KMS framebuffer — while keeping the same JS source (React + three.js + WebGPU) that runs on the existing macOS / Windows / Linux-desktop targets.

> Status: design doc, v4 (after Phase 0 + standalone Phase 2 validation on Pi 5 + Debian Trixie 13.4 + V3DV).
> Branch: `kortexa/linux-wpe`.
> **Phase 0: complete (2026-04-24).** Architecture validated empirically — see §5 Phase 0 results.
> **Phase 1: partial (2026-04-24).** Commits 1–3 landed (backend seam, Rect migration, AbstractView header extraction). Commit 4 (GtkBackend/WebKitBackend wrappers) deferred as optional.
> **Phase 2: standalone proof complete (2026-04-24).** `drm_hello` and `wpe_hello` render pixels end-to-end via our own DRM scanout code; `wpe_hello` renders real HTML via WPE WebKit. See §5 Phase 2 standalone validation. **Remaining: integration with Electrobun FFI/build — see §11.**
> **Path-to-demo priority (smallest wedge first):**
> 1. ~~Phase 1 + Phase 2 + just-enough Phase 6 = **bare HTML "Hello, Electrobun" on the Pi from a VT.**~~ **Pixels achieved via standalone `wpe_hello` (2026-04-24).** Remaining: FFI/build integration so it's a real Electrobun binary. See §11.
> 2. Same path validates **JS + React** (WPE has full JS/DOM; once HTML renders, React renders).
> 3. Phases 3–5 (Dawn/WebGPU + three.js) are aspirational and deferred until the basic app-building experience works end-to-end.

---

## 1. Goal

One Electrobun app, four packaged builds, identical JS source:

| Target            | Webview               | GPU backend                          | Display                 |
|-------------------|-----------------------|--------------------------------------|-------------------------|
| macOS             | WKWebView             | Dawn / Metal                         | AppKit                  |
| Windows           | WebView2              | Dawn / D3D12                         | Win32                   |
| Linux desktop     | WebKitGTK (or CEF)    | Dawn / Vulkan                        | X11 or Wayland via GTK  |
| **Linux embedded (new)** | **WPE WebKit**   | **Dawn / Vulkan via `VK_KHR_display`** | **DRM/KMS, no compositor** |

JS side is untouched. All platform differences live in the native shell.

---

## 2. Why WPE, not the other options

- **WebKitGTK** needs GTK → X11/Wayland. Non-starter on bare DRM.
- **CEF on Linux** assumes X11 (Ozone-X11) or Wayland; doesn't expose `--ozone-platform=drm` cleanly through the CEF embed API. Size also defeats the Electrobun value prop.
- **Chromium `--ozone-platform=drm` directly** works but isn't Electrobun — ships a whole browser, collapses the "single native architecture across four targets" story.
- **WPE WebKit** — sibling port of WebKitGTK designed exactly for this: no window system assumption, DRM/FDO/fbdev backends, ships in set-top boxes and car IVI. Same JavaScriptCore, same WebKit core, same WebGPU flag story.

---

## 3. What's already in the repo (key findings)

Read through `package/src/native/linux/nativeWrapper.cpp` (11,066 LOC, single TU) and `package/build.ts` (2,306 LOC). Relevant findings:

**Existing abstractions we can reuse:**
- `class AbstractView` (line 2115) with three impls:
  - `WebKitWebViewImpl` (line 2287) — WebKitGTK path
  - `WGPUViewImpl` (line 3342) — native Dawn surface path
  - `CEFWebViewImpl` (line 3633) — CEF OSR path
- `createGTKWindow` / `createX11Window` — two window strategies already (GTK for webview, X11 direct for CEF). Precedent for a third.
- Dawn symbol loading via `dlsym` (line ~6888 onwards) — `wgpuInstanceCreateSurface`, `wgpuSurfaceConfigure`, `wgpuSurfaceGetCurrentTexture`, `wgpuSurfacePresent` are all dynamically resolved. Entirely reusable.
- FFI export ABI (`ELECTROBUN_EXPORT` + `extern "C"`) is clean and backend-agnostic.

**Existing build plumbing we can copy:**
- `buildNativeWrapper` in `package/build.ts` (Linux branch, ~line 1860) compiles `nativeWrapper.cpp` once, links twice — GTK-only `libNativeWrapper.so` and CEF-enhanced `libNativeWrapper_cef.so`. We add a third link step.
- CLI `package/src/cli/index.ts:2739` already picks the `.so` based on `config.build.linux?.bundleCEF`. We extend to pick `libNativeWrapper_wpe.so` for `bundleWPE` or `build.linux.embedded`.
- Config defaults at `src/cli/index.ts:1497` / `1511` / `1518` already have `bundleCEF` / `bundleWGPU` fields; `bundleWPE` / `embedded` follows the pattern.

**Existing frictions that force some refactor:**
- `AbstractView` base carries GTK-specific members (`GtkWidget* widget`, `GdkRectangle visualBounds`) — not truly abstract for display purposes. A WPE impl cannot cleanly inherit without either dragging GTK headers or carving a seam.
- `WGPUViewImpl` currently creates an `XCreateWindow` for its Dawn surface. Hardcoded X11 path; WPE target needs `VK_KHR_display`.
- Event loop is `gtk_main()`. WPE target needs its own (GLib main context driven by libwpe + a libinput fd + DRM fd).
- GTK/X11 headers are included unconditionally at the top of `nativeWrapper.cpp`.

---

## 4. Architectural strategy (minimal-change principles)

Two design rules to keep the upstream PR tractable:

1. **Do not fork `nativeWrapper.cpp`.** Duplicating 11K lines is a maintenance disaster and a bad PR.
2. **Do not `#ifdef` the existing file into swiss cheese.** Harder to read, harder to review, couples backends.

Middle path: **carve a thin backend seam**, keep the bulk of the existing code where it is, add WPE as a new TU behind the seam.

The seam has three concerns:
- **Display backend** — owns the output surface (GTK window, X11 window, DRM scanout).
- **Webview backend** — produces HTML pixels (WebKitGTK webview widget, WPE view on an FDO target).
- **Event loop** — drives input + webview updates (GTK main vs GLib+libinput+DRM).

WGPU surface creation, Dawn integration, FFI surface, navigation rules, IPC, and everything else stays shared.

### Seam proposal

Extract three tiny interfaces (headers only), declared in `package/src/native/linux/backend.h`:

```cpp
// Opaque display backend — owns windows, compositor, scanout.
struct IDisplayBackend {
    virtual ~IDisplayBackend() = default;
    virtual void* createWindow(WindowSpec spec) = 0;
    virtual void runEventLoop() = 0;
    virtual void present(void* windowHandle) = 0;
    // + whatever else the window FFI actually needs, kept minimal
};

// Opaque webview backend — creates web views that render into a display window.
struct IWebviewBackend {
    virtual ~IWebviewBackend() = default;
    virtual std::shared_ptr<AbstractView> createWebview(WebviewSpec spec) = 0;
};

IDisplayBackend& currentDisplayBackend();
IWebviewBackend& currentWebviewBackend();
```

The GTK+WebKit+X11 code stays in `nativeWrapper.cpp` but is wrapped in a `GtkBackend` / `WebKitBackend` implementation of those interfaces. Zero behavior change.

`AbstractView` loses its hard GTK members; they move to a subclass-visible opaque pointer or to `WebKitWebViewImpl` directly.

This refactor is the one thing we do to upstream's code. Everything else is additive.

---

## 5. Phased plan

Each phase is an independent commit / reviewable chunk. Resist combining.

### Phase 0 — prove ingredients outside Electrobun (on moodymoose)

Before touching any code, verify the stack works on Pi 5 aarch64 Debian Bookworm from a bare VT:

1. `apt install cog libwpewebkit-1.1-0 libwpebackend-fdo-1.0-1` (package names subject to drift).
2. Ctrl-Alt-F1 to a VT. `cog --platform=drm https://webgpureport.org`. Confirms WPE+DRM path works.
3. `vulkaninfo | tee pi-vulkan-baseline.txt`. Confirms V3DV is present; record the feature set for later Dawn debugging.
4. 50-line C program: create `VkSurfaceKHR` via `VK_KHR_display`, clear red, present. Confirms direct-to-display Vulkan without any compositor.
5. 50-line libinput consumer from a VT: print keyboard events. Confirms input path.

If any of 1–5 fail, stop. Do not write Electrobun code against a stack you haven't proven. Expected friction point: **(3)** — V3DV's feature set may not satisfy Dawn. Document gaps; they shape Phase 5.

#### Phase 0 results (2026-04-24)

Tested on:
- **Hardware:** Raspberry Pi 5 ("moodymoose"), Broadcom V3D 7.1.7.0
- **OS:** Debian GNU/Linux 13 (trixie) 13.4, kernel 6.12.75+rpt-rpi-2712 aarch64
- **Mesa:** 25.0.7-2+rpt4 (V3DV + llvmpipe), Vulkan 1.3.305
- **Display:** 480×1920 portrait HDMI panel with capacitive touch (`wch.cn USB2IIC_CTP_CONTROL`, 216×137 mm) — non-standard geometry that doubles as a stress test for stride/rotation handling.
- Throwaway diagnostics live in `~/src/wpe-phase0-step4/` and `~/src/wpe-phase0-step5/`.

##### Step 1 — install (✓ with drift)
Doc's package guess (`libwpewebkit-1.1-0`) is outdated for Trixie; ABI bumped to 2.0.
Final install set:
```
cog libwpewebkit-2.0-1 libwpebackend-fdo-1.0-1
libdrm-tests libinput-tools libvulkan-dev libinput-dev libudev-dev
```
Versions installed: `cog 0.18.4`, `WPE WebKit 2.48.3`, `libwpe 1.16.2`, `libwpebackend-fdo 1.16.0`. `pkg-config` modules to use in Phase 6: `wpe-webkit-2.0` (not 1.1), `wpebackend-fdo-1.0`, `libinput`, `libdrm`, `libgbm`, `vulkan`.

##### Step 2 — `cog --platform=drm` (✓ with finding)
Page loads (`webgpureport.org` → `Loaded successfully` in cog log; `WPEWebProcess` stays alive). Visual rendering happens — but on a 480-wide panel the output shows red content interleaved with **white horizontal "lines"**: a classic stride/pitch mismatch. A `data:text/html,<body style=background:red>` test reproduced the same artifact, ruling out content-side scaling.

**Diagnosis:** Cog's DRM scanout writes `width × bpp` bytes per source row (here: 480 × 4 = 1920 B). The V3D KMS framebuffer has padded scanline pitch (driver alignment requirement). The unwritten trailing bytes per row scan out as the framebuffer's cleared color (white), producing the visible artifact. On a 1920-wide display this would be invisible — 1920 self-aligns to typical pitches — making the bug latent on "normal" hardware.

**Implication:** Cog's DRM backend is *not safe to vendor as-is*. Phase 2's `drm_display.cpp` MUST query the framebuffer's actual pitch from `drmModeAddFB2` and respect it unconditionally for every blit/copy. **The bar screen is a useful stress test that prevents writing accidentally-correct code.**

Also observed: `Cog-DRM-WARNING: Renderer 'modeset' does not support rotation 0 (0 degrees)`. Cog probes the DRM rotation property; the V3D vc4 KMS connector doesn't expose it. Benign for Phase 0; relevant for Phase 2's rotation handling — we cannot rely on the CRTC rotation property on V3D, so rotation must happen in our compositor (CPU memcpy with stride remap, or a fragment shader during the final composite pass).

##### Step 3 — `vulkaninfo` (✓ with finding)
Full dumps preserved at `/home/pi/pi-vulkan-baseline-{summary,full}.txt`. Highlights:

- ✓ V3DV 7.1.7.0, Vulkan 1.3.305, conformance 1.3.8.3 (passes Vulkan 1.3 CTS).
- ✓ `VK_KHR_display`, `VK_KHR_swapchain`, `VK_KHR_get_display_properties2` all present at instance level — the architectural assumption in §4 is supported by the driver.
- ✓ `robustBufferAccess`, `fragmentStoresAndAtomics`, `sampleRateShading` all true.
- ⚠ **`shaderSampledImageArrayDynamicIndexing = false`** — this is the **refined Phase 5 risk.** WebGPU spec assumes dynamic indexing of sampled image arrays. Many three.js materials lower to shaders that need this. Dawn-on-V3DV will likely refuse the adapter outright, or require us to disable WebGPU features that depend on it. Mitigation lives in Phase 5; the cliff is now mapped, not just suspected.
- ⚠ `maxImageDimension2D = 4096` — fine for 1080p and the bar (1920×480), but **4K targets are off the table on V3D.** Document and move on.
- llvmpipe is also enumerated as device 1 (Mesa 25.0.7) — useful as a known-good Vulkan reference if V3DV is ever suspect.

##### Step 4 — `VK_KHR_display` direct-to-display Vulkan (✓✓ load-bearing)
Test program at `~/src/wpe-phase0-step4/vk_display_test.c` (~220 LOC). Creates a Vulkan instance with `VK_KHR_display`, picks V3D, enumerates display + modes + planes, creates `VkSurfaceKHR` via `vkCreateDisplayPlaneSurfaceKHR`, builds a swapchain (3 images, `VK_FORMAT_B8G8R8A8_SRGB`, FIFO present mode), and clears each acquired image to a cycling red→green→blue color via `vkCmdClearColorImage` for ~9 seconds at 60 Hz.

**Result: uniform red, green, blue cycles on the bar display, no stripes, no glitches, no compositor.** All Vulkan calls returned `VK_SUCCESS`.

**This is the load-bearing finding of Phase 0.** It proves three things:

1. The V3DV + KMS + `VK_KHR_display` path produces correct framebuffer scanout on this hardware. The Cog stride bug from step 2 is purely in Cog's user-space code, not in the underlying driver stack.
2. Phase 3's "create `VkSurfaceKHR` via `VK_KHR_display`" branch in `WGPUViewImpl` is empirically supported.
3. The architecture in §4 of this doc is real, not aspirational.

Cleanup leaves benign `MESA: error: destroy dumb object N: Invalid argument` warnings — V3DV's swapchain teardown over-frees its dumb buffers. Cosmetic.

##### Step 5 — libinput from VT (✓)
Test program at `~/src/wpe-phase0-step5/libinput_test.c` (~80 LOC). Opens a libinput context with the udev backend on `seat0`, enumerates devices, polls `libinput_get_fd()` for 30 seconds, prints events.

Devices enumerated:

| sysname | name | caps | note |
|---------|------|------|------|
| event0 | pwr_button | K | Pi 5 power button |
| event1 | vc4-hdmi-0 | KP | HDMI CEC (port 1) |
| event3 | vc4-hdmi-1 | KP | HDMI CEC (port 2) |
| event5 | HID 1908:1331 | K | unknown USB receiver |
| event6 | **wch.cn USB2IIC_CTP_CONTROL** | **T** | **the touchscreen** |
| event7 | HD Webcam eMeet C950 | K | webcam volume/mute hotkeys |

Touch events fire correctly: `LIBINPUT_EVENT_TOUCH_DOWN/MOTION/UP` on slot 0 with x/y in **panel-native millimeters** (range 0–216 × 0–137). The phasing is clean (down → optional motion → up).

**Bonus discovery:** the bar display has a 216 × 137 mm capacitive touchscreen. Kiosk-with-touch is now a real demo target. Phase 2's input handler must transform touch coordinates from panel-native portrait-mm to rotated-landscape-pixels before pumping `PointerEvent`s into WPE.

#### Updated risk assessment (post-Phase 0)

- ~~"V3DV may not satisfy Dawn"~~ → **Refined: V3DV lacks `shaderSampledImageArrayDynamicIndexing`.** This is the precise cliff. Mitigation lives in Phase 5; outcome will likely be "WebGPU works for non-dynamic-indexed shaders, WebGL fallback for the rest."
- ~~"WPE+DRM may not work on Pi 5"~~ → **Confirmed working at the WPE+driver level.** Cog's user-space scanout is broken on non-1920px widths; we replace it.
- ~~"Should we vendor Cog's DRM backend?"~~ → **CLOSED: write our own.** Vendoring would inherit a buggy stride path that wasn't designed for non-standard panel widths.
- **NEW:** CRTC rotation property is not exposed by V3D vc4 driver on this connector. Rotation lives in our code, not in DRM. **Strategy:** CPU rotation in Phase 2 is acceptable as a stopgap (480×1920×4 @ 60 Hz ≈ 220 MB/s each way, sustainable on Pi 5 LPDDR4X but wasteful), but **must not survive into Phase 4.** Phase 4's compositor is already a GPU pass; rotation becomes a sampler transform in the composite shader (a few instructions per fragment, zero extra bandwidth). Mark CPU rotation as TODO-DELETE in Phase 2 code; Phase 4 removes it.
- **NEW:** Touch coordinate space is panel-native portrait-mm, not screen-rotated pixels. Phase 2 input handler must transform.
- **NEW:** Cog's DRM backend prints a misleading "rotation 0 not supported" warning even when no rotation is requested; harmless but confusing in logs. Our own backend won't have this.

#### Phase 1 results (2026-04-24)

Three commits landed on `kortexa/linux-wpe`:

| Commit | Files | Description |
|--------|-------|-------------|
| 1 | `package/src/native/linux/backend.h` (new) | `IDisplayBackend` + `IWebviewBackend` interfaces, `Rect`, `WindowSpec`, `WebviewSpec`. ~85 LOC. |
| 2 | `package/src/native/linux/nativeWrapper.cpp` | Replace `GdkRectangle` with backend-neutral `Rect` across `AbstractView` members, `resize()` virtual signature, pending-resize queue, all FFI dispatchers, CEF's `pendingFrame` (the code-smell cleanup). `GdkRectangle` remains only in the legitimate GDK-monitor-enumeration call sites. Added `toRect` / `toGdkRect` conversion helpers. |
| 3 | `package/src/native/linux/abstract_view.h` (new) | Extract `AbstractView` class declaration to a header; forward-declare `GtkWidget` so Phase 2's WPE TU can derive from `AbstractView` without pulling in `<gtk/gtk.h>`. The `GtkWidget* widget` member survives (opaque pointer semantics). |

Commit 4 (write explicit `GtkBackend`/`WebKitBackend` impls + `currentDisplayBackend()`/`currentWebviewBackend()` getters) was designed but **deferred**. The separate-`.so` approach for the embedded target (see §11) doesn't need runtime backend selection — each `.so` provides its own FFI implementation. We can come back to Commit 4 later for taste or if a future target wants runtime selection.

#### Phase 2 standalone validation (2026-04-24)

Two standalone test binaries in `package/src/native/linux/wpe/` render pixels end-to-end, bypassing Electrobun's FFI/build plumbing so we could prove each layer independently before committing to the full integration. Both live alongside the real Phase 2 code (`drm_display.{h,cpp}`, `input.{h,cpp}`, `wpe_backend.cpp`) and exercise the same `DrmDisplay` class the integrated binary will use.

##### `drm_hello` (Cairo text → DRM scanout)

Uses just `DrmDisplay` + Cairo. Cleared background hue-cycles; foreground renders `"Hello, Electrobun"` via `cairo_show_text` centered and rotated 90° CCW for the physically-rotated bar panel.

Proved:
- `DrmDisplay::init()` — opens `/dev/dri/card1`, enumerates connectors/modes, picks 480×1920 @ 60 Hz preferred mode, allocates dumb buffers with driver-assigned pitch, sets mode ✓
- `DrmDisplay::acquire()` + `present()` — page-flip via `drmModePageFlip` + completion via `drmHandleEvent` works without `EBUSY` ✓
- **Stride handling is correct** on this panel geometry — no Cog-style white-line artifacts ✓
- DRM master hand-off from SSH via `openvt -c 2 -s -f --` works cleanly against fbcon ✓

##### `wpe_hello` (WPE WebKit → DRM scanout)

Uses `DrmDisplay` + `libwpe` + `WPEBackend-fdo` (SHM export path) + `libwpewebkit-2.0`. Creates an exportable view backend at landscape dimensions (1920×480), wraps it in a `WebKitWebViewBackend` + `WebKitWebView`, loads inline HTML (`<body><style>...</style>Hello, Electrobun<span class="dot">.</span></body>`). When WPE calls our `export_shm_buffer` callback, we read `wl_shm_buffer_get_data()` and `wl_shm_buffer_get_stride()`, blit-rotate into the DrmDisplay frame (`D[r][c] = L[c][landscapeW - 1 - r]`, CCW 90°), and page-flip.

Proved:
- **libwpe + WPEBackend-fdo SHM path works on V3DV** — no EGL context required for a functional webview ✓
- `webkit_web_view_load_html` renders CSS gradients, font layout, and glyph rasterization straight into a CPU-readable buffer ✓
- `wl_shm_buffer_get_stride()` returns whatever padded stride the compositor chose — we respect it unconditionally ✓
- The WPE GLib main loop cohabitates with DRM page-flip events and libinput (in principle; wpe_hello doesn't pump input yet, but `InputDispatcher` is written to integrate the same way — proven separately in `~/src/wpe-phase0-step5`) ✓
- Integer-pitch rotation math is correct for the 1920×480 ↔ 480×1920 transpose (text reads horizontally on the physically-rotated panel) ✓

##### What the standalone tests did NOT prove

- **FFI integration** — neither test uses Electrobun's `ELECTROBUN_EXPORT` surface or interacts with Bun. See §11.
- **Build system** — both tests are built by a local `Makefile`, not by `package/build.ts`.
- **Multi-webview composition** — each test drives a single scanout plane with one source. Real Electrobun apps open multiple webviews with z-order and transparency; that's Phase 4's compositor territory, not Phase 2's.
- **Navigation events, postMessage bridges, preload scripts, sandboxing, partitions** — all stubbed to TODOs in `wpe_backend.cpp`. Phase 2.1 completes them.
- **WebGPU via WPE** — out of scope for Phase 2; lives in Phases 3–5.
- **Shutdown hygiene** — tests use a 60-second timer. Real apps need clean SIGINT/SIGTERM handling, seat release, DRM drop-master.



### Phase 1 — refactor: carve the seam (no new functionality)

One commit, touches existing code only, behavior identical.

- Add `package/src/native/linux/backend.h` with `IDisplayBackend` / `IWebviewBackend` / `IEventLoop` (exact shape TBD during impl — start with the smallest surface that works).
- Move GTK-specific members out of `AbstractView`. Put them behind an opaque pointer or into `WebKitWebViewImpl` directly.
- Wrap the existing GTK code path into `GtkBackend` / `WebKitBackend` impls of the interfaces. No code moves across files yet; we can split files later.
- Add a `g_backend` getter; all existing call sites that poke GTK directly should start going through the getter.

Exit criterion: existing Linux desktop build passes its tests unchanged. Diff should be mostly mechanical.

Scope guard: **only** the minimal seam needed to make Phase 2 not require duplicating code. If you find yourself refactoring for taste, stop and ship Phase 1 as-is.

### Phase 2 — hello WPE webview (no WGPU yet)

- New directory `package/src/native/linux/wpe/` with:
  - `wpe_backend.cpp` — `IDisplayBackend` + `IWebviewBackend` impl
  - `drm_display.cpp` — DRM/KMS output (study Cog's DRM backend, MIT-licensed; vendor or reimplement)
  - `input.cpp` — libinput → web events
- New `WpeWebViewImpl : public AbstractView` in `wpe_backend.cpp`, using libwpe + WPEBackend-fdo to render offscreen, then blit via DRM.
- Event loop: GLib main context driving libwpe, libinput fd, DRM pageflip events.
- Electrobun initializes with the WPE backend when target is `linux-embedded`.

Exit criterion: kitchen-sink app boots on Pi from a VT, renders HTML, responds to keyboard/mouse input. No WGPU yet.

**Rotation handling (Phase 2 stopgap):** if the target panel needs rotation (e.g. our 480×1920 bar), do it in CPU as a memcpy-with-coord-swap during the blit. Mark with `// TODO(phase4): delete — absorbed into composite shader`. Don't optimize this; it's throwaway.

### Phase 3 — hello native WGPU on DRM (no webview yet)

- Teach the Dawn surface creation path to accept `linux-embedded` target.
- On that target, create `VkSurfaceKHR` via `VK_KHR_display` instead of `XCreateWindow` + `VK_KHR_xlib_surface`.
- The existing `WGPUViewImpl` + dlsym'd Dawn symbols are reused unchanged.
- Run with webview disabled — just a clear-color window, then a triangle.

Exit criterion: native Dawn surface renders directly to framebuffer, independent of the webview. Works with Electrobun's existing `initWGPUView` FFI unmodified.

### Phase 4 — composite WPE + WGPU

Two viable strategies, pick the simpler first:

**A. Software-composited** (start here). WPE renders to an offscreen buffer (WPEBackend-fdo's native mode). `<electrobun-wgpu>` surfaces render to offscreen textures. A final Dawn pass samples them in Z order and scans out one plane via `VK_KHR_display` swapchain. **Rotation absorbed here:** the composite shader's texcoord transform handles display rotation as a free sampler operation. Delete Phase 2's CPU rotation in this commit.

**B. KMS-plane-composited** (optimization). WPE on one plane, Dawn on another, let the display controller composite. Fussier; revisit only if (A) is CPU/GPU-bound.

Exit criterion: three.js `WebGPURenderer` spinning cube composited over a React UI rendered by WPE, on Pi framebuffer, no X/Wayland.

### Phase 5 — V3DV reality check

Run real three.js WebGPU scenes. Expect problems; triage:
- Dawn refuses the adapter → identify missing feature, pin versions, file upstream, document.
- Mid-frame crash → minimal repro.
- Slow but correct → fine, document the supported feature matrix.
- Nothing works → JS-side feature-detect fallback to `WebGLRenderer` (GLES3 on V3D is solid). Document `electrobun.wgpu.fallback` escape hatch.

Exit criterion: a matrix of "what works on V3DV today" plus a graceful fallback, not a promise of perfect WebGPU.

### Phase 6 — build system & CLI integration

- `package/build.ts`: add a third Linux link step producing `libNativeWrapper_wpe.so`. Follows the existing GTK/CEF pattern ~line 1970. Extra `pkg-config` discovery for `wpe-webkit-1.1` + `wpebackend-fdo-1.0` + `libinput` + `libdrm` + `libgbm`.
- `package/src/cli/index.ts`:
  - Add `bundleWPE` (or `embedded: true`) to Linux defaults at lines 1497/1511/1518.
  - Extend the binary selection at line 2739 to pick `libNativeWrapper_wpe.so` for embedded target.
  - Arbitration: GTK / CEF / WPE are mutually exclusive on Linux.
- Self-extractor (Zig) already handles aarch64-linux; extend target-triple metadata if needed.

Exit criterion: `bun build.ts` on an aarch64 Linux host with WPE dev packages produces all three Linux variants. CLI selects the right one for app config.

### Phase 7 — packaging, docs, launch hygiene

- Systemd unit template for kiosk boot (DRM master, seat grab, auto-restart).
- Update `BUILD.md` and add an `EMBEDDED.md` for the Pi workflow (or section in existing docs).
- Document how to choose between desktop and embedded Linux builds.

### Phase 8 — upstream PR

- Organize into reviewable chunks along phase boundaries.
- Phase 1 (refactor) goes up first as a standalone PR — lowest risk, easiest to review, no new functionality, unblocks everything else. Land it on its own.
- Phases 2–6 as a second PR (or a series if reviewers prefer).
- Expect review on: seam design, naming (`linux-embedded` vs `linux-wpe` vs `linux-drm`), vendoring (Cog's DRM backend), binary-size implications of a third `.so`.

---

## 6. Landmines

- **V3DV vs Dawn feature set.** Biggest risk, **now well-defined post-Phase 0**: V3DV lacks `shaderSampledImageArrayDynamicIndexing`. Mitigation: Phase 5 — try with reduced features, accept WebGL fallback for shaders that require dynamic indexing. Aspirational, but the cliff is mapped.
- **WebGPU inside WPE (`navigator.gpu`).** Behind `ENABLE_WEBGPU` at WPE compile time; less exercised than Safari. Most WebGPU goes through `<electrobun-wgpu>` (native Dawn) anyway — in-webview `navigator.gpu` is nice-to-have.
- **Seat / DRM master / VT permissions.** Classic embedded time sink. Pick one supported launch path (systemd user service with seat grab) and document it; don't support every variant.
- **Refactor creep in Phase 1.** Temptation to "also clean up while I'm in there." Resist. The seam is the only goal; ship it ugly if it's less code.
- **Input mapping depth.** Full keymap + IME + dead keys is a project. Ship US-QWERTY + basic modifiers; defer the rest.
- **HDMI mode selection.** 1080p works fine on V3D; 4K is **out of reach** (`maxImageDimension2D = 4096`, confirmed Phase 0 step 3). Document tested modes.
- **DRM stride/pitch handling.** Confirmed Phase 0 step 2 — V3D KMS framebuffers have padded scanline pitch. Our Phase 2 backend must use the pitch returned by `drmModeAddFB2` for every blit/copy, never `width × bpp`.
- **No CRTC rotation property on V3D vc4.** Rotation lives in our compositor, not in DRM. Phase 2 plans accordingly.
- **Audio.** Out of scope here. ALSA direct or PipeWire-without-Wayland both work; revisit post-Phase 7.

---

## 7. Open questions (decide before Phase 2)

- **Target name.** `linux-embedded` (descriptive) vs `linux-wpe` (engine) vs `linux-drm` (display). Electrobun precedent is capability-based (`bundleCEF`, `bundleWGPU`). Leaning `bundleWPE` as the config flag, with the *target output* named `linux-embedded` in packaging.
- **Config shape.** `build.linux.bundleWPE: true` (parallel to `bundleCEF`) vs `build.linux.embedded: true` (broader). First is lower-friction; second is more honest about what's changing.
- ~~**Vendor Cog's DRM backend or write our own?**~~ → **CLOSED post-Phase 0: write our own.** Cog's DRM scanout is broken on non-1920px-wide panels (Phase 0 step 2). Vendoring would inherit a latent bug with non-obvious failure mode. Studying Cog's source is still useful as reference; copying it isn't.
- **aarch64-only or also x86_64 embedded?** Pi drives the work but x86 industrial/kiosk boxes exist. Build system should support both triples; only aarch64 gets CI coverage initially.
- **Does `<electrobun-wgpu>` need API changes?** Ideally no — behavior identical from JS. But z-order, transparency, clip-rect semantics must match desktop exactly or the single-source claim breaks. Verify in Phase 4.
- **NEW post-Phase 0: touch input as a first-class platform input?** The bar display has a touchscreen. Phase 2's input handler can either map touch to `PointerEvent` (web-standard, easy) or expose a richer Touch API. Recommend `PointerEvent` for parity with desktop; revisit if multi-touch gestures are needed.
- **NEW post-Phase 0: rotation source-of-truth?** Config flag (`build.linux.embedded.rotate: 90 | 180 | 270`)? Auto-detect from EDID? Read from `fbcon=rotate:N` cmdline? Lean: explicit config flag, since EDID often misreports panel orientation on weird hardware.

---

## 8. Dev loop on the Pi

```bash
# on snappy
git push

# on moodymoose (ssh 140)
cd ~/src/electrobun && git pull
cd package
bun build.ts --target=linux-embedded

# run from a real VT (Ctrl-Alt-F1), NOT from SSH-in-a-tmux —
# DRM master and seat grab require a real tty
cd ~/src/electrobun/kitchen
./dist/linux-embedded/kitchen
```

Faster iteration on snappy is possible with a Linux VM (virtio-gpu + `VK_KHR_display`) but V3DV bugs won't reproduce anywhere else. Pi hardware remains authoritative for GPU behavior.

---

## 9. Non-goals

- Supporting X11 or Wayland through this target (use `linux-desktop`).
- Window management — kiosk/single-app target, no tabs, no multi-window.
- Replacing the Linux desktop backend — parallel, not successor.
- Perfect WebGPU parity with desktop — ship what V3DV supports, fall back where it doesn't, document the matrix.

---

## 10. Concrete code-change inventory (for the PR author's checklist)

Files expected to change, in order of phase:

Phase 1 (refactor only):
- `package/src/native/linux/backend.h` — NEW. Interface declarations.
- `package/src/native/linux/nativeWrapper.cpp` — modified: `AbstractView` loses GTK members; GTK code wrapped in `GtkBackend` / `WebKitBackend` impls; all external GTK access goes via `currentDisplayBackend()` getter.

Phase 2:
- `package/src/native/linux/wpe/wpe_backend.cpp` — NEW.
- `package/src/native/linux/wpe/drm_display.cpp` — NEW.
- `package/src/native/linux/wpe/input.cpp` — NEW.

Phase 3:
- `package/src/native/linux/nativeWrapper.cpp` — modified: `WGPUViewImpl` surface creation adds `VK_KHR_display` branch for embedded target.

Phase 6:
- `package/build.ts` — modified: add WPE dev-package pkg-config discovery and third link step in `buildNativeWrapper` Linux branch (~line 1860). Add `libNativeWrapper_wpe.so` to dist copy at ~line 669.
- `package/src/cli/index.ts` — modified: `bundleWPE` defaults (~1497/1511/1518); binary selection (~2739); dev-mode .so swap (~4366).

Phase 7:
- `BUILD.md`, `README.md` — modified.
- New `EMBEDDED.md` or section.
- Systemd unit template (location TBD).

No changes to: WKWebView/macOS code, WebView2/Windows code, Zig self-extractor core (possibly target-triple metadata).

---

## 11. Integration plan: standalone tests → real Electrobun app

Post-Phase-0 + standalone-Phase-2, every capability for a bare-DRM Electrobun app is empirically proven. What's left is plumbing — not research. This section breaks the plumbing into five reviewable sub-phases. Scope is "HTML Hello, Electrobun loaded by Bun via Electrobun's FFI on the Pi from a VT"; JS/React works by the same path once HTML works.

### Phase 2.1 — Merge `wpe_hello.cpp` into `wpe_backend.cpp`

Currently `wpe_backend.cpp` has a class skeleton (`WpeBackend`, `WpeWebViewImpl`) with most method bodies stubbed, while `wpe_hello.cpp` has a working `main()` that does the real WPE+DRM wiring. Fold the working code into the class, delete `wpe_hello.cpp` (or keep it as `wpe_hello_test` under `#ifdef STANDALONE`).

Concrete changes in `wpe_backend.cpp`:

- `WpeBackend::createWindow` — move the `DrmDisplay` construction + `InputDispatcher` start from the skeleton into a working path. Already sketched; just remove the WPE-init work that's currently in `wpe_hello.cpp::main` and put it here (since we want a single WPE runtime even across multiple webviews).
- `WpeBackend::createWebview` — actually create the exportable SHM view backend, the `WebKitWebViewBackend`, and the `WebKitWebView`. Store them on `WpeWebViewImpl`. Wire `webkit_web_view_load_uri` inside the first frame.
- `WpeWebViewImpl::loadURL` / `loadHTML` — call `webkit_web_view_load_uri` / `webkit_web_view_load_html`. Already drafted in the skeleton.
- `WpeWebViewImpl::resize` — call `wpe_view_backend_dispatch_set_size(view_backend, w, h)` (and recreate DRM buffers if the logical size changes — single-window kiosk usually doesn't need this).
- **Input event translation** — `WpeBackend::onInputEvent` currently just logs. Translate to `wpe_input_keyboard_event` / `wpe_input_pointer_event` / `wpe_input_touch_event` and call `wpe_view_backend_dispatch_*` on each active webview's backend.
- **SHM export callback** — move `onExportShm` from `wpe_hello.cpp` into `WpeBackend` as a member (or static dispatcher), route each frame to the target webview's `DrmDisplay`.

**Exit criterion:** a `WpeBackend` instance exposed through `IDisplayBackend` + `IWebviewBackend` renders HTML onto the DRM framebuffer with input working. Equivalent behavior to `wpe_hello`, just class-oriented and FFI-ready.

~200 LOC of real work. Uses only already-installed dependencies.

### Phase 2.2 — `nativeWrapper_wpe.cpp` (parallel FFI surface)

The doc's architectural choice is **two `.so` files**, not runtime selection. The GTK `.so` keeps `nativeWrapper.cpp` unchanged. A new `nativeWrapper_wpe.cpp` provides the same `ELECTROBUN_EXPORT` surface but routes through `WpeBackend` instead of GTK calls.

Strategy: **minimum viable subset first, noop stubs for the rest.**

Required for Hello:
- `createGTKWindow` → `currentDisplayBackend().createWindow(...)`. The GTK-named function stays (CLI/Bun doesn't know it's GTK-specific) — it just does the right thing.
- `initWebview` → `currentWebviewBackend().createWebview(...)`, register in the internal map.
- `loadURLInWebView` / `loadHTMLInWebView` / `resizeWebview` / `webviewRemove` → dispatch to `AbstractView` virtual methods (already work; just expose them).
- `startEventLoop` / `stopEventLoop` → `currentDisplayBackend().runEventLoop()` / `stopEventLoop()`.
- `waitForShutdownComplete` / `forceExit` / `shutdownApplication` → trivial.
- `simpleTest` → return 1.
- `setWindowTitle` / `showWindow` / `setWindowFrame` / etc. → no-op or single-implementation (one fullscreen window always; title is irrelevant on a kiosk).

Stubbed as noops returning 0/nullptr/false:
- `clipboard*` (no clipboard service running on bare DRM by default)
- `createTray`, all tray functions (no system tray)
- `openFileDialog` / `showMessageBox` (could route to a WebKit-rendered modal later)
- `registerGlobalShortcut` and friends (no window manager)
- `getAllDisplays` / `getPrimaryDisplay` — **implement**: one display, `DrmDisplay::logicalWidth()` × `logicalHeight()`. Real JS apps use this for layout.
- `getCursorScreenPoint` / `getMouseButtons` — maintain from libinput; Phase 2.1 already tracks pointer state.
- Everything CEF — stub hard (CEF isn't viable on bare DRM on Pi). `isCEFAvailable` returns `false`, all CEF-specific exports error-log and return.

**Exit criterion:** `libNativeWrapper_wpe.so` has the full FFI export surface. GTK-only functions are noop; DRM-capable functions route to `WpeBackend`.

~300 LOC. Almost all boilerplate (one-line stubs) plus the ~15 real routing functions.

### Phase 2.3 — `package/build.ts` (a third Linux link step)

Existing Linux branch (~line 1860) already compiles `nativeWrapper.cpp` once and links twice (GTK-only `.so`, CEF-enhanced `.so`). Add a **third link step** for the WPE target:

- Compile `nativeWrapper_wpe.cpp` + `wpe/wpe_backend.cpp` + `wpe/drm_display.cpp` + `wpe/input.cpp` with `HAVE_WPE=1`.
- Link with `pkg-config --libs wpe-1.0 wpebackend-fdo-1.0 wpe-webkit-2.0 wayland-server libdrm libinput libudev glib-2.0 gio-unix-2.0` (empirically verified package names on Debian Trixie; drift expected on other distros).
- Output: `libNativeWrapper_wpe.so`.
- Add to dist copy at ~line 669 (the existing GTK+CEF copy logic).

**Exit criterion:** `bun build.ts` on an aarch64 Debian Trixie host with the WPE dev packages produces `libNativeWrapper_wpe.so` alongside the existing `.so` files.

~50 LOC change in `build.ts`.

### Phase 2.4 — CLI flag + binary selection

In `package/src/cli/index.ts`:

- Add `bundleWPE` (or `build.linux.embedded: true`) in Linux defaults (~lines 1497 / 1511 / 1518).
- Binary selection at ~line 2739: pick `libNativeWrapper_wpe.so` when `bundleWPE` is set; arbitrate mutual exclusion with `bundleCEF` (both on is an error).
- Dev-mode `.so` swap at ~line 4366.
- Target-triple metadata: `linux-embedded-aarch64` (vs. existing `linux-aarch64`). Zig self-extractor picks up the new triple automatically if we teach its tables.

**Exit criterion:** `electrobun build --target=linux-embedded` (or equivalent via `electrobun.config.ts`) packages an app that bundles `libNativeWrapper_wpe.so`.

~50 LOC in CLI plus maybe 20 in the extractor triple registry.

### Phase 2.5 — Launch infrastructure + minimal kitchen test

Two pieces:

**(a) A minimal kitchen test app** (or a flag in the existing kitchen) with exactly one webview loading a tiny `index.html` that says "Hello, Electrobun". Config:

```ts
// kitchen/electrobun-embedded.config.ts
export default defineConfig({
  name: "hello-embedded",
  build: {
    linux: { embedded: true },
  },
  windows: [{
    initialURL: "views://hello/index.html",
    frame: { x: 0, y: 0, width: 1920, height: 480 },
    styleMask: { borderless: true, fullSize: true },
  }],
});
```

with `views/hello/index.html`:
```html
<!doctype html><meta charset=utf-8>
<body style="margin:0;display:flex;align-items:center;justify-content:center;
             background:#1e2a56;color:#fff;font:bold 90px sans-serif;height:100vh">
  Hello, Electrobun.
</body>
```

**(b) Launch harness:**
- A systemd user unit that `ExecStart=/path/to/app` on a free VT (via `agetty --autologin` or `openvt -c <N> -s`) with the right seat grabbing and restart policy.
- `BUILD.md` / `EMBEDDED.md` paragraph on how a user boots into a kiosk.
- SIGINT/SIGTERM handler that releases DRM master and `g_main_loop_quit`s cleanly.

**Exit criterion:** on the Pi, from a fresh boot:
```bash
cd hello-embedded && bun build.ts --target=linux-embedded
sudo ./dist/linux-embedded/hello-embedded   # or via the systemd unit
```
→ `<h1>Hello, Electrobun.</h1>` on the bar screen.

~100 LOC + docs.

### Total remaining

| Sub-phase | LOC | Kind of work | Risk |
|-----------|-----|--------------|------|
| 2.1 merge into `wpe_backend.cpp` | ~200 | class refactor of working code | **low** (code already runs) |
| 2.2 `nativeWrapper_wpe.cpp` | ~300 | FFI stubs + ~15 real routings | low (mostly mechanical) |
| 2.3 `build.ts` | ~50 | build system | medium (bun-specific quirks) |
| 2.4 CLI | ~50 | TypeScript + extractor triple | low |
| 2.5 launch + demo | ~100 + docs | packaging | low |
| **Total** | **~700 LOC + docs** | | |

No research questions left at this level. Every dependency is installed and working. The webcam feedback loop de-risks every iteration.

### After 2.5 lands

- Phase 3 (Dawn / `VK_KHR_display`) slots into the already-proven swapchain path (see Phase 0 step 4).
- Phase 4 (composite) deletes the CPU rotation from 2.1 and absorbs it into a shader.
- Phase 5 (V3DV reality check) is the V3DV + Dawn feature-set negotiation (`shaderSampledImageArrayDynamicIndexing = false` was flagged in Phase 0).
- Phase 6 extends 2.3's build step if pkg discovery changes.
- Phase 7 generalizes 2.5's launch infra.
- Phase 8 reorganizes commits for upstream PR (Commit 1–3 first as a refactor, then 2.1–2.5 as the functional chunk).

---

## 12. Next session — Phase 2.2–2.5 focus brief

**Picking up from commit `9a160c6e` on `kortexa/linux-wpe`.** Everything this session's work established is committed. Webcam feedback loop is confirmed working (`ffmpeg -f v4l2 -i /dev/video0 ...` on the Pi → readable JPG of the bar screen).

### State at start of next session

Already done:
- Phase 0 (5/5 validation steps; results in §5).
- Phase 1 Commits 1–3: `backend.h`, `Rect` migration in `nativeWrapper.cpp`, `abstract_view.h` with forward-declared `GtkWidget`.
- Phase 2 backend classes (`DrmDisplay`, `InputDispatcher`, `WpeBackend`, `WpeWebViewImpl`) all written, compiling clean against installed system packages.
- Phase 2 standalone test binaries (`drm_hello`, `wpe_hello`) proven end-to-end: HTML/CSS render, JS executes, touch→click works on a capacitive screen.
- `~/src/hello-embedded/` sibling project: real Electrobun layout (`src/bun/index.ts`, `src/main/{index.html,index.ts}`, `electrobun.config.ts`, `package.json`, `tsconfig.json`). Portable — `bun run build` on stock Electrobun produces the normal desktop binary.

Not done:
- **Phase 2.2** — `nativeWrapper_wpe.cpp` (parallel FFI surface).
- **Phase 2.3** — `package/build.ts` third Linux link step producing `libNativeWrapper_wpe.so`.
- **Phase 2.4** — `package/src/cli/index.ts` — `build.linux.embedded: true` flag + target selection.
- **Phase 2.5** — systemd unit, SIGINT/SIGTERM cleanup, `hello-embedded` working through `bun run build:embedded`.

### Concrete exit criterion for the next session

On the Pi, from a clean checkout:

```bash
cd ~/src/hello-embedded
bun install
bun run build:embedded
sudo openvt -c 2 -s -f -- ./dist/linux-embedded/hello-embedded
```

A webcam snap of the bar screen shows "Hello, Electrobun." with the frame counter ticking and the click counter incrementing when the touchscreen is pressed — **delivered by Electrobun's own build pipeline, not by a throwaway test binary**.

### Recommended order of attack

1. **Phase 2.2 first.** Read `nativeWrapper.cpp` to inventory the 75 `ELECTROBUN_EXPORT` functions. Partition into three groups in `nativeWrapper_wpe.cpp`:
   - **Real routings (~15):** `createGTKWindow` / `initWebview` / `loadURL*` / `loadHTML*` / `resizeWebview` / `startEventLoop` / `stopEventLoop` / `shutdownApplication` / `waitForShutdownComplete` / `setWindowTitle` / `showWindow` / `hideWindow` / `closeWindow` / `webviewGoBack` / `webviewGoForward` / `webviewReload` / `webviewCanGoBack` / `webviewCanGoForward`. These dispatch to `currentDisplayBackend()`, `currentWebviewBackend()`, or `AbstractView`'s virtual methods.
   - **Implement-as-degenerate (~10):** `getAllDisplays` / `getPrimaryDisplay` / `getWindowFrame` / `getWindowSize` (one fullscreen display always; report `DrmDisplay::logicalWidth()` × `logicalHeight()`).
   - **Noop stubs (~50):** clipboard, tray, file dialogs, global shortcuts, CEF-specific, menu bars, etc. Return 0/nullptr/false, log to stderr at WARN level on first call.

2. **Phase 2.3** — read `package/build.ts` ~line 1860 for the existing Linux branch that compiles `nativeWrapper.cpp` and links twice (GTK + CEF). Add a third link step:
   - Compile `nativeWrapper_wpe.cpp` + `wpe/wpe_backend.cpp` + `wpe/drm_display.cpp` + `wpe/input.cpp`.
   - `pkg-config --libs wpe-1.0 wpebackend-fdo-1.0 wpe-webkit-2.0 wayland-server libdrm libinput libudev glib-2.0 gio-unix-2.0`.
   - Output: `libNativeWrapper_wpe.so`.
   - Add to the dist copy at ~line 669.

3. **Phase 2.4** — `package/src/cli/index.ts`:
   - Add `bundleWPE` / `build.linux.embedded` field in Linux defaults (~lines 1497/1511/1518).
   - Binary selection at ~line 2739 picks `libNativeWrapper_wpe.so` when the flag is set.
   - Arbitrate with `bundleCEF` (mutually exclusive).
   - Target triple `linux-embedded-aarch64` registered for Zig self-extractor.

4. **Phase 2.5** — packaging + launch:
   - `hello-embedded/electrobun.config.ts` → add `build: { linux: { embedded: true } }`.
   - `bun run build:embedded` script already exists in `package.json`.
   - Systemd template (`hello-embedded.service`) — `ExecStart=/path/to/binary`, seat grabbing, `Restart=on-failure`.
   - SIGINT/SIGTERM handler in `WpeBackend::teardown()` that `drmDropMaster()` before `close(fd)`.
   - Document the "boot to kiosk" workflow in the hello-embedded README.

### Things that will probably trip us up

- **Bun FFI symbol resolution.** If any of the ~75 FFI functions is missing from `nativeWrapper_wpe.cpp`, `dlsym` fails at Bun startup and the app crashes before rendering anything. Easy fix; painful to diagnose at 2am. Inventory first, stub all 75, *then* write real implementations.
- **`isCEFAvailable()` must return `false`** on the embedded `.so` and never touch CEF code paths. Bun's FFI layer probably guards on this.
- **`views://` URL scheme handler.** Electrobun's views:// scheme is handled by the WebKit side (a URL scheme callback). Verify `WpeBackend::createWebview` wires this on WPE WebKit — likely `webkit_web_context_register_uri_scheme` on the shared `WebKitWebContext`, mirroring what `nativeWrapper.cpp` does for the GTK backend.
- **ASAR resolution.** The project's built assets land inside an ASAR archive (see `shared/asar.h`). The WPE path has to resolve `views://main/index.html` out of ASAR the same way the GTK path does. Reuse `nativeWrapper.cpp`'s ASAR code via `shared/` includes.
- **Startup order.** Bun calls `simpleTest()` → `startEventLoop()` (which blocks in `g_main_loop_run`). Windows and webviews are created *from* callbacks on the main loop, not before `startEventLoop`. `WpeBackend::createWindow` must be tolerant of being called before `runEventLoop`.
- **Clean shutdown.** `drmDropMaster` before `close(fd)`, or the next VT's login session can't take the display. Install a signal handler that sets a flag and exits from the main loop rather than calling `exit()` directly.

### Smoke-test plan once 2.2–2.4 are in

Before doing packaging (2.5), smoke-test the build system in isolation:
```bash
cd ~/src/electrobun/package
bun build.ts  # or the relevant build entry; verify libNativeWrapper_wpe.so appears
ldd dist/.../libNativeWrapper_wpe.so  # confirm it links
nm -D dist/.../libNativeWrapper_wpe.so | grep ELECTROBUN | head -5  # confirm FFI symbols exported
```

Then run `bun run build:embedded` on hello-embedded and inspect the produced `dist/linux-embedded/` tree. If the binary exists and links cleanly, the webcam-snap test is the final confirmation.

### Context the next session should skip re-establishing

- The 5 Phase 0 findings are locked; don't re-validate.
- The class structure in `wpe/` is final for Phase 2; don't refactor for taste.
- `hello-embedded` project shape is final; just wire `build: { linux: { embedded: true } }` when Phase 2.4 is ready.
- Rotation strategy: CPU blit in Phase 2 (TODO(phase4)), shader in Phase 4. Don't second-guess.


## 13. Session results (2026-04-25 — Phase 2.2–2.5 + architectural fix)

Session went past the §12 brief and hit a deeper rendering-pipeline bug. End state: WPE renders user HTML on the bar via the kortexa Pi (visually confirmed by the user — "blue background! Hello Electrobun!"), with one open Bun-runtime issue blocking full launcher-driven end-to-end.

### Done

- **Phase 2.2** — `package/src/native/linux/nativeWrapper_wpe.cpp` (~480 lines): full parallel FFI surface, all 115 `ELECTROBUN_EXPORT` symbols from `nativeWrapper.cpp` plus 8 the original `nativeWrapper.cpp` is missing that Bun's FFI binding expects (`webviewSetTransparent` / `webviewSetPassthrough` / `webviewSetHidden`, `clipboardReadImage`, `setJSUtils`, `showItemInFolder`, `showNotification`, `testFFI2`). Real routings dispatch to `currentDisplayBackend()` / `currentWebviewBackend()` / `AbstractView` virtuals; degenerate routings report a single fullscreen display; tray/clipboard/menu/global-shortcut/session families are silent stubs (one-shot `[wpe] unimplemented FFI on embedded target: NAME` warning on first call).

- **Phase 2.3** — `package/build.ts`:
  - third Linux link step gated on `pkg-config --exists wpe-1.0 wpebackend-fdo-1.0 wpe-webkit-2.0 wayland-server libdrm libinput libudev glib-2.0 gio-unix-2.0`. Outputs `src/native/build/libNativeWrapper_wpe.so`, links `asarLib` + `-Wl,-rpath,$ORIGIN` (so deployed `.so` finds sibling `libasar.so`).
  - GTK/CEF link steps now also gated on `pkg-config --exists webkit2gtk-4.1 gtk+-3.0`. An embedded-only Pi (no `libgtk-3-dev`) can produce just the WPE `.so` without bombing the build.
  - Zig 0.13 aarch64-linux build-runner crash workaround: `buildLauncher` / `buildSelfExtractor` try `zig build`, catch the `unreachable code; Panicked during a panic` failure, fall back to `zig build-exe main.zig --name X -lc -target $TGT -O $OPT` and move output into `zig-out/bin/`.
  - Fixed `src/extractor/main.zig` line 1060: `makeDirPath` → `makePath` (Zig stdlib API drift; pre-existing bug, not touched on macOS path).

- **Phase 2.4** — `package/src/cli/index.ts`:
  - `NATIVE_WRAPPER_LINUX_WPE` added to `getPlatformPaths`.
  - `build.linux.embedded: false` field added to defaults (with type comment).
  - Linux native-wrapper selection (build path ~line 2737, dev-mode path ~line 4361) picks `libNativeWrapper_wpe.so` when `embedded` is true, throws on `embedded && bundleCEF`. Both edits live inside existing `targetOS === "linux"` / `OS === "linux"` branches — macOS/Win paths untouched.
  - Pre-existing `src/bun/webGPU.ts` bug fixed: missing `dirname` import. Without this any non-WGPU Bun-runtime app crashes with `ReferenceError: dirname is not defined` on startup, before WPE ever loads.

- **Phase 2.5** — `hello-embedded` + shutdown hygiene:
  - `electrobun.config.ts`: `build: { linux: { embedded: true } }` and `copy: { "src/main/index.html": "views/main/index.html" }`. The `copy` directive is required — without it the bundled `app.asar` only contains the bundled JS (`views/main/index.js`) and the views:// scheme handler returns 404 for `index.html`. Confirmed by `zig-asar list` of the produced archive.
  - `package.json`: dropped the bogus `build:embedded` script (used a non-existent `--target=linux-embedded` flag); plain `bun run build` does the right thing because the host arch decides target.
  - `wpe/drm_display.cpp`: `~Impl()` calls `drmDropMaster(fd)` before `close(fd)` so the next VT's session can take the display on clean exit.
  - `wpe/wpe_backend.cpp`: SIGINT/SIGTERM handlers via `g_unix_signal_add` (registered in `runEventLoop` at first entry); they call `g_main_loop_quit` which lets `~WpeBackend()` run teardown cleanly.
  - `views://` scheme handler wired in `WpeBackend::initWpeOnce` — reads from `app.asar` via libasar, falls back to flat `Resources/app/views/...`. Mirrors `nativeWrapper.cpp`'s GTK handler (file_path lookup, mime detection, `webkit_uri_scheme_request_finish`).

### The architectural finding (the real bug behind §12)

The §12 brief assumed the dispatch path matched GTK's `dispatch_sync_main` pattern would be sufficient. It is not. Two distinct issues stacked:

**1. Worker-thread → main-thread dispatch is mandatory.** Bun's FFI calls our exports from a `bun:worker`-spawned pthread, not the main thread. WebKit-WPE traps with `brk #0x3e8` (compiler-emitted `__builtin_trap`) when WebView ops run on a thread other than the one running `g_main_loop_run`. We added `dispatchSyncMain<Fn>(Fn fn)` (uses `g_idle_add_full` + `std::promise`) and member-function bodies (`createWindowOnMain`, `createWebviewOnMain`) — running both on the main thread fixed the EXIT-0 / SIGABRT crashes.

**2. WPE-FDO requires its WPE setup to happen BEFORE `g_main_loop_run` starts.** Even with calls correctly marshalled to the main thread, doing `wpe_view_backend_exportable_fdo_create` + `webkit_web_view_new` from inside an idle callback while the loop is already iterating leaves the WebProcess unable to export frames — `onExportShm` is never invoked even though the wayland protocol exchange (visible via `WAYLAND_DEBUG=1`) is byte-identical to `wpe_hello`'s. Side-by-side proof:
- Standard dispatched path → 0 `onExportShm` calls in 10s.
- `FORCE_WPE_HELLO` branch (full setup pre-loop, mimicking `wpe_hello`'s `main()`) → frames flow, page renders.

The likely mechanism is that WPE-FDO attaches its wayland-server `GSource` via `g_source_attach(m_source, g_main_context_get_thread_default())` (verified in `WPEBackend-fdo/src/ws.cpp` line 464). When attached during a running-loop iteration, something about the thread-default state at that moment leaves the source detached from the iterating context.

**The fix:** `WpeBackend::primeWpeView()` runs in `runEventLoop` before `g_main_loop_run`, doing the full bring-up:
- `DrmDisplay` init
- `InputDispatcher` start (gated on `!ELECTROBUN_NO_INPUT` env var)
- `wpe_view_backend_exportable_fdo_create` + `webkit_web_view_new`
- `webkit_web_view_load_html` with a placeholder gradient page (so the user sees something while their URL loads)
- `primaryView_` assignment

`createWindow()` and `createWebview()` become thin shims that bind the user's `webviewId` onto the existing primed view and (for `createWebview`) dispatch `webkit_web_view_load_uri(user_url)` to the main thread. Single webview total — the prior dual-webview state where Worker-side `createWebview` made a second one is gone.

### Webcam workflow that worked

`sudo openvt -s -f -- /tmp/run_X.sh` switches VTs synchronously, so DRM scanout from our process is the active output. Without `openvt`, DRM master can be acquired but the kernel doesn't route scanout to the panel because tty1 (desktop) owns the active VT. ffmpeg capture: `ffmpeg -y -f v4l2 -input_format mjpeg -video_size 1280x720 -i /dev/video0 -frames:v 1 /tmp/snap.jpg`. The `-input_format mjpeg -video_size 1280x720` part is non-negotiable for the eMeet C950 — without it `ioctl(VIDIOC_QBUF): Bad file descriptor`.

### Still broken (next session's target)

Bun launcher path (`./launcher` → `./bun ../Resources/main.js` → Worker spawns hello-embedded's `bun/index.ts` → `new BrowserWindow` → FFI to our `createWindow`/`createWebview`) crashes with **`panic(main thread): Bus error at address 0x11CA76B`** in Bun's text segment, AFTER:
- views://main/index.html is fetched (one log line: `[wpe views://] serving main/index.html (2125 bytes, text/html)`)
- BEFORE views://main/index.js is fetched
- BEFORE WPE produces any frames

The crash address differs between runs (`0x11CA76B` vs `0x7FFF00000006`) so it's not a single fixed instruction. SIGTRAP exit (`Aborted, signal 5`) is Bun's panic handler; the original SIGSEGV/SIGBUS happened first. Bun's stripped binary makes the stack untraceable with available symbols.

The standalone harness (`/tmp/wpe_harness.cpp`) using the same libNativeWrapper.so works fine — process stays alive, frames flow, page renders. So the bug is purely Bun runtime + our FFI usage, NOT our libNativeWrapper.

### Files changed (uncommitted)

```
M  package/build.ts                                  (Zig fallback + GTK gating + WPE link)
M  package/src/bun/proc/native.ts                    (1-line console.error on FFI dlopen failure)
M  package/src/bun/webGPU.ts                         (1-line: import { dirname })
M  package/src/cli/index.ts                          (build.linux.embedded flag)
M  package/src/extractor/main.zig                    (1-char: makeDirPath → makePath)
M  package/src/native/linux/wpe/drm_display.cpp      (drmDropMaster on shutdown)
M  package/src/native/linux/wpe/wpe_backend.cpp      (primeWpeView + dispatchSyncMain + signals + views://)
?? package/src/native/linux/nativeWrapper_wpe.cpp    (new, ~480 lines)
M  hello-embedded/electrobun.config.ts               (linux.embedded + copy directive)
M  hello-embedded/package.json                       (removed bogus build:embedded script)
```

Diagnostic env vars left in `wpe_backend.cpp` (cheap, not load-bearing — clean up when the Bun crash is fixed):
- `ELECTROBUN_NO_INPUT=1` — skip `InputDispatcher` (libinput/udev fd watch).
- `ELECTROBUN_SKIP_USER_URL=1` — `createWebview` doesn't `loadURL` user's URL; primed-page stays.
- `ELECTROBUN_FORCE_WPE_HELLO=1` — second exportable+view alongside primed one (was the proof-of-concept; can delete).
- `ELECTROBUN_ROTATE=0|90|180|270` — pick blit rotation. Default 270 (CCW90) for portrait bar panel.

### Standalone harness for non-Bun testing

`/tmp/wpe_harness.cpp` — a 60-line C++ program that `dlopen`s `libNativeWrapper.so`, calls `startEventLoop` on the main thread, and from a worker pthread calls `createGTKWindow` + `initWebview`. Builds with: `g++ -std=c++17 /tmp/wpe_harness.cpp -lpthread -ldl -o /tmp/wpe_harness`. Use this to isolate WPE-side issues from Bun-runtime issues during further debugging.

## 14. Next session — Bun launcher bus-error

### One-line state at start

Architecture works (user visually confirmed "blue background, Hello Electrobun" on the bar). Standalone harness renders. Bun launcher path crashes mid-load with `panic(main thread): Bus error`.

### Concrete exit criterion

`sudo openvt -s -f -- /home/pi/src/hello-embedded/build/dev-linux-arm64/HelloElectrobun-dev/bin/launcher` runs for 30+ seconds without crashing, the bar shows hello-embedded's actual `index.html` (gradient + "Hello, Electrobun." + "Frame N · Clicks 0" updating every animation frame), JS `setInterval` heartbeat (`[hello-embedded] frames=N clicks=0`) appears in stdout. Touchscreen click increments the click counter.

### Recommended order of attack

1. **First read these landmines**, in order, from §13:
   - The two-bug stack (worker-thread dispatch + pre-loop WPE setup) is solved; don't re-litigate.
   - The diagnostic env vars are scaffolding; use them but plan to delete before merging.
   - Bun crash is BUN-internal, not in our code (harness same code, no crash).

2. **Decode Bun's crash report URL.** The launcher prints `https://bun.report/1.3.11/...` on panic. The encoded payload contains the exact stack frames. Either fetch the URL (its server decodes it) or run `bun --cli` against the encoded part. This narrows the crash to a specific Bun source line.

3. **Hypothesis worth testing first**: Bun's main thread, after our `lib.symbols.startEventLoop()` returns, runs `lib.symbols.forceExit(0)`. But the bus error happens BEFORE `startEventLoop` returns — the loop is still in `g_main_loop_run`. So Bun's main thread isn't in JS land when the crash happens. The crash address is in Bun's *text segment*. So Bun's libuv/event loop is doing something on a thread that's NOT the JS main thread (a worker, a watchdog, or a signal handler). Two specific suspects:
   - **Bun's signal-handling thread**: Bun installs SIGCHLD/SIGPIPE handlers. WPEWebProcess subprocess churn (fork→bwrap→xdg-dbus-proxy→bwrap→WPEWebProcess) generates many SIGCHLDs. Try running the launcher with `BUN_DEBUG_QUIET_LOGS=1` and/or `--no-deprecation` to see if quieting Bun's diagnostics changes timing.
   - **libuv ↔ glib main loop conflict**: Bun's main thread is parked in our `g_main_loop_run`, but libuv elsewhere might assume the main loop is libuv's. Try having our `startEventLoop` integrate with libuv via `uv_default_loop()` instead of running a separate glib loop on the main thread — long-term that's the right architecture for Electrobun on Linux anyway.

4. **Quick diagnostic**: in `package/src/launcher/main.ts`, add a `setInterval(() => {}, 100)` BEFORE `lib.symbols.startEventLoop(...)`. If the crash goes away, Bun expects regular libuv ticks even when an FFI call is blocking; if not, the crash is unrelated to libuv idleness.

5. **If the above doesn't crack it**, the cleanest fix is probably to mark `startEventLoop` as `threadsafe: true` in the Bun FFI declaration (in `src/launcher/main.ts`'s `dlopen` call). That tells Bun to run the FFI on a thread-pool thread instead of the main JS thread, which keeps the JS main thread free to run libuv. The trade-off: we need our `g_main_loop_run` to run somewhere, and "thread-pool thread" might re-trigger the WebKit cross-thread trap. Mitigate by storing the FFI thread's tid as the "main thread" in our `g_mainThreadTid` and making sure ALL WebKit/WPE-FDO calls go through `dispatchSyncMain` to it.

### Things that will probably trip us up

- **Don't rebuild kitchen.** The user's CLAUDE.md says "Never try to run the project; it's already running." That refers to kitchen. We're working in hello-embedded; that's fine to rebuild freely.
- **Don't undo §13's architectural fix.** `primeWpeView` pre-loop is correct and load-bearing. If a refactor "looks cleaner" by moving WPE setup into `createWindow`/`createWebview`, IT WILL BREAK RENDERING. The two-bug stack is real and verified.
- **Don't try to commit.** Per CLAUDE.md, the user does the commit step.
- **Webcam needs `-input_format mjpeg -video_size 1280x720`.** Default v4l2 negotiation fails on the eMeet C950.
- **Run via `sudo openvt -s -f --` for visual tests.** Without it DRM master may be acquired but scanout doesn't reach the panel.

### Smoke test once the crash is fixed

```bash
cd ~/src/hello-embedded
bun run build
sudo openvt -s -f -- ./build/dev-linux-arm64/HelloElectrobun-dev/bin/launcher
# (in another shell)
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 1280x720 -i /dev/video0 -frames:v 1 /tmp/check.jpg
# Read /tmp/check.jpg — should show "Hello, Electrobun." + "Frame N · Clicks 0"
# Touch the bar; verify click counter increments
sudo chvt 1  # restore desktop
```

### Things settled — don't re-litigate

- The §13 architecture (primeWpeView pre-loop, single primed view) is final. Bun bug is BUN's bug.
- `nativeWrapper_wpe.cpp` symbol coverage (123 exports) is final. If Bun complains about a missing symbol, it's a Bun upgrade adding a new FFI; add it to `nativeWrapper_wpe.cpp` AND the original `nativeWrapper.cpp`.
- `dispatchSyncMain` is the canonical worker→main marshal; don't reinvent.
- `views://` scheme handler is wired and working (we saw it serve `index.html` from app.asar).
- Webcam framing on the eMeet captures only the upper portion of the bar — text appearing in the upper-left is normal (don't assume "wrong rotation" from camera-only evidence; ask the user to eyeball if in doubt).

## 15. Session results (2026-04-25, follow-up — actual root cause: dangling stack pointer)

End state: **hello-embedded renders end-to-end through the real Bun launcher.** Navy-blue gradient, "Hello, Electrobun.", frame counter ticking, the "Press me" button is live, touchscreen → click counter increments. Visually confirmed:

```
sudo openvt -s -f -- ./build/dev-linux-arm64/HelloElectrobun-dev/bin/launcher
# log: panic count: 0; onExportShm count: 26; frames=1651 clicks=2; EXIT=124 (clean timeout)
```

### The bug, in one line

In `WpeBackend::primeWpeView()`:
```cpp
// BEFORE (crashes):
wpe_view_backend_exportable_fdo_client client = {};
client.export_shm_buffer = &WpeBackend::onExportShmStatic;
auto* exportable = wpe_view_backend_exportable_fdo_create(&client, this, ...);

// AFTER (works):
static wpe_view_backend_exportable_fdo_client client = {};
client.export_shm_buffer = &WpeBackend::onExportShmStatic;
auto* exportable = wpe_view_backend_exportable_fdo_create(&client, this, ...);
```

`wpe_view_backend_exportable_fdo_create` keeps the **pointer** to the client struct, not a copy. The previous code put `client` on `primeWpeView`'s stack frame; once `primeWpeView` returned to `runEventLoop`, that stack memory was reused. Then `g_main_loop_run` started iterating the WPE-FDO wayland-server source, which dereferenced the freed bytes inside the (now-invalid) client struct and called the corrupted function pointer.

`wpe_hello.cpp`'s standalone test never returns from `main()` while the loop runs, so its identical local-`client` pattern is fine — its stack frame stays alive. Our class-based `primeWpeView` returned before the loop got to it. That is the entire delta.

### Why every prior hypothesis was wrong

- **§14's "Bun's libuv conflicts with glib"** — wrong. The crash is in C-level memory corruption from a foreign library, nothing to do with libuv.
- **§14's "WTF::jscSignalHandler hijack"** — partially right (bun.report did correctly identify Bun's static `WTF::jscSignalHandler` as the top frame), but it was the *consequence*, not the cause. The handler was correctly invoked when a Bun thread took the SIGBUS triggered by the corrupted-pointer indirect call; it then panicked because the bytecode-level fault wasn't a JSC trap it could recover from. `libsig_passthrough.so` "saving" Bun's handlers across the libwpewebkit dlopen verified Bun's handlers were never clobbered — the handlers ran fine, the corruption upstream was the real problem.
- **"Run WPE on a dedicated pthread"** — didn't help because the corruption is in the wayland-server source's iteration, which runs on whatever thread is in `g_main_loop_run`; moving that thread didn't move the bug. (Reverted.)
- **"openvt vs `chvt 7` makes a difference"** — the apparent difference was timing. With `chvt 7` we manually triggered slower VT switches and Bun got further before the corruption hit; with `openvt -s -f` the WebProcess started faster and the corrupted iteration hit sooner. Same bug either way.
- **"System-wide WPE breakage" (mid-session panic)** — webcam was lying. The dark-blue gradient I kept seeing was *room reflection* on the panel, not rendered content; the panel was actually BLACK. Once the user eyeballed it directly and confirmed, the bisect proceeded honestly. `cog`'s "white" output is its known stride bug from §5 step 2 — also not a regression. The stack `wpe_hello` binary worked the whole time once we tested it directly.

### What in the working tree is the fix

One file, one keyword:

- `package/src/native/linux/wpe/wpe_backend.cpp` — `static` on the `wpe_view_backend_exportable_fdo_client client` declaration in `primeWpeView`. (Same fix would also be needed in the `ELECTROBUN_FORCE_WPE_HELLO` diag branch in `runEventLoop`, but that branch is already marked dead.)

Plus the existing §13 architectural fixes that were already correct (primeWpeView pre-loop, dispatchSyncMain for worker→main marshal, single primed view, views:// scheme handler, `drmDropMaster` on shutdown, glib SIGINT/SIGTERM via `g_unix_signal_add`).

### Diagnostic scaffolding still in the tree

These were useful during the bisect but are NOT the fix; consider for cleanup before merge:

- `package/src/native/linux/wpe/sig_passthrough.cpp` + `libsig_passthrough.so` — proves Bun's signal handlers survive the libwpewebkit dlopen. Useful diagnostic for any future signal-handler-related WPE/Bun interaction question. Not wired into the source `main.ts` (the `Resources/main.js` patch was reverted; clean main.js is bundled now).
- `package/src/native/linux/wpe/wpe_helper.cpp` — out-of-process WebKit driver, in case we ever want process isolation for real (e.g. to embed multiple Electrobun apps in one parent). Currently unused; the in-process path works.
- `ELECTROBUN_NO_PLACEHOLDER` env var in `wpe_backend.cpp` — handy bisect knob; could be deleted, but it's gated and harmless.
- The diagnostic SIGBUS chain handler in `sig_passthrough.cpp` (`electrobun_install_diag_handlers`) — leave it in `sig_passthrough.cpp` as a debugging-only helper; not invoked by the launcher anymore.

### What works at session end

- `sudo openvt -s -f -- ./launcher` from `hello-embedded/build/.../bin/` — renders hello-embedded, frame counter ticks, `Press me` button responds to touch, click counter increments. No panics. No openvt-specific crash. No `chvt`-vs-`openvt` distinction.
- `sudo openvt -s -f -- ./wpe_helper "views://main/index.html"` — same behavior in the standalone helper.
- `sudo openvt -s -f -- /tmp/wpe_harness` and `wpe_hello` — both still work as in §5/§13 (Phase 2 standalone validation).
- Webcam capture continues to be misleading on the eMeet C950 — reflections look like rendered gradients. Always have the user eyeball the panel for visual confirmation.

### Open follow-ups (none load-bearing)

- Re-run the wpe_hello "FORCE_WPE_HELLO" diag branch with the same `static` fix — currently the branch's stack-local client would hit the same UAF if anyone enabled the env var. Easy two-line fix; out of session scope.
- The §13 dispatchSyncMain pattern still applies for worker-thread FFI calls (createWindow / createWebview from Bun's worker thread). Was tested live in the final session run: hello-embedded's `BrowserWindow({ url })` from `src/bun/index.ts` flows through the worker → main dispatch → primeWpeView's primed webview → views:// scheme handler → `app.asar` → rendered HTML. All good.
- `package/build.ts`'s WPE link step at line ~2050 currently uses `-std=c++20`; rebuilding with `-std=c++17 -O2 -g` (matching the standalone tests' Makefile) gave identical behavior so the language standard isn't load-bearing.
- The Bun version doesn't matter (1.2.5, 1.3.11, 1.3.13 all behave identically once the C++ side is fixed). Stick with the vendored 1.3.11 unless there's a JS-side reason to bump.

## 16. Next session — wire navigation policy + load events for full GTK parity

Goal: bring `linux-embedded`'s navigation event surface up to parity with the GTK backend. Hello-embedded already renders end-to-end (§15); this is the work that turns "kiosk demo" into "every-app-just-works."

The good news: WebKit-WPE uses the **identical** `webkit_*` C API for navigation as WebKit-GTK. Same signal names, same `WebKitNavigationAction` / `WebKitNavigationPolicyDecision` / `WebKitURIRequest` / `WebKitLoadEvent` types. This is a straight port from `nativeWrapper.cpp`, not a re-design.

Estimated effort: ~3 hours focused, mostly mechanical.

### Reference code (read these first)

| Function | File | Lines |
|---|---|---|
| `onDecidePolicy` (the meat) | `package/src/native/linux/nativeWrapper.cpp` | ~2700-2820 |
| `onLoadChanged` | `package/src/native/linux/nativeWrapper.cpp` | 2821-2844 |
| `onLoadFailed` | `package/src/native/linux/nativeWrapper.cpp` | 2846-2853 |
| signal-connect site | `package/src/native/linux/nativeWrapper.cpp` | 2299-2304 |
| `lastNavigationWasBlocked` member | `package/src/native/linux/nativeWrapper.cpp` | 2177 |
| Callback typedefs | `package/src/native/shared/callbacks.h` | 16-17 |
| `WebviewSpec.navigationHandler` / `eventBridgeHandler` | `package/src/native/linux/backend.h` | 60-61 |
| `AbstractView::navigationRules` + `setNavigationRulesFromJSON` + `isNavigationAllowed` (or whatever it's called) | `package/src/native/linux/abstract_view.h` | 55-100 |

The two callback types:

```cpp
// shared/callbacks.h:16-17
typedef uint32_t (*DecideNavigationCallback)(uint32_t webviewId, const char* url);
typedef void     (*WebviewEventHandler)    (uint32_t webviewId, const char* type, const char* url);
```

`DecideNavigationCallback` returns `1` = allow, `0` = block.

### What to add in `package/src/native/linux/wpe/wpe_backend.cpp`

1. **`WpeWebViewImpl` member additions** (~5 lines around line 183):
   ```cpp
   DecideNavigationCallback navigationCallback_ = nullptr;  // copied from spec
   WebviewEventHandler      eventHandler_       = nullptr;  // copied from spec.eventBridgeHandler
   bool                     lastNavigationWasBlocked_ = false;
   ```

2. **Plumb them through `WpeBackend::createWebview`** (line ~352). Currently it does
   ```cpp
   primaryView_->webviewId = spec.webviewId;
   ```
   Add:
   ```cpp
   primaryView_->navigationCallback_ = (DecideNavigationCallback)spec.navigationHandler;
   primaryView_->eventHandler_       = (WebviewEventHandler)spec.eventBridgeHandler;
   ```

3. **Signal connections in `primeWpeView`** — right after `webkit_settings_set_enable_write_console_messages_to_stdout`, before the placeholder `load_html`:
   ```cpp
   g_signal_connect(webView, "decide-policy", G_CALLBACK(&WpeWebViewImpl::onDecidePolicyStatic), nullptr);
   g_signal_connect(webView, "load-changed",  G_CALLBACK(&WpeWebViewImpl::onLoadChangedStatic),  nullptr);
   g_signal_connect(webView, "load-failed",   G_CALLBACK(&WpeWebViewImpl::onLoadFailedStatic),   nullptr);
   ```
   Pass `nullptr` as user-data because at primeWpeView time we don't have the WpeWebViewImpl yet (we make it three lines later). Inside the static thunks, look up `primaryView_` via `wpeBackendInstance()` — there's only one webview on this target, so this is fine.

4. **The three static thunks + member impls** — port verbatim from `nativeWrapper.cpp`. The `onDecidePolicy` body has one GTK-specific bit to drop: the ctrl+click debounce uses `gtk_get_current_event_state()` to read the modifier keys. WPE doesn't have a modifier-key state on a kiosk panel; just delete that block (or guard it on `#ifdef HAVE_GTK`). Keep:
   - URL extraction via `webkit_navigation_action_get_request` / `webkit_uri_request_get_uri`
   - `AbstractView::isNavigationAllowed(url)` (or whatever the exact name is — confirm from `abstract_view.h:90+`)
   - Setting `lastNavigationWasBlocked_` based on the outcome
   - Calling `eventHandler_(webviewId, "will-navigate", uri)` with the JSON-encoded "allowed" payload (mirror exactly what GTK does — Bun-side parses `eventData` as JSON)
   - Returning `TRUE` to cancel, `FALSE` to allow

5. **Free string conventions**: GTK code calls `eventHandler_(impl->webviewId, strdup("did-navigate"), strdup(url))`. The Bun-side JSCallback assumes the strings are owned-and-freed by the callback site. WTF that's not symmetric — copy the GTK semantics exactly so the Bun side's `free()` call lines up.

### Smoke test in `hello-embedded`

Add to `src/main/index.html` (or a new `index.js`):
```html
<a href="https://example.com/">link to example</a>
<button onclick="window.location='views://main/index.html#refresh'">refresh self</button>
```

In `src/bun/index.ts`, listen for the events:
```ts
const win = new BrowserWindow({ url: "views://main/index.html", ... });
win.webview.on("will-navigate", (e) => console.log("[bun] will-navigate", e.url, "allowed=", e.allowed));
win.webview.on("did-navigate",  (e) => console.log("[bun] did-navigate",  e.url));
win.webview.on("load-failed",   (e) => console.log("[bun] load-failed",   e.url));
```

Build, run via `sudo openvt -s -f -- ./launcher`, touch the link, eyeball the bar for the navigation, and grep the launcher log for `[bun] will-navigate`. If those events fire and the URL changes, navigation parity is done.

### Bonus (only if time allows)

- **`load-failed-with-tls-errors`** signal — GTK has a separate handler for SSL errors that emits `load-failed-tls`. Trivial port (~10 lines).
- **Permission requests** (`onPermissionRequest` in GTK, lines ~2890+) — GTK pops a GtkDialog for camera/mic permission. On a bare-DRM kiosk there's no dialog system; either auto-allow (kiosk semantics) or auto-deny with a TODO. ~20 lines.
- Delete the `// Navigation action / script message helpers (legacy — unused on WPE)` block at `nativeWrapper_wpe.cpp:383+` — those were placeholders for this work and are no longer "unused on WPE."

### What NOT to do

- Do NOT touch `wpe_helper.cpp` or `sig_passthrough.cpp` — they're diagnostic-only per §15, kept for future debugging. Navigation work goes only in `wpe_backend.cpp`.
- Do NOT re-litigate the threading model. `dispatchSyncMain` is the canonical worker→main marshal; the signal handlers fire on the WPE main thread directly so they don't need it. The JSCallback they invoke is `threadsafe: true` on the Bun side, which handles the marshal back to JS.
- Do NOT add the `static` keyword treatment to other locals in `primeWpeView` "to be safe." The `wpe_view_backend_exportable_fdo_client` was the only one WPE-FDO holds by reference; everything else is fine on the stack.
- Do NOT skip the smoke test. The whole point of full parity is "Bun apps that work on macOS work here too." A regression test you can re-run is what proves that.

## 17. Session results (2026-04-25, follow-up — §16 navigation parity port)

End state: **navigation events flow C++ → Bun → JS.** hello-embedded's home page round-trips with `views://main/page2.html` and back, plus `views://main/index.html?t=…` self-reload, and every tap surfaces a matching `[bun] will-navigate {"url":…,"allowed":true}` + `[bun] did-navigate <url>` pair in the launcher log. The JSON payload + ownership semantics match GTK's `WebKitWebViewImpl` exactly. Visually confirmed on the bar panel by the user; logs captured by tee'ing the launcher's stdout to `/tmp/hello-embedded-$$.log`.

Sample log from a clean run:

```
[bun] will-navigate {"url":"about:blank","allowed":true}
[bun] will-navigate {"url":"views://main/index.html","allowed":true}
[bun] did-navigate  views://main/index.html
[bun] will-navigate {"url":"views://main/page2.html","allowed":true}    ← tap "page 2"
[bun] did-navigate  views://main/page2.html
[bun] will-navigate {"url":"views://main/index.html","allowed":true}    ← tap "back to home"
[bun] did-navigate  views://main/index.html
[bun] will-navigate {"url":"views://main/index.html?t=1777130637210","allowed":true}  ← self-reload
[bun] did-navigate  views://main/index.html?t=1777130637210
```

### One brief-was-wrong correction to §16

§16 step 2 said:
```cpp
primaryView_->eventHandler_ = (WebviewEventHandler)spec.eventBridgeHandler;
```

That is incorrect. `WebviewSpec::eventBridgeHandler` is typed `void*` but its semantics are a `HandlePostMessage` (`(uint32_t, const char*) → void`) — it's the JSON-bridge for events emitted from JS preload scripts. Calling it as a `WebviewEventHandler` (`(uint32_t, const char*, const char*) → void`) is UB.

The actual `WebviewEventHandler` slot that GTK uses (`webviewEventHandler` in `nativeWrapper.cpp:6537`) was being **discarded** in the WPE seam — `nativeWrapper_wpe.cpp:278` had `(void)webviewEventHandler;`. So `WebviewSpec` had no field to receive it.

Fix: added a `void* webviewEventHandler = nullptr;` field to `WebviewSpec` (`backend.h`), populated it in `nativeWrapper_wpe.cpp::initWebview`, consumed it in `wpe_backend.cpp::createWebview`. Both `eventBridgeHandler` and `webviewEventHandler` now ride along; semantics distinguish via the inline comments on the WebviewSpec fields.

### Files changed in the working tree

- `package/src/native/linux/backend.h` — added `void* webviewEventHandler` field to `WebviewSpec` between `navigationHandler` and `eventBridgeHandler`.
- `package/src/native/linux/nativeWrapper_wpe.cpp` — populated `spec.webviewEventHandler`, removed the `(void)webviewEventHandler;` discard.
- `package/src/native/linux/wpe/wpe_backend.cpp`:
  - included `../../shared/callbacks.h` for the callback typedefs
  - added `navigationCallback_` / `eventHandler_` / `lastNavigationWasBlocked_` members + the three handler member fns (`onDecidePolicy` / `onLoadChanged` / `onLoadFailed`) on `WpeWebViewImpl`, ported verbatim from GTK with the GDK-modifier-state ctrl+click block dropped (kiosk panel has no keyboard modifiers)
  - added the three static thunks (`onDecidePolicyStatic` etc.) on `WpeBackend`, hopping from `WpeBackend* user_data` → `primaryView_` → impl method (cleaner than §16's nullptr+singleton-lookup proposal; same semantics since WpeBackend is a singleton)
  - wired `g_signal_connect(... decide-policy / load-changed / load-failed ...)` in `primeWpeView`, before the placeholder `load_html` so the thunks see all loads (including placeholder); both thunks early-return while `primaryView_` / `eventHandler_` are still null, so no spurious events fire before `createWebview`
  - in `createWebview`, copied `spec.navigationHandler` and `spec.webviewEventHandler` onto the impl
  - **side-fix:** `handleViewsURIScheme` now strips `?query` and `#fragment` from the path before resolving against ASAR/disk, so e.g. `views://main/index.html?t=12345` serves `main/index.html` instead of 404'ing. The GTK handler in `nativeWrapper.cpp:5055` has the same omission and will hit the same bug; tracked as a follow-up below.

### hello-embedded smoke-test wiring (also in the tree)

Test app at `/home/pi/src/hello-embedded` got these adjustments — kept around as the regression test for future sessions:

- `electrobun.config.ts` — added `"src/main/page2.html": "views/main/page2.html"` to `build.copy` (the map is explicit, not glob — new view files won't ship until added here).
- `src/main/index.html` — orange-on-charcoal gradient (`#cc3300` → `#ff8a00`) so the bar panel's webcam reflection (which reads black-TTY as washed navy) doesn't masquerade as rendered content. Bumped touch-target sizing to `clamp(28px, 5.5vmin, 48px)` font + `0.7em 1.6em` padding (~80-100px tall after padding) for the 7" panel's fat-finger ergonomics. Added two test pills: "tap: page 2" (→ `views://main/page2.html`) and "tap: self-reload" (→ `views://main/index.html?t=Date.now()`).
- `src/main/page2.html` — second internal page with a "tap: back to home" pill, charcoal-on-orange (inverted from home page) so the visual flip on tap is unambiguous.
- `src/bun/index.ts` — listens for `will-navigate` and `did-navigate` on the BrowserWindow's webview and console.log's `event.data.detail`. (Note: the event shape is `{data: {detail: string}}`, not `{detail: string}`; first attempt logged `undefined`.)

### Workflow gotchas surfaced this session

- **Don't `timeout sudo openvt -- launcher`**: `sudo` doesn't forward signals to descendants by default, so SIGTERM dies at sudo and the launcher tree (bash + launcher + bun) becomes orphans of init. `bun` keeps DRM master locked → next run hits `drmModeSetCrtc: Permission denied`. Use background `sudo openvt -w` + an explicit `sudo pkill -TERM -f "HelloElectrobun-dev/bin/launcher"` after the sleep window. Pattern is in `~/.claude/projects/.../memory/feedback_pi_launcher_kill.md`.
- **Don't `rm /tmp/hello-embedded.log` without sudo if a prior `sudo … tee` created it**: the file is root-owned, the `rm` fails with EPERM, and an `&& sudo openvt …` chain short-circuits silently — launcher never starts and you spend several minutes wondering why the log is empty. Use a `$$`-suffixed unique log path or `sudo rm -f` first.
- **Webcam still lying as predicted**: the example.com link (first version of the smoke test) navigated successfully, the user saw a white page on the panel, and we lost a few minutes wondering whether to chase it. Switched the test to all-internal navigation (`views://main/page2.html` ↔ `views://main/index.html`) so the round-trip is reversible from the panel itself with no internet dependency.

### Open follow-ups (none load-bearing)

- **GTK side has the same query-string bug**. `nativeWrapper.cpp::handleViewsURIScheme` (~line 5055) does `fullPath = uri + 8;` with no `?` / `#` strip, identical to the WPE bug fixed this session. Mirroring the WPE fix is ~5 lines. Out of session scope.
- **`navigationCallback_` is wired but never called.** GTK's `onDecidePolicy` only consults `AbstractView::shouldAllowNavigationToURL` (the rules-based check) and surfaces the result via the `will-navigate` event. The user-supplied synchronous `DecideNavigationCallback` (the `webviewDecideNavigation` JSCallback on the Bun side) is registered through the FFI but neither GTK nor (now) WPE actually invokes it. Either delete the field on both sides or wire it up — both should land together. Out of scope here.
- **Permission requests** (`onPermissionRequest` for camera/mic). On bare-DRM there's no dialog system; auto-allow with a TODO is the kiosk-appropriate behavior. ~20 lines if/when needed.
- **`load-failed-with-tls-errors`**. Trivial port (~10 lines). Punt until something actually loads HTTPS on the kiosk and we have a reproduce case.
- **Comments in `nativeWrapper_wpe.cpp:383+` mark a "Navigation action / script message helpers (legacy — unused on WPE)" block as legacy.** That comment is now misleading — navigation IS used on WPE. Either delete the legacy block or update the comment. One-line cleanup, deferred.
- **`webview.on("load-failed", …)` handler in hello-embedded was sketched in §16 but not wired**. The `load-failed` C++ thunk is in place, but Bun's `BrowserView.on()` whitelist (`BrowserView.ts:304-314`) doesn't include `"load-failed"` — adding it requires touching the cross-platform event surface, which is in scope for a different cleanup session (the same one that wires `navigationCallback_`).


## 18. Session results (2026-04-25, follow-up — kiosk chrome bar prototype)

**Goal:** give the bare-DRM kiosk a way to exit back to TTY without `pkill`. On macOS/GTK the OS provides a titlebar with a close button; on bare-DRM there's no compositor, no titlebar, no [X]. So today the only way out is killing the process from another terminal.

**Decision:** _no new Electrobun abstraction._ Instead, mirror what macOS already exposes:
- Set `titleBarStyle: "hidden"` on the BrowserWindow → on macOS/GTK this removes native chrome (so the in-page chrome isn't doubled up); on WPE-on-DRM this is a no-op since there's no native chrome anyway.
- The app draws its own chrome bar in HTML/CSS (a `<header>` pinned to the top, app body fills the rest).
- The chrome's [X] button calls a Bun-side RPC that invokes `Utils.quit()` to drive the existing shutdown path.

**Why not a separate chrome webview?** WpeBackend is single-view by design — DRM has one scanout plane, the SHM exporter wires one WebKitWebView to one fullscreen frame. Compositing N webviews into one scanout would require extending `onExportShm` to merge buffers, which is real work for a feature that doesn't need it on macOS/GTK either.

**Why `Utils.quit()` not `BrowserWindow.close()`?** On WPE the window-close callback is not wired (kiosk has a degenerate single-fullscreen-window lifecycle, see `nativeWrapper_wpe.cpp:171`). Calling `close()` would call `stopEventLoop()` → `g_main_loop_quit()`, but Bun never observes the close event, so it stays running and the process hangs. `Utils.quit()` calls `stopEventLoop` + `forceExit(0)` which causes the process to terminate, which runs `~WpeBackend()` → `teardown()` → `display_.reset()` (DRM master drop). Verified in `package/src/bun/core/Utils.ts:122-146`. The WPE seam already exposes `waitForShutdownComplete` and `forceExit` (lines 106-113 of `nativeWrapper_wpe.cpp`), so Bun's `quit()` works end-to-end.

### What shipped (after two ratchets)

**First attempt:** wired `[✕]` via the standard Electrobun RPC (`Electroview.defineRPC` + `BrowserView.defineRPC`). **Silently broke both buttons.** Root cause: `Electroview.init()` does `window.__electrobun!.receiveMessageFromBun = …` (`browser/index.ts:36`), throws TypeError because `window.__electrobun` is never set on WPE — the WPE seam discarded `electrobunPreloadScript` (`nativeWrapper_wpe.cpp:279` had `(void)electrobunPreloadScript;`). The throw killed the rest of the view's `index.ts`, including the pure-CSS `[⛶]` toggle.

**Stopgap:** chrome `[✕]` does `window.location.href = "electrobun://quit"`, fires `decide-policy` → `will-navigate` → Bun's listener pattern-matches the URL and calls `Utils.quit()`. Worked, but page2 had no chrome (manually duplicated as a workaround that doesn't generalize past two pages).

**Final state:** ported the WPE preload + script-message-handler bridge, then added auto-chrome injection to the preload pipeline. No app-side chrome HTML or wiring needed — `titleBarStyle: "hidden"` is the entire opt-in.

### Files touched

**electrobun package (the bridge port + auto-chrome):**
- `package/src/native/linux/backend.h` — added `std::string electrobunPreloadScript;` to `WebviewSpec`.
- `package/src/native/linux/nativeWrapper_wpe.cpp::initWebview` — populated `spec.electrobunPreloadScript` instead of discarding it.
- `package/src/native/linux/wpe/wpe_backend.cpp`:
  - Added `#include <chrono>` + `<thread>` (for the GTK-pattern deferred-free).
  - `WpeWebViewImpl`: added `bunBridgeHandler_` / `internalBridgeHandler_` / `eventBridgeHandler_` (typed `HandlePostMessage`) + non-owning `WebKitUserContentManager* userContentManager_`. Implemented `addPreloadScriptToWebView` (was a TODO stub) using `webkit_user_script_new` + `webkit_user_content_manager_add_script` with `WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START`.
  - `WpeBackend::primeWpeView`: pulled the user content manager via `webkit_web_view_get_user_content_manager(webView)`, registered three `script-message-received::{bunBridge,internalBridge,eventBridge}` signal handlers + `webkit_user_content_manager_register_script_message_handler(manager, "...", nullptr)` (WPE's 3-arg form, the doc example showing 2-arg is stale). Stored the manager on the impl so `addPreloadScriptToWebView` can reach it.
  - `WpeBackend::createWebview`: copies `spec.bunBridgeHandler` / `internalBridgeHandler` / `eventBridgeHandler` onto the impl and calls `primaryView_->addPreloadScriptToWebView(spec.electrobunPreloadScript.c_str())` BEFORE the deferred `loadURL` so it injects at document-start of the user URL.
  - Added `forwardBridgeMessage` helper + 3 static thunks (`onBunBridgeMessageStatic` etc.). **WPE-WebKit 2.0 API drift:** the signal callback receives `JSCValue*` directly (not `WebKitJavascriptResult*` like older GTK). Skip `webkit_javascript_result_get_js_value` — the GTK code in `nativeWrapper.cpp:2630-2715` was the wrong template for this part. Lifetime: same "leak then free 1s later in detached thread" pattern GTK uses, because the Bun JSCallback may still be using the string async.
- `package/src/bun/preload/chrome.ts` — **new file.** Auto-injected chrome `<header>` with `[⛶]` + `[✕]`. Idempotent (skips if already present). Reads `window.__electrobunTitleBarStyle === "hidden"` to decide whether to inject. Title pulled from `document.title` and updated via `MutationObserver` so single-page apps get correct titles. `[✕]` calls `send("electrobunChromeQuit", {})` (internal RPC). `[⛶]` sets `data-hidden` on the header + a `data-electrobun-chrome-hidden` flag on `<body>` (which the injected `<style>` reads to remove `padding-top`). Hidden state persisted via `sessionStorage` (`__electrobun_chrome_hidden`) so navigating between pages preserves the user's choice.
- `package/src/bun/preload/index.ts` — call `initChrome()` after the other inits.
- `package/src/bun/preload/globals.d.ts` — added optional `__electrobunTitleBarStyle` global.
- `package/src/bun/proc/native.ts` — read `parentWindow?.titleBarStyle ?? "default"` and emit `window.__electrobunTitleBarStyle = ${JSON.stringify(...)};` into the dynamic preload prefix. Added `internalRpcHandlers.message.electrobunChromeQuit` handler that inline-requires `Utils.quit`.
- `package/src/bun/core/BrowserWindow.ts` — store `titleBarStyle` as a member field so the FFI side can read it.

**hello-embedded (reverted to clean — no chrome HTML):**
- `src/bun/index.ts` — just `titleBarStyle: "hidden"` on the BrowserWindow + the will-navigate logging.
- `src/main/index.html` — original orange-on-charcoal app body, no `<header>`. Generic `button {}` scoped to `.card button` so the home page's "Press me" doesn't bleed into nav-test. nav-test uses two `<button>` elements (not `<a>`+`<button>`) for baseline alignment.
- `src/main/index.ts` — original frame counter + click counter. No chrome wiring at all.
- `src/main/page2.html` — back to the original minimal page (no chrome HTML).

### Verified end-to-end on the bar panel (2026-04-25)

- ✅ Chrome auto-injects on home page (no app code touched).
- ✅ Chrome auto-injects on page2 too (cross-page proof).
- ✅ `[✕]` quits cleanly via real RPC; DRM master released; relaunch succeeds.
- ✅ `[⛶]` hides chrome; tap body to bring it back. Persists via sessionStorage across navigation.
- ✅ Bridge port works — `Electroview.defineRPC` would now succeed (verified by the chrome's `send("electrobunChromeQuit", ...)` reaching the Bun-side handler).

### Gotchas surfaced this session

- **WPE-WebKit 2.0 deprecated `WebKitJavascriptResult`.** The `script-message-received` signal callback now receives `JSCValue*` directly. The old GTK code with `webkit_javascript_result_get_js_value(js_result)` doesn't compile — the function isn't declared anywhere reachable from `wpe/webkit.h`. Skipped that step, used `JSCValue*` as the param type.
- **`webkit_user_content_manager_register_script_message_handler` is 3-arg in WPE.** Doc-comment example shows 2-arg `(manager, "name")` but actual signature is `(manager, name, world_name)`. Pass `nullptr` for default world. Compiler error pointed straight at the line.
- **Inline `style="display: flex"` on the chrome `<header>` outranks external CSS without `!important`.** First version of `[⛶]` toggle changed the body padding (with `!important`) but the bar stayed visible. Adding `!important` to `[data-electrobun-chrome="true"][data-hidden] { display: none !important; }` fixed it.
- **`[✕]` is a tap-target hazard right next to `[⛶]`.** User accidentally tapped close instead of fullscreen multiple times. Future polish: separate them, or make `[✕]` require confirmation, or move `[✕]` to a different corner. Deferred.

### Open follow-ups

- **`[⛶]` and `[✕]` are too close on a touchscreen.** Visual or spatial separation needed.
- **Chrome bar height/padding on the 1920×480 panel.** Currently 44px fixed. Buttons sometimes overflow at narrow widths. Tune `font-size` or use viewport-relative units that scale to actual height. Not blocking.
- **macOS traffic lights with `titleBarStyle: "hidden"`?** `"hidden"` removes them entirely. If you want traffic lights on macOS while having in-page chrome, switch macOS to `"hiddenInset"` and offset the chrome's leading padding to clear them.
- **`closeWindow` callback not wired on WPE.** If a future session adds programmatic window close (`BrowserWindow.close()` instead of `Utils.quit()`), wire the close callback so Bun's `BrowserWindowMap` cleanup runs and `exitOnLastWindowClosed` triggers `Utils.quit()`. Today the only programmatic exit is `Utils.quit()` directly.
- **`callAsyncJavascript` and `updateCustomPreloadScript` on WpeWebViewImpl** are still TODO stubs — not exercised by the chrome flow but the next webview-tag or context-menu work will hit them.
- **Chrome theming** is hard-coded charcoal/orange in `chrome.ts`. Apps will eventually want to override colors / hide buttons / customize. Probably opt-in via window options (`chrome: { background, accent, buttons: [...] }`) once a second use case emerges.

## 19. Session results (2026-07-28 — first-frame latency)

### Measured baseline

On moodymoose, `hello-embedded` took about 45 seconds from
`loadURL("views://mainview/index.html")` to its first exported SHM frame. CPU
and memory were idle, and installing AT-SPI did not change the result.

`strace -ff -ttt -T` found two sequential 25-second `ppoll()` timeouts in
`WPEWebProcess`. Debugger stacks identified them as:

1. GLib's `g_power_profile_monitor_dup_default()` creating the portal-backed
   power-profile monitor.
2. WebKit's realtime-thread fallback creating
   `org.freedesktop.portal.Realtime`.

Both calls activate `org.freedesktop.portal.Desktop`. With no portal
configuration and no graphical session, xdg-desktop-portal selected its GTK
backend as a last-resort fallback. GTK could not start on the console, so the
portal frontend never acquired its bus name before WebKit's calls timed out.

### Fix

The linux-embedded extractor now writes:

```ini
[preferred]
default=none
```

to `~/.config/xdg-desktop-portal/portals.conf` and restarts the user portal
service before starting the kiosk. The setting applies to `--no-kiosk`
installs too, so a launcher run manually from a TTY gets the same behavior.

`default=none` disables GUI portal backends; it does not disable the portal
frontend's built-in PowerProfileMonitor and Realtime interfaces. This is the
correct contract for a bare-DRM console with deliberately no GTK.

### Result

Cold D-Bus activation after the change:

- launcher start → seed WPE frame: **0.64 seconds**
- app `loadURL` → first app frame: **44 milliseconds**
- launcher start → first app frame: **6.90 seconds**

The remaining ~6 seconds are in Electrobun/Bun app startup and are now
separate from WebKit rendering. `WpeBackend` logs both “first WPE frame” and
“first frame after loadURL” timings so this regression is visible directly in
the journal next time, without summoning the strace kraken.

### Remaining Cottontail startup

A network-only trace ruled out `hello-embedded`'s Vite development-server
probe: the failed localhost request took about 27 ms. Most of the remaining
delay was Cottontail loading the app's Bun entrypoint.

The tiny main process bundled to more than 9 MB because `electrobun/bun`
currently re-exports Three.js and Babylon.js, pulling 1,930 modules into the
bundle even though this app does not use them. Setting `minify: true` in
`build.cottontail` preserved the same cross-platform source while producing:

- Bun entrypoint: **9,108,349 → 5,676,505 bytes**
- complete `app.asar`: **10,443,856 → 7,012,004 bytes**
- entrypoint written → `createWebview`: **4.94 → 4.32 seconds**
- launcher → first app frame: **7.50 → 6.94 seconds**

Those control runs used an already-running portal service. The next larger
upstream opportunity is making the main SDK entrypoint tree-shake its optional
Three.js/Babylon.js exports; minification is a safe proving-ground win, not a
substitute for that cleanup.

## 20. Session results (2026-07-28 — native window decorations)

### What was broken

The injected close button navigated to `electrobun://chrome/close`, then relied
on the general `will-navigate` callback reaching Bun. That path carried C
strings through a threaded Bun FFI callback which intermittently arrived as
`Buffer is already detached`. It also intentionally trusted only `trusted`
views, so an `untrusted` About window could never close through that shortcut.

Maximize was not a native window operation at all. It only hid the injected
HTML header and removed the body's top padding. That happened to resemble
fullscreen for the already-panel-sized main window, but an About window kept
its small compositor rectangle and merely became a borderless small rectangle.

### Native decoration path

WPE now registers a dedicated `electrobunChrome` WebKit script-message handler
on every view. It recognizes only `close`, `maximize`, and `restore`, associates
the action with that view's host window, and queues it onto the GLib loop after
the WebKit signal unwinds. No action string crosses Bun's asynchronous FFI
boundary.

- Close calls the window's normal core close trampoline. Core removes the
  window's child views, emits the Bun close event, and applies
  `exitOnLastWindowClosed`.
- Maximize saves the primary view's compositor frame and resizes it to the DRM
  panel.
- Restore reapplies that exact saved frame.
- The exported `maximizeWindow`, `unmaximizeWindow`, and
  `isWindowMaximized` functions use the same WPE state rather than no-ops.

This makes a secondary About window fill the panel when maximized and return to
its requested position and size when restored. The main view is already fitted
to the panel, so its native frame stays panel-sized while its framework-owned
chrome view collapses to the reveal handle.

### The first-tap deadlock

The first panel test froze after maximize. A live debugger stack showed:

```text
GLib idle callback
  -> setWindowMaximized
  -> dispatchSyncMain
  -> std::future::wait
```

The launcher and ElectrobunCore both run the shared default GLib context from
different threads. `dispatchSyncMain` remembered only the most recent runner's
thread ID, so a callback executing on the other legitimate context-owning
thread tried to synchronously dispatch to itself.

`dispatchSyncMain` now first asks
`g_main_context_is_owner(g_main_context_default())`. A current context owner
runs the operation inline; worker FFI calls still marshal through an idle
callback and wait. Repeated maximize/restore cycles no longer freeze.

### Touch UX

The broad “tap near the top to restore” behavior is gone because it steals taps
from application controls. When maximized:

- a touch beginning within 16 px of the physical top edge is observed but not
  consumed;
- only a downward, predominantly vertical pull of at least 36 px is claimed;
- the pull reveals the titlebar over the still-maximized content and sends no
  native window action;
- the revealed maximize button changes to Restore; tapping it restores the
  native frame and returns the content to its normal below-titlebar layout;
- a possible synthetic post-touch click is suppressed for 500 ms;
- ordinary taps, including application buttons near the top, remain untouched.

A visible top-center reveal handle remains as a discoverable tap fallback.
For the 1920×480 touch panel, the titlebar grew from 44 px to 60 px, its two
button targets grew from 44×44 to 68×60 px, and the restore handle grew from
52×20 to 76×28 px. The larger titlebar buttons were confirmed more comfortable
on the physical panel.

### Why the resize fix temporarily lost the titlebar controls

The old header lived inside the application's WebView. Moving it with body
padding could not resize apps that use `100vh` (including
`hello-embedded`'s `h-screen`), so the content retained its maximized layout
and was merely pushed downward. The correct fix was a separate
framework-owned BrowserView: the app receives a real 1920×420 viewport and the
60 px chrome view is composited above it.

The pooled-view allocator created every WPE-FDO backend at the full
1920×480 panel size, loaded `about:blank`, and only then resized the chrome
slot to 1920×60. WebKit's DOM and visual viewport both reported 1920×60, but
DRM captures showed a stale 512 px render tile:

- a related/shared WebProcess painted the title while clipping the
  right-aligned buttons;
- a separate WebProcess moved the stale tile and painted the buttons while
  losing the left-side title;
- CSS geometry changes moved neither underlying failure.

`createOnePooledView` now receives the first assigned frame and creates the
WPE-FDO exportable at that size. The chrome's first SHM buffer is therefore
1920×60 rather than 1920×480. Full-panel captures show the title at the left
and both controls at the right, and shared- versus separate-process captures
are pixel-identical. Chrome stays `trusted`, so it shares the app's single
WPEWebProcess without the rendering regression.

The title is also present in the initial HTML rather than inserted after first
paint, and the control icons use CSS geometry instead of font glyphs. Those
are deterministic hardening; neither was the cause of the missing 512 px tile.

### JavaScript evaluation thread affinity

The framebuffer probe used `BrowserView.executeJavascript` to try to drive the
chrome controls automatically. That exposed a separate parity bug: the WPE
implementation called WebKit directly from Bun's FFI thread. It now copies the
script, marshals evaluation through the GLib/WebKit context, and consumes the
async result so WebKit errors are logged instead of silently discarded.
The related chrome-view probe still produced no DOM effect and no WebKit error,
so automated control activation remains a follow-up; the physical touch run is
still the authoritative interaction regression test.

### Verified on moodymoose

With `hello-embedded` built against the matching runtime manifest:

- main maximize → restore completed repeatedly;
- About maximize filled the panel and restore returned it to its dialog frame;
- About close removed only About;
- main close exited the launcher service cleanly;
- the native log associated every action with the expected `webviewId`;
- first app frame after `loadURL` remained 22–54 ms;
- a DRM `kmsgrab` capture verified all 1920 titlebar pixels, and the final
  shared-process run used one WPEWebProcess.

The general `webviewEventJSCallback` still reports an occasional detached
buffer while forwarding navigation events. Decoration actions deliberately no
longer depend on that path, but the general event-string lifetime is a separate
follow-up goblin.
