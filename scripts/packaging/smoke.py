#!/usr/bin/env python3
"""Verify installed assets, executable startup, document tools and rendered pixels."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[2]


def document_fixture(path):
    stream = b'BT /F1 22 Tf 36 120 Td (OpenXMB package smoke) Tj ET\n'
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 320 180] '
               b'/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
               b'<< /Length ' + str(len(stream)).encode() + b' >>\nstream\n' + stream + b'endstream']
    contents = bytearray(b'%PDF-1.4\n')
    offsets = [0]
    for number, value in enumerate(objects, 1):
        offsets.append(len(contents))
        contents.extend(f'{number} 0 obj\n'.encode() + value + b'\nendobj\n')
    start = len(contents)
    contents.extend(f'xref\n0 {len(offsets)}\n0000000000 65535 f \n'.encode())
    for offset in offsets[1:]:
        contents.extend(f'{offset:010d} 00000 n \n'.encode())
    contents.extend(f'trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n'.encode())
    path.write_bytes(contents)


def check_helpers(helpers, directory, environment):
    def invoke(name, *arguments):
        return subprocess.run([str(helpers[name]), *map(str, arguments)], env=environment,
                              capture_output=True, timeout=45, check=True)

    pdf = directory / 'document.pdf'
    document_fixture(pdf)
    invoke('pdftotext', pdf, directory / 'document.txt')
    if 'OpenXMB package smoke' not in (directory / 'document.txt').read_text():
        raise RuntimeError('The packaged PDF tools could not extract document text')
    invoke('pdftoppm', '-singlefile', '-scale-to', '320', '-png', pdf, directory / 'document')
    from PIL import Image, ImageStat
    with Image.open(directory / 'document.png') as image:
        if max(ImageStat.Stat(image.convert('RGB')).stddev) < 3:
            raise RuntimeError('The packaged PDF tools rendered an empty page')
    audio = directory / 'audio.wav'
    with wave.open(str(audio), 'wb') as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(22050)
        handle.writeframes(b'\x00\x00' * 2205)
    probe = json.loads(invoke('ffprobe', '-v', 'error', '-show_streams', '-of', 'json', audio).stdout)
    if not any(stream.get('codec_name') == 'pcm_s16le' for stream in probe['streams']):
        raise RuntimeError('The packaged media tools did not recognize PCM audio')
    invoke('ffmpeg', '-v', 'error', '-i', audio, '-f', 'null', '-')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--render', action='store_true')
    parser.add_argument('--background', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    app, assets = args.app.resolve(), args.assets.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    version = (ROOT / 'VERSION').read_text().strip()
    environment = os.environ.copy()
    # Keep system commands, but remove build-tool dependency directories. All
    # application DLLs/dylibs and packaged helpers must resolve from the package.
    if platform.system() == 'Windows':
        environment['PATH'] = os.pathsep.join([str(app.parent), str(app.parent / 'tools/poppler/bin'),
                                              os.environ['SystemRoot'] + '\\System32', os.environ['SystemRoot']])
    elif platform.system() == 'Darwin':
        environment['PATH'] = str(app.parent) + ':/usr/bin:/bin:/usr/sbin:/sbin'
        for key in ['DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'DYLD_FRAMEWORK_PATH']:
            environment.pop(key, None)
    for key in ['XMB_ASSET_DIR', 'XMB_LOCALE_DIR', 'OPENXMB_CONFIG', 'OPENXMB_DATA_DIR']:
        environment.pop(key, None)
    for relative in ['Play-Regular.ttf', 'catalog/en.json', 'icons/theme/icon_setting.png',
                     'normalmaps/theme/icon_setting.png', 'sounds/ok.wav']:
        if not (assets / relative).is_file():
            raise RuntimeError('Missing packaged asset: ' + relative)
    for option in ['--help', '--version']:
        result = subprocess.run([str(app), option], env=environment, cwd=args.output,
                                text=True, capture_output=True, timeout=45, check=True)
        if option == '--version' and version not in result.stdout + result.stderr:
            raise RuntimeError('Packaged executable reports the wrong release version')
    checks = ['packaged assets', 'clean-environment startup and version']
    defaults = [app.parent / 'config.json', app.parent / '../Resources/share/OpenXMB/config.json',
                app.parent / '../share/OpenXMB/config.json']
    default_config = next((path for path in defaults if path.is_file()), None)
    if default_config is None:
        raise RuntimeError('Missing packaged default configuration')
    helpers = {}
    for name in ['ffmpeg', 'ffprobe', 'pdftoppm', 'pdftotext', 'pdfinfo']:
        helper = app.parent / (name + ('.exe' if platform.system() == 'Windows' else ''))
        if platform.system() == 'Windows' and name.startswith('pdf'):
            helper = app.parent / 'tools/poppler/bin' / (name + '.exe')
        if platform.system() == 'Linux':
            helper = Path('/usr/bin') / name
        helpers[name] = helper
        subprocess.run([str(helper), '-version' if name.startswith('ff') else '-v'],
                       env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=30, check=True)
    check_helpers(helpers, args.output.resolve(), environment)
    checks.append('packaged PDF rendering/text extraction and audio decoding')
    if args.render or args.background:
        with tempfile.TemporaryDirectory(prefix='openxmb-package-smoke-') as temporary:
            root = Path(temporary)
            config = json.loads(default_config.read_text())
            config['render'].update({'sample-count': 1, 'vsync': False, 'max-fps': 30,
                                     'show-fps': False, 'show-mem': False})
            config['shell'].update({'font-path': 'default', 'background-type': 'original',
                                    'theme-colour-mode': 'custom', 'theme-custom-colour': '#512360'})
            (root / 'config.json').write_text(json.dumps(config))
            frames = root / 'frames'
            frames.mkdir()
            environment.update({
                'OPENXMB_CONFIG': str(root / 'config.json'), 'OPENXMB_DATA_DIR': str(root / 'data'),
                'SDL_AUDIODRIVER': 'dummy', 'SDL_VIDEODRIVER': 'dummy',
                'AUROREENGINE_HEADLESS': '1', 'AUROREENGINE_HEADLESS_AUDIO': '1',
                'AUROREENGINE_HEADLESS_WIDTH': '960', 'AUROREENGINE_HEADLESS_HEIGHT': '640',
                'AUROREENGINE_HEADLESS_FRAMES': '16', 'AUROREENGINE_HEADLESS_OUTPUT_DIR': str(frames),
                'AUROREENGINE_HEADLESS_OUTPUT_PATTERN': 'frame-{:06d}.png',
                'OPENXMB_HEADLESS_STARTUP': '0', 'OPENXMB_FIXED_BOOT_SECONDS': '17',
                'OPENXMB_FIXED_UNIX_SECONDS': '1781524800', 'OPENXMB_FIXED_WAVE_SECONDS': '4',
            })
            command = [str(app), '--width', '960', '--height', '640', '--no-fullscreen']
            if args.background:
                command.append('--background-only')
            with (args.output / 'render.log').open('w') as log:
                subprocess.run(command, env=environment, cwd=root, stdout=log,
                               stderr=subprocess.STDOUT, check=True, timeout=180)
            images = sorted(frames.glob('*.png'))
            if len(images) < 5:
                raise RuntimeError('The installed renderer did not produce frames')
            from PIL import Image, ImageStat
            image = Image.open(images[-1]).convert('RGB')
            if image.size != (960, 640) or max(ImageStat.Stat(image).stddev) < 3:
                raise RuntimeError('The installed renderer produced a blank image')
            image.save(args.output / 'render.png')
            if args.background:
                args.background.parent.mkdir(parents=True, exist_ok=True)
                image.save(args.background)
            checks.append('offscreen Vulkan rendering')
    (args.output / 'report.json').write_text(json.dumps({'status': 'passed', 'checks': checks}, indent=2) + '\n')
    print('PASS package smoke: ' + ', '.join(checks))


if __name__ == '__main__':
    main()
