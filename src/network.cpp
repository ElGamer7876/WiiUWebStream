#include "network.hpp"

#include "capture.hpp"
#include "frame_store.hpp"
#include "settings.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <nn/ac.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

constexpr size_t MAX_REQUEST_BYTES = 8192;
constexpr size_t MAX_CLIENTS_PER_PORT = 8;
constexpr const char *MJPEG_BOUNDARY = "wiiuframe";

enum class ListenerKind {
    Web,
    TV,
    GamePad,
};

struct HttpRequest {
    std::string method;
    std::string path;
};

bool SendAll(int socketFd, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t sentTotal = 0;

    while (sentTotal < size) {
        const ssize_t sent =
                send(socketFd,
                     bytes + sentTotal,
                     size - sentTotal,
                     MSG_NOSIGNAL);

        if (sent <= 0) {
            return false;
        }

        sentTotal += static_cast<size_t>(sent);
    }

    return true;
}

bool SendAll(int socketFd, const std::string &data) {
    return SendAll(socketFd, data.data(), data.size());
}

void CloseSocket(int socketFd) {
    if (socketFd < 0) {
        return;
    }

    shutdown(socketFd, SHUT_RDWR);
    close(socketFd);
}

void SendResponse(int socketFd,
                  int status,
                  const char *reason,
                  const char *contentType,
                  const std::string &body,
                  const char *extraHeaders = nullptr) {
    std::string header;
    header.reserve(384);

    header += "HTTP/1.1 ";
    header += std::to_string(status);
    header += " ";
    header += reason;
    header += "\r\n";
    header += "Server: WiiUWebStream/0.7\r\n";
    header += "Content-Type: ";
    header += contentType;
    header += "\r\n";
    header += "Content-Length: ";
    header += std::to_string(body.size());
    header += "\r\n";
    header += "Connection: close\r\n";
    header += "Cache-Control: no-store\r\n";
    header += "Access-Control-Allow-Origin: *\r\n";
    header += "X-Content-Type-Options: nosniff\r\n";

    if (extraHeaders != nullptr) {
        header += extraHeaders;
    }

    header += "\r\n";

    SendAll(socketFd, header);
    SendAll(socketFd, body);
}

void SendBinaryResponse(int socketFd,
                        int status,
                        const char *reason,
                        const char *contentType,
                        const uint8_t *data,
                        size_t size) {
    std::string header;
    header.reserve(320);

    header += "HTTP/1.1 ";
    header += std::to_string(status);
    header += " ";
    header += reason;
    header += "\r\n";
    header += "Server: WiiUWebStream/0.7\r\n";
    header += "Content-Type: ";
    header += contentType;
    header += "\r\n";
    header += "Content-Length: ";
    header += std::to_string(size);
    header += "\r\n";
    header += "Connection: close\r\n";
    header += "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n";
    header += "Pragma: no-cache\r\n";
    header += "Access-Control-Allow-Origin: *\r\n";
    header += "\r\n";

    if (!SendAll(socketFd, header)) {
        return;
    }

    if (size > 0 && data != nullptr) {
        SendAll(socketFd, data, size);
    }
}

bool ReadRequest(int socketFd, HttpRequest &request) {
    std::string buffer;
    buffer.reserve(1024);

    std::array<char, 512> chunk{};

    while (buffer.size() < MAX_REQUEST_BYTES) {
        const ssize_t received =
                recv(socketFd,
                     chunk.data(),
                     chunk.size(),
                     0);

        if (received <= 0) {
            return false;
        }

        buffer.append(chunk.data(), static_cast<size_t>(received));

        const size_t lineEnd = buffer.find("\r\n");
        if (lineEnd != std::string::npos) {
            const std::string firstLine = buffer.substr(0, lineEnd);

            const size_t firstSpace = firstLine.find(' ');
            if (firstSpace == std::string::npos) {
                return false;
            }

            const size_t secondSpace =
                    firstLine.find(' ', firstSpace + 1);
            if (secondSpace == std::string::npos) {
                return false;
            }

            request.method = firstLine.substr(0, firstSpace);
            request.path =
                    firstLine.substr(
                            firstSpace + 1,
                            secondSpace - firstSpace - 1);

            const size_t query = request.path.find('?');
            if (query != std::string::npos) {
                request.path.resize(query);
            }

            return true;
        }
    }

    return false;
}

