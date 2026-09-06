# User Instruction Memory

This file records user instructions, preferences, and teachings for reference in future interactions.

## Format

### User Instruction Entry
User instruction entries should follow this format:

[User Instruction Summary]
- Date: [YYYY-MM-DD]
- Context: [Mentioned scenario or time]
- Instructions:
  - [Content of user teaching or instruction, described line by line]

### Project Knowledge Entry
Entries discovered by the Agent during task execution should follow this format:

[Project Knowledge Summary]
- Date: [YYYY-MM-DD]
- Context: Discovered by Agent while performing [specific task description]
- Category: [Operations & Deployment|Build Methods|Testing Methods|Troubleshooting & Debugging|Workflow & Collaboration|Environment Configuration]
- Instructions:
  - [Specific knowledge points, described line by line]

## Deduplication Strategy
- Before adding a new entry, check for similar or identical instructions.
- If a duplicate is found, skip the new entry or merge it with the existing one.
- When merging, update the context or date information.
- This helps avoid redundant entries and keeps the memory file tidy.

## Entries

[Project Knowledge Summary]
- Date: 2026-08-05
- Context: Discovered by Agent while fixing Gitee API 405 error in build-dart-version.yml tag sync
- Category: Troubleshooting & Debugging
- Instructions:
  - Gitee API v5 没有 /repos/{owner}/{repo}/git/tags 端点，对该路径 POST 会返回 405 Method Not Allowed。
  - 创建 tag 的正确接口是 POST /repos/{owner}/{repo}/tags，参数为 refs（起点，传分支名或已存在的 commit sha，如 master）、tag_name、tag_message，成功返回 201 及 Tag 对象。
  - Gitee API v5 完整 swagger spec 可无认证从 https://gitee.com/api/v5/swagger_doc.json 获取（版本 5.x），用于查证任何接口的路径、HTTP 方法和参数。
  - 另有从 Swagger 自动提取的中文接口文档可作参考：https://corper.cn/down.php/25b7311581ab33080c58cfaf124909f4.md（264 个接口，2026-08-05 生成）。
  - Gitee API 认证支持 Authorization: token <access_token> header，也支持 URL query 参数 access_token。
  - 若目标 Gitee 仓库没有源码历史（只有初始 commit），创建 tag 时 refs 只能传该仓库已存在的 ref（如 master），不能传 GitHub 仓库的 commit sha。
  - Gitee API v5 没有删除 tag 的接口（DELETE /repos/{owner}/{repo}/tags/{tag} 在 nginx 层返回 404，swagger 264 个接口中 tags 仅 GET/POST）。删除 tag 只能通过 git push --delete refs/tags/{tag} 或网页操作。
  - Gitee API 的写操作（POST/PUT/DELETE/PATCH）在路由匹配之前有全局登录中间件，未登录时任意路径都返回 401 登录失效；因此无法用无 token 请求探测写接口是否存在，需用 GET 公开接口或 swagger spec 确认。

[Project Knowledge Summary]
- Date: 2026-08-24
- Context: Discovered by Agent while adapting blutter to parse an obfuscated APK
- Category: Build Methods & Environment Configuration
- Instructions:
  - blutter 主工程为第三方定制版 /workspace/blutter（含 x64 扩展，src/ 平铺，文件与官方版有差异不能直接覆盖）；官方版源码仅存于其 git 分支 `260824-feat-obfuscated-size-recovery`，本地 /tmp 副本已清理
  - 构建与运行必须用 background terminal 工具（编译 memory_percent=30 + timeout 600000ms；完整解析 memory_percent=30 + timeout 1200000ms），不得用普通 bash 直接跑
  - 构建：cd /tmp/opencode/workspace_build/blutter_dartvm3.3.4_android_arm64 && ninja -j4（第三方版构建目录，删除后需重新 cmake 配置）
  - 运行必须使用构建目录下同名二进制（不是 bin/ 下的旧版）
  - 完整解析输出加 stdbuf -oL -eL 分行缓冲，日志约 2-3 分钟，成功以 EXIT=0 且出现 "Generating Frida script" 为标志
  - 输入 libapp.so 唯一：/tmp/opencode/apk_extract/lib/arm64-v8a/libapp.so；输出目录每次用新路径便于对比
  - 解析输出中带 `Analysis error at line ...` 的 InsnException 打印属预期（单个函数分析退化），程序不崩溃即正常；真正致命的是未捕获异常 terminate（需 gdb 定位）
  - 第三方版构建必须显式传 -DHAS_RECORD_TYPE=1（CMake 变量）；缺失时会在 DartTypes.cpp:330 FATAL "Invalid abstract type" 崩溃（gdb 栈：loadFromObjectPool → DartTypeDb::FindOrAdd）
  - 第三方版源码目录需含 scripts/frida.template.js（从官方版复制），并在运行 CWD 放 scripts 软链（FridaWriter 按 exe 目录→父目录→CWD 顺序找模板，否则 copy_file 报 No such file）；该副本已随清理删除，如需本地运行可从仓库根 /workspace/scripts/frida.template.js 复制
  - 构建依赖 /workspace/packages（已由 symlink 转为真实目录，含 dartvm3.3.4_android_arm64 头文件与静态库），find_package 从 `../packages` 相对源码目录定位

