// Runtime API for promoting / demoting a linux-wpe app between kiosk
// and TTY-launch modes (Phase A.5 of the OTA design).
//
// Kiosk mode = a systemd user service that auto-starts the launcher at
// boot, with linger so it doesn't need a logged-in session, and
// Restart=on-failure for crash recovery. The Zig extractor sets this up
// at first install; this module is the runtime equivalent so an app's
// settings UI can flip the switch without re-running the installer.
//
// Linux-only. mac/win/desktop-Linux don't have an equivalent concept
// (or have very different mechanisms — LaunchAgents, Task Scheduler);
// these methods throw on non-linux.

import { existsSync, mkdirSync, writeFileSync, unlinkSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
import { OS } from "../../../shared/platform";

// Mirrors the systemd unit template emitted by extractor/main.zig at install
// time. Hand-kept in sync — it's a 10-line string, drift risk is low. If you
// change one, change the other.
function unitText(displayName: string, launcherPath: string): string {
	return `[Unit]
Description=${displayName} (Electrobun kiosk)
After=graphical-session.target

[Service]
Type=simple
ExecStart=${launcherPath}
Restart=on-failure
RestartSec=2
StartLimitIntervalSec=30
StartLimitBurst=3
TimeoutStopSec=5

[Install]
WantedBy=default.target
`;
}

interface AppMetadata {
	identifier: string;
	channel: string;
	name: string;
}

let _meta: AppMetadata | null = null;
function getMeta(): AppMetadata {
	if (_meta) return _meta;
	// Launcher's cwd is .../app.{hash}/bin, so version.json sits at ../Resources/.
	const versionPath = join("..", "Resources", "version.json");
	const raw = readFileSync(versionPath, "utf-8");
	const parsed = JSON.parse(raw);
	_meta = {
		identifier: parsed.identifier,
		channel: parsed.channel,
		name: parsed.name,
	};
	return _meta;
}

function unitDir(): string {
	return join(homedir(), ".config", "systemd", "user");
}

function unitFilename(): string {
	return `${getMeta().identifier}.service`;
}

function unitPath(): string {
	return join(unitDir(), unitFilename());
}

function launcherPath(): string {
	const { identifier, channel } = getMeta();
	return join(homedir(), ".local", "share", identifier, channel, "current", "bin", "launcher");
}

interface RunResult {
	ok: boolean;
	exitCode: number;
	stderr: string;
}

function run(argv: string[]): RunResult {
	const result = Bun.spawnSync(argv);
	const exitCode = result.exitCode ?? -1;
	return {
		ok: exitCode === 0,
		exitCode,
		stderr: result.stderr ? result.stderr.toString() : "",
	};
}

function notSupported(action: string): never {
	throw new Error(`Kiosk.${action}: not supported on ${OS}. Linux-WPE / linux-embedded only.`);
}

export interface InstallOptions {
	/**
	 * Also start the service immediately after enabling it.
	 *
	 * Default: `false`. If you call `install()` from a running app, the
	 * caller process is already holding DRM master — starting another
	 * service instance would race for it and lose, then hit the
	 * StartLimitBurst and give up. Leave this off; the service will come
	 * up cleanly on next boot, or after the caller exits.
	 *
	 * Pass `true` only when you know nothing else is running on this
	 * panel (e.g. a one-shot setup script).
	 */
	startNow?: boolean;
}

/**
 * Install the systemd user unit for this app and enable it at boot.
 *
 * Idempotent — safe to call when already installed (overwrites the unit
 * and re-runs daemon-reload + enable). Best-effort on `loginctl
 * enable-linger`: failures are silent, the user can run it manually.
 */
export function install(opts: InstallOptions = {}): void {
	if (OS !== "linux") notSupported("install");

	const { name } = getMeta();

	mkdirSync(unitDir(), { recursive: true });
	writeFileSync(unitPath(), unitText(name, launcherPath()));

	const user = process.env["USER"];
	if (user) run(["loginctl", "enable-linger", user]);

	run(["systemctl", "--user", "daemon-reload"]);
	run(["systemctl", "--user", "enable", unitFilename()]);
	if (opts.startNow) run(["systemctl", "--user", "start", unitFilename()]);
}

export interface UninstallOptions {
	/**
	 * Also stop the service immediately. **Will kill the calling process
	 * if it is itself the service's MainPID** — typical settings-UI flow
	 * is to leave this off and let the next boot honor the disabled state.
	 *
	 * Default: `false`.
	 */
	stopNow?: boolean;
}

/**
 * Disable the systemd user unit and remove it.
 *
 * Idempotent — safe to call when nothing is installed (best-effort
 * operations swallow ENOENT/etc). Does **not** stop the currently
 * running process by default; pass `{ stopNow: true }` to also stop the
 * service (which kills the caller if it's the service's MainPID).
 */
export function uninstall(opts: UninstallOptions = {}): void {
	if (OS !== "linux") notSupported("uninstall");

	if (opts.stopNow) run(["systemctl", "--user", "stop", unitFilename()]);
	run(["systemctl", "--user", "disable", unitFilename()]);
	try {
		unlinkSync(unitPath());
	} catch {
		// ENOENT — already gone, fine.
	}
	run(["systemctl", "--user", "daemon-reload"]);
}

/**
 * True if the unit file is present at the expected path AND `systemctl
 * --user is-enabled` agrees. Returns false on disagreement (treat as
 * "broken state" — call `install()` to repair, or `uninstall()` to clean).
 */
export function isInstalled(): boolean {
	if (OS !== "linux") return false;
	if (!existsSync(unitPath())) return false;
	return run(["systemctl", "--user", "is-enabled", unitFilename()]).ok;
}

/**
 * True if the service is currently active (running). Distinct from
 * `isInstalled()` (which checks enabled-at-boot). A typical kiosk has
 * both true; a TTY-launched app has both false.
 */
export function isActive(): boolean {
	if (OS !== "linux") return false;
	return run(["systemctl", "--user", "is-active", unitFilename()]).ok;
}
