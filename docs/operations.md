# 日志、时延、网络观测及长期运行

## 定时状态与时延统计

worker 控制线程默认每 10 秒生成 status，交给本进程唯一 logger 写进程事件文件；controller 空闲时产生自己的 heartbeat。两者的事件位于 run-dir/events，各有独立 instance_id/event_seq；事件不经 IPC 汇集 stdout。媒体打开、解码或写出不会直接堵住控制轮询，状态可能是旧媒体快照，须结合 freshness/progress 判断。任务结束尝试最终报表并停止重复该摘要。当前事件合同见[进程事件](process-events.md)，当前验收见[核心整理验证](validation-core-refactor.md)。

```sh
./out/bin/naki-live --status-interval 10 --run-dir out/runs \
  --log-queue-capacity 512 --log-rotate-bytes 67108864 --log-rotate-seconds 3600
```

时延统计不对帧抽样：**每个成功写出的输出帧都计入**累计指标和当前窗口。HTTP 查询仅查看，不清空窗口；只有定时报表和最终报表切换窗口。字段 `window` 包含窗口长度、输入/输出 FPS、阶段耗时及帧类型统计；根级指标累计到当前任务。所有时间差使用单调时钟。

各阶段包含 demux_read、input_queue、decode、setup、branch_queue、filter、encode、mux、ingress_to_mux；pace 回放另含 scheduled_to_mux。reader 的 ingress 在解封装、可选 pace 和追踪元数据准备之后记录、进入 ring 等待之前；input_queue 包含等 ring 空位和 async pending 重试。demux_read 覆盖本次取包过程，含 resumable EAGAIN/poll 等待。interleave_call 从编码包返回到 mux/PES 写入返回，包含交织驻留及可能的网络 I/O；avio_flush 单列其后的 flush。逐帧 interleave_queue 是实际开始写前的子区间，不能把这些重叠字段相加。完整边界见[服务](service.md)。

每项报告 count、mean_ms、max_ms、slow_count，以及 `p50_upper_ms` / `p95_upper_ms` / `p99_upper_ms`。分位数是固定直方图的桶上界估计：0–200 ms 用 1 ms 桶，200–1000 ms 用 20 ms 桶，1–16 s 用 1 s 桶，超过 16 s 单列 overflow。分位数落到溢出桶时返回 null，真实最大值仍保留。不能把上界估计当成精确分位数。count=0 表示未观察到样本。

慢帧阈值 `slow_frame_ms` 默认 200 ms，可按业务 SLA 设置。短窗口中的 I 帧可能只有几个，p99 常接近最大值，应同时看样本数、max 和 slow_count。长时间累计指标可能掩盖新问题，需同时查看窗口值。

这些分布只覆盖已经完成写出的帧：卡住的在途帧还没有最终时延。需要结合 `write_in_progress_ms`、`write_phase`、`last_output_age_ms` 和队列深度判断停滞。自动解码统计由其 owner 线程更新，阻塞时会变旧，`auto_stats_age_ms` 明确表示其新鲜度。

## I 帧和关键帧

`frame_types` 分别记录解码后的 I/P/B 帧的服务内部端到端时延；`first_i` 单列本任务首个 I 帧，`recurring_i` 记录之后的 I 帧，避免首帧初始化成本污染稳态判断。`i_frame_stages` 是 I 帧的各阶段拆分。

同时分别记录 decoded_key 和 output_key，不能把解码 I 等同输出 KEY 或 IDR/CRA。输出关键帧取编码包 KEY 标志，实际输出 I/P/B 另由编码器统计。未配置 intra 时保留原编码器周期/profile；显式 intra 策略可请求普通 I，选帧保护和 SEI 保护也按各自合同执行，见[帧策略](frame-policy.md)。这些分类不因普通 I 被确认而合并为一个 key 指标。

不为降低 I 帧数字而改变解码或显示顺序，也不绕过参考依赖。大 I 帧的计算、编码包突发和发送积压需要结合分阶段记录分析。

## 逐帧日志、队列和轮转

逐帧明细默认关闭。按需在任务 JSON 中设置：

```json
{"input":"/data/input.mp4","output":"/data/output.ts","frame_log":true,"frame_log_every":10,"slow_frame_ms":200}
```

