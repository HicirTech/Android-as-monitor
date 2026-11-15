import { describe, expect, test } from "bun:test";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { KEY_PROTOCOL_PREFIX } from "../browser/wire.js";
import { defaultKeyPath, describeMismatch, keyMatches, loadOrCreateKey, offeredKey } from "./browser-key.ts";

const scratch = () => mkdtempSync(join(tmpdir(), "aum-key-"));

describe("the key file", () => {
  test("lives in the home folder, which Windows never redirects for packaged apps the way it does AppData", () => {
    expect(defaultKeyPath().startsWith(homedir())).toBe(true);
    expect(defaultKeyPath()).not.toContain("AppData");
  });

  test("a missing file gets a fresh key that later starts reuse", () => {
    const path = join(scratch(), "nested", "browser-key.txt");
    const key = loadOrCreateKey(path);
    expect(key).toMatch(/^[a-km-np-z2-9]{4}(-[a-km-np-z2-9]{4}){3}$/);
    expect(readFileSync(path, "utf8").trim()).toBe(key);
    expect(loadOrCreateKey(path)).toBe(key);
  });

  test("a key the user wrote is kept, whatever line ending follows it", () => {
    const path = join(scratch(), "browser-key.txt");
    writeFileSync(path, "chosen-by-hand\r\nnotes\r\n");
    expect(loadOrCreateKey(path)).toBe("chosen-by-hand");
  });

  test("two PCs get different keys", () => {
    expect(loadOrCreateKey(join(scratch(), "k.txt"))).not.toBe(loadOrCreateKey(join(scratch(), "k.txt")));
  });
});

describe("what a screen offers", () => {
  test("the key comes out of the subprotocol list", () => {
    expect(offeredKey(`chat, ${KEY_PROTOCOL_PREFIX}abc123`)).toEqual({ protocol: `${KEY_PROTOCOL_PREFIX}abc123`, key: "abc123" });
  });

  test("no header or no key protocol offers nothing", () => {
    expect(offeredKey(null)).toBeNull();
    expect(offeredKey("chat, superchat")).toBeNull();
  });

  test("only the key matches, whatever its case, spaces or grouping", () => {
    expect(keyMatches("abc123", "abc123")).toBe(true);
    expect(keyMatches("ABC-1 23", "abc123")).toBe(true);
    expect(keyMatches("abc1-23", "abc-123")).toBe(true);
    expect(keyMatches("abc124", "abc123")).toBe(false);
    expect(keyMatches("abc12", "abc123")).toBe(false);
    expect(keyMatches("", "")).toBe(false);
    expect(keyMatches("--", "")).toBe(false);
  });
});

describe("explaining a refused key", () => {
  test("a different length is told without either key", () => {
    const text = describeMismatch("abcd-efgh", "abcd-efgh-jkmn-pqrs");
    expect(text).toBe("8 characters where the key has 16");
    expect(text).not.toContain("abcd");
  });

  test("nothing tells how close a guess of the right length came", () => {
    expect(describeMismatch("abcdefghjkmnpqrt", "abcdefghjkmnpqrs")).toBe(describeMismatch("zzzzzzzzzzzzzzzz", "abcdefghjkmnpqrs"));
  });

  test("characters a generated key never has point to a misread one", () => {
    expect(describeMismatch("abcdefghjkmnpq0s", "abcdefghjkmnpqrs")).toBe("16 characters where the key has 16, 1 of them cannot be in a generated key");
  });
});
