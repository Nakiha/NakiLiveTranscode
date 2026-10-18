# 可扩展处理链草案（未实现）

当前可发送字段以[OpenAPI](openapi.json)为准。固定多输出/输入适配、单水印 v1、typed watermark 更新及显式类型 SEI 已实现，见[文档索引](README.md)和[当前验证](validation-core-refactor.md)。第四批已完成按 owner 的职责搬移，实际文件见[模块导航](../app/README.md)；下文通用 JobSpec/planner/分析节点并非当前文件清单。`/v2/jobs`、任意滤镜、ROI、HDR、通用一对多谱系和在线输出重建仍是草案，不能整体复制到 v1。

直播客户的过载抽帧、随源/固定周期/随源+最大间隔 I 帧、PTS 与 UNKNOWN SEI 合同见 [帧策略草案](live-frame-policy.md)。已确认源 PTS 自身的空洞可以保留，不补帧、不因此终止；源 I 和到期需强制 I 的画面必须先于抽帧决策受到保护。

TS→TS 的 SCTE-35 原始信令、节目时间和客户临时安排注入见 [SCTE-35 草案](scte35-design.md)。该事件通道独立于视频/SEI，不随抽帧丢弃。

目前已接好默认空实现的 [内部媒体 C hook](media-hooks.md)：音频包观察、编码后视频包处理及任务生命周期，供内网后续组合“音频 copy + 分析 + 视频 SEI”业务。响度统计算法、窗口计算和客户 SEI payload 均不实现；客户的松散口径为两次 I 帧编出之间，接口同时提供单调时钟和 PTS，不擅自将它固定成 EBU 时间窗。

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

未来接口应在创建前校验字段/范围/白名单/编译能力，首帧后再校验裁剪/色彩/编码与预算。202 不代表已完成或已打开直播源。当前 v1 已有 job/operation 身份、有限重试和 typed watermark 更新，见[组件更新](component-updates.md)；X-Request-ID 只关联本次 HTTP 尝试。这里没有新增 validate/prepare 接口。

## 复用 libavfilter 的方式

服务直接链接 libavfilter，在 C worker 推入/取出 AVFrame。当前 pipeline_output.c 调用 video_filter.c 的准备函数，按 width/height 生成固定 scale/null,format 描述并调用 avfilter_graph_parse_ptr/config；不开放任意 filtergraph 字符串。

请求边界保持结构化；内部固定线性链可以编译成滤镜描述字符串，让 libavfilter 协商格式、链接及执行。复杂分支、节点级配置或控制建议通过 `avfilter_graph_create_filter`、AVOptions 和 `avfilter_link` 创建图。两者最终使用同一套滤镜执行机制。用 API 连接节点并不会消除滤镜本身的排队或重排。

优先复用 scale/crop/pad、bwdif/yadif、unsharp 等已有实现；实际取舍应验证像素正确性、CPU 和缓存。HDR→SDR 要明确输入 primaries/transfer/matrix/range、峰值、tone mapping、输出色彩和位深，不能只改像素格式或删 HDR 标记。可评估 zscale/tonemap 或 libplacebo 路径，再显式增加构建依赖、SDK 键和能力描述。裁剪、缩放、补边、锐化的先后影响画面和 ROI，必须保留有序语义。

不开放任意客户端 filtergraph 字符串作为正式接口。字符串会让 FFmpeg 选项成为 API 契约，并可能绕过白名单、资源限制和媒体文件访问策略；先完成经过校验的 typed node→graph 编译层。手写像素算法留给已有滤镜不满足、且有可测收益的部分。

## app 模块划分

第四批实际拆分见[模块导航](../app/README.md)。下面是更广处理链的候选职责，不表示 planner/analysis/通用 JobSpec 已实现，也不要求为每项增加线程：

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

## 可热更新的图片/动画水印

状态说明：每输出单水印、required 起播与 revision 更新已经实现，typed watermark patch/CAS/操作查询也已实现。下面仅保留多 layer、百分比及其他策略的早期候选，不表示整段请求可发送。

