"""Serve deterministic HTML/download fixtures for the native Windows browser test."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
import json
import os
from pathlib import Path
import hashlib
from datetime import datetime, timezone
import subprocess
import threading


class Page(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == '/download':
            body = b'OpenXMB WebView2 download fixture\n'
            self.send_response(200)
            self.send_header('Content-Disposition', 'attachment; filename="fixture.txt"')
            self.send_header('Content-Type', 'application/octet-stream')
        else:
            title = 'First page' if self.path.startswith('/first') else 'Popup page' if self.path.startswith('/popup') else 'Second page'
            options_script = ''
            if self.path.startswith('/options'):
                code = parse_qs(urlparse(self.path).query).get('code', ['Tab'])[0]
                options_script = '<script>setTimeout(()=>dispatchEvent(new KeyboardEvent("keydown",{code:' + json.dumps(code) + ',bubbles:true,cancelable:true})),100);</script>'
            body = (f'<!doctype html><meta charset="utf-8"><title>{title}</title>'
                    '<body style="background:#2ac78f"><input autofocus '
                    'oninput="document.title=\'Typed:\'+this.value">'
                    '<a style="position:absolute;left:20px;top:70px" href="/popup" target="_blank">Open popup</a>'
                    '<script>window.open("/unwanted");</script>' + options_script).encode()
            if self.path.startswith('/osk'):
                kind = parse_qs(urlparse(self.path).query).get('kind', ['input'])[0]
                field = '<input id="field" value="Existing value">'
                if kind == 'textarea': field = '<textarea id="field">Existing value</textarea>'
                if kind == 'contenteditable': field = '<div id="field" contenteditable="true">Existing value</div>'
                if kind == 'password': field = '<input id="field" type="password" value="existing-password-must-stay-in-dom">'
                if kind == 'readonly': field = '<input id="field" readonly value="Existing value">'
                body = (f'<!doctype html><meta charset="utf-8"><title>OSK {kind}</title>'
                        '<body style="background:#2ac78f;font:24px sans-serif;padding:60px">'
                        f'<h1>Native WebView2 editable focus</h1><label>{kind}</label><br>{field}'
                        '<p>Focus / accept / cancel fixture</p><script>'
                        'const f=document.getElementById("field");let inputs=0;f.addEventListener("input",()=>{inputs++;if(parent!==window)parent.postMessage({fixtureEdited:f.value==="Frame replacement"},"*");});'
                        'function title(){const v=f.type==="password"?"Password length:"+f.value.length:"Value:"+(f.isContentEditable?f.textContent:f.value);'
                        'const t=v+" |inputs:"+inputs;if(document.title!==t)document.title=t;}setInterval(title,50);'
                        'setTimeout(()=>f.focus(),200);</script>').encode()
                if kind == 'frame':
                    child = f'http://localhost:{self.server.server_port}/osk?kind=input'
                    body = ('<!doctype html><title>Cross-origin OSK fixture</title><body style="background:#2ac78f">'
                            f'<iframe src="{child}" style="width:800px;height:500px"></iframe>'
                            '<script>let leaks=0;function inspect(d){if(d&&typeof d==="object"&&(d.openxmbEditReply||d.openxmbEditRequest))leaks++;'
                            'if(d&&d.fixtureEdited)document.title="Frame edited |leaks:"+leaks;}'
                            'addEventListener("message",e=>inspect(e.data));chrome.webview.addEventListener("message",e=>inspect(e.data));</script>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test', required=True)
    parser.add_argument('--interactive-input', action='store_true', help='Also move/click the physical pointer; requires exclusive desktop use')
    args = parser.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        command = [args.test, f'http://127.0.0.1:{server.server_port}']
        if args.interactive_input:
            command.append('--interactive-input')
        result = subprocess.run(command, timeout=240).returncode
        if output := os.environ.get('OPENXMB_BROWSER_SMOKE_OUTPUT'):
            Path(output).mkdir(parents=True, exist_ok=True)
            report = Path(output) / 'metadata.json'
            metadata = json.loads(report.read_text()) if report.exists() else {}
            helper = Path(args.test).resolve().parent / 'openxmb-web.exe'
            metadata.update({'status': 'passed' if result == 0 else 'failed',
                             'captured_at_utc': datetime.now(timezone.utc).isoformat(),
                             'helper_path': str(helper), 'helper_sha256': hashlib.sha256(helper.read_bytes()).hexdigest(),
                             'fixture': 'native parent window; shell OSK/chrome visual acceptance is separate'})
            report.write_text(json.dumps(metadata, indent=2))
        return result
    finally:
        server.shutdown()


if __name__ == '__main__':
    raise SystemExit(main())
