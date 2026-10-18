# Linux C 服务模块与所有权

当前是单输入、1–4 个固定逻辑输出的实验服务。第四批按职责搬移，未增加模块线程、通用插件框架或 IPC 媒体通道。当前验证见[核心整理验证](../docs/validation-core-refactor.md)。

- `service.c`：启动、参数、libevent 注册及退出编排。
- `service_http.c`：HTTP 路由、JSON 校验/响应与有界待确认请求。
- `service_projection.c`：状态/健康/freshness 与操作投影，不重置 worker 指标窗口。
- `service_records.c/.h`：32 项有界创建/终态记录、活动/待 RPC 记录保护和缓存释放。
- `service_supervisor.c`：worker spawn、peer/pidfd 核验、恢复、IPC ACK/结果协调、轮询与停服。
- `service_internal.h`：controller 私有状态及接口，同一个 libevent 线程拥有。
- `worker.c` / `ipc.c`：worker 控制、媒体启动回收与有界 SOCK_SEQPACKET JSON；控制器失联不取消媒体。
- `operation_store.c` / `operation_cache.c` / `component_update.c`：worker 权威结果/pin、controller 版本缓存、typed patch/CAS/规范化意图。
- `pipeline.c`：共享输入/async frontend、滤镜送取、音频策略与分支编排。
- `pipeline_reader.c/.h`：唯一 demux reader、8 项/64 MiB ring、音频参数快照和 SEI obligation；提供 init/start/pop/wake/join/clear/destroy 边界。
- `pipeline_output.c`：每输出初始化、正常 drain、finalize、引用释放及终态；保留主错误，分别记录 trailer/flush/close。
- `pipeline_encode.c`：每输出 encoder send/receive、256 项 PTS ledger 及 delivery 引用转交；缺映射/满额显式失败。
- `pipeline_mux.c`：唯一 mux owner 的有界 live mux → av_write_frame → flush → 指标/首帧/写出结算；迟到音频丢弃计数。
- `pipeline_internal.h`：worker 私有 Stamp/Pipeline/Branch 与接口，不把原始帧/包放入 IPC。
- `input_probe.c` / `input_probe.h`：带缓存的首条 H.264/HEVC 视频就绪探测；其他音轨/数据可继续晚发现，不在初始化阶段固定完整流集合。
- `frame_policy.c` / `frame_policy.h`：纯策略核心，包含严格时间轴、接受后推进的 I 调度、因果选择和有界在途身份。
- `frame_policy_config.c` / `frame_policy_config.h`：可选帧策略组的严格 JSON 合同；缺省兼容 legacy，过载阈值必须显式提供。
- `frame_policy_runtime.c`：worker 独立轮询、在途年龄和过载失败；媒体 owner 仍独占 filter/encoder/mux。
- `watermark_config.c` / `watermark.c`：单水印严格配置、后台有界资源准备、required 门槛及 SDR 8/10-bit 合成；资源线程不接触主媒体 graph/encoder/mux。
- `common.c` / `live.h`：yyjson 配置、状态/指标 JSON 和共享助手；JobLifecycle/JobProgress/JobStats/JobFlow 分组，句柄/共享引用仍由 Job 持有。
- `job_state.h`：内部枚举与原有 wire string 映射，不增加状态转换或 HTTP 字段。
- `logger.c` / `logger.h`：独立日志线程、有界队列、丢弃计数和追踪文件轮转。
- `netstats.c` / `netstats.h`：FFmpeg TCP/UDP observer 的无日志原子计数与报表。
- `client.c`：libcurl 命令行 HTTP 客户端。

构建、使用及测量边界见 [服务说明](../docs/service.md)。

固定多输出：output_config.c 解析 outputs 与预算；branch_queue.c 持有有界帧/音频参数引用；video_filter.c 准备独立滤镜图。共享输入/解码，各分支 owner 独占 graph/encoder/mux，在固定输出下适配已支持的输入变化。可返回的分支故障只取消该路；共享输入错误、用户停止或 worker 原生崩溃仍可能终止全部输出。见[固定输出合同](../docs/branching-contract.md)。

reader 只传参数快照和引用，mux owner 不读取可变 input streams。关闭输入前先唤醒并 join reader，再释放 async；分支先 abort/finish queue，join 后销毁，finalize/hook 与媒体引用释放保持原顺序。队列使用自身 mutex，发布 Job 状态前先释放队列锁；Job mutable groups 沿用 Job.mutex，cancel/done/branch_failed 使用 atomics，根快照按原有 parent → child 顺序读取。不得新增反向锁序，或在网络 observer/fatal 回调中加应用锁。

CMake 的 NAKI_PIPELINE_SOURCES 同时供生产 worker 和故障 helper 使用；包装器只进入 naki-live-hook-worker。日志 I/O 由 logger 独占，网络回调只作原子观测，fatal handler/独立 collector 边界保持，见[事件](../docs/process-events.md)、[健康](../docs/health-progress.md)、[崩溃](../docs/crashes.md)。
