// Gives every decoded frame its own vsync, the earliest one it can still reach: the browser's side of
// android/app/src/main/java/dev/androidusbmonitor/VsyncSlots.kt.
//
// The PC's frame clock and the screen never run at quite the same rate, so the phase between them keeps sliding. While it slides
// past the moment the browser draws, arrival jitter would put two frames on one vsync and none on the next: one frame lost and one
// repeated, many times a second. With a vsync per frame the screen slips about once per cycle of the phase. A browser offers no
// vsync deadlines, so a frame's earliest vsync is the first animation frame callback that starts after it was decoded.

// A frame may be shown at most this many vsyncs after the earliest one it could have had.
const MAX_VSYNCS_LATE = 1;
// How many late frames in a row must have had room for the earlier vsync before one is skipped to take it.
const FRAMES_TO_JUDGE_ROOM = 16;

export class FramePacer {
  #waiting = [];
  #lastCallbackMs = -Infinity;
  #framesJudged = 0;
  #leastRoomMs = Infinity;
  #mostRoomMs = -Infinity;

  add(frame, decodedMs) {
    this.#waiting.push({ frame, decodedMs });
  }

  // Called at the start of every animation frame callback. Returns the frame to draw now, if any, whether it is a vsync later than
  // it could have been, and the frames given up so that none waits longer than that.
  next(callbackMs) {
    const skipped = [];
    while (this.#waiting.length > MAX_VSYNCS_LATE + 1) {
      skipped.push(this.#waiting.shift().frame);
    }
    let late = false;
    if (this.#waiting.length > 0) {
      late = this.#waiting[0].decodedMs < this.#lastCallbackMs;
      if (!late) {
        this.#restartJudgingRoom();
      } else if (this.#hasRoomForTheEarlierVsync(this.#lastCallbackMs - this.#waiting[0].decodedMs)) {
        skipped.push(this.#waiting.shift().frame);
        late = false;
      }
    }
    this.#lastCallbackMs = callbackMs;
    const shown = this.#waiting.shift();
    return { frame: shown ? shown.frame : null, late: Boolean(shown) && late, skipped };
  }

  // A frame running a vsync late could have had the earlier one. Going back costs a skipped frame, so it is only worth it once even
  // the latest of the last frames clears that vsync by half of how much they jitter. Closer than that they would soon miss it again.
  #hasRoomForTheEarlierVsync(roomMs) {
    this.#leastRoomMs = Math.min(this.#leastRoomMs, roomMs);
    this.#mostRoomMs = Math.max(this.#mostRoomMs, roomMs);
    if (++this.#framesJudged < FRAMES_TO_JUDGE_ROOM) {
      return false;
    }
    const hasRoom = this.#leastRoomMs * 2 > this.#mostRoomMs - this.#leastRoomMs;
    this.#restartJudgingRoom();
    return hasRoom;
  }

  #restartJudgingRoom() {
    this.#framesJudged = 0;
    this.#leastRoomMs = Infinity;
    this.#mostRoomMs = -Infinity;
  }
}
