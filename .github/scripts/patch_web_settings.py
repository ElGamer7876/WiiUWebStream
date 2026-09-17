from pathlib import Path
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
p = root / 'src/network.cpp'
s = p.read_text()

def repl(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'marker not found: {label}')
    s = s.replace(old, new, 1)

repl('#include "types.hpp"\n', '#include "types.hpp"\n#include "web_settings.hpp"\n', 'web settings include')

repl(
    '        if (request.method != "GET") { SendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET is supported.\\n", "Allow: GET\\r\\n"); return; }',
    '        const bool settingsPost = mKind == ListenerKind::Web && request.method == "POST" && request.path == "/api/settings";\n'
    '        if (request.method != "GET" && !settingsPost) { SendResponse(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET is supported, except POST /api/settings.\\n", "Allow: GET, POST\\r\\n"); return; }',
    'POST allowance')

repl(
    '<div class="links"><strong>OBS</strong>:',
    '<div class="links"><a class="btn" href="/settings)HTML" << auth << R"HTML(">Settings</a> <strong>OBS</strong>:',
    'dashboard settings button')

repl(
    '        if (r.path == "/") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildMainHtml(r)); return; }\n'
    '        if (r.path == "/health") { SendResponse(fd, 200, "OK", "application/json", "{\\"ok\\":true}"); return; }',
    '        if (r.path == "/") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", BuildMainHtml(r)); return; }\n'
    '        if (r.path == "/settings" && r.method == "GET") { SendResponse(fd, 200, "OK", "text/html; charset=utf-8", WebSettings::BuildPage(AuthSuffix(r))); return; }\n'
    '        if (r.path == "/api/settings" && r.method == "POST") { SendResponse(fd, 200, "OK", "application/json; charset=utf-8", WebSettings::ApplyQuery(r.query)); return; }\n'
    '        if (r.path == "/health") { SendResponse(fd, 200, "OK", "application/json", "{\\"ok\\":true}"); return; }',
    'settings routes')

p.write_text(s)
print('network.cpp patched for web settings')
