# 工程落地追踪（2026-10-18）

范围说明：本页保留原 W01–W11 / A01–A22 工程功能交付的源快照与检查点，末尾 candidate-ehp5SC 是该次窗口，不是后续未提交修改的验证身份。后续“测试入口→日志→生命周期→模块→文档”五批记录与当前候选见[核心整理验证](validation-core-refactor.md)。不把旧窗口未做的 ASan/线程故障注入说成当前仍缺失，也不把本地回归说成目标机/远程发布验收。

原始规格：Library `libfile_47049d428cd88191b84555677daef399`，文件 `NakiLiveTranscode-Engineering-Implementation-Plan-20261018.md`，80,393 字节、998 行，SHA-256 `6c48639f5a9b5895c214aee0b7191d06d946c121e9090c8da7a1ab4dca76cae5`。已完整读取并通过当前 Library helper 下载到本机 `out/engineering-plan-20261018/source/`；不修改或替换原文件。

实际起点：主仓 `6fa891f168ac2c66c3979742f5b4c4a90c5294f8`，FFmpeg `4bee779b3dd59407a07baa8d6fc386cf95d8398b`，VVenC `29e3dc57a91d03833ead6370f5cd03f6d61242a5`。本地工程分支 `codex/engineering-plan-20261018`。原规格基于旧 57b2721，其中 F06 的即时失败与阻塞输入轮询已经修复；保留持续压力窗口、小容量防重置、真实 EOF/错误、GENPTS 及 HTTP/section 验收，不退回低突发成功源。

按三个完整工程批次实施，不将 11 个工作包机械映射成 11 个 PR。本任务先本地实现和测试，不发布 SDK/tag/release、不合并部署、不连接真实生产输出；最多两个编译任务，不安装依赖，生成物仅写项目 out 和既有 WSL 缓存。原有 `docs/next-metadata-batch-and-release.md` 保持原样。

| 批次 | 完整目标与依赖 | 工作包 |
| --- | --- | --- |
| 1：控制正确性与基础边界 | 独立 SDK 消费期望先关闭错误依赖入口；显式 job/operation 身份、worker accepted、恢复门槛、统一停服；先补 finalize 和共同资源基础，供后续更新结算复用 | W01、W02；W04 基础；W09 gate/cache |
| 2：进程事件与权威更新 | 进程本地事件、采集维护、worker desired/effective、typed patch/CAS/dedup/有限结果、真实 applied/first_written、健康和 freshness；更新的媒体边界依赖批次1 | W03、W05；W10 |
| 3：媒体合同与交付验收 | 统一生命周期剩余抽取、独立音视频预算、音轨签名拒绝、有效 fps/GOP/work、发布身份、完整验收和迁移回滚；保留前两批协议及 owner 模型 | W04、W06–W08；W09 manifest；W11 |

## 兼容与所有权

保持共享 reader/decoder、每 output 单 owner、有限内存、真实写出/flush 结算及独立 crash collector。新逻辑单路 output_id 为 main，不能通过简单将 output_count=0 改成1来迁移旧执行路径。旧写接口必须固定 job_id；新接口将创建 operation 与每次 HTTP request 区分。结果表不是持久 journal，事件文件不是 WAL。

SDK expectation 由可信完整 checkout 的源码、recipe 和 gitlink 生成，source-less 消费必须显式提供；不得从候选 SDK 自带的 manifest 复制期望。原 SDK 字节/receipt 校验保留为完整性层。开发期仍可在完整源码存在时使用已核实内容相同的本地 SDK，不因新增 consumer gate 重建未改变的原生代码。

## A01–A22 验收账本

状态仅在实际运行对应验收后更新；接口存在不算通过。初始均待实现/验证，A14/A15已有稳定性基线但仍需覆盖本规格扩展。

