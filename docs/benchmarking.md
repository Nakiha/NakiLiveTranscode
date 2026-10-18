# 独立性能测量入口

性能测量与 quick/full 功能回归分开。先在 Linux/WSL 构建服务，再运行：

~~~sh
bash scripts/build-service.sh

# 三种 VVenC 配置：生成短样本，重复比较首帧和阶段时延，另带 UDP 输入检查。
bash scripts/bench-service.sh latency --repeats 3

# 把 JSON 的 input 改成已有本地媒体；相对输入路径以 JSON 所在目录为基准。
bash scripts/bench-service.sh service --config /path/to/workload.json \
  --mode paced --seconds 60 --repeats 3

# 不主动按源时间轴限速，观察这个具体负载的吞吐。
bash scripts/bench-service.sh service --config /path/to/workload.json \
  --mode throughput --seconds 60 --repeats 3

# 本地有限文件作源：真实 loopback RTMP 拉入、转码后 RTMP 推出并接收。
# seconds 是观察上限，须给起播留余量；未达到视频目标则失败。
bash scripts/bench-service.sh rtmp --config /path/to/workload.json \
  --input-fps 60 --input-gop 120 --mode paced --seconds 120 --repeats 3

# 每轮原始接收文件总会保留；也可复制到便于查找的新目录。
bash scripts/bench-service.sh rtmp --config /path/to/workload.json \
  --input-fps 60 --input-gop 120 --seconds 120 --repeats 1 \
  --save-output-dir out/saved-output-unique
~~~

`rtmp` 入口读取配置中的本地 H.264/HEVC 输入，在计时前 copy-remux 成独立 FLV，不修改原文件；单个输出替换成临时 loopback RTMP 地址，封装固定为 Enhanced FLV。源端测试工具监听 RTMP，服务主动拉取并向另一接收端推送；这是两个直接协议连接，不包含中转流媒体服务器、跨主机网络或播放器。`paced` 由源端按 FLV 时间戳供包，`throughput` 不主动限速。RTMP 断开不保证表现为正常 demux EOF，因此有限源发完后保持连接；驱动在距源尾至少一个名义秒（最少32帧）的位置设置视频目标，通过 job_id 请求停止、等待封装收尾。这样为 decoder/encoder 重排留出余量，不要求直播源产生文件 EOF。连接提前断开、目标未达到或接收解码不符都判为失败，不将 Broken pipe 视作成功。记录实际输出帧数和停止原因，nominal_video_seconds 只按名义帧率换算，不声称是 VFR 的真实 PTS 时长。每轮独立启动服务、源端和接收端，三者资源分开记录。接收媒体由固定 SDK 的 VVC decoder 独立解码，核对视频帧数、尺寸、编码器类型和音频存在性；核验不在计时窗口中。

结果在 `out/rtmp-bench-*/`，外层仍经过 SDK consumer、源码与二进制身份检查。保存命令、输入/配置哈希、实际请求、每 250ms 的资源及完整状态、最终状态、各进程退出码和接收媒体。服务在本轮结束后 SIGTERM，源端在 API 停止后 SIGTERM，接收端自然结束；这些退出码保留。接收解码数须等于服务写出数，目标按已选画面计算；全部完整解码画面须计入 selected/rate-drop/overload-drop，停止时的未输出重排尾部不计作已交付画面。`frame_log=false` 仍由服务统计每个完成画面，明细只保留无条件启动检查点与现有状态窗口。

`--input-fps 60 --input-gop 120` 会独立解码核对输入的实际PTS间隔及每个I位置；`output_fps=30` 是服务显式降帧字段，`fps` 仍只是缺失名义帧率的回退。RTMP降帧测量当前限定 follow_source、无额外SEI保护/过载丢帧，便于按输入显示序号及源I精确重建保留列表。接收画面须关联到保留的源PTS；除最后最多4张取消尾部外，逐张核对PTS和I，完整I间隔检查3B结构，并验证实际NAL只有首张IDR。文件EOF全排空、off-grid I及完整尾组另由 cqf-rate 功能回归验证。每个有限源最多250000张，逐帧证据在测量窗口外生成并保存。

540p30原生CQF示例见 [rtmp-vvc-cqf-540p30.json](../examples/rtmp-vvc-cqf-540p30.json)，做benchmark时只把input改为本地有限文件，输出自动隔离。默认质量基线可显式选 qp32/maxrate6000000，并记录它们而不是将QP称为CRF；这些值不是质量或实时性保证。严格FLV需为输出重排留正DTS空间；本轮准备的源文件离线显式增加1秒起点，copy-remux使用copyts保留该时间轴，服务不会偷偷平移它。

