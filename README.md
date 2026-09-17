# Wii U Web Stream

A Wii U **Aroma/WUPS plugin** that streams the TV and GamePad outputs over your local network using HTTP/MJPEG, with optional experimental PCM audio. It is designed for modern browsers, OBS Studio, phones, tablets and simple MJPEG clients without a dedicated PC application.

> `v0.2.0-dev` is a **pre-release development build**. TV and GamePad streaming have now been confirmed on a physical Wii U, but audio, high-risk overrides and broad game compatibility still need more hardware testing before a stable release.

## Important: WUPS config memory warning

During physical Wii U testing, the original full WUPS configuration menu could run out of memory while Aroma was rendering the large number of settings.

To reduce that risk, the current build intentionally keeps the **WUPS menu minimal** and moves the full configuration interface to the browser:

```text
http://WIIU_IP:7770/settings
```

When the minimal WUPS menu is opened, Wii U Web Stream temporarily stops capture/audio and releases their working buffers while Aroma renders the menu. The network listener remains available so Web Settings can still be opened from another device.

The WUPS menu is intended primarily as a recovery entry point. Do **not** expect the full settings list there anymore.

## Default ports

| Port | Purpose |
|---|---|
| `7770` | Dashboard, Web Settings, status, snapshots, OBS pages and audio endpoints |
| `7771` | TV MJPEG |
| `7772` | GamePad MJPEG |

No UPnP, NAT-PMP or automatic port forwarding is used. Public/non-LAN source addresses are rejected by the server.

## Web Settings

Open:

```text
http://WIIU_IP:7770/settings
```

The page provides the primary configuration UI for:

- Presets: Custom, Low Latency, Balanced, Quality and OBS.
- TV/GamePad enable switches.
- TV/GamePad target FPS.
- Output resolutions.
- JPEG quality.
- Adaptive FPS.
- Listener ports.
- Health watchdog.
- Optional LAN access code.
- Logging level.
- High-risk actions and their confirmation gate.
- Experimental audio streaming.
- Continuous capture.

Listener/server changes are applied asynchronously outside the HTTP request thread. This avoids stopping the web listener from inside its own client thread when the web port changes. Reconnect on the new port after a few seconds if you change it.

## v0.2 highlights

### Presets

- **Low Latency** — higher FPS, lower JPEG quality and reduced output size.
- **Balanced** — general-purpose default.
- **Quality** — up to 960x540 TV with lower FPS and higher JPEG quality.
- **OBS** — OBS-oriented quality/FPS settings.
- **Custom** — manual FPS, resolution and JPEG settings.

### Adaptive FPS

When enabled, the capture path keeps a separate effective FPS. If the shared JPEG worker is still busy when a new frame arrives, the effective FPS backs off instead of blocking the render path. It gradually recovers toward the configured target after the encoder remains healthy.

### High-risk actions

High-risk options remain hidden until both of these are enabled in Web Settings:

```text
Enable high-risk actions
I understand and accept the risk
```

The current high-risk section can expose:

- TV target FPS up to 60.
- GamePad target FPS up to 60.
- JPEG quality up to 95.
- 1280x720 and 1920x1080 output options.
- **Audio streaming (HIGH RISK)**.
- **Continuous capture without viewers (HIGH RISK)**.

These are experimental, not recommended defaults. They can sharply increase CPU, GPU, mapped-memory, encoder and network load and can reduce game performance or freeze/crash the console.

Disabling either high-risk confirmation switch automatically returns FPS, JPEG quality and resolutions to the safe range and disables high-risk audio/continuous capture.

### Audio streaming (HIGH RISK)

Audio is captured from the Wii U AX device final mix for TV and DRC/GamePad. The callback copies/down-converts samples into a small fixed ring; it does not allocate memory, encode video, perform socket I/O or wait for a busy slot. A busy slot causes an audio packet drop instead of blocking the AX callback.

The plugin preserves/chains the previously installed TV/DRC final-mix callbacks and restores them when audio streaming is disabled or the runtime stops.

Audio is served as stereo PCM16LE in a streaming WAV container:

```text
http://WIIU_IP:7770/audio/tv.wav
http://WIIU_IP:7770/audio/gamepad.wav
http://WIIU_IP:7771/audio.wav
http://WIIU_IP:7772/audio.wav
```

The `7771` endpoint maps to TV audio and `7772` maps to GamePad audio. Audio defaults to **off** and requires the high-risk double confirmation.

### Client and socket safeguards

