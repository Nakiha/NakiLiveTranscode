# 同输入编码分支进程隔离与动态输出：本地验收

**实验版，最终验收未完成，不建议生产启用。** 本文区分 issue #4 的实现、本机可复核结果和生产环境仍需完成的工作。不能把本机功能通过写成原生服务器容量、安全复审或部署验收通过。

## 实现与依赖

本轮实现同一输入共享 reader/demux/decoder，各 rendition 独立 exec；另提供相同逻辑、协议和预算的 thread 后端。动态增删、手动/有限自动恢复、无输出保活、控制器接管以及 AVC 同码流多目标均为显式选择的新后端能力。省略 `rendition_backend` 仍使用既有固定输出路径。

默认手动恢复。`idle_timeout_ms=0` 禁用删除最后一个逻辑输出后的保活，正整数指定时长，默认 60000ms。只有真正挂接的新输出取消计时；准备中、重复请求、控制器重连不会续期。接口和边界见 [运行时](rendition-runtime.md)、[保活](session-lifetime.md)、[OpenAPI](openapi.json)。

应用基线为 `318100a912c305fe826c995c9a68c95422612368`。本轮没有修改 FFmpeg/VVenC 算法或 gitlink：FFmpeg `a5b2aa74f271e8462ca50ccdc2ad9ef22fd20901`，VVenC `b894d109e1b0522c9421d6966765c93ba75afba9`。SDK identity key 为 `0147f7987263e5fec809390662f1e2db0f404970142db5ae373f154436374b65`。本轮数据不沿用历史 phase2 或旧 n7 构建的性能结论。

环境是 Apple Silicon 上的 x86_64 QEMU 系统虚拟机及 Linux 容器，4 个虚拟 CPU、6GiB 客体内存。pidfd、memfd、sealing、seccomp 来自真实 Linux 内核。容器启用 `--init` 回收孤儿 collector；仓库 Compose 的 `init: true` 对应此条件。应用正常构建与 ASan/UBSan 构建分离；SDK 和系统库未被本轮 sanitizer 插桩。进程守卫禁止派生子进程/导出媒体 FD，因此媒体进程测试关闭需要派生 tracer 的 LSan；独立 quick 单元检查仍启用 LSan。没有削弱进程守卫来迁就测试。

## 关键实现边界

- 共享池 producer 的可写映射先于 sealing 建立；消费者只有 O_RDONLY FD 和只读 AVBuffer。slot/credit 账本留在输入 owner 内，释放与实际交付分开，进程退出须同时得到 pidfd 与 waitpid 证明。
- 挂接要求 owner 已进入准备流程且 action 是 add/restart；刚受理、尚未处理的 remove 不会被下一帧误当成新挂接。stop/EOF 拒绝新挂接。确定性 `rendition-attach` 单元与真实增删压力共同覆盖这个边界。
- raw credit 直到最后一次像素访问才归还；同编码器多目标的逻辑在途费用直到最后一个压缩包引用才结算。关联账本丢失是明确失败，不能作为成功完成。完成通道独立且有预留容量。
- 多目标每层队列合计留在 rendition 的 128 帧预算内；目标写错/满额只失败该目标。primary 删除后，其他目标实际 flush 仍能确认水印；每个镜像自身的视频起点还会打开自己的音频 gate。
- 音频传输保留参数、压缩字节、时间基及白名单侧数据。AAC 参数探测显式使用 manual-skip，避免 initial padding 把一次性探测的唯一一帧裁空；MPEG-TS stream-id 仅接受一字节。未知元数据不会被静默忽略。
- 每个目标单独校验 source_all 到 mux 时基的范围与单调性。AVC RAP 依据真实 NAL5 和固定 SPS/PPS，普通 I/KEY 不是热接入依据。constrained_crf 的实际写反馈仍留在独立 rendition 内，暂不开放多目标复用。
- 事件日志路径独立于可选帧 trace，关闭 frame_log 也能准备分支。网络库在资源加载线程前初始化；控制器恢复先导出实际拓扑/版本，missing ACK 保留不确定交付语义。

## 证据方法

每次测试创建唯一的 `out/` 目录，预先固定输入帧数/循环数，要求正常完成和非零错误退出传播。独立解码采用 `-xerror -fps_mode passthrough`，避免 FFmpeg 默认 CFR 变换把实际输出补帧/丢帧后再比较。源帧 ID、显示 PTS、逐帧解码像素、SEI、实际 mux flush、最终 credit/槽闭合分开核对。

