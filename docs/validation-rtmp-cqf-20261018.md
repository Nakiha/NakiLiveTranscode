# RTMP 1080p60 → VVenC CQF 540p30：2026-10-18

本机完整素材测量：2线程的中段输出约13.03fps，4线程约18.28fps，均不足以实时处理30fps输出。4线程提高约40.3%，服务进程树CPU核当量增加约70.5%；它是本轮更高吞吐的可选配置，不是达到实时目标的配置。两种线程数首轮的1770张共同画面按PTS比较，编码视频包SHA-256全部相同；该结论仅覆盖本次素材、配置和共同前缀。

## 输入、配置与边界

- CPU：AMD Ryzen 7 9700X，WSL2/Linux，16个可用逻辑CPU。CPU亲和性、内存、内核、SDK身份、ELF/helper哈希与源码摘要保存在原始结果中。本轮是正常优化构建，不是sanitizer结果。
- 源素材：`DJI_20250501110804_0049_D.mp4`，30–90秒林荫步行段，树叶/碎石细节、移动行人和持续运动视角；基于缩略图挑选，不宣称穷举素材后的最高复杂度。
- 离线准备：3840×2880中心裁成3840×2160，Lanczos缩为1920×1080，转为精确60fps；H.264/8-bit，输入x264 CRF18、GOP120、3B、关闭场景切换额外I，AAC 128kbps/48kHz。准备计算不在性能窗口内，服务仍只有AAC/MP3透传。
- 网络输入文件为 `out/cqf-implementation-20261018-a/forest-1080p60-gop120-offset1.flv`，SHA-256 `2e81a557fc8b1a6fb8634b2018a98752b3ea86d078a3ecb1b6252c60983ca2e7`。原素材哈希、完整生成命令和离线时间偏移见同目录 `input-manifest.json`。所有媒体、日志、符号和结果均在忽略的out/，没有加入Git。
- 输出：960×540、名义30fps、VVenC faster、原生capped CQF、QP32、QPA开启、maxrate6000000bps，保留MCTF、无未来MCTF，BIM关闭、MaxParallelFrames=0。`source_all`、`follow_source`，完整组3B，I边界及取消/EOF可出现短组。只改变encoder_threads=2/4，其余配置相同。不是自研CRF或固定目标码率。
- 拓扑：本地RTMP源监听端 → 服务拉取/完整解码/二选一/缩放/编码 → 服务RTMP推送 → 接收端copy封装。没有中转服务器、跨主机网络和播放器。
- 严格FLV的30fps重排需要正DTS预滚，输入在离线阶段显式增加1秒起点；源端从首个coded媒体tag起按相对时间节拍供流，原PTS不变。接收端也使用copyts。

输入文件60秒、3600张。直播连接不以文件EOF结束：源端保持连接，驱动在写出约1770张（约59秒）后按job_id停止，留出源尾部重排余量；实际计数逐轮保存。paced只是尝试按媒体时间供包，TCP背压可能推迟真实到达，不代表服务持续以60fps墙钟速度接收成功。throughput不主动限速。二者都按实际墙钟和媒体推进量报告。

## 有效完整素材结果

实时节拍各2次、不限速各1次。多次结果使用中位数。中段FPS剔除起始和末尾约5秒，实际端点保存在progress_window；总FPS包含起播和API停止/封装收尾。CPU为每250ms采样的controller及后代之和，RSS累加会重复计入共享页，短命进程可能漏记；不是cgroup内核峰值或物理核占比。

| 编码线程/供流 | 次数 | 中段输出fps | 总输出fps | CPU核当量 | 采样RSS峰值MiB | 首mux ms | ingress→mux平均ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2 / paced | 2 | 13.03 | 12.95 | 2.24 | 257.1 | 767 | 1235 |
| 2 / throughput | 1 | 12.87 | 12.78 | 2.24 | 260.7 | 754 | 1253 |
| 4 / paced | 2 | 18.28 | 18.15 | 3.82 | 265.2 | 719 | 891 |
| 4 / throughput | 1 | 18.35 | 18.23 | 3.82 | 268.3 | 706 | 885 |

2线程paced分别在136.953/136.560秒写出1770/1771张；4线程paced在97.160秒左右的首轮写出1771张，另一轮写出1774张。超出目标的少量画面来自状态轮询和停止之间继续完成的工作。所有有效轮次均无overload丢帧、无音频水位丢弃，预期二选一降帧独立计数，不能将其解释为完整60fps原样透传。

paced下平均encode驻留610→434ms，decode驻留353→261ms，filter约0.73ms。encode/decode数字包括排队、重排、后续喂帧或下游等待；它们不是单帧CPU计算时间，不能按这些均值直接分配CPU热点。ingress→mux平均降低约27.8%，其p95直方图上界仍为2000ms，不能据此声称p95精确值或改善幅度。所有服务时延止于实际write/flush，不含采集、服务ingress之前的等待、客户端上传或播放器显示。

2线程每名义媒体秒约需5.19 CPU秒，4线程约6.32 CPU秒。4线程的吞吐增加伴随更高总CPU成本。两种供流模式吞吐接近，编码线程数影响明显，而filter开销很小，支持优先排查编码计算及其背压；不把decode驻留直接解释成解码器自身消耗。

## 实现与核验

