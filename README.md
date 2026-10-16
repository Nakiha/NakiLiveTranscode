# NakiLiveTranscode

实验性的 CPU 直播转码项目，起点为 FFmpeg 7.0.3 的异步提前出帧和自动并行度调度实验。

当前底座为 **FFmpeg n9.0.2 + 异步解码 API 2 / 网络观测 API 1**；保留原 7.0.3 分支与历史验证结果。历史子模块目录名 `ffmpeg-7.0.3` 暂不改名。源码修改与本地构建见 [FFmpeg 开发](docs/ffmpeg-development.md)。

**当前状态：Linux 单路 C 实验服务，版本 0.8.0。** H.264/HEVC 输入 → 异步自动调度解码 → 8/10-bit filter → H.264 或 VVC 输出。支持 RTMP/Enhanced FLV 和 SRT/MPEG-TS；AAC/MP3 音轨可以晚加入、原连接继续输出。音频为透传，不做转码；MP4 保持纯视频。见 [直播封装与动态音轨](docs/live-mux.md)、[服务说明](docs/service.md)、[OpenAPI](docs/openapi.json)。后续处理链的结构化请求草案见 [管线设计](docs/pipeline-design.md)，尚未实现。

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

JSON 使用 yyjson，HTTP 服务端使用 libevent evhttp，C 客户端使用 libcurl。服务默认监听 `127.0.0.1:8090`。当前无音频转码、无在线视频规格切换；“端到端”指标指服务收到解封装包到转码包写出，不含采集端和播放器。交织默认最多排队 50ms，媒体线程每 10ms 检查；网络写入另有 10 秒中断期限。

内网自定义业务可以通过 [内部 C 媒体 hook](docs/media-hooks.md) 观察音频包、处理编码后的视频包并管理任务上下文。默认空实现；响度计算、客户 SEI 格式和时间戳修复仍只是后续接入点，不是当前功能。

默认每 10 秒输出状态 JSONL，逐帧明细按需开启。累计/窗口时延覆盖全部成功输出帧，包含分位数上界和 I 帧独立统计；独立日志线程使用有界队列，并轮转追踪文件。TCP/UDP socket 收发另有原子计数观测，不等同逐帧网络端到端时延。详见 [运维与观测](docs/operations.md)。

VVC 默认低时延配置为 `live_3b_2s`，PTS 顺序 I BBB P…，以首帧 PTS 为起点，每到 2 秒时间目标的第一帧输出关键帧，不再靠固定帧数决定；边界及 EOF 允许 IP / IBP / IBBP 短组。保留码率控制与重排，支持单调的变帧率时间轴。实时文件回放首输出约 0.45 秒（旧配置约 3.3 秒），见 [低时延测量](docs/validation-vvenc-lowlatency-20261017.md) 和 [PTS 调度验证](docs/validation-vvenc-pts-20261017.md)。

容器运行：`docker compose up -d --build transcode`，见 [Docker 部署](docs/docker.md)。

0.3.0 使用独立转码进程，默认保留 1 个待命 worker；控制进程异常退出时转码继续，新控制进程可接管原任务。每个任务额外输出首帧链路日志。容器内轻量监管入口负责重启控制进程，详情见 [进程池与接管](docs/processes.md)。仍只允许一路任务，尚未实现多路容量和运行中动态改配置。

0.4.0 默认启用独立 C 崩溃收集器：信号处理函数只传固定现场，外部进程限时抓栈、记录模块 Build ID 并生成限额 JSON 报告，不依赖完整 core。见 [崩溃诊断](docs/crashes.md)。

Linux amd64 CI 将 FFmpeg SDK 构建与应用构建分开复用；main 每次构建分别保存镜像包和按 Build ID 索引的独立符号，并验证真实崩溃报告到源码行的离线解析。见 [CI 与构建产物](docs/ci.md)。

## 目录

| 路径 | 用途 |
| --- | --- |
| `app/` | Linux C 服务与 HTTP 客户端 |
| `scripts/` | 服务及解码专项的构建与回归入口 |
| `third_party/yyjson/` | 固定版本 JSON 库子模块 |
| `third_party/vvenc/` | Nakiha/vvenc fork，固定 v1.14.0，可独立修改 |
| `examples/`、`tests/` | 请求示例与完整链路集成测试 |
| `docs/` | 项目现状和整理说明 |
| `experiments/decoder-20261016/ffmpeg-7.0.3/` | 固定版本的 FFmpeg 子模块；底层代码保留独立历史 |
| `experiments/decoder-20261016/` | 本地历史媒体、基线、报告、安装目录和交付包，均不上传 |
| `out/` | 新构建与测试结果，不上传 |

## 获取源码

```sh
git clone --recurse-submodules https://github.com/Nakiha/NakiLiveTranscode.git
cd NakiLiveTranscode
```

FFmpeg 子模块固定到 [Nakiha/FFmpeg 的 n9.0.2 live 分支](https://github.com/Nakiha/FFmpeg/tree/live/n9.0.2-async-early-output)，精确提交以本仓库 gitlink 为准，以上游 `n9.0.2`（`946fcce07b6dcd0331c8cc609192aeff5e1924f8`）为基线。不要使用 `git submodule update --remote` 无意升级已测版本。原 `cbf2341520725a4f8b217052f864aa7dd3028a8d` 仍保存在 n7.0.3 live 分支。

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