以下保留早期设计讨论，不是 v1 请求结构。用户现已确认单水印 `/v1/job/watermark`、完整 revision 替换、required 起播失败及热更失败保留旧图；当前实现和资源限制以 [单水印 v1](watermark.md) 为准。百分比、多水印、HDR 和其他列出的候选策略没有随本批开放。

需求包括中途从无到有、更新 URI、锚点、偏移和宽高，支持 JPEG/PNG/GIF。建议复用 libavformat/libavcodec 解码与 libswscale/libavfilter 缩放/合成，把原先“大滤镜”的网络和资源生命周期拆出去，保留一个职责窄的常驻合成节点。媒体 owner 不下载、不等待资源、不持有跨线程可变解码器。

草案层配置如下，同样尚未实现：

```json
{
  "id": "customer_logo",
  "enabled": true,
  "resource": {"uri": "https://assets.example/logo.gif", "load_timeout_ms": 3000},
  "startup": {"required": true, "timeout_ms": 5000, "on_timeout": "fail"},
  "update": {"on_error": "keep_previous", "apply_at": "next_frame"},
  "placement": {
    "space": "output_canvas", "anchor": "bottom_right",
    "offset": {"x": -24, "y": -24, "unit": "px"},
    "size": {"width": 160, "height": 80, "unit": "px", "fit": "contain"},
    "opacity": 1.0
  },
  "animation": {"clock": "media_pts", "loop": "forever", "restart_on_replace": true}
}
```

规定 anchor 是水印外接矩形对应锚点对齐画布同名锚点，再施加向右/向下为正的偏移；百分比 x/y 分别以参考空间宽/高为基数。明确裁剪后源画面、缩放后内容区域、补边后输出画布三种坐标空间。contain 保持比例并在配置矩形中居中，stretch 明确变形；裁出屏幕的部分裁剪，完全离屏是合法不可见状态还是配置错误应固定策略。多水印按稳定 z-order 合成。输出帧率/尺寸不因水印布局改变。

后台 asset manager 用有总超时、字节和并发上限的 libcurl 加载 URI，再由独立解码上下文生成资源。JPEG/PNG 静态图只留一帧；GIF 按实际帧时长、透明及 disposal 生成完整显示帧，不能按恒定 fps 猜测。限制压缩大小、解码像素、帧数、总动画长度和总 raw 内存；1080p RGBA 一帧约 7.9MiB，100 帧就约 791MiB，不能只按下载文件大小限额。长 GIF 选择拒绝或独立有界滚动解码，两种策略都要保证取帧不会阻塞主视频；默认可以先拒绝超限动画。

资源缓存键包含内容 hash、解码/颜色参数、目标尺寸与插值方式；同 URI 内容可能变化，每次加载产出不可变 content generation。布局变化可以复用像素；尺寸变化后台重算。准备中的多次更新按 revision 合并/取消过期工作，限制同时存活的候选资源、旧资源和缓存总额，防止频繁更新绕过限额。缓存只含资源自有像素，逐帧 PTS 放在轻量 AVFrame 引用头中，不能修改共享缓存帧。失效/超时资源不进入 active revision。

prepare 成功后通过有界消息发布 immutable layer set，媒体 owner 在帧边界一次切换整份版本；回报 accepted revision 与实际 applied revision、视频 PTS、content hash。准备失败保持旧水印，旧资源在所有在途 AVFrame 引用释放后回收；HTTP 和资源线程不直接修改 graph。未来可用 If-Match/revision 控制并发更新，当前 API 没有这类路径。

静态图每个视频帧复用同一像素，GIF 以 active revision 生效时的媒体 PTS 为动画起点，按可变 duration 查找帧，最后给本次合成输入赋主视频帧 PTS。明确源 PTS 前跳、动画循环次数、最后一帧保持及 URI 替换时是否重启动画。资源供帧器在禁用/未就绪且允许无水印时也提供合法透明帧，不能让 framesync 等待第二路数据；禁止使用 wall clock 无意追赶/跳帧。

