# NakiLiveTranscode

实验性的 Linux/WSL C 直播转码服务，版本 **0.9.0**。一个活动输入任务可创建 **1–4 个固定逻辑输出**，不宣称生产就绪、硬实时或容量/SLO 保证。

当前底座为 **FFmpeg n9.0.2 + 异步解码 API 2 / 网络观测 API 1**；保留原 7.0.3 分支与历史验证结果。历史子模块目录名 `ffmpeg-7.0.3` 暂不改名。源码修改与本地构建见 [FFmpeg 开发](docs/ffmpeg-development.md)。

[文档索引与能力状态](docs/README.md) · [服务/API](docs/service.md) · [OpenAPI](docs/openapi.json) · [测试入口](docs/testing.md) · [当前本地验证](docs/validation-core-refactor.md)

## 支持范围

| 范围 | 当前实现与边界 |
| --- | --- |
| 视频 | 第一条可用 H.264/HEVC 原生软件解码流，共享 reader/decoder，默认 async、可显式选择 sync；每个输出独立 filter、libx264 H.264 或 libvvenc VVC 编码及 mux owner |
| 输出 | FLV/Enhanced FLV（含 RTMP）、MPEG-TS（含 UDP/SRT）、本地 MP4；MP4 在 EOF/trailer 完成索引，保持纯视频 |
| 音频 | FLV/TS 有界 AAC/MP3 透传、最多 16 条音轨状态，允许晚加入；参数签名改变会拒绝该轨，落后输出时间水位的包显式丢弃并计数；不转码、不删除或替换已注册输出流 |
| 输出规格 | 创建后输出集合、URL、编码器和画布固定；已支持的输入分辨率/位深变化可通过重建输入 filter 适配；primaries/transfer 变化及不支持格式失败 |
| 帧与元数据 | 可选严格 `source_all`、普通 I 策略与显式过载抽帧；按显式类型选择有界 SEI opaque type/payload 透传，选中画面受保护 |
| 水印 | 每输出单水印 v1，SDR 8/10-bit，JPEG/PNG/GIF、本地或 HTTP/HTTPS 资源；运行中只开放 watermark 更新，支持 desired/effective、CAS、有限 operation 重试/查询 |

没有音频转码、任意音频 codec、运行中增删输出、删除/替换已注册音轨、任意视频规格切换、HDR tone mapping、通用滤镜 API、ROI、SCTE-35、脏时间戳修复或 `/v2/jobs`。详细合同见[固定输出](docs/branching-contract.md)、[直播封装](docs/live-mux.md)、[帧策略](docs/frame-policy.md)、[SEI](docs/sei.md)及[水印](docs/watermark.md)。[管线设计](docs/pipeline-design.md)仍包含未实现的草案字段，不可整体发送。

## 解码开关与本机主线集成（2026-10-22）

`decoder_async` 与 `decoder_early_output` 是独立的初始化开关。默认 async 开启、HEVC 快出开启、H.264 快出关闭；`decoder_pump` 是独立的前台调度选项，要求 async 开启。配置和比较方法见[解码控制](docs/decoder-controls-20261022.md)。

H.264 快出只在满足 HRD/CFR 等排序依据时自动生效，无声明、多 slice 等输入回退；当前加固路径仍可能压缩帧间并行。HEVC 使用合格 IDR 起点后的连续 POC 前缀，不保证任意输入都能提前交付。请求开关为 true 不代表每帧命中。

本机功能、停止/重启、重放及内存检查的精确范围见[本次 main 集成验收](docs/validation-decoder-main-20261022.md)。目标机 CPU、持续吞吐、队列稳定性和 P95/P99 仍须独立实测，不将本机合入门槛等同完整生产验收。

## 历史验证状态（2026-10-18）

本次目录与接口整理：full 35 步、应用 ASan/UBSan 6 步、纯服务构建与自定义目录选测通过；性能入口完成短时 smoke，见[本次验证](docs/validation-organization.md)。

下述为第四批冻结源码窗口的验证，不能自动覆盖后续目录/接口整理。第四批模块拆分候选的 full **34 步通过**（含 15 个原生测试），应用/yyjson/helper ASan/UBSan **6 步通过**；两套均含 86 个媒体生命周期场景。逐职责 quick/受影响套件、源码/ELF/SDK 绑定及拆分前后对照见[验证记录](docs/validation-core-refactor.md)。第五批只更新文档，不将本次静态检查冒充新的媒体回归；证据绑定具体源码窗口，不以未提交工作树的 HEAD 代替。

