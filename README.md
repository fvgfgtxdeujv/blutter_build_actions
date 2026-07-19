# Blutter Build Actions

GitHub Actions workflow for building [blutter](https://github.com/kyoheiu/blutter) binaries via cross-compilation.

## 产物

交叉编译为 **aarch64 (Android ARM64)**，适用于 Termux 等环境。

| 文件 | 说明 |
|------|------|
| `bin/blutter_dartvm<ver>_android_arm64` | ARM64 二进制，直接在 Termux 上运行 |

## 用法

1. Fork 本仓库到你的 GitHub
2. 进入仓库 → Actions → "Build Blutter Binary" → **Run workflow**
3. 填写 **Dart version**（如 `3.3.4`、`3.4.2`）
4. 编译完成后从 Artifacts 下载二进制文件
5. 放到 blutter 项目的 `bin/` 目录或在 Termux 上直接运行

## 原理

在 x86_64 Ubuntu runner 上用 `aarch64-linux-gnu-gcc` 交叉编译，输出 ARM64 二进制。
