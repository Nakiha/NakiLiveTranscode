# VVenC字节参考与整数搜索SIMD（2026-10-19）

完成样本/残差类型分离、完成图字节参考缓存和单向整数运动搜索的scalar/SSE/AVX2 SAD，接入live开关并通过同payload/CTS回归。这是继续推进原生8bit的混合阶段：16bit重建、亚像素和其他工作路径仍存在，不能称全链路原生8bit或参考内存减半。

同版SDK的ABBA四轮正式对照，byte参考平均CPU 189.360s 对wide参考 187.920s（+0.766%），没有整机省算力证据；平均峰值RSS增加 8.041MiB。四轮均通过本次有限回放的300ms目标，但10bit控制组有7张超时，前期冷启动轮也曾达到530.376ms，因此仍不能宣称尾延迟稳定达标。不开启此实验开关时保留旧参考路径，默认关闭。

## 实现、数据所有权与剩余范围

| 位置 | 本批实际实现 | 仍未迁移 |
| --- | --- | --- |
| 样本与残差 | Sample8=uint8_t；Residual/WorkingSample=int16_t；残差buffer接口明确有符号类型；TCoeff仍32bit | 全局Pel仍是工作样本，不能统一换成uint8_t |
| 参考图 | 最终重建、滤波和边界完成后生成带margin的字节平面，发布isReconstructed之前完成；就绪映射缺失显式失败 | 保留16bit重建图，新增byte缓存有额外内存与拷贝 |
| 整数ME | 单向、非加权路径每个ME原图块打包一次，TZ/全搜索走unsigned-byte SAD；scalar/SSE/AVX2及阈值提前终止 | 双向/加权、分数像素、HAD、IBC、DMVR、原图与MCTF等仍走原精度 |
| live接入 | encoder_native8_reference=true，受8bit编码/live CQF/future guard/直接输出约束；默认false | 不是成熟原生CRF或硬VBV |
| 诊断 | ABI v3增加byte_me_blocks、byte_reference_pack_us，trace关闭不做逐块原子累计 | CPU累计不是并行关键路径墙钟 |

参考图使用既有owner与重建发布屏障，不增加未来画面、输入队列或参考就绪绕过。配置LiveNative8Ref使用int类型，避免参数解析器把字符0当非零整数。源码实际允许8线程时内部IFP自动标志，但MaxParallelFrames=0使maxPicEncoder=1，没有多画面编码重叠；早期文档把“串行画面”等同于“IFP标志必为0”的表述不适用于当前配置。

## 固定输入与测量范围

输入沿用冻结forest复杂片段：1080p60 HEVC Main8，2-pass ABR目标8000kbps、实际7943.79253kbps，GOP120、3B，BT.709 limited，原片30–90s。文件out/codec-phases-20261018-a/forest-hevc-abr8m-1080p60-g120-b3-offset1.flv，SHA256 decaae53011dcc8766b21123144b85aec5075eb6d2b256001f2200bdaaf59968。媒体不入Git。

出流960×540p30，显式60→30，跟随源普通I、仅启动IDR，P-anchor+3B、future guard4（实际最大3）。live_3b_2s_speed/faster，8编码线程，decoder线程2/预算2/pump=true，reader与frontend有界小队列，mux_wait20ms、TCP_NODELAY、BUSY_WAIT_TIME=0；AAC透传。保留QPA/SAO/deblock/过去MCTF，沿用已记录的QT16/chroma8、搜索64、RDOQ/ALF关闭工具组合，没有用本批开关改变这些决策。

constrained_crf=32，双预算取滚动1s源流与maxrate1100000bps，声明1.1Mbit桶/启动授信。1100kbps是视频上限，不是必须填满的平均码率；不施加每帧1100/30硬限额。统计视频payload与一次codec config，排除AAC及封装/协议开销。

