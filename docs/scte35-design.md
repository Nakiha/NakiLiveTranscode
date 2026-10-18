# TS SCTE-35 透传与注入草案（未实现）

已确认业务范围只包括 **MPEG-TS 输入 → MPEG-TS 输出**（例如 SRT→SRT），不扩展到 RTMP/Enhanced FLV。需求包含源信号保留和客户运行中安排临时插入信号。SCTE-35 是节目事件，不归属于某张视频帧，也不属于任意 SEI 透传。当前服务及 API 不支持本页功能。

## 早期依赖审查的缺口（历史 pin）

以下审查针对历史 FFmpeg fork `f5113758`，不把其行号/pin 当作当前依赖身份。当前服务仍未实现 SCTE-35 API 或专用输出路径；当前 pin/支持范围见[文档索引](README.md)。

- mpegts.c 识别 `stream_type=0x86` 与节目 CUEI registration，使用 section filter 收到完整 section。scte_data_cb 给 AVPacket 填的 PTS/DTS 来自最近 PCR 估计，不是消息内部声明的拼接时间；初始化时没有 ts->pkt 还存在直接返回路径，需要专项验证首批信号是否丢失。
- mpegtsenc.c 没有 SCTE-35 专用 stream type/section 写出路径，普通未知 data 会落入 private data 路径；不能只 map 数据流就宣称完成合法 SCTE-35 TS 输出。
- app reader 只接受选定视频和 AAC/MP3 音频，其余流直接丢弃。live_mux API 1 的动态注册只接受音频，不能用它追加 SCTE。

