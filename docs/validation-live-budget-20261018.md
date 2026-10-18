# RTMP 首次读取、四帧前视与双预算基线（2026-10-18）

本轮完成逐帧首个网络读取到实际 mux/flush 的追踪、有限前视的原生 CQF 排空修复、双预算审计、完整素材基线和第一轮调优。**测量完成不等于客户合同通过**：当前原生 CQF 尚未实现 CRF 与双预算执法，1100kbps 的滚动窗口和 300ms 最大时延均有违规。

## 合同与可重复条件

- 输入 H.264 1920×1080、精确 60fps、闭合 GOP120；显式降为 960×540、名义30fps，跟随每个源 I，保留3B。VVenC `live_3b_2s`、`faster`、原生 CQF QP32、`maxrate=1100000`、QPA开启、MCTF只用过去参考、frame-parallel及RC lookahead关闭。
- 最多4张**540p编码输入画面**的未来依赖；不是4张1080p60画面，也不是所有内部历史参考帧之和。wrapper逐个AU检查已提交最高显示序号与该AU显示序号之差，超限明确失败。本轮实际最大值3。
- 时延从源视频RTMP消息首个chunk首字节的TCP `recv`成功返回，到对应输出帧实际`av_write_frame`及flush结束。包括探测、队列、重排、滤镜、编码、音频交织和输出写入等待；不含尚未被app读取的内核/上游积压，也不含wire/display时间。
- 视频码量计AVPacket载荷加首包的codec configuration，音频及FLV/RTMP封装开销不计码率，但其等待仍计时延。固定上限另查DTS与实际写出时钟的半开滚动1秒窗口 `(t-1s,t]`。
- 两只独立审计桶均声明55,000bit最大突发、0初始额度；源桶的收入按过去1秒**已经组装完成**的视频消息码量积分，绝对桶按1,100,000bit/s积分。负余额保留，不逐包赦免。扣款粒度是完成写出的整AU；55kbit是本轮声明的严格审计参数，不是客户另行指定的突发值，也不是额外300ms缓冲。审计不执行等待、重编码或额外丢帧。

输入来自本机 `D:\video\action5\DJI_20250501110804_0049_D.mp4` 的30–90秒复杂树林段，离线制为1080p60 H.264（CRF18、veryfast、GOP120、3B、scenecut0、AAC128k），copy时间轴增加1秒以满足严格FLV的非负DTS范围。输入 SHA-256：`2e81a557fc8b1a6fb8634b2018a98752b3ea86d078a3ecb1b6252c60983ca2e7`，本地路径 `out/cqf-implementation-20261018-a/forest-1080p60-gop120-offset1.flv`。3600张源画面，完整性能轮在1770张目标附近API停止并排空，不伪造网络EOF；实际输出及取消尾部逐PTS核验。

本机 Ryzen 7 9700X（16个可用逻辑CPU）、Ubuntu WSL2、Linux 6.6.87.1；服务、源端、接收端为直接loopback RTMP连接，无中转服务器。`paced`按源DTS供包，但下游不够快时会受TCP背压；因此名义1080p60/540p30不等于已达到实时吞吐。无同时运行的画质分析或其他本项目benchmark；单次短样本不作目标机容量或长稳结论。

## 实现与身份

FFmpeg fork安装 `libavformat/read_origin.h` API1，plain RTMP opt-in `rtmp_read_origin=1`。成功recv时间沿RTMP分chunk重组、最多256项FLV tag位置账本、FLV demux opaque_ref和完整AU parser保留；parser拆并包、账本缺失/溢出明确失败。暂不支持aggregate、TLS/tunnel、seek或fake-duration插入。该机制不从socket observer猜测帧归属。

应用新增 `network_trace`、`encoder_max_future_frames` 和 `rtmp_tcp_nodelay`，默认关闭。每个源视频包记录不可变来源时间，每张写出画面经decode/filter的opaque_ref和编码PTS账本关联；全帧统计和有界异步logger保持分离。审计要求日志无丢失且sealed、源包序号/出流帧完整、来源相等、阶段之和覆盖总时延、状态最大值与明细相等。输入I/首I/重复I和output-key保留独立分组及不重叠窗口。

