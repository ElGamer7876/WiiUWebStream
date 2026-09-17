#include "network.hpp"

#include "capture.hpp"
#include "frame_store.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "settings.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <nn/ac.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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
constexpr int CLIENT_TIMEOUT_SECONDS = 3;
constexpr const char *MJPEG_BOUNDARY = "wiiuframe";

enum class ListenerKind { Web, TV, GamePad };
struct HttpRequest { std::string method; std::string path; std::string query; };

bool WaitForSocket(int socketFd, bool writable) {
    fd_set readSet; fd_set writeSet;
    FD_ZERO(&readSet); FD_ZERO(&writeSet);
    if (writable) FD_SET(socketFd, &writeSet); else FD_SET(socketFd, &readSet);
    timeval timeout{}; timeout.tv_sec = CLIENT_TIMEOUT_SECONDS; timeout.tv_usec = 0;
    const int result = select(socketFd + 1, writable ? nullptr : &readSet, writable ? &writeSet : nullptr, nullptr, &timeout);
    return result > 0;
}

bool SendAll(int socketFd, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data); size_t sentTotal = 0;
    while (sentTotal < size) {
        if (!WaitForSocket(socketFd, true)) return false;
        const ssize_t sent = send(socketFd, bytes + sentTotal, size - sentTotal, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        sentTotal += static_cast<size_t>(sent);
    }
    return true;
}
bool SendAll(int socketFd, const std::string &data) { return SendAll(socketFd, data.data(), data.size()); }
void CloseSocket(int socketFd) { if (socketFd >= 0) { shutdown(socketFd, SHUT_RDWR); close(socketFd); } }

void SendResponse(int socketFd, int status, const char *reason, const char *contentType, const std::string &body, const char *extraHeaders = nullptr) {
    std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    header += "Server: WiiUWebStream/0.2\r\nContent-Type: "; header += contentType; header += "\r\nContent-Length: "; header += std::to_string(body.size());
    header += "\r\nConnection: close\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nX-Content-Type-Options: nosniff\r\n";
    if (extraHeaders) header += extraHeaders;
    header += "\r\n"; SendAll(socketFd, header); SendAll(socketFd, body);
}

void SendBinaryResponse(int socketFd, const char *contentType, const uint8_t *data, size_t size) {
    std::string header = "HTTP/1.1 200 OK\r\nServer: WiiUWebStream/0.2\r\nContent-Type: "; header += contentType;
    header += "\r\nContent-Length: " + std::to_string(size) + "\r\nConnection: close\r\nCache-Control: no-store, no-cache, must-revalidate, max-age=0\r\nPragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    if (SendAll(socketFd, header) && data && size) SendAll(socketFd, data, size);
}

bool ReadRequest(int socketFd, HttpRequest &request) {
    std::string buffer; buffer.reserve(1024); std::array<char, 512> chunk{};
    while (buffer.size() < MAX_REQUEST_BYTES) {
        if (!WaitForSocket(socketFd, false)) return false;
        const ssize_t received = recv(socketFd, chunk.data(), chunk.size(), 0); if (received <= 0) return false;
        buffer.append(chunk.data(), static_cast<size_t>(received));
        const size_t lineEnd = buffer.find("\r\n"); if (lineEnd == std::string::npos) continue;
        const std::string firstLine = buffer.substr(0, lineEnd); const size_t firstSpace = firstLine.find(' '); if (firstSpace == std::string::npos) return false;
        const size_t secondSpace = firstLine.find(' ', firstSpace + 1); if (secondSpace == std::string::npos) return false;
        request.method = firstLine.substr(0, firstSpace); std::string target = firstLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        const size_t q = target.find('?'); request.path = q == std::string::npos ? target : target.substr(0, q); request.query = q == std::string::npos ? "" : target.substr(q + 1);
        return true;
    }
    return false;
}

std::string QueryValue(const std::string &query, const std::string &key) {
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start); const std::string item = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t eq = item.find('=');
        if ((eq == std::string::npos ? item : item.substr(0, eq)) == key) return eq == std::string::npos ? "" : item.substr(eq + 1);
        if (end == std::string::npos) break; start = end + 1;
    }
    return "";
}

