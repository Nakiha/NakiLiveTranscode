# 固定多输出与动态输入适配（v1，实验实现）

本批合同已确认。主聊天明确提出固定 2–4 路、单路失败释放及固定输出适配三个建议，用户回复：“我觉得就这样就行”。早期客户完整逐字需求仍未检得，不能把旧 `/v2/jobs` 草案当作额外合同。水印完整功能已本地提交 `5052d203bc438dd1796bfb5273c1d82a5013ea77`，本批多输出改动已通过独立源码复审，按授权保存本地完整提交，不推送。

## 创建和控制

沿用 `POST /v1/job`，只允许一个活动输入任务。旧的顶层 `output` 单路请求继续有效；多路使用 `outputs` 数组，禁止同时提交顶层 `output` 或 `watermark`。

```json
{
  "input": "/media/input.ts",
  "pace": true,
  "timestamps": {"mode": "source_all"},
  "outputs": [
    {"id": "preview", "output": "/media/preview.mp4", "output_format": "mp4",
     "width": 640, "height": 360, "encoder": "libx264", "encoder_preset": "veryfast"},
    {"id": "main", "output": "srt://receiver:9000?mode=caller", "output_format": "mpegts",
     "width": 1920, "height": 1080, "encoder": "libvvenc", "encoder_preset": "faster"}
  ]
}
```

id 为唯一的 1–32 字节 ASCII 字母、数字、下划线或连字符，目标 URI 也不能完全重复。每路支持 output/output_format、encoder/preset/profile、width/height、bitrate、encoder_threads、audio_copy、mux_wait_ms、watermark 和下表预算。没有覆盖的媒体字段继承顶层配置或原默认值；更换编码器时继承的 preset/profile 仍须匹配该编码器。input、pace、decoder 参数、timestamps、intra 和 frame_selection 位于任务级；I/过载运行状态及编码关联则各路独立。不做运行中增删路、编码共享、独立降帧率或 ABR 对齐。

水印仍是每路单水印，像素位置相对于各自输出画布；初始化资源及 accepted/prepared/applied/failed revision 各路独立。多路热更必须指定目标，单路请求禁止 output_id：

```json
{"output_id":"preview","revision":2,"watermark":{"enabled":true,"resource":{"uri":"/media/logo.png"}}}
```

整组热更规则、required 不可清空及失败保留旧图不变。未知或已结束分支返回 409，revision 必须等于该路 accepted_revision+1。HTTP 同时仍只保留一个待 ACK 请求，token 区分跨路同 revision 的旧 ACK。

## 单次读入、资源与失败

一个 reader、一个异步 decoder owner 生成共同 input_sequence/frame_id。每路独立 AVFrame 头、引用队列、filter/watermark compositor、encoder、ledger、live mux 和 I/O owner；写像素遵守 COW。音频参数先由 reader 快照，再复制给各路 owner，绝不从 mux 线程读取 input streams 可变数组。音频仍只支持有界 AAC/MP3 copy，MP4 保持纯视频；落后输出水位的音频显式丢弃并计数。

| 每路字段 | 默认 | 范围与含义 |
| --- | --- | --- |
| queue_items | 16 | 1–16 个视频/音频项合计 |
| queue_bytes | 128MiB | 4KiB–256MiB，按实际保留缓冲及参数计费 |
| queue_media_ms | 5000 | 1–30000ms，视频按任务 fps 的名义处理时长累计；音频按包/采样时长。不是首尾 PTS 差，不因源时间戳空洞立即超限 |
| max_lag_ms | 10000 | 100–30000ms，从 ingress 到实际 mux 的在途年龄，含队列/encoder lookahead/mux 等待；无新到达也轮询 |
| io_timeout_ms | 5000 | 100–10000ms，输出打开、写出和收尾的 interrupt 截止 |
| inflight_bytes | 512MiB | 1MiB–1GiB，保留源缓冲加固定编码输入像素的预算，直到实际 mux/flush 才退休；另限 128 张在途视频帧 |

共享 buffer 可以重复计费；frame side data/opaque/metadata、packet side data、codec extradata、coded side data 和 custom channel map 都计入相应保留预算。解码后的尺寸、encoder 配置和 lookahead profile 固定有界；多路输入上限 4096×2160 像素，压缩 packet 在进入应用队列前限制为 8MiB；每路输出画布面积不超过 4096×2160，所有 encoder_threads 合计不超过 16。decoder 的 max_threads/budget_slots 仍是独立预算，滤镜图各 1 线程，不会每路新建 decoder。