bool IsLanAddress(uint32_t networkAddress) {
    const uint32_t ip = ntohl(networkAddress);

    const uint8_t a = static_cast<uint8_t>((ip >> 24) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((ip >> 16) & 0xFF);

    if (a == 10) {
        return true;
    }

    if (a == 172 && b >= 16 && b <= 31) {
        return true;
    }

    if (a == 192 && b == 168) {
        return true;
    }

    if (a == 169 && b == 254) {
        return true;
    }

    if (a == 127) {
        return true;
    }

    // RFC 6598 Shared Address Space; useful on some private/CGNAT LANs.
    if (a == 100 && b >= 64 && b <= 127) {
        return true;
    }

    return false;
}

std::string BuildMainHtml() {
    const int tvPort = Settings::tvPort.load();
    const int gamepadPort = Settings::gamepadPort.load();

    std::ostringstream out;
    out << R"HTML(<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<link rel="icon" href="data:,">
<title>Wii U Web Stream</title>
<style>
:root{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:#0d1117;color:#e6edf3}
*{box-sizing:border-box}
body{margin:0;background:#0d1117}
main{width:min(1280px,calc(100% - 28px));margin:auto;padding:26px 0 40px}
h1{margin:0;font-size:clamp(1.8rem,5vw,2.7rem)}
.sub{color:#8b949e;margin:.4rem 0 1.2rem}
.badge{display:inline-flex;gap:.55rem;align-items:center;background:#161b22;border:1px solid #30363d;border-radius:999px;padding:.55rem .8rem;margin-bottom:1.2rem}
.dot{width:.65rem;height:.65rem;border-radius:50%;background:#3fb950;box-shadow:0 0 10px #3fb950}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:14px;overflow:hidden}
.video{background:#000;aspect-ratio:16/9;display:grid;place-items:center}
.video img{display:block;width:100%;height:100%;object-fit:contain}
.body{padding:14px 16px}
h2{margin:0 0 7px;font-size:1.2rem}
.meta{color:#8b949e;display:flex;gap:14px;flex-wrap:wrap;font-size:.95rem}
.links{margin-top:18px;padding:16px;background:#161b22;border:1px solid #30363d;border-radius:14px;line-height:1.85}
code{background:#21262d;padding:.15rem .4rem;border-radius:5px}
.warn{color:#d29922}
</style>
</head>
<body>
<main>
<h1>Wii U Web Stream</h1>
<p class="sub">Aroma/WUPS · LAN only · MJPEG</p>
<div class="badge"><span class="dot"></span><span id="server">Servidor activo</span></div>

<div class="grid">
<article class="card">
  <div class="video"><img id="tv" alt="TV"></div>
  <div class="body">
    <h2>TV</h2>
    <div class="meta">
      <span id="tvFps">FPS: —</span>
      <span id="tvClients">Clientes: —</span>
      <span id="tvRes">Resolución: —</span>
    </div>
  </div>
</article>
<article class="card">
  <div class="video"><img id="gamepad" alt="GamePad"></div>
  <div class="body">
    <h2>GamePad</h2>
    <div class="meta">
      <span id="gpFps">FPS: —</span>
      <span id="gpClients">Clientes: —</span>
      <span id="gpRes">Resolución: —</span>
    </div>
  </div>
</article>
</div>

<div class="links">
<strong>OBS / acceso directo</strong><br>
TV: <code id="tvUrl"></code><br>
GamePad: <code id="gpUrl"></code><br>
OBS alternativo: <code>/obs/tv</code>, <code>/obs/gamepad</code>, <code>/obs/dual</code><br>
Snapshots: <code>/snapshot/tv.jpg</code>, <code>/snapshot/gamepad.jpg</code>
</div>
</main>

<script>
const host = location.hostname;
const tvPort = )HTML";
    out << tvPort;
    out << R"HTML(;
const gpPort = )HTML";
    out << gamepadPort;
    out << R"HTML(;

const tvStream = `http://${host}:${tvPort}/stream.mjpg`;
const gpStream = `http://${host}:${gpPort}/stream.mjpg`;

document.getElementById('tv').src = tvStream;
document.getElementById('gamepad').src = gpStream;
document.getElementById('tvUrl').textContent = `http://${host}:${tvPort}/`;
document.getElementById('gpUrl').textContent = `http://${host}:${gpPort}/`;

async function updateStatus(){
  try{
    const r=await fetch('/api/status',{cache:'no-store'});
    const s=await r.json();
    document.getElementById('server').textContent=`Servidor activo · ${s.ip}`;
    document.getElementById('tvFps').textContent=`FPS: ${s.tv.fps.toFixed(1)}`;
    document.getElementById('tvClients').textContent=`Clientes: ${s.tv.clients}`;
    document.getElementById('tvRes').textContent=`Resolución: ${s.tv.width}×${s.tv.height}`;
    document.getElementById('gpFps').textContent=`FPS: ${s.gamepad.fps.toFixed(1)}`;
    document.getElementById('gpClients').textContent=`Clientes: ${s.gamepad.clients}`;
    document.getElementById('gpRes').textContent=`Resolución: ${s.gamepad.width}×${s.gamepad.height}`;
  }catch(e){
    document.getElementById('server').textContent='Sin respuesta de /api/status';
  }
}
updateStatus();
setInterval(updateStatus,1000);
</script>
</body>
</html>)HTML";

    return out.str();
}

std::string BuildObsHtml(VideoSource source) {
    const int port =
            source == VideoSource::TV
            ? Settings::tvPort.load()
            : Settings::gamepadPort.load();

    std::ostringstream out;
    out << R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
img{width:100%;height:100%;display:block;object-fit:contain}
</style>
</head>
<body>
<img id="stream">
<script>
document.getElementById('stream').src='http://'+location.hostname+':)HTML";
    out << port;
    out << R"HTML(/stream.mjpg';
</script>
</body>
</html>)HTML";

    return out.str();
}

std::string BuildObsDualHtml() {
    const int tvPort = Settings::tvPort.load();
    const int gpPort = Settings::gamepadPort.load();

    std::ostringstream out;
    out << R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
body{display:grid;grid-template-columns:2fr 1fr;gap:0}
.frame{width:100%;height:100%;object-fit:contain;background:#000}
</style>
</head>
<body>
<img id="tv" class="frame"><img id="gp" class="frame">
<script>
const h=location.hostname;
document.getElementById('tv').src='http://'+h+':)HTML";
    out << tvPort;
    out << R"HTML(/stream.mjpg';
document.getElementById('gp').src='http://'+h+':)HTML";
    out << gpPort;
    out << R"HTML(/stream.mjpg';
</script>
</body>
</html>)HTML";

    return out.str();
}

std::string BuildViewHtml(VideoSource source) {
    std::ostringstream out;
    out << "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<style>html,body{margin:0;width:100%;height:100%;background:#000;"
           "overflow:hidden}img{width:100%;height:100%;object-fit:contain}</style>"
           "</head><body><img src=\"/stream.mjpg\" alt=\"";
    out << VideoSourceName(source);
    out << "\"></body></html>";
    return out.str();
}

std::string BuildStatusJson() {
    const auto tv = FrameStore::Stats(VideoSource::TV);
    const auto gp = FrameStore::Stats(VideoSource::GamePad);

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);

    out << "{";
    out << "\"ok\":true,";
    out << "\"ip\":\"" << Network::ConsoleIpAddress() << "\",";
    out << "\"ports\":{";
    out << "\"web\":" << Settings::webPort.load() << ",";
    out << "\"tv\":" << Settings::tvPort.load() << ",";
    out << "\"gamepad\":" << Settings::gamepadPort.load();
    out << "},";
    out << "\"jpegQuality\":" << Settings::jpegQuality.load() << ",";
    out << "\"tv\":{";
    out << "\"enabled\":" << (Settings::tvEnabled.load() ? "true" : "false") << ",";
    out << "\"fps\":" << tv.fps << ",";
    out << "\"clients\":" << tv.clients << ",";
    out << "\"width\":" << tv.width << ",";
    out << "\"height\":" << tv.height << ",";
    out << "\"sequence\":" << tv.sequence;
    out << "},";
    out << "\"gamepad\":{";
    out << "\"enabled\":" << (Settings::gamepadEnabled.load() ? "true" : "false") << ",";
    out << "\"fps\":" << gp.fps << ",";
    out << "\"clients\":" << gp.clients << ",";
    out << "\"width\":" << gp.width << ",";
    out << "\"height\":" << gp.height << ",";
    out << "\"sequence\":" << gp.sequence;
    out << "}";
    out << "}";

    return out.str();
}

void ServeSnapshot(int socketFd, VideoSource source) {
    const uint64_t before = FrameStore::Sequence(source);

    Capture::RequestOne(source);

    auto frame =
            FrameStore::WaitForNew(
                    source,
                    before,
                    std::chrono::milliseconds(1500));

    if (!frame) {
        frame = FrameStore::Latest(source);
    }

    if (!frame || frame->bytes.empty()) {
        SendResponse(
                socketFd,
                503,
                "Service Unavailable",
                "text/plain; charset=utf-8",
                "No hay un frame disponible todavia.\n");
        return;
    }

    SendBinaryResponse(
            socketFd,
            200,
            "OK",
            "image/jpeg",
            frame->bytes.data(),
            frame->bytes.size());
}

class StreamClientCountGuard {
public:
    explicit StreamClientCountGuard(VideoSource source) : mSource(source) {
        FrameStore::ClientConnected(mSource);
    }

    ~StreamClientCountGuard() {
        FrameStore::ClientDisconnected(mSource);
    }

private:
    VideoSource mSource;
};

void ServeMjpeg(int socketFd,
                VideoSource source,
                const std::atomic_bool &listenerRunning) {
    StreamClientCountGuard clientGuard(source);

    std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Server: WiiUWebStream/0.7\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
            "Pragma: no-cache\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=";
    header += MJPEG_BOUNDARY;
    header += "\r\n\r\n";

    if (!SendAll(socketFd, header)) {
        return;
    }

    uint64_t lastSequence = 0;

    while (listenerRunning.load() && Network::IsRunning()) {
        auto frame =
                FrameStore::WaitForNew(
                        source,
                        lastSequence,
                        std::chrono::milliseconds(100));

        if (!frame) {
            continue;
        }

        lastSequence = frame->sequence;

        std::string partHeader;
        partHeader.reserve(180);
        partHeader += "--";
        partHeader += MJPEG_BOUNDARY;
        partHeader += "\r\n";
        partHeader += "Content-Type: image/jpeg\r\n";
        partHeader += "Content-Length: ";
        partHeader += std::to_string(frame->bytes.size());
        partHeader += "\r\n";
        partHeader += "X-Sequence: ";
        partHeader += std::to_string(frame->sequence);
        partHeader += "\r\n\r\n";

        if (!SendAll(socketFd, partHeader) ||
            !SendAll(socketFd,
                     frame->bytes.data(),
                     frame->bytes.size()) ||
            !SendAll(socketFd, "\r\n", 2)) {
            break;
        }
    }
}

class Listener {
public:
    explicit Listener(ListenerKind kind) : mKind(kind) {}

    ~Listener() {
        Stop();
    }

    bool Start(uint16_t port) {
        if (mRunning.exchange(true)) {
            return true;
        }

        mPort = port;
        mThread = std::thread([this]() { AcceptLoop(); });

        auto *thread =
                reinterpret_cast<OSThread *>(mThread.native_handle());
        OSSetThreadName(thread, ThreadName());
        OSSetThreadAffinity(thread, OS_THREAD_ATTRIB_AFFINITY_CPU2);

        return true;
    }

    void Stop() {
        if (!mRunning.exchange(false)) {
            return;
        }

        const int listenSocket = mListenSocket.exchange(-1);
        if (listenSocket >= 0) {
            CloseSocket(listenSocket);
        }

        for (auto &slot : mClients) {
            const int socketFd = slot.socket.exchange(-1);
            if (socketFd >= 0) {
                CloseSocket(socketFd);
            }
        }

        FrameStore::NotifyAll();

        if (mThread.joinable()) {
            mThread.join();
        }

        for (auto &slot : mClients) {
            if (slot.thread.joinable()) {
                slot.thread.join();
            }
            slot.active.store(false);
        }
    }

    bool IsRunning() const {
        return mRunning.load();
    }

private:
    struct ClientSlot {
        std::atomic_bool active{false};
        std::atomic_int socket{-1};
        std::thread thread;
    };

    const char *ThreadName() const {
        switch (mKind) {
            case ListenerKind::Web: return "WiiUWebStream Web";
            case ListenerKind::TV: return "WiiUWebStream TV";
            case ListenerKind::GamePad: return "WiiUWebStream GamePad";
        }

        return "WiiUWebStream";
    }

    VideoSource SourceForStreamListener() const {
        return mKind == ListenerKind::TV
                ? VideoSource::TV
                : VideoSource::GamePad;
    }

    void AcceptLoop() {
        const int serverSocket =
                socket(AF_INET, SOCK_STREAM, 0);

        if (serverSocket < 0) {
            OSReport("[WiiUWebStream] socket() failed on port %u\n", mPort);
            mRunning.store(false);
            return;
        }

        int reuse = 1;
        setsockopt(
                serverSocket,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(mPort);

        if (bind(serverSocket,
                 reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) < 0) {
            OSReport("[WiiUWebStream] bind() failed on port %u\n", mPort);
            CloseSocket(serverSocket);
            mRunning.store(false);
            return;
        }

        if (listen(serverSocket, 8) < 0) {
            OSReport("[WiiUWebStream] listen() failed on port %u\n", mPort);
            CloseSocket(serverSocket);
            mRunning.store(false);
            return;
        }

        mListenSocket.store(serverSocket);

        OSReport("[WiiUWebStream] Listening on 0.0.0.0:%u\n", mPort);

        while (mRunning.load()) {
            sockaddr_in clientAddress{};
            socklen_t clientAddressLength = sizeof(clientAddress);

            const int clientSocket =
                    accept(
                            serverSocket,
                            reinterpret_cast<sockaddr *>(&clientAddress),
                            &clientAddressLength);

            if (clientSocket < 0) {
                if (!mRunning.load()) {
                    break;
                }
                continue;
            }

            // LAN-only safety. No UPnP/NAT-PMP is ever performed, and public
            // source addresses are rejected even if a router is misconfigured.
            if (!IsLanAddress(clientAddress.sin_addr.s_addr)) {
                SendResponse(
                        clientSocket,
                        403,
                        "Forbidden",
                        "text/plain; charset=utf-8",
                        "LAN only.\n");
                CloseSocket(clientSocket);
                continue;
            }

            ClientSlot *freeSlot = nullptr;

            for (auto &slot : mClients) {
                if (!slot.active.load()) {
                    if (slot.thread.joinable()) {
                        slot.thread.join();
                    }
                    freeSlot = &slot;
                    break;
                }
            }

            if (freeSlot == nullptr) {
                SendResponse(
                        clientSocket,
                        503,
                        "Service Unavailable",
                        "text/plain; charset=utf-8",
                        "Demasiados clientes simultaneos.\n");
                CloseSocket(clientSocket);
                continue;
            }

            freeSlot->active.store(true);
            freeSlot->socket.store(clientSocket);

            freeSlot->thread =
                    std::thread([this, freeSlot]() {
                        HandleClient(*freeSlot);

                        const int socketFd =
                                freeSlot->socket.exchange(-1);
                        if (socketFd >= 0) {
                            CloseSocket(socketFd);
                        }

                        freeSlot->active.store(false);
                    });
        }

        const int owned = mListenSocket.exchange(-1);
        if (owned >= 0) {
            CloseSocket(owned);
        }
    }

    void HandleClient(ClientSlot &slot) {
        const int socketFd = slot.socket.load();
        if (socketFd < 0) {
            return;
        }

        HttpRequest request;
        if (!ReadRequest(socketFd, request)) {
            return;
        }

        if (request.method != "GET") {
            SendResponse(
                    socketFd,
                    405,
                    "Method Not Allowed",
                    "text/plain; charset=utf-8",
                    "Solo GET esta soportado.\n",
                    "Allow: GET\r\n");
            return;
        }

        if (mKind == ListenerKind::Web) {
            HandleWebRequest(socketFd, request.path);
        } else {
            HandleStreamPortRequest(
                    socketFd,
                    request.path,
                    SourceForStreamListener());
        }
    }

    void HandleWebRequest(int socketFd, const std::string &path) {
        if (path == "/") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "text/html; charset=utf-8",
                    BuildMainHtml());
            return;
        }

        if (path == "/health" || path == "/api/status") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "application/json; charset=utf-8",
                    BuildStatusJson());
            return;
        }

        if (path == "/stream/tv") {
            if (!Settings::tvEnabled.load()) {
                SendResponse(socketFd, 503, "Service Unavailable",
                             "text/plain; charset=utf-8",
                             "TV capture is disabled.\n");
                return;
            }
            ServeMjpeg(socketFd, VideoSource::TV, mRunning);
            return;
        }

        if (path == "/stream/gamepad") {
            if (!Settings::gamepadEnabled.load()) {
                SendResponse(socketFd, 503, "Service Unavailable",
                             "text/plain; charset=utf-8",
                             "GamePad capture is disabled.\n");
                return;
            }
            ServeMjpeg(socketFd, VideoSource::GamePad, mRunning);
            return;
        }

        if (path == "/snapshot/tv.jpg") {
            ServeSnapshot(socketFd, VideoSource::TV);
            return;
        }

        if (path == "/snapshot/gamepad.jpg") {
            ServeSnapshot(socketFd, VideoSource::GamePad);
            return;
        }

        if (path == "/obs/tv") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "text/html; charset=utf-8",
                    BuildObsHtml(VideoSource::TV));
            return;
        }

        if (path == "/obs/gamepad") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "text/html; charset=utf-8",
                    BuildObsHtml(VideoSource::GamePad));
            return;
        }

        if (path == "/obs/dual") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "text/html; charset=utf-8",
                    BuildObsDualHtml());
            return;
        }

        if (path == "/favicon.ico") {
            SendResponse(
                    socketFd,
                    204,
                    "No Content",
                    "text/plain",
                    "");
            return;
        }

        SendResponse(
                socketFd,
                404,
                "Not Found",
                "text/plain; charset=utf-8",
                "404 - Recurso no encontrado.\n");
    }

    void HandleStreamPortRequest(int socketFd,
                                 const std::string &path,
                                 VideoSource source) {
        if (path == "/" || path == "/stream.mjpg") {
            const bool enabled = source == VideoSource::TV
                    ? Settings::tvEnabled.load()
                    : Settings::gamepadEnabled.load();

            if (!enabled) {
                SendResponse(socketFd, 503, "Service Unavailable",
                             "text/plain; charset=utf-8",
                             "Capture for this source is disabled.\n");
                return;
            }

            ServeMjpeg(socketFd, source, mRunning);
            return;
        }

        if (path == "/snapshot.jpg") {
            ServeSnapshot(socketFd, source);
            return;
        }

        if (path == "/view") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "text/html; charset=utf-8",
                    BuildViewHtml(source));
            return;
        }

        if (path == "/health") {
            SendResponse(
                    socketFd,
                    200,
                    "OK",
                    "application/json; charset=utf-8",
                    BuildStatusJson());
            return;
        }

        SendResponse(
                socketFd,
                404,
                "Not Found",
                "text/plain; charset=utf-8",
                "404 - Recurso no encontrado.\n");
    }

    ListenerKind mKind;
    uint16_t mPort = 0;
    std::atomic_bool mRunning{false};
    std::atomic_int mListenSocket{-1};
    std::thread mThread;
    std::array<ClientSlot, MAX_CLIENTS_PER_PORT> mClients;
};