正常完整入口为 `scripts/test-service.py full --suite native --suite renditions`；可再选择既有媒体和控制回归。`renditions` 间接运行静态四路、热增删、真实故障、恢复、多目标、音频/SEI、输入死亡、格式/stop/EOF 竞争、增删压力、32 组合矩阵、高级能力及持续负载。sanitizer 入口为 `scripts/test-service.py sanitize --suite native --suite renditions-sanitize`。正常与 sanitizer 必须使用各自匹配的 build/bin 目录。

时延边界是服务 packet ingress 至实际 write/flush 完成，覆盖池复制、IPC、分支及 mux 排队；不是 source-to-screen。每个实际完成的视频帧参与 P50/P95/P99。CPU 使用整棵服务进程树的用户/内核 ticks（含已 wait 的子进程累计），另留容器 cgroup 总 CPU 作为带监控开销的观察值。内存使用 PSS/private/shared，避免将共享池按各进程 RSS 重复计费。POST 到终态包含初始化与 drain；持续实时能力还要看中段进度和队列，不能只把帧数除以总墙钟时间。

发布前审查曾发现旧资源读取器将 `/proc/<pid>/smaps_rollup` 地址头误当数值，随后把失败记成零。此前全零的 CPU/PSS 数据和基于它的内存检查全部弃用。最终数据只接受修复后的读取器：解析失败、权限错误和缺字段明确失败，CPU 必须前进，存活服务的 PSS/线程/FD 必须非零；正常与 sanitizer 检查重新执行。

两种新后端都走同一份池复制/描述符协议。这里的 thread/process 对照隔离宿主方式的代价，不代表既有本地 AVFrame 引用线程路径的零拷贝性能基线。

## 本轮结果

最终选定检查分两次完成。正常构建 `rendition-acceptance-20261026-04` 的 SDK/native/renditions/updates/lifecycle-media 通过，随后 audio-signatures 下载 HE-AAC 标准样本时 TLS 握手超时，入口整体为 failed；剩余三套在 `rendition-completion-20261026-01` 使用已有、固定大小和 SHA256 验证的标准样本补跑通过。ASan/UBSan `rendition-sanitize-20261026-04` 整体通过。三轮源码摘要一致，运行前后源码及二进制未变。下表只覆盖选定套件，不代表整个 full profile 或生产验收。

| 阶段 | 本轮已实现并验证 |
| --- | --- |
| M1 | 同输入共享解码、4 路 exec、只读共享池、独立 credit/完成通道、退出证明；9 类真实故障下健康路固定 150 帧及像素一致。 |
| M2 | prepare/attach/detach/drain、幂等/CAS/代际、手动与有限自动恢复、可配置保活、音频/SEI 订阅屏障、实际拓扑接管；TCP 接收端 3 次重启/4 次连接。 |
| M3 | 同逻辑 thread/process，AVC/HEVC 3B/7B × async/early 32 组合，高级能力 2 例、750 帧持续负载 2 例；每种后端正常与 sanitizer 各 20 轮增删。 |
| M4 | AVC 共享码流多目标、真实 IDR gate、独立 RAP、慢 TCP 目标失败隔离、primary 删除后的镜像像素/音频/水印实际写确认。 |

| 入口 | 选定步骤及退出码 |
| --- | --- |
| normal | sdk-consumer=0, native=0, renditions=0, updates=0, lifecycle-media=0, audio-signatures=1 |
| completion | sdk-consumer=0, audio-signatures=0, video-rate=0, control-lifecycle=0 |
| sanitizer | sdk-consumer=0, native=0, renditions-sanitize=0 |

两套 `native` 均 27/27。正常入口另外选择 updates、lifecycle-media、audio-signatures、video-rate、control-lifecycle；lifecycle-media 保留既有 86 场景。远端 CI 没有作为本轮完成证据。OpenAPI 的 37 个组件用 Ajv 8.20 / draft 2020-12 编译并检查正反例；未启用 format 校验，不等于完整 OpenAPI 官方验证。Linux 测试入口 14/14、源码组织检查 6/6 另有本地记录。

### 构建和文件绑定

