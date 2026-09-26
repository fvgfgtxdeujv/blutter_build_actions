#!/usr/bin/python3
"""B(l)utter 定制版运行/构建入口。

运行：
    python3 blutter.py <apk | lib 目录 | Windows 应用目录/app.so> <输出目录>

查找匹配版本的 blutter 可执行文件（$HOME/blutter/bin）：
  * 已存在 -> 直接解析；
  * 不存在 -> 优先从 Releases 远程下载；下载不可用/失败时，若处于本仓库的
    完整源码检出（scripts/build.py 与 blutter/ 就位），用定制版 blutter/src
    构建 dartvm + blutter（复刻官方 dartvm_fetch_build.py + blutter.py 的行为）；
  * `--rebuild` -> 跳过下载，强制用定制版 blutter/src 重建。
"""
import argparse
import io
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib
from struct import unpack

import requests

from elftools.elf.elffile import ELFFile

class DartLibInfo:
    def __init__(self, version: str, os_name: str, arch: str, has_compressed_ptrs = None, snapshot_hash = None):
        self.version = version
        self.os_name = os_name
        self.arch = arch
        self.snapshot_hash = snapshot_hash
        # 目标仅支持 android(arm64) 与 windows(x64)。缺省按安卓惯例视为压缩指针；
        # windows 二进制命名依赖此缺省（无 _no-compressed-ptrs 后缀）
        self.has_compressed_ptrs = has_compressed_ptrs is None or has_compressed_ptrs
        self.lib_name = f'dartvm{version}_{os_name}_{arch}'

# 二进制固定存放目录（$HOME/blutter/bin）
BIN_DIR = os.path.join(os.path.expanduser('~'), 'blutter', 'bin')


class BlutterInput:
    def __init__(self, libapp_path: str, dart_info: DartLibInfo, outdir: str, no_analysis: bool, blacklist: str = None, rebuild: bool = False):
        self.libapp_path = libapp_path
        self.dart_info = dart_info
        self.outdir = outdir
        self.blacklist = blacklist
        self.rebuild = rebuild

        vers = dart_info.version.split('.', 2)
        if int(vers[0]) == 2 and int(vers[1]) < 15:
            if not no_analysis:
                print('Dart version <2.15, force "no-analysis" option')
            no_analysis = True
        self.no_analysis = no_analysis

        # Note: null-safety is detected in blutter application, so no need another build of blutter for null-safety
        self.name_suffix = ''
        if not dart_info.has_compressed_ptrs:
            self.name_suffix += '_no-compressed-ptrs'
        if no_analysis:
            self.name_suffix += '_no-analysis'
        # derive blutter executable filename
        self.blutter_name = f'blutter_{dart_info.lib_name}{self.name_suffix}'
        self.blutter_file = os.path.join(BIN_DIR, self.blutter_name) + ('.exe' if os.name == 'nt' else '')


# ========== Dart 版本信息提取 ==========

def extract_snapshot_hash_flags(libapp_file):
    with open(libapp_file, 'rb') as f:
        elf = ELFFile(f)
        # find "_kDartVmSnapshotData" symbol
        dynsym = elf.get_section_by_name('.dynsym')
        sym = dynsym.get_symbol_by_name('_kDartVmSnapshotData')[0]
        #section = elf.get_section(sym['st_shndx'])
        assert sym['st_size'] > 128
        f.seek(sym['st_value']+20)
        snapshot_hash = f.read(32).decode()
        data = f.read(256) # should be enough
        flags = data[:data.index(b'\0')].decode().strip().split(' ')
    
    return snapshot_hash, flags

def extract_libflutter_info(libflutter_file):
    with open(libflutter_file, 'rb') as f:
        elf = ELFFile(f)
        if elf.header.e_machine == 'EM_AARCH64': # 183
            arch = 'arm64'
        elif elf.header.e_machine == 'EM_IA_64': # 50
            arch = 'x64'
        else:
            assert False, f"Unsupport architecture: {elf.header.e_machine}"

        section = elf.get_section_by_name('.rodata')
        data = section.data()
        
        sha_hashes = re.findall(b'\x00([a-f\\d]{40})(?=\x00)', data)
        #print(sha_hashes)
        # all possible engine ids
        engine_ids = [ h.decode() for h in sha_hashes ]
        assert len(engine_ids) == 2, f'found hashes {", ".join(engine_ids)}'
        
        # beta/dev version of flutter might not use stable dart version (we can get dart version from sdk with found engine_id)
        # support stable, beta and dev channels
        m = re.search(br'\x00([\d\w\.-]+) \((stable|beta|dev)\)', data)
        if m is None:
            dart_version = None
        else:
            dart_version = m.group(1).decode()
        
    return engine_ids, dart_version, arch, 'android'

