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

