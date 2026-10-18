# 单水印 v1

这是实验服务的每输出单水印接口，适用于 SDR 8/10-bit 视频。创建任务时在 spec 中增加 watermark（多输出位于各输出），省略保持无水印。JPEG、静态 PNG 和 GIF 来自本地普通文件或 HTTP/HTTPS，资源下载独立使用 libcurl。推荐运行中使用[typed patch/CAS/操作查询](component-updates.md)；下文完整 revision 替换是兼容接口，二者的请求形状和重试保证不同。

```json
{
  "enabled": true,
  "resource": {"uri": "/data/logo.gif", "load_timeout_ms": 3000},
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

上述组和字段均可省略并使用示例默认值，但 enabled 默认 false、startup.required 默认 false，启用时须给 URI。未知、重复字段和不支持策略拒绝。URI 上限 2047 字节；超时 1..30000ms；宽高各 1..2048；偏移各 -8192..8192；opacity 0..1。兼容更新完整替换，省略字段恢复默认值；typed patch 从当前 desired 合并省略字段。

兼容更新发送 `POST /v1/job/watermark`，body 不超过 8192 字节，必须固定真实 job_id；示例需替换 JOB_ID。多路还须 output_id，单路可省略，给出时必须匹配逻辑输出 ID：

```json
{"job_id":"JOB_ID","revision":2,"watermark":{"enabled":false}}
```

任务的初始配置 revision 为 1。新 revision 必须等于 worker 的 `accepted_revision + 1`，重复和过期 revision 返回 409。202 表示 worker 已接受配置，资源仍可能在准备或失败；不表示已生效。控制端同时只等待一个 ACK，最多 2 秒；503 ACK 超时的结果不确定，先查询状态再决定下一个 revision，不能盲目复用。并发请求可能返回 409。URI 和布局在候选资源全部准备成功后于下一个保留视频帧切换。失败保留旧资源；旧版本不发生局部布局变更。新请求合并、取消尚未应用的旧候选。

`startup.required` 在任务内不可更改。required 任务的更新必须再次包含 `startup.required: true`；禁止关闭必需水印。首次等待从完整创建请求进入 HTTP handler 时累计，涵盖调度、下载、解码、缩放；每次更新或重试不重置起播预算。等待期间不打开媒体输入，不产生音视频包，停止和状态请求继续可用。资源就绪后先保留初始代，再打开输入，避免首帧前的新更新清掉唯一可用水印。请求到首帧 mux 的原有指标包含这段等待，不包含客户端上传或显示时间。

状态中的 `watermark` 回报 accepted/prepared/applied/failed revision、错误、实际应用的源 PTS（微秒）和活动资源内容 SHA-256。applied 表示配置已在视频帧上生效（禁用时为透传），不能当成网络送达或显示确认；逐帧 trace 的 `watermark_revision` 随原有 PTS ledger 到实际 mux 记录。控制进程丢失不取消媒体或资源线程，恢复后以 worker 状态为准。

`video_properties_known` 区分视频实参尚未到达和已经判定；`video_compatible` 仅在前者为 true 时表示兼容性。兼容性按帧边界生效的配置判断：初始配置是基准，成功准备的关闭配置可在首帧检查之前替代该基准。关闭没有资源 I/O，接受后同步 prepared，但仍须经过视频帧才 applied；它不等待旧资源下载完成。required 仍禁止关闭。基准关闭时，首帧之前接受的可选启用更新不保证视频兼容；HDR/不支持原色/超限画布使该启用 revision 失败并取消准备，原无水印视频继续。基准仍启用时不兼容则在输出头前失败，失败或未就绪的启用候选不会改变基准。已知不兼容时的新启用请求返回 409。

anchor 支持九宫格 `top_left/top_center/top_right/center_left/center/center_right/bottom_left/bottom_center/bottom_right`。水印矩形同名锚点与输出画布对齐，再加向右/向下为正的像素偏移。contain 保持比例并透明居中；stretch 允许变形。画布外部分裁剪，完全离屏是合法不可见布局。宽高、帧率、主过滤图、编码器和 muxer 不因换图重建。合成节点在首次资源应用时创建；启用时为每个主帧提供同 PTS 的水印输入，关闭时直接透传主帧，不等待未来水印帧，也不让旧水印图的色彩限制阻断透传。

GIF 按实际 GCE 时长、透明度及 disposal 生成完整显示帧。循环固定 forever，不采用文件内循环次数。URI 替换或尺寸重新准备从首次应用的媒体 PTS 重新计时；复用同一像素代的纯布局修改继续原动画相位。源 PTS 前跳直接选择对应相位；legacy PTS 回退到激活点之前时显示第一帧，严格 source_all 仍在上游拒绝回退。多帧 GIF 缺失或零时长明确拒绝，不偷偷套用默认帧率；单帧 GIF 可无时长。缓存像素和 AVFrame 头保持不可变，每次合成独立赋主视频 PTS。

每个输出的水印上下文限制一个加载线程、一个最新请求、一个活动代和一个已准备候选：编码文件最多 8MiB、源图片最多 2097152 像素、每代最多 128 帧/60 秒/64MiB 持久 RGBA 像素；超限失败。准备下一帧前先计算对齐后的像素布局并检查剩余预算，不能先多分配一个 RGBA 帧再拒绝。每个上下文的活动和候选持久像素合计最多 128MiB，多输出各自计费；worker 还保留共享任务的水印上下文，不能把线程数说成整个 worker 只有一个。布局复用可以共享活动像素；再次提交同 URI 且无布局变化会重新下载并按内容生成新代。AVBuffer padding、临时编码缓冲、解码器内部缓冲及缩放临时数据另外存在，不能把 128MiB 称为进程总内存上限。每输出合成画布最多 4096×2160 像素，固定三个 RGBA 槽；槽仍被过滤器引用时不扩容。大尺寸画布每帧清空和拷贝会增加带宽开销，尚无吞吐量或容量保证。

下载限制最多三次 HTTP/HTTPS 跳转，并限制总时间、编码字节数；本地拒绝目录、FIFO 等非普通文件。`resource.load_timeout_ms` 从加载线程开始本次准备时计时，覆盖加载、解码和缩放；队列中等待旧准备取消的时间另计，但 required 起播仍受从请求进入 handler 开始的固定总预算约束。取消和旧 revision 检查在读写、下载回调及逐帧解码之间执行，单次 codec、缩放、DNS/文件系统调用没有硬实时中断保证。HTTP 服务继承现有受信控制面权限，URI 也应由受信调用方提供。

首版不支持多水印、百分比坐标、APNG、HDR/BT.2020 水印色彩合成、ICC 色彩管理或任意滤镜。检测到 PQ/HLG、BT.2020、不支持的 YUV 矩阵或超过画布上限且帧边界生效配置启用水印时，在输出头前失败；生效配置关闭的此类视频拒绝后续启用水印。PNG 的压缩文本、ICC/cICP/HDR 元数据及非标准 gamma 明确拒绝，避免独立于像素预算的解压和隐式色彩处理。cHRM 按固定 SDK 的原色识别及容差只接受 BT.709；不支持或不能识别的 cHRM 不会回退成普通 RGB，解码后也检查原色。带 ICC 的解码图像也拒绝。普通 SDR JPEG、无此类元数据的 PNG/GIF 统一为 straight-alpha RGBA；缩放时临时预乘 alpha，避免透明像素隐藏颜色形成边缘光晕，合成前恢复 straight alpha；JPEG 不含透明度。Logo 明确转换到主视频的 BT.601/709 矩阵和 full/limited 范围；未标记时采用 BT.601 limited。10-bit 主视频保持 10-bit YUV，不通过 8-bit RGB 往返；不承诺线性光合成。合成输出直接继承 overlay 携带的主帧属性，不再次复制 side data，保留 SEI、字幕和 timecode 的原始数量与内容。

新增 SDK 依赖只有 zlib 开发包；JPEG/GIF 使用 FFmpeg 内置解码器。构建、符号和 SDK identity 必须匹配新配方。功能证据见该批次的验证记录；没有重跑的历史结果不代表当前通过。