service 接收现有服务配置，可用固定 1/2/4 输出、各路编码器、线程和画布组成单个负载。每次重复创建独立 controller/worker 与新输出文件；所有输出地址替换为新结果目录内的本地文件，原配置文件不会修改。输出格式和其他媒体配置沿用模板，pace 由 mode 指定，实际请求保存在 resolved-config.json。

观察到 EOF 可以提前结束；到 --seconds 上限则通过当前 job_id 请求停止并等待收尾。记录 stop_reason 和实际时长，不把短文件包装成长时稳定性验证。单次最多观察 72 小时，但没有自动循环输入，也没有自动执行 24/72 小时 soak；需要提供长度足够的媒体，另行设计直播网络和目标宿主机验收。

服务 JSON 示例：

~~~json
{
  "input": "input.mp4",
  "outputs": [
    {
      "id": "a",
      "output": "replaced-a.ts",
      "output_format": "mpegts",
      "encoder": "libx264",
      "width": 640,
      "height": 360
    },
    {
      "id": "b",
      "output": "replaced-b.ts",
      "output_format": "mpegts",
      "encoder": "libvvenc",
      "width": 640,
      "height": 360
    }
  ]
}
~~~

证据入口为新的 out/checks-benchmark-*/summary.json，负载结果在对应步骤 artifacts 中：

- SDK consumer gate、源码前后摘要、ELF/helper 哈希、配置与媒体 SHA-256、命令、退出码和超时；失败保留并返回非零。
- 每次重复的实际配置、最终服务状态、按输出的帧数/时延指标、输出媒体和独立解码检查。
- 每 250ms 采样 controller 及其后代的 CPU、RSS、线程、FD；JSONL 持续写入文件，内存中只保留聚合和相邻采样。身份以 PID 加启动时间区分，不向观测 PID 发送信号。

总体 elapsed/FPS 包含任务创建、起播和任务收尾；独立媒体检查及 controller 启停不在这个窗口。资源数字是采样进程总和，RSS 会重复计入共享页；轮询之间结束的进程可能漏记，不等于 cgroup 物理内存或内核峰值。服务直方图分位数仍是上界，保留普通 I/首 I/重复 I 与输出 key 分组，截止实际本地 mux/flush；不包括采集、客户端上传、wire time 或播放器显示。

--list 只展示计划；--build-dir、--bin-dir、--result-dir 与功能入口相同，结果目录必须新建于 out/ 下。latency 使用自己的固定小样本，接受 1–5 次重复；service 接受 1–10 次重复。通过表示测量和输出检查完成，不表示达到容量/SLO，不自动比较或承诺性能改善。

logger 仍有独立微基准：

~~~sh
python3 scripts/bench-logger.py --baseline-dir /path/to/saved/logger-source --repeats 3
~~~

baseline-dir 中应有匹配的 logger.c/logger.h；该工具使用内存 stdout 回调，不测真实磁盘或媒体时延。decoder 专项仍走 scripts/test-decoder.sh，需要本地历史媒体；冻结 n7 结果不能作为当前 n9 性能。

## 保存出流与离线画质检查

`rtmp` 每轮的 `captured.flv` 是接收端以 `-copyts -c copy` 保存的 Enhanced FLV，包含实际 VVC 码流与收到的音频，不重新编码。`--save-output-dir` 可在计时、结构核验结束后额外复制为 `output-1.flv` 等；目标必须是 `out/` 下的新目录，`manifest.json` 记录原测量目录、成功/失败状态和输出 SHA-256。默认 capture、显式保存的副本、输入与日志均不入库。

对已经通过的 RTMP 测量，可单独做一轮画质检查，不需要重跑性能矩阵：

~~~sh
python3 scripts/check-video-quality.py \
  --benchmark-run out/rtmp-bench-EXAMPLE --repeat 1 \
  --metrics-ffmpeg /path/to/analysis/ffmpeg \
  --result-dir out/quality-unique

# WSL 也可调用本机已有的 Windows 分析工具，媒体须在 Windows 挂载盘上。
# --metrics-ffmpeg /mnt/c/program1/ffmpeg.exe
~~~

先通过 `scripts/build-service.sh` 构建 `naki-quality-video-export`。质量入口校验原测量的输入/出流哈希并执行 SDK consumer gate，然后用固定 SDK 独立解码源和出流，按精确有理数 PTS 配对；参考帧使用相同整数降帧策略（保留源 I），并直接调用服务的 `naki_video_filter_prepare` 做 bilinear 缩放到出流尺寸和 `yuv420p10le`。当前入口限定固定 VVenC、follow_source、无水印、颜色标签一致且明确的 limited-range SDR。不按相邻时间戳猜帧，不自动移除 PTS 偏移；停止尾部即使有重排缺口，也逐帧匹配实际源 PTS，不重复末帧补齐。

