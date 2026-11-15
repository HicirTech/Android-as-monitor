import { describe, expect, test } from "bun:test";
import {
  FLAG_KEYFRAME, HEADER_BYTES, PacketSplitter, PacketType, codecFromKeyframe, decodePacket, encodePacket, int64Payload, keyValueText, nalUnits, parseKeyValues,
} from "./wire.js";

const fromHex = (text) => Uint8Array.from(text.match(/../g).map((byte) => parseInt(byte, 16)));

// Parameter sets as NVENC writes them on this project's encoder settings.
const AVC_SPS = "6764003200000000";
const HEVC_SPS = "42010121600000030090000003000003007ba003c0801107cb965d29084645d50c05a808080820000003002000000785";

describe("packets", () => {
  test("round trip keeps every header field and the payload", () => {
    const payload = Uint8Array.from([1, 2, 3, 4, 5]);
    const packet = decodePacket(encodePacket(PacketType.VideoFrame, FLAG_KEYFRAME, 0x123456789abcn, payload));
    expect(packet.type).toBe(PacketType.VideoFrame);
    expect(packet.flags).toBe(FLAG_KEYFRAME);
    expect(packet.timestamp).toBe(0x123456789abcn);
    expect(Array.from(packet.payload)).toEqual([1, 2, 3, 4, 5]);
  });

  test("the header is little endian, as the PC writes it", () => {
    const packet = encodePacket(PacketType.Ping, 0, 1n, new Uint8Array(258));
    expect(Array.from(packet.subarray(0, 16))).toEqual([6, 0, 0, 0, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]);
  });

  test("a message whose length disagrees with its header is refused", () => {
    const packet = encodePacket(PacketType.Hello, 0, 0n, new Uint8Array(4));
    expect(() => decodePacket(packet.subarray(0, HEADER_BYTES + 3))).toThrow();
  });

  test("int64 payloads hold negative and large values", () => {
    const view = new DataView(int64Payload([-5n, 1763190000000000000n]).buffer);
    expect(view.getBigInt64(0, true)).toBe(-5n);
    expect(view.getBigInt64(8, true)).toBe(1763190000000000000n);
  });
});

describe("splitting a byte stream", () => {
  const packets = [
    encodePacket(PacketType.Start, 0, 0n, new TextEncoder().encode("codec=video/hevc\n")),
    encodePacket(PacketType.Ping, 0, 42n),
    encodePacket(PacketType.VideoFrame, FLAG_KEYFRAME, 7n, new Uint8Array(1000).fill(9)),
  ];
  const stream = new Uint8Array(packets.reduce((sum, p) => sum + p.byteLength, 0));
  packets.reduce((offset, p) => (stream.set(p, offset), offset + p.byteLength), 0);

  for (const chunkSize of [1, 7, 16, 17, 100, stream.byteLength]) {
    test(`chunks of ${chunkSize} bytes give back the same packets`, () => {
      const splitter = new PacketSplitter();
      const out = [];
      for (let offset = 0; offset < stream.byteLength; offset += chunkSize) {
        out.push(...splitter.push(stream.subarray(offset, offset + chunkSize)));
      }
      expect(out.map((p) => Array.from(p))).toEqual(packets.map((p) => Array.from(p)));
    });
  }

  test("a partial packet waits for the rest", () => {
    const splitter = new PacketSplitter();
    expect(splitter.push(packets[2].subarray(0, 500))).toEqual([]);
    expect(splitter.push(packets[2].subarray(500)).length).toBe(1);
  });
});

describe("key=value text", () => {
  test("survives a round trip and ignores lines without a key", () => {
    const text = keyValueText({ panelWidth: 1920, refreshHz: 59.94 }) + "garbage\n=x\n";
    expect(parseKeyValues(text)).toEqual({ panelWidth: "1920", refreshHz: "59.94" });
  });

  test("keeps an equals sign inside the value", () => {
    expect(parseKeyValues("decoder=a=b\n")).toEqual({ decoder: "a=b" });
  });
});

describe("codec strings", () => {
  test("NAL units are found behind both start code lengths", () => {
    const annexB = fromHex("00000001" + "0901" + "000001" + "0601");
    expect(Array.from(nalUnits(annexB)).map((n) => Array.from(n))).toEqual([[9, 1], [6, 1]]);
  });

  test("H.264 comes from profile, constraints and level", () => {
    expect(codecFromKeyframe("video/avc", fromHex("00000001" + AVC_SPS + "00000001" + "68ee3c80"))).toBe("avc1.640032");
  });

  test("HEVC skips emulation prevention and reverses the compatibility flags", () => {
    // 0x21 after the sub-layer byte: profile space 0, tier flag 1 (NVENC declares High), Main profile.
    expect(codecFromKeyframe("video/hevc", fromHex("00000001" + "40010c01" + "00000001" + HEVC_SPS))).toBe("hvc1.1.6.H123.90");
  });

  test("a keyframe without a parameter set is an error", () => {
    expect(() => codecFromKeyframe("video/avc", fromHex("00000001" + "6588"))).toThrow();
  });
});
