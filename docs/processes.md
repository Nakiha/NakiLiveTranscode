# 进程池、首帧追踪与控制接管

当前 0.9.0 是 Linux 单输入、1–4 个固定逻辑输出实验服务，FLV/TS 支持有界 AAC/MP3 透传，MP4 视频限定。naki-live 使用 libevent HTTP；独立 worker 持有同任务媒体连接、共享解码及各输出 filter/encoder/mux，唯一 logger 写进程事件/trace。容器配置 Python 标准库监管入口重启异常退出的 controller；媒体/IPC 为 C。本机验证与实际镜像/远程验收须分开读取，见[合并前复查](validation-final-review.md)及对应提交的 CI 记录。

## 小型待命池

`--spare-workers` 默认 1，范围 0..4；另最多 1 个工作进程，因此最多 5 个 worker。0 表示冷启动。ready 表示已 exec、加载共享库、创建日志线程、eventfd 和私有监听端点，完成网络基础初始化。任务相关的媒体连接、codec/filter 实例在获得配置后建立，不假称已完成这些预热。

收到任务时优先分配 ready worker，立即开始补充待命数量。每个进程只处理一个任务，完成后退出，避免把旧 codec、网络和日志状态带入新任务。已存在活动任务返回 409；池暂不可用或接管未完成返回 503，没有无界任务队列。待命进程反复失败时按 1..16 秒退避补位。池状态见 `/health.pool`。

worker 通过 `posix_spawn` + exec 创建，子进程只保留标准描述符和初始 IPC fd，不继承 HTTP 监听器、状态锁或其他 worker 通道。两个二进制必须位于同一目录。工程分支接管使用 `SO_PEERPIDFD` 将 pidfd 原子绑定到已连接的 socket peer，避免读取 peer PID 后再打开数字 pidfd 的复用窗口；因此接管要求 Linux 6.5+、glibc 2.34+。本机 WSL 6.6 已确认支持，目标机器仍须验证；不支持时 fail closed，不向未验证进程发信号。

## IPC 与快照

采用非阻塞 Unix SOCK_SEQPACKET，IPC v2 的有界 JSON，单消息最多 64 KiB；传递 start/固定 job+worker instance 的 stop、standby_stop、drain/final ACK，以及 accepted/stop_ack、ready、快照、组件更新/结果和恢复分页。进程事件直接本地写入，不经 IPC 转发日志。初连 socketpair，之后私有具名 socket 接管；无 TCP IPC 端口，不传原始帧/包或 FFmpeg 指针。除 IPC 版本外，恢复还要求匹配更新/查询/result_version 能力；不兼容存活 worker 保持可见并关闭新任务，不承诺无条件滚动兼容。

worker 控制线程独立于媒体线程，约每 250 ms 上报一次完整快照。发送缓冲满时不排队堆积快照，下次重新生成最新值，`ipc_snapshots_coalesced` 计数。stop 的发送会在控制端重试，并受退出期限约束；最终快照重发直到收到确认。媒体线程只更新短锁保护的指标和有界日志队列，不等待 IPC。

`GET /v1/job` 读取最近快照，`supervisor.snapshot_age_ms` 表示控制端缓存年龄；worker 的 `last_output_age_ms`、`write_in_progress_ms` 等是生成快照时的数值，不能忽略缓存年龄。worker 自己的 `auto_stats_age_ms` 还表示媒体 owner 更新自动调度数据的年龄。控制在线但整个 worker 不再发送任何消息时，默认 5 秒判定无响应并强制结束；这和控制进程已经断开是不同状态。没有实现依据“持续无媒体输出”自动重启的策略。

worker/controller 各自 logger 写本进程 events，trace 另有前缀；stdout 仅保留 launcher 等诊断，不作为事件汇聚通道，见[事件合同](process-events.md)。控制失联期间媒体和本地日志继续，快照合并与日志丢失分开计数。退出确认前 finishing 仍占任务名额；最终可为 completed/completed_partial/stopped/failed。恢复须先重建归属与有界 operation 历史，不能先接受新任务。

## 控制进程异常退出与接管

控制进程异常退出或 SIGKILL：worker 检测到控制连接关闭，但不设置 cancel、不关闭媒体，不依赖父进程死亡信号。它继续本地媒体、统计和追踪文件轮转，等待新控制端连接。若任务在失联期间完成，worker 释放媒体资源，保留最终状态等待接管确认。不会因为失联无限缓存日志或帧。

新控制进程须使用同一 private state 位置和服务 namespace；默认由规范化 run-dir 派生。未显式 state-dir 时，绝对 TMPDIR（缺省 /tmp）下使用 naki-live-uid-hash，目录 owner 为服务用户、权限严格 0700；显式目录也须核验。运行中不得删除端点；生产迁移前须单独验证目标路径/权限/重启保留策略。

控制进程先获取独占锁，拒绝第二个管理者，然后扫描旧端点，通过 Unix peer credentials 验证 UID/PID，并打开 pidfd 监视与发送信号，避免对被复用的数字 PID 误操作。worker 仅接受同 UID 的控制连接，已有控制连接时拒绝第二条。恢复配置、任务 ID、请求 ID、原始单调时间和新快照后，才恢复新任务 admission；不会重新打开输入或输出。若发现多条活动任务冲突，会停止 admission，保留媒体供人工处理。

接管是重新建立控制权，不是把活进程的内核 parent PID 改成新控制进程。新控制端通常不能 waitpid 回收旧进程或读取其精确退出码，因此使用 pidfd 判断退出；这类任务 `supervisor.recovered=true`、`exit_status_known=false`，不会伪造退出码。最终任务状态来自 worker 协议，异常失联/死亡则报告失败。回收由系统 init 或容器 init 负责。

