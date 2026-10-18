# HEVC 8Mbps输入与内部逐帧打点（2026-10-18）

本批在第二批受限CRF实现上补充诊断，不改变其搜索profile、QP模型、编码工具、源I策略、3B、未来画面守卫或队列容量。输入按新要求重新制备为HEVC ABR 8000kbps、1920×1080p60、3B、GOP120。旧H.264输入的性能/画质证据保持原样，不能将输入变化当作实现提速。

完整60秒源的off/on/off三轮各写出1781张，均无300ms超时或双预算超支；最大时延分别253.453/257.275/251.634ms。解码与VVenC内部打点已接入自动审计。诊断轮显示主要驻留是HEVC显示重排、VVenC组内未来输入及GOP派发，实际CTU计算跨度明显短于外层encode驻留；I帧无需未来输入。短scout有一处DTS窗口码率超标，完整结果不覆盖或抹掉这一失败。

| 完整轮 | 打点 | 写出张数 | 中段fps | 平均/P95/最大时延ms | >300ms |
| --- | --- | ---: | ---: | ---: | ---: |
| off1 | 关 | 1781 | 29.9553 | 169.200 / 234.302 / 253.453 | 0 |
| on | 开 | 1781 | 29.9593 | 169.955 / 235.772 / 257.275 | 0 |
| off2 | 关 | 1781 | 29.9847 | 168.873 / 233.391 / 251.634 | 0 |

| 完整轮 | 视频均值kbps | 墙钟滚动1s峰值bit | DTS滚动1s峰值bit | 双桶超支/债务bit |
| --- | ---: | ---: | ---: | ---: |
| off1 | 818.447 | 971152 | 982152 | 0 / 0 |
| on | 817.426 | 971104 | 982152 | 0 / 0 |
| off2 | 818.021 | 971152 | 982152 | 0 / 0 |

全部三轮保留30张源I、最多连续3B、仅首个IDR，未来帧最大3≤4；无过载丢帧，AAC各透传2783包、无音频丢弃。60→30主动丢帧1783–1784张，停止尾部导致计数差异。该8Mbps源没有使动态源预算成为主要约束，不能据此声称低源码率/突降场景已全部达标。

off1的全部1781张PSNR为30.835189dB、VMAF为70.928654；on分别30.831984dB、70.894362。输入是HEVC Main 8-bit，独立检查确认出流为VVC Main 10、yuv420p10le；`live_3b_2s_speed`是项目工具配置名。

相对两轮off均值，on的平均总时延增加0.919ms，采样服务CPU由平均186.90CPU秒变为189.44CPU秒，约+1.36%；服务峰值RSS由约188.26MiB变为188.88MiB。该有限三轮观察未见吞吐退化，但不是零开销或统计显著性证明。实际受限码控会使用墙钟反馈，完整轮payload可能不同；严格payload开关等价另由高cap功能用例验证。

诊断scout（337张）最大242.140ms、墙钟1s峰值971104bit、双桶无超支，但一个DTS滚动窗口达到1111608bit（比1100000高约1.06%），其`conformance_status=failed`原样保留。benchmark gate的passed表示测量有效，不表示所有历史窗口均满足合同。受限CRF仍是实验性反馈控制，不能从有限完整轮推导硬保证。

## 新输入

沿用相机原片`D:/video/action5/DJI_20250501110804_0049_D.mp4`的30–90秒复杂树林片段。离线中心裁3840×2880→3840×2160，fps60、Lanczos缩放至1920×1080、yuv420p。使用x265 veryfast、两遍ABR目标8000000bps、固定3B、禁止场景切换、闭合GOP120；音频AAC128k。ABR不是CBR，没有填充或每秒8Mbps硬限制。

独立解码/packet检查：3600张、60/1fps、I位置0/120/…/3480、最大连续3B；视频AVPacket payload均值7,943,792.53bps，约比目标低0.7%。输入Main/8-bit、limited BT.709。系统FFmpeg仅用于离线制备；Enhanced FLV由固定SDK仅copy remux，并显式增加1秒时间起点，为输出3B留正DTS预滚。服务内不重定时。

制备与检查入口为`out/codec-phases-20261018-a/input-manifest.json`、`input-probe.json`、`prepare-commands.json`。原MP4、网络FLV、720帧scout和两遍stats均只在out中；网络文件为`forest-hevc-abr8m-1080p60-g120-b3-offset1.flv`。

