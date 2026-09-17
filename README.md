# Wii U Web Stream

A Wii U **Aroma/WUPS plugin** that streams the TV and GamePad outputs over your local network using HTTP/MJPEG. It is designed for modern browsers, OBS Studio, VLC/mpv-style MJPEG clients, phones and tablets without a dedicated PC client.

> `v0.2.0-dev` is a development build. The project compiles in CI with the current Wii U toolchain, but hardware testing is still required before a stable release.

## Default ports

| Port | Purpose |
|---|---|
| `7770` | Web dashboard, status, snapshots and OBS pages |
| `7771` | TV MJPEG |
| `7772` | GamePad MJPEG |

No UPnP, NAT-PMP or automatic port forwarding is used. Public/non-LAN source addresses are rejected by the server.

## v0.2 highlights

### Presets

- **Low Latency** — higher FPS, lower JPEG quality and reduced output size.
- **Balanced** — recommended general-purpose profile.
- **Quality** — 720p TV with lower FPS and higher JPEG quality.
- **OBS** — 720p TV plus native-target GamePad size with OBS-friendly quality/FPS.
- **Custom** — manual FPS, resolution and JPEG settings.

Changing a manual FPS/resolution/JPEG value automatically makes the profile Custom.

### Adaptive FPS

When enabled, the capture path keeps a separate effective FPS. If the shared JPEG worker is still busy when a new frame arrives, the effective FPS backs off rather than blocking the render thread. It gradually recovers toward the configured target after the encoder has remained healthy.

### Diagnostics

```text
http://WIIU_IP:7770/api/status
http://WIIU_IP:7770/debug/performance
http://WIIU_IP:7770/health
```

`/api/status` includes actual FPS, target/effective FPS, connected clients, last-frame age, capture attempts, rate-limit drops, encoder-busy drops, copy/queue/encode failures, average JPEG size, average scaling time, average JPEG encoding time and watchdog counters.

### Port conflict detection

The three sockets are now created/bound before listener threads start. If another plugin already owns one of the ports, the other available listeners can still start and the failed listener reports its bind error in `/api/status` and the WUPS menu.

### Optional access code

Enable **Require URL access code** in the WUPS menu and choose a numeric code. Then use:

```text
http://WIIU_IP:7770/?key=777777
http://WIIU_IP:7771/stream.mjpg?key=777777
http://WIIU_IP:7772/stream.mjpg?key=777777
```

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
```

One frame is encoded once per source and shared between all clients. Slow network clients never hold the capture mutex while sending. Frames are dropped instead of blocking the game's render thread when the encoder is overloaded.

The watchdog is deliberately conservative: it detects stale streams and requests a fresh capture. It does **not** tear down GX2 state from a background thread.

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

## Intentionally deferred

These ideas are not enabled yet because they need additional API/hardware validation before they are safe to ship:

- mDNS / `wiiu.local`
- Wii U audio capture/streaming
- H.264 backend
- asynchronous/double-buffer GX2 readback replacing `GX2DrawDone()`
- automatic title blacklist based on title IDs
- writing automatic snapshots to SD

They remain reasonable future milestones, but the project will not invent or depend on unverified Wii U APIs to implement them.

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY.md`.
