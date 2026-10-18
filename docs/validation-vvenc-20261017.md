# VVenC 接入验证（2026-10-17）

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

环境：本机 WSL Ubuntu 24.04、GCC 13、FFmpeg n9.0.2 fork `8eabad8e5820b58ec303772f777c034eb7bdb14c`、VVenC v1.14.0 `9428ea8636ae7f443ecde89999d16b2dfc421524`。VVenC 使用共享库 RelWithDebInfo、faster、2 个编码线程，服务沿用异步解码自动调度。

本地 `tests/vvenc_integration.py` 全部通过，证据目录 `out/vvenc-test-9whvfwze/`：

| 输入 | 帧数 | filter / 输出 | 验证 |
| --- | --- | --- | --- |
| H.264 8-bit，25 fps，B3 | 175 | 320×180 → 256×144、10-bit、MPEG-TS | 按 DTS 实时回放，输入 EOF 前已有 VVC 输出 |
| HEVC 10-bit，25 fps，B3 | 75 | 原尺寸、10-bit、MP4 | 完整 trailer 与解码帧数 |
| H.264 10-bit，30000/1001 fps，B3 | 65 | 256×144、10-bit、UDP MPEG-TS | 独立 socket 捕获、解码及 DTS 间距 |
| HEVC 8-bit，25 fps | 1 | 原尺寸、10-bit、MP4 | 单帧 EOF 排空；关闭逐帧明细仍有首帧日志 |

独立验证器是同次构建的 n9 ffprobe 原生 VVC decoder。输出全部为 VVC/YUV420P10LE，帧数精确一致；测试逐项核对输入 packet 序号、PTS、输入解码帧型和 key 标志，实际输出出现 PTS 重排，DTS 严格递增。每个成功输出帧均有各阶段统计，输入 first I / recurring I 与输出 key 分开；无逐帧明细的任务仍生成 first_frame。非法编码器、preset 和 VVC/FLV 组合在 HTTP 层返回 400。

原有 decoder、service、observability、process、crash 集成回归全部通过；覆盖异步解码像素一致性、真实网络 hook、日志反压、worker/HTTP 崩溃与接管。`tests/symbols_integration.py` 同时验证 FFmpeg、VVenC 的 Build ID 匹配与源码行解析；VVenC 从未剥离的构建库导出 DWARF，运行库剥离后 Build ID 保持一致。符号证据在 `out/symbols-test-_i_2qxga/`。

这些小样本用于正确性验收，不是实时容量或画质评测；没有据此承诺 1080p/4K 实时、7×24 稳定性或低时延 SLA。首帧、I 帧和编码驻留时延的边界见 [VVenC](vvenc.md)。镜像与 CI 的最终状态以该提交对应的 Actions / Release 为准。
