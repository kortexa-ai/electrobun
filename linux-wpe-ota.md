# linux-wpe OTA: auto-updates for bare-DRM kiosk builds

Working notes for adding OTA/update support to the `linux-wpe` (linux-embedded) target. Sibling doc to `linux-wpe.md`.

> Status: design doc, v2.
> Branch: `kortexa/linux-wpe`.
> Scope: linux-wpe only. Desktop Linux, macOS, Windows keep their existing OTA paths unchanged.
> Decisions baked in: **user-mode systemd service**, **auto-rollback on first-launch crash (supervisor-agnostic — works under systemd AND under manual TTY launch)**, **default-to-kiosk install with explicit opt-out (installer flag + runtime API)**, **no impact on non-embedded targets**.
>
> **Phase A landed (2026-04-30):** versioned-dir layout + systemd user unit generation/install + linger. Commits `487537c2` (Phase A scaffolding), `bc4d90b5` (composite frame_complete + bare-DRM frame defaults + secure views scheme — bug fixes surfaced by smoke-testing), `6178c6aa` (chrome top-edge restore UX). Smoke-tested end-to-end on the kortexa bar panel with hello-embedded.

---

## 1. Goal

A `linux-wpe` kiosk on a Pi receives updates the same way the macOS/Windows builds do — drop a new tarball on the update host, the device picks it up on next check, applies it, and restarts cleanly. If the new build crashes on first launch, the device rolls back to the previous version automatically. No manual intervention, no black screens, no SSH-in-and-fix.

The same OTA machinery also has to keep working in **TTY-mode dev workflows** — a developer who installed the build with `--no-kiosk` (or used `Electrobun.Kiosk.uninstall()` at runtime) just runs `~/.local/share/{id}/{channel}/current/bin/launcher` from a tty shell. Apply path detects the absence of a supervisor and self-execs the new launcher in place; rollback still works because it lives in the launcher's own startup, not in a systemd `ExecStartPre`.

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

1. ~~**Restart strategy is wrong for a kiosk.**~~ *(Phase B — pending.)* `Updater.applyUpdate` on Linux today does `spawn(launcher, { detached: true }); exit(0)` (`Updater.ts:1062`). On bare DRM, the old process must release DRM master before the new one can grab it; spawn-and-exit races. There's no window manager to re-activate, no taskbar, no user click to relaunch.
2. ~~**No supervisor.**~~ ✅ *(Phase A — landed.)* The extractor now writes a systemd user unit at install time and enables linger so the kiosk comes up at boot without any login session.
3. ~~**No rollback.**~~ *(Phase C — pending.)* Updater preserves the previous tarball but never restores it. If a bad build crashes on startup, systemd `Restart=on-failure` boot-loops a black screen.
4. ~~**No pre-update hook.**~~ *(Phase B — pending.)* Kiosk apps may want to flush state before going down; macOS/Windows get away without one because the user clicks Quit.
5. *(Added in v2.)* **No operational controls.** First install is hard-coded to "kiosk mode (with systemd unit + linger + enable-now)". A developer wanting to run the same app from a TTY for testing has no clean opt-out, and a running app can't toggle kiosk mode at runtime. *(Phase A.5 — pending.)*

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

### 4.4 Auto-rollback — sentinel + launcher startup probe

Two cooperating pieces, both inside the launcher itself:

**(a) Launcher writes a "boot succeeded" sentinel.** After `WpeBackend::primeWpeView()` returns successfully and the GLib main loop has spun for ~3 seconds without exiting, the launcher writes `state/last-good` = current hash. This proves the new version reached steady-state rendering, not just process-started.

**(b) Launcher startup probe checks for rollback.** Before priming WPE, the launcher itself runs:

```
if state/boot-attempt exists and != state/last-good and current points to boot-attempt:
    # Last attempt didn't reach last-good. Roll back.
    swap current ↔ previous
    clear boot-attempt
    log to journal
    re-exec the (new) current/bin/launcher
```

**Why in the launcher, not a systemd `ExecStartPre`:** the rollback works whether the launcher was started by systemd or by a developer running `./bin/launcher` from a TTY. A `ExecStartPre` hook would only fire under systemd — TTY-mode dev workflow would silently lose rollback semantics, and the unit-file vs no-unit-file split would be a constant source of "works on my Pi". Putting the probe in the launcher itself collapses the two modes into one code path.

This makes rollback recovery automatic on the *next* start attempt. Combined with `StartLimitBurst=3` under systemd (or a manual re-run from TTY), the kiosk gets up to 3 tries on the new version, and if none of them write `last-good`, the next start cycle rolls back. After rollback, the previous version starts and (presumably) writes its own `last-good` again — system stable.

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

### 4.7 Operational controls — installer flags + runtime API