“常驻”指逻辑合成节点，不代表任意尺寸图像可以直接塞入固定 buffersrc。overlay 的 x/y 命令、输入尺寸/像素格式协商、alpha 模式和 EOF/framesync 行为要分别验证；只设置 repeatlast 并不能解决从未来过第一张图的等待。可以先固定水印输入画布尺寸，后台把图渲染到透明画布，再用 overlay 合成，代价是内存带宽。若多层、尺寸频繁变化或 10-bit/HDR 性能不合适，再做一个只取已准备帧并合成的专用 C 节点；该节点不负责下载、demux 或缓存调度。不为了换图销毁整条视频 graph/编码器/muxer。

建议 SDR 输出在 tone mapping/缩放后合成 SDR logo，随后做编码输入格式转换；HDR 输出则需明确 logo 的颜色空间、亮度与 alpha 合成规则。JPEG 无 alpha，PNG/GIF 的透明度、straight/premultiplied alpha 及颜色范围都要统一处理；不能把普通 8-bit logo 的数值直接当作 HDR 光强。当前精简 FFmpeg 尚未启用 JPEG/PNG/GIF 解码器或 overlay，URI 图片水印不是现有功能；后续实现应把所需组件、符号与能力检测纳入 SDK。

## 必需水印与起播等待

当前 v1 已有 waiting_assets、请求起点的 required deadline、等待期间不打开媒体输入和可停止查询。多 required layer/分阶段资源直方图等仍属下文候选；当前实际合同以 watermark.md 和 component-updates.md 为准。

用户要求：部分客户第一帧起必须带水印，资源未就绪则等待至水印拉取超时。它是 startup gate，与运行中资源更新不同；不能自动套用“失败保留旧图”，因为起播时可能没有旧图。

建议 task 状态增加 preparing/waiting_assets，并在 startup 日志记录资源准备起止、下载/解码/缩放耗时、重试次数及等待原因。HTTP 仍异步返回 202，状态查询和停止始终可用；取消需要中断等待、下载及解码任务。在 required layer 的资源、布局和首张可合成帧全部 ready 前，不提交任何视频到编码器，也不输出无水印视频包。多个 required layer 是一个联合 gate，所有 layer ready 才起播。

`startup.timeout_ms` 是从任务接受开始的累计等待预算，不能每次重试重置；单次 URI load_timeout_ms 不得突破剩余预算。超时策略必须显式：`fail` 保证不输出无水印内容，建议 strict required 默认值；`start_without_watermark` 只在客户允许“等到超时后裸流起播”时配置。后者可以将资源设为可选并保留等待预算，但不能同时声称始终有水印。是否后台继续重试及失败后何时再更新也需另设有界策略。下载成功仍不等于 ready，坏图片、解码/缩放失败也计入同一预算。

第一版在 gate ready 后再打开直播输入和推流输出，避免等水印期间积攒压缩包/原始帧、推流端空等超时；request-to-first-frame 明确包含整个 asset wait。以后若要与输入打开/探测并行，必须另设计缓存上限与等待结束后的 keyframe/catch-up 策略，不能用无界缓存来缩短表面启动时间。

运行中更新默认 keep_previous；没有旧资源的新增可选 layer 在新资源 ready 后才启用，期间视频继续。若客户要求“更新请求到达立即遮挡/停画，直到新图 ready”，那是另外的业务连续性策略，需单独配置并评估接收端超时，不能隐式套用到所有热更新。

## 一入多出与缩放分支并行

当前已经实现共享解码、2–4 路独立 owner、有界 FIFO、固定输出适配和可返回错误的 fail_branch，使用 output_id。下文共享编码、任意 rendition/destination 层、独立降帧率、block_all/drop_raw_frames、ABR 对齐与更多分支池仍是设计候选，不是现有 API 或性能承诺。

