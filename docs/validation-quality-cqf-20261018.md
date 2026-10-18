# RTMP CQF 540p30 画质检查（2026-10-18）

本轮补充了明确的出流保存选项与独立 PSNR/VMAF 入口，并对已有完整 RTMP 实测出流完成一次全帧质量检查。使用 `faster`、4线程、原生CQF QP32/QPA、maxrate 6Mbps；输入1080p60、GOP120，输出960×540、名义30fps、源I跟随、3B。这里只增加离线质量证据，原性能测量及媒体不改写。

## 结果

| 指标 | 本轮结果 |
| --- | ---: |
| 对齐并评分的出流帧数 | 1771（全部实际解码帧） |
| PSNR Y | 32.443749 dB |
| PSNR U / V | 40.148035 / 43.307367 dB |
| PSNR average | **33.939714 dB** |
| 单帧 PSNR average 最小 / 最大 | 30.692053 / 36.491713 dB |
| VMAF 算术均值 | **92.317747** |
| VMAF 调和均值 | 92.168728 |
| VMAF P05 / P50 | 86.254470 / 92.188196 |
| VMAF 最小 / 最大 | 80.167199 / 100.000000 |

PSNR average 是 FFmpeg 按 YUV 样本 MSE 汇总后的分贝值，不是逐帧分贝的算术平均。原始10-bit YUV420、峰值1023；VMAF模型为内嵌 `vmaf_v0.6.1`，全部帧计算，原生960×540评分，未做1080p放大。VMAF日志报告版本 `f85a8536`，独立分析工具为本机 FFmpeg 9.0.2 full build，具体构建参数和二进制SHA保留在结果中。

参考是本次实际1080p输入解码后，按相同60→30策略选帧并用服务原有 bilinear/10-bit 滤镜缩放的画面。分数度量编码相对这个540p参考的失真，不包括相机4K原片制备为1080p的损失、60→30的时间分辨率损失、音频或显示链路，不应跨尺寸直接比较VMAF。

## 对齐与工具核验

- 复用 `out/rtmp-bench-8e3_15d0` 第1轮完整出流，来自[上一轮性能检查](validation-rtmp-cqf-20261018.md)的4线程 paced 配置。本轮没有重新编码这一份压缩出流。
- 重新通过SDK consumer gate，用固定n9 SDK独立解码3600帧输入和1771帧出流，按整数PTS与各自有理数time_base精确配对；全部出流帧都有符合降帧/源I保留规则的源帧。首/末PTS为1.033s / 60.100s。停止时尾部可能有B重排缺口，本次按尾部各自实际PTS评分，无错位配对、末帧重复或额外裁除。
- 参考导出直接链接 `app/media/video_filter.c:naki_video_filter_prepare`；出流原生10-bit解码导出。两路每路2,754,259,200字节，逐帧导出索引/PTS、颜色标签、字节数与SHA均已核对。
- 独立分析工具只接收等长的原始帧，两路使用相同连续帧时钟；PSNR与VMAF日志各1771条，帧序号连续。没有把接近PTS的图像交给framesync猜配对。
- 16帧自身对比：PSNR=inf，VMAF均值99.839270。工具、输入、相关导出源码及SDK manifest前后哈希一致。

这是对已完成测量的离线检查，不产生新的实时吞吐、端到端显示时延或容量结论。上一轮4线程的服务处理速率约18.28fps，尚未达到30fps；后续速度优化可以同时以本轮质量分数和相同内容的码率作比较，不能只用平均fps判定取舍。

## 设施与复现

新增 `scripts/bench-service.py rtmp --save-output-dir out/NEW-DIRECTORY`：每轮结构验证之后复制接收端压缩出流，生成SHA manifest；复制发生在性能计时外，拒绝覆盖已有目录。默认每轮的 `captured.flv` 继续保留。

新增质量入口与SDK导出helper，使用方式见[性能入口](benchmarking.md)。质量入口在新目录另存 `output.flv`；中间raw成功后删除，可用 `--keep-raw` 留存；失败现场保留。分析FFmpeg不并入服务SDK。

本轮实际命令：

~~~sh
NAKI_SKIP_FFMPEG=1 bash scripts/build-service.sh
python3 scripts/check-video-quality.py \
  --benchmark-run out/rtmp-bench-8e3_15d0 --repeat 1 \
  --metrics-ffmpeg /mnt/c/program1/ffmpeg.exe \
  --result-dir out/quality-cqf-540p30-20261018-a
~~~

证据与媒体均在本机忽略目录：

- `out/quality-cqf-540p30-20261018-a/COMPLETE.json`、`results.json`：汇总、输入/工具/源码SHA、SDK gate、评分边界。
- 同目录 `output.flv`：14,661,485字节，SHA256 `87482c3310d9716dbcb8dbf2f86896a5988e10b5497fbc2567ea4c40236eb3c0`，与原接收文件逐字节一致。
- `alignment.json`、`reference-frames.json`、`distorted-frames.json`：源/出流逐帧关联。
- `metrics/psnr.log`、`metrics/vmaf.json`：逐帧分数；`identity-check/`：自身对比。
- `out/checks-quality-tools-20261018-a/summary.json`：quality-tools、organization、entrypoints三步通过（分别5、6、13项检查），源码/二进制前后身份一致。
- `out/checks-save-output-20261018-a/summary.json`：SDK gate和真实RTMP短样本保存流程通过；`out/rtmp-bench-7zccw_px` 实际接收333帧，`out/saved-output-check-20261018-a/output-1.flv` 与接收文件SHA一致（`e7915e1cd78b8eeee29fcb3e5c0c9b86febf499d9f9fa8e38c982bcdd25cff7d`）。这只是新保存选项的流程检查，不替代完整素材的性能测量。

两套检查的源码摘要均为 `b6057d3daede9122a49dcbfa39ce40c74eab096ad8a682c56b35e36bb8dffdc7`；本轮未修改FFmpeg/VVenC子模块。源码快照与二进制补丁另存于 `out/quality-source-evidence-20261018-a/`，不包含输入/出流媒体。

新覆盖包括不同time_base的精确配对、保护离格源I、停止尾部实际PTS映射、拒绝偏移/重复/非单调/前缀丢帧、导出源不够长失败、输出拒绝覆盖，以及保存参数转发与目录校验。没有为这次设施修改重跑完整性能矩阵。

输入、出流、raw、日志、分析工具和符号均未入库。既有 `docs/next-metadata-batch-and-release.md` 未修改。