首次强制四帧检查暴露了现有wrapper每次新输入只取一个AU、已就绪B帧继续滞留的问题。VVenC增加opt-in `vvenc_encode_pending`，不提交新画面、不置EOF，在有界循环内推进已经完整的短组；PTS planner公布真实组长，CQF QP初始化不再等待下一组哨兵。wrapper先取完现有AU再接受新输入，普通NULL EOF排空保持原语义。仅开放serial live-PTS CQF，无RC lookahead、无未来MCTF或frame parallel；不把该入口扩大为通用异步API。

两库修改先独立提交并推送，父库gitlink已提交：

| 项目 | 精确身份 |
| --- | --- |
| FFmpeg | `46bebdf7b986da8c393cce8a2f369475cf291f5e` |
| VVenC | `9e59cfe50ec3e0379f3ae248790fc8354f9415c3` |
| 父库依赖pin | `928dd18` |
| dependency key | `232f548784151dddb1d875a2` |
| SDK identity | `96654a3b2224843fe021012154576566107b27ad01a5401e643ad0ac1cf9492f` |
| VVenC Build ID | `b7bd8ab890cd8691e0f9fd7cdf92de82833f543a` |
| 应用/工具源码摘要 | `3e7c71b44cbedc36dc711ef5eee1d8bb054df0435f3a5df1b04467481a04e4a4` |

匹配DWARF在 `out/network-budget-20261018-a/symbols-vvenc-final` 与 `symbols-ffmpeg-final`，manifest绑定精确pins/dependency key。`dependency-provenance.json`记录依赖身份；服务最终构建日志为 `out/build-network-app-20261018-f.log`。原有冻结n7/旧CQF结果不作本轮证据。

## 完整素材结果

两轮均使用相同输入与QP/maxrate。调优1同时改为8编码线程、异步解码max2/min1/budget2/queue1，并开启RTMP TCP_NODELAY（源fixture、服务和接收端均开启）；不能将组合收益单独归因于某一个开关。基线4编码线程、解码12/2/12/4、TCP_NODELAY关闭。两轮保留音频透传、50ms mux等待上限。

| 指标 | 基线4线程 | 调优1：8线程、小解码队列、TCP_NODELAY |
| --- | ---: | ---: |
| 实际输出帧数 | 1782 | 1781 |
| 中段实际FPS（去头尾各5秒） | 23.783 | 23.073 |
| 网络读取→写出均值 / 最大ms | 550.096 / 1052.293 | 394.040 / 640.546 |
| P95 / P99 ms | 745.111 / 955.368 | 565.773 / 603.187 |
| 超300ms帧数 | 1700 | 1341 |
| 首I / 后续I最大ms | 453.372 / 773.407 | 203.146 / 456.724 |
| 最大未来画面数 | 3 | 3 |
| 写出墙钟滚动1秒峰值kbps | 1325.296 | 1294.704 |
| DTS滚动1秒峰值kbps | 1591.320 | 1595.472 |
| 源桶 / 绝对桶违规事件 | 593 / 1038 | 401 / 783 |
| 服务采样平均CPU核数 | 3.839 | 5.662 |
| 服务采样RSS峰值MiB | 246.98 | 199.25 |
| PSNR average dB | 30.839115 | 30.849474 |
| VMAF mean / P05 | 75.743011 / 52.699972 | 75.918144 / 52.851702 |

最大时延下降39.1%，但持续吞吐没有改善，8线程增加了CPU成本，不能称为整体吞吐优化。源I均保留30张、2秒间隔，除首张IDR外为普通I，完整前缀保留3B，解码及取消尾部检查通过，无额外过载丢帧。`status=passed`仅表示测量完整；两轮 `conformance_status=failed`。

基线证据 `out/checks-network-baseline-20261018-f/summary.json` → `out/rtmp-bench-xgwtbq4x/`；调优1 `out/checks-network-candidate-20261018-f/summary.json` → `out/rtmp-bench-ja96mu3j/`。每轮 `1/budget-audit.json` 含完整窗口与两桶逐AU账本。压缩出流副本分别在 `out/network-output-baseline-20261018-f/output-1.flv`、`out/network-output-candidate-20261018-f/output-1.flv`，manifest记录哈希和原测量目录。

