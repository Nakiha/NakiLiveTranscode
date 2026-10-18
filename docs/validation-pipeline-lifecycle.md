# 第三批：单输出与多输出生命周期

历史验证窗口：本页的“当前/最新”、pin、SDK、提交/复审状态及未运行项只指下述那次源码与环境。当前工程整理候选及未验证项见[核心整理验证](validation-core-refactor.md)，能力/草案见[文档索引](README.md)。原始结果、数值和失败尝试不回写。

按本轮五批改造计划的第三批实施，只在 `app/pipeline.c` 内收敛重复步骤；不拆模块，不调整 Job 结构、HTTP/IPC、worker/controller 生命周期、调度算法或 native 库。前两批改动保留，历史验证与用户原有文档不覆盖。

## 共同步骤与保留的行为

- `drain_output` 共用于正常 EOF 的 filter → encoder → deferred audio → live mux 排空，末尾检查 PTS ledger 必须清空。旧单输出在原位置调用；分支仍在私有队列 EOF 时调用，原 media_phase 发布位置不变。取消/错误仍沿原关闭路径处理。
- `finalize_output` 原本已共用，继续保留唯一输出 owner 执行 trailer、flush、协议/自定义 FD close、hook close 与 format free 的顺序，不引入第二套实现。
- `release_media_resources` 共用延迟音频、BSF/音轨签名、接收帧、codec、filter、compositor、input 与 ledger 引用释放顺序。分支没有共享 decoder/input/接收帧，相应空指针释放为 no-op；共享 owner 的 pending/ring 清理仍在调用前执行。
- `complete_job` 在媒体引用释放后结算终态、发最终报告、置 done；保留分支失败优先于父取消、根任务 stopped/failed/completed_partial/completed 的原规则。只有共享 owner 在分支全部 join 后结束公共 trace。

旧顶层单输出及 `outputs` 单元素仍归一化为 `output_count=0` 的原执行路径；单元素保留自定义 output_id。2/4 路仍是共享 reader/decoder 加各自私有输出 owner，没有把单输出伪装成分支，也没有扩大任何队列或额度。

### 初始化与所有权账本

| 资源/边界 | 归属与有效标记 | 关闭约束 |
| --- | --- | --- |
| reader、ring、pending input | 共享 owner；reader_started、ring 内引用、pending 指针 | 错误/停止先广播 ring_space，再等待私有输出关闭；reader join 后清 reader_started，再 free async |
| decoder/async | create 成功时 decoder 转交 async 并置空；失败仍归共享 owner | owner 串行 free async，等待内部线程结束；之后才能释放输入和 eventfd userdata，后者仍由原 worker 收尾 |
| 分支队列与线程 | initialized 只在队列成功初始化后设置；started 只在 pthread_create 成功后设置 | 只终止/销毁已初始化队列，只 join 已启动线程；原 finish_branches 顺序保留 |
| graph/encoder/output/audio/ledger | 每个输出 owner；空指针表示尚未取得；header_written 表示可写 trailer，custom_output_fd 初始 -1 | 局部初始化失败也走共同 finalize/release；资源释放完才清在途账本、报告终态 |
| watermark operation 与 hook | 原 owner_finish/stop 点；hook 归该输出 owner | 不移动 operation 结算边界；finalize 保留每个已设置 close hook 调用一次，再释放 codec/filter/ledger 中剩余引用 |
| trace/logger | worker 共享 logger；根任务持有公共 trace | 分支只发报告并唤醒根任务；根任务在 join 全部分支后 finish trace，worker 后续 release/stop 顺序不改 |

这里没有新增取消、重建或通用管线状态机。已有初始化标记、空指针和所有权转移约定继续承担边界；本批不重写 POSIX 同步对象初始化失败的处理策略。

### 错误规则

已有媒体主错误为负时，trailer/flush/close 只记录各自错误，不替换主错误。未停止且媒体成功时，按 trailer → flush → close 的顺序提升首个错误。停止时仍记录清理故障，保持原停止分类；连续过载的超时覆盖规则也留在原位置。每个分支的原始错误与共享任务的聚合错误分别保留；健康兄弟分支可继续完成。

