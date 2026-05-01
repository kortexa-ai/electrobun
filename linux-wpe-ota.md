# linux-wpe OTA: auto-updates for bare-DRM kiosk builds

Working notes for adding OTA/update support to the `linux-wpe` (linux-embedded) target. Sibling doc to `linux-wpe.md`.

> Status: design doc, v1.
> Branch: `kortexa/linux-wpe`.
> Scope: linux-wpe only. Desktop Linux, macOS, Windows keep their existing OTA paths unchanged.
> Decisions baked in: **user-mode systemd service**, **auto-rollback on first-launch crash**, **no impact on non-embedded targets**.

---

## 1. Goal

A `linux-wpe` kiosk on a Pi receives updates the same way the macOS/Windows builds do — drop a new tarball on the update host, the device picks it up on next check, applies it, and restarts cleanly. If the new build crashes on first launch, the device rolls back to the previous version automatically. No manual intervention, no black screens, no SSH-in-and-fix.

---

## 2. What already works for free

The existing Updater + extractor + build pipeline give us most of the OTA flow at no cost on linux-wpe:

- Bundle layout (`bin/launcher`, `Resources/version.json`, `Resources/app.asar`) is identical to desktop Linux.
- `electrobun build` already emits `linux-{app}.tar.zst` + `linux-update.json` + `{app}-Setup.tar.gz` when `config.build.linux.embedded === true`.
- `Updater.checkForUpdate` / `downloadUpdate` patch chains and full-download fallback work unchanged — they only care about hashes and tarballs.
- The extractor's first-install flow (read embedded metadata, decompress to `~/.local/share/{id}/{channel}/app/`) works unchanged.

The work below is **only** about how the running kiosk gets stopped, swapped, and restarted — and how we recover if the new version is broken.

---

## 3. What's missing

From the survey of `Updater.ts` + `extractor/main.zig`:

1. **Restart strategy is wrong for a kiosk.** `Updater.applyUpdate` on Linux today does `spawn(launcher, { detached: true }); exit(0)` (`Updater.ts:1062`). On bare DRM, the old process must release DRM master before the new one can grab it; spawn-and-exit races. There's no window manager to re-activate, no taskbar, no user click to relaunch.
2. **No supervisor.** The extractor drops a `.desktop` file (useless on a headless boot), but no systemd unit. There's nothing to restart the app if it crashes, and nothing for the updater to talk to.
3. **No rollback.** Updater preserves the previous tarball but never restores it. If a bad build crashes on startup, systemd `Restart=on-failure` boot-loops a black screen.
4. **No pre-update hook.** Kiosk apps may want to flush state before going down; macOS/Windows get away without one because the user clicks Quit.

Read-only rootfs handling is **explicitly out of scope** for v1. We assume `~/.local/share` is writable. If someone needs locked-down `/opt` later, we'll add it then.

---

## 4. Design

### 4.1 Layout — symlink-swapped versioned directories

Today: `~/.local/share/{id}/{channel}/app/` is a directory; `applyUpdate` rm's it and moves the new one into place.

New layout for linux-wpe:

```
~/.local/share/{id}/{channel}/
  current        → app.{hash-new}/      (symlink, atomic swap point)
  previous       → app.{hash-old}/      (symlink, rollback target)
  app.{hash-new}/   bin/launcher, Resources/, ...
  app.{hash-old}/   bin/launcher, Resources/, ...
  self-extraction/  (existing tar/patch cache, unchanged)
  state/
    last-good     (file: hash of last version that booted successfully)
    boot-attempt  (file: hash of version currently being attempted)
```

The systemd unit always invokes `~/.local/share/{id}/{channel}/current/bin/launcher`. Update = drop new directory next to the old one, atomically `rename(2)` the symlink, restart the unit. Rollback = flip the symlink back to `previous`. No directory moves while the service is stopped — only symlink flips.

### 4.2 Systemd user unit

Extractor generates and installs `~/.config/systemd/user/{id}.service` at first install:

