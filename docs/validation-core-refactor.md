# 核心整理五批记录与当前验证（2026-10-18）

以下记录前五批各自完成时的本机冻结窗口；业务实现对应第四批模块拆分，第五批只整理文档，当时未提交、推送或部署。后续合并前整体复查、修复和候选身份单独见[合并前复查](validation-final-review.md)，不回写本页原有测量与摘要。本轮五批编号与早期 probe/帧策略/水印/多输出的功能批次不同。

## 批次与证据身份

| 本轮批次 | 完成范围 | 证据/对应源码摘要 |
| --- | --- | --- |
| 1：测试入口与基线 | quick/full、独立应用 sanitizer、失败传播/覆盖保护、源码/ELF/SDK 绑定 | `out/batch1-baseline-516a5160/baseline.json`；`061ad959d7d1b91ba557634eedea92608642bc44017860c6e27f707746319980` |
| 2：日志提交临界区 | FIFO 预留→锁外准备→发布，顺序、丢失和资源边界 | [专题记录](validation-logger-submit.md)；`out/batch2-logger-50637366/evidence.json`；`a7c9dc1f1726faf49c8f3a8bce4bafd6af9e2cf20a9d0ff8153d547f4fe8f515` |
| 3：媒体生命周期 | 共享正常 drain、释放和终态步骤，保留错误优先级；86 场景故障审计 | [专题记录](validation-pipeline-lifecycle.md)；`out/batch3-lifecycle-f030348d/evidence.json`；`156a5493b092cad80e400b418949725eb8cf953a9719a8fccb0fd06f9a47b2fe` |
| 4：按所有权拆模块 | reader/output/encode/mux；记录/监督/HTTP/投影；Job 分组与枚举状态 | `out/batch4-modules-92f08a31/evidence.json`；`bc8001f8e814ce9e169cc3bc85617efdc49eab34a6681270f13e59565c810760` |
| 5：文档收口 | 当前合同、草案、历史证据与未验证项分开；OpenAPI 对照实现和已保存响应 | `out/batch5-docs-bf325dcc/`；只记录本批静态检查和文档差异，不重跑或改写媒体基线 |

这些 out 目录是本机生成物，不入 Git，新克隆不包含它们。文档保留必要身份和结论；复核完整命令、ELF/输入哈希与原始日志需要对应归档。历史失败尝试和既有 AAC 起播调度差异不覆盖成成功结果。

主仓基点 `a1b7e0584355756d0c5b13eaa6668d2a724d2a45` 加本轮未提交修改。固定依赖：FFmpeg `4bee779b3dd59407a07baa8d6fc386cf95d8398b`（n9.0.2 fork）、VVenC `29e3dc57a91d03833ead6370f5cd03f6d61242a5`、yyjson `6447536015f3d600f3d65323b10976103b337ca7`。SDK identity 为 `4c6d30a2595d52765a93db3b68ad2a5a0dfca6e6f1880d1a0607041f536a4e16`，receipt manifest SHA-256 为 `b090b92635b021391f4f41697ad5e3b69641717881d57e315f7df8e261b0c67c`。

## 第四批实际改动和边界

按单一职责依次搬移，每项构建并运行 quick/受影响套件后继续；11 个最终阶段均通过，命令、退出码和失败修正尝试见 `stage-index.json`、`stages/*/results.json`。实际文件与 owner 见[模块导航](../app/README.md)。

reader 模块封装启动/唤醒/join/队列回收与参数快照；output 模块保持初始化、正常排空、finalize/hook、引用释放和终态顺序；encode 模块保留 PTS ledger 与 delivery 转交；mux 模块保持有界 live mux→av_write_frame→flush→实际写出结算。没有把音频 stream 可变数组交给写出线程，没有增加模块线程或混用 av_interleaved_write_frame。

控制器拆分有界 CreationStore、worker 监督恢复、HTTP 和投影；ACK 接受、原 operation 重试、恢复前关闭 admission、旧 job 隔离和 peer/pidfd 约束不变。Job 的 lifecycle/progress/stats/flow 分组仍沿用原锁，枚举仅替代内部字符串，对外状态名不变。

保留 reader 8 项/64 MiB、单 item 16 MiB、256 项编码关联、16 音轨、128 项延后音频、每输出 live mux 256 包/4 MiB、5 个 worker 槽及 32 项创建保留。完整 profile、独立字节/work/age/在途/水印限制仍以[服务](service.md)、[固定输出](branching-contract.md)和[事件](process-events.md)为准；这些不是 RSS 或吞吐保证。资源常量与未改的队列/logger 字节核对见 `resource-contract.json`、`preserved-app-files.json`。

## 第四批构建与验证

环境为 WSL Ubuntu、GCC 13.3.0、Python 3.12.3、CMake/CTest 3.28.3，最多 2 个编译任务，复用同一核实过的 native SDK。

