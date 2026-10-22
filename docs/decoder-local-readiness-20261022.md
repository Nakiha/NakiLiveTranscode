# async 与 AVC/HEVC 快出：本地改进

**实验版，最终验收未完成，不建议生产启用。**

本轮只在本地开发分支工作，不等待远端 CI，不部署，也不修改默认开关。
应用基于 `16ed34c32a5d36ef55f8640f65c74f840fbbc045`，本地分支为
`development/decoder-local-readiness-20261022`。独立 FFmpeg 提交为
`487ae0cc1fd307037c7f81812592f3db785d747f`，已先提交，再更新应用 gitlink。
这些本轮提交尚未推送；以后发布时应先推 FFmpeg 依赖，再推应用。

## 已改进的内容

1. 可选 decoder pump 用独立的 nonblocking/CLOEXEC eventfd 接收通知。
   先消费通知，再检查工作；检查后、等待前到达的通知仍保持可读，消除原先
   cond_signal 竞态及其 2ms 轮询补救。生产者不取 pump mutex，也不等消费。
   读包、解码完成、音视频槽释放和 join 都显式通知。只为 pump 分配额外 fd，
   在 reader join 和 async_free 后销毁；申请失败及通知错误进入正常错误处理。
2. AVC 输出计划只有原 DPB 已选中帧时，直接走原 finalize_frame，省去临时帧
   和 staging FIFO。真正的额外前缀仍先准备所有引用，再提交 DPB 变化。
   此项降低分配开销，没有恢复 AVC 帧间并行，也没有放宽快出资格。
3. 完整服务回归增加 async+pump 的快出关/开两项，注册入口现为六种组合；
   每项均检查完整帧数、源 ID、显示顺序、实际输出可解码，以及在途停止/重启。
4. 增加只使用既有输入的本地时延对照工具，绑定声明帧数、源/二进制/实际加载
   库的哈希和 SDK 身份。固定线程、队列和编码设置，两轮颠倒测试顺序，分别
   报全帧与剔除首 32/尾 48 帧的精确 trace 分位数。记录失败和未完成项，
   不按实际输出帧数缩短预期。

完整 SDK 从新 gitlink 单独构建，身份为
`9129fe5057388eb0dac759371081c1fcd661cdaaaf2093029fa415cf8d56a994`；
正常和应用 ASan/UBSan 服务均通过消费者身份核验，没有替换旧 SDK。

## 已完成的本地检查

环境仍为本机 Colima Ubuntu 24.04 amd64/Rosetta、8 vCPU/12 GiB。

| 检查 | 结果 |
| --- | --- |
| AVC 小优化的完整 codec 矩阵 | 83/83；绑定旧 SDK 对照和新解码构建，见 FFmpeg 的 local-readiness 文档 |
| 最终完整服务 SDK 的 codec 矩阵 | 83/83；用新探针绑定实际服务 SDK，源输入和循环预期不变 |
| 正常专项入口 | selected 7/7：SDK consumer、native、infrastructure、codec-phases、entrypoints、audio-signatures、decoder-controls |
| 正常 native quick | 20/20；包含通知先于等待、4 个生产者共 40,000 次通知、计数器饱和及 fd 申请失败 |
| 应用 ASan/UBSan native quick | 20/20；完整 SDK 和系统库在此构建中未插桩 |
| 新 SDK 的完整服务组合 | 正常与应用消毒器各五组 × 六配置，共 60 项，完整 192 帧及在途停止/重启均通过 |

五组输入为 HRD AVC B3、B7、动态 B16、HEVC B7、无 HRD AVC B7。
六配置为 sync、async 固定线程、async+pump 固定线程，各自快出关/开。
功能矩阵固定解码上限 2、队列 1；没有把 decoder_pump 当作 async 开关。

功能结果为 `/build/local-readiness-functional-{normal,asan}-*`，专项入口为
应用 `out/local-readiness-selected-01`。完整 codec 结果和身份摘要保存在独立
开发 volume；媒体、日志、构建产物和环境配置不进入 Git。

最终完整 SDK 矩阵摘要 SHA256：
`ac1683449bb657e5470a89cc6d80e7510d9024e3de4955d0491cc0225faf0a30`，
原始目录为 `/build/local-readiness-final-codec83`。

## 本地时延对照结果

`local-readiness-bench-02` **60/60 通过**：五组既有 320×180/60fps 输入，
每组六配置 × 两轮；固定解码上限 4、编码线程 1、队列 1、auto 关闭，按原
帧率供包。每轮 192 帧，稳态窗口为输出第 33 至 144 帧，共 112 帧。

下表每格为 **平均 / P95 / P99，毫秒**。平均为两轮稳态均值的平均；
P95/P99 为两轮各自分位数的较大者，不是混合分位数。边界为服务 ingress
到实际 mux/flush，不含采集或播放器显示。前四种配置的 pump 均关闭。