Phase A's "default-to-kiosk install" was the right starting point but baked in an assumption — that every linux-wpe build *is* a kiosk. Two real workflows break that:

1. **Developer running on a dev Pi** wants to drop the kiosk service and launch the binary by hand from a TTY for testing.
2. **App switches modes at runtime** — e.g., a settings UI flips between "kiosk on boot" and "manual launch" without re-running the installer.

Both need the same underlying primitives — write/remove the unit file, enable/disable the service, toggle linger — exposed at two surfaces:

**Installer CLI flags** (`extractor/main.zig`):

| Flag | Effect |
|---|---|
| `./installer` *(no flag)* | unchanged — kiosk install (current Phase A behavior) |
| `./installer --no-kiosk` | extract files only, skip systemd unit + linger + enable |
| `./installer --uninstall` | reverse: stop, disable, rm unit, rm `~/.local/share/{id}/{channel}/` |
| `./installer --uninstall --keep-data` | as above but preserve user data |
| `./installer --help` | usage |

**Runtime API** (`package/src/bun/core/Kiosk.ts`):

```ts
import { Kiosk } from "electrobun/bun";
await Kiosk.install();    // write unit + enable-linger + enable --now
await Kiosk.uninstall();  // disable --now + rm unit + daemon-reload
Kiosk.isInstalled();      // unit file exists?
Kiosk.isEnabled();        // systemctl is-enabled?
```

Linux-only; on mac/win/desktop-Linux these throw `"not supported on this platform"`. Implementation shells out to `systemctl --user` / `loginctl` via `Bun.spawn` — no FFI, no native code. The unit template is a literal string in both the Zig extractor and the TS module; drift risk is low (~10-line template).

**Source of truth.** Two signals — unit file exists *and* `systemctl --user is-enabled` returns enabled — both should agree. `Kiosk.isInstalled()` checks both and returns false on disagreement; `Kiosk.install()` is idempotent and overwrites/repairs.

**Self-uninstall while running.** Legitimate ("settings → disable kiosk"). Order: disable → rm unit → daemon-reload → return to caller. The current process keeps running until it exits naturally; no auto-restart afterward.

---

## 5. Phasing

Each phase is independently shippable and testable.

### Phase A — service-managed launch ✅ *landed 2026-04-30*

Goal: a fresh install of a linux-embedded build comes up under systemd, restarts on crash, survives reboot via linger.

- ✅ `cli/index.ts`: writes `linuxTarget: "embedded"` into `version.json` and `metadata.json` when `build.linux.embedded`.
- ✅ `extractor/main.zig`: when metadata says `linuxTarget: "embedded"`, after extracting:
  - Creates the symlinked layout (`current` → `app.{hash}/`, no `previous` yet — Phase B will add it).
  - Generates the systemd unit from a literal template (no marker; ~10-line string in main.zig was simpler than embedding via a binary marker).
  - Runs `loginctl enable-linger {user}` (idempotent, best-effort).
  - Runs `systemctl --user daemon-reload && systemctl --user enable --now {id}`.
  - Skips the `.desktop` shortcut for embedded builds.

