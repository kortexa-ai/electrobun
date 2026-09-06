import { expect, test } from "bun:test";

test("binary and legacy RPC preserve arrival order with concurrent decryption", async () => {
    class Socket extends EventTarget {
        static CONNECTING = 0;
        static OPEN = 1;
        readyState = 1;
        send() {}
    }
    const pending: Array<(value: string) => void> = [];
    const previousWindow = globalThis.window;
    const previousSocket = globalThis.WebSocket;
    Object.assign(globalThis, { WebSocket: Socket, window: {
        __electrobunWebviewId: 1, __electrobunHostSocketPort: 1234,
        __electrobunPlatform: "linux", __electrobun: {},
        __electrobun_decrypt_binary: () => new Promise<string>((r) => pending.push(r)),
        __electrobun_decrypt: () => new Promise<string>((r) => pending.push(r)),
    } });
    try {
        const { Electroview } = await import("./index");
        const view = new Electroview({});
        const received: number[] = [];
        view.rpcHandler = (message: any) => received.push(message.sequence);
        const drain = () => new Promise((r) => setTimeout(r, 5));
        for (const data of [new ArrayBuffer(32), JSON.stringify({ encryptedData: "x", iv: "x", tag: "x" })]) {
            pending.length = received.length = 0;
            view.hostSocket!.dispatchEvent(new MessageEvent("message", { data }));
            view.hostSocket!.dispatchEvent(new MessageEvent("message", { data }));
            pending[1]!(JSON.stringify({ sequence: 2 }));
            await drain();
            expect(received).toEqual([]);
            pending[0]!(JSON.stringify({ sequence: 1 }));
            await drain();
            expect(received).toEqual([1, 2]);
        }
    } finally {
        Object.assign(globalThis, { window: previousWindow, WebSocket: previousSocket });
    }
});
