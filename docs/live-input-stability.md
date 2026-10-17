# 默认 TS 突发与暂停输入

本轮从主仓 `57b272175849038a518967b7a048b254c1da1eb0`、FFmpeg `d8303fa436059a96bb773df8566a73c6253c8159` 开始，本地分支均为 `codex/live-input-stability`。两个问题分别位于应用分支准入和输入 demux，不涉及新业务功能。

默认 MPEG-TS mux 延迟可形成连续 16 个音频包；队列已有视频时，原实现对下一项的 `EAGAIN` 立即失败。现在保持 16 项等硬上限，先投递所有健康分支，再在共同的 100ms 持续压力窗口内重试。低于半水位才恢复，容量 2 不会因腾出一个槽而反复延长窗口。持续慢输出失败后，后续快路继续；暂存项、所有权和额外保留边界见 [分支合同](branching-contract.md)。

原始 codec probe 的 100ms 预算原本只在两次 demux 调用之间检查。未知晚到 PID 后，底层 transport 阻塞可让已缓存视频一直等源恢复。新的 fork-only `avformat_live_input_probe_poll()` API 1 显式启用 MPEG-TS 可恢复轮询：固定物理包缓冲保存 188/192/204 字节半包，PES/section 在临时 `EAGAIN` 时不冲刷，真实 EOF 才完成收尾；取消和真实读错误保持负错误。HTTP 临时饥饿不会当成 range 续请求，分块行、ICY 元数据半包及解压状态保留。

服务在初始探流后启用此 API，跨 `EAGAIN` 保持同一次逻辑读取的 10 秒中断截止；其他 demuxer 仍用原 `probe_limit`。libavformat 微版本升至 63.3.104，旧 API 保留；新服务必须匹配新 SDK，旧 63.3.103 归档不能复用为新依赖。自定义 AVIO 回调须自行返回 `EAGAIN`；文件系统、不可抢占 codec 调用和未验证协议不具有硬实时抢占承诺。测到的是本机已缓存包/服务写出进度，不是源到屏幕延迟或容量结论。

回归入口：

```sh
out/bin/naki-branch-queue-test
python3 tests/multi_output_integration.py
python3 tests/live_input_stability_integration.py
```

最后一条生成新的 24 秒晚到 PID 源，检查 TCP 三种包尺寸、HTTP 固定长度/分块体/分块头、gzip、ICY、SRT 暂停恢复，及真实服务单路/双路、同 worker 控制接管、暂停时取消。恢复后的音视频 packet payload、PTS、DTS、duration 和数量对照普通文件路径；另查真实 EOF、TCP reset 与取消。默认多输出成功源恢复原始 mux 默认值，并要求实际包含至少 16 个连续音频包，不再用低突发源规避问题。

2026-10-17 本地构建及上述回归已完成。SDK/服务均使用项目脚本和最多 2 个编译任务；前后多输出运行均固定到同一个 CPU，默认 TS 源 SHA-256 均为 `a92022414e51ca1aaab8cf705054828c8aaf0c58b482b1bdc683af4adc6187d4`。旧版四路退化为 `completed_partial`，新版四路各完成 125 帧（共 500）；全部 15 类原多输出场景通过，突发前置断言独立记录连续音频包为 16。阻塞 TCP 慢路失败，健康路在同 worker 内继续完成 125 帧。

暂停源前后 SHA-256 均为 `b24c403ad6422fb0ca1fbb098c5c820a60318ce030bfccb95dfdede6c8be62eb`。TCP 三种包尺寸在前 500ms 的交付从 151 增至 198 包；单次读最大耗时从约 1189–1195ms 降至本次的 0.199–0.265ms。这是特定本机夹具测量，不是调度或网络的普遍时限。真实服务暂停 750ms 时，解码/实际写出从 149 增至 196 帧；双路接管保留同一 worker（306984）和一次输入连接，恢复后各完成 600 帧。真实 EOF 为 600 视频包，TCP reset 保留 `ECONNRESET`，取消保留 `AVERROR_EXIT`。

证据：旧版传输 `out/input-poll-djyh0p8j/COMPLETE.json`，旧版服务 `out/input-service-zojjz_o7/COMPLETE.json`；新版传输 `out/input-poll-c6pf3w9r/COMPLETE.json`（12 项），服务 `out/input-service-2mj7htz6/COMPLETE.json`，默认多输出 `out/multi-output-5t0vkng2/COMPLETE.json`。原生队列/滤镜/mux、既有探流和实时 SRT/RTMP/MP3 回归日志汇总于 `out/live-input-stability-RwHVkR/after-UjSky9/`，实时链路明细为 `out/live-mux-g_tt9lme/`。

此前冻结的验证记录继续表示其原提交和原测试条件，不因本轮修改而改写。本轮没有暂存、提交、推送、PR、发行归档、合并或部署；父 HEAD 和子模块 gitlink 保持起点，补丁等待独立复核。未重跑容器 CI、ASan/TSan、长稳或容量测试，不能用本轮功能回归替代这些证据。

## 最终 GENPTS 与 section 复核

独立审查另发现 GENPTS 缓存包等待补 PTS 时，会把非 EAGAIN 错误也当作 EOF。最终产品增量仅为 `av_read_frame()` 的三行保护：启用 live input poll 时，只有真实 EOF 才允许合成 PTS 并排空；其他错误立即传播，缓存和所有权不变。普通路径保持原行为。

新增 FFmpeg 原生测试 `libavformat/tests/live_input_genpts.c` 与 `fate-live-input-genpts`：确定性放入 DTS=100、PTS 未知、duration=40 的缓存包，再从 read 回调返回 EIO、读中取消、EOF、EAGAIN；另测普通路径 EIO。旧库前两项均错误返回成功、PTS=140；新库分别返回 -5 和 AVERROR_EXIT，保留 PTS 未知的缓存包，之后明确输入真实 EOF 才可正常排空，引用恰好释放一次。

```sh
make -C "${NAKI_BUILD_ROOT:-$HOME/.cache/naki-live-transcode-n9}/ffmpeg" -j2 fate-live-input-genpts
```

最终传输回归扩为 16 项：保留此前全部用例，新增公开 poll API 的 GENPTS EOF/reset/取消，以及唯一 version-31 PMT 的跨 TS 包暂停。专用 section 为 278 字节、CRC 正确，位于最后一份 PMT，跨两个 TS 包；在续包交付 73 字节后暂停，恢复后必须观察到 version 31，且 600 视频包及完整音频账本与普通文件完全一致，没有后续 PMT 可掩盖丢失。

最终 SDK 内容身份为 `d3018b7ba4a40ef2f7d59c78bff83a440d12c7ac0ad3a659eb7e0cced6c6fb0e`，仍为未发布的 63.3.104。FATE、原生前后对照及最终传输/服务/探流回归全部通过，证据位于 `out/live-input-stability-RwHVkR/genpts-OL26rf/verify-DmSgkT/`，传输明细 `out/input-poll-wkt64o8p/COMPLETE.json`，服务明细 `out/input-service-qj_kzxke/COMPLETE.json`。此前完整多输出和 SRT/RTMP 矩阵在上一轮 SDK 身份 `75e93ac…` 上通过；本次三行 GENPTS 修复后未重复这些矩阵，不将先前结果描述为本次重跑。
