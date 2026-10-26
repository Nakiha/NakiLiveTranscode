# 编码分支进程隔离与动态输出（issue #4）

**开发验收中，最终验收未完成。服务仍是实验性 Linux 程序；不要把这里的本机功能结果当作生产容量或部署许可。**

本功能保留一个输入 reader、demux 和视频 decoder。每个 rendition 独立负责滤镜、编码、mux 和实际写出。创建任务时显式选择 `rendition_backend`：省略保留既有固定输出执行路径；`process` 为独立 exec 的编码分支；`thread` 为同一个媒体引擎、描述符、队列和预算的进程内对照。两种新后端都共享输入，线程后端没有 native 故障隔离能力。不能把多路各自打开输入的转码任务当成此处的后端对照。

```json
{
  "input": "input.ts",
  "rendition_backend": "process",
  "idle_timeout_ms": 60000,
  "rendition_auto_restart": false,
  "rendition_restart_limit": 3,
  "rendition_restart_backoff_ms": 1000,
  "outputs": [
    {"id": "preview", "output": "preview.mp4", "output_format": "mp4",
     "width": 640, "height": 360, "encoder": "libx264",
     "encoder_threads": 2, "audio_copy": false}
  ]
}
```

初始化参数固定在输入会话上。`decoder_async` 与 `decoder_early_output` 独立；`decoder_pump` 是异步解码后的调度泵，要求 async，不能把它当作 async 开关。新后端允许原先限制为单输出的 pump/codec_trace/输出抽帧与 VVenC 码率控制配置，通过同一输入传到各自 decoder 或 encoder owner；实际支持范围须以验收记录为准。

## 共享帧与退出证明

每个解码帧的像素只复制一次到会话的 memfd 池。producer 在加封印前建立 RW mapping，然后关闭 RW FD。消费者只得到 O_RDONLY FD；FUTURE_WRITE/GROW/SHRINK/SEAL 封印禁止新的共享可写映射、pwrite 和改变大小。只读 AVBuffer 包装保证需要写像素的滤镜/编码器走 COW。控制面 JSON 不含像素、AVFrame/AVBuffer 原生结构、指针或进程内引用计数。

媒体槽是独立版本协议：pool id、槽号与单调槽代次；帧协议 v2 的尺寸、像素格式、显式 plane offset/stride/length、源 PTS/时基、颜色/裁剪/SAR、输入序号、订阅代次、SEI 和解码 trace 字段。元数据采用有界、拒绝重复键的编码。只支持 planar YUV420 8-bit / 10-bit LE；其他输入像素格式以及未列入白名单的帧 side data 显式失败，不静默丢失。

每个 rendition 最多保留 32 个原始视频槽、16 个原始音频槽。共享视频池 129 槽，音频池 65 槽，始终预留 producer 槽。上限约 4GiB/640MiB 是稀疏虚拟映射预算，并非已提交物理内存；报告内存时必须用 PSS/private/shared，不能叠加各进程 RSS 得出复制倍数。

原始像素信用在最后一个本地像素引用销毁时归还，和进入队列、编码返回、mux 完成是不同边界。ACCEPTED 与 RELEASE 走独立 completion 通道；描述符匹配 session、rendition instance/global generation、pool/slot/generation，重复或过期 ACK 不释放新代次。

超时、控制断线和 finished 消息都不是退出证明。先停止新投递，有限 drain/停止请求，必要时 pidfd 发 SIGKILL，只有 pidfd 可读且 waitpid 确认退出后才能回收剩余槽。线程对照只有完整 join 并关闭所有 codec/logger 线程和映射后才回收；线程卡死只能隔离预算并最终停止整个 worker，不能安全强杀单个线程。无法证明退出时保留 quarantine，拒绝继续叠加持有者。

exec 子进程只继承 bootstrap 所需 FD。collector 在媒体映射前创建；随后在资源加载/logger/codec 线程启动前安装进程守卫。进程守卫允许 codec pthread，禁止 fork/vfork、非线程 clone、exec 及 sendmsg/sendmmsg，因此不能用 SCM_RIGHTS 导出池 FD。守卫不可用即准备失败。这是 native 错误的生命周期边界，不是恶意本机代码安全沙箱。v1 进程后端明确拒绝 SRT（sendmsg 限制）；使用线程/既有后端。没有自动降级成无守卫的“进程隔离”。当前没有每 rendition 的 cgroup CPU/memory/OOM 限额 API；仅继承 worker 的内核资源限制。传输预算与 native 崩溃隔离不构成 OOM 或 CPU 容量隔离承诺。