def get_dart_sdk_url_size(engine_ids):
    #url = f'https://storage.googleapis.com/dart-archive/channels/stable/release/3.0.3/sdk/dartsdk-windows-x64-release.zip'
    for engine_id in engine_ids:
        url = f'https://storage.googleapis.com/flutter_infra_release/flutter/{engine_id}/dart-sdk-windows-x64.zip'
        resp = requests.head(url)
        if resp.status_code == 200:
           sdk_size = int(resp.headers['Content-Length'])
           return engine_id, url, sdk_size
    
    return None, None, None

def get_dart_commit(url):
    # in downloaded zip
    # * dart-sdk/revision - the dart commit id of https://github.com/dart-lang/sdk/
    # * dart-sdk/version  - the dart version
    # revision and version zip file records should be in first 4096 bytes
    # using stream in case a server does not support range
    commit_id = None
    dart_version = None
    fp = None
    with requests.get(url, headers={"Range": "bytes=0-4096"}, stream=True) as r:
        if r.status_code // 10 == 20:
            x = next(r.iter_content(chunk_size=4096))
            fp = io.BytesIO(x)
    
    if fp is not None:
        while fp.tell() < 4096-30 and (commit_id is None or dart_version is None):
            _, _, _, compMethod, _, _, _, compressSize, _, filenameLen, extraLen = unpack('<IHHHHHIIIHH', fp.read(30))
            filename = fp.read(filenameLen)
            #print(filename)
            if extraLen > 0:
                fp.seek(extraLen, io.SEEK_CUR)
            data = fp.read(compressSize)
            
            # expect compression method to be zipfile.ZIP_DEFLATED
            assert compMethod == zipfile.ZIP_DEFLATED, 'Unexpected compression method'
            if filename == b'dart-sdk/revision':
                commit_id = zlib.decompress(data, wbits=-zlib.MAX_WBITS).decode().strip()
            elif filename == b'dart-sdk/version':
                dart_version = zlib.decompress(data, wbits=-zlib.MAX_WBITS).decode().strip()
    
    # TODO: if no revision and version in first 4096 bytes, get the file location from the first zip dir entries at the end of file (less than 256KB)
    return commit_id, dart_version

def extract_dart_info(libapp_file: str, libflutter_file: str):
    snapshot_hash, flags = extract_snapshot_hash_flags(libapp_file)
    #print('snapshot hash', snapshot_hash)
    #print(flags)

    engine_ids, dart_version, arch, os_name = extract_libflutter_info(libflutter_file)
    # print('possible engine ids', engine_ids)
    # print('dart version', dart_version)

    if dart_version is None:
        engine_id, sdk_url, sdk_size = get_dart_sdk_url_size(engine_ids)
        # print(engine_id)
        # print(sdk_url)
        # print(sdk_size)

        commit_id, dart_version = get_dart_commit(sdk_url)
        # print(commit_id)
        # print(dart_version)
        #assert dart_version == dart_version_sdk
    
    # 仅识别 Android（ELF 提取恒为 android）；Windows 桌面走 extract_windows_dart_info
    return dart_version, snapshot_hash, flags, arch, os_name


def detect_windows_target(indir: str):
    """自动检测 Flutter Windows 桌面目标：flutter_windows.dll 是强特征。
    返回 (app_so, flutter_windows.dll) 绝对路径；未识别返回 None。"""
    if os.path.isdir(indir):
        dll_cands = [
            os.path.join(indir, 'flutter_windows.dll'),
            os.path.join(os.path.dirname(os.path.abspath(indir)), 'flutter_windows.dll'),
        ]
        app_cands = [
            os.path.join(indir, 'data', 'app.so'),
            os.path.join(indir, 'app.so'),
        ]
    else:
        d = os.path.dirname(os.path.abspath(indir))
        dll_cands = [
            os.path.join(d, 'flutter_windows.dll'),
            os.path.join(os.path.dirname(d), 'flutter_windows.dll'),
        ]
        app_cands = [os.path.abspath(indir)]
    dll = next((c for c in dll_cands if os.path.isfile(c)), None)
    if dll is None:
        return None
    app = next((c for c in app_cands if os.path.isfile(c)), None)
    if app is None:
        return None
    return os.path.abspath(app), os.path.abspath(dll)


