"""Generate only original synthetic media and run the native preview decoder tests.

Usage: python tests/media/run_preview_decode_tests.py --ffmpeg FFMPEG
       --decoder PREVIEW_DECODE_TESTS --output PRIVATE_FIXTURE_DIRECTORY
No fixtures or build outputs are written into the source tree.
"""
import argparse
import base64
import shutil
import struct
import zlib
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--decoder', '--player', dest='decoder', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)

    def ff(*options):
        subprocess.run([args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y',
                        *map(str, options)], check=True, timeout=25)

    for name, width, height in [('red', 640, 320), ('small', 40, 20)]:
        ppm = root / (name + '.ppm')
        ppm.write_bytes(f'P6\n{width} {height}\n255\n'.encode() + bytes([255, 0, 0]) * width * height)
        ff('-i', ppm, '-frames:v', 1, root / (name + '.png'))
    ff('-i', root/'red.png', '-frames:v', 1, root/'red.jpg')
    audio = ['-f', 'lavfi', '-i', 'sine=frequency=440:duration=0.4']
    ff(*audio, '-c:a', 'libmp3lame', root/'noart.mp3')
    ff(*audio, '-c:a', 'aac', root/'noart.m4a')
    ff(*audio, root/'audio.wav')
    for filename, picture, codec in [('jpeg.mp3', 'red.jpg', 'libmp3lame'),
                                      ('png.m4a', 'red.png', 'aac'),
                                      ('png.mp3', 'red.png', 'libmp3lame'),
                                      ('jpeg.m4a', 'red.jpg', 'aac'),
                                      ('png.flac', 'red.png', 'flac')]:
        ff(*audio, '-i', root/picture, '-map', '0:a', '-map', '1:v',
           '-c:a', codec, '-c:v', 'copy', '-disposition:v', 'attached_pic', root/filename)
    ff('-f', 'lavfi', '-i', 'color=c=red:s=3000x3000', '-frames:v', 1, root/'large.png')
    ff(*audio, '-i', root/'large.png', '-map', '0:a', '-map', '1:v', '-c:a', 'aac',
       '-c:v', 'copy', '-disposition:v', 'attached_pic', root/'large.m4a')
    # A syntactically valid PNG header with impossible dimensions must be rejected
    # before allocating a decoded frame. No large image data is generated.
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    (root/'oversized.png').write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', 100000, 100000, 8, 2, 0, 0, 0)) + chunk(b'IEND', b''))
    # A bad embedded picture must not prevent a usable external fallback.
    target = root/'damaged-embedded'
    target.mkdir(exist_ok=True)
    payload = b'\x00image/png\x00\x03\x00not a png image'
    frame = b'APIC' + len(payload).to_bytes(4, 'big') + b'\x00\x00' + payload
    n = len(frame)
    synchsafe = bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
    (target/'track.mp3').write_bytes(b'ID3\x03\x00\x00' + synchsafe + frame + (root/'noart.mp3').read_bytes())
    shutil.copyfile(root/'red.png', target/'cover.png')
    # Vorbis stores cover art as a base64 FLAC picture metadata block.
    picture = (root/'red.png').read_bytes()
    mime = b'image/png'
    block = (struct.pack('>II', 3, len(mime)) + mime + struct.pack('>I', 0) +
             struct.pack('>IIIII', 640, 320, 24, 0, len(picture)) + picture)
    ff(*audio, '-c:a', 'libvorbis', '-metadata',
       'METADATA_BLOCK_PICTURE=' + base64.b64encode(block).decode(), root/'png.ogg')
    native = root/'音楽 café'
    native.mkdir(exist_ok=True)
    shutil.copyfile(root/'jpeg.mp3', native/'曲.mp3')
    for directory, media, art in [('external', 'track.flac', 'cover.jpg'),
                                  ('folder', 'track.mp3', 'folder.png'),
                                  ('broken-cover', 'track.mp3', 'folder.jpg')]:
        target = root/directory
        target.mkdir(exist_ok=True)
        if media.endswith('.flac'):
            ff(*audio, '-c:a', 'flac', target/media)
        else:
            shutil.copyfile(root/'noart.mp3', target/media)
        shutil.copyfile(root/('red.png' if art.endswith('.png') else 'red.jpg'), target/art)
    (root/'broken-cover'/'cover.jpg').write_bytes(b'not an image')
    # A red opening followed by blue proves that a useful one-second seek occurred.
    ff('-f', 'lavfi', '-i', 'color=c=red:s=640x320:r=10:d=0.6',
       '-f', 'lavfi', '-i', 'color=c=blue:s=640x320:r=10:d=1.4',
       '-filter_complex', '[0:v][1:v]concat=n=2:v=1:a=0',
       '-c:v', 'mpeg4', '-g', 1, root/'movie.mp4')
    ff('-f', 'lavfi', '-i', 'color=c=red:s=640x320:r=10:d=0.2', '-c:v', 'mpeg4', root/'short.mp4')
    ff('-f', 'lavfi', '-i', 'color=c=red:s=320x640:r=10:d=0.2', '-c:v', 'mpeg4', root/'portrait.mp4')
    ff('-f', 'lavfi', '-i', 'color=c=red:s=320x320:r=10:d=0.2', '-vf', 'setsar=2', '-c:v', 'mpeg4', root/'anamorphic.mp4')
    (root/'invalid.png').write_bytes(b'not an image\x00\xff')
    (root/'truncated.mp4').write_bytes((root/'movie.mp4').read_bytes()[:80])
    subprocess.run([args.decoder, str(root)], check=True, timeout=60)


if __name__ == '__main__':
    main()