| 检查 | 实际结果与证据（相对 `out/batch4-modules-92f08a31/`） |
| --- | --- |
| 正式 Linux 构建 | `scripts/build-service.sh` 通过，SDK consumer gate 通过；`build.log`、`validation.json` |
| 逐职责快速/受影响验证 | 最终 11 阶段通过；reader 的音轨/SEI/生命周期，output 的 finalize，mux 的 live mux/多输出，encode 的 hook/VVC PTS，控制侧的更新/恢复/健康/观测等均有对应记录 |
| full | 34 步通过，含 15 项 quick 原生测试及媒体/更新/恢复/故障/符号；`full/summary.json` |
| 应用层 ASan/UBSan | 6 步通过，含 15 项原生测试、水印复审、多输出、86 场景与服务；`sanitize/summary.json`；Debug 构建为 `out/sanitizers-3l2QBA/build.log` |
| 生命周期对照 | 普通与 sanitizer 各 86 场景通过；`comparisons.json` 的两组 strict_different_cases 均为空 |
| 音频计数 | 对成功完成输出核对实际 AAC 包数=audio_packets_out，写出+audio_dropped=共享/直接 hook 观察数=输入 46；`audio-accounting-full.json`、`audio-accounting-sanitize.json` |
| 输入绑定 | full/sanitize 同一源码摘要，前后源码、所选 ELF 和 SDK manifest 不变；最终重新核对当前文件；`evidence.json` |

生命周期对照使用第三批修改后、第四批之前的 `out/pipeline-lifecycle-kc0_7vf2/COMPLETE.json` 为基线，普通重放为 `out/pipeline-lifecycle-0yz186dx/COMPLETE.json`，sanitizer 路径见 comparisons.json。输入 SHA-256 均为 `4bca9f27a4e36e9d53005d3f98cfef4d2087779692b6021423f8469667738d1e`。比较终态、稳定错误/hook/finalize、健康输出解码帧数、PTS、AAC 载荷和相应计数；不要求动态时间、PID、取消时自然写出的帧数或编码文件字节相同。本次严格一致不把第三批已记录的起播迟到音频前缀变化变成不存在。

函数核对的归一化范围及六处接口封装人工核对在 `audit-summary.json`，不等同独立第三方源码复审，也不代替运行测试。本批不宣称时延、容量或吞吐改善；第二批日志微基准仍只按其自身 CPU/队列测量边界解释。

## 第五批静态核对与结果

检查 README/模块路径、文档本地链接、JSON 重复键与 OpenAPI 本地引用/结构、请求示例和第四批保存的响应、证据文件/摘要/退出码及未验证项，结果写入本批新目录。没有安装新验证依赖或修改 tests/scripts 来迁就文档。

统一测试输入摘要包含 `app/README.md`，所以更新模块导航后总摘要会变化；第五批须逐文件记录该文档差异，单独证明所有非文档源码/配置、测试、依赖和既有证据字节未变。不能把第四批摘要说成文档更新后的逐文件摘要，也不因文档变化重跑全量媒体/性能矩阵。

在 WSL 执行 `python3 out/batch5-docs-bf325dcc/check_docs.py` 通过，`git diff --check` 通过。本批检查器和结果仅存新 out 目录，不加入测试/业务实现。

| 静态检查 | 实际覆盖/结果 |
| --- | --- |
| 接口结构 | 12 个路径、13 个操作、33 个 schema；重复键、JSON Schema Draft 2020-12、本地引用、路由及请求大小边界核对通过；未运行完整 OpenAPI 元 schema 验证器 |
| 示例 | examples 中 7 份请求、已实现接口文档中的 7 个 JSON 示例通过；6 个非法请求反例被 schema 拒绝；没有发送请求或打开媒体 |
| 保存响应 | 第四批普通/sanitizer 各 86 份终态，以及保存的操作结果，共 181 份状态/操作文档通过 schema 核对；这是重读证据，不是新增运行回归 |
| 本地导航 | 全部 52 份 Markdown 的本地路径与已用标题锚点通过；不验证外部网址可用性 |
| 范围保护 | 第四批统一输入清单内 172 个非文档文件字节一致，唯一输入差异为 app/README.md；本批开始前清单的非文档文件及原用户规划文件保持不变，没有新增非文档输入 |
| 历史记录 | 旧 validation 文档只加历史窗口提示，原正文与数值保留；原 out/媒体/符号未改写 |

文档更新后的统一输入 SHA-256 为 `6aa581a1b008b9e8a08960f7f5dd00a94c0b12bd378d3b63bd4f8727f5ab83e8`，与第四批的差异仅为上述模块导航；README/docs 的其他修改按独立文档清单记录。详细检查、逐文件摘要和本批差异见新目录的 `checks.json`、`source-after.json` 与 `evidence.json`。本批没有新构建、媒体运行、基准、提交或发布。

## 未验证与未交付

- `NAKI_TEST_HTTPS_URL` 未配置：外部可信 HTTPS 水印成功项在 full/sanitize 各记录 not_run；本地 TLS 拒绝路径已通过。
- 应用、yyjson 和 helper 有 ASan/UBSan/leak 检查，FFmpeg/VVenC/系统库未重新插桩；没有 TSan 或“全媒体栈无泄漏/竞态”结论。
- 上述五批本机窗口没有实际 Docker 构建/smoke、远程 Actions、真实 Filebeat 采集/补采、目标机容量与 24/72 小时 soak、实际迁移回滚或生产发布；历史镜像结果不能补齐。后续远程验收须查询相应 PR/提交的检查记录。
- 未知帧率分支使用测试专用注入，不证明源本身没有帧率元数据。生命周期故障包装应用调用点，不覆盖 SDK 内所有分配或任意损坏码流。
- 资源限额不是 RSS/吞吐/SLO 保证；单次不可抢占库调用仍可影响终止时间。首帧和逐帧均为服务 handler/ingress→实际本地 mux/flush，排除采集、客户端上传、wire time 和播放器显示。

当前能力、草案和历史记录统一从[文档索引](README.md)进入；目标机器的外部验收门槛见[迁移说明](candidate-migration-runbook.md)。
