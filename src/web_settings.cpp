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

struct RequestedSettings {
    bool enabled;
    int webPort;
    int tvPort;
    int gamepadPort;
    bool tvEnabled;
    bool gamepadEnabled;
    bool adaptiveFps;
    bool watchdogEnabled;
    bool authEnabled;
    int authCode;
    int logLevel;
    bool highRiskEnabled;
    bool highRiskAccepted;
    bool audioStreaming;
    bool continuousCapture;
    bool safetyGovernor;
    int preset;
    int tvFps;
    int gamepadFps;
    int jpegQuality;
    int tvResolution;
    int gamepadResolution;
};

RequestedSettings ParseRequested(const std::string &query) {
    RequestedSettings v{
        QueryBool(query, "enabled", Settings::enabled.load()),
        QueryInt(query, "webPort", Settings::webPort.load()),
        QueryInt(query, "tvPort", Settings::tvPort.load()),
        QueryInt(query, "gamepadPort", Settings::gamepadPort.load()),
        QueryBool(query, "tvEnabled", Settings::tvEnabled.load()),
        QueryBool(query, "gamepadEnabled", Settings::gamepadEnabled.load()),
        QueryBool(query, "adaptiveFps", Settings::adaptiveFps.load()),
        QueryBool(query, "watchdogEnabled", Settings::watchdogEnabled.load()),
        QueryBool(query, "authEnabled", Settings::authEnabled.load()),
        QueryInt(query, "authCode", Settings::authCode.load()),
        QueryInt(query, "logLevel", Settings::logLevel.load()),
        QueryBool(query, "highRiskEnabled", Settings::highRiskEnabled.load()),
        false,
        QueryBool(query, "audioStreaming", false),
        QueryBool(query, "continuousCapture", false),
        QueryBool(query, "safetyGovernor", Settings::safetyGovernor.load()),
        QueryInt(query, "preset", Settings::preset.load()),
        QueryInt(query, "tvFps", Settings::tvFps.load()),
        QueryInt(query, "gamepadFps", Settings::gamepadFps.load()),
        QueryInt(query, "jpegQuality", Settings::jpegQuality.load()),
        QueryInt(query, "tvResolution", Settings::tvResolution.load()),
        QueryInt(query, "gamepadResolution", Settings::gamepadResolution.load())
    };
    v.highRiskAccepted = v.highRiskEnabled && QueryBool(query, "highRiskAccepted", Settings::highRiskAccepted.load());
    return v;
}

bool Validate(const RequestedSettings &v, std::string &error) {
    auto validPort = [](int p) { return p >= 1024 && p <= 65535; };
    if (!validPort(v.webPort) || !validPort(v.tvPort) || !validPort(v.gamepadPort)) {
        error = "Ports must be between 1024 and 65535.";
        return false;
    }
    if (v.webPort == v.tvPort || v.webPort == v.gamepadPort || v.tvPort == v.gamepadPort) {
        error = "Web, TV and GamePad ports must be unique.";
        return false;
    }
    if (v.preset < 0 || v.preset > 5) {
        error = "Invalid preset.";
        return false;
    }
    if (v.authCode < 0 || v.authCode > 999999 || v.logLevel < 0 || v.logLevel > 3) {
        error = "Invalid security or logging value.";
        return false;
    }

    const bool risk = v.highRiskEnabled && v.highRiskAccepted;
    if (v.preset == static_cast<int>(Settings::Preset::Custom)) {
        const int maxFps = risk ? 60 : 15;
        const int maxQuality = risk ? 95 : 85;
        const int maxTvResolution = risk ? 5 : 3;
        const int maxGamePadResolution = risk ? 5 : 2;
        if (v.tvFps < 1 || v.tvFps > maxFps || v.gamepadFps < 1 || v.gamepadFps > maxFps) {
            error = risk ? "FPS must be between 1 and 60." : "FPS above 15 requires HIGH RISK mode.";
            return false;
        }
        if (v.jpegQuality < 35 || v.jpegQuality > maxQuality) {
            error = risk ? "JPEG quality must be between 35 and 95." : "JPEG quality above 85 requires HIGH RISK mode.";
            return false;
        }
        if (v.tvResolution < 0 || v.tvResolution > maxTvResolution ||
            v.gamepadResolution < 0 || v.gamepadResolution > maxGamePadResolution) {
            error = "Selected resolution requires HIGH RISK mode or is invalid for this source.";
            return false;
        }
    }
    if ((v.audioStreaming || v.continuousCapture) && !risk) {
        error = "Audio streaming and continuous capture require HIGH RISK confirmation.";
        return false;
    }
    return true;
}

std::string AuthKey(const std::string &authSuffix) {
    const size_t pos = authSuffix.find("key=");
    return pos == std::string::npos ? "" : authSuffix.substr(pos + 4);
}

} // namespace

