# Live VVenC字节输入第一阶段（2026-10-19）

已完成只读packed-byte输入、YUV420P滤镜协商和码控活动度尺度适配。它取消外部8→10→8往返和wrapper整帧16-bit暂存，但VVenC接收时仍零扩展到自有16-bit Pel；参考图、残差和SIMD仍为原有16位实现。不是全链路字节像素编码器。精度对照无计算收益后，用户授权开始该方向；上一批报告及源码快照保持冻结。

当前输入入口功能通过，但未稳定达到最大300ms：首次off与trace分别出现503.628/361.392ms，后续off复测252.504ms通过。没有删除失败轮、丢弃超时帧或改变时延边界。保留显式可选入口，默认输入/编码精度均为10，不将单次通过当硬实时保证。

## 实现与边界

请求增加`encoder_input_bit_depth`（8/10，默认10），8要求`encoder_bit_depth=8`、live CQF和future guard，当前只接受直接输出。默认10保留精度对照入口；只有8值加入归一化指纹，不改变旧默认请求身份。状态输出两种精度。实例见`examples/rtmp-vvc-byte-input-540p30.json`。

VVenC fork的`VVENC_LIVE_INPUT8_VERSION=1`扩展vvencYUVBuffer，使用三个非拥有、只读byte指针，尺寸/字节stride明确；拒绝不完整平面、负/短stride、错尺寸、混用16-bit指针，以及非live串行CQF或任一活动分量input/internal/MSB非8的组合。库在返回前复制到自有工作像素，右/下padding由原owner完成；不修改调用者内存、不增加临时整帧、不新增队列或未来画面依赖。ABI需要所有客户端使用匹配SDK重编；不是上游稳定ABI承诺。

FFmpeg桥接显式支持YUV420P，只在匹配fork和guarded live8模式启用。外部QP活动度读取字节样本后归一到原10-bit尺度（<<2），保留4096样本上限及历史运动项，不把byte指针误读为uint16。相同8-bit像素的byte与16-bit容器活动度结果逐次相等；C API编码payload/CTS也严格等价。MCTF/QPA、源I/3B、JSON及actual-write统计保持。

## 同版SDK下的完整回放

输入、机器、60→30、源I跟随、3B、future guard4、1100kbps/source双预算、crf32、speed工具、8线程、队列/音频和计量边界均沿用[精度对照](validation-bitdepth-live-20261019.md)。唯一主对照是byte8（8-bit filter/输入）与wide8（10-bit filter/输入，库内转换8-bit）；两者编码精度均8，使用本批同一新SDK。均有限回放、1770目标后API停止、实际1781张，正常EOF另由功能检查验证。

| 轮次 | 输入精度 | 诊断 | 视频kbps | 服务CPU-ms/帧 | 中段fps | 峰值RSS MiB | 最大时延ms | >300ms张数 | 目标审计 |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| byte8 | 8 | off | 828.068 | 106.199 | 29.9480 | 187.797 | 503.628 | 5 | failed |
| wide8 | 10 | off | 829.930 | 105.424 | 29.9603 | 188.473 | 274.088 | 0 | passed |
| trace8 | 8 | on | 827.653 | 105.688 | 29.9590 | 187.465 | 361.392 | 6 | failed |
| repeat8 | 8 | off | 829.979 | 104.537 | 30.0280 | 187.035 | 252.504 | 0 | passed |

首次主对照实际码率误差0.2245%，小于事先1%门槛。两轮byte off的CPU105.368ms/帧均值与wide单轮105.424接近，不能认定有计算收益；首轮byte甚至略高。RSS仅下降约0.68–1.44MiB，内部像素/参考存储未减半。所有轮次独立验证I30/P445/B1306、仅首IDR、actual future max3、过载丢弃0、AAC有输出且丢弃0。

| 轮次 | 墙钟滚动1秒峰值bit | DTS滚动1秒峰值bit |
| --- | ---: | ---: |
| byte8 | 1038560 | 1067120 |
| wide8 | 1059528 | 1051600 |
| trace8 | 1061840 | 1053560 |
| repeat8 | 1061840 | 1053560 |

声明1.1Mbit桶/启动授信及严格滚动1秒视频窗口全部通过，失败项是时延。范围为视频AVPacket payload加一次codec config，排除AAC/FLV/RTMP开销。SDK gate/measurement passed不等于target conformance passed。

## PSNR/VMAF

