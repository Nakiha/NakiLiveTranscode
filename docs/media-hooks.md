# 内部媒体扩展接口（API 2）

当前已接入可编译的 C 扩展点，默认为空实现；不实现响度测量、I 帧窗口统计、具体 SEI 格式或时间戳修复，也不新增 HTTP 字段。客户特有逻辑后续由内网实现。声明在 [media_hooks.h](../app/media_hooks.h)，默认工厂在 [media_hooks_default.c](../app/media_hooks_default.c)。这是随源码编译的内部接口，不是稳定的动态插件 ABI。

选中源消息的[SEI 透传](sei.md)由服务独立处理：解码前提取，私有 `video_packet` hook 返回后统一注入，再交 mux。该模块不替客户生成新的响度 SEI；私有 hook 不应重复注入同一批源消息。

## 当前音视频路径

reader 独占 demux，通过同一个 8 项有界队列向 pipeline owner 交付视频包或带 codecpar/time_base 快照的音频项。

- 视频：H.264/HEVC → async decode/自动预算 → null/scale/format → libx264/libvvenc → 编码 PTS ledger → FLV/TS live queue 或直接 MP4 → mux/PES/AVIO flush。
- 音频：AAC/MP3 → 参数准备、必要的 AAC ADTS→ASC 转换、时间戳检查/补齐 → 首视频之前有界暂存 → 原连接上注册输出音轨 → live queue → mux/PES/AVIO flush。没有重新编码；初始化时可能用一包解码探测参数，不是持续音频分析。
- 当前 MP4 始终只输出视频，audio_copy=false 关闭音频交付。copy 指压缩媒体未重新编码，不承诺输出传输头/packet bytes/时间戳逐字不变；重复、过期或不支持的音频包会被计数并丢弃。

启用私有工厂后，音频包观察与 copy 路径解耦：即使 audio_copy=false 或 MP4 只交付视频，只要注册 audio_packet，reader 仍保留可接受的音频项交给 hook。默认未注册时维持原来的丢弃行为。输入流数量、参数大小和队列上限仍适用。

## 扩展点与生命周期

| 接口 | 时点与用途 |
| --- | --- |
| naki_media_hooks_open | 每个任务在 pipeline owner 上调用，创建私有任务上下文，配置 callback table |
| audio_packet | 每个接受的输入音频项恰好一次，尚未进行 copy 参数修改/暂存；可引用压缩包交给自己的分析队列 |
| video_packet | 每个编码视频 AU 返回后、时间基换算和 mux 之前，包括 EOF 排空；观察编码返回时刻，或附加私有 SEI |
| close | 正常完成、停止、失败时一次清理；工厂失败但已设置 close 时也调用。崩溃/强杀不保证执行 |

所有 callback 串行运行在同一个 pipeline owner，不持有应用锁；不是在 HTTP 或 demux reader 上执行。跨线程分析需要扩展自身拥有有界队列及独立上下文，packet callback 不能等待分析、网络或磁盘。主服务不因为注册 hook 自动开线程、解码音频、缓存 PCM 或构建客户状态机。不要将音频 decoder、SEI serializer 和 mux 写入互相调用，建议各自只通过私有 typed result/context 对接。

事件和指针只借用至 callback 返回。持有音频包用 av_packet_clone，参数另做 avcodec_parameters_copy；复制 AVPacket 指针或结构体浅拷贝不转移所有权。视频 payload 修改先 make_writable；异步线程不得在返回后继续修改已经交还管线的包。不要把客户状态塞进服务使用的 opaque_ref。

video_packet 允许替换/扩展压缩载荷，净增长最多 64KiB；必须保持完整合法 AU，并保留 PTS、DTS、duration、flags、stream_index。服务检查这些字段及增长上限，错误返回负 AVERROR 并使任务失败，不静默回退；不负责验证任意私有载荷是否符合编码标准。该 hook 不是修改 DTS/PTS 或抽帧的接口。

## 两次 I 帧编出之间的业务窗口

已记录客户口径：上次 I 帧从编码器返回，到本次 I 帧从编码器返回之间，对业务选取的音频做统计。这轮只暴露时间与包，不替客户定义或实现响度算法。

