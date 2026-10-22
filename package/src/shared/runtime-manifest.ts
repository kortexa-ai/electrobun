import { createHash } from "crypto";
import {
	existsSync,
	readFileSync,
	statSync,
	type PathLike,
} from "fs";
import { basename } from "path";

export const RUNTIME_MANIFEST_FILENAME = "runtime-manifest.json";
export const RUNTIME_MANIFEST_SCHEMA_VERSION = 1;

export type RuntimeManifest = {
	schemaVersion: typeof RUNTIME_MANIFEST_SCHEMA_VERSION;
	platform: string;
	buildId: string;
	artifacts: Record<
		string,
		{
			file: string;
			size: number;
			sha256: string;
		}
	>;
};

export function sha256File(filePath: PathLike): string {
	return createHash("sha256")
		.update(Uint8Array.from(readFileSync(filePath)))
		.digest("hex");
}

export function createRuntimeManifest(
	platform: string,
	artifactPaths: Record<string, string>,
): RuntimeManifest {
	const artifacts: RuntimeManifest["artifacts"] = {};

	for (const [name, filePath] of Object.entries(artifactPaths).sort(([a], [b]) =>
		a.localeCompare(b),
	)) {
		if (!existsSync(filePath)) continue;
		artifacts[name] = {
			file: basename(filePath),
			size: statSync(filePath).size,
			sha256: sha256File(filePath),
		};
	}

	const buildId = createHash("sha256")
		.update(
			Object.entries(artifacts)
				.map(([name, artifact]) => `${name}:${artifact.sha256}`)
				.join("\n"),
		)
		.digest("hex");

	return {
		schemaVersion: RUNTIME_MANIFEST_SCHEMA_VERSION,
		platform,
		buildId,
		artifacts,
	};
}

export function assertRuntimeArtifacts(
	manifest: RuntimeManifest,
	artifactPaths: Record<string, string>,
): void {
	const problems: string[] = [];

	if (manifest.schemaVersion !== RUNTIME_MANIFEST_SCHEMA_VERSION) {
		problems.push(
			`unsupported manifest schema ${manifest.schemaVersion}; expected ${RUNTIME_MANIFEST_SCHEMA_VERSION}`,
		);
	}

	for (const [name, filePath] of Object.entries(artifactPaths)) {
		const expected = manifest.artifacts[name];
		if (!expected) {
			problems.push(`${name}: missing from runtime manifest`);
			continue;
		}
		if (!existsSync(filePath)) {
			problems.push(`${name}: artifact is missing (${filePath})`);
			continue;
		}

		const actualSize = statSync(filePath).size;
		if (actualSize !== expected.size) {
			problems.push(
				`${name}: size mismatch (expected ${expected.size}, got ${actualSize})`,
			);
			continue;
		}

		const actualHash = sha256File(filePath);
		if (actualHash !== expected.sha256) {
			problems.push(
				`${name}: SHA-256 mismatch (expected ${expected.sha256}, got ${actualHash})`,
			);
		}
	}

	if (problems.length > 0) {
		throw new Error(`Electrobun runtime artifacts are mixed:\n- ${problems.join("\n- ")}`);
	}
}
