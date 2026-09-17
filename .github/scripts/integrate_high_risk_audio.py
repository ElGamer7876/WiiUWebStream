from pathlib import Path
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f'patch marker not found: {label}')
    return text.replace(old, new, 1)


# Capture: high-risk FPS/JPEG limits and optional continuous capture.
p = root / 'src/capture.cpp'
s = p.read_text()
s = replace_once(
    s,
    'int TargetFps(VideoSource source) { return std::clamp(source == VideoSource::TV ? Settings::tvFps.load() : Settings::gamepadFps.load(), 1, 15); }',
    'int TargetFps(VideoSource source) { return std::clamp(source == VideoSource::TV ? Settings::tvFps.load() : Settings::gamepadFps.load(), 1, Settings::HighRiskAccepted() ? 60 : 15); }',
    'capture TargetFps',
)
s = replace_once(
    s,
    '    const int quality = std::clamp(Settings::jpegQuality.load(), 10, 95);',
    '    const int quality = std::clamp(Settings::jpegQuality.load(), 35, Settings::HighRiskAccepted() ? 95 : 85);',
    'capture JPEG quality',
)
s = replace_once(
    s,
    '    if (!hasStreamClients && !oneShot) return;',
    '    if (!hasStreamClients && !oneShot && !(Settings::HighRiskAccepted() && Settings::continuousCapture.load())) return;',
    'continuous capture',
)
p.write_text(s)


# Network: live PCM16 stereo wrapped in a streaming WAV container.
p = root / 'src/network.cpp'
s = p.read_text()
s = replace_once(
    s,
    '#include "network.hpp"\n\n#include "capture.hpp"',
    '#include "network.hpp"\n\n#include "audio.hpp"\n#include "capture.hpp"',
    'network audio include',
)

marker = 'class Listener;\nListener *gWebPtr = nullptr; Listener *gTVPtr = nullptr; Listener *gGamePadPtr = nullptr;'
helper = '''void PutLe16(uint8_t *p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void PutLe32(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::array<uint8_t, 44> BuildStreamingWavHeader(uint32_t sampleRate) {
    std::array<uint8_t, 44> header{};
    std::memcpy(header.data(), "RIFF", 4);
    PutLe32(header.data() + 4, 0x7FFFFFFFu);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    PutLe32(header.data() + 16, 16);
    PutLe16(header.data() + 20, 1);
    PutLe16(header.data() + 22, 2);
    PutLe32(header.data() + 24, sampleRate);
    PutLe32(header.data() + 28, sampleRate * 4);
    PutLe16(header.data() + 32, 4);
    PutLe16(header.data() + 34, 16);
    std::memcpy(header.data() + 36, "data", 4);
    PutLe32(header.data() + 40, 0x7FFFFFFFu);
    return header;
}

'''
s = replace_once(s, marker, helper + marker, 'WAV helpers')

old_status = '''            << "\\\"uptimeMs\\\":" << Network::UptimeMs() << ",\\\"jpegQuality\\\":" << Settings::jpegQuality.load() << ",\\\"tv\\\":" << JsonSource(VideoSource::TV) << ",\\\"gamepad\\\":" << JsonSource(VideoSource::GamePad) << "}";'''
new_status = '''            << "\\\"uptimeMs\\\":" << Network::UptimeMs() << ",\\\"jpegQuality\\\":" << Settings::jpegQuality.load()
            << ",\\\"highRiskAccepted\\\":" << (Settings::HighRiskAccepted() ? "true" : "false")
            << ",\\\"audioEnabled\\\":" << (Audio::IsActive() ? "true" : "false")
            << ",\\\"tvAudioClients\\\":" << Audio::GetStats(Audio::Source::TV).clients
            << ",\\\"gamepadAudioClients\\\":" << Audio::GetStats(Audio::Source::GamePad).clients
            << ",\\\"tvAudioDropped\\\":" << Audio::GetStats(Audio::Source::TV).droppedPackets
            << ",\\\"gamepadAudioDropped\\\":" << Audio::GetStats(Audio::Source::GamePad).droppedPackets
            << ",\\\"tv\\\":" << JsonSource(VideoSource::TV) << ",\\\"gamepad\\\":" << JsonSource(VideoSource::GamePad) << "}";'''
s = replace_once(s, old_status, new_status, 'status audio fields')