namespace WebSettings {

std::string BuildPage(const std::string &authSuffix) {
    const bool highRisk = Settings::HighRiskAccepted();
    const std::string authKey = AuthKey(authSuffix);
    std::ostringstream out;
    out << R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wii U Web Stream - Settings</title><style>
:root{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:#0b1118;color:#eef5ff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#15314b,#0b1118 45%)}main{width:min(960px,calc(100% - 24px));margin:auto;padding:24px 0 48px}a{color:#9fd0ff}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.card{background:#111c28dd;border:1px solid #29435c;border-radius:16px;padding:16px;margin:14px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.field{display:flex;flex-direction:column;gap:6px}.check{display:flex;gap:9px;align-items:center;margin:8px 0}input,select,button{font:inherit;border:1px solid #3f6585;background:#10283a;color:#fff;border-radius:9px;padding:9px}button{cursor:pointer;background:#1d4a6c}.warn{border-color:#8b5e24;background:#2a2114}.danger{border-color:#984b4b;background:#2b1717}.safe{border-color:#35704a;background:#14251a}.muted{color:#9db2c7;font-size:.92rem}.status{min-height:1.4em;margin-top:10px}.row{display:flex;gap:8px;flex-wrap:wrap}.stop{background:#741f1f;border-color:#c25b5b}.recover{background:#245f38;border-color:#5d9c70}.overlay{position:fixed;inset:0;background:#05080def;display:grid;place-items:center;padding:18px;z-index:20}.modal{max-width:650px;background:#171d25;border:1px solid #a45b45;border-radius:16px;padding:22px;box-shadow:0 20px 80px #000}.badge{display:inline-block;padding:3px 8px;border-radius:999px;background:#25384a;color:#cbe6ff;font-size:.82rem}</style></head><body><main>
)HTML";
    if (!Settings::safetyWarningAccepted.load()) {
        out << R"HTML(<div class="overlay" id="warningOverlay"><div class="modal"><h2>Development build safety warning</h2><p>This plugin can freeze the Wii U during gameplay and may require a forced power-off. A forced shutdown can lose unsaved progress and may increase the risk of data or filesystem corruption.</p><p>If freezes occur, lower resolution/FPS/JPEG quality. If instability continues, disable or uninstall Wii U Web Stream.</p><button id="acceptWarning">I understand — continue</button></div></div>)HTML";
    }
    out << "<div class=\"top\"><div><h1>Wii U Web Stream Settings</h1><div class=\"muted\">v0.2.0-dev · LAN only</div></div><a href=\"/" << authSuffix << "\">Back to dashboard</a></div>";

    out << R"HTML(<section class="card safe"><h2>Safety & Recovery</h2><div id="safetyState" class="muted">Loading safety status…</div><label class="check"><input type="checkbox" form="settings" name="safetyGovernor")HTML"
        << Checked(Settings::safetyGovernor.load()) << R"HTML(> Enable automatic Safety Governor</label><p class="muted">When sustained overload is detected, the governor temporarily caps FPS, JPEG quality and resolution without overwriting your configured preset.</p><div class="row"><button type="button" class="recover" onclick="control('recovery')">Apply Recovery preset</button><button type="button" class="stop" onclick="control('emergencyStop')">STOP ALL STREAMING</button><button type="button" onclick="control('resume')">Resume Streaming</button><a href="/diagnostics.txt)HTML"
        << authSuffix << R"HTML(">Download diagnostics</a></div></section>)HTML";

    out << R"HTML(<form id="settings"><section class="card"><h2>General</h2>)HTML"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"enabled\"" << Checked(Settings::enabled.load()) << "> Enable server</label>"
        << "<div class=\"field\"><label>Preset</label><select name=\"preset\">"
        << "<option value=\"0\"" << Selected(Settings::preset.load(),0) << ">Custom</option>"
        << "<option value=\"1\"" << Selected(Settings::preset.load(),1) << ">Low Latency</option>"
        << "<option value=\"2\"" << Selected(Settings::preset.load(),2) << ">Balanced</option>"
        << "<option value=\"3\"" << Selected(Settings::preset.load(),3) << ">Quality</option>"
        << "<option value=\"4\"" << Selected(Settings::preset.load(),4) << ">OBS</option>"
        << "<option value=\"5\"" << Selected(Settings::preset.load(),5) << ">Recovery / Safe</option></select></div></section>";

    out << "<section class=\"card\"><h2>Network</h2><div class=\"grid\">"
        << "<label class=\"field\">Web port<input type=\"number\" min=\"1024\" max=\"65535\" name=\"webPort\" value=\"" << Settings::webPort.load() << "\"></label>"
        << "<label class=\"field\">TV port<input type=\"number\" min=\"1024\" max=\"65535\" name=\"tvPort\" value=\"" << Settings::tvPort.load() << "\"></label>"
        << "<label class=\"field\">GamePad port<input type=\"number\" min=\"1024\" max=\"65535\" name=\"gamepadPort\" value=\"" << Settings::gamepadPort.load() << "\"></label></div>"
        << "<p class=\"muted\">Listener changes apply asynchronously. Reconnect on the new web port after a few seconds.</p></section>";

    out << "<section class=\"card\"><h2>Video</h2>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"tvEnabled\"" << Checked(Settings::tvEnabled.load()) << "> Enable TV capture</label>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"gamepadEnabled\"" << Checked(Settings::gamepadEnabled.load()) << "> Enable GamePad capture</label>"
        << "<div class=\"grid\">"
        << "<label class=\"field\">TV target FPS<input type=\"number\" name=\"tvFps\" min=\"1\" max=\"60\" value=\"" << Settings::tvFps.load() << "\"></label>"
        << "<label class=\"field\">GamePad target FPS<input type=\"number\" name=\"gamepadFps\" min=\"1\" max=\"60\" value=\"" << Settings::gamepadFps.load() << "\"></label>"
        << "<label class=\"field\">JPEG quality<input type=\"number\" name=\"jpegQuality\" min=\"35\" max=\"95\" value=\"" << Settings::jpegQuality.load() << "\"></label>";

    auto resolutionSelect = [&out](const char *name, int current, const char *label) {
        out << "<label class=\"field\">" << label << "<select name=\"" << name << "\">"
            << "<option value=\"0\"" << Selected(current,0) << ">426x240</option>"
            << "<option value=\"1\"" << Selected(current,1) << ">640x360</option>"
            << "<option value=\"2\"" << Selected(current,2) << ">854x480</option>"
            << "<option value=\"3\"" << Selected(current,3) << ">960x540</option>"
            << "<option value=\"4\"" << Selected(current,4) << ">1280x720 (HIGH RISK)</option>"
            << "<option value=\"5\"" << Selected(current,5) << ">1920x1080 (HIGH RISK)</option></select></label>";
    };
    resolutionSelect("tvResolution", Settings::tvResolution.load(), "TV resolution");
    resolutionSelect("gamepadResolution", Settings::gamepadResolution.load(), "GamePad resolution");
    out << "</div><label class=\"check\"><input type=\"checkbox\" name=\"adaptiveFps\"" << Checked(Settings::adaptiveFps.load()) << "> Adaptive FPS</label>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"watchdogEnabled\"" << Checked(Settings::watchdogEnabled.load()) << "> Health watchdog</label></section>";

    out << "<section class=\"card\"><h2>Security & Logging</h2>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"authEnabled\"" << Checked(Settings::authEnabled.load()) << "> Require URL access code</label>"
        << "<div class=\"grid\"><label class=\"field\">Access code<input type=\"number\" name=\"authCode\" min=\"0\" max=\"999999\" value=\"" << Settings::authCode.load() << "\"></label>"
        << "<label class=\"field\">Log level<select name=\"logLevel\">"
        << "<option value=\"0\"" << Selected(Settings::logLevel.load(),0) << ">Off</option>"
        << "<option value=\"1\"" << Selected(Settings::logLevel.load(),1) << ">Errors</option>"
        << "<option value=\"2\"" << Selected(Settings::logLevel.load(),2) << ">Info</option>"
        << "<option value=\"3\"" << Selected(Settings::logLevel.load(),3) << ">Verbose</option></select></label></div>"
        << "<p class=\"muted\">The access code is LAN access control only; HTTP is not encrypted.</p></section>";

    out << "<section class=\"card warn\"><h2>Advanced / Experimental</h2>"
        << "<label class=\"check\"><input id=\"riskEnable\" type=\"checkbox\" name=\"highRiskEnabled\"" << Checked(Settings::highRiskEnabled.load()) << "> Enable high-risk actions</label>"
        << "<label class=\"check\"><input id=\"riskAccept\" type=\"checkbox\" name=\"highRiskAccepted\"" << Checked(Settings::highRiskAccepted.load()) << "> I understand and accept the risk</label>"
        << "<p><strong>WARNING:</strong> High-risk actions can significantly increase CPU, GPU, memory and network load and may freeze or crash the console.</p>"
        << "<div id=\"riskOptions\" class=\"danger\" style=\"padding:12px;border:1px solid;border-radius:10px;display:" << (highRisk ? "block" : "none") << "\">"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"audioStreaming\"" << Checked(Settings::audioStreaming.load()) << "> Audio streaming (HIGH RISK)</label>"
        << "<label class=\"check\"><input type=\"checkbox\" name=\"continuousCapture\"" << Checked(Settings::continuousCapture.load()) << "> Continuous capture without viewers (HIGH RISK)</label>"
        << "<p class=\"muted\">720p/1080p, up to 60 FPS and JPEG quality above 85 require HIGH RISK mode.</p></div></section>";

    out << "<div class=\"row\"><button type=\"submit\">Save settings</button><a href=\"/" << authSuffix << "\">Cancel</a></div><div id=\"status\" class=\"status muted\"></div></form>";

    out << R"HTML(<script>