[Project Knowledge Summary]
- Date: 2026-08-19
- Context: Discovered by Agent while adding ubuntu_22_windows / ubuntu_24_windows build targets to workflows（该目标已于 2026-08-27 移除）
- Category: Workflow & Collaboration
- Instructions:
  - GitHub Actions 的 jobs.<job_id>.name 不支持表达式求值（${{ }} 会原样显示在 UI），本项目约定 job name 一律静态化，动态信息（构建目标等）通过 workflow 级 run-name 展示。
  - 判断产物源文件名时以 scripts/build.py 的 _arch_suffix 为准：aarch64→android_arm64、windows_x64→windows_x64、x86_64→linux_x64（x86_64 在 x64 宿主为非交叉编译，产物不带头文件后缀）。
  - 定制版 blutter.py 的下载 base 必须与解析目标一致（blutter_dartvm<ver>_{os_name}_{arch}），曾因硬编码 android_arm64 导致 windows_x64 产物无法自动下载。
  - build.py 的 arch 目标拆为「产物架构（os/arch）」与「解析架构（blutter_arch，决定编哪套 CodeAnalyzer/Disassembler）」两维；blutter_arch=x64 时 CMake 传 -DNO_FRIDA=1（不生成 frida.js）。aarch64_windows 目标（ARM 宿主解析 Windows 桌面）已随 ubuntu_*_windows 一并移除。
  - ubuntu_*_windows 目标已在 build-blutter.yml / build-dart-version.yml / build.py 中整体移除（2026-08-27），保留的构建目标为 windows_android、windows_windows、ubuntu_22、ubuntu_24。

