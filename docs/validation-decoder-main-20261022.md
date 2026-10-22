# async、AVC/HEVC 快出：main 集成本机验收

**实验版，最终验收未完成，不建议生产启用。**

本次合入采用已确认的门槛：完成本机验证后进入服务 main，由后续目标机
试用核对容量和时延。合入不代表任意码流的快出保证、目标机 SLO 或完整
生产验收已经完成。没有部署服务或更改已有生产安装。

## 实现和依赖身份

- 应用功能来自 `c92dac4682134c853c3a44e4e7e068f77bb9d39f`。
  初次 main 集成 `a5024a593a71018cc79c26f463dc4b9969890864` 的后续改动是
  停止/重启检查、可指定次数的重放探针、CI 证据收集修复及技术说明；该次集成
  没有进一步修改 `app/` 下的 C/H。下述控制恢复跟进另行修改
  `app/controller/service_http.c`，不修改解码算法、默认值或 SDK pin。
- FFmpeg 精确 pin 为 `2673319b815fa6da6fccd0c3801e099b2e473066`。
  它包含加固后的 AVC 实现及修正后的测试判定。其 codec 源码仍与 phase3
  一致，不能将 phase2 加固前性能写成该版本的测量。
- 本机正常 SDK identity 为
  `abc4c05d75393cf97edc5996c6ff1ca48a67eb918e60512ba6ed3dfdd120a334`，
  消费者按 checkout 独立核验；没有跳过身份门槛。
- FFmpeg 的默认 master 为上游镜像，当前是 `8.0.git`；服务采用 n9.0.2
  定制历史。因此服务 main 固定上述独立提交，不把跨版本源码树覆盖到
  FFmpeg master。

## 功能合同

| 开关 | 默认与边界 |
| --- | --- |
| `decoder_async` | true；false 使用普通 send/receive，仍可使用 FFmpeg 帧线程 |
| `decoder_auto_threads` | true；false 固定 async 上限，同步路径不运行自动控制器 |
| `decoder_early_output` | 省略时 HEVC 开、AVC 关；与 async 独立，均为初始化配置 |
| `decoder_pump` | false；单直接输出的独立前台调度选项，要求 async=true |

HEVC 在合格 `IDR_N_LP` 起点后利用整数 POC 的连续前缀，缺口或不合格输入
回到原路径。AVC 不把常见的偶数 POC 或编码器名称当作证明；自动路径需要
HRD/CFR 等约束，无声明、多 slice 等继续回退。服务不声明未经核验的
`live_poc_verified_even` 合同。

AVC 加固路径仍推迟 frame setup，可能压缩帧间并行；本次没有实现状态拆分，
也不声称已解决吞吐代价。开关请求值不等于逐帧快出命中，当前状态字段仅报告
请求。吞吐不足时必须检查队列增长，不能只看更早吐出的个别帧。

## 本机检查窗口

环境为本机 macOS ARM 上的 Colima Ubuntu 24.04 amd64/Rosetta 开发容器。
初始为 4 vCPU/8 GiB；检查四输出准入超时后调整为 8 vCPU/12 GiB。
本次所有输入与结果均使用独立目录，原工作区、旧报告与旧日志保持。

| 检查 | 结果与范围 |
| --- | --- |
| 正常 Linux codec 矩阵 | 83/83；同一最终 SDK 以快出关闭作对照，81 项有序帧比较和 2 项在途关闭 |
| 应用 ASan/UBSan native quick | 20/20；生产 SDK 与系统库未在此构建中插桩 |
| 正常及应用 ASan/UBSan 四组合 | 五组各 192 帧输入，各四组合均通过；核对完整输出、计时合法性和状态 |
| 四组合停止/重启 | 正常及应用 ASan/UBSan 各五组输入；每组合先在解码进行中停止，再完整重启并核对 192 帧及有序源 ID |
| 长重放 | ASan/UBSan 后端探针：五组输入 × 六模式 × 100 轮 × 192 帧，共 3,000 轮、576,000 帧；全部通过，含每轮 drain/flush 及像素/PTS/源 ID/元数据比较 |
| 解码库 ASan/UBSan | 独立禁 asm 的完整 h264/hevc 解码构建；83/83，含逐帧比较及在途关闭 |
| paced 生命周期矩阵 | 86/86；单、双、四输出的正常/失败/停止/释放路径，生产队列限制不变 |
| 本机基础门槛 | selected 9/9；SDK consumer、native、基础设施、组织、入口、SDK identity/expected、发布绑定、多输出 |
| 本机媒体专项 | audio-signatures、frame-policy、decoder-controls、decoder、service、live-input 逐步通过；所属后续 selected 在 processes 失败，不将整体标为 passed |
| CI 收集器回归 | 13/13；数组 summary 原样保留、runner 引用保留、退出 0/7 原样传播、非法 summary 不丢原始证据 |
| 本机收尾门槛 | selected 4/4；SDK consumer、修复后的基础设施检查、health-progress、output-finalize |

五组输入为 HRD AVC B3、B7、动态 B16、HEVC B7 以及无 HRD AVC B7。
六模式为 sync/async-fixed/async-auto × 快出关/开。完整有效输入只允许快出
打开时的输出触发 `pkt_dts` 变化，其他已记录字段和像素保持一致；不将它
用于显示重定时。取消时仅检查有效、有序、无重复的已交付前缀，不要求两种
驻留策略在中断时具有相同前缀长度。

四组合的 in-flight stop/restart 已登记在 full 的 `decoder-controls` 套件，
探针可额外指定重放次数。新测试继续采用预先声明的帧数，拒绝零帧、少循环、
失败退出和覆盖既有结果；上一窗口的负向验证见
[解码开关验证](validation-decoder-controls-20261022.md)。