def extract_windows_dll_info(dll_file):
    """从 flutter_windows.dll（PE 格式）提取 Flutter engine id（SHA1）与 Dart 版本字符串。
    与 Android libflutter.so 的 .rodata 提取逻辑对应（引擎内同款字符串）。"""
    with open(dll_file, 'rb') as f:
        data = f.read()
    engine_ids = list(dict.fromkeys(h.decode() for h in re.findall(b'\x00([a-f\\d]{40})(?=\\x00)', data)))
    m = re.search(br'\x00([\d\w\.-]+) \((stable|beta|dev)\)', data)
    dart_version = m.group(1).decode() if m else None
    return engine_ids, dart_version


def extract_windows_dart_info(libapp_file, dll_file):
    """Windows 桌面目标：snapshot hash/flags 来自 app.so（x64 ELF），Dart 版本来自 flutter_windows.dll。"""
    snapshot_hash, flags = extract_snapshot_hash_flags(libapp_file)
    engine_ids, dart_version = extract_windows_dll_info(dll_file)
    if dart_version is None and engine_ids:
        engine_id, sdk_url, sdk_size = get_dart_sdk_url_size(engine_ids[:5])
        if sdk_url:
            commit_id, dart_version = get_dart_commit(sdk_url)
    if dart_version is None:
        print("错误：无法从 flutter_windows.dll 自动识别 Dart 版本。")
        print("请用 --dart-version <版本>_windows_x64 手动指定，例如 --dart-version 3.3.4_windows_x64")
        sys.exit(1)
    return dart_version, snapshot_hash, flags, 'x64', 'windows'


def find_lib_files(indir: str):
    app_file = os.path.join(indir, 'libapp.so')
    if not os.path.isfile(app_file):
        sys.exit("Cannot find libapp.so（仅支持 Android arm64 与 Flutter Windows 桌面目标，不支持 iOS）")
    
    flutter_file = os.path.join(indir, 'libflutter.so')
    if not os.path.isfile(flutter_file):
        sys.exit("Cannot find libflutter.so")
    
    return os.path.abspath(app_file), os.path.abspath(flutter_file)

def extract_libs_from_apk(apk_file: str, out_dir: str):
    with zipfile.ZipFile(apk_file, "r") as zf:
        try:
            app_info = zf.getinfo('lib/arm64-v8a/libapp.so')
            flutter_info = zf.getinfo('lib/arm64-v8a/libflutter.so')
        except:
            sys.exit("Cannot find libapp.so or libflutter.so in the APK")

        zf.extract(app_info, out_dir)
        zf.extract(flutter_info, out_dir)

        app_file = os.path.join(out_dir, app_info.filename)
        flutter_file = os.path.join(out_dir, flutter_info.filename)
        return app_file, flutter_file


def get_dart_lib_info(libapp_path: str, libflutter_path: str) -> DartLibInfo:
    # getting dart version
    dart_version, snapshot_hash, flags, arch, os_name = extract_dart_info(libapp_path, libflutter_path)
    print(f'Dart version: {dart_version}, Snapshot: {snapshot_hash}, Target: {os_name} {arch}')
    print('flags: ' + ' '.join(flags))

    has_compressed_ptrs = 'compressed-pointers' in flags
    return DartLibInfo(dart_version, os_name, arch, has_compressed_ptrs, snapshot_hash)

# blutter 构建产物仓库：GitHub 主源 + Gitee 国内镜像
GITHUB_RELEASE_REPO = 'fvgfgtxdeujv/blutter_build_actions'
GITEE_RELEASE_REPO = 'qeruiop_admin/blutter_build_actions'


def get_ubuntu_info():
    # 读取 /etc/os-release 获取发行版 ID 与主版本号（如 Ubuntu 22.04 -> ('ubuntu', '22')）
    try:
        with open('/etc/os-release', 'r') as f:
            content = f.read()
        m_id = re.search(r'^ID="?([a-zA-Z]+)"?', content, re.M)
        m_ver = re.search(r'^VERSION_ID="?([\d.]+)"?', content, re.M)
        distro = m_id.group(1) if m_id else None
        major = m_ver.group(1).split('.')[0] if m_ver else None
        return distro, major
    except Exception:
        return None, None