bool IsAuthorized(const HttpRequest &request) {
    if (!Settings::authEnabled.load()) return true;
    const std::string raw = QueryValue(request.query, "key"); if (raw.empty()) return false;
    char *end = nullptr; const long code = std::strtol(raw.c_str(), &end, 10);
    return end != raw.c_str() && *end == '\0' && code == Settings::authCode.load();
}

std::string AuthSuffix(const HttpRequest &request) {
    if (!Settings::authEnabled.load()) return "";
    const std::string key = QueryValue(request.query, "key");
    return key.empty() ? "" : "?key=" + key;
}

bool IsLanAddress(uint32_t networkAddress) {
    const uint32_t ip = ntohl(networkAddress); const uint8_t a = static_cast<uint8_t>((ip >> 24) & 0xFF); const uint8_t b = static_cast<uint8_t>((ip >> 16) & 0xFF);
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) || a == 127 || (a == 100 && b >= 64 && b <= 127);
}

std::string JsonSource(VideoSource source) {
    const auto stream = FrameStore::Stats(source); const auto m = Metrics::Snapshot(source);
    const uint64_t avgBytes = m.encodedFrames ? m.jpegBytesTotal / m.encodedFrames : 0;
    const uint64_t avgScale = m.encodedFrames ? m.scaleMicrosTotal / m.encodedFrames : 0;
    const uint64_t avgEncode = m.encodedFrames ? m.encodeMicrosTotal / m.encodedFrames : 0;
    std::ostringstream out; out.setf(std::ios::fixed); out.precision(2);
    out << "{\"enabled\":" << ((source == VideoSource::TV ? Settings::tvEnabled.load() : Settings::gamepadEnabled.load()) ? "true" : "false")
        << ",\"fps\":" << stream.fps << ",\"targetFps\":" << (source == VideoSource::TV ? Settings::tvFps.load() : Settings::gamepadFps.load())
        << ",\"effectiveFps\":" << m.effectiveFps << ",\"clients\":" << stream.clients << ",\"width\":" << stream.width << ",\"height\":" << stream.height
        << ",\"sequence\":" << stream.sequence << ",\"lastFrameAgeMs\":" << stream.lastFrameAgeMs << ",\"latestJpegBytes\":" << stream.latestJpegBytes
        << ",\"captureAttempts\":" << m.captureAttempts << ",\"copiedFrames\":" << m.copiedFrames << ",\"encodedFrames\":" << m.encodedFrames
        << ",\"droppedRateLimit\":" << m.droppedRateLimit << ",\"droppedEncoderBusy\":" << m.droppedEncoderBusy
        << ",\"copyFailures\":" << m.copyFailures << ",\"queueFailures\":" << m.queueFailures << ",\"encodeFailures\":" << m.encodeFailures
        << ",\"avgJpegBytes\":" << avgBytes << ",\"avgScaleUs\":" << avgScale << ",\"avgEncodeUs\":" << avgEncode
        << ",\"watchdogStalls\":" << m.watchdogStalls << ",\"watchdogNudges\":" << m.watchdogNudges << "}";
    return out.str();
}

class Listener;
Listener *gWebPtr = nullptr; Listener *gTVPtr = nullptr; Listener *gGamePadPtr = nullptr;

class Listener {
public:
    explicit Listener(ListenerKind kind) : mKind(kind) {}
    ~Listener() { Stop(); }

    bool Start(uint16_t port) {
        if (mRunning.load()) return true; mPort = port; SetError("");
        const int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket < 0) { SetError("socket failed"); return false; }
        int reuse = 1; setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(mPort);
        if (bind(serverSocket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
            const int error = errno; CloseSocket(serverSocket); SetError("bind failed (errno " + std::to_string(error) + ")"); Log::Error("port %u bind failed errno=%d", mPort, error); return false;
        }
        if (listen(serverSocket, 8) < 0) { const int error = errno; CloseSocket(serverSocket); SetError("listen failed (errno " + std::to_string(error) + ")"); return false; }
        mListenSocket.store(serverSocket); mRunning.store(true); mThread = std::thread([this]() { AcceptLoop(); });
        auto *thread = reinterpret_cast<OSThread *>(mThread.native_handle()); OSSetThreadName(thread, ThreadName()); OSSetThreadAffinity(thread, OS_THREAD_ATTRIB_AFFINITY_CPU2);
        Log::Info("listening on 0.0.0.0:%u", mPort); return true;
    }