共享输入错误会在 join 前设置内部取消，但根任务的 stopped 使用设置取消前的快照，保留“内部错误触发清理”与“用户停止”的区别。所有输出已失败时，根任务可能在某个已失败分支 finalize 期间开始取消；该分支 hook 的 cancelled 可随这一原有竞态为 0/1，原始主错误仍保留。

## 新增 86 场景矩阵

`tests/pipeline_lifecycle_integration.py` 同时进入 full 和 sanitizer。旧顶层单输出、outputs 单元素各 20 场景，2/4 路各 23 场景：正常 EOF、实际空文件、空解码 EOF、decoder/async/filter/encoder/output/open/custom AVIO/header 的部分初始化失败、reader 创建失败、共享解码错误、私有视频失败、主错误叠加 trailer 错误、trailer/flush/协议 close 故障、停止与停止叠加 close 故障；2/4 路另覆盖全部分支失败、第二个队列初始化失败及首个分支线程创建失败。

空解码 EOF 和共享解码错误是测试 worker 的明确注入，不宣称实际损坏码流的所有 decoder 行为。初始化与 finalize 注入也只包装应用调用点，不改 FFmpeg 内部。输出通过真实 TCP 接收并独立解码；custom AVIO 失败场景使用实际本地路径，检查打开 FD 后的失败回收。

故障与资源包装器只链接进已有 `naki-live-hook-worker`，生产 worker/controller 不包含它们。启用本测试环境变量才记录计数或注入；既有 hook/API 测试继续使用该 helper 的原行为。包装器核对已取得的 codec（含转交 async 的 decoder）、graph、input/output format、AVIO 和 async 的归还；每个已建立 hook 关闭一次，成功 header 对应一次 trailer。它不是 SDK 内部所有 malloc/ref 的完整审计。

worker 已 join 媒体线程、发送 finished 前记录审计：线程数低于开始媒体时；FD 数不超过开始值加一个 logger 尚未封存的 trace FD；输入/本地输出文件没有遗留句柄。每例结束还等待已观察到的 worker PID 退出。任务状态核对队列/在途额度已清、high water 没有超过原配额、水印 active 引用和 invariant_error 为零。

成功输出检查完整 24 帧、显示 PTS 单调、packet DTS 严格递增且 DTS≤PTS，以及 AAC 原载荷按输入子序列排列。PTS、AAC 载荷顺序还进入前后对照。停止/共享取消可能原本返回 0 或 AVERROR_EXIT，不比较这一调度相关值；根任务注入主错误与稳定错误码仍精确检查。时间、PID、瞬时计数和失败前自然可写出的帧数不要求逐值相同，不要求编码文件字节一致。

## 改造前后对照

修改前保存的源码是 `out/batch3-lifecycle-f030348d/before/pipeline.c`。使用当前普通构建相同的编译/链接参数，仅替换 hook worker 的 pipeline 对象，其他应用对象、故障 fixture、native SDK 和输入相同。旧对象及 ELF、完整命令和哈希保存在 `baseline-6_1efnpt/before-build.json` 与构建日志中；不重新构建 native 库。

| 对照 | 结果位置 |
| --- | --- |
| 修改前 86 例全部通过 | `out/pipeline-lifecycle-_26_95rt/COMPLETE.json` |
| 修改后 86 例全部通过 | `out/pipeline-lifecycle-kc0_7vf2/COMPLETE.json` |
| 规范化的全部场景对照一致 | `out/batch3-lifecycle-f030348d/comparison.json`，different_cases 为空 |

输入 SHA-256 均为 `4bca9f27a4e36e9d53005d3f98cfef4d2087779692b6021423f8469667738d1e`。比较终态、稳定主错误/hook 错误、trailer/flush/close 结果，以及健康输出的解码帧数、PTS 与 AAC 载荷；原始终态、hook 和资源审计仍完整保留。没有将原有停止竞态改成新的状态语义。

