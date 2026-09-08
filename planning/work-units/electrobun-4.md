# Early WPE WebProcess extension configuration

Owning issue: https://github.com/kortexa-ai/electrobun/issues/4

Keep this capability generic and opt-in. Resolve an app-selected absolute
extension directory on WebKit's owning UI thread before the first pooled view
can launch a WebProcess. No app-specific audio properties, clock injection or
global audio defaults belong in the core. Invalid paths must not invoke WebKit's
extension-directory setter; no configuration must preserve existing behavior.

Validate unset/empty, relative, missing, regular-file and valid-directory inputs
without opening any display or audio device. Verify actual extension loading
and startup order through the custom WPE runtime during the consuming app's
staged integration. The library build alone is not proof of runtime behavior.

Build only the Electrobun core with reused vendors and the existing WPE/WebRTC
libraries. Do not rebuild WPE or GStreamer. Keep the running kiosk's copied
bundle intact while the devkit is rebuilt; app packaging, rollback and targeted
deployment remain in https://github.com/kortexa-ai/hermes-desk/issues/10.