六轮串行paced loopback：ref16/ref8/ref8r2/ref16r2组成ABBA，编码精度和输入均8，只切字节参考开关；trace8另开完整诊断；ten10为同SDK输入/编码均10的控制。每轮1770目标后API停止，实际1781张全部写出计量。有限60s素材回放不是长稳或吞吐容量证明；正常EOF另在功能测试覆盖。时延是逐帧首个相关网络读取到对应包实际write/flush，包含重排/排队，不是源到屏幕或线缆时间。

| 轮次 | 工作/参考精度 | 视频kbps | CPU-ms/帧 | 峰值RSS MiB | 最大时延ms | >300ms帧 | 目标合同 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ref16 | 8 / 16 | 829.112 | 105.154 | 187.457 | 254.329 | 0 | passed |
| ref8 | 8 / byte+16 | 829.132 | 106.204 | 195.465 | 255.632 | 0 | passed |
| ref8r2 | 8 / byte+16 | 828.675 | 106.440 | 195.734 | 253.451 | 0 | passed |
| ref16r2 | 8 / 16 | 829.384 | 105.873 | 187.660 | 252.128 | 0 | passed |
| trace8 | 8 / byte+16 | 828.748 | 107.805 | 195.543 | 279.767 | 0 | passed |
| ten10 | 10 / 16 | 827.885 | 107.951 | 188.480 | 410.138 | 7 | failed |

所有轮均I30/P445/B1306，future max3、无过载丢帧，音频有输出且丢弃0。测量gate通过仅表示设施运行/关联成功，目标合同单列，不能把ten10的failed隐藏成passed。

| 轮次 | 墙钟滚动1s峰值bit | DTS滚动1s峰值bit |
| --- | --- | --- |
| ref16 | 1061840 | 1053560 |
| ref8 | 1061840 | 1053560 |
| ref8r2 | 1061840 | 1053560 |
| ref16r2 | 1061840 | 1053560 |
| trace8 | 1061840 | 1053560 |
| ten10 | 1060952 | 1054040 |

全部视频滚动1秒与声明突发预算通过；ten10失败项为时延。ABBA平均CPU约105.514→106.322ms/帧，平均峰值RSS 187.559→195.600MiB。只有两轮每模式，不能以约1%波动宣称稳定速度收益；byte路径在本机没有回报新增缓存成本。

## 同实际码率PSNR/VMAF

原有540p10bit参考、1023峰值、精确PTS、bilinear缩放与vmaf_v0.6.1；8bit解码结果提升到同一10bit计分精度，参考不先降为8bit。每轮1781张全部计分，不补重复尾帧；不覆盖准备1080p的损失、降帧流畅度、音频或显示。

| 比较 | 实际码率差% | 共同PTS帧 |
| --- | --- | --- |
| ref8_vs_ref16 | 0.0023 | 1781 |
| ref8r2_vs_ref16r2 | 0.0856 | 1781 |
| ref8_vs_ten10 | 0.1504 | 1781 |
| ref8r2_vs_ten10 | 0.0953 | 1781 |

全部预定比较低于1%实际码率匹配门槛，不能用同QP替代实际码率检查。

| 轮次 | PSNR-YUV dB | VMAF均值 | VMAF P05 | VMAF最小 |
| --- | --- | --- | --- | --- |
| ref16 | 30.852559 | 71.062244 | 57.448055 | 41.020239 |
| ref8 | 30.849819 | 71.062008 | 57.283264 | 36.295053 |
| ref8r2 | 30.845890 | 70.995306 | 57.316206 | 38.368878 |
| ref16r2 | 30.849612 | 71.030213 | 57.347472 | 35.675713 |

第一组byte−wide分帧型结果（MSE合并后计算PSNR；不平均dB）：

| 分组 | 帧数 | byte−wide PSNR dB | byte−wide VMAF |
| --- | --- | --- | --- |
| I | 30 | -0.080399 | -0.481635 |
| recurring_I | 29 | -0.081945 | -0.498243 |
| P | 445 | -0.005798 | +0.012722 |
| B | 1306 | -0.000058 | +0.006407 |

