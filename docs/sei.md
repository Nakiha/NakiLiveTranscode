# 按类型选择的 SEI 透传

实验性 Linux 服务支持 H.264/HEVC 输入到 H.264/VVC 输出的显式 SEI 选择；TS、MP4、FLV 沿用各自的既有视频封装路径。不新增 HEVC 编码、音频转码或 SCTE-35 能力。

```json
{
  "input": "/data/input.ts",
  "output": "/data/output.ts",
  "timestamps": {"mode": "source_all"},
  "sei": {"types": ["uuid", 100, 101]}
}
```

`uuid` 是数字 type 5 的别名。它不表示 payload 必须包含有效 UUID，甚至不要求达到 16 字节。所有选中消息的 payload 都按不透明字节携带：不验证其业务格式，不修正、不截断、不去重。空 payload、相同类型的多条消息及重复消息也保留。

列表最多 64 个唯一类型；数字范围 0..65535，`uuid` 与 5 同时出现算重复。未知字符串、负数、小数、布尔、null、重复/未知字段均拒绝。列表会排序归一化，用于任务创建意图指纹；`["uuid",100]` 与 `[100,5]` 表示相同选择。省略 `sei` 或 `types: []` 关闭本功能；`sei` 对象必须包含 `types`。这不是任意类型自动全保留开关。

顶层 `sei` 是输出模板默认值；每个 `outputs[]` 可用完整 `sei` 对象覆盖，空列表可显式关闭某路。启动时固定，当前 typed update 仍只支持水印，不能运行中修改 SEI 选择。

## 字节、画面和封装合同

- 保留选中消息的数字 type、解去 emulation-prevention 后的 payload 原始字节、消息顺序、重复次数及原画面关联。跨编码格式也不做 payload 语义转换。输出重新生成 NAL 头、连续 255 type/size 字段、RBSP trailing bits、escape 和目标封装；不保证原 SEI NAL 的字节相同。
- H.264 使用 SEI NAL；VVC 使用 prefix/suffix SEI NAL。HEVC suffix 在 VVC 仍为 suffix；转到 H.264 时映射到其 VCL 前的 SEI NAL，保留消息类型、payload 及顺序，不承诺相同源 NAL 位置。VVC suffix 在 EOS/EOB 之前写入。
- 解码前从完整 AU 提取并移除选中的源消息，避免 FFmpeg 解码器丢失 UNKNOWN、校验私有 type 5 或让 wrapper 再次自动注入。未选中的源消息仍经过既有解码路径，输出编码器仍可生成自己的 SEI。`types` 是源消息的保留选择，不是最终输出码流的 SEI 类型白名单。
- 提取结果存入无内嵌指针的有界 `opaque_ref` 尾部，随引用计数、写时复制、共享解码分支、scale/水印和编码 PTS ledger 传递。原生结构中的按值 Stamp 副本不独立拥有 payload；ledger/mux 继续持有完整 owner 引用，直到实际写出或清理。
- 编码后内部 hook 返回后，统一注入选中消息，再交给唯一 mux owner。私有 hook 不应重复注入同一源消息。PTS/DTS、duration、flags 等包属性不因 SEI 重封装而改变。源 PTS 时间轴的严格保留仍由显式 `timestamps.mode=source_all` 控制。

当一张画面带有该输出选中的至少一条消息时，抽帧器必须保留画面，包括空 payload。保护发生在 filter/编码之前，不把消息挪到下一画面。不同输出按自己的选择独立保护。若每帧都有选中消息，则过载策略不能靠抽帧缓解压力，继续服从既有持续过载终止、分支预算及 I/O 截止规则；“不允许抽帧”不代表失败、停止或进程崩溃后仍保证写出。

## 有界输入及失败

启用时要求 demux/parser 交付单层、单张完整画面的 AU，支持 Annex B 以及由 avcC/hvcC 声明的 1..4 字节 NAL 长度前缀。源 framing 来自初始化快照，不猜测形似起始码的长度字段。H.264 数据分区、MVC/SVC/辅助画面、多画面包、未绑定画面的 SEI-only 包、跨画面的 prefix，以及携带选中消息的隔行画面明确拒绝。HEVC 非零 layer 和运行中的 NEW_EXTRADATA 也明确拒绝。不是任意分片、任意多层或分场关联实现。

外层长度、NAL framing、SEI type/size 编码、escape 和 trailing bits 必须可安全解析；“不验证 UUID 格式”不允许越界或畸形外层长度。不会递归解析或修改嵌套 payload。

每个源 AU（所有输出选择的并集）最多保留 64 条消息、合计 65536 字节 payload。每个自包含 bundle 额外含固定消息描述表；整个引用大小计入 reader/分支已有 retained-byte 预算，输出编码包也计入既有 mux packet/byte 限额。现有 reader/async 队列、512 项在途跟踪、256 项编码 ledger、每分支在途帧/字节限制继续生效，不新增无界缓存。最多 512 项输入消息关联等待记录，成功解码时恰好领取一次；正常 EOF 尚有选中消息未关联画面时失败。

解析、消息/字节或关联预算超限属于共享输入失败；某路注入、编码或写出失败沿用该分支失败规则。停止和错误清理释放相应引用。状态提供 `sei_types`、`sei_extracted`、`sei_written`、`sei_protected_frames`；written 只在实际本地 write/flush 成功后累计，不是网络接收或播放确认。多输出根 extracted 是输入并集的提取数量，根 written/protected 是各输出之和，各路 extracted 是该路领取的选中消息数。日志/IPC 不携带 payload。

## 接收端语义与验收

跨 codec 保留 type/payload 是双方约定的 opaque 合同，不保证原 payload 符合目标编码标准。数值类型也可能定义为 timing/recovery/hash/HDR/几何信息；调用方须选择适用于新编码和画面处理的类型，本功能不会重算或改写这些内容。特别是非标准短 type 5，标准 VVC 解码器可能直接拒绝；这里遵照模板原样保留，接收端需要按业务约定处理。

`tests/sei_integration.py` 用独立 fixture 序列化源消息，从实际 TS/MP4/FLV 输出独立提取并逐消息比对类型、payload、顺序及画面 PTS。为区分私有 SEI 语义与视频编码正确性，独立解码探针移除 SEI NAL 后解码，核对全部画面和 PTS；此检查不能被描述为标准播放器能直接接受任意私有 SEI。覆盖 B 重排、PTS 偏移/空洞、短 UUID、重复消息、type 300/长 payload、prefix/suffix、缩放/水印、各路选择、过载保护、1–5 帧 VVC 尾组、controller 接管、停止和错误清理。