本机证据未覆盖外部可信 HTTPS 水印成功样例（未配置 `NAKI_TEST_HTTPS_URL`）、实际 Docker 镜像/smoke、远程 Actions、真实 Filebeat 采集、目标机器 CPU/RSS/PID/FD 容量与 24/72 小时 soak、迁移回滚和生产验收。镜像和远程流程以对应 PR/提交的检查结果单独确认；最新源码复查见[合并前复查](docs/validation-final-review.md)。核心 native SDK/系统库未重新插桩，没有 TSan；旧版本的镜像/HTTPS 或 n7 性能报告不补齐这些门槛。

## 运行服务（Linux / WSL）

```sh
sudo apt-get install build-essential cmake pkg-config nasm libx264-dev libsrt-openssl-dev libssl-dev libevent-dev libcurl4-openssl-dev libunwind-dev python3
git submodule update --init --recursive
bash scripts/build-service.sh
./out/bin/naki-live
# 在另一终端中；先把 examples/file.json 的输入/输出改成实际路径：
./out/bin/naki-live-ctl start examples/file.json
./out/bin/naki-live-ctl status
./out/bin/naki-live-ctl stop
```

JSON 使用 yyjson，HTTP 服务端使用 libevent evhttp，C 客户端使用 libcurl。服务默认监听 `127.0.0.1:8090`，无身份认证，属于本机实验控制接口。`start/stop` 为兼容命令；需要调用者稳定 operation ID 时使用 canonical 创建/更新/停止接口。202 的确认边界按接口区分，不表示任务完成或媒体送达。交织默认等待 50ms、周期轮询；阻塞 I/O、调度及库内调用另受各自边界限制，并非硬实时 50ms 上限，见[服务说明](docs/service.md)。

内网自定义业务可以通过 [内部 C 媒体 hook](docs/media-hooks.md) 观察音频包、处理编码后的视频包并管理任务上下文。默认空实现；响度计算、客户 SEI 格式和时间戳修复仍只是后续接入点，不是当前功能。

默认每 10 秒输出状态 JSONL，逐帧明细按需开启。累计/窗口时延覆盖全部成功输出帧，包含分位数上界和 I 帧独立统计；独立日志线程使用有界队列，并轮转追踪文件。TCP/UDP socket 收发另有原子计数观测，不等同逐帧网络端到端时延。详见 [运维与观测](docs/operations.md)。

VVC 默认配置为 `live_3b_2s`，PTS 顺序 I BBB P…，以首帧 PTS 为起点，每到 2 秒时间目标的第一帧输出关键帧；边界及 EOF 允许 IP / IBP / IBBP 短组。保留码率控制与重排，支持单调的变帧率时间轴。历史[低时延测量](docs/validation-vvenc-lowlatency-20261017.md)和[PTS 调度验证](docs/validation-vvenc-pts-20261017.md)只描述各自源码/输入/环境窗口，不作为当前候选的首帧或容量保证。

时延按每个成功写出视频帧统计，覆盖排队和实际 mux/AVIO flush；首帧启动边界为完整 HTTP 请求进入 handler → 首次 mux/flush。它们不含客户端上传、源端采集或播放器显示，不是 source-to-screen；socket 调用时间也不是 wire time 或逐帧网络关联。

容器运行：`docker compose up -d --build transcode`，见 [Docker 部署](docs/docker.md)。

使用独立转码进程，默认保留 1 个待命 worker；控制进程异常退出时转码继续，新控制进程可接管原任务。每个任务额外输出首帧链路日志。容器内轻量监管入口负责重启控制进程，详情见 [进程池与接管](docs/processes.md)。当前只允许一个活动输入任务，可配置最多 4 路固定输出；支持每路单水印更新，其他运行中动态配置仍受各专题合同限制。

默认启用独立 C 崩溃收集器：信号处理函数只传固定现场，外部进程限时抓栈、记录模块 Build ID 并生成限额 JSON 报告，不依赖完整 core。见 [崩溃诊断](docs/crashes.md)。