[Project Knowledge Summary]
- Date: 2026-08-31
- Context: Discovered by Agent while performing arm64 semantic regression on blutter
- Category: Build Methods & Troubleshooting & Debugging
- Instructions:
  - `libdartvm3.3.4_android_arm64.a`（及 dartvm aarch64 库）实际是 x86-64 机器码 + ARM64 语义宏（TARGET_ARCH_ARM64 / DART_COMPRESSED_POINTERS）：blutter 是解析器不执行 arm64，"android_arm64" 构建 = host x64 编译 + ARM64 宏控制对象布局/语义
  - build.py 的 aarch64 目标依赖 cross toolchain（`-DCMAKE_TOOLCHAIN_FILE=cross/aarch64-toolchain.cmake`），本地无该文件时 cmake 直接失败；本地 arm64 语义回归应手动 cmake 构建
  - 手动 arm64 语义构建：cmake 传 `-DBLUTTER_ARCH=arm64` + `-DDARTLIB=dartvm3.3.4_android_arm64`，C/C++ 编译器都要 clang-16（C 编译器若用 gcc 会因不认 `-stdlib=libc++` 在 ABI 探测阶段失败），`-DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++`
  - arm64 语义回归测试输入唯一：/tmp/opencode/apk_extract/lib/arm64-v8a/libapp.so（winapp/app.so 是 windows 快照，arm64 版会报 "Snapshot not compatible" 属预期）
  - 回归基线法：在 /workspace/blutter（独立 git 仓库）`git stash push` 未提交修改 → 编译基线版 → 跑同一 libapp.so → `git stash pop` → diff 输出目录；pp.txt / objs.txt 中 NativeFn/Closure 运行时地址（0x7f.. 等）每次运行不同属正常，objdump 原始反汇编应一致，asm/*.dart 输出增多为功能增强

[Project Knowledge Summary]
- Date: 2026-09-02
- Context: Discovered by Agent while adding winapp regression and regression.sh
- Category: Build Methods & Testing Methods
- Instructions:
  - Flutter Windows 桌面样本（/tmp/opencode/winapp/app.so）解析须用 x64 构建：`BLUTTER_ARCH=x64` + `DARTLIB=dartvm3.3.4_linux_x64` + `NO_FRIDA=1`，编译目录 /tmp/opencode/ci/build/blutter_dartvm3.3.4_linux_x64（clang-16 + `-stdlib=libc++`）；用 arm64 语义构建解析会报 "Snapshot not compatible"
  - x64/NO_FRIDA 构建的成功标志是 `Generating application assemblies`（无 "Generating Frida script"）；arm64/android 构建才打印后者
  - 两个样本的语义回归一条命令入口：`scripts/regression.sh`（默认先 ninja 两个构建目录；断言基线见 .monkeycode/docs/semantic-clue-collection.md 第 8 节）
  - 解析产物 asm/*.dart 部分文件含二进制字节，grep 必须加 `-a`（--text），否则被当二进制跳过导致统计失真
  - 混淆样本的 `Xxx::call`（如 `_fw::call`、`_dw::call`）是 async/stream 包装的稳定噪声，跨 Android/Windows 样本形态一致仅类名不同；按"静态黑名单不启发式"约束逐条加进 `CALL_BLACKLIST`

[Project Knowledge Summary]
- Date: 2026-09-02
- Context: Discovered by Agent while merging upstream updates from official blutter
- Category: Workflow & Collaboration
- Instructions:
  - 定制版源码基线 = 官方 blutter commit 3f8cf8a^（2026-08-13 前）；官方 main 最新 4a60ac6（2026-08-18，即 PR #217 merge）。官方相对定制版缺失的 src 更新仅 3f8cf8a 一处（Dart 3.11+ ldur x2,[x29,#-8] 叶子调用前重载参数跳过逻辑），其余官方 commit（f2778a7/528acbe IDA 修复等）已在定制版中
  - 对比方法：浅克隆 https://github.com/worawit/blutter 到 /tmp/opencode/blutter_official，先 diff 文件清单（官方 blutter/src 43 文件全部存在，定制版多 CodeAnalyzer_x64.cpp/Disassembler_x64.cpp/Disassembler_x64.h），再逐文件 diff 定位差异；用 `git show <commit>:<path>` 取官方各版本文件与定制版比 diff 行数可精确锁定 fork 基线
  - CodeAnalyzer_arm64.cpp 中 try-catch 包裹 processCheckStackOverflowInstr 是定制版自研容错，合并官方补丁时须保留
  - 官方根目录 blutter.py/dartvm_fetch_build.py 是官方 dartvm 构建工具链，与定制版 scripts/build.py + packages/ 体系不同，不属于源码合并范畴

[Project Knowledge Summary]
- Date: 2026-09-02
- Context: Discovered by Agent while fixing Windows memory mapping in ElfHelper.cpp
- Category: Troubleshooting & Debugging
- Instructions:
  - blutter/src/ElfHelper.cpp 的 _WIN32 分支：整文件读入用 VirtualAlloc(PAGE_READWRITE) + 分块 ReadFile；此前整块 VirtualProtect 为 PAGE_EXECUTE_READWRITE（全量 RWX）。官方 Windows 宿主用 CreateFileMapping(FILE_MAP_COPY) 纯 RW 即可，是因为官方解析的是 arm64 snapshot（arm64 指令无法在 x64 宿主执行）；定制版解析 Windows x64 app.so 时 dartvm 的 Dart_Initialize 会真正执行 snapshot 代码，RW 页触发 DEP 崩溃，因此需要给代码页执行权限
  - 2026-09-02 起改为按 ELF PT_LOAD 分段：仅 PF_X 段提升 PAGE_EXECUTE_READWRITE，其余保持 PAGE_READWRITE（winapp/app.so 上 RWX 从整文件 25MB 收窄到代码段 ~15MB，数据/BSS 段不再可执行），以降低 Defender/沙箱启发式告警面；ELF 程序头用 dartvm platform/elf.h 的 ElfHeader/ProgramHeader
  - 该 _WIN32 分支本地无法编译/运行验证（无 mingw、无 windows dartvm 库、非 Windows 宿主）：语法用 stub windows.h + clang++ -fsyntax-only 检查（/tmp/opencode/winstub/），段范围用 python struct 模拟验证；真实 Windows 行为需在 Windows 宿主上实测

[Project Knowledge Summary]
- Date: 2026-09-03
- Context: Discovered by Agent while implementing IDA semantic function renaming (specs/2026-09-03-ida-semantic-fn-rename)
- Category: Build Methods
- Instructions:
  - blutter/src/DartDumper.h/.cpp 语义重命名闭环：DumpCode 把业务库（url 无 ':'）混淆函数（__unknown_function__/剥 _ 核心≤4 且含大写或数字的短名）的线索登记进 fnSemanticClues_（ep → strings/calls）；Dump4Ida 查表用 set_name 覆盖为 fn_{token} 并 set_cmt 记 origin，另写 ida_script/semantic_names.txt 追踪表
  - 命名候选规则（两轮实测校准，改命名先看此）：业务形字符串（isBusinessToken：无空格/小写开头/词形可读/长3-28/非 NAME_BLACKLIST）取引用序最后一个（_hFk 的 8 条 VPN 串→fn_startVpn）；无则 call 方法段兜底（同受 NAME_BLACKLIST+isUsefulIdent 约束）。SDK 库（url 含 ':'）函数名逐字节不变，NO_CODE_ANALYSIS/空表退化为旧输出
  - 曾犯错误：初版对任意字符串命名产生 fn_HDEFWVNQfh...(随机串)/fn_dart_ui×179/fn_while_dispatching...(句子) 噪声；calls 环最初无黑名单导致 fn_length/fn_Icd 漏网。验证靠 arm64+x64 双样本实测 + regression.sh check_semrename（PASS=61）
  - 规格：.monkeycode/specs/2026-09-03-ida-semantic-fn-rename/（requirements/design/tasklist），产物回归在 /tmp/opencode/{zip_test,winapp}/out_regress/