后续 full 重放的严格比较出现一个差异：one-normal 的 AAC 从写出 33/丢弃 13 变为写出 31/丢弃 15，其余字段一致，新载荷序列是旧序列去掉前两包的后缀。原始差异保留在 `comparison-full.json`，没有改写成“完全一致”。full 本身的载荷顺序、解码与资源检查通过。

为区分原有调度行为，普通版本与旧对象各做三轮无暂停/已有 40ms 首视频暂停的交错重放，共额外 48 场景。旧版也观察到单输出写出/丢弃从 33/13 到 35/11 的变化；新版还有 31/15、34/12，2/4 路健康输出为 45/1。每条输出均验证：实际 AAC 载荷数 = audio_packets_out，且已写 + audio_dropped = hook 观察数 = 输入载荷数 46。证据为 `audio-timing-qqp_my2i/summary.json`，包括命令、ELF 哈希和各次新目录。这是既有输出 watermark 之前的迟到音频显式丢弃规则；没有扩大队列、退回时间轴或调整媒体逻辑来消除差异。

最终 `comparison-contract.json` 对 full、sanitize 与旧版分别核对全部 86 场景：生命周期、稳定错误及视频结果逐值一致；仅允许上述起始音频前缀的调度差异，并要求同一输入、原始载荷不变、顺序保持、丢失完全计数。full 有这一项前缀差异，sanitize 没有。初次精确一致、后续严格差异、重复验证和合同对照均保留，聚合索引不隐藏它们。

开发阶段的失败测试保留在其他 `out/pipeline-lifecycle-*`，包括错误地要求短时探测 codec 必须在 hook 后释放，以及把全部失败时的 cancelled 当作固定值的断言。首次 fixture 编译缺少 avformat 头文件等失败见本次执行记录；修正的是测试建模/编译问题，没有降低资源、错误码、独立解码或 sanitizer 门槛。

## 当前候选验证

本轮证据根为 `out/batch3-lifecycle-f030348d/`，可执行源码摘要为 `156a5493b092cad80e400b418949725eb8cf953a9719a8fccb0fd06f9a47b2fe`，范围沿用 [统一测试入口](testing.md)。

| 验证 | 结果与证据 |
| --- | --- |
| 普通应用构建与 SDK consumer gate | 通过；`build.log`，使用 `scripts/build-service.sh`，最多 2 个构建任务 |
| full | 34 个步骤全部通过，包含 15 项原生测试与新增 86 场景；`full/summary.json` |
| 重点媒体/控制/故障回归 | full 内的 VVC 短尾、B 帧/PTS、混合队列、水印更新、media hook、真实 HTTP close-only 故障、接管、健康、崩溃和独立符号均通过 |
| ASan/UBSan | 6 个步骤全部通过，包括 15 项原生测试、水印复审、多输出、86 场景与基本服务；`sanitize/summary.json`，独立 Debug 构建日志 `out/sanitizers-8NEaH5/build.log` |
| 前后与重复对照 | 旧、新 86 场景及额外 48 次正常 EOF 场景通过各自检查；精确/合同差异按上文分别保存 |
| 最终身份与范围 | `evidence.json`；full/sanitize 前后源码、所选二进制和 SDK manifest 未变，两入口源码摘要一致；最后再次核对当前源码/ELF，前一批 logger 与用户原文档字节不变 |

基于主仓 `a1b7e0584355756d0c5b13eaa6668d2a724d2a45` 加前三批未提交修改。SDK identity 仍为 `4c6d30a2595d52765a93db3b68ad2a5a0dfca6e6f1880d1a0607041f536a4e16`；FFmpeg/VVenC/yyjson 子模块 pin 不变。sanitizer 保持 leak 检查和遇错停止，没有降低告警或运行时门槛；独立库的插桩边界保持第一批约定。

本批不声称时延、吞吐或容量改善；不重跑历史性能矩阵。独立 native SDK 与系统库没有重新插桩，没有做 TSan、真实采集端、实际 Docker、远程 Actions 或目标机器长时验收。可信外部 HTTPS 水印成功样本仍由 `NAKI_TEST_HTTPS_URL` 单独配置，未配置必须记为 not_run。本轮不提交、推送或部署。
