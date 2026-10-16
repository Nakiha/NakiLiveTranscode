# Linux 服务完整链路验证：2026-10-16

环境：本机 WSL Ubuntu 24.04，GCC，系统 libevent `2.1.12-stable-9ubuntu2.2`、libcurl `8.5.0-2ubuntu10.16`、libx264 `0.164.3108`；JSON 子模块 yyjson `0.13.0` / `6447536015f3d600f3d65323b10976103b337ca7`。FFmpeg 自定义 API 2 固定为 `382492970c62d0c77ce0110d731d5ad78973b2b4`，未修改底层源码。

执行 `scripts/build-service.sh` 和 `python3 tests/service_integration.py`：

| 场景 | 结果 |
| --- | --- |
| H.264、3 B 帧、960×540 文件实时回放 → scale 640×360 → libx264 → MPEG-TS | 独立 ffprobe 确认 75 帧、3 秒，尺寸和编码正确 |
| HEVC、3 B 帧 → libx264 → MP4 | 30 帧、1.2 秒，正常排空和 trailer/seek |
| 输入包序号 → B 帧重排后的输出帧 | 用原始输入包 PTS 独立核对，逐帧关联一致 |
| 逐帧 JSONL | 时延有限且非负；各阶段合计与服务内部端到端一致；自动模式上下限可见 |
| UDP 输出 | 独立 socket 接收原始 TS，再由 ffprobe 确认 75 帧 |
| 实时 UDP 输入 → 完整转码 → 文件输出 | 持续写出至少 30 帧，停止后文件可解码 |
| HTTP 控制 | C/libcurl 客户端健康查询和提交；运行中 24 次并发健康请求；忙时 409 |
| 非法 JSON/字段/重复键、坏输入、已有输出 | 明确拒绝/failed；已有文件字节不变；服务仍可接收下一任务 |
| 运行中停止、无包 UDP 探测时停止、SIGTERM | 在测试期限内退出并回收任务线程 |

正常运行产物保留在本机 `out/service-test-wo3vfsp4/`，入口日志 `out/test-service.log`。

另以 `-fsanitize=address,undefined -fno-omit-frame-pointer` 编译应用与 yyjson，设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1`，使用 `NAKI_SERVICE_BIN=.../out/bin-asan` 重跑同一完整集成测试，通过。产物为 `out/service-test-wpqxefgy/`，入口日志 `out/test-service-asan.log`。该轮未对 FFmpeg、libevent、libcurl、libx264 系统/共享库重新插桩，也未进行 ThreadSanitizer 检查。

本记录验证功能、关联与资源生命周期，不是吞吐或生产时延承诺。逐帧 JSONL 同步写盘会影响实际耗时。HTTP/RTSP/RTMP/FLV 等虽在构建或配置中提供，本轮没有分别做外部服务器协议验收；音频、在线规格变化和多路转码不在本版范围内。