- Up to 8 clients per listener.
- One JPEG encode per source/frame, shared by all clients.
- Slow or stalled sockets use WUT `select()` with a 3-second readiness timeout.
- Stalled clients are disconnected instead of blocking the capture producer.
- Listener bind conflicts are detected synchronously.
- Failed listeners can be retried independently.
- Network/listener maintenance continues even if stream-stall watchdog checks are disabled.

## Dashboard and diagnostics

Dashboard:

```text
http://WIIU_IP:7770/
```

Diagnostics:

```text
http://WIIU_IP:7770/api/status
http://WIIU_IP:7770/debug/performance
http://WIIU_IP:7770/health
```

`/api/status` includes uptime, actual FPS, target/effective FPS, video clients, last-frame age, capture/drop/failure counters, average JPEG size, scaling/encode timing, listener state, watchdog counters and audio state/counters.

## Optional access code

Enable **Require URL access code** in Web Settings and choose a numeric code. Then use URLs such as:

```text
http://WIIU_IP:7770/?key=777777
http://WIIU_IP:7770/settings?key=777777
http://WIIU_IP:7771/stream.mjpg?key=777777
http://WIIU_IP:7772/stream.mjpg?key=777777
```

Audio endpoints use the same `?key=CODE` control when enabled.

This is LAN access control only. HTTP is not encrypted, so the code is not an Internet-grade password.

## OBS

TV:

```text
http://WIIU_IP:7770/obs/tv
```

GamePad:

```text
http://WIIU_IP:7770/obs/gamepad
```

Dual:

```text
http://WIIU_IP:7770/obs/dual
```

Picture-in-picture:

```text
http://WIIU_IP:7770/obs/dual?layout=pip
```

When high-risk audio streaming is active, single-source OBS pages include the corresponding audio stream. The dual page uses TV audio by default and accepts:

```text
audio=tv
audio=gamepad
audio=none
```

Single-source OBS pages also accept:

```text
fit=contain|cover
bg=transparent|black
mirror=0|1
rotate=0|90|180|270
```

## Snapshots

```text
http://WIIU_IP:7770/snapshot/tv.jpg
http://WIIU_IP:7770/snapshot/gamepad.jpg
http://WIIU_IP:7771/snapshot.jpg
http://WIIU_IP:7772/snapshot.jpg
```

## Performance architecture

```text
GX2 TV / DRC hook
      |
      v
mapped linear RGBA buffer
      |
      v
single CPU2 JPEG worker
      |
      +--> latest shared TV JPEG
      +--> latest shared GamePad JPEG
                 |
                 +--> Browser / OBS / phone clients

AX TV / DRC final mix
      |
      v
fixed non-blocking PCM ring
      |
      v
stereo PCM16LE / streaming WAV
      |
      +--> Browser / OBS audio clients
```

One video frame is encoded once per source and shared. Frames are dropped instead of blocking the game's render path when the encoder is overloaded. The audio callback follows the same principle: a busy ring slot causes a packet drop rather than a wait inside AX.

## Build

The repository includes a pinned Dockerfile and GitHub Actions build.

With Podman:

```powershell
podman build -t wiiu-web-stream-builder .
podman run --rm -v "${PWD}:/project" wiiu-web-stream-builder make
```

Output:

```text
WiiUWebStream.wps
```

Install to:

```text
SD:/wiiu/environments/aroma/plugins/WiiUWebStream.wps
```

## Current validation status

The current `v0.2.0-dev` source successfully cross-compiles with the pinned devkitPPC/WUT/WUPS toolchain and produces a non-empty `.wps` in CI.

A first physical Wii U test has confirmed that the dashboard loads and both TV and GamePad video streams can operate simultaneously on real hardware. This is an important milestone, but it does **not** yet establish stability across games, long sessions, audio correctness, high-risk resolutions/FPS, or every Aroma environment.

The WUPS-menu memory issue discovered during that test is the reason full configuration has been moved to the Web Settings page and capture/audio are paused while the minimal WUPS menu is open.

## Intentionally deferred

These remain disabled or unavailable pending additional validation:

- mDNS / `wiiu.local`
- H.264 backend
- large multi-buffer capture paths
- asynchronous/double-buffer GX2 readback replacing `GX2DrawDone()`
- automatic title blacklist until the title-ID path is verified
- continuous recording to SD

High-risk 720p/1080p and 30–60 FPS overrides exist, but remain experimental rather than recommended operating modes.

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY.md`.