分析 FFmpeg 必须带 rawvideo 输入、PSNR、libvmaf 和 null 输出；它独立于服务 SDK，不将这些工具加入生产构建。对等长、逐帧对应的 10-bit 原始帧计算全部帧 PSNR（峰值1023）与 VMAF `vmaf_v0.6.1`，`n_subsample=1`，评分尺寸为原生出流尺寸，未放大到1080p。首先用16帧自身对比检查 PSNR=inf、VMAF≥99；随后核对两个指标的完整帧数与连续序号。VMAF 是指定模型和尺寸下的分数，不应直接等同主观画质或跨尺寸比较。

新目录保留可播放的压缩出流 `output.flv`、工具版本/哈希、原始探测、`alignment.json`、导出帧 PTS、命令、逐帧 PSNR/VMAF、汇总 `results.json` 与成功标志 `COMPLETE.json`。工具/输入/相关导出源码前后哈希变化会判失败。中间 raw 文件只在本轮成功后删除（可用 `--keep-raw` 留存，约每帧宽×高×3字节、每路一份）；失败保留现场。质量检查、复制与指标计算均在性能计时之外。

这个参考定义测服务编码链路相对其1080p输入的空间编码损失，包含同样的540p缩放参考，但不量化4K相机原片制备为1080p的损失、60→30的时间分辨率损失、音频或实际显示效果。首轮记录见 [CQF 540p画质检查](validation-quality-cqf-20261018.md)。

## 首次网络读取与双预算审计

第二批新增`reader_queue_capacity=1..8`（默认8）。reader在读下一包前等待名额，避免先读后额外持包；主owner每轮至多取一张解码帧，再继续喂包。已有async早出帧和自适应预算API仍启用。前期对照为reader1、async queue1/max_threads2/min_threads1/budget_slots1；最终示例保持队列大小，改为min_threads2/budget_slots2，实际目标固定2。背压可能移到内核或上游，必须同时报告实际吞吐，不能只据app读取后的时延宣称实时。

受限CRF模式按约1秒的视频预算预测和反馈，不施加单帧硬上限。本轮可显式选`--audit-burst-bits 1100000`，声明1秒预算容量；绝对桶另声明最多1个窗口的启动额度，源桶首次写出时只用已经完成的过去1秒源视频初始化（不超过声明容量），未收到的源视频不授信。既有CQF审计维持初始额度0的旧合同。这不是新增发送等待，精确滚动1秒峰值仍必须单独检查，不将“秒级附近”自动解释为任意超限容差。控制器参数见[vvenc说明](vvenc.md)。

`decoder_pump=true`是单个直接输出的实验开关，默认false；outputs数组不接受。独立线程串行拥有async send/receive/stats，只用一个派发槽及已有有界async队列，并在正常EOF和取消时先join再释放decoder。follow_source、无过载策略时，在解码后先完成原有PTS校验和整数降帧，保留I/首帧/指定SEI，避免随后会丢弃的画面占用派发槽；不增加丢帧。每张保留画面在派发槽中也被跟踪，原始opaque_ref析构引用保持到输出owner接管。新增async_ready与dispatch_queue时间点，后者包含在setup/dispatch总阶段里，不能再与总阶段相加。

在RTMP配置中加 `network_trace=true`、`encoder_max_future_frames=4`，可记录每个源视频包和每张实际写出的画面。前者只支持plain RTMP，源媒体消息首个chunk首字节的TCP recv完成时间在协议层绑定到合成FLV tag位置，经过探测缓存、完整AU parser、decode/filter opaque_ref和编码PTS ledger保留至实际mux/flush。缺失/溢出明确失败；不从network_observer推测帧归属，不将av_read_frame调用开始或返回时间冒充首次读取。当前追踪扩展不支持RTMP aggregate、TLS/tunnel、seek或fake-duration插入。

`network_to_mux` 对每个成功写出的画面采样，包括首次I、重复I与output-key独立分组；`network_read.exceeded` 统计严格大于300ms的画面。窗口计数与分组按原有报告窗口清零；lifetime保留真实最大值。旧ingress_to_mux与HTTP-handler-to-first-mux边界不变。network_trace会通过既有有界logger强制输出全量源包/出流帧明细，仍不让生产者等待磁盘；审计要求无logger丢失、帧计数及原始时间关联完整。

`network_to_mux.slow_count`沿用通用`slow_frame_ms`阈值；300ms合同使用专门的`network_read.exceeded`及窗口`network_deadline_exceeded`，network_read中的画面分组也使用300ms，不能用通用slow_count代替合同计数。

