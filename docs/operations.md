# 日志、时延、网络观测及长期运行

## 定时状态与时延统计

服务默认每 10 秒由 HTTP 控制事件循环生成一条 JSONL `status`，由独立日志线程写 stdout。输入打开、解封装、编码或网络写出阻塞不阻塞该定时器。输入初始化完成前也能报告 starting。无任务时产生 heartbeat；任务结束立即尝试记录最终状态，停止重复打印该任务摘要。

```sh
./out/bin/naki-live --status-interval 10 --run-dir out/runs \
  --log-queue-capacity 512 --log-rotate-bytes 67108864 --log-rotate-seconds 3600
```

时延统计不对帧抽样：**每个成功写出的输出帧都计入**累计指标和当前窗口。HTTP 查询仅查看，不清空窗口；只有定时报表和最终报表切换窗口。字段 `window` 包含窗口长度、输入/输出 FPS、阶段耗时及帧类型统计；根级指标累计到当前任务。所有时间差使用单调时钟。

各阶段包含 demux_read、input_queue、decode、setup、filter、encode、mux、ingress_to_mux；实时文件回放另含 scheduled_to_mux。`interleave_call` 和 `avio_flush` 分开计时，前者仍包括封装器内部发生的网络 I/O，不应解释为纯交织队列等待。

每项报告 count、mean_ms、max_ms、slow_count，以及 `p50_upper_ms` / `p95_upper_ms` / `p99_upper_ms`。分位数是固定直方图的桶上界估计：0–200 ms 用 1 ms 桶，200–1000 ms 用 20 ms 桶，1–16 s 用 1 s 桶，超过 16 s 单列 overflow。分位数落到溢出桶时返回 null，真实最大值仍保留。不能把上界估计当成精确分位数。count=0 表示未观察到样本。

慢帧阈值 `slow_frame_ms` 默认 200 ms，可按业务 SLA 设置。短窗口中的 I 帧可能只有几个，p99 常接近最大值，应同时看样本数、max 和 slow_count。长时间累计指标可能掩盖新问题，需同时查看窗口值。

这些分布只覆盖已经完成写出的帧：卡住的在途帧还没有最终时延。需要结合 `write_in_progress_ms`、`write_phase`、`last_output_age_ms` 和队列深度判断停滞。自动解码统计由其 owner 线程更新，阻塞时会变旧，`auto_stats_age_ms` 明确表示其新鲜度。

## I 帧和关键帧

`frame_types` 分别记录解码后的 I/P/B 帧的服务内部端到端时延；`first_i` 单列本任务首个 I 帧，`recurring_i` 记录之后的 I 帧，避免首帧初始化成本污染稳态判断。`i_frame_stages` 是 I 帧的各阶段拆分。

同时分别记录 decoded_key 和 output_key。I 帧不必然等于可随机接入的 IDR；这里依赖 FFmpeg 输出帧类型/KEY 标志，不解析 H.264 IDR、HEVC CRA/IDR 细分类别。输出关键帧来自编码包 KEY 标志，与输入 GOP 不必相同。编码器自主选择 GOP，输入 picture_type 只用于追踪，不用于强制输出帧类型。

不为降低 I 帧数字而改变解码或显示顺序，也不绕过参考依赖。大 I 帧的计算、编码包突发和发送积压需要结合分阶段记录分析。

## 逐帧日志、队列和轮转

逐帧明细默认关闭。按需在任务 JSON 中设置：

```json
{"input":"/data/input.mp4","output":"/data/output.ts","frame_log":true,"frame_log_every":10,"slow_frame_ms":200}
```

`frame_log_every=10` 只抽取普通帧明细，I 帧、输出关键帧和达到慢帧阈值的帧总是尝试入队。统计仍然覆盖全部成功输出帧。默认 every=1 即保留全部帧明细。默认关闭时不为每帧构造 JSON 文本。

日志线程独占 stdout 和追踪文件的写入、flush、关闭与轮转。入队不等待磁盘或网络；队列默认 512 条，单条序列化文本限制 64 KiB，另最多一条正在写出。16 个队列名额为状态/生命周期消息预留；满时丢弃日志并计数，不丢媒体帧、不让转码因日志失败终止。状态类日志在全部名额耗尽时也会丢弃，不能作为无损审计日志。

HTTP 状态含 `logging.queued/capacity/high_water/dropped_frames/dropped_status/io_errors/rotations`，这些是本进程 logger 的累计计数。磁盘满或慢 sink 的影响通过这些值暴露。stdout 长期不可写时，日志线程及其优雅退出可能等待；容器 stop grace period 是最终退出期限，不承诺日志完整落盘。

启用追踪后 `log_path` 是路径前缀，实际文件如：

```text
<job>.trace.000001.jsonl.active   # 正在写入，维护程序绝不处理
<job>.trace.000001.jsonl          # 关闭并成功封存
<job>.trace.000001.jsonl.gz       # 外部维护程序压缩完成
```

达到 64 MiB 或 1 小时轮转；单条日志不拆开，单段可能超过上限一个记录的大小。时间轮转也在空闲时检查。任务正常结束时封存尾段。写出错误的段以及进程崩溃时留下的 `.active` 不会自动当成完好封存文件；停服确认后再人工检查恢复，避免误压活跃文件。

