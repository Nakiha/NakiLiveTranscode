# 目录整理与测试入口验证（2026-10-18）

本轮完成测试公共设施、按套件执行、共享接口拆分、独立性能入口及源码目录整理。基点为主仓 06d2bb6b0b93fa3af25838e5f2c0df52d2a3a774 加本轮未提交修改；未提交、推送、发布或部署。依赖子模块 gitlink 未改，历史实验与证据未迁移或覆盖；原有未跟踪规划文档未改动。

## 实际改动

- tests/support 提取 HTTP/追踪、两个保持原超时合同的服务启动器、媒体探针、图片与进程工具。启动器显式接收调用套件的结果目录；导入模块不再创建目录、端口或进程。
- test-service.sh 支持重复 --suite ID，保持 profile 内清单顺序，拒绝未知/重复/跨 profile 选择。非 quick 选测保留 SDK gate，结果用 scope=selected 区分局部通过。
- 原 live.h 拆为配置、指标、worker 私有 Job、前置声明接口、运行时及管线入口；原 common.c 按原正文拆成配置、任务操作、报告、指标和运行时。controller 不包含 Job 完整状态。naki-control 编译配置/控制代码，BUILD_TESTING=OFF 不生成测试程序。
- app 按 controller/worker/control/media/observability/platform/crash/cli 分组；tests 按 unit/integration/fixtures/support/checks/benchmarks 分组。Docker 按类别复制，CI 引用和结果采集前缀同步更新；根 media_hooks.h 保留原私有工厂 include 的兼容入口。
- bench-service.sh 提供 latency 与 service 两种独立测量；service 的 1–4 路固定本地输出、pace/吞吐、进程资源采样、逐路服务指标和独立视频解码结果均保存在新目录。工具与边界见[性能测量](benchmarking.md)。

36 个搬移 C 文件在忽略 include/空白后正文 token 相同，原 common.c 的五组函数抽取也通过正文核对；没有修改媒体算法、线程、队列、锁顺序、mux 所有权或状态转换。详情在 implementation-identity.json，迁移表为 path-map.json。

## 最终源码窗口

统一测试输入 SHA-256：b7e48acdddaae0c6734b0540d5cc0703491f847d90bebea34d8bcefc67ab5718。下列 full、sanitizer、自定义目录选测及三组最终负载 smoke 的源码摘要相同，前后源码、所选 ELF/helper 和 SDK receipt 均未改变；结束后重新核对工作树摘要一致。SDK 仍复用已验证的 native 构建，未改库源码或安装字节。

证据根为 out/organization-20261018-151940/，生成物不入 Git：

| 检查 | 实际结果 |
| --- | --- |
| 普通构建 | final-build-3.log 通过；原头文件间接依赖改成显式 include |
| full | 35 步通过，含 15 项原生 quick、组织/入口检查及 86 个媒体生命周期场景；full-final/summary.json |
| ASan/UBSan | 独立 Debug 构建及 6 步通过，含同一 86 场景；sanitize/summary.json |
| 纯服务构建 | NAKI_BUILD_TESTING=OFF，只产生四个服务 ELF；production-build.log |
| 自定义二进制目录 | 从上述纯服务构建运行 full --suite service，SDK gate 与服务套件通过；custom-bin-selected/summary.json |
| 独立 service 入口 | 单路 EOF、双路混合 x264/VVenC 限时停止、四路 EOF 均通过，实际输出独立解码；final-benchmark-1/2/4 |
| 独立 latency 入口 | 首轮独立入口通过，其相关实现文件与最终窗口相同；benchmark-latency/summary.json。最终 full 再次运行同一驱动 |
| logger 微基准入口 | 原 HEAD 保存的 logger 源码对与搬移后源码均可编译并完成一次 smoke；logger-benchmark.log |
| 静态检查 | Python/shell/JSON、局部头文件闭包、组织边界、导入无副作用及函数内部旧布局导入检查通过；Git diff 空白检查通过 |

上表性能运行仅验证入口、采集与输出检查，样本很小，不能据此宣称容量或性能改善。CPU/RSS 是采样进程总和；吞吐/时延的具体窗口、量化边界见 benchmarking.md。

## 失败尝试与限制

原 full/summary.json 保留一次失败：probe 测试函数内部仍导入旧的 live_stream_fixtures 路径。改为 support 导入并增加全 AST 检查后，先通过 probe 定向检查，再在 full-final 新目录完整重跑。早期构建的缺失头文件依赖及修正日志也保留，不覆盖失败为成功。

外部可信 HTTPS 水印成功 fixture 未配置，full/sanitizer 都明确记录 not_run；本地 TLS 拒绝路径照常执行。没有实际 Docker 构建/smoke、远程 Actions、目标宿主机容量或 24/72 小时 soak。sanitizer 不重新插桩 FFmpeg/VVenC 和系统库，没有 TSan 结论。历史 n7 和旧镜像证据不补齐这些门槛。

使用方式见[测试入口](testing.md)、[测试导航](../tests/README.md)和[模块所有权](../app/README.md)。本轮不扩大媒体功能，不改变 controller 丢失时 worker 继续运行、单 owner mux、日志 I/O 及 fatal handler 的原有边界。