## 诊断开关与所有权

`codec_trace=true`默认关闭，仅直接输出、live VVenC CQF/受限CRF及未来画面守卫可用；运行时明确要求原生软件HEVC输入。它支持文件功能检查及plain RTMP实测；network_trace仍只在plain RTMP启用。诊断强制保留所有解码画面事件和实际写出画面事件，logger保持有界非阻塞，任何丢失使审计失败。未消费的API停止尾部派发槽最多可以多一条decoder事件，正常EOF必须完整计数。

解码原始事件存于真实HEVC DPB画面的引用计数side data，经过显示重排和async返回。应用在取帧后做原子快照，写入原有opaque_ref Stamp，再通过filter/有界编码PTS ledger关联至实际write/flush。既有opaque_ref未被替换；未按帧输出顺序猜关联。

VVenC在每次PicShared复用时重置诊断数据，CTS/POC绑定输入与AU；CTU线程只更新该画面原子聚合计数和首末实际工作时间。ready-check探测不读时钟、不写诊断计数，不新增任务队列，不在库内写日志。最终AU携带定长快照，FFmpeg wrapper转为整数metadata，由同一个输出owner取回、写日志。

## 解码阶段口径

| 时间点/数值 | 含义 |
| --- | --- |
| dispatch / worker_start | 提交该packet任务、worker开始；间隔含上下文同步与调度 |
| decode_begin / picture_start | packet NAL解析开始、该画面DPB/属性准备完成 |
| reconstructed | packet解码与帧末处理完成、报告像素进度之前 |
| output_selected | DPB选择该画面输出；可能早于重建完成，不能当成像素就绪 |
| worker_output | 包含该显示画面的receive回调返回，可能由更晚packet的任务返回 |
| async_queued / frontend | 协调线程入输出队列、应用取走 |
| decode_wall / decode_cpu | NAL解码窗口墙钟/调用线程CPU；不含其他线程CPU |
| reference_wait | 引用进度检查未满足时进入等待路径的驻留，避免对每个已就绪块读时钟 |

重排释放等待使用`max(reconstructed, output_selected)`。decode_wall减decode_cpu还包含OS调度/停顿，不能全部叫引用等待；reference_wait是其中单独观测的子集。队列、worker交付与async handoff分开记录。每张画面用源PTS、输入sequence及应用frame_id交叉关联；POC可能在IDR重置，不能仅靠POC做跨GOP身份。

## VVenC阶段口径

输入接收→最后一张所需组画面到达→组决策→逐画面QPA预处理→MCTF→GOP dispatch→首个实际CTU任务→最后实际CTU任务→finalize回调开始/结束→重建发布→AU构建→wrapper返回分别记录。

CTU的CPU按搜索/量化、deblock/reshape、SAO、ALF/CCALF、finish分类求和；CPU总量可以大于阶段墙钟，因为多个线程同时工作。成功任务的worker wall之和也不是关键路径。compress返回与CTU工作重叠，因此不强制compress返回早于最后任务结束。

预处理、dispatch主线程、finalize另记调用线程CPU。MCTF目前只给阶段墙钟，包含其并行计算与等待，没有收集MCTF各worker总CPU；不把它冒称为纯等待或纯CPU。Linux CLOCK_MONOTONIC和CLOCK_THREAD_CPUTIME_ID是本轮验证时钟。

## 自动审计与功能检查

`scripts/audit-codec-phases.py`检查sealed段、日志无丢失、每张出流与decoder事件的精确关联、事件因果顺序、CPU/墙钟边界，并按实际输出I/P/B汇总。RTMP benchmark在codec_trace开启时自动执行，另存`codec-phase-audit.json`；它与1100kbps/300ms合同审计分开，不以诊断完整代替目标达标。

`tests/integration/codec_phases_integration.py`用HEVC/3B正常EOF检查240张解码→120张出流、精确PTS、源I/3B/仅首IDR、完整内部事件，以及高cap不绑定预算时打点开关前后逐包payload hash/PTS/DTS完全相同；不支持的H.264诊断请求必须显式失败。既有H.264/HEVC默认async像素/元数据和生命周期回归单独执行。

首个A功能窗口因离线frame事件未包含外层绝对时间戳而失败，已补齐；B的内部关联/EOF/开关payload等价检查通过，但新suite未登记到entrypoints固定inventory，窗口仍记失败并保留。修正后使用新C窗口，历史失败目录没有覆盖或据新审计重新标记。