本机新证据根为 `out/main-readiness-20261022-01/`；容器内结果和 SDK 位于独立
开发 volume。媒体、日志、构建产物和环境代理配置均不进入 Git。

正常 codec 矩阵 summary SHA256：
`845b3efd032bd057a02ab0ab886c0024b77821e1574f1f7f9c4ee266701aee3e`。
解码库 ASan/UBSan 矩阵 summary SHA256：
`f57b17e2be02f5cf5780545a961a6f28df72457e9112be99fb5d1b3569a2d808`。

### 保留的失败：四输出不限速供包

最初 selected 20 步在 lifecycle-media 的 four-normal 失败；独立 normal
复查及 8 vCPU 复查仍触发相同边界。四分支初始化期间，各自的 video_count
准入压力窗口超过原有 100ms，最终 all-outputs-failed。审计中的引用、codec、
graph、IO 和 hook 均平衡；该记录不能写成通过，也不能据此承诺四输出容量。

为区分媒体功能与突发供包压力，生命周期工具新增显式 `--pace`，按原媒体
速率供包后 86 项通过。没有增加队列、延长生产时限或改变默认 full 入口的
不限速负载。该结果不覆盖不限速失败，失败记录分别保留于
`service-regression.log`、`lifecycle-normal-recheck.log` 和
`lifecycle-normal-8cpu.log`。目标机应再次验证不限速及稳态负载。

另外，本机 processes 的原生 SIGSEGV 断言未通过：Rosetta 环境观察到正常
exit_code=139、exit_signal=0，而断言要求原生 signal=SIGSEGV；后续控制恢复
检查还观察到恢复窗口内的 HTTP 503。对应 selected 保持 failed，不删除这些
记录，不把信号语义或控制恢复门槛放宽成通过。下述原生 Linux full 中这两套
检查均通过。本机其他未继续执行的步骤不从失败的 selected 推断为通过。

## 远端检查和目标机后续验收

[原生 Linux 候选 CI](https://github.com/Nakiha/NakiLiveTranscode/actions/runs/39748507011)
绑定最初候选 c92dac4：精确 SDK 构建/发布、镜像构建及 full 43/43 步均通过，
包含原始不限速生命周期、processes、控制恢复、崩溃及符号检查；未配置的
外部可信 HTTPS 成功样例仍显式 not_run。**该次 CI job 整体失败**：full
完成后的 `collect-test-evidence.py` 将解码 suite 的数组 summary 当对象读取，
抛出 AttributeError，后续应用消毒器、Docker smoke、最终打包均未运行。

此次修复将数组 summary 作为不解释的原始证据收集，仅对 runner 对象提取
steps/artifacts；保留非法引用、symlink、未封口 trace 和容量边界检查。新增
测试先复现修复前 AttributeError，再验证全部基础设施测试通过。新 main 的
CI 状态按其精确提交另行读取，不能将旧 run 或本机检查写成新提交 CI passed。
SDK 已归档在[精确依赖发布](https://github.com/Nakiha/NakiLiveTranscode/releases/tag/ffmpeg-linux-amd64-5306ab531bcb2e7af9fd1efe)，
key 为 `5306ab531bcb2e7af9fd1efe`。本机模拟执行不用于容量结论。

### main CI 的控制恢复跟进

[初次 main CI](https://github.com/Nakiha/NakiLiveTranscode/actions/runs/39751904660)
绑定 a5024a5：复用精确 SDK、镜像构建成功，full 前 38 步通过，第 39 步
control-lifecycle 在恢复后重放已接受的创建请求时得到 HTTP 503，整体失败。
修复后的证据收集器成功归档该失败，未继续消毒器、smoke 或最终打包。

原因是创建入口先检查所有 worker 是否完成恢复，才查找已有 operation_id。
即使活动任务已恢复，只要备用 worker 尚在恢复，读取已确认结果也被拒绝。
跟进改动先核对保留记录及 fingerprint：相同意图返回已有结果，冲突返回
409；没有记录的新任务仍经过所有恢复、冲突和准入检查，恢复期间继续 503。
关闭服务、stop/update 和健康状态的门槛保持不变。

回归主动 SIGSTOP 一个备用 worker，再重启控制器，在 readiness=false 且
recovering>0 的窗口中同时核对已有创建重放成功、新创建被拒绝和指纹冲突。
该定向用例先在旧代码上稳定复现 503。测试对需要新控制操作的步骤显式等待
readiness，对新任务等待 admission，而不把 assignment 已恢复或创建 ACK
误当成操作历史和整个池都已恢复。恢复关闭期间的负向断言保留。

跟进本机结果：正常构建 control-lifecycle 7/7；注册入口 selected 3/3
（SDK consumer、infrastructure 13 项及 control-lifecycle）；应用 ASan/UBSan
下 acceptance-exit 与上述定向 targets-and-recovery 两项均通过。尝试把整个
control-lifecycle 直接放进应用消毒器构建时，transient-recovery 的故障注入
LD_PRELOAD 因 ASan runtime 排序检查退出，保留失败记录；不关闭该检查，
不把未完成的完整消毒器运行算通过。该 full 专用套件与正式 sanitize 入口的
覆盖范围保持分开。新提交的原生 CI 结果另行按提交读取。

目标机仍需比较相同构建、相同固定线程/队列下的四组合：真实输入的 B3/B7/
动态 B、持续吞吐、CPU/帧、RSS/FD、队列深度及最老帧年龄、首帧启动、服务
入队到实际 mux/flush 的 P95/P99，以及断流/恢复和长时运行。该边界不含源端
采集或播放器显示。当前没有这些目标机指标的验收值，没有 TSan 或最终独立
复审结论。
