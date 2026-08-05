# Blutter 构建工作流

用于构建 [blutter](https://github.com/worawit/blutter) 二进制文件的 GitHub Actions 工作流，支持单版本构建与多版本批量构建。

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

### 1. 构建 Blutter（单版本）

构建单个 Dart 版本的 blutter 二进制。

1. 进入仓库 → Actions → **构建 Blutter（单版本）** → **Run workflow**
2. 填写参数：
   - **Dart version**（必填）：单个版本，如 `3.3.4`
   - **编译环境（Ubuntu 版本）**（下拉选择）：`22.04` 或 `24.04`，默认 `22.04`（与手机 Droidspaces 环境一致）
   - **Upload packages**（可选）：是否上传 Dart VM 开发包（默认不上传）
   - **Upload release**（可选）：是否上传到 GitHub Release（默认开启）；关闭时构建产物仅以「构建产物」（Actions Artifacts）形式提供
3. 编译完成后从对应 Release 或构建产物中下载文件

### 2. 批量构建 Blutter

一次处理多个 Dart 版本，在所选编译环境下并行构建。版本列表被分割成最多 **20 个 worker** 同时编译以加快速度，并支持**增量补齐**：某版本的该环境产物已存在则自动跳过，缺失才构建发布。

1. 进入仓库 → Actions → **批量构建 Blutter** → **Run workflow**
2. 填写参数：
   - **Dart versions**（必填）：逗号分隔的版本列表，如 `3.3.4, 3.4.2, 3.5.2` 或 `[3.3.4, 3.4.2]`
   - **编译环境（Ubuntu 版本）**（下拉选择）：`22.04` 或 `24.04`，默认 `22.04`（与手机 Droidspaces 环境一致）
3. 编译完成后从各版本对应的 Release 下载文件

每个版本 Release 包含两个二进制（通过两次运行分别补齐）：

| 文件 | 编译环境 |
|------|----------|
| `blutter_dartvm<ver>_android_arm64_22` | Ubuntu 22.04（与手机 Droidspaces 环境一致） |
| `blutter_dartvm<ver>_android_arm64_24` | Ubuntu 24.04 |

**并行机制**：先由 `prepare` 主 worker 解析版本列表，并检查每个版本在本环境（所选 Ubuntu 版本）的 Release 中是否已有对应后缀产物——已有则直接从编译列表排除；剩余版本再计算 worker 数（少于 20 时每版本一个 worker，最多 20 个），按列表顺序轮流分片给各 worker 并行编译。例如 10 个待构建版本在 22.04 下就是 10 个 job 并行编译（`build-dart-version` 界面中「构建 Worker 1~10」），已存在的版本不会占用 worker。

**增量补齐逻辑**：主 worker 检查时，某版本本环境后缀产物已存在即跳过（不构建不发布）；缺失才纳入编译。例如选择 `24.04` 时，已有 `_22` 的版本会补上 `_24`，已有 `_24` 的版本会排除；选择 `22.04` 时对称。单个版本构建失败自动跳过，不影响其他版本。

**构建机制**：编译直接在所选环境对应的 GitHub 托管 arm64 runner（`ubuntu-22.04-arm` / `ubuntu-24.04-arm`）上原生执行；22.04 自动安装 gcc-13/g++-13，24.04 使用系统自带 gcc-13。Release 已存在时自动追加上传。

### 3. 获取待构建 Dart 版本

查询所有 Dart SDK 版本中尚未构建发布的部分，避免手动遗漏。

1. 进入仓库 → Actions → **获取待构建 Dart 版本** → **Run workflow**
2. 运行结束后，日志末尾会输出待构建版本列表（JSON 数组）
3. 复制该列表，填入「批量构建 Blutter」的 **Dart versions** 输入框手动触发

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

## 定制版 blutter（Android 手机端）

仓库根目录的 `定制版blutter.zip` 是精简后的手机端运行包，包含：

| 文件 | 说明 |
|------|------|
| `blutter.py` | 入口脚本（自动检测 Dart 版本，检查 bin 目录） |
| `scripts/frida.template.js` | 生成 Frida 脚本的模板 |
| `README.md` | 使用说明 |

### 安装

1. 下载 `定制版blutter.zip`，解压到手机 Droidspaces 容器的用户目录：

   ```bash
   unzip 定制版blutter.zip -d ~/blutter
   ```

2. 安装运行依赖（Droidspaces Ubuntu 容器内）：

   ```bash
   pip install pyelftools requests
   ```

3. 运行 blutter 需要各 Dart 版本的二进制，从本仓库 Releases 下载后放入 `$HOME/blutter/bin/`。

### 使用流程

1. 运行解析（支持 APK 或已解压的 lib 目录）：

   ```bash
   python3 blutter.py xxx.apk out
   ```

   ```bash
   python3 blutter.py path/to/lib/arm64-v8a out
   ```

2. 脚本自动检测 Dart 版本，并在 `$HOME/blutter/bin/` 查找对应版本的二进制：
   - **存在**：直接解析，输出到 `out` 目录
   - **不存在**：自动下载匹配版本（自动识别系统为 22.04 还是 24.04，优先下载带 `_22` / `_24` 后缀的二进制并重命名为无后缀格式放入 bin），随后自动继续解析；若下载失败则打印所需版本号，提示手动下载

下载源自动选择（国内 / 国外）：脚本综合 **系统语言**（是否中文环境）、**系统时区**（是否 `Asia/Shanghai` 等国内时区）、**GitHub 延迟**（`curl` 实测 `https://github.com` 响应时间）三个信号判断网络区域，国内信号 ≥ 2 判定为国内。判定国内时优先从 Gitee 镜像 `qeruiop_admin/blutter_build_actions` 下载，否则优先从 GitHub `fvgfgtxdeujv/blutter_build_actions` 下载；主源失败自动切换到备选源。

3. 若自动下载失败，从 Releases 手动下载对应版本二进制放入 `$HOME/blutter/bin/`，重新运行命令即可解析。

二进制命名格式：`blutter_dartvm<版本>_android_arm64`，例如 Dart 3.4.2 对应 `blutter_dartvm3.4.2_android_arm64`。批量构建的产物带环境后缀（`_22` / `_24`），手机 Droidspaces 为 Ubuntu 22.04，建议下载 `blutter_dartvm3.4.2_android_arm64_22`，放入 bin 目录后重命名为 `blutter_dartvm3.4.2_android_arm64`。

### Frida 模板定位

运行 blutter 需要 `scripts/frida.template.js`（定制版运行包已包含，即 `~/blutter/scripts/frida.template.js`）。二进制按以下顺序自动定位模板：

1. 可执行文件同目录下的 `scripts/frida.template.js`
2. 可执行文件上一级目录的 `scripts/frida.template.js`
3. 当前工作目录下的 `scripts/frida.template.js`

## 构建环境

编译在 GitHub 托管的 arm64 runner（`ubuntu-22.04-arm` / `ubuntu-24.04-arm`）上原生执行，无需 Docker。每个 workflow 提供「编译环境（Ubuntu 版本）」下拉选择框：

| 选项 | runner | 特点 |
|------|--------|------|
| `22.04`（默认） | `ubuntu-22.04-arm` | 与 Android 手机 Droidspaces 容器的 Ubuntu 22.04 rootfs 一致；自动安装 gcc-13/g++-13 |
| `24.04` | `ubuntu-24.04-arm` | 系统自带 gcc-13，无需额外工具链 |

> 建议默认使用 `22.04`：编译环境与产物运行环境（Droidspaces Ubuntu 22.04 rootfs）完全一致，动态链接的库版本直接匹配。

| 项 | 说明 |
|----|------|
| 基础系统 | Ubuntu 22.04（默认）或 24.04（可选） |
| 编译器 | gcc-13/g++-13；22.04 由 `ppa:ubuntu-toolchain-r/test` 安装，24.04 系统自带 |
| libstdc++ | gcc-13 提供（22.04 默认 GCC 11 不含 `<format>`，必需） |
| ICU | 动态链接系统 `libicuuc.so`（22.04 自带 libicu70，与运行环境一致） |
| capstone | 动态链接系统 `libcapstone.so`（运行环境需安装 libcapstone） |

## 依赖

runner 启动后自动安装以下依赖（`22.04` 额外安装 gcc-13 工具链）：
- CMake / Ninja
- libcapstone-dev（反汇编库）
- libicu-dev（国际化库）
- CCache（编译缓存加速）
- Python3 / pip / pyelftools / requests

## 输出文件说明

运行 blutter 后，输出目录包含：

| 文件 | 说明 |
|------|------|
| `asm/*` | libapp 反汇编文件（含符号信息） |
| `blutter_frida.js` | Frida 脚本模板 |
| `objs.txt` | Object Pool 中的对象完整转储 |
| `pp.txt` | Object Pool 中所有 Dart 对象 |