for old, new in [
    ('<html lang="es">', '<html lang="en">'),
    ('Aroma/WUPS · v0.2.0-dev · LAN only · MJPEG', 'Aroma/WUPS · v0.2.0-dev · LAN only · MJPEG + PCM audio'),
    ('Pausar/Reanudar', 'Pause/Resume'),
    ('Copiar URL', 'Copy URL'),
    ('${s.tv.clients} clientes', '${s.tv.clients} clients'),
    ('${s.gamepad.clients} clientes', '${s.gamepad.clients} clients'),
    ('<strong>Diagnóstico:</strong>', '<strong>Diagnostics:</strong>'),
]:
    s = s.replace(old, new)

s = replace_once(
    s,
    '<strong>Diagnostics:</strong> <code>/api/status</code> · <code>/debug/performance</code> · <code>/health</code>',
    '<span id="audioBlock" hidden><strong>Audio (HIGH RISK)</strong>: TV <audio id="tvAudio" controls preload="none"></audio> GamePad <audio id="gpAudio" controls preload="none"></audio><br></span><strong>Diagnostics:</strong> <code>/api/status</code> · <code>/debug/performance</code> · <code>/health</code>',
    'dashboard audio block',
)
s = replace_once(
    s,
    "document.getElementById('tvObs').href='/obs/tv'+auth;document.getElementById('gpObs').href='/obs/gamepad'+auth;",
    "document.getElementById('tvObs').href='/obs/tv'+auth;document.getElementById('gpObs').href='/obs/gamepad'+auth;const tvAudio=document.getElementById('tvAudio'),gpAudio=document.getElementById('gpAudio'),audioBlock=document.getElementById('audioBlock');",
    'dashboard audio refs',
)
s = replace_once(
    s,
    "listeners.textContent=`${s.listeners.web==='listening'?'W':'!'} ${s.listeners.tv==='listening'?'T':'!'} ${s.listeners.gamepad==='listening'?'G':'!'}`;",
    "listeners.textContent=`${s.listeners.web==='listening'?'W':'!'} ${s.listeners.tv==='listening'?'T':'!'} ${s.listeners.gamepad==='listening'?'G':'!'}`;audioBlock.hidden=!s.audioEnabled;if(s.audioEnabled){if(!tvAudio.getAttribute('src'))tvAudio.src='/audio/tv.wav'+auth;if(!gpAudio.getAttribute('src'))gpAudio.src='/audio/gamepad.wav'+auth}else{tvAudio.removeAttribute('src');gpAudio.removeAttribute('src')}",
    'dashboard audio state',
)

old_obs = '''        std::ostringstream out; out << "<!doctype html><html><head><meta charset=\\\"utf-8\\\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:" << bg
            << "}img{width:100%;height:100%;display:block;object-fit:" << fit << ";transform:" << (mirror ? "scaleX(-1) " : "") << "rotate(" << rotate << "deg)}</style></head><body><img src=\\\"http://" << Network::ConsoleIpAddress() << ":" << port << "/stream.mjpg" << AuthSuffix(request) << "\\\"></body></html>";
        return out.str();'''
new_obs = '''        const std::string auth = AuthSuffix(request);
        std::ostringstream out; out << "<!doctype html><html><head><meta charset=\\\"utf-8\\\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:" << bg
            << "}img{width:100%;height:100%;display:block;object-fit:" << fit << ";transform:" << (mirror ? "scaleX(-1) " : "") << "rotate(" << rotate << "deg)}</style></head><body><img src=\\\"http://" << Network::ConsoleIpAddress() << ":" << port << "/stream.mjpg" << auth << "\\\">";
        if (Settings::HighRiskAccepted() && Settings::audioStreaming.load()) {
            out << "<audio autoplay src=\\\"/audio/" << (source == VideoSource::TV ? "tv" : "gamepad") << ".wav" << auth << "\\\"></audio>";
        }
        out << "</body></html>";
        return out.str();'''
s = replace_once(s, old_obs, new_obs, 'single OBS audio')

old_dual = '''        std::ostringstream out; out << "<!doctype html><html><head><meta charset=\\\"utf-8\\\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}body{" << (pip ? "position:relative" : "display:grid;grid-template-columns:2fr 1fr") << "}.tv{width:100%;height:100%;object-fit:contain;background:#000}.gp{" << (pip ? "position:absolute;right:2%;bottom:2%;width:32%;height:32%;border-radius:12px;box-shadow:0 4px 20px #000a;" : "width:100%;height:100%;") << "object-fit:contain;background:#000}</style></head><body><img class=\\\"tv\\\" src=\\\"http://" << Network::ConsoleIpAddress() << ":" << Settings::tvPort.load() << "/stream.mjpg" << auth << "\\\"><img class=\\\"gp\\\" src=\\\"http://" << Network::ConsoleIpAddress() << ":" << Settings::gamepadPort.load() << "/stream.mjpg" << auth << "\\\"></body></html>";
        return out.str();'''