## 外部压缩与清理

```sh
python3 scripts/maintain-logs.py --directory out/runs \
  --max-age-days 7 --max-total-bytes 2147483648
```

脚本仅处理本目录内符合 `<job>.trace.<数字>.jsonl[.gz]` 的普通文件，不递归、不跟随文件符号链接、不处理 `.active`。默认等封存文件至少 60 秒才压缩，使用 gzip level 1、临时文件、fsync 和原子发布；成功后删除原始封存文件。维护锁防止多个实例同时运行。压缩失败保留原文件；已有目标冲突也保留原文件供检查。

先按 7 天过期清理，再按封存文件实际总大小从最旧文件开始清理到 2 GiB。此配额不包含活跃/异常 `.active`、临时文件、无关文件、Docker stdout 日志和媒体文件，不能替代整卷空间监控。断电遗留的压缩 `.tmp-*` 也应在确认维护程序未运行后检查清理。

可调整并安装 `deploy/naki-log-maintenance.service` / `.timer`，由宿主机 systemd 每五分钟执行一次，低 CPU/I/O 优先级。模板使用 naki 用户和 `/opt/naki`，部署前修改路径与所有权；本项目不自动修改宿主机 systemd。

## FFmpeg TCP/UDP 网络埋点

fork 新增 `libavformat/network_observer.h`，API 1 / libavformat 61.2.100。进程在打开任何协议之前注册 observer，所有上下文及其后台线程关闭后才可清除/更换。回调可能并发运行，不可分配、写日志、阻塞或重入 FFmpeg。服务回调只更新固定大小的原子计数器。

覆盖 TCP recv/send；UDP 直接 recvfrom/send/sendto；UDP 后台接收和限速发送线程。后台 RX 埋点在数据进入 FFmpeg FIFO 之前。disabled 时不读时钟；回调前后保存/恢复 errno（Windows 网络构建另保存 Winsock 错误）。不改变返回值、超时或重试语义。

公开事件提供协议、方向、后台线程标志、socket fd、opaque 连接身份、请求/实际字节数或错误，以及操作开始、syscall 开始、完成的单调时间戳。TCP/UDP 直接路径的操作耗时包括显式 readiness wait；后台 UDP recv 的 syscall 本身可能等待新数据。**recv 调用耗时不是网络传播时延。**

服务按 tcp_receive/tcp_send/udp_receive/udp_send 汇总 calls/bytes/errors/would_block/EOF、后台调用数、调用均值/最大值、syscall 累计耗时、最后成功收发时刻和距今时长；提供累计及窗口值。would_block 合并可重试的 EAGAIN/EINTR。原子计数快照近似一致，窗口边界附近各字段可能跨过同一个事件。当前单路任务开始前重置这些计数，输出协议握手/控制数据也可能计入，不能等同媒体负载字节。

这是 socket API 边界，send 成功只代表操作系统接受数据，不是已上网线或已到接收端。未覆盖 connect/accept/close、内核/NIC 时间戳和非 TCP/UDP 传输；没有实现网络字节区间与视频帧的逐帧映射。原 `ingress_to_mux` 边界不因此改变。

## 封装交织的边界

当前仍是单路纯视频，没有音频流可等待。`av_interleaved_write_frame` 内部也可能执行阻塞 I/O；耗时长不一定意味着在等待另一流。日志区分该调用与显式 flush，另提供网络发送数据用于排查，但没有添加丢包、强制越序或异步无限写队列。

以后增加音频时，应专项验证缺包、断流、时间戳跳变及封装队列；为 interleave delta、输出 I/O 超时和有界背压制定策略。FFmpeg 的 `max_interleave_delta=0` 表示关闭按时间戳差强制冲刷的限制，不是“零等待”；当前值在单视频场景不证明多流直播安全。该选项约束时间戳跨度，也不是严格的墙钟超时。

## 7×24 小时的进程结构

无需在容器里再做传统 daemon 双重 fork。当前程序以前台单进程运行，线程不能隔离 native crash：一个 codec/filter 崩溃可能结束整个服务。

推荐的长期结构是“Docker/systemd 等外部监管 → 控制进程 → 每路转码 worker 进程”。从系统看可以有三层职责，但不必自己再写一层常驻 daemon 与 Docker 重复监管。worker 使用 spawn/exec 启动，避免已启动多线程的父进程 fork 后直接继续操作 FFmpeg。

控制进程管理任务期望状态、IPC、心跳和有界重启退避；worker 负责一路媒体及其资源。这样单路崩溃或输出阻塞可单独重建。还需要任务状态持久化、重启风暴抑制、CPU/内存/FD 限额、磁盘监控、媒体进度 watchdog、网络重连及长时间 soak test。这些尚未由本轮实现。

Compose 的 healthcheck 检查控制 API 存活，不代表正在持续出流；普通 Compose 不会仅因 unhealthy 自动重启，restart 策略主要针对进程/容器退出。Docker 重启目前也不会自动恢复上一次内存中的转码任务。

参考：[Docker 进程与 init](https://docs.docker.com/engine/containers/multi-service_container/)、[重启策略](https://docs.docker.com/engine/containers/start-containers-automatically/)、[local 日志驱动](https://docs.docker.com/engine/logging/drivers/local/)。