应用及测试源码摘要：`f585d283995762a18f4dc3a344781e89ab60859ce58c766d07c7c66761146989`。摘要算法见 `scripts/test-service.py::source_snapshot`；逐文件 SHA256、每个 fixture 的 SHA256、每例 run ID 与原摘要 SHA256、完整分位数见 [机器可读证据](evidence/rendition-20261026.json)。文档在媒体检查结束后整理，不属于该源码摘要。完整提交文件清单见 [文件清单](evidence/rendition-20261026-files.txt)，提交身份以包含本报告的 Git commit 为准。

| 构建 | ELF | SHA256 | Build ID |
| --- | --- | --- | --- |
| normal | `naki-live` | `c5792458737c4eff22d5df043c5d18374b358d55345b23bb883e7a5d463af5a3` | `01de4225ded8925eda99925298246e72f3c37699` |
| normal | `naki-live-worker` | `02ebf3cfc74491a423254a7bc8d28a0b5240ef29d10c4d41ff8f73ddff3028f8` | `b6a42dab6208dfbb01609cf2c2649e71f2ec91d7` |
| normal | `rendition-faults/naki-live-worker` | `68f4cabcdc1849fa964ad73d49c6a094d55e7eda4a2fda58257ff61f846c16ef` | `1dde3091a199fc77ebc3475859c829beb7137771` |
| sanitizer | `naki-live` | `40e74f64d2663e59d4f8d171e0adf52604ba9be2c1568cbf5071cc631f7cad10` | `4514b08cad47878caa53ab031148e7ac1671926d` |
| sanitizer | `naki-live-worker` | `edb31b010fec09a200edc1a109d492263f85be255ab70dd9bfca70d5f2c5be02` | `b8b324af851f72c6c0c4d6f0e0b0d87784f6609e` |
| sanitizer | `rendition-faults/naki-live-worker` | `55ce62e3c9bc3e302634b181ebca7d7c731ef3b6b20fa2e68a118eb7b5ab73c3` | `cad5d4a8cb6247ebe0096ab7c2c4bcc2c9a6c2bb` |

SDK manifest SHA256：`31f9df804e52f91323fd76374c3de454baa66688377f9b8e7877b58716cf8818`。故障 helper 带测试钩子，与正常 worker 分别记录，未把故障钩子加入默认服务二进制。

### async / 解码快出开关矩阵

每例固定 75 个源帧、两路 AVC 编码输出（160×96、80×48，25fps，ultrafast，编码线程各 1、decoder 固定 2）。AVC 输入显式声明 HRD/CBR/CFR，HEVC 7B 为 10-bit；同 fixture 的各开关/后端输出逐帧解码像素完全一致，无丢重帧，最终池占用为零。`decoder_pump=false` 固定，避免与 async 混淆。CPU-ms/源帧合计整棵服务进程树，包含两路输出；PSS 是采样峰值。时延列均按 out0/out1 展示各自 P50/P95/P99（ms）。单次小样本可复核行为，不能估计稳定容量或据此保证收益。

