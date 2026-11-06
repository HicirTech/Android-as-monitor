# Android USB Monitor

Spike: an Android phone, without root, becomes a Windows display over one USB cable or Wi-Fi, at its panel's own resolution, size and refresh rate, with picture and sound in one low-latency stream.

| Part | Runs on | Role |
| --- | --- | --- |
| `android/` | Android 13+, USB or wireless debugging on | Reports the panel, decodes and shows frames 1:1, plays sound |
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

## Structure

```text
android/        App: PanelSpec reads the panel, StreamSession owns the link, VideoDecoder and AudioPlayer play it, VsyncSlots gives each frame its own vsync
companion/      PC program: run_monitor is the flow, edid builds the monitor description, video_pipeline drops frames before encoding, never after, nvenc_encoder and mf_encoder encode, gpu_clock_holder keeps NVIDIA out of its idle clocks, phone_cadence reads what reached the panel
driver/         IddCx driver: control_pipe unplugs the monitor when the companion goes away, frame_tap shares desktop frames with the companion
shared/         What both sides must agree on: driver protocol, stream protocol, EDID timing
third_party/    nvEncodeAPI.h at a pinned commit, see SOURCE.txt
tools/          Toolchain, build, driver install and check scripts
```
