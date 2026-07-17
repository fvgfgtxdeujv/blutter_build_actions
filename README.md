# Blutter Build Actions

GitHub Actions workflow for building [blutter](https://github.com/kyoheiu/blutter) binaries.

## 用法

1. Fork 本仓库到你的 GitHub
2. 进入仓库 → Actions → "Build Blutter Binary" → **Run workflow**
3. 填写 **Dart version**（如 `3.3.4`、`3.4.2`），其余默认安卓 arm64
4. 编译完成后从 Artifacts 下载二进制文件
5. 放到 blutter 项目的 `bin/` 目录即可使用

## 产物

| 平台 | 文件 |
|------|------|
| Linux | `bin/blutter_dartvm<ver>_android_arm64` |
| Windows | `bin/blutter_dartvm<ver>_android_arm64.exe` |
