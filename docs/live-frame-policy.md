# 直播抽帧、I 帧、PTS 与 SEI 策略草案（未实现）

这是对客户合同和后续实现的设计补充，不是 0.8.0 已有能力。当前接口仍以 [OpenAPI](openapi.json) 为准；本页字段不可发送到现有 `/v1/job`。管线整体结构见 [处理链草案](pipeline-design.md)。

## 已确认的业务语义

客户分别选择随源 I 帧、固定 2s GOP，或随源 I 帧且最大间隔 3s，不应把随源与固定周期默认叠加。这里“随源”是同一画面重新编码为 I 帧，不是复制输入压缩数据；是否需要 IDR/CRA/random-access 另行配置。源 PTS 自身出现超过 3s 的空洞允许保留，不补帧、不压缩时间轴，也不因此终止任务；空洞后首张可用画面若已越过期限，就强制为 I 帧。

过载抽帧是临时策略：按时延进入不同抽帧等级，性能恢复后退出，持续不恢复则失败相应分支或任务。PTS 可配置全帧保留，或只保护源 I 画面的 PTS。SEI 要保留已知非解码依赖类型和 UNKNOWN，不能仅使用一个已知 SEI 白名单就宣称满足合同。

SCTE-35 属于独立节目事件，范围限定 TS→TS；源信令和客户临时注入的安排见 [SCTE-35 草案](scte35-design.md)，不依附被抽帧的画面。

## 请求配置分组

将 frame_selection、intra、timestamps、sei 放在 rendition 的策略内，不作为 libavfilter 字符串选项。概念示例（数值仅说明结构，不是测得的生产阈值）：

```json
{
  "frame_selection": {
    "mode": "live_drop_only",
    "overload": {
      "signal": "oldest_inflight_ingress_age_ms",
      "levels": [
        {"enter_ms": 400, "sustain_ms": 1000, "keep_one_in": 2},
        {"enter_ms": 800, "sustain_ms": 1000, "keep_one_in": 3}
      ],
      "recover_below_ms": 200,
      "recover_sustain_ms": 3000,
      "max_continuous_degraded_ms": 20000,
      "on_exhausted": "fail_rendition"
    }
  },
  "intra": {
    "mode": "follow_source_with_max_gap",
    "max_gap_ms": 3000,
    "source_match": "decoded_intra_picture",
    "source_pts_holes": "allow",
    "refresh_type": "encoder_intra",
    "protect_from_drop": true
  },
  "timestamps": {
    "mode": "source_all",
    "unwrap": "source_program_clock",
    "missing_pts": "fail",
    "discontinuity": "fail",
    "output_precision": "container_quantized"
  },
  "sei": {
    "mode": "preserve_non_coding",
    "unknown": "preserve_payload",
    "dropped_picture": "protect_picture",
    "cross_codec": "require_explicit_mapping_or_opaque_contract",
    "on_overflow": "fail"
  }
}
```

正常降帧率与临时过载降级是两个独立来源，但只能由一个最终选择器合并决策，避免两个 drop 阶段相乘且不能解释。配置 `keep_one_in=3` 表示普通候选帧三留一；受保护 I/SEI 画面会额外保留，因此实际帧率可能高于名义目标。若要求硬上限帧率，同时要求保留所有受保护画面，配置校验和运行期都必须能报告不可满足。

## 到帧即决定的抽帧器