画质入口 `out/quality-network-baseline-20261018-f/COMPLETE.json`、`out/quality-network-candidate-20261018-f/COMPLETE.json`。对每张实际出流PTS选择对应源画面，用固定SDK的同一bilinear缩放及10-bit格式生成参考，原生540p全帧评分，PSNR峰值1023、VMAF `vmaf_v0.6.1`、n_subsample1，16张自身对比通过。两轮取消尾部相差1张，上表为各自全部交付帧，不将0.18分差异解释为已证实的质量改善。评分不包含60→30的时间损失、4K源制备损失、音频或显示。原始中间YUV仅成功后删除，压缩出流和逐帧分数保留。

两路相同PTS的1781张画面，VMAF均值分别75.741141和75.918144，配对均值差0.177003；`out/network-budget-20261018-a/quality-common-f.json`保留复核。每配置只有一轮，不作质量改善的显著性结论。

## 热点与优化方向

独立12秒scout在第3秒后采样6秒 `cpu-clock`/199Hz，4522个样本、0丢失、perf正常结束；`out/network-profile-20261018-e/profile-identity.json`绑定worker PID及启动时间、命令和原测量目录 `out/rtmp-bench-0jaus9pi`，匹配Build ID符号解析。此轮有采样开销，不混入上表性能。

16.25% self样本位于 `NoMallocThreadPool::findNextTask`，其次为RDOQ（3.23%）及inter residual/CABAC/运动搜索等；这只是采样占比，不是可直接回收的总墙钟时间。完整素材的平均阶段如下，decode字段包含异步排队/重排/等待，不能解释为纯解码CPU时间。

| 平均阶段ms | 基线 | 调优1 |
| --- | ---: | ---: |
| 输入队列 | 111.907 | 126.452 |
| 解码驻留 | 251.168 | 123.817 |
| 编码驻留 | 133.770 | 128.515 |
| mux/交织/写出 | 51.593 | 13.908 |
| 缩放滤镜 | 0.724 | 0.671 |

第一批已修复不必要的编码AU滞留，并实测较小解码队列与TCP选项的组合效果。后续优先检查有界reader队列、异步decoder驻留及编码线程任务调度；不通过增加B帧未来依赖或悄悄丢画面换时延。原生CQF的maxrate为近似约束，严格合同还需要编码前的复杂度/码量预测、两桶共同约束的帧级预算、QP/lambda调整和实际bits反馈。首I约351kbit，本轮55kbit原子AU突发参数必然超支；需明确I帧预算、允许的发送缓冲/突发及最坏画质退化规则，不能在输出后靠等待掩盖。

随后用相同源前12秒，串行完成三个筛选scout，均为小解码队列、TCP_NODELAY、QP32/1100kbps和四帧守卫。不是完整素材吞吐或单因素随机实验：

| 配置 | 中段FPS | 平均CPU核数 | 平均 / 最大时延ms | 原始目录 |
| --- | ---: | ---: | ---: | --- |
| 8线程，MCTF开，BUSY_WAIT_TIME=0 | 24.945 | 3.852 | 364.761 / 553.233 | `out/rtmp-bench-ymhu_la6` |
| 4线程，MCTF开，默认忙等1ms | 22.224 | 3.465 | 405.366 / 644.855 | `out/rtmp-bench-hll15ory` |
| 8线程，MCTF关，默认忙等1ms | 21.598 | 5.662 | 451.370 / 716.130 | `out/rtmp-bench-6l0eq9gi` |

入口证据分别为 `out/checks-network-scout-{busy0,four-threads,no-mctf}-20261018-h/summary.json`，均通过完整性和身份检查，合同仍失败。关闭MCTF没有实测收益，本轮不推广；选保留MCTF的忙等0配置作完整素材复测。忙等0使用VVenC现有环境变量，不修改默认等待策略，也不让所有线程无唤醒地睡眠。

### 忙等0的完整素材复测

`out/checks-network-busy0-20261018-i/summary.json` → `out/rtmp-bench-8txve76d/`。与调优1使用同一JSON，唯一额外变化是启动时 `BUSY_WAIT_TIME=0`；输入、应用源码、ELF及SDK身份检查保持不变。