## 动态操作

POST `/v1/jobs/{job_id}/updates` 使用与组件更新一致的 operation id、指纹、重放和结果版本。拓扑操作使用全局 `expected_revision`；对存活分支的更新、目标分发和 RAP 请求还要求 `expected_generation`。版本冲突为 409，转换/预算忙为 429，能力不支持为 422；实际错误映射见 OpenAPI。重复 operation 先匹配原意图再检查当前 CAS，不能因后来的版本变化重复执行。

```json
{
  "operation_id": "add-preview-2",
  "action": "add_rendition",
  "expected_revision": 1,
  "target": {"component": "rendition", "output_id": "preview2"},
  "spec": {"id": "preview2", "output": "preview2.ts", "output_format": "mpegts"}
}
```

另有 `remove_rendition`、`restart_rendition`，无需 spec。添加先保留容量、准备独立 owner，再在共享 owner 的完整解码帧边界挂接；重新检查 source format generation 和 reader 的 audio/SEI 订阅屏障，不能把旧订阅入队的 B 帧漏给新输出。新编码器从当前完整解码帧起，不等待输入 I 帧。输出启动须经新的编码器初始化和合法码流头完成。`applied/attached` 只确认挂接；`delivery.first_written` 只在真实 write/flush 确认后出现。

删除先从拓扑摘除，记录最后接纳帧，再有限 drain、退出证明和释放预算。旧 holder 没退出时 ID 不能重用。重启保留同一逻辑 ID，分配全会话单调的新 generation；默认必须手动恢复。启用自动恢复后按有限次数、指数退避执行，每次都是可查询的独立操作；最多 10 次，backoff 配置范围 100..60000ms。人工恢复重置本轮自动重试预算。

本地文件使用独占创建，重启不覆盖故障前文件：后续 incarnation 写到 `<原路径>.g<generation>`。网络地址保持配置的 endpoint。完整 rendition 状态的 `actual_output` 给出本代次路径。删除文件或拼接这些片段属于调用方的显式操作。

最后一个逻辑 rendition 被删除后执行 [无输出保活](session-lifetime.md)。失败但尚未删除的 rendition 仍计入逻辑拓扑，不触发删除保活。

## 状态与恢复

GET `/v1/job` 的 `outputs` 是有界摘要，包含 rendition PID/generation、独立原始保留量、队列/实际写出、退出原因和恢复次数。GET `/v1/jobs/{job_id}/renditions/{output_id}` 返回当前 worker 缓存的完整分支状态，包括组件 desired/effective、完整首次出帧检查点和各 destination 状态。后者在会话 worker 退出后不可作为永久明细仓库；明细事件/trace 留在本轮唯一日志目录。

控制器丢失不停止媒体；新控制器先接回私有 worker endpoint，核对所有权并导出操作历史与实际状态，随后开放写操作。`readiness.ready` 可在任务运行时为 true，`admission.open` 因已有任务而为 false；恢复测试不能等 admission 再检查正在运行的任务。missing ACK 只有在退出证明后成为 uncertain，不能伪装为已知未写出。

## 同一编码器的多个目标

`add_destination` / `remove_destination` 的 target 使用 `component: destination`、`output_id`、`destination_id`；add 的 spec 严格为 `{output, output_format}`。`primary` 表示 rendition 创建时的原目标。`request_random_access` 是单独的 rendition 操作，不是普通强制 I 帧的别名。

热复用当前仅接受 libx264、没有 live-frame-intra 和 constrained_crf、且有完整不可变 SPS/PPS 的编码器；其他组合明确拒绝，调用方应添加独立 rendition。libvvenc 编码的是 VVC；其动态 RAP 契约本轮尚不支持。HEVC 输入仍可共享解码并编码为 AVC 后复用目标。不能把输入解码的“快出”与输出编码器的 RAP 契约混同，更不能用普通 I/KEY 冒充 IDR。

首次热复用后，每个目标由自己的 mux owner、压缩包队列和 I/O deadline 驱动；每目标最多 16 包、8MiB、10 秒入场年龄，I/O 单次 1 秒。新后端每个 live mux 另限 4 包；4 × (16 + 1 + 4) = 84 个压缩包引用，加 32 个原始帧 credit 和瞬时引用余量，低于分支 128 帧预算（复用要求 zerolatency AVC 无 B 帧/前瞻）。每个 rendition 最多 4 个目标，准备中、失败但未删除的目标也占容量。开启后目标间共享编码压缩包的只读引用；持续激活的目标不随意丢弃参考包。`packets_out`/传统时延统计作用域为 `primary_destination`，删除 primary 后保留历史；镜像或唯一剩余目标以 `destinations[]` 的视频包数、音频包数、AVIO 实际写字节及其目标日志为准。任一目标实际 flush 成功都可确认水印交付。单目标预算/写错误终止该目标，原始解码不等待它；目标原生故障仍属于所在 rendition 进程的故障域。