`rate_control=cqf` 将目标码率置零、开启QPA并传入原生qp/maxrate；与bitrate冲突或错误编码器组合明确拒绝。`output_fps=30` 在完整解码后、scale/编码前按已知60fps整数比例选择，源PTS和受保护I/SEI等画面保留，统计rate-drop与overload-drop分开。非整数比例/上转换/未知名义率明确失败，不提供补帧或通用硬CFR。

VVenC零目标码率没有RC lookahead，原PTS GOP路径首AU之后等待每次输入都产出AU，会阻塞下一短组所需的输入。fork修复仅对LivePtsGop零目标码率允许接受输入而暂不输出AU，EOF仍完整排空；码率模式保留原行为。库提交 `56600b42a483415521df4786f2bd42344937fe91` 已推送，父仓库gitlink提交 `718d415`。FFmpeg仍固定 `4bee779b3dd59407a07baa8d6fc386cf95d8398b`，没有修改其工作区。

VVenC Build ID `1b3fd211874045467223a91872474e337c2e6725` 的独立DWARF、manifest和构建配置保存在 `out/cqf-implementation-20261018-a/symbols/vvenc/`；SDK identity为 `8768c027eb2a15258dd953631716112b4b90f9194ee81349aa0811df5588ade0`。应用与工具为本轮工作区改动，完整源码摘要和前后相等检查见每组summary/source-before/source-after。

四组正式测量的应用/工具源码摘要均为 `649fe40e0fad3fa33b79527b75d64da922aa21860ee300d4746b66b22802c40b`，各组源码、二进制和SDK manifest前后检查全部相等。

相关8步检查通过：SDK consumer、native、video-rate、frame-policy、cqf-rate、vvenc、vvenc-pts、live-mux。新增cqf-rate独立核对480→240张、源PTS、I跟随、3B、仅首IDR、off-grid I保护、两路不同降帧率、QP响应、EOF排空和每完成帧的统计/trace账目。

六轮完整素材接收均由固定n9原生VVC decoder独立解码：画面数=服务实际写出数，显示PTS对应保留的源画面，所有源I的位置一致，完整I区间为3B结构，NAL仅首张为IDR/refresh，AAC存在。每轮都写出30张I。最后最多4张取消尾部不用于精确节拍/完整组断言，但仍须独立解码、计数并关联到保留源PTS；文件EOF完整尾组由功能回归验证。

## 优化试探与下一步

同一输入前720张（完整6个闭合源GOP）的短前缀throughput试探：2/4/8线程中段为14.08/20.62/22.39fps，CPU约2.18/3.73/6.15核当量。短样本比整段轻，不能直接替代完整素材结果。8相对4线程的收益小，CPU代价高，本轮不继续把更多线程当作主要优化方向。

4线程是已验证的可选吞吐提升：本段首轮2/4线程的1770个共同PTS，编码视频包SHA-256全部相同，见 `thread-payload-comparison.json`。这证明这批共同编码载荷没有因调线程而改变；不推广成所有素材、平台或线程数的质量等价保证。全局默认仍为2线程，4线程配置为显式选择。

直接RTMP配置见 [2线程基线](../examples/rtmp-vvc-cqf-540p30.json) 与 [4线程候选](../examples/rtmp-vvc-cqf-540p30-4threads.json)，使用时替换源/接收URL；两者都不承诺实时性。

要达到本场景实时30fps，4线程仍需约64%的额外吞吐提升。下一步应另开有性能采样的诊断运行，定位VVenC内部ME/RDO、MCTF和CTU线程同步各自CPU占比，再按热点改算法/任务粒度，并同时核对编码载荷或质量—码率曲线。启动中输入打开及输出header各约数百毫秒，RTMP TCP_NODELAY是可单独验证的启动时延方向；它不解释当前约18fps的稳态计算上限。本轮没有以关闭MCTF、改变QP、减少编码画面或加入更多跨帧并行来宣称无代价提速。

## 证据与复跑

原始索引：

- `out/checks-cqf-rate-20261018-a/summary.json`：相关8步回归。
- `out/checks-cqf-baseline-paced-20261018-b/summary.json` → `out/rtmp-bench-4wj8_j_g/`。
- `out/checks-cqf-baseline-throughput-20261018-b/summary.json` → `out/rtmp-bench-df3x3onh/`。
- `out/checks-cqf-threads4-paced-20261018-b/summary.json` → `out/rtmp-bench-8e3_15d0/`。
- `out/checks-cqf-threads4-throughput-20261018-b/summary.json` → `out/rtmp-bench-31etwskx/`。
- `out/cqf-implementation-20261018-a/measurement-summary.json`、`thread-payload-comparison.json` 和各配置/准备命令。

本轮失败尝试保留：`out/cqf-rate-ngq0w4vc/` 为修复前CQF停顿；`out/rtmp-bench-xv74iqgr/` 为接收端漏copyts而归零时间轴、主动停止的无效测量；`out/rtmp-bench-qoogwtvq/` 为按-t复制截断B尾，被输入60fps断言拒绝的短源。它们不计入有效结果，未覆盖/改写任何冻结n7证据。

```sh
bash scripts/build-service.sh
bash scripts/bench-service.sh rtmp \
  --config out/cqf-implementation-20261018-a/baseline.json \
  --input-fps 60 --input-gop 120 --mode paced --seconds 240 --repeats 2
# throughput对照改为 --mode throughput --repeats 1
# 4线程使用同目录 threads4.json；通用RTMP示例在 examples/。
```

结果是此正常构建、此主机和此有限素材的测量，不是多路容量、长时稳定性、跨主机RTMP交付或source-to-screen验收。
