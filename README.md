# Blutter 构建工作流

基于 GitHub Actions 自动构建 [blutter](https://github.com/worawit/blutter) 二进制的仓库，同时支持在本地用仓库根的 `blutter.py` 从源码构建；覆盖 Linux（aarch64）与 Windows（x64）两类宿主编译环境、单版本构建与多版本批量构建。

源码合并自 [1903247335/blutter-windows](https://github.com/1903247335/blutter-windows) 的 Flutter Windows (x64) 支持：除 Android arm64 快照解析（保持兼容）外，还可分析 Flutter Windows 桌面应用的 `data/app.so`（x64）。`scripts/build.py` 的目标拆分为「产物架构」与「解析架构」两维：`--arch aarch64`（安卓，压缩指针）、`--arch windows_x64`（Windows 宿主解析 x64）、`--arch x86_64`（Linux x64 宿主解析 x64）。

## 目标范围

支持 **Android arm64** 与 **Flutter Windows 桌面 x64** 两类目标。不适配 iOS / macOS：Mach-O 解析、iOS 目录布局、Darwin 宿主编译等代码已整体移除，CI 矩阵与新增功能均不引入。

## 仓库结构

| 路径 | 说明 |
|------|------|
| `blutter/` | 定制版 blutter C++ 源码（`src/` 平铺；相较官方多出 x64 的 `CodeAnalyzer_x64.cpp`、`Disassembler_x64.cpp/.h`，arm64 分析器保持兼容） |
| `blutter.py` | 运行 / 源码构建入口（`定制版blutter.zip` 内的同名文件即此文件的打包版本） |
| `scripts/build.py` | 构建脚本：`clone-dart` / `generate-sources` / `build-dartvm` / `build-blutter` / `generate-toolchain` / `setup-icu` |
| `scripts/CMakeLists.txt`、`scripts/dartvm_create_srclist.py` | Dart VM 的 CMake 模板与源清单生成 |
| `scripts/frida.template.js`、`scripts/frida.windows.template.js` | 运行时 Frida 脚本模板（Android / Windows 各一） |
| `packages/` | 构建产物：Dart VM 头文件 + 静态库（`find_package` 定位，不入库） |
| `bin/` | blutter 可执行文件输出目录（不入库） |
| `dartsdk/`、`build/`、`cross/` | Dart SDK 检出、构建目录与交叉 toolchain（不入库） |
| `定制版blutter.zip` | 精简运行包（`blutter.py` + Frida 模板 + README / LICENSE / THIRD_PARTY_NOTICES） |
| `THIRD_PARTY_NOTICES.md` | 第三方组件（blutter、blutter-windows、Taywee/args、Dart SDK、Capstone、ICU）的版权与许可声明 |
| `.github/workflows/` | Actions：单版本构建 / 批量构建 / 获取待构建版本 |
| `.monkeycode/` | 规格、文档与记忆 |

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

### 4. 本地从源码构建（不依赖 Actions）

仓库根的 `blutter.py` 默认优先从 Releases 远程下载匹配二进制；下载不可用或失败时，用定制版 `blutter/src` 从源码构建（`--rebuild` 可跳过下载、强制重建）。源码构建内部复用 `scripts/build.py` 的四步流水线（与 `.github/workflows/build-dart-version.yml` 一致）：

```
python3 blutter.py <apk/lib目录/app目录或app.so> <输出目录> [--rebuild]
```

也可只跑构建步骤：

```bash
cd /workspace
python3 scripts/build.py clone-dart 3.3.4
python3 scripts/build.py generate-sources 3.3.4
python3 scripts/build.py build-dartvm 3.3.4 --arch x86_64
python3 scripts/build.py build-blutter 3.3.4 --arch x86_64
```

- `--arch`：`aarch64`（Android arm64）/ `x86_64`（Linux 宿主解析 x64）/ `windows_x64`（Windows 宿主解析 x64）；产物落 `bin/`
- 构建依赖：cmake / ninja / git / clang-16 / libc++-16-dev / libc++abi-16-dev / libcapstone-dev / libicu-dev / ccache。没有 gcc-13（libstdc++ 缺 `std::format`）的发行版必须用 clang-16 + libc++，且 Dart VM 与 blutter 共用同一套 C++ 标准库，避免静态库 ABI 不一致
- aarch64 交叉编译需预先准备 `/usr/aarch64-linux-gnu` sysroot（arm64 libc、ICU 与 aarch64 版 libc++）。Debian/Ubuntu 的 libc++ arm64 与 amd64 包在 `/usr/lib/llvm-*/lib` 共享路径冲突，multiarch 无法并存，Android arm64 建议在 arm64 主机或 CI 的 arm64 runner 上原生构建

## 定制版 blutter（`定制版blutter.zip`）

精简运行包，解压后运行 `python3 blutter.py <apk/lib目录/app目录或app.so> <输出目录>`。自动检测目标类型（Android / Flutter Windows 桌面）与 Dart 版本，在 `$HOME/blutter/bin` 查找匹配二进制；缺失时优先从 Releases 远程下载（Linux 自动识别 `_22`/`_24`，Windows 下载 `_win.exe`，Windows 下首次运行自动补齐三个运行 dll），下载不可用或失败且处于完整源码检出（仓库根同时有 `scripts/build.py` 与 `blutter/`）时，再用定制版 `blutter/src` 从源码构建 dartvm + blutter。下载源按国内/国外自动选择（Gitee 镜像 / GitHub 双源，失败自动切换），也可手动下载二进制放入 `$HOME/blutter/bin/`。

仓库根的 `blutter.py` 即本包的规范来源（打进 zip 的即此文件）：直接在完整源码检出中运行即可走「优先下载、失败再源码构建」的流程。

- `--rebuild`：强制从源码重建 blutter 可执行文件（需完整仓库检出；`--no-analysis` 等无对应构建产物的变体仍走下载）
- `--blacklist <file>`：语义黑名单文件透传给二进制（覆盖内置默认，`$BLUTTER_BLACKLIST` 环境变量同样生效）
- `--no-analysis`：选择 no-analysis 变体产物（该变体无源码构建产物，始终下载）
- `--dart-version <v>_<os>_<arch>`：无 libflutter 时手动指定 Dart 版本，如 `3.4.2_android_arm64`、`3.3.4_windows_x64`（仅支持 android / windows）
- `-p` / `--pseudo`：为 `asm/` 输出追加 `// pseudo:` 伪代码注释段（默认关闭；透传给底层二进制，与 `--blacklist` 相同的传递方式）
- iOS / macOS 输入直接报错拒绝（`App`/Mach-O 布局、`--dart-version <ver>_ios_*` 均已移除）
- 解析产物含 `asm/`（带 `// semantic:` 注释；引用对象池的 IL 行会附上 `; [pp+off] <描述>`，与紧随其后的汇编行池描述一致，如 `[PP+0x75178] = r0  ; [pp+0x75178] IMM: 0x0`、`InitLateStaticField(0x9a4) // ...  ; [pp+0x13f68] Field <...>`）、`objs.txt`/`pp.txt`（实例字段在类元数据可用时标注字段名，形如 `_name (off_8): "value"`，无名字段保持 `off_x: value`；实例类名带定义库前缀，如 `Obj![package:foo/bar.dart] MyClass<int>@addr`，enum 值显示为 `EnumName.value` 而非不透明实例）、`strings_to_funcs.txt` 交叉表、`ida_script/`（`addNames.py` 语义重命名 + `semantic_names.txt` 追踪表）；Frida 动态 dump 脚本按目标生成：Android arm64 → `blutter_frida.js`，Flutter Windows 桌面 x64 → `blutter_frida_windows.js`（非压缩指针、栈参数读取、多锚点 base 发现；锚点阈值与运行时行为需在真实 Windows+Frida 环境复核）

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

## 伪代码输出（可选：`-p` / `--pseudo`）

默认不生成伪代码，`asm/*.dart` 与经典反汇编输出逐字节一致。传入 `-p`（或 `--pseudo`）后，blutter 会在每个函数的汇编体之后追加一段 `// pseudo:` 注释，给出该函数的最佳努力（best-effort）语义视图：

- 丢弃入口/出口样板（EnterFrame / AllocStack / CheckStackOverflow 等）
- 寄存器数据流折叠为表达式（`obj->field_x`、`array[idx]`、`[fp-0x8]` 槽位、`[SP]` 调用参数）
- 字段/数组读写还原为 `obj->field_x = v;` 形式的语句
- 调用点收集栈参数并折叠为 `f(...)` / `return f(...)`
- 条件/无条件跳转标注为 `// if (...) goto 0x...` / `// goto 0x...`

该视图为增量注释：不改动原有汇编行，IL 未识别的指令保留 `// 0x... <asm>` 参考行，信息不丢失；x64 与 arm64 使用同一套折叠规则（按架构适配操作数文本）。表达式长度有上限，避免个别函数出现指数级膨胀。

```bash
# 直接调用二进制：追加伪代码段
blutter_dartvm3.3.4_android_arm64 -i libapp.so -o out -p
```

## 其他

- **版本兼容**：与官方 blutter 支持的 Dart 版本一致
- **解析健壮性**：SIMD typed array（`Float32x4` / `Int32x4` / `Float64x2`）与未处理的内部类不再中止整次解析，分别输出元素值 / `UnhandledClass(name, cid=N)` 占位；手工 cmake 构建会在 `find_package` 后自动探测 `HAS_TYPE_REF` / `HAS_RECORD_TYPE`，避免调用方漏传宏导致 `Invalid abstract type` 中止（`scripts/build.py` 显式传值时以后者为准）
- **输出目录**：运行 blutter 后生成 `asm/`（反汇编；加 `-p` 时每个函数另含 `// pseudo:` 伪代码注释段）、`objs.txt` / `pp.txt`（Object Pool 转储）；`blutter_frida.js`（Frida 脚本，Android 手机端 hook 用）仅解析安卓的目标生成，解析 Windows 桌面 `app.so` 的目标不生成

## 许可证

本仓库以 GNU GPLv3 发布，全文见 `LICENSE`。

仓库源码基于上游 MIT 项目（blutter / blutter-windows）修改而来，并包含 Dart SDK / Dart VM、Taywee/args、Capstone、ICU 等第三方组件；这些组件的版权声明与许可全文按 GPLv3 要求保留在 `THIRD_PARTY_NOTICES.md`。
