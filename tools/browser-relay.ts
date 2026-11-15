// Serves the browser client and joins its WebSocket to the companion's stream port, one protocol packet per message, once the
// screen has shown this PC's key. TLS belongs to the reverse proxy in front of it, since browsers decode video only on HTTPS pages.
// Run: bun tools/browser-relay.ts    (RELAY_PORT overrides the port, RELAY_KEY_FILE where the key is kept)
import { readdirSync } from "node:fs";
import { createConnection, type Socket } from "node:net";
import { extname, join } from "node:path";
import type { ServerWebSocket } from "bun";
import { PacketSplitter } from "../browser/wire.js";
import { defaultKeyPath, describeMismatch, keyMatches, loadOrCreateKey, offeredKey } from "./browser-key.ts";

const port = Number(Bun.env.RELAY_PORT ?? 27190);
const keyPath = Bun.env.RELAY_KEY_FILE ?? defaultKeyPath();
const key = loadOrCreateKey(keyPath);
const STREAM_PORT = 27183; // kStreamPort in shared/stream_protocol.h
// Past this much unsent data the relay stops reading from the companion, so a slow link backs up into the companion,
// which then drops frames before encoding them, never after.
const MAX_BUFFERED_BYTES = 4 * 1024 * 1024;
const CLOSE_REASON_BYTES = 120;
const STATS_LOG_BYTES = 400;

const root = join(import.meta.dir, "..", "browser");
const contentTypes: Record<string, string> = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8" };
// Every page and module in browser/, read at start, so a new module is served without being listed here. Tests stay home.
const files: Record<string, string> = { "/": "index.html" };
for (const name of readdirSync(root)) {
  if (contentTypes[extname(name)] && !name.endsWith(".test.js")) {
    files[`/${name}`] = name;
  }
}

type Link = { remote: string; companion: Socket | null };
let connected: ServerWebSocket<Link> | null = null;

function closeWith(ws: ServerWebSocket<Link>, code: number, reason: string) {
  ws.close(code, reason.slice(0, CLOSE_REASON_BYTES));
}

function joinCompanion(ws: ServerWebSocket<Link>) {
  const splitter = new PacketSplitter();
  const companion = createConnection({ host: "127.0.0.1", port: STREAM_PORT });
  companion.setNoDelay(true);
  companion.on("data", (chunk: Buffer) => {
    for (const packet of splitter.push(chunk)) {
      ws.send(packet);
    }
    if (ws.getBufferedAmount() > MAX_BUFFERED_BYTES) {
      companion.pause();
    }
  });
  companion.on("error", (error: NodeJS.ErrnoException) => {
    closeWith(ws, 1011, error.code === "ECONNREFUSED" ? "the PC is not waiting: start aum-companion run --client browser" : `companion: ${error.message}`);
  });
  companion.on("close", () => closeWith(ws, 1000, "the companion ended the stream"));
  ws.data.companion = companion;
}

const server = Bun.serve<Link, undefined>({
  port,
  hostname: "0.0.0.0",
  fetch(req, server) {
    const { pathname } = new URL(req.url);
    if (pathname === "/ws") {
      const remote = req.headers.get("x-real-ip") ?? server.requestIP(req)?.address ?? "unknown";
      const offered = offeredKey(req.headers.get("sec-websocket-protocol"));
      if (!offered || !keyMatches(offered.key, key)) {
        console.log(`refused a screen from ${remote}: ${offered ? `wrong key (${describeMismatch(offered.key, key)})` : "no key"}`);
        return new Response("this screen has not shown the PC's key", { status: 403 });
      }
      const upgraded = server.upgrade(req, { data: { remote, companion: null }, headers: { "Sec-WebSocket-Protocol": offered.protocol } });
      return upgraded ? undefined : new Response("upgrade failed", { status: 400 });
    }
    const name = files[pathname];
    if (!name) {
      return new Response("not found", { status: 404 });
    }
    return new Response(Bun.file(join(root, name)), { headers: { "content-type": contentTypes[extname(name)], "cache-control": "no-store" } });
  },
  websocket: {
    maxPayloadLength: 1 << 20,
    open(ws) {
      if (connected) {
        closeWith(ws, 1013, "another screen is already connected");
        return;
      }
      connected = ws;
      console.log(`screen connected from ${ws.data.remote}`);
      joinCompanion(ws);
    },
    drain(ws) {
      if (ws.getBufferedAmount() < MAX_BUFFERED_BYTES) {
        ws.data.companion?.resume();
      }
    },
    message(ws, message) {
      if (typeof message === "string") {
        // The client's own statistics, for whoever reads this console; the companion never sees them.
        console.log(`${new Date().toISOString().slice(11, 19)} stats from ${ws.data.remote}: ${message.slice(0, STATS_LOG_BYTES)}`);
      } else {
        ws.data.companion?.write(message);
      }
    },
    close(ws) {
      ws.data.companion?.destroy();
      if (connected === ws) {
        connected = null;
        console.log(`screen from ${ws.data.remote} disconnected`);
      }
    },
  },
});

console.log(`browser relay on http://0.0.0.0:${server.port}/, joining the companion on 127.0.0.1:${STREAM_PORT}`);
console.log(`screens open the page as https://<this relay's public name>/#key=<the key in ${keyPath}>`);
