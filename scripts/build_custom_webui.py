"""
build_custom_webui.py - Customer dashboard HTML -> flashable .bin for web flasher.

Usage:
    python scripts/build_custom_webui.py <customer-dashboard.html> [--out <path>] [--force]

Output: webflasher_builds/master_custom_<8-char-hash>.bin by default.
"""
import argparse
import gzip
import hashlib
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
WEBUI_FW_DIR = PROJECT_ROOT / 'firmware' / 'wifi_master_esp32_WEBUI'
HEADER_PATH  = WEBUI_FW_DIR / 'index_html.h'
ENV_DIR      = PROJECT_ROOT / '.pio' / 'build' / 'master_html'
BOOT_APP0    = (Path.home() / '.platformio' / 'packages'
                 / 'framework-arduinoespressif32' / 'tools' / 'partitions'
                 / 'boot_app0.bin')
OUT_DIR      = PROJECT_ROOT / 'webflasher_builds'


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('html', help='Customer dashboard HTML file.')
    p.add_argument('--out', help='Output .bin path. Default: '
                                 f'{OUT_DIR.name}/master_custom_<8-char-hash>.bin')
    p.add_argument('--force', action='store_true',
                   help='Force a clean rebuild of the master_html env.')
    args = p.parse_args()

    html_path = Path(args.html).resolve()
    if not html_path.is_file():
        print(f'ERROR: {html_path} not found')
        return 1

    if args.out:
        out_bin = Path(args.out).resolve()
    else:
        short = hashlib.sha256(html_path.read_bytes()).hexdigest()[:8]
        out_bin = OUT_DIR / f'master_custom_{short}.bin'
    out_bin.parent.mkdir(parents=True, exist_ok=True)

    print(f'1. Gzipping {html_path.name} -> index_html.h')
    data = html_path.read_bytes()
    compressed = gzip.compress(data)
    c_array = ', '.join(f'0x{b:02X}' for b in compressed)
    HEADER_PATH.write_text(
        f'// Auto-generated from {html_path.name} via scripts/build_custom_webui.py\n'
        f'#ifndef INDEX_HTML_H\n#define INDEX_HTML_H\n'
        f'#include <Arduino.h>\n'
        f'const uint8_t index_html_gz[] PROGMEM = {{ {c_array} }};\n'
        f'#endif\n',
        encoding='ascii',
    )

    print('2. Building master_html')
    if args.force:
        subprocess.check_call(
            [sys.executable, '-m', 'platformio', 'run', '-e', 'master_html', '-t', 'clean'],
            cwd=str(PROJECT_ROOT),
        )
    subprocess.check_call(
        [sys.executable, '-m', 'platformio', 'run', '-e', 'master_html'],
        cwd=str(PROJECT_ROOT),
    )

    print(f'3. Merging -> {out_bin}')
    subprocess.check_call([
        sys.executable, '-m', 'esptool', '--chip', 'esp32', 'merge_bin',
        '-o', str(out_bin),
        '--flash_mode', 'dio', '--flash_size', '4MB',
        '0x1000',  str(ENV_DIR / 'bootloader.bin'),
        '0x8000',  str(ENV_DIR / 'partitions.bin'),
        '0xe000',  str(BOOT_APP0),
        '0x10000', str(ENV_DIR / 'firmware.bin'),
    ], cwd=str(PROJECT_ROOT))

    print(f'\nDone: {out_bin}  ({out_bin.stat().st_size:,} bytes)')
    print('Hand this .bin to your customer. They flash it via '
          'https://esptool.spacehuhn.com/ (Baudrate 115200, Address/Offset 0x0).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
