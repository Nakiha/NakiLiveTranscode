# Linux 单输入转码实验服务

当前版本 0.9.0，一个活动输入、1–4 个固定逻辑输出；能力/草案索引见[文档导航](README.md)，最近的源码绑定结果见[核心整理验证](validation-core-refactor.md)。本页描述当前 v1 合同，历史验证窗口不自动成为当前候选验收。

## 依赖和构建

应用为 C11，仅支持 Linux；Windows 本机通过 WSL Ubuntu 开发和验证。

- JSON：[yyjson 0.13.0](https://github.com/ibireme/yyjson/releases/tag/0.13.0)，固定子模块，MIT。
- HTTP 服务端：[libevent evhttp](https://libevent.org/)，系统开发包，BSD 系许可。使用发行版安全更新，不按上游版本号单独判断是否包含回补。
- HTTP 客户端：[libcurl](https://curl.se/libcurl/)，系统开发包，curl 许可。
- 媒体：FFmpeg fork API 2，链接自定义共享库；H.264 编码使用 libx264，VVC 编码使用独立子模块 VVenC v1.14.0（内部为 C++，通过 FFmpeg 的 C API 调用）。
- 外部崩溃抓栈：系统 libunwind-ptrace；主服务、worker 和 collector 均为 C。

Ubuntu / WSL：

```sh
sudo apt-get update
sudo apt-get install build-essential cmake pkg-config nasm libx264-dev libsrt-openssl-dev libssl-dev zlib1g-dev libevent-dev libcurl4-openssl-dev libunwind-dev
git submodule update --init --recursive
bash scripts/build-service.sh
```

构建目录默认在 Linux 文件系统 `$HOME/.cache/naki-live-transcode-n9/`，可用 `NAKI_BUILD_ROOT` 指定另一目录。构建先安装 VVenC 共享库，再构建 FFmpeg 和服务。二进制在项目 `out/bin/`，RPATH 指向该构建前缀，不混用系统 FFmpeg。迁移部署时需在目标系统重建。

`naki-live`、`naki-live-worker` 和 `naki-crash-collector` 必须放在同一目录。控制进程和每个 worker 各预启动一个独立 collector；故障发生时才临时派生抓栈进程。默认不生成完整 core，报告位置由 `/health.crash_report_dir` 给出，限时与配额参数见 [崩溃诊断](crashes.md)。

此构建启用 GPL 的 libx264，生成的 FFmpeg 库按 GPL 条件处理；分发整体程序前应按实际链接方式确认适用许可。保留各依赖的原始许可文件。HTTP 客户端不参与媒体转码，FFmpeg 处理媒体协议。

## 运行与 API

从项目根目录运行：

```sh
./out/bin/naki-live --bind 127.0.0.1 --port 8090 --run-dir out/runs
./out/bin/naki-live-ctl health
./out/bin/naki-live-ctl start examples/file.json
./out/bin/naki-live-ctl status
./out/bin/naki-live-ctl stop
```

`naki-live-ctl --url http://127.0.0.1:8090 status` 可指定服务地址。服务默认仅监听回环地址，无身份认证；这是本机实验控制接口。每次只允许一个运行中的任务，完成后保留最后一个任务的状态；新任务替换它。日志始终保留。

| 方法和路径 | 行为 |
| --- | --- |
| `GET /health` | 能力查询；handler 可响应时 200，另含 liveness/readiness/admission，不证明媒体输出正常 |
| `GET /health/live` | 控制器 event loop 能应答，返回 200 |
| `GET /health/ready` | 控制归属已协调且 active worker 控制通道可用/新鲜；200 或带原因的 503，busy 不单独关闭 ready |
| `GET /health/admission` | 新建任务是否可尝试；busy/恢复/保留容量/退避等可返回 503，不预留容量 |
| `POST /v1/jobs` | 调用者 operation_id + spec；worker ACK 后 202，保留相同意图可重试，冲突 409，不确定结果 503 |
| `GET /v1/jobs/{job_id}` | 当前或仍保留的指定任务，未知/过期 404 |
| `POST /v1/jobs/{job_id}/stop` | 调用者 operation_id；worker 确认停止意图后 202，不等于已 stopped；旧目标不作用于新任务 |
| `POST /v1/jobs/{job_id}/updates` | watermark typed patch、desired revision CAS；接受/仍在应用 202，终结重放 200，预算拒绝 429 |
| `GET /v1/jobs/{job_id}/operations/{operation_id}` | 有界创建/停止/更新结果，检查 outcome_known、result_source 和 delivery；查询 200 不保证已写出 |
| `POST /v1/job` | 兼容裸 spec，worker ACK 后 202；controller 生成创建操作 ID，忙时 409，恢复/不可用 503 |
| `GET /v1/job` | 当前/最后任务：idle / starting / waiting_assets / running / stopping / finishing / completed / completed_partial / stopped / failed |
| `POST /v1/job/watermark` | 兼容完整 revision 替换，必须 job_id；202 为 worker 接受，仍需查询 applied/失败；见[水印](watermark.md) |
| `POST /v1/job/stop` | 必须 job_id，可选 operation_id；首次 202 只确认 controller 的停止意图，不能当作 worker ACK；重放可返回保留的停止操作结果 |

`POST /v1/jobs` 请求为 `{"operation_id":"create-example","spec":{...}}`。首次 202 是 JobStatus；已接受的创建重试返回 creation operation 投影，job_id 指向原 owner。超时可能已执行，应保留原 operation_id 与相同 spec 查询/重试，不能换新 ID 猜测重建。同一规范化 spec 返回原 job_id，改变 spec 返回 409。更新的范围、写出边界、有限保留及未知结果语义见[组件更新](component-updates.md)，设计草案中的其他组件字段仍不接受。

JSON 请求须无重复键/NUL，ID 为有界 ASCII 字母、数字、下划线或连字符。job_id 最长 63 字节，operation_id/request_id 最长 128 字节。创建 body 上限 16384 字节，更新/兼容水印 8192 字节，停止 1024 字节；header 上限 8192 字节，HTTP timeout 10 秒。URI 精确匹配，不承诺 query string 或尾斜线；兼容已知路径错误 method 返回 405，部分 canonical 错误 method 落入 404。libevent framing/上限错误和分配失败可能返回 HTML；应用 JSON 错误有 event/error/error_code，不能假定所有非 2xx 均是相同 JSON。

`POST /v1/jobs/{job_id}/stop` 请求为 `{"operation_id":"stop-example"}`，等待 worker 停止意图确认后返回 202。旧 stop/watermark 写接口也必须在请求体带明确的 `job_id`；CLI 会先读一次 current，再固定目标发送，失败时不会改写新任务。request_id 每次 HTTP 尝试独立，创建时的关联 ID 保存在 create_request_id，操作保留 origin_request_id。

创建索引属于当前固定服务命名空间，可用 `--namespace` 显式配置；默认由规范化 private state 目录派生。接管时从存活 worker 的 assignment 重建索引；瞬时连接失败和不兼容存活 worker 均保持接单关闭。当前控制器最多保留 32 个创建结果，每项终态 snapshot 不超过 64 KiB；终态至少保留 30 分钟，容量耗尽时先拒绝新建，每任务预留一个停止结果。它是有界内存重试保证，全部 owner 和历史丢失后不提供宿主机崩溃持久性。

单路和多路均使用 `live-4k-v1` profile：输入/输出像素面积不超过 4096×2160，输入 packet payload 不超过 8 MiB，reader ring 保留字节不超过 64 MiB。reader 与 consumer 各可另持一个不超过 16 MiB 的有界 item；这些计费上限不等于进程 RSS 上限。8192×8192 等旧单路 canvas 现在在静态校验阶段拒绝。

参数示例见 `examples/`。`input`、`output` 必填；相对路径以服务进程的工作目录为准。本地输出使用排他创建，已有文件不会被覆盖；失败或停止时可能保留部分输出。输出父目录需预先存在。

机器可读接口规范见 [openapi.json](openapi.json)，包括全部当前路径、请求参数、范围和状态码。它是仓库静态文档，服务尚不提供 `/openapi.json` 或 Swagger UI。未来的有序滤镜、ROI 分析与颜色处理见 [处理链设计草案](pipeline-design.md)；草案字段当前会被拒绝。

已加入可选的 `timestamps`、`intra` 和 `frame_selection`，见 [单路帧策略](frame-policy.md) 与[该功能历史验证](validation-frame-policy.md)；当前整理候选的验证范围见[核心整理验证](validation-core-refactor.md)。未提供这三组时保留 legacy 路径。`source_all` 只承诺 demux 后显示时间轴的换算与容器量化，缺失/重复/回退明确失败；source_i_anchors、自动时间戳修复仍未实现。当前 SEI 按显式选择的类型有界透传，见 [SEI 合同](sei.md)；完整后续合同见 [直播帧策略设计](live-frame-policy.md)。

内网业务可编译接入 [内部媒体 hook](media-hooks.md)，观察音频和编码后的视频包；默认空实现，没有新增 hook HTTP 参数或响度检测算法。显式选择的源 SEI 透传由内建媒体链实现，不依赖私有 hook，也不等于客户任意 SEI 注入 API。

`width`、`height` 同时为正偶数时缩放；同时为零/省略则由首帧确定并固定输出画布。libx264 使用 YUV420P，libvvenc 使用 YUV420P10LE。`bitrate` 默认 1.5 Mbps；`encoder_threads` 默认 2。`max_threads` 默认 12，自动模式 `min_threads` 默认 2，`budget_slots` 默认 12；预算只约束解码任务，不含 filter 和编码器。`queue_capacity` 默认 4，此外有 8 个包的输入 ring。`fps` 是名义帧率不可用时的回退值，不做帧率转换；已知有效分数帧率优先，见[帧率口径](video-rate.md)。`pace=true` 仅用于本地文件按 DTS 实时回放。

`outputs` 接受 1–4 项，不能与顶层 output/watermark 同时出现。单元素默认 id=main，也允许显式 id，归一化为直接媒体路径，状态不增加 outputs 数组；2–4 项必须显式唯一 id。每路参数继承和预算见[固定输出合同](branching-contract.md)。

本版转码第一条可用视频流：原生 H.264/HEVC 软解 → null/scale + format filter → 编码 → 输出。默认 `encoder=libx264`，`encoder_preset=veryfast`，YUV420P、zerolatency、无输出 B 帧，支持 MPEG-TS / MP4 / FLV。选择 `encoder=libvvenc` 时默认 `encoder_preset=faster`、`encoder_profile=live_3b_2s`，强制 YUV420P10LE，支持 MPEG-TS / MP4 / Enhanced FLV；完整组为 I BBB P…，关键帧按首帧 PTS 锚定的 2 秒时间网格调度，边界及 EOF 允许 IP / IBP / IBBP。保留单遍码控前瞻与必要重排，单调变帧率及向前时间戳缺口不再因非 CFR 被拒绝；回退或重复 PTS 显式失败。其 preset 可用 faster/fast/medium/slow/slower，非法组合返回 400。详见 [VVenC](vvenc.md)。配置不接受未知或重复字段；请求体上限 16 KiB。

`audio_copy` 默认为 true：FLV/MPEG-TS 透传 AAC/MP3，晚出现的音轨直接注册到已打开的输出；MP4 仍只输出视频。上限 16 条输入音轨状态，非支持编码及参数变化拒绝该音轨并计数，视频继续。字幕和数据流不输出。过期/重复/无法恢复时间戳的音频显式丢弃并计数，不改写视频时间轴。`audio_copy=false` 禁用音频。`mux_wait_ms` 默认 50，范围 1..1000，控制有界墙钟交织排队，详见 [动态音轨](live-mux.md)。

当前构建包含 file、UDP、TCP、RTP、HTTP、RTMP、RTSP、SRT 相关能力，不包含 HTTPS 或硬件编解码。RTMP 输出必须选择 flv，SRT 输出必须选择 mpegts；两者的 VVC 实流及晚音轨已纳入测试，其余协议的编译可用不等于均已实流验收。输入分辨率及 yuv420p/yuv420p10le 变化可重建滤镜适配固定输出；encoder、连接、时间基准和输出规格保持。primaries/transfer 变化及其他未支持格式明确失败，不做 HDR tone mapping。固定 2–4 路接口、独立预算/失败终态和 output_id 水印热更见[本批合同](branching-contract.md)。

## 进程、线程与停止

HTTP 控制进程只管理任务，通过 Unix SOCK_SEQPACKET 与独立 worker 传递有界控制/状态/日志 JSON，默认预留 1 个空闲 worker。共享媒体 owner 独占 async frontend；每输出 owner 独占其 filter、编码器和 muxer。reader 独占读取并快照音频参数，通过有界队列交付引用；mux owner 不读可变 input stream 数组。FFmpeg 后台协调线程执行自适应调度。通知回调只写 nonblocking eventfd，不操作 codec 或 filter。线程归属与[模块导航](../app/README.md)、[进程池与接管](processes.md)一致。

输入队列满时阻塞生产者；async 队列满时保留 pending packet 等待通知，不丢参考帧、不无限扩容。满载时真实 UDP 的发送端无法接收反压，仍可能发生内核/网络丢包；`overrun_nonfatal=0` 选择显式错误而非静默继续。

正常 EOF 依次排空 decoder、filter、encoder、延后音频及 live mux，再 finalize。stop / 服务 SIGTERM 取消输入和队列等待、释放后台线程并尽力写 trailer，不保证交付全部在途帧。输入读取/探测使用 10 秒中断期限；每输出打开/写入/finalize 默认 5 秒，outputs 可配置 100–10000ms。DNS/内核/文件系统和单次不可抢占库调用不保证立即中断。控制侧默认 3 秒停止/退出期限可更早强制结束 worker，此时 trailer、日志和部分输出不保证完整。退出确认前保持 active，不允许重叠新建。控制器崩溃/SIGKILL 保留 worker 继续媒体，重启先核实并接管；服务 SIGTERM 则有意停止全部 worker。

`output_finalize` 分别记录开始/结束/截止及 primary/trailer/flush/close 错误。已有媒体错误优先；媒体成功且未停止时按 trailer→flush→close 提升首个错误；用户停止仍记录清理故障而保留停止分类，连续过载仍按原失败规则处理。hook 只关闭一次，媒体引用回收后发布终态。可返回的分支错误允许健康兄弟继续，根任务可为 completed_partial；这不隔离 worker 原生崩溃。

## JSONL 指标的边界

worker 默认每 10 秒生成 status，由自身 logger 写进程事件文件；controller 空闲 heartbeat 和生命周期事件写自己的进程事件文件，路径在 run-dir/events。首帧 checkpoint 保留供查询，接管不重放相同 worker 事件。逐帧明细默认关闭，启用 frame_log 后写独立轮转 trace。HTTP 查询读取约每 250 ms 的媒体快照，operation 查询通过有界 RPC 取得 worker 权威结果；重连另发送一次归属信息和操作分页。详见[进程事件](process-events.md)、[组件更新](component-updates.md)及[运行观测](operations.md)。

使用单调时钟计算耗时，另有 `unix_us` 便于关联日志。关联通过 packet opaque_ref 传递到 decoded/filter frame，再按 encoder PTS 匹配输出包；关联缺失、PTS 不递增或有界映射溢出会明确失败。

| 字段 | 起止边界 |
| --- | --- |
| `demux_read_ms` | 本次取包开始 → 有效包返回，含 resumable EAGAIN/poll 等待和网络等待 |
| `input_queue_ms` | reader 记录视频包 ingress（可选 pace/追踪准备后、等 ring 前）→ async 接受，含 ring 等位和 pending 重试 |
| `decode_ms` | async 接受压缩包 → 对应完整解码帧返回，包含后台排队、依赖与 B 帧重排 |
| `setup_or_dispatch_ms` | 解码帧返回 → 送 filter；首帧包含 filter/编码器/输出初始化 |
| `filter_ms` | 送 filter → 取出对应滤镜结果 |
| `encode_ms` | 送编码器 → 对应输出包返回，包含计算和编码器排队 |
| `mux_ms` | 编码包返回 → 交织排队 → 封装写入及 avio_flush 完成 |
| `interleave_queue_ms` | 编码包返回 → 开始实际封装写入，逐帧日志字段 |
| `ingress_to_mux_ms` | 上述视频包 ingress → 对应转码包完成本地 mux/AVIO flush；不含 ingress 之前的探流/pace/追踪准备 |
| `scheduled_to_mux_ms` | 文件回放计划到达时间 → 写出，只有 pace=true 时出现，暴露反压积压 |

`ingress_to_mux_ms` 是**服务内部端到端时延**，不是源端采集到播放器显示时延。它不含收到包之前的网络/探测缓冲，也不代表接收端已收到或显示；写文件完成不等于 fsync 持久化。`decoder_ready.startup_ms` 从完整 HTTP 请求进入业务回调开始，截止 async decoder 初始化；完整启动耗时看 `startup.request_to_first_video_mux_ms` / `request_to_first_key_mux_ms` 和无条件输出的 `first_frame` 事件。它们包含校验、进程等待/创建、IPC、输入打开探测和首帧各阶段，仍不包含 HTTP 上传之前/期间或接收端显示。摘要对已经输出的帧统计；失败或取消时未输出帧不进入耗时均值。

时延统计覆盖每个成功输出视频帧，不受逐帧明细采样或日志队列丢弃影响；输入 I/首 I/后续 I、decoded key 和 output key 分开，窗口不重叠。日志队列满时丢弃日志并计数，不等待日志 I/O；锁外准备仍有并发/输入/scratch 上限，见[事件合同](process-events.md)。`bytes_out` 是所有音视频 AVIO 累计字节，`encoded_bytes` 是单包视频载荷；`packets_in/out` 保持视频口径，音频另有 audio_packets_out。interleave_call_ms 从编码包返回到 av_write_frame（含 TS PES flush）返回，包含交织排队；interleave_queue_ms 单列实际写前等待，avio_flush_ms 单列其后 AVIO flush，不能把这些重叠字段相加。TCP/UDP socket API 观测不与逐帧指标混用，也不覆盖 libsrt 内部 UDP socket。

## 验证

统一入口为 `bash scripts/test-service.sh quick`、`bash scripts/test-service.sh full` 和 `bash scripts/test-sanitizers.sh`。下面的专题脚本仍可单独运行；CTest 只覆盖原生测试，不代表整个服务已验收。依赖、间接调用关系与本轮基线见 [测试入口与基线](testing.md)。

`python3 tests/live_mux_integration.py` 验证真实 RTMP、SRT 双向连接、VVC 独立解码、两条晚音轨、音频停顿恢复、未知晚数据流、交织墙钟/容量上限、MP3 透传、EOF 排空及关闭音频。见 [本轮记录](validation-live-mux-20261017.md)。

`python3 tests/service_integration.py` 自动启动独立服务、生成合成输入并检查文件完整转码、HTTP 错误处理、停止、坏输入、已有输出保护，以及 UDP 输入。每次使用新的 `out/` 子目录，不覆盖冻结基线。测试需要系统 `ffmpeg`/`ffprobe`（仅用于生成和独立检查媒体）以及 Python 3；运行前先构建服务。

当前功能候选、逐批源码/SDK 身份与未验证项见[核心整理验证](validation-core-refactor.md)。[2026-10-16 验证记录](validation-20261016.md)及下列专题记录保留其各自历史窗口，不代表最新工作树已在相同环境重跑。

新增日志/网络测试：`python3 tests/observability_integration.py`，覆盖 10 秒默认定时器、窗口计数、I 帧、轮转、压缩清理、队列饱和、阻塞输出期间心跳以及 TCP/UDP 收发。

新增进程测试：`python3 tests/process_integration.py`，覆盖待命补位、原生崩溃隔离、超时回收、控制失联期间继续转码、同 PID 接管、离线完成接管和首帧链路。结果见 [进程版本验证记录](validation-processes-20261016.md)。

轻量崩溃测试：`python3 tests/crash_integration.py`，覆盖真实非法访问、线程/栈溢出、持锁故障、超时与权限降级、限额、worker/HTTP 崩溃报告和接管。见 [崩溃采集验证记录](validation-crashes-20261016.md)。

[新增验证记录](validation-observability-20261016.md)；容器测试为 `python3 tests/docker_smoke.py`（先构建镜像）。
