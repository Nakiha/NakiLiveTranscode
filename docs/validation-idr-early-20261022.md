# HEVC IDR 提前输出与 CPU 时钟审计验证

2026-10-22，本地实现提交 `3c9ba3c907cc23eaafea32098c73ce251b44805e`；FFmpeg 独立提交 `42dd642a1974a085728dbc3ecc4e1b5e7814d5b9`，VVenC 保持 `8a1c5e6f5defc2ec2c95fa67456be7af9cc0c64c`。历史目录内实际为 n9 fork，本次不引用 n7 性能作为对照。SDK dependency key 为 `492f40160b1addfab31a5cf3`，source identity 为 `1f36215475ff6da815f15d2ba57bd076753e8814ae27e720c9504ace6c60e5e6`。

## 结果

沿用上一轮配置与同一输入，仅替换实现，保持 `codec_trace=true`、`BUSY_WAIT_TIME=0`。1080p60 HEVC Main 8-bit、GOP120/B3/AAC，经 paced loopback RTMP 输入，输出 540p30 VVenC faster/8 threads、受限 CRF32、1100kbps 上限和来源码率约束。完整参数及命令保存在本次证据目录。

两轮各有30个输入 I 帧、1781个输出画面。以下均为输入 I 帧的平均值，阶段不重叠、可相加，单位 ms：

| 阶段 | 优化前 | 优化后 |
| --- | ---: | ---: |
| async 接纳、启动及解码前等待 | 0.431 | 0.454 |
| 自身解码与重建窗口 | 15.725 | 16.063 |
| 重建完成后等待 DPB 放行 | 13.027 | 0 |
| 已选中输出至 codec 返回 | 3.297 | 0.005 |
| 返回后 async/前端交接 | 0.103 | 0.264 |
| 总解码驻留 | **32.583** | **16.786** |

I 帧平均驻留下降15.798ms（48.5%）；P95 从34.117ms降到19.064ms，最大值从35.929ms降到20.577ms。首次 I 从33.847ms降到15.196ms，其余29个 I 的新均值为16.840ms。I 的 reference wait 均为0；新调用线程 CPU 均值16.058ms，属于解码窗口内的子指标，不再相加。没有改变熵解码/重建算法，不能把等待减少说成计算提速。

全部输出画面的入口到实际 mux/flush 平均时延从172.612ms到172.299ms，P95从237.882ms到237.752ms；这点差异不足以独立证明整体提速。I 仅占30/1781，且编码未来帧等待等阶段仍存在，不能宣称全部帧都节省近16ms。这不是来源到屏幕时延，也不是容量结论。本次是新实现与前一轮同配置 n9 诊断测量对照，并非紧邻的随机化 A/B；前一轮严格 CPU 审计失败状态保持不变。

## 实现边界

解码器私有选项 `live_idr_early_output` 默认关闭；服务对原生 HEVC 显式开启。仅对有有效画面 PTS、VPS 声明单层、单活动解码层、软件 `IDR_N_LP` 生效。CRA、`IDR_W_RADL`、多层、硬件及缺失 PTS 不提前。

新序列仍先按原规则排空或丢弃旧画面，再用现有 DPB 选择函数选中 IDR，在 `finish_setup` 复制线程上下文前清除 OUTPUT 标记，避免后续 worker 重复输出。选中的当前帧只留在 codec 私有 pending 指针中，所有 NAL、像素重建、film grain、checksum 成功后才进入输出 FIFO；错误时清除 pending。帧仍经过原 receive、通用后处理和按提交顺序退休的 async 路径。flush 同时排空 codec FIFO 和 pending 状态。

保证像素、显示顺序、PTS、best-effort timestamp、时长、尺寸、像素格式、SAR及色彩信息不变。`pkt_dts` 表示触发输出的压缩包 DTS：提前返回的 IDR 会使用其当前包 DTS，不能声称与原先后续包触发时相同。服务继续按源 PTS 映射，不以该输出触发字段重定时。

## CPU 审计根因与修正

