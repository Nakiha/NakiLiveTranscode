# R28 腾讯同角色码流验收

R28 Live 和 Quality 共用同一入口。参考表来自 S1、S4 各两个完整 IDR 周期和 mini8 的 8 张图，共 248 张。角色按相对最近一个真实 IDR 的 POC 划分。每张图分别检查亮度 CU 预测方式和实际分裂、L0/L1 实际引用、显式 AMVR 精度、普通 merge 的原始索引、MMVD、SBT 方向与位置、三个平面的 TU 宽高、实际 transform skip、CCLM 以及 slice 的亮度和色度 SAO 启用位。没有启用的可选工具可以计零。量化系数、CBF 和命中比例由素材决定。

用原始实验目录中的 `structure/`、`confirmed/` 和 `mini/` 重算证据。检查器校验原始 trace 的 SHA-256，可从两个原始 VTM 探针和 FFmpeg 头部记录重建两份受版本管理的参考表。

```sh
python3 experiments/vvc-tool-screen/check-r28-geometry.py verify-originals /path/to/frozen/structure
python3 experiments/vvc-tool-screen/test-r28-geometry.py
```

用同时输出结构事件和实际使用事件的 VTM DecoderApp-structure，以及独立的 FFmpeg VVC 解码器检查**整个**成品流。输出文件应使用新路径。

```sh
python3 experiments/vvc-tool-screen/check-r28-geometry.py check video.266 fresh-result.json \
  --structure /path/to/DecoderApp-structure --ffmpeg /path/to/vvc-enabled/ffmpeg \
  --trace-ffmpeg /usr/bin/ffmpeg --frames 300
```

检查器核对每张图的 CU 和三个 TU 平面的面积、结构与实际使用事件的对应关系、头部 POC 与亮度和色度 SAO 标志，再比较两套解码器在码流原生位深下的可见 I420 像素 SHA-256。任一差集、像素不一致、解码失败或必要事件缺失都返回非零。末尾不足 60 张图的 IDR 周期同样检查。JSON 记录各项越界的具体角色、参数、次数，以及输入、探针和证据哈希。

例如，`start+2/8x4` 的普通 merge 索引 2 有原片证据，索引 3 没有。`steady+0/64x64` 只实见显式 AMVR 模式 0。`tail+1` 没有亮度 4×4 TU 的证据。`start+2` 允许启用亮度 SAO，色度 SAO 须关闭。

Service CI 执行门禁的无媒体回归。成品验收需要上述 VTM 探针和两个 FFmpeg 功能。完整发布同时需要 SPS/PH/SH、依序 RPL、严格码控及服务复核。历史 SDK 的记录与候选构建的结果分别保存在 `r28-live-rc-sdk.json`。
