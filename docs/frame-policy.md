# 单路帧身份、严格时间轴、I 策略与显式过载

工作区已实现本页的单路子集；本地功能验收、独立复审修复与实际 SDK 身份以 [第二批验证记录](validation-frame-policy.md) 为准。当前尚未提交或更新父 gitlink，等待独立源码复审。既有请求不带下面三个配置组时保留原来的时间戳回退、编码器周期与不抽帧行为。服务仍是实验性 Linux 单视频转码；音频仅为既有有界 AAC/MP3 passthrough。

```json
{
  "input": "/data/input.mp4",
  "output": "/data/output.ts",
  "timestamps": {"mode": "source_all"},
  "intra": {"mode": "follow_source_with_max_gap", "max_gap_ms": 3000, "refresh_type": "encoder_intra"},
  "frame_selection": {
    "mode": "live_drop_only",
    "overload": {
      "signal": "oldest_inflight_ingress_age_ms",
      "levels": [{"enter_ms": 400, "sustain_ms": 1000, "keep_one_in": 2}],
      "recover_below_ms": 200,
      "recover_sustain_ms": 2000,
      "max_continuous_degraded_ms": 20000,
      "on_exhausted": "fail_task"
    }
  }
}
```

阈值只是配置结构示例，不是推荐生产阈值或实测容量。没有隐式过载默认值：`live_drop_only` 必须提供全部阈值，且显式选择 `source_all` 和 I 策略。`frame_selection.mode=all` 不接受 overload。嵌套未知键、重复键、NUL、类型错误、空/超过三级的 levels 均拒绝；进入阈值及抽帧因子逐级增大，恢复阈值必须为正且低于首级进入阈值。零恢复阈值被拒绝，因为年龄严格小于零永远不成立。单路任务耗尽后失败整个任务。

## 时间与身份

每个完整解码画面分配独立 `frame_id`，作用域为 job，当前不支持规格重建，所以 generation=1。保留 source packet sequence、demux 后包 PTS/DTS、解码原始 PTS、时间基、展开后 PTS 和 provenance，经引用计数 opaque_ref、filter、编码 PTS ledger 到实际 mux 完成。frame_id 不随抽帧重新编号；丢帧不创建等待编码器回包的 ledger 项。缺映射或编码 PTS 非单调明确失败。

`timestamps.mode=source_all` 对完整解码画面的 PTS 做严格检查：缺失、重复、回退失败，仅展开可识别的流时钟 modulo 回绕；demux 已展开的值继续保留。任意向前源空洞允许保留，不补帧、不压缩时间。输出只换时间基并接受容器量化；量化后不可单调时失败。包字段已经经过 demux，不是线上的原始字节时间。`legacy` 保持此前 best-effort / synthetic 回退并在逐帧记录中标明来源。本批不开放 source_i_anchors 或脏时间戳修复。

VVenC reference profile 的名义帧率时间基不足以实现严格 VFR 保留，因此 source_all 与 reference 的组合在 HTTP 配置阶段拒绝，即使没有提供 intra。legacy + reference 保持原有行为。最终容器精度在保留画面送入编码器之前按显示顺序检查，并只在编码器接受后推进；FLV 毫秒量化导致相邻保留画面同 PTS 时明确报 `strict output PTS precision collision`，不补 tick。B 帧导致编码/写出顺序的 packet PTS 回退是合法的，不据此判定显示时间轴失败。

严格模式禁用 mux 自动平移负时间。当前 FLV writer 的时间戳边界另有已知限制，严格模式对负值、超过 INT32_MAX 毫秒或超出有符号 24-bit composition offset 的视频包明确失败；不把本批称为任意长时钟/负预滚 FLV 透传。legacy 行为保持。MP4 仍只有视频，FLV/TS 音频仍使用原有单 mux owner 与水位丢弃规则。

## I 决策与实际确认