- video event 的 encoded_us 在 avcodec_receive_packet 成功返回后立即采样，采用服务单调时钟；它先于 hook、交织排队和实际写出。
- audio event 的 demuxed_us 是 av_read_frame 返回时刻，dispatched_us 是 owner 调用音频 hook 的时刻。两者都不是 socket/wire 接收时刻；初始 probe 缓存重放会影响观察时间。
- 每个事件还带 time_base，音频包本身保留所观察到的 PTS/DTS，视频包是编码器时间基下的 PTS/DTS。私有实现可以选择墙钟窗口或媒体时间口径，但不能把“在窗口内处理的音频”和“PTS 落在窗口内的声音”混称同一项。
- 音频仍可能晚交织到达、分析线程也可能滞后；本次 I 画面已编出时不保证结果完整。私有逻辑须选择使用已完成结果、写无效标记、限时等待或失败；当前 packet callback 合同不允许等待，若需要等待，应另设计有界 pending AU/完成通知并计入时延，不能堵住 owner。

input_picture_type 来自原解码画面，不能当成输出帧型。output_picture_type 在编码器 AV_PKT_DATA_QUALITY_STATS 有报告时提供，否则为 NONE；匹配工作区的 x264 和 VVenC wrapper 均提供实际 I/P/B，后者来自 access unit 的 sliceType。output_key 单独保留，不等于普通 I。video_packet 对所有 AU 调用，事件额外保留 frame_id、generation、源 PTS/time_base；这仍是随源码编译的内部接口。实际类型由 `tests/media_hooks_integration.py` 对 x264/TS、VVC/TS 和 VVC/MP4 逐帧独立解码核对，不能只验证“非 NONE”的计数。未匹配的旧库可能不提供类型，不可伪装成确定的 P/B/I。

media_hook_ms 逐帧记录视频 callback 自身耗时；它属于 mux_ms（编码返回至实际写出）的子项。interleave_queue_ms 原有边界仍为编码返回至开始写入，包含包准备和 hook，不能在注册 hook 后把它当成纯队列驻留时间。整体阶段总和仍保持原口径。

## 私有实现与构建

私有 C 文件定义唯一 naki_media_hooks_open，填写 api_version=NAKI_MEDIA_HOOK_API、opaque 和需要的 callback；未用项为 NULL。任务状态留在 worker 私有上下文，控制进程崩溃/接管不重建它，但任务退出、worker/容器崩溃不提供持久化恢复。

```sh
# 路径是 Linux/WSL 路径；示例文件由内网自行实现。
NAKI_SKIP_FFMPEG=1 \
NAKI_MEDIA_HOOK_SOURCE=/path/to/private_media_hooks.c \
bash scripts/build-service.sh

# 取消变量后构建会明确切回默认实现，避免 CMake 缓存残留。
NAKI_SKIP_FFMPEG=1 bash scripts/build-service.sh
```

只有已安装匹配 FFmpeg 时才跳过依赖构建。Docker 支持同名 build arg，源码应在 app/ 内以便现有 COPY 带入；不要把宿主机绝对路径直接传进容器。每次私有构建仍需归档该构建的 Build ID 符号和私有源码版本。

测试 `python3 tests/media_hooks_integration.py` 使用独立测试工厂验证串行 owner、借用引用留存、音频观察与 copy/MP4 解耦、编码重排和 EOF、回调失败/清理、非法时间戳修改和超量 payload 增长拒绝、取消清理。测试工厂不进入运行镜像，也没有实现响度算法或 SEI。

API 2 增加固定多输出上下文：工厂先为共享输入打开一次（output_id=NULL），再为每个 output_id 打开独立私有状态。输入音频只在共享上下文观察一次，编码后视频在各路上下文观察；不同 owner 可并行，不能共用未同步的可变 opaque。共享回调错误失败任务，输出回调错误仅失败该分支；close 的 output_failures 回报部分成功的失败路数。所有上下文随同一 worker 接管，不因控制端重连重建。这是源码接口变化，私有 hook 须按 API 2 重编译并审查多实例所有权。
