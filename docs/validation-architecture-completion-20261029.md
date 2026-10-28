# 边界收口完成验证（2026-10-29）

本轮继续完成[架构清单](architecture-completion.md)，实现合同见[组件边界](architecture.md)。应用仍是实验性 Linux/WSL C 服务，媒体能力与既有预算不变。FFmpeg、VVenC、yyjson 的子模块指针及源码未修改，未迁移或覆盖历史性能证据。

## 已落地的维护边界

- `Job` 在 worker 之外不透明；媒体只能借用不可变配置/身份并发布有类型的事件。工厂、远端投影、逐帧统计、窗口和最终分类由 worker 持有。
- 输入/解码、滤镜/编码、mux 分别拥有私有状态和构建目标；固定分支的队列/线程也不再暴露给主调度。关闭顺序和 async 转交、EAGAIN、谱系、PTS ledger、实际 flush 结算边界保留。
- mux 保持稳定分配和唯一 I/O owner；目的地使用深拷贝并封存的编码参数与标量 stream timing，不借用活跃 encoder 或 AVStream。对象回收要求成功 join。
- 分支的逻辑成员、holder、握手、停止策略、结果及拓扑阶段通过有校验的转换推进；目的地的原子执行状态与接入状态分开。失败/完成不被迟到 ready 覆盖，finished/done 不作为退出证明。
- 默认构建和 organization 检查约束私有头、组件源集合、native I/O/API owner、事件发布权限及不透明状态访问。负例验证绕过确实被拒绝。`app/media/AGENTS.md`、`app/worker/AGENTS.md` 和测试范围表说明后续扩展入口。

`media-lifecycle` 不依赖媒体 SDK，遍历 454 个可达分支状态、8,172 次事件尝试，并检查拓扑屏障、世代/迟到事件、失败候选退休、IDR 接入、join 和 writer 终态并发。真实媒体回归继续验证这些模型与 adapter 的连接。

## 证据窗口

证据根：`out/architecture-completion-20261028/`。普通构建通过 `scripts/build-service.sh` 初始化独立 CMake 目录，后续在同目录构建；sanitizer 使用 `scripts/test-sanitizers.sh`，产物在 `out/sanitizers-VYwsg1/bin/`。均使用已校验的同一原生 SDK：

`0147f7987263e5fec809390662f1e2db0f404970142db5ae373f154436374b65`

| 检查 | 结果与证据 |
| --- | --- |
| 最终应用实现的普通构建 | 无警告；`build-complete.log` |
| 全部原生检查 | 33/33（31 quick 合同及两项 native integration helper）；`native-complete.log` |
| quick profile | 11/11；`quick-complete/summary.json` |
| full profile | 单次 44/44；`full-complete/summary.json` |
| 原 sanitizer profile | 前六项通过，最后 service 的 HTTP 断言失败；保留 `sanitize-complete/summary.json` |
| 修正测试后的 sanitizer service | SDK 校验与整个 service 检查 2/2；`sanitize-service-fixed/summary.json` |
| 修正测试后的普通定向复验 | SDK、31 native quick、organization、service、control-lifecycle、health-progress 六步通过；`readiness-contracts/summary.json` |

full/quick 与原 sanitizer 的应用、脚本、测试摘要：

`db2463d3c9aa25c1651ae31eed4e631019953c6fd41f37b6be994327f8c986c2`

最终测试修正后的摘要：

`0dc9ffcc36de58eaa630e6071453f986ab39e61da696e0e4e5a97d564209af6d`

两个窗口之间**只修改 `tests/integration/service_integration.py`**：202 代表 worker 接收任务，控制器仍可能同步 operation 历史，此时新的无关创建按原合同返回 503。测试原先立即要求 409，sanitizer 调度暴露了该竞争。现在先有界等待 `/health` 明确 ready 且 admission 为 busy，再严格断言 409；不重试失败的 409 断言，不放宽生产恢复门禁。应用代码和构建配方未变。

`completion-coverage.json` 由 `verify-completion-final.py` 逐文件比较两个窗口，核对唯一变化路径、当前源码摘要、普通构建各窗口之间及 sanitizer 各窗口之间的共同二进制哈希、SDK 身份/清单，以及各运行期间 source/binaries/SDK manifest 不变。它验证 full 的 44 项覆盖、sanitizer 前六项与修正后 service 合并后的七项覆盖。**不把原 sanitizer 失败写成单次通过，也不声称最终测试脚本版本单次跑完 full**；测试修正后的实际相关合同已定向重跑。

早期开发构建、聚焦验证和人工中断的 `full-life-final` 原始记录保留，不计入最终通过。Job 收口及输入/视频 owner 拆分各阶段的额外回归见架构清单；本报告以最终应用实现的窗口为准。

## 验证边界

唯一未配置的可选正例类型为外部可信 HTTPS 水印 fixture；本地 HTTPS 拒绝检查已执行。ASan/UBSan 覆盖应用、yyjson 与 helper，SDK/系统库未重新插桩；受 holder guard 限制的媒体进程按既有规则关闭 LSan tracer，原生 quick 保留 LSan。故意 fatal/assertion 和完整组合/持续输入场景在普通 full 中执行。

没有 TSan、远端 CI、Docker 部署或生产可用性结论，也未把这次回归中的时延数据当作新容量/性能结论。后续局部修改按 owner 选择合同测试；跨所有权、协议生命周期或共享引用变化仍需对应真实媒体与 sanitizer 回归。