    void Stop() {
        if (!mRunning.exchange(false)) return;
        const int listenSocket = mListenSocket.exchange(-1); if (listenSocket >= 0) CloseSocket(listenSocket);
        for (auto &slot : mClients) { const int fd = slot.socket.exchange(-1); if (fd >= 0) CloseSocket(fd); }
        FrameStore::NotifyAll(); if (mThread.joinable()) mThread.join();
        for (auto &slot : mClients) { if (slot.thread.joinable()) slot.thread.join(); slot.active.store(false); }
    }
    bool IsRunning() const { return mRunning.load(); }
    uint16_t Port() const { return mPort; }
    std::string Error() const { std::lock_guard<std::mutex> lock(mErrorMutex); return mError; }
    std::string StateText() const { return IsRunning() ? "listening" : (Error().empty() ? "stopped" : Error()); }

private:
    struct ClientSlot { std::atomic_bool active{false}; std::atomic_int socket{-1}; std::thread thread; };
    const char *ThreadName() const { return mKind == ListenerKind::Web ? "WiiUWebStream Web" : (mKind == ListenerKind::TV ? "WiiUWebStream TV" : "WiiUWebStream GamePad"); }
    VideoSource Source() const { return mKind == ListenerKind::TV ? VideoSource::TV : VideoSource::GamePad; }
    void SetError(const std::string &error) { std::lock_guard<std::mutex> lock(mErrorMutex); mError = error; }

    void AcceptLoop() {
        while (mRunning.load()) {
            sockaddr_in clientAddress{}; socklen_t len = sizeof(clientAddress);
            const int clientSocket = accept(mListenSocket.load(), reinterpret_cast<sockaddr *>(&clientAddress), &len);
            if (clientSocket < 0) { if (!mRunning.load()) break; continue; }
            if (!IsLanAddress(clientAddress.sin_addr.s_addr)) { SendResponse(clientSocket, 403, "Forbidden", "text/plain; charset=utf-8", "LAN only.\n"); CloseSocket(clientSocket); continue; }
            ClientSlot *freeSlot = nullptr;
            for (auto &slot : mClients) if (!slot.active.load()) { if (slot.thread.joinable()) slot.thread.join(); freeSlot = &slot; break; }
            if (!freeSlot) { SendResponse(clientSocket, 503, "Service Unavailable", "text/plain; charset=utf-8", "Too many simultaneous clients.\n"); CloseSocket(clientSocket); continue; }
            freeSlot->active.store(true); freeSlot->socket.store(clientSocket);
            freeSlot->thread = std::thread([this, freeSlot]() { HandleClient(*freeSlot); const int fd = freeSlot->socket.exchange(-1); if (fd >= 0) CloseSocket(fd); freeSlot->active.store(false); });
        }
    }

    void Unauthorized(int fd) { SendResponse(fd, 401, "Unauthorized", "text/plain; charset=utf-8", "Missing or invalid ?key=XXXXXX\n"); }

