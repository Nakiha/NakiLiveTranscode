# VVenC 实验链路

源码为 [Nakiha/vvenc](https://github.com/Nakiha/vvenc)，上游 [fraunhoferhhi/vvenc](https://github.com/fraunhoferhhi/vvenc)。独立子模块 `third_party/vvenc` 基于 v1.14.0；精确提交由 gitlink 固定。0.7.0 在公开 fork 独立实现 PTS 调度，不使用内网 patch；FFmpeg n9.0.2 fork 的 wrapper 同时增加显式启用选项与 VFR DTS 处理。

构建保留 VVenC 默认的内部 nlohmann JSON 支持：v1.14.0 的单遍码控前瞻缓存依赖该路径，关闭后即使不写 stats 文件也会在排空时失败。它仅是编码库内部依赖，服务的 HTTP/日志 JSON 继续使用 yyjson。

运行请求见 [vvc.json](../examples/vvc.json)：`encoder=libvvenc`、`encoder_preset=faster`、默认 `encoder_profile=live_3b_2s`，1.5 Mbps、2 个编码线程。应用、HTTP 控制、IPC、崩溃采集仍为 C；VVenC 是独立 C++ 共享库，容器含 libstdc++。filter 根据编码器协商 YUV420P10LE，覆盖 8-bit 转 10-bit 与原生 10-bit 输入；这不等于把 SDR 自动转换为 HDR，色彩元数据从输入传递。

0.8.0 开放 MPEG-TS（含 UDP/SRT）、Enhanced FLV（含 RTMP）和本地 MP4。FLV/TS 支持 AAC/MP3 透传与晚音轨追加，见 [直播封装](live-mux.md)。普通 MP4 要在 EOF/trailer 后完成索引，不适合直接作为持续直播交付格式，当前仍只输出视频。输入视频限 H.264/HEVC，不接受 VVC 回灌；在线视频规格重建与音频转码尚未实现。

## 编码精度

`encoder_bit_depth`仅接受8或10，默认10，仅用于libvvenc；固定输出分支也可各自设置。它传入VVenC原生`InternalBitDepth`，改变内部编码精度与码流位深。显式`encoder_input_bit_depth`默认10，8选择YUV420P字节filter及匹配fork的只读packed-byte接口，要求编码精度8、live CQF与future guard；当前guard仅支持直接输出。wrapper不修改输入平面、不分配全帧16bit暂存；VVenC接收时一次零扩展到自有工作缓冲。字节输入的外部QP活动度归一到10bit尺度。默认关闭字节参考开关时，原图、重建图和热点均保留旧工作存储路径。8bit输出仍可声明Main10 profile，实际精度以SPS和独立解码pix_fmt确认。

`encoder_native8_reference: true` 显式启用当前 fork 的 `LiveNative8Ref`，要求直接输出、8bit编码精度、live CQF和前视guard。它为完成重建的参考图建立字节缓冲，并让单向整像素运动搜索使用字节原图块及SSE/AVX2 SAD；残差接口显式使用有符号16bit类型，变换系数仍为32bit。字节参考在原有CTU/滤波/边界任务全部完成后、发布重建就绪前生成，不增加前视或改变源I/3B结构。双向/加权搜索、亚像素插值及其余工作缓冲仍保留原有精度，16bit重建图仍在，当前是混合路径，不能宣称全部参考内存减半。该开关默认关闭；关闭时保留上一批入口路径。codec trace v3新增实际字节ME块数与参考打包耗时。


质量对照统一使用原有540p10-bit参考，8-bit解码结果经同尺寸格式转换提升到10-bit后计分，不将参考先降为8-bit。必须匹配实际码率并重新核对源I、3B、前视、逐帧实际写出时延和滚动码率；同QP不代表同码率或同质量。

## 原生 CQF 与显式降帧

显式 `rate_control="cqf"` 接入 VVenC 原生 capped CQF：目标码率置零、`qp`（0–63，默认32）、QPA开启，`maxrate` 必填（10000–100000000 bps）。不能同时给 `bitrate`，也不允许其他编码器或在 bitrate 模式下附带 qp/maxrate；HTTP 配置校验会拒绝。默认不提供新字段仍走已有目标码率模式。参数、每路输出配置和归一化指纹均包含此选择；这不是自研 CRF，QP数值也不能当作其他编码器的 CRF 刻度。最大码率是库的近似码率约束，不是严格的网络瞬时上限。

CQF 的零目标码率让库关闭 RC lookahead，保留 QPA、MCTF 与自定义 3B。PTS GOP 的质量模式允许接收一张输入但暂不返回 AU，避免首帧输出后等待下一完整短组、同时阻塞调用方继续喂帧；EOF 仍完整排空。码率模式保持单遍 lookahead，不修改该路径的等待行为；库的内部 JSON 仍启用。普通 I 继续使用 `intra.mode="follow_source"`，启动 IDR、后续 TRAIL I，完整组仍为 3B，I 边界和 EOF 可出现短组。

`output_fps=30` 与已知60fps输入组合，在完整解码后、scale/编码前二选一，保留源PTS并更新编码器名义帧率；I及其他受保护画面额外保留。详见[帧率合同](video-rate.md)。示例见 [RTMP CQF 540p30](../examples/rtmp-vvc-cqf-540p30.json)。严格 FLV 仍要求合法非负输出 DTS；benchmark 的源文件显式在离线准备阶段增加1秒起点，为30fps重排留足预滚，服务内不重定时。

`tests/integration/cqf_rate_integration.py` 独立解码核对 CQF 的 QP响应、60→30帧数/PTS、源I跟随、3B、仅首个IDR、off-grid I保护、EOF尾组和逐帧统计；不使用配置字符串代替实际码流验证。

显式 `encoder_max_future_frames=1..4` 限定单个直接输出的 live CQF，0/省略保持旧路径。单位是降帧后实际进入编码器的显示画面；wrapper按CTS维护有界序号映射，检查每个AU释放时已经提交的更新画面数量，超限失败。该路径启用fork `m_liveCqfPoll`，根据PreProcess已经发布的精确短组大小初始化CQF，不再用下一组画面作“本组已齐”的哨兵；`vvenc_encode_pending` 不接收新图像、不触发EOF/补帧，以有界串行阶段推进取得待输出AU。FFmpeg先取尽可输出AU再取新帧；普通NULL输入仍完整排空。这是前视/额外输出排队上界，不是总pipeline在途帧数或CPU时间保证。RC lookahead、跨帧并行和未来MCTF仍禁止；QPA与内部JSON保留。见子模块 `doc/live-cqf-poll.md`。

## 实验受限 CRF

第二批另有 `rate_control="constrained_crf"` 实验模式：`crf=0..63`（默认32）是本项目质量锚点，不与x264/x265数值等价。限定单个直接live VVenC输出、显式output_fps、follow_source、最多4帧守卫；不能给qp/bitrate或使用outputs数组。独立开关为`source_rate_limit=true`（需plain RTMP及network_trace）和可选maxrate，至少启用一项。既有CQF/bitrate模式不变。

控制器先根据稀疏10-bit亮度梯度和历史运动活动调整质量锚点，再保留原有QPA和3B层级。预算取过去1秒已完成RTMP视频消息统计与固定maxrate的较小值；按实际DTS/写出窗口及在途画面预测预留选QP，实际AU bits替换预留并反馈码量模型，mux写出后转为实际历史。预测目标为82%预算、窗口余量为95%，I/P/B预测分配权重6/3.2/0.2，重视参考质量；I帧可花更多预算，1100kbps不除以30作为单帧硬上限。不等待输出、不重编码、不额外丢帧。预测误差、场景切换及QP饱和仍可能超限，不能称为严格VBV保证。

QP delta经frame metadata `naki_qp_offset`、fork `vvencYUVBuffer.liveQpOffset`及PicShared关联，在该画面的QP/QPA/lambda计算前生效，不在重排期间改全局配置。`VVENC_LIVE_QP_OFFSET_VERSION=1`扩展YUV/AU结构，客户端必须随固定SDK重编；非零delta只接受live CQF poll路径。返回`live_slice_qp`是实际slice QP，CTU QPA仍可细调。

trace新增`rc_decision_monotonic_us`、所选/实际slice QP、活动度、有效/源码率、预测bits、分配预算和饱和标志。审计`enforcement=true`只表示反馈控制器启用，合同是否达到仍看conformance；这不是成熟CRF或无条件码率/时延保证。QP63下必要码流语法也可能超过很低的源预算。

CQF实验搜索profile另提供`live_3b_2s_qt8`（最小QT8/chroma4）、`live_3b_2s_qt16`（QT16/chroma8），两者SearchRange64、MinSearchWindow32、FastIntraTools2；`live_3b_2s_rdoq0`在qt16上关闭RDOQ/SelectiveRDOQ，`live_3b_2s_speed`再关闭ALF/CCALF。这些profile保留源I、3B、QPA、过去参考MCTF及未来画面守卫，只接受CQF/受限CRF；默认live profile不改。关闭工具有实际画质代价，对照见[第二批测量](validation-constrained-crf-20261018.md)。请求示例见[受限CRF配置](../examples/rtmp-vvc-constrained-crf-540p30.json)，实验时需在服务启动前设置`BUSY_WAIT_TIME=0`。

诊断示例[codec-trace](../examples/rtmp-vvc-codec-trace-540p30.json)增加`codec_trace=true`，只用于原生软件HEVC→单个live VVenC输出。匹配fork在AU中返回PicShared绑定的内部阶段及CPU聚合，wrapper传入有界整数metadata，应用按原有PTS ledger取回。它强制全量日志，默认关闭；对照正式性能必须关闭诊断。事件口径、MCTF CPU未聚合等限制见[内部打点记录](validation-codec-phases-hevc-20261018.md)。

后续I预算改为与启动IDR独立建模，普通I先验0.40、IDR先验0.8，普通I反馈50%更新；从95%窗口边界扣掉在途预留后，先为随后P+3B留名义目标额度（最多当前余量60%），I使用其余部分。首个预测P参考锚可使用剩余窗口85%，后续P/B仍40%，I目标权重仍6。码量模型按真实输出帧型反馈，短组预测B转P不会再更新B模型。新增trace模型类型/系数/窗口余量/后续留额。GOP诊断ABI2另拆admission、前串行图重建完成、slice初始化与dispatch，并记录真实条件拒绝次数；不把计数当时间。测量和边界见[后续I预算与GOP派发](validation-i-budget-gop-dispatch-20261018.md)。

媒体预算窗口现由已完成编码AU的实际DTS推进，并按输出time base舍入；输入显示PTS不再提前释放仍在DTS窗口内的启动I。编码未写出的AU仍计入在途预留，独立审计通过`rc_media_clock_us`检查其因果来源。

## 显式普通 I（已实现）

可选 app `intra` 策略在现有 live profile 上启用 FFmpeg wrapper 的 `live-frame-intra=1`。匹配 VVenC fork 通过 `VVENC_LIVE_FRAME_INTRA_VERSION=1`、`m_liveFrameIntra` 和每张输入的 `forceIntra` 接收请求，在 PreProcess 的 PTS-group 决策之前关闭隐含周期 CRA、截断前一短组，并把后续请求编码为普通 TRAIL I；启动仍保留 IDR。GOP4 / 3B、PTS/DTS FIFO、one-pass JSON lookahead RC 的既有边界保持，reference profile 不接受该选项。

FFmpeg `libvvenc.c` 将实际 access unit 的 sliceType 提供为 I/P/B 统计，hook 和追踪不再把全部 VVC 类型记为未知。实际 NAL、显示顺序 I 位置和逐帧类型由独立解码核对，见[帧策略](frame-policy.md)。不提供新策略仍使用下面的周期 profile。ABI 必须匹配固定 gitlink/SDK；早期复审见[历史记录](validation-frame-policy.md)，当前身份与回归见[核心整理验证](validation-core-refactor.md)。

## 时间与帧关联

0.7.0 的默认 VVC profile 是 `live_3b_2s`：完整 4 帧组的显示顺序 `I BBB P BBB P…`、编码顺序 P4/B2/B1/B3。关键帧按首个编码输入 PTS 锚定的 `+2s、+4s、+6s…` 时间网格调度，选择达到目标的第一帧；不以实际关键帧位置重新推算下一目标，避免累积漂移。到边界时先用 P 结束前一组，再单独编码 CRA，完整组仍为 3B；边界及 EOF 可为 IP / IBP / IBBP。若输入向前跳过多个周期，只为新占用区间插一次 CRA，不连续补 I 帧。没有恰好处在目标时间的帧时，实际关键帧会迟于目标，不能保证任意 VFR 输入每段精确 2.000 秒或最大 GOP 时长。名义帧率及 `IntraPeriod=round(2×fps)` 只保留为 RC 提示；名义周期小于 4 帧仍拒绝。`STA=0` 禁止场景切换额外插 I，`POC0IDR=1` 使首输出从首画面开始，不强制源 PTS 归零；后续关键帧为 CRA，未承诺闭合 IDR。

VVenC opt-in `LivePtsGop=1` 在参考关系对下游可见之前计划最多 4 个待处理画面，预先构造并检查所有短组的参考列表与 DPB 约束，不改写已发布帧的 GOP 信息。MCTF 能消费成组到达的画面；串行码控在分析数据齐备后才初始化下一锚帧。此路径限制为已验证的 GOP4、P-anchor/B-interior、单遍、无未来 MCTF、无帧并行配置，其他参考布局显式拒绝。旧/default 编码路径不启用这些行为。详见子模块 `doc/live-pts-gop.md`。

默认 bitrate 模式保持单遍目标码率和 `LookAhead=1`；显式 CQF 模式按上节使用原生质量控制并关闭 RC lookahead，不能把两种模式的时延差都归因于实现优化。关闭跨帧并行队列 `MaxParallelFrames=0`，仍用 `encoder_threads` 做帧内线程；保留 MCTF、QPA，但 `MCTFFutureReference=0`，不为时域滤波等待未来帧。`BIM=0`：本次 custom P-anchor + MCTF + BIM 组合在 v1.14.0 的 320×180、10-bit 回归中可稳定触发未捕获的 `copyClip: Incompatible formats`；独立 CLI 也能复现，关闭 BIM 后通过。未将它当作一般性 BIM 缺陷，也未改动 VVenC 源码掩盖异常。

另有 `live_3b_2s_no_mctf`，只进一步关闭 MCTF，保留 QPA；本轮没有显示出稳定的额外时延收益，默认保留滤波。`reference` 复现上一版默认 VVenC GOP32、2 秒自动取整配置，仅供对照，**不满足客户的 3B/2s 约束**。x264 只接受 profile `default`。非法组合在 HTTP 层拒绝；解析器不会开放任意 vvenc-params。preset 改变速度/质量，不保证任何机器上实时，也不保证首帧时延上限。

历史首轮测量从约 3.29 秒降到 0.45 秒，源码/输入/环境见[低时延记录](validation-vvenc-lowlatency-20261017.md)，不作为当前候选的性能或首帧保证。这里仍有重排和码控前瞻，不能套用 x264 zerolatency；没有采用纯 P 或 low-delay B 伪装连续 3B。

live profiles 显式启用 FFmpeg `live-pts-gop=1`，使用 1/90000 时间基保留 PTS，精度为约 11.1 微秒，不再量化到名义帧周期。不丢帧、不补帧，允许单调的变帧率、间隔抖动和向前缺口；回退、重复、无效或溢出的编码时间戳显式失败。VVenC 此模式不再返回由名义帧间隔猜测的有效 DTS，匹配的 wrapper 维护 256 项有界 PTS FIFO，在编码顺序输出时使用向后错开两帧的输入显示时间轴作为 DTS；前两个 DTS 按名义周期向首 PTS 之前外推。校验 DTS 严格递增且不大于对应 PTS，FIFO 只存时间戳，不存画面。`reference` 保留原版一帧一 tick 路径。`fps` 请求字段仍只用于输入缺失名义帧率时回退，不强制帧率转换。

这里使用的是解码/filter 输出的有效显示时间戳：解码器的 best_effort_timestamp 恢复和输入完全无 PTS 时按名义帧率合成的既有回退仍然存在。没有有效输入时间信息时无法凭空恢复真实媒体时间，不能把该回退当作对源端变帧率的准确计量。

`pipeline_ready` 记录 `keyframe_schedule=pts_grid`、`keyframe_period_ms=2000` 与 `rate_control_intra_period_frames`，避免把 RC 提示误当作实际关键帧周期。输出关键帧的明细及首帧日志增加 `keyframe_target_pts_us`、`keyframe_lateness_ms`、`previous_key_interval_ms`（首关键帧为 null）。lateness 是媒体时间轴上晚于目标的差值，不是该帧编码计算耗时；各阶段真实驻留时延仍按原有字段统计。

每个输入 Stamp 沿 packet/frame opaque_ref 进入 filter，编码后按 packet PTS 在 256 项有界 ledger 中找回。不能按输出顺序关联：VVC 输出顺序可能不是显示顺序。正常 EOF 排空 encoder 后必须清空 ledger，否则任务失败；容量满也明确失败，不丢统计、不扩成无界队列。first I 在解码显示顺序中标记，重排后仍保留该身份；输入 I 与 VVC 输出关键帧分别统计。

`encode_ms` 从该帧送入编码器之前到对应编码包取出，包含编码调用、前瞻、内部排队、重排和后续喂帧等待。它是驻留时间，不是单帧 CPU 用时；多个帧的驻留时间可以重叠。每帧成功写出均更新各阶段及输入 I 分组的累计/窗口统计，逐帧日志只控制明细量。每次任务仍无条件输出首视频包、首输出关键帧的 `first_frame`，保留 HTTP 完整请求 handler 到 mux flush 的启动链路。上述值不包含客户端请求网络、编码器之前的探流包等待、源采集和播放器显示；探流/初始化耗时会体现在请求到首输出指标中。

新增 `encode_submit_ms`（送帧开始到 FFmpeg 接受）与 `encode_wait_ms`（接受到取出对应包），二者之和等于 encode_ms；接受并不表示 VVenC 已经完成计算，receive_packet 也可能执行编码。这两个区间均不是 CPU 用时。累计/窗口和输入 I 阶段统计同时包含 encode_submit / encode_wait。`startup` 新增 input_open_started/done、input_probe_done、decoder_ready、first_packet、first_decoded、encoder_open_started/done、output_header_done、first_encoder_submit 的 `_ms`，均从完整请求 handler 开始计时，未发生时为 null。first_frame 还带 `encoder_submitted_frames_at_output` 与 `encoder_api_wall_to_output_ms`，后者是送帧/取包 API 调用累计墙钟时间，不包含 API 调用之间等待输入的时间，也不是 CPU 核时。

## 修改、CI 与符号

`bash scripts/build-service.sh` 先在 Linux cache 编译 VVenC，再启用 FFmpeg `--enable-libvvenc`；可独立运行 `bash scripts/build-vvenc.sh`。修改子模块后先在 fork 提交并 push，再更新主项目 gitlink。不要使用 `submodule update --remote` 意外换 pin。`NAKI_SKIP_FFMPEG=1` 仅适用于依赖已构建的应用开发，不会重编 VVenC。

依赖 CI 一次构建 VVenC + FFmpeg，缓存键含两个子模块提交及构建配方。SDK 包含独立 `vvenc-symbols/`，服务发布包中的对应目录为 `vvenc/`，与 `app/`、`ffmpeg/` 并列。库从同一次构建导出 DWARF 后剥离运行库，保留 Build ID 和 unwind；manifest 记录准确 VVenC 源码提交。离线解析同时提供三个目录：

```sh
python3 scripts/symbolize-crash.py crash.json \
  --symbols symbols/app --symbols symbols/ffmpeg --symbols symbols/vvenc
```

`tests/vvenc_integration.py` 生成 H.264/HEVC 的 8/10-bit B 帧输入，覆盖缩放、MPEG-TS、MP4、UDP、分数帧率、重排与单帧 EOF，使用构建出的 n9 ffprobe 原生 VVC decoder 独立解码并计数。`tests/vvenc_pts_integration.py` 另覆盖 12 组变帧率、抖动、向前跳变、非零起点、所有短尾组和关键帧邻近 EOF，核对每帧 PTS 间隔、DTS、时间网格关键帧、日志时差及各阶段帧关联。CI 还验证运行镜像能装载并执行 VVenC，以及 VVenC Build ID 到函数/源码行的离线解析。

输出帧型与关键帧周期由 `tests/vvc_structure.py` 对独立解码结果验收。`python3 tests/vvenc_latency.py` 默认对 reference/live/no-MCTF 各做 2 次实时文件回放，并验证真实 UDP 输入；结果写入新的 `out/vvenc-latency-*`。CI 使用 `--repeats 1`，保存配置、首帧日志、逐帧解码与阶段指标，验证结构/算术关系，不以共享 runner 上的绝对毫秒数作为门槛。