[FFmpeg fps 文档](https://ffmpeg.org/ffmpeg-filters.html#fps) 定义的是通过复制/丢弃生成 CFR。当前 n9 `vf_fps.c` 正常路径先缓存两帧，或等待 EOF，再决定下一时间格点的输出，并重写 PTS。这个行为与“直播临时丢帧，保留选中画面的原 PTS”是不同目标。

建议 app 增加有界、因果的 FrameSelector：每来一张完整解码画面就 decide keep/drop，无须等下一张；不做补帧，不生成 CFR 格点，不重编号保留画面的源 PTS。可使用稳定序号的 N 留 1，或按源 PTS 控制最高采样率，但速率变化不能引入等未来帧的缓存。原始帧抽掉后不再做该路昂贵的 scale/ROI/水印/编码，并及时释放元数据与引用。

抽帧位于完整解码之后，不能随意丢 H.264/HEVC 压缩参考包。它只能节省下游成本，不能凭此宣称降低了共享 decoder 的负载。存在去交错/降噪等依赖邻帧的 temporal filter 时，planner 要决定是否必须先完整供帧给公共时间域节点；不能未经验证就在其前面抽帧。

过载监视器必须在没有新视频输出时仍运行。只看已完成帧的 mean/p95 会漏掉已经卡住的任务，因此同时监视 oldest in-flight 的单调时钟年龄、各队列等待/占用、处理速率和 write-in-progress。阈值的起点是服务 ingress，不是源端采集时刻；源 PTS 差值与处理墙钟时延分开。输出网络阻塞也可能造成高时延，此时抽帧未必有用，应按原因进入写入超时/分支失败路径。

用 enter sustain、较低的 recover threshold 和 recovery sustain 避免抖动；等级切换不重置整个连续降级期限，只有持续健康恢复才复位。记录状态 normal/degraded/recovering/failed、原因、等级、生效时间、保留/丢弃/保护帧数及终止原因。所有丢帧都有唯一决策和计数，不创建等待 encoder 返回的 ledger 条目。被保留且已交给编码器的参考画面不再任意撤销。

## I 帧策略与 3B

| mode | 语义 |
| --- | --- |
| `follow_source` | 每张源 I 画面在输出仍是 I，不额外强制固定 2s 周期；启动画面仍要满足编码器随机访问要求 |
| `periodic_pts_grid` | 以共同起点 PTS+N×period 调度，例如 2s；不跟随源 I，边界取首张满足时间目标的可用画面 |
| `follow_source_with_max_gap` | 跟随源 I；每来一帧，若距上次计划的 I 已满 max_gap，则本帧也强制 I，随后重新计时 |

第三种伪代码是 `force_intra = source_is_I || first_picture || source_pts-last_planned_intra_pts >= max_gap`。强制决策要在抽帧之前完成，任何到期画面及源 I 均受保护。源 I 出现在 0s/1.2s/6s 时，deadline 是 1.2+3s；若 4.2s 有源画面就在该处插 I，6s 的源 I 仍保留。如果输入直接从 1.2s 跳到 6s，则不制造 4.2s 画面，在 6s 输出 I，允许这段源 PTS 空洞。

该规则是对每张实际可用源画面执行截止检查；VFR 的下一帧可能不恰好落在 3.000s，所以不得将它宣传成在没有画面的时间点也会生成 I。输入空洞与 selector 自己抽掉的帧分别记账；不能主动丢掉过期候选后再把它说成源空洞。PTS 小幅回退/重复或源时间轴重置也不是回绕或向前空洞，要另走配置的 discontinuity 策略。

planner 按显示 PTS 次序处理，强制请求需在编码器的 GOP/lookahead 决策之前送达；编码器接受请求后推进 planned I 状态，输出后另确认实际帧型、PTS 和原因。不能等网络写出才决定下一期限，否则编码器中已经排队的画面会误插 I。失败请求或实际未执行时明确失败，不只记录“已要求”。

用解码画面的 I 分类匹配，不能以 AV_PKT_FLAG_KEY 代替所有 I；普通 I 不一定是随机访问点。多个 slice/field 的输入还要明确完整画面的定义。若使用倍帧去交错，需定义源 I 对应哪一个子画面，不能默认让两个 field 输出都强制 I。`encoder_intra` 与 `idr/cra` 是不同合同，encoder adapter 必须声明支持情况。

完整重排组仍可为 I BBB P…；源 I/最大期限边界截断前组，允许 IP/IBP/IBBP。若客户同时要求任何位置都完整 3B，则与任意源 I 位置可能冲突，不能默默推迟源 I。当前 VVenC wrapper 未读取逐帧 AVFrame.pict_type，公开 vvencYUVBuffer 也没有逐帧 force-intra 字段；未来需要 fork 的显式逐帧控制入口，并修改 GOP/RC 调度，单纯设置 pict_type=I 不会完成接入。x264 wrapper 有对应映射，但仍应验证普通 I/IDR、短组与重排输出。

## PTS：保留媒体时间，不把丢帧变成快放

建议保存以下独立信息：原始 PTS/DTS（若可获取）、原始 time_base、回绕域/epoch、展开后的媒体时间、处理 frame_id、最终输出时间戳。耗时只使用单调时钟，不能用媒体 PTS 代替。

- `source_all`：每张保留画面的展示时刻保持源时间轴，仅做 time_base 换算及目标封装必要量化；抽帧留下空洞，播放速度和音频时间轴不被压缩。
- `source_i_anchors`：源 I 画面的 PTS 是硬锚点，其他画面允许按明确规则重新调时。不能承诺零前瞻、任意输入、精确 CFR、未来 I 硬锚点全部同时成立；新锚点若撞上已发出的 PTS，应明确报冲突，不能偷偷移动 I。实现第一阶段可先只开放 source_all。

源 TS 1/90000 → 输出 TS 1/90000 在适当条件下能保留刻度；输出 FLV 的主时间轴是毫秒，需要量化。配置必须区分“同一媒体时刻”和“相同整数刻度/线上原值”。若客户要求逐刻度精确相等而目标封装不可表达，应在校验中拒绝。量化使相邻保留帧同 PTS 时不能加 1 tick 还声称严格透传。B 帧的输出 DTS 由新的编码/参考重排结构产生，不能复制输入 DTS，也不能全部令 DTS=PTS。

源 PTS 不能用 packet 到达顺序简单判断回绕：B 帧会使 packet PTS 正常前后跳。TS 33-bit 90kHz 约每 26.5 小时回绕，应结合 DTS/程序时钟展开 epoch，再相对该 epoch 解释 PTS；音视频共享时钟域。输出按目标协议重新折回有限位宽。向前空洞允许保留；真正 discontinuity 的策略独立。不能把所有负差值都加一整圈。

当前 process_frame 优先使用 best_effort_timestamp，缺失时合成 PTS，编码/封装还有 time_base 转换，因此现有测试通过不等于线上 raw PTS 逐刻度保留。严格模式需要在通用推断/修正前记录源时间；若公共 API 已丢失原值，再增加窄范围、显式启用的 fork hook。先审查 [correct_ts_overflow、avoid_negative_ts、output_ts_offset](https://ffmpeg.org/ffmpeg-formats.html#Format-Options) 及各 muxer 的偏移，不全局删掉 FFmpeg 的时间戳处理；禁用其中一个选项不会自动关掉其他补算/平移。

当前 flvenc.put_timestamp 还屏蔽 extended timestamp 高位，长时间/跨回绕的实际线上行为需要专门验收；内部换成 int64_t 不代表输出文件/协议已经正确。缺失 PTS 的严格透传默认失败，允许合成时必须标记 provenance，不能把合成值当作原始值。保留时序不代表现有 VVenC DTS FIFO 已覆盖任意程度的抽帧，仍需新增稀疏/不规则 PTS 与强制 I 的组合回归。

## SEI：解码前提取，按画面关联，编码后重封装

不能只依赖 AV_FRAME_DATA_SEI_UNREGISTERED 等已知 side data：当前 H.264/HEVC decoder 会跳过未处理的 SEI，任意 UNKNOWN 不保证成为 AVFrame side data。建议解码前在已识别的 AU 边界提取，保存到有界、引用计数 MetadataBundle，经 opaque_ref/画面 id/分支谱系与编码 PTS ledger 关联，编码完成后、mux/格式转换前插入目标 AU。容器包不一定等于一张画面；prefix/suffix 与多个 AU/多消息的归属必须正确，不能简单按返回顺序贴标签。

保留 payloadType、payload 原字节、来源 codec、prefix/suffix、源 AU/frame id、源 PTS、层信息及消息顺序。Annex B 与 length-prefixed NAL 分别解析；处理 payloadType/payloadSize 的连续 255 编码、emulation prevention 和 RBSP 尾部。输出重新生成目标 codec 的 NAL 头、escape 和封装，不拷贝整个源 NAL。每 AU 消息数、payload bytes、在途总量及嵌套深度有上限；严格全保留合同下超限失败，不静默截断。无需把 payload 转 JSON 或交给 logger。

策略至少分三类：

1. **绑定原压缩结构/时序的 SEI**：buffering period、picture timing、recovery/参考相关、decoded picture hash 等，不能作为原字节无脑附加到新编码图像。由输出编码器生成或按合同移除；缩放/水印后原 decoded-picture hash 必然不再描述输出画面。
2. **可保留的业务/展示元数据**：按类型/业务语义转接。UUID user data 的 UUID 和内容原样保留；其他业务 SEI 包括 registered user data 也不能只按顶层类型一刀切，因为同类可能含字幕/HDR 等不同语义。HDR→SDR、裁剪、旋转、去交错后，相关颜色/几何/field 描述须更新/移除或判配置冲突，不能同时宣称语义正确和全部原样保留。
3. **UNKNOWN**：保存并进入显式 opaque-payload 策略，不因 FFmpeg 不认识就丢弃。[FFmpeg sei.h](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.2/libavcodec/sei.h) 说明类型号共享命名空间，但部分 payload 内容依赖编码标准；UNKNOWN 也不自动证明与解码无关。跨 H.264/HEVC→VVC 要明确客户是否要求同 payloadType+bytes、哪些输出位置/codec 可接受。若要求业务 payload 任意原样携带，可在双方约定的 opaque 模式下重封装；若改用带原类型信息的私有 user-data 容器，也必须双方同意，这不等于原 SEI 原类型透传。没有这份约定，不能宣称任意 UNKNOWN 的跨 codec 语义已保证。

SEI 与抽帧的关系必须显式配置：`protect_picture` 会保护带需透传 SEI 的画面，最接近保留原关联；`drop_with_picture` 只承诺保留选中画面的 SEI；`carry_to_next` 把业务事件带到后续画面但改变了 AU/时刻关联，不能套用到所有未知消息。源每帧都有 SEI 且要求 protect_picture 时可能完全抽不动，应进入持续过载失败策略，不能偷偷丢元数据。

流级持久状态、逐画面数据、一次性业务事件的生命周期不同；未知消息不能擅自去重、合并或重复到倍帧结果。消息迁移需保留原 PTS 供接收方解释，并设最大等待/队列限额；没有后续保留画面时，结束/失败如何处理要写进合同。编码器自己生成的 SEI 与透传 SEI 必须有唯一注入路径，避免 wrapper 与后处理重复注入。

## 后续实现和验证顺序

先做 Timeline/MetadataBundle/FrameSelector/IntraPolicy 四个模块，再分别适配编码器；scale/水印不负责业务抽帧或重新编号时间戳。每路日志新增 source AU/frame id、原/展开/输出 PTS、时间戳来源、丢帧原因、强制 I 原因和 SEI 提取/保留/迁移/拒绝计数，保留现有首帧/I/实际写出耗时边界。

必须新增：随源/固定周期/随源+3s 三种策略、普通非 key I、短 B 组、过载等级抖动/恢复/持续终止、受保护帧过多、原输入 PTS 空洞与主动丢帧区分、视频/音频时钟一致性、TS 回绕边界/B 帧乱序、FLV 长时刻度/量化/偏移、缺失与重复/回退 PTS、SEI UNKNOWN 多消息/长 payload/异常长度、跨 codec 重封装、HDR/裁剪冲突、丢帧后元数据关联，以及多路分别降级不串扰。像素相同/独立解码、画面 PTS/帧型和抽取后的 SEI bytes 均需验证，不能只看编码器调用成功或 metadata 计数。