`frame_log_every=10` 只抽取普通帧明细，I 帧、输出关键帧和达到慢帧阈值的帧总是尝试入队。统计覆盖全部成功写出帧。默认 every=1 即全部明细；关闭时不为每帧构造 trace，但首个视频/首个输出 KEY 的 first_frame checkpoint 无条件生成并保留供恢复查询，不受明细开关或采样抑制。

每个进程的 logger 独占 event/trace 文件写入、flush、关闭和轮转。队列默认 512、最多 8192 项，为 event 留出 16 个 trace 不可占用的名额；单条文本最多 65536 字节。提交先短锁预留 FIFO/event_seq，再锁外准备、短锁发布；并发准备另限 32、trace 16，输入遍历和 scratch 也有硬上限。queued 包含准备占位和正在写出的一项，最多 capacity+1，high_water 只计队列。producer 不等日志 I/O，满额/准备失败按类别计丢失；状态也会丢失，不能当作无损审计。完整限额和 sink/stop 调用方责任见[事件合同](process-events.md)。

HTTP 的 logging.queued/capacity/high_water/dropped_frames/dropped_status/io_errors/rotations 是 worker 进程 logger 累计值，多输出只在根报告一次；controller 另有 supervisor.log_dropped_status/log_io_errors。控制失联不停止 worker 本地事件/trace，IPC 快照合并丢失另见 ipc_snapshots_coalesced，不能混作日志丢失。磁盘满/慢 I/O 经日志统计暴露；优雅退出仍受 sink 和生产者准备完成影响，stop 期限/强杀不保证最终日志完整。事件落盘不是 operation 的持久化 WAL 或外部采集确认。

启用追踪后 `log_path` 是路径前缀，实际文件如：

```text
<job>.trace.000001.jsonl.active   # 正在写入，维护程序绝不处理
<job>.trace.000001.jsonl          # 关闭并成功封存
<job>.trace.000001.jsonl.gz       # 外部维护程序压缩完成
```

达到 64 MiB 或 1 小时轮转；单条日志不拆开，单段可能超过上限一个记录的大小。时间轮转也在空闲时检查。任务正常结束时封存尾段。写出错误的段以及进程崩溃时留下的 `.active` 不会自动当成完好封存文件；停服确认后再人工检查恢复，避免误压活跃文件。

## 外部压缩与清理

```sh
python3 scripts/maintain-logs.py --directory out/runs --kind trace \
  --max-age-days 7 --max-total-bytes 2147483648
```

脚本仅处理本目录内符合 `<job>.trace.<数字>.jsonl[.gz]` 的普通文件，不递归、不跟随文件符号链接、不处理 `.active`。默认等封存文件至少 60 秒才压缩，使用 gzip level 1、临时文件、fsync 和原子发布；成功后删除原始封存文件。维护锁防止多个实例同时运行。压缩失败保留原文件；已有目标冲突也保留原文件供检查。

先按 7 天过期清理，再按封存文件实际总大小从最旧文件开始清到 2 GiB。此处是 trace 策略，不包含 active/异常段、临时文件、events、crash、stdout 或媒体，不能替代整卷监控。events 另用 --kind events --recover-orphans，须成功取得同前缀生命周期锁并校验 envelope 后才恢复遗留段，首版不 gzip；不能用裸 PID 判断可处理 active，见[事件维护](process-events.md)。

可调整并安装 `deploy/naki-log-maintenance.service` / `.timer`，由宿主机 systemd 每五分钟执行一次，低 CPU/I/O 优先级。模板使用 naki 用户和 `/opt/naki`，部署前修改路径与所有权；本项目不自动修改宿主机 systemd。

## FFmpeg TCP/UDP 网络埋点

fork 提供 `libavformat/network_observer.h` API 1；61.2.100 是早期 n7 接入窗口，当前 n9 依赖身份见[当前验证](validation-core-refactor.md)。进程在打开任何协议之前注册 observer，所有上下文及其后台线程关闭后才可清除/更换。回调可能并发运行，不可分配、写日志、阻塞或重入 FFmpeg。服务回调只更新固定大小的原子计数器。

覆盖 TCP recv/send；UDP 直接 recvfrom/send/sendto；UDP 后台接收和限速发送线程。后台 RX 埋点在数据进入 FFmpeg FIFO 之前。disabled 时不读时钟；回调前后保存/恢复 errno（Windows 网络构建另保存 Winsock 错误）。不改变返回值、超时或重试语义。