| ID | 批次 | 场景与判据 | 当前状态 |
| --- | --- | --- | --- |
| A01 | 1/2 | A→B迟到操作不触及B；已有操作按保留结果重放 | 创建/停止及真实 typed 更新的旧任务重放均通过；共享 ID 域冲突在停止前拒绝 |
| A02 | 1 | assignment/ACK/HTTP切点；creation operation恢复唯一owner | 接单前退出、ACK/HTTP丢失、恢复原创建键已验证 |
| A03 | 1 | recovery瞬错/不兼容占用fail closed，媒体继续 | EAGAIN窗口和真实worker不兼容已验证，期间媒体继续增长 |
| A04 | 1 | launcher backoff SIGTERM，绝对deadline不漂移 | backoff SIGTERM恢复并停止已验证worker已验证 |
| A05 | 2 | 预热至接管同worker事件前缀，首帧不重复 | 进程基础通过；W05 真实重启/重试后每个 worker 操作阶段只记录一次，job/output/revision/origin/create 关联字段通过 |
| A06 | 2 | 日志拥塞/短写/磁盘故障/离线、独立flush轮转与遗留段 | 并发顺序、拥塞/短写、启动失败、独立 flush/轮转及 5 项遗留维护用例已验证；真实采集端待验 |
| A07 | 1/3 | 仅在trailer/close故障，有界结束且hook/终态见错 | 单/双路真实HTTP close-only RST、hook同错误码及健康sibling完成已验证；更多协议待补 |
| A08 | 1/3 | 1-5 frame/GOP tails, partial init and stop closure | Passed current candidate VVC one-frame, full PTS/tails and output-finalize suites |
| A09 | 2 | typed patch presence/CAS/dedup，重试不重复加载 | parser/CAS/desired 合并、真实 HTTP 资源只加载一次、跨输出 ID 冲突、CLI 及重连重试通过 |
| A10 | 2 | 快更/prepare失败/owner领取与stop竞态 | 原生 owner/外部取消/跨输出回收竞态、真实 prepare 失败保留 effective、native 输出失败闭合通过 |
| A11 | 2 | B帧旧revision晚写，真实first_written帧 | 真实 VVC 8 个 operation 的 applied/首写帧身份与历史晚写通过，记录不被后续失败覆盖 |
| A12 | 2 | 关日志/丢日志、worker上报前死，已知/未知明确 | 关闭帧日志、实际 event fwrite 失败/丢失、真实 applied/pending 后 SIGKILL 的 unknown/last_observed 查询通过 |
| A13 | 2 | operation表满先拒绝，保留与仍在途记录pin | 256 条 worker/cache 表满、跨 controller 恢复分页、保留重放和独立 stop 预算通过；store 保留/pin/并发引用单测通过 |
| A14 | 3 | Default TS 1 video + 16 AAC, 2/4 outputs and independent type quotas | Passed: w06-queue-6Qgo1a, w06-mixed-cLnl8l and fresh w06-after-p2 run; default burst / smooth TS / bounded 40ms owner pause / payload and reference accounting |
| A15 | 1/3 | Slow sibling isolation, pressure recovery and release | Passed current full multi/mixed/input suites; deterministic low-water-before-refill repair and original failing late.ts retry. No capacity/SLO claim |
| A16 | 3 | Same-PID continuous-PTS AAC/MP3 changes and stable HE-AAC | Passed nine real ADTS/ASC/MP3/HE cases, fresh after W08 in out/audio-signature-q6kolqyd; input signature and one-time rejection/payload/reference checks |
| A17 | 3 | Rational/unknown rate, VFR offset/gap, fallback invariance | Passed out/video-rate-el3avlzd and full ordinary-I/VVC suites; unknown library rate is explicitly test-only injection, not a truly metadata-free source claim |
| A18 | 1 | 当前期望A拒绝自洽SDK B；正确A和字节校验通过 | 9项负例/消费入口测试、12项原测试及本机真实SDK gate已验证 |
| A19 | 1 | 上游头文件保留mtime，下游实际编译行为/缓存更新 | 真正include头文件的DSO前后回归、13项identity/9项expected及中断恢复已通过；真实SDK已重建并验证 |
| A20 | 2 | Busy/recovering/waiting-input/stalled control health and progress | Passed current health-progress and seven lifecycle cases, including unversioned-result capability refusal while media continues. Host budgets remain external |
| A21 | 3 | Source/image/SDK/symbol manifest and repeated build identity | Passed 11 release tests, static CI wiring, actual local production/native ELF/DWARF create+verify; source unchanged. Dirty/no-image local manifest is publication_eligible=false. Actual Docker/CI gate remains |
| A22 | 3 | Target-host acceptance, migration/rollback, budget and SLO | Local source-bound complete candidate passed; runbook and budget examples delivered. Actual collector/host/SLO/24-72h/migration remain external gates |

