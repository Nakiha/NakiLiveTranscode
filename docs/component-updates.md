# 有界组件更新与操作查询

实验性 Linux 服务支持 watermark 的 typed patch；编码器、输出 URL、尺寸、封装、输入和 outputs 集合不可热改。每个逻辑输出独立维护 desired/effective 配置及 revision，初始为 1；单路默认 ID 为 main，仍走原直接媒体路径。

## 请求与重试

`POST /v1/jobs/{job_id}/updates`：

```json
{
  "operation_id": "logo-opacity-2",
  "target": {"output_id": "main", "component": "watermark"},
  "expected_revision": 1,
  "patch": {"placement": {"opacity": 0.65, "offset": {"x": -24}}}
}
```

可修改 enabled、resource.uri/load_timeout_ms、placement.anchor/opacity、offset.x/y、size.width/height/fit。省略字段保留当前 **desired**，null、未知/重复键和 startup/update/animation 策略字段拒绝；required 不能禁用。准备失败保留已接受的 desired/revision 和旧 effective，后续 patch 从失败的 desired 合并。

worker 先按 job + operation ID 查原始规范化意图，再检查关闭、CAS 和预算。相同 ID/意图重试不重新加载资源或重启动画，也不要求旧 expected_revision 再匹配。不同 target、字段存在性、值、expected_revision 或 kind 返回 `409 operation_id_conflict`。create/update/stop 共用每 job 的 ID 域。相同 URI 的新操作仍可能刷新资源；URI 相同不代表内容不变。

request_id 只关联本次 HTTP 尝试，origin_request_id 保存首次接受 ID。连接丢失或超时后保留原 operation_id，通过响应中的 operation_url 或 `GET /v1/jobs/{job_id}/operations/{operation_id}` 查询、重试。

| HTTP | 含义 |
| --- | --- |
| 202 | worker 已接受，或匹配操作仍在应用/等待写出 |
| 200 | 查询成功，或重试返回已终结结果；仍须检查 outcome_known |
| 400 | JSON、类型、未知/重复字段或不可变能力字段错误 |
| 404 | job/output/保留操作不存在 |
| 409 | 新操作的任务已关闭、revision/意图冲突或合并配置不允许 |
| 429 | 命令或结果预算不足，拒绝发生在执行前 |
| 503 | worker 不可查询、恢复未完成或结果尚未确认 |

错误含稳定 error_code 和可读 error。最多一个 update/query RPC 在等待，确认等待上限 2 秒，停止使用独立控制/结果预算。这些数字是边界，不是吞吐量结论。

## 应用与写出

`accepted → preparing → prepared → applying → applied`，或 failed/superseded/cancelled。只有未被 owner 领取的候选可以 superseded；领取后由 owner 裁定。加载成功不代表 native compositor 成功。可选首次启用的 setup 失败可保留原裸流；其他 native 合成失败保留真实媒体错误，不承诺任意故障都无缝回退。

applied_checkpoint 是 owner 实际成功处理的首帧；delivery.first_written 是实际 mux 写出并完成既有 AVIO flush 边界的首个成功帧，各自带 frame_id/source_pts、时间基及输出 PTS。B 帧重排可让两者属于不同帧，也可让旧 revision 在新 revision 后写出。它们不是全局 revision 水位或播放器显示时间。

引用从不可变帧 Stamp 经 encoder ledger 转交 mux 包。只有 revision 已退休、全部 frame/encoder/ledger/packet/mux 引用闭合且从未成功写出，才能给出 not_written。已有 applied/first_written 事实不会被后来的任务错误覆盖；查询不依赖帧日志、采样或事件落盘。

## 恢复与保留

worker 每 job 共享最多 256 个 update 槽、2 MiB store 预算，另保留 create/stop 槽。desired/effective、owner、在途媒体及未闭合 delivery 阻止回收；完整结果至少保留 30 分钟，表满先拒绝。result_expires_at=null 不承诺墙钟过期时刻；无 pin/引用时可给 result_expires_monotonic_us，表示同一主机单调时钟下最早回收时间。

controller 每 job 缓存最多 256 条、静态内存小于 2 MiB，恢复另有同上限 staging 缓存；最多 32 个保留 job。新操作先预留缓存再发给 worker，缓存满可触发权威分页同步刷新回收资格。重连先发送一次 assignment/归属/能力，再逐页发送最多 8 个 operation，完成后才开放新写入。周期快照只投影当前 desired/effective；正常结束先导出结果，再允许 worker 退出。能力不匹配时关闭 admission，保留现有媒体。

worker 未上报最终结果就死亡时，旧 pending 不能证明失败或 not_written。查询返回 outcome_known=false、state=unknown、reason=operation_outcome_unknown 和 last_observed，并保留确定的 applied checkpoint。已经确认的 first_written 或应用终态仍可从 controller_history 查询。幸存 worker 重连后重新给权威结果。正常结束的缓存至少保留至任务终态后 30 分钟，但 controller 重启会丢失已退出 worker 的历史。这里没有持久化 journal 或永久去重，日志不是 WAL。

## CLI 与兼容入口

```sh
out/bin/naki-live-ctl create create-envelope.json
out/bin/naki-live-ctl job JOB_ID
out/bin/naki-live-ctl update JOB_ID update.json
out/bin/naki-live-ctl operation JOB_ID OPERATION_ID
out/bin/naki-live-ctl stop-job JOB_ID STOP_OPERATION_ID
```

旧命令保留。旧 watermark 仍要求 job_id、递增 revision，多路还要求 output_id；完整配置转为相同 operation 模型。其 startup timeout 不改变创建策略，required 必须与原策略一致。兼容操作 ID 由 controller 分配，旧请求不获得调用者自选幂等键保证，超时仍须核对原 revision。

验收入口为 tests/update_api_integration.py 和 tests/watermark_operation_test.c；当前源码绑定的完整回归与未验证项见[核心整理验证](validation-core-refactor.md)。接口文档本身不证明候选已部署或在目标机通过。

## Result ordering

Worker records carry a job-scoped result_version, incremented under the store mutex for every observable mutation, including frame leases, delivery, retirement and retention pins. The controller keeps the entire newest version; later receipt of an older RPC cannot replace a newer history export or refresh its receipt timestamp. This is a result sequence, not a media revision or wall clock. Explicit legacy rejection ACKs identify the job and operation so empty controller reservations can be released even after an HTTP timeout.

Private worker ready/owner_info handshakes require operation_result_version_api=1 before assignment or recovery is admitted. A live older worker without result versions remains visible with reconciliation/readiness/admission closed while its media continues; a controller does not accept its unversioned result stream as if it had the current ordering guarantee.
