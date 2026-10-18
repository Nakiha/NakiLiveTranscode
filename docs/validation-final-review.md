# 五批改动合并前整体复查（2026-10-18）

范围为[五批核心整理](validation-core-refactor.md)相对主线 `cdd762f6071c3d2947989dc003d492aab6e4368d` 的全部差异。该主线与五批起点 a1b7e05 的文件树相同；复查分支为 codex/core-refactor-review。FFmpeg/VVenC/yyjson pin、SDK identity 与五批记录一致，没有修改子模块或重新宣称性能改善。

## 发现与修复

发现一个交付阻断：Docker build 阶段没有复制 CMake 新引用的 tests/logger_submit_test.c、tests/pipeline_lifecycle_fixture.c 及本地头文件。完整检出的本机构建通过不能发现此问题，后面的 test 阶段完整 COPY 也无法补救之前失败的 build 阶段。

在执行 build-service 前补齐三份文件；入口保护测试增加对 Docker build 阶段 CMake 源文件及其本地 include 的覆盖核对。按真实 COPY 清单重建两套独立上下文，使用同一已验证 SDK 执行真实 CMake：修复前 exit 1 并报告缺失源文件，修复后 exit 0。该证明不依赖 Docker daemon，也不算实际镜像验收。

同时移除发布文档对原有未跟踪本地规划文件的链接，保留该文件原字节且不纳入 PR。文档区分五批本机冻结窗口、此次复查与随后按提交绑定的远程 CI，避免把历史“未提交/未运行”写成永久状态。

## 行为、所有权与资源核对

- 日志：队列位置与 event_seq 在短锁内预留，准备完成再发布，writer 不越过 PREPARING 队头；sink 的 pending 覆盖准备、排队和写出。溢出、准备失败、状态丢失分别按原类别计数，日志 I/O/轮转仍由唯一 writer 执行。队列/记录/准备并发/遍历和 scratch 上限保持。
- 媒体：reader 唤醒/join 后才释放 async 和 input；输出 owner 执行 drain、finalize/hook、引用释放和终态发布；分支 join 后销毁队列。首个错误、用户停止与分支失败优先级、一次 hook 关闭和实际 write/flush 后结算保持。
- 引用：只有 reader 读取可变 input stream 数组，跨线程只传有界参数快照和引用；每输出 owner 独占 filter/encoder/mux。opaque_ref、编码 PTS ledger、watermark delivery 和 mux packet 引用在各自边界交接，缺映射显式失败。
- 控制：记录/HTTP/投影/监督仍由单 libevent 线程拥有；32 项 CreationStore 保留活动与待 RPC 记录。peer/pidfd 核验和操作结果对账先于准入，控制器丢失不取消媒体，服务 SIGTERM 有意停止全部 worker。
- 限额：resource_limits.h、branch_queue 实现、音轨/延后音频、live mux 包/字节/单调墙钟上限、fatal handler 与网络回调边界没有改动。锁序保持 parent→child；队列引用释放与 Job 状态发布不引入反向嵌套。

与第四批实际验证输入逐文件比较，唯一差异为 app/README.md、Dockerfile 和 tests/test_entrypoints_test.py。所有应用 C/H 字节与第四批一致；本次修复限于交付输入、保护测试和文档。

## 本机候选与验证证据

新证据根为 `out/final-review-8e8556c3/`，不入 Git。统一测试输入 SHA-256 为 `9f5c0fd2ec965f51199d8d93570b92aed2c1d432454d27aad80e5478b62e4c51`；其定义包含 app/README.md，不包含根 README/docs。Windows 本机原始行尾与 Linux checkout 的内容摘要可能不同，远程结果应按各自提交、输入、ELF/SDK 绑定读取，不能混用。

普通构建使用 `NAKI_SKIP_FFMPEG=1 FFMPEG_LIVE_BUILD_JOBS=2 bash scripts/build-service.sh`；full 使用统一入口并指定匹配的 FFmpeg/VVenC 符号目录，sanitizer 使用独立 Debug 入口。命令、退出码、源码/ELF/SDK 前后绑定分别在 commands.json、full/summary.json、sanitize/summary.json。未结束或失败的尝试不记作通过。

| 本轮实际检查 | 结果 |
| --- | --- |
| Linux 正式构建 | 通过，SDK consumer gate 保持；build.log |
| full | 34 步通过，含 15 项原生测试、10 项入口保护及 86 个生命周期场景；full/summary.json |
| 应用 ASan/UBSan | 6 步通过，含 15 项原生测试、水印、多输出、86 场景与服务；sanitize/summary.json；独立构建日志 out/sanitizers-AKYqSt/build.log |
| 输入绑定 | full/sanitize 使用上述同一摘要，源码/所选 ELF/SDK manifest 前后均不变；SDK identity 与五批记录相同 |
| Docker 构建上下文 | 真实 CMake 修复前失败、修复后通过；入口测试保护源文件及本地头文件，未冒充 Docker image 执行 |
| 文档静态检查 | 12 路径/13 操作/33 schema、7 份请求文件、7 个 JSON 示例、6 个非法反例及 181 份已保存响应通过；本地链接与差异检查通过 |

范围核对为 review-scope.json；Docker 配置的失败/成功对照为 docker-context.json；接口/示例/本地导航和已保存响应的静态核对为 docs-checks.json。静态检查使用 JSON Schema Draft 2020-12 和本地 OpenAPI 结构/引用核对，不等同完整 OpenAPI 元 schema 验证，也不替代新运行验证。

本轮普通生命周期与第四批对照，legacy-normal 的 AAC 起播前缀从写出 33/丢弃 13 变为写出 31/丢弃 15，其余案例严格对照一致；sanitizer 的 86 案例严格对照全部一致。前后所选 ELF 字节相同，视频 24 帧及其 PTS、终态、错误、finalize/hook 一致，AAC 保留部分为相同有序后缀；两套各 56 个成功输出均核对实际 AAC 载荷数=audio_packets_out，写出+丢弃=输入 46。原始 strict_different_cases 保留这一项，见 lifecycle-comparisons.json；这属于第三批已记录的启动时交织墙钟/调度差异，不宣称所有重复运行音频前缀相同，也不改写第四批原来两组严格一致的记录。

## 远程门槛和剩余缺口

合并须确认对应 PR 当前 head 的 CI 检查通过；实际镜像、Docker smoke、独立符号和远程执行的结果以该提交的 Actions 记录为准，本机重建 COPY 上下文不能替代。远程实时状态不回写成前五批当时已完成的验收。

外部可信 HTTPS 成功样例仍取决于 NAKI_TEST_HTTPS_URL；缺省须显式记 not_run。本地 ASan/UBSan 覆盖应用/yyjson/helper，native SDK/系统库未插桩，没有 TSan。真实 Filebeat 采集/补采、目标机容量和 24/72 小时 soak、实际迁移回滚与生产验收仍未覆盖；不承诺 RSS、吞吐、硬实时终止或源端到显示时延。
