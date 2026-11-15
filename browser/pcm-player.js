// Plays the PC's interleaved stereo float32. The queue stays short: past its maximum the oldest sound is dropped back down to the
// starting level, so sound never drifts behind the picture, the same rule as android/.../AudioPlayer.kt.
class PcmPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.maxFrames = options.processorOptions.maxFrames;
    // Playing starts only at this level, so the jitter of arriving packets does not empty the queue at once.
    this.startFrames = options.processorOptions.startFrames;
    this.chunks = [];
    this.offset = 0;
    this.queuedFrames = 0;
    this.playing = false;
    this.underruns = 0;
    this.discardedFrames = 0;
    this.lastReport = currentTime;
    this.port.onmessage = (event) => this.enqueue(event.data);
  }

  enqueue(samples) {
    this.chunks.push(samples);
    this.queuedFrames += samples.length / 2;
    if (this.queuedFrames <= this.maxFrames) {
      return;
    }
    while (this.queuedFrames > this.startFrames && this.chunks.length > 1) {
      const dropped = this.chunks.shift();
      const frames = (dropped.length - this.offset) / 2;
      this.offset = 0;
      this.queuedFrames -= frames;
      this.discardedFrames += frames;
    }
  }

  process(inputs, outputs) {
    const [left, right] = outputs[0];
    if (!this.playing && this.queuedFrames >= this.startFrames) {
      this.playing = true;
    }
    for (let i = 0; i < left.length; i++) {
      if (!this.playing || this.chunks.length === 0) {
        left[i] = 0;
        right[i] = 0;
        continue;
      }
      const chunk = this.chunks[0];
      left[i] = chunk[this.offset];
      right[i] = chunk[this.offset + 1];
      this.offset += 2;
      this.queuedFrames--;
      if (this.offset >= chunk.length) {
        this.chunks.shift();
        this.offset = 0;
      }
    }
    if (this.playing && this.chunks.length === 0) {
      this.playing = false;
      this.underruns++;
    }
    if (currentTime - this.lastReport >= 1) {
      this.lastReport = currentTime;
      this.port.postMessage({ queuedMs: (this.queuedFrames / sampleRate) * 1000, underruns: this.underruns, discardedFrames: this.discardedFrames });
    }
    return true;
  }
}

registerProcessor("pcm-player", PcmPlayer);
