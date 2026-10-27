#!/usr/bin/env python3
"""Exercise the optional native web surface with real, local HTML and X11."""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not os.environ.get('DISPLAY'):
        os.execvp('xvfb-run', ['xvfb-run', '-a', '-s', '-screen 0 1920x1080x24', sys.executable, __file__, *sys.argv[1:]])
    source = Path(__file__).resolve().parents[2] / 'scripts/desktop-smoke.py'
    spec = importlib.util.spec_from_file_location('native_browser_smoke', source)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    native = module.NativeInput()
    x, display = native.x11, native.display
    pointer, window = ctypes.c_void_p, ctypes.c_ulong
    x.XCreateSimpleWindow.argtypes = [pointer, window, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, window, window]
    x.XCreateSimpleWindow.restype = window
    x.XMapWindow.argtypes = [pointer, window]
    x.XDestroyWindow.argtypes = [pointer, window]
    x.XResizeWindow.argtypes = [pointer, window, ctypes.c_uint, ctypes.c_uint]
    x.XGetGeometry.argtypes = [pointer, window, ctypes.POINTER(window), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), *([ctypes.POINTER(ctypes.c_uint)] * 4)]
    parent = x.XCreateSimpleWindow(display, native.root, 0, 0, 1920, 1080, 0, 0, 0)
    x.XMapWindow(display, parent)
    x.XSetInputFocus(display, parent, 2, 0)
    x.XSync(display, 0)

    class Page(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            title = 'First page' if self.path.startswith('/first') else 'Second page'
            body = (f'<!doctype html><meta charset="utf-8"><title>{title}</title>'
                    '<body style="background:#2ac78f"><input autofocus oninput="document.title=\'Typed:\'+this.value">'
                    '<a href="/second">Next page</a>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f'http://127.0.0.1:{server.server_port}'
    with tempfile.TemporaryDirectory(prefix='openxmb-web-') as temporary:
        output = args.output.resolve() if args.output else Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        state = output / 'page-state.json'
        log = (output / 'application.log').open('w')
        process = subprocess.Popen([str(args.helper.resolve()), origin + '/first', str(parent), str(output / 'profile'), str(state)], stdout=log, stderr=subprocess.STDOUT)
        sequence = 0
        checks = []

        def wait(predicate, label):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'Web surface exited {process.returncode}: {output / "application.log"}')
                try:
                    value = json.loads(state.read_text())
                    if predicate(value):
                        checks.append(label)
                        return value
                except (FileNotFoundError, json.JSONDecodeError): pass
                time.sleep(0.05)
            raise RuntimeError('Timed out: ' + label)

        def command(name, value=''):
            nonlocal sequence
            sequence += 1
            pending = output / 'page-command.tmp'
            pending.write_text(json.dumps({'sequence': sequence, 'command': name, 'value': value}, ensure_ascii=False))
            pending.replace(output / 'page-command.json')

        def geometry(target):
            root, left, top = window(), ctypes.c_int(), ctypes.c_int()
            width, height, border, depth = [ctypes.c_uint() for _ in range(4)]
            x.XGetGeometry(display, target, ctypes.byref(root), ctypes.byref(left), ctypes.byref(top), ctypes.byref(width), ctypes.byref(height), ctypes.byref(border), ctypes.byref(depth))
            return left.value, top.value, width.value, height.value

        try:
            wait(lambda p: p['title'] == 'First page' and not p['loading'], 'real HTML loaded')
            child = native.locate(process.pid)
            if not child or geometry(child) != (414, 76, 1094, 928):
                raise RuntimeError('Native web surface does not fit the XMB viewport')
            checks.append('native child viewport')
            # A loaded title is insufficient: require the live page's painted pixels.
            time.sleep(1)
            capture=output/'native-page.png'
            subprocess.run(['import','-silent','-window','root','PNG24:'+str(capture)],check=True)
            from PIL import Image
            image=Image.open(capture).convert('RGB')
            region=image.crop((500,200,1400,900))
            green=sum(1 for red,g,b in module.pixels(region) if abs(red-42)<8 and abs(g-199)<8 and abs(b-143)<8)
            if green/(region.width*region.height)<0.90:raise RuntimeError('Loaded HTML did not paint its expected background')
            checks.append('actual painted page pixels')
            command('text', 'Unicode ✓')
            wait(lambda p: p['title'] == 'Typed:Unicode ✓', 'UTF-8 editing reached live DOM')
            command('navigate', origin + '/second')
            wait(lambda p: p['title'] == 'Second page' and p['back'], 'live page navigation')
            command('back')
            wait(lambda p: p['uri'].endswith('/first') and not p['loading'], 'page history')
            x.XResizeWindow(display, parent, 960, 540)
            x.XSync(display, 0)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and geometry(child) != (207, 38, 547, 464): time.sleep(0.05)
            if geometry(child) != (207, 38, 547, 464): raise RuntimeError('Web surface did not resize with its parent')
            checks.append('resized child viewport')
            x.XDestroyWindow(display, parent)
            x.XSync(display, 0)
            parent = 0
            if process.wait(timeout=5): raise RuntimeError('Web surface did not close cleanly with its parent')
            checks.append('parent close lifecycle')
            (output / 'report.json').write_text(json.dumps({'status': 'passed', 'checks': checks}, indent=2))
            print('PASS native web view: ' + ', '.join(checks))
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            if parent:
                x.XDestroyWindow(display, parent)
                x.XSync(display, 0)
            log.close()
            server.shutdown()


if __name__ == '__main__':
    main()