```ini
[Unit]
Description={DisplayName} (Electrobun kiosk)
After=graphical-session.target

[Service]
Type=simple
ExecStart=%h/.local/share/{id}/{channel}/current/bin/launcher
Restart=on-failure
RestartSec=2
StartLimitIntervalSec=30
StartLimitBurst=3
# DRM master cleanup is done in WpeBackend dtor; give it time
TimeoutStopSec=5

[Install]
WantedBy=default.target
```

After install: `systemctl --user daemon-reload && systemctl --user enable --now {id}`.

User-mode means no sudo at install time, but requires `loginctl enable-linger {user}` for headless boot (no logged-in session). The extractor will run that too (it's idempotent and doesn't need root if the user is the target user).

`StartLimitBurst=3` means systemd gives up after 3 crashes in 30s instead of boot-looping forever — this is the trigger for our rollback probe (§4.4).

### 4.3 Update apply — branch in `Updater.applyUpdate`

When `process.platform === 'linux'` **and** the bundle is `linux-embedded` (detected by presence of the systemd unit, or by a flag in `version.json` written at build time — leaning toward the flag, cleaner):

1. Extract the new tarball to `~/.local/share/{id}/{channel}/app.{newhash}/` (sibling of `current`, **not** a replacement).
2. Run `chmod +x` on the known binaries (existing logic).
3. Fire the new `beforeUpdate(currentHash, newHash)` Bun lifecycle hook; await with a short timeout (~2s) so a stuck app can't block updates indefinitely.
4. Atomically update symlinks:
   - `previous` → whatever `current` points to
   - `current` → `app.{newhash}`
   (`rename(2)` of a tmp symlink is atomic on Linux.)
5. Write `state/boot-attempt` = `{newhash}`. Do **not** touch `state/last-good` yet.
6. `systemctl --user restart {id}`. Updater process exits; systemd brings the new launcher up.
7. GC: keep `current` and `previous` directory targets, delete any older `app.{hash}/` dirs.

The existing macOS/Windows/desktop-Linux branches in `applyUpdate` are untouched.

### 4.4 Auto-rollback — startup probe + restart-limit trigger

Two cooperating pieces:

**(a) Launcher writes a "boot succeeded" sentinel.** After `WpeBackend::primeWpeView()` returns successfully and the GLib main loop has spun for ~3 seconds without exiting, the launcher writes `state/last-good` = current hash. This proves the new version reached steady-state rendering, not just process-started.

**(b) `ExecStartPre` checks for rollback.** A small shell script (or Zig helper, TBD) runs before each launcher start:

```
if state/boot-attempt exists and != state/last-good and current points to boot-attempt:
    # Last attempt didn't reach last-good. Roll back.
    swap current ↔ previous
    clear boot-attempt
    log to journal
```

This makes rollback recovery automatic on the *next* start attempt. Combined with `StartLimitBurst=3`, the kiosk gets up to 3 tries on the new version, and if none of them write `last-good`, the next systemd restart cycle rolls back. After rollback, the previous version starts and (presumably) writes its own `last-good` again — system stable.

If even the rolled-back version fails to start (highly unlikely — it was working yesterday), systemd hits the burst limit and stops trying. The screen goes black, but the device is recoverable via SSH. That's the irreducible failure mode; we accept it.

### 4.5 `beforeUpdate` lifecycle hook

Add to `Updater.ts` and surface through the existing event/lifecycle pattern. Default no-op. Apps that need to flush state register a handler:

```ts
electrobun.events.on('willUpdate', async ({ currentHash, newHash }) => {
  await flushState();
});
```

Updater awaits with a 2s timeout. The hook is platform-agnostic (will fire on macOS/Windows too) but only linux-wpe needs it for rollback semantics; we're getting it for free on the other platforms.

### 4.6 Build-side: flag the bundle as linux-embedded

Add to `version.json` (and `metadata.json`):

```json
{ "version": "...", "hash": "...", "channel": "...", "linuxTarget": "embedded" }
```