| 输入 | 后端 | async | 快出 | CPU-ms/源帧 | PSS MiB | out0 P50/P95/P99 | out1 P50/P95/P99 | 最大排队帧 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| h264-b3 | thread | 0 | 0 | 16.93 | 36.07 | 121.48/244.30/246.37 | 121.06/244.09/245.80 | 2 |
| h264-b3 | thread | 0 | 1 | 17.07 | 36.12 | 83.29/164.88/165.26 | 82.50/164.26/164.65 | 4 |
| h264-b3 | thread | 1 | 0 | 18.13 | 36.09 | 82.67/204.67/205.96 | 82.24/204.22/205.51 | 2 |
| h264-b3 | thread | 1 | 1 | 16.80 | 36.16 | 44.46/125.86/127.29 | 43.49/125.12/126.55 | 2 |
| h264-b3 | process | 0 | 0 | 19.87 | 41.38 | 121.62/243.78/244.61 | 121.14/243.39/244.30 | 2 |
| h264-b3 | process | 0 | 1 | 19.60 | 44.30 | 83.96/164.81/165.61 | 82.89/164.44/165.15 | 3 |
| h264-b3 | process | 1 | 0 | 20.27 | 41.21 | 82.38/204.19/204.50 | 81.81/203.76/204.00 | 2 |
| h264-b3 | process | 1 | 1 | 20.00 | 41.39 | 44.83/126.30/127.72 | 43.85/125.36/127.04 | 4 |
| h264-b7 | thread | 0 | 0 | 16.93 | 36.00 | 82.16/403.22/405.08 | 81.65/402.88/404.66 | 2 |
| h264-b7 | thread | 0 | 1 | 16.80 | 36.05 | 43.53/323.99/325.61 | 43.06/323.49/324.86 | 2 |
| h264-b7 | thread | 1 | 0 | 17.07 | 36.08 | 43.26/362.96/363.97 | 42.75/362.60/363.42 | 2 |
| h264-b7 | thread | 1 | 1 | 17.20 | 36.20 | 3.75/285.14/285.96 | 3.34/284.44/285.14 | 4 |
| h264-b7 | process | 0 | 0 | 19.73 | 41.31 | 82.68/403.01/403.19 | 82.24/402.34/402.79 | 2 |
| h264-b7 | process | 0 | 1 | 19.87 | 41.49 | 43.36/324.36/324.72 | 42.80/323.57/324.25 | 2 |
| h264-b7 | process | 1 | 0 | 20.53 | 41.46 | 43.29/363.80/366.88 | 42.92/363.31/366.47 | 2 |
| h264-b7 | process | 1 | 1 | 20.53 | 41.54 | 3.80/285.46/287.19 | 3.43/284.50/286.81 | 0 |
| hevc-b3 | thread | 0 | 0 | 18.40 | 34.62 | 121.51/243.45/245.77 | 120.72/242.71/245.62 | 2 |
| hevc-b3 | thread | 0 | 1 | 18.13 | 34.51 | 85.15/242.76/243.25 | 83.99/242.13/242.51 | 0 |
| hevc-b3 | thread | 1 | 0 | 18.13 | 34.72 | 83.14/205.90/206.46 | 82.38/205.12/205.65 | 2 |
| hevc-b3 | thread | 1 | 1 | 18.67 | 34.64 | 46.23/205.13/208.16 | 44.79/204.38/207.42 | 2 |
| hevc-b3 | process | 0 | 0 | 20.93 | 40.14 | 121.66/243.56/243.97 | 120.95/242.69/243.09 | 2 |
| hevc-b3 | process | 0 | 1 | 20.80 | 40.05 | 84.50/242.99/245.25 | 83.70/242.26/244.53 | 4 |
| hevc-b3 | process | 1 | 0 | 20.80 | 40.41 | 83.44/204.44/204.66 | 82.85/203.76/203.92 | 2 |
| hevc-b3 | process | 1 | 1 | 20.93 | 40.27 | 47.24/206.91/207.55 | 45.62/206.23/206.68 | 4 |
| hevc-b7 | thread | 0 | 0 | 18.53 | 36.39 | 83.22/403.52/404.28 | 82.09/402.43/402.95 | 2 |
| hevc-b7 | thread | 0 | 1 | 18.13 | 36.38 | 46.55/402.67/405.46 | 46.10/401.57/404.40 | 0 |
| hevc-b7 | thread | 1 | 0 | 18.67 | 36.58 | 44.45/365.09/365.95 | 43.30/364.08/364.94 | 2 |
| hevc-b7 | thread | 1 | 1 | 18.93 | 36.41 | 10.16/364.01/366.93 | 8.63/362.90/365.91 | 0 |
| hevc-b7 | process | 0 | 0 | 21.73 | 41.98 | 83.32/404.16/404.72 | 82.24/403.07/403.77 | 2 |
| hevc-b7 | process | 0 | 1 | 22.67 | 41.81 | 47.67/403.47/404.79 | 46.40/402.46/404.03 | 2 |
| hevc-b7 | process | 1 | 0 | 22.00 | 41.88 | 44.76/365.44/366.94 | 43.66/364.46/365.37 | 2 |
| hevc-b7 | process | 1 | 1 | 21.73 | 42.04 | 10.60/364.87/365.79 | 9.71/363.71/364.68 | 2 |

本轮 process 小样本中，全部关闭→同时开启 async/快出，out0 P99 分别为 AVC 3B 244.61→127.72ms、AVC 7B 403.19→287.19ms、HEVC 3B 243.97→207.55ms、HEVC 7B 404.72→365.79ms。HEVC 快出单独开启主要改善中位数，未明显缩短这里的 P95/P99；不能把“开关已开启”写成每帧提前或尾时延普遍下降。固定 pacing 的 75 帧例也不是最大吞吐测试。

