# Blutter Build Actions

GitHub Actions workflow for building [blutter](https://github.com/kyoheiu/blutter) binaries via cross-compilation.

## 产物

同时构建两种架构，每次 Release 包含两个二进制文件：

| 文件 | 架构 | 说明 |
|------|------|------|
| `bin/blutter_dartvm<ver>_android_arm64` | aarch64 | 交叉编译，适用于 Termux 等 Android ARM64 环境 |
| `bin/blutter_dartvm<ver>_linux_x64` | x86_64 | 原生编译，适用于 Linux x86_64 环境 |

## 用法

1. Fork 本仓库到你的 GitHub
2. 进入仓库 → Actions → "Build Blutter Binary" → **Run workflow**
3. 填写 **Dart version**（如 `3.3.4`、`3.4.2`）
4. 编译完成后从 Release 或 Artifacts 下载所需架构的二进制文件
5. 放到 blutter 项目的 `bin/` 目录直接运行

## 原理

- **aarch64**：在 x86_64 Ubuntu runner 上使用 `clang --target=aarch64-linux-gnu` 交叉编译
- **x86_64**：在 Ubuntu runner 上原生编译
