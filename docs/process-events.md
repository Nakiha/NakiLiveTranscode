# 进程事件文件（工程分支）

每个 controller/worker 在接单前建立自己的事件前缀：`<run-dir>/events/controller-<instance_id>.events` 或 `worker-<instance_id>.events`。预热、任务和控制接管都沿用该 worker 前缀。段名为 `.000001.jsonl.active`，正常封存后去掉 `.active`。事件不再经 IPC 转发到 controller stdout；launcher 的少量监管消息仍走 stdout，原生诊断走 stderr。逐帧 trace 使用独立的 `<job_id>.trace` 前缀，默认关闭。

进程只有一个 logger 线程。event/frame_trace 类别决定预留名额和丢失计数，与事件文件/trace 文件目的地分离。event_seq 在 logger 入队锁内、准入或丢弃前分配；锁内同时预留 FIFO 位置，锁外添加 envelope、脱敏、序列化并释放输入文档，再短暂加锁发布结果。logger 不越过尚在准备的队头，文件中的事件序号保持递增；被拒绝或写出失败的缺口须结合 dropped_status/io_errors 解读。trace 不消耗 event_seq。多 output 的 logger 统计只在根任务报告一次，并明确标记 process scope 和 instance_id。

队列仍最多 8192 项，frame_trace 留出 16 个控制事件名额；queued 包含准备占位与正在写出的一项，最大 capacity+1，high_water 只计队列。并发准备另限 32 项，其中 trace 最多 16 项，保留事件准备余量；超限立即按类别计丢失，不做 JSON 序列化。单条 JSON 仍限 65536 字节（不含换行），排队文本按实际大小分配。序列化 scratch 的单缓冲上限为 1 MiB，realloc 搬移时最多同时存在旧、新两个缓冲；输入遍历最多深度 24、65536 个值（包括对象键）、256 KiB 字符串/raw 总字节，超过预算或准备失败也只计一次丢失。上述是 logger 的工作与队列边界，不是包含调用方预建 yyjson 文档、分配器及线程栈的进程 RSS 上限。

TraceSink 的 pending 在占位时增加，覆盖锁外准备、排队和写出/丢弃；finish/release 不会提前回收它。准备完成时若 sink 已结束或 logger 已开始停机，该占位转为丢弃，由 logger 按原顺序回收并解除 pending，不再次增加丢失数。生产者不等待日志 I/O，但为保序，慢准备会推迟后续记录写出；logger 等待期间仍周期 flush/轮转已有文件。调用方须在释放 sink 或启动 logger_stop 前停止新提交，且让已占位的准备正常返回；这里没有新增线程取消、抢占或媒体生命周期协议。具体回归与测量边界见 [日志提交验证](validation-logger-submit.md)。

事件默认按 16 MiB 或 1 小时轮转；trace 保留原 64 MiB 或 1 小时配置。正常 I/O 时 logger 每约 100 ms 检查 flush/轮转，即使 trace 队列持续繁忙也会检查事件文件。该可见性目标不承诺磁盘卡死时的硬实时性。事件文件初始打开失败会使进程在 ready/admission 前退出；运行期故障计数并限流 stderr 诊断，媒体 producer 不等待日志 I/O。

每个前缀持有 `.events.lock` 生命周期锁，内容绑定同一 instance_id。维护器只在成功非阻塞取得这个现存锁并验证标记后处理遗留 active；PID 不存在、文件旧、文件名包含 PID 都不能代替锁。恢复仅发布通过 envelope/序号/JSON 校验的完整行，损坏原件放入不匹配采集路径的 `.damaged-*` 隔离文件。超过 64 MiB 的异常遗留文件保留供人工诊断。旧 trace active 继续保守保留。

维护命令必须显式区分类型和目录：

```sh
python3 scripts/maintain-logs.py --directory out/runs/events --kind events --recover-orphans
python3 scripts/maintain-logs.py --directory out/runs --kind trace
```

events 首版不 gzip；events 和 trace 各自默认 age 7 天、sealed 总额 2 GiB，size 策略可能更早删除。active、隔离段、临时文件和 crash 不包含在 sealed 配额内，整个 run 卷仍需额外空间告警。不要把封存或保留时长当成已采集证明。

[Filebeat 示例](../examples/filebeat-process-events.yml) 只采集 events 的 active/sealed 两种明确路径，不采 trace 或隔离文件。保留现有部署的 input 类型，持久化 registry，避免 file/stdout 双采。示例只提升稳定白名单字段，其余内容收敛到 details_json 字符串；输出、映射、版本兼容、离线补采容量及 ignore/clean 参数须在实际采集环境验证，当前没有替用户部署或证明补采 SLA。事件中的网络 URI 会移除用户信息、路径及查询内容；API 状态仍是受本地控制接口保护的业务查询。

首帧 checkpoint 继续保存在 worker 状态中，接管查询可读；本地 first_frame 事件只写一次，不因 controller 重连重放。controller 对 worker snapshot 的日志使用新的 controller envelope，原观察放在 snapshot 中，记录接收和观察时刻。crash collector 从正常初始化参数接收 instance_id，不改变固定 fatal record，也不在信号处理器中生成 ID、分配内存或调用 logger。

本机日志回归和测量结果见 [日志提交验证](validation-logger-submit.md)；Filebeat 的实际采集验收尚未执行。