与上一批相同的10-bit参考、精确PTS、1023峰值与vmaf_v0.6.1；两种8-bit码流解码结果均提升到同一计分精度。主对照1781张PTS完全一致，所有帧计分。参考不随byte输入重新量化；因此服务8-bit缩放的舍入差异和活动度精度变化均计入。这不是纯拷贝微基准，编码输入像素/外部活动度可能与10-bit缩放后再取8不同。

| 轮次 | PSNR-YUV dB | VMAF均值 | VMAF P05 | VMAF最小 |
| --- | ---: | ---: | ---: | ---: |
| byte8 | 30.834084 | 70.886863 | 57.321595 | 35.675713 |
| wide8 | 30.857089 | 71.092003 | 57.673140 | 44.342704 |

主对照byte−wide约PSNR−0.023005dB、VMAF−0.205140。该byte轮时延审计失败，不能将画质表包装成满足300ms的候选。repeat8没有重新计分，不将首轮分数移植为复测成绩。画质结果随墙钟反馈预算变化，不能将所有差值都归因于字节输入或断言所有素材8-bit劣化。

## 超时定位与未解决项

首轮最差frame117、PTS2.933s：decode驻留103.388ms、encode驻留366.550ms、mux31.890ms，总503.628ms。后续短组P（frame119、PTS2.967）decode驻留298.702ms；源I（frame121、PTS3.000）对应demux读取调用265.393ms，输入FIFO仅0.058ms、其解码22.319ms。输入包sequence119→120的首次读取相隔270.732ms，名义DTS仅推进17ms；不能把前面B的366ms encode驻留解释为366ms计算，也不能把没有sender/socket关联证据的间隙断言为线缆网络问题。可能包含发送节奏、socket等待或线程调度，尚未逐项归因。

诊断轮的另一个局部低谷位于约13.8–14.3s。最差frame779：decode188.976ms、encode153.524ms、mux17.312ms，总361.392ms；GOP enter→初始化等待79.203ms与前串行图完成重叠，实际CTU任务墙钟21.888ms、工作线程CPU累计57.648ms。初始化仅约0.016ms。frame771 CTU墙钟48.706ms、工作线程CPU62.283ms，说明局部关键路径也有波动；不能只归因于前一个3s读取间隙，或靠跳过参考就绪来消除。

最后off复测0超时并未消除这两个失败证据。当前只能确认入口与数据正确性，尚不能确认尾延迟稳定达标，也没有8-bit专用计算收益。后续应先分离unsigned sample与signed residual/interpolation类型，分阶段推进orig/reference字节存储及SAD/SATD/预测热点；同时保留输入到参考就绪的时序证据。不要全局替换Pel为uint8_t，不扩大前视、不取消3B或通过丢帧掩盖时延。

## 检查、版本与归档

最终功能gate `out/checks-native8-functional-20261019-b2/summary.json`通过SDK、17项native、organization、quality-tools、codec-phases、live-budget、entrypoints及cqf-rate。新C测试对只读mmap输入、padding/stride、非法位深/尺寸/指针、forced I/短尾EOF进行验证，byte/legacy8 payload与CTS逐字节相同。codec-phases覆盖10入10编、10入8编、8入8编，各自trace关/开并比较同配置payload等价。首次功能gate因新增unit样例缺少已有必需timestamps策略失败，修正样例后完整重跑通过；未改媒体逻辑绕过。

VVenC `a347b3b9e009ba70b77f2209dce59dd41ba0b791`、FFmpeg `a4145412670af5a52d2e5de4a8884f9159faf2ac`均已独立提交并推送，父仓gitlink提交`2d18897184bada6a888d1c33febafa5172f8d881`。SDK内容identity `50cada7288ef55b25310fccdb1a14e27bb7e910441c22d7c63c16159082fea9a`，依赖key `33108985819e035074912c55`。VVenC Build ID `edaa54d566d3e1a5b327952ac523444dbb2e78e7`，匹配DWARF在`out/native8-20261019-b/symbols-vvenc-r2`，FFmpeg在`symbols-ffmpeg-r2`；旁边首次构建符号保留但不用于最终SDK。依赖manifest与source/key绑定单独保存。

汇总`out/native8-20261019-b/summary.json`；每轮媒体manifest位于`out/network-output-native8-{byte8,wide8,trace8,repeat8}-20261019-b/`，完整性能门禁`out/checks-native8-{label}-20261019-b/`，两份质量`out/quality-native8-{byte8,wide8}-20261019-b/`。失败段另存first-worst-frames.json、diagnostic-worst.json。媒体/日志/符号仅在ignored out内，成功质量检查只清理本轮raw中间文件。最终源与gate/二进制归档见`out/native8-source-20261019-b/`。
