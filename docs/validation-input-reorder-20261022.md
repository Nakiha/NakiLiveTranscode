# 3B/7B、open GOP 入流正确性补测

2026-10-22，测试现有实现 `a63dff4`（媒体实现 `25e6c28`），FFmpeg `94bd940f`、VVenC `b894d109`，SDK key `f57d98cb2c23e7ccc07c81e0`。本次只添加测试与文档，没有修改解码算法、提前输出策略或服务二进制。

此前已测 B0、固定3B、带真实CRA/RASL的open GOP，但没有系统覆盖7B及B-pyramid交叉组合。本次新增以下14种构造：

| 编码 | 位深 | 连续B | GOP | B-pyramid | 样本数 |
| --- | --- | --- | --- | --- | ---: |
| HEVC | 8 | 3 / 7 | closed / open | 关 / 开 | 8 |
| HEVC | 10 | 7 | closed / open | 开 | 2 |
| H.264 | 8 | 3 / 7 | closed / open | 开 | 4 |

每段320×180、60fps、192帧，keyint=48、scenecut关闭、b-adapt=0，保留正PTS偏移；分别跨越4个GOP。不是仅传入编码选项：独立ffprobe和NAL扫描确认最大连续B确为3或7，HEVC关闭pyramid时参考B为0、开启时确有参考B。每个HEVC closed样本有4个IDR_N_LP、无CRA/RASL；每个open样本有1个IDR_N_LP、3个CRA，3B含9个RASL，7B含21个RASL。H.264 closed有4个IDR，open仅首个IDR。位深/像素格式也单独核实。

## 解码对照

使用当前匹配SDK重新编译probe，2/12线程、capacity=1，各跑1轮和3轮drain/flush回放。共56组、21,504个画面，全部与关闭优化的原解码路径相同：像素SHA256、显示顺序、PTS、best-effort timestamp、时长、尺寸、格式、SAR与色彩一致。每个样本还与系统FFmpeg解码生成的逐帧SHA256相同，避免只比较同一实现的两个入口。

HEVC的输出触发包 `pkt_dts` 按既有定义允许改变，它不是显示PTS；H.264则包含该字段也逐帧完全相同。7B open+pyramid的HEVC通过首包暂停、gate、在途flush、错误恢复、close、API检查；H.264通过gate、flush、错误恢复、close、API检查。

首次矩阵尝试错误地对H.264 7B套用了“只送首个IDR包就必须出画面”的pause断言，30秒超时。独立单线程原解码器首包探针确认 `avcodec_receive_frame=-11/EAGAIN`、`has_b_frames=2`：H.264仍按SPS重排，这项HEVC提前输出断言不适用于它。修正测试适用范围后重跑整个decoder套件通过，其他恢复检查与全部像素/PTS比较保留；未改变媒体实现使其迎合测试。首次失败目录及日志不覆盖。

新增可重复矩阵在 `tests/support/input_reorder_matrix.py`，接入原 `full --suite decoder`，清单明确登记间接依赖。既有H.264/HEVC B0/3、容量、重放、三owner预算和lifetime检查也在最终整套运行中通过。

## 当前服务端到端

同14份输入分别经过当前 `out/bin` 的单个直接输出：decoder pump开启、队列1、解码min/max/budget=2、VVenC faster/8线程、future guard=4、受限CRF32（本轮高上限避免以限码压力混淆结构检查）、256×144/30fps、source_all、follow_source。

14例全部正常EOF完成，共解码2688帧、写出1344帧；每例192→96，另外96帧为明确的输出帧率丢弃，无过载丢帧。输出可完整解码，PTS和显示顺序对应源的每隔一帧，FLV毫秒时间基允许1.1ms舍入差；输出I/3B结构检查通过。10个HEVC例均开启codec_trace并通过严格阶段/CPU审计；4个H.264例因该诊断不支持而未启用，不声称对它们通过HEVC专用审计。

这些是文件输入的小样本正确性、顺序、恢复和EOF验证，不是1080p paced RTMP时延测量。上次报告的139.40ms等数值仍只对应其1080p、固定3B、closed GOP配置，不能外推为7B/open GOP的时延。

证据：`out/reorder-matrix-20261022-022312/acceptance.json`、`checks-v2/summary.json`、`service/summary.json`、各例config/final-state/codec-phase-audit、`actual-streams.json`、`entrypoints-linux.log`（14项通过）、`first-packet.c/.log`。最终生成媒体、命令、NAL计数、独立oracle与逐帧hash位于 `out/decoder-test-3jzh6tc7/input-reorder/`。首次失败保留在 `checks/summary.json` 与 `out/decoder-test-73ptyvc7/input-reorder/`；一次在Windows误跑Linux入口测试的失败另存 `entrypoints-windows.log`，Linux正式入口检查通过。