new_dual = '''        std::ostringstream out; out << "<!doctype html><html><head><meta charset=\\\"utf-8\\\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}body{" << (pip ? "position:relative" : "display:grid;grid-template-columns:2fr 1fr") << "}.tv{width:100%;height:100%;object-fit:contain;background:#000}.gp{" << (pip ? "position:absolute;right:2%;bottom:2%;width:32%;height:32%;border-radius:12px;box-shadow:0 4px 20px #000a;" : "width:100%;height:100%;") << "object-fit:contain;background:#000}</style></head><body><img class=\\\"tv\\\" src=\\\"http://" << Network::ConsoleIpAddress() << ":" << Settings::tvPort.load() << "/stream.mjpg" << auth << "\\\"><img class=\\\"gp\\\" src=\\\"http://" << Network::ConsoleIpAddress() << ":" << Settings::gamepadPort.load() << "/stream.mjpg" << auth << "\\\">";
        if (Settings::HighRiskAccepted() && Settings::audioStreaming.load()) {
            const std::string audioChoice = QueryValue(request.query, "audio");
            if (audioChoice != "none") {
                const char *audioPath = audioChoice == "gamepad" ? "gamepad" : "tv";
                out << "<audio autoplay src=\\\"/audio/" << audioPath << ".wav" << auth << "\\\"></audio>";
            }
        }
        out << "</body></html>";
        return out.str();'''
s = replace_once(s, old_dual, new_dual, 'dual OBS audio')

marker = '    void HandleWeb(int fd, const HttpRequest &r) {'
audio_method = '''    void ServeAudioWav(int fd, Audio::Source source) {
        if (!Settings::HighRiskAccepted() || !Settings::audioStreaming.load() || !Audio::IsActive()) {
            SendResponse(fd, 503, "Service Unavailable", "text/plain; charset=utf-8", "Audio streaming is disabled. Enable and accept high-risk actions first.\\n");
            return;
        }

        Audio::ClientConnected(source);
        uint64_t sequence = 0;
        Audio::Packet packet{};
        for (int i = 0; i < 200 && !Audio::ReadAfter(source, sequence, packet); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (packet.sampleRate == 0 || packet.bytes == 0) {
            Audio::ClientDisconnected(source);
            SendResponse(fd, 503, "Service Unavailable", "text/plain; charset=utf-8", "No audio frame available yet.\\n");
            return;
        }

        const auto wav = BuildStreamingWavHeader(packet.sampleRate);
        std::string header = "HTTP/1.1 200 OK\\r\\nServer: WiiUWebStream/0.2\\r\\nConnection: close\\r\\nCache-Control: no-store\\r\\nPragma: no-cache\\r\\nAccess-Control-Allow-Origin: *\\r\\nContent-Type: audio/wav\\r\\n\\r\\n";
        bool ok = SendAll(fd, header) && SendAll(fd, wav.data(), wav.size()) && SendAll(fd, packet.pcm.data(), packet.bytes);
        while (ok && mRunning.load() && Network::IsRunning() && Audio::IsActive()) {
            Audio::Packet next{};
            if (!Audio::ReadAfter(source, sequence, next)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            ok = SendAll(fd, next.pcm.data(), next.bytes);
        }
        Audio::ClientDisconnected(source);
    }

'''
s = replace_once(s, marker, audio_method + marker, 'ServeAudioWav')

s = replace_once(
    s,
    '        if (r.path == "/snapshot/tv.jpg") { ServeSnapshot(fd, VideoSource::TV); return; }',
    '        if (r.path == "/audio/tv.wav") { ServeAudioWav(fd, Audio::Source::TV); return; }\n        if (r.path == "/audio/gamepad.wav") { ServeAudioWav(fd, Audio::Source::GamePad); return; }\n        if (r.path == "/snapshot/tv.jpg") { ServeSnapshot(fd, VideoSource::TV); return; }',
    'web audio routes',
)
s = replace_once(
    s,
    '        if (r.path == "/snapshot.jpg") { ServeSnapshot(fd, source); return; }',
    '        if (r.path == "/audio.wav") { ServeAudioWav(fd, source == VideoSource::TV ? Audio::Source::TV : Audio::Source::GamePad); return; }\n        if (r.path == "/snapshot.jpg") { ServeSnapshot(fd, source); return; }',
    'stream audio route',
)

p.write_text(s)
print('patched capture.cpp and network.cpp')
