# HEVC 连续 POC 提前输出、前台排队与编码领取调度验证

2026-10-22。实现提交 `a0bfa0c`、`25e6c2897bc7da536837a5b81143447ca5dcb39b`；FFmpeg 独立提交 `94bd940f1c4682dfce13e8ae43891df01621e16e`，VVenC 独立提交 `b894d109e1b0522c9421d6966765c93ba75afba9`。SDK dependency key `f57d98cb2c23e7ccc07c81e0`，source identity `aac0fcb982a5d9297c74f377f8f50b5b29d788e7f4de77546e99f726570259d6`。历史目录内为 n9 fork；不使用 n7 性能作为对照。

## 结果及测量边界

同一份1080p60 HEVC Main8、GOP120/B3、AAC、60秒输入，经 paced loopback RTMP，输出540p30 VVenC faster/8 threads，受限 CRF32、1100kbps 上限、来源码率约束、future guard=4。保留 decoder pump、2个解码线程、各队列capacity=1、`codec_trace=true`、`BUSY_WAIT_TIME=0`。源文件 SHA256 `decaae53011dcc8766b21123144b85aec5075eb6d2b256001f2200bdaaf59968`。

对照为上次 IDR-only 的 `out/rtmp-bench-kwb0ua3d`；本次最终测量 `out/rtmp-bench-hmptkv4e`。两轮不是紧邻随机化 A/B，开启诊断，不是容量测量或来源到屏幕时延。原轮写1781帧，新轮写1774帧；受控停止时交付前缀长度可不同。以下按两轮共同的1774个源 PTS 比较，其中输入 I/P/B 为30/858/886，避免输出帧选择不同造成偏差。单位均为 ms，除注明外为均值。

| 指标 | IDR-only | 本次 |
| --- | ---: | ---: |
| 输入 P 解码驻留 | 82.289 | 53.892 |
| 输入 B 解码驻留 | 52.798 | 19.163 |
| P 重建后等 DPB 放行 | 67.777 | 41.873 |
| B 重建后等 DPB 放行 | 37.201 | 12.075 |
| P 重建且被选中后等 codec 交付 | 5.617 | 2.788 |
| B 重建且被选中后等 codec 交付 | 3.148 | 2.795 |
| P 前台加应用分发队列 | 0.122 | 4.650 |
| B 前台加应用分发队列 | 5.994 | 4.075 |
| B 上述两队列 P95 / 最大值 | 48.899 / 67.681 | 13.492 / 26.952 |
| 服务入队到实际 mux/flush | 171.418 | 139.401 |
| 上述实际写出时延 P95 / 最大值 | 236.438 / 252.030 | 184.603 / 201.292 |

共同画面实际写出均值下降32.018ms（18.68%）。不能只报告 async 前台队列从 B 的5.915ms降到0.018ms：本次应用分发队列仍有 B 的4.057ms，且 P 队列增加。全部共同画面的两队列均值合计从3.069ms增到4.286ms，P95从41.348ms降到13.030ms。此实现减少长时间连续占用 owner、推进解码与缩短总时延，并没有消除所有排队，也没有通过挪动时间戳隐藏它。

本次全部3553个解码画面为 I/P/B=30/888/2635，解码驻留均值15.998/53.265/8.582ms；仅保留写出的 B 为19.163ms，不能混用两个总体。首次 I 为15.018ms，其余29个 I 均值16.032ms。

写出画面的非重叠解码分解如下，列可相加得到解码驻留；CPU和参考等待属于解码窗口内子指标，不再相加。

| 输入类型 | 调度 | 自身重建窗口 | DPB | codec交付 | async移交 | 前台队列 | 应用交接 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| I | 0.108 | 15.783 | 0 | 0.005 | 0.036 | 0.050 | 0.017 |
| P | 0.110 | 8.709 | 41.873 | 2.788 | 0.382 | 0.019 | 0.012 |
| B | 0.114 | 4.038 | 12.075 | 2.795 | 0.111 | 0.018 | 0.012 |