## 实测方法

固定第二批最终输出设置：960×540p30、constrained_crf/crf32、maxrate1100000及source_rate_limit、源I跟随、3B、future guard4、faster/live_3b_2s_speed、8编码线程、reader1/async queue1/decoder预算2及单槽解码泵、mux_wait50、AAC透传、TCP_NODELAY、BUSY_WAIT_TIME=0。两只审计桶声明1,100,000bit容量，启动授信口径沿第二批。

先通过功能与默认路径回归，再做12秒诊断scout，然后依次关闭诊断、开启诊断、关闭诊断，各跑同一完整60秒源。所有配置/媒体/SDK/二进制在各窗口前后核验。源有限回放达到1770张目标后API停止，所有已写画面独立核对；源端有限EOF不伪装成服务正常EOF。

关闭诊断轮作为性能基线，开启轮用于解释等待与CPU并量化观测开销。正式性能不与质量计算、媒体制备或回归测试并跑。质量对齐新HEVC解码源的精确PTS，经服务同样bilinear缩放至540p10-bit，全部实际出流帧做PSNR/VMAF；不与旧H.264源分数直接归因比较。

## 完整诊断轮：等待与计算

以下来自`out/rtmp-bench-hwoe7umo/1/codec-phase-audit.json`及同轮`budget-audit.json`，1781张实际写出画面均完成关联；独立decoder事件3567条，包括60→30丢弃画面及API停止尾部。表中分位数是原始逐帧值，非直方图上界。各行最大值可能属于不同帧，不能相加。

### 解码与编码器之外

| 阶段 | 平均ms | P95 ms | 最大ms |
| --- | ---: | ---: | ---: |
| decoder async admission | 0.454 | 2.533 | 5.746 |
| worker启动 | 0.042 | 0.061 | 0.186 |
| NAL decode窗口（含引用等待） | 7.225 | 10.579 | 19.864 |
| 上项调用线程CPU | 6.346 | 10.296 | 19.862 |
| 上项引用进度等待路径 | 0.901 | 4.892 | 7.923 |
| 重建/输出选择就绪前的显示重排驻留 | 51.317 | 97.657 | 115.095 |
| 输出选择就绪→worker交付 | 4.327 | 9.057 | 19.837 |
| async传递 | 0.035 | 0.050 | 0.220 |
| frontend队列 | 3.254 | 42.760 | 69.488 |
| 应用取帧handoff | 0.019 | 0.033 | 0.108 |

完整应用decode驻留均值66.672ms，并不是66ms纯解码计算。引用等待路径也包含进入/退出同步的CPU，不能严格等同于off-CPU；调用线程wall−CPU平均0.879ms还包含其他调度因素。输入是3B的HEVC，重建后的显示重排确实是最大项，不能靠多开解码线程或跳过必要进度同步直接消除。

逐帧总时延减去VVenC外层encode驻留后，编码器之外平均78.353ms、P95 136.107ms、最大161.051ms。其余外层阶段：reader排队平均4.012ms、缩放0.645ms、mux驻留6.107ms（P95 23.837ms、最大35.158ms）；网络组包0.767ms，demux缓存0.022ms。此处仍是应用首个相关TCP读取→实际write/flush，不含网络到达应用之前或接收端显示。

### VVenC

| 阶段 | 平均ms | P95 ms | 最大ms |
| --- | ---: | ---: | ---: |
| 组内最后一张输入到达前 | 31.641 | 57.509 | 71.973 |
| 最后输入到达→组决策 | 0.872 | 1.440 | 22.108 |
| 逐画面预处理 | 0.421 | 0.560 | 0.988 |
| MCTF阶段墙钟 | 1.698 | 6.591 | 8.986 |
| MCTF完成→GOP dispatch | 37.975 | 67.442 | 84.557 |
| dispatch→首个实际CTU任务 | 1.119 | 1.494 | 1.999 |
| 首个→最后实际CTU任务 | 14.776 | 27.604 | 35.119 |
| 最后任务→finalize回调 | 0.005 | 0.016 | 0.067 |
| finalize | 0.919 | 2.523 | 3.964 |
| 重建发布→AU完成 | 0.072 | 0.122 | 0.273 |

