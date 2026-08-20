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
- Date: 2026-08-19
- Context: Discovered by Agent while adding ubuntu_22_windows / ubuntu_24_windows build targets to workflows
- Category: Workflow & Collaboration
- Instructions:
  - GitHub Actions 的 jobs.<job_id>.name 不支持表达式求值（${{ }} 会原样显示在 UI），本项目约定 job name 一律静态化，动态信息（构建目标等）通过 workflow 级 run-name 展示。
  - 判断产物源文件名时以 scripts/build.py 的 _arch_suffix 为准：aarch64→android_arm64、windows_x64→windows_x64、x86_64→linux_x64、aarch64_windows→linux_arm64（x86_64 在 x64 宿主为非交叉编译，产物不带头文件后缀）。
  - 定制版 blutter.py 的下载 base 必须与解析目标一致（blutter_dartvm<ver>_{os_name}_{arch}），曾因硬编码 android_arm64 导致 windows_x64 产物无法自动下载。
  - build.py 的 arch 目标拆为「产物架构（os/arch）」与「解析架构（blutter_arch，决定编哪套 CodeAnalyzer/Disassembler）」两维：aarch64_windows = linux/arm64 产物 + 解析 x64 + 非压缩指针；blutter_arch=x64 时 CMake 传 -DNO_FRIDA=1（不生成 frida.js）。
  - ubuntu_*_windows 目标在 ARM64 runner（ubuntu-*-arm）上原生构建，产物是 aarch64 可执行文件，mv 为 _windows_x64_<suffix> 命名；Dart VM 库 linux/arm64 + COMPRESSED_PTRS=0 的布局与 x64 非压缩一致（64 位小端），可解析 Windows x64 snapshot。
