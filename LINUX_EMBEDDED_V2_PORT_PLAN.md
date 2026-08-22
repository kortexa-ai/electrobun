# Electrobun v2 Linux-embedded port plan

Research checkpoint: 2026-08-22, Raspberry Pi 5 (`aarch64`, Debian 13,
Mesa 26.2.0), Electrobun 2.0.1, Hutch 0.24.3, Cottontail 0.5.0.

## Decision

Port the existing WPE/DRM backend onto Electrobun v2 first. Do not replace WPE
with Electrobun's Dawn/WebGPU UI in this port.

This is now a proved forward-port rather than a speculative design. Hutch built
the current `hello-embedded` source against an exact local Electrobun 2.0.1
devkit containing the minimally ABI-updated WPE wrapper. That unmodified output
bundle ran on the panel at about 60 FPS and completed main/view RPC round trips
in 7–19 ms. The run used Cottontail 0.5.0 and the Pi's V3D EGL renderer.
Physical visual correctness remains to be checked locally by a person.

Direct Dawn rendering is not viable on this Pi with the 2.0.1 runtime:

- The bundled Dawn rejects V3DV because required dynamic-indexing Vulkan
  features are absent. Default adapter selection falls back to LLVMpipe, so a
  nominal WebGPU UI would be CPU-rendered.
- Dawn exposes Xlib, XCB, Wayland, Metal, Win32, and Android surface descriptors,
  but not a bare-DRM/KMS or imported `VkSurfaceKHR` descriptor. The Pi supports
  `VK_KHR_display`; Electrobun's Dawn integration has no route to it.
- Electrobun's Linux `GpuWindow` and `WGPUView` implementations still create
  GTK/X11 windows. The native wrapper initializes GTK even for compute-only
  WGPU use.
- `electrobun/main/ui` is an interesting no-DOM prototype, but it is not a
  transparent home for this React 19/Tailwind/Three.js/browser-navigation app.
  Using it would be a renderer rewrite and would lose current desktop/kiosk
  source parity.

A small Wayland compositor could make upstream GTK/WebKitGTK run on KMS, but
that trades WPE for a compositor and does not solve Dawn's V3DV rejection. It
is not a simpler or more direct kiosk architecture.

## The v2 architecture

The ownership boundary is:

```text
project config/source
        |
        v
Hutch 0.24.3
  exact versions, dependencies, scripts, toolchains, devkit projection,
  compilation, bundle layout, signing, installers, updates
        |
        +---- native-devkit.json + Electrobun SDKs/runtime artifacts
        |
        v
Cottontail 0.5.0                 Electrobun Core (Zig, ABI v1)
  main-process JSC runtime  <-->  IDs, registries, callbacks, native dispatch
        |                                      |
        +---------------- FFI -----------------+
                                               |
                                               v
                                  libNativeWrapper.so
                                  window/view/display backend
                                               |
                             +-----------------+----------------+
                             |                                  |
                       desktop wrapper                    kiosk wrapper
                       GTK/WebKitGTK/CEF                 WPE + EGL + DRM/KMS
```

Hutch is the package/build/release orchestrator. Its projected `.hutch/devkit`
is generated state and is the source of the exact SDK, core, runtime, and
toolchain graph. Cottontail executes the main-process bundle and supplies the
Node/Bun APIs the application uses; it is deliberately not a package manager.
The native wrapper remains the correct extension point for kiosk display and
webview behavior.

The npm `electrobun` package is only the version-matched Hutch bootstrap in v2.
The application correctly pins all three moving pieces:

```ts
// @hutch cli=0.24.3 cottontail=0.5.0
electrobun: { version: "2.0.1" }
```

For unpublished core development Hutch already supports an exact local
artifact root through `HUTCH_ELECTROBUN_DEVKIT_ROOT`. That is the right inner
loop; do not edit `.hutch/devkit` or restore the old TypeScript CLI.

## What the spike proved