    void HandleClient(ClientSlot &slot) {
        const int fd = slot.socket.load(); if (fd < 0) return; HttpRequest request; if (!ReadRequest(fd, request)) return;
        if (request.method != "GET") { SendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET is supported.\n", "Allow: GET\r\n"); return; }
        if (request.path != "/health" && !IsAuthorized(request)) { Unauthorized(fd); return; }
        if (mKind == ListenerKind::Web) HandleWeb(fd, request); else HandleStream(fd, request, Source());
    }

    std::string BuildStatusJson() const {
        std::ostringstream out; out << "{\"ok\":true,\"version\":\"0.2.0-dev\",\"ip\":\"" << Network::ConsoleIpAddress() << "\",\"preset\":\"" << Settings::PresetName()
            << "\",\"adaptiveFps\":" << (Settings::adaptiveFps.load() ? "true" : "false") << ",\"authEnabled\":" << (Settings::authEnabled.load() ? "true" : "false")
            << ",\"ports\":{\"web\":" << Settings::webPort.load() << ",\"tv\":" << Settings::tvPort.load() << ",\"gamepad\":" << Settings::gamepadPort.load() << "},\"listeners\":{"
            << "\"web\":\"" << (gWebPtr ? gWebPtr->StateText() : "unknown") << "\",\"tv\":\"" << (gTVPtr ? gTVPtr->StateText() : "unknown") << "\",\"gamepad\":\"" << (gGamePadPtr ? gGamePadPtr->StateText() : "unknown") << "\"},"
            << "\"uptimeMs\":" << Network::UptimeMs() << ",\"jpegQuality\":" << Settings::jpegQuality.load() << ",\"tv\":" << JsonSource(VideoSource::TV) << ",\"gamepad\":" << JsonSource(VideoSource::GamePad) << "}";
        return out.str();
    }

    std::string BuildMainHtml(const HttpRequest &request) const {
        const std::string auth = AuthSuffix(request); const int tvPort = Settings::tvPort.load(), gpPort = Settings::gamepadPort.load();
        std::ostringstream out;
        out << R"HTML(<!doctype html><html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark"><link rel="icon" href="data:,"><title>Wii U Web Stream</title><style>
:root{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:#0b1118;color:#eef5ff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#15314b,#0b1118 45%)}main{width:min(1280px,calc(100% - 24px));margin:auto;padding:24px 0 50px}h1{margin:0;font-size:clamp(1.8rem,5vw,2.8rem)}.sub{color:#9db2c7;margin:.35rem 0 1rem}.top,.grid,.metrics{display:grid;gap:14px}.grid{grid-template-columns:repeat(auto-fit,minmax(310px,1fr))}.metrics{grid-template-columns:repeat(auto-fit,minmax(190px,1fr));margin-top:14px}.card,.metric{background:#111c28dd;border:1px solid #29435c;border-radius:16px;overflow:hidden;box-shadow:0 10px 30px #0005}.video{background:#000;aspect-ratio:16/9;display:grid;place-items:center}.video img{display:block;width:100%;height:100%;object-fit:contain}.body,.metric{padding:14px 16px}h2{margin:0 0 8px}.meta{display:flex;gap:12px;flex-wrap:wrap;color:#9db2c7;font-size:.9rem}.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}button,a.btn{border:1px solid #3f6585;background:#18334b;color:white;border-radius:10px;padding:8px 11px;text-decoration:none;cursor:pointer}.good{color:#56d364}.warn{color:#e3b341}.bad{color:#ff7b72}code{background:#172536;padding:2px 5px;border-radius:5px}.value{font-size:1.35rem;font-weight:700}.label{color:#9db2c7;font-size:.82rem}.links{margin-top:14px;padding:16px;background:#111c28dd;border:1px solid #29435c;border-radius:16px;line-height:1.8}</style></head><body><main><h1>Wii U Web Stream</h1><p class="sub">Aroma/WUPS · v0.2.0-dev · LAN only · MJPEG</p><div class="grid"><article class="card"><div class="video"><img id="tv" alt="TV"></div><div class="body"><h2>TV</h2><div class="meta"><span id="tvFps">FPS —</span><span id="tvRes">—</span><span id="tvClients">—</span></div><div class="row"><button onclick="toggle('tv')">Pausar/Reanudar</button><button onclick="copyUrl('tv')">Copiar URL</button><a class="btn" id="tvObs">OBS</a></div></div></article><article class="card"><div class="video"><img id="gp" alt="GamePad"></div><div class="body"><h2>GamePad</h2><div class="meta"><span id="gpFps">FPS —</span><span id="gpRes">—</span><span id="gpClients">—</span></div><div class="row"><button onclick="toggle('gp')">Pausar/Reanudar</button><button onclick="copyUrl('gp')">Copiar URL</button><a class="btn" id="gpObs">OBS</a></div></div></article></div><div class="metrics"><div class="metric"><div class="label">Preset</div><div class="value" id="preset">—</div></div><div class="metric"><div class="label">JPEG</div><div class="value" id="jpeg">—</div></div><div class="metric"><div class="label">TV encode</div><div class="value" id="tvEnc">—</div></div><div class="metric"><div class="label">GamePad encode</div><div class="value" id="gpEnc">—</div></div><div class="metric"><div class="label">Listeners</div><div class="value" id="listeners">—</div></div></div><div class="links"><strong>OBS</strong>: <a class="btn" href="/obs/dual)HTML" << auth << R"HTML(">Dual</a> <a class="btn" href="/obs/dual?layout=pip">PiP</a><br><strong>Diagnóstico:</strong> <code>/api/status</code> · <code>/debug/performance</code> · <code>/health</code></div><script>
const host=location.hostname, auth=)HTML" << (auth.empty() ? "''" : "'" + auth + "'") << R"HTML(;const tvPort=)HTML" << tvPort << R"HTML(,gpPort=)HTML" << gpPort << R"HTML(;const urls={tv:`http://${host}:${tvPort}/stream.mjpg${auth}`,gp:`http://${host}:${gpPort}/stream.mjpg${auth}`};document.getElementById('tv').src=urls.tv;document.getElementById('gp').src=urls.gp;document.getElementById('tvObs').href='/obs/tv'+auth;document.getElementById('gpObs').href='/obs/gamepad'+auth;function toggle(id){const el=document.getElementById(id);el.src=el.src?'':urls[id]}async function copyUrl(id){try{await navigator.clipboard.writeText(urls[id])}catch(e){prompt('URL',urls[id])}}async function update(){try{const r=await fetch('/api/status'+auth,{cache:'no-store'}),s=await r.json();preset.textContent=s.preset;jpeg.textContent=s.jpegQuality;tvFps.textContent=`${s.tv.fps.toFixed(1)} FPS / ${s.tv.effectiveFps}`;gpFps.textContent=`${s.gamepad.fps.toFixed(1)} FPS / ${s.gamepad.effectiveFps}`;tvRes.textContent=`${s.tv.width}×${s.tv.height}`;gpRes.textContent=`${s.gamepad.width}×${s.gamepad.height}`;tvClients.textContent=`${s.tv.clients} clientes`;gpClients.textContent=`${s.gamepad.clients} clientes`;tvEnc.textContent=`${(s.tv.avgEncodeUs/1000).toFixed(1)} ms`;gpEnc.textContent=`${(s.gamepad.avgEncodeUs/1000).toFixed(1)} ms`;listeners.textContent=`${s.listeners.web==='listening'?'W':'!'} ${s.listeners.tv==='listening'?'T':'!'} ${s.listeners.gamepad==='listening'?'G':'!'}`;}catch(e){listeners.textContent='offline'}}update();setInterval(update,1000);</script></main></body></html>)HTML";
        return out.str();
    }

    std::string BuildObsHtml(VideoSource source, const HttpRequest &request) const {
        const int port = source == VideoSource::TV ? Settings::tvPort.load() : Settings::gamepadPort.load();
        const std::string fitRaw = QueryValue(request.query, "fit"); const std::string fit = fitRaw == "cover" ? "cover" : "contain";
        const std::string bgRaw = QueryValue(request.query, "bg"); const std::string bg = bgRaw == "black" ? "#000" : "transparent";
        const bool mirror = QueryValue(request.query, "mirror") == "1";
        const std::string rotateRaw = QueryValue(request.query, "rotate"); const int rotate = (rotateRaw == "90" || rotateRaw == "180" || rotateRaw == "270") ? std::atoi(rotateRaw.c_str()) : 0;
        std::ostringstream out; out << "<!doctype html><html><head><meta charset=\"utf-8\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:" << bg
            << "}img{width:100%;height:100%;display:block;object-fit:" << fit << ";transform:" << (mirror ? "scaleX(-1) " : "") << "rotate(" << rotate << "deg)}</style></head><body><img src=\"http://" << Network::ConsoleIpAddress() << ":" << port << "/stream.mjpg" << AuthSuffix(request) << "\"></body></html>";
        return out.str();
    }

    std::string BuildObsDualHtml(const HttpRequest &request) const {
        const bool pip = QueryValue(request.query, "layout") == "pip"; const std::string auth = AuthSuffix(request);
        std::ostringstream out; out << "<!doctype html><html><head><meta charset=\"utf-8\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}body{" << (pip ? "position:relative" : "display:grid;grid-template-columns:2fr 1fr") << "}.tv{width:100%;height:100%;object-fit:contain;background:#000}.gp{" << (pip ? "position:absolute;right:2%;bottom:2%;width:32%;height:32%;border-radius:12px;box-shadow:0 4px 20px #000a;" : "width:100%;height:100%;") << "object-fit:contain;background:#000}</style></head><body><img class=\"tv\" src=\"http://" << Network::ConsoleIpAddress() << ":" << Settings::tvPort.load() << "/stream.mjpg" << auth << "\"><img class=\"gp\" src=\"http://" << Network::ConsoleIpAddress() << ":" << Settings::gamepadPort.load() << "/stream.mjpg" << auth << "\"></body></html>";
        return out.str();
    }

    void ServeSnapshot(int fd, VideoSource source) {
        const uint64_t before = FrameStore::Sequence(source); Capture::RequestOne(source);
        auto frame = FrameStore::WaitForNew(source, before, std::chrono::milliseconds(1500)); if (!frame) frame = FrameStore::Latest(source);
        if (!frame || frame->bytes.empty()) { SendResponse(fd, 503, "Service Unavailable", "text/plain; charset=utf-8", "No frame available yet.\n"); return; }
        SendBinaryResponse(fd, "image/jpeg", frame->bytes.data(), frame->bytes.size());
    }

    void ServeMjpeg(int fd, VideoSource source) {
        FrameStore::ClientConnected(source);
        std::string header = "HTTP/1.1 200 OK\r\nServer: WiiUWebStream/0.2\r\nConnection: close\r\nCache-Control: no-store, no-cache, must-revalidate, max-age=0\r\nPragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: multipart/x-mixed-replace; boundary=";
        header += MJPEG_BOUNDARY; header += "\r\n\r\n";
        if (SendAll(fd, header)) {
            uint64_t lastSequence = 0;
            while (mRunning.load() && Network::IsRunning()) {
                auto frame = FrameStore::WaitForNew(source, lastSequence, std::chrono::milliseconds(500)); if (!frame) continue; lastSequence = frame->sequence;
                std::string part = "--" + std::string(MJPEG_BOUNDARY) + "\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(frame->bytes.size()) + "\r\nX-Sequence: " + std::to_string(frame->sequence) + "\r\n\r\n";
                if (!SendAll(fd, part) || !SendAll(fd, frame->bytes.data(), frame->bytes.size()) || !SendAll(fd, "\r\n", 2)) break;
            }
        }
        FrameStore::ClientDisconnected(source);
    }

    void HandleWeb(int fd, const HttpRequest &r) {
        if (r.path == "/") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildMainHtml(r)); return; }
        if (r.path == "/health") { SendResponse(fd, 200, "OK", "application/json", "{\"ok\":true}"); return; }
        if (r.path == "/api/status" || r.path == "/debug/performance") { SendResponse(fd, 200, "OK", "application/json; charset=utf-8", BuildStatusJson()); return; }
        if (r.path == "/snapshot/tv.jpg") { ServeSnapshot(fd, VideoSource::TV); return; }
        if (r.path == "/snapshot/gamepad.jpg") { ServeSnapshot(fd, VideoSource::GamePad); return; }
        if (r.path == "/obs/tv") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildObsHtml(VideoSource::TV, r)); return; }
        if (r.path == "/obs/gamepad") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildObsHtml(VideoSource::GamePad, r)); return; }
        if (r.path == "/obs/dual") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildObsDualHtml(r)); return; }
        if (r.path == "/favicon.ico") { SendResponse(fd, 204, "No Content", "text/plain", ""); return; }
        SendResponse(fd, 404, "Not Found", "text/plain; charset=utf-8", "404\n");
    }

    void HandleStream(int fd, const HttpRequest &r, VideoSource source) {
        if (r.path == "/" || r.path == "/stream.mjpg") { ServeMjpeg(fd, source); return; }
        if (r.path == "/snapshot.jpg") { ServeSnapshot(fd, source); return; }
        if (r.path == "/health") { SendResponse(fd, 200, "OK", "application/json", "{\"ok\":true}"); return; }
        if (r.path == "/view") {
            std::string body = "<!doctype html><html><style>html,body{margin:0;width:100%;height:100%;background:#000}img{width:100%;height:100%;object-fit:contain}</style><body><img src=\"/stream.mjpg" + AuthSuffix(r) + "\"></body></html>";
            SendResponse(fd, 200, "OK", "text/html; charset=utf-8", body); return;
        }
        SendResponse(fd, 404, "Not Found", "text/plain; charset=utf-8", "404\n");
    }

    ListenerKind mKind; uint16_t mPort = 0; std::atomic_bool mRunning{false}; std::atomic_int mListenSocket{-1}; std::thread mThread; std::array<ClientSlot, MAX_CLIENTS_PER_PORT> mClients;
    mutable std::mutex mErrorMutex; std::string mError;
};