- `follow_source`：每张解码 I 画面重新编码为 I，不叠加固定周期；按画面类型识别，不能用输入 key 标志代替。
- `periodic_pts_grid`：必须显式给 `period_ms`，以首画面的源 PTS 为格点起点；取每个新时间格的首张实际画面，不额外跟随源 I。
- `follow_source_with_max_gap`：必须显式给 `max_gap_ms`，跟随源 I；到期后的首张实际画面也强制 I。源空洞没有画面时不生成帧。

决策在抽帧前完成，第一张、所有源 I 及到期 I 都受到保护。编码器接受后推进 I 截止状态，编码回包必须有实际 I 类型确认，否则失败。`encoder_intra` 要求普通 I；保留编码器启动所需的首张 IDR，后续不自动升级为 IDR/CRA。其他 refresh_type 明确拒绝。

匹配 fork 的 `live-frame-intra` 在 libx264 选择普通 I，并关闭自动 keyint/scenecut 调度；在 VVenC 显式传入逐画面请求，关闭其隐含 PTS-grid CRA。VVenC 保留既有 GOP4 / 3B 和 one-pass JSON lookahead RC；边界截断前组，允许短组。VVenC reference profile 不接受本批 I 策略。FFmpeg libavcodec 63.4.103 / `AVCODEC_LIVE_FRAME_INTRA_VERSION=1` 和 VVenC `VVENC_LIVE_FRAME_INTRA_VERSION=1` 必须与实际库匹配；默认旧编码路径不启用新选项。

x264 在无 B 帧时关闭 open-GOP，达到 min-keyint 的 I 会提升为 IDR。修改位于 FFmpeg fork 的 `libavcodec/libx264.c`：应用仅按显式策略开启该 wrapper 选项，wrapper 选择普通 I、禁用自动调度、设置足够大的 min-keyint，并在单次会话达到 `X264_KEYINT_MAX_INFINITE/2` 张输入画面前明确报范围错误。外部 x264 源码和系统库没有修改，也没有新增 B 帧来改变旧的重排行为。普通 I 已经通过实际 NAL 与独立解码验收，但有限会话上界不是长期运行能力或容量测量。H.264 recovery SEI 也可能使普通 I 的 decoder key 标志为真，不能据 key 标志判定 IDR。[x264 参数与帧型实现](https://raw.githubusercontent.com/mirror/x264/master/encoder/slicetype.c)。

## 过载与边界

worker 控制循环每约 50ms 独立检查有界在途记录，覆盖等待解码的包及完整解码画面，画面直到实际 write/flush 后才退休。原包无输出且最后引用释放时退休；decoded/frame 的记录按独立身份交接。记录总限额 512，超限明确失败。回调只做短锁和元数据释放，不做日志或 I/O；IPC 仍只有有界 JSON。

普通候选按稳定 frame_id 作 N 留 1，到帧即决定；不等待未来帧、不丢压缩参考包，不在昂贵的 scale/编码后补丢。受保护画面额外保留，因此不承诺硬输出帧率。升级不重置连续降级期限；只有低于恢复阈值并持续健康才回到 normal，持续不恢复则设置失败并通过已有取消机制中断媒体 I/O。控制器断开不关闭该监视循环。

同一次延迟轮询同时观察到恢复完成和连续耗尽时，比较它们各自的计划完成时刻：恢复必须严格早于降级截止，等时由耗尽优先，不能用较晚的恢复抹掉已到期失败。若恢复实际满足的计划时刻较早，即使本次轮询迟到，仍可按先发生的恢复结束该段降级。

50ms 轮询、系统调度和单次不可抢占库调用都影响响应时间；不是任意驱动下的硬实时终止保证。耗时从服务 ingress 到实际 mux/flush，不是 source-to-screen。现有每完成帧的时延采样、输入 I/首次 I/周期 I 与输出 key 分类、窗口边界和 logger 有界非阻塞语义保持。

状态增加 selected/dropped/protected、I requested/confirmed、frame_id、clock wraps 和 overload 状态、级别、恢复/升级及失败时写入阶段。逐帧日志记录身份、时间来源、强制 I 原因和实际输出类型；只有 trace 细节按采样配置输出，计数覆盖全部决策。
