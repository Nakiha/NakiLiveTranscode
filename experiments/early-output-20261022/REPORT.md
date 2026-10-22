# 解码提前出帧独立开关：实验归档

> **实验版，最终验收未完成，不建议生产启用**

2026-10-22。本轮只核对、整理并公开既有实现和技术材料，没有运行新测试、编译、生成样片、修改算法或部署。应用基线为 `67f5e07798575e20e1f3fe1dcc6e37b40f3d64fa`。应用独立分支为 `experiment/decoder-early-output-archive-20261022`；主分支、SDK gitlink 和生产默认值均未修改。原工作区及暂存区保留。

本提交公开六个原有改动：`app/control/config.c`、`config.h`、`control_spec.c`，`app/media/decode_output_policy.h`、`pipeline.c` 及 `docs/openapi.json`；同时留存三份配置初始化测试源码/脚本和本技术说明。完整文件清单见 [PUBLICATION_FILES.txt](PUBLICATION_FILES.txt)。

## 版本与证据对应

应用原始 diff 的 SHA256 是 `38c1333654d8e93b698fb20099be62f6ab61e967e89a8e927cff49eb8d1f637e`，与研究目录 phase2、phase3 两份 `application-switch.patch` 逐字节相同。本分支的六个源码/接口文件与该快照相同，没有随文档整理改变策略。

对应 FFmpeg 基线为 `94bd940f1c4682dfce13e8ae43891df01621e16e`。phase2 的 FFmpeg patch 是加固前版本（SHA256 `b138d0e9bb5dd21e9e9f6211d261f3ef84ebf3e3c3e87444c094cdc2e094654d`），不是当前源码；当前六个 H.264 源文件匹配 phase3 冻结身份及 `ffmpeg-final.patch`（SHA256 `8da6856d143c24252103f4d3babd7119cccbb872150b9e1405d875b8020f2034`）。完整报告、历史报告、补丁、测试工具及证据索引保存在 [FFmpeg 独立实验分支](https://github.com/Nakiha/FFmpeg/tree/experiment/h264-early-output-archive-20261022/doc/early-output-20261022)。对应精确 FFmpeg 提交身份记录在 [COMPANION.json](COMPANION.json)。这里的元数据引用不会更新应用的 submodule pin。

phase2 独立审查以 F1/F2 拒绝安全验收：首 slice 已摘除额外 DPB 图后，完整 AU 的异常/late/重复 SEI、短 timing payload 不能可靠使其失效。phase3 源码增加完整 AU 资格守卫，把额外提交移到 field_end 之后，并在全部输出准备成功后才原子应用及发布真实 frame setup；多 slice 保守回退。phase3 专项复审的修复结论保留为历史有限范围结论，不能等同最终验收。

**本次发现的证据缺口：** phase3 代表性测试脚本以 file.stem 生成前缀，HRD/普通目录中的同名 `x264-fixed7-pyr1-open1.mp4` 共用路径。83 对 summary 中有 20 对（0-based 10–19、50–59）涉及跨输入原始记录归属冲突，其中 5 个 early-on 条目的 differing_columns 与当前 raw 不符。完整报告将这些证据保守标为未知，未重跑或改写原日志。不能沿用“83 对原始证据全部独立核实”的描述。

## 开关语义与自动路径限制

`decoder_early_output` 是 avcodec_open2 前设置的可选布尔初始化策略，与 async owner 和 decoder_pump 独立，不支持热切换。

| 值 | 行为 |
| --- | --- |
| 省略 | 内部 -1，保留原 HEVC 开、H.264 关；省略字段不改变旧 fingerprint |
| false | 关闭 HEVC live_idr/live_poc，H.264 使用原 DPB 策略 |
| true | 请求受证明条件限制的输出策略，不绕开资格守卫 |

H.264 自动路径限定软件 Main/High、逐行 POC0、whole AU、相等场 POC、无 bottom POC offset/film grain、default discard、有效 picture PTS；还需 timing_info、fixed_frame_rate=1、pic_struct absent、单 CPB NAL HRD、非 VCL HRD、low_delay_hrd=0 和当前有效 timing/BP。多 slice 和其他不支持情况回退。无 HRD/CFR/timing 声明的普通流回旧 DPB，不能声称任意普通 H.264 都有收益。旧 SDK 没有 H.264 option 时同样回退。

自动模式等待旧 DPB 选出 IDR anchor，保留 H.264 启动等待。连续两 tick 时槽推导依赖输入遵守规范声明，不是完整合规校验或未来无损坏证明。库中 `live_poc_verified_even` 是另一个默认 false 的可信调用者全文件契约；应用不设置它，不得从编码器名字或此布尔开关推断它。离线全片证书模式不是直播收益。

应用自身没有 async-off 配置，decoder_pump 只是前端派发；codec harness 的同步/异步 × early off/on 不是服务四组合实跑。省略该新字段仍保留旧行为；公开分支不改变生产请求默认。

## 已有结果与剩余验收

既有 phase2 配置/旧 SDK 及 phase3 初始化日志有 PASS，原 codec harness 的正常矩阵、88 对加固后针对性检查、10 对附加边界及 5 项 ASan 有对应技术记录；严格未来坏 AU 的 off/on 前缀可为 10/11 帧，不能写成任意损坏流全字段、同长度一致。部分代表性证据存在上述冲突。

phase2 的 162 次性能数据属于加固前版本。phase3 已有 156 条数值记录，包含 24 条旧 phase2 构建对照，当前程序哈希和原日志帧数/terminal 与冻结材料吻合；这不是最终性能解释或最终验收。本次不把旧数字当作新版本成绩，也不宣称加固后 CPU、吞吐或端到端收益已确认。AVFrame.pkt_dts 可能随交付触发改变；既有 HEVC 的不一致 PTS=DTS 时间轴还涉及 best_effort 边界，不能只豁免 DTS 就算精确通过。

仍未完成：冲突记录的可靠补证、加固后性能及最终综合复审、Linux 完整服务/匹配 SDK 的显式实验集成、真实流与 filter/编码/封装链、全部异常/重配置/分配故障范围、TSan 和 LeakSanitizer，以及生产场景容量验收。当前 pinned SDK 不含本次 H.264 新实现；测试脚本的存在不代表服务已接入。完整 Linux 服务未在原 Mac 实验中构建运行，也未在本轮部署。

## 脚本留存与公开边界

`research/phase2/{build_config_test.sh,config_switch_test.c,config_identity_adapter.c}` 是已有初始化测试的逐字节副本。它们按原实验布局查找父目录，不应直接从本归档目录执行。原布局需单独的 LAB，包含 `FFmpeg/`、`NakiLiveTranscode/`、`research/phase2/` 和私有 `out/`；完整工具和布局说明见 FFmpeg 归档。脚本未运行，未来复现需另建全新输出目录并单独授权。

提交中不包含媒体、FATE 样片、原始 out/ 日志、二进制/构建产物、内网资料、凭据或无关文件。本说明和测试脚本的公开不构成启用或部署建议。