std::atomic_bool gNetworkRunning{false}; std::mutex gLifecycleMutex;
std::chrono::steady_clock::time_point gNetworkStarted{};
Listener gWeb{ListenerKind::Web}; Listener gTV{ListenerKind::TV}; Listener gGamePad{ListenerKind::GamePad};

} // namespace

namespace Network {
std::string ConsoleIpAddress() {
    uint32_t address = 0; nn::ac::GetAssignedAddress(&address); if (address == 0) return "0.0.0.0";
    char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF); return buffer;
}

bool Start() {
    std::lock_guard<std::mutex> lock(gLifecycleMutex); if (gNetworkRunning.load() || !Settings::enabled.load()) return true;
    if (!Settings::PortsAreValid()) { Log::Error("ports invalid or duplicated"); return false; }
    gWebPtr = &gWeb; gTVPtr = &gTV; gGamePadPtr = &gGamePad;
    const bool web = gWeb.Start(static_cast<uint16_t>(Settings::webPort.load()));
    const bool tv = gTV.Start(static_cast<uint16_t>(Settings::tvPort.load()));
    const bool gp = gGamePad.Start(static_cast<uint16_t>(Settings::gamepadPort.load()));
    gNetworkRunning.store(web || tv || gp);
    if (gNetworkRunning.load()) gNetworkStarted = std::chrono::steady_clock::now();
    Log::Info("network start: web=%d tv=%d gamepad=%d", web, tv, gp);
    return gNetworkRunning.load();
}
void Stop() {
    std::lock_guard<std::mutex> lock(gLifecycleMutex); if (!gNetworkRunning.exchange(false) && !gWeb.IsRunning() && !gTV.IsRunning() && !gGamePad.IsRunning()) return;
    FrameStore::NotifyAll(); gWeb.Stop(); gTV.Stop(); gGamePad.Stop(); gNetworkStarted = {}; Log::Info("network stopped");
}
void Restart() { Stop(); Start(); }