std::atomic_bool gNetworkRunning{false};
std::mutex gLifecycleMutex;

Listener gWebListener{ListenerKind::Web};
Listener gTVListener{ListenerKind::TV};
Listener gGamePadListener{ListenerKind::GamePad};

} // namespace

namespace Network {

std::string ConsoleIpAddress() {
    uint32_t address = 0;

    // Current Aroma plugins such as ftpiiu use nn::ac::GetAssignedAddress.
    nn::ac::GetAssignedAddress(&address);

    if (address == 0) {
        return "0.0.0.0";
    }

    char buffer[32];
    std::snprintf(
            buffer,
            sizeof(buffer),
            "%u.%u.%u.%u",
            (address >> 24) & 0xFF,
            (address >> 16) & 0xFF,
            (address >> 8) & 0xFF,
            address & 0xFF);

    return buffer;
}

bool Start() {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);

    if (gNetworkRunning.load()) {
        return true;
    }

    if (!Settings::enabled.load()) {
        return true;
    }

    if (!Settings::PortsAreValid()) {
        OSReport("[WiiUWebStream] Ports invalid or duplicated\n");
        return false;
    }

    gNetworkRunning.store(true);

    gWebListener.Start(
            static_cast<uint16_t>(Settings::webPort.load()));
    gTVListener.Start(
            static_cast<uint16_t>(Settings::tvPort.load()));
    gGamePadListener.Start(
            static_cast<uint16_t>(Settings::gamepadPort.load()));

    OSReport(
            "[WiiUWebStream] Web http://%s:%d/ | TV :%d | GamePad :%d\n",
            ConsoleIpAddress().c_str(),
            Settings::webPort.load(),
            Settings::tvPort.load(),
            Settings::gamepadPort.load());

    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);

    if (!gNetworkRunning.exchange(false)) {
        return;
    }

    FrameStore::NotifyAll();

    gWebListener.Stop();
    gTVListener.Stop();
    gGamePadListener.Stop();

    OSReport("[WiiUWebStream] Network stopped\n");
}

void Restart() {
    Stop();
    Start();
}

bool IsRunning() {
    return gNetworkRunning.load();
}

} // namespace Network
