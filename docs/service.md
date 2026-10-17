# Linux 单输入转码实验服务

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

`naki-live`、`naki-live-worker` 和 `naki-crash-collector` 必须放在同一目录。0.4.0 为控制进程和每个 worker 各预启动一个独立 collector；故障发生时才临时派生抓栈进程。默认不生成完整 core，报告位置由 `/health.crash_report_dir` 给出，限时与配额参数见 [崩溃诊断](crashes.md)。

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
| `GET /health` | 版本与健康状态 |
| `POST /v1/job` | JSON 启动一个任务，返回 202；忙时 409；接管中或 worker 不可用时 503 |
| `GET /v1/job` | idle / starting / waiting_assets / running / stopping / finishing / completed / stopped / failed，计数与耗时摘要 |
| `POST /v1/job/watermark` | 单水印完整 revision 热更；202 仅代表 worker 接受，查询状态确认 applied 或失败；见[接口边界](watermark.md) |
| `POST /v1/job/stop` | 取消运行中的任务，返回 202；通过 status 等待 stopped |

参数示例见 `examples/`。`input`、`output` 必填；相对路径以服务进程的工作目录为准。本地输出使用排他创建，已有文件不会被覆盖；失败或停止时可能保留部分输出。输出父目录需预先存在。

机器可读接口规范见 [openapi.json](openapi.json)，包括全部当前路径、请求参数、范围和状态码。它是仓库静态文档，服务尚不提供 `/openapi.json` 或 Swagger UI。未来的有序滤镜、ROI 分析与颜色处理见 [处理链设计草案](pipeline-design.md)；草案字段当前会被拒绝。

工作区已加入可选的 `timestamps`、`intra` 和 `frame_selection`，见 [单路帧策略](frame-policy.md) 与[本地验证](validation-frame-policy.md)，尚待独立源码复审。未提供这三组时保留 legacy 路径。`source_all` 只承诺 demux 后显示时间轴的换算与容器量化，缺失/重复/回退明确失败；source_i_anchors、自动时间戳修复和任意 SEI 透传仍未实现。完整后续合同见 [直播帧策略设计](live-frame-policy.md)。

内网业务可编译接入 [内部媒体 hook](media-hooks.md)，观察音频和编码后的视频包；默认空实现，没有新增 HTTP 参数，也没有启用响度检测或 SEI 注入。

`width`、`height` 同时为正偶数时缩放；同时省略则保持输入尺寸，但转换为 YUV420P。`bitrate` 默认 1.5 Mbps；`encoder_threads` 默认 2。`max_threads` 默认 12，自动模式 `min_threads` 默认 2，`budget_slots` 默认 12；预算只约束解码任务，不含 filter 和编码器。`queue_capacity` 默认 4，此外有 8 个包的输入队列。`fps` 是缺失帧率时的回退值；不做帧率转换。`pace=true` 仅用于本地文件按 DTS 实时回放。

本版转码第一条可用视频流：原生 H.264/HEVC 软解 → null/scale + format filter → 编码 → 输出。默认 `encoder=libx264`，`encoder_preset=veryfast`，YUV420P、zerolatency、无输出 B 帧，支持 MPEG-TS / MP4 / FLV。选择 `encoder=libvvenc` 时默认 `encoder_preset=faster`、`encoder_profile=live_3b_2s`，强制 YUV420P10LE，支持 MPEG-TS / MP4 / Enhanced FLV；完整组为 I BBB P…，关键帧按首帧 PTS 锚定的 2 秒时间网格调度，边界及 EOF 允许 IP / IBP / IBBP。保留单遍码控前瞻与必要重排，单调变帧率及向前时间戳缺口不再因非 CFR 被拒绝；回退或重复 PTS 显式失败。其 preset 可用 faster/fast/medium/slow/slower，非法组合返回 400。详见 [VVenC](vvenc.md)。配置不接受未知或重复字段；请求体上限 16 KiB。

