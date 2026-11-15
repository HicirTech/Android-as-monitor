// The stream protocol of shared/stream_protocol.h, shared by the browser client and tools/browser-relay.ts.
// Mirrored in android/app/src/main/java/dev/androidusbmonitor/Wire.kt.

export const HEADER_BYTES = 16;

export const PacketType = Object.freeze({
  Hello: 1,
  Start: 2,
  Ready: 3,
  VideoFrame: 4,
  AudioPcm: 5,
  Ping: 6,
  Pong: 7,
  FrameStats: 8,
  KeyframeRequest: 9,
});

export const FLAG_KEYFRAME = 0x01;

// A screen offers its key as the WebSocket subprotocol `${KEY_PROTOCOL_PREFIX}<key>`, which keeps the key out of URLs and so out
// of proxy logs. The page itself carries it in its fragment, which browsers never send.
export const KEY_PROTOCOL_PREFIX = "aum-key-";

export function encodePacket(type, flags, timestamp, payload = new Uint8Array(0)) {
  const packet = new Uint8Array(HEADER_BYTES + payload.byteLength);
  const view = new DataView(packet.buffer);
  view.setUint8(0, type);
  view.setUint8(1, flags);
  view.setUint32(4, payload.byteLength, true);
  view.setBigUint64(8, BigInt(timestamp), true);
  packet.set(payload, HEADER_BYTES);
  return packet;
}

// One whole packet, as a WebSocket message carries it. The payload is a view, not a copy.
export function decodePacket(bytes) {
  if (bytes.byteLength < HEADER_BYTES) {
    throw new Error(`packet of ${bytes.byteLength} bytes is shorter than its header`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint32(4, true);
  if (bytes.byteLength !== HEADER_BYTES + length) {
    throw new Error(`packet says ${length} payload bytes but carries ${bytes.byteLength - HEADER_BYTES}`);
  }
  return {
    type: view.getUint8(0),
    flags: view.getUint8(1),
    timestamp: view.getBigUint64(8, true),
    payload: bytes.subarray(HEADER_BYTES),
  };
}

// Cuts a TCP byte stream into whole packets, whatever sizes the chunks arrive in.
export class PacketSplitter {
  #buffer = new Uint8Array(0);

  push(chunk) {
    const joined = new Uint8Array(this.#buffer.byteLength + chunk.byteLength);
    joined.set(this.#buffer, 0);
    joined.set(chunk, this.#buffer.byteLength);
    const packets = [];
    let offset = 0;
    while (joined.byteLength - offset >= HEADER_BYTES) {
      const length = new DataView(joined.buffer, offset + 4, 4).getUint32(0, true);
      const end = offset + HEADER_BYTES + length;
      if (end > joined.byteLength) {
        break;
      }
      packets.push(joined.slice(offset, end));
      offset = end;
    }
    this.#buffer = joined.slice(offset);
    return packets;
  }
}

export function int64Payload(values) {
  const payload = new Uint8Array(values.length * 8);
  const view = new DataView(payload.buffer);
  values.forEach((value, i) => view.setBigInt64(i * 8, BigInt(value), true));
  return payload;
}

export function keyValueText(values) {
  return Object.entries(values).map(([key, value]) => `${key}=${value}\n`).join("");
}

export function parseKeyValues(text) {
  const values = {};
  for (const line of text.split("\n")) {
    const separator = line.indexOf("=");
    if (separator > 0) {
      values[line.slice(0, separator)] = line.slice(separator + 1);
    }
  }
  return values;
}

// The NAL units of an Annex B access unit, without their start codes.
export function* nalUnits(annexB) {
  let start = -1;
  for (let i = 0; i + 2 < annexB.length; i++) {
    if (annexB[i] === 0 && annexB[i + 1] === 0 && annexB[i + 2] === 1) {
      if (start >= 0) {
        yield annexB.subarray(start, annexB[i - 1] === 0 ? i - 1 : i);
      }
      start = i + 3;
      i += 2;
    }
  }
  if (start >= 0) {
    yield annexB.subarray(start);
  }
}

function withoutEmulationPrevention(nal) {
  const out = [];
  for (let i = 0; i < nal.length; i++) {
    if (i >= 2 && nal[i] === 3 && nal[i - 1] === 0 && nal[i - 2] === 0) {
      continue;
    }
    out.push(nal[i]);
  }
  return Uint8Array.from(out);
}

const hex = (value) => value.toString(16).toUpperCase();
const hex2 = (value) => hex(value).padStart(2, "0");

function avcCodec(sps) {
  return `avc1.${hex2(sps[1])}${hex2(sps[2])}${hex2(sps[3])}`;
}

// ISO/IEC 14496-15 annex E: profile, compatibility flags in reverse bit order, tier and level, then constraint bytes.
function hevcCodec(sps) {
  const rbsp = withoutEmulationPrevention(sps);
  const ptl = 3; // after the two-byte NAL header and the byte with the VPS id, sub-layer count and nesting flag
  const profileSpace = ["", "A", "B", "C"][rbsp[ptl] >> 6];
  const tier = (rbsp[ptl] >> 5) & 1 ? "H" : "L";
  const profile = rbsp[ptl] & 0x1f;
  let compatibility = 0;
  for (let i = 0; i < 4; i++) {
    compatibility = (compatibility << 8) | rbsp[ptl + 1 + i];
  }
  let reversed = 0;
  for (let bit = 0; bit < 32; bit++) {
    reversed = (reversed << 1) | ((compatibility >>> bit) & 1);
  }
  const constraints = Array.from(rbsp.subarray(ptl + 5, ptl + 11));
  while (constraints.length && constraints[constraints.length - 1] === 0) {
    constraints.pop();
  }
  const level = rbsp[ptl + 11];
  return [`hvc1`, `${profileSpace}${profile}`, hex(reversed >>> 0), `${tier}${level}`, ...constraints.map(hex2)].join(".");
}

// The WebCodecs codec string for a stream, read from the parameter set in its keyframe.
export function codecFromKeyframe(mimeType, annexB) {
  for (const nal of nalUnits(annexB)) {
    if (mimeType === "video/avc" && (nal[0] & 0x1f) === 7) {
      return avcCodec(nal);
    }
    if (mimeType === "video/hevc" && ((nal[0] >> 1) & 0x3f) === 33) {
      return hevcCodec(nal);
    }
  }
  throw new Error(`no ${mimeType} parameter set in the keyframe`);
}
