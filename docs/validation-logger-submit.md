# 第二批：缩小日志提交临界区

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

范围是 `app/logger.c`、提交所有权注释、对应原生测试/入口与验证记录。保留第一批已有改动；媒体生命周期、worker/controller 协议、模块拆分及 FFmpeg/VVenC/yyjson 子模块均不调整。

## 实现与边界

提交分为锁内预留、锁外准备、锁内发布。event_seq、FIFO 位置、准入计数及 sink pending 一起预留；事件 envelope 和 URI 脱敏、JSON 序列化、输入文档释放全部在锁外完成。时间取自预留点，pid/tid 仍是提交线程。准备顺序不影响写出顺序；准备失败留下已计一次丢失的占位，由唯一 logger 顺序回收。预留后发生 finish/release/stop 时有明确的丢弃与 pending 解除路径。

原队列容量、16 个事件预留位置、64 KiB 单条 JSON 及 frame/status 分类计数保持。新增并发准备上限 32、trace 准备上限 16，以及输入遍历和 1 MiB 序列化 scratch 预算，防止锁外工作随队列容量或异常输入放大。排队文本只保留实际长度；所有文件写入、flush、轮转、封存和 I/O 故障处理仍由 logger 执行。完整限制及调用方责任见 [事件合同](process-events.md)。

FIFO 保序意味着队头生产者被暂停会推迟后续写出；等待期间 logger 仍维护已有文件。没有新增准备取消或停机超时机制；调用方须停止新提交并让已占位准备返回。这次不改媒体线程的结束、释放或模块归属。

## 回归方法

新增 `logger-submit` 进入 quick/full/sanitizers 的同一 CTest 清单，并通过 wrapper 为每次执行建立独立证据目录。链接包装器在序列化边界暂停指定生产者，直接验证：

- 第一条尚未准备完时，统计查询和第二次提交仍能返回，但第二条不能先写；event_seq、mono_us、pid/tid 及脱敏结果正确。
- 队列满时不进入序列化；准备名额满时立即按类别计数，trace 不占用事件准备预留；queued/high_water 不越界。
- 准备期间 finish/release 不提前回收 sink；停机等待已占位提交结束，丢失只计一次；每份非空输入文档释放一次。
- 65536 字节 JSON 可写，超一字节、转义膨胀、scratch/输入字节/节点/深度超限、无效 UTF-8、写序列化失败、错误类别和空文档均明确计丢失。
- 队头准备暂停时，已写事件仍可正常封存。原 `event-logger` 的并发序号、阻塞 I/O、控制预留、繁忙 trace 下 flush/轮转、短写及启动失败测试继续执行。

## 同条件微基准（2026-10-18，WSL）

旧源码在修改前保存到 `out/batch2-logger-50637366/before/`，logger.c SHA-256 为 `a96a6c70251124c9f6d2a0ec73da31c924ec6bdfb0d5589cdceac1f8f4a2311a`。不改写第一批或更早冻结的结果。

复现入口：

```sh
python3 scripts/bench-logger.py --baseline-dir out/batch2-logger-50637366/before
```

脚本每次建立新的 `out/logger-bench-*`，用同一 GCC 13.3.0、`-O2`、yyjson 和测试源码分别编译旧/新实现；每组各独立进程跑三遍，旧/新交错并反转次序。本次最终结果为 `out/logger-bench-79ig3vn2/summary.json`，包含源码/ELF 哈希、完整命令和每遍数据，确认输入未变化。前两次探索结果保留在 `logger-bench-exjjk69y`、`logger-bench-etdiktt5`，不混入最终统计。

计时覆盖 logger_submit（包含其文档释放），调用方创建文档在单次计时外。mutex hold/wait 按每次提交内所有加锁求和，包含测量开销和持锁期间被系统调度走的时间；新实现准入通常两次加锁。吞吐的墙钟包含调用方造文档和排空。writer 只调用内存回调，不写磁盘；阻塞场景在一条种子记录处暂停 writer，生产结束后放行。共享 WSL 宿主环境，未隔离 CPU；这是日志 CPU/队列实验，不是磁盘、媒体时延或服务容量测量。