**Bug fixes surfaced during smoke-test** (also landed in Phase A's commits, technically pre-existing in the multi-view linux-wpe code):
- `bc4d90b5` — composite `frame_complete` was gated on a previous `pendingShm_`, so the very first frame of every view was never acked back to WPE-FDO and the renderer stalled forever after one paint per view. Now `frame_complete` fires on every export.
- `bc4d90b5` — `BrowserWindow.frame` is fictional on bare-DRM (no compositor, single window = the panel) but `createWebview` was honoring it as if it were a desktop position+size. Now the implicit primary view of each window is forced to fill the panel; explicit `BrowserView` frames stay honored.
- `bc4d90b5` — `alwaysTopmost` views (the `__electrobun_chrome__` partition convention) on bare-DRM now span panel width — same portability story.
- `bc4d90b5` — `views://` scheme now registered as secure + cors-enabled via the WebKit security manager, fixing DOM Storage / IndexedDB on app-bundled pages (silently no-op'd before).
- `6178c6aa` — auto-injected chrome's "tap anywhere to restore" gesture conflicted with normal touch UX; restore is now scoped to a top-edge tap.

### Phase A.5 — operational controls *(pending)*

Goal: kiosk-by-default but explicitly opt-out-able, both at install time and at runtime.

- `extractor/main.zig`: argv parser for `--no-kiosk`, `--uninstall`, `--uninstall --keep-data`, `--help`. Uninstall path (new) stops + disables service, rm's unit, rm's app dirs (preserving data with `--keep-data`).
- `package/src/bun/core/Kiosk.ts`: new TS module exposing `install()`, `uninstall()`, `isInstalled()`, `isEnabled()`. Implementation via `Bun.spawn` — no FFI, no native changes.

Why before Phase B: lets developers run the same build in TTY mode for testing without committing to systemd, AND establishes the supervisor-vs-unsupervised distinction Phase B's apply path needs.

Test: `./installer --no-kiosk` on a clean Pi → no unit installed → manually run `~/.local/share/.../current/bin/launcher` from a TTY → kiosk renders. Then `Kiosk.install()` from a running app → unit appears + service starts.

### Phase B — service-aware OTA apply *(pending)*

Goal: an update lands while the kiosk is running (or a dev TTY launcher is running), swaps cleanly, the new version comes up.

- `Updater.applyUpdate`: branch on `linuxTarget === "embedded"`, implement §4.3 (versioned-dir extract + atomic symlink flip).
- After the flip, detect supervisor via `INVOCATION_ID`/`JOURNAL_STREAM` env vars. Two paths:
  - **Supervised** (systemd present): `systemctl --user restart {id}` and exit cleanly. systemd brings up the new launcher.
  - **Unsupervised** (`--no-kiosk` install or manual TTY launch): self-`exec` `current/bin/launcher` to replace the current process image. PID stays the same; user's shell sees a continuously-running process running new code.
- Add `beforeUpdate` event (§4.5).
- `WpeBackend`: verify `drmDropMaster` happens before process exit so the next instance can grab master cleanly. (Per the design doc this already happens in the dtor; verify under SIGTERM from systemd as a Phase B diagnostic.)

Test: run hello-embedded under systemd, push a new version with a visible change, watch it apply and restart on the panel without manual intervention. Then repeat with `--no-kiosk` install + manual TTY launch — same result via self-exec.

### Phase C — auto-rollback *(pending)*

Goal: a deliberately-broken update auto-rolls back within a few startup attempts.

- Launcher: write `state/last-good` after `primeWpeView` + ~3s steady-state.
- Launcher: startup probe (§4.4) — *not* a systemd `ExecStartPre`, but inline at the start of the launcher's own `main`. Works under systemd AND under TTY-mode manual relaunch.
- Updater: maintain `previous` symlink and GC of older `app.{hash}/` dirs.

Test: ship a "broken" build that exits 1 immediately. Under systemd: confirm three crashes, then a rollback to the prior hash, steady operation thereafter. Under TTY: re-run the launcher 2–3 times manually, confirm rollback flips on the next start. Verify journal log makes the rollback obvious to a human.

---

## 6. Open / deferred

- **Read-only rootfs split** (`/opt/{id}` immutable, `/var/lib/{id}` writable): not v1. Add when there's a real device using it.
- **Background download**: today the updater is invoked explicitly from app code. Whether the kiosk should poll on a timer is an app-level concern, not an OTA-architecture concern.
- **Signed updates**: out of scope for v1. The existing Updater has no signature verification on any platform; that's a separate cross-platform feature.
- **Multiple channels on one device** (dev + canary side-by-side): already supported by the per-channel directory structure; no work needed.
- **Crash detection during steady-state** (kiosk runs fine for an hour, then segfaults): handled by `Restart=on-failure`, *not* by rollback — a steady-state crash isn't an update-induced regression by definition. We only roll back failed *first launches*.

---

## 7. Files touched (summary)

- ✅ `package/src/cli/index.ts` — wrote `linuxTarget` into version.json/metadata.json. Phase A (landed `487537c2`).
- ✅ `package/src/extractor/main.zig` — versioned-dir layout + systemd unit + linger + enable-now on first install; skips `.desktop` for embedded. Phase A (landed `487537c2`). **Phase A.5 will add** argv parsing for `--no-kiosk` / `--uninstall` and a new uninstall code path.
- ✅ `package/src/native/linux/wpe/wpe_backend.cpp` — composite frame_complete fix, BrowserWindow primary fills panel, alwaysTopmost spans panel width, views:// registered as secure + cors-enabled. Phase A bug fixes (landed `bc4d90b5`).
- ✅ `package/src/bun/preload/chrome.ts` — top-edge restore zone + 250ms cooldown so the same tap can't toggle hide↔show. Phase A UX fix (landed `6178c6aa`).
- *new* `package/src/bun/core/Kiosk.ts` — runtime API (`install` / `uninstall` / `isInstalled` / `isEnabled`) shelling out to `systemctl --user` via `Bun.spawn`. Phase A.5.
- `package/src/bun/core/Updater.ts` — branch on `linuxTarget === "embedded"` in `applyUpdate`, supervised-vs-unsupervised dispatch, add `beforeUpdate` event, maintain `previous` symlink + GC. Phases B–C.
- `package/src/launcher/main.zig` — startup rollback probe (Phase C) + write `state/last-good` after steady-state (Phase C).
- `package/src/native/linux/wpe/...` — verify SIGTERM cleanup releases DRM master. Phase B (verification only, no new code expected).
