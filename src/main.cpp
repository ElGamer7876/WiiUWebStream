#include "capture.hpp"
#include "network.hpp"
#include "settings.hpp"

#include <coreinit/debug.h>
#include <nn/ac.h>

#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config_api.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>

/*
 * Wii U Web Stream
 *
 * Runtime architecture:
 *   GX2 TV/GamePad hooks
 *       -> one mapped linear RGBA buffer per source
 *       -> one low-priority JPEG worker
 *       -> shared latest JPEG frame
 *       -> HTTP/MJPEG fan-out
 *
 * Default ports:
 *   7770 = Web/control/snapshots/OBS pages
 *   7771 = TV MJPEG
 *   7772 = GamePad MJPEG
 */

WUPS_PLUGIN_NAME("Wii U Web Stream");
WUPS_PLUGIN_DESCRIPTION("TV and GamePad MJPEG streaming over LAN");
WUPS_PLUGIN_VERSION("v0.1.0-dev");
WUPS_PLUGIN_AUTHOR("ElGamer7876");
WUPS_PLUGIN_LICENSE("GPL-3.0-or-later");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("wiiu_web_stream");

namespace {

std::atomic_bool gApplicationRunning{false};

void ApplyRuntimeSettings() {
    if (!gApplicationRunning.load()) {
        return;
    }

    if (Settings::enabled.load()) {
        Network::Restart();
    } else {
        Network::Stop();
    }
}

void EnabledChanged(ConfigItemBoolean *, bool value) {
    Settings::SetEnabled(value);
}

void TvEnabledChanged(ConfigItemBoolean *, bool value) {
    Settings::SetTvEnabled(value);
}

void GamePadEnabledChanged(ConfigItemBoolean *, bool value) {
    Settings::SetGamePadEnabled(value);
}

void PortChanged(ConfigItemIntegerRange *item, int value) {
    const std::string id = item->identifier;

    if (id == Settings::KEY_WEB_PORT) {
        Settings::SetWebPort(value);
    } else if (id == Settings::KEY_TV_PORT) {
        Settings::SetTvPort(value);
    } else if (id == Settings::KEY_GAMEPAD_PORT) {
        Settings::SetGamePadPort(value);
    }
}

void FpsChanged(ConfigItemIntegerRange *item, int value) {
    const std::string id = item->identifier;

    if (id == Settings::KEY_TV_FPS) {
        Settings::SetTvFps(value);
    } else if (id == Settings::KEY_GAMEPAD_FPS) {
        Settings::SetGamePadFps(value);
    }
}

void QualityChanged(ConfigItemIntegerRange *, int value) {
    Settings::SetJpegQuality(value);
}

WUPSConfigAPICallbackStatus ConfigMenuOpened(
        WUPSConfigCategoryHandle rootHandle) {
    try {
        WUPSConfigCategory root(rootHandle);

        root.add(
                WUPSConfigItemBoolean::Create(
                        Settings::KEY_ENABLED,
                        "Enable server",
                        Settings::DEFAULT_ENABLED,
                        Settings::enabled.load(),
                        EnabledChanged));

        root.add(
                WUPSConfigItemStub::Create(
                        "Network (LAN only; no UPnP / port forwarding)"));

        root.add(
                WUPSConfigItemIntegerRange::Create(
                        Settings::KEY_WEB_PORT,
                        "Web / combined port",
                        Settings::DEFAULT_WEB_PORT,
                        Settings::webPort.load(),
                        1024,
                        65535,
                        PortChanged));

        root.add(
                WUPSConfigItemIntegerRange::Create(
                        Settings::KEY_TV_PORT,
                        "TV MJPEG port",
                        Settings::DEFAULT_TV_PORT,
                        Settings::tvPort.load(),
                        1024,
                        65535,
                        PortChanged));

        root.add(
                WUPSConfigItemIntegerRange::Create(
                        Settings::KEY_GAMEPAD_PORT,
                        "GamePad MJPEG port",
                        Settings::DEFAULT_GAMEPAD_PORT,
                        Settings::gamepadPort.load(),
                        1024,
                        65535,
                        PortChanged));

        root.add(WUPSConfigItemStub::Create("TV"));

        root.add(
                WUPSConfigItemBoolean::Create(
                        Settings::KEY_TV_ENABLED,
                        "Enable TV capture",
                        Settings::DEFAULT_TV_ENABLED,
                        Settings::tvEnabled.load(),
                        TvEnabledChanged));

        root.add(
                WUPSConfigItemIntegerRange::Create(
                        Settings::KEY_TV_FPS,
                        "TV target FPS",
                        Settings::DEFAULT_TV_FPS,
                        Settings::tvFps.load(),
                        1,
                        15,
                        FpsChanged));

        root.add(WUPSConfigItemStub::Create("GamePad"));

        root.add(
                WUPSConfigItemBoolean::Create(
                        Settings::KEY_GAMEPAD_ENABLED,
                        "Enable GamePad capture",
                        Settings::DEFAULT_GAMEPAD_ENABLED,
                        Settings::gamepadEnabled.load(),
                        GamePadEnabledChanged));

        root.add(
                WUPSConfigItemIntegerRange::Create(
                        Settings::KEY_GAMEPAD_FPS,
                        "GamePad target FPS",
                        Settings::DEFAULT_GAMEPAD_FPS,
                        Settings::gamepadFps.load(),
                        1,
                        15,
                        FpsChanged));

        root.add(WUPSConfigItemStub::Create("JPEG"));

        root.add(
                WUPSConfigItemIntegerRange::Create(
                        Settings::KEY_JPEG_QUALITY,
                        "JPEG quality",
                        Settings::DEFAULT_JPEG_QUALITY,
                        Settings::jpegQuality.load(),
                        10,
                        95,
                        QualityChanged));

        char addressText[96];
        const std::string ip = Network::ConsoleIpAddress();

        std::snprintf(
                addressText,
                sizeof(addressText),
                "Web: http://%s:%d",
                ip.c_str(),
                Settings::webPort.load());

        root.add(WUPSConfigItemStub::Create(addressText));

        std::snprintf(
                addressText,
                sizeof(addressText),
                "TV: http://%s:%d/",
                ip.c_str(),
                Settings::tvPort.load());

        root.add(WUPSConfigItemStub::Create(addressText));

        std::snprintf(
                addressText,
                sizeof(addressText),
                "GamePad: http://%s:%d/",
                ip.c_str(),
                Settings::gamepadPort.load());

        root.add(WUPSConfigItemStub::Create(addressText));

        if (!Settings::PortsAreValid()) {
            root.add(
                    WUPSConfigItemStub::Create(
                            "ERROR: the three ports must be different."));
        }
    } catch (const std::exception &e) {
        OSReport(
                "[WiiUWebStream] Config menu error: %s\n",
                e.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosed() {
    Settings::Save();
    ApplyRuntimeSettings();
}

void StartRuntime() {
    bool expected = false;
    if (!gApplicationRunning.compare_exchange_strong(expected, true)) {
        return;
    }

    nn::ac::Initialize();
    nn::ac::ConnectAsync();

    if (!Capture::Start()) {
        OSReport("[WiiUWebStream] Capture startup failed\n");
    }

    if (Settings::enabled.load()) {
        Network::Start();
    }
}

void StopRuntime() {
    if (!gApplicationRunning.exchange(false)) {
        return;
    }

    // Stop sockets first. This removes stream demand and disconnects clients.
    Network::Stop();

    // Then stop the encoder and free mapped GX2 buffers.
    Capture::Stop();
}

} // namespace

INITIALIZE_PLUGIN() {
    OSReport("[WiiUWebStream] INITIALIZE_PLUGIN\n");

    Settings::Load();

    WUPSConfigAPIOptionsV1 options{
            .name = "Wii U Web Stream",
    };

    const auto result =
            WUPSConfigAPI_Init(
                    options,
                    ConfigMenuOpened,
                    ConfigMenuClosed);

    if (result != WUPSCONFIG_API_RESULT_SUCCESS) {
        OSReport(
                "[WiiUWebStream] WUPS config init failed: %d\n",
                result);
    }
}

DEINITIALIZE_PLUGIN() {
    StopRuntime();
    OSReport("[WiiUWebStream] DEINITIALIZE_PLUGIN\n");
}

ON_APPLICATION_START() {
    StartRuntime();
}

ON_APPLICATION_REQUESTS_EXIT() {
    StopRuntime();
}

ON_APPLICATION_ENDS() {
    StopRuntime();
}