The first v2 bundle was built from the current `../hello-embedded` source with
`bunx electrobun prepare --env=dev` and `./build.sh dev`, then used the WPE
wrapper from `kortexa/linux-wpe` in place of the official GTK wrapper. After
the compatibility fixes, the same result was reproduced through Hutch's
supported local-devkit path: `HUTCH_ELECTROBUN_DEVKIT_ROOT` selected a complete
2.0.1 devkit whose `nativeWrapper` was the WPE library, and `./build.sh dev`
packaged the byte-identical wrapper without manual bundle editing.

The first launch found two concrete compatibility issues:

1. The WPE shared object relied on its old host executable to export GCC's
   out-of-line AArch64 atomic helpers. Compiling it with
   `-mno-outline-atomics` makes the plugin self-contained for Cottontail 0.5.0.
2. v2 eagerly binds `wgpuSurfaceCapabilitiesFreeMembersShim`. Adding the WPE
   no-op and the small v2 window/webview ABI delta allowed the app to run.

The successful run then recorded:

- WPE prime view and `views://` scheme initialization;
- EGL 1.5 with `GL renderer=V3D 7.1.7.0`;
- panel geometry 480×1920 with fbcon rotation 3 and logical WPE geometry
  1920×480;
- `views://mainview/perf.html` navigation and first post-navigation frame in
  41 ms;
- 600 exported frames in the final 15-second Hutch-built run (and 960 in the
  earlier 20-second substitution run), normally 60 FPS;
- one-layer composition normally around 5.7–7.3 ms after warmup;
- live RPC ticks with 7–19 ms round trips;
- a separate 30-second Three.js/WebGL stress run reached 392 knots / 115,584
  triangles at 60.02 FPS with an 18 ms p95; 400 knots was just beyond the
  test's 60-FPS acceptance threshold;
- SIGTERM entering Electrobun's normal quit sequence;
- no remaining launcher, Cottontail, WPE web process, or stolen VT afterward.

Logs and disposable artifacts:

- `/run/user/1000/electrobun-v2-wpe-smoke.log`
- `/run/user/1000/electrobun-v2-hutch-wpe.log`
- `/run/user/1000/electrobun-v2-hutch-wpe-three.log`
- `/home/pi/src/electrobun-v2-wpe-smoke`
- `/home/pi/src/electrobun-v2-wpe-devkit`
- `/home/pi/src/electrobun-wgpu-probe/dawn_device_probe`

## Port shape

Create a new port branch from current `main`; do not merge the entire old
feature branch. Its merge base predates 186 upstream commits and its package
still identifies itself as `1.18.4-beta.19`. Selectively transplant the kiosk
backend and its tests, then implement the v2-owned integration points.

### Keep and adapt

- `nativeWrapper_wpe.cpp` and the `native/linux/wpe` backend abstraction.
- DRM/KMS mode selection, rotation, EGL frame export/composition, libinput
  coordinate mapping, `views://` handling, navigation policy, RPC bridges,
  composited kiosk chrome, and multi-window emulation.
- WPE dependency detection and a separate `libNativeWrapper_wpe.so` build.
- Existing performance, navigation, touch, Three.js/WebGL, secondary-window,
  shutdown, and VT-release tests.
- The service concept for an unattended kiosk, after fitting it to v2's
  installer/update ownership.

### Reimplement at v2 boundaries

- Match every v2 Core-to-NativeWrapper symbol and exact signature. Add the WPE
  wrapper to `native-symbol-contract.test.ts` so future ABI changes fail in CI.
- Include the v2 should-close callback in window creation and implement
  `requestWindowClose` separately from the final `closeWindow` action.
- Add kiosk semantics/stubs for the current missing surface:
  `captureScreenRegion`, `centerWindow`, `getWindowButtonPosition`,
  `isWindowVisible`, `webviewSetSpellCheck`,
  `wgpuSurfaceCapabilitiesFreeMembersShim`, and
  `wgpuToggleGPUTestShader`. `runNativeEventLoopTick` is Darwin-only.
