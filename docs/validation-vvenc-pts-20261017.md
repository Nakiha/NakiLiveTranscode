# VVenC 按 PTS 调度关键帧：2026-10-17

0.7.0 使用公开 fork 独立实现 PTS 时间网格调度：以首帧 PTS 为起点，每 2 秒目标后的第一帧为关键帧。完整组保持 3B，边界/EOF 可以 IP、IBP、IBBP；不为凑满 B 组延后关键帧。VVenC pin `2b90643`，FFmpeg pin `9d4c37c2`。名义帧率仍用于码控，但不是关键帧位置的判断依据。

本机 WSL/Linux 使用真实 H.264/HEVC 8/10-bit 输入，经服务异步解码、10-bit filter、单遍 VVenC 码控和 MP4/MPEG-TS 输出，由构建的 n9 原生 VVC decoder 独立解码。`tests/vvenc_pts_integration.py` 的 12 组样本核对全部输出画面数、显示顺序、每帧 PTS 间隔、严格递增的 DTS、DTS≤PTS、时间网格关键帧、短组和日志关联。不是只检查传给编码器的配置。

| 输入时间轴 | 实测关键帧位置（首帧为 0） |
|---|---|
| 先 30fps、后 15fps 的显示间隔 | 0、50、80 |
| 25fps 基础上周期性增加 10ms | 0、49、98 |
| 分别增加 200ms、600ms 时间戳缺口 | 0、45、80 |
| 中途向前跳 7 秒 | 0、20、25、75；跳过空周期，不补连续 I |

另覆盖非零 PTS 起点，以及 2/3/4/5/50/51/52 帧 EOF：独立解码分别验证 IP、IBP、IBBP、IBBBP 和关键帧前后尾组。输出关键帧日志中的目标 PTS、lateness、前一个关键帧间隔与独立解码结果一致。对输入完全缺失 PTS 的情况，既有名义帧率合成回退仍存在，不能当作源端真实媒体时间。

原有 `tests/vvenc_integration.py` 的 175 帧 H.264/8-bit→TS、75 帧 HEVC/10-bit→MP4、65 帧分数帧率 H.264/10-bit→UDP TS、1 帧 HEVC/8-bit→MP4 均通过；时间戳间断从旧版的非 CFR 拒绝改为保留时间轴。输入 I、首 I、后续 I、输出关键帧和各阶段时延统计保持独立。

本轮实时回放证据 `out/vvenc-latency-v0fwuiu2/`：相同 175 帧/25fps 输入、faster、2 线程、1.5 Mbps、输出 256×144，reference 请求到首 mux flush 3290.6ms，PTS live 448.3ms，关闭 MCTF 的 live 446.1ms；live 首帧编码驻留 347.8ms。真实 UDP 输入请求到首 mux flush 410.0ms，探流完成点 326.9ms。探流与包缓存存在重叠，不能把两者简单相减作优化收益。这里只是一轮本机测量，不承诺所有规格上的实时性能，也不是源采集到播放器显示时延。

服务 CI 运行新 PTS 用例并保留 `vvenc-pts-*` 证据目录。两个库 pin 参与 SDK 缓存键，FFmpeg/VVenC 与应用的 Build ID 符号仍分别归档；不能用旧 SDK 搭配新 live 配置。服务现在会拒绝编码器未消费的选项，避免旧 wrapper 静默忽略 PTS 模式。

后续主要传输组合为 Enhanced FLV over RTMP、MPEG-TS over SRT。当前服务仍为纯视频实验，VVC+FLV 请求限制尚未解除，libsrt 尚未编入裁剪依赖；本次不把动态加流或这两项协议接入宣称为已完成。输入快速带缓存探流的工作也不包含在本次库提交中。
