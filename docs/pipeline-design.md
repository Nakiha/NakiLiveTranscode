# 可扩展处理链草案（未实现）

当前实现和可发送字段只以 [OpenAPI 0.8.0](openapi.json) 为准。当前仅支持 null/scale/format，没有这里的任意滤镜、ROI、HDR 转换或动态更新接口。以下是后续开发建议，不应复制到 `/v1/job` 试运行。

## 请求模型

建议增加独立 `/v2/jobs`，保留 v1 的扁平配置适配器，将两者归一化成同一个不可变 JobSpec。协议输入、视频处理、音频策略、输出和观测分组；滤镜使用**有序、具名、强类型列表**，避免继续向 Config 堆布尔开关。每个节点有稳定 id，便于错误定位、耗时和配置修订。以下只是概念示例：

```json
{
  "schema_version": 2,
  "input": {"url": "srt://0.0.0.0:15000?mode=listener", "format": "mpegts"},
  "video": {
    "select": {"index": 0},
    "filters": [
      {"id": "deint", "type": "deinterlace", "mode": "auto", "output_rate": "frame"},
      {"id": "crop", "type": "crop", "x": 0, "y": 0, "width": 1920, "height": 1080},
      {"id": "sdr", "type": "hdr_to_sdr", "target": "bt709", "peak_nits": 100},
      {"id": "resize", "type": "scale", "width": 1280, "height": 720, "aspect": "fit"},
      {"id": "border", "type": "pad", "width": 1280, "height": 720, "color": "black"},
      {"id": "detail", "type": "sharpen", "amount": 0.25}
    ],
    "analysis": [{"id": "roi", "type": "roi_detect", "every_n_frames": 5,
                  "deadline_ms": 20, "on_timeout": "omit", "coordinates": "encoder_input"}],
    "encoder": {"name": "libvvenc", "bitrate": 1500000,
                "gop": {"period_ms": 2000, "b_frames": 3}, "roi_source": "roi"}
  },
  "audio": {"mode": "copy", "late_tracks": "append", "unsupported": "skip"},
  "output": {"url": "rtmp://receiver.example/live/output", "format": "flv",
             "interleave": {"wait_ms": 50, "max_packets": 256, "max_payload_bytes": 4194304}},
  "observability": {"status_interval_ms": 10000, "frame_log": false}
}
```

`auto`、HDR 默认、aspect、ROI 坐标等必须在首帧规格可用后解析为明确值，报告 effective spec，禁止悄悄跳过能力。“降采样”分开命名：scale 改空间分辨率，fps 改时间采样，format/chroma 改位深/色度，audio.resample 改音频采样率。音频重采样意味着解码/处理/重新编码，copy 模式不得同时要求 resample。

创建前静态校验 JSON、范围、字段冲突、滤镜白名单与编译能力；等首帧后再校验裁剪范围、色彩来源、编码格式和可实现的预算。HTTP 202 仍只表示已接受，运行期校验失败通过任务状态及字段路径报告。新版本可增加 validate/prepare 能力接口，但不能把纯静态校验宣称为已成功打开直播源。现有 X-Request-ID 仅用于关联，未来幂等键须另设计。

## 复用 libavfilter 的方式

服务直接链接 libavfilter，C worker 中推入/取出 AVFrame，不通过命令行 FFmpeg 进程。现有 `open_output` 已用结构化 width/height 生成固定 `scale/null,format` 描述，调用 `avfilter_graph_parse_ptr` 和 `avfilter_graph_config`。

请求边界保持结构化；内部固定线性链可以编译成滤镜描述字符串，让 libavfilter 协商格式、链接及执行。复杂分支、节点级配置或控制建议通过 `avfilter_graph_create_filter`、AVOptions 和 `avfilter_link` 创建图。两者最终使用同一套滤镜执行机制。用 API 连接节点并不会消除滤镜本身的排队或重排。