离线检查这份完整、连续 POC 的源，以当前及所有先前显示画面的重建完成时刻为顺序下界：新 P/B 在该下界之后额外等 codec 的均值仅0.0066/0.0069ms，最大0.027/0.028ms。剩余约2.8ms主要是先前显示的 B 尚在重建；不能越过它输出 P 或后续 B。连续 POC 优化已经把此源中可证明不必要的 DPB 等待移除；P 的剩余重排仍需要等未来到达的前序 B，并非应当全部归零。这个离线下界是解释证据，不能替代一般直播流的在线安全证明。

预算审计的 `latency_ms` 实际边界是 **RTMP 视频消息首字节读入到实际 mux/flush**，比服务入队边界更早。本次全1774帧为均值140.383ms、P95 185.605ms、最大202.363ms，旧轮全1781帧为172.299/237.752/253.697ms。前次 IDR 报告将预算字段误称为 ingress-to-mux；这里明确纠正口径，原报告与原始证据保留。本报告表格使用逐帧 `ingress_to_mux_ms`，两者不能混称。

## 实现与安全边界

- 新 HEVC 私有选项 `live_poc_early_output` 默认关闭，服务显式开启。仅从有效 PTS、单层、软件、非 field_seq、默认 skip_frame 的 `IDR_N_LP/POC0` 建立水位。DPB 最小 POC 等于已选水位+1时，没有整数 POC 可插入其间，允许提前选择；缺口继续原 SPS reorder/buffering 规则。CRA、IDR_W_RADL、前导图及不满足条件者不建立该前缀。异常回退或新序列关闭前缀，直到下一个合格 IDR。
- 水位和 OUTPUT 标记在 `finish_setup` 之前传播到后续帧线程，防止重复输出。私有 DPB 前缀 FIFO 持有 AVFrame 引用，保证非参考 B 清 OUTPUT 后仍存活；整个 AU 的解析、重建、film grain/checksum 成功后才发布，错误丢弃私有引用。flush 清空 FIFO/水位，继续原有通用后处理与 async 按任务顺序退休。
- 保证完整有效输入 EOF 的像素、显示顺序、PTS、best-effort timestamp、时长、尺寸、格式、SAR及色彩一致。`pkt_dts` 是触发输出的压缩包 DTS，会随提前交付改变，不是显示 PTS；服务不使用它重定时。取消、flush、`no_output_of_prior_pics` 不能撤回已交付的前缀，因而不要求不同驻留策略在中断时交付同样长度的前缀。
- 解码 pump 使用一个视频槽和一个不可变音频参数/包槽，两槽均就绪时交替领取，owner仍独占音频注册和 mux。没有增加帧池或 IPC 媒体通道。
- VVenC 的 receive pending 不只是读已完成队列，它可能同步推进编码计算。单个直接输出、decoder pump 模式中，正常每领取一个 AU 就回输入处理；输入暂时无进展时继续取已知待取输出。EOF恢复完整排空，NULL-send EAGAIN先取包再重试。分支保持原独立 owner 路径。没有改 VVenC 编码算法、未来帧上限或受限 CRF 规则。

## CPU 审计及失败记录

第一轮 `out/rtmp-bench-si5c2ceu` 的流媒体和码率审计通过，但严格 codec audit 在 encoder trace v3 失败：9个单线程窗口的 CPU 比 MONOTONIC 多于5µs，最大8µs。该轮没有 RAW 字段，不能逐条追溯重判通过，失败状态保留。同机前次独立嵌套 CPU/RAW/MONOTONIC 探针已复现可校时时钟与线程 CPU 的速率差异；本次旧 trace 异常与其相容。

