# 进程事件文件（工程分支）

每个 controller/worker 在接单前建立自己的事件前缀：`<run-dir>/events/controller-<instance_id>.events` 或 `worker-<instance_id>.events`。预热、任务和控制接管都沿用该 worker 前缀。段名为 `.000001.jsonl.active`，正常封存后去掉 `.active`。事件不再经 IPC 转发到 controller stdout；launcher 的少量监管消息仍走 stdout，原生诊断走 stderr。逐帧 trace 使用独立的 `<job_id>.trace` 前缀，默认关闭。

进程只有一个 logger 线程。event/frame_trace 类别决定预留名额和丢失计数，与事件文件/trace 文件目的地分离。event_seq 在 logger 入队锁内、准入或丢弃前分配；成功入队顺序就是文件中的序号顺序，缺口须结合 dropped_status/io_errors 解读。trace 不消耗 event_seq。多 output 的 logger 统计只在根任务报告一次，并明确标记 process scope 和 instance_id。

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

此模块的原生构建和故障回归进行中，Filebeat 的实际采集验收尚未执行。
