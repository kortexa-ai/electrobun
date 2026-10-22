# Current-main WPE port and hello-embedded qualification

Issue: https://github.com/kortexa-ai/electrobun/issues/5

Preserve the existing WPE branch as a recovery ref. Consolidate its final delta
onto current upstream main so obsolete intermediate CLI code does not reappear.
Resolve the current SDK, transport, Zig, launcher and Linux build interfaces.
Use the supported Hutch/Cottontail toolchain and rebuild native artifacts.
Detect the actual WPE wrapper through an optional capability, with the existing
metadata fallback for older builds. Keep schema-1 local devkits and vanilla
desktop wrappers compatible without application-specific platform branches.

The reserved framework chrome partition keeps its native action bridge, but
Core supplies only the event preload. This prevents a second injected header
and application RPC initialization on the chrome document's data URL. Ordinary
application views retain their existing RPC and sandbox behavior.

Validate the exact resulting devkit with hello-embedded: build the application,
run it on the Pi panel, exercise navigation and window chrome, and capture
screenshots. Restore Hermes Desk to its previous service state after validation.
Keep exact build, runtime and screenshot evidence in the owning issue.