| 输入 | sync，快出关 | sync，快出开 | async，快出关 | async，快出开 |
| --- | --- | --- | --- | --- |
| hrd-b3 | 87.5 / 138.6 / 139.4 | 71.5 / 106.4 / 106.9 | 37.9 / 88.9 / 90.0 | 22.1 / 57.0 / 58.0 |
| hrd-b7 | 87.4 / 204.7 / 205.8 | 75.4 / 173.8 / 175.0 | 37.7 / 155.4 / 157.1 | 25.7 / 123.1 / 125.2 |
| hrd-dynamic | 87.2 / 188.2 / 205.1 | 72.0 / 157.5 / 174.9 | 37.8 / 139.4 / 155.1 | 22.3 / 107.2 / 126.3 |
| hevc-b7 | 86.5 / 204.3 / 205.7 | 75.5 / 174.3 / 176.3 | 37.7 / 155.2 / 156.9 | 25.8 / 124.5 / 126.2 |
| h264-fallback | 87.1 / 204.7 / 206.4 | 86.4 / 204.0 / 205.7 | 36.9 / 155.0 / 156.9 | 37.0 / 155.5 / 156.6 |

async+pump 与 async 直接前端的快出开启结果如下（同样的平均 / P95 / P99）：

| 输入 | async，pump 关 | async，pump 开 |
| --- | --- | --- |
| hrd-b3 | 22.1 / 57.0 / 58.0 | 21.9 / 56.6 / 58.0 |
| hrd-b7 | 25.7 / 123.1 / 125.2 | 25.2 / 123.0 / 125.4 |
| hrd-dynamic | 22.3 / 107.2 / 126.3 | 22.5 / 106.3 / 125.1 |
| hevc-b7 | 25.8 / 124.5 / 126.2 | 25.5 / 124.3 / 125.0 |
| h264-fallback | 37.0 / 155.5 / 156.6 | 36.4 / 154.2 / 155.3 |

这些数据支持本次短样本下的时延差异，不是优化前/后的 A/B 测量，也不足以
量化本轮分配优化或 eventfd 替换的单独收益。pump 没有表现出一致的额外
尾延迟收益，因此本轮没有把它改为默认开启。无 HRD AVC 的快出开/关差异
不应解释为快出命中；该路径继续回退。CPU、持续吞吐和连续队列峰值未验收。

脱敏汇总及原始结果/计划的 SHA256 见
[机器可读汇总](decoder-local-readiness-results-20261022.json)。

时延工具的防误报检查：把 192 帧输入错误声明为 96 帧，进程 exit=1，
summary=failed，完成 0/4；强制 LD_LIBRARY_PATH 指向旧 SDK，虽然指定前缀
的 SDK 验证通过，仍因实际加载路径不匹配而预检 exit=1。分别保留于
`local-readiness-bench-bad-count-01` 与 `local-readiness-bench-wrong-sdk-01`，
不将负向用例误记为性能测试通过。

### 保留的失败

- macOS 主机直接运行入口自检时，Linux 平台门槛先于“缺少工具”断言触发；
  对应运行失败，容器内同套入口检查 14/14 通过。
- 新应用消毒器最初误用了 RelWithDebInfo，既有 `component_update.c` 在 GCC
  优化下触发 format-truncation 警告并因 Werror 失败。随后改用此前消毒器验证
  的 Debug 配置构建通过；没有关闭警告或改动该文件。
- 首次时延对照 `local-readiness-bench-01` 完成 32/60 后准入超时，整体失败。
  已保存的状态为 readiness=true、备用 worker ready，但 admission.reason 为
  `creation_retention_capacity`。这是 32 条创建结果保留上限的正常反压，
  并非解码卡死。工具修正为每组输入使用独立控制器、至多 24 次创建，不改
  30 分钟结果保留、容量或准入规则。原失败目录保持不变。

## 后续达到可用级别的关键工作

- **AVC 首要项仍是状态拆分。** 参考 DPB/MMCO/POC 依赖继续走原帧线程；
  待显示列表、水位及 HRD 时钟由一个按提交顺序推进的输出 owner 管理。
  完整 AU 结果必须携带不可变参数身份、错误状态及独立持有的帧引用。
  需要先解决非参考 B 帧寿命、恢复标记、DPB 复用、公共帧后处理、错误次序、
  分配失败和 flush 代次，再提前放行 setup。不能只把 finish_setup 挪早。
- **快出资格必须保持可证明。** HEVC 的 IDR_N_LP/连续整数 POC 与 AVC 的
  HRD/CFR 是不同证据。AVC 无声明、多 slice 等继续稳定回退；常见编码器和
  偶数 POC 不是可接受的通用合同。需要补实际快出命中/回退原因的观测，
  当前服务字段仅报告请求值。
- **async 接着验证调度收益。** 保持单 frontend owner、有限队列和及时完成
  通知，在单路和多路下核对自动目标与共享预算。分别测重建重叠和 setup/
  参考等待，不能用 active_workers 峰值代替 CPU 并行或吞吐。
- **本地性能门槛还不完整。** 需要长输入的 CPU/帧、持续吞吐、RSS/FD、
  队列深度/最老年龄趋势及服务 ingress 到实际 mux/flush 的 P95/P99。
  短 320×180 样本和 1Hz 左右的队列快照不能证明容量或连续峰值；本轮时延
  工具尚未测 CPU。最终独立复审及 TSan 也未完成。

这轮结果只证明列出的本地功能范围，既不引用 phase2 加固前性能作为新结果，
也不承诺普通 AVC 片源普遍快出或任意机器上的生产 SLO。
