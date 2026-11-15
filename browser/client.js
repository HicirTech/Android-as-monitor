// Shows the PC's stream in a browser: the page's canvas is the monitor, at its own device pixels and refresh rate.
// Browsers decode video only on secure pages, so this page has to be reached over HTTPS.
import { FramePacer } from "./frame-pacer.js";
import {
  FLAG_KEYFRAME, KEY_PROTOCOL_PREFIX, PacketType, codecFromKeyframe, decodePacket, encodePacket, int64Payload, keyValueText, parseKeyValues,
} from "./wire.js";

const params = new URLSearchParams(location.search);
// A browser cannot tell how big its screen is: the page is told the diagonal of the whole screen in inches.
// 15.4 is the centre screen of the car this client was written for.
const screenDiagonalInches = Number(params.get("diagonal") ?? 15.4);
const displayName = params.get("name") ?? "Web display";
// Sound starts once this much is queued and never queues more than the maximum. The car's own audio path already adds about
// 250 ms, so this jitter allowance costs little and keeps WebSocket arrival jitter from emptying the queue.
const AUDIO_START_MS = 40;
const AUDIO_MAX_MS = 100;
const STATS_INTERVAL_MS = 2000;
const HUD_SECONDS = 6;
const MAX_TRACED_FRAMES = 240;
const REFRESH_SAMPLE_MS = 2000;

const canvas = document.getElementById("screen");
const context = canvas.getContext("2d", { alpha: false, desynchronized: true });
const startButton = document.getElementById("start");
const hud = document.getElementById("hud");
const exitButton = document.getElementById("exit");

let socket = null;
let opened = false;
let exiting = false;
let audioContext = null;
let player = null;
let audioStats = null;
let decoder = null;
let mimeType = null;
let awaitingKeyframe = true;
let keyframeRequested = false;
let hudUntil = 0;
const pacer = new FramePacer();
const traces = new Map();
const counts = { drawn: 0, late: 0, skipped: 0 };

const toNs = (ms) => BigInt(Math.round((performance.timeOrigin + ms) * 1e6));
const nowNs = () => toNs(performance.now());

function show(text, seconds = HUD_SECONDS) {
  hud.textContent = text;
  hud.hidden = false;
  hudUntil = performance.now() + seconds * 1000;
}

function send(type, timestamp, payload) {
  if (socket?.readyState === WebSocket.OPEN) {
    socket.send(encodePacket(type, 0, timestamp, payload));
  }
}

// The exact backing size that maps one canvas pixel to one screen pixel.
function devicePixelSize() {
  return new Promise((resolve) => {
    const observer = new ResizeObserver(([entry]) => {
      observer.disconnect();
      const box = entry.devicePixelContentBoxSize?.[0];
      resolve(box ? { width: box.inlineSize, height: box.blockSize }
                  : { width: Math.round(entry.contentRect.width * devicePixelRatio), height: Math.round(entry.contentRect.height * devicePixelRatio) });
    });
    observer.observe(canvas, { box: "device-pixel-content-box" });
  });
}

// The mean frame interval, leaving out the frames the browser missed.
async function measureRefreshHz() {
  const stamps = await new Promise((resolve) => {
    const seen = [];
    const tick = (t) => {
      seen.push(t);
      if (t - seen[0] < REFRESH_SAMPLE_MS) requestAnimationFrame(tick); else resolve(seen);
    };
    requestAnimationFrame(tick);
  });
  const intervals = stamps.slice(1).map((t, i) => t - stamps[i]);
  const median = [...intervals].sort((a, b) => a - b)[Math.floor(intervals.length / 2)];
  const steady = intervals.filter((interval) => Math.abs(interval - median) < median / 4);
  return 1000 / (steady.reduce((sum, interval) => sum + interval, 0) / steady.length);
}

function physicalSizeMm({ width, height }) {
  const mmPerPixel = (screenDiagonalInches * 25.4) / Math.hypot(screen.width * devicePixelRatio, screen.height * devicePixelRatio);
  return { widthMm: width * mmPerPixel, heightMm: height * mmPerPixel };
}

async function decodesInHardware(codec, { width, height }) {
  try {
    return (await VideoDecoder.isConfigSupported({ codec, codedWidth: width, codedHeight: height, hardwareAcceleration: "prefer-hardware" })).supported;
  } catch {
    return false;
  }
}

function deviceId() {
  try {
    let id = localStorage.getItem("aum-device-id");
    if (!id) {
      id = Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) => b.toString(16).padStart(2, "0")).join("");
      localStorage.setItem("aum-device-id", id);
    }
    return id;
  } catch {
    // Without storage Windows sees a new monitor each time, which only costs it a fresh entry.
    return `${screen.width}x${screen.height}-browser`;
  }
}

async function keepScreenOn() {
  try {
    await navigator.wakeLock?.request("screen");
  } catch {
    // The screen may dim on its own; the stream does not depend on it.
  }
}

function requestKeyframe() {
  if (!keyframeRequested) {
    keyframeRequested = true;
    send(PacketType.KeyframeRequest, 0n);
  }
}