`audio_copy` 默认为 true：FLV/MPEG-TS 透传 AAC/MP3，晚出现的音轨直接注册到已打开的输出；MP4 仍只输出视频。上限 16 条输入音轨状态，非支持编码及参数变化拒绝该音轨并计数，视频继续。字幕和数据流不输出。过期/重复/无法恢复时间戳的音频显式丢弃并计数，不改写视频时间轴。`audio_copy=false` 禁用音频。`mux_wait_ms` 默认 50，范围 1..1000，控制有界墙钟交织排队，详见 [动态音轨](live-mux.md)。

当前构建包含 file、UDP、TCP、RTP、HTTP、RTMP、RTSP、SRT 相关能力，不包含 HTTPS 或硬件编解码。RTMP 输出必须选择 flv，SRT 输出必须选择 mpegts；两者的 VVC 实流及晚音轨已纳入测试，其余协议的编译可用不等于均已实流验收。输入分辨率及 yuv420p/yuv420p10le 变化可重建滤镜适配固定输出；encoder、连接、时间基准和输出规格保持。primaries/transfer 变化及其他未支持格式明确失败，不做 HDR tone mapping。固定 2–4 路接口、独立预算/失败终态和 output_id 水印热更见[本批合同](branching-contract.md)。

## 进程、线程与停止

HTTP 控制进程只管理任务，通过 Unix SOCK_SEQPACKET 与独立 worker 通信，默认预留 1 个空闲 worker。worker 的媒体线程独占 async frontend、filter、编码器和 muxer；输入线程独占解封装读取，通过固定容量队列交付包。FFmpeg 后台协调线程执行自适应调度。通知回调只写 nonblocking eventfd，不操作 codec 或 filter。详见 [进程池与接管](processes.md)。

输入队列满时阻塞生产者；async 队列满时保留 pending packet 等待通知，不丢参考帧、不无限扩容。满载时真实 UDP 的发送端无法接收反压，仍可能发生内核/网络丢包；`overrun_nonfatal=0` 选择显式错误而非静默继续。

正常 EOF 依次排空 decoder、filter、encoder，再写 trailer。stop / 服务 SIGTERM 会取消输入和队列等待、释放后台线程并尽力写 trailer；不保证交付取消时所有在途帧。网络操作设置 10 秒中断期限，DNS/内核/文件系统等阻塞仍可能不支持立即中断。控制进程另有默认 3 秒退出期限，超过后强制结束 worker；此时 trailer、日志和部分输出不保证完整。退出得到确认前保持任务 active，不允许重叠创建新任务。控制进程崩溃或 SIGKILL 则保留 worker 继续媒体处理，重启后接管。

## JSONL 指标的边界

worker 默认每 10 秒生成 status，经其日志线程和非阻塞 IPC 交给控制进程的有界日志线程写 stdout；空闲时由控制进程发 heartbeat。生命周期、首帧链路及最终状态也写 stdout。逐帧明细默认关闭，可用任务 `frame_log=true` 开启，文件由 worker 日志线程按大小/时间轮转。HTTP 查询读取 worker 每 250 ms 上报的快照，包含累计及窗口耗时、分位数上界、I 帧拆分、自动调度、日志/网络计数和快照年龄。详见 [运行观测与长期运维](operations.md)。

使用单调时钟计算耗时，另有 `unix_us` 便于关联日志。关联通过 packet opaque_ref 传递到 decoded/filter frame，再按 encoder PTS 匹配输出包；关联缺失、PTS 不递增或有界映射溢出会明确失败。