优先复用 scale/crop/pad、bwdif/yadif、unsharp 等已有实现；实际取舍应验证像素正确性、CPU 和缓存。HDR→SDR 要明确输入 primaries/transfer/matrix/range、峰值、tone mapping、输出色彩和位深，不能只改像素格式或删 HDR 标记。可评估 zscale/tonemap 或 libplacebo 路径，再显式增加构建依赖、SDK 键和能力描述。裁剪、缩放、补边、锐化的先后影响画面和 ROI，必须保留有序语义。

不开放任意客户端 filtergraph 字符串作为正式接口。字符串会让 FFmpeg 选项成为 API 契约，并可能绕过白名单、资源限制和媒体文件访问策略；先完成经过校验的 typed node→graph 编译层。手写像素算法留给已有滤镜不满足、且有可测收益的部分。

## app 模块划分

建议逐步将 pipeline.c 拆成以下职责，先搬运行为，保持现有回归：

| 模块 | 职责 |
| --- | --- |
| config / spec | JSON 解析、版本适配、静态校验、深拷贝的有界 JobSpec |
| planner | 输入规格到 effective plan：节点顺序、格式/颜色、缓存与延迟预算 |
| input | demux owner、参数变更事件、带规格快照的有界压缩包队列 |
| video_decode | async decoder、metadata/PTS 来源关联 |
| video_filter | 图创建、送帧/收帧、EOF、节点参数命令及配置 generation |
| analysis | 可选有界 ROI 分支、超时/跳帧/坐标变换策略 |
| video_encode | 编码器适配、GOP/重排、ROI 支持能力、输出包映射 |
| output | 动态音轨、有界交织、mux 与实际 I/O/flush |
| telemetry | 追踪谱系、每阶段指标、首帧/I帧、配置 revision |

pipeline driver 保持单一媒体 owner 驱动状态机，各模块具有 init/push/pull/drain/close 和显式背压/EOF 约定；不为每个滤镜新增线程，也不默认把原始帧搬到进程 IPC。每节点线程数、在途帧/字节和分析等待要计入整条管线预算；某些滤镜会等待相邻帧。

ROI 检测属于分析支路：低分辨率采样可降低成本，结果记录源帧 PTS、配置 revision、坐标变换及过期时间。等待当前帧结果会增加直播时延，默认应限时，超时不使用 ROI；复用旧结果需明确年龄与运动误差上限。到编码前生成编码器支持的 ROI 描述。不能假定 VVenC wrapper 已消费 AV_FRAME_DATA_REGIONS_OF_INTEREST；需逐个编码器实现/验证适配，能力缺失应在校验中拒绝。

## 追踪与运行中更新

当前一包/一帧 ledger 适合既有保帧链，不能直接覆盖丢帧、倍帧、去交错和混帧。未来保留 input_sequence，同时新增处理 frame_id、parent_ids 和 generation：fps 丢帧有明确计数，field-rate 解交错的一源多输出各有子 id；合帧节点记录贡献来源。编码关联使用输出 frame_id/PTS，避免重复 PTS 或丢帧把现有 ledger 填满。需要明确每种操作改变 PTS/duration、是否改变输出帧率及 2s GOP 决策。

先支持启动时固定图。后续配置更新采用有 revision 的 prepare→validate→apply，指定帧边界或目标 PTS，IPC 只传有界配置和 ACK；不能在 HTTP 线程直接碰 graph。亮度/锐化等节点也只有验证过运行时命令才允许原地变更。尺寸、像素格式、色彩或输出时序改变可能要求重建图/编码器及刷新序列头；这与“晚加入音轨”不同，不能承诺任意修改均不断流。先明确各封装和接收端能力再开放。

落地顺序建议：模块搬运和 JobSpec → crop/pad/scale/sharpen 保帧链 → 追踪一对多后接解交错/fps → 完整颜色模型与 HDR→SDR → ROI 分支和编码器适配 → 在线 revision 更新。每一步补实际像素/色彩/时间戳、EOF 和缓存上限验证，不用只检查滤镜名能否找到。
