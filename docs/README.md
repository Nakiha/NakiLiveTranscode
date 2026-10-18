# 文档索引与当前状态

更新日期：2026-10-18。当前为实验性的 Linux/WSL 0.9.0 服务：一个活动 H.264/HEVC 输入、1–4 个固定逻辑输出。FFmpeg 使用 n9.0.2 fork，历史目录名 `ffmpeg-7.0.3` 不代表当前库版本。

## 当前合同

| 阅读目的 | 文档 | 状态/边界 |
| --- | --- | --- |
| 运行与 HTTP 字段 | [服务](service.md)、[OpenAPI](openapi.json) | 已实现的 v1；无认证的本机接口，静态 OpenAPI 不由服务托管 |
| 创建/更新/停止与查询 | [组件更新](component-updates.md)、[健康进度](health-progress.md)、[进程恢复](processes.md) | worker ACK、显式 job/operation 身份、有限内存重试；不提供持久 journal 或 exactly-once |
| 固定多输出/动态输入 | [固定输出合同](branching-contract.md) | 2–4 路独立 owner；单元素 outputs 归一化为直接路径；不在线增删路或改输出规格 |
| 音频与封装 | [live mux](live-mux.md)、[音轨签名](audio-signatures.md)、[输入恢复](live-input-stability.md) | AAC/MP3 透传与晚音轨追加；MP4 视频限定；不是音频转码或任意 codec |
| 帧/时钟/元数据 | [帧策略](frame-policy.md)、[SEI](sei.md)、[名义帧率](video-rate.md)、[VVenC](vvenc.md) | 严格 source_all、显式 I/抽帧、选定 SEI 类型；不修复脏时间戳，不自动保留所有消息 |
| 水印 | [单水印 v1](watermark.md) | 每输出 SDR 8/10-bit，JPEG/PNG/GIF，完整 revision 兼容接口与 typed patch；HDR/多水印未实现 |
| 观测与运维 | [指标](operations.md)、[事件](process-events.md)、[崩溃](crashes.md)、[内部 hook](media-hooks.md) | 成功本地写/flush 边界；socket time 不是 wire time，hook 默认不做响度算法 |
| 构建/交付 | [FFmpeg 开发](ffmpeg-development.md)、[CI](ci.md)、[Docker](docker.md)、[发布绑定](release-binding.md)、[目标机验收](candidate-migration-runbook.md) | 配置/流程存在不等于远程、镜像或目标机通过；须核对相应提交的检查 |
| 源码所有权导航 | [app 模块](../app/README.md) | 当前按职责分目录的文件、锁序和 owner；不是通用插件框架 |

## 当前验证与历史证据

本次目录、公共设施与接口整理的最终窗口见[目录整理验证](validation-organization.md)，功能入口见[测试说明](testing.md)，独立测量见[性能入口](benchmarking.md)。历史报告继续保留原源码窗口。

五批范围和各自冻结窗口从[核心整理验证](validation-core-refactor.md)进入；最新发现、修复及候选绑定见[合并前复查](validation-final-review.md)，维护入口和结果口径见[测试入口](testing.md)。第四批源码窗口 full 34 步、应用层 sanitizer 6 步通过，两套均含 86 个生命周期场景。第五批只对文档/接口描述作静态核对，不产生新的媒体时延或容量结论。

- [日志提交记录](validation-logger-submit.md)、[生命周期记录](validation-pipeline-lifecycle.md)分别保留第二、三批的构建、哈希、测量和失败尝试，不能把不同源码的窗口拼成“当前全部通过”。
- 其他 `validation-*.md` 是各自日期、pin、输入、环境的专题历史记录；原始 out/媒体/符号不改写。n7 性能与旧版本 Docker/HTTPS 结果不是当前候选证据。
- [工程落地追踪](engineering-implementation-20261018.md)与[原功能计划](implementation-plan.md)记录之前的功能批次，与本轮“测试入口→日志→生命周期→模块→文档”的五批编号不同。
- [工作区迁移](workspace.md)只描述原历史媒体/工具迁移；服务当前合同看上表。

## 草案与明确未实现

| 文档 | 尚未实现的范围 |
| --- | --- |
| [处理链草案](pipeline-design.md) | `/v2/jobs`、通用有序滤镜、crop/pad/sharpen 配置、去交错/fps、一对多谱系、HDR/ROI、任意在线输出重建 |
| [直播帧策略后续设计](live-frame-policy.md) | source_i_anchors、脏时间戳修复、旧示例中的自动 SEI 全保留/迁移；已实现子集以 frame-policy.md / sei.md 为准 |
| [SCTE-35 草案](scte35-design.md) | TS 节目信令保留、专用 section 输出、注入/查询/取消；不是已开放接口 |

草案 JSON 不能整体发送到 v1。选定数字类型/uuid 的 SEI opaque 透传已实现，但不保证消息语义、HDR/几何修正、完整多层/多画面 AU 或任意 UNKNOWN 自动策略。

## 本机证据未覆盖的项目

外部可信 HTTPS 水印成功样例未配置，full/sanitize 明确记录 not_run；本地 TLS 拒绝路径已执行。本机记录未覆盖实际 Docker 镜像/smoke、远程 Actions、真实 Filebeat 采集/补采、目标机容量与 24/72 小时 soak、真实迁移回滚或生产验收。镜像/远程验收以对应 PR/提交的检查记录单独确认，不把本机配置检查算作镜像通过。ASan/UBSan 仅覆盖应用、yyjson 和 helper，native SDK/系统库未重新插桩，TSan 未完成。未知帧率用例包含测试注入，不宣称真实输入完全无帧率元数据。这些缺口不能由文档、部署示例或短功能回归补齐。