功能测试在同输入/同参数下验证字节与宽参考payload/CTS精确相同。实时双预算受实际写出时序反馈影响，因此有限回放的码量、QP和单帧分数仍可变化；不能把这些质量差全部归于整数SAD数值或概括所有素材。ten10质量计分在用户要求暂停时尚未完成，没有可发布的分数；同实际码率的8bit/10bit画质结论仍待补齐。ten10的性能测量已完成，但300ms目标失败。

## 等待与计算，以及仍保留的超时

trace8实际命中字节ME 3874535块，参考打包均值0.260966ms/帧、最大0.978ms；已确认走到新路径，而非仅设置开关。以下均是该诊断轮，不能移植为关闭诊断轮的内部耗时。

| 帧型 | 总驻留均值/最大ms | 解码驻留均值ms | 编码驻留均值ms | mux均值ms |
| --- | --- | --- | --- | --- |
| I | 125.761 / 138.294 | 32.506 | 86.177 | 0.877 |
| recurring_I | 126.445 / 138.294 | 32.429 | 88.096 | 0.869 |
| P | 109.848 / 148.816 | 50.286 | 38.886 | 1.734 |
| B | 197.207 / 279.767 | 73.283 | 109.467 | 12.963 |

内部阶段单位ms（列间存在包含/重叠，不可直接相加，CTU CPU为工作线程累计）：

| 输出帧型 | 解码CPU均值 | 显示重排均值 | 等未来输入均值 | GOP派发等待均值 | 其中等前图均值 | CTU墙钟均值/最大 | CTU累计CPU均值 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| I | 15.720 | 12.911 | 0.000 | 51.111 | 50.893 | 16.406 / 21.847 | 58.878 |
| P | 8.598 | 36.731 | 0.000 | 0.401 | 0.000 | 25.060 / 38.590 | 81.956 |
| B | 5.417 | 57.154 | 41.121 | 51.437 | 44.306 | 11.745 / 24.914 | 35.341 |

I帧等未来输入为0，不是攒4张再编码I；平均约51ms派发等待中50.893ms来自前串行图未完成。B同时承担未来锚与前序图依赖。整体CTU墙钟均值15.151ms、最大38.590ms，但累计CTU CPU均值47.384ms，不能将编码驻留或多线程CPU当作单帧计算墙钟。输入reader FIFO在最坏帧上仅约0.06–0.08ms，继续单纯压reader队列不能解决这些依赖。

trace8最坏B、PTS28.533s总279.767ms，其中解码驻留171.061、编码驻留80.009、mux26.043ms。ten10最坏B、PTS25.467s总410.138ms，解码233.899、编码130.914、mux43.035ms；该控制轮关闭内部诊断，不能再细分其解码CPU/DPB等待。时延没有用丢帧或缩短测量边界消掉。

前期D轮wide参考冷启动基线也保留：frame117/PTS2.933s总530.376ms，网络组包1.342、demux缓存0.044、reader FIFO0.073、解码99.977、filter0.478、编码397.274、mux31.106ms；frame119解码驻留346.025ms。输入packet119→120首次读取相隔319.404ms、媒体DTS只推进17ms，随后I包163648bytes（PTS3.0/DTS2.967）。原FLV视频/音频tag DTS在该区间有序，排除已检查的文件tag乱序，但没有sender/socket关联证据，不能断言是发送端、线缆或主机调度哪一项，更不能把397ms全算成编码CPU。原始失败证据在out/native8-ref-20261019-d/及e/early-cold-worst.json、source-tag-order.json；后续通过轮不覆盖它。

## 审计修复与验证

解码私有worker_output与另一worker的reconstructed可能并行，ff_thread_async_receive在STATE_INPUT_READY按提交顺序接合。审计以二者max作为已完成分支，在对外async_queued/front-end处验证；worker_output_lead只记录重叠，不作为可加阶段，原始时间戳不改。没有绕过解码参考、伪造未来状态或删除必要同步，也没有修改本批FFmpeg解码实现。