| 字段 | 起止边界 |
| --- | --- |
| `demux_read_ms` | 对应压缩包一次 av_read_frame 调用，可能包含等待网络数据 |
| `input_queue_ms` | 压缩包进入服务队列 → 被 async 接受 |
| `decode_ms` | async 接受压缩包 → 对应完整解码帧返回，包含后台排队、依赖与 B 帧重排 |
| `setup_or_dispatch_ms` | 解码帧返回 → 送 filter；首帧包含 filter/编码器/输出初始化 |
| `filter_ms` | 送 filter → 取出对应滤镜结果 |
| `encode_ms` | 送编码器 → 对应输出包返回，包含计算和编码器排队 |
| `mux_ms` | 编码包返回 → 交织排队 → 封装写入及 avio_flush 完成 |
| `interleave_queue_ms` | 编码包返回 → 开始实际封装写入，逐帧日志字段 |
| `ingress_to_mux_ms` | 服务收到解封装包 → 对应转码包完成输出写入 |
| `scheduled_to_mux_ms` | 文件回放计划到达时间 → 写出，只有 pace=true 时出现，暴露反压积压 |

`ingress_to_mux_ms` 是**服务内部端到端时延**，不是源端采集到播放器显示时延。它不含收到包之前的网络/探测缓冲，也不代表接收端已收到或显示；写文件完成不等于 fsync 持久化。`decoder_ready.startup_ms` 从完整 HTTP 请求进入业务回调开始，截止 async decoder 初始化；完整启动耗时看 `startup.request_to_first_video_mux_ms` / `request_to_first_key_mux_ms` 和无条件输出的 `first_frame` 事件。它们包含校验、进程等待/创建、IPC、输入打开探测和首帧各阶段，仍不包含 HTTP 上传之前/期间或接收端显示。摘要对已经输出的帧统计；失败或取消时未输出帧不进入耗时均值。

时延统计覆盖每个成功输出视频帧，不受逐帧明细采样或日志队列丢弃影响。日志队列满时丢弃日志并计数，不阻塞媒体线程等待写盘。`bytes_out` 是所有音视频的 AVIO 累计写出字节，`encoded_bytes` 是单包视频编码载荷大小；`packets_in/out` 保持视频口径，音频另有 `audio_packets_out`。`interleave_call_ms` 包含交织排队和 mux/PES 写入，`avio_flush_ms` 单列。TCP/UDP socket API 观测不与逐帧指标混用，也不覆盖 libsrt 内部 UDP socket。

## 验证

`python3 tests/live_mux_integration.py` 验证真实 RTMP、SRT 双向连接、VVC 独立解码、两条晚音轨、音频停顿恢复、未知晚数据流、交织墙钟/容量上限、MP3 透传、EOF 排空及关闭音频。见 [本轮记录](validation-live-mux-20261017.md)。

`python3 tests/service_integration.py` 自动启动独立服务、生成合成输入并检查文件完整转码、HTTP 错误处理、停止、坏输入、已有输出保护，以及 UDP 输入。每次使用新的 `out/` 子目录，不覆盖冻结基线。测试需要系统 `ffmpeg`/`ffprobe`（仅用于生成和独立检查媒体）以及 Python 3；运行前先构建服务。

实际验证环境与结果见 [2026-10-16 验证记录](validation-20261016.md)。

新增日志/网络测试：`python3 tests/observability_integration.py`，覆盖 10 秒默认定时器、窗口计数、I 帧、轮转、压缩清理、队列饱和、阻塞输出期间心跳以及 TCP/UDP 收发。

新增进程测试：`python3 tests/process_integration.py`，覆盖待命补位、原生崩溃隔离、超时回收、控制失联期间继续转码、同 PID 接管、离线完成接管和首帧链路。结果见 [进程版本验证记录](validation-processes-20261016.md)。

轻量崩溃测试：`python3 tests/crash_integration.py`，覆盖真实非法访问、线程/栈溢出、持锁故障、超时与权限降级、限额、worker/HTTP 崩溃报告和接管。见 [崩溃采集验证记录](validation-crashes-20261016.md)。

[新增验证记录](validation-observability-20261016.md)；容器测试为 `python3 tests/docker_smoke.py`（先构建镜像）。