这些是应用持有/准入预算，不是进程 RSS 上限：codec 内部副本、分配器、临时解码/缩放等开销另受固定规格和库实现约束。水印自身保留原有资源/画布上限；每路 live mux 仍限 256 包/4MiB，起播延后音频仍限 128 项/4MiB 并计入完整参数快照大小。不能据此承诺容量或硬实时终止；单次不可抢占库调用仍会影响退出时间。

队列暂满时，先向全部健康分支投递当前项，再共同重试，持续压力窗口为 100ms；健康分支的后续项也会等待这个窗口，不能承诺完全无背压。一次成功投递不重置窗口，只有消费端及时将项数、字节和媒体时长均降到严格低于半水位才恢复；容量 1 或 2 时必须排空。持续拥塞只失败该路，此后健康分支继续。原有项数、字节、媒体时长、在途、max_lag 和 I/O 预算仍生效，不丢压缩包换进度，不重开输出目标。

等待期间，生产者最多为每个活动分支持有当前一个未转交项，不积累下一项。这一项不属于 queue_items/queue_bytes 指标；重试前必须通过单项字节/媒体时长上限，因此「队列加生产者待交项」最多为 queue_items+1 项、2×queue_bytes 和 2×queue_media_ms 的保守计费量。音频 payload 与当前共享输入包引用同一缓冲，参数是独立快照；视频待交项仍计入已有在途预算。此处不包含 owner 正在处理的项、codec 内部和其他已列明的独立缓冲，不能将队列指标当成全部内存或 RSS 上限。

单项超限、在途预算超限、持续队列压力、写出超时、可返回的分支运行错误只失败该路，取消其 I/O、释放其队列/codec/mux 引用，其余继续。原生崩溃仍影响同一 worker 的全部输出。全部分支失败则共享输入取消、任务 failed。正常 EOF 时全部成功为 completed，至少一路成功且有失败为 completed_partial；每路有独立终态和错误。用户停止为 stopped。共享输入/解码错误仍失败整个任务。暂停输入与持续音频突发的新增回归见 [输入稳定性](live-input-stability.md)。

所有 required 资源共同门禁在打开输入之前完成，以免某路等待时遗漏最初输入帧。某一路门禁失败按分支失败处理，仍有就绪分支才打开输入；全部门禁失败时根本不打开输入。控制进程丢失不取消 owner，接管继续同一 worker、各路状态和 revision；服务 SIGTERM 仍有意停止所有分支。

## 动态输入规格

保持同一 H.264 或 HEVC 视频流，不切换输入 codec/视频 stream。支持解码帧的分辨率及 yuv420p/yuv420p10le 变化；输出尺寸、像素格式/位深、SAR、编码器、stream/time_base 与连接保持首次确定值。没有显式尺寸时冻结首张输入画布。变化后整幅输入缩放到固定画布并使用固定输出 SAR；不自动裁剪、补边或生成新画布。

准备私有新 graph，显式声明输入矩阵/范围并转换到固定输出矩阵/范围；先排空旧图，再在帧边界切换。编码器不重开，旧的 B 帧、PTS ledger、时间戳展开和 I 调度继续；不重写 PTS 来掩盖 discontinuity。输出 FLV 的毫秒量化规则仍有效，不能宣称逐整数刻度相同。PNG/GIF 水印的时间相位与 revision 保持，变化时重新校验 required 水印，不能以裸帧兜底。

primaries/transfer 变化（包括 SDR↔HDR）明确失败该分支，不做假 tone mapping；不支持的像素格式也明确失败。固定输出合同同样适用于 MP4/FLV/MPEG-TS，因此没有在线修改 MP4 sample description、重建编码器、重开文件或发送新输出规格序列头的行为。

## 状态与验证

顶层 frames_decoded 和 decode 只统计共同解码一次；packets_out、frames_filtered、bytes_out 为分支输出之和。各路摘要位于 outputs，提供 output_id、终态、错误、队列/在途计数、filter_generations、水印、实际首/末 mux 时刻与时延。完整逐帧和首帧记录带 output_id；每路窗口独立、不重叠，公共解码与各路端到端时延不能相加后充当 CPU/显示时延。

本轮最终证据为 `out/multi-output-acceptance-20261017T1430/`，本轮 15 项实流场景、13 套功能回归及单测/符号检查通过，独立源码复审已通过，结果以该目录 ACCEPTANCE.json 为准。开发窗口 attempt-4 已通过 13 项真实场景，后续补入 PTS 空洞及全部 required 门禁失败，共 15 项。使用独立同步解码 probe 检查实际 H.264/VVC 画面，另核对音频 payload 顺序及写出+显式丢弃数量闭合。它们是功能证据，不是生产性能、容量或任意长时间稳定性证明。
