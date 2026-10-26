# 编码分支审计修复与合入检查

本轮从 `feature/rendition-process-20261026@740b0d1bcaa4daf16c7e5b9710098a5e28957b1a` 修复审计问题。服务仍为显式选择的实验功能，默认后端不变；本机功能检查不等于生产容量、部署或任意编解码能力验收。

## 修复与回归

| 问题 | 修复 | 对应回归 |
| --- | --- | --- |
| primary 迁移后协议中断对象失配 | 迁移后的 mux 引用协议最初使用的稳定 Interrupt 地址；原 Pipeline 在所有 destination writer join 后才释放 | FLV/TCP primary 停读后单目标失败，健康 mirror 完成固定 400 帧输入的完整后缀；process/thread 均验证 |
| `file:` 热加目标绕过独占创建 | 普通创建与动态目标共用协议/封装验证；协议别名明确拒绝，本地路径继续 O_EXCL | 运行中拒绝 file URI 与非法 RTMP/MP4 组合，已有 sentinel 字节不变 |
| required 水印沿用会话旧 deadline | 单独传递本次准备操作的接受时间；初始分支仍使用任务接受时间，会话/首帧追踪起点保留 | 超过旧期限后添加 required 水印，故障进程手动重启，以及 thread 删除/重加均实际 first_written |
| ready 与 PREPARED 的竞态 | 媒体 owner 在首个 IDR 入场前顺序提交 PREPARED/APPLYING/APPLIED，并检查返回值；交付确认失败明确返回错误 | 原生测试直接构造“上一 poll 未准备、writer 已 ready、当前是 IDR”，调用实际 destination packet 入场函数 |
| destination 丢弃 finalizer 错误 | 接收 trailer/flush/close 的最终返回值，并更新目标失败状态 | 链接故障 worker 在真实 trailer 后返回 EIO，两个目标均 failed/-EIO，不能报告 completed |
| 每路 io_timeout_ms 丢失 | 经受界 bootstrap 传递并验证 100..10000ms，媒体 owner 用其驱动打开、写出、收尾 | 对不回复握手的 RTMP 接收端分别配置 100/1000ms，核对实际连接时长与失败状态 |
| 串行帧逐渐触碰全部稀疏槽 | 优先复用最近的空闲槽；保留 credit、代次、只读映射和退出证明 | 原生槽重用测试，以及相同独立池探针的前后比较 |

额外修复了两项检查缺口：入口保护断言补上已注册的 `renditions-sanitize`；故障 Harness 从 `/health.crash_report_dir` 获取实际目录，在清理 WSL 私有目录前归档已关闭的崩溃报告。原先 WSL 下硬编码 `logs/crashes` 会漏报并删掉证据。

准备预算同时修正为至少 12 秒；required 水印启动预算加输出 I/O 预算及 2 秒余量更长时使用该值，最大 42 秒。这样允许的 30 秒资源预算不会被固定 12 秒监管期限提前截断。原生测试验证上界与非 required 对照。

## 本机完整检查

环境为原生 x86_64 WSL2 Linux，GCC 13.3、Python 3.12；沿用相同固定依赖：FFmpeg `a5b2aa74f271e8462ca50ccdc2ad9ef22fd20901`、VVenC `b894d109e1b0522c9421d6966765c93ba75afba9`。未改变子模块源码、gitlink 或依赖构建配方。SDK identity 为 `0147f7987263e5fec809390662f1e2db0f404970142db5ae373f154436374b65`；匹配的 FFmpeg/VVenC Build ID DWARF 分开归档。

证据根：`out/rendition-fixes-20261026-232637/`，测试运行跨越 10 月 26–27 日。生成物均不入 Git，早期失败尝试保留，最终结果只引用完成的检查。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 服务构建、原生 quick | 通过，28 项原生测试 | `build-final.log`、`full-retry/02-native.log` |
| quick profile | 11 步通过 | `quick-final/summary.json` |
| full profile | 44 步全部通过 | `full-retry/summary.json` |
| ASan/UBSan profile | 7 步全部通过，独立 Debug 构建；native 保留 LSan，受守卫约束的进程媒体场景关闭 LSan tracer | `sanitize/summary.json`、`sanitize-build-final.log` |
| 审计定向媒体回归 | 正常构建与 sanitizer 均通过，纳入两套 rendition 入口 | `rendition-regressions-3rzt1i98/summary.json`、`rendition-regressions-md7v0mdr/summary.json`（均在 out 下） |
| 实际容器构建、Compose smoke 与匹配符号 | 通过：媒体、worker/HTTP SIGSEGV 栈报告、同 worker 接管、符号解析、日志维护、SIGTERM 与临时资源清理 | `docker-build-windows.log`、`docker-symbols.log`、`docker-smoke.log`；`out/docker-smoke-r90az90v/COMPLETE.json` |

full 与 sanitizer 都确认测试前后源码摘要、选定二进制及 SDK manifest 未变化。full 包含九类进程故障、控制恢复、两后端增删压力、32 基础组合、2 高级组合和持续场景，以及既有媒体/控制/崩溃/符号检查。两套 profile 各自明确记录一个未执行的可选项：未配置可信 HTTPS 水印 fixture；默认必需的本地 HTTPS 拒绝检查仍执行。

容器使用仓库 Dockerfile 的完整 Linux 构建流程，固定子模块在容器内重建；只把独立 SDK expectation 的 COPY 输入改为 named build context，避免覆盖历史 `out/expected-sdk.json`。独立标签为 `naki-live-transcode:rendition-fixes-20261026-232637`，image ID 为 `sha256:121326ae9eb1f030fc96040c195f1aa1511f1711e300a7cee5c2453104914762`；Compose 通过临时 override 使用该标签，未替换既有 local 镜像。容器源码摘要为 `e7d6a796ca0ed5b99799b26a3e393fa8dc1456ff66f4c30edead69b10a0d498f`，应用、FFmpeg、VVenC 三套匹配符号位于本轮 `docker-symbols/`。

本轮本地合入检查已通过；不据此宣称远端 CI、生产部署或目标机容量验收完成。初期未修正的清单断言、WSL 报告目录假设，以及 Docker 准备时的 NTFS Git 超时/行尾差异均保留失败日志。最终 SDK expectation 使用同一仓库脚本与正常 Git 行尾规则生成，identity 与上述 SDK 一致；未跳过依赖完整性校验。

旧测试摘要中固定的 QEMU/仿真环境描述来自早期测试脚本，不作为本轮环境或性能声明；本轮环境以上述 WSL2 说明为准。没有将历史 n7 或旧 QEMU 数据当成本轮容量证据。

## 独立共享池复测

同一个原始探针：129 槽、1920×1080 YUV420 8-bit，每帧 offer 后立即归还消费者和 producer 引用，观察 1/129/258 帧。修复前峰值 RSS 为 4608/393728/393728 KiB，修复后为 4608/4608/4608 KiB；occupied/retained 始终为零。证据分别在 `out/audit-verify-20261026-230813/native-results.log` 和本轮 `pool-probe.log`。

这些数值仅为独立探针的单进程峰值 RSS，不是完整转码服务的 PSS、共享内存复制倍数或生产 OOM 隔离。正常运行时工作集仍取决于各分支真正保留的帧，消费者退出证明前不能回收其引用。
