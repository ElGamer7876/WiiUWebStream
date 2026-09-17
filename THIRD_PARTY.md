# Third-party projects and technical references

Wii U Web Stream is developed for the open-source Wii U homebrew ecosystem.

## ScreenshotWUPS

Repository: `wiiu-env/ScreenshotWUPS`

Used as the primary technical reference for current Aroma/WUPS GX2 capture patterns, including TV/DRC scan targets, mapped GX2 memory, linear RGBA readback, AA resolve/copy behavior, and the relevant WUPS GX2 hooks.

## WiiUPluginSystem

Repository: `wiiu-env/WiiUPluginSystem`

Reference for plugin metadata, lifecycle, configuration APIs and `.wps` build rules.

## libmappedmemory / MemoryMappingModule

Repository: `wiiu-env/libmappedmemory`

Provides GX2-compatible mapped memory allocation used by the capture buffers.

## ftpiiu_plugin

Repository: `wiiu-env/ftpiiu_plugin`

Reference for `nn::ac` networking and retrieving the Wii U assigned IPv4 address.

## Ristretto

Repository: `wiiu-smarthome/Ristretto`

Reference for a modern HTTP/TCP server running inside an Aroma plugin.

## libjpeg-turbo

Repository: `libjpeg-turbo/libjpeg-turbo`

TurboJPEG API is used to encode in-memory RGBA frames as JPEG.

## Licensing

Each third-party project remains subject to its own license. Wii U Web Stream itself is distributed under `GPL-3.0-or-later`.
