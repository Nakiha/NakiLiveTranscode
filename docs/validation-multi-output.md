# 固定多输出与动态输入适配：本地验证（2026-10-17）

当前实现基于已确认的[固定输出合同](branching-contract.md)。本轮 15 项多输出实流场景、13 套 Python 功能回归、5 个 C 单测、12 项 SDK 身份单测及符号回归均返回 0；**23 项水印本地复审通过，1 项公共可信 HTTPS 成功路径未完成**。没有把未运行项算作本轮全绿。新的完整接入已通过独立源码复审，按授权保存本地完整提交，不推送。

最新证据目录：`out/multi-output-acceptance-20261017T1430/`。精确退出码、源码快照/摘要、git 状态、完整 binary-safe 差异、匹配 Build ID/DWARF、媒体与接管证据见其中 `ACCEPTANCE.json`。基线 HEAD 为 `5052d203bc438dd1796bfb5273c1d82a5013ea77`；FFmpeg `d8303fa436059a96bb773df8566a73c6253c8159`、VVenC `29e3dc57a91d03833ead6370f5cd03f6d61242a5` 未改。SDK 身份保持 `952134bb13b47fe2de5099abebc8e156b0f35c9c67d395fdce05989517f084e1`；应用 21 个 ELF 的匹配 DWARF 为本轮新归档，FFmpeg/VVenC 复用相同 Build ID 的 6+1 个归档。

| 功能证据 | 实际检查 |
| --- | --- |
| 一次输入、两/四路 | 125 张共同 decoded frame、decode 样本 125；四路各写出 125 包，共 500 条带 output_id 的帧记录；各路实际首/末 mux 区间存在共同交集 |
| 私有帧头与 COW | 四路分别红水印、蓝水印、裸帧及 10-bit VVC；独立同步解码器核对每路尺寸、颜色、背景、帧数和 VVC 的实际 B 帧；没有跨路水印污染 |
| 音频 | AAC payload 为源序列的有序子序列；实际写出包数与状态计数一致，写出+明确丢弃等于全部输入音频包数，unsupported=0；另有单路对照，不以任意保留百分比定义成功 |
| 独立热更 | 两路各自 revision 2，更新一路时另一 accepted_revision 保持 1；未知/缺失 output_id、已失败路拒绝；required 首/末帧均有水印 |
| 队列/全部失败 | 单路字节预算不足，另一仍完整写出 125 包，终态 completed_partial；坏目标+超限使全部失败，任务 failed；所有队列/在途计数归零 |
| 真实慢路 | TCP 接收端已连接但完全不读，接收/发送缓冲缩小；300ms I/O 预算使该路失败，健康路同一 worker 完整写出 125 包 |
| 动态输入 | 单个 HEVC 流 64×48/8-bit→96×64/8-bit→128×96/10-bit→64×48/8-bit，共 80 张；四路固定 MP4/FLV/TS/H.264/VVC 输出，各 4 代滤镜、80 包；逐帧 PTS（FLV 允许毫秒量化）、画面和水印正确，encoder/连接未重开 |
| 源 PTS 空洞 | 8 帧源中插入 5 秒空洞，400ms 队列名义处理预算不把它当作积压；两路保持全部 PTS 和帧数 |
| 起播门禁 | required HTTP 资源未就绪时 input_open_started 为空；释放后共同读入。一路资源失败时只失败该路；全部门禁失败时不存在的输入从未打开 |
| 必需水印重检 | 中途 SDR→HDR 明确失败各分支；失败前的 MP4 前缀逐帧均有水印，没有裸流兜底或假 tone mapping |
| 接管/取消 | 控制进程 SIGKILL 后两路文件继续增长；新控制进程接管同一 worker、状态和帧数继续，output_id 热更有效，再停止后所有分支 stopped 且引用计数归零 |

独立基础审查提出的 P2 一并修复：`avcodec_parameters_copy` 分配的 custom `ch_layout.u.map` 按 `nb_channels*sizeof(AVChannelCustom)` 计入音频保留预算，检查乘加溢出；单测真实复制映射、先释放源参数，再验证独立副本、单项/累计超限以及失败时调用方所有权。延后音频队列也使用完整保留计费，不只计算 payload/extradata。

开发时的失败证据保留在 `out/multi-output-dev-20261017T1400/` 各独立 attempt。动态水印曾因初始图和新图色彩协商不一致失败；现在显式声明 buffersrc 矩阵/范围，编码器使用 sink 协商属性，新图转换至固定属性。旧 graph 先排空，之后才交换 source/sink 和更新输入规格；旧帧使用原 Stamp/generation，不按新输入宽高解释。部分成功曾被控制面误报异常退出，现已将 completed_partial 纳入正式终态并实流覆盖。

分支停止不在共同输入循环里 join；先取消该路并清空引用，继续处理健康路，只在整个任务收尾时等待 owner。I/O 打开、写出、trailer/close 保留截止及 cancel 回调；单次不可抢占 native 调用仍不具硬实时终止保证。原生崩溃影响同一 worker 全部输出，区别于可返回错误/队列压力的分支隔离。

本轮没有修改 FFmpeg/VVenC、安装依赖、改变安全设置、推送或部署。AGENTS.md 保持用户原文；本批范围以本轮明确确认的固定输出合同为准，不扩大成任意动态输出规格。未运行 Docker 镜像构建、远端 CI、ASan/TSan、容量/时延基准及长期高规格稳定性测试；不能把功能验收描述为生产能力证明。

收尾静态核对后又加固了线程创建失败的终态及共享 reader 的提前唤醒：没有 owner 启动时直接标为 failed；取消/错误在私有输出清理前先广播 reader ring。13 套完整回归窗口保留不覆盖；这项局部修改后重跑受影响的 15 项多输出实流、process_integration 和当前符号回归，证据位于同目录 terminal-state-check。其他运行源码未变，当前源文件快照和应用 DWARF 以该子目录为准；不能把此前完整窗口说成在修改后重跑。