`Updater` reads this and routes to the wpe-specific apply path. Cleaner than sniffing for a systemd unit at runtime.

---

## 5. Phasing

Each phase is independently shippable and testable.

### Phase A — service-managed launch (no OTA changes yet)

Goal: a fresh install of a linux-embedded build comes up under systemd, restarts on crash, survives reboot via linger.

- `cli/index.ts`: write `linuxTarget: "embedded"` into `version.json` and `metadata.json` when `build.linux.embedded`.
- `extractor/main.zig`: when metadata says `linuxTarget: "embedded"`, after extracting:
  - Create the symlinked layout (`current` → `app.{hash}/`, no `previous` yet).
  - Generate the systemd unit from a template embedded in the extractor (new marker: `ELECTROBUN_SYSTEMD_UNIT_V1`).
  - Run `loginctl enable-linger {user}` (idempotent).
  - Run `systemctl --user daemon-reload && systemctl --user enable --now {id}`.
  - Skip the `.desktop` shortcut for embedded builds (it's noise on a kiosk).

Test: install hello-embedded fresh on a clean Pi, reboot, confirm the kiosk comes up under systemd without any login.

### Phase B — service-aware OTA apply

Goal: an update lands while the service is running, swaps cleanly, restarts.

- `Updater.applyUpdate`: branch on `linuxTarget === "embedded"`, implement §4.3.
- Add `beforeUpdate` event (§4.5).
- `WpeBackend`: ensure `drmDropMaster` happens before process exit so the next systemd start can grab master cleanly. (Per the design doc this already happens in the dtor; verify under SIGTERM from systemd.)

Test: run hello-embedded, push a new version with a visible change (e.g., different background color), watch it apply and restart on the panel without manual intervention.

### Phase C — auto-rollback

Goal: a deliberately-broken update auto-rolls back within ~10 seconds.

- Launcher: write `state/last-good` after primeWpeView + 3s steady-state.
- `ExecStartPre` rollback check (§4.4) — embedded as a small Zig helper invoked from the unit, or inlined as a sh script. Leaning Zig helper for consistency with the existing extractor.
- Updater: maintain `previous` symlink and GC of older `app.{hash}/` dirs.

Test: ship a "broken" build that exits 1 immediately. Confirm three crashes, then a rollback to the prior hash, and steady operation thereafter. Verify journal log makes the rollback obvious to a human.

---

## 6. Open / deferred

- **Read-only rootfs split** (`/opt/{id}` immutable, `/var/lib/{id}` writable): not v1. Add when there's a real device using it.
- **Background download**: today the updater is invoked explicitly from app code. Whether the kiosk should poll on a timer is an app-level concern, not an OTA-architecture concern.
- **Signed updates**: out of scope for v1. The existing Updater has no signature verification on any platform; that's a separate cross-platform feature.
- **Multiple channels on one device** (dev + canary side-by-side): already supported by the per-channel directory structure; no work needed.
- **Crash detection during steady-state** (kiosk runs fine for an hour, then segfaults): handled by `Restart=on-failure`, *not* by rollback — a steady-state crash isn't an update-induced regression by definition. We only roll back failed *first launches*.

---

## 7. Files touched (summary)

- `package/src/cli/index.ts` — write `linuxTarget` into version.json/metadata.json. Phase A.
- `package/src/extractor/main.zig` — embed systemd unit template, generate layout + unit + linger on first install, skip .desktop for embedded. Phase A.
- `package/src/bun/core/Updater.ts` — branch on `linuxTarget === "embedded"` in `applyUpdate`, add `beforeUpdate` event, maintain `previous` symlink, GC. Phases B–C.
- `package/src/launcher/main.ts` — write `state/last-good` after steady-state. Phase C.
- New: small Zig helper for `ExecStartPre` rollback check (or shell script — TBD). Phase C.
- `package/src/native/linux/wpe/...` — verify SIGTERM cleanup releases DRM master. Phase B (verification only, no new code expected).
