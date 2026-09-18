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
    out << R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wii U Web Stream Settings</title><style>
body{font:15px system-ui;margin:14px;max-width:760px;background:#111;color:#eee}fieldset{margin:10px 0;padding:10px;border:1px solid #555}label{display:block;margin:5px 0}input,select,button{font:inherit;margin:2px;padding:4px}a{color:#9cf}.row{display:flex;gap:6px;flex-wrap:wrap}.warn{border-color:#a66}.hide{display:none}small{color:#aaa}
</style></head><body><h1>Settings</h1><p><a href="/)HTML" << authSuffix << R"HTML(">Dashboard</a> · <a href="/diagnostics.txt)HTML" << authSuffix << R"HTML(">Diagnostics</a></p>)HTML";
    if (!Settings::safetyWarningAccepted.load()) {
        out << R"HTML(<fieldset class="warn" id="warning"><legend>Safety warning</legend><p>This development build may freeze the Wii U during gameplay and can require a forced power-off. Save progress before testing.</p><button id="acceptWarning" type="button">I understand</button></fieldset>)HTML";
    }

    out << "<fieldset><legend>Safety & Recovery</legend><p id="safetyState">Loading…</p>"
        << "<label><input type="checkbox" form="settings" name="safetyGovernor"" << Checked(Settings::safetyGovernor.load()) << "> Safety Governor</label>"
        << "<div class="row"><button type="button" onclick="control('recovery')">Recovery preset</button><button type="button" onclick="control('emergencyStop')">STOP ALL STREAMING</button><button type="button" onclick="control('resume')">Resume</button></div></fieldset>";

    out << "<form id="settings"><fieldset><legend>General</legend>"
        << "<label><input type="checkbox" name="enabled"" << Checked(Settings::enabled.load()) << "> Enable server</label>"
        << "<label>Preset <select name="preset">"
        << "<option value="0"" << Selected(Settings::preset.load(),0) << ">Custom</option>"
        << "<option value="1"" << Selected(Settings::preset.load(),1) << ">Low Latency</option>"
        << "<option value="2"" << Selected(Settings::preset.load(),2) << ">Balanced</option>"
        << "<option value="3"" << Selected(Settings::preset.load(),3) << ">Quality</option>"
        << "<option value="4"" << Selected(Settings::preset.load(),4) << ">OBS</option>"
        << "<option value="5"" << Selected(Settings::preset.load(),5) << ">Recovery</option></select></label></fieldset>";

    out << "<fieldset><legend>Network</legend>"
        << "<label>Web port <input type="number" min="1024" max="65535" name="webPort" value="" << Settings::webPort.load() << ""></label>"
        << "<label>TV port <input type="number" min="1024" max="65535" name="tvPort" value="" << Settings::tvPort.load() << ""></label>"
        << "<label>GamePad port <input type="number" min="1024" max="65535" name="gamepadPort" value="" << Settings::gamepadPort.load() << ""></label></fieldset>";

    out << "<fieldset><legend>Video</legend>"
        << "<label><input type="checkbox" name="tvEnabled"" << Checked(Settings::tvEnabled.load()) << "> TV</label>"
        << "<label><input type="checkbox" name="gamepadEnabled"" << Checked(Settings::gamepadEnabled.load()) << "> GamePad</label>"
        << "<label>TV FPS <input type="number" name="tvFps" min="1" max="60" value="" << Settings::tvFps.load() << ""></label>"
        << "<label>GamePad FPS <input type="number" name="gamepadFps" min="1" max="60" value="" << Settings::gamepadFps.load() << ""></label>"
        << "<label>JPEG quality <input type="number" name="jpegQuality" min="35" max="95" value="" << Settings::jpegQuality.load() << ""></label>";

    auto res = [&out](const char *name, int current, const char *label) {
        out << "<label>" << label << " <select name="" << name << "">"
            << "<option value="0"" << Selected(current,0) << ">426x240</option>"
            << "<option value="1"" << Selected(current,1) << ">640x360</option>"
            << "<option value="2"" << Selected(current,2) << ">854x480</option>"
            << "<option value="3"" << Selected(current,3) << ">960x540</option>"
            << "<option value="4"" << Selected(current,4) << ">1280x720 HIGH RISK</option>"
            << "<option value="5"" << Selected(current,5) << ">1920x1080 HIGH RISK</option></select></label>";
    };
    res("tvResolution", Settings::tvResolution.load(), "TV resolution");
    res("gamepadResolution", Settings::gamepadResolution.load(), "GamePad resolution");

    out << "<label><input type="checkbox" name="adaptiveFps"" << Checked(Settings::adaptiveFps.load()) << "> Adaptive FPS</label>"
        << "<label><input type="checkbox" name="watchdogEnabled"" << Checked(Settings::watchdogEnabled.load()) << "> Watchdog</label></fieldset>";

    out << "<fieldset><legend>Security & Logging</legend>"
        << "<label><input type="checkbox" name="authEnabled"" << Checked(Settings::authEnabled.load()) << "> Require URL code</label>"
        << "<label>Access code <input type="number" name="authCode" min="0" max="999999" value="" << Settings::authCode.load() << ""></label>"
        << "<label>Log <select name="logLevel">"
        << "<option value="0"" << Selected(Settings::logLevel.load(),0) << ">Off</option>"
        << "<option value="1"" << Selected(Settings::logLevel.load(),1) << ">Errors</option>"
        << "<option value="2"" << Selected(Settings::logLevel.load(),2) << ">Info</option>"
        << "<option value="3"" << Selected(Settings::logLevel.load(),3) << ">Verbose</option></select></label></fieldset>";

    out << "<fieldset class="warn"><legend>Advanced / Experimental</legend>"
        << "<label><input id="riskEnable" type="checkbox" name="highRiskEnabled"" << Checked(Settings::highRiskEnabled.load()) << "> Enable HIGH RISK</label>"
        << "<label><input id="riskAccept" type="checkbox" name="highRiskAccepted"" << Checked(Settings::highRiskAccepted.load()) << "> I accept the risk</label>"
        << "<div id="riskOptions" class="" << (highRisk ? "" : "hide") << "">"
        << "<label><input type="checkbox" name="audioStreaming"" << Checked(Settings::audioStreaming.load()) << "> Audio streaming HIGH RISK</label>"
        << "<label><input type="checkbox" name="continuousCapture"" << Checked(Settings::continuousCapture.load()) << "> Continuous capture HIGH RISK</label>"
        << "</div></fieldset>";

    out << "<button type="submit">Save</button> <span id="status"></span></form>";

    out << R"HTML(<script>