def is_supported_environment():
    # 仅支持 Ubuntu 22.04 或 24.04
    distro, major = get_ubuntu_info()
    return distro == 'ubuntu' and major in ('22', '24')


def get_system_timezone():
    # 读取系统时区（优先 TZ 环境变量，其次 /etc/timezone，最后 date 命令）
    tz = os.environ.get('TZ')
    if tz:
        return tz.strip()
    try:
        with open('/etc/timezone', 'r') as f:
            return f.read().strip()
    except Exception:
        pass
    try:
        out = subprocess.run(['date', '+%Z'], capture_output=True, text=True, timeout=5)
        return out.stdout.strip()
    except Exception:
        return None


def measure_github_latency(timeout=10):
    # 用 curl 测试 github.com 的响应时间（秒 -> 毫秒），失败返回 None
    try:
        out = subprocess.run(
            ['curl', '-o', '/dev/null', '-s', '-w', '%{time_total}',
             '--connect-timeout', '5', '-m', str(timeout), 'https://github.com'],
            capture_output=True, text=True, timeout=timeout + 2)
        return float(out.stdout.strip()) * 1000
    except Exception:
        return None


CN_TIMEZONES = ('Asia/Shanghai', 'Asia/Chongqing', 'Asia/Urumqi',
                'Asia/Harbin', 'Asia/Macau', 'Asia/Hong_Kong', 'Asia/Taipei', 'PRC')


def detect_region():
    # 综合语言、时区、GitHub 延迟判断网络区域，返回 'gitee'（国内）或 'github'
    score = 0
    # 1. 语言：系统语言含中文（如 zh_CN）计 1 分
    lang = ' '.join(os.environ.get(k, '') for k in ('LANG', 'LC_ALL', 'LANGUAGE'))
    if re.search(r'zh[-_.]', lang, re.I):
        print(f"语言: {lang.strip() or '未知'} -> 中文环境")
        score += 1
    # 2. 时区：中国时区计 1 分
    tz = get_system_timezone()
    if tz and any(cn in tz for cn in CN_TIMEZONES):
        print(f"时区: {tz} -> 国内时区")
        score += 1
    # 3. GitHub 延迟：响应时间大于阈值（800ms）计 1 分
    latency = measure_github_latency()
    if latency is not None:
        print(f"GitHub 延迟: {latency:.0f} ms")
        if latency > 800:
            score += 1
    region = 'gitee' if score >= 2 else 'github'
    print(f"区域判定: 国内信号 {score}/3 -> 使用 {region}")
    return region


def download_blutter_binary(input: BlutterInput) -> bool:
    # 按区域选择主源（国内 Gitee / 国外 GitHub），失败自动切换到备选源
    ver = input.dart_info.version
    distro, major = get_ubuntu_info()
    base = f'blutter_dartvm{ver}_{input.dart_info.os_name}_{input.dart_info.arch}'

    region = detect_region()
    if region == 'gitee':
        hosts = [
            f'https://gitee.com/{GITEE_RELEASE_REPO}/releases/download',
            f'https://github.com/{GITHUB_RELEASE_REPO}/releases/download',
        ]
    else:
        hosts = [
            f'https://github.com/{GITHUB_RELEASE_REPO}/releases/download',
            f'https://gitee.com/{GITEE_RELEASE_REPO}/releases/download',
        ]

    urls = []
    for host in hosts:
        if os.name == 'nt':
            # Windows 平台产物带 _win 后缀（releases 附件，运行时 dll 从仓库 build/ 目录下载）
            urls.append(f'{host}/{ver}/{base}_win.exe')
        if major in ('22', '24'):
            urls.append(f'{host}/{ver}/{base}_{major}')
        urls.append(f'{host}/{ver}/{base}')

    os.makedirs(BIN_DIR, exist_ok=True)
    for url in urls:
        try:
            print(f"正在下载: {url}")
            r = requests.get(url, timeout=180)
            if r.status_code != 200:
                print(f"  HTTP {r.status_code}，尝试下一个地址")
                continue
            with open(input.blutter_file, 'wb') as f:
                f.write(r.content)
            os.chmod(input.blutter_file, 0o755)
            print(f"下载完成: {input.blutter_file}")
            return True
        except Exception as e:
            print(f"  下载出错: {e}")
    return False


