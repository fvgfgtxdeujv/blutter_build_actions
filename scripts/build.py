#!/usr/bin/env python3
"""
Build blutter for ARM64 / x86_64
Called from GitHub Actions workflow
"""
import os
import sys
import platform
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
PKG_DIR = PROJECT_DIR / "packages"
PKG_LIB_DIR = PKG_DIR / "lib"
PKG_INC_DIR = PKG_DIR / "include"
BIN_DIR = PROJECT_DIR / "bin"
CMAKE_TEMPLATE = SCRIPT_DIR / "CMakeLists.txt"
CREATE_SRCLIST = SCRIPT_DIR / "dartvm_create_srclist.py"
DART_GIT_URL = "https://github.com/dart-lang/sdk.git"

HOST_MACHINE = platform.machine()


def is_cross_compile(arch):
    if arch == "aarch64":
        return HOST_MACHINE != "aarch64"
    if arch == "x86_64":
        return HOST_MACHINE not in ("x86_64", "amd64")
    return False


def run(cmd, **kwargs):
    """Run command and check return code"""
    print(f">>> {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        print(f"ERROR: Command failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(1)
    return result


def setup_icu():
    """Install ICU arm64 libs: try apt first, fallback to local .deb"""
    print("[*] Installing libicu-dev:arm64...")

    result = subprocess.run(
        ["sudo", "apt-get", "install", "-y", "--no-install-recommends",
         "libicu-dev:arm64"],
        capture_output=True
    )
    if result.returncode == 0:
        print("[+] libicu-dev:arm64 installed via apt")
        return

    print("[!] apt install failed, falling back to local .deb download")
    target_dir = "/usr/aarch64-linux-gnu"
    run(["sudo", "mkdir", "-p", f"{target_dir}/include", f"{target_dir}/lib"])

    _install_arm64_deb_to_sysroot(
        pkg="libicu-dev",
        base_url="https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/pool/main/i/icu",
        lib_name="icu",
        include_dirs=["unicode"],
        runtime_pkg_prefix="libicu",
    )
    print(f"[+] ICU libs installed to {target_dir}")


def _install_arm64_deb_to_sysroot(pkg, base_url, lib_name, include_dirs,
                                   runtime_pkg=None, runtime_pkg_prefix=None):
    print(f"[*] Installing {pkg}:arm64 to sysroot...")
    ver = subprocess.run(
        ["dpkg-query", "-W", "-f=${Version}", pkg],
        capture_output=True, text=True
    ).stdout.strip()

    target = "/usr/aarch64-linux-gnu"
    deb_files = [f"{pkg}_{ver}_arm64.deb"]

    if runtime_pkg:
        deb_files.append(f"{runtime_pkg}_{ver}_arm64.deb")
    elif runtime_pkg_prefix:
        major = ver.split(".")[0]
        deb_files.append(f"{runtime_pkg_prefix}{major}_{ver}_arm64.deb")

    for deb_name in deb_files:
        local = PROJECT_DIR / deb_name
        if local.exists():
            run(["cp", str(local), f"/tmp/{deb_name}"])
        else:
            run(["wget", "-q", f"{base_url}/{deb_name}", "-O", f"/tmp/{deb_name}"])

    extract_dir = f"/tmp/{lib_name}-arm64-dev"
    run(["dpkg", "-x", f"/tmp/{deb_files[0]}", extract_dir])

    for d in include_dirs:
        run(["sudo", "cp", "-a", f"{extract_dir}/usr/include/{d}", f"{target}/include/"])

    dev_lib = Path(extract_dir) / "usr" / "lib" / "aarch64-linux-gnu"
    if dev_lib.exists():
        for f in dev_lib.glob(f"lib{lib_name}*"):
            run(["sudo", "cp", "-a", str(f), f"{target}/lib/"])

    for deb_name in deb_files[1:]:
        run(["dpkg", "-x", f"/tmp/{deb_name}", extract_dir])
        rt_lib = Path(extract_dir) / "usr" / "lib" / "aarch64-linux-gnu"
        if rt_lib.exists():
            for f in rt_lib.glob(f"lib{lib_name}*"):
                run(["sudo", "cp", "-a", str(f), f"{target}/lib/"])

    run(["rm", "-rf", extract_dir, *[f"/tmp/{d}" for d in deb_files]])


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
set(CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN /usr)
set(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN /usr)

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


def fix_python312_compatibility(clone_dir):
    """Fix Python 3.12 compatibility for old Dart versions"""
    utils_path = clone_dir / "tools" / "utils.py"
    if not utils_path.exists():
        return
    
    # Read file in binary mode first to check content
    content = utils_path.read_text(encoding='utf-8')
    original_content = content
    
    # Replace imp module (old Dart versions)
    if "import imp" in content:
        content = content.replace(
            "import imp",
            "import importlib.util\nimport importlib.machinery\n\ndef load_source(modname, filename):\n    loader = importlib.machinery.SourceFileLoader(modname, filename)\n    spec = importlib.util.spec_from_file_location(modname, filename, loader=loader)\n    module = importlib.util.module_from_spec(spec)\n    loader.exec_module(module)\n    return module\n"
        )
        content = content.replace("imp.load_source", "load_source")
    
    # Fix invalid escape sequences (Python 3.12+)
    # Only fix non-regex strings, leave \\d patterns intact
    content = content.replace(" ' awk ", " r' awk ")
    
    if content != original_content:
        utils_path.write_text(content)
        print("[+] Python 3.12 compatibility fixed")


def clone_dart_sdk(version):
    """Clone Dart SDK at specific version"""
    print(f"[*] Cloning Dart SDK {version}...")
    clone_dir = SDK_DIR / f"v{version}"

    if clone_dir.exists():
        shutil.rmtree(clone_dir)

    # Full clone with complete history to avoid version generation issues
    run([GIT_CMD, "clone", "-c", "advice.detachedHead=false",
         "-b", version,
         DART_GIT_URL, str(clone_dir)])

    # Remove unnecessary directories to save space (keep runtime, tools, third_party/double-conversion)
    for dir_name in [".git", ".github", "CHANGELOG.md", "CONTRIBUTING.md", 
                     "README.md", "analysis_options.yaml", "bin", "docs", "examples",
                     "pkg", "samples", "tests"]:
        target = clone_dir / dir_name
        if target.exists():
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()
    
    # Remove third_party except double-conversion
    third_party = clone_dir / "third_party"
    if third_party.exists():
        for item in third_party.iterdir():
            if item.name != "double-conversion":
                if item.is_dir():
                    shutil.rmtree(item)
                else:
                    item.unlink()

    # Fix Python 3.12 compatibility for old Dart versions
    fix_python312_compatibility(clone_dir)

    # Generate version.cc directly (bypass make_version.py issues)
    version_cc = clone_dir / "runtime" / "vm" / "version.cc"
    version_cc.write_text('''// Copyright (c) 2012, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/version.h"
#include "vm/globals.h"

namespace dart {

const char* Version::String() {
  return str_;
}

const char* Version::SnapshotString() {
  return snapshot_hash_;
}

const char* Version::CommitString() {
  return commit_;
}

const char* Version::SdkHash() {
  return git_short_hash_;
}

const char* Version::Channel() {
  return channel_;
}

const char* Version::snapshot_hash_ = "";
const char* Version::str_ = "''' + version + ''' (stable) on "' kHostOperatingSystemName '_' kTargetArchitectureName '"';
const char* Version::commit_ = "''' + version + '''";
const char* Version::git_short_hash_ = "";
const char* Version::channel_ = "stable";

}  // namespace dart
''')

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


ARCH_MAP = {
    "aarch64": {"os": "android", "arch": "arm64", "compressed_ptrs": True},
    "x86_64":  {"os": "linux",   "arch": "x64",   "compressed_ptrs": False},
}


def _arch_suffix(arch):
    a = ARCH_MAP[arch]
    return f"{a['os']}_{a['arch']}"


def _dart_lib_name(version, arch):
    return f"dartvm{version}_{_arch_suffix(arch)}"


def build_dart_runtime(version, arch):
    """Build Dart runtime static library"""
    cross = is_cross_compile(arch)
    a = ARCH_MAP[arch]
    print(f"[*] Building Dart runtime (target={a['os']}/{a['arch']}, cross={cross})...")
    clone_dir = SDK_DIR / f"v{version}"
    dart_lib_name = _dart_lib_name(version, arch)
    build_path = BUILD_DIR / dart_lib_name

    build_path.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        CMAKE_CMD, "-GNinja", "-B", str(build_path),
        f"-DTARGET_OS={a['os']}", f"-DTARGET_ARCH={a['arch']}",
        f"-DCOMPRESSED_PTRS={1 if a['compressed_ptrs'] else 0}",
        "-DCMAKE_BUILD_TYPE=Release",
        "--log-level=NOTICE",
        f"-DCMAKE_INSTALL_PREFIX={PROJECT_DIR / 'packages'}",
        str(clone_dir),
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
    ]

    env = os.environ.copy()

    if cross:
        tc_file = PROJECT_DIR / "cross" / "aarch64-toolchain.cmake"
        env["PKG_CONFIG_LIBDIR"] = "/usr/aarch64-linux-gnu/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig"
        cmake_args += [
            f"-DCMAKE_TOOLCHAIN_FILE={tc_file}",
            "-DICU_ROOT=/usr/aarch64-linux-gnu",
        ]

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