新目标先开 mux 及参数头，跳过接入前压缩包；只有实际 NAL 类型确认 H.264 IDR（NAL5）才开始媒体，不依赖 AV_PKT_FLAG_KEY。音频不早于该视频接入点，AAC/MP3 codec 参数仍使用不可变 snapshot，MP4 保持 video-only。MPEG-TS stream-id 元数据按标准的一字节传输；未知侧数据仍明确拒绝。保留原有音频时间戳策略：缺失或迟到并落后于已写 watermark 的包会丢弃并计数，不能借热加/重启倒退输出时钟。普通大 PES 聚合的 MP3 输入可产生大量这种丢弃；本轮 MP3 同步正例使用低延迟、逐帧 PES，不代表新增了 MP3 缺失时间戳修复。每目标有真实写出时间与帧序号事件。RAP 请求经 encoder owner 为选中帧请求 forced IDR，必须在返回的包中再验证，确认接纳和实际写出分别记录。目标删除必须 drain/join 后才可复用其 ID。

## 验证边界

热复用 destination v1 同样拒绝 SRT；需要 SRT 时，可另建独立 thread rendition 或使用既有任务路径。`codec_trace` 仍要求 native software HEVC 输入，AVC 输入明确拒绝这项 trace，不能因迁移到新后端扩大解码器能力范围。

容器应使用能回收孤儿子进程的 init（本仓库 Compose 已设置 `init: true`，直接 `docker run` 对应 `--init`）。本轮测试容器也启用了 init，不能把不能回收子进程的 `sleep` PID1 当作相同生命周期环境。

本机是 Apple Silicon 上的 x86_64 QEMU 系统虚拟机，容器使用真实 Linux 内核 pidfd、memfd、seccomp；不是 Rosetta syscall 模拟。它可验证生命周期、字节/帧序、故障与功能。吞吐和 CPU 只描述这个开发环境，不能外推原生 Linux x86_64 生产容量；没有部署或生产默认变更。

测试必须预先固定输入帧数/循环数，独立解码输出，按源帧 id/PTS 检查丢重与显示顺序。EOF、异常退出、零帧、比较失败都必须使进程非零退出。对照要同一 SDK/构建、输入、滤镜、质量、GOP/B、线程和 pacing；CPU 合计服务所有进程（含 collector），实际写出 P50/P95/P99 取每个完成帧。离线不限速灌入触发独立分支预算时应报告失败/过载，不能以剩余输出当性能通过。

当前验收证据、未完成项目、构建标识及数据摘要见 [本轮验证报告](rendition-acceptance-20261026.md)；本文描述接口与实现契约，不替代验收结论。

## 审计修复后的生命周期边界

修复与本轮 full、sanitizer、Compose 验证结果见 [2026-10-27 审计复核](validation-rendition-audit-20261027.md)。

初始输出和动态 destination 共用输出协议/封装校验；`file:` 等协议别名明确拒绝。本地路径始终独占创建，不能借热加目标截断已有录制。迁移 primary mux 时保留底层协议最初引用的中断对象地址，由新的唯一 mux owner 驱动期限，直至该 writer join 后才释放原对象。每个 destination 的 trailer、flush、close 错误均进入目标终态；仅在所有保留目标都失败时令 rendition 失败。

每路 `io_timeout_ms` 通过 rendition bootstrap 传递，适用于复用前的输出打开、写出和收尾；热复用后的各目标仍按既定 1 秒预算执行。required 水印的期限以本次 add/restart 操作接受时间计算；初始分支保留任务接受时间。会话追踪中的 HTTP-handler-to-mux 起点不变。准备上限至少 12 秒；required 资源预算与输出 I/O 预算之和超过该值时，额外保留 2 秒收尾/调度余量，最大 42 秒，不再提前截断合法的长资源预算。

异步 writer 的 ready 只是资源准备完成；媒体 owner 必须先提交 PREPARED，再在验证 IDR 时提交 APPLYING/APPLIED，确认实际写出后更新 delivery。状态提交失败不能继续伪装激活。共享视频/音频池优先复用最近的空闲槽，保留原 credit、代次与退出证明规则，避免串行无积压播放也逐渐触碰所有稀疏槽；这不等于进程 OOM 隔离。
