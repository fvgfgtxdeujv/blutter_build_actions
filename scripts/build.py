#!/usr/bin/env python3
"""
Cross-compile blutter for ARM64 (Android)
Called from GitHub Actions workflow
"""
import os
import sys
import subprocess
import shutil
import tempfile
import zipfile
import argparse
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
PROJECT_DIR = SCRIPT_DIR.parent
CMAKE_CMD = "cmake"
NINJA_CMD = "ninja"
GIT_CMD = "git"
CROSS_TRIPLE = "aarch64-linux-gnu"

SDK_DIR = PROJECT_DIR / "dartsdk"
BUILD_DIR = PROJECT_DIR / "build"
PKG_LIB_DIR = PROJECT_DIR / "packages" / "lib"
PKG_INC_DIR = PROJECT_DIR / "packages" / "include"
BIN_DIR = PROJECT_DIR / "bin"
CMAKE_TEMPLATE = SCRIPT_DIR / "scripts" / "CMakeLists.txt"
CREATE_SRCLIST = SCRIPT_DIR / "scripts" / "dartvm_create_srclist.py"
DART_GIT_URL = "https://github.com/dart-lang/sdk.git"


def run(cmd, **kwargs):
    """Run command and check return code"""
    print(f">>> {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        print(f"ERROR: Command failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(1)
    return result


def setup_cross_compiler():
    """Install clang and build ICU from source for aarch64"""
    print("[*] Setting up clang cross-compiler...")

    # Install clang (no need for gcc-aarch64-linux-gnu)
    run(["sudo", "apt-get", "update", "-qq"])

    # 使用阿里云镜像解决 arm64 仓库 404 问题
    run(["sudo", "sed", "-i",
         "s|http://archive.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g",
         "/etc/apt/sources.list"])
    run(["sudo", "sed", "-i",
         "s|http://security.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g",
         "/etc/apt/sources.list"])
    run(["sudo", "apt-get", "update", "-qq"])

    run(["sudo", "apt-get", "install", "-y", "--no-install-recommends",
         "clang", "libicu-dev:arm64"])

    # Verify arm64 ICU is installed
    result = subprocess.run(
        ["dpkg", "-s", "libicu-dev:arm64"],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("[!] Failed to install libicu-dev:arm64, falling back to source build")

    # Build ICU from source for aarch64
    print("[*] Building ICU for aarch64 (this takes ~2 min)...")
    icu_ver = "75.1"
    icu_archive = f"icu4c-{icu_ver.replace('.', '_')}-src.tgz"
    icu_url = f"https://github.com/unicode-org/icu/releases/download/release-{icu_ver}/{icu_archive}"

    run(["wget", "-q", icu_url])
    run(["tar", "xzf", icu_archive])

    icu_src = Path(f"icu/icu4c-{icu_ver.replace('.', '_')}-source")
    build_dir = icu_src / "build-arm64"
    build_dir.mkdir(parents=True, exist_ok=True)

    env = {**os.environ,
           "CC": "clang --target=aarch64-linux-gnu",
           "CXX": "clang++ --target=aarch64-linux-gnu"}

    run(["../source/configure",
         "--host=aarch64-linux-gnu",
         "--prefix=/usr/aarch64-linux-gnu",
         "--disable-shared", "--enable-static",
         "--disable-samples", "--disable-tests"], cwd=build_dir, env=env)
    run(["make", "-j", str(os.cpu_count())], cwd=build_dir, env=env)
    run(["sudo", "make", "install"], cwd=build_dir, env=env)

    # Cleanup source
    shutil.rmtree("icu", ignore_errors=True)
    os.remove(icu_archive)
    print("[+] ICU for aarch64 installed to /usr/aarch64-linux-gnu")


def generate_toolchain_file():
    """Generate CMake toolchain file for aarch64 cross-compilation"""
    print("[*] Generating toolchain file...")
    tc_path = PROJECT_DIR / "cross" / "aarch64-toolchain.cmake"
    tc_path.parent.mkdir(parents=True, exist_ok=True)

    content = f"""set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)

set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_STRIP llvm-strip)
set(CMAKE_OBJCOPY llvm-objcopy)
set(CMAKE_OBJDUMP llvm-objdump)
set(CMAKE_NM llvm-nm)
set(CMAKE_READELF llvm-readelf)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_PREFIX_PATH "/usr/aarch64-linux-gnu")
"""
    tc_path.write_text(content)
    print(f"[+] Toolchain file: {tc_path}")


def clone_dart_sdk(version):
    """Clone Dart SDK at specific version (with sparse checkout)"""
    print(f"[*] Cloning Dart SDK {version}...")
    clone_dir = SDK_DIR / f"v{version}"

    if clone_dir.exists():
        shutil.rmtree(clone_dir)

    run([GIT_CMD, "clone", "-c", "advice.detachedHead=false",
         "-b", version, "--depth", "1",
         "--filter=blob:none", "--sparse",
         DART_GIT_URL, str(clone_dir)])

    run([GIT_CMD, "sparse-checkout", "set",
         "runtime", "tools", "third_party/double-conversion"], cwd=clone_dir)

    # Remove loose files at root
    for f in clone_dir.iterdir():
        if f.is_file():
            f.unlink()

    # Generate version.cc
    run([sys.executable, "tools/make_version.py",
         "--output", "runtime/vm/version.cc",
         "--input", "runtime/vm/version_in.cc"], cwd=clone_dir)

    print(f"[+] Dart SDK cloned to {clone_dir}")


def generate_sources(version):
    """Generate sourcelist.cmake and detect C++ standard"""
    print("[*] Generating source list...")
    clone_dir = SDK_DIR / f"v{version}"

    # Generate sourcelist.cmake
    run([sys.executable, str(CREATE_SRCLIST), str(clone_dir)])

    # Detect C++ standard
    clang_tidy = clone_dir / "runtime" / "tools" / "run_clang_tidy.dart"
    content = clang_tidy.read_text()
    pos = content.find("-std=c++")
    cpp_std = "17" if pos == -1 else content[pos + 8:pos + 10]

    # Write CMakeLists.txt from template
    template = CMAKE_TEMPLATE.read_text()
    cmake_content = template.replace("VERSION_PLACE_HOLDER", version).replace("CXX_STD_PLACE_HOLDER", cpp_std)
    (clone_dir / "CMakeLists.txt").write_text(cmake_content)

    # Write Config.cmake.in
    (clone_dir / "Config.cmake.in").write_text(
        "@PACKAGE_INIT@\n\ninclude(\"${CMAKE_CURRENT_LIST_DIR}/dartvmTarget.cmake\")\n"
    )

    print(f"[+] C++ standard: {cpp_std}")
    return cpp_std


def build_dart_runtime(version, is_aarch64):
    """Build Dart runtime static library"""
    print(f"[*] Building Dart runtime (cross={is_aarch64})...")
    clone_dir = SDK_DIR / f"v{version}"
    dart_lib_name = f"dartvm{version}_android_arm64"
    build_path = BUILD_DIR / dart_lib_name

    build_path.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        CMAKE_CMD, "-GNinja", "-B", str(build_path),
        "-DTARGET_OS=android", "-DTARGET_ARCH=arm64",
        "-DCOMPRESSED_PTRS=1", "-DCMAKE_BUILD_TYPE=Release",
        "--log-level=NOTICE",
        f"-DCMAKE_INSTALL_PREFIX={PROJECT_DIR / 'packages'}",
        str(clone_dir),
    ]

    if is_aarch64:
        tc_file = PROJECT_DIR / "cross" / "aarch64-toolchain.cmake"
        env = {
            **os.environ,
            "CC": "ccache clang",
            "CXX": "ccache clang++",
            "PKG_CONFIG_PATH": "/usr/aarch64-linux-gnu/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig",
        }
        cmake_args += [
            f"-DCMAKE_TOOLCHAIN_FILE={tc_file}",
            "-DICU_ROOT=/usr/aarch64-linux-gnu",
        ]
    else:
        env = {
            **os.environ,
            "CC": "ccache clang",
            "CXX": "ccache clang++",
        }

    run(cmake_args, env=env)
    run([NINJA_CMD], cwd=build_path, env=env)
    run([CMAKE_CMD, "--install", "."], cwd=build_path, env=env)

    print(f"[+] Dart runtime: {PKG_LIB_DIR}/lib{dart_lib_name}.a")


def detect_macros(version):
    """Detect Dart version compatibility macros"""
    print("[*] Detecting compatibility macros...")
    vm_inc = PKG_INC_DIR / f"dartvm{version}" / "vm"
    macros = []

    def check(filepath, pattern, macro):
        p = vm_inc / filepath
        if p.exists() and pattern in p.read_text():
            macros.append(macro)

    def check_not(filepath, pattern, macro):
        p = vm_inc / filepath
        if p.exists() and pattern not in p.read_text():
            macros.append(macro)

    check("class_id.h", "V(LinkedHashMap)", "-DOLD_MAP_SET_NAME=1")
    check_not("class_id.h", "V(ImmutableLinkedHashMap)", "-DOLD_MAP_NO_IMMUTABLE=1")
    check_not("class_id.h", " kLastInternalOnlyCid ", "-DNO_LAST_INTERNAL_ONLY_CID=1")
    check("class_id.h", "V(TypeRef)", "-DHAS_TYPE_REF=1")
    if version.startswith("3."):
        check("class_id.h", "V(RecordType)", "-DHAS_RECORD_TYPE=1")
    check("class_table.h", "class SharedClassTable {", "-DHAS_SHARED_CLASS_TABLE=1")
    check_not("stub_code_list.h", "V(InitLateStaticField)", "-DNO_INIT_LATE_STATIC_FIELD=1")
    check_not("object_store.h", "build_generic_method_extractor_code)", "-DNO_METHOD_EXTRACTOR_STUB=1")
    check_not("object.h", "AsTruncatedInt64Value()", "-DUNIFORM_INTEGER_ACCESS=1")

    print(f"[+] Macros: {' '.join(macros) if macros else 'none'}")
    return macros


def build_blutter_binary(version, is_aarch64, macros):
    """Build blutter executable"""
    print(f"[*] Building blutter binary (cross={is_aarch64})...")
    dart_lib = f"dartvm{version}_android_arm64"
    bin_name = f"blutter_{dart_lib}"
    build_path = BUILD_DIR / bin_name
    build_path.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        CMAKE_CMD, "-GNinja", "-B", str(build_path),
        f"-DDARTLIB={dart_lib}", "-DNAME_SUFFIX=",
        "-DCMAKE_BUILD_TYPE=Release", "--log-level=NOTICE",
        str(PROJECT_DIR / "blutter"),
    ]

    if is_aarch64:
        tc_file = PROJECT_DIR / "cross" / "aarch64-toolchain.cmake"
        env = {
            **os.environ,
            "CC": "ccache clang",
            "CXX": "ccache clang++",
            "PKG_CONFIG_PATH": "/usr/aarch64-linux-gnu/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig",
        }
        cmake_args += [f"-DCMAKE_TOOLCHAIN_FILE={tc_file}"]
    else:
        env = {
            **os.environ,
            "CC": "ccache clang",
            "CXX": "ccache clang++",
        }

    cmake_args.extend(macros)

    run(cmake_args, env=env)
    run([NINJA_CMD], cwd=build_path, env=env)
    run([CMAKE_CMD, "--install", "."], cwd=build_path, env=env)

    output = BIN_DIR / bin_name
    if output.with_suffix(".exe").exists():
        output.with_suffix(".exe").rename(output)

    print(f"\n[+] Built: {output}")
    subprocess.run(["file", str(output)])


def main():
    parser = argparse.ArgumentParser(description="Build blutter for ARM64")
    parser.add_argument("version", help="Dart version (e.g. 3.3.4)")
    parser.add_argument("--arch", choices=["aarch64", "x86_64"], default="aarch64")
    args = parser.parse_args()

    is_aarch64 = args.arch == "aarch64"

    # 1. Setup cross-compiler if needed
    if is_aarch64:
        setup_cross_compiler()
        generate_toolchain_file()

    # 2. Clone Dart SDK
    if not (SDK_DIR / f"v{args.version}" / "runtime" / "vm" / "version.cc").exists():
        clone_dart_sdk(args.version)
    else:
        print(f"[=] Dart SDK {args.version} already cloned")

    # 3. Generate sources
    generate_sources(args.version)

    # 4. Build Dart runtime
    build_dart_runtime(args.version, is_aarch64)

    # 5. Detect macros
    macros = detect_macros(args.version)

    # 6. Build blutter binary
    build_blutter_binary(args.version, is_aarch64, macros)

    print("\n=== Build complete ===")


if __name__ == "__main__":
    main()
