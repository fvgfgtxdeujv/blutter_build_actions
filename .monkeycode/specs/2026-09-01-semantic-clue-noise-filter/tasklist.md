# 实施任务：semantic 线索噪声过滤

- 日期：2026-09-01
- 对应：`requirements.md` / `design.md`

## 任务

- [x] 1. `DartDumper.cpp` 头部增加 `CALL_BLACKLIST` / `TYPE_BLACKLIST`
- [x] 2. `callClueFromFn`：dart:core URL 丢弃、`method` 以 `_` 开头丢弃、私有类 tear-off 丢弃、黑名单精确匹配
- [x] 3. `typeClueFromName`：TYPE_BLACKLIST 丢弃
- [x] 4. `DumpCode` 收集循环：Type/Call 分桶，限额 Type≤2 / Call≤4，汇总顺序 String+Field → Type → Call
- [x] 5. clang-16 增量编译 `/tmp/opencode/ci/build/blutter_arm64_dbg/`（EXIT=0）
- [x] 6. zip `libapp.so` 回归到 `/tmp/opencode/zip_test/out_noisefilter/`，对照 `design.md` 第 6 节（EXIT=0，`Generating Frida script`）