| 指标 | 调优2：8线程、小队列、TCP_NODELAY、忙等0 |
| --- | ---: |
| 实际输出帧数 / 中段FPS | 1777 / 23.695 |
| 网络读取→写出均值 / 最大ms | 383.180 / 646.875 |
| P95 / P99 ms | 548.488 / 593.205 |
| 超300ms帧数 | 1277 |
| 首I / 后续I最大ms | 201.744 / 449.672 |
| 最大未来画面数 | 3 |
| 写出墙钟 / DTS滚动1秒峰值kbps | 1294.704 / 1595.472 |
| 源桶 / 绝对桶违规事件 | 404 / 821 |
| 服务平均CPU核数 / RSS峰值MiB | 3.849 / 198.56 |
| PSNR average dB | 30.859328 |
| VMAF mean / P05 | 75.964944 / 53.059423 |

相对调优1，采样平均CPU下降32.0%，实际中段FPS增加约2.7%；最大时延略高，因此不能推广短样本553ms作为完整素材最大值。相对初始基线，最大时延下降38.5%，吞吐基本不变，仍未实时30fps。所有30张源I、3B和无额外过载丢帧检查通过，合同仍失败。

`out/network-budget-20261018-a/payload-comparison-busy0-i.json`核对两轮1777个共同PTS的压缩视频包，DTS及逐包SHA-256全部相同；因此全文件VMAF的微小变化来自停止尾部长度，不能说忙等0提高编码画质。完整画质入口 `out/quality-network-busy0-20261018-i/COMPLETE.json`逐PTS评分1777张，identity guard通过，原始YUV成功后删除。出流为 `out/network-output-busy0-20261018-i/output-1.flv`，SHA-256 `e975d37ec92f7fd61558c2ed2c0978ecb8be2523f223a83418c38f8b7abf2d09`；画质目录也保留相同压缩出流。

平均输入队列122.613ms、解码驻留120.739ms、编码驻留125.181ms、mux13.328ms、滤镜0.656ms。剩余重点是减少已有画面在串行pipeline/reader/async decoder中的驻留，以及实现帧级预算反馈；并无证据支持继续堆编码线程或关闭MCTF。

本素材源视频平均约30.8Mbps、最低1秒约13.8Mbps，高于1.1Mbps，不能证明“源降到低码率后动态跟随”的控制效果；审计算法已有低/高源速率分别绑定的单元场景，实际动态执法尚未实现。

## 检查与失败保留

最终源码窗口 `out/checks-network-final-20261018-f/summary.json` 共8步通过：SDK、native、organization、live-budget、entrypoints、cqf-rate、read-origin、observability；源码/二进制/SDK前后身份均不变。native包含15项CTest。cqf-rate覆盖QP28/40/32、EOF、非整组源I、实际未来上限和过紧上限明确失败；read-origin验证探测缓存与200ms延后读取仍保留原时间，启用/关闭后的媒体载荷和PTS/DTS完全一致。

较早C窗口13步回归另含multi-output、frame-policy、vvenc、vvenc-pts、live-mux，通过但属于其当时源码窗口，不拼成最终全套通过。E窗口cqf-rate多输出半支路因新增循环后意外沿用QP32触发既有100ms准入保护；已将该功能用例恢复原QP40负载，未放宽保护，F窗口通过。没有重跑全套sanitizer、Docker、远程网络、真实低源码率动态控制或长稳。

失败尝试均保留：首次协议追踪因parser未转交opaque_ref失败（scout-a）；转交后四帧守卫揭示旧wrapper未及时取完AU（scout-b）；排空修复后阶段之和遗漏1微秒准备时间导致审计失败（scout-c），已新增显式阶段；较早perf12秒被正常worker停止打断，保留诊断但不视为干净采样。后续干净6秒采样与完整基线使用新目录，没有重标旧结果。

所有输入、出流、raw、日志、perf、DWARF及离线工具都位于Git忽略的 `out/`，没有加入仓库。父库应用/设施变更留在工作树供审阅；依赖库历史独立保留。

最终应用/工具及文档快照在 `out/network-budget-source-20261018-final`：`parent.patch`使用`git diff --no-textconv --binary`导出，`sources.zip`包含新增文件，manifest记录逐文件哈希、父HEAD、依赖pins和各成功入口的哈希；复制及zip解包内容逐字节核验。未包含用户原有的 `docs/next-metadata-batch-and-release.md`，其SHA-256保持 `4d69d926ba2d23018079250a99aba532f436db22b3b50f7627a92d20d5eabc77`。