下表取三遍各指标的中位数，箭头为旧→新；KiB 是进程 `/proc/self/status` 的 VmHWM，包含测试框架、线程及调用方文档，不是 logger 单独用量。原始文件另外保留可能含启动器 RSS 下限的 getrusage 数值，不混用。

| 场景 | 每次提交持锁总和 p99（μs） | 提交总耗时 p99（μs） | 提交尝试/秒 | VmHWM（KiB） | 写出 / 丢弃 |
| --- | --- | --- | --- | --- | --- |
| 1 线程，128 字节，2000 次 | 9.548→12.955 | 25.278→17.764 | 205876→474450 | 3456→1920 | 两者 2000 / 0 |
| 4 线程，128 字节，各 2000 次 | 18.085→0.991 | 156.217→49.795 | 260427→870598 | 2304→2432 | 两者 8000 / 0 |
| 8 线程，8192 字节，各 200 次 | 25.639→0.350 | 2697.250→54.865 | 46393→437117 | 18944→10752 | 两者 1600 / 0 |
| 8 线程，8192 字节，各 2000 次，过载 | 21.912→0.290 | 1708.614→50.496 | 67395→928345 | 108416→70400 | 13609 / 2391→11723 / 4277 |
| 4 线程，8192 字节，writer 阻塞、队列 32 | 7.464→0.071 | 349.607→19.167 | 170531→4415598 | 2304→2176 | 两者 33 / 7968，含种子 |

并发场景的临界区与提交等待减少；单线程小记录的持锁 p99 没有改善，不能宣称所有指标均下降。这些数字同时包含 value writer 的 scratch 分配策略和队满时提前拒绝的影响，不能全部归因于移动锁边界。过载下新实现更快碰到相同队列上限，丢弃更多；尝试吞吐不能直接解释为接收或服务容量提升。前两种无丢失场景的成功回调吞吐分别约 200829→466857 和 258280→861456 条/秒，仍仅限本实验边界。

## 当前候选验证

普通构建通过，三项日志原生测试各连续 8 次通过，证据为 `out/batch2-logger-50637366/build.log`、`logger-races.log`。full 的 33 个步骤全部通过，包含 15 项 quick 原生测试以及实流观测、事件维护、更新 API、进程/控制接管与故障回归；证据为同目录的 `full/summary.json`、逐步日志和 `validation.json`。

测试输入 SHA-256 为 `a7c9dc1f1726faf49c8f3a8bce4bafd6af9e2cf20a9d0ff8153d547f4fe8f515`，按 [测试入口](testing.md) 定义的可执行源码范围生成。full 前后源码、所选二进制和 SDK manifest 均未变化。基于主仓 `a1b7e0584355756d0c5b13eaa6668d2a724d2a45` 加第一批及本批未提交修改；SDK identity 仍为 `4c6d30a2595d52765a93db3b68ad2a5a0dfca6e6f1880d1a0607041f536a4e16`，实际 native 子模块 pin 不变。

ASan/UBSan 的 5 个步骤全部通过：SDK consumer gate、15 项 quick 原生测试、水印复审、多输出和基本服务。构建为独立 Debug，开启 `detect_leaks=1:halt_on_error=1` 与 UBSan 遇错停止；没有降低告警门槛。证据为 `out/sanitizers-ZOscN4/build.log` 和本轮 `sanitize/summary.json`。额外的插桩阻塞管道 logger helper 也通过，见 `sanitize-blocked-pipe.log`。插桩范围为应用、yyjson 和测试 helper，不包含外部 native SDK/系统库，也没有执行 TSan。

full 与 sanitize 使用同一测试源码摘要，分别确认前后源码、所选二进制与 SDK manifest 未变化；最终再次核对普通/插桩 ELF、微基准输入以及原有用户文档，均保持对应身份。聚合索引为 `out/batch2-logger-50637366/evidence.json`。应用改动只有 logger.c/logger.h，三个 native 子模块 pin 不变；未提交、推送或发布。

可信外部 HTTPS 水印样本因未设置 `NAKI_TEST_HTTPS_URL` 在 full/sanitize 各明确记为 not_run；本地 TLS 拒绝路径已跑。没有运行实际 Docker、远程 Actions、真实采集端或目标机器 soak，也不据此宣称生产就绪、无所有竞态或媒体性能提升。
