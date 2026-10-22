import { afterEach, describe, expect, test } from "bun:test";
import {
	mkdtempSync,
	rmSync,
	writeFileSync,
} from "fs";
import { tmpdir } from "os";
import { join } from "path";
import {
	assertRuntimeArtifacts,
	createRuntimeManifest,
} from "./runtime-manifest";

let tempDir = "";

afterEach(() => {
	if (tempDir) rmSync(tempDir, { recursive: true, force: true });
	tempDir = "";
});

describe("runtime artifact manifest", () => {
	test("accepts the exact artifacts used to create it", () => {
		tempDir = mkdtempSync(join(tmpdir(), "electrobun-runtime-manifest-"));
		const core = join(tempDir, "libElectrobunCore.so");
		const wrapper = join(tempDir, "libNativeWrapper_wpe.so");
		writeFileSync(core, "core-a");
		writeFileSync(wrapper, "wrapper-a");

		const artifacts = { core, "native-wrapper-wpe": wrapper };
		const manifest = createRuntimeManifest("linux-arm64", artifacts);

		expect(() => assertRuntimeArtifacts(manifest, artifacts)).not.toThrow();
	});

	test("rejects an artifact copied from another build", () => {
		tempDir = mkdtempSync(join(tmpdir(), "electrobun-runtime-manifest-"));
		const core = join(tempDir, "libElectrobunCore.so");
		const wrapper = join(tempDir, "libNativeWrapper_wpe.so");
		writeFileSync(core, "core-a");
		writeFileSync(wrapper, "wrapper-a");

		const artifacts = { core, "native-wrapper-wpe": wrapper };
		const manifest = createRuntimeManifest("linux-arm64", artifacts);
		writeFileSync(core, "core-b");

		expect(() => assertRuntimeArtifacts(manifest, artifacts)).toThrow(
			"Electrobun runtime artifacts are mixed",
		);
	});
});