function startDecoder(keyframe) {
  if (decoder && decoder.state !== "closed") decoder.close();
  const codec = codecFromKeyframe(mimeType, keyframe);
  decoder = new VideoDecoder({
    output: onDecoded,
    error: (error) => {
      show(`decoder stopped: ${error.message}; waiting for a keyframe`);
      awaitingKeyframe = true;
      traces.clear();
      requestKeyframe();
    },
  });
  decoder.configure({ codec, optimizeForLatency: true });
  awaitingKeyframe = false;
  keyframeRequested = false;
}

function onVideo(frameId, keyframe, payload, receivedNs) {
  if (awaitingKeyframe) {
    if (!keyframe) {
      requestKeyframe();
      return;
    }
    startDecoder(payload);
  }
  if (traces.size >= MAX_TRACED_FRAMES) traces.delete(traces.keys().next().value);
  traces.set(frameId, { received: receivedNs, queued: nowNs(), decoded: 0n });
  decoder.decode(new EncodedVideoChunk({ type: keyframe ? "key" : "delta", timestamp: frameId, data: payload }));
}

function report(frameId, presentNs) {
  const trace = traces.get(frameId);
  traces.delete(frameId);
  if (trace) {
    send(PacketType.FrameStats, BigInt(frameId), int64Payload([trace.received, trace.queued, trace.decoded, presentNs]));
  }
}

function onDecoded(frame) {
  const trace = traces.get(frame.timestamp);
  if (!trace) {
    frame.close();
    return;
  }
  trace.decoded = nowNs();
  pacer.add(frame, performance.now());
}

function draw(vsyncMs) {
  const { frame, late, skipped } = pacer.next(performance.now());
  for (const given of skipped) {
    counts.skipped++;
    report(given.timestamp, 0n);
    given.close();
  }
  if (frame) {
    if (frame.displayWidth <= canvas.width && frame.displayHeight <= canvas.height) {
      context.drawImage(frame, 0, 0);
    } else {
      // Only while the stream and the window disagree, as when the window changed size after the monitor was made.
      context.drawImage(frame, 0, 0, canvas.width, canvas.height);
    }
    counts.drawn++;
    counts.late += late ? 1 : 0;
    report(frame.timestamp, toNs(vsyncMs));
    frame.close();
  }
  requestAnimationFrame(draw);
}

async function startAudio(sampleRate) {
  if (audioContext.sampleRate !== sampleRate) {
    await audioContext.close();
    audioContext = new AudioContext({ sampleRate, latencyHint: "interactive" });
  }
  await audioContext.audioWorklet.addModule("pcm-player.js");
  player = new AudioWorkletNode(audioContext, "pcm-player", {
    numberOfInputs: 0,
    outputChannelCount: [2],
    processorOptions: { startFrames: Math.round((sampleRate * AUDIO_START_MS) / 1000), maxFrames: Math.round((sampleRate * AUDIO_MAX_MS) / 1000) },
  });
  player.port.onmessage = (event) => { audioStats = event.data; };
  player.connect(audioContext.destination);
  if (audioContext.state !== "running") await audioContext.resume().catch(() => {});
}

function onAudio(payload) {
  if (player) {
    // A copy, so the samples start on a fresh, aligned buffer that can be handed over without another copy.
    const samples = new Float32Array(payload.slice().buffer);
    player.port.postMessage(samples, [samples.buffer]);
  }
}

async function onStart(start) {
  mimeType = start.codec;
  if (start.sampleRate) await startAudio(Number(start.sampleRate));
  send(PacketType.Ready, 0n);
  show(`${start.codec} ${start.width}x${start.height} at ${Number(start.refreshHz).toFixed(3)} Hz`);
}

function onMessage(event) {
  try {
    handle(decodePacket(new Uint8Array(event.data)), nowNs());
  } catch (error) {
    show(`stream error: ${error.message}`);
  }
}

function handle(packet, receivedNs) {
  switch (packet.type) {
    case PacketType.VideoFrame:
      onVideo(Number(packet.timestamp), (packet.flags & FLAG_KEYFRAME) !== 0, packet.payload, receivedNs);
      break;
    case PacketType.AudioPcm:
      onAudio(packet.payload);
      break;
    case PacketType.Ping:
      send(PacketType.Pong, packet.timestamp, int64Payload([nowNs()]));
      break;
    case PacketType.Start:
      onStart(parseKeyValues(new TextDecoder().decode(packet.payload))).catch((error) => show(`cannot start: ${error.message}`, 600));
      break;
  }
}

