# WPE review implementation — 2026-09-06

Applied on `kortexa/linux-wpe`, with matching client updates in hello-embedded
and hermes-desk. Original private WebKit 2.48.3 WebRTC and ECDSA GStreamer
libraries were preserved; only Electrobun and application artifacts rebuilt.

## Landed

- Kept original SDK ABI entry points; added versioned extended entry points.
- Explicit binary-RPC capability negotiation with legacy JSON compatibility;
  binary and text decryption now preserve arrival order.
- Fixed a native symbol-cache alias: same-signature functions accidentally
  shared a cache slot. Cache types now capture the symbol name, with atomic
  pointer publication and a regression test. The live startup check exposed
  this when transparency flags overwrote asset protocol permissions.
- Serialized WebKit/view lifecycle on the actual GLib event-loop thread.
- Enforced sandbox bridge/media isolation, per-view asset policy and custom
  roots, rejected asset path traversal, reset reused view state, honored
  transparency/passthrough/visibility and close policy.
- Bounded frame-presentation failure recovery and retained buffer ownership;
  reused compositor layer storage and fixed-size touch arrays.
- Atomic installer symlink selection; application-scoped portal policy;
  bounded startup read-ahead and opt-in native preheat.
- `--reuse-vendors --serial` supports low-memory rebuilds without re-fetching
  vendor toolchains. Embedded-only builds omit the unused CEF payload.

Creation orchestration fell from CCN 38 to 13 (policy binding 10); asset
request handling is 12 with asset loading 12, versus the original combined
handler's 18. Lizard 1.24.0, same counting rules as the initial review. This is
a maintainability result, not a claim of speed from moving code alone.

Hermes now stops capture on failure/disconnect and rejects late permission
results, bounds connection waits and screenshot payloads, uses the actual
desktop camera provider, expires stale camera frames, reconnects MJPEG, and
limits preview to 8 FPS. Its new small canvas orb is static while muted and
capped at 12 FPS when active, with no browser ML or WebGL. Settings remains
visible in the app header and uses a native modal dialog for focus/Escape.
Optional local attention processing runs outside WPE only while leased by an
opted-in UI. See Hermes's attention benchmark report for cost and limitations.

## Validation

- ElectrobunCore and extractor Zig tests passed; launcher/native release build
  passed using the existing vendors. Focused ABI/ownership/transport tests:
  22 passed. Native blend and asset-path C++ tests passed.
- hello-embedded typecheck and startup tests passed; both published vanilla
  Electrobun 2.0.2-beta.14 and local WPE devkit built from the same app sources.
  These are build checks, not a claim that the GTK bundle ran on bare DRM.
- Hermes typecheck, 20 TypeScript tests, and seven Python tests passed.
  Browser layout checks at 1920×480 and 390×844: no horizontal overflow;
  settings remained a 54×54 target; modal focus/Escape worked.
- Live kiosk connected via Realtime and stayed muted; `/health` returned fresh
  camera data. Process maps verified `/opt/wpewebkit-rtc` and the private
  `/opt/gstreamer-ecdsa/.../libgstdtls.so`.
- One final-path startup sample: first WPE frame 187 ms, first frame after
  app navigation 24 ms. These are framework frame milestones, not end-to-end
  voice latency or a cold-boot percentile. WPE process RSS was around 222 MiB
  during the bounded preview smoke runs; no automatic service restarts.
- The original kiosk bundle is retained under Hermes's ignored `.rollback/`.
  Only kiosk/sidecar services were restarted. No machine or backend service
  restart, and no camera images were retained by the detector or benchmarks.

## Remaining engineering, not claimed complete

The compositor still performs synchronous GPU readback. DMA-BUF/GBM scanout
and fence-based buffer lifetime management require a separate measured change,
not a speculative swap in this deployment. EGL initialization, input dispatch,
and composition retain relatively high complexity. A long-duration hardware
soak and a real-user gaze calibration/false-trigger evaluation remain useful;
short smoke tests cannot prove the absence of every leak or focus error.

Smarty's vision service was stopped and its GPU was occupied by other jobs;
RF-DETR latency was not measured. The local MediaPipe alternative was measured
and implemented. Async voice task delivery was assessed, not deployed; its
proposal is tracked in Hermes Desk's docs.
