# Android USB Monitor

Spike: an Android phone without root, or [a web browser](#in-a-browser) on any screen, becomes a Windows display over one USB cable or Wi-Fi, at its panel's own resolution, size and refresh rate, with picture and sound in one low-latency stream.

| Part | Runs on | Role |
| --- | --- | --- |
| `android/` | Android 13+, or Android 11 on [the TV](#on-a-tv), USB or wireless debugging on | Reports the panel, decodes and shows frames 1:1, plays sound |
| `browser/` | A Chromium browser, on an HTTPS page | The same client as a web page, decoding with WebCodecs |
| `companion/` | Windows 11, unelevated | Builds the EDID, captures, encodes, streams through `adb reverse` or directly over Wi-Fi |
| `driver/` | Windows 11, UMDF | Virtual display (IddCx) that offers exactly one mode, the one in the EDID |

## Set up

The APK build needs a JDK 17 or newer and the Android SDK with platform 36, through `JAVA_HOME` and `ANDROID_HOME`.

```powershell
tools\get-toolchain.ps1     # private MSVC, SDK and WDK in %LOCALAPPDATA%\usb-monitor-toolchain
tools\build-driver.ps1
tools\build-companion.ps1
tools\build-apk.ps1
adb install -r android\app\build\outputs\apk\release\app-release.apk
```

Then, from an elevated PowerShell, with the companion stopped: `tools\install-driver.ps1`. `tools\uninstall-driver.ps1` removes everything it added. Rerun the install after each driver rebuild.

## Run

Connect the phone, unlock it, and tap "Got it" if Android explains full screen the first time. Set the phone to its highest refresh rate, or Android only offers 60 Hz. With more than one device attached, add `--serial <serial>`.

The monitor renders and encodes on the NVIDIA GPU when there is one, through NVENC; any other GPU uses Media Foundation. `--render-adapter NAME` picks the GPU.

Windows sees the monitor at twice the panel's refresh rate, and the phone gets one frame per panel period. Composing an indirect display at the panel's own rate, Windows loses frames in bursts; `--compose-multiple 1` shows it.

```powershell
out\companion\aum-companion.exe run --source pattern --seconds 30 --audio-endpoint tone   # whole pipeline, no driver needed
out\companion\aum-companion.exe run --seconds 30                                          # native mode check and frame rate test
out\companion\aum-companion.exe run                                                       # use it as a monitor; Ctrl+C unplugs it
```

| Command | Purpose |
| --- | --- |
| `run` | Plugs the monitor in and streams; prints PASS/FAIL checks and per-stage latency each second |
| `encoder-bench` | Measures capture to bitstream with no phone; `--pattern scroll` is the worst case, `--dump` saves the bitstream |
| `inspect-displays` | Shows what Windows reports for every display, and the playback endpoints |

Runs with `--seconds` also read SurfaceFlinger's present times: the `on screen` and `capture to on screen` lines and the PASS or FAIL are what really reached the panel, and `panel` is the rate it really runs at. Keep the PC awake and unlocked: with its screen off, Windows gives the virtual monitor no frames and timers run late.

`tools\driver-events.ps1` prints the driver's messages from the Application event log.

### Over Wi-Fi

`--connect` streams straight to the phone over TCP instead of through `adb reverse`, and the app holds a low-latency Wi-Fi lock. adb still starts the app, so turn on wireless debugging and pass that device as `--serial`. The phone listens, so Windows needs no firewall rule.

```powershell
out\companion\aum-companion.exe run --connect <phone IPv4 address> --serial <wireless adb serial>
```

Nothing authenticates or encrypts this link yet: the picture crosses the network in the clear, and while the app shows it, anyone on the network can connect to the phone. Use it only on a network you trust.

A Wi-Fi run does not reach the phone through adb once it streams, because that stalled the picture for seconds at a time. So the 1:1 check and the `on screen` lines are off; `--phone-checks on` runs them, knowing that they can disturb what they measure.

Without adb traffic on a 6 GHz Wi-Fi 7 network, 5 minutes on an idle PC showed no gap over 50 ms between frames. With a game running, a freeze of under a second came about every 4 minutes.

### On a TV

One TV works so far: a TCL C745 (`Build.PRODUCT` `G05_4K_GB`) on Android 11, at 3840x2160 and 119 Hz. Run it over Wi-Fi as above, with the TV awake, because its screensaver keeps the app from getting focus. Leave out `--seconds` to use it as a monitor, because such a run covers the monitor with its moving test window.

- Android reports only the TV's 1080p layout, so `TvPanel` holds the real panel. Another TV needs its own entry there.
- SurfaceFlinger stays at 60 Hz, so the picture uses tunneled playback, straight to the video plane.
- The decoder firmware halves a 4K stream above 60 fps. It reads the rate from the SPS and also measures it from the timestamps, so the app declares 60 fps in both. In low latency mode the firmware shows frames as they arrive, whatever they say.
- The decoder path tops out near 1000 Mpixel/s, which is 119.7 fps at the 3840x2176 NVENC codes for 2160 lines. At 120 fps its queue grows by about 2 ms each second, so the app asks for 119 Hz.

From capture to the decoder's report that a frame was rendered, the median is about 18 ms, once the queue left by the start has drained after about 10 seconds.

- Full-screen motion (`--pattern scroll`) reaches about 113 fps, and latency climbs past 180 ms.
- The decoder holds the newest 2 frames until newer ones arrive, so the last change on a desktop that goes still can stay hidden until the next one.
- What reaches the panel has not been measured, only what the decoder reports.

### In a browser

A screen with a recent Chromium browser can be the monitor, a car's centre screen for one. The monitor is the page: its size in device pixels, rounded down to even numbers for the encoder, at the refresh rate the page measures. A browser cannot tell how big its screen is, so the page takes the whole screen's diagonal in inches as `?diagonal=`, 15.4 if it is left out.

```powershell
bun tools\browser-relay.ts                              # serves browser\ on port 27190 and joins each page to the companion
out\companion\aum-companion.exe run --client browser    # waits up to 15 minutes for a page instead of starting the app
```

- Browsers decode video only on HTTPS pages, so the relay needs a reverse proxy with a real certificate in front of it, with WebSockets on. Some browsers, car browsers among them, refuse private addresses even behind a name, so that name may have to point at a public address that the router loops back.
- The relay serves a page only with this PC's key, which it makes on its first start in `%USERPROFILE%\.android-usb-monitor\browser-key.txt`; write your own there and restart the relay if you prefer. Open the page as `https://<name>/#key=<key>`: the fragment never leaves the browser, and the page hands the key over as the WebSocket subprotocol, so it stays out of proxy logs. An address list on the proxy alone is not enough, since a proxy that trusts `X-Real-IP` from CDN ranges can be talked past.
- Tap the screen for its statistics and an Exit button, which ends the stream and so unplugs the monitor. One run of the companion serves one page. The page also sends its statistics to the relay every 2 seconds, and the relay prints them.

In a car's browser over 5 GHz Wi-Fi, at 1922x1080 and 60 Hz, capture to the vsync a frame is drawn on took 15 ms at the median and 23 ms at p95. The phase between the PC and the screen slides, so about a fifth of the frames wait one vsync, and 2.4% were skipped, mostly in bursts. Sound queues 40 to 100 ms in the page, on top of about 270 ms that the car's browser reports for its own audio path.

## Structure

```text
android/        App: PanelSpec reads the panel, TvPanel knows the panel a TV hides, StreamSession owns the link, VideoDecoder and AudioPlayer play it, HevcSpsTiming changes the rate a stream declares, VsyncSlots gives each frame its own vsync
browser/        Web client: client is the flow, wire mirrors the stream protocol, frame-pacer gives each frame its own vsync, pcm-player plays sound
companion/      PC program: run_monitor is the flow, edid builds the monitor description, video_pipeline drops frames before encoding, never after, nvenc_encoder and mf_encoder encode, gpu_clock_holder keeps NVIDIA out of its idle clocks, phone_cadence reads what reached the panel
driver/         IddCx driver: control_pipe unplugs the monitor when the companion goes away, frame_tap shares desktop frames with the companion
shared/         What both sides must agree on: driver protocol, stream protocol, EDID timing
third_party/    nvEncodeAPI.h at a pinned commit, see SOURCE.txt
tools/          Toolchain, build, driver install and check scripts; browser-relay and the key it checks
```