def build_blutter_binary(version, arch, macros):
    """Build blutter executable"""
    cross = is_cross_compile(arch)
    dart_lib = _dart_lib_name(version, arch)
    print(f"[*] Building blutter binary (arch={arch}, cross={cross})...")
    bin_name = f"blutter_{dart_lib}"
    build_path = BUILD_DIR / bin_name
    build_path.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        CMAKE_CMD, "-GNinja", "-B", str(build_path),
        f"-DDARTLIB={dart_lib}", "-DNAME_SUFFIX=",
        "-DCMAKE_BUILD_TYPE=Release", "--log-level=NOTICE",
        str(PROJECT_DIR / "blutter"),
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
    ]

    env = os.environ.copy()

    if cross:
        tc_file = PROJECT_DIR / "cross" / "aarch64-toolchain.cmake"
        env["PKG_CONFIG_LIBDIR"] = "/usr/aarch64-linux-gnu/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig"
        cmake_args += [f"-DCMAKE_TOOLCHAIN_FILE={tc_file}"]

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
    parser = argparse.ArgumentParser(description="Build blutter")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("setup-icu")
    p = sub.add_parser("generate-toolchain")
    p = sub.add_parser("clone-dart")
    p.add_argument("version")
    p = sub.add_parser("generate-sources")
    p.add_argument("version")
    p = sub.add_parser("build-dartvm")
    p.add_argument("version")
    p.add_argument("--arch", choices=["aarch64", "x86_64"], default="aarch64")
    p = sub.add_parser("build-blutter")
    p.add_argument("version")
    p.add_argument("--arch", choices=["aarch64", "x86_64"], default="aarch64")

    args = parser.parse_args()

    if args.command == "setup-icu":
        setup_icu()
    elif args.command == "generate-toolchain":
        generate_toolchain_file()
    elif args.command == "clone-dart":
        if not (SDK_DIR / f"v{args.version}" / "runtime" / "vm" / "version.cc").exists():
            clone_dart_sdk(args.version)
        else:
            print(f"[=] Dart SDK {args.version} already cloned")
    elif args.command == "generate-sources":
        generate_sources(args.version)
    elif args.command == "build-dartvm":
        build_dart_runtime(args.version, args.arch)
    elif args.command == "build-blutter":
        macros = detect_macros(args.version)
        build_blutter_binary(args.version, args.arch, macros)


if __name__ == "__main__":
    main()
