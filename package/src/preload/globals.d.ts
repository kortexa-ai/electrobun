// Type declarations for Electrobun preload globals
// These are set dynamically per-webview before the preload script runs

declare global {
	interface Window {
		__electrobunPlatform: "linux" | "macos" | "windows";
		__electrobunWebviewId: number;
		__electrobunWindowId: number;
		__electrobunRpcSocketPort: number;
		__electrobunHostSocketPort?: number;
		__electrobunPlaintextHostSocket?: boolean;
		__electrobunSecretKeyBytes: number[];
		// Event-only bridge (all webviews, including sandboxed)
		__electrobunEventBridge?: {
			postMessage: (message: string) => void;
		};
		// Internal RPC bridge (trusted webviews only)
		__electrobunInternalBridge?: {
			postMessage: (message: string) => void;
		};
		// User RPC bridge (trusted webviews only)
		__electrobunHostBridge?: {
			postMessage: (message: string) => void;
		};
		__electrobunBunBridge?: {
			postMessage: (message: string) => void;
		};
		__electrobun_encrypt: (
			plaintext: string,
		) => Promise<{ encryptedData: string; iv: string; tag: string }>;
		__electrobun_decrypt: (
			encryptedData: string,
			iv: string,
			tag: string,
		) => Promise<string>;
		// Binary packet variants (iv(12) | ciphertext | tag(16)) used by the
		// host WebSocket transport. Optional: older preloads don't set them.
		__electrobun_encrypt_binary?: (plaintext: string) => Promise<ArrayBuffer>;
		__electrobun_decrypt_binary?: (packet: ArrayBuffer) => Promise<string>;
		__electrobunSendToHost: (message: unknown) => void;
		__electrobunPendingHostMessages?: unknown[];
		// titleBarStyle from BrowserWindow options. The page reads it only for
		// informational purposes (e.g. an app's own custom chrome could behave
		// differently in inset mode) — the auto-inject decision is made in
		// __electrobunAutoInjectChrome below.
		__electrobunTitleBarStyle?: "default" | "hidden" | "hiddenInset";
		// Computed from (platform, build target, titleBarStyle). True
		// only when the framework should inject its own chrome bar — i.e. on
		// linux-embedded with titleBarStyle "default", where there is no OS
		// chrome to fall back on. chrome.ts reads only this boolean and stays
		// platform-agnostic itself.
		__electrobunAutoInjectChrome?: boolean;
		__electrobun: {
			receiveMessageFromHost: (msg: unknown) => void;
			receiveInternalMessageFromHost: (msg: unknown) => void;
			receiveMessageFromBun: (msg: unknown) => void;
			receiveInternalMessageFromBun: (msg: unknown) => void;
		};
	}
}

export {};
