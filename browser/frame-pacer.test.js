import { describe, expect, test } from "bun:test";
import { FramePacer } from "./frame-pacer.js";

const PERIOD = 1000 / 60;

// Runs callbacks every period from 0 and decodes each frame at its given time. Returns what every callback did.
function play(decodeTimes, callbacks) {
  const pacer = new FramePacer();
  const frames = decodeTimes.map((at, id) => ({ id, at })).sort((a, b) => a.at - b.at);
  const log = [];
  for (let i = 0; i < callbacks; i++) {
    const now = i * PERIOD;
    while (frames.length && frames[0].at < now) {
      const frame = frames.shift();
      pacer.add(frame.id, frame.at);
    }
    const { frame, late, skipped } = pacer.next(now);
    log.push({ shown: frame, late, skipped });
  }
  return log;
}

const shownIds = (log) => log.map((entry) => entry.shown).filter((id) => id !== null);
const skippedIds = (log) => log.flatMap((entry) => entry.skipped);

describe("frame pacing", () => {
  test("frames on time each get the next vsync and none is late", () => {
    const log = play(Array.from({ length: 30 }, (_, i) => i * PERIOD + 8), 32);
    expect(shownIds(log)).toEqual(Array.from({ length: 30 }, (_, i) => i));
    expect(log.some((entry) => entry.late)).toBe(false);
    expect(skippedIds(log)).toEqual([]);
  });

  test("two frames in one interval are both shown, a vsync apart, instead of one being lost", () => {
    const log = play([3, 14], 4);
    expect(log[1].shown).toBe(0);
    expect(log[2].shown).toBe(1);
    expect(log[2].late).toBe(true);
    expect(skippedIds(log)).toEqual([]);
  });

  test("a third waiting frame costs the oldest, so nothing is shown two vsyncs late", () => {
    const log = play([2, 6, 12], 4);
    expect(log[1].skipped).toEqual([0]);
    expect(log[1].shown).toBe(1);
    expect(log[2].shown).toBe(2);
  });

  test("steady room for the earlier vsync gives one frame up to get it back", () => {
    // One burst puts every later frame a vsync late, and each is ready 6 ms before the vsync it could have had.
    const times = [5, 10, ...Array.from({ length: 40 }, (_, i) => (i + 2) * PERIOD - 6)];
    const log = play(times, 45);
    expect(skippedIds(log).length).toBe(1);
    const afterSkip = log.slice(log.findIndex((entry) => entry.skipped.length) + 1);
    expect(afterSkip.filter((entry) => entry.shown !== null).every((entry) => !entry.late)).toBe(true);
  });

  test("jittery room keeps the later vsync rather than missing the earlier one again and again", () => {
    const jitter = [1, 14, 3, 12, 2, 15, 4, 11, 1, 13, 2, 14, 3, 12, 1, 15, 2, 13, 4, 12];
    const times = [5, 10, ...jitter.map((before, i) => (i + 2) * PERIOD - before)];
    const log = play(times, 25);
    expect(skippedIds(log)).toEqual([]);
  });
});
