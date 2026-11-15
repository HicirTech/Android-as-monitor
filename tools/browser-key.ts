// The key a screen must present before tools/browser-relay.ts joins it to the stream. An address list in front of the relay can be
// talked past, for example by a proxy that trusts X-Real-IP from CDN ranges; a key cannot. Each PC makes its own and keeps it
// outside the repository.
import { createHash, randomInt, timingSafeEqual } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join } from "node:path";
import { KEY_PROTOCOL_PREFIX } from "../browser/wire.js";

// No l, o, 0 or 1, which look alike on a touch keyboard.
const KEY_ALPHABET = "abcdefghijkmnpqrstuvwxyz23456789";
// 80 bits, typed once on the screen and then bookmarked, written in groups of four so it is easier to read off.
const KEY_LENGTH = 16;
const KEY_GROUP = 4;

// Case, spaces and dashes do not count, so a key typed on a touch keyboard matches however it was grouped or capitalised.
export function normalizeKey(text: string): string {
  return text.toLowerCase().replace(/[\s-]/g, "");
}

// In the home folder rather than AppData: Windows redirects what a packaged app's processes write to AppData into the package's
// own folder, so a relay started from one would keep its key where the user never looks, and read that one instead of theirs.
export function defaultKeyPath(): string {
  return join(homedir(), ".android-usb-monitor", "browser-key.txt");
}

export function loadOrCreateKey(path: string): string {
  try {
    const key = readFileSync(path, "utf8").split(/\r?\n/)[0].trim();
    if (key) {
      return key;
    }
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") {
      throw error;
    }
  }
  const characters = Array.from({ length: KEY_LENGTH }, () => KEY_ALPHABET[randomInt(KEY_ALPHABET.length)]).join("");
  const key = characters.match(new RegExp(`.{1,${KEY_GROUP}}`, "g"))!.join("-");
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, `${key}\n`, { mode: 0o600 });
  return key;
}

// The subprotocol that carries a key, from a Sec-WebSocket-Protocol header, and the key in it.
export function offeredKey(header: string | null): { protocol: string; key: string } | null {
  const protocol = (header ?? "").split(",").map((part) => part.trim()).find((part) => part.startsWith(KEY_PROTOCOL_PREFIX));
  return protocol ? { protocol, key: protocol.slice(KEY_PROTOCOL_PREFIX.length) } : null;
}

// Compares digests, so neither the length nor the first differing character shows in the time it takes.
export function keyMatches(offered: string, key: string): boolean {
  const digest = (text: string) => createHash("sha256").update(normalizeKey(text)).digest();
  return normalizeKey(offered).length > 0 && timingSafeEqual(digest(offered), digest(key));
}

// What kind of mistake a refused key is, for the relay's log. It compares lengths only: counting the characters that differ would
// let whoever reads the log learn the key one guess at a time.
export function describeMismatch(offered: string, key: string): string {
  const given = normalizeKey(offered);
  const foreign = Array.from(given).filter((character) => !KEY_ALPHABET.includes(character)).length;
  const parts = [`${given.length} characters where the key has ${normalizeKey(key).length}`];
  if (foreign > 0) {
    parts.push(`${foreign} of them cannot be in a generated key`);
  }
  return parts.join(", ");
}