实际原计划、intake 与源文件校验保存在 `out/engineering-plan-20261018/`。完整候选 CI/发布、目标机器 soak 与生产替换不得写成已通过；缺目标/样本/权限时保留明确门槛。

## 已冻结的阶段证据

- `out/engineering-plan-20261018/runtime-foundation-XYQe9T/`：当前停止 operation 与创建去重、6组真实生命周期切点、单/双路 HTTP close-only 错误，以及原 process 集成全部通过。后续原子 peer-pidfd 和进程事件改动另行回归，不能把该次结果当成最新工作树全过。
- `out/engineering-plan-20261018/transitive-before-9UwJ2I/test.log` 及 `out/sdk-build-61w1l7og/TRANSITIVE-BEFORE.json`：冻结旧逻辑在头文件内容改变而mtime保持时的实际 wrapper 陈旧行为。
- `out/engineering-plan-20261018/transitive-after-KxeFpy/`、`out/sdk-build-edybl11g/COMPLETE.json`：修复后 wrapper 实际从2变为102；恢复原头文件后回到2，并通过原有编译失败、SIGTERM/SIGKILL、seal/configuration失败恢复。
- `out/engineering-plan-20261018/native-cache-migration-dZztQo/`：实际本机 SDK 完成重建，identity `4c6d30a2595d52765a93db3b68ad2a5a0dfca6e6f1880d1a0607041f536a4e16`，源码期望 gate 通过，FFmpeg/VVenC匹配DWARF已分别归档。本任务没有发布任何SDK归档。
- `out/engineering-plan-20261018/process-events-foundation-buVHcW/`：上述 SDK 的应用构建、进程事件、原子 `SO_PEERPIDFD` 接管、logger 故障、维护及生命周期回归均已通过。`event-media-regressions-u1XrRH/` 的 observability 和多路输出套件也已通过。此记录对应冻结基础，不等于后续 W05 工作树全量通过；[事件合同](process-events.md) 明确尚未执行实际 Filebeat 验收。
- `out/engineering-plan-20261018/watermark-operation-8sLx6U/`：owner 领取后新 desired、stop/compose 竞态、旧 CAS 重放、setup/native 失败、历史 revision 写出状态、容量先拒绝及水印复审套件已通过。原生状态单测不替代真实 B 帧与公共 operation API 集成验收。
- `out/engineering-plan-20261018/w05-integration-ApxKYW/`：W05 最终候选正式构建、OpenAPI 引用/语法、parser/store/cache 单测、7 组真实 updates 验收、原水印两套、6 组生命周期及完整多路回归均通过，运行前后源码哈希一致。updates 证据为 `out/update-api-zqgkv0gw/COMPLETE.json`。Filebeat 实际采集环境和整份候选最终矩阵仍有独立验收门槛。

Review P2 follow-up: `w05-integration-wOW3l7` passed complete W05 suites and unchanged source hashes. `out/update-api-q0qnnvbu/COMPLETE.json` includes 256 explicit legacy rejections followed by accepted legacy/typed updates, and actual final-history-page then older applied/pending RPC for both first_written and not_written. Worker result_version covers all state/lease/retention mutations; live/staging controller reconciliation and HTTP projection keep the whole newest result. W07-W11 and actual collector/target-host soak remain.

Latest local scope: full candidate `candidate-ehp5SC` passed, with unchanged source and actual local runtime/native ELF/DWARF binding. External image/collector/target-host acceptance remains pending.

Final local candidate: `out/engineering-plan-20261018/candidate-ehp5SC`, `complete.exit=0`; all 22 integration suites plus unit/static, bounded VVC measurement and symbols passed. Source tree SHA-256 `45d1397b54da2d23091929f8c38fdc003cec70e9883e03e9bec5b268c86e9eca`. SDK regression-prefix reuse is explicit in `REUSED-SDK-EVIDENCE.json` (unchanged 10 recipe/test hashes and pins; prior aggregate failure preserved); actual consumer gate and affected application suites ran fresh. No commit/push/publication/deployment occurred.