VVenC四个单线程采样窗口改为CPU结束先于wall结束，修复一次GOP-init CPU31µs落在11µs墙钟外的测量错误。审计仍严格拒绝CPU超过wall+2µs、负CPU、byte打包超过finalize；未放宽成忽略错误。早期E诊断因此失败后，使用最终SDK重跑全部F性能及四份8bit质量。ten10计分在用户要求暂停时中断，没有拼接E时延当F结果。

最终full selected gate out/checks-native8-ref-functional-20261019-f通过SDK、native18、organization、quality-tools5、codec-phase-tools8、codec-phases8、cqf-rate、live-budget及entrypoints13。native覆盖字节平面margin/reset/越界拒绝、scalar/SSE/AVX2精确SAD及early-exit/奇宽/尾部/guard page；C API以只读mmap输入、forced I、短尾EOF及8线程/IFP配置验证payload+CTS与旧路径相同。集成8种精度/入口/字节参考/诊断组合均8编码线程，独立解码检查实际位深、I/3B/PTS并比payload。预检曾发现选项int8解析、32键上限、过严IFP标志、审计并行join及测试注册问题，均修正后完整重跑；历史失败目录保留。

## 版本、符号和复现

VVenC 8a1c5e6f5defc2ec2c95fa67456be7af9cc0c64c已独立提交推送，FFmpeg a4145412670af5a52d2e5de4a8884f9159faf2ac本批未改。测量时父仓gitlink HEAD 28eb43651ebb33c173fa18b413ec2dea7b8e4387；应用/测试/文档的完整工作树由下述源码gate与快照绑定，不能把仅gitlink提交描述成全部已提交推送。

SDK identity 296a8b23c7d105f1e57789d793b32119060f220968d407ed85bcea0023196c4e，依赖key 32a7781b416741e344626847，包含两子模块pin与构建配方。VVenC Build ID 5d9bdc269a810436257b30142cb22f3eb531a860。out/native8-ref-20261019-f/symbols-vvenc、symbols-ffmpeg、symbols-app独立保存匹配DWARF；dependency-binding.json逐项核对15个已安装模块Build ID及debug哈希，不能使用C/D/E旧符号替代F。

完整汇总out/native8-ref-20261019-f/summary.json；媒体manifest out/network-output-native8-ref-{ref16,ref8,ref8r2,ref16r2,trace8,ten10}-20261019-f/；性能gate out/checks-native8-ref-{label}-20261019-f/；四份已完成8bit质量out/quality-native8-ref-{label}-20261019-f/（ten10目录为暂停时的未完成结果）。生成输入/出流/日志/符号均在ignored out内，质量成功后仅清理本轮显式raw中间文件，不改冻结媒体。

源码/二进制/门禁哈希归档out/native8-ref-source-20261019-f/，父仓与子模块patch使用git diff --no-textconv --binary，sources.zip保存未提交源码并逐文件回读核对，gate源码全部与本次测量一致。未包含无关用户文档；原内容SHA保持不变。


## 用户要求暂停时的交接

2026-10-19用户要求“主仓也推了吧，任务暂时进行到这”。六轮性能及四份8bit质量已完成，10bit离线质量计分中断，等待中的诊断CPU采样已停止，本批未产生可引用的热点采样结论。已生成的输入、出流与中间结果保留在本地，不入库；PAUSED.json记录此次停止的任务进程与未完成项。

下一次继续时，先在新质量结果目录复用ten10已保存出流完成计分，再单独做热点采样；不要覆盖本批未完成目录或把其他轮分数移植进来。进一步字节化应先确认SAD、块打包、亚像素/双向预测和MCTF各自的真实CPU占比，再决定移除宽重建镜像的范围。当前能确认参考缓存打包和额外内存成本，尚不能声称定位了所有计算热点。300ms仍需处理输入/DPB及串行参考依赖的尾部，保留源I、3B及最多4帧约束。
