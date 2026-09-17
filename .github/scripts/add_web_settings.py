from pathlib import Path
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f'patch marker not found: {label}')
    return text.replace(old, new, 1)

p = root / 'src/network.cpp'
s = p.read_text()

# Allow POST only for the settings API on the web listener.
s = replace_once(
    s,
    '        if (request.method != "GET") { SendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET is supported.\\n", "Allow: GET\\r\\n"); return; }\n        if (request.path != "/health" && !IsAuthorized(request)) { Unauthorized(fd); return; }\n        if (mKind == ListenerKind::Web) HandleWeb(fd, request); else HandleStream(fd, request, Source());',
    '        const bool settingsPost = request.method == "POST" && mKind == ListenerKind::Web && request.path == "/api/settings";\n        if (request.method != "GET" && !settingsPost) { SendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET is supported except POST /api/settings.\\n", "Allow: GET, POST\\r\\n"); return; }\n        if (request.path != "/health" && !IsAuthorized(request)) { Unauthorized(fd); return; }\n        if (mKind == ListenerKind::Web) HandleWeb(fd, request); else HandleStream(fd, request, Source());',
    'allow settings POST',
)

marker = '    std::string BuildMainHtml(const HttpRequest &request) const {'
insert = r'''    int QueryInt(const HttpRequest &request, const char *key, int fallback) const {
        const std::string value = QueryValue(request.query, key);
        if (value.empty()) return fallback;
        char *end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        return end != value.c_str() && *end == '\0' ? static_cast<int>(parsed) : fallback;
    }

    bool QueryBool(const HttpRequest &request, const char *key, bool fallback) const {
        const std::string value = QueryValue(request.query, key);
        if (value.empty()) return fallback;
        return value == "1" || value == "true" || value == "on";
    }

    std::string BuildSettingsJson() const {
        std::ostringstream out;
        out << "{\"ok\":true"
            << ",\"enabled\":" << (Settings::enabled.load() ? "true" : "false")
            << ",\"preset\":" << Settings::preset.load()
            << ",\"webPort\":" << Settings::webPort.load()
            << ",\"tvPort\":" << Settings::tvPort.load()
            << ",\"gamepadPort\":" << Settings::gamepadPort.load()
            << ",\"tvEnabled\":" << (Settings::tvEnabled.load() ? "true" : "false")
            << ",\"gamepadEnabled\":" << (Settings::gamepadEnabled.load() ? "true" : "false")
            << ",\"tvFps\":" << Settings::tvFps.load()
            << ",\"gamepadFps\":" << Settings::gamepadFps.load()
            << ",\"jpegQuality\":" << Settings::jpegQuality.load()
            << ",\"tvResolution\":" << Settings::tvResolution.load()
            << ",\"gamepadResolution\":" << Settings::gamepadResolution.load()
            << ",\"adaptiveFps\":" << (Settings::adaptiveFps.load() ? "true" : "false")
            << ",\"watchdogEnabled\":" << (Settings::watchdogEnabled.load() ? "true" : "false")
            << ",\"authEnabled\":" << (Settings::authEnabled.load() ? "true" : "false")
            << ",\"authCode\":" << Settings::authCode.load()
            << ",\"logLevel\":" << Settings::logLevel.load()
            << ",\"highRiskEnabled\":" << (Settings::highRiskEnabled.load() ? "true" : "false")
            << ",\"highRiskAccepted\":" << (Settings::highRiskAccepted.load() ? "true" : "false")
            << ",\"audioStreaming\":" << (Settings::audioStreaming.load() ? "true" : "false")
            << ",\"continuousCapture\":" << (Settings::continuousCapture.load() ? "true" : "false")
            << "}";
        return out.str();
    }

    std::string ApplySettings(const HttpRequest &request) {
        Settings::SetHighRiskEnabled(QueryBool(request, "highRiskEnabled", false));
        Settings::SetHighRiskAccepted(QueryBool(request, "highRiskAccepted", false));

        Settings::SetEnabled(QueryBool(request, "enabled", Settings::enabled.load()));
        Settings::SetWebPort(QueryInt(request, "webPort", Settings::webPort.load()));
        Settings::SetTvPort(QueryInt(request, "tvPort", Settings::tvPort.load()));
        Settings::SetGamePadPort(QueryInt(request, "gamepadPort", Settings::gamepadPort.load()));
        Settings::SetTvEnabled(QueryBool(request, "tvEnabled", Settings::tvEnabled.load()));
        Settings::SetGamePadEnabled(QueryBool(request, "gamepadEnabled", Settings::gamepadEnabled.load()));
        Settings::SetAdaptiveFps(QueryBool(request, "adaptiveFps", Settings::adaptiveFps.load()));
        Settings::SetWatchdogEnabled(QueryBool(request, "watchdogEnabled", Settings::watchdogEnabled.load()));
        Settings::SetAuthEnabled(QueryBool(request, "authEnabled", Settings::authEnabled.load()));
        Settings::SetAuthCode(QueryInt(request, "authCode", Settings::authCode.load()));
        Settings::SetLogLevel(QueryInt(request, "logLevel", Settings::logLevel.load()));

        const int requestedPreset = std::clamp(QueryInt(request, "preset", Settings::preset.load()), 0, 4);
        if (requestedPreset != static_cast<int>(Settings::Preset::Custom)) {
            Settings::SetPreset(requestedPreset);
        } else {
            Settings::SetPreset(static_cast<int>(Settings::Preset::Custom));
            Settings::SetTvFps(QueryInt(request, "tvFps", Settings::tvFps.load()));
            Settings::SetGamePadFps(QueryInt(request, "gamepadFps", Settings::gamepadFps.load()));
            Settings::SetJpegQuality(QueryInt(request, "jpegQuality", Settings::jpegQuality.load()));
            Settings::SetTvResolution(QueryInt(request, "tvResolution", Settings::tvResolution.load()));
            Settings::SetGamePadResolution(QueryInt(request, "gamepadResolution", Settings::gamepadResolution.load()));
        }

        Settings::SetAudioStreaming(QueryBool(request, "audioStreaming", false));
        Settings::SetContinuousCapture(QueryBool(request, "continuousCapture", false));
        Settings::EnforceSafeLimits();
        Settings::Save();
        Audio::ApplySettings();

        std::ostringstream out;
        out << "{\"ok\":true,\"message\":\"Settings saved\",\"listenerReconfigureWithinSeconds\":5,\"settings\":" << BuildSettingsJson() << "}";
        return out.str();
    }

    std::string BuildSettingsHtml(const HttpRequest &request) const {
        const std::string auth = AuthSuffix(request);
        std::ostringstream out;
        out << R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark"><title>Wii U Web Stream Settings</title><style>
:root{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;background:#0b1118;color:#eef5ff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#15314b,#0b1118 45%)}main{width:min(1040px,calc(100% - 24px));margin:auto;padding:22px 0 50px}.nav{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:18px}.nav a,button{border:1px solid #3f6585;background:#18334b;color:#fff;border-radius:10px;padding:9px 12px;text-decoration:none;cursor:pointer}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:14px}.card{background:#111c28dd;border:1px solid #29435c;border-radius:16px;padding:16px}.card h2{margin:0 0 12px}.field{display:grid;grid-template-columns:1fr minmax(110px,180px);align-items:center;gap:12px;margin:10px 0}.field input[type=number],.field select{width:100%;background:#0c1722;color:#fff;border:1px solid #36536e;border-radius:8px;padding:8px}.field input[type=checkbox]{width:22px;height:22px;justify-self:end}.help{color:#9db2c7;font-size:.88rem}.warn{background:#3c2a12;border:1px solid #a66b20;border-radius:12px;padding:12px;color:#ffd28a}.danger{background:#3a1717;border:1px solid #9e3d3d;border-radius:12px;padding:12px;color:#ffb2b2}.actions{position:sticky;bottom:8px;margin-top:16px;background:#0b1118e8;border:1px solid #29435c;border-radius:14px;padding:12px;display:flex;gap:10px;align-items:center}.save{background:#176b3a;border-color:#2f9e5b}.status{color:#9db2c7}.hidden{display:none}code{background:#172536;padding:2px 5px;border-radius:5px}</style></head><body><main>
<div class="nav"><a href="/)HTML" << auth << R"HTML(">Dashboard</a><a href="/settings)HTML" << auth << R"HTML(">Settings</a><a href="/debug/performance)HTML" << auth << R"HTML(">Diagnostics</a></div>
<h1>Settings</h1><p class="help">Configuration is hosted here to reduce memory pressure in Aroma's WUPS configuration renderer. Changes are stored on the Wii U.</p>
<div class="grid">
<section class="card"><h2>General</h2><label class="field">Enable server<input id="enabled" type="checkbox"></label><label class="field">Preset<select id="preset"><option value="0">Custom</option><option value="1">Low Latency</option><option value="2">Balanced</option><option value="3">Quality</option><option value="4">OBS</option></select></label><p class="help">Manual video controls are used only with Custom.</p></section>
<section class="card"><h2>Network</h2><label class="field">Web port<input id="webPort" type="number" min="1024" max="65535"></label><label class="field">TV port<input id="tvPort" type="number" min="1024" max="65535"></label><label class="field">GamePad port<input id="gamepadPort" type="number" min="1024" max="65535"></label><p class="help">Port changes may take up to 5 seconds. Changing the Web port disconnects this page.</p></section>
<section class="card"><h2>TV</h2><label class="field">Enable TV capture<input id="tvEnabled" type="checkbox"></label><label class="field manual">Target FPS<input id="tvFps" type="number" min="1" max="60"></label><label class="field manual">Output resolution<select id="tvResolution"><option value="0">426x240</option><option value="1">640x360</option><option value="2">854x480</option><option value="3">960x540</option><option class="riskOpt" value="4">1280x720 (HIGH RISK)</option><option class="riskOpt" value="5">1920x1080 (HIGH RISK)</option></select></label></section>
<section class="card"><h2>GamePad</h2><label class="field">Enable GamePad capture<input id="gamepadEnabled" type="checkbox"></label><label class="field manual">Target FPS<input id="gamepadFps" type="number" min="1" max="60"></label><label class="field manual">Output resolution<select id="gamepadResolution"><option value="0">426x240</option><option value="1">640x360</option><option value="2">854x480</option><option class="riskOpt" value="3">960x540 (HIGH RISK)</option><option class="riskOpt" value="4">1280x720 (HIGH RISK)</option><option class="riskOpt" value="5">1920x1080 (HIGH RISK)</option></select></label></section>
<section class="card"><h2>Performance</h2><label class="field manual">JPEG quality<input id="jpegQuality" type="number" min="35" max="95"></label><label class="field">Adaptive FPS<input id="adaptiveFps" type="checkbox"></label><label class="field">Health watchdog<input id="watchdogEnabled" type="checkbox"></label></section>
<section class="card"><h2>Security & logging</h2><label class="field">Require URL access code<input id="authEnabled" type="checkbox"></label><label class="field">Access code<input id="authCode" type="number" min="0" max="999999"></label><label class="field">Log level<select id="logLevel"><option value="0">Off</option><option value="1">Errors</option><option value="2">Info</option><option value="3">Verbose</option></select></label><p class="help">The access code is LAN access control, not encryption.</p></section>
<section class="card"><h2>Advanced / Experimental</h2><div class="warn"><strong>HIGH RISK</strong><br>These actions may significantly increase CPU, GPU, memory, network or SD-card load and may freeze or crash the console.</div><label class="field">Enable high-risk actions<input id="highRiskEnabled" type="checkbox"></label><label class="field">I understand and accept the risk<input id="highRiskAccepted" type="checkbox"></label><div id="riskPanel" class="hidden"><div class="danger"><strong>High-risk options are active.</strong></div><label class="field">Audio streaming (HIGH RISK)<input id="audioStreaming" type="checkbox"></label><label class="field">Continuous capture without viewers (HIGH RISK)<input id="continuousCapture" type="checkbox"></label><p class="help">30/60 FPS, JPEG up to 95 and 720p/1080p become available through Custom while HIGH RISK is accepted.</p><p class="help">H.264 streaming (HIGH RISK) — Not available in this build.<br>mDNS discovery (HIGH RISK) — Not available in this build.</p></div></section>
</div><div class="actions"><button class="save" id="save">Save settings</button><span class="status" id="status">Loading…</span></div>
<script>
const auth=)HTML" << (auth.empty() ? "''" : "'" + auth + "'") << R"HTML(;
const ids=['enabled','preset','webPort','tvPort','gamepadPort','tvEnabled','gamepadEnabled','tvFps','gamepadFps','jpegQuality','tvResolution','gamepadResolution','adaptiveFps','watchdogEnabled','authEnabled','authCode','logLevel','highRiskEnabled','highRiskAccepted','audioStreaming','continuousCapture'];
const el=id=>document.getElementById(id);const isCheck=id=>el(id).type==='checkbox';
function riskAccepted(){return el('highRiskEnabled').checked&&el('highRiskAccepted').checked}
function refreshUi(){const risk=riskAccepted(),custom=el('preset').value==='0';el('riskPanel').classList.toggle('hidden',!risk);document.querySelectorAll('.manual input,.manual select').forEach(x=>x.disabled=!custom);document.querySelectorAll('.riskOpt').forEach(x=>x.hidden=!risk);el('tvFps').max=risk?60:15;el('gamepadFps').max=risk?60:15;el('jpegQuality').max=risk?95:85}
async function load(){try{const r=await fetch('/api/settings'+auth,{cache:'no-store'}),s=await r.json();ids.forEach(id=>{if(s[id]===undefined)return;isCheck(id)?el(id).checked=!!s[id]:el(id).value=s[id]});refreshUi();el('status').textContent='Ready'}catch(e){el('status').textContent='Failed to load settings'}}
el('preset').addEventListener('change',refreshUi);el('highRiskEnabled').addEventListener('change',e=>{if(e.target.checked&&!confirm('WARNING: High-risk actions may freeze or crash the Wii U. Continue?'))e.target.checked=false;if(!e.target.checked)el('highRiskAccepted').checked=false;refreshUi()});el('highRiskAccepted').addEventListener('change',e=>{if(e.target.checked&&!el('highRiskEnabled').checked)e.target.checked=false;if(e.target.checked&&!confirm('Confirm that you understand and accept the HIGH RISK options.'))e.target.checked=false;refreshUi()});
el('save').addEventListener('click',async()=>{const q=new URLSearchParams();ids.forEach(id=>q.set(id,isCheck(id)?(el(id).checked?'1':'0'):el(id).value));if(auth){const k=new URLSearchParams(auth.slice(1)).get('key');if(k)q.set('key',k)}el('status').textContent='Saving…';try{const r=await fetch('/api/settings?'+q.toString(),{method:'POST',cache:'no-store'}),s=await r.json();if(!r.ok||!s.ok)throw new Error();el('status').textContent='Saved. Listener changes apply within 5 seconds.';setTimeout(load,700)}catch(e){el('status').textContent='Save failed'}});load();
</script></main></body></html>)HTML";
        return out.str();
    }

'''
s = replace_once(s, marker, insert + marker, 'settings page and API')