快出配置与实际选择分开：JSON 每例的 `decoder_early_observation` 包含 `selected_by_mode`、候选/回退计数。H.264 自动路径仍要求 HRD/CFR 时序和满足资格的完整单 slice 图像；无声明或不满足条件会回退。HEVC 使用自身已验证的 POC/IDR/prefix 策略。快出是解码选择，不是 async 或调度泵的别名。本轮未修改 FFmpeg 算法；这张表不能覆盖任意动态 B 结构或所有生产片源。

高级能力另外固定 HEVC 3B 输入：两个后端各 1 例，VVenC native8 reference、codec_trace、constrained_crf、decoder_pump、future-frame 守卫与 5fps 选帧联合检查，固定 75 源帧/每输出 15 帧、显示顺序与像素对照通过；不支持的 VVC 动态 RAP 返回 422。没有用调整这些工具制造后端收益。

### 持续实时、资源和队列

持续负载固定 750 帧/30 秒 AVC 7B、25fps，输出 640×360 与 320×180。两种后端均 async/快出开启，其他参数相同。中段 5–25 秒要求 ≥24fps；24–29 秒与 5–10 秒平均队列差 ≤4 帧。全部 750 帧逐帧独立解码和身份检查，最终 credit 及槽闭合。30 秒是短持续检查，不是 24/72 小时 soak。

| 后端 | 中段 fps | CPU-ms/源帧 | PSS MiB | 最大队列/原始持帧 | 平均队列增长 | 观察到的最老在途 ms | out0 P50/P95/P99 | out1 P50/P95/P99 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| thread | 25.05 | 27.57 | 84.09 | 4/4 | -0.17 | 142.70 | 14.30/301.90/304.97 | 11.54/296.44/299.74 |
| process | 25.12 | 30.07 | 89.36 | 4/4 | 0.00 | 301.64 | 14.76/303.20/305.53 | 12.13/297.70/301.25 |

| 增删检查 | 后端 | cycles | root FD 基线/采样范围 | 全树线程基线/采样范围 | PSS 首/末/峰 MiB |
| --- | --- | --- | --- | --- | --- |
| normal | process | 20 | 23/23–23 | 16/16–16 | 23.64/27.91/27.91 |
| normal | thread | 20 | 29/29–29 | 15/15–15 | 21.68/25.20/25.20 |
| sanitizer | process | 20 | 23/23–23 | 16/16–16 | 144.42/181.65/181.65 |
| sanitizer | thread | 20 | 29/29–29 | 15/15–15 | 100.49/135.43/137.17 |

每轮等真实 detach/drain/reap，再检查 root FD 与全树线程回到各自基线；第 10 轮后要求 PSS 后续窗口不比基准窗口增长 16MiB。PSS 包含分配器缓存与 sanitizer 隔离区，平台期检查不是证明整个媒体栈无泄漏。完整资源样本在 JSON，无法读取会直接失败。

### 热加、恢复和故障代价

| 操作 | accepted→first_written ms | attached→first_written ms |
| --- | --- | --- |
| 音频/SEI 热加 | 360.63 | 5.88 |
| 音频/SEI 手动重启 | 96.89 | 5.15 |
| 复用目标 | 346.24 | 2.18 |
| 显式 RAP | 39.58 | 0.70 |
| TCP 重启 1 | 107.44 | 7.53 |
| TCP 重启 2 | 115.02 | 5.26 |
| TCP 重启 3 | 100.68 | 5.24 |

TCP 同一 endpoint 经 3 次 SIGKILL/手动重启产生 4 次真实连接；最终连接验证固定后缀、NAL5、SEI、AAC 首尾时间锚，健康路/共享 decoder 不重启。故障前连接只确认已独立验证的前缀，崩溃尾部可能已写未报，保持 uncertain。AAC 热加与重启 A/V 首尾锚检查通过；逐帧 PES MP3 正例固定 75 视频帧、116 音频帧，FLV/TS 均写出 115、明确计数丢弃 1，未知侧数据丢弃为零。不是“音频零丢弃”的验收声明。

