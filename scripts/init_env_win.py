#!/usr/bin/env python3
# Setup required libraries for compiling blutter on Windows
# Note: for Windows only, downloads ICU and capstone to external/ directory
import io
import os
import shutil
import sys
import zipfile
from pathlib import Path

try:
    import requests
except ImportError:
    print("Error: requests is required. Run: pip install requests", file=sys.stderr)
    sys.exit(1)

ICU_LIB_URL = 'https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-Win64-MSVC2019.zip'
CAPSTONE_LIB_URL = 'https://github.com/capstone-engine/capstone/releases/download/4.0.2/capstone-4.0.2-win64.zip'

SCRIPT_DIR = Path(__file__).parent
PROJECT_DIR = SCRIPT_DIR.parent
BIN_DIR = PROJECT_DIR / 'bin'
EXTERNAL_DIR = PROJECT_DIR / 'external'
ICU_WINDOWS_FILE = EXTERNAL_DIR / 'icu-windows.zip'
ICU_WINDOWS_DIR = EXTERNAL_DIR / 'icu-windows'
CAPSTONE_DIR = EXTERNAL_DIR / 'capstone'


def main():
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    EXTERNAL_DIR.mkdir(parents=True, exist_ok=True)

    # capstone
    print('Downloading Capstone from ' + CAPSTONE_LIB_URL)
    r = requests.get(CAPSTONE_LIB_URL, timeout=600)
    r.raise_for_status()
    print('Extracting Capstone library')
    if CAPSTONE_DIR.exists():
        shutil.rmtree(CAPSTONE_DIR)
    with zipfile.ZipFile(io.BytesIO(r.content)) as z:
        capstone_zip_dir = z.namelist()[0].split('/', 1)[0]
        z.extractall(EXTERNAL_DIR)
    os.rename(EXTERNAL_DIR / capstone_zip_dir, CAPSTONE_DIR)

    # icu
    print('Downloading ICU library from ' + ICU_LIB_URL)
    r = requests.get(ICU_LIB_URL, timeout=600)
    r.raise_for_status()
    print('Extracting ICU library')
    if ICU_WINDOWS_DIR.exists():
        shutil.rmtree(ICU_WINDOWS_DIR)
    with zipfile.ZipFile(io.BytesIO(r.content)) as z:
        with z.open(z.namelist()[-1]) as zf, open(ICU_WINDOWS_FILE, 'wb') as f:
            shutil.copyfileobj(zf, f)
    with zipfile.ZipFile(ICU_WINDOWS_FILE) as z:
        z.extractall(ICU_WINDOWS_DIR)
    ICU_WINDOWS_FILE.unlink()

    # copy to bin (version of icu dll MUST be updated)
    print('Copying dlls to bin directory')
    shutil.copy(CAPSTONE_DIR / 'capstone.dll', BIN_DIR)
    shutil.copy(ICU_WINDOWS_DIR / 'bin64' / 'icudt73.dll', BIN_DIR)
    shutil.copy(ICU_WINDOWS_DIR / 'bin64' / 'icuuc73.dll', BIN_DIR)

    print('Done')


if __name__ == '__main__':
    main()
