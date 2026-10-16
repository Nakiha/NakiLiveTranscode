# 0.3.0 进程池与接管验证

日期：2026-10-16，Asia/Hong_Kong。环境：Windows 宿主机、WSL Ubuntu 24.04、Docker Desktop Linux engine。保持 FFmpeg/yyjson 子模块不变；构建应用和 worker 的 RelWithDebInfo 版本，另构建应用层 ASan/UBSan 版本。没有重跑冻结的解码器基准，也没有修改宿主机 core_pattern。

## 验证范围与证据

| 检查 | 结果 | 本机新生成证据目录（不上传） |
| --- | --- | --- |
| 完整单路转码与 API | 通过：75 帧 H.264 B 帧输入、HEVC、缩放、文件/UDP 输出、坏输入、排他输出、取消、静默 UDP 停止及 SIGTERM | `out/service-test-5pi9d9w3` |
| 进程池与崩溃/接管 | 通过：ready 选择、立即补位、单路 admission、重复管理者拒绝、SIGSEGV 隔离及 worker_crash 日志、心跳超时、强制停止、待命崩溃退避补位 | `out/service-test-asn0a3ys` |
| 控制失联期间持续媒体 | 通过：真实 UDP 输入转本地 MPEG-TS，控制进程 SIGKILL，失联 2.5 秒（超过测试心跳期限），输出文件继续增长；重启后原 job 和 worker PID 保持，帧计数增加，可停止并确认退出 | 同上 |
| 离线完成 | 通过：worker 在无控制端期间完成 25 帧，保留最终状态；新控制端接管确认后退出，没有重播任务 | 同上 |
| 首帧链路 | 通过：默认 frame_log 关闭仍记录 first_frame；请求 ID 关联、各阶段单调顺序和总耗时一致；接管后重放首帧 checkpoint 并标记 | 同上 |
| 原日志与网络观测 | 通过：非重叠窗口、I 帧分类、TCP/HTTP、UDP 后台 TX 与直接 RX、阻塞写出时的独立汇总/HTTP、日志队列饱和、轮转/压缩/清理、默认 10 秒 heartbeat | `out/service-test-ncy5khgu` |
| ASan/UBSan 进程测试 | 通过；有意注入 SIGSEGV 时使用 handle_segv=0，使测试可核对真实退出信号 | `out/service-test-p9bvk25f` |
| ASan/UBSan 完整链路 | 通过，开启 leak 检测和遇错即停 | `out/service-test-y4z9we32` |
| Docker/Compose | 通过：非 root、健康检查、75 帧独立解码、共享卷压缩、容器内 HTTP 被杀后监管入口重启并接管同 PID worker、恢复输出完整 75 帧、容器 RestartCount=0、SIGTERM exit=0 | `out/docker-smoke-1kkoanp4` |

本次 Docker 镜像 ID：`sha256:65e8aee4e247b789d8960a40f2f2d01a54f318aa579814bbb61d78ecb9866a50`。测试使用新建 Compose 项目和卷，结束只回收本次测试资源，保留镜像。

## 冷启动与待命启动样本

原生 RelWithDebInfo、320×180/25 fps 合成 H.264 B 帧本地文件、25 帧转码、两种模式各 10 次。边界是完整 HTTP 请求进入业务回调，到首个视频包完成封装写入及显式 flush。此处不含请求上传、互联网输入、接收端显示；不包括额外为观察终态等待的快照/进程回收时间。

| 指标 | 待命 1 个：中位数（最小–最大） | 待命 0 个：中位数（最小–最大） |
| --- | --- | --- |
| request → dispatch | 0.0235 ms（0.022–0.037） | 9.1185 ms（3.860–14.087） |
| dispatch → worker 接单 | 0.072 ms（0.065–0.698） | 0.690 ms（0.544–0.815） |
| request → 首个视频 mux/flush | 11.1795 ms（10.885–12.204） | 20.429 ms（14.634–25.431） |

原始样本位于 `out/service-test-asn0a3ys/startup-measurements.json`。测量运行在共享 WSL 开发环境，期间有其他检查；样本少，不能推导生产 SLA、互联网首帧时延、精确 p99 或几百路容量。此处 dispatch→worker 包含调度、解析等开销，不是纯 socket 微基准。待命 worker 的基础初始化在请求前发生，仍需为每个任务打开输入并建立 codec/filter/输出。

## 已知边界

- 只允许一路工作任务和 0..4 个待命进程；未实现几百路 admission、事件化海量连接、控制/遥测分离或运行中动态配置。
- 控制异常退出保留 worker；SIGTERM 是停止全部 worker 的明确意图。容器/宿主机重启、worker 自身崩溃不能靠控制接管保住媒体。
- 接管不改变内核 parent 关系；旧 worker 退出码可能未知，用 pidfd 确认退出，不伪造信号。任务最终状态由最后 worker 协议/快照决定。
- 日志、状态快照是有界且可能丢失的观测；失联期间不无限积压 stdout。独立追踪文件继续，最多两个首帧 checkpoint 可重放。
- 核心库和系统依赖未全部 ASan 插桩；真实 core 落盘、容量保留和符号化没有在本机验收。[崩溃诊断](crashes.md) 给出部署与验证步骤。
- 未实现 HTTP 进程“活着但卡死”的自动重启、媒体进度 watchdog、网络重连、跨容器任务恢复或长期 soak test。

运行入口：`python3 tests/service_integration.py`、`python3 tests/process_integration.py`、`python3 tests/observability_integration.py`、`python3 tests/docker_smoke.py`。构建与运行参数见 [进程池与接管](processes.md)。