# Windows 运行依赖 dll（capstone + ICU，与 Dart 版本无关），存放于构建仓库 build/ 目录
WIN_RUN_DLLS = ('capstone.dll', 'icuuc73.dll', 'icudt73.dll')


def get_build_raw_hosts():
    # 按区域返回构建仓库 build/ 目录的 raw 下载地址（国内 Gitee 优先，国外 GitHub 优先）
    region = detect_region()
    if region == 'gitee':
        return [
            f'https://gitee.com/{GITEE_RELEASE_REPO}/raw/master/build',
            f'https://raw.githubusercontent.com/{GITHUB_RELEASE_REPO}/master/build',
        ]
    return [
        f'https://raw.githubusercontent.com/{GITHUB_RELEASE_REPO}/master/build',
        f'https://gitee.com/{GITEE_RELEASE_REPO}/raw/master/build',
    ]


def ensure_win_dlls() -> bool:
    # 仅 Windows 平台需要；检查 bin 目录 dll，缺失则从构建仓库 build/ 目录下载
    if os.name != 'nt':
        return True
    missing = [d for d in WIN_RUN_DLLS if not os.path.isfile(os.path.join(BIN_DIR, d))]
    if not missing:
        return True
    print(f"缺少 Windows 运行依赖 dll: {', '.join(missing)}，尝试从构建仓库 build/ 目录下载...")
    os.makedirs(BIN_DIR, exist_ok=True)
    ok = True
    for dll in missing:
        got = False
        for base in get_build_raw_hosts():
            url = f'{base}/{dll}'
            try:
                print(f"正在下载: {url}")
                r = requests.get(url, timeout=180)
                if r.status_code == 200:
                    with open(os.path.join(BIN_DIR, dll), 'wb') as f:
                        f.write(r.content)
                    print(f"下载完成: {os.path.join(BIN_DIR, dll)}")
                    got = True
                    break
                print(f"  HTTP {r.status_code}，尝试下一个地址")
            except Exception as e:
                print(f"  下载出错: {e}")
        if not got:
            ok = False
            print(f"下载失败: {dll}")
    return ok


# ========== 源码构建（基于本仓库定制版 blutter/src） ==========
# 复刻官方 dartvm_fetch_build.py + blutter.py 的行为：默认优先远程下载，
# 下载不可用/失败时用仓库内的 scripts/build.py 从定制版 blutter/src 构建
#   clone-dart -> generate-sources -> build-dartvm -> build-blutter
# 仓库源码不完整时（例如只解压了运行包）则无法源码构建。

# 判定“完整源码仓库”需同时具备的文件
REPO_MARKER_FILES = (
    os.path.join('scripts', 'build.py'),
    os.path.join('scripts', 'CMakeLists.txt'),
    os.path.join('scripts', 'dartvm_create_srclist.py'),
    os.path.join('blutter', 'CMakeLists.txt'),
)

# dart_info 的 (os_name, arch) -> scripts/build.py 的 --arch 取值
BUILD_ARCH_MAP = {
    ('android', 'arm64'): 'aarch64',
    ('windows', 'x64'): 'windows_x64',
    ('linux', 'x64'): 'x86_64',
}


def find_repo_root():
    """定位完整源码仓库根目录；未找到返回 None。

    搜索顺序：脚本所在目录 -> 其父目录 -> 当前工作目录。既支持脚本放在仓库根
    直接运行，也支持从 zip 解压后置于仓库内运行。
    """
    script_dir = os.path.dirname(os.path.realpath(__file__))
    candidates = [script_dir, os.path.dirname(script_dir), os.getcwd()]
    for cand in candidates:
        if all(os.path.isfile(os.path.join(cand, rel)) for rel in REPO_MARKER_FILES):
            return os.path.abspath(cand)
    return None