void Reconfigure() {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    if (!Settings::enabled.load()) {
        gWeb.Stop(); gTV.Stop(); gGamePad.Stop();
        gNetworkRunning.store(false);
        gNetworkStarted = {};
        return;
    }
    if (!Settings::PortsAreValid()) { Log::Error("ports invalid or duplicated"); return; }
    auto ensure = [](Listener &listener, uint16_t desiredPort) {
        if (listener.IsRunning() && listener.Port() != desiredPort) listener.Stop();
        if (!listener.IsRunning()) listener.Start(desiredPort);
    };
    ensure(gWeb, static_cast<uint16_t>(Settings::webPort.load()));
    ensure(gTV, static_cast<uint16_t>(Settings::tvPort.load()));
    ensure(gGamePad, static_cast<uint16_t>(Settings::gamepadPort.load()));
    const bool any = gWeb.IsRunning() || gTV.IsRunning() || gGamePad.IsRunning();
    if (any && !gNetworkRunning.load()) gNetworkStarted = std::chrono::steady_clock::now();
    gNetworkRunning.store(any);
}

void EnsureListeners() { Reconfigure(); }
bool IsRunning() { return gNetworkRunning.load(); }
uint64_t UptimeMs() {
    if (!gNetworkRunning.load() || gNetworkStarted.time_since_epoch().count() == 0) return 0;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gNetworkStarted).count());
}
std::string ListenerStatusSummary() {
    std::ostringstream out; out << "W:" << gWeb.StateText() << " T:" << gTV.StateText() << " G:" << gGamePad.StateText(); return out.str();
}
} // namespace Network