明确的 API stop 或服务 SIGTERM 则停止任务；默认 `--worker-stop-timeout 3` 秒后仍未退出会 SIGKILL，输出和日志可能不完整。不要用 SIGTERM 模拟“控制崩溃后继续转码”。裸机手动重启应保持相同参数和工作目录。systemd 默认清理整个 cgroup 的行为可能结束幸存 worker，需与运维停止/重启策略配套；本仓库没有自动安装该配置。

容器中的监管入口只重新拉起异常退出的 HTTP 进程，避免触发容器级清理。完整容器/宿主机重启仍会结束所有 worker；当前没有磁盘任务数据库或跨主机迁移。监管入口只检测进程退出，尚不检测 HTTP 活着但完全卡死。

监管入口区分 running/backoff/draining/exited；即使 SIGTERM 到达退避窗口，也启动一次 `--drain-only` 控制进程连接已有端点并有界清理，不监听 HTTP、不补 standby、不接受任务。重复信号不重置绝对期限。启动时校验 `worker-stop-timeout + 2秒 < NAKI_LAUNCHER_HARD_TIMEOUT < NAKI_CONTAINER_STOP_GRACE`，后两者默认20/30秒；容器实际 grace 必须与该声明一致。不兼容或无法验证的遗留进程会使 drain 报告失败，不能靠裸 PID 强杀。

崩溃摘要、core 收集与符号化的当前边界见 [C 崩溃诊断](crashes.md)。

0.4.0 在 HTTP 和每个 worker 旁各预启动一个独立 C collector，不计入上述待命 worker 数量。报告只保存限额现场/调用栈，不默认生成完整 core；抓栈子进程仅在故障发生时临时创建。监管入口按本轮假设不做自身崩溃注入。

## 每个任务额外记录首帧

可传 `X-Request-ID`（最多 128 个 ASCII 字母、数字、下划线或连字符）关联单次 HTTP 尝试，未提供时生成随机 128 位 ID，非法值返回 400。创建关联另存 create_request_id；创建和停止的业务重试使用 operation_id，详见 [服务接口](service.md)。`request_received` 记录完整 HTTP 请求进入业务回调的时刻；此时 HTTP 库已经收完请求体，ACK 等待不会重置它。

不论 `frame_log` 开关，每个输出都尝试记录 `first_frame`：首个输出视频包一条；若首个输出关键帧不是同一包，再记录一条。事件包含请求、dispatch、worker 接单、该帧包 ingress、解码提交/完成、filter 提交/完成、编码提交/完成、mux 开始和 flush 完成的单调时间、分段耗时、总耗时以及帧类型/关键帧分类。每输出最多两个 checkpoint 保存在 worker 内存中，随状态快照在接管后查询；本地 first_frame 事件不因重连重放，没有 `replayed_after_reconnect` 字段。常规日志仍受有界队列丢失约束。

最终可用快照仍未观察到首帧时输出 `startup_incomplete`，包含已有快照、原因和请求到退出时间。原生崩溃前可能有尚未上报的输出，不能据此断言绝对没有发送过媒体；需结合首帧日志和快照年龄。`startup` 摘要包含：

| 字段 | 边界 |
| --- | --- |
| `request_to_dispatch_ms` | 完整 HTTP 请求进入业务回调 → 配置成功发送，包含校验、等待 ready、冷进程创建 |
| `dispatch_to_worker_ms` | 控制端发送前 → worker 解析配置后接单，包含 socket/调度及解析开销 |
| `worker_boot_ms` | spawn 前 → worker 基础初始化完成；待命时这段发生在请求之前，不再加进请求总耗时 |
| `request_to_first_video_mux_ms` | 请求回调 → 首个视频包的 interleaved write 和显式 AVIO flush 返回成功 |
| `request_to_first_key_mux_ms` | 请求回调 → 首个输出 KEY 包完成同样写入/flush |

未到达边界时首帧字段为 null。首包可能含封装头，首个视频包或 KEY 包写入不代表接收端拿到可显示画面；UDP 后台限速发送仍可能在 AVIO 返回后发出。因此不能把它命名为网络 wire-time 或客户屏幕首帧。TCP/UDP observer 仍单独观测 socket 边界，尚无媒体字节与首帧跨层关联。客户发出请求到服务收完整请求这段，以及接收端缓冲、解码、显示，需要客户端配合测量，不能靠不对时的客户端墙钟直接相减。

## 几百个 worker 的后续工作

当前实现只面向上述小池，控制端以 10 ms 定时器有界扫描，完整快照约 4 Hz；没有宣称几百路容量。扩容应先改成每个连接/pidfd 的事件监听，拆分控制与日志/指标通道，心跳只携带小型进度摘要、完整指标低频或按需提供，限制总队列内存与每轮处理预算，并错开上报/重连时间。

watermark 已接入带 job/output/operation 身份的[组件更新](component-updates.md)，由媒体 owner 结算应用，并在实际 write/flush 后另记 first_written。控制连接恢复时先交换归属/能力，再按每页最多 8 项对账操作结果，完成后开放新写入；周期快照投影当前 desired/effective。worker 正常结束前同样先导出结果。worker 死亡而结果未确认时，缓存查询显式返回 unknown/last_observed，不把旧 pending 当作仍在执行，也不推断 not_written。IPC 保持有界 JSON，不携带帧、包或原生指针；其他动态媒体命令仍需单独设计。

参考：[Unix socket](https://man7.org/linux/man-pages/man7/unix.7.html)、[epoll](https://man7.org/linux/man-pages/man7/epoll.7.html)、[pidfd_open](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)。
