"""Serve deterministic HTML/download fixtures for the native Windows browser test."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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
            body = (f'<!doctype html><meta charset="utf-8"><title>{title}</title>'
                    '<body style="background:#2ac78f"><input autofocus '
                    'oninput="document.title=\'Typed:\'+this.value">'
                    '<a style="position:absolute;left:20px;top:70px" href="/popup" target="_blank">Open popup</a>'
                    '<script>window.open("/unwanted");</script>').encode()
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
        return subprocess.run(command, timeout=240).returncode
    finally:
        server.shutdown()


if __name__ == '__main__':
    raise SystemExit(main())