独立程序共3000次嵌套采样，读取校时状态、不改变系统时间；覆盖原 affinity、首核、末核。175次线程 CPU 超过 MONOTONIC 时长加5µs，RAW 检查0次超限。最大例子在同一核8上：MONOTONIC=19.957050ms、RAW=20.001708ms、CPU=20.001182ms，CPU 对前者多44.132µs，但对后者仍少0.526µs。该样本的只读 adjtimex `tick=9976`、`freq=5.593094ppm`；本机校时中的两种时钟速率不同，固定5µs跨时钟硬比较会误报，不需要解码、跨核迁移或计时重叠即可复现。此证据未识别负责调整校时的具体宿主进程。

decoder trace v2 在 CPU 开始之前读取 RAW 开始值、CPU 结束之后读取 RAW 结束值，追加 `decode_cpu_wall_us`。CPU 对外围 RAW 区间仍执行原5µs余量，未剪裁 CPU、未放宽阈值。阶段链继续统一 MONOTONIC；新增 `cpu_monotonic_excess` 保留跨时钟差异，off-CPU 子指标使用 RAW 与 CPU，不能混加为媒体阶段。v1 历史和 encoder v2/v3 的旧规则保持不变。

新 RTMP 轮全部3568条 decoder 事件的 RAW/CPU 检查通过，CPU 对 RAW 最大正超差为0；此轮 CPU 对 MONOTONIC 也无正超差。不能用新轮无异常抹去旧轮1/80条异常，时钟差异的直接复现来自独立3000样本。

## 验证与证据

- `scripts/build-decoder.sh linux`、`scripts/test-decoder.sh linux` 完成。选项关闭时原有 H.264/HEVC、B0/B3、2/12线程、逐帧元数据、10-bit、尺寸变化、gate、错误恢复、drain/flush/close 测试通过。历史 n7 文件只用于保存的正确性 hash/PTS 对照，不作为性能证据。
- 新增 `tools/live-async/test-idr-early.py`：最终构建44组 off/on 对照、33952个比较画面，逐帧像素和除输出触发 `pkt_dts` 外全部记录字段相等。包含真实3600帧1080p输入、2/12线程、capacity=1、三轮回放、10-bit、四 slice、CRA/RASL、实际 IDR_W_RADL/RADL、原始流缺失 PTS。另跑开启优化的首包暂停、gate、在途 flush、错误恢复、close、API 检查，均通过。
- 服务 quick profile 全部通过：18项 native 测试和10个 Python suites，其中 CPU/阶段工具12项。源码、二进制及 SDK manifest 在快速检查和测量期间均未变化。
- RTMP 结果 `measurement=complete`、`conformance=passed`、严格 codec-phase audit passed、日志无丢失。接收端直接保存 FLV，独立验证1781视频包及2788音频包并解码。受控关闭时 RTMP 对端未确认关闭，接收进程返回12；审计以停止意图、FLV计数和独立解码确认完整性，保留原始未确认状态，没有将它改成正常EOF。
- 匹配的6个FFmpeg、1个VVenC和44个应用 ELF 已分别归档 Build ID DWARF，运行库未混入调试符号。VVenC 本次没有源码修改。

所有新证据位于 `out/idr-early-20261022-005456/`，原始 RTMP 测量在 `out/rtmp-bench-kwb0ua3d/`，没有覆盖旧实验。主要入口：`comparison.json`、`I-frames.csv`、`checks/summary.json`、`quick/summary.json`、`idr-regression-final/summary.json`、`extra-regression-final/summary.json`、`clock-summary.json`、`clock-probe-long.csv`、`extra-nal-types.json`、`build-identity.json` 与 `symbols-*/manifest.json`。前一轮对照保留在 `out/codec-trace-i-analysis-20261022-a_cnn9xa/`。

验收后的44个二进制已按 SHA256 核对并放入本地 `out/bin/`；替换前完整目录保留在本次 `default-before/`，安装清单为 `default-install.json`。本次实现、子模块及报告均为本地提交，未推送远端或发布新 SDK/CI；不能据此声称远端 main 已包含本优化。

先期生成额外样本时系统 libx265 在 `pools=none:slices=4` 下崩溃，失败样本未用于验收；最终采用 `pools=2` 重新生成至独立目录并验证 NAL 类型。WSL Git 读取大子模块状态曾超时，依赖 key 改由同一工作区的 Windows Git/Python 只读校验取得；没有跳过 pin 或 dirty 校验。上述尝试的日志与最终通过证据分别保留。