const f=document.getElementById('settings'),st=document.getElementById('status'),re=document.getElementById('riskEnable'),ra=document.getElementById('riskAccept'),ro=document.getElementById('riskOptions');
const authKey=')HTML" << authKey << R"HTML(';
function addKey(q){if(authKey)q.set('key',authKey)}
function syncRisk(){ro.style.display=(re.checked&&ra.checked)?'block':'none'} re.onchange=syncRisk;ra.onchange=syncRisk;
async function control(action){const q=new URLSearchParams({action});addKey(q);st.textContent='Applying…';try{const r=await fetch('/api/control?'+q,{method:'POST',cache:'no-store'});const j=await r.json();st.textContent=j.message||'Done';setTimeout(updateSafety,300)}catch(e){st.textContent='Control request failed'}}
async function updateSafety(){try{const u=new URL('/api/status',location.origin);if(authKey)u.searchParams.set('key',authKey);const s=await (await fetch(u,{cache:'no-store'})).json();const el=document.getElementById('safetyState');const label=s.emergencyStopped?'EMERGENCY STOP':(s.safetyActive?'GOVERNOR LEVEL '+s.safetyLevel:'NORMAL');el.innerHTML='<span class="badge">'+label+'</span> '+(s.safetyReason||'No overload detected.');}catch(e){}}
f.addEventListener('submit',async e=>{e.preventDefault();const q=new URLSearchParams(new FormData(f));for(const n of ['enabled','tvEnabled','gamepadEnabled','adaptiveFps','watchdogEnabled','authEnabled','highRiskEnabled','highRiskAccepted','audioStreaming','continuousCapture','safetyGovernor']){const el=f.elements[n];q.set(n,el&&el.checked?'1':'0')}addKey(q);st.textContent='Validating and saving…';try{const r=await fetch('/api/settings?'+q,{method:'POST',cache:'no-store'});const j=await r.json();st.textContent=j.message||'Saved';if(j.ok)setTimeout(()=>location.reload(),700)}catch(e){st.textContent='Save failed'}});
const aw=document.getElementById('acceptWarning');if(aw)aw.onclick=async()=>{await control('acceptWarning');document.getElementById('warningOverlay').remove()};
syncRisk();updateSafety();setInterval(updateSafety,2000);
</script></main></body></html>)HTML";
    return out.str();
}

