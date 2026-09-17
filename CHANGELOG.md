# Changelog

## v0.2.0-dev

- Added performance presets: Custom, Low Latency, Balanced, Quality and OBS.
- Added selectable TV/GamePad output resolutions.
- Added adaptive FPS that backs off when the encoder is busy and recovers gradually.
- Added detailed capture/encode telemetry and `/debug/performance`.
- Added safe health watchdog that requests a fresh frame when a watched stream stalls.
- Listener sockets are bound before their threads start, so port conflicts are detected immediately.
- Added per-listener status to `/api/status` and the Aroma config menu.
- Added optional URL access code (`?key=XXXXXX`) for LAN access control.
- Added configurable log level.
- Improved web dashboard with pause/copy controls and live performance data.
- Improved OBS pages with `fit`, `bg`, `mirror`, `rotate`, and dual/PiP layouts.
- Version bumped to `v0.2.0-dev`.

## v0.1.0-dev

- Initial development build.
- TV and GamePad capture via GX2 hooks.
- JPEG/MJPEG streaming.
- Web UI, snapshots and OBS pages.
- WUPS configuration.
- Ports 7770/7771/7772.