def _build_toolchain_env():
    """构建所需环境变量。Debian 等发行版的 libstdc++ 缺 std::format，统一用
    clang-16 + libc++，并让 Dart VM 与 blutter 使用同一套 C++ 标准库。"""
    env = os.environ.copy()
    cxx = env.get('CXX')
    if not cxx:
        for cand in ('clang++-16', 'clang++'):
            if shutil.which(cand):
                cxx = cand
                env.setdefault('CC', cand.replace('++', ''))
                break
    if cxx:
        env['CXX'] = cxx
        env['CXXFLAGS'] = (env.get('CXXFLAGS', '') + ' -stdlib=libc++').strip()
        env['LDFLAGS'] = (env.get('LDFLAGS', '') + ' -stdlib=libc++').strip()
    return env


def build_from_source(input: BlutterInput, repo_root: str) -> bool:
    """用仓库内 scripts/build.py 构建定制版 blutter，成功返回 True。

    仅支持默认（含代码分析、压缩指针）变体；no-analysis 等无对应构建产物的
    变体由调用方走下载路径。
    """
    di = input.dart_info
    if input.name_suffix:
        print(f"源码构建不支持该变体（{input.name_suffix}），跳过，改用下载。")
        return False

    build_arch = BUILD_ARCH_MAP.get((di.os_name, di.arch))
    if build_arch is None:
        print(f"源码构建不支持目标 {di.os_name}/{di.arch}，跳过，改用下载。")
        return False

    missing = [t for t in ('cmake', 'ninja', 'git') if not shutil.which(t)]
    if missing:
        print(f"缺少源码构建所需工具：{', '.join(missing)}")
        print("请安装依赖后重试，例如：")
        print("  apt-get install -y cmake ninja-build git clang-16 libc++-16-dev libc++abi-16-dev libcapstone-dev libicu-dev ccache")
        print("本次改用下载预编译二进制。")
        return False

    build_py = os.path.join(repo_root, 'scripts', 'build.py')
    env = _build_toolchain_env()
    host = platform.machine().lower()
    cross = build_arch == 'aarch64' and host not in ('aarch64', 'arm64')

    def step(*args):
        cmd = [sys.executable, build_py, *args]
        print(f">>> {' '.join(cmd)}")
        return subprocess.run(cmd, cwd=repo_root, env=env).returncode

    dart_lib = di.lib_name
    if os.name == 'nt':
        dartlib_file = os.path.join(repo_root, 'packages', 'lib', dart_lib + '.lib')
    else:
        dartlib_file = os.path.join(repo_root, 'packages', 'lib', 'lib' + dart_lib + '.a')

    print(f"检测到完整源码仓库：{repo_root}")
    print(f"从定制版 blutter/src 构建 {dart_lib}（arch={build_arch}, cross={cross}）...")

    if not os.path.isfile(dartlib_file):
        if step('clone-dart', di.version) != 0:
            return False
        if step('generate-sources', di.version) != 0:
            return False
        if cross:
            if step('generate-toolchain') != 0:
                return False
            if step('setup-icu') != 0:
                return False
        if step('build-dartvm', di.version, '--arch', build_arch) != 0:
            return False
    else:
        print(f"[=] 复用已有 Dart VM 静态库：{dartlib_file}")

    if step('build-blutter', di.version, '--arch', build_arch) != 0:
        return False

    built = os.path.join(repo_root, 'bin', f'blutter_{dart_lib}')
    if sys.platform == 'win32':
        built += '.exe'
    if not os.path.isfile(built):
        print(f"构建完成但未找到产物：{built}")
        return False

    os.makedirs(os.path.dirname(input.blutter_file), exist_ok=True)
    if os.path.abspath(built) != os.path.abspath(input.blutter_file):
        shutil.copy2(built, input.blutter_file)
    print(f"[+] 源码构建产物：{input.blutter_file}")
    return True


