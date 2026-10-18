# Live VVenC 8/10-bit编码精度对照（2026-10-19）

本批仅切换VVenC原生InternalBitDepth，输入/filter保持540p10-bit、库内Pel仍为int16_t、SIMD仍是现有16位路径。不是全链路字节像素实现。相同实际码率下，没有测到计算节省；8-bit的整段PSNR/VMAF略低。随后用户授权开始原生8-bit路径，后续改造必须使用独立证据目录，不覆盖本批。

## 固定条件与口径

输入沿用冻结的HEVC两遍ABR目标8000kbps、1080p60、GOP120、固定3B树林源：`out/codec-phases-20261018-a/forest-hevc-abr8m-1080p60-g120-b3-offset1.flv`，SHA256 `decaae53011dcc8766b21123144b85aec5075eb6d2b256001f2200bdaaf59968`。源码制备和解码实测视频均值约7943.793kbps，目标ABR不等于恰好8000kbps。

输出960×540p30，源I跟随，3B，future guard 4，crf32、maxrate1100000、source_rate_limit滚动已完成输入1秒、speed工具配置、8编码线程、解码预算2、reader/decoder队列1、decoder_pump、TCP_NODELAY、mux_wait20ms、BUSY_WAIT_TIME=0，AAC透传。两种精度只改变encoder_bit_depth，其余相同。位深切换可选，默认10；参数验证、输出归一化指纹和状态已接入。

Ryzen7 9700X、WSL2/Linux、单路paced loopback；每轮有限回放到1770张目标后API停止，实际1781张写出，PTS1.000–60.333s，不把API停止当正常EOF。时延从app首次相关RTMP读取到逐帧实际write/flush，包含排队，不是线缆或显示时延。四轮正式顺序10/8/8/10，两轮额外诊断；所有耗时测量顺序执行，质量分析在正式回放之后进行。CPU为/proc控制器及后代采样总核时，含解码/滤镜/编码/日志/封装，除以写出帧数；不是单帧编码墙钟时间。

## 性能与双预算

| 轮次 | 编码精度 | 实际视频kbps | 服务CPU-ms/帧 | 中段fps | 最大时延ms | 墙钟/DTS滚动1秒峰值bit | 审计 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| off10q32 | 10 | 831.110 | 105.003 | 29.9787 | 250.497 | 1035824/1028912 | passed |
| off8q32 | 8 | 829.299 | 105.379 | 30.0406 | 252.350 | 1036720/1029664 | passed |
| off8r2 | 8 | 829.551 | 105.553 | 29.9792 | 250.830 | 1031768/1024208 | passed |
| off10r2 | 10 | 829.040 | 106.025 | 29.9520 | 267.700 | 1041088/1033024 | passed |
| on8 | 8 | 828.524 | 106.659 | 29.9734 | 253.066 | 1037976/1029888 | passed |
| on10 | 10 | 829.269 | 105.581 | 29.9571 | 255.568 | 1035824/1028912 | passed |

视频范围为编码AVPacket payload加一次codec config，排除AAC/FLV/RTMP开销。实际码率按相同1781张、名义30fps媒体时长统计；未把1100kbps上限当实际码率。事先采用两结果实际码率相差≤1%作为匹配门槛，两组分别为0.2182%和0.0616%，无需改crf进行调参。

四轮off的10-bit平均CPU105.514ms/帧，8-bit平均105.466ms/帧，差约−0.045%。这个样本没有可辨认的计算收益，不能将两轮最大时延的差作为确定的位深收益。诊断on8/on10的CTU任务累计CPU均值46.295/45.963ms，仅作定位，不与off混成性能提升；它是多线程CPU累计，不是CTU墙钟时间。

六轮各1781张都没有>300ms；声明的1.1Mbit桶和严格滚动1秒两种审计均通过。源I30张、P445、B1306，仅首IDR、实际最大未来画面3、过载丢弃0。AAC均有输出、迟到丢弃0。声明的小突发与启动授信不改变严格窗口峰值的报告。此输入主要由固定maxrate绑定，不证明低源码率骤降或全内容硬实时保证。

## 质量

统一10-bit参考：原HEVC解码、原服务bilinear缩放到540p、精确源PTS选帧；8-bit输出解码后在同尺寸下提升到10-bit，保持同一份未降为8-bit的参考。PSNR峰值1023，YUV综合采用汇总MSE换算；VMAF固定vmaf_v0.6.1、原生540p、n_subsample=1，所有1781张精确配对。没有重置PTS后猜测对应帧。量化、缩放输出精度的损失都计入；不包含原相机素材到1080p的制备损失、60→30流畅度、音频或显示。

| 轮次 | PSNR-YUV dB | VMAF均值 | VMAF P05 | VMAF最小 |
| --- | ---: | ---: | ---: | ---: |
| off10q32 | 30.884386 | 71.186031 | 57.809337 | 45.410871 |
| off8q32 | 30.851698 | 71.098098 | 57.786703 | 34.867231 |
| off8r2 | 30.856993 | 71.104901 | 57.243120 | 34.867231 |
| off10r2 | 30.877584 | 71.157631 | 57.809337 | 37.276413 |

逐PTS配对8−10：第一组PSNR−0.032688dB、VMAF−0.087933；反向组PSNR−0.020592dB、VMAF−0.052730。后续I平均差分别−1.842758/−0.330980，说明局部质量及墙钟反馈码控仍有时序敏感性；不能把不同预算下的单张最差I全部归因为8-bit量化。最差连续30张（约1秒）VMAF：10-bit均58.646075，8-bit两次58.301785/58.419513。分组、逐帧依据见`out/bitdepth-20261019-a/quality-comparison.json`。

## 验证与证据

`out/checks-bitdepth-functional-20261019-a/summary.json`通过SDK、16项native、organization、quality-tools（5项）、codec-phases、live-budget。codec-phases用HEVC 240→120正常EOF，对10/8-bit分别做打点关/开，独立确认pix_fmt、PTS、源I/3B/首IDR、future guard及同位深payload等价。质量工具验证8-bit解码提升到10-bit的字节结果、严格PTS关联、拒绝覆盖与截断输出。

每轮原始目录由`out/network-output-bitdepth-{label}-20261019-a/manifest.json`定位；性能汇总为`out/bitdepth-20261019-a/summary.json`。四份质量证据位于`out/quality-bitdepth-{label}-20261019-a/COMPLETE.json`，压缩出流、指标及哈希保留，成功后仅清理该轮生成的raw YUV。输入/出流/日志/符号均不入Git。

库版本仍为FFmpeg `ed8b95e1088a4ac2ae3974a34a58bca0eb95a142`、VVenC `cfc91dada26fbef4957d2a114260a048cbee2ff5`，未修改库。依赖key `1d78e3e6bf17019f9ea14509`，源内容identity `d0a6670d7122a355c064c593962aa61366eb1590e083983eefb041901f86cfd1`。VVenC Build ID `d1021b947ae5da1d1f9f0f5e47dde51ac17de962`，对应DWARF复用`out/i-budget-20261018-b/symbols-vvenc`；本批应用DWARF另归档`out/bitdepth-20261019-a/symbols-app`。源码快照、二进制与依赖manifest见`out/bitdepth-source-20261019-a/`。

下一阶段应先建立只读packed-byte输入及8-bit滤镜协商，再按热点拆分像素与有符号残差、参考存储和专用内核。入口接受8-bit不代表库内已经字节化；任何后续收益都须新测，不覆盖本批。
