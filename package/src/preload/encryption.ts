// Encryption/Decryption for secure RPC
// Uses per-webview secret key set in window.__electrobunSecretKeyBytes

import "./globals.d.ts";

function base64ToUint8Array(base64: string): Uint8Array {
	const binary = atob(base64);
	if (window.__electrobunPlatform === "linux") {
		// WebKitGTK becomes allocation-bound on large RPC bursts if every byte
		// is first expanded into a split/map array.
		const bytes = new Uint8Array(binary.length);
		for (let i = 0; i < binary.length; i++) {
			bytes[i] = binary.charCodeAt(i);
		}
		return bytes;
	}

	return new Uint8Array(binary.split("").map((char) => char.charCodeAt(0)));
}

function uint8ArrayToBase64(uint8Array: Uint8Array): string {
	let binary = "";
	for (let i = 0; i < uint8Array.length; i++) {
		binary += String.fromCharCode(uint8Array[i]!);
	}
	return btoa(binary);
}

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
	const buffer = new ArrayBuffer(bytes.byteLength);
	new Uint8Array(buffer).set(bytes);
	return buffer;
}

async function generateKeyFromBytes(rawKey: Uint8Array): Promise<CryptoKey> {
	return await window.crypto.subtle.importKey(
		"raw",
		toArrayBuffer(rawKey),
		{ name: "AES-GCM" },
		true,
		["encrypt", "decrypt"],
	);
}

export async function initEncryption(): Promise<void> {
	const secretKey = await generateKeyFromBytes(
		new Uint8Array(window.__electrobunSecretKeyBytes),
	);

	const encryptString = async (
		plaintext: string,
	): Promise<{ encryptedData: string; iv: string; tag: string }> => {
		const encoder = new TextEncoder();
		const encodedText = encoder.encode(plaintext);
		const iv = window.crypto.getRandomValues(new Uint8Array(12));
		const encryptedBuffer = await window.crypto.subtle.encrypt(
			{ name: "AES-GCM", iv: toArrayBuffer(iv) },
			secretKey,
			toArrayBuffer(encodedText),
		);

		// Split the tag (last 16 bytes) from the ciphertext
		const encryptedData = new Uint8Array(encryptedBuffer.slice(0, -16));
		const tag = new Uint8Array(encryptedBuffer.slice(-16));

		return {
			encryptedData: uint8ArrayToBase64(encryptedData),
			iv: uint8ArrayToBase64(iv),
			tag: uint8ArrayToBase64(tag),
		};
	};

	const decryptString = async (
		encryptedDataB64: string,
		ivB64: string,
		tagB64: string,
	): Promise<string> => {
		const encryptedData = base64ToUint8Array(encryptedDataB64);
		const iv = base64ToUint8Array(ivB64);
		const tag = base64ToUint8Array(tagB64);

		// Combine encrypted data and tag to match the format expected by SubtleCrypto
		const combinedData = new Uint8Array(encryptedData.length + tag.length);
		combinedData.set(encryptedData);
		combinedData.set(tag, encryptedData.length);

		const decryptedBuffer = await window.crypto.subtle.decrypt(
			{ name: "AES-GCM", iv: toArrayBuffer(iv) },
			secretKey,
			toArrayBuffer(combinedData),
		);

		const decoder = new TextDecoder();
		return decoder.decode(decryptedBuffer);
	};

	// Binary packet variants used by the host WebSocket transport.
	// Layout: iv(12) | ciphertext | tag(16) — the tail is exactly
	// SubtleCrypto's native ciphertext||tag output, so encrypt just prepends
	// the iv and decrypt just strips it. No base64, no JSON envelope.
	const encryptBinary = async (plaintext: string): Promise<ArrayBuffer> => {
		const encoded = new TextEncoder().encode(plaintext);
		const iv = window.crypto.getRandomValues(new Uint8Array(12));
		const encrypted = await window.crypto.subtle.encrypt(
			{ name: "AES-GCM", iv },
			secretKey,
			encoded,
		);
		const packet = new Uint8Array(12 + encrypted.byteLength);
		packet.set(iv);
		packet.set(new Uint8Array(encrypted), 12);
		return packet.buffer;
	};

	const decryptBinary = async (packet: ArrayBuffer): Promise<string> => {
		const bytes = new Uint8Array(packet);
		if (bytes.length < 12 + 16) {
			throw new Error("Transport packet too short");
		}
		const iv = bytes.subarray(0, 12);
		const ciphertextAndTag = bytes.subarray(12);
		const decrypted = await window.crypto.subtle.decrypt(
			{ name: "AES-GCM", iv: iv as unknown as ArrayBuffer },
			secretKey,
			ciphertextAndTag as unknown as ArrayBuffer,
		);
		return new TextDecoder().decode(decrypted);
	};

	window.__electrobun_encrypt = encryptString;
	window.__electrobun_decrypt = decryptString;
	window.__electrobun_encrypt_binary = encryptBinary;
	window.__electrobun_decrypt_binary = decryptBinary;
}
