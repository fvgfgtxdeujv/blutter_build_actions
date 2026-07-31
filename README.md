# Blutter Build Actions

GitHub Actions workflow for building [blutter](https://github.com/worawit/blutter) binaries.

## 产物

每次 Release 包含以下内容：

### 1. Blutter 可执行文件

| 文件 | 架构 | 说明 |
|------|------|------|
| `bin/blutter_dartvm<ver>_android_arm64` | aarch64 | 交叉编译，适用于 Termux 等 Android ARM64 环境 |

### 2. Dart VM 开发包

| 目录 | 内容 |
|------|------|
| `packages/include/dartvm<ver>_*` | Dart VM 头文件（API、运行时、编译器相关） |
| `packages/lib/` | Dart VM 静态库 (`libdartvm*.a`) + CMake 配置文件 |

## 用法

### 方式一：直接使用编译好的二进制

1. Fork 本仓库到你的 GitHub
2. 进入仓库 → Actions → "Build Blutter Binary" → **Run workflow**
3. 填写 **Dart version**（如 `3.3.4`、`3.4.2`）
4. 编译完成后从 Release 下载对应版本的压缩包
5. 解压后得到：
   - `bin/blutter_dartvm<ver>_android_arm64` - 可执行文件
   - `packages/` - Dart VM 开发包

### 方式二：本地编译（需要官方 blutter 项目）

下载 Release 中的 `packages/` 目录到官方 blutter 项目，然后使用官方脚本自动构建：

```bash
# 将 packages 解压到 blutter 项目根目录
python3 blutter.py path/to/app/lib/arm64-v8a out_dir
```

## 架构支持

- **aarch64**：在 ARM64 Ubuntu runner 上原生编译（使用 `ubuntu-24.04-arm`）

## 版本兼容性

支持 Dart 2.15+ 版本，包括：
- 自动检测版本兼容性宏（`OLD_MAP_SET_NAME`、`HAS_TYPE_REF` 等）
- 支持压缩/非压缩指针模式
- 自动生成对应的 Frida 脚本模板

## 依赖

构建过程自动安装以下依赖：
- CMake
- Ninja
- Clang/LLVM
- libcapstone-dev（反汇编库）
- libicu-dev（国际化库）
- CCache（编译缓存加速）

## 输出文件说明

运行 blutter 后，输出目录包含：

| 文件 | 说明 |
|------|------|
| `asm/*` | libapp 反汇编文件（含符号信息） |
| `blutter_frida.js` | Frida 脚本模板 |
| `objs.txt` | Object Pool 中的对象完整转储 |
| `pp.txt` | Object Pool 中所有 Dart 对象 |