# Add Settings link to dashboard.
s = replace_once(
    s,
    '<div class="links"><strong>OBS</strong>:',
    '<div class="links"><a class="btn" href="/settings)HTML" << auth << R"HTML(">Settings</a> <strong>OBS</strong>:',
    'dashboard settings link',
)

# Add routes.
s = replace_once(
    s,
    '        if (r.path == "/health") { SendResponse(fd, 200, "OK", "application/json", "{\\\"ok\\\":true}"); return; }\n        if (r.path == "/api/status" || r.path == "/debug/performance") { SendResponse(fd, 200, "OK", "application/json; charset=utf-8", BuildStatusJson()); return; }',
    '        if (r.path == "/health") { SendResponse(fd, 200, "OK", "application/json", "{\\\"ok\\\":true}"); return; }\n        if (r.path == "/settings") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildSettingsHtml(r)); return; }\n        if (r.path == "/api/settings" && r.method == "GET") { SendResponse(fd, 200, "OK", "application/json; charset=utf-8", BuildSettingsJson()); return; }\n        if (r.path == "/api/settings" && r.method == "POST") { SendResponse(fd, 200, "OK", "application/json; charset=utf-8", ApplySettings(r)); return; }\n        if (r.path == "/api/status" || r.path == "/debug/performance") { SendResponse(fd, 200, "OK", "application/json; charset=utf-8", BuildStatusJson()); return; }',
    'settings routes',
)
p.write_text(s)

# Network listener maintenance runs independently of the capture watchdog switch.
p = root / 'src/watchdog.cpp'
s = p.read_text()
s = replace_once(
    s,
    '''        if (Settings::watchdogEnabled.load()) {
            if (Settings::tvEnabled.load()) Check(VideoSource::TV);
            if (Settings::gamepadEnabled.load()) Check(VideoSource::GamePad);
            if (++networkTick >= 5) {
                networkTick = 0;
                Network::EnsureListeners();
                Audio::EnsureCallbacks();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));''',
    '''        if (Settings::watchdogEnabled.load()) {
            if (Settings::tvEnabled.load()) Check(VideoSource::TV);
            if (Settings::gamepadEnabled.load()) Check(VideoSource::GamePad);
        }
        if (++networkTick >= 5) {
            networkTick = 0;
            Network::EnsureListeners();
            Audio::EnsureCallbacks();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));''',
    'independent network maintenance',
)
p.write_text(s)
print('web settings patch applied')