function connect(hello, screenKey) {
  socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`, [`${KEY_PROTOCOL_PREFIX}${screenKey}`]);
  socket.binaryType = "arraybuffer";
  socket.onopen = () => {
    opened = true;
    send(PacketType.Hello, 0n, new TextEncoder().encode(hello));
  };
  socket.onmessage = onMessage;
  socket.onclose = (event) => {
    exitButton.hidden = true;
    if (exiting) {
      show("Stopped: the PC has unplugged this monitor.\ntap to start again", 3600);
    } else if (!opened) {
      show("Could not connect: the key is wrong, or the PC is not running the relay.\ntap to try again", 3600);
    } else {
      show(`disconnected${event.reason ? `: ${event.reason}` : ""}\ntap to reload`, 3600);
    }
    document.addEventListener("click", () => location.reload(), { once: true });
  };
}

// Ending the stream is what unplugs the monitor: the relay drops the companion's link and the companion stops.
function exit(event) {
  event.stopPropagation();
  exiting = true;
  exitButton.hidden = true;
  socket?.close(1000, "stopped on the screen");
}

function showControls() {
  hud.hidden = false;
  exitButton.hidden = false;
  hudUntil = performance.now() + HUD_SECONDS * 1000;
}

function reportEverySecond() {
  let lastDrawn = 0;
  setInterval(() => {
    const drawn = counts.drawn - lastDrawn;
    lastDrawn = counts.drawn;
    const audio = audioStats ? `audio queue ${audioStats.queuedMs.toFixed(0)} ms, underruns ${audioStats.underruns}, dropped ${audioStats.discardedFrames}` : "no audio";
    if (performance.now() < hudUntil && socket?.readyState === WebSocket.OPEN) {
      hud.textContent = `${canvas.width}x${canvas.height} ${drawn} fps, late ${counts.late}, skipped ${counts.skipped}, decode queue ${decoder?.decodeQueueSize ?? 0}\n${audio}`;
    } else if (performance.now() >= hudUntil) {
      hud.hidden = true;
      exitButton.hidden = true;
    }
  }, 1000);
}

// Text messages stop at tools/browser-relay.ts, which prints them, so what the car hears and shows can be read on the PC.
function sendStatsToRelay() {
  setInterval(() => {
    if (socket?.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ ...counts, decodeQueue: decoder?.decodeQueueSize ?? 0, audio: audioStats }));
    }
  }, STATS_INTERVAL_MS);
}

async function start() {
  startButton.hidden = true;
  // The PC's key travels in the fragment, which the browser never sends anywhere, and then as the WebSocket subprotocol. It is read
  // now rather than when the page loaded, since adding a fragment to an open page does not load it again.
  const screenKey = new URLSearchParams(location.hash.slice(1)).get("key");
  if (!screenKey) {
    show(`This page needs the PC's key: open it as https://${location.host}/#key=<the key in browser-key.txt on the PC>\ntap to try again`, 3600);
    document.addEventListener("click", () => location.reload(), { once: true });
    return;
  }
  // Made inside the tap, since browsers let sound start only from a user gesture.
  audioContext = new AudioContext({ latencyHint: "interactive" });
  keepScreenOn();
  document.addEventListener("visibilitychange", () => { if (document.visibilityState === "visible") keepScreenOn(); });
  if (!window.isSecureContext || typeof VideoDecoder === "undefined") {
    show("This page needs HTTPS: browsers decode video only on secure pages.", 3600);
    return;
  }
  show("measuring the screen...");
  const size = await devicePixelSize();
  canvas.width = size.width;
  canvas.height = size.height;
  // NV12 and the encoders work in 2x2 blocks, so the monitor is the largest even area; a leftover pixel line stays black.
  const monitor = { width: size.width & ~1, height: size.height & ~1 };
  const refreshHz = await measureRefreshHz();
  const { widthMm, heightMm } = physicalSizeMm(monitor);
  const hello = keyValueText({
    protocol: 1,
    manufacturer: "Web",
    model: displayName,
    deviceId: deviceId(),
    panelWidth: monitor.width,
    panelHeight: monitor.height,
    surfaceWidth: monitor.width,
    surfaceHeight: monitor.height,
    refreshHz: refreshHz.toFixed(5),
    widthMm: widthMm.toFixed(1),
    heightMm: heightMm.toFixed(1),
    audioSampleRate: audioContext.sampleRate,
    hevcDecoder: "WebCodecs",
    hevcCoversNative: await decodesInHardware("hvc1.1.6.L153.B0", monitor),
    avcDecoder: "WebCodecs",
    avcCoversNative: await decodesInHardware("avc1.640033", monitor),
    devicePixelRatio,
    userAgent: navigator.userAgent,
  });
  show(`${monitor.width}x${monitor.height} at ${refreshHz.toFixed(3)} Hz, ${widthMm.toFixed(0)} x ${heightMm.toFixed(0)} mm\nconnecting...`);
  window.addEventListener("resize", () => show("the window changed size: reload so the monitor matches it", 3600));
  document.addEventListener("click", showControls);
  exitButton.addEventListener("click", exit);
  connect(hello, screenKey);
  showControls();
  reportEverySecond();
  sendStatsToRelay();
  requestAnimationFrame(draw);
}

startButton.addEventListener("click", () => start().catch((error) => show(`cannot start: ${error.message}`, 3600)), { once: true });
// A key typed into the address of an open page changes only its fragment; loading again lets the page start over with it.
window.addEventListener("hashchange", () => location.reload());