补丁讨论可作为线索，例如 [FFmpeg SCTE-35 section 输出提案](https://ffmpeg.org/pipermail/ffmpeg-devel/2023-July/311543.html)，但提案存在不代表已在当前 pin 合入，不能直接声称可用。

## 数据与事件两层

底层保留完整 splice_info_section 原字节，包括未知 command/descriptor；上层为能识别的事件提取节目/PID、命令、事件 id、取消、持续时长和有效时间。原始字节与解析视图同时存在，解析器不认识 descriptor 不能导致整条消息被丢弃或重写成简化版 cue-out/cue-in。

验证 section 边界、table id、长度和 CRC，正确处理跨 TS 包、pointer field、continuity/discontinuity 及一个载荷内多个 section。解析使用有界长度，设置消息数/总字节/未来计划时长/每秒注入量上限。未实现的加密或命令只做有标记的原始保留，不谎报已经理解事件；需要重定时但无法安全改写的情况明确失败或按客户配置拒绝该事件。

有效节目时间需要解析消息内部 splice_time 与 pts_adjustment（33-bit 域），与到达时的 PCR/packet PTS 分开。无显式时间、immediate、取消和未来预告不是同一类事件。参考 [SCTE 信令标准目录](https://account.scte.org/standards/library/catalog/scte-35-1-digital-program-insertion-cueing-message-part-1-legacy-splice-based-and-time-based-signaling/) 与 [FFmpeg 的时间偏移处理提案](https://ffmpeg.org/pipermail/ffmpeg-devel/2023-July/311545.html)；实现前固定适用标准版本和验收向量。

## TS 输出与时间轴

任务启动配置 SCTE enabled 时，建议从最初 PMT 就预留稳定的输出 PID、正确 stream_type/CUEI signaling；即使长期没有 cue，也不让 A/V 交织等待。若以后开放运行中启用 SCTE，才扩展 fork 的专用 data-stream 注册，原连接上更新 PMT version，并保留已有 PID/PCR/counters。不能把 section 包成普通 PES，也不能用长表 section helper 错加通用表头或第二个 CRC。

节目事件用独立有界队列，由唯一 mux owner 输出，不成为“每轮必须有包”的音视频交织参与者。预告到达后按发送策略写出，不扣到内部 splice_time 才发送；同时记录收到、计划发送、实际写入/flush 和生效媒体时间。重复广播的 SCTE section 可能是有意提高可靠性，不能只按 event id 全局去重。事件状态表用于理解取消/更新，原始转发策略另行定义。

抽帧保持源 PTS 空洞时，cue 仍独立存活。若输出时刻只相对源时刻加常量偏移，应对相关有效时间做同一映射，必要时更新 pts_adjustment 并重算 CRC；只改 AVPacket.pts 不会修改 section 内容。若使用 I 锚点重定时等非线性映射，不能再用一个常量偏移解决全部 cue，须显式映射或拒绝不支持组合。输出音视频、PCR 和 cue 必须属于同一节目时钟域，使用相同回绕 epoch。

## 客户临时注入接口

建议新增未来接口 `POST /v2/jobs/{job_id}/events/scte35`，并有查询/取消入口。它不是已有 API。支持两种清晰分开的请求：

- 提供完整 binary section（HTTP 用 base64），指定何时把消息写入输出；不擅自把内部 splice 时间改成发送时刻。显式声明 section 已使用的时钟域。
- 提供结构化命令和目标生效媒体时间，由服务生成 section，再按照提前量发送。至少区分 splice_insert、time_signal+segmentation_descriptor；不能用一个 cue-out 布尔字段代表所有 SCTE-35。

概念示例：

```json
{
  "idempotency_key": "customer-break-20261017-001",
  "program": "main",
  "payload": {
    "format": "structured",
    "command": "splice_insert",
    "splice_event_id": 12001,
    "out_of_network": true,
    "program_splice": true
  },
  "effective_at": {
    "mode": "media_pts",
    "domain": "source_unwrapped",
    "value": 9000000,
    "time_base": "1/90000"
  },
  "announcement": {"lead_ms": 2000, "on_late": "emit_and_report"},
  "video_boundary": "signaling_only"
}
```

这是示意结构，不是完整 splice_insert 编码字段规范。API 接受的计划时刻使用展开后的媒体时间，避免每 33-bit 回绕一次就歧义。第一阶段可只开放 media_pts 和 immediate；若客户要 UTC 定时，要有明确的 UTC↔节目媒体时钟映射和误差报告，不能拿服务器 wall clock 或 PCR 数值直接当作 UTC。

**发送时刻与生效时刻分开**：提前 2s 发送的消息仍声明未来目标点；“现在插一条消息”和“现在发生拼接”是两个命令。晚到的预告按配置立即发送并报 late、拒绝，或显式改计划，不能悄悄改内部 target。输入源 PTS 空洞仍允许存在：若计划落在空洞，说明消息实际何时发送、目标是否未命中，不补视频、不回溯改已输出帧。不能将计划事件本身无限缓存到找得到画面为止。

任务 worker 持有计划和幂等表，HTTP 只经现有有界控制 IPC 提交；控制进程重启后可查询和接管同一份计划。限制幂等记录数量和保留窗口，ACK 区分 accepted/queued/written/failed/cancelled；written 只代表本地 mux/AVIO 写入，不是接收端确认，不能承诺网络 exactly-once。待发送计划可取消；已经发出的 splice 取消需要按协议发取消消息，不能只删除本地记录。源事件与手动事件 id 冲突要明确处理，不擅自替换客户的源 id。

## 信号与视频边界

`signaling_only` 只保留/注入 SCTE，不默认改 GOP，也不执行广告内容替换。可另外开放 `align_random_access`，提前向 IntraPolicy 安排指定 PTS 的可拼接边界，并让该画面免于抽帧；多 rendition 应使用同一个目标时间。普通 I 不自动等于干净拼接点，需要验证 IDR/CRA、leading picture、参考依赖和音频边界。已进入编码 lookahead 或已经写出的目标画面无法事后补改，迟到请求必须明确失败/报告。

原样信号、时间正确、切点正确、实际广告替换是四项不同验收，不能用“收到一包 data”代替。透传也不要求把计划事件绑定到被抽掉的某张图上：即使视频抽帧，节目事件仍独立按时处理。

## 验证范围

必须生成/保存新的独立测试向量，验证原始 section bytes、CRC、输出 PMT/PID 与 section 而非 PES、PTS adjustment 和回绕、重复/取消/未知 descriptor、晚加入 PID、初始化首批消息、源 PTS 空洞、抽帧期间不丢事件、提前发送和迟到处理、手动安排/查询/取消、相同幂等请求重试、HTTP 崩溃后计划继续、各路时钟映射与独立接收解析。SRT 只是承载 TS，不替代任何 SCTE 语义验证；不能以 ffprobe 看见 data stream 作为完成标准。