VVenC trace v4 给 preprocess、GOP init、dispatch、finalize 追加 RAW 外围时长，采样顺序为 MONOTONIC开始→RAW开始→CPU开始→工作→CPU结束→RAW结束→MONOTONIC结束。严格审计仍使用原5µs余量，不裁剪 CPU，不放宽阈值；原 MONOTONIC 阶段链不变，并保留两种超差指标。历史 encoder v2/v3、decoder v1 沿用旧规则。公共结构扩展后 VVenC、wrapper、应用已一起重建并绑定新依赖 key。

最终3553条 decoder trace v2 的 RAW 检查全部通过；1774条 encoder trace v4、共7096个单线程 RAW 窗口全部通过，最大正超差1µs。此轮 MONOTONIC 最大正超差也仅1µs，不能据此抹去旧轮异常。并行 CTU CPU仍独立求和，MCTF worker CPU仍未聚合，不能与阶段墙钟相减。

## 验证、归档与本地状态

- `scripts/build-decoder.sh linux`、`scripts/test-decoder.sh linux` 通过；选项关闭的 H.264/HEVC B0/B3、2/12线程、完整元数据包括触发 DTS、10-bit、尺寸变化、API、gate、错误/flush/close仍通过。旧 n7 文件仅用于冻结的正确性对照。
- `test-idr-early.py --poc` 共56组、35456个比较画面，像素及除输出触发 DTS 外的全部字段相同；覆盖完整3600帧输入、2/12线程、capacity=1、三轮回放、10-bit、multi-slice、CRA/RASL、真实RADL、缺失PTS、POC缺口及带PTS尺寸切换。另与上次 IDR-only 的3600帧结果逐帧比较，2/12线程各自全部相同。
- 首 IDR suffix decoded-picture MD5 故意破坏，在严格校验错误前不得交付坏帧；flush后重放原始96帧，像素、PTS、顺序及元数据均与 legacy 相同。开启两项优化的 pause、gate、flush、error、close、api 模式均通过。
- 最终 `scripts/build-service.sh`、quick的19项native和10个Python suites通过；编码阶段 EOF240→120帧、诊断开关载荷一致、AAC/MP3/HE-AAC交错与参数变化（含pump）、受限CRF回归通过。
- ASan/UBSan 的 service、lifecycle-media、独立新队列测试通过，另用同一 sanitizer 应用跑 codec-phases 与 audio-signatures 通过。范围为应用/yyjson/测试辅助，SDK及系统库未插桩。
- RTMP `measurement=complete`、`conformance=passed`、严格 codec audit passed，日志无丢失/错误。实际写出滚动1秒视频最大1054688bits，1100000上限无违例，future实际最大3，300ms deadline无超限。FLV直接捕获1774视频包、2775音频包，独立验证/解码通过。受控关闭的receiver返回12、RTMP对端关闭未确认，保留 `verified_controlled_close` 及原始 unconfirmed 状态，不称正常EOF。
- 最终45个应用ELF、6个FFmpeg DSO、1个VVenC DSO的匹配Build ID DWARF分别归档，并逐一核对52个运行模块Build ID。默认 `out/bin` 已安装45个验收二进制，SHA256核对一致，原完整目录保存在 `default-before`。独立库提交已推送各自分支；父仓库本次实现与报告为本地main提交，尚未推送 main、发布此新SDK或声称远端CI通过。

证据总目录 `out/pb-early-20261022-014525/`。入口为 `correctness.json`、`baseline.json`、`first-failed.json`、`final.json`、`matched-comparison.json`、`*-frames.csv`、`quick-v2/summary.json`、`integration-v2/summary.json`、`sanitize-v2/summary.json`、`sanitize-{codec-phases,audio,dispatch}.log`、`cooperative-v2/checks/summary.json`、`expected-sdk-v2.json`、`sdk-build-v2.json`、`symbols-v2-*/manifest.json`、`runtime-symbol-check.json`、`default-install.json`。第一轮与最终RTMP原始目录分开保留。首轮编译类型修正、一次错误套件名及一次 probe 缺运行库路径的尝试均有独立失败日志，成功重试未覆盖这些记录。