- Remove obsolete WPE JSC flags that produce warnings on the installed engine
  (`JSC_useExplicitResourceManagement` and `JSC_useAsyncStackTrace`) unless
  feature detection proves a compatible spelling.
- Generate a v2 `native-devkit.json` containing an optional
  `layout.runtime.nativeWrapperWpe` path. Keep this additive under schema 1 and
  retain core/sdk ABI version 1.
- Extend Hutch's devkit resolution and `PlatformPaths` with the optional WPE
  wrapper. Restore `build.linux.embedded`, make it mutually exclusive with
  `bundleCEF`, and copy the selected WPE file into the bundle under the fixed
  runtime name `libNativeWrapper.so`. The Core and TypeScript FFI loaders both
  intentionally use that fixed name.
- Add the same `embedded` property to Electrobun's v2 config types and tests.
  `../hello-embedded/electrobun.config.ts` can then set it only in the Linux
  platform block while preserving identical application source.

For the initial inner loop, a custom local devkit may expose the WPE wrapper as
its default `nativeWrapper` on this ARM64 machine. That requires no generated
project edits and uses:

```sh
HUTCH_ELECTROBUN_DEVKIT_ROOT=/absolute/path/to/package/dist \
  bunx electrobun prepare --env=dev
HUTCH_ELECTROBUN_DEVKIT_ROOT=/absolute/path/to/package/dist \
  bunx electrobun build --env=dev
```

The explicit `nativeWrapperWpe` plus Hutch selection is required before this is
a clean distributable feature rather than a Pi-local development override.

### Do not carry forward

- The v1 TypeScript CLI/build pipeline or its package-version machinery.
- Old launcher, extractor, signing, installer, and generic OTA changes already
  superseded by Hutch v2. Preserve only kiosk-specific service behavior not
  represented in Hutch.
- Bun-runtime assumptions. Cottontail is the supported v2 main process for this
  app and the successful spike already validates it.
- Phase-3-to-5 claims that Dawn can use `VK_KHR_display` on the Pi. Reopen that
  work only after the hardware and surface gates below pass.

## Implementation sequence and gates

### 1. Establish the v2 port branch

Branch from current upstream/main and transplant only WPE backend files,
focused shared abstractions, tests, and documentation. Keep the old feature
head and `backup/linux-wpe-before-sync` as read-only archaeology.

Gate: upstream native wrapper/core tests still pass before WPE is selected.

### 2. Make the WPE wrapper a complete v2 ABI peer

Apply the compatibility-spike changes, update all signatures against v2 Core,
and extend the symbol-contract test to Linux WPE. Add behavior tests for close
request/cancel/final close, view cleanup, hidden/visible state, navigation,
RPC reply handling, and unsupported WGPU calls.

Gate: build with no unresolved dynamic symbols; ABI contract has no required
missing symbols; official v2 Cottontail can load the wrapper without a host
bridge fallback.

### 3. Integrate the artifact graph with Hutch

Teach Electrobun's build/devkit manifest and Hutch's devkit loader/bundler
about `nativeWrapperWpe` and `build.linux.embedded`. Validate the mutual
exclusion with CEF, local-devkit refresh, manifest checksums, dev/canary/stable
bundle layouts, and absence of GTK/CEF libraries from the kiosk dependency
closure.

Gate: an ordinary `hello-embedded` Hutch build—not a hand-edited bundle—selects
WPE and includes Cottontail 0.5.0, Electrobun 2.0.1-compatible Core/SDK ABI 1,
and all required WPE shared-library dependencies.

### 4. Prove application parity on the Pi

Run bounded tests through a real VT with no X11 or Wayland session. Exercise:

- main and page-2 navigation plus back/forward/reload;
- perf RPC for at least five minutes, tracking FPS, frame time, memory, and
  round-trip latency;