std::string ApplyQuery(const std::string &query) {
    const RequestedSettings v = ParseRequested(query);
    std::string error;
    if (!Validate(v, error)) {
        return "{\"ok\":false,\"message\":\"" + error + " No settings were changed.\"}";
    }

    // All validation is complete before the first persistent write.
    Settings::SetHighRiskEnabled(v.highRiskEnabled);
    Settings::SetHighRiskAccepted(v.highRiskAccepted);
    Settings::SetEnabled(v.enabled);
    Settings::SetWebPort(v.webPort);
    Settings::SetTvPort(v.tvPort);
    Settings::SetGamePadPort(v.gamepadPort);
    Settings::SetTvEnabled(v.tvEnabled);
    Settings::SetGamePadEnabled(v.gamepadEnabled);
    Settings::SetAdaptiveFps(v.adaptiveFps);
    Settings::SetWatchdogEnabled(v.watchdogEnabled);
    Settings::SetAuthEnabled(v.authEnabled);
    Settings::SetAuthCode(v.authCode);
    Settings::SetLogLevel(v.logLevel);
    Settings::SetSafetyGovernor(v.safetyGovernor);

    Settings::SetPreset(v.preset);
    if (v.preset == static_cast<int>(Settings::Preset::Custom)) {
        Settings::SetTvFps(v.tvFps);
        Settings::SetGamePadFps(v.gamepadFps);
        Settings::SetJpegQuality(v.jpegQuality);
        Settings::SetTvResolution(v.tvResolution);
        Settings::SetGamePadResolution(v.gamepadResolution);
    }

    Settings::SetAudioStreaming(v.audioStreaming);
    Settings::SetContinuousCapture(v.continuousCapture);
    Settings::EnforceSafeLimits();
    Settings::Save();
    Audio::ApplySettings();

    return "{\"ok\":true,\"message\":\"Settings validated and saved. Listener changes apply within a few seconds.\"}";
}

} // namespace WebSettings
