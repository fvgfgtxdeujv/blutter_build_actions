# Blutter Build Actions

GitHub Actions workflow for building [blutter](https://github.com/worawit/blutter) binaries.

## 产物

每次 Release 包含以下内容：

### 1. Blutter 可执行文件

| 文件 | 架构 | 说明 |
|------|------|------|
| `bin/blutter_dartvm<ver>_android_arm64` | aarch64 | 适用于 Android ARM64 环境 |

### 2. Dart VM 开发包（需要勾选 Upload Packages）

| 目录 | 内容 |
|------|------|
| `packages/include/dartvm<ver>_*` | Dart VM 头文件（API、运行时、编译器相关） |
| `packages/lib/` | Dart VM 静态库 (`libdartvm*.a`) + CMake 配置文件 |

## 用法

1. Fork 本仓库到你的 GitHub
2. 进入仓库 → Actions → "Build Blutter Binary" → **Run workflow**
3. 填写参数：
   - **Dart version**（必填）：如 `3.3.4`、`3.4.2`
   - **Upload packages**（可选）：是否上传 Dart VM 开发包（默认不上传）
4. 编译完成后从 Release 或 Artifacts 下载文件

### Upload Packages 选项说明

| 选项 | 上传内容 | 适用场景 | 文件大小 |
|------|----------|----------|----------|
| ❌ 关闭（默认） | 仅 `bin/` 目录 | 只需运行 blutter | ~50 MB |
| ✅ 开启 | `bin/` + `packages/` | 本地开发/调试 | ~500 MB |

### Download Packages 使用场景

**需要 packages 目录：**
- 本地编译/调试 blutter
- 为多个 Dart 版本构建（复用头文件）
- 开发相关工具
- 学习 Dart VM 内部结构

**不需要 packages 目录：**
- 直接使用编译好的 blutter 二进制
- 只需要运行工具分析 APK

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
