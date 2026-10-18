# 直播封装与晚音轨（0.8.0）

支持 H.264/HEVC 输入到 VVC 输出的 RTMP/Enhanced FLV、SRT/MPEG-TS 两条链路。音频为 AAC/MP3 透传：发现新音轨时由当前 worker 的同一个 mux owner 追加流，保留视频解码器、编码器、已写出的轨道和原推流连接。测试中两条音轨分别晚加入、停顿再恢复，视频继续写出。

请求示例见 [SRT](../examples/srt-vvc.json) 与 [RTMP](../examples/rtmp-vvc.json)。输入和输出传输可以独立选择，RTMP 输出必须配 flv，SRT 输出必须配 mpegts。SRT URL 的 `latency` 单位为微秒，示例 50000；它独立于服务 `mux_wait_ms`，现场应按网络条件配置。接收端须支持 VVC 以及 Enhanced FLV 多音轨/TS PMT 更新；只在连接建立时固定轨道集合的接收程序不能自动获得后加入音轨。

## 实现边界

FFmpeg fork 提供 `libavformat/live_mux.h` API 1；63.3.102 是该扩展最初的版本窗口，当前固定 n9.0.2 fork 的 libavformat 为 63.3.104，依赖身份见[当前验证](validation-core-refactor.md)。普通上游 muxer API 并未自动支持动态加流，必须显式启用本扩展。`avformat_write_header` 之后调用 `avformat_live_mux_init`，然后 `avformat_live_add_audio`。注册复制完整参数且不进行网络 I/O，失败回滚新流；新轨道的 sequence header/PMT 在其媒体之前写出。

FLV 扩容每轨状态数组，保持已有 track id 和时间戳，先发新增 AAC 配置/多轨元数据再发媒体。TS 为新音轨分配未用 PID，增加 PMT version、重发 PAT/PMT，保持视频 PID、PCR 源、计数器和原 PES 状态；每次实际输出后排空当前 PES 和 AVIO，避免音频聚合隐含额外缓存。当前只支持单 service、无显式 AVProgram、非 M2TS。FLV keyframe-index 模式不支持追加。

reader 独占会变化的输入 AVStream 数组，每个音频队列项复制 codecpar/time_base，mux owner 不跨线程读取 demux 状态。输入最多 64 流；应用最多记录 16 条音轨状态，库最多 32 条输出流。首次视频写出前临时音频队列最多 128 包/4MiB；满时显式丢最旧音频并计数。首次视频建立输出起点后再追加音轨。

音轨只追加，不移除或替换参数，不支持音频转码。AAC 要有可用 ASC，ADTS 可恢复基础配置；PCE 等未实现情况拒绝，不猜测布局。Enhanced AAC 使用一次独立包解码取得采样参数，原压缩媒体仍透传。ADTS 同一 PES 内缺失/重复 DTS 使用样本数及采样率补齐，保留新的 PES 时间戳锚点。新音轨带来未知编码、无效参数或既有音轨参数改变时拒绝该音轨，视频仍继续。字幕/数据不输出。

## 有界交织

媒体线程只使用一套 `live_mux_send/receive → av_write_frame → PES/AVIO flush`。在启用 live queue 的 context 上混入 `av_interleaved_write_frame` 会报错，避免两套缓存叠加。队列按跨 time_base 的 DTS 排序；各音视频轨道有包时允许写出，缺轨道时按以下条件释放：

- 默认最旧包排队满 50ms，配置范围 1..1000ms；采用单调墙钟，不依赖 DTS 前进。
- 达到硬上限 256 包/4MiB 载荷时释放；无法接纳的大包明确报错。
- EOF 强制排空。媒体线程即使没有新输入，也每 10ms 检查。

这不是“所有输出操作最多 50ms”的承诺：期限不含编码计算、调度停顿和网络写入。协议写入另有 10 秒 interrupt deadline，不能保证任何内核/DNS/库调用均能立即打断。音频恢复后落后于实际输出 DTS 水位或本轨 DTS 不递增时，明确丢包并计数；不回绕音频时间轴，不丢视频追踪。超时放行意味着无法保证所有轨道的全局 DTS 严格排序，尤其当编码视频本身较晚返回时。

`audio_tracks/audio_packets_out/audio_dropped/audio_unsupported` 报告音频；后两项是包计数。`mux_queued_packets/bytes`、`mux_high_water_packets` 与 `mux_timeout_releases/pressure_releases/forced_releases` 报告队列状态，release 计数可能同时属于多个原因。视频 `packets_out`、首帧、输入 I/输出 key 和各阶段耗时均在实际写入/flush 后记录，`mux_ms` 包含排队，逐帧 `interleave_queue_ms` 可拆看。取消时未交付帧不计入耗时样本，正常 EOF 排空。

## 探测与网络观测

仍保留 FFmpeg 输入缓存。裁剪构建必须包含原始 aac/mp3 demuxer，否则 TS 晚 AAC 的 raw codec probe 可能直到全局字节上限才释放所有音视频。reader 启动前另设 100ms 的后续 raw-probe 墙钟预算，未知晚数据也不能在持续有包时长期堵住视频。这不修改初始 `avformat_find_stream_info` 的策略；完全没有网络数据时仍由传输读取期限处理。

现有 TCP/UDP observer 可观测 FFmpeg 自己的 socket，覆盖 RTMP 的 TCP 路径；libsrt 自行创建的 UDP socket 不经过这些 hook。SRT 的内部缓存、重传和线上交付不能从现有 network 字段推算，当前没有新增虚假的逐帧 wire-time 关联。

验证入口 `python3 tests/live_mux_integration.py`，记录见 [2026-10-17](validation-live-mux-20261017.md)。
