# Blutter 构建工作流

基于 GitHub Actions 自动构建 [blutter](https://github.com/worawit/blutter) 二进制的仓库，支持 Linux（aarch64）与 Windows（x64）两类宿主编译环境、单版本构建与多版本批量构建。

源码合并自 [1903247335/blutter-windows](https://github.com/1903247335/blutter-windows) 的 Flutter Windows (x64) 支持：除 Android arm64 快照解析（保持兼容）外，还可分析 Flutter Windows 桌面应用的 `data/app.so`（x64）。`scripts/build.py` 的目标拆分为「产物架构」与「解析架构」两维：`--arch aarch64`（安卓，压缩指针）、`--arch windows_x64`（Windows 宿主解析 x64）、`--arch x86_64`（Linux x64 宿主解析 x64）。

## 目标范围

支持 **Android arm64** 与 **Flutter Windows 桌面 x64** 两类目标。不适配 iOS / macOS：Mach-O 解析、iOS 目录布局、Darwin 宿主编译等代码已整体移除，CI 矩阵与新增功能均不引入。

## 产物

| 文件 | 宿主平台 | 说明 |
|------|---------|------|
| `blutter_dartvm<ver>_android_arm64_22` / `_24` | Linux aarch64 | 解析安卓，按 Ubuntu 22.04 / 24.04 区分（单版本与批量构建产物命名一致） |
| `blutter_dartvm<ver>_android_arm64_win.exe` | Windows x64 | Windows 下解析 Android ARM64 快照 |
| `blutter_dartvm<ver>_windows_x64_win.exe` | Windows x64 | Windows 下分析 Flutter Windows 桌面 `app.so`（对象池 + 汇编注释 + IDA 脚本 + `blutter_frida_windows.js`） |

可选上传内容：
- **packages 目录**（Dart VM 头文件 + 静态库）：勾选 `Upload packages` 时打包上传（Linux 为 `.zip`，Windows 为 `_win.zip`）
- **Windows dll**（`capstone.dll` / `icuuc73.dll` / `icudt73.dll`）：勾选 `Upload dlls` 时打包到 Artifacts（不进 Release）；也可由定制版 blutter.py 运行时自动下载

## 用法

### 1. 单版本构建

Actions → **构建 Blutter（单版本）** → Run workflow。参数：

- **Dart version**：单个版本，如 `3.3.4`
- **构建目标**（四选一，默认 `ubuntu_22`）：
  - `windows_android`：Windows 解析安卓，产出 `blutter_dartvm<ver>_android_arm64_win.exe`
  - `windows_windows`：Windows 解析 Windows（Flutter Windows 桌面 `app.so`），产出 `blutter_dartvm<ver>_windows_x64_win.exe`
  - `ubuntu_22`：解析安卓（Ubuntu 22.04，与手机 Droidspaces 环境一致），产出 `blutter_dartvm<ver>_android_arm64_22`
  - `ubuntu_24`：解析安卓（Ubuntu 24.04），产出 `blutter_dartvm<ver>_android_arm64_24`
- **Upload packages / Upload release / Upload dlls**：均为可选项；Release 默认不上传，勾选后产物才进入 Release，否则仅以 Actions Artifacts 形式提供（dll 仅 `windows_android` 目标生效，始终只进 Artifacts）

### 2. 批量构建

Actions → **批量构建 Blutter** → Run workflow。参数：**Dart versions**（逗号分隔版本列表）+ **构建目标**（四选一，与单版本构建一致：`windows_android` / `windows_windows` / `ubuntu_22` / `ubuntu_24`）。一次处理多个版本，分割成最多 20 个 worker 并行构建，并支持增量补齐：某版本该目标产物已存在则自动跳过，缺失才构建发布。产物同时上传 GitHub Release 并后台同步到 Gitee 镜像，不同目标的产物可并存于同一 Release。

### 3. 获取待构建版本

Actions → **获取待构建 Dart 版本**，运行后从日志末尾复制待构建版本列表，填入批量构建输入框。

## 定制版 blutter（`定制版blutter.zip`）

精简运行包，解压后运行 `python3 blutter.py <apk/lib目录/app目录或app.so> <输出目录>`。自动检测目标类型（Android / Flutter Windows 桌面）与 Dart 版本，从仓库 Releases 下载匹配二进制（Linux 自动识别 `_22`/`_24`，Windows 下载 `_win.exe`，Windows 下首次运行自动补齐三个运行 dll）。下载源按国内/国外自动选择（Gitee 镜像 / GitHub 双源，失败自动切换），也可手动下载二进制放入 `$HOME/blutter/bin/`。

- `--blacklist <file>`：语义黑名单文件透传给二进制（覆盖内置默认，`$BLUTTER_BLACKLIST` 环境变量同样生效）
- iOS / macOS 输入直接报错拒绝（`App`/Mach-O 布局、`--dart-version <ver>_ios_*` 均已移除）
- 解析产物含 `asm/`（带 `// semantic:` 注释）、`objs.txt`/`pp.txt`、`strings_to_funcs.txt` 交叉表、`ida_script/`（`addNames.py` 语义重命名 + `semantic_names.txt` 追踪表）；Frida 动态 dump 脚本按目标生成：Android arm64 → `blutter_frida.js`，Flutter Windows 桌面 x64 → `blutter_frida_windows.js`（非压缩指针、栈参数读取、多锚点 base 发现；锚点阈值与运行时行为需在真实 Windows+Frida 环境复核）

## 构建目标

| 选项 | runner | 特点 |
|------|--------|------|
| `windows_android` | `windows-latest` | MSVC x64，解析安卓，产出 `_android_arm64_win.exe`（ICU + capstone win64 自动下载） |
| `windows_windows` | `windows-latest` | MSVC x64，分析 Flutter Windows 桌面 `app.so`，产出 `_windows_x64_win.exe` |
| `ubuntu_22`（默认） | `ubuntu-22.04-arm` | 与 Droidspaces 环境一致，自动装 gcc-13 |
| `ubuntu_24` | `ubuntu-24.04-arm` | 系统自带 gcc-13 |

建议 Linux 默认 `ubuntu_22`，产物动态库版本与运行环境直接匹配。

## 语义黑名单配置（可选）

解析时 blutter 会收集语义线索（`// semantic:` 注释）并给混淆函数生成可读的 `fn_` 名称（写入 `ida_script/addNames.py`）。候选过滤用的三张黑名单（`call:` 调用线索 / `type:` 类型线索 / `name:` 命名候选）已外置为文本文件，加词无需重编译：

```text
# 文件：blutter/src/semantic_blacklist.txt
call:_fw::call          # 逐条精确匹配，'#' 为注释
type:String
name:dart_ui
```

加载优先级：`--blacklist <file>` 命令行参数 > `$BLUTTER_BLACKLIST` 环境变量 > 编译期内置的默认文件 > 代码内置默认。指定的文件可读时**整体替换**内置默认（注释掉的条目即被禁用）；不可读或缺省时回退内置默认。单次运行可对二进制直接传参：

```bash
# 自定义黑名单（覆盖默认）
blutter_dartvm3.3.4_android_arm64 --blacklist /path/to/my.txt -i libapp.so -o out
```

## 其他

- **版本兼容**：与官方 blutter 支持的 Dart 版本一致
- **输出目录**：运行 blutter 后生成 `asm/`（反汇编）、`objs.txt` / `pp.txt`（Object Pool 转储）；`blutter_frida.js`（Frida 脚本，Android 手机端 hook 用）仅解析安卓的目标生成，解析 Windows 桌面 `app.so` 的目标不生成
