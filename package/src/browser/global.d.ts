// Global type declarations for Electrobun browser environment

interface ElectrobunEncryptResult {
  encryptedData: string;
  iv: string;
  tag: string;
}

interface ElectrobunBridge {
  receiveMessageFromHost: (msg: unknown) => void;
  receiveInternalMessageFromHost: (msg: unknown) => void;
  receiveMessageFromBun: (msg: unknown) => void;
  receiveInternalMessageFromBun: (msg: unknown) => void;
}

interface MessageHandler {
  postMessage: (msg: string) => void;
}

declare global {
  interface Window {
    __electrobunPlatform: "linux" | "macos" | "windows";
    __electrobunWebviewId: number;
    __electrobunWindowId: number;
    __electrobunRpcSocketPort: number;
    __electrobunHostSocketPort?: number;
    __electrobunPlaintextHostSocket?: boolean;
    __electrobun?: ElectrobunBridge;
    __electrobunPendingHostMessages?: unknown[];
    __electrobun_encrypt: (msg: string) => Promise<ElectrobunEncryptResult>;
    __electrobun_decrypt: (encryptedData: string, iv: string, tag: string) => Promise<string>;
    // Binary packet variants (iv(12) | ciphertext | tag(16)) used by the
    // host WebSocket transport. Optional: older preloads don't set them.
    __electrobun_encrypt_binary?: (plaintext: string) => Promise<ArrayBuffer>;
    __electrobun_decrypt_binary?: (packet: ArrayBuffer) => Promise<string>;
    __electrobunInternalBridge?: MessageHandler;
    __electrobunHostBridge?: MessageHandler;
    __electrobunBunBridge?: MessageHandler;
    __electrobunSendToHost: (message: unknown) => void;
  }
}

export {};