其他wrapper和阶段间传递也保留于完整JSON，以上仅列主要项。GOP dispatch间隔是阶段驻留，包含编码顺序、已有任务/参考可用性、上层再次推进及初始化；当前没有按各ready拒绝原因计时，因此不能把37.975ms全部归为某个锁、PARC或单一轮询。

CTU任务CPU按worker求和：搜索/量化平均46.224ms、deblock/reshape 1.328ms、SAO 0.064ms、ALF相关任务路径0.062ms；任务墙钟求和47.937ms。它们并行重叠，不能从14.776ms关键路径跨度里相减来求等待。当前speed配置禁用ALF搜索，仍会经过对应状态检查，微小任务CPU不代表启用了完整ALF。MCTF worker CPU暂未聚合，6ms左右的P/I帧MCTF墙钟不在这份CPU总量内。

### I/P/B差异

外层encode包含内部排队，CTU跨度则只从首个实际任务到最后任务；两列含义不同。

| 画面分类 | 张数 | encode平均/最大ms | 总时延平均/最大ms |
| --- | ---: | ---: | ---: |
| 首I | 1 | 29.228 / 29.228 | 106.039 / 106.039 |
| 后续I | 29 | 82.783 / 92.375 | 120.747 / 130.667 |
| 输出P | 445 | 38.673 / 60.945 | 109.648 / 139.931 |
| 输出B | 1306 | 109.881 / 144.772 | 191.646 / 257.275 |

| 实际输出类型 | 组内未来输入等待均值ms | GOP dispatch驻留均值ms | CTU跨度平均/最大ms |
| --- | ---: | ---: | ---: |
| I（含首I） | 0.000 | 49.092 | 14.259 / 21.041 |
| P | 0.000 | 0.393 | 24.998 / 35.119 |
| B | 43.148 | 50.526 | 11.304 / 22.233 |

全部30张I均验证`group_last_input_us == input_us`，外部future计数也全部0。当前后续I没有攒4张未来输入后才做预处理；较长驻留主要落在MCTF及GOP派发之前。B帧平均等待组内未来输入43.148ms、再到GOP dispatch 50.526ms，是这批最值得继续细分的编码器内部驻留。不能用外层B帧109.881ms推断B帧计算比P帧贵。

最慢帧为frame_id661、源PTS12.000s、输出B：总257.275ms，其中decode110.365ms、encode120.557ms、mux23.951ms。该帧纯NAL窗口7.846ms、显示重排94.645ms、组内未来输入44.198ms、GOP派发前48.784ms、CTU跨度21.821ms。它没有引用进度阻塞，不能将这帧的110ms decode驻留归因于引用等待或HEVC算力。

## PSNR/VMAF与画质低谷

`out/quality-codec-off1-20261018-c`与`out/quality-codec-on-20261018-c`均完成所有1781张精确PTS对齐，首PTS1.000s、末60.333s。使用固定SDK导出的服务同款bilinear 540p10-bit参考，PSNR峰值1023，VMAF `vmaf_v0.6.1`、原生540p、n_subsample=1；Windows metrics FFmpeg版本/哈希保留于每轮结果。该分数只衡量解码后的HEVC输入到VVC的空间编码失真，不包含原相机到8Mbps HEVC的制备损失、60→30的时间采样损失或显示体验。

| 轮次 | PSNR Y dB | PSNR YUV平均dB | VMAF均值 | VMAF P05 | 最低单帧VMAF |
| --- | ---: | ---: | ---: | ---: | ---: |
| off1 | 29.260793 | 30.835189 | 70.928654 | 56.831697 | 25.642294 |
| on | 29.257896 | 30.831984 | 70.894362 | 56.690561 | 25.642294 |

on相对off1为PSNR −0.003205dB、VMAF −0.034292；该受限反馈运行存在轻微码量/QP差异，不以此宣称严格码流等价或观测改变画质的因果效应。identity检查16张PSNR均为inf、VMAF均值99.839268；成功完成后两份2.77GB raw均删除，保留压缩输出、逐帧指标与哈希。

按输出类型对齐off1的逐帧VMAF与真实slice QP（无跨帧猜测）：

| 输出类型 | 张数 | VMAF均值 | 最低VMAF | 实际slice QP均值 |
| --- | ---: | ---: | ---: | ---: |
| I | 30 | 56.495307 | 25.642294 | 37.533 |
| P | 445 | 77.319254 | 50.580615 | 34.854 |
| B | 1306 | 69.082700 | 40.249112 | 42.308 |