RTMP入口发现network_trace后，在测量和结构验证完成之后自动执行 `scripts/audit-live-budget.py`。审计1100kbps时配置 `maxrate=1100000`，可用 `--audit-burst-bits 55000` 指定两只独立审计桶各自最多55kbit突发，初始额度为0；这是约50ms×1100kbps的声明额度，不是300ms的发送缓冲。源桶按过去1秒**已经组装完成**的RTMP视频消息统计，未读完的包不提前授信；绝对桶按固定maxrate补充额度。借款记为负余额继续结算，不在每次超支后清零。同时独立检查出流DTS和实际write墙钟的任意半开滚动1秒码量是否超过maxrate。视频AVPacket payload及首包时的codec configuration计入，音频和FLV/RTMP封装不计入码量，仍计入实际等待时延。

审计工具本身不控制编码。原生CQF的maxrate仍是近似约束；新`constrained_crf`通过编码前QP预测和实际AU反馈控制，两者均不以等待输出、额外丢帧或抹掉违规满足预算。`COMPLETE.json/status=passed`只表示测量及完整性检查完成，另外的 `target_conformance` 和每轮 `budget-audit.json/conformance_status` 才表示是否同时满足时延/前视/固定码率/源桶与突发合同。审计CLI可加 `--require-conformance`，违规则返回非零。源码及SDK变化后必须使用新目录，旧结果不能据新代码重新标记通过。

`rtmp_tcp_nodelay=true`显式开启服务plain RTMP输入/输出的TCP_NODELAY；RTMP benchmark同时为自有源fixture与接收端开启，实际命令保留。默认false，既有其他协议不受影响。这是可对照的传输选项，不保证降低所有网络的时延。`BUSY_WAIT_TIME=0`可用于实验VVenC已有的线程池等待策略；入口记录该环境值，服务默认不覆盖它，不能将环境不同的轮次当作同一配置。

首轮1100kbps合同、精确依赖身份、失败尝试、性能及画质见[本轮验证](validation-live-budget-20261018.md)。

[1100kbps审计配置](../examples/rtmp-vvc-cqf-audit-1100k.json)复现本轮调优请求；这是未满足合同的实验配置。benchmark前将input替换成本地有限1080p60/GOP120源文件，服务实际请求会改为自有RTMP连接。复测忙等0时在启动整个benchmark或服务进程前显式设置环境变量：

~~~sh
BUSY_WAIT_TIME=0 bash scripts/bench-service.sh rtmp \
  --config /path/to/local-input-workload.json --input-fps 60 --input-gop 120 \
  --mode paced --seconds 180 --repeats 1 --audit-burst-bits 55000 \
  --save-output-dir out/saved-output-unique --result-dir out/checks-budget-unique
~~~

第二批[受限CRF请求](../examples/rtmp-vvc-constrained-crf-540p30.json)启用编码前双预算反馈；将input替换为本地有限源，按同一入口把审计容量改为`--audit-burst-bits 1100000`，新建输出与检查目录。真实RTMP请求可直接使用示例URL，但输入须自行满足1080p60/GOP120；服务不重配上游编码器。性能、码率/时延合同、源I/3B结构、PSNR/VMAF及后续尾部超时对照均见[第二批记录](validation-constrained-crf-20261018.md)。该示例是实验基线，不是任意内容的300ms保证。

`codec_trace=true`另启用原生软件HEVC及live VVenC内部逐帧时间点和线程CPU聚合，要求单个直接输出与future guard，强制完整trace。RTMP入口自动运行`audit-codec-phases.py`并保存`codec-phase-audit.json`。正式性能先关诊断，另做开关A/B；CPU求和不能与并行墙钟相减。事件、限制及HEVC ABR 8Mbps新输入的独立窗口见[内部打点记录](validation-codec-phases-hevc-20261018.md)。

解码器私有worker出队与另一worker的画面重建可能并行；审计在对外async队列处检查二者均已完成，不强制私有出队晚于重建。`worker_output_lead`只表示两分支重叠，不能加进时延分解。原始时间戳保持不变，解码CPU、参考等待及后续frontend顺序仍独立校验。VVenC trace v3的单线程CPU采样完全放在对应墙钟区间内，审计拒绝超过区间加2µs量化余量的数值；多线程CTU CPU另作累计，不施加单线程上界。

字节参考候选需在同一SDK下比较开关关闭/开启及10bit控制组，保存实际码率匹配、独立码流结构、PSNR/VMAF与逐帧实际写出时延。诊断记录`byte_me_blocks`及`byte_reference_pack_us`以确认热点命中与打包代价；字节缓存与16bit工作缓冲同时存在时不能声称参考内存减半。性能、质量与CPU采样分开运行，采样结果仅用于热点定位。
