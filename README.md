# Wii U Web Stream

> **Development status:** experimental / pre-release. The current source still needs its first successful devkitPPC CI build and hardware validation on a real Wii U before a stable release is published.

> [!WARNING]
> Installing and using this plugin can cause the Wii U to **freeze during gameplay**. In some cases the console may stop responding completely and require a **forced power-off** by holding the console's POWER button. A forced shutdown can cause loss of unsaved progress and may increase the risk of data or filesystem corruption. If this happens, we recommend lowering the output resolution first (and, if necessary, FPS/JPEG quality). If freezes continue, disable or uninstall Wii U Web Stream before continuing to play. Use this development build at your own risk, save your game frequently, and avoid testing HIGH RISK options when important unsaved data is open.

Wii U Web Stream is an open-source **Aroma/WUPS plugin** that exposes the Wii U **TV output** and **GamePad output** over the local network as HTTP/MJPEG streams. The streams are designed to work in a normal web browser and in OBS Studio through Browser Source.

## Features

- TV and GamePad streaming at the same time.
- Browser UI with both outputs.
- Direct MJPEG endpoints for OBS and other clients.
- JPEG snapshot endpoints.
- Configurable FPS, JPEG quality and ports through the WUPS configuration menu.
- Shared encoded frames: multiple viewers reuse the same JPEG instead of re-encoding once per client.
- Capture only when a viewer or snapshot request needs a frame.
- LAN-only filtering and **no UPnP/NAT-PMP/automatic port forwarding**.

## Default ports

| Port | Purpose |
| ---: | --- |
| `7770` | Web UI, status API, snapshots and OBS helper pages |
| `7771` | TV MJPEG stream |
| `7772` | GamePad MJPEG stream |

## URLs

Replace `WIIU_IP` with the console IP address.

```text
http://WIIU_IP:7770/                       Web UI
http://WIIU_IP:7770/api/status             JSON status
http://WIIU_IP:7770/snapshot/tv.jpg        TV snapshot
http://WIIU_IP:7770/snapshot/gamepad.jpg   GamePad snapshot
http://WIIU_IP:7770/obs/tv                 OBS helper page (TV)
http://WIIU_IP:7770/obs/gamepad            OBS helper page (GamePad)
http://WIIU_IP:7770/obs/dual               OBS helper page (both)

http://WIIU_IP:7771/                       TV MJPEG
http://WIIU_IP:7772/                       GamePad MJPEG
```

## OBS Studio

For maximum flexibility, add two **Browser Sources**:

```text
TV:      http://WIIU_IP:7770/obs/tv
GamePad: http://WIIU_IP:7770/obs/gamepad
```

Recommended initial source sizes:

- TV: `640x360`
- GamePad: `854x480`

The default target is intentionally conservative at 5 FPS per output until hardware profiling is complete. The WUPS menu allows values from 1 to 15 FPS.

## Installation

Stable releases will provide `WiiUWebStream.wps` and an SD-ready ZIP. The plugin belongs at:

```text
sd:/wiiu/environments/aroma/plugins/WiiUWebStream.wps
```

The GX2 readback path depends on Aroma's MemoryMappingModule.

## Building

### Podman (open source)

```powershell
podman build -t wiiu-web-stream-builder .
podman run --rm -v "${PWD}:/project" wiiu-web-stream-builder make
```

### Docker

```powershell
docker build -t wiiu-web-stream-builder .
docker run --rm -v "${PWD}:/project" wiiu-web-stream-builder make
```

Output:

```text
WiiUWebStream.wps
```

GitHub Actions also runs this build automatically on pushes and pull requests.

## Capture architecture

The capture path follows current Aroma/WUPS GX2 techniques used by ScreenshotWUPS:

```text
GX2 TV / GamePad scan buffer
        |
        v
mapped linear RGBA8 buffer
        |
        v
low-priority JPEG worker (libjpeg-turbo)
        |
        v
latest shared JPEG frame
        |
        +--> browser
        +--> OBS
        +--> phone/tablet
```

The rendering hook does not perform socket I/O or JPEG compression. If the encoder still owns the previous buffer, a new frame is dropped instead of blocking the game to wait for the encoder.

## Security model

This project is intended for a trusted local network.

- No UPnP.
- No NAT-PMP.
- No automatic router configuration.
- Incoming source addresses are restricted to common private/LAN address ranges.

Do not manually expose ports `7770`-`7772` to the public Internet. Authentication is not implemented yet.

## Project status / testing

Before the first stable release, the following must be verified on real hardware:

- clean devkitPPC/WUPS build;
- TV capture in multiple titles;
- GamePad capture in multiple titles;
- simultaneous TV + GamePad stream;
- OBS Browser Source compatibility;
- application switching and HOME Menu behavior;
- extended streaming for crashes, leaks and stutter.

Please use GitHub Issues for reproducible test results and include the game/application, Aroma version, selected FPS/quality and relevant logs.

## Credits and technical references

The implementation was informed by several open-source Wii U projects, especially:

- `wiiu-env/ScreenshotWUPS` for modern GX2 TV/GamePad screenshot capture patterns;
- `wiiu-env/WiiUPluginSystem` for WUPS plugin/configuration APIs;
- `wiiu-env/libmappedmemory` for GX2-compatible mapped allocations;
- `wiiu-env/ftpiiu_plugin` and `wiiu-smarthome/Ristretto` for network/server patterns on Aroma/WUT;
- `libjpeg-turbo` for JPEG encoding.

See [`THIRD_PARTY.md`](THIRD_PARTY.md) for details.

## License

Copyright (C) 2026 ElGamer7876

Wii U Web Stream is licensed under **GNU GPL v3 or later** (`GPL-3.0-or-later`). See [`LICENSE`](LICENSE).