公开事件提供协议、方向、后台线程标志、socket fd、opaque 连接身份、请求/实际字节数或错误，以及操作开始、syscall 开始、完成的单调时间戳。TCP/UDP 直接路径的操作耗时包括显式 readiness wait；后台 UDP recv 的 syscall 本身可能等待新数据。**recv 调用耗时不是网络传播时延。**

服务按 tcp_receive/tcp_send/udp_receive/udp_send 汇总 calls/bytes/errors/would_block/EOF、后台调用数、调用均值/最大值、syscall 累计耗时、最后成功收发时刻和距今时长；提供累计及窗口值。would_block 合并可重试的 EAGAIN/EINTR。原子计数快照近似一致，窗口边界附近各字段可能跨过同一个事件。任务开始前重置这些进程级计数，多输出共用，输出协议握手/控制数据也可能计入，不能等同单输出媒体负载字节。

这是 socket API 边界，send 成功只代表操作系统接受数据，不是已上网线或已到接收端。未覆盖 connect/accept/close、内核/NIC 时间戳和非 TCP/UDP 传输；没有实现网络字节区间与视频帧的逐帧映射。原 `ingress_to_mux` 边界不因此改变。

## 封装交织的边界

FLV/TS 已支持有界 AAC/MP3 passthrough 和原连接上的晚音轨追加；MP4 仍只输出视频。pipeline owner 独占注册与写出，经 fork 的 live mux 包数/字节硬上限、单调墙钟期限和定期 poll 后，只调用 `av_write_frame`，不混用 `av_interleaved_write_frame` 的内部队列。落后于输出水位的晚音频显式丢弃并计数，不能倒退输出时间轴；详见 [直播封装](live-mux.md)。

`interleave_queue_ms` 包含实际排队，mux/write 与 AVIO flush 仍可能阻塞；每帧时延在写出/flush 后结算，不能把调用耗时解释成纯队列等待。`max_interleave_delta` 约束时间戳跨度，并非墙钟超时，不能代替 fork 的有界队列与轮询期限。可选过载策略由独立 worker 循环监视，在持续预算耗尽时请求取消；不是任意不可抢占驱动下的硬实时终止保证，见 [单路帧策略](frame-policy.md)。

## 7×24 小时的进程结构

无需传统 daemon 双重 fork。当前采用“Docker/init → 前台轻量监管入口 → 可重启的 HTTP 控制进程”，独立 worker 持有媒体连接及状态。HTTP 进程使用 spawn/exec 启动小型待命池；其异常退出不会取消转码。容器内监管入口只重启控制进程，新控制进程经私有 socket 接管原 worker，期间不重启容器、不重新打开媒体输出。详见 [进程池与接管](processes.md)。

worker 崩溃会令任务失败，保留诊断，不自动重播或重建媒体任务；待命进程退出会退避补位。stop/SIGTERM 是明确的停止意图，会停止全部 worker。容器或宿主机重启会结束媒体进程，当前没有跨容器/主机的任务持久化恢复。原生 systemd 部署也必须避免监管器在 HTTP 故障后清理整个 cgroup，否则无法接管幸存 worker。

固定多输出的预算 admission、控制 RPC 与本地日志分离、水印 typed operation 确认已经实现。多任务调度、依据媒体停滞自动恢复的 watchdog、目标机 CPU/内存/FD 容量、网络断线自动重连、任务持久化及长时间 soak 仍需另做。当前单活动任务/固定多输出的进程池不证明几百路容量或 7×24 小时可用性。

0.4.0 默认启用轻量外部崩溃采集，信号处理函数只发送固定现场；collector 限时抓栈并维护有大小/数量上限的私有 JSON 报告目录，不依赖完整 core。查看 [崩溃诊断](crashes.md) 的路径、权限、限额、导出和不完整报告语义。

Compose 的 healthcheck 检查控制 API 存活，不代表正在持续出流；普通 Compose 不会仅因 unhealthy 自动重启。容器内监管入口目前只检测控制进程退出，不检测它还活着但卡死的情况。

参考：[Docker 进程与 init](https://docs.docker.com/engine/containers/multi-service_container/)、[重启策略](https://docs.docker.com/engine/containers/start-containers-automatically/)、[local 日志驱动](https://docs.docker.com/engine/logging/drivers/local/)。
