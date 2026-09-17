#include "web_settings.hpp"

#include "audio.hpp"
#include "settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

std::string QueryValue(const std::string &query, const std::string &key) {
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const std::string item = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t eq = item.find('=');
        const std::string itemKey = eq == std::string::npos ? item : item.substr(0, eq);
        if (itemKey == key) return eq == std::string::npos ? "" : item.substr(eq + 1);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

int QueryInt(const std::string &query, const std::string &key, int fallback) {
    const std::string raw = QueryValue(query, key);
    if (raw.empty()) return fallback;
    char *end = nullptr;
    const long value = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') return fallback;
    return static_cast<int>(value);
}

bool QueryBool(const std::string &query, const std::string &key, bool fallback) {
    const std::string raw = QueryValue(query, key);
    if (raw.empty()) return fallback;
    return raw == "1" || raw == "true" || raw == "on";
}

const char *Checked(bool value) { return value ? " checked" : ""; }
const char *Selected(int current, int value) { return current == value ? " selected" : ""; }

} // namespace

namespace WebSettings {

std::string BuildPage(const std::string &authSuffix) {
    const bool highRisk = Settings::HighRiskAccepted();
    std::ostringstream out;
    out << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Wii U Web Stream - Settings</title><style>"
           ":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:#0b1118;color:#eef5ff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#15314b,#0b1118 45%)}main{width:min(960px,calc(100% - 24px));margin:auto;padding:24px 0 48px}a{color:#9fd0ff}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.card{background:#111c28dd;border:1px solid #29435c;border-radius:16px;padding:16px;margin:14px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.field{display:flex;flex-direction:column;gap:6px}.check{display:flex;gap:9px;align-items:center;margin:8px 0}input,select,button{font:inherit;border:1px solid #3f6585;background:#10283a;color:#fff;border-radius:9px;padding:9px}button{cursor:pointer;background:#1d4a6c}.warn{border-color:#8b5e24;background:#2a2114}.danger{border-color:#984b4b;background:#2b1717}.muted{color:#9db2c7;font-size:.92rem}.status{min-height:1.4em;margin-top:10px}.row{display:flex;gap:8px;flex-wrap:wrap}</style></head><body><main>";
    out << "<div class=\"top\"><div><h1>Wii U Web Stream Settings</h1><div class=\"muted\">v0.2.0-dev · LAN only</div></div><a href=\"/" << authSuffix << "\">Back to dashboard</a></div>";
    out << "<form id=\"settings\">";

    out << "<section class=\"card\"><h2>General</h2>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"enabled\"" << Checked(Settings::enabled.load()) << "> Enable server</label>"
        << "<div class=\"field\"><label>Preset</label><select name=\"preset\">"
        << "<option value=\"0\"" << Selected(Settings::preset.load(), 0) << ">Custom</option>"
        << "<option value=\"1\"" << Selected(Settings::preset.load(), 1) << ">Low Latency</option>"
        << "<option value=\"2\"" << Selected(Settings::preset.load(), 2) << ">Balanced</option>"
        << "<option value=\"3\"" << Selected(Settings::preset.load(), 3) << ">Quality</option>"
        << "<option value=\"4\"" << Selected(Settings::preset.load(), 4) << ">OBS</option></select></div></section>";

    out << "<section class=\"card\"><h2>Network</h2><div class=\"grid\">"
        << "<label class=\"field\">Web port<input type=\"number\" min=\"1024\" max=\"65535\" name=\"webPort\" value=\"" << Settings::webPort.load() << "\"></label>"
        << "<label class=\"field\">TV port<input type=\"number\" min=\"1024\" max=\"65535\" name=\"tvPort\" value=\"" << Settings::tvPort.load() << "\"></label>"
        << "<label class=\"field\">GamePad port<input type=\"number\" min=\"1024\" max=\"65535\" name=\"gamepadPort\" value=\"" << Settings::gamepadPort.load() << "\"></label></div>"
        << "<p class=\"muted\">Listener changes are applied asynchronously. If you change the web port, reconnect using the new port after a few seconds.</p></section>";

    out << "<section class=\"card\"><h2>Video</h2>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"tvEnabled\"" << Checked(Settings::tvEnabled.load()) << "> Enable TV capture</label>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"gamepadEnabled\"" << Checked(Settings::gamepadEnabled.load()) << "> Enable GamePad capture</label>"
        << "<div class=\"grid\">"
        << "<label class=\"field\">TV target FPS<input type=\"number\" name=\"tvFps\" min=\"1\" max=\"60\" value=\"" << Settings::tvFps.load() << "\"></label>"
        << "<label class=\"field\">GamePad target FPS<input type=\"number\" name=\"gamepadFps\" min=\"1\" max=\"60\" value=\"" << Settings::gamepadFps.load() << "\"></label>"
        << "<label class=\"field\">JPEG quality<input type=\"number\" name=\"jpegQuality\" min=\"35\" max=\"95\" value=\"" << Settings::jpegQuality.load() << "\"></label>";

    auto resolutionSelect = [&out](const char *name, int current) {
        out << "<label class=\"field\">" << (std::string(name) == "tvResolution" ? "TV resolution" : "GamePad resolution") << "<select name=\"" << name << "\">"
            << "<option value=\"0\"" << Selected(current, 0) << ">426x240</option>"
            << "<option value=\"1\"" << Selected(current, 1) << ">640x360</option>"
            << "<option value=\"2\"" << Selected(current, 2) << ">854x480</option>"
            << "<option value=\"3\"" << Selected(current, 3) << ">960x540</option>"
            << "<option value=\"4\"" << Selected(current, 4) << ">1280x720 (HIGH RISK)</option>"
            << "<option value=\"5\"" << Selected(current, 5) << ">1920x1080 (HIGH RISK)</option></select></label>";
    };
    resolutionSelect("tvResolution", Settings::tvResolution.load());
    resolutionSelect("gamepadResolution", Settings::gamepadResolution.load());
    out << "</div><label class=\"check\"><input type=\"checkbox\" name=\"adaptiveFps\"" << Checked(Settings::adaptiveFps.load()) << "> Adaptive FPS</label>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"watchdogEnabled\"" << Checked(Settings::watchdogEnabled.load()) << "> Health watchdog</label></section>";

    out << "<section class=\"card\"><h2>Security & Logging</h2>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"authEnabled\"" << Checked(Settings::authEnabled.load()) << "> Require URL access code</label>"
        << "<div class=\"grid\"><label class=\"field\">Access code<input type=\"number\" name=\"authCode\" min=\"0\" max=\"999999\" value=\"" << Settings::authCode.load() << "\"></label>"
        << "<label class=\"field\">Log level<select name=\"logLevel\">"
        << "<option value=\"0\"" << Selected(Settings::logLevel.load(), 0) << ">Off</option>"
        << "<option value=\"1\"" << Selected(Settings::logLevel.load(), 1) << ">Errors</option>"
        << "<option value=\"2\"" << Selected(Settings::logLevel.load(), 2) << ">Info</option>"
        << "<option value=\"3\"" << Selected(Settings::logLevel.load(), 3) << ">Verbose</option></select></label></div>"
        << "<p class=\"muted\">The access code is LAN access control only; HTTP is not encrypted.</p></section>";

    out << "<section class=\"card warn\"><h2>Advanced / Experimental</h2>"
        << "<label class=\"check\"><input id=\"riskEnable\" type=\"checkbox\" name=\"highRiskEnabled\"" << Checked(Settings::highRiskEnabled.load()) << "> Enable high-risk actions</label>"
        << "<label class=\"check\"><input id=\"riskAccept\" type=\"checkbox\" name=\"highRiskAccepted\"" << Checked(Settings::highRiskAccepted.load()) << "> I understand and accept the risk</label>"
        << "<p><strong>WARNING:</strong> High-risk actions can significantly increase CPU, GPU, memory and network load and may freeze or crash the console.</p>"
        << "<div id=\"riskOptions\" class=\"danger\" style=\"padding:12px;border:1px solid;border-radius:10px;display:" << (highRisk ? "block" : "none") << "\">"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"audioStreaming\"" << Checked(Settings::audioStreaming.load()) << "> Audio streaming (HIGH RISK)</label>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"continuousCapture\"" << Checked(Settings::continuousCapture.load()) << "> Continuous capture without viewers (HIGH RISK)</label>"
        << "<p class=\"muted\">720p/1080p, up to 60 FPS and JPEG quality above 85 are accepted only while HIGH RISK mode is active.</p></div></section>";

    out << "<div class=\"row\"><button type=\"submit\">Save settings</button><a href=\"/" << authSuffix << "\">Cancel</a></div><div id=\"status\" class=\"status muted\"></div></form>";

    out << "<script>const f=document.getElementById('settings'),st=document.getElementById('status'),re=document.getElementById('riskEnable'),ra=document.getElementById('riskAccept'),ro=document.getElementById('riskOptions');"
           "function syncRisk(){ro.style.display=(re.checked&&ra.checked)?'block':'none'}re.onchange=syncRisk;ra.onchange=syncRisk;"
           "f.addEventListener('submit',async e=>{e.preventDefault();const q=new URLSearchParams(new FormData(f));for(const n of ['enabled','tvEnabled','gamepadEnabled','adaptiveFps','watchdogEnabled','authEnabled','highRiskEnabled','highRiskAccepted','audioStreaming','continuousCapture']){const el=f.elements[n];q.set(n,el&&el.checked?'1':'0')}";
    if (!authSuffix.empty()) {
        const size_t keyPos = authSuffix.find("key=");
        if (keyPos != std::string::npos) out << "q.set('key','" << authSuffix.substr(keyPos + 4) << "');";
    }
    out << "st.textContent='Saving...';try{const r=await fetch('/api/settings?'+q.toString(),{method:'POST',cache:'no-store'});const j=await r.json();st.textContent=j.message||'Saved';if(j.ok)setTimeout(()=>location.reload(),800)}catch(err){st.textContent='Save failed'}});syncRisk();</script></main></body></html>";
    return out.str();
}

std::string ApplyQuery(const std::string &query) {
    const bool riskEnabled = QueryBool(query, "highRiskEnabled", Settings::highRiskEnabled.load());
    const bool riskAccepted = riskEnabled && QueryBool(query, "highRiskAccepted", Settings::highRiskAccepted.load());
    Settings::SetHighRiskEnabled(riskEnabled);
    Settings::SetHighRiskAccepted(riskAccepted);

    Settings::SetEnabled(QueryBool(query, "enabled", Settings::enabled.load()));
    Settings::SetWebPort(QueryInt(query, "webPort", Settings::webPort.load()));
    Settings::SetTvPort(QueryInt(query, "tvPort", Settings::tvPort.load()));
    Settings::SetGamePadPort(QueryInt(query, "gamepadPort", Settings::gamepadPort.load()));
    Settings::SetTvEnabled(QueryBool(query, "tvEnabled", Settings::tvEnabled.load()));
    Settings::SetGamePadEnabled(QueryBool(query, "gamepadEnabled", Settings::gamepadEnabled.load()));
    Settings::SetAdaptiveFps(QueryBool(query, "adaptiveFps", Settings::adaptiveFps.load()));
    Settings::SetWatchdogEnabled(QueryBool(query, "watchdogEnabled", Settings::watchdogEnabled.load()));
    Settings::SetAuthEnabled(QueryBool(query, "authEnabled", Settings::authEnabled.load()));
    Settings::SetAuthCode(QueryInt(query, "authCode", Settings::authCode.load()));
    Settings::SetLogLevel(QueryInt(query, "logLevel", Settings::logLevel.load()));

    const int requestedPreset = std::clamp(QueryInt(query, "preset", Settings::preset.load()), 0, 4);
    Settings::SetPreset(requestedPreset);
    if (requestedPreset == static_cast<int>(Settings::Preset::Custom)) {
        Settings::SetTvFps(QueryInt(query, "tvFps", Settings::tvFps.load()));
        Settings::SetGamePadFps(QueryInt(query, "gamepadFps", Settings::gamepadFps.load()));
        Settings::SetJpegQuality(QueryInt(query, "jpegQuality", Settings::jpegQuality.load()));
        Settings::SetTvResolution(QueryInt(query, "tvResolution", Settings::tvResolution.load()));
        Settings::SetGamePadResolution(QueryInt(query, "gamepadResolution", Settings::gamepadResolution.load()));
    }

    Settings::SetAudioStreaming(QueryBool(query, "audioStreaming", false));
    Settings::SetContinuousCapture(QueryBool(query, "continuousCapture", false));
    Settings::EnforceSafeLimits();
    Settings::Save();
    Audio::ApplySettings();

    const bool portsOk = Settings::PortsAreValid();
    std::ostringstream out;
    out << "{\"ok\":" << (portsOk ? "true" : "false")
        << ",\"message\":\"" << (portsOk ? "Settings saved. Listener changes apply within a few seconds." : "Settings saved, but the three ports must be unique and >= 1024.") << "\"}";
    return out.str();
}

} // namespace WebSettings
