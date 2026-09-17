# Wii U Web Stream

A Wii U **Aroma/WUPS plugin** that streams the TV and GamePad outputs over your local network using HTTP/MJPEG. It is designed for modern browsers, OBS Studio, VLC/mpv-style MJPEG clients, phones and tablets without a dedicated PC client.

`v0.2.0-dev` is a development build. The current branch compiles in CI with the Wii U toolchain, including the optional AX final-mix audio path, but physical Wii U testing is still required before a stable release.

## Default ports

| Port | Purpose |
|---|---|
| `7770` | Web dashboard, status, snapshots, OBS pages and audio endpoints |
| `7771` | TV MJPEG |
| `7772` | GamePad MJPEG |

No UPnP, NAT-PMP or automatic port forwarding is used. Public/non-LAN source addresses are rejected by the server.

## v0.2 highlights

### Presets

- **Low Latency** — higher FPS, lower JPEG quality and reduced output size.
- **Balanced** — recommended general-purpose profile.
- **Quality** — up to 960x540 TV with lower FPS and higher JPEG quality.
- **OBS** — up to 960x540 TV plus GamePad-oriented output with OBS-friendly quality/FPS.
- **Custom** — manual FPS, resolution and JPEG settings.

Changing a manual FPS/resolution/JPEG value automatically makes the profile Custom.

### High-risk actions

Experimental options stay hidden until both of these switches are enabled in the WUPS configuration menu:

```text
Enable high-risk actions
I understand and accept the risk
```

After both switches are accepted, the plugin exposes a separate **High-risk overrides (HIGH RISK)** section. It currently allows:

- TV target FPS up to 60.
- GamePad target FPS up to 60.
- JPEG quality up to 95.
- TV/GamePad output resolutions up to 1280x720 and 1920x1080.
- **Audio streaming (HIGH RISK)**.
- **Continuous capture without viewers (HIGH RISK)**.

These settings are intentionally not treated as safe defaults. They can increase CPU, GPU, mapped-memory, network and encoder load and may reduce game performance or cause instability. Disabling either high-risk confirmation switch automatically returns FPS, JPEG quality and resolutions to the safe range and disables high-risk audio/continuous capture.

### Audio streaming (HIGH RISK)

Audio is captured from the Wii U AX **device final mix** for TV and DRC/GamePad. The callback only copies/down-converts samples into a small fixed ring; it does not allocate memory, encode JPEG, perform socket I/O or wait for a busy audio slot. If a slot is in use, that audio packet is dropped instead of blocking the AX callback.

The plugin preserves and chains the previously installed TV/DRC final-mix callbacks and restores them when audio streaming is disabled or the plugin runtime stops. The watchdog periodically checks that the callbacks remain installed while the feature is active.

Audio is served as stereo PCM16LE inside a streaming WAV container. Current endpoints are:

```text
http://WIIU_IP:7770/audio/tv.wav
http://WIIU_IP:7770/audio/gamepad.wav
http://WIIU_IP:7771/audio.wav
http://WIIU_IP:7772/audio.wav
```

The `7771` endpoint maps to TV audio and the `7772` endpoint maps to GamePad audio. The dashboard only exposes its audio controls when the audio backend is active.

Audio streaming is optional and defaults to **off**. It requires the high-risk double confirmation first.

### Adaptive FPS

When enabled, the capture path keeps a separate effective FPS. If the shared JPEG worker is still busy when a new frame arrives, the effective FPS backs off rather than blocking the render thread. It gradually recovers toward the configured target after the encoder has remained healthy.

### Client and socket safeguards

- Up to 8 clients per listener.
- Capture still encodes only one JPEG per source/frame and fans that frame out to clients.
- Slow or stalled sockets are gated through WUT `select()` with a 3-second readiness timeout before reads/writes.
- A stalled client is disconnected instead of blocking capture or the listener indefinitely.
- Closing the Aroma config only restarts listeners whose configured port changed or which are currently down.
- The network watchdog retries failed listeners every 5 seconds without touching GX2 state.

### Diagnostics

```text
http://WIIU_IP:7770/api/status
http://WIIU_IP:7770/debug/performance
http://WIIU_IP:7770/health
```

`/api/status` includes uptime, actual FPS, target/effective FPS, connected video clients, last-frame age, capture attempts, rate-limit drops, encoder-busy drops, copy/queue/encode failures, average JPEG size, average scaling time, average JPEG encoding time, listener state, watchdog counters, high-risk state, audio-active state, audio client counts and audio dropped-packet counters.

### Port conflict detection

The three sockets are created/bound before listener threads start. If another plugin already owns one of the ports, the other available listeners can still start and the failed listener reports its bind error in `/api/status` and the WUPS menu.

### Optional access code

Enable **Require URL access code** in the WUPS menu and choose a numeric code. Then use:

```text
http://WIIU_IP:7770/?key=777777
http://WIIU_IP:7771/stream.mjpg?key=777777
http://WIIU_IP:7772/stream.mjpg?key=777777
```

Audio endpoints use the same `?key=CODE` access control when enabled.

This is LAN access control only. HTTP is not encrypted, so the key must not be treated as an Internet-grade password.

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

When high-risk audio streaming is active, single-source OBS pages include their corresponding audio stream automatically. The dual page uses TV audio by default. It also accepts:

```text
audio=tv
audio=gamepad
audio=none
```

Single-source OBS pages accept:

```text
fit=contain|cover
bg=transparent|black
mirror=0|1
rotate=0|90|180|270
```

Example:

```text
http://WIIU_IP:7770/obs/gamepad?fit=cover&mirror=1&rotate=180
```

If access-code protection is enabled, append `key=CODE` as another query parameter.

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
                 +--> Browser / OBS / phone / VLC clients

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

One video frame is encoded once per source and shared between all clients. Slow network clients never hold the capture mutex while sending. Frames are dropped instead of blocking the game's render thread when the encoder is overloaded.

The audio callback follows the same principle: a busy ring slot causes an audio packet drop rather than a wait inside AX.

The capture watchdog is deliberately conservative: it detects stale streams and requests a fresh capture. It does **not** tear down GX2 state from a background thread. The network watchdog only repairs listeners and checks the AX callback chain while high-risk audio is active.

## Build

The repository includes a pinned Dockerfile and GitHub Actions build.

With Podman (open source):

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

The integrated `v0.2.0-dev` source has successfully cross-compiled with the pinned devkitPPC/WUT/WUPS toolchain and produced a non-empty `WiiUWebStream.wps` in CI.

That confirms source/API/link compatibility for the current build. It does **not** prove runtime stability, audio correctness, latency, game compatibility, 60 FPS operation or 1080p performance on physical Wii U hardware. Those still require console testing.

## Intentionally deferred

These ideas remain disabled because they need additional API/hardware validation or could raise Wii U load too far:

- mDNS / `wiiu.local`
- H.264 backend
- large multi-buffer capture paths
- asynchronous/double-buffer GX2 readback replacing `GX2DrawDone()`
- automatic title blacklist until the title-ID path is verified
- continuous recording or automatic repeated snapshots to SD

High-risk 720p/1080p and 30–60 FPS overrides now exist, but they remain experimental rather than recommended operating modes.

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY.md`.