建议未来 JobSpec 用稳定 output/rendition id 的列表，视频 processing/encoder 归属于 rendition，交付 URL/封装归属于 destination。同分辨率、处理链、码率、编码/GOP 设置完全一致的多个推流目标可以共享编码，再按引用分发压缩包到各自 mux；不同规格/水印的 rendition 各自编码。不能只按尺寸相同就共享编码器，需要完整 effective spec 匹配且编码头/封装要求兼容。

对真正多规格输出，先实现一进程内应用级 fan-out：demux 一次、decode 一次，公共处理后向每路有界 AVFrame 队列发独立引用，各路 owner 各自驱动 buffersrc→filter graph→encoder→live mux→I/O。同一 graph/codec/mux context 不能并发驱动。参考 [AVFrame 引用 API](https://ffmpeg.org/doxygen/trunk/group__lavu__frame.html) 和 [buffersrc 引用语义](https://ffmpeg.org/doxygen/trunk/group__lavfi__buffersrc.html)。

当前 n9 的 vf_scale 已将 graph/filter 线程预算传给 SwsContext，并调用 sws_scale_frame；它的内部多线程与跨 split 分支并行是两件事。单张图的调度不应当作应用级分支并行保证。每路独立 graph/owner 可以复用原始 scale，通过应用并发运行分支，无须先侵入式改 async scale。先从 2～4 路固定 owner 线程开始；如果上百分支，改为有界调度池且同一分支最多一个计算任务在途，另隔离可能阻塞的输出 I/O，避免一个网络写占死共享计算池。

解码帧由只读 AVBuffer 引用共享，每路独立 AVFrame 头和追踪 id。滤镜/编码器要写像素时必须遵守 make_writable/COW；不能共享可写 SwsContext 或 AVFrame 头。CPU 帧不通过现有 socket IPC 传输。当前 worker 进程隔离可保留为“一路输入会话”，内部各 rendition 线程；某路算法原生崩溃仍会影响同 worker 全部 rendition，如果未来要求进一步崩溃隔离，需另设计跨进程帧池和所有权，不能直接传 AVFrame 指针。

每路同时限制队列帧数/字节/媒体时间及最大滞后，pending/encoder lookahead、滤镜缓存也计入总在途内存。1080p YUV420P10LE 一帧约 5.9MiB；仅四个队列项就约 23.7MiB，即使引用共享，慢分支也会延长原帧存活期。decode、每路 scale/filter、encoder 的内部线程共享总 CPU 预算，不能每路自动开满核；当 CPU 或内存带宽已经饱和，拆线程不能消除实际总计算量。

慢分支必须显式选择：

- `block_all`：所有输出逐帧完整，任一路持续跟不上最终反压输入，不能同时承诺其他路永不受影响。
- `drop_raw_frames`：只对允许降级的分支，在入编码器前跳过过期原始帧；计数并保留单调 PTS，重新验证 GOP 和短组。不随意丢压缩参考包。强制满帧率/完整 3B 排列的分支未必允许此策略。
- `fail_branch`：队列或滞后超限后失败这一输出，释放引用，其余继续；失败传播、客户端状态与后续重试规则要明确。

音频压缩包也按引用 fan-out，由各自 mux owner 根据自己的参数、时间基和 DTS 水位追加晚音轨；某路拒绝编码或写出失败不能持锁阻塞所有分支。输出 GOP 若需 ABR 对齐，要从共同源 PTS 锚点决定目标关键帧，而不是各路第一张实际接收帧重新锚定；当前单路 VVenC wrapper 没有多路共同锚点接口，需另外实现并验证。共用水印资源缓存，但每路按自己的坐标/尺寸准备变体；required 水印起播门槛需明确是全任务 gate，还是各路独立起播。

多输出日志增加 rendition_id/destination_id，保留同一个 input_sequence 及每路 frame_id；首帧、I/key、队列等待、filter/scale/encode/mux/I/O 和丢帧均按分支统计。公共解码耗时只计算一次，端到端按各路实际写出结束。回归必须验证 2/4 路真实并行区间、像素正确性、PTS/GOP、EOF，以及一条输出阻塞/慢速时其他路行为符合策略；不能只看到多个线程存在就宣称并行或吞吐提升。