- Three.js/WebGL and confirmation that WebKit reports a hardware V3D renderer;
- touch coordinates and rotation at all panel edges;
- external navigation policy;
- About/secondary-window emulation and close cancellation;
- composited chrome hide/reveal/close;
- SIGTERM, app-requested quit, crash, and clean DRM/VT release;
- repeated cold/warm launches and a run with the network unavailable.

Gate: no source-level platform branch in `hello-embedded`, no leaked process or
DRM master, and no functional regression versus the macOS build. A person must
perform the final pixel/orientation/touch inspection.

### 5. Reconcile kiosk installation and OTA with Hutch

Use Hutch's v2 installer, uninstall manager, versioned bundle, and update
metadata as the base. Add only the Pi-specific systemd/user-session behavior:
real-VT launch, service ownership, linger, restart policy, DRM/input groups,
atomic update handoff, health acknowledgement, and rollback.

Gate: install, start, update, rollback, disable, and uninstall are repeatable;
app data preservation is explicit; an update never leaves DRM owned by a dead
process or the panel stranded on the kiosk VT.

### 6. Re-evaluate direct WebGPU separately

Do not couple this experiment to the production WPE port. It becomes viable
only when all of these are true:

1. The selected Dawn release enumerates V3DV as a supported hardware adapter
   without LLVMpipe fallback.
2. Dawn/Electrobun has a maintained DRM/KMS, `VK_KHR_display`, or acceptable
   direct Wayland surface route.
3. A retained UI stack can meet React/browser parity requirements, or the
   product explicitly accepts a renderer rewrite.
4. A Pi benchmark beats or materially simplifies the WPE path for startup,
   frame time, memory, touch, and operational reliability.

Until then, raw Vulkan/GLES would be a new UI renderer and a maintained Dawn
fork would be a long-lived platform fork; neither belongs in the first port.

## Known risks

- Cottontail's Linux ARM64 native/local checkpoint is strong, but its own docs
  do not claim a complete multi-thousand-test Node/Bun compatibility pass.
  Our application canary must remain a release gate.
- WPE WebKit and Cottontail each embed/use JavaScriptCore in separate processes.
  Signal ownership and shutdown order need stress testing; Cottontail recently
  moved its GC suspension signal away from libuv's `SIGUSR1` on Linux.
- WPE engine versions vary by distro. Preload syntax and JSC options must be
  tested against the deployment image, not assumed from desktop WebKit.
- The current compositor performs rotation/readback work. The measured steady
  frame cost fits 60 Hz on this Pi, but Three.js, multiple layers, and thermal
  load still need sustained tests.
- Hutch 0.24.3 has no WPE field today. A production-quality target needs a
  coordinated Electrobun/Hutch change or a clearly versioned downstream Hutch
  release; a copied local wrapper is only a development bridge.

## Human visual verification checklist

Tomorrow, launch the bounded v2/WPE bundle on VT2, then check:

1. The 1920×480 UI fills the rotated 480×1920 panel with no crop, mirror, or
   one-frame stale border.
2. Text and images are sharp and colors/alpha are correct.
3. Touch all four corners and the center; verify the visible control under the
   finger activates.
4. Navigate main → page 2 → back; open/close About; hide and reveal kiosk
   chrome.
5. Run the Three.js page and look for animation, corruption, tearing, or
   obvious software-rendering behavior.
6. Close the app and confirm the panel returns to tty1 immediately.

The safe SSH/tmux launch pattern used for this research is:

```sh
sudo openvt -c 2 -f -s -w -- sudo -u pi sh -c \
  'cd /path/to/app/bin && exec env HOME=/home/pi USER=pi \
  XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  timeout --signal=TERM --kill-after=5s 300s ./launcher \
  >/run/user/1000/electrobun-kiosk-visual.log 2>&1'
sudo chvt 1
```

Keep a second SSH session available. If the bounded app misbehaves, terminate
its launcher/Cottontail process and run `sudo chvt 1`; do not reboot.