const f=document.getElementById('settings'),st=document.getElementById('status'),re=document.getElementById('riskEnable'),ra=document.getElementById('riskAccept'),ro=document.getElementById('riskOptions'),K=')HTML" << authKey << R"HTML(';
function key(q){if(K)q.set('key',K)}
function risk(){ro.className=(re.checked&&ra.checked)?'':'hide'}re.onchange=risk;ra.onchange=risk;
async function control(a){const q=new URLSearchParams({action:a});key(q);try{const j=await(await fetch('/api/control?'+q,{method:'POST',cache:'no-store'})).json();st.textContent=j.message||'';status()}catch(e){st.textContent='Request failed'}}
async function status(){try{const q=new URLSearchParams;key(q);const s=await(await fetch('/api/status'+(q.toString()?'?'+q:''),{cache:'no-store'})).json();safetyState.textContent=s.emergencyStopped?'EMERGENCY STOP':(s.safetyActive?'Governor level '+s.safetyLevel+': '+(s.safetyReason||''):'NORMAL')}catch(e){}}
f.onsubmit=async e=>{e.preventDefault();const q=new URLSearchParams(new FormData(f));for(const n of ['enabled','tvEnabled','gamepadEnabled','adaptiveFps','watchdogEnabled','authEnabled','highRiskEnabled','highRiskAccepted','audioStreaming','continuousCapture','safetyGovernor']){const x=f.elements[n];q.set(n,x&&x.checked?'1':'0')}key(q);st.textContent='Saving...';try{const j=await(await fetch('/api/settings?'+q,{method:'POST',cache:'no-store'})).json();st.textContent=j.message||'';if(j.ok)setTimeout(()=>location.reload(),500)}catch(e){st.textContent='Save failed'}};
const aw=document.getElementById('acceptWarning');if(aw)aw.onclick=async()=>{await control('acceptWarning');warning.remove()};
risk();status();setInterval(status,5000);
</script></body></html>)HTML";
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