最差四张为源PTS3/5/9/7秒的后续I，VMAF25.642/32.616/35.722/36.123，实际QP45/44/42/42；3秒I的控制器请求QP46。类型之间画面内容不同，这张表本身不能证明因果，但已给出优先调查对象：I帧预测与跨GOP预算分配，尤其前几个GOP。可复算关联保留在`out/codec-phases-20261018-a/quality-classes-c.py`及`.json`。

## 下一步范围

优先在不改源I/3B/参考同步的前提下继续分解GOP派发驻留：区分前组未完成、参考未就绪、空闲picture encoder、RC更新和上层未再次推进。不要绕过ready检查。decoder优先看frontend队列长尾及输出owner等待mux造成的背压；HEVC自身3B显示重排有结构性成本，需要在保持像素/PTS正确的前提下验证任何提前交付方案，不能承诺把51ms全部消掉。

画质改动应复用本次HEVC源做独立A/B，首先查I帧码量预测/预算分配与画质低谷；再研究ALF或RDOQ的局部恢复、P帧搜索热点。这些不是本批修改。当前关闭诊断的有限轮最大值离300ms有约47–48ms余量，但这是单帧时延余量，不能直接当成每帧可增加的计算预算。固定输入/profile后同时检验持续30fps、最大时延、码率及PSNR/VMAF，才可选入下一基线。提高I帧预算也必须同时覆盖scout已出现的DTS窗口超标风险。

## 构建、检查与证据

父仓库依赖提交`c9b8c6b273adf169505520aa2ab1868c608a054b`；FFmpeg pin `5334de78c0dbcafeabed1bc3bf867840c1a441ec`、VVenC pin `d90ce7edf8e20fedfc3f04e9fd948b9288b4fdb1`，均已在各自fork提交并推送，再更新父gitlink。正常构建使用`scripts/build-service.sh`。

- dependency key：`0ac8038902c968d28c3c04f5`。
- SDK identity：`c2f9e7d7bc4d597f9d75fa7fff001f3b5711c93d75221d789bb84003a401d056`。
- 应用/脚本/测试source snapshot：`2e53fe1a5369511b5cfe9974e68a450f8171e214f2d81464feb1ba8a2a584113`。
- VVenC Build ID：`3a81f1c081d1791c7ce6b615e983f10d0297708a`；匹配DWARF在`out/codec-phases-20261018-c/symbols-vvenc`及`symbols-ffmpeg`独立封存，provenance同目录。
- 新网络输入SHA256：`decaae53011dcc8766b21123144b85aec5075eb6d2b256001f2200bdaaf59968`；benchmark每轮另记录copy remux后的实际源哈希。

功能窗口`out/checks-codec-phases-functional-20261018-c`的8项全部通过：SDK、native（16/16）、organization、codec-phases、live-budget、entrypoints、cqf-rate、read-origin。默认路径回归`out/checks-codec-phases-regression-20261018-c`的8项全部通过：SDK、multi-output、lifecycle-media、decoder、vvenc、vvenc-pts、live-mux、observability。三个完整RTMP窗口均检查source/binary/SDK前后未变。没有同时运行性能测试和离线质量计算。

| 轮次 | benchmark原始目录 | 输出和manifest |
| --- | --- | --- |
| scout | `out/rtmp-bench-p93hider` | `out/network-output-codec-scout-on-20261018-c` |
| off1 | `out/rtmp-bench-t4gtch91` | `out/network-output-codec-off1-20261018-c` |
| on | `out/rtmp-bench-hwoe7umo` | `out/network-output-codec-on-20261018-c` |
| off2 | `out/rtmp-bench-q7n8ord7` | `out/network-output-codec-off2-20261018-c` |

配置与顺序执行脚本保留在`out/codec-phases-20261018-a/full-trace0.json`、`full-trace1.json`、`run-c.py`，汇总`summary-c.json`；可提交的使用示例为`examples/rtmp-vvc-codec-trace-540p30.json`。输入、出流、原始帧、日志、SDK/符号和其他生成物全部在忽略目录，不入Git；本批源代码/文档快照在`out/codec-phase-source-20261018-final`，逐文件与ZIP校验。未改用户自有`docs/next-metadata-batch-and-release.md`。
