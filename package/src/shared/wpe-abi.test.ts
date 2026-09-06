import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";

const core = readFileSync(new URL("../core/main.zig", import.meta.url), "utf8");
const native = readFileSync(new URL("../sdks/main/proc/native.ts", import.meta.url), "utf8");

test("published webview ABI remains compatible with native-language SDKs", () => {
    for (const [name, count] of Object.entries({
        configureWebviewRuntime: 3, configureWebviewRuntimeV2: 4,
        createWebview: 22, createWebviewV2: 23,
    })) {
        const signature = core.match(new RegExp(`export fn ${name}\\(([\\s\\S]*?)\\) (?:bool|u32) \\{`))![1]!;
        expect(signature.split(",").filter((arg) => arg.trim()).length).toBe(count);
    }
    expect(native).toContain("symbols.configureWebviewRuntimeV2(");
    expect(native).toContain("symbols.createWebviewV2(");
});