def build_and_run(input: BlutterInput):
    # 缺少匹配二进制（或显式 --rebuild）时：
    #   默认优先从 Releases 远程下载，下载不可用/失败再回退本地源码构建；
    #   --rebuild 则跳过下载，直接用定制版 blutter/src 重建。
    if not os.path.isfile(input.blutter_file) or input.rebuild:
        print("=" * 60)
        if input.rebuild:
            print(f"--rebuild：从源码重建 blutter（Dart {input.dart_info.version}）")
        else:
            print(f"未找到对应版本的 blutter 二进制: {input.blutter_file}")
            print(f"该目标使用的 Dart 版本为: {input.dart_info.version}")
        if input.dart_info.os_name == 'windows' and os.name != 'nt':
            print("解析 Flutter Windows 桌面目标需在 Windows 宿主运行（下载 _win.exe 产物）。")
            print(f"或在 {BIN_DIR} 手动放置兼容二进制后重新运行。")
            print("=" * 60)
            sys.exit(1)

        built = False
        if input.rebuild:
            # 显式强制源码重建：跳过远程下载
            repo_root = find_repo_root()
            if repo_root is None:
                print("--rebuild 需要完整源码仓库（scripts/build.py、blutter/ 等），当前未检测到。")
                print("请在仓库根目录运行，或去掉 --rebuild 使用自动下载。")
                print("=" * 60)
                sys.exit(1)
            built = build_from_source(input, repo_root)
        else:
            # 优先远程下载，失败再回退本地源码构建
            if os.name == 'nt' or is_supported_environment():
                print("尝试从 Releases 自动下载匹配的二进制...")
                built = download_blutter_binary(input)
                if built:
                    print("自动下载完成，开始解析。")
                else:
                    print("自动下载失败，尝试从源码构建...")
            else:
                distro, major = get_ubuntu_info()
                print(f"当前系统或当前系统版本不支持下载（当前: {distro or '未知系统'} {major or ''}，仅 Ubuntu 22.04 或 24.04），跳过远程下载。")
                print("尝试从源码构建...")
            if not built:
                repo_root = find_repo_root()
                if repo_root is not None:
                    built = build_from_source(input, repo_root)
                else:
                    print("未检测到完整源码仓库（scripts/build.py、blutter/ 等），无法源码构建。")

        if not built:
            print("=" * 60)
            print("获取匹配的 blutter 二进制失败。请手动从 blutter 构建仓库的 Releases 下载")
            print(f"例如 blutter_dartvm{input.dart_info.version}_{input.dart_info.os_name}_{input.dart_info.arch}（或带 _22/_24/_win.exe 后缀），")
            print(f"放入 {BIN_DIR} 目录后，重新运行本命令即可解析。")
            print("=" * 60)
            sys.exit(1)
        print("=" * 60)

    # Windows 宿主运行前确保依赖 dll 存在
    if not ensure_win_dlls():
        print("=" * 60)
        print(f"Windows 运行依赖 dll 下载失败，请从构建仓库 build/ 目录手动下载：")
        print(f"  {' / '.join(WIN_RUN_DLLS)}")
        print(f"  放入 {BIN_DIR} 目录后，重新运行本命令即可解析。")
        print("=" * 60)
        sys.exit(1)

    # execute blutter
    cmd = [input.blutter_file, '-i', input.libapp_path, '-o', input.outdir]
    if input.blacklist:
        cmd += ['--blacklist', input.blacklist]
    subprocess.run(cmd, check=True)

def main_no_flutter(libapp_path: str, dart_version: str, outdir: str, no_analysis: bool, blacklist: str = None, rebuild: bool = False):
    version, os_name, arch = dart_version.split('_')
    if os_name not in ('android', 'windows'):
        print(f"错误：不支持的目标系统 '{os_name}'。仅支持 android(arm64) 与 windows(x64)，不适配 iOS/macOS。")
        sys.exit(1)
    if os_name == 'windows':
        # Flutter Windows 桌面解析：需要 app.so 与 flutter_windows.dll（引擎 dll）同时存在
        # 典型 release 目录结构：
        #   应用根目录/flutter_windows.dll
        #   应用根目录/data/app.so
        if not os.path.isfile(libapp_path):
            print(f"错误：找不到 app.so 文件: {libapp_path}")
            print("请将参数改为 Flutter Windows 桌面应用 release 包中的 data/app.so 文件路径。")
            sys.exit(1)
        app_dir = os.path.dirname(os.path.abspath(libapp_path))
        dll_candidates = [
            os.path.join(app_dir, 'flutter_windows.dll'),                       # data/ 同目录
            os.path.join(os.path.dirname(app_dir), 'flutter_windows.dll'),      # 应用根目录
        ]
        dll_path = next((c for c in dll_candidates if os.path.isfile(c)), None)
        if dll_path is None:
            print("错误：解析 Flutter Windows 桌面应用需要 flutter_windows.dll（引擎 dll）与 app.so 同时存在。")
            print("请确认该 dll 位于以下任一位置：")
            for c in dll_candidates:
                print(f"  {c}")
            print("典型 release 包结构：")
            print("  <应用根目录>/flutter_windows.dll")
            print("  <应用根目录>/data/app.so")
            sys.exit(1)
        print(f"找到 flutter_windows.dll: {dll_path}")
    dart_info = DartLibInfo(version, os_name, arch)
    input = BlutterInput(libapp_path, dart_info, outdir, no_analysis, blacklist, rebuild=rebuild)
    build_and_run(input)
    
