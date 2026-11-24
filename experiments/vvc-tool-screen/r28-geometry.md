# R28 亮度分块复核

原 R28 SDK 的提交为 `13e69a43`。冻结包暂停晋级。旧检查把不同 GOP 位置的 B 图合并，能够检出全局缺少的尺寸，却漏掉了只在部分位置使用的分块和预测方式。

本目录的几何检查使用 VTM DecoderApp-structure 的实际叶块与分裂事件。参考表含 S1、S4 各两个 60 图 IDR 周期和 mini8 的 8 张图，记录原始码流及探针压缩日志的 SHA-256。每个周期从真实 IDR 的 POC 计数，覆盖 IDR、起始四图、稳定四种角色和末尾三图。对比项目为亮度 CU 的预测方式与宽高，以及实际分裂的父块宽高、方向、边界、强制标志和选择数。每张图检查叶面积是否覆盖编码尺寸。频率和系数值可以随输入变化。

```sh
python3 experiments/vvc-tool-screen/check-r28-geometry.py verify-originals /path/to/frozen/structure
python3 experiments/vvc-tool-screen/test-r28-geometry.py
python3 experiments/vvc-tool-screen/check-r28-geometry.py check video.266 fresh-result.json \
  --structure /path/to/DecoderApp-structure --frames 300
```

一个可重复的反例是起始第一张 B 图。原片允许 4×8 亮度帧间块，帧内模式在该尺寸没有实见。闭合周期相对 POC 58 仅实见 16×16、32×32、64×64 帧间块。验证器逐项检查，并输出每种越界组合的准确次数。

几何修正后的候选还需独立检查预测工具。按块角色重放可检出实际 AMVR 精度、普通 merge 索引和部分变换划分的既有越界。完整晋级仍要求同角色的工具和参考审核、VTM 与独立 FFmpeg 像素一致，以及相应档位的码率与服务验收。