| 故障 | 检出前健康样本数/P95/P99 ms | 检出后健康样本数/P95/P99 ms |
| --- | --- | --- |
| sigsegv | 18/203.60/204.20 | 132/203.74/205.75 |
| sigabrt | 10/203.75/203.75 | 140/204.10/204.44 |
| sigkill | 10/203.99/203.99 | 140/203.83/204.71 |
| sigstop | 32/203.93/205.14 | 118/203.99/205.80 |
| busy | 21/204.94/205.35 | 129/204.59/206.31 |
| deadlock | 21/204.13/204.43 | 129/204.25/205.72 |
| hold_raw | 32/204.35/206.77 | 118/204.33/205.56 |
| stop_release | 32/203.83/205.71 | 118/204.10/204.66 |
| write_shared | 2/87.28/87.28 | 148/203.98/205.46 |

每例健康路固定 150 帧、像素对照通过，输入/健康实例不变，失败分支归因明确，最终池占用零。零样本窗口的分位数为未知；小窗口分位数仅是观察值，不能承诺资源竞争时健康路时延绝对不变。

### 保留的旧容量失败

正常旧入口 `rendition-acceptance-20261026-03` 在 `multi-output` 的四路混合 AVC/VVC 容量场景失败，原因是 VVC 触发既有 100ms bounded admission / video_count。这个失败没有被改成通过。另建精确基线 `318100a9`、同一声明 SDK，在相同输入、preset、质量、线程和阈值下预先指定交替 3 轮：基线 3/3、本轮 3/3 均重现同类容量失败，VVC 写出 8–9 包；三条 AVC 每条固定 125 帧且六轮像素摘要相同。由此只能确认该 QEMU 场景基线也不够容量，不能证明本轮原生吞吐无回归。

这次最终正常检查显式选择 issue #4 与五套既有回归，没有选择上述已知失败的容量场景；其中下载超时的检查使用已校验缓存样本另轮补完，不能写成全部 full/CI 通过。历史窗口全零资源样本不进入最终数据，只保留这次容量失败、基线比较及相同最终应用二进制绑定。比较明细与原摘要 SHA256 在机器可读证据中。

## 使用边界与后续验收

- 最多 4 个逻辑 rendition、16 个显式编码线程预算；每分支原始视频/音频 credit 为 32/16，退出证明前仍计费。共享池及私有在途预算是故障稳健性边界，不是全系统 CPU/OOM 隔离。
- 每个 rendition 最多 4 个 destination；同进程内的目标共享原生故障域。每目标压缩队列 16 包/8MiB、live mux 4 包、写入 deadline 1 秒。慢目标预算失败不停止健康目标。
- 热复用仅支持具有完整固定 SPS/PPS 和经 NAL5 验证 IDR 的 zerolatency AVC，不支持 VVenC 动态 RAP 或 constrained_crf 的多目标反馈。后者显式拒绝，应新建独立 rendition。VVenC 编码的是 VVC；HEVC 输入可以共享解码后再编码 AVC。
- 进程后端和目标热复用 v1 暂不支持 SRT；可另建独立 thread rendition 或既有任务。守卫不可用时准备失败。仅支持 YUV420 planar 8-bit/10-bit LE 和列入白名单的元数据，未知能力不静默降级。
- 删除 primary 后，传统 `packets_out`/时延保留 primary 历史；剩余镜像使用 `destinations[]` 的实际视频/音频计数、AVIO 字节及目标 trace。AVIO 累计写字节包含头、音频、trailer 和 seek 覆写，可能大于文件长度。任一成功目标均可确认水印交付。
- 仍需原生目标 Linux 机器验证实际分辨率/并发、网络接收端重连、长时间稳定性、CPU/带宽竞争以及 P95/P99/容量指标。本机 QEMU 数字不能用于生产容量承诺。每 rendition 的 cgroup CPU/memory/OOM 配置接口未实现；有部署权限的环境需单独评估。
- 音频沿用显式迟到/无时间戳丢弃计数规则。低延迟逐帧 PES 的 MP3 正例不等于任意 MPEG-TS 聚合输入无丢音频；本轮另确认大 PES MP3 的旧路径也有同类丢弃，没有把它伪装成跨进程引入的回归或已实现时间戳修复。
- 独立最终代码复审、远端 CI 和生产部署验收未完成。本轮没有部署、修改生产默认或将实验性能写成生产通过。
