# 组件边界加固验证（2026-10-28）

本轮基于 `bdd11ca` 工作树，修改应用组件边界，不修改 FFmpeg/VVenC 子模块、gitlink 或 SDK 构建配方。服务仍为实验性 Linux/WSL 程序。已实现的维护合同见[组件边界](architecture.md)。

## 修改范围

- 增加无需 SDK 的架构检查：组件依赖、相对/angle include、私有头使用者、传递状态泄漏、计算式 include、实现文件 include、输出 I/O 唯一所有者。检查接入默认构建、`naki-control` 构建依赖及 quick/full organization 套件；负向测试验证非法修改确实被拒绝。
- 输出三个实现编译成 `naki-media-output`。`Pipeline` 只保存不透明 `NakiMux *`；format context、FD、音轨/BSF、暂存音频、交织状态、水位及中断对象属于同一个稳定分配。
- 热复用 primary 通过显式的独占 move/rollback 移交同一对象，不再逐字段搬迁音轨、FD 或回调。新目标在线程启动前分配中断存储；writer 关闭输出并退休音频引用后，协调者必须 join 才能释放对象，防止收尾与取消竞争。
- 音频参数处理/注册/时钟逻辑从主调度文件移入输出实现；除不透明状态访问、分配和空状态处理外，核对其算法文本与原实现一致。原预算、丢弃计数、写出/flush 时延边界不变。
- rendition 接口不再包含完整 Job；frame wire codec 改为包含独立帧谱系合同，不再依赖 Pipeline/Job 私有状态。
- 控制库不再链接 FFmpeg。SHA256 改由 OpenSSL Crypto 提供；用原 AVSHA 实现编译的探针比对新旧指纹，并加入冻结兼容向量。控制源文件可在不传任何媒体 SDK include 路径的情况下编译；身份/指纹测试 ELF 的直接依赖只有 Crypto 与 libc。

## 最终验证窗口

环境为本机原生 x86_64 WSL2 Linux。应用/脚本/测试内容摘要为：

`0182bfdf1a2e9cb1102e2c1e2f107d3a51609d7c828dc9b0ac6bd55f014263d5`

SDK identity 保持：

`0147f7987263e5fec809390662f1e2db0f404970142db5ae373f154436374b65`

证据根为 `out/architecture-hardening-20261028/`。普通构建使用独立 `architecture-service` CMake 目录和该证据根的 `bin/`；通过 `scripts/build-service.sh` 构建，沿用经校验的 SDK。sanitizer 使用独立 Debug 构建，二进制位于 `out/sanitizers-VQxUeM/bin/`。

| 检查 | 结果与证据 |
| --- | --- |
| 普通构建与架构门禁 | 通过；`build-verified.log`、`build-final-2.log` |
| 最终原生 quick | 29/29；`native-final.log`，包含新的 `mux-lifetime` |
| 必需 full 检查覆盖 | 44/44，按同一源码和相同普通构建二进制分段完成；`coverage.json` 列出每项成功日志 |
| ASan/UBSan profile | 7/7；`sanitize-final/summary.json`，包含最终 29 项 native 及 rendition 功能门禁 |
| 控制库独立编译与指纹兼容 | organization、identity-intent 通过；`control-build-flags.txt`、`control-link-evidence.txt`、`fingerprint-before.txt`、`fingerprint-after.txt` |
| 补丁格式 | `git diff --check` 通过 |

full 原运行前 21 步通过后，`audio-signatures` 获取官方 HE-AAC 样本时发生外部 HTTPS 握手 EOF，尚未进入该 HE-AAC 正例。原始失败保留在 `full-final/summary.json`，该 profile **不是单次全通过**。通过既有 `NAKI_HE_AAC_FIXTURE` 入口提供本地历史样本副本，核对 199678 字节和固定 SHA-256 `7e7c0390c950eda21775d1a193e05ee703257bed85602884e7d30a8b3c65be90` 后，重跑失败套件及全部剩余套件；24 步（含重复 SDK 校验）在 `full-remainder/summary.json` 全部通过。

`coverage.json` 的汇总脚本核对两段成功项恰好覆盖全部 44 个必需检查、源码摘要一致、交集二进制哈希一致，以及各段源码/二进制/SDK manifest 在运行期间不变。sanitizer 的源码摘要相同，三段 SDK identity 相同。早期编译失败和为修正回收时序而人工中断的日志均保留，不计为最终通过。

最终覆盖包含 process/thread、动态增删、故障与接管、32 基础组合、2 高级组合及持续输入场景；还包含音频/SEI、真实迁移 socket、I/O 期限、trailer 错误、86 场景媒体生命周期、默认固定输出、帧/时钟策略、崩溃与符号检查。新 `mux-lifetime` 使用真实输出实现验证独占转移/失败回退、原 Pipeline 释放后的稳定回调、FD 关闭、音频引用恰好一次退休，以及退休后到 join 前仍可请求取消。

## 覆盖边界与剩余工作

唯一未配置的可选正例类型为可信 HTTPS 水印 fixture；本地 HTTPS 拒绝检查执行。ASan/UBSan 覆盖应用、yyjson 和 helper，原生 SDK/系统库未重新插桩；受进程守卫约束的媒体场景按既有规则关闭 LSan tracer，native 检查保留 LSan。没有 TSan 结论，也没有新的性能、容量、远端 CI 或部署验收声明。

本轮收紧资源边界和新增依赖的入口；`Pipeline`/`Job` 在枚举的私有实现中仍较宽，rendition 的正交状态标志尚未统一为完整的类型化转换模型。后续应继续按具体 owner 收敛这些状态，保留对应合同测试，不能把本轮通过解释为所有内部耦合已消除。