def main2(libapp_path: str, libflutter_path: str, outdir: str, no_analysis: bool, blacklist: str = None, rebuild: bool = False):
    dart_info = get_dart_lib_info(libapp_path, libflutter_path)
    input = BlutterInput(libapp_path, dart_info, outdir, no_analysis, blacklist, rebuild=rebuild)
    build_and_run(input)

def main(indir: str, outdir: str, no_analysis: bool, blacklist: str = None, rebuild: bool = False):
    if indir.endswith(".apk"):
        with tempfile.TemporaryDirectory() as tmp_dir:
            libapp_file, libflutter_file = extract_libs_from_apk(indir, tmp_dir)
            main2(libapp_file, libflutter_file, outdir, no_analysis, blacklist, rebuild=rebuild)
        return

    if os.path.isdir(indir):
        # Android 目录布局（libapp.so / libflutter.so 为强特征）
        if (os.path.isfile(os.path.join(indir, 'libapp.so'))
                or os.path.isfile(os.path.join(indir, 'libflutter.so'))):
            libapp_file, libflutter_file = find_lib_files(indir)
            main2(libapp_file, libflutter_file, outdir, no_analysis, blacklist, rebuild=rebuild)
            return

    # 自动检测 Flutter Windows 桌面目标：flutter_windows.dll 为强特征
    wt = detect_windows_target(indir)
    if wt is not None:
        app_file, dll_file = wt
        print("检测到 Flutter Windows 桌面目标")
        print(f"  app.so: {app_file}")
        print(f"  flutter_windows.dll: {dll_file}")
        dart_version, snapshot_hash, flags, arch, os_name = extract_windows_dart_info(app_file, dll_file)
        print(f'Dart version: {dart_version}, Snapshot: {snapshot_hash}, Target: {os_name} {arch}')
        print('flags: ' + ' '.join(flags))
        # Windows 构建产物统一不带 _no-compressed-ptrs 后缀（与手动 --dart-version 模式一致）
        dart_info = DartLibInfo(dart_version, os_name, arch, snapshot_hash=snapshot_hash)
        input = BlutterInput(app_file, dart_info, outdir, no_analysis, blacklist, rebuild=rebuild)
        build_and_run(input)
        return

    if os.path.isdir(indir):
        libapp_file, libflutter_file = find_lib_files(indir)
        main2(libapp_file, libflutter_file, outdir, no_analysis, blacklist, rebuild=rebuild)
        return

    print(f"错误：无法识别 {indir} 的目标类型。")
    print("支持的输入：")
    print("  - APK 文件（Android）")
    print("  - 含 libapp.so + libflutter.so 的目录（Android）")
    print("  - Flutter Windows 桌面应用目录（data/app.so + flutter_windows.dll），或 app.so 文件路径")
    sys.exit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog='B(l)utter',
        description='Reversing a flutter application tool')
    parser.add_argument('indir', help='An apk (Android), or a directory with libapp.so+libflutter.so (Android), or a Flutter Windows app (data/app.so + flutter_windows.dll) / app.so file path')
    parser.add_argument('outdir', help='An output directory')
    parser.add_argument('--no-analysis', action='store_true', default=False, help='Select the no-analysis binary variant')
    parser.add_argument('--rebuild', action='store_true', default=False, help='Force rebuilding the blutter executable from source (requires a full repo checkout with scripts/build.py + blutter/)')
    parser.add_argument('--blacklist', help='Semantic-clue blacklist file passed to blutter (--blacklist <file>); overrides the built-in default. $BLUTTER_BLACKLIST also works')
    # rare usage scenario
    parser.add_argument('--dart-version', help='Run without libflutter (indir become libapp.so) by specify dart version such as "3.4.2_android_arm64"')
    args = parser.parse_args()

    if args.dart_version is None:
        main(args.indir, args.outdir, args.no_analysis, args.blacklist, args.rebuild)
    else:
        main_no_flutter(args.indir, args.dart_version, args.outdir, args.no_analysis, args.blacklist, args.rebuild)