Linux amd64 CI 配置将 FFmpeg SDK 构建与应用构建分开复用；通过门槛的 main 构建分别保存镜像包和按 Build ID 索引的独立符号。源码本机通过不代表远程 CI 或发布通过，须核对相应提交的检查和产物，见 [CI 与构建产物](docs/ci.md)。

构建后运行 `bash scripts/test-service.sh quick` 做快速检查，`bash scripts/test-service.sh full` 做完整功能回归，`bash scripts/test-sanitizers.sh` 做独立的应用 ASan/UBSan 检查。可用 `--suite service --suite live-mux` 运行选定套件；独立性能入口为 `scripts/bench-service.sh`，见[性能测量](docs/benchmarking.md)。完整回归需要匹配的 FFmpeg/VVenC 符号目录；覆盖范围、参数与结果口径见 [测试入口与基线](docs/testing.md)。

## 目录

| 路径 | 用途 |
| --- | --- |
| [app/](app/README.md) | Linux C 服务、模块/所有权导航与 HTTP 客户端 |
| `scripts/` | 服务及解码专项的构建与回归入口 |
| `third_party/yyjson/` | 固定版本 JSON 库子模块 |
| `third_party/vvenc/` | Nakiha/vvenc fork，固定 v1.14.0，可独立修改 |
| `examples/`、`tests/` | 请求示例与完整链路集成测试 |
| [docs/](docs/README.md) | 当前合同、设计草案和按窗口归档的验证记录 |
| `experiments/decoder-20261016/ffmpeg-7.0.3/` | 固定版本的 FFmpeg 子模块；底层代码保留独立历史 |
| `experiments/decoder-20261016/` | 本地历史媒体、基线、报告、安装目录和交付包，均不上传 |
| `out/` | 新构建与测试结果，不上传 |

## 获取源码

```sh
git clone --recurse-submodules https://github.com/Nakiha/NakiLiveTranscode.git
cd NakiLiveTranscode
```

FFmpeg 子模块固定到 [Nakiha/FFmpeg 的 n9.0.2 定制提交](https://github.com/Nakiha/FFmpeg/commit/2673319b815fa6da6fccd0c3801e099b2e473066)，精确提交以本仓库 gitlink 为准，以上游 `n9.0.2`（`946fcce07b6dcd0331c8cc609192aeff5e1924f8`）为基线。不要使用 `git submodule update --remote` 无意升级已测版本。原 `cbf2341520725a4f8b217052f864aa7dd3028a8d` 仍保存在 n7.0.3 live 分支。

## 原解码专项验证构建

以下是保留的精简解码专项工具。服务使用上面的独立构建配置，启用网络、filter 和 libx264。

Linux / WSL（安装 GCC、Make、NASM、pkg-config、Python 3）：

```sh
bash scripts/build-decoder.sh linux
```

Windows：从 MSYS2 UCRT64 运行（安装 UCRT64 GCC、Make、NASM、pkg-config、Python）：

```sh
bash scripts/build-decoder.sh windows
```

默认最多 8 个构建任务，可用 `FFMPEG_LIVE_BUILD_JOBS` 覆盖。输出位于 `out/build/<platform>-auto` 和 `out/install/<platform>-auto`。Linux 建议将整个新克隆放在 Linux 文件系统，以改善编译性能。

本机保留了历史媒体与原版 golden，构建后可运行：

```sh
bash scripts/test-decoder.sh linux
# 或在 MSYS2 UCRT64 中：
bash scripts/test-decoder.sh windows
```

每次回归使用新的结果目录，保留历史证据。新克隆不包含媒体；测试入口会在缺少基线时停止。获取方式见 [实验说明](experiments/decoder-20261016/README.md)。

## 已有文档与结果

- [异步接入与移植指导](experiments/decoder-20261016/ffmpeg-7.0.3/doc/live-async-guide-zh.txt)
- [自动模式接入](experiments/decoder-20261016/ffmpeg-7.0.3/doc/live-async-auto-zh.txt)
- [n9.0.2 移植说明](experiments/decoder-20261016/ffmpeg-7.0.3/doc/live-n9-port-zh.txt)
- [冻结测试与报告](experiments/decoder-20261016/ffmpeg-7.0.3/tools/live-async/validation/README.txt)
- [整理记录及后续工作](docs/workspace.md)

FFmpeg 及其附带代码遵循子模块内的许可说明；部署和分发应按实际构建配置检查其许可要求。
